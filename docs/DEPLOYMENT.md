# SwarmRT — Deployment & Operations

How to run a compiled SwarmRT program in production: supported platforms, the
full configuration surface, graceful shutdown, recovery/restart, health checks,
and backup/DR.

A SwarmRT program is a **single self-contained native binary** produced by
`swc build file.sw -o mybin`. It has no VM, no bytecode, no runtime install —
it links `libswarmrt.a` statically and boots in well under 10 ms. Deployment is
therefore "ship one binary + run it under a process supervisor."

---

## Supported platforms & dependencies

| Platform | Status | Notes |
|----------|--------|-------|
| Linux x86_64 | Supported (CI) | Primary production target. |
| Linux ARM64 | Supported | Same code path as x86_64. |
| macOS ARM64 | Supported (dev) | Development + local runs. |

**Link-time dependencies** (present on the build host; standard everywhere):
- `libsqlite3` — the `db_*` builtins (the durable store).
- `libz` — compression used by the runtime.
- `libssl`/`libcrypto` — **only** when built with TLS (`wss://` client via
  `wsc_connect_tls`, `ed25519_verify`). On Linux these are linked by default; on
  macOS TLS is opt-in with `make SWARMRT_TLS=1`. Without TLS the binary still
  serves `ws://` and returns nil for `wss://` — it never fails to build.

The produced binary is self-contained apart from these shared libs; deploy it to
a host with the same libc/arch. There is no separate runtime to install.

---

## Configuration reference (environment variables)

All runtime tuning is via environment variables, read once at startup unless
noted. **Every one is optional**; the defaults are the product defaults.

### Resource limits & quotas (Phase 3 hardening)

