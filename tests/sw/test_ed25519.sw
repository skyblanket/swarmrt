module Test_ed25519

# Tests for ed25519_verify(public_key, signature, message) -> 'true'|'false'|nil
#
# This builtin verifies an Ed25519 signature using openssl EVP, gated behind
# SWARMRT_TLS (the same gate as wsc_connect_tls). It exists so a sw program can
# verify Telnyx webhook signatures in-process (the voice-agent port had to
# fail-closed-stub this; see /Users/sky/voice-agent/src/Telnyx.sw).
#
# Input contract (documented):
#   public_key : 32 raw bytes (a `bytes` value) OR a base64 STRING (decoded).
#                Telnyx delivers the portal key as base64, so the string form
#                is the ergonomic path.
#   signature  : 64 raw bytes (a `bytes` value) OR a base64 STRING (decoded).
#                Telnyx delivers the `telnyx-signature-ed25519` header base64.
#   message    : the signed bytes — a `bytes` value OR a plain STRING taken as
#                its raw UTF-8 bytes (NOT base64). For Telnyx this is the literal
#                "{timestamp}|{raw_body}".
#   returns    : 'true'  signature is valid for (key, message)
#                'false' invalid / wrong-length / undecodable input
#                nil     this build has no TLS (openssl) — mirrors wsc_connect_tls
#
# This file is GREEN in BOTH builds: under a non-TLS build ed25519_verify
# returns nil, and we assert exactly that; under SWARMRT_TLS=1 we assert the
# real true/false crypto outcome against a known vector.
#
# Known vector (generated with openssl, mirrors Telnyx exactly):
#   key  = openssl genpkey -algorithm ed25519; raw 32-byte pubkey, base64
#   msg  = '1733360000|{"event":"hello"}'
#   sig  = openssl pkeyutl -sign -rawin, base64
# Cross-checked: `openssl pkeyutl -verify -rawin` -> "Signature Verified".

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# Known-good Telnyx-style vector (base64 key + base64 sig + literal message).
fun vec_pubkey() { "ode1c+QXL7Ex2wevPaqVKNt13QQxwYRda3uPWV/fE9I=" }
fun vec_sig()    { "QO0/IdCgnIsPxUE55DgBWDZafsT7dUPse/Ko1Dkz3FUbpDIf/ChCbOUAFd2ioSk/OajvZSJhABtwtwLWi4MWCw==" }
fun vec_msg()    { "1733360000|{\"event\":\"hello\"}" }

# RFC 8032 Test 1 (empty message), as base64 of the spec hex values.
#   pubkey hex d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a
#   sig    hex e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b
fun rfc_pubkey() { "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=" }
fun rfc_sig()    { "5VZDAMNgrHKQhuLMgG6CioSHfx645dl02HPgZSJJAVVfuIIVkKM7rMYeOXAc+bRr0lv18FlbviRlUUFDjnoQCw==" }

fun is_tls_build() {
    # Probe: a valid vector returns 'true' under TLS, nil otherwise.
    case ed25519_verify(vec_pubkey(), vec_sig(), vec_msg()) {
        nil -> 'false'
        _   -> 'true'
    }
}

# ---- TLS-off build: ed25519_verify is a documented nil (like wsc_connect_tls) ----
fun test_tls_off_returns_nil() {
    assert_eq("tls_off_valid_is_nil", ed25519_verify(vec_pubkey(), vec_sig(), vec_msg()), nil)
}

# ---- TLS build: real crypto outcomes ----
fun test_valid_signature_true() {
    assert_eq("valid_b64_vector_true",
              ed25519_verify(vec_pubkey(), vec_sig(), vec_msg()), 'true')
}

fun test_rfc8032_test1_true() {
    # empty message
    assert_eq("rfc8032_test1_true",
              ed25519_verify(rfc_pubkey(), rfc_sig(), ""), 'true')
}

fun test_tampered_message_false() {
    assert_eq("tampered_message_false",
              ed25519_verify(vec_pubkey(), vec_sig(), "1733360000|{\"event\":\"HELLO\"}"), 'false')
}

fun test_tampered_sig_false() {
    # flip the last base64 char of the sig -> different bytes
    bad = "QO0/IdCgnIsPxUE55DgBWDZafsT7dUPse/Ko1Dkz3FUbpDIf/ChCbOUAFd2ioSk/OajvZSJhABtwtwLWi4MWCA=="
    assert_eq("tampered_sig_false",
              ed25519_verify(vec_pubkey(), bad, vec_msg()), 'false')
}

fun test_wrong_key_false() {
    assert_eq("wrong_key_false",
              ed25519_verify(rfc_pubkey(), vec_sig(), vec_msg()), 'false')
}

fun test_raw_bytes_inputs_true() {
    # Same vector but pass key + sig as raw bytes, message as raw bytes.
    pk = bytes_from_base64(vec_pubkey())
    sg = bytes_from_base64(vec_sig())
    msg = string_to_bytes(vec_msg())
    assert_eq("raw_bytes_inputs_true", ed25519_verify(pk, sg, msg), 'true')
}

fun test_garbage_lengths_false() {
    # 5-byte "key" is not 32 bytes -> false, never a crash
    r1 = assert_eq("short_key_false", ed25519_verify("aGVsbG8=", vec_sig(), vec_msg()), 'false')
    # 5-byte "sig" is not 64 bytes -> false
    r2 = assert_eq("short_sig_false", ed25519_verify(vec_pubkey(), "aGVsbG8=", vec_msg()), 'false')
    r1 + r2
}

fun main() {
    fails = 0
    case is_tls_build() {
        'false' -> {
            # Non-TLS build: assert the documented nil behavior only.
            fails = fails + test_tls_off_returns_nil()
        }
        _ -> {
            fails = fails + test_valid_signature_true()
            fails = fails + test_rfc8032_test1_true()
            fails = fails + test_tampered_message_false()
            fails = fails + test_tampered_sig_false()
            fails = fails + test_wrong_key_false()
            fails = fails + test_raw_bytes_inputs_true()
            fails = fails + test_garbage_lengths_false()
        }
    }

    if (fails == 0) { print("OK test_ed25519 all/all") sys_exit(0) }
    else { print(f"FAIL test_ed25519 {fails} assertions failed") sys_exit(1) }
}
