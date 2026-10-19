#!/usr/bin/env python3
"""Mock OpenAI-compatible chat endpoint with deterministic, injected faults.

The episode id and turn are read from the prompt ("EP <id> TURN <t> ..."), so
both runners see exactly the same faults:

  id % 20 == 3    first attempt of each turn -> 429 with Retry-After: 1
  id % 50 == 11   first attempt of each turn -> 500
  id % 100 == 7   turn 0 never answers (the connection hangs)
  otherwise       answers after 50-150 ms (seeded by id)

POST /reset {"faults": false} serves every request cleanly (the "clean"
scenario, for throughput).

Turn 0 replies "TOOL <id>", turn 1 replies "ANSWER <n>" where n is read from
the prompt's "RESULT <n>". GET /stats returns request counts, so re-billed
calls after a resume can be counted.

    python3 mock_server.py [port]
"""
import asyncio
import json
import random
import re
import sys
from collections import Counter

from aiohttp import web

attempts = Counter()          # (id, turn) -> attempts seen
calls_per_ep = Counter()      # id -> total requests
totals = Counter()            # status -> count
faults = {"on": True}         # POST /reset {"faults": false} turns them off
EP = re.compile(r"EP (\d+) TURN (\d+)")
RES = re.compile(r"RESULT (-?\d+)")


def reply(content):
    return web.json_response({
        "id": "mock", "object": "chat.completion",
        "choices": [{"index": 0, "finish_reason": "stop",
                     "message": {"role": "assistant", "content": content}}],
    })


async def chat(request):
    body = await request.json()
    prompt = body["messages"][-1]["content"]
    m = EP.search(prompt)
    if not m:
        totals["bad"] += 1
        return web.json_response({"error": {"message": "no EP tag"}}, status=400)
    ep, turn = int(m.group(1)), int(m.group(2))
    calls_per_ep[ep] += 1
    attempts[(ep, turn)] += 1
    n = attempts[(ep, turn)]

    if faults["on"] and ep % 20 == 3 and n == 1:
        totals["429"] += 1
        return web.json_response({"error": {"message": "rate limited", "type": "rate_limit"}},
                                 status=429, headers={"Retry-After": "1"})
    if faults["on"] and ep % 50 == 11 and n == 1:
        totals["500"] += 1
        return web.json_response({"error": {"message": "server error"}}, status=500)
    if faults["on"] and ep % 100 == 7 and turn == 0:
        totals["hang"] += 1
        await asyncio.sleep(3600)

    await asyncio.sleep(random.Random(ep * 31 + turn).uniform(0.05, 0.15))
    totals["200"] += 1
    if turn == 0:
        return reply(f"TOOL {ep}")
    r = RES.search(prompt)
    return reply(f"ANSWER {r.group(1) if r else 'none'}")


async def stats(request):
    multi = sum(1 for c in calls_per_ep.values() if c > 2)
    return web.json_response({
        "totals": dict(totals),
        "requests": sum(calls_per_ep.values()),
        "episodes_seen": len(calls_per_ep),
        "episodes_with_extra_calls": multi,
    })


async def reset(request):
    attempts.clear(); calls_per_ep.clear(); totals.clear()
    try:
        faults["on"] = bool((await request.json()).get("faults", True))
    except ValueError:
        faults["on"] = True
    return web.json_response({"ok": True})


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18500
    app = web.Application(client_max_size=4 * 1024 * 1024)
    app.add_routes([web.post("/v1/chat/completions", chat),
                    web.get("/stats", stats), web.post("/reset", reset)])
    web.run_app(app, host="127.0.0.1", port=port, print=None, backlog=4096)


if __name__ == "__main__":
    main()
