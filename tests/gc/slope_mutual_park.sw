# slope_mutual_park.sw — memory-slope gate for mutual tail calls that suspend.
#
# A body entered through a mutual tail-call hop that suspended at its entry
# used to resume without its chain's arena floor and recompute a higher one,
# so each resume raised the floor and turn checkpoints reclaimed less
# (Phase 2c review, PR #192: 605 s and 104 MB for 400K turns under
# SW_SUSPEND_TORTURE=50; fixed: 0.35 s, flat). The gate runs this under
# SW_SUSPEND_TORTURE so entry suspensions happen every few turns; RSS must
# stay flat as the turn count grows.
module Main

fun a(n, parent) {
    if (n == 0) { send(parent, 'done') }
    else { junk = [n, n + 1, n + 2, to_string(n)]  b(n - 1, parent) }
}
fun b(n, parent) { x = to_string(n) ++ "-pad"  a(n, parent) }

fun job_count(xs) { if (length(xs) >= 2) { to_int(hd(tl(xs))) } else { 100000 } }

fun main() {
    rounds = job_count(os_args())
    print("slope_mutual_park rounds=" ++ to_string(rounds))
    me = self()
    spawn(a(rounds, me))
    receive { 'done' -> 0 after 120000 { print("TIMEOUT") } }
    print("PROBE_OK")
    0
}
