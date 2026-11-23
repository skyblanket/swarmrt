#!/usr/bin/env python3
"""signoff.py: run the mixed-workload soak for N hours and write a verdict.

The release sign-off in docs/RELEASING.md (#48) asks for a 24-hour soak on a
dedicated Linux host. This is the harness for it: one command that runs
tests/soak/soak.sw, samples the node from outside and inside, and writes a
self-contained report a human can sign off on (or attach to the issue).

    make soak-signoff SOAK_HOURS=24                  # the release sign-off
    python3 tests/soak/signoff.py --hours 1          # a shorter run
    python3 tests/soak/signoff.py --seconds 120 --out /tmp/soak

What it samples
  outside  every --sample-seconds: the OS RSS of the soak process
           (/proc/<pid>/status VmRSS on Linux, `ps -o rss=` elsewhere)
  inside   every --report-seconds: the SOAK_STAT heartbeat soak.sw prints
           from swarm_stats() (live processes, spawns, crashes, restarts,
           drop counters, overflow queue, io ports, resident bytes)
  errors   the soak's stderr, classified line by line: the trace of the
           deliberate per-round panic is counted, anything else is kept
           (first 200 lines in soak.err) and fails the run. stderr is not
           stored whole: the deliberate panics alone are ~2 GB a day.

What it judges (each criterion is PASS or FAIL in the report; any FAIL fails
the run and the exit status is 1)
  clean_exit     exit status 0 and PROBE_OK printed
  stderr_clean   nothing on stderr but the deliberate panic's trace (no
                 watchdog, signal, sanitizer, resource-limit or other line)
  heartbeats     a heartbeat at least every 3x --report-seconds (no hang)
  rss_budget     peak RSS <= --rss-budget-mb
  rss_flat       no leak trend: the median RSS of the last 20% of samples is
                 at most --rss-growth-pct above the median of the window
                 just after warm-up (20%-40% of the run). Needs >= 10 samples.
  procs_bounded  live processes never above --max-procs, and the last
                 heartbeat's count is within --proc-slack of the first
  no_drops       mailbox_dropped and msgsize_dropped stay 0 (the soak never
                 approaches the caps, so a drop means a wedged consumer)
  progress       rounds and restarts keep increasing between heartbeats

Outputs, in --out (default build/soak/<UTC timestamp>):
  verdict.md   the report: host, commit, parameters, criteria, numbers
  verdict.json the same, machine-readable
  rss.csv      t_s,rss_kb
  stats.csv    one row per heartbeat
  soak.out     the soak's stdout
  soak.err     the first 200 unexpected stderr lines (empty on a clean run)
"""

import argparse
import datetime as dt
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ANSI = re.compile(r"\x1b\[[0-9;]*m")
# The soak crashes a supervised child every round on purpose (soak.sw flaky:
# hd([])), and the runtime prints that panic's trace. Those lines are the
# only expected stderr; anything else is unexpected and fails stderr_clean.
EXPECTED_STDERR = re.compile(
    r"^(panic: hd: list is empty"
    r"|\s*call chain \(innermost first\):"
    r"|\s*at \S*tests/soak/soak\.sw:\d+"
    r"|\s*\[\d+\] Main\.(flaky|_lambda_Main_\d+) at src/Main\.sw:\d+"
    r"|\s*)$")
KEEP_UNEXPECTED = 200


def rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
        return None
    except FileNotFoundError:
        pass
    except OSError:
        return None
    try:
        out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)],
                             capture_output=True, text=True).stdout.strip()
        return int(out) if out else None
    except (OSError, ValueError):
        return None


def sh(cmd):
    try:
        return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                              shell=True).stdout.strip()
    except OSError:
        return ""


