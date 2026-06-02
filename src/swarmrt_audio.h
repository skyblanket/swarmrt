/*
 * swarmrt_audio.h — pure G.711 (mu-law) + linear PCM codec helpers for
 * native real-time voice agents.
 *
 * DESIGN: every builtin exposed to sw is base64-in / base64-out. Raw
 * 16-bit PCM samples (which contain NUL bytes and so cannot live in a
 * sw string — sw strings are NUL-terminated `char *`, see
 * swarmrt_lang.h) exist ONLY transiently inside C between a base64
 * decode and a base64 encode. sw never holds raw bytes. This is
 * "Option B" from the voice plan: deliver codecs with zero core-runtime
 * change.
 *
 * The functions here are header-only static helpers so both the codegen
 * path (swarmrt_builtins_studio.h) and the tree-walking interpreter
 * (swarmrt_lang.c) can share one implementation with no link surface.
 *
 * mu-law follows ITU-T G.711 (the "mu-law" / PCMU companding used by
 * North-American PSTN and by Telnyx Media Streaming + OpenAI Realtime
 * g711_ulaw). 8 kHz, one 8-bit code per sample.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_AUDIO_H
#define SWARMRT_AUDIO_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* === base64 (binary-safe, length-carrying — NOT the sw-string one) ===
 *
 * These return a malloc'd buffer + length via out params so NUL bytes in
 * decoded PCM survive. The caller frees. Distinct from _sw_b64_encode in
 * swarmrt_builtins_studio.h (which is NUL-terminated/string oriented). */

/* Encode `len` raw bytes → base64 string (NUL-terminated, no newlines).
 * Returns a malloc'd C string the caller must free, or NULL on OOM. */
static char *_sw_audio_b64_encode(const uint8_t *in, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_cap = ((len + 2) / 3) * 4 + 1;
    char *out = (char *)malloc(out_cap);
    if (!out) return NULL;
    size_t i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        out[j++] = tbl[(in[i] >> 2) & 0x3f];
        out[j++] = tbl[((in[i] & 0x03) << 4) | ((in[i+1] >> 4) & 0x0f)];
        out[j++] = tbl[((in[i+1] & 0x0f) << 2) | ((in[i+2] >> 6) & 0x03)];
        out[j++] = tbl[in[i+2] & 0x3f];
    }
    if (i < len) {
        out[j++] = tbl[(in[i] >> 2) & 0x3f];
        if (i + 1 < len) {
            out[j++] = tbl[((in[i] & 0x03) << 4) | ((in[i+1] >> 4) & 0x0f)];
            out[j++] = tbl[(in[i+1] & 0x0f) << 2];
        } else {
            out[j++] = tbl[(in[i] & 0x03) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = 0;
    return out;
}

/* Decode base64 string → malloc'd raw byte buffer. *out_len gets the
 * decoded length. Returns NULL on bad input or OOM (then *out_len=0).
 * Tolerates whitespace and missing padding. Binary-safe (NULs survive). */
static uint8_t *_sw_audio_b64_decode(const char *src, size_t *out_len) {
    static const signed char dec[256] = {
        ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
        ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
        ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
        ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
        ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
        ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63
    };
    *out_len = 0;
    if (!src) return NULL;
    size_t slen = strlen(src);
    size_t cap = (slen / 4 + 1) * 3 + 1;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) return NULL;
    size_t olen = 0;
    unsigned int buf = 0; int bits = 0;   /* buf unsigned: base64 sextet accumulator must not signed-shift (UB) */
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        signed char v = dec[c];
        if (v < 0) { free(out); return NULL; }
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[olen++] = (uint8_t)((buf >> bits) & 0xff); }
    }
    *out_len = olen;
    return out;
}

/* === G.711 mu-law companding (ITU-T) === */

#define SW_ULAW_BIAS 0x84
#define SW_ULAW_CLIP 32635

/* Encode one 16-bit linear PCM sample → 8-bit mu-law code. */
static uint8_t _sw_pcm16_to_ulaw_sample(int16_t pcm) {
    int sign = (pcm >> 8) & 0x80;
    if (sign) pcm = (int16_t)-pcm;
    if (pcm > SW_ULAW_CLIP) pcm = SW_ULAW_CLIP;
    pcm = (int16_t)(pcm + SW_ULAW_BIAS);
    int exponent = 7;
    for (int mask = 0x4000; (pcm & mask) == 0 && exponent > 0; mask >>= 1)
        exponent--;
    int mantissa = (pcm >> (exponent + 3)) & 0x0f;
    uint8_t code = (uint8_t)~(sign | (exponent << 4) | mantissa);
    return code;
}

