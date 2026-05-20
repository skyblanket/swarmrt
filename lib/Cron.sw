# Cron.sw — wake scheduler for autonomous agents.
#
# Spawn a sw process that wakes itself periodically and invokes a
# callback. Use it for:
#   - background polling ("check inbox every 30s")
#   - heartbeats / health checks
#   - timed cleanup ("run housekeeping every hour")
#   - autonomy loops ("self-prompt every N minutes")
#
# Three flavours:
#
#   Cron.every(interval_ms, fn)
#     Calls fn() every interval_ms. Returns the wake pid; send
#     it 'stop' to kill.
#
#   Cron.at("HH:MM", fn)
#     Calls fn() once per day at local HH:MM (24-hour). Computes the
#     next firing using `timestamp()` + the system's local-time
#     offset (read from /bin/date on first call; cached).
#
#   Cron.in_ms(delay_ms, fn)
#     One-shot — fires fn() once after delay_ms. (Named `in_ms`
#     because `after` is a sw keyword.)
#
# The wake processes call `fn()` synchronously, then loop. If fn
# panics, the wake process dies (use try/catch inside fn for
# recoverable errors).
#
# These are pure sw — they use `spawn` + `sleep` + `receive`. No
# new builtins needed.

module Cron
# Note: `after` is a sw keyword (used in `receive ... after N { ... }`),
# so the one-shot timer is named `in_ms` instead.
export [every, at, in_ms, stop]

fun every(interval_ms, fn) {
    spawn(every_loop(interval_ms, fn))
}

fun every_loop(interval_ms, fn) {
    receive {
        'stop' -> 'done'
        after interval_ms {
            fn()
            every_loop(interval_ms, fn)
        }
    }
}

fun in_ms(delay_ms, fn) {
    spawn(after_loop(delay_ms, fn))
}

fun after_loop(delay_ms, fn) {
    receive {
        'stop' -> 'done'
        after delay_ms {
            fn()
            'done'
        }
    }
}

# at("14:30", fn) — fires daily at HH:MM local time.
# Implementation: compute ms until next HH:MM using `date +%s` for
# epoch and `date +%H,%M,%S` for current local time. Sleeps that
# long, fires, then loops back for the next day.
fun at(hhmm, fn) {
    parts = string_split(hhmm, ":")
    if (length(parts) != 2) { panic(f"Cron.at: expected HH:MM, got {hhmm}") }
    else {
        target_h = to_int(hd(parts))
        target_m = to_int(hd(tl(parts)))
        spawn(at_loop(target_h, target_m, fn))
    }
}

fun at_loop(target_h, target_m, fn) {
    wait_ms = ms_until(target_h, target_m)
    receive {
        'stop' -> 'done'
        after wait_ms {
            fn()
            at_loop(target_h, target_m, fn)
        }
    }
}

# Helper: ms until the next target_h:target_m local time. Uses
# `date` since sw doesn't expose strftime. If the target is in the
# past today, schedule for tomorrow.
fun ms_until(target_h, target_m) {
    r = shell("date +%H:%M:%S")
    out = string_trim(elem(r, 1))
    bits = string_split(out, ":")
    if (length(bits) < 3) { 60000 }  # fallback: 1 min retry
    else {
        h = to_int(elem(bits, 0))
        m = to_int(elem(bits, 1))
        s = to_int(elem(bits, 2))
        now_secs = h * 3600 + m * 60 + s
        target_secs = target_h * 3600 + target_m * 60
        diff = if (target_secs > now_secs) { target_secs - now_secs }
               else { 86400 - now_secs + target_secs }
        diff * 1000
    }
}

# Send 'stop' to a wake pid started by any of the above.
fun stop(wake_pid) {
    send(wake_pid, 'stop')
}

# ---- helpers ----

fun to_int(s) {
    # Cheap atoi via shell — sw doesn't currently expose strtol.
    # Returns 0 on parse failure.
    r = shell(f"printf %d {s} 2>/dev/null || echo 0")
    raw = string_trim(elem(r, 1))
    case raw {
        "" -> 0
        _  -> parse_digits(raw, 0, 0)
    }
}

fun parse_digits(s, i, acc) {
    if (i >= string_length(s)) { acc }
    else {
        ch = string_sub(s, i, 1)
        d = digit_value(ch)
        case d {
            -1 -> acc
            _  -> parse_digits(s, i + 1, acc * 10 + d)
        }
    }
}

fun digit_value(ch) {
    case ch {
        "0" -> 0 ; "1" -> 1 ; "2" -> 2 ; "3" -> 3 ; "4" -> 4
        "5" -> 5 ; "6" -> 6 ; "7" -> 7 ; "8" -> 8 ; "9" -> 9
        _   -> -1
    }
}
