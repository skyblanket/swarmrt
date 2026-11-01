module Test_json_float_roundtrip

# #195: json_encode wrote 2.0 as "2", so json_decode gave back an int and
# `/` on it silently became integer division. Floats must stay floats, ints
# stay ints, and the text stays short. Both backends (compiled here + the
# tests/sw/run/ copy via swc run).

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun rt(v) { json_decode(json_encode(v)) }

fun main() {
    fails = 0
    fails = fails + assert_eq("enc_whole_float", json_encode(2.0), "2.0")
    fails = fails + assert_eq("enc_list", json_encode([2.0, 0.5, -3.0, 7]), "[2.0,0.5,-3.0,7]")
    fails = fails + assert_eq("enc_short", json_encode(0.1), "0.1")
    fails = fails + assert_eq("enc_map", json_encode(%{tau: 3.0}), "{\"tau\":3.0}")
    fails = fails + assert_eq("rt_type_float", typeof(rt(2.0)), "float")
    fails = fails + assert_eq("rt_type_int", typeof(rt(2)), "int")
    fails = fails + assert_eq("rt_division", rt(2.0) / 4, 0.5)
    fails = fails + assert_eq("rt_value", rt(3.0), 3.0)
    fails = fails + assert_eq("rt_exact_0.1", rt(0.1), 0.1)
    third = 1.0 / 3.0
    fails = fails + assert_eq("rt_exact_third", rt(third), third)
    big = math_pow(10.0, 20.0)
    fails = fails + assert_eq("rt_big_type", typeof(rt(big)), "float")
    fails = fails + assert_eq("rt_big_value", rt(big), big)
    tiny = math_pow(10.0, -7.0)
    fails = fails + assert_eq("rt_tiny_value", rt(tiny), tiny)
    fails = fails + assert_eq("rt_nested", hd(map_get(rt(%{g: [3.0]}), "g")) / 2, 1.5)
    if (fails == 0) { print("OK json_float_roundtrip") ; sys_exit(0) }
    else { print("FAIL json_float_roundtrip") ; sys_exit(1) }
}
