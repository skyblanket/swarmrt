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

# ============================================================
# GA `gpt-realtime` session.update wire contract
# (the voice-agent port proved these shapes against the loopback;
#  see /Users/sky/voice-agent/src/Bridge.sw session_update_full).
# ============================================================

# Voice.audio_format(wire) -> the GA format OBJECT (not the old beta token).
fun test_ga_audio_format() {
    pcmu = Voice.audio_format("pcmu")
    r1 = assert_eq("ga_fmt_pcmu_type", map_get(pcmu, "type"), "audio/pcmu")
    pcm = Voice.audio_format("pcm16")
    r2 = assert_eq("ga_fmt_pcm16_type", map_get(pcm, "type"), "audio/pcm")
    r3 = assert_eq("ga_fmt_pcm16_rate", map_get(pcm, "rate"), 24000)
    # A raw object passes through unchanged (caller-supplied format).
    raw = Voice.audio_format(%{type: "audio/pcm", rate: 16000})
    r4 = assert_eq("ga_fmt_passthrough", map_get(raw, "rate"), 16000)
    r1 + r2 + r3 + r4
}

# The full GA-nested session.update shape, with transcription + tools + VAD.
fun test_ga_session_update_full() {
    opts = %{
        format: "pcmu",
        voice: "marin",
        instructions: "be nice",
        turn_detection: "server_vad",
        transcription: %{model: "gpt-realtime-whisper", language: "hi", delay: "medium"},
        tools: [%{name: "end_call"}, %{name: "handoff_to_human"}]
    }
    v = json_decode(Voice.session_update_ga(opts))
    r1 = assert_eq("ga_su_type", map_get(v, "type"), "session.update")
    sess = map_get(v, "session")
    # GA top-level: type:"realtime" + output_modalities:["audio"] (NOT beta-flat).
    r2 = assert_eq("ga_su_session_type", map_get(sess, "type"), "realtime")
    r3 = assert_eq("ga_su_output_modalities", map_get(sess, "output_modalities"), ["audio"])
    # NESTED audio.input / audio.output (the load-bearing GA nesting).
    audio = map_get(sess, "audio")
    ain   = map_get(audio, "input")
    aout  = map_get(audio, "output")
    # format OBJECTS on each leg.
    r4 = assert_eq("ga_su_in_fmt",  map_get(map_get(ain, "format"), "type"), "audio/pcmu")
    r5 = assert_eq("ga_su_out_fmt", map_get(map_get(aout, "format"), "type"), "audio/pcmu")
    r6 = assert_eq("ga_su_voice",   map_get(aout, "voice"), "marin")
    # turn_detection nested under audio.input (drives barge-in via speech_started).
    td = map_get(ain, "turn_detection")
    r7 = assert_eq("ga_su_vad_type", map_get(td, "type"), "server_vad")
    # caller transcription nested under audio.input.
    tr = map_get(ain, "transcription")
    r8 = assert_eq("ga_su_tr_model", map_get(tr, "model"), "gpt-realtime-whisper")
    r9 = assert_eq("ga_su_tr_delay", map_get(tr, "delay"), "medium")
    # tools present => tool_choice defaults to "auto".
    r10 = assert_eq("ga_su_tools_len", length(map_get(sess, "tools")), 2)
    r11 = assert_eq("ga_su_tool_choice", map_get(sess, "tool_choice"), "auto")
    # GA flat keys must be ABSENT (a GA session rejects them).
    r12 = assert_eq("ga_su_no_flat_in",  map_get(sess, "input_audio_format"), 'nil')
    r13 = assert_eq("ga_su_no_flat_out", map_get(sess, "output_audio_format"), 'nil')
    r1 + r2 + r3 + r4 + r5 + r6 + r7 + r8 + r9 + r10 + r11 + r12 + r13
}

