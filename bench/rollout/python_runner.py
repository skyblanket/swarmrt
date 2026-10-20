#!/usr/bin/env python3
"""The same rollout benchmark as swarmrt_runner.sw, written the way a careful
asyncio harness would be: aiohttp with one shared session, a semaphore for
concurrency, per-episode asyncio.wait_for deadlines, gather(return_exceptions=
True) so one bad episode costs one result, Retry-After honoured on 429, a
backoff on 5xx, and subprocess tools with a timeout. Graders run inline, as
reward functions do in verl, OpenRLHF and TRL.

Env: BENCH_N, BENCH_CONC, BENCH_URL, BENCH_OUT, BENCH_SCENARIO ("api",
"runaway" or "clean"). Episodes already in BENCH_OUT are skipped (resume).
"""
import asyncio
import json
import os
import sys
import time

import aiohttp

N = int(os.environ.get("BENCH_N", "1000"))
CONC = int(os.environ.get("BENCH_CONC", "128"))
URL = os.environ.get("BENCH_URL", "http://127.0.0.1:18500/v1/chat/completions")
OUT = os.environ.get("BENCH_OUT", "python_results.jsonl")
SCENARIO = os.environ.get("BENCH_SCENARIO", "api")
EPISODE_DEADLINE = 15.0
TOOL_TIMEOUT = 5.0


def record(out, id_, status, kind, correct):
    out.write(json.dumps({"id": id_, "status": status, "kind": kind, "correct": correct}) + "\n")
    out.flush()


async def llm(session, prompt):
    for attempt in range(5):
        async with session.post(URL, json={"max_tokens": 16, "messages": [
                {"role": "user", "content": prompt}]}) as r:
            if r.status == 429:
                await asyncio.sleep(float(r.headers.get("Retry-After", "1")))
                continue
            if r.status >= 500:
                await asyncio.sleep(min(2 ** attempt, 8))
                continue
            r.raise_for_status()
            body = await r.json()
            return body["choices"][0]["message"]["content"].strip()
    raise RuntimeError("llm: retries exhausted")


def tool_cmd(id_):
    return "sleep 300 & wait" if SCENARIO != "clean" and id_ % 100 == 13 else f"echo $(({id_} * 2))"


async def tool(id_):
    proc = await asyncio.create_subprocess_exec(
        "sh", "-c", tool_cmd(id_), stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL)
    try:
        out, _ = await asyncio.wait_for(proc.communicate(), TOOL_TIMEOUT)
        return proc.returncode, out.decode()
    except asyncio.TimeoutError:
        proc.kill()             # the common pattern: kill the child we started
        await proc.wait()
        return 124, ""


def grade(id_, answer):
    if SCENARIO != "clean" and id_ % 100 == 29:
        raise RuntimeError(f"grader crashed on episode {id_}")
    if SCENARIO == "runaway" and id_ % 100 == 53:
        i = 0
        while True:             # a grader that never returns
            i += 1
    return answer == f"ANSWER {id_ * 2}"


async def episode(session, out, id_):
    r0 = await llm(session, f"EP {id_} TURN 0: pick a tool")
    if not r0.startswith("TOOL"):
        raise RuntimeError(f"bad turn 0 reply: {r0}")
    code, result = await tool(id_)
    if code == 124:
        record(out, id_, "error", "tool_timeout", False)
        return "tool_timeout"
    r1 = await llm(session, f"EP {id_} TURN 1: RESULT {result.strip()}")
    ok = grade(id_, r1)
    record(out, id_, "ok", "done", ok)
    return ok


async def guarded(sem, session, out, id_):
    async with sem:
        try:
            return await asyncio.wait_for(episode(session, out, id_), EPISODE_DEADLINE)
        except asyncio.TimeoutError:
            record(out, id_, "error", "timeout", False)
        except Exception:
            record(out, id_, "error", "crash", False)


def done_ids(path):
    ids = set()
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                try:
                    ids.add(json.loads(line)["id"])
                except (ValueError, KeyError):
                    pass
    return ids


async def main():
    seen = done_ids(OUT)
    todo = [i for i in range(N) if i not in seen]
    print(f"python: {len(todo)} of {N} episodes to run, concurrency {CONC}", file=sys.stderr)
    t0 = time.time()
    sem = asyncio.Semaphore(CONC)
    conn = aiohttp.TCPConnector(limit=CONC)
    async with aiohttp.ClientSession(connector=conn, timeout=aiohttp.ClientTimeout(total=300)) as session:
        with open(OUT, "a") as out:
            await asyncio.gather(*(guarded(sem, session, out, i) for i in todo),
                                 return_exceptions=True)
    print(f"python: finished in {int((time.time() - t0) * 1000)} ms", file=sys.stderr)


if __name__ == "__main__":
    asyncio.run(main())
