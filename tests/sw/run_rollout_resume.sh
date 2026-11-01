#!/bin/bash
# Rollout.run resume after SIGKILL (lib/Rollout.sw, issue #64).
#
# A run of 40 episodes, 4 at a time, every 5th episode a timeout, is killed
# with SIGKILL partway and restarted on the same JSONL file with resume. Checks:
#   - every episode ends up with exactly one row (no duplicates, none lost);
#   - the restart runs only the episodes that had no row;
#   - episodes started twice are at most the ones in flight at the kill (4):
#     failures were written as they happened, so none of them re-ran.
#
# Needs bin/swc (make swc). Exit non-zero if a check fails.
set -u
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/resume_child.sw" <<'SW'
module Resume_child
import Std
import Rollout

fun main() {
    out = getenv("OUT")
    starts = getenv("STARTS")
    tasks = Std.map(Std.range(0, 40), fn(i) { %{id: i} })
    s = Rollout.run(tasks, %{run: fn(task, ctx) {
            id = map_get(task, 'id')
            file_append(starts, f"{id}\n")
            if (id % 5 == 0) { sleep(60000) }
            sleep(250)
            {1, nil}
        }}, %{max_concurrency: 4, episode_timeout_ms: 400, out: out, resume: 'true'})
    print(f"ran={map_get(s, 'ran')} skipped={map_get(s, 'skipped')}")
}
SW
"$SWC" build "$WORK/resume_child.sw" -o "$WORK/resume_child" >"$WORK/build.log" 2>&1 \
    || { cat "$WORK/build.log"; echo "FAIL rollout_resume: build"; exit 1; }

export OUT="$WORK/out.jsonl" STARTS="$WORK/starts.txt"
"$WORK/resume_child" >"$WORK/run1.log" 2>&1 &
pid=$!
sleep 1.5
kill -9 "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
before=$(grep -c . "$OUT" 2>/dev/null || echo 0)
"$WORK/resume_child" >"$WORK/run2.log" 2>&1 || { cat "$WORK/run2.log"; echo "FAIL rollout_resume: restart"; exit 1; }

python3 - "$OUT" "$STARTS" "$before" "$WORK/run2.log" <<'PY'
import collections, json, re, sys
out, starts, before, log = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
rows = [json.loads(l) for l in open(out) if l.strip()]
ids = collections.Counter(r["task_id"] for r in rows)
fails = []
if not (0 < before < 40):
    fails.append(f"kill landed outside the run ({before} rows before it); the check proves nothing")
if sorted(ids) != list(range(40)):
    fails.append(f"episodes with rows: {len(ids)} of 40")
dups = {i: c for i, c in ids.items() if c > 1}
if dups:
    fails.append(f"duplicate rows: {dups}")
for r in rows:
    want = "timeout" if r["task_id"] % 5 == 0 else "done"
    if r["kind"] != want:
        fails.append(f"episode {r['task_id']}: kind {r['kind']}, want {want}")
m = re.search(r"ran=(\d+) skipped=(\d+)", open(log).read())
if not m or int(m.group(1)) != 40 - before or int(m.group(2)) != before:
    fails.append(f"restart summary {m and m.group(0)}, want ran={40 - before} skipped={before}")
started = collections.Counter(int(l) for l in open(starts) if l.strip())
twice = [i for i, c in started.items() if c > 1]
if len(twice) > 4:
    fails.append(f"{len(twice)} episodes re-ran after the kill (more than the 4 in flight): {twice}")
pre_kill_failures = [r["task_id"] for r in rows[:before] if r["kind"] != "done"]
rerun_failures = [i for i in pre_kill_failures if started[i] > 1]
if rerun_failures:
    fails.append(f"failures recorded before the kill re-ran: {rerun_failures}")
for f in fails:
    print("FAIL rollout_resume:", f)
if fails:
    sys.exit(1)
print(f"OK rollout_resume ({before} rows before SIGKILL, {len(twice)} re-run in flight, "
      f"{len(pre_kill_failures)} failures kept)")
PY