# Defaults: no opts -> µ-law passthrough, marin, server_vad, no tools.
fun test_ga_session_update_defaults() {
    v = json_decode(Voice.session_update_ga(map_new()))
    sess  = map_get(v, "session")
    audio = map_get(sess, "audio")
    ain   = map_get(audio, "input")
    aout  = map_get(audio, "output")
    r1 = assert_eq("ga_def_in_fmt", map_get(map_get(ain, "format"), "type"), "audio/pcmu")
    r2 = assert_eq("ga_def_voice",  map_get(aout, "voice"), "marin")
    r3 = assert_eq("ga_def_vad",    map_get(map_get(ain, "turn_detection"), "type"), "server_vad")
    # no transcription, no tools, no tool_choice when not supplied.
    r4 = assert_eq("ga_def_no_tr",     map_get(ain, "transcription"), 'nil')
    r5 = assert_eq("ga_def_no_tools",  map_get(sess, "tools"), 'nil')
    r6 = assert_eq("ga_def_no_choice", map_get(sess, "tool_choice"), 'nil')
    r1 + r2 + r3 + r4 + r5 + r6
}

# Optional model-gated keys are OMITTED unless explicitly set, then present.
fun test_ga_session_optional_keys() {
    # absent by default
    v0 = json_decode(Voice.session_update_ga(map_new()))
    s0 = map_get(v0, "session")
    r1 = assert_eq("ga_opt_no_reasoning", map_get(s0, "reasoning"), 'nil')
    r2 = assert_eq("ga_opt_no_maxtok",    map_get(s0, "max_output_tokens"), 'nil')
    # set them -> present
    v1 = json_decode(Voice.session_update_ga(%{reasoning_effort: "low", max_output_tokens: 2048}))
    s1 = map_get(v1, "session")
    r3 = assert_eq("ga_opt_reasoning", map_get(map_get(s1, "reasoning"), "effort"), "low")
    r4 = assert_eq("ga_opt_maxtok",    map_get(s1, "max_output_tokens"), 2048)
    r1 + r2 + r3 + r4
}

# semantic_vad turn detection, and a full custom turn_detection map passthrough.
fun test_ga_turn_detection() {
    v = json_decode(Voice.session_update_ga(%{turn_detection: "semantic_vad"}))
    ain = map_get(map_get(map_get(v, "session"), "audio"), "input")
    r1 = assert_eq("ga_td_semantic", map_get(map_get(ain, "turn_detection"), "type"), "semantic_vad")
    # custom map is passed through verbatim
    custom = %{type: "server_vad", threshold: 0.9, silence_duration_ms: 500}
    v2 = json_decode(Voice.session_update_ga(%{turn_detection: custom}))
    td = map_get(map_get(map_get(map_get(v2, "session"), "audio"), "input"), "turn_detection")
    r2 = assert_eq("ga_td_custom_threshold", map_get(td, "threshold"), 0.9)
    r1 + r2
}

# Legacy beta-flat session.update is preserved unchanged (back-compat).
fun test_beta_session_update_legacy() {
    v = json_decode(Voice.session_update(%{format: "g711_ulaw", voice: "alloy"}))
    sess = map_get(v, "session")
    # beta-flat keys present, GA nesting absent.
    r1 = assert_eq("beta_su_in_fmt",  map_get(sess, "input_audio_format"), "g711_ulaw")
    r2 = assert_eq("beta_su_out_fmt", map_get(sess, "output_audio_format"), "g711_ulaw")
    r3 = assert_eq("beta_su_voice",   map_get(sess, "voice"), "alloy")
    r4 = assert_eq("beta_su_no_audio_nest", map_get(sess, "audio"), 'nil')
    r5 = assert_eq("beta_su_no_session_type", map_get(sess, "type"), 'nil')
    r1 + r2 + r3 + r4 + r5
}

# pcm16 format object carries the rate (24 kHz linear PCM).
fun test_ga_session_pcm16() {
    v = json_decode(Voice.session_update_ga(%{format: "pcm16"}))
    aout = map_get(map_get(map_get(v, "session"), "audio"), "output")
    fmt = map_get(aout, "format")
    r1 = assert_eq("ga_pcm16_type", map_get(fmt, "type"), "audio/pcm")
    r2 = assert_eq("ga_pcm16_rate", map_get(fmt, "rate"), 24000)
    r1 + r2
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
    fails = fails + test_ga_audio_format()
    fails = fails + test_ga_session_update_full()
    fails = fails + test_ga_session_update_defaults()
    fails = fails + test_ga_session_optional_keys()
    fails = fails + test_ga_turn_detection()
    fails = fails + test_beta_session_update_legacy()
    fails = fails + test_ga_session_pcm16()

    if (fails == 0) { print("OK test_voice_codec all/all") sys_exit(0) }
    else { print(f"FAIL test_voice_codec {fails} assertions failed") sys_exit(1) }
}
