#!/usr/bin/env python3
"""Run a SwarmRT binary and attribute its resident memory by mapping kind.

    python3 bench/memory/measure_smaps.py ./ring N=10000 MODE=busy SW_SCHEDULERS=4 [AT=5]

Linux only (reads /proc/PID/smaps). Arguments after the binary are
environment variables for it; AT is how many seconds to wait before sampling
(default 5). Prints VmRSS, RSS per process (VmRSS / N), and the RSS of each
mapping kind: glibc thread arenas (anonymous mappings of 64 MB or more),
process stacks (128 KB anonymous mappings), guard pages, the brk heap, and
everything else. Used for the numbers in docs/design/PROCESS_WEIGHT.md.
"""
import collections
import os
import re
import subprocess
import sys
import time

STACK_SIZES = (128 * 1024, 132 * 1024, 136 * 1024)


def kind(name, perms, size):
    if name == "[heap]":
        return "heap(brk)"
    if name.startswith("/"):
        return "file"
    if name.startswith("["):
        return name
    if perms.startswith("---"):
        return "guard"
    if size in STACK_SIZES:
        return "proc-stack(128K)"
    if size >= 64 * 1024 * 1024:
        return "big-anon(>=64MB)"
    return "anon-other"


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.update(dict(a.split("=", 1) for a in sys.argv[2:]))
    n = int(env.get("N", "10000"))
    proc = subprocess.Popen([binary], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(float(env.get("AT", "5")))
        rss = collections.Counter()
        maps = collections.Counter()
        cur = None
        with open(f"/proc/{proc.pid}/smaps") as f:
            for line in f:
                m = re.match(r"^([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \S+\s*(.*)$", line)
                if m:
                    size = int(m.group(2), 16) - int(m.group(1), 16)
                    cur = kind(m.group(4).strip(), m.group(3), size)
                    maps[cur] += 1
                elif line.startswith("Rss:") and cur:
                    rss[cur] += int(line.split()[1])
        with open(f"/proc/{proc.pid}/status") as f:
            vmrss = int(re.search(r"VmRSS:\s+(\d+)", f.read()).group(1))
    finally:
        proc.kill()
        proc.wait()
    print(f"VmRSS {vmrss / 1024:.1f} MB  per-process {vmrss / n:.1f} KB  ({' '.join(sys.argv[2:])})")
    for k, kb in rss.most_common():
        print(f"   {k:20s} {kb / 1024:8.1f} MB  maps={maps[k]:6d}  per-process {kb / n:7.2f} KB")


if __name__ == "__main__":
    main()
