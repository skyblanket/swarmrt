#!/usr/bin/env python3
"""Per-process resident memory of a SwarmRT program, as JSON.

    python3 bench/memory/measure.py BINARY [--n N] [--mode busy|idle]
                                   [--at 6,8,10] [--schedulers S]

Runs BINARY (built from bench/memory/ring.sw) twice: with N=0 (runtime fixed
cost) and with N=target. For each run it waits for the program's READY line,
samples RSS at each --at offset (seconds after READY), and takes the median.
per_process_bytes = (median RSS(N) - median RSS(0)) / N.

RSS comes from /proc/PID/status on Linux and `ps -o rss=` on macOS. On Linux
it also counts mappings (vma_count), which bounds how many processes fit
under vm.max_map_count. Prints one JSON object. See
docs/design/PROCESS_WEIGHT.md (Phase 0).
"""
import argparse
import json
import os
import platform
import statistics
import subprocess
import sys
import time


def rss_kb(pid):
    if sys.platform.startswith("linux"):
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
        return 0
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)],
                         capture_output=True, text=True).stdout.strip()
    return int(out or 0)


def vma_count(pid):
    if not sys.platform.startswith("linux"):
        return None
    with open(f"/proc/{pid}/maps") as f:
        return sum(1 for _ in f)


def run_once(binary, n, mode, ats, schedulers):
    env = dict(os.environ, N=str(n), MODE=mode,
               HOLD_MS=str(int((max(ats) + 2) * 1000)))
    if schedulers:
        env["SW_SCHEDULERS"] = str(schedulers)
    t0 = time.monotonic()
    p = subprocess.Popen([binary], env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True)
    try:
        for line in p.stdout:
            if line.strip() == "READY":
                break
        else:
            raise RuntimeError(f"{binary} exited before READY (rc={p.wait()})")
        ready = time.monotonic()
        samples, vmas = [], None
        for at in sorted(ats):
            time.sleep(max(0.0, ready + at - time.monotonic()))
            samples.append(rss_kb(p.pid) * 1024)
            vmas = vma_count(p.pid)
        return {"ready_ms": int((ready - t0) * 1000), "rss_samples": samples,
                "rss_bytes": int(statistics.median(samples)), "vma_count": vmas}
    finally:
        p.kill()
        p.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--n", type=int, default=10000)
    ap.add_argument("--mode", default="busy", choices=["busy", "idle"])
    ap.add_argument("--at", default="6,8,10", help="seconds after READY to sample")
    ap.add_argument("--schedulers", type=int, default=4)
    a = ap.parse_args()
    ats = [float(x) for x in a.at.split(",")]
    base = run_once(a.binary, 0, a.mode, ats, a.schedulers)
    run = run_once(a.binary, a.n, a.mode, ats, a.schedulers)
    print(json.dumps({
        "n": a.n, "mode": a.mode, "schedulers": a.schedulers,
        "os": f"{platform.system()} {platform.machine()}",
        "rss_bytes": run["rss_bytes"], "rss_base_bytes": base["rss_bytes"],
        "per_process_bytes": int((run["rss_bytes"] - base["rss_bytes"]) / a.n),
        "rss_samples": run["rss_samples"], "vma_count": run["vma_count"],
        "ready_ms": run["ready_ms"],
    }))


if __name__ == "__main__":
    main()
