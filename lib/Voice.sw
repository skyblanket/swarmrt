# Voice.sw — native real-time voice-agent helpers for swarmrt.
#
# Bridges a telephony provider (Telnyx Media Streaming, which connects to
# a swarmrt WS *server*) to a speech-to-speech model (OpenAI Realtime,
# reached as a swarmrt WS *client* over wss). The headline trick: if the
# Realtime session is configured input/output_audio_format = g711_ulaw
# (8 kHz), Telnyx's PCMU passes straight through with NO transcoding —
# base64-encoded mu-law stays ASCII end-to-end, so sw never touches raw
# bytes.
#
# Lookup falls back to <swarmrt-root>/lib/, so `import Voice` works from
# any project.
#
#   ## Typical wiring (see examples/voice_agent.sw)
#     oa = Voice.realtime_connect(%{
#            model: "gpt-realtime",
#            api_key: getenv("OPENAI_API_KEY"),
#            format: "g711_ulaw"})       # pure passthrough
#     wsc_send(oa, Voice.session_update(%{format: "g711_ulaw", voice: "alloy",
#                                         instructions: "..."}))
#     ...
#     wsc_send(oa, Voice.realtime_append(b64))         # caller audio → model
#     ws_send(conn, Voice.telnyx_media(b64))           # model audio → caller
#
# Everything below is pure (string in / string out) EXCEPT realtime_connect,
# which wraps wsc_connect_tls. No raw-byte handling for the g711 path.

module Voice

export [
    # OpenAI Realtime
    realtime_connect, realtime_poll,
    session_update, realtime_append, realtime_commit, realtime_cancel,
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
#     beta     (default "realtime=v1"; the OpenAI-Beta header value)
fun realtime_connect(opts) {
    model = opt(opts, "model", "gpt-realtime")
    key   = opt(opts, "api_key", "")
    beta  = opt(opts, "beta", "realtime=v1")
    url   = opt(opts, "url", f"wss://api.openai.com/v1/realtime?model={model}")
    headers = [f"Authorization: Bearer {key}", f"OpenAI-Beta: {beta}"]
    wsc_connect_tls(url, headers)
}

# realtime_poll(handle) -> text | nil  — non-blocking single-frame poll.
fun realtime_poll(handle) {
    wsc_recv(handle, 0)
}

# session.update — pin the audio format on BOTH directions and enable
# server-side VAD (so the model emits input_audio_buffer.speech_started,
# our barge-in trigger). `format` is an OPTION, never hardcoded, because
# the Realtime API shape is still churning (beta-flat "g711_ulaw" vs the
# GA-nested "audio/pcmu").
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
