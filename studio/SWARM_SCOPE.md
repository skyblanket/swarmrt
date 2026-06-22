# SwarmScope

SwarmScope is a live visual stress lab built entirely on SwarmRT.

It runs a real actor workload, samples the live process table, and streams
runtime telemetry to an animated browser dashboard over SwarmRT's built-in
HTTP and WebSocket server.

## Run

From the SwarmRT repository root:

```bash
make swc libswarmrt
bin/swc build studio/swarm_scope.sw -o /tmp/swarm_scope
SW_MAX_PROCS=12000 /tmp/swarm_scope
```

Open <http://localhost:4010>.

Set `SWARM_SCOPE_PAIRS` to change the initial workload. Each pair consists of
two processes exchanging ping/pong messages:

```bash
SW_MAX_PROCS=20000 SWARM_SCOPE_PAIRS=2000 /tmp/swarm_scope
```

## What Is Real

- The process galaxy is generated from the live process count and state
  distribution; the hot-process table contains direct sampled
  `process_info()` rows.
- Message throughput comes from real actor ping/pong round trips.
- Process states, reductions, mailbox depths, and heap values come from
  `process_list()` and `process_info()`.
- Runtime RSS and CPU are sampled from the SwarmScope host process every two
  seconds.
- The transient burst control spawns short-lived compute actors.
- The chaos control crashes permanent children under a dynamic supervisor;
  SwarmRT restarts them.
- Each connected dashboard tab is itself a SwarmRT process.

The browser only renders telemetry. There is no Node, Python, or external
dashboard server.
