/*
 * swarmrt_battery_interp.c — battery builtins for the interpreter.
 *
 * Batteries (see g_batteries in swarmrt_lang.c) are compiled into a program
 * only when it imports them. swarmrt_lang.c is linked into every compiled
 * program, so the interpreter's copies live here instead, in a file linked
 * only into the swc binary (`swc run`, `swc test`, the REPL), which installs
 * them at startup. The resolver has already checked that the calling module
 * imports the battery.
 *
 * Audio: G.711 mu-law / PCM16 / resample, base64 and bytes forms (twins of
 * the compiled path's _builtin_audio_* in swarmrt_builtins_studio.h).
 * Pdf: the shared sw_pdf_builtin_* (swarmrt_pdf.c).
 * Chrome: not available in the interpreter (chrome_launch warns and returns
 * nil there); build the program to use it.
 */

#include "swarmrt_lang.h"
#include "swarmrt_audio.h"
#include "swarmrt_pdf.h"
#include <stdlib.h>
#include <string.h>

static sw_val_t *interp_battery_call(const char *fname, sw_val_t **args, int nargs) {
    if (strcmp(fname, "pdf_text") == 0)  return sw_pdf_builtin_text(args, nargs);
    if (strcmp(fname, "pdf_pages") == 0) return sw_pdf_builtin_pages(args, nargs);
    if (strcmp(fname, "pdf_meta") == 0)  return sw_pdf_builtin_meta(args, nargs);

    /* === Audio codecs (base64 in/out, fully binary-safe) ======== */
    if (strcmp(fname, "audio_ulaw_to_pcm16") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        size_t inlen = 0; uint8_t *ulaw = _sw_audio_b64_decode(args[0]->v.str, &inlen);
        if (!ulaw) return sw_val_nil();
        size_t pcmlen = 0; uint8_t *pcm = _sw_ulaw_to_pcm16(ulaw, inlen, &pcmlen);
        free(ulaw); if (!pcm) return sw_val_nil();
        char *b64 = _sw_audio_b64_encode(pcm, pcmlen); free(pcm);
        if (!b64) return sw_val_nil();
        sw_val_t *r = sw_val_string(b64); free(b64); return r;
    }
    if (strcmp(fname, "audio_pcm16_to_ulaw") == 0 && nargs >= 1 && args[0]->type == SW_VAL_STRING) {
        size_t inlen = 0; uint8_t *pcm = _sw_audio_b64_decode(args[0]->v.str, &inlen);
        if (!pcm) return sw_val_nil();
        size_t ulen = 0; uint8_t *ulaw = _sw_pcm16_to_ulaw(pcm, inlen, &ulen);
        free(pcm); if (!ulaw) return sw_val_nil();
        char *b64 = _sw_audio_b64_encode(ulaw, ulen); free(ulaw);
        if (!b64) return sw_val_nil();
        sw_val_t *r = sw_val_string(b64); free(b64); return r;
    }
    if (strcmp(fname, "audio_resample") == 0 && nargs >= 3 &&
        args[0]->type == SW_VAL_STRING && args[1]->type == SW_VAL_INT && args[2]->type == SW_VAL_INT) {
        size_t inlen = 0; uint8_t *pcm = _sw_audio_b64_decode(args[0]->v.str, &inlen);
        if (!pcm) return sw_val_nil();
        size_t outlen = 0;
        uint8_t *out = _sw_pcm16_resample(pcm, inlen, (int)args[1]->v.i, (int)args[2]->v.i, &outlen);
        free(pcm); if (!out) return sw_val_nil();
        char *b64 = _sw_audio_b64_encode(out, outlen); free(out);
        if (!b64) return sw_val_nil();
        sw_val_t *r = sw_val_string(b64); free(b64); return r;
    }

    /* === Bytes-native audio codec twins (NUL-safe; parity) ====== */
    if (strcmp(fname, "audio_ulaw_to_pcm16_b") == 0 && nargs >= 1 && args[0]->type == SW_VAL_BYTES) {
        size_t pcmlen = 0;
        uint8_t *pcm = _sw_ulaw_to_pcm16(args[0]->v.bytes.data, args[0]->v.bytes.len, &pcmlen);
        if (!pcm) return sw_val_nil();
        sw_val_t *r = sw_val_bytes(pcm, pcmlen); free(pcm); return r;
    }
    if (strcmp(fname, "audio_pcm16_to_ulaw_b") == 0 && nargs >= 1 && args[0]->type == SW_VAL_BYTES) {
        size_t ulen = 0;
        uint8_t *ulaw = _sw_pcm16_to_ulaw(args[0]->v.bytes.data, args[0]->v.bytes.len, &ulen);
        if (!ulaw) return sw_val_nil();
        sw_val_t *r = sw_val_bytes(ulaw, ulen); free(ulaw); return r;
    }
    if (strcmp(fname, "audio_resample_b") == 0 && nargs >= 3 &&
        args[0]->type == SW_VAL_BYTES && args[1]->type == SW_VAL_INT && args[2]->type == SW_VAL_INT) {
        size_t outlen = 0;
        uint8_t *out = _sw_pcm16_resample(args[0]->v.bytes.data, args[0]->v.bytes.len,
                                          (int)args[1]->v.i, (int)args[2]->v.i, &outlen);
        if (!out) return sw_val_nil();
        sw_val_t *r = sw_val_bytes(out, outlen); free(out); return r;
    }
    return NULL;
}

void sw_interp_batteries_install(void) {
    sw_interp_battery_call = interp_battery_call;
}
