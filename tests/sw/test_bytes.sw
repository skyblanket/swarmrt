module Test_bytes

# Tests for SW_VAL_BYTES — the length-carrying, NUL-safe byte vector type.
#
# The whole point of this type is that it survives embedded NUL bytes, which
# the NUL-terminated sw string cannot (base64_decode truncates at 0x00 — that
# limitation is locked in tests/sw/test_voice_codec.sw). These tests exercise
# the cases the base64-string path provably could NOT do:
#   - base64 <-> bytes <-> string round-trips with NUL-bearing data
#   - byte_size / byte_at / byte_slice / bytes_concat
#   - whole-value equality (works free via sw_val_equal, usable in `case`)
#   - bytes as ETS keys + bytes over same-node send
#   - a PCM16 codec round-trip on a sample whose low byte is 0x00 (0x8000)
#
# Self-contained harness; main() exits non-zero if any assertion fails.

fun assert_true(name, cond) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name ++ ": condition false") ; 1 }
}

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# ---- base64 <-> bytes round-trip, including NUL-bearing data ----
# "AP8Afw==" decodes to [00 FF 00 7F]. As a sw STRING this truncates at the
# first 0x00 (string_length -> 0; see test_voice_codec). As BYTES it survives.
fun test_base64_bytes_roundtrip() {
    b = bytes_from_base64("AP8Afw==")
    r1 = assert_eq("bytes_size_survives_nul", byte_size(b), 4)
    r2 = assert_eq("bytes_to_base64_roundtrip", bytes_to_base64(b), "AP8Afw==")
    r3 = assert_eq("typeof_bytes", typeof(b), "bytes")
    r1 + r2 + r3
}

# ---- byte_at: index individual bytes, NULs and all ----
fun test_byte_at() {
    b = bytes_from_base64("AP8Afw==")   # [00 FF 00 7F]
    r1 = assert_eq("byte_at_0_is_nul", byte_at(b, 0), 0)
    r2 = assert_eq("byte_at_1", byte_at(b, 1), 255)
    r3 = assert_eq("byte_at_2_is_nul", byte_at(b, 2), 0)
    r4 = assert_eq("byte_at_3", byte_at(b, 3), 127)
    r1 + r2 + r3 + r4
}

# ---- byte_slice: subrange with clamping ----
fun test_byte_slice() {
    b = bytes_from_base64("AP8Afw==")   # [00 FF 00 7F]
    mid = byte_slice(b, 1, 2)            # [FF 00] = "/wA="
    r1 = assert_eq("slice_size", byte_size(mid), 2)
    r2 = assert_eq("slice_value", bytes_to_base64(mid), "/wA=")
    # len clamps to end
    tail = byte_slice(b, 2, 100)         # [00 7F] = "AH8="
    r3 = assert_eq("slice_clamped_size", byte_size(tail), 2)
    r4 = assert_eq("slice_clamped_value", bytes_to_base64(tail), "AH8=")
    # start past end -> empty
    empty = byte_slice(b, 10, 4)
    r5 = assert_eq("slice_past_end_empty", byte_size(empty), 0)
    r1 + r2 + r3 + r4 + r5
}

# ---- bytes_concat: associative join ----
fun test_bytes_concat() {
    a = bytes_from_base64("AP8=")        # [00 FF]
    b = bytes_from_base64("AH8=")        # [00 7F]
    ab = bytes_concat(a, b)              # [00 FF 00 7F]
    r1 = assert_eq("concat_size", byte_size(ab), 4)
    r2 = assert_eq("concat_value", bytes_to_base64(ab), "AP8Afw==")
    # associativity: (a++b)++c == a++(b++c)
    c = bytes_from_base64("QQ==")        # [41]
    left  = bytes_concat(bytes_concat(a, b), c)
    right = bytes_concat(a, bytes_concat(b, c))
    r3 = assert_eq("concat_associative", bytes_to_base64(left), bytes_to_base64(right))
    r1 + r2 + r3
}

# ---- string <-> bytes bridges (explicit, documented truncation) ----
fun test_string_bytes_bridge() {
    sb = string_to_bytes("hello")
    r1 = assert_eq("string_to_bytes_size", byte_size(sb), 5)
    r2 = assert_eq("bytes_to_string_roundtrip", bytes_to_string(sb), "hello")
    # bytes_to_string truncates at the first NUL BY DESIGN — the [00 FF..]
    # value renders as an empty string (this is the opt-in, named choice).
    nul = bytes_from_base64("AP8Afw==")  # starts with 0x00
    r3 = assert_eq("bytes_to_string_truncates_at_nul", string_length(bytes_to_string(nul)), 0)
    r1 + r2 + r3
}

