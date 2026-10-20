# Rollout benchmark results

One run on 2026-10-19/20, in a 4-core Linux container with 16 GB of RAM, using
`bash bench/rollout/run_bench.sh 10000 128` (10,000 episodes, 128 at a time,
Python mock server):

- SwarmRT: swarmrt `v2.1.0`, `swc 2.1.0` at commit `98c4dcf`.
- Python baseline: Python 3.11.15 and aiohttp 3.14.5.

## Summary

SwarmRT got every one of the 10,000 episodes right in every scenario, killed
hung tools along with their grandchildren, and kept running when graders never
returned. The asyncio baseline misclassified the hung tools and orphaned their
children. When graders never returned, its whole run froze.

SwarmRT is much slower and heavier on CPU, because every model call forks
`curl`. With no faults it took 4.6x the wall time and 11x the CPU. Until that
is fixed, Python finishes first on large API runs.

## Results

| Scenario | Runner | Wall | CPU | Peak RSS | Outcomes | Healthy episodes lost | Orphans after the run |
|---|---|---|---|---|---|---|---|
| clean | SwarmRT | 79.4 s | 268 s | 35 MB | 10,000 ok | 0 | none |
| clean | Python | **17.2 s** | **24.6 s** | 51 MB | 10,000 ok | 0 | none |
| api | SwarmRT | 107.6 s | 312 s | 45 MB | **all 10,000 correct**: 9,700 ok, 100 timeout, 100 tool_timeout, 100 crash | 0 | **100 `curl`** |
| api | Python | 61.3 s | 28.7 s | 52 MB | **100 wrong**: the hung tools are reported as episode timeouts | 0 | **100 `sleep 300`** |
| runaway | SwarmRT | 125.2 s | 380 s | 48 MB | **all 10,000 correct**; the runaway graders end at their deadline | 0 | 100 `curl` |
| runaway | Python | killed at 300 s | – | – | **84 of 10,000 finished**: the event loop froze | 9,516 | – |
| resume | SwarmRT | 87.6 s after restart | – | – | all correct, 0 duplicates | 0 | – |
| resume | Python | 45.1 s after restart | – | – | 0 duplicates, the same 100 wrong | 0 | – |

The server saw 20,000 model requests in the clean runs and 21,200 in the api
runs: the extra 1,200 are 429 and 500 retries. After the resume it saw 21,360
(SwarmRT) and 21,336 (Python), so 160 and 136 calls were billed twice. The
runners were killed at the same time (20 s) but had made different progress
by then: SwarmRT had recorded 2,216 episodes and Python 4,499.

## What each result comes from

- **SwarmRT's throughput:**
  - **Cause:** every `llm_complete` writes the request body to a temp file
    and forks and execs `curl`. There's no connection reuse and about 13 ms
    of CPU per model call.
  - **Evidence:** 268 CPU-seconds for 20,000 calls on 4 cores, so the run was
    CPU-bound. aiohttp spends about 1.2 ms per call on one persistent session.
- **Orphaned `curl` (SwarmRT):** when an episode dies at its deadline while
  `llm_complete` waits on a hung request, the `curl` keeps running, up to its
  300 s `--max-time`, and holds an offload thread. `llm_complete` has no
  per-request timeout either.
- **Hung tools (Python):** `proc.kill()` kills `sh`, not the `sleep` it left
  behind. The grandchild still holds the stdout pipe, so `proc.wait()` doesn't
  return. The 5 s tool timeout turns into the 15 s episode deadline, and the
  grandchild is orphaned. `exec_managed` kills the whole process group, so
  SwarmRT reports `tool_timeout` and leaves nothing behind.
- **Runaway graders (Python):** a CPU loop in a coroutine never reaches an
  `await`. That starves the event loop, so no deadline can fire and every
  other episode stops. SwarmRT preempts at every call, so `task_stream`'s
  deadline kills the runaway episode and the others run on. This matches
  failure mode 2 in the research: cooperative cancellation can't stop
  blocking code, which is how verl, OpenRLHF and TRL run reward functions.
- **Resume:** both runners skip episodes already in the results file, and
  neither lost or duplicated an episode. SwarmRT re-ran more work, because
  `Std.task_stream` returns its error cells only when the whole stream ends,
  so the timeouts and crashes from before the kill were never recorded.

## Dogfooding: the model mock in sw

`MOCK=sw bash run_bench.sh 1000 128` serves the model API from
`mock_server.sw`. SwarmRT's own HTTP server handled 128 concurrent clients
and about 2,100 requests per scenario, and every scenario reproduced the
pattern above (both runners, 1,000 episodes).

Writing it found one API footgun: an `{'http_request', ...}` message carries
the method as an atom (`'GET'`), while `http_request`'s client options take a
string (`"GET"`). That isn't documented, and `'GET' == "GET"` is false.

## Caveats

- **Setup:** a mock model on the same host, with 50–150 ms latency. Against
  a real API with 1–30 s latency, per-call CPU matters less, but 13 ms of CPU
  per call still caps a 4-core box at about 300 calls/s.
- **One baseline:** it is careful, but it isn't every harness. Some harnesses
  run graders in a process pool, or use `start_new_session` and `killpg` for
  tools. Those avoid the two Python failures, at the cost of extra code that
  every harness has to get right.
- **Native crashes:** neither runner survives a native crash of its own
  process, such as a segfault in C code or the OS out-of-memory killer.
- **A single run:** no variance measured. Wall times include the 15 s
  deadlines of the hung episodes.
