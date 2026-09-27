# Audio.sw — G.711 mu-law / PCM16 codecs and resampling for voice agents.
# A battery: the codecs are compiled into a program only when it imports
# this module (see also lib/Voice.sw for the Realtime / telephony wiring).
#
#   import Audio
#   pcm  = Audio.ulaw_to_pcm16(b64_ulaw)          # base64 in, base64 out
#   ulaw = Audio.pcm16_to_ulaw(b64_pcm)
#   up   = Audio.resample(b64_pcm, 8000, 16000)   # little-endian PCM16
#
# The *_bytes variants take and return bytes values instead of base64.
# A module that imports Audio may also call the audio_* builtins directly;
# one that doesn't is rejected at compile time.

module Audio

export [ulaw_to_pcm16, pcm16_to_ulaw, resample,
        ulaw_to_pcm16_bytes, pcm16_to_ulaw_bytes, resample_bytes]

fun ulaw_to_pcm16(b64) { audio_ulaw_to_pcm16(b64) }
fun pcm16_to_ulaw(b64) { audio_pcm16_to_ulaw(b64) }
fun resample(b64, from_hz, to_hz) { audio_resample(b64, from_hz, to_hz) }

fun ulaw_to_pcm16_bytes(b) { audio_ulaw_to_pcm16_b(b) }
fun pcm16_to_ulaw_bytes(b) { audio_pcm16_to_ulaw_b(b) }
fun resample_bytes(b, from_hz, to_hz) { audio_resample_b(b, from_hz, to_hz) }
