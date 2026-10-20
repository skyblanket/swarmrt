#!/usr/bin/env python3
"""Check a results JSONL against the expected outcome of every episode.

    python3 analyze.py results.jsonl N [scenario]

Expected outcomes (see README.md):
  id % 100 == 7   timeout (the model never answers)
  id % 100 == 13  tool_timeout (the tool hangs)
  id % 100 == 29  crash (the grader raises)
  id % 100 == 53  timeout, in the "runaway" scenario (the grader never returns)
  otherwise       ok and correct (every episode, in the "clean" scenario)
Prints one JSON object.
"""
import json
import sys
from collections import Counter


def expected(i, scenario):
    if scenario == "clean":
        return "ok"
    if i % 100 == 7:
        return "timeout"
    if i % 100 == 13:
        return "tool_timeout"
    if i % 100 == 29:
        return "crash"
    if scenario == "runaway" and i % 100 == 53:
        return "timeout"
    return "ok"


def main():
    path, n = sys.argv[1], int(sys.argv[2])
    scenario = sys.argv[3] if len(sys.argv) > 3 else "api"
    rows = []
    try:
        with open(path) as f:
            for line in f:
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    pass
    except FileNotFoundError:
        pass
    count = Counter(r["id"] for r in rows)
    last = {r["id"]: r for r in rows}
    got = Counter()
    wrong = []
    for i in range(n):
        r = last.get(i)
        exp = expected(i, scenario)
        if r is None:
            got["missing"] += 1
            if exp == "ok":
                wrong.append(i)
            continue
        kind = "ok" if (r["status"] == "ok" and r["correct"]) else r["kind"]
        got[kind] += 1
        if kind != exp:
            wrong.append(i)
    healthy = sum(1 for i in range(n) if expected(i, scenario) == "ok")
    print(json.dumps({
        "rows": len(rows), "episodes": n, "healthy_expected": healthy,
        "outcomes": dict(got),
        "duplicates": sum(c - 1 for c in count.values() if c > 1),
        "wrong_outcome": len(wrong),
        "healthy_lost": sum(1 for i in wrong if expected(i, scenario) == "ok"),
        "examples": wrong[:5],
    }))


if __name__ == "__main__":
    main()
