# Voice.sw — native real-time voice-agent helpers for swarmrt.
#
# Bridges a telephony provider (Telnyx Media Streaming, which connects to
# a swarmrt WS *server*) to a speech-to-speech model (OpenAI Realtime,
# reached as a swarmrt WS *client* over wss). The headline trick: if the
# Realtime session is configured for µ-law (8 kHz) on both legs, Telnyx's
# PCMU passes straight through with NO transcoding — base64-encoded mu-law
# stays ASCII end-to-end, so sw never touches raw bytes.
#
# Lookup falls back to <swarmrt-root>/lib/, so `import Voice` works from
# any project.
#
#   ## Typical wiring — GA `gpt-realtime` (DEFAULT; see examples/voice_agent.sw)
#     oa = Voice.realtime_connect(%{
#            model: "gpt-realtime",
#            api_key: getenv("OPENAI_API_KEY")})        # GA: NO OpenAI-Beta header
#     wsc_send(oa, Voice.session_update_ga(%{
#            format: %{type: "audio/pcmu"},              # GA format OBJECT
#            voice: "marin",
#            instructions: "..."}))                      # pure µ-law passthrough
#     ...
#     wsc_send(oa, Voice.realtime_append(b64))           # caller audio → model
#     ws_send(conn, Voice.telnyx_media(b64))             # model audio → caller
#
#   ## Legacy beta wiring (only if you must target the old beta wire)
#     oa = Voice.realtime_connect(%{model: ..., api_key: ...,
#                                   beta: "realtime=v1"})  # opt back IN to beta
#     wsc_send(oa, Voice.session_update(%{format: "g711_ulaw", voice: "alloy"}))
#
# The Realtime API is now GA: `session.type="realtime"`, `output_modalities`,
# nested `audio.input`/`audio.output.{format,...}` with format OBJECTS
# ({"type":"audio/pcmu"} / {"type":"audio/pcm","rate":24000}), and NO
# `OpenAI-Beta` header. `realtime_connect` therefore defaults to NO beta
# header, and `session_update_ga` builds the GA-nested session. The legacy
# beta-flat `session_update` + opt-in `beta:` header are kept for anything
# still on the old wire.
#
# Everything below is pure (string in / string out) EXCEPT realtime_connect,
# which wraps wsc_connect_tls. No raw-byte handling for the µ-law path.

module Voice

export [
    # OpenAI Realtime
    realtime_connect, realtime_poll,
    session_update, session_update_ga, audio_format,
    realtime_append, realtime_commit, realtime_cancel,
    realtime_create_response,
    # Telnyx Media Streaming
    telnyx_media, telnyx_clear, telnyx_mark,
    # frame inspection
    telnyx_event, openai_type, frame_field,
    # audio (non-passthrough bridges)
    ulaw_to_pcm16, pcm16_to_ulaw, resample
]

# ============================================================
# OpenAI Realtime — WS *client* over wss
# ============================================================

# realtime_connect(opts) -> handle | nil
#   opts keys:
#     model    (default "gpt-realtime")
#     api_key  (required; from getenv("OPENAI_API_KEY"))
#     url      (override the full wss URL; otherwise built from model)
#     beta     (the OpenAI-Beta header value. DEFAULT-OFF for GA: "" / nil =>
#              the header is OMITTED entirely. The GA `gpt-realtime` interface
#              explicitly does NOT send OpenAI-Beta. Pass beta:"realtime=v1"
#              ONLY to opt back in to the old beta wire.)
#     safety_identifier (optional GA header `openai-safety-identifier`; "" / nil
#              => omitted)
#
# Headers are built additively so the GA path sends just `Authorization`.
fun realtime_connect(opts) {
    model = opt(opts, "model", "gpt-realtime")
    key   = opt(opts, "api_key", "")
    url   = opt(opts, "url", f"wss://api.openai.com/v1/realtime?model={model}")
    base  = [f"Authorization: Bearer {key}"]
    # OpenAI-Beta is opt-IN now (GA omits it). Empty/nil => no beta header.
    hdrs  = case opt(opts, "beta", "") {
        "" -> base
        b  -> list_append(base, f"OpenAI-Beta: {b}")
    }
    # Optional GA safety identifier.
    hdrs2 = case opt(opts, "safety_identifier", "") {
        "" -> hdrs
        s  -> list_append(hdrs, f"openai-safety-identifier: {s}")
    }
    wsc_connect_tls(url, hdrs2)
}

# realtime_poll(handle) -> text | nil  — non-blocking single-frame poll.
fun realtime_poll(handle) {
    wsc_recv(handle, 0)
}

