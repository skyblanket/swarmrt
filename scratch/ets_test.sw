module ETSTest

fun main() {
    print("=== ETS Test ===")
    
    table = ets_new()
    ets_put(table, "key1", "value1")
    ets_put(table, "key2", 42)
    ets_put(table, "key3", [1, 2, 3])
    
    print("key1:", ets_get(table, "key1"))
    print("key2:", ets_get(table, "key2"))
    print("key3:", ets_get(table, "key3"))
    print("missing:", ets_get(table, "missing"))
    
    # JSON test
    data = %{name: "test", value: 123}
    json = json_encode(data)
    print("JSON:", json)
    
    decoded = json_decode(json)
    print("Decoded name:", decoded.name)
    
    print("=== Done ===")
}
