#!/usr/bin/env bash
# http_keepalive_gate.sh — concurrency gate for HTTP response delivery.
#
# Phase 1: 50 concurrent keep-alive clients each POST 100 times to an echo
# handler that replies at once, with SW_SCHEDULERS at 2, 4 and 8. Every
# request must get exactly one reply, and it must be its own body (no reply
# routed to another request, no duplicate, no empty reply).
#
# Phase 2: a handler that answers ~20 ms late, with SW_HTTP_IDLE_TIMEOUT_MS at
# 100. The requests queue behind one handler, so their replies are owed for
# longer than the idle timeout. The server must not close a connection whose
# reply it still owes: before the fix, the idle sweep dropped most of these
# clients with an empty reply.
#
# Phase 3: pipelining. One keep-alive connection sends request A, then request
# B 5 ms later, while A's reply (20 ms handler) is still owed. Both must get
# their own reply. A single in-flight flag let A's reply clear it, so B's
# reply was dropped. Repeated on fresh connections.
#
# Expects bin/http_keepalive_server, bin/http_keepalive_slow_server and
# bin/http_keepalive_client prebuilt (make http-keepalive-gate does this).
# Port 9352, loopback only.
set -u
REPO="${SWARMRT_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/../.." && pwd)}"
cd "$REPO" || { echo "http-keepalive-gate: FAIL (no repo)"; exit 99; }

PORT=9352
CLIENTS=50
REQS=100
ulimit -n 4096 2>/dev/null || true

SERVER_PID=""
cleanup() { [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

start_server() {  # $1 = server binary, $2 = scheduler count, $3 = idle timeout (ms)
    SW_SCHEDULERS="$2" SW_HTTP_IDLE_TIMEOUT_MS="$3" "./bin/$1" >/dev/null 2>bin/_ka.err &
    SERVER_PID=$!
    for _ in $(seq 1 100); do
        if ./bin/http_keepalive_client "$PORT" 1 1 >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    echo "http-keepalive-gate: FAIL (server did not come up)"; cat bin/_ka.err; exit 1
}

stop_server() {
    kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null; SERVER_PID=""
    sleep 0.3   # let the port drain
}

# --- Phase 1: fast handler, every scheduler count ----------------------------
for s in 2 4 8; do
    start_server http_keepalive_server "$s" ""
    if ./bin/http_keepalive_client "$PORT" "$CLIENTS" "$REQS" >bin/_ka.out 2>bin/_ka.err2; then
        echo "http-keepalive-gate: phase 1 OK (SW_SCHEDULERS=$s: $(cat bin/_ka.out))"
    else
        echo "http-keepalive-gate: FAIL (phase 1, SW_SCHEDULERS=$s)"
        cat bin/_ka.err2; cat bin/_ka.out; cat bin/_ka.err; exit 1
    fi
    stop_server
done

# --- Phase 2: slow handler against a short idle timeout ----------------------
start_server http_keepalive_slow_server 4 100
if ./bin/http_keepalive_client "$PORT" "$CLIENTS" 4 >bin/_ka.out 2>bin/_ka.err2; then
    echo "http-keepalive-gate: phase 2 OK (owed replies outlive the idle timeout: $(cat bin/_ka.out))"
else
    echo "http-keepalive-gate: FAIL (phase 2 — a connection was closed while its reply was owed)"
    cat bin/_ka.err2; cat bin/_ka.out; cat bin/_ka.err; exit 1
fi
stop_server

# --- Phase 3: pipelined B while A's reply is still owed ----------------------
# Sends A, waits 5 ms (A's 20 ms reply is still owed), sends B on the same
# socket, then reads until the server goes quiet. Exactly one AAA and one BBB
# must come back.
pipelined_probe() {  # $1 = output file
    exec 3<>/dev/tcp/127.0.0.1/$PORT || return 1
    printf 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nAAA' >&3
    sleep 0.005
    printf 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nBBB' >&3
    timeout 2 cat <&3 >"$1"
    exec 3>&-
    [ "$(grep -o AAA "$1" | wc -l)" -eq 1 ] && [ "$(grep -o BBB "$1" | wc -l)" -eq 1 ]
}

start_server http_keepalive_slow_server 4 ""
for i in $(seq 1 20); do
    if ! pipelined_probe bin/_ka.pipe; then
        echo "http-keepalive-gate: FAIL (phase 3 — pipelined request $i lost its reply)"
        cat bin/_ka.pipe; cat bin/_ka.err; exit 1
    fi
done
echo "http-keepalive-gate: phase 3 OK (20 pipelined pairs, each answered twice)"
stop_server

rm -f bin/_ka.err bin/_ka.err2 bin/_ka.out bin/_ka.pipe
echo "http-keepalive-gate: PASS"
exit 0