# audio_format(wire[, rate]) -> the GA format OBJECT for session_update_ga.
#   "pcmu"  -> %{type: "audio/pcmu"}                 (µ-law passthrough, 8 kHz)
#   "pcm16" -> %{type: "audio/pcm", rate: <rate>}    (linear PCM, default 24 kHz)
# Anything else is treated as already-an-object and passed through unchanged,
# so callers can hand a raw %{type: ...} map straight to session_update_ga.
fun audio_format(wire) { audio_format_rate(wire, 24000) }
fun audio_format_rate(wire, rate) {
    case wire {
        "pcmu"  -> map_put(map_new(), "type", "audio/pcmu")
        "pcm16" -> map_put(map_put(map_new(), "type", "audio/pcm"), "rate", rate)
        _       -> wire
    }
}

# ── session.update — GA `gpt-realtime` (the DEFAULT, nested shape) ────────────
#
# Builds the GA Realtime session contract (NOT the old beta-flat shape). The
# nesting is load-bearing: a GA session rejects flat input_audio_format /
# output_audio_format and emits response.output_audio.delta. Shape:
#   { "type":"session.update",
#     "session": {
#       "type":"realtime", "output_modalities":["audio"],
#       "audio": {
#         "input":  { "format":{...}, ["transcription":{...},] "turn_detection":{...} },
#         "output": { "format":{...}, "voice":... } },
#       "instructions":..., ["tools":[...], "tool_choice":...]
#       [, "reasoning":{effort}] [, "max_output_tokens":N] } }
#
# opts keys (all optional except sensible defaults):
#   format        GA format object %{type:"audio/pcmu"} | %{type:"audio/pcm",rate:N},
#                 OR a wire string "pcmu"/"pcm16" (passed through audio_format).
#                 Default %{type:"audio/pcmu"} (µ-law passthrough).
#   voice         (default "marin")
#   instructions  (default a concise phone-agent persona)
#   turn_detection  "server_vad" | "semantic_vad" (default "server_vad"),
#                   OR a full map (e.g. %{type:"server_vad", threshold:0.5, ...})
#                   passed through verbatim.
#   transcription  caller-speech transcription map %{model, language[, delay]}.
#                  Omitted entirely if absent ("" / nil). The whisper `delay`
#                  knob is valid only for *whisper* transcribers — gate it
#                  yourself, or use transcription:%{model, language} and add
#                  delay only for whisper.
#   tools         list of function-tool schemas. Omitted if absent/empty;
#                 when present, tool_choice defaults to "auto".
#   tool_choice   override the default "auto" (only used when tools present).
#   reasoning_effort   model-gated; omitted unless a non-empty string is given.
#   max_output_tokens  omitted unless > 0.
fun session_update_ga(opts) {
    fmt   = audio_format(opt(opts, "format", map_put(map_new(), "type", "audio/pcmu")))
    voice = opt(opts, "voice", "marin")
    instr = opt(opts, "instructions", "You are a concise, friendly phone agent.")
    vad   = ga_turn_detection(opt(opts, "turn_detection", "server_vad"))

    # audio.input — format + turn_detection (+ optional transcription)
    audio_in = map_put(map_new(), "format", fmt)
    audio_in = case opt(opts, "transcription", "") {
        "" -> audio_in
        tr -> map_put(audio_in, "transcription", tr)
    }
    audio_in = map_put(audio_in, "turn_detection", vad)

    # audio.output — format + voice
    audio_out = map_put(map_put(map_new(), "format", fmt), "voice", voice)

    audio = map_put(map_put(map_new(), "input", audio_in), "output", audio_out)

    sess = map_new()
    sess = map_put(sess, "type", "realtime")
    sess = map_put(sess, "output_modalities", ["audio"])
    sess = map_put(sess, "audio", audio)
    sess = map_put(sess, "instructions", instr)

    # tools (+ tool_choice) — omitted entirely when absent/empty.
    tools = opt(opts, "tools", [])
    sess = case tools {
        []  -> sess
        nil -> sess
        ts  ->
            s = map_put(sess, "tools", ts)
            map_put(s, "tool_choice", opt(opts, "tool_choice", "auto"))
    }

    # Optional, model-gated keys — omitted unless explicitly set (mini rejects
    # an unknown `reasoning` key; an explicit token cap is opt-in).
    sess = case opt(opts, "reasoning_effort", "") {
        "" -> sess
        ef -> map_put(sess, "reasoning", map_put(map_new(), "effort", ef))
    }
    sess = case opt(opts, "max_output_tokens", 0) {
        0 -> sess
        n -> map_put(sess, "max_output_tokens", n)
    }

    json_encode(map_put(map_put(map_new(), "type", "session.update"), "session", sess))
}

