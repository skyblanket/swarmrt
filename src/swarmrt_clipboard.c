/*
 * swarmrt_clipboard.c — Cross-platform clipboard (text-only)
 *
 * macOS:   ObjC runtime (objc_msgSend) → NSPasteboard
 * Linux:   wl-paste/wl-copy (Wayland) or xclip (X11), via popen()
 * Windows: Win32 OpenClipboard / GetClipboardData(CF_UNICODETEXT)
 */

#include "swarmrt_clipboard.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 * macOS — ObjC runtime, pure C (no .m files)
 * ============================================================ */
#ifdef __APPLE__

#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <objc/NSObjCRuntime.h>

/* objc_msgSend has variable signature — cast for each call pattern */
typedef id   (*msg_id)(id, SEL);
typedef id   (*msg_id_id)(id, SEL, id);
typedef void (*msg_void)(id, SEL);
typedef BOOL (*msg_bool_id)(id, SEL, id);
typedef NSInteger (*msg_int)(id, SEL);
typedef const char* (*msg_cstr)(id, SEL);

static id nsstring(const char *s) {
    id cls = (id)objc_getClass("NSString");
    SEL sel = sel_registerName("stringWithUTF8String:");
    return ((id(*)(id, SEL, const char*))objc_msgSend)(cls, sel, s);
}

static id pasteboard(void) {
    Class cls = objc_getClass("NSPasteboard");
    SEL sel = sel_registerName("generalPasteboard");
    return ((msg_id)objc_msgSend)((id)cls, sel);
}

static id ns_pasteboard_type_string(void) {
    return nsstring("public.utf8-plain-text");
}

int sw_clipboard_get_text(char **out_text, uint32_t *out_len) {
    if (!out_text || !out_len) return SWD_ERR_INIT;

    /* Push autorelease pool */
    id pool = ((msg_id)objc_msgSend)((id)objc_getClass("NSAutoreleasePool"),
                                      sel_registerName("new"));

    id pb = pasteboard();
    id type = ns_pasteboard_type_string();

    SEL selStr = sel_registerName("stringForType:");
    id str = ((msg_id_id)objc_msgSend)(pb, selStr, type);

    if (!str) {
        ((msg_void)objc_msgSend)(pool, sel_registerName("drain"));
        *out_text = NULL;
        *out_len = 0;
        return SWD_ERR_PLATFORM;
    }

    const char *utf8 = ((msg_cstr)objc_msgSend)(str, sel_registerName("UTF8String"));
    if (!utf8) {
        ((msg_void)objc_msgSend)(pool, sel_registerName("drain"));
        *out_text = NULL;
        *out_len = 0;
        return SWD_ERR_PLATFORM;
    }

    size_t slen = strlen(utf8);
    *out_text = (char *)malloc(slen + 1);
    if (!*out_text) {
        ((msg_void)objc_msgSend)(pool, sel_registerName("drain"));
        return SWD_ERR_INIT;
    }
    memcpy(*out_text, utf8, slen + 1);
    *out_len = (uint32_t)slen;

    ((msg_void)objc_msgSend)(pool, sel_registerName("drain"));
    return SWD_OK;
}

int sw_clipboard_set_text(const char *text, uint32_t len) {
    if (!text) return SWD_ERR_INIT;
    (void)len; /* NSString handles length from null-terminated string */

    id pool = ((msg_id)objc_msgSend)((id)objc_getClass("NSAutoreleasePool"),
                                      sel_registerName("new"));

    id pb = pasteboard();

    /* clearContents */
    ((msg_int)objc_msgSend)(pb, sel_registerName("clearContents"));

    /* setString:forType: */
    id str = nsstring(text);
    id type = ns_pasteboard_type_string();
    SEL selSet = sel_registerName("setString:forType:");
    BOOL ok = ((BOOL(*)(id, SEL, id, id))objc_msgSend)(pb, selSet, str, type);

    ((msg_void)objc_msgSend)(pool, sel_registerName("drain"));
    return ok ? SWD_OK : SWD_ERR_PLATFORM;
}

int sw_clipboard_has_text(void) {
    id pool = ((msg_id)objc_msgSend)((id)objc_getClass("NSAutoreleasePool"),
                                      sel_registerName("new"));

    id pb = pasteboard();
    id type = ns_pasteboard_type_string();

    /* availableTypeFromArray: — pass single-element array */
    Class arrCls = objc_getClass("NSArray");
    SEL selArr = sel_registerName("arrayWithObject:");
    id arr = ((msg_id_id)objc_msgSend)((id)arrCls, selArr, type);

    SEL selAvail = sel_registerName("availableTypeFromArray:");
    id result = ((msg_id_id)objc_msgSend)(pb, selAvail, arr);

    ((msg_void)objc_msgSend)(pool, sel_registerName("drain"));
    return result ? 1 : 0;
}

