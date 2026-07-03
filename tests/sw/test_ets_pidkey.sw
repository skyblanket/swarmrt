module Test_ets_pidkey

# Regression (independent review B2): ETS with a PID key. _vets_key_eq compares
# pids by NUMERIC id, but _vets_hash_val used to hash the pid by POINTER (the
# default case) — so two equal pids that were different arena-copied sw_val_t
# landed in different buckets yet compared equal, breaking the hash invariant:
# a pid-keyed put never found the prior entry, so the table accumulated
# DUPLICATES and lookups/deletes missed. Now _vets_hash_val hashes a pid by id
# too. This locks the contract in BOTH backends (compiled here + run/ copy).

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun test_pid_key_dedup() {
    t = ets_new()
    me = self()
    fails = 0
    ets_put(t, me, "a")
    ets_put(t, me, "b")          # same pid key → REPLACE, not a second entry
    fails = fails + assert_eq("pid_put_replaces_not_adds", ets_count(t), 1)
    fails = fails + assert_eq("pid_get_returns_latest", ets_get(t, me), "b")
    ets_delete(t, me)            # delete must find the pid-keyed entry
    fails = fails + assert_eq("pid_delete_removes", ets_count(t), 0)
    fails = fails + assert_eq("pid_get_after_delete", ets_get(t, me), nil)
    fails
}

# Two DISTINCT pids remain distinct keys (the fix must not collapse them).
fun echo() { receive { {'who', from} -> send(from, self()) ; echo() } }

fun test_two_pids_distinct() {
    t = ets_new()
    p = spawn(echo())
    send(p, {'who', self()})
    other = receive { pid -> pid after 5000 { self() } }
    fails = 0
    ets_put(t, self(), "mine")
    ets_put(t, other, "theirs")
    fails = fails + assert_eq("two_pids_two_entries", ets_count(t), 2)
    fails = fails + assert_eq("first_pid_intact", ets_get(t, self()), "mine")
    fails = fails + assert_eq("second_pid_intact", ets_get(t, other), "theirs")
    fails
}

fun main() {
    fails = 0
    fails = fails + test_pid_key_dedup()
    fails = fails + test_two_pids_distinct()
    if (fails == 0) { print("OK ets_pidkey 7/7") ; sys_exit(0) }
    else { print("FAIL ets_pidkey " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