def build(binary):
    swc = os.path.join(ROOT, "bin", "swc")
    if not os.access(swc, os.X_OK):
        sys.exit("signoff: bin/swc not found; run `make swc libswarmrt` first")
    r = subprocess.run([swc, "build", os.path.join(ROOT, "tests/soak/soak.sw"),
                        "-o", binary], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("signoff: soak.sw failed to compile:\n" + r.stdout + r.stderr)


def run(args, out_dir):
    binary = os.path.join(out_dir, "soak")
    build(binary)
    env = dict(os.environ, SW_QUIET="1",
               SOAK_REPORT_SECONDS=str(args.report_seconds))
    if args.leak_kb:
        env["SOAK_LEAK_KB"] = str(args.leak_kb)
    out_f = open(os.path.join(out_dir, "soak.out"), "w")
    err_f = open(os.path.join(out_dir, "soak.err"), "w")
    stats, rss = [], []
    t0 = time.monotonic()
    proc = subprocess.Popen([binary, str(args.seconds)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env, text=True, bufsize=1,
                            errors="replace")
    errs = {"panics": 0, "unexpected": 0}

    def err_reader():
        for line in proc.stderr:
            clean = ANSI.sub("", line.rstrip("\n"))
            if EXPECTED_STDERR.match(clean):
                if clean.startswith("panic: "):
                    errs["panics"] += 1
                continue
            errs["unexpected"] += 1
            if errs["unexpected"] <= KEEP_UNEXPECTED:
                err_f.write(clean + "\n")
                err_f.flush()

    def reader():
        for line in proc.stdout:
            out_f.write(line)
            out_f.flush()
            if line.startswith("SOAK_STAT "):
                try:
                    row = json.loads(line[len("SOAK_STAT "):])
                except ValueError:
                    continue
                row["wall_s"] = round(time.monotonic() - t0, 1)
                stats.append(row)
                if args.verbose:
                    print(f"[{row['wall_s']:>8.0f}s] rounds={row['rounds']} "
                          f"procs={row['processes']} crashes={row['crashes']} "
                          f"rss={(rss[-1][1] // 1024) if rss else '?'}MB", flush=True)

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    th_err = threading.Thread(target=err_reader, daemon=True)
    th_err.start()
    # The soak exits on its own at its deadline; give it a grace period, then
    # kill it, so a hang is a FAIL with a report rather than a stuck job.
    hard_deadline = t0 + args.seconds + max(120, args.seconds * 0.05)
    killed = False
    while proc.poll() is None:
        k = rss_kb(proc.pid)
        if k is not None:
            rss.append((round(time.monotonic() - t0, 1), k))
        if time.monotonic() > hard_deadline:
            proc.kill()
            killed = True
            break
        time.sleep(args.sample_seconds)
    ec = proc.wait()
    th.join(timeout=10)
    th_err.join(timeout=10)
    out_f.close()
    err_f.close()
    return ec, killed, stats, rss, errs, time.monotonic() - t0


def judge(args, ec, killed, stats, rss, errs, out_dir):
    crit = {}

    def put(name, ok, detail):
        crit[name] = {"pass": bool(ok), "detail": detail}

    out = open(os.path.join(out_dir, "soak.out")).read()
    bad = [l.rstrip("\n") for l in open(os.path.join(out_dir, "soak.err"))]

    put("clean_exit", ec == 0 and "PROBE_OK" in out and not killed,
        "killed after missing its deadline (hang)" if killed else
        f"exit {ec}, PROBE_OK {'printed' if 'PROBE_OK' in out else 'MISSING'}")
    put("stderr_clean", errs["unexpected"] == 0,
        f"only the {errs['panics']} deliberate panic traces" if not errs["unexpected"] else
        f"{errs['unexpected']} unexpected line(s), first: {bad[0][:200]}")

    times = [s["t_ms"] / 1000 for s in stats]
    gaps = [b - a for a, b in zip(times, times[1:])]
    max_gap = max(gaps) if gaps else None
    limit = 3 * args.report_seconds
    enough = len(stats) >= 2
    put("heartbeats", enough and max_gap <= limit,
        f"{len(stats)} heartbeats, largest gap {max_gap:.0f}s (limit {limit}s)"
        if enough else f"only {len(stats)} heartbeat(s)")

    peak_kb = max((k for _, k in rss), default=0)
    put("rss_budget", rss and peak_kb <= args.rss_budget_mb * 1024,
        f"peak {peak_kb / 1024:.1f} MB, budget {args.rss_budget_mb} MB"
        if rss else "no RSS samples")

    if len(rss) >= 10:
        n = len(rss)
        base = statistics.median(k for _, k in rss[n // 5: 2 * n // 5] or rss)
        tail = statistics.median(k for _, k in rss[-max(1, n // 5):])
        growth = (tail - base) * 100.0 / base if base else 0.0
        put("rss_flat", growth <= args.rss_growth_pct,
            f"median RSS {base / 1024:.1f} MB after warm-up -> {tail / 1024:.1f} MB "
            f"at the end ({growth:+.1f}%, limit +{args.rss_growth_pct}%)")
    else:
        put("rss_flat", False, f"only {len(rss)} RSS samples; need >= 10 to judge a trend")

    if stats:
        procs = [s["processes"] for s in stats]
        last_ok = procs[-1] <= procs[0] + args.proc_slack
        put("procs_bounded", max(procs) <= args.max_procs and last_ok,
            f"live processes first {procs[0]}, max {max(procs)}, last {procs[-1]} "
            f"(max limit {args.max_procs}, end slack {args.proc_slack})")
        drops = (stats[-1]["mailbox_dropped"], stats[-1]["msgsize_dropped"])
        put("no_drops", drops == (0, 0),
            f"mailbox_dropped {drops[0]}, msgsize_dropped {drops[1]}")
        rounds = [s["rounds"] for s in stats]
        restarts = [s["restarts"] for s in stats]
        stalled = [i for i in range(1, len(stats))
                   if rounds[i] <= rounds[i - 1] or restarts[i] <= restarts[i - 1]]
        put("progress", len(stats) >= 2 and not stalled,
            f"{rounds[-1]} rounds, {restarts[-1]} supervisor restarts, "
            f"{stats[-1]['crashes']} crashes (deliberate), {stats[-1]['spawns']} spawns"
            + (f"; no progress between heartbeats at {[times[i] for i in stalled][:5]}s"
               if stalled else ""))
    else:
        for name in ("procs_bounded", "no_drops", "progress"):
            put(name, False, "no heartbeats")
    return crit, peak_kb


def write_reports(args, out_dir, ec, elapsed, stats, rss, crit, peak_kb):
    with open(os.path.join(out_dir, "rss.csv"), "w") as f:
        f.write("t_s,rss_kb\n")
        f.writelines(f"{t},{k}\n" for t, k in rss)
    keys = ["wall_s", "t_ms", "rounds", "processes", "spawns", "crashes", "restarts",
            "mailbox_dropped", "msgsize_dropped", "overflow_queue", "io_ports",
            "resident_bytes", "kept"]
    with open(os.path.join(out_dir, "stats.csv"), "w") as f:
        f.write(",".join(keys) + "\n")
        f.writelines(",".join(str(s.get(k, "")) for k in keys) + "\n" for s in stats)

    verdict = "PASS" if all(c["pass"] for c in crit.values()) else "FAIL"
    host = {
        "hostname": platform.node(), "os": platform.platform(),
        "cpus": os.cpu_count(), "python": platform.python_version(),
        "mem_total": sh("awk '/MemTotal/{print $2\" kB\"}' /proc/meminfo"),
    }
    meta = {
        "verdict": verdict,
        "commit": sh("git rev-parse HEAD"), "describe": sh("git describe --tags --always --dirty"),
        "started_utc": args.started, "requested_seconds": args.seconds,
        "elapsed_seconds": round(elapsed, 1), "exit_status": ec,
        "params": {k: getattr(args, k) for k in ("rss_budget_mb", "rss_growth_pct",
                   "max_procs", "proc_slack", "sample_seconds", "report_seconds", "leak_kb")},
        "host": host, "peak_rss_kb": peak_kb,
        "rss_samples": len(rss), "heartbeats": len(stats), "criteria": crit,
    }
    with open(os.path.join(out_dir, "verdict.json"), "w") as f:
        json.dump(meta, f, indent=2)

    hours = args.seconds / 3600
    lines = [f"# Soak sign-off: {verdict}", "",
             f"- commit `{meta['describe']}` ({meta['commit'][:12]})",
             f"- started {args.started} UTC, ran {elapsed / 3600:.2f} h of {hours:.2f} h requested",
             f"- host `{host['hostname']}`: {host['os']}, {host['cpus']} CPUs, {host['mem_total']}",
             f"- {len(rss)} RSS samples every {args.sample_seconds}s, "
             f"{len(stats)} heartbeats every {args.report_seconds}s",
             "", "| criterion | result | detail |", "|---|---|---|"]
    lines += [f"| {k} | {'PASS' if c['pass'] else '**FAIL**'} | {c['detail']} |"
              for k, c in crit.items()]
    if args.leak_kb:
        lines += ["", f"_SOAK_LEAK_KB={args.leak_kb}: a deliberate leak was injected; "
                  "this run is the leak detector's self-test, not a sign-off._"]
    # A coarse RSS timeline: ~12 points across the run.
    if rss:
        step = max(1, len(rss) // 12)
        pts = rss[::step] + ([rss[-1]] if (len(rss) - 1) % step else [])
        lines += ["", "RSS over the run:", "", "| t (h:mm:ss) | RSS |", "|---|---|"]
        lines += [f"| {int(t) // 3600}:{int(t) % 3600 // 60:02d}:{int(t) % 60:02d} | {k / 1024:.1f} MB |"
                  for t, k in pts]
    lines += ["", "Files: verdict.json, rss.csv, stats.csv, soak.out, soak.err."]
    with open(os.path.join(out_dir, "verdict.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    return verdict


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--hours", type=float)
    g.add_argument("--seconds", type=int)
    ap.add_argument("--out")
    ap.add_argument("--rss-budget-mb", type=int, default=512)
    ap.add_argument("--rss-growth-pct", type=float, default=10.0)
    ap.add_argument("--max-procs", type=int, default=1000)
    ap.add_argument("--proc-slack", type=int, default=16)
    ap.add_argument("--sample-seconds", type=float, default=None)
    ap.add_argument("--report-seconds", type=int, default=None)
    ap.add_argument("--leak-kb", type=int, default=0,
                    help="test only: keep N KB per round alive (the verdict must FAIL)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if args.hours is not None:
        args.seconds = int(args.hours * 3600)
    if args.seconds is None:
        args.seconds = 3600
    if args.seconds < 30:
        sys.exit("signoff: run at least 30 seconds")
    # Aim for a few hundred RSS samples and ~100+ heartbeats however long the run.
    if args.sample_seconds is None:
        args.sample_seconds = min(30.0, max(1.0, args.seconds / 400))
    if args.report_seconds is None:
        args.report_seconds = int(min(300, max(2, args.seconds / 120)))
    args.started = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
    out_dir = args.out or os.path.join(ROOT, "build", "soak", args.started.replace(":", ""))
    os.makedirs(out_dir, exist_ok=True)

    print(f"signoff: {args.seconds}s soak, report in {out_dir}", flush=True)
    ec, killed, stats, rss, errs, elapsed = run(args, out_dir)
    crit, peak_kb = judge(args, ec, killed, stats, rss, errs, out_dir)
    verdict = write_reports(args, out_dir, ec, elapsed, stats, rss, crit, peak_kb)
    print(open(os.path.join(out_dir, "verdict.md")).read())
    print(f"signoff: {verdict} ({out_dir}/verdict.md)")
    sys.exit(0 if verdict == "PASS" else 1)


if __name__ == "__main__":
    main()
