# voice_agent.sw — real-time voice agents, one supervised sw process per call.
#
# Topology (g711 passthrough, NO transcoding):
#
#   Telnyx phone call ──(WS server: this process)──┐
#                                                   │  base64 PCMU stays
#   OpenAI Realtime ──(WS client: wsc_*)───────────┘  ASCII end-to-end
#
# A Supervisor owns the WS *server*. The server's accept handler spawns
# ONE call process per phone call (on Telnyx 'start'); each call process
# opens its OWN OpenAI Realtime WS *client* and bridges the two streams
# with a single selective-receive loop. A crash in one call kills only
# that call's process — every other call keeps talking.
#
# This example COMPILES today (`swc build examples/voice_agent.sw`). A real
# end-to-end call additionally needs: a Telnyx number pointed at this WS
# server (behind a TLS terminator, since Telnyx dials wss to you), an
# OPENAI_API_KEY in the environment, and a TLS-enabled swc build for the
# OpenAI client leg (`make SWARMRT_TLS=1` on macOS; on by default on Linux).
#
# All bridge frames are built by lib/Voice.sw — the hot path here is just
# string shuttling.

module VoiceAgent

import Voice

# ── config ────────────────────────────────────────────────────────────

# g711_ulaw 8kHz on both sides → Telnyx PCMU passes straight through.
fun realtime_opts() {
  o = map_put(map_new(), "model", "gpt-realtime")
  o = map_put(o, "api_key", getenv("OPENAI_API_KEY"))
  map_put(o, "format", "g711_ulaw")
}

fun session_opts() {
  o = map_put(map_new(), "format", "g711_ulaw")
  o = map_put(o, "voice", "alloy")
  map_put(o, "instructions", "You are a concise, friendly phone agent.")
}

# ── supervisor + listener ───────────────────────────────────────────────

fun main() {
  # The acceptor is a named, supervised process. http_listen binds the WS
  # server's events to whoever calls it — so the acceptor calls it itself.
  sup = supervise('one_for_one', [
    {'voice_acceptor', fun() { acceptor_init() }, 'permanent'}
  ])
  print("voice_agent supervisor:", sup)
  receive { 'never' -> 'done' }   # park main forever
}

fun acceptor_init() {
  http_listen(8080)
  print("voice_agent listening on ws://0.0.0.0:8080 (Telnyx Media Streaming)")
  acceptor_loop()
}

# The acceptor receives ws_message/ws_close for EVERY new connection until
# that connection is re-homed to its own call process via ws_set_handler.
# Telnyx opens the socket, sends 'connected', then 'start' — on 'start' we
# spawn the per-call bridge and hand the connection off to it.
fun acceptor_loop() {
  receive {
    {'ws_message', conn, text} ->
      case Voice.telnyx_event(text) {
        "start" ->
          stream_id = Voice.frame_field(text, "stream_id")
          # spawn the isolated per-call process, then transfer this WS to it.
          call = spawn(fun() { call_init(conn, stream_id) })
          ws_set_handler(conn, call)
          print(f"call up: stream={stream_id} pid={call}")
          acceptor_loop()
        _ ->
          # 'connected' (and anything pre-start) — ignore, keep accepting.
          acceptor_loop()
      }
    {'ws_close', _conn} ->
      acceptor_loop()
  }
}

# ── per-call bridge process (one per phone call) ─────────────────────────

fun call_init(conn, stream_id) {
  # open this call's private OpenAI Realtime client (wss + auth header).
  oa = Voice.realtime_connect(realtime_opts())
  case oa {
    'nil' ->
      print(f"[{stream_id}] OpenAI connect FAILED")
      ws_close(conn)
    _ ->
      wsc_send(oa, Voice.session_update(session_opts()))
      print(f"[{stream_id}] bridge live")
      bridge(conn, oa, stream_id)
  }
}

# THE BRIDGE LOOP. Two async streams interleaved in ONE process:
#   - Telnyx → us arrives as {'ws_message', conn, text} mailbox messages
#     (pushed by the WS server bridge). We drain those with selective recv.
#   - OpenAI → us is a client socket we must POLL (wsc_recv). We poll it in
#     the `after` clause: if no Telnyx frame arrived within 5ms, check OA.
# This gives fair, low-latency interleaving without threads.
fun bridge(conn, oa, sid) {
  receive {
    # ----- inbound: caller audio + control from Telnyx -----
    {'ws_message', conn2, text} ->
      case Voice.telnyx_event(text) {
        "media" ->
          # base64 PCMU straight through to OpenAI (no transcode).
          b64 = Voice.frame_field(text, "payload")
          wsc_send(oa, Voice.realtime_append(b64))
          bridge(conn2, oa, sid)
        "dtmf" ->
          bridge(conn2, oa, sid)
        "stop" ->
          print(f"[{sid}] caller hung up")
          wsc_close(oa)
        _ ->
          bridge(conn2, oa, sid)
      }
    {'ws_close', _c} ->
      print(f"[{sid}] telnyx socket closed")
      wsc_close(oa)

    # ----- outbound: poll OpenAI, forward its audio to Telnyx -----
    after 5 {
      drained = drain_openai(conn, oa, sid, 0)
      case drained {
        'closed' -> ws_close(conn)         # OA hung up → end the call
        _        -> bridge(conn, oa, sid)
      }
    }
  }
}

# Drain up to a bounded burst of OpenAI frames per tick (so one busy side
# can't starve the other). Returns 'ok' or 'closed'.
fun drain_openai(conn, oa, sid, count) {
  msg = Voice.realtime_poll(oa)             # non-blocking poll
  case msg {
    'nil' -> 'ok'                            # nothing pending right now
    _ ->
      done = handle_openai(conn, oa, sid, msg)
      case done {
        'closed' -> 'closed'
        _ ->
          if (count >= 16) { 'ok' }          # burst cap → yield to Telnyx side
          else { drain_openai(conn, oa, sid, count + 1) }
      }
  }
}

fun handle_openai(conn, oa, sid, text) {
  case Voice.openai_type(text) {
    "response.audio.delta" ->
      # base64 g711 from OpenAI → Telnyx outbound media (no transcode).
      b64 = Voice.frame_field(text, "delta")
      ws_send(conn, Voice.telnyx_media(b64))
      'ok'
    "input_audio_buffer.speech_started" ->
      # BARGE-IN: caller started talking over the agent. Stop the model's
      # current response AND flush whatever audio Telnyx still has queued.
      wsc_send(oa, Voice.realtime_cancel())
      ws_send(conn, Voice.telnyx_clear())
      'ok'
    "response.done" -> 'ok'
    "error" ->
      print(f"[{sid}] OA error: {text}")
      'ok'
    _ -> 'ok'
  }
}
