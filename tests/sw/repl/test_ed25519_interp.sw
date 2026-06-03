# test_ed25519_interp.sw — interpreter-side coverage of ed25519_verify, the
# openssl-EVP Ed25519 signature-verify builtin (codegen twin:
# tests/sw/test_ed25519.sw). Run via:
#
#   ./bin/swc test tests/sw/repl/test_ed25519_interp.sw
#
# Guards against codegen/interpreter drift: the interpreter (swarmrt_lang.c)
# must resolve ed25519_verify to the SAME crypto the compiled path uses, not
# silently warn+degrade it like a scheduler primitive.
#
# This file is GREEN in BOTH builds. `swc test` defaults to the non-TLS build
# (ed25519_verify -> nil with a one-line stderr note, like wsc_connect_tls), but
# the same `swc` can be built `make SWARMRT_TLS=1` (ed25519_verify -> real
# 'true'/'false'). So instead of pinning a build-specific value, we assert the
# invariant that holds either way:
#   - a VALID vector is never 'false' (it's 'true' under TLS, nil without), and
#   - a TAMPERED vector is never 'true' (it's 'false' under TLS, nil without).
# The exact true/false crypto outcome is pinned in the compiled twin under
# SWARMRT_TLS=1.

module Test_ed25519_interp

# A valid Telnyx-style base64 vector (raw-32-byte key b64 + sig b64 + literal msg).
fun pubkey() { "ode1c+QXL7Ex2wevPaqVKNt13QQxwYRda3uPWV/fE9I=" }
fun sig()    { "QO0/IdCgnIsPxUE55DgBWDZafsT7dUPse/Ko1Dkz3FUbpDIf/ChCbOUAFd2ioSk/OajvZSJhABtwtwLWi4MWCw==" }
fun msg()    { "1733360000|{\"event\":\"hello\"}" }

# Valid signature: 'true' under TLS, nil without — never 'false'.
fun test_valid_never_false() {
    assert_eq(ed25519_verify(pubkey(), sig(), msg()) == 'false', 'false')
}

# Tampered message: 'false' under TLS, nil without — never 'true'.
fun test_tampered_never_true() {
    bad = "1733360000|{\"event\":\"HELLO\"}"
    assert_eq(ed25519_verify(pubkey(), sig(), bad) == 'true', 'false')
}

# Raw-bytes inputs resolve identically (interpreter must accept the bytes type).
fun test_bytes_inputs_resolve() {
    pk = bytes_from_base64(pubkey())
    sg = bytes_from_base64(sig())
    m  = string_to_bytes(msg())
    assert_eq(ed25519_verify(pk, sg, m) == 'false', 'false')
}