| Variable | Default | Meaning |
|----------|---------|---------|
| `SW_MAILBOX_MAX` | `1000000` | Max pending messages per process mailbox. Over-cap user sends are dropped loudly (counter + rate-limited stderr); the receiver is still woken. `0` disables. EXIT/DOWN signals and timer fires are exempt. |
| `SW_MSG_MAX_BYTES` | `0` (unlimited) | Max size of a single local value message (the deep-copied region's bytes). Over-cap sends are dropped loudly, leak-free. EXIT/DOWN exempt. Set for operators hardening against hostile/buggy senders. |
| `SW_PROC_MEM_MAX` | `0` (unlimited) | Per-process value-arena memory cap in bytes. A process that would exceed it dies with a loud panic; the node and siblings survive (a supervisor can restart it). Scope is the per-process value arena. |
| `SW_MAX_PROCS` | build default | Max concurrent process slots (arena sizing). Accepted range `[16, SWARM_MAX_PROCESSES]`. |
| `SW_HTTP_MAX_REQUEST` | `33554432` (32 MB) | Max bytes buffered per HTTP connection before a 413 / connection close. WebSocket frames are separately capped at 16 MB. |
| `SW_HTTP_IDLE_TIMEOUT_MS` | `30000` | Close an HTTP connection with no inbound bytes for this long (slow-loris defense) and free its slot. `0` disables. |
| `SW_HTTP_WS_IDLE_TIMEOUT_MS` | `0` (never) | Idle timeout for **established** WebSocket connections. Off by default — a quiet LiveView/agent session is legitimate. Inbound client pings count as activity. |

### Scheduling & runtime

| Variable | Default | Meaning |
|----------|---------|---------|
| `SW_SCHEDULERS` | # online CPUs | Number of scheduler OS threads (BEAM-style, one per core). Compiled binaries only; `swc run` is always single-scheduler. |
| `SW_SPIN_US` | on (seq_cst) | Bounded spin-before-park in the scheduler idle loop (cross-scheduler wake latency). Advanced tuning; leave default. |
| `SW_TURN_RESET_BYTES` | `262144` | Tail-call/turn checkpoint threshold for per-process arena reclaim. Advanced GC tuning; leave default. |
| `SW_GC_OFF` | off | Reverts to the pre-GC global-heap allocation model (A/B/debug escape hatch). Do not use in production — memory then grows until the OS process exits. |

### Shutdown, logging & diagnostics

| Variable | Default | Meaning |
|----------|---------|---------|
| `SW_SHUTDOWN_GRACE_MS` | `5000` | Graceful-shutdown drain deadline in ms (clamped to `[0, 3600000]`). See **Graceful shutdown** below. |
| `SW_NO_SIGNAL_SHUTDOWN` | off | If set, the runtime does NOT install SIGTERM/SIGINT handlers (for embedders that own signal disposition). |
| `SW_LOG_JSON` | off | If `1`, every abnormal process exit emits one JSON line on stderr: `{"ev":"proc_crash","pid":N,"reason":R[,"msg"][,"name"],"ts":MS}`. The human-readable panic trace remains the default. |
| `SW_QUIET` / `SW_RUNTIME_QUIET` | off | Suppress the startup banner and operational stderr notices. |
| `SW_DEADLOCK_DETECT` / `SW_DEADLOCK_MS` | on / tuned | Deadlock watchdog enable + interval. Leave default unless diagnosing. |
| `SW_SCHED_TRACE` | off | Scheduler stall/interleave tracing (`1`/`2`) for diagnosing wedges. |

> Provider/credential vars used by builtins (not runtime config): `OPENAI_API_KEY`,
> `OTONOMY_API_KEY`, `LLM_URL`, `OLLAMA_HOST`. `SW_FAIL_ALLOC_AT` is a test-only
> fault-injection hook (compiled only under `-DSW_ALLOC_FAULT`).

---

## Graceful shutdown

A compiled binary (and `swc run`) installs async-signal-safe **SIGTERM** and
**SIGINT** handlers. On the first signal the runtime performs an orderly
drain-with-deadline; a **second** signal hard-exits immediately (an operator
escape hatch: exit 130 for SIGINT, 143 for SIGTERM).

The drain sequence:

1. **Stop accepting new work** — a `draining` flag flips. It's observable via
   `swarm_stats()` (`draining: true`) and therefore via the `/readyz` health
   endpoint, so a load balancer can stop routing new requests to this instance.
2. **Drain outstanding messages** — currently-runnable fibers finish their work
   and mailboxes empty. The runtime polls for quiescence up to
   `SW_SHUTDOWN_GRACE_MS` (default 5000 ms).
3. **Cancel timers** — pending one-shot/interval timers are discarded (a
   heartbeat timer must not keep the node "busy" forever).
4. **Terminate** — the schedulers are joined and each process's `on_destroy`
   hook fires. This is **bounded even for a hung workload**: a never-quiescing
   fiber is returned to its scheduler by reduction preemption, which then honors
   the exit flag — so shutdown always completes at roughly the deadline, never
   hangs forever.

**Drain guarantee:** messages already queued in mailboxes are drained (fibers
run to consume them) until quiescent or the deadline; pending timers are
cancelled, not awaited. In-flight internal sends are **not** rejected mid-drain
(rejecting a gen_server call/reply would prevent quiescence).

**Under systemd/containers:** SIGTERM is the standard stop signal. Set your
`TimeoutStopSec` (systemd) or `stop_grace_period` (Docker/K8s) comfortably above
`SW_SHUTDOWN_GRACE_MS` so the drain completes before the supervisor escalates to
SIGKILL. Programmatic shutdown is available to embedders via
`sw_shutdown_graceful(swarm_id, deadline_ms)`.

---

## Restart & recovery (operational OTA)

Because a SwarmRT program is one native binary that boots in well under 10 ms,
the recovery model is **restart, not in-place patch**:

1. Flush durable state to **SQLite** (see below) — this happens continuously if
   your program persists as it goes.
2. Receive SIGTERM → graceful drain (above) → exit.
3. An **external supervisor** (systemd, a container orchestrator, or a parent
   process) restarts the binary.
4. The new process boots in milliseconds and **rehydrates its state from
   SQLite**.

To ship a new version, replace the binary on disk and restart — the sub-10 ms
boot makes this near-seamless behind a load balancer that is already draining
this instance (via `/readyz` reporting `draining`). There is **no** in-place
hot code swap of a compiled binary; the fast restart is the deliberate trade of
the single-binary design.

---

## Health & readiness

`lib/Health.sw` provides a drop-in HTTP health surface (see
`examples/health_endpoint.sw`):

```sw
module Main
import Health

fun main() {
    Health.serve(8080)   # blocks, serving the endpoints below
}
```

- `GET /healthz` → `200 ok` — **liveness**: the process is up and answering.
- `GET /readyz` → `200` with `swarm_stats()` as JSON — **readiness**: includes
  `draining` (true once graceful shutdown has begun — treat as NOT ready),
  `processes`, `crashes`, `restarts`, mailbox/message-drop counters, and
  per-scheduler stats. Point your load balancer / orchestrator readiness probe
  here so it drains the instance the moment shutdown starts.
- Any other path → `404`.

`Health.start(port)` is the non-blocking variant (spawns, returns the pid) so
you can run the health server alongside your own work in the same node.

---

## Backup & disaster recovery

- **SQLite is the durable store.** Everything you need to survive a restart must
  be written via the `db_*` builtins. Writes are per-statement autocommit;
  graceful shutdown lets in-flight writers finish. Back up the SQLite file(s)
  with normal SQLite backup practice (`.backup`, WAL-aware copy, or filesystem
  snapshot of a quiesced instance).
- **ETS does NOT survive a restart.** ETS tables live on the process heap and
  are gone when the OS process exits. Use ETS for hot in-memory shared state;
  persist anything that must outlive the process to SQLite.
- **Mailbox contents, live PIDs, and timers do not migrate** across a restart.
  Design work to be idempotent / re-drivable from persisted state, the same way
  you would for any crash-restart supervisor model.