/* Decode one 8-bit mu-law code → 16-bit linear PCM sample. */
static int16_t _sw_ulaw_to_pcm16_sample(uint8_t code) {
    code = (uint8_t)~code;
    int sign = code & 0x80;
    int exponent = (code >> 4) & 0x07;
    int mantissa = code & 0x0f;
    int sample = ((mantissa << 3) + SW_ULAW_BIAS) << exponent;
    sample -= SW_ULAW_BIAS;
    return (int16_t)(sign ? -sample : sample);
}

/* mu-law bytes (n codes) → little-endian PCM16 (2n bytes). Caller frees. */
static uint8_t *_sw_ulaw_to_pcm16(const uint8_t *ulaw, size_t n, size_t *out_len) {
    uint8_t *out = (uint8_t *)malloc(n * 2 + 1);
    if (!out) { *out_len = 0; return NULL; }
    for (size_t i = 0; i < n; i++) {
        int16_t s = _sw_ulaw_to_pcm16_sample(ulaw[i]);
        out[i*2]   = (uint8_t)(s & 0xff);
        out[i*2+1] = (uint8_t)((s >> 8) & 0xff);
    }
    *out_len = n * 2;
    return out;
}

/* little-endian PCM16 (2n bytes) → mu-law (n codes). Caller frees.
 * An odd trailing byte is ignored. */
static uint8_t *_sw_pcm16_to_ulaw(const uint8_t *pcm, size_t len, size_t *out_len) {
    size_t n = len / 2;
    uint8_t *out = (uint8_t *)malloc(n + 1);
    if (!out) { *out_len = 0; return NULL; }
    for (size_t i = 0; i < n; i++) {
        int16_t s = (int16_t)((uint16_t)pcm[i*2] | ((uint16_t)pcm[i*2+1] << 8));
        out[i] = _sw_pcm16_to_ulaw_sample(s);
    }
    *out_len = n;
    return out;
}

/* === Linear PCM16 resampler (mono) ===
 *
 * Simple linear interpolation between integer rates. Good enough for the
 * 8k/16k/24k voice bridges (G.711 is 8 kHz, OpenAI PCM16 is 24 kHz,
 * Deepgram/most ASR want 16 kHz). Not a polyphase anti-alias filter —
 * documented limitation; fine for telephony-band speech. Caller frees. */
static uint8_t *_sw_pcm16_resample(const uint8_t *pcm, size_t len,
                                   int from_hz, int to_hz, size_t *out_len) {
    *out_len = 0;
    if (from_hz <= 0 || to_hz <= 0) return NULL;
    size_t n_in = len / 2;
    if (n_in == 0) { uint8_t *e = (uint8_t *)malloc(1); if (e) e[0]=0; return e; }
    if (from_hz == to_hz) {
        size_t b = n_in * 2;
        uint8_t *out = (uint8_t *)malloc(b + 1);
        if (!out) return NULL;
        memcpy(out, pcm, b);
        *out_len = b;
        return out;
    }
    /* output sample count, rounded */
    size_t n_out = (size_t)(((double)n_in * (double)to_hz) / (double)from_hz + 0.5);
    if (n_out == 0) n_out = 1;
    uint8_t *out = (uint8_t *)malloc(n_out * 2 + 1);
    if (!out) return NULL;
    const int16_t *src = (const int16_t *)pcm; /* note: assumes LE host; we
                                                  read via bytes below to stay
                                                  endian-correct */
    (void)src;
    double ratio = (double)(n_in - 1) / (double)(n_out > 1 ? (n_out - 1) : 1);
    for (size_t i = 0; i < n_out; i++) {
        double pos = (double)i * ratio;
        size_t i0 = (size_t)pos;
        size_t i1 = i0 + 1 < n_in ? i0 + 1 : i0;
        double frac = pos - (double)i0;
        int16_t s0 = (int16_t)((uint16_t)pcm[i0*2] | ((uint16_t)pcm[i0*2+1] << 8));
        int16_t s1 = (int16_t)((uint16_t)pcm[i1*2] | ((uint16_t)pcm[i1*2+1] << 8));
        int32_t v = (int32_t)((double)s0 + ((double)s1 - (double)s0) * frac);
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        out[i*2]   = (uint8_t)(v & 0xff);
        out[i*2+1] = (uint8_t)((v >> 8) & 0xff);
    }
    *out_len = n_out * 2;
    return out;
}

#endif /* SWARMRT_AUDIO_H */
