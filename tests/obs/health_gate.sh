#!/usr/bin/env bash
# health_gate.sh — gate for the Phase-4 health/readiness endpoint
# (lib/Health.sw over http_listen + swarm_stats).
#
# Boots examples/health_endpoint.sw on a loopback port and asserts the
# operator contract end-to-end with a real HTTP client (curl):
#   /healthz  → 200 with body "ok"        (liveness)
#   /readyz   → 200 with the swarm_stats() JSON — must carry the real
#               metric keys ("processes", "schedulers", "crashes",
#               "scheduler_stats"), not just any 200
#   /anything → 404                       (no accidental catch-all 200)
#
# Expects bin/health_demo prebuilt (make health-gate does this).
# Port 18123, loopback only.
set -u
REPO="${SWARMRT_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/../.." && pwd)}"
cd "$REPO" || { echo "health-gate: FAIL (no repo)"; exit 99; }

PORT="${HEALTH_GATE_PORT:-18123}"
BASE="http://127.0.0.1:$PORT"

SERVER_PID=""
cleanup() { [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

PORT="$PORT" ./bin/health_demo >bin/_hg.out 2>&1 &
SERVER_PID=$!

up=0
for _ in $(seq 1 100); do   # poll until the server answers (max ~10s)
    if curl -s -o /dev/null --max-time 2 "$BASE/healthz"; then up=1; break; fi
    sleep 0.1
done
if [ "$up" -ne 1 ]; then
    echo "health-gate: FAIL (server did not come up on :$PORT)"
    cat bin/_hg.out; exit 1
fi

fail() { echo "health-gate: FAIL ($1)"; cat bin/_hg.out; exit 1; }

# 1. liveness: 200 + "ok"
code=$(curl -s -o bin/_hg.body -w '%{http_code}' --max-time 5 "$BASE/healthz") || fail "curl /healthz"
[ "$code" = "200" ] || fail "/healthz status $code, want 200"
grep -q '^ok$' bin/_hg.body || fail "/healthz body is not 'ok'"

# 2. readiness: 200 + real swarm_stats JSON (the metric keys, not just a 200)
code=$(curl -s -o bin/_hg.body -w '%{http_code}' --max-time 5 "$BASE/readyz") || fail "curl /readyz"
[ "$code" = "200" ] || fail "/readyz status $code, want 200"
for key in '"processes"' '"schedulers"' '"spawns"' '"crashes"' '"restarts"' '"scheduler_stats"'; do
    grep -q "$key" bin/_hg.body || fail "/readyz JSON missing $key"
done

# 3. unknown path: 404, not an accidental catch-all 200
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE/definitely-not-a-route") || fail "curl 404 probe"
[ "$code" = "404" ] || fail "unknown path answered $code, want 404"

rm -f bin/_hg.out bin/_hg.body
echo "health-gate: PASS (/healthz 200 ok, /readyz 200 with live swarm_stats JSON, 404 elsewhere)"
