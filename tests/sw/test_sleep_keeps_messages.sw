module Test_sleep_keeps_messages

# Regression: sleep(ms) must not consume messages. It used to wait with a
# receive-any and free whatever arrived, so a reply that landed during a
# sleep was silently lost.

fun main() {
    me = self()
    spawn(fun() { send(me, {'hello', 1}) ; send(me, {'hello', 2}) })
    sleep(200)
    a = receive { {'hello', n} -> n after 1000 -> 'lost' }
    b = receive { {'hello', n} -> n after 1000 -> 'lost' }
    t0 = timestamp()
    sleep(150)
    slept = timestamp() - t0
    fails = 0
    if (a == 1 && b == 2) { print("PASS kept_in_order") } else { print(f"FAIL kept_in_order: {a} {b}") ; fails = 1 }
    if (slept >= 140) { print("PASS slept_full") } else { print(f"FAIL slept_full: {slept}ms") ; fails = fails + 1 }
    if (fails == 0) { print("OK sleep_keeps_messages 2/2") ; sys_exit(0) }
    else { print("FAIL sleep_keeps_messages") ; sys_exit(1) }
}