# GA turn-detection. server_vad = silence-timer with tuning; semantic_vad =
# content-aware. Both emit speech_started, so barge-in works either way. A map
# argument is passed through verbatim (full control).
fun ga_turn_detection(mode) {
    case mode {
        "server_vad" ->
            d = map_put(map_new(), "type", "server_vad")
            d = map_put(d, "threshold", 0.5)
            d = map_put(d, "prefix_padding_ms", 300)
            map_put(d, "silence_duration_ms", 200)
        "semantic_vad" -> map_put(map_new(), "type", "semantic_vad")
        _ -> mode
    }
}

# ── session.update — LEGACY beta-flat (only for the old beta wire) ────────────
# Pin the audio format on BOTH directions (flat input_audio_format /
# output_audio_format) and enable server-VAD. Kept for back-compat with agents
# still on `OpenAI-Beta: realtime=v1`. For GA `gpt-realtime`, use
# session_update_ga (the nested shape) instead.
#   opts keys: format (default "g711_ulaw"), voice (default "alloy"),
#              instructions, vad_threshold (default 0.5)
fun session_update(opts) {
    fmt   = opt(opts, "format", "g711_ulaw")
    voice = opt(opts, "voice", "alloy")
    instr = opt(opts, "instructions", "You are a concise, friendly phone agent.")
    thr   = opt(opts, "vad_threshold", 0.5)

    vad = map_put(map_put(map_new(), "type", "server_vad"), "threshold", thr)
    sess = map_new()
    sess = map_put(sess, "input_audio_format", fmt)
    sess = map_put(sess, "output_audio_format", fmt)
    sess = map_put(sess, "turn_detection", vad)
    sess = map_put(sess, "voice", voice)
    sess = map_put(sess, "instructions", instr)
    json_encode(map_put(map_put(map_new(), "type", "session.update"), "session", sess))
}

# input_audio_buffer.append — forward one base64 audio chunk to the model.
fun realtime_append(b64) {
    json_encode(map_put(map_put(map_new(), "type", "input_audio_buffer.append"),
                        "audio", b64))
}

# input_audio_buffer.commit — end the current input turn (manual VAD only).
fun realtime_commit() {
    json_encode(map_put(map_new(), "type", "input_audio_buffer.commit"))
}

# response.cancel — barge-in: stop the model's in-flight response.
fun realtime_cancel() {
    json_encode(map_put(map_new(), "type", "response.cancel"))
}

# response.create — ask the model to start a response (manual turns).
fun realtime_create_response() {
    json_encode(map_put(map_new(), "type", "response.create"))
}

# ============================================================
# Telnyx Media Streaming — WS *server* side (we receive its frames)
# ============================================================

# telnyx_media(b64) — outbound audio to the caller (event="media").
fun telnyx_media(b64) {
    media = map_put(map_new(), "payload", b64)
    json_encode(map_put(map_put(map_new(), "event", "media"), "media", media))
}

# telnyx_clear() — flush Telnyx's outbound audio buffer (barge-in).
fun telnyx_clear() {
    json_encode(map_put(map_new(), "event", "clear"))
}

# telnyx_mark(name) — insert a named mark to track playback position.
fun telnyx_mark(name) {
    mk = map_put(map_new(), "name", name)
    json_encode(map_put(map_put(map_new(), "event", "mark"), "mark", mk))
}

# ============================================================
# Frame inspection
# ============================================================

# telnyx_event(text) -> the top-level "event" string, or "".
fun telnyx_event(text) {
    frame_field(text, "event")
}

# openai_type(text) -> the top-level "type" string, or "".
fun openai_type(text) {
    frame_field(text, "type")
}

# frame_field(text, key) -> map_get(json_decode(text), key) or "".
fun frame_field(text, key) {
    v = map_get(json_decode(text), key)
    case v { 'nil' -> "" _ -> v }
}

# ============================================================
# Audio codecs (only needed for NON-passthrough bridges:
# Deepgram / ElevenLabs / OpenAI@24kHz PCM16). base64 in / base64 out.
# ============================================================

fun ulaw_to_pcm16(b64) { audio_ulaw_to_pcm16(b64) }
fun pcm16_to_ulaw(b64) { audio_pcm16_to_ulaw(b64) }
fun resample(b64, from_hz, to_hz) { audio_resample(b64, from_hz, to_hz) }

# ============================================================
# internal
# ============================================================

fun opt(m, key, default) {
    v = map_get(m, key)
    case v { 'nil' -> default _ -> v }
}
