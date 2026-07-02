#!/usr/bin/env bash
# slowloris_gate.sh — bidirectional gate for the HTTP idle timeout
# (SW_HTTP_IDLE_TIMEOUT_MS; Phase 3 limits & quotas).
#
# Phase 1 (fix ON, 500ms timeout): fill the connection table with idle
# sockets + one established-but-quiet WS conn. The sweep must close every
# idle HTTP conn (slots freed), the WS conn must survive (established WS
# exempt by default), and a real request must then succeed.
# Phase 2 (fix OFF, SW_HTTP_IDLE_TIMEOUT_MS=0): the same attack pins all
# SW_HTTP_MAX_CONNS slots forever — zero closes, and a real request FAILS.
# Phase 2 is what the server looked like before the fix, so the gate has
# been seen to FAIL (well: demonstrate the DoS) without it — bidirectional.
#
# Expects bin/slowloris_server + bin/slowloris_client prebuilt (make
# slowloris-gate does this). Port 9351, loopback only.
set -u
REPO="${SWARMRT_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/../.." && pwd)}"
cd "$REPO" || { echo "slowloris-gate: FAIL (no repo)"; exit 99; }

PORT=9351
MAX_CONNS=256           # keep in sync with SW_HTTP_MAX_CONNS
ulimit -n 4096 2>/dev/null || true

SERVER_PID=""
cleanup() { [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

wait_up() {  # poll with the probe until the server answers (max ~10s)
    for _ in $(seq 1 100); do
        if ./bin/slowloris_client probe "$PORT"; then return 0; fi
        sleep 0.1
    done
    return 1
}

start_server() {  # $1 = SW_HTTP_IDLE_TIMEOUT_MS value ("" = unset/default off for gate)
    SW_HTTP_IDLE_TIMEOUT_MS="$1" ./bin/slowloris_server >/dev/null 2>bin/_sl.err &
    SERVER_PID=$!
    if ! wait_up; then
        echo "slowloris-gate: FAIL (server did not come up)"; cat bin/_sl.err; exit 1
    fi
}

stop_server() {
    kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null; SERVER_PID=""
    sleep 0.3   # let the port drain
}

# --- Phase 1: fix ON (500ms idle timeout) ------------------------------------
start_server 500
# 1 WS conn + (MAX_CONNS-1) idle conns = full table; sweep must free them all.
if ./bin/slowloris_client sweep "$PORT" $((MAX_CONNS - 1)); then
    echo "slowloris-gate: phase 1 OK (idle conns closed, WS survived, request served)"
else
    echo "slowloris-gate: FAIL (phase 1 — idle timeout not enforcing)"; cat bin/_sl.err; exit 1
fi
stop_server

# --- Phase 2: fix OFF (timeout disabled = pre-fix behavior) -------------------
start_server 0
if ./bin/slowloris_client pinned "$PORT" "$MAX_CONNS"; then
    echo "slowloris-gate: phase 2 OK (timeout off: slots pinned, request starved — the DoS the fix closes)"
else
    echo "slowloris-gate: FAIL (phase 2 — expected pinned slots with timeout disabled)"; cat bin/_sl.err; exit 1
fi
stop_server

rm -f bin/_sl.err
echo "slowloris-gate: PASS (bidirectional)"
exit 0