# ---- whole-value equality: works free, usable in `case` ----
fun test_bytes_equality() {
    a = bytes_from_base64("AAD/fwCAQgE=")
    b = bytes_from_base64("AAD/fwCAQgE=")
    c = bytes_from_base64("AP8Afw==")
    r1 = assert_true("bytes_eq_same",  a == b)
    r2 = assert_true("bytes_neq_diff", (a == c) == 'false')
    # bytes are NOT equal to a string (Erlang keeps binaries/char-lists distinct)
    r3 = assert_true("bytes_neq_string", (a == "AAD/fwCAQgE=") == 'false')
    # whole-value match in a case expression
    sig = bytes_from_base64("AP8Afw==")
    label = case sig {
        x when x == c -> "matched"
        _             -> "no"
    }
    r4 = assert_eq("bytes_case_match", label, "matched")
    r1 + r2 + r3 + r4
}

# ---- PCM16 codec round-trip the base64-STRING path could not do ----
# 4 PCM16 LE samples, two containing NUL bytes:
#   sample 0 = 0x0000 (silence)      -> 00 00
#   sample 1 = 0x7FFF (max positive) -> FF 7F
#   sample 2 = 0x8000 (max negative) -> 00 80   <- low byte is NUL
#   sample 3 = 0x0142                -> 42 01
# base64([00 00 FF 7F 00 80 42 01]) = "AAD/fwCAQgE="
fun test_pcm16_nul_bearing_roundtrip() {
    pcm = bytes_from_base64("AAD/fwCAQgE=")        # 8 raw bytes, NULs and all
    r1 = assert_eq("pcm_size", byte_size(pcm), 8)
    # Read sample 2 (the 0x8000 NUL-bearing one) byte-for-byte:
    r2 = assert_eq("pcm_sample2_lo_nul", byte_at(pcm, 4), 0)     # the NUL survived
    r3 = assert_eq("pcm_sample2_hi",     byte_at(pcm, 5), 128)
    # mu-law codec twins: 8 PCM bytes -> 4 mu-law -> 8 PCM
    ulaw = audio_pcm16_to_ulaw_b(pcm)
    r4 = assert_eq("ulaw_size", byte_size(ulaw), 4)
    back = audio_ulaw_to_pcm16_b(ulaw)
    r5 = assert_eq("ulaw_roundtrip_size", byte_size(back), 8)
    # resample 8k -> 16k roughly doubles the byte count
    up = audio_resample_b(pcm, 8000, 16000)
    r6 = assert_eq("resample_up_size", byte_size(up), 16)
    # slice on sample boundaries + concat == original (no base64 detour)
    head = byte_slice(pcm, 0, 4)
    tail = byte_slice(pcm, 4, 4)
    rejoined = bytes_concat(head, tail)
    r7 = assert_true("slice_concat_identity",
                     bytes_to_base64(rejoined) == bytes_to_base64(pcm))
    r1 + r2 + r3 + r4 + r5 + r6 + r7
}

# ---- bytes survive a same-node send (aliased-pointer payload) ----
fun byte_echo() {
    receive {
        {from, b} -> send(from, {'echoed', bytes_to_base64(b)})
    }
}

fun test_bytes_over_send() {
    p = spawn(byte_echo())
    b = bytes_from_base64("AAD/fwCAQgE=")
    send(p, {self(), b})
    got = receive {
        {'echoed', b64} -> b64
        after 2000 { 'timeout' }
    }
    assert_eq("bytes_survive_send", got, "AAD/fwCAQgE=")
}

# ---- bytes as ETS values AND keys ----
fun test_bytes_in_ets() {
    t = ets_new("bytes_tab", 'set')
    v = bytes_from_base64("AAD/fwCAQgE=")
    ets_put(t, "vkey", v)
    r1 = assert_eq("ets_bytes_value", byte_size(ets_get(t, "vkey")), 8)
    # bytes used as a key — a freshly-decoded equal value must hit it
    k = bytes_from_base64("AP8Afw==")
    ets_put(t, k, "by-bytes-key")
    k2 = bytes_from_base64("AP8Afw==")
    r2 = assert_eq("ets_bytes_key", ets_get(t, k2), "by-bytes-key")
    r1 + r2
}

fun main() {
    fails = 0
    fails = fails + test_base64_bytes_roundtrip()
    fails = fails + test_byte_at()
    fails = fails + test_byte_slice()
    fails = fails + test_bytes_concat()
    fails = fails + test_string_bytes_bridge()
    fails = fails + test_bytes_equality()
    fails = fails + test_pcm16_nul_bearing_roundtrip()
    fails = fails + test_bytes_over_send()
    fails = fails + test_bytes_in_ets()

    if (fails == 0) { print("OK test_bytes all/all") sys_exit(0) }
    else { print(f"FAIL test_bytes {fails} assertions failed") sys_exit(1) }
}
