# Rollout benchmark: SwarmRT vs a careful Python asyncio harness

An eval-style workload run by two runners that do the same work against the
same mock model server:

- `swarmrt_runner.sw` uses only what SwarmRT ships: `Std.task_stream`,
  `llm_complete` and `exec_managed`.
- `python_runner.py` is written the way a careful asyncio harness would be.
  It uses one aiohttp session, a semaphore, `asyncio.wait_for` per episode,
  `gather(return_exceptions=True)`, `Retry-After` on 429, backoff on 5xx and
  subprocess tools with a timeout. Graders run inline, as reward functions do
  in verl, OpenRLHF and TRL.

The background is in the research report that motivated this
benchmark: how 18 eval and RL rollout systems run episodes and what fails.

## The workload

Each episode `id`:

1. asks the model for a tool (turn 0);
2. runs the tool, `sh -c 'echo $((id*2))'`, with a 5 s timeout;
3. asks the model again with the tool's output (turn 1);
4. grades the answer.

Every episode has a 15 s deadline, and the run is capped at 128 episodes at a
time.

Faults are injected deterministically by episode id, so both runners see the
same ones (`mock_server.py`, `analyze.py`):

| Episodes | Fault | Expected outcome |
|---|---|---|
| `id % 20 == 3` | first model call of each turn returns 429 with `Retry-After: 1` | ok, after a retry |
| `id % 50 == 11` | first model call of each turn returns 500 | ok, after a retry |
| `id % 100 == 7` | turn 0 never answers | `timeout` |
| `id % 100 == 13` | the tool hangs and leaves a grandchild (`sleep 300 & wait`) | `tool_timeout` |
| `id % 100 == 29` | the grader raises | `crash` |
| `id % 100 == 53` | (runaway scenario only) the grader never returns | `timeout` |

## Scenarios

- **clean**: no faults; measures throughput, CPU and memory.
- **api**: every fault above except the runaway grader.
- **runaway**: `api` plus graders that never return.
- **resume**: `api`; the runner is killed with SIGKILL after 20 s, then
  restarted on the same results file.

## What is measured

- **Correct outcomes**: does every episode get its expected outcome?
- **Healthy episodes lost**: episodes with no fault that didn't finish correctly.
- **Duplicates**: episodes recorded more than once after a resume.
- **Orphans left after the run**: `sleep 300` grandchildren and `curl` processes.
- **Wall time, CPU time and peak RSS** of the runner.
- **Requests the server saw**: compared with the clean count, this gives calls re-billed after a resume.

## Running

```bash
make swc libswarmrt
pip install aiohttp
bash bench/rollout/run_bench.sh 10000 128   # N episodes, concurrency
```

`MOCK=sw bash bench/rollout/run_bench.sh ...` serves the model API from
`mock_server.sw` instead of `mock_server.py`. It has the same contract and
dogfoods SwarmRT's HTTP server under load. Comparisons use the Python mock, so
a SwarmRT server bug can't skew SwarmRT's own numbers.

Each run prints one JSON line. Per-run results, logs and timings go to
`bench/rollout/out/`.

## Results

See `RESULTS.md`. It records the numbers from one run on a 4-core Linux
container, with the commit they came from.