int sw_clipboard_clear(void) {
    id pool = ((msg_id)objc_msgSend)((id)objc_getClass("NSAutoreleasePool"),
                                      sel_registerName("new"));

    id pb = pasteboard();
    ((msg_int)objc_msgSend)(pb, sel_registerName("clearContents"));

    ((msg_void)objc_msgSend)(pool, sel_registerName("drain"));
    return SWD_OK;
}

/* ============================================================
 * Linux — wl-paste/wl-copy or xclip via popen
 * ============================================================ */
#elif defined(__linux__)

static int is_wayland(void) {
    return getenv("WAYLAND_DISPLAY") != NULL;
}

static int is_x11(void) {
    return getenv("DISPLAY") != NULL;
}

int sw_clipboard_get_text(char **out_text, uint32_t *out_len) {
    if (!out_text || !out_len) return SWD_ERR_INIT;

    const char *cmd;
    if (is_wayland())
        cmd = "wl-paste --no-newline 2>/dev/null";
    else if (is_x11())
        cmd = "xclip -selection clipboard -o 2>/dev/null";
    else
        return SWD_ERR_PLATFORM;

    FILE *fp = popen(cmd, "r");
    if (!fp) return SWD_ERR_PLATFORM;

    size_t cap = 4096, used = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { pclose(fp); return SWD_ERR_INIT; }

    size_t n;
    while ((n = fread(buf + used, 1, cap - used - 1, fp)) > 0) {
        used += n;
        if (used + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); pclose(fp); return SWD_ERR_INIT; }
            buf = nb;
        }
    }
    pclose(fp);

    buf[used] = '\0';
    *out_text = buf;
    *out_len = (uint32_t)used;
    return SWD_OK;
}

int sw_clipboard_set_text(const char *text, uint32_t len) {
    if (!text) return SWD_ERR_INIT;

    const char *cmd;
    if (is_wayland())
        cmd = "wl-copy 2>/dev/null";
    else if (is_x11())
        cmd = "xclip -selection clipboard 2>/dev/null";
    else
        return SWD_ERR_PLATFORM;

    FILE *fp = popen(cmd, "w");
    if (!fp) return SWD_ERR_PLATFORM;

    fwrite(text, 1, len, fp);
    int rc = pclose(fp);
    return (rc == 0) ? SWD_OK : SWD_ERR_PLATFORM;
}

int sw_clipboard_has_text(void) {
    char *text = NULL;
    uint32_t len = 0;
    int rc = sw_clipboard_get_text(&text, &len);
    if (rc == SWD_OK) {
        int has = (len > 0) ? 1 : 0;
        free(text);
        return has;
    }
    return 0;
}

int sw_clipboard_clear(void) {
    return sw_clipboard_set_text("", 0);
}

/* ============================================================
 * Windows — Win32 OpenClipboard / GetClipboardData
 * ============================================================ */
#elif defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int sw_clipboard_get_text(char **out_text, uint32_t *out_len) {
    if (!out_text || !out_len) return SWD_ERR_INIT;

    if (!OpenClipboard(NULL)) return SWD_ERR_PLATFORM;

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        *out_text = NULL;
        *out_len = 0;
        return SWD_ERR_PLATFORM;
    }

    LPCWSTR wstr = (LPCWSTR)GlobalLock(hData);
    if (!wstr) {
        CloseClipboard();
        return SWD_ERR_PLATFORM;
    }

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (utf8_len <= 0) {
        GlobalUnlock(hData);
        CloseClipboard();
        return SWD_ERR_PLATFORM;
    }

    *out_text = (char *)malloc(utf8_len);
    if (!*out_text) {
        GlobalUnlock(hData);
        CloseClipboard();
        return SWD_ERR_INIT;
    }

    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, *out_text, utf8_len, NULL, NULL);
    *out_len = (uint32_t)(utf8_len - 1); /* exclude null terminator */

    GlobalUnlock(hData);
    CloseClipboard();
    return SWD_OK;
}

int sw_clipboard_set_text(const char *text, uint32_t len) {
    if (!text) return SWD_ERR_INIT;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, (int)len, NULL, 0);
    if (wlen <= 0) return SWD_ERR_PLATFORM;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(WCHAR));
    if (!hMem) return SWD_ERR_INIT;

    LPWSTR wstr = (LPWSTR)GlobalLock(hMem);
    MultiByteToWideChar(CP_UTF8, 0, text, (int)len, wstr, wlen);
    wstr[wlen] = L'\0';
    GlobalUnlock(hMem);

    if (!OpenClipboard(NULL)) {
        GlobalFree(hMem);
        return SWD_ERR_PLATFORM;
    }

    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, hMem);
    /* System takes ownership of hMem — do NOT GlobalFree */
    CloseClipboard();
    return SWD_OK;
}

int sw_clipboard_has_text(void) {
    return IsClipboardFormatAvailable(CF_UNICODETEXT) ? 1 : 0;
}

int sw_clipboard_clear(void) {
    if (!OpenClipboard(NULL)) return SWD_ERR_PLATFORM;
    EmptyClipboard();
    CloseClipboard();
    return SWD_OK;
}

#else
#error "Unsupported platform for swarmrt_clipboard"
#endif
