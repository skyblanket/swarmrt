module Phase12Test

# Test all Phase 12 language features

fun test_elsif() {
    x = 2
    result = if (x == 1) {
        "one"
    } elsif (x == 2) {
        "two"
    } elsif (x == 3) {
        "three"
    } else {
        "other"
    }
    print("elsif:", result)
}

fun test_string_interp() {
    name = "world"
    age = 42
    msg = "Hello #{name}, you are #{age} years old!"
    print("interp:", msg)
    # Escaped hash should not interpolate
    escaped = "Price is \#100"
    print("escaped:", escaped)
}

fun test_maps() {
    m = %{name: "Alice", age: 30}
    print("map:", m)
    print("name:", m.name)
    print("age:", map_get(m, 'age'))

    m2 = map_put(m, 'city', "NYC")
    print("updated:", m2)
    print("keys:", map_keys(m2))
    print("values:", map_values(m2))
    print("has city:", map_has_key(m2, 'city'))
    print("has zip:", map_has_key(m2, 'zip'))
}

fun test_for_loop() {
    # For over list
    items = [10, 20, 30]
    for x in items {
        print("item:", x)
    }
    # For over range
    for i in 1..5 {
        print("range:", i)
    }
}

fun test_try_catch() {
    result = try {
        error("something broke")
        "unreachable"
    } catch e {
        print("caught:", e)
        "recovered"
    }
    print("try result:", result)
}

fun test_list_cons() {
    lst = [1, 2, 3, 4, 5]
    h = hd(lst)
    t = tl(lst)
    print("head:", h)
    print("tail:", t)
    # Build list with cons
    new_list = [0 | lst]
    print("cons:", new_list)
}

fun test_defaults(greeting = "hello", name = "stranger") {
    print(greeting, name)
}

fun test_dot_access() {
    config = %{host: "localhost", port: 8080}
    print("host:", config.host)
    print("port:", config.port)
}

fun test_typeof() {
    print("typeof 42:", typeof(42))
    print("typeof str:", typeof("hello"))
    print("typeof atom:", typeof('ok'))
    print("typeof list:", typeof([1,2]))
    print("typeof map:", typeof(%{a: 1}))
}

fun main() {
    print("=== Phase 12 Language Tests ===")
    test_elsif()
    test_string_interp()
    test_maps()
    test_for_loop()
    test_try_catch()
    test_list_cons()
    test_defaults()
    test_defaults("hi", "Alice")
    test_dot_access()
    test_typeof()
    print("=== All tests passed ===")
}
