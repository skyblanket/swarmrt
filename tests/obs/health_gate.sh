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
#   /processes, /processes?limit=1, /tree, /top?by=..&n=..
#             → 200 JSON with the documented shape; the demo's "backlog"
#               process (50 undrained messages) must top /top?by=mailbox
#   malformed /top and /processes queries → 400, never a defaulted 200
#   /anything → 404                       (no accidental catch-all 200)
#
# Expects bin/health_demo prebuilt (make health-gate does this).
# Port 18123, loopback only.
set -u
REPO="${SWARMRT_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/../.." && pwd)}"
cd "$REPO" || { echo "health-gate: FAIL (no repo)"; exit 99; }
. "$REPO/scripts/gate_common.sh"
gate_print_limits health-gate
gate_need_cmd curl "sudo apt-get install curl" || { echo "health-gate: FAIL (missing prerequisite, see above)"; exit 2; }
gate_need_cmd python3 "sudo apt-get install python3" || { echo "health-gate: FAIL (missing prerequisite, see above)"; exit 2; }

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

# 2b. /readyz carries node RSS and per-scheduler utilization (#168)
python3 - bin/_hg.body <<'PY' || fail "/readyz resident_bytes / utilization"
import json, sys
d = json.load(open(sys.argv[1]))
assert isinstance(d["resident_bytes"], int) and d["resident_bytes"] > 0, d["resident_bytes"]
for sc in d["scheduler_stats"]:
    assert 0 <= sc["utilization"] <= 100, sc
PY

# 2c. JSON introspection (#168). get PATH -> body in bin/_hg.body, echoes code.
get() { curl -s -o bin/_hg.body -w '%{http_code}' --max-time 10 "$BASE$1"; }
ROW_KEYS='["pid","parent","name","status","mailbox_len","memory","resident_bytes","messages_sent","messages_recv"]'

code=$(get /processes) || fail "curl /processes"
[ "$code" = "200" ] || fail "/processes status $code, want 200"
python3 - bin/_hg.body "$ROW_KEYS" <<'PY' || fail "/processes shape"
import json, sys
rows, keys = json.load(open(sys.argv[1])), json.loads(sys.argv[2])
assert len(rows) >= 3, rows                       # main, worker, backlog
for r in rows:
    assert sorted(r) == sorted(keys), r
assert any(r["name"] == "backlog" and r["mailbox_len"] == 50 for r in rows), rows
PY

code=$(get "/processes?limit=1") || fail "curl /processes?limit=1"
[ "$code" = "200" ] || fail "/processes?limit=1 status $code, want 200"
python3 - bin/_hg.body <<'PY' || fail "/processes?limit=1 is not 1 row + 1 summary row"
import json, sys
rows = json.load(open(sys.argv[1]))
assert len(rows) == 2, rows
s = rows[1]
assert s["summary"] is True and s["omitted"] >= 2, s
assert s["mailbox_len"] >= 50, s                  # the backlog is in the rest
PY

code=$(get /tree) || fail "curl /tree"
[ "$code" = "200" ] || fail "/tree status $code, want 200"
python3 - bin/_hg.body <<'PY' || fail "/tree is not a forest of nodes with children"
import json, sys
roots = json.load(open(sys.argv[1]))
assert roots and all(isinstance(r["children"], list) and "pid" in r for r in roots), roots
PY

code=$(get "/top?by=mailbox&n=1") || fail "curl /top"
[ "$code" = "200" ] || fail "/top?by=mailbox&n=1 status $code, want 200"
python3 - bin/_hg.body <<'PY' || fail "/top?by=mailbox&n=1 does not name the backlog"
import json, sys
rows = json.load(open(sys.argv[1]))
assert len(rows) == 1 and rows[0]["name"] == "backlog" and rows[0]["mailbox_len"] == 50, rows
PY
for by in memory resident rate; do
    code=$(get "/top?by=$by&n=2") || fail "curl /top?by=$by"
    [ "$code" = "200" ] || fail "/top?by=$by status $code, want 200"
    python3 - bin/_hg.body "$by" <<'PY' || fail "/top?by=$by not ranked worst-first"
import json, sys
rows, by = json.load(open(sys.argv[1])), sys.argv[2]
key = {"memory": lambda r: r["memory"], "resident": lambda r: r["resident_bytes"],
       "rate": lambda r: r["messages_sent"] + r["messages_recv"]}[by]
assert len(rows) == 2 and key(rows[0]) >= key(rows[1]), rows
PY
done

# 2d. malformed queries are a loud 400, not a silently-defaulted 200
for q in "/top?by=bogus" "/top?n=0" "/top?n=abc" "/processes?limit=12x" "/processes?limit=-1"; do
    code=$(get "$q") || fail "curl $q"
    [ "$code" = "400" ] || fail "$q answered $code, want 400"
done

# 3. unknown path: 404, not an accidental catch-all 200
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE/definitely-not-a-route") || fail "curl 404 probe"
[ "$code" = "404" ] || fail "unknown path answered $code, want 404"

rm -f bin/_hg.out bin/_hg.body
echo "health-gate: PASS (/healthz, /readyz, /processes, /tree, /top JSON; 400 on bad queries; 404 elsewhere)"
