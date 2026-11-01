#!/bin/bash
# Regression test for #4: idle schedulers must actually sleep.
#
# The idle park used to be a blind 0.5 ms timed wait, so every idle scheduler
# woke ~2000 times a second (plus its spin window and steal probes) and an
# idle node burned ~10% of a core per scheduler. Now a scheduler parks until a
# producer signals it or the next timer is due. This runs a program that sits
# idle in a `receive ... after` (timer-driven, every scheduler parked) on 8
# schedulers, then checks:
#   - the whole run used well under a core-second of CPU (main: ~1.5 s here);
#   - receive-after timers still fire on time with every scheduler parked
#     (the park must end at the next timer deadline, and a newly armed
#     earliest timer must wake a parked scheduler).
#
# Needs bin/swc (make swc). Exit non-zero if the check fails.
set -u
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cat > "$WORK/idle_cpu.sw" <<'SW'
module Idle_cpu
# Overshoot of a 20 ms receive-after, worst of n tries.
fun lat(n, worst) {
    if (n == 0) { worst }
    else {
        t0 = timestamp()
        receive { _never -> 0 after 20 { 0 } }
        d = timestamp() - t0 - 20
        lat(n - 1, if (d > worst) { d } else { worst })
    }
}
fun main() {
    receive { _never -> 0 after 2000 { 0 } }
    print("worst_overshoot_ms=" ++ to_string(lat(25, 0)))
}
SW
"$SWC" build "$WORK/idle_cpu.sw" -o "$WORK/idle_cpu" >/dev/null 2>&1 || { echo "FAIL idle_cpu: build"; exit 1; }
TIMEFORMAT='%U %S'
cpu=$( { time SW_SCHEDULERS=8 "$WORK/idle_cpu" > "$WORK/out" 2>&1; } 2>&1 )
out=$(cat "$WORK/out")
total=$(awk -v c="$cpu" 'BEGIN{split(c,a," "); printf "%.3f", a[1]+a[2]}')
over=$(printf '%s' "$out" | sed -n 's/^worst_overshoot_ms=//p')
if [ -z "$over" ]; then
    echo "FAIL idle_cpu: program did not finish"; printf '%s\n' "$out" | head -5; exit 1
fi
# 2.5 s of mostly-idle wall time on 8 schedulers. Budget: 0.4 core-seconds.
if awk -v t="$total" 'BEGIN{exit !(t >= 0.4)}'; then
    echo "FAIL idle_cpu: idle node used ${total}s CPU over ~2.5s (8 schedulers; budget 0.4s)"; exit 1
fi
if [ "$over" -gt 15 ]; then
    echo "FAIL idle_cpu: receive-after overshot by ${over} ms with every scheduler parked"; exit 1
fi
echo "PASS idle_cpu (cpu=${total}s, worst receive-after overshoot=${over}ms)"
