module Test_voice_codec

# Tests for the native voice-agent foundation:
#   - base64 ASCII round-trip (the g711-passthrough invariant)
#   - base64 binary-safety limitation (NUL truncation — LOCKED IN as a
#     test so the deferred bytes-type decision is never forgotten)
#   - G.711 mu-law <-> PCM16 round-trip (decode->encode idempotency)
#   - linear PCM resample sanity (8k <-> 16k sample-count + reversibility)
#   - Telnyx/OpenAI frame passthrough builders are byte-identical
#
# Self-contained harness; main() exits non-zero if any assertion fails.

import Voice

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

# ---- base64 ASCII round-trip: the passthrough invariant ----
# base64 mu-law is pure printable ASCII, so it survives as a sw string.
fun test_base64_ascii_roundtrip() {
    s = "Hello, mu-law passthrough!"
    rt = base64_decode(base64_encode(s))
    assert_eq("base64_ascii_roundtrip", rt, s)
}

# ---- base64 binary-safety: KNOWN LIMITATION, locked in ----
# bytes [00,FF,00,7F] = "AP8Afw==". Because sw strings are NUL-terminated,
# decode truncates at the first NUL -> length 0. If this assertion ever
# starts FAILING (returning 4), the runtime grew a real bytes type and the
# audio path can be simplified — revisit Option A/C of the voice plan.
fun test_base64_nul_truncation() {
    decoded = base64_decode("AP8Afw==")
    assert_eq("base64_nul_truncation_locked", string_length(decoded), 0)
}

# ---- mu-law <-> PCM16 round-trip ----
# Test vector FF 80 00 55 AA 11 C3 3C (deliberately excludes 0x7F, the
# negative-zero alias that canonicalizes to 0xFF per ITU-T G.711).
fun test_ulaw_pcm16_roundtrip() {
    ulaw = "/4AAVaoRwzw="
    pcm  = audio_ulaw_to_pcm16(ulaw)
    back = audio_pcm16_to_ulaw(pcm)
    r1 = assert_true("ulaw_roundtrip_identical", back == ulaw)
    # 8 mu-law codes -> 16 PCM bytes. base64 of 16 bytes = 24 chars (incl padding).
    r2 = assert_eq("ulaw_to_pcm16_len", string_length(pcm), 24)
    r1 + r2
}

# Via the Voice module wrappers (same result, exercises the lib surface).
fun test_voice_codec_wrappers() {
    ulaw = "/4AAVaoRwzw="
    pcm  = Voice.ulaw_to_pcm16(ulaw)
    back = Voice.pcm16_to_ulaw(pcm)
    assert_true("voice_codec_wrapper_roundtrip", back == ulaw)
}

# ---- resample sanity ----
# 8 PCM samples (16 bytes) @8k -> @16k ~doubles count, -> @8k returns to 16 bytes.
fun test_resample_lengths() {
    pcm8  = audio_ulaw_to_pcm16("/4AAVaoRwzw=")   # 16 bytes (8 samples)
    up    = audio_resample(pcm8, 8000, 16000)
    down  = audio_resample(up, 16000, 8000)
    # up: 8 samples -> 16 samples = 32 bytes -> b64 len 44 (32/3->11*4=44)
    r1 = assert_eq("resample_up_len",   string_length(up),   44)
    # down back to 8 samples = 16 bytes -> b64 len 24
    r2 = assert_eq("resample_down_len", string_length(down), 24)
    r1 + r2
}

# resample with from==to is identity.
fun test_resample_identity() {
    pcm = audio_ulaw_to_pcm16("/4AAVaoRwzw=")
    same = audio_resample(pcm, 8000, 8000)
    assert_true("resample_identity", same == pcm)
}

# ---- frame builders: g711 passthrough is byte-identical ----
# The same base64 payload must appear unchanged inside both the inbound
# (OpenAI append) and outbound (Telnyx media) frames — proving no mutation.
fun test_passthrough_frames() {
    b64 = "/4AAVaoRwzw="
    media   = Voice.telnyx_media(b64)
    append  = Voice.realtime_append(b64)
    # the payload substring must survive verbatim in both directions
    r1 = assert_true("telnyx_media_carries_b64",  string_contains(media, b64))
    r2 = assert_true("realtime_append_carries_b64", string_contains(append, b64))
    # round-trip the embedded payload through json to confirm exact value
    r3 = assert_eq("telnyx_media_payload_exact",
                   map_get(map_get(json_decode(media), "media"), "payload"), b64)
    r4 = assert_eq("realtime_append_audio_exact",
                   map_get(json_decode(append), "audio"), b64)
    r1 + r2 + r3 + r4
}

# ---- frame inspection helpers ----
fun test_frame_inspection() {
    media = Voice.telnyx_media("AAAA")
    r1 = assert_eq("telnyx_event", Voice.telnyx_event(media), "media")
    oa = Voice.realtime_cancel()
    r2 = assert_eq("openai_type", Voice.openai_type(oa), "response.cancel")
    clear = Voice.telnyx_clear()
    r3 = assert_eq("telnyx_clear_event", Voice.telnyx_event(clear), "clear")
    r1 + r2 + r3
}

fun main() {
    fails = 0
    fails = fails + test_base64_ascii_roundtrip()
    fails = fails + test_base64_nul_truncation()
    fails = fails + test_ulaw_pcm16_roundtrip()
    fails = fails + test_voice_codec_wrappers()
    fails = fails + test_resample_lengths()
    fails = fails + test_resample_identity()
    fails = fails + test_passthrough_frames()
    fails = fails + test_frame_inspection()

    if (fails == 0) { print("OK test_voice_codec all/all") sys_exit(0) }
    else { print(f"FAIL test_voice_codec {fails} assertions failed") sys_exit(1) }
}
