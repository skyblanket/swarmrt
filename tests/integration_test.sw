module IntegrationTest

# Integration tests for all new runtime features:
#   - Selective receive & after 0
#   - Process introspection (process_info, process_list, registered)
#   - JSON encode/decode
#   - String utils (split, trim, upper, lower, starts_with, ends_with)
#   - ETS basics
#   - Default parameters
#   - Float precision

# === Selective receive: after 0 (non-blocking mailbox check) ===
fun test_after_0() {
    # Empty mailbox — after 0 should fire immediately
    result = receive {
        x -> "got something"
        after 0 {
            "empty"
        }
    }
    print("after_0 empty:", result)

    # Send to self, then after 0 should find it
    me = self()
    send(me, "hello_self")
    result2 = receive {
        msg -> msg
        after 0 {
            "missed"
        }
    }
    print("after_0 found:", result2)
}

# === Selective receive: pattern match skips non-matching ===
fun selective_worker() {
    # Receive only :ping, leave others in mailbox
    result = receive {
        'ping' -> "pong"
        after 2000 {
            "timeout"
        }
    }
    print("selective:", result)
}

fun test_selective_receive() {
    pid = spawn(selective_worker())
    sleep(50)
    # Send noise first, then the matching message
    send(pid, "noise1")
    send(pid, 42)
    send(pid, 'ping')
    sleep(200)
}

# === Process introspection ===
fun named_proc() {
    receive {
        'stop' -> 'ok'
    }
}

fun test_process_info() {
    me = self()
    info = process_info(me)
    print("info type:", typeof(info))
    print("info pid:", map_get(info, 'pid'))
    print("info status:", map_get(info, 'status'))
}

fun test_process_list() {
    procs = process_list()
    print("proc_list type:", typeof(procs))
    count = length(procs)
    # Should have at least main + schedulers
    print("proc count >= 1:", if (count >= 1) { "yes" } else { "no" })
}

fun test_registered() {
    pid = spawn(named_proc())
    sleep(50)
    register('test_named', pid)

    regs = registered()
    print("registered type:", typeof(regs))
    found = 0
    for entry in regs {
        # Each entry is {name_string, pid}
        entry_name = elem(entry, 0)
        if (entry_name == "test_named") {
            found = 1
        }
    }
    print("found test_named:", if (found == 1) { "yes" } else { "no" })

    # Clean up
    send(pid, 'stop')
    sleep(50)
}

# === JSON encode/decode ===
fun test_json() {
    # Encode
    data = %{name: "Alice", age: 30}
    json = json_encode(data)
    print("json_encode:", json)

    # Decode (json keys are atoms for dot access)
    decoded = json_decode("{\"x\":42,\"y\":\"hello\"}")
    print("json_decode x:", map_get(decoded, 'x'))
    print("json_decode y:", map_get(decoded, 'y'))

    # Round-trip list
    list_json = json_encode([1, 2, 3])
    print("json list:", list_json)
    back = json_decode(list_json)
    print("json round-trip:", back)
}

# === String utilities ===
fun test_string_utils() {
    # split
    parts = string_split("a,b,c", ",")
    print("split:", parts)
    print("split len:", length(parts))

    # trim
    trimmed = string_trim("  hello  ")
    print("trimmed:", trimmed)

    # upper / lower
    print("upper:", string_upper("hello"))
    print("lower:", string_lower("WORLD"))

    # starts_with / ends_with
    print("starts_with:", string_starts_with("hello world", "hello"))
    print("ends_with:", string_ends_with("hello world", "world"))
    print("not starts:", string_starts_with("hello", "xyz"))
}

# === ETS basics ===
fun test_ets() {
    t = ets_new()
    ets_put(t, "key1", "val1")
    ets_put(t, "key2", 42)
    ets_put(t, "key3", [1, 2, 3])

    print("ets get key1:", ets_get(t, "key1"))
    print("ets get key2:", ets_get(t, "key2"))
    print("ets get key3:", ets_get(t, "key3"))

    # Overwrite
    ets_put(t, "key1", "updated")
    print("ets overwrite:", ets_get(t, "key1"))

    # Delete
    ets_delete(t, "key2")
    print("ets deleted:", ets_get(t, "key2"))
}

# === Float precision ===
fun test_float_precision() {
    x = 3.141592653589793
    print("pi:", x)
    y = 0.1 + 0.2
    print("0.1+0.2:", y)
}

# === Default parameters ===
fun greet(name = "world", greeting = "hello") {
    greeting ++ " " ++ name
}

fun test_defaults() {
    print("default:", greet())
    print("one arg:", greet("Alice"))
    print("both:", greet("Bob", "hi"))
}

# === Main ===
fun main() {
    print("=== Integration Test ===")

    print("--- after 0 ---")
    test_after_0()

    print("--- selective receive ---")
    test_selective_receive()

    print("--- process info ---")
    test_process_info()

    print("--- process list ---")
    test_process_list()

    print("--- registered ---")
    test_registered()

    print("--- json ---")
    test_json()

    print("--- string utils ---")
    test_string_utils()

    print("--- ets ---")
    test_ets()

    print("--- float precision ---")
    test_float_precision()

    print("--- defaults ---")
    test_defaults()

    print("=== All integration tests passed ===")
}
