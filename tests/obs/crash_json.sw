module CrashJson

# Fixture for `make crashlog-gate` (Phase-4 structured crash logs).
#
# A REGISTERED, monitored child panics; main survives the crash (fault
# isolation) and prints PROBE_OK. Run with SW_LOG_JSON=1, stderr must carry
# one {"ev":"proc_crash",...} JSON record with the panic msg and the
# registered name; run without it, stderr must carry none (the default
# surface stays the human-readable panic trace). The gate greps both ways.

fun named_boom() {
    register('json_gate_victim', self())
    panic("json_gate_boom")
}

fun main() {
    res = spawn_monitor(named_boom())
    receive {
        {'DOWN', _, _, _, _} -> 'ok'
        after 3000 { print("NO_DOWN") ; sys_exit(1) }
    }
    print("PROBE_OK")
}
