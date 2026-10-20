module Test_telemetry_names

# Telemetry.names() must report every distinct event name emitted through
# the hub, not a hard-coded empty list. (Compiled suite: the hub is a
# real process, so it needs the scheduler.)

import Telemetry

fun has(lst, v) {
    if (length(lst) == 0) { 0 }
    else { if (hd(lst) == v) { 1 } else { has(tl(lst), v) } }
}

fun main() {
    failures = 0
    Telemetry.configure(%{sinks: []})
    Telemetry.emit("metric.a", %{v: 1})
    Telemetry.emit("metric.b", %{v: 2})
    Telemetry.emit("metric.a", %{v: 3})
    Telemetry.flush()

    got = Telemetry.names()
    has_a = has(got, "metric.a")
    has_b = has(got, "metric.b")
    print(f"names: {got}")

    failures = failures + (if (has_a == 1) { print("PASS names_has_a") ; 0 }
                           else { print("FAIL names_has_a") ; 1 })
    failures = failures + (if (has_b == 1) { print("PASS names_has_b") ; 0 }
                           else { print("FAIL names_has_b") ; 1 })
    failures = failures + (if (length(got) == 2) { print("PASS names_distinct") ; 0 }
                           else { print("FAIL names_distinct") ; 1 })

    if (failures == 0) { print("OK test_telemetry_names 3/3") ; sys_exit(0) }
    else { print(f"FAIL test_telemetry_names: {failures}") ; sys_exit(1) }
}
