#!/usr/bin/env bash
# bench/actor/run.sh — head-to-head actor-model microbenchmarks:
#   swarmrt  vs  Erlang/OTP (BeamAsm JIT)  vs  Go (goroutines).
#
# Four workloads, identical shape in each language:
#   spawn     — spawn 1,000,000 processes that immediately exit
#   pingpong  — 2 processes exchange 2,000,000 request/reply round-trips
#   fanout    — 1 producer streams 5,000,000 messages to 1 consumer
#   parallel  — 20 independent producer/consumer pairs, 500,000 msgs each (10M total)
#
# Reports best-of-3 wall-clock seconds (lower is better). Erlang and Go are
# skipped gracefully if their toolchains aren't installed, so the swarmrt
# column always runs. This is a RELATIVE harness (same machine, same run) —
# absolute numbers vary by hardware; the columns next to each other are the point.
#
#   ./bench/actor/run.sh              # default scheduler count (nproc)
#   SW_SCHEDULERS=1 ./bench/actor/run.sh   # swarmrt single-scheduler
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SWC="$ROOT/bin/swc"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

have() { command -v "$1" >/dev/null 2>&1; }
HAVE_ERL=0; have erl && have erlc && HAVE_ERL=1
HAVE_GO=0;  have go && HAVE_GO=1
[ -x "$SWC" ] || { echo "error: build the compiler first —  make swc libswarmrt"; exit 2; }

# best-of-3 wall-clock of a command (stdout discarded); prints seconds or "-"
best3() {
    local best=99999 t i
    for i in 1 2 3; do
        t=$({ /usr/bin/time -p "$@" >/dev/null; } 2>&1 | awk '/^real/{print $2}')
        [ -n "$t" ] && awk "BEGIN{exit !($t < $best)}" && best=$t
    done
    [ "$best" = 99999 ] && echo "-" || echo "$best"
}

BENCHES="spawn pingpong fanout parallel"

# Pre-build everything once.
for b in $BENCHES; do
    "$SWC" build "$HERE/$b.sw" -O -o "$WORK/${b}_sw" >/dev/null 2>&1 || echo "warn: swc build $b failed"
    [ "$HAVE_ERL" = 1 ] && (cd "$WORK" && erlc "$HERE/$b.erl" >/dev/null 2>&1)
    [ "$HAVE_GO"  = 1 ] && go build -o "$WORK/${b}_go" "$HERE/$b.go" >/dev/null 2>&1
done

printf '\n  swarmrt actor benchmarks — best-of-3 wall-clock seconds (lower is better)\n'
printf '  schedulers=%s   erlang=%s   go=%s\n\n' \
    "${SW_SCHEDULERS:-nproc}" \
    "$([ $HAVE_ERL = 1 ] && erl -noshell -eval 'io:format("~s",[erlang:system_info(otp_release)]),init:stop()' 2>/dev/null || echo skip)" \
    "$([ $HAVE_GO = 1 ] && go version 2>/dev/null | awk '{print $3}' || echo skip)"
printf '  %-10s %12s %12s %12s\n' bench swarmrt erlang go
printf '  %-10s %12s %12s %12s\n' ---------- -------- -------- --------
for b in $BENCHES; do
    sw=$(best3 env SW_QUIET=1 "$WORK/${b}_sw")
    er="-"; [ "$HAVE_ERL" = 1 ] && er=$(best3 erl -noshell -pa "$WORK" -eval "$b:main(), init:stop().")
    go="-"; [ "$HAVE_GO"  = 1 ] && go=$(best3 "$WORK/${b}_go")
    printf '  %-10s %12s %12s %12s\n' "$b" "$sw" "$er" "$go"
done
echo
