module Phase13Test

fun main() {
    print("=== Phase 13: Agent Stdlib Batteries ===")
    print("")

    # --- JSON encode/decode ---
    print("--- json_encode / json_decode ---")
    data = %{name: "mally", version: 13, active: 'true'}
    encoded = json_encode(data)
    print("encoded:", encoded)

    decoded = json_decode(encoded)
    print("decoded.name:", decoded.name)
    print("decoded.version:", decoded.version)

    # Nested structures
    nested = %{agents: [1, 2, 3], config: %{model: "gpt-4o", temp: 0.7}}
    print("nested:", json_encode(nested))

    # Decode a raw JSON string
    raw = "{\"status\":\"ok\",\"count\":42,\"tags\":[\"ai\",\"runtime\"]}"
    parsed = json_decode(raw)
    print("parsed.status:", parsed.status)
    print("parsed.count:", parsed.count)
    print("")

    # --- Shell ---
    print("--- shell ---")
    result = shell("echo hello from swarmrt")
    print("exit code:", elem(result, 0))
    print("output:", string_trim(elem(result, 1)))

    result2 = shell("ls /tmp | head -3")
    print("ls result:", string_trim(elem(result2, 1)))
    print("")

    # --- File I/O extensions ---
    print("--- file_exists / file_list / file_delete / file_append ---")
    file_write("/tmp/sw_test_phase13.txt", "hello phase 13")
    print("exists:", file_exists("/tmp/sw_test_phase13.txt"))

    file_append("/tmp/sw_test_phase13.txt", "\nline two")
    content = file_read("/tmp/sw_test_phase13.txt")
    print("after append:", content)

    files = file_list("/tmp")
    print("files in /tmp:", length(files), "entries")

    file_delete("/tmp/sw_test_phase13.txt")
    print("after delete:", file_exists("/tmp/sw_test_phase13.txt"))
    print("")

    # --- String utilities ---
    print("--- string_split / string_trim / string_upper / string_lower ---")
    parts = string_split("hello,world,foo", ",")
    print("split:", parts)
    print("split length:", length(parts))

    print("trim:", string_trim("  hello  "))
    print("upper:", string_upper("hello world"))
    print("lower:", string_lower("HELLO WORLD"))
    print("starts_with:", string_starts_with("hello world", "hello"))
    print("ends_with:", string_ends_with("hello world", "world"))
    print("")

    # --- HTTP GET ---
    print("--- http_get ---")
    resp = http_get("https://httpbin.org/get")
    print("http_get response length:", string_length(resp))
    print("")

    # --- Timers ---
    print("--- delay / interval ---")
    cb1 = fun() { print("delay 100ms: fired!") }
    delay(100, cb1)

    cb2 = fun() { print("tick!") }
    timer_pid = interval(200, cb2)

    # Let timers run briefly
    sleep(700)
    print("")

    print("=== Phase 13 tests complete! ===")
}
