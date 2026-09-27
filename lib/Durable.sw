# Durable.sw — agent state that survives an OS-process restart, pure sw over
# the checkpoint / restore / checkpoint_delete builtins (SQLite, $SW_STATE_DB,
# default ./.swarm/state.db).
#
#   import Durable
#
#   # A step function gets the current state and returns
#   #   {'next', new_state}   → checkpoint new_state, call step again
#   #   {'done', result}      → clear the checkpoint, return result
#   fun step(s) {
#       if (map_get(s, "n") < 100) { {'next', map_put(s, "n", map_get(s, "n") + 1)} }
#       else { {'done', map_get(s, "n")} }
#   }
#   Durable.loop("job:42", %{"n" => 0}, fun(s) { step(s) })
#
# Kill the process at any point and run it again: loop() restores the last
# checkpointed state and carries on from there instead of starting over. The
# same holds inside a supervisor — a restarted child that calls loop() with
# the same key picks up where the crashed one left off.
#
# Semantics: the checkpoint is written AFTER each step returns, so a crash
# inside a step re-runs that step from the previous state (at-least-once).
# Keep side effects in a step idempotent, or record them in the state
# before performing them. State must be checkpointable: no pids or funs
# (store a registered name instead of a pid). A failed checkpoint panics —
# carrying on would silently give up durability; let the supervisor decide.

module Durable

export [loop, load, save, clear]

# load(key, init) — the last checkpointed state for `key`, or `init` when
# nothing is saved (first run, or after clear()).
fun load(key, init) {
    saved = restore(key)
    if (saved == nil) { init } else { saved }
}

# save(key, state) — checkpoint `state` under `key`; panics on failure.
fun save(key, state) {
    case checkpoint(key, state) {
        'ok' -> 'ok'
        {'error', reason} -> panic(f"Durable.save({key}): checkpoint failed: {reason}")
    }
}

# clear(key) — forget the saved state; the next load() returns its init.
fun clear(key) { checkpoint_delete(key) }

# loop(key, init, step) — restore, then step + checkpoint until step says
# {'done', result}; clears the checkpoint and returns result.
fun loop(key, init, step) { _run(key, load(key, init), step) }

fun _run(key, state, step) {
    case step(state) {
        {'next', s2} ->
            save(key, s2)
            _run(key, s2, step)
        {'done', result} ->
            clear(key)
            result
        other -> panic("Durable.loop(" ++ key ++ "): step must return {'next', state} or {'done', result}, got " ++ to_string(other))
    }
}
