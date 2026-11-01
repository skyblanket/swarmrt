#!/usr/bin/env python3
"""CI regression gate over run_bench.sh's output (issue #70).

    MOCK=sw RUN_PY=0 SCENARIOS="api runaway resume" \\
        bash run_bench.sh 500 64 | python3 ci_gate.py

Reads the one-JSON-object-per-line summaries run_bench.sh prints and fails
(exit 1) unless, for every SwarmRT run:
  - every episode has its expected outcome (0 wrong, 0 healthy lost);
  - no episode is recorded twice (0 duplicates, which matters after resume);
  - no `sleep 300` grandchild of a hung tool and no `curl` survived the run
    (for resume: the restarted run; what the SIGKILLed first run leaves
    behind is reported as a warning, see orphans_from_kill);
  - the runner itself exited 0 (the resume run's restart, for resume);
and every scenario in EXPECT (default: api runaway resume) actually ran.
Throughput numbers are printed, never asserted: shared runners are too noisy.
"""
import json
import os
import sys


def main():
    expect = os.environ.get("EXPECT", "api runaway resume").split()
    runs = {}
    for line in sys.stdin:
        line = line.strip()
        if not line.startswith("{"):
            if line:
                print(line)
            continue
        try:
            r = json.loads(line)
        except ValueError:
            print("ci_gate: unparseable summary line:", line)
            return 1
        runs[r["run"]] = r
        print(line)

    fails = []
    for sc in expect:
        name = "swarmrt_" + sc
        r = runs.get(name)
        if r is None:
            fails.append(f"{name}: no summary (the run did not happen)")
            continue
        res = r.get("result", {})
        if r.get("rc") != 0:
            fails.append(f"{name}: runner exited {r.get('rc')}")
        for key in ("wrong_outcome", "healthy_lost", "duplicates"):
            if res.get(key, 1) != 0:
                fails.append(f"{name}: {key} = {res.get(key)} (examples {res.get('examples')})")
        if res.get("rows", 0) < res.get("episodes", 1):
            fails.append(f"{name}: {res.get('rows')} rows for {res.get('episodes')} episodes")
        orphans = r.get("orphans_after", {})
        for what in ("sleep", "curl"):
            if orphans.get(what, 1) != 0:
                fails.append(f"{name}: {orphans.get(what)} orphaned `{what}` after the run")
        if sc == "resume" and any(r.get("orphans_from_kill", {}).values()):
            print(f"WARN {name}: the SIGKILLed first run left {r['orphans_from_kill']} behind "
                  "(tools in flight when the runner died); not gated")
        if sc == "resume" and not (0 < r.get("rows_before_kill", 0) < res.get("episodes", 0)):
            fails.append(f"{name}: the SIGKILL landed outside the run "
                         f"({r.get('rows_before_kill')} rows before it); lower KILL_AT")

    for f in fails:
        print("FAIL", f)
    if fails:
        return 1
    print("OK bench/rollout gate: " + ", ".join(
        f"{n} {runs[n].get('wall_s', runs[n].get('resume_wall_s'))} s" for n in sorted(runs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
