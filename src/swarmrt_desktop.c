/*
 * swarmrt_desktop.c — Cross-platform desktop automation
 *
 * macOS:   CoreGraphics (CGEventCreateMouseEvent, CGEventKeyboardSetUnicodeString)
 * Linux:   /dev/uinput (struct input_event, absolute coords)
 * Windows: SendInput (MOUSEEVENTF_ABSOLUTE, KEYEVENTF_UNICODE)
 *
 * Features: click (left/right/middle/double), mouse_down/up, drag, type_text (Unicode),
 *           press_key, key_combo (modifiers), scroll (vertical + horizontal),
 *           get_mouse_pos, get_screen_size, permission check.
 */

#include "swarmrt_desktop.h"
#include <string.h>

/* ============================================================
 * Shared tiling math (all platforms)
 * ============================================================ */

static void calc_tile_rect(int pos, int sx, int sy, int sw, int sh,
                           int *ox, int *oy, int *ow, int *oh) {
    switch (pos) {
        case SWD_TILE_LEFT_HALF:         *ox=sx; *oy=sy; *ow=sw/2; *oh=sh; break;
        case SWD_TILE_RIGHT_HALF:        *ox=sx+sw/2; *oy=sy; *ow=sw-sw/2; *oh=sh; break;
        case SWD_TILE_TOP_HALF:          *ox=sx; *oy=sy; *ow=sw; *oh=sh/2; break;
        case SWD_TILE_BOTTOM_HALF:       *ox=sx; *oy=sy+sh/2; *ow=sw; *oh=sh-sh/2; break;
        case SWD_TILE_TOP_LEFT:          *ox=sx; *oy=sy; *ow=sw/2; *oh=sh/2; break;
        case SWD_TILE_TOP_RIGHT:         *ox=sx+sw/2; *oy=sy; *ow=sw-sw/2; *oh=sh/2; break;
        case SWD_TILE_BOTTOM_LEFT:       *ox=sx; *oy=sy+sh/2; *ow=sw/2; *oh=sh-sh/2; break;
        case SWD_TILE_BOTTOM_RIGHT:      *ox=sx+sw/2; *oy=sy+sh/2; *ow=sw-sw/2; *oh=sh-sh/2; break;
        case SWD_TILE_LEFT_THIRD:        *ox=sx; *oy=sy; *ow=sw/3; *oh=sh; break;
        case SWD_TILE_CENTER_THIRD:      *ox=sx+sw/3; *oy=sy; *ow=sw/3; *oh=sh; break;
        case SWD_TILE_RIGHT_THIRD:       *ox=sx+2*sw/3; *oy=sy; *ow=sw-2*sw/3; *oh=sh; break;
        case SWD_TILE_LEFT_TWO_THIRDS:   *ox=sx; *oy=sy; *ow=2*sw/3; *oh=sh; break;
        case SWD_TILE_RIGHT_TWO_THIRDS:  *ox=sx+sw/3; *oy=sy; *ow=sw-sw/3; *oh=sh; break;
        case SWD_TILE_MAXIMIZE:          *ox=sx; *oy=sy; *ow=sw; *oh=sh; break;
        case SWD_TILE_CENTER:            *ow=sw*2/3; *oh=sh*2/3; *ox=sx+(sw-*ow)/2; *oy=sy+(sh-*oh)/2; break;
        default:                         *ox=sx; *oy=sy; *ow=sw; *oh=sh; break;
    }
}

/* Helper: tile a window given its id */
static int tile_window_impl(uint64_t window_id, int position) {
    int sx, sy, sw, sh;
    int rc = sw_desktop_get_usable_rect(&sx, &sy, &sw, &sh);
    if (rc != SWD_OK) return rc;

    int ox, oy, ow, oh;
    calc_tile_rect(position, sx, sy, sw, sh, &ox, &oy, &ow, &oh);
    return sw_desktop_set_window_frame(window_id, ox, oy, ow, oh);
}

int sw_desktop_tile_window(uint64_t window_id, int position) {
    return tile_window_impl(window_id, position);
}

int sw_desktop_tile_focused(int position) {
    sw_desktop_window_t win;
    int rc = sw_desktop_get_focused_window(&win);
    if (rc != SWD_OK) return rc;
    return tile_window_impl(win.window_id, position);
}

/* ============================================================
 * macOS — CoreGraphics
 * ============================================================ */
#ifdef __APPLE__

#include <pthread.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>

static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_op_mutex   = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

int sw_desktop_check_permission(void) {
    /* Check TCC accessibility permission — CGEventPost silently fails without it */
    const void *keys[]   = { kAXTrustedCheckOptionPrompt };
    const void *values[] = { kCFBooleanTrue };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    Boolean trusted = AXIsProcessTrustedWithOptions(opts);
    CFRelease(opts);
    return trusted ? SWD_OK : SWD_ERR_PERMISSION;
}

int sw_desktop_init(void) {
    pthread_mutex_lock(&g_init_mutex);
    if (!g_initialized) {
        g_initialized = 1;
    }
    pthread_mutex_unlock(&g_init_mutex);
    return SWD_OK;
}

void sw_desktop_shutdown(void) {
    pthread_mutex_lock(&g_init_mutex);
    g_initialized = 0;
    pthread_mutex_unlock(&g_init_mutex);
}

#define ENSURE_INIT() do { if (!g_initialized && sw_desktop_init() != SWD_OK) return SWD_ERR_INIT; } while(0)

/* --- Mouse --- */

int sw_desktop_mouse_move(int x, int y) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    CGPoint point = CGPointMake((CGFloat)x, (CGFloat)y);
    CGEventRef event = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, point, kCGMouseButtonLeft);
    if (!event) { pthread_mutex_unlock(&g_op_mutex); return SWD_ERR_PLATFORM; }
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

static int macos_click_internal(CGPoint point, int click_type) {
    CGEventType down_type, up_type;
    CGMouseButton button;

    if (click_type == SWD_CLICK_RIGHT) {
        down_type = kCGEventRightMouseDown;
        up_type   = kCGEventRightMouseUp;
        button    = kCGMouseButtonRight;
    } else if (click_type == SWD_CLICK_MIDDLE) {
        down_type = kCGEventOtherMouseDown;
        up_type   = kCGEventOtherMouseUp;
        button    = kCGMouseButtonCenter;
    } else {
        down_type = kCGEventLeftMouseDown;
        up_type   = kCGEventLeftMouseUp;
        button    = kCGMouseButtonLeft;
    }

    /* Move to position first */
    CGEventRef move = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, point, button);
    if (!move) return SWD_ERR_PLATFORM;
    CGEventPost(kCGHIDEventTap, move);
    CFRelease(move);

    if (click_type == SWD_CLICK_DOUBLE) {
        /* First click */
        CGEventRef down1 = CGEventCreateMouseEvent(NULL, down_type, point, button);
        CGEventSetIntegerValueField(down1, kCGMouseEventClickState, 1);
        CGEventPost(kCGHIDEventTap, down1);
        CFRelease(down1);

        CGEventRef up1 = CGEventCreateMouseEvent(NULL, up_type, point, button);
        CGEventSetIntegerValueField(up1, kCGMouseEventClickState, 1);
        CGEventPost(kCGHIDEventTap, up1);
        CFRelease(up1);

        /* Second click — clickState=2 */
        CGEventRef down2 = CGEventCreateMouseEvent(NULL, down_type, point, button);
        CGEventSetIntegerValueField(down2, kCGMouseEventClickState, 2);
        CGEventPost(kCGHIDEventTap, down2);
        CFRelease(down2);

        CGEventRef up2 = CGEventCreateMouseEvent(NULL, up_type, point, button);
        CGEventSetIntegerValueField(up2, kCGMouseEventClickState, 2);
        CGEventPost(kCGHIDEventTap, up2);
        CFRelease(up2);
    } else {
        CGEventRef down = CGEventCreateMouseEvent(NULL, down_type, point, button);
        CGEventSetIntegerValueField(down, kCGMouseEventClickState, 1);
        CGEventPost(kCGHIDEventTap, down);
        CFRelease(down);

        CGEventRef up = CGEventCreateMouseEvent(NULL, up_type, point, button);
        CGEventSetIntegerValueField(up, kCGMouseEventClickState, 1);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(up);
    }

    return SWD_OK;
}

int sw_desktop_click(int x, int y, int click_type) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);
    int rc = macos_click_internal(CGPointMake((CGFloat)x, (CGFloat)y), click_type);
    pthread_mutex_unlock(&g_op_mutex);
    return rc;
}

static void macos_button_event(CGPoint point, int button, int down) {
    CGEventType etype;
    CGMouseButton btn;

    if (button == SWD_CLICK_RIGHT) {
        etype = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
        btn = kCGMouseButtonRight;
    } else if (button == SWD_CLICK_MIDDLE) {
        etype = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
        btn = kCGMouseButtonCenter;
    } else {
        etype = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
        btn = kCGMouseButtonLeft;
    }

    CGEventRef ev = CGEventCreateMouseEvent(NULL, etype, point, btn);
    if (ev) {
        CGEventSetIntegerValueField(ev, kCGMouseEventClickState, 1);
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

int sw_desktop_mouse_down(int x, int y, int button) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    CGPoint point = CGPointMake((CGFloat)x, (CGFloat)y);
    /* Move first */
    CGEventRef move = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, point, kCGMouseButtonLeft);
    if (move) { CGEventPost(kCGHIDEventTap, move); CFRelease(move); }
    macos_button_event(point, button, 1);

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

int sw_desktop_mouse_up(int x, int y, int button) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    CGPoint point = CGPointMake((CGFloat)x, (CGFloat)y);
    macos_button_event(point, button, 0);

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

int sw_desktop_drag(int x1, int y1, int x2, int y2, int button) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    CGPoint p1 = CGPointMake((CGFloat)x1, (CGFloat)y1);
    CGPoint p2 = CGPointMake((CGFloat)x2, (CGFloat)y2);

    /* Move to start */
    CGEventRef move1 = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, p1, kCGMouseButtonLeft);
    if (move1) { CGEventPost(kCGHIDEventTap, move1); CFRelease(move1); }

    /* Button down */
    macos_button_event(p1, button, 1);

    /* Drag to end — use LeftMouseDragged / RightMouseDragged / OtherMouseDragged */
    CGEventType drag_type;
    CGMouseButton btn;
    if (button == SWD_CLICK_RIGHT) {
        drag_type = kCGEventRightMouseDragged;
        btn = kCGMouseButtonRight;
    } else if (button == SWD_CLICK_MIDDLE) {
        drag_type = kCGEventOtherMouseDragged;
        btn = kCGMouseButtonCenter;
    } else {
        drag_type = kCGEventLeftMouseDragged;
        btn = kCGMouseButtonLeft;
    }

    CGEventRef drag = CGEventCreateMouseEvent(NULL, drag_type, p2, btn);
    if (drag) { CGEventPost(kCGHIDEventTap, drag); CFRelease(drag); }

    /* Button up at destination */
    macos_button_event(p2, button, 0);

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

int sw_desktop_get_mouse_pos(int *x, int *y) {
    CGEventRef ev = CGEventCreate(NULL);
    if (!ev) return SWD_ERR_PLATFORM;
    CGPoint pos = CGEventGetLocation(ev);
    CFRelease(ev);
    *x = (int)pos.x;
    *y = (int)pos.y;
    return SWD_OK;
}

/* --- Keyboard --- */

int sw_desktop_type_text(const char *text, uint32_t len) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    /* Convert UTF-8 to UTF-16 for CGEventKeyboardSetUnicodeString */
    /* Process in chunks — CG supports max 20 chars per event */
    const uint8_t *src = (const uint8_t *)text;
    const uint8_t *end = src + len;

    while (src < end) {
        UniChar buf[20];
        int buf_len = 0;

        while (src < end && buf_len < 20) {
            uint32_t cp;
            if ((*src & 0x80) == 0) {
                cp = *src++;
            } else if ((*src & 0xE0) == 0xC0) {
                cp = (*src++ & 0x1F) << 6;
                if (src < end) cp |= (*src++ & 0x3F);
            } else if ((*src & 0xF0) == 0xE0) {
                cp = (*src++ & 0x0F) << 12;
                if (src < end) cp |= (*src++ & 0x3F) << 6;
                if (src < end) cp |= (*src++ & 0x3F);
            } else if ((*src & 0xF8) == 0xF0) {
                cp = (*src++ & 0x07) << 18;
                if (src < end) cp |= (*src++ & 0x3F) << 12;
                if (src < end) cp |= (*src++ & 0x3F) << 6;
                if (src < end) cp |= (*src++ & 0x3F);
            } else {
                src++;
                continue;
            }

            if (cp <= 0xFFFF) {
                buf[buf_len++] = (UniChar)cp;
            } else if (buf_len < 19) {
                cp -= 0x10000;
                buf[buf_len++] = (UniChar)(0xD800 + (cp >> 10));
                buf[buf_len++] = (UniChar)(0xDC00 + (cp & 0x3FF));
            } else {
                break;
            }
        }

        if (buf_len > 0) {
            CGEventRef down = CGEventCreateKeyboardEvent(NULL, 0, true);
            CGEventKeyboardSetUnicodeString(down, buf_len, buf);
            CGEventPost(kCGHIDEventTap, down);
            CFRelease(down);

            CGEventRef up = CGEventCreateKeyboardEvent(NULL, 0, false);
            CGEventKeyboardSetUnicodeString(up, buf_len, buf);
            CGEventPost(kCGHIDEventTap, up);
            CFRelease(up);
        }
    }

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

/* macOS virtual keycodes */
static int macos_keycode(const char *name) {
    if (strcmp(name, "return") == 0 || strcmp(name, "enter") == 0)       return 0x24;
    if (strcmp(name, "escape") == 0 || strcmp(name, "esc") == 0)         return 0x35;
    if (strcmp(name, "tab") == 0)            return 0x30;
    if (strcmp(name, "space") == 0)          return 0x31;
    if (strcmp(name, "delete") == 0 || strcmp(name, "backspace") == 0)   return 0x33;
    if (strcmp(name, "forward-delete") == 0) return 0x75;
    if (strcmp(name, "up") == 0 || strcmp(name, "arrow-up") == 0)        return 0x7E;
    if (strcmp(name, "down") == 0 || strcmp(name, "arrow-down") == 0)    return 0x7D;
    if (strcmp(name, "left") == 0 || strcmp(name, "arrow-left") == 0)    return 0x7B;
    if (strcmp(name, "right") == 0 || strcmp(name, "arrow-right") == 0)  return 0x7C;
    if (strcmp(name, "home") == 0)           return 0x73;
    if (strcmp(name, "end") == 0)            return 0x77;
    if (strcmp(name, "page-up") == 0 || strcmp(name, "pageup") == 0)     return 0x74;
    if (strcmp(name, "page-down") == 0 || strcmp(name, "pagedown") == 0) return 0x79;
    /* Modifier keys */
    if (strcmp(name, "shift") == 0)          return 0x38;
    if (strcmp(name, "control") == 0 || strcmp(name, "ctrl") == 0)       return 0x3B;
    if (strcmp(name, "option") == 0 || strcmp(name, "alt") == 0)         return 0x3A;
    if (strcmp(name, "command") == 0 || strcmp(name, "cmd") == 0 || strcmp(name, "meta") == 0 || strcmp(name, "super") == 0) return 0x37;
    /* Letter keys (for combos like cmd+c) */
    if (strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'z') {
        /* macOS keycodes for a-z are not sequential — use lookup */
        static const int letter_codes[26] = {
            0x00,0x0B,0x08,0x02,0x0E,0x03,0x05,0x04,0x22,0x26,
            0x28,0x25,0x2E,0x2D,0x1F,0x23,0x0C,0x0F,0x01,0x11,
            0x20,0x09,0x0D,0x07,0x10,0x06
        };
        return letter_codes[name[0] - 'a'];
    }
    /* F-keys */
    if (strcmp(name, "f1") == 0)  return 0x7A;
    if (strcmp(name, "f2") == 0)  return 0x78;
    if (strcmp(name, "f3") == 0)  return 0x63;
    if (strcmp(name, "f4") == 0)  return 0x76;
    if (strcmp(name, "f5") == 0)  return 0x60;
    if (strcmp(name, "f6") == 0)  return 0x61;
    if (strcmp(name, "f7") == 0)  return 0x62;
    if (strcmp(name, "f8") == 0)  return 0x64;
    if (strcmp(name, "f9") == 0)  return 0x65;
    if (strcmp(name, "f10") == 0) return 0x6D;
    if (strcmp(name, "f11") == 0) return 0x67;
    if (strcmp(name, "f12") == 0) return 0x6F;
    return -1;
}

/* Map modifier flags to CGEventFlags */
static CGEventFlags macos_mod_flags(uint32_t modifiers) {
    CGEventFlags flags = 0;
    if (modifiers & SWD_MOD_SHIFT) flags |= kCGEventFlagMaskShift;
    if (modifiers & SWD_MOD_CTRL)  flags |= kCGEventFlagMaskControl;
    if (modifiers & SWD_MOD_ALT)   flags |= kCGEventFlagMaskAlternate;
    if (modifiers & SWD_MOD_META)  flags |= kCGEventFlagMaskCommand;
    return flags;
}

int sw_desktop_press_key(const char *key_name) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    int kc = macos_keycode(key_name);
    if (kc < 0) { pthread_mutex_unlock(&g_op_mutex); return SWD_ERR_PLATFORM; }

    CGEventRef down = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)kc, true);
    CGEventPost(kCGHIDEventTap, down);
    CFRelease(down);

    CGEventRef up = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)kc, false);
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(up);

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

int sw_desktop_key_combo(uint32_t modifiers, const char *key_name) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    int kc = macos_keycode(key_name);
    if (kc < 0) { pthread_mutex_unlock(&g_op_mutex); return SWD_ERR_PLATFORM; }

    CGEventFlags flags = macos_mod_flags(modifiers);

    CGEventRef down = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)kc, true);
    CGEventSetFlags(down, flags);
    CGEventPost(kCGHIDEventTap, down);
    CFRelease(down);

    CGEventRef up = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)kc, false);
    CGEventSetFlags(up, flags);
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(up);

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

/* --- Scroll --- */

int sw_desktop_scroll(int x, int y, int amount) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    CGPoint point = CGPointMake((CGFloat)x, (CGFloat)y);
    CGEventRef move = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, point, kCGMouseButtonLeft);
    if (!move) { pthread_mutex_unlock(&g_op_mutex); return SWD_ERR_PLATFORM; }
    CGEventPost(kCGHIDEventTap, move);
    CFRelease(move);

    /* CG uses positive=up, we use positive=down — negate */
    CGEventRef scroll = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, -amount);
    if (!scroll) { pthread_mutex_unlock(&g_op_mutex); return SWD_ERR_PLATFORM; }
    CGEventPost(kCGHIDEventTap, scroll);
    CFRelease(scroll);

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

int sw_desktop_scroll_h(int x, int y, int amount) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    CGPoint point = CGPointMake((CGFloat)x, (CGFloat)y);
    CGEventRef move = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, point, kCGMouseButtonLeft);
    if (!move) { pthread_mutex_unlock(&g_op_mutex); return SWD_ERR_PLATFORM; }
    CGEventPost(kCGHIDEventTap, move);
    CFRelease(move);

    /* 2 axes: vertical=0, horizontal=amount (positive=right) */
    CGEventRef scroll = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 2, 0, amount);
    if (!scroll) { pthread_mutex_unlock(&g_op_mutex); return SWD_ERR_PLATFORM; }
    CGEventPost(kCGHIDEventTap, scroll);
    CFRelease(scroll);

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

/* --- Screen info --- */

int sw_desktop_get_screen_size(int *width, int *height) {
    CGDirectDisplayID mainDisplay = CGMainDisplayID();
    *width  = (int)CGDisplayPixelsWide(mainDisplay);
    *height = (int)CGDisplayPixelsHigh(mainDisplay);
    return SWD_OK;
}

/* --- Window management (AXUIElement) --- */

#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <libproc.h>
#include <stdlib.h>

/* ObjC runtime helpers (same pattern as swarmrt_clipboard.c) */
static id objc_msg(id self, SEL sel) {
    return ((id(*)(id, SEL))objc_msgSend)(self, sel);
}
int sw_desktop_get_focused_window(sw_desktop_window_t *out) {
    ENSURE_INIT();
    memset(out, 0, sizeof(*out));

    AXUIElementRef syswide = AXUIElementCreateSystemWide();
    AXUIElementRef focusedApp = NULL;
    AXError err = AXUIElementCopyAttributeValue(syswide, kAXFocusedApplicationAttribute, (CFTypeRef *)&focusedApp);
    CFRelease(syswide);
    if (err != kAXErrorSuccess || !focusedApp) return SWD_ERR_PLATFORM;

    /* Get app name */
    CFStringRef appTitle = NULL;
    AXUIElementCopyAttributeValue(focusedApp, kAXTitleAttribute, (CFTypeRef *)&appTitle);
    if (appTitle) {
        CFStringGetCString(appTitle, out->app_name, sizeof(out->app_name), kCFStringEncodingUTF8);
        CFRelease(appTitle);
    }

    /* Get PID */
    pid_t pid = 0;
    AXUIElementGetPid(focusedApp, &pid);
    out->pid = (int32_t)pid;

    /* Get focused window */
    AXUIElementRef focusedWin = NULL;
    err = AXUIElementCopyAttributeValue(focusedApp, kAXFocusedWindowAttribute, (CFTypeRef *)&focusedWin);
    CFRelease(focusedApp);
    if (err != kAXErrorSuccess || !focusedWin) return SWD_ERR_NOT_FOUND;

    /* Get title */
    CFStringRef winTitle = NULL;
    AXUIElementCopyAttributeValue(focusedWin, kAXTitleAttribute, (CFTypeRef *)&winTitle);
    if (winTitle) {
        CFStringGetCString(winTitle, out->title, sizeof(out->title), kCFStringEncodingUTF8);
        CFRelease(winTitle);
    }

    /* Get position */
    AXValueRef posVal = NULL;
    AXUIElementCopyAttributeValue(focusedWin, kAXPositionAttribute, (CFTypeRef *)&posVal);
    if (posVal) {
        CGPoint pos;
        AXValueGetValue(posVal, kAXValueCGPointType, &pos);
        out->x = (int)pos.x;
        out->y = (int)pos.y;
        CFRelease(posVal);
    }

    /* Get size */
    AXValueRef sizeVal = NULL;
    AXUIElementCopyAttributeValue(focusedWin, kAXSizeAttribute, (CFTypeRef *)&sizeVal);
    if (sizeVal) {
        CGSize size;
        AXValueGetValue(sizeVal, kAXValueCGSizeType, &size);
        out->width = (int)size.width;
        out->height = (int)size.height;
        CFRelease(sizeVal);
    }

    /* Use CGWindowID as window_id — get via _AXUIElementGetWindow (private but stable) */
    uint32_t wid = 0;
    extern AXError _AXUIElementGetWindow(AXUIElementRef, uint32_t *);
    _AXUIElementGetWindow(focusedWin, &wid);
    out->window_id = (uint64_t)wid;
    out->focused = 1;

    CFRelease(focusedWin);
    return SWD_OK;
}

int sw_desktop_set_window_frame(uint64_t window_id, int x, int y, int w, int h) {
    ENSURE_INIT();

    /* Find the AXUIElement for this window_id by iterating running apps */
    CFArrayRef appList = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!appList) return SWD_ERR_PLATFORM;

    pid_t target_pid = 0;
    CFIndex count = CFArrayGetCount(appList);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = CFArrayGetValueAtIndex(appList, i);
        CFNumberRef numRef = CFDictionaryGetValue(info, kCGWindowNumber);
        if (!numRef) continue;
        uint32_t wid = 0;
        CFNumberGetValue(numRef, kCFNumberSInt32Type, &wid);
        if ((uint64_t)wid == window_id) {
            CFNumberRef pidRef = CFDictionaryGetValue(info, kCGWindowOwnerPID);
            if (pidRef) CFNumberGetValue(pidRef, kCFNumberSInt32Type, &target_pid);
            break;
        }
    }
    CFRelease(appList);
    if (target_pid == 0) return SWD_ERR_NOT_FOUND;

    /* Create AXUIElement for the app, then find the window */
    AXUIElementRef appRef = AXUIElementCreateApplication(target_pid);
    CFArrayRef windows = NULL;
    AXUIElementCopyAttributeValue(appRef, kAXWindowsAttribute, (CFTypeRef *)&windows);
    if (!windows) { CFRelease(appRef); return SWD_ERR_NOT_FOUND; }

    AXUIElementRef targetWin = NULL;
    for (CFIndex i = 0; i < CFArrayGetCount(windows); i++) {
        AXUIElementRef win = (AXUIElementRef)CFArrayGetValueAtIndex(windows, i);
        uint32_t wid = 0;
        extern AXError _AXUIElementGetWindow(AXUIElementRef, uint32_t *);
        _AXUIElementGetWindow(win, &wid);
        if ((uint64_t)wid == window_id) {
            targetWin = win;
            CFRetain(targetWin);
            break;
        }
    }
    CFRelease(windows);
    CFRelease(appRef);
    if (!targetWin) return SWD_ERR_NOT_FOUND;

    /* Size-Position-Size trick (Rectangle pattern — handles cross-display moves) */
    CGSize newSize = CGSizeMake((CGFloat)w, (CGFloat)h);
    AXValueRef sizeVal = AXValueCreate(kAXValueCGSizeType, &newSize);

    CGPoint newPos = CGPointMake((CGFloat)x, (CGFloat)y);
    AXValueRef posVal = AXValueCreate(kAXValueCGPointType, &newPos);

    /* First size (so position math works) */
    AXUIElementSetAttributeValue(targetWin, kAXSizeAttribute, sizeVal);
    /* Set position */
    AXUIElementSetAttributeValue(targetWin, kAXPositionAttribute, posVal);
    /* Re-apply size (for windows that auto-resize on position change) */
    AXUIElementSetAttributeValue(targetWin, kAXSizeAttribute, sizeVal);

    CFRelease(sizeVal);
    CFRelease(posVal);
    CFRelease(targetWin);
    return SWD_OK;
}

int sw_desktop_get_usable_rect(int *x, int *y, int *w, int *h) {
    /* Use NSScreen.mainScreen.visibleFrame via ObjC runtime */
    id NSScreenClass = (id)objc_getClass("NSScreen");
    if (!NSScreenClass) return SWD_ERR_PLATFORM;

    id mainScreen = objc_msg(NSScreenClass, sel_registerName("mainScreen"));
    if (!mainScreen) return SWD_ERR_PLATFORM;

    /* visibleFrame returns NSRect (CGRect) — need to use objc_msgSend_stret on some arches */
    CGRect visibleFrame;
#if defined(__aarch64__)
    /* ARM64: struct returns in registers */
    visibleFrame = ((CGRect(*)(id, SEL))objc_msgSend)(mainScreen, sel_registerName("visibleFrame"));
#else
    /* x86_64: large struct returns via stret */
    ((void(*)(CGRect *, id, SEL))objc_msgSend_stret)(&visibleFrame, mainScreen, sel_registerName("visibleFrame"));
#endif

    /* NSScreen frame has origin at bottom-left, convert to top-left */
    CGRect fullFrame;
#if defined(__aarch64__)
    fullFrame = ((CGRect(*)(id, SEL))objc_msgSend)(mainScreen, sel_registerName("frame"));
#else
    ((void(*)(CGRect *, id, SEL))objc_msgSend_stret)(&fullFrame, mainScreen, sel_registerName("frame"));
#endif

    *x = (int)visibleFrame.origin.x;
    /* Convert from bottom-left to top-left coordinate system */
    *y = (int)(fullFrame.size.height - visibleFrame.origin.y - visibleFrame.size.height);
    *w = (int)visibleFrame.size.width;
    *h = (int)visibleFrame.size.height;
    return SWD_OK;
}

int sw_desktop_find_window_by_app(const char *app_name, sw_desktop_window_t *out) {
    ENSURE_INIT();
    memset(out, 0, sizeof(*out));

    /* Find PID by scanning CGWindowList for owner name match.
     * Use kCGWindowListOptionAll to catch windows behind others too. */
    pid_t target_pid = 0;

    /* Find PID by process name using proc_listallpids + proc_name (libproc.h).
     * No Screen Recording or AppKit needed. Then verify via AXUIElement. */
    {
        int pidcount = proc_listallpids(NULL, 0);
        if (pidcount > 0) {
            pid_t *pids = (pid_t *)malloc(pidcount * sizeof(pid_t));
            if (pids) {
                pidcount = proc_listallpids(pids, pidcount * sizeof(pid_t));
                for (int i = 0; i < pidcount && target_pid == 0; i++) {
                    char pname[256];
                    if (proc_name(pids[i], pname, sizeof(pname)) > 0) {
                        if (strcasecmp(pname, app_name) == 0) {
                            target_pid = pids[i];
                            strncpy(out->app_name, pname, sizeof(out->app_name) - 1);
                        }
                    }
                }
                free(pids);
            }
        }
    }

    if (target_pid == 0) return SWD_ERR_NOT_FOUND;

    /* Get the app's windows via AXUIElement.
     * Try kAXWindowsAttribute, kAXFocusedWindowAttribute, kAXMainWindowAttribute. */
    AXUIElementRef appRef = AXUIElementCreateApplication(target_pid);
    AXUIElementRef targetWin = NULL;

    /* Method 1: kAXWindowsAttribute */
    CFArrayRef windows = NULL;
    AXError axerr = AXUIElementCopyAttributeValue(appRef, kAXWindowsAttribute, (CFTypeRef *)&windows);
    if (axerr == kAXErrorSuccess && windows && CFArrayGetCount(windows) > 0) {
        targetWin = (AXUIElementRef)CFArrayGetValueAtIndex(windows, 0);
        CFRetain(targetWin);
    }
    if (windows) CFRelease(windows);

    /* Method 2: kAXFocusedWindowAttribute */
    if (!targetWin) {
        AXUIElementCopyAttributeValue(appRef, kAXFocusedWindowAttribute, (CFTypeRef *)&targetWin);
    }

    /* Method 3: kAXMainWindowAttribute */
    if (!targetWin) {
        AXUIElementCopyAttributeValue(appRef, kAXMainWindowAttribute, (CFTypeRef *)&targetWin);
    }

    if (!targetWin) {
        CFRelease(appRef);
        return SWD_ERR_NOT_FOUND;
    }

    AXUIElementRef win = targetWin;
    out->pid = (int32_t)target_pid;

    /* Get window ID */
    uint32_t wid = 0;
    extern AXError _AXUIElementGetWindow(AXUIElementRef, uint32_t *);
    _AXUIElementGetWindow(win, &wid);
    out->window_id = (uint64_t)wid;

    /* Get title */
    CFStringRef winTitle = NULL;
    AXUIElementCopyAttributeValue(win, kAXTitleAttribute, (CFTypeRef *)&winTitle);
    if (winTitle) {
        CFStringGetCString(winTitle, out->title, sizeof(out->title), kCFStringEncodingUTF8);
        CFRelease(winTitle);
    }

    /* Get position */
    AXValueRef posVal = NULL;
    AXUIElementCopyAttributeValue(win, kAXPositionAttribute, (CFTypeRef *)&posVal);
    if (posVal) {
        CGPoint pos;
        AXValueGetValue(posVal, kAXValueCGPointType, &pos);
        out->x = (int)pos.x;
        out->y = (int)pos.y;
        CFRelease(posVal);
    }

    /* Get size */
    AXValueRef sizeVal = NULL;
    AXUIElementCopyAttributeValue(win, kAXSizeAttribute, (CFTypeRef *)&sizeVal);
    if (sizeVal) {
        CGSize size;
        AXValueGetValue(sizeVal, kAXValueCGSizeType, &size);
        out->width = (int)size.width;
        out->height = (int)size.height;
        CFRelease(sizeVal);
    }

    CFRelease(targetWin);
    CFRelease(appRef);
    return SWD_OK;
}

int sw_desktop_tile_app(const char *app_name, int position) {
    ENSURE_INIT();

    /* Get usable rect for tiling math */
    int sx, sy, sw, sh;
    int rc = sw_desktop_get_usable_rect(&sx, &sy, &sw, &sh);
    if (rc != SWD_OK) return rc;

    int ox, oy, ow, oh;
    calc_tile_rect(position, sx, sy, sw, sh, &ox, &oy, &ow, &oh);

    /* Find PID by process name */
    pid_t target_pid = 0;
    {
        int pidcount = proc_listallpids(NULL, 0);
        if (pidcount > 0) {
            pid_t *pids = (pid_t *)malloc(pidcount * sizeof(pid_t));
            if (pids) {
                pidcount = proc_listallpids(pids, pidcount * sizeof(pid_t));
                for (int i = 0; i < pidcount && target_pid == 0; i++) {
                    char pname[256];
                    if (proc_name(pids[i], pname, sizeof(pname)) > 0) {
                        if (strcasecmp(pname, app_name) == 0)
                            target_pid = pids[i];
                    }
                }
                free(pids);
            }
        }
    }
    if (target_pid == 0) return SWD_ERR_NOT_FOUND;

    /* Get AX window directly — no window_id roundtrip */
    AXUIElementRef appRef = AXUIElementCreateApplication(target_pid);
    AXUIElementRef win = NULL;

    /* Try kAXWindowsAttribute first */
    CFArrayRef windows = NULL;
    AXError axerr = AXUIElementCopyAttributeValue(appRef, kAXWindowsAttribute, (CFTypeRef *)&windows);
    if (axerr == kAXErrorSuccess && windows && CFArrayGetCount(windows) > 0) {
        win = (AXUIElementRef)CFArrayGetValueAtIndex(windows, 0);
        CFRetain(win);
    }
    if (windows) CFRelease(windows);

    /* Fallback: focused window */
    if (!win)
        AXUIElementCopyAttributeValue(appRef, kAXFocusedWindowAttribute, (CFTypeRef *)&win);

    /* Fallback: main window */
    if (!win)
        AXUIElementCopyAttributeValue(appRef, kAXMainWindowAttribute, (CFTypeRef *)&win);

    CFRelease(appRef);
    if (!win) return SWD_ERR_NOT_FOUND;

    /* Size-Position-Size trick directly on the AX element */
    CGSize newSize = CGSizeMake((CGFloat)ow, (CGFloat)oh);
    AXValueRef sizeVal = AXValueCreate(kAXValueCGSizeType, &newSize);
    CGPoint newPos = CGPointMake((CGFloat)ox, (CGFloat)oy);
    AXValueRef posVal = AXValueCreate(kAXValueCGPointType, &newPos);

    AXUIElementSetAttributeValue(win, kAXSizeAttribute, sizeVal);
    AXUIElementSetAttributeValue(win, kAXPositionAttribute, posVal);
    AXUIElementSetAttributeValue(win, kAXSizeAttribute, sizeVal);

    CFRelease(posVal);
    CFRelease(sizeVal);
    CFRelease(win);
    return SWD_OK;
}

/* ============================================================
 * Linux — /dev/uinput
 * ============================================================ */
#elif defined(__linux__)

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/uinput.h>
#include <linux/input.h>

static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_op_mutex   = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;
static int g_uinput_fd = -1;

/* Screen resolution — cached after first detection */
static int g_screen_w = 0;
static int g_screen_h = 0;

static void emit(int fd, int type, int code, int val) {
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type  = type;
    ie.code  = code;
    ie.value = val;
    (void)write(fd, &ie, sizeof(ie));
}

static void syn(int fd) {
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

/* Detect screen resolution: DRM sysfs → xrandr → fallback */
static void detect_resolution(void) {
    if (g_screen_w > 0) return;

    /* Try DRM sysfs first (no subprocess, works on Wayland) */
    FILE *fp = fopen("/sys/class/drm/card0-eDP-1/modes", "r");
    if (!fp) fp = fopen("/sys/class/drm/card0-HDMI-A-1/modes", "r");
    if (!fp) fp = fopen("/sys/class/drm/card0-DP-1/modes", "r");
    if (fp) {
        char buf[64];
        if (fgets(buf, sizeof(buf), fp)) {
            int w = 0, h = 0;
            if (sscanf(buf, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                g_screen_w = w;
                g_screen_h = h;
            }
        }
        fclose(fp);
    }

    /* Fallback: try xrandr (works on X11 + XWayland) */
    if (g_screen_w == 0) {
        fp = popen("xrandr --current 2>/dev/null | grep '\\*' | head -1 | awk '{print $1}'", "r");
        if (fp) {
            char buf[64];
            if (fgets(buf, sizeof(buf), fp)) {
                int w = 0, h = 0;
                if (sscanf(buf, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                    g_screen_w = w;
                    g_screen_h = h;
                }
            }
            pclose(fp);
        }
    }

    /* Last resort */
    if (g_screen_w == 0) { g_screen_w = 1920; g_screen_h = 1080; }
}

int sw_desktop_check_permission(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return SWD_ERR_PERMISSION;
    close(fd);
    return SWD_OK;
}

int sw_desktop_init(void) {
    pthread_mutex_lock(&g_init_mutex);
    if (g_initialized) {
        pthread_mutex_unlock(&g_init_mutex);
        return SWD_OK;
    }

    g_uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_uinput_fd < 0) {
        pthread_mutex_unlock(&g_init_mutex);
        return SWD_ERR_PERMISSION;
    }

    /* Enable event types — check ioctl return values */
    int rc = 0;
    rc |= ioctl(g_uinput_fd, UI_SET_EVBIT, EV_KEY);
    rc |= ioctl(g_uinput_fd, UI_SET_EVBIT, EV_ABS);
    rc |= ioctl(g_uinput_fd, UI_SET_EVBIT, EV_REL);
    rc |= ioctl(g_uinput_fd, UI_SET_EVBIT, EV_SYN);

    /* Mouse buttons — including middle */
    rc |= ioctl(g_uinput_fd, UI_SET_KEYBIT, BTN_LEFT);
    rc |= ioctl(g_uinput_fd, UI_SET_KEYBIT, BTN_RIGHT);
    rc |= ioctl(g_uinput_fd, UI_SET_KEYBIT, BTN_MIDDLE);

    /* Absolute axes for mouse positioning */
    rc |= ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_X);
    rc |= ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_Y);

    /* Scroll: vertical + horizontal */
    rc |= ioctl(g_uinput_fd, UI_SET_RELBIT, REL_WHEEL);
    rc |= ioctl(g_uinput_fd, UI_SET_RELBIT, REL_HWHEEL);

    /* Keyboard keys — full range including modifiers */
    for (int k = KEY_ESC; k <= KEY_F12; k++)
        ioctl(g_uinput_fd, UI_SET_KEYBIT, k);
    /* Navigation keys */
    for (int k = KEY_HOME; k <= KEY_DELETE; k++)
        ioctl(g_uinput_fd, UI_SET_KEYBIT, k);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_UP);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_DOWN);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_LEFT);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_RIGHT);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_PAGEUP);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_PAGEDOWN);
    /* Modifier keys (explicitly, not just by range) */
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_RIGHTSHIFT);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_LEFTCTRL);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_RIGHTCTRL);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_LEFTALT);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_RIGHTALT);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_LEFTMETA);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, KEY_RIGHTMETA);

    if (rc < 0) {
        close(g_uinput_fd);
        g_uinput_fd = -1;
        pthread_mutex_unlock(&g_init_mutex);
        return SWD_ERR_PLATFORM;
    }

    /* Setup uinput device */
    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "swarmrt-desktop");
    setup.id.bustype = BUS_USB;
    setup.id.vendor  = 0x1234;
    setup.id.product = 0x5678;

    /* Configure absolute axes */
    struct uinput_abs_setup abs_x = {0};
    abs_x.code = ABS_X;
    abs_x.absinfo.minimum = 0;
    abs_x.absinfo.maximum = 32767;
    ioctl(g_uinput_fd, UI_ABS_SETUP, &abs_x);

    struct uinput_abs_setup abs_y = {0};
    abs_y.code = ABS_Y;
    abs_y.absinfo.minimum = 0;
    abs_y.absinfo.maximum = 32767;
    ioctl(g_uinput_fd, UI_ABS_SETUP, &abs_y);

    if (ioctl(g_uinput_fd, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(g_uinput_fd, UI_DEV_CREATE) < 0) {
        close(g_uinput_fd);
        g_uinput_fd = -1;
        pthread_mutex_unlock(&g_init_mutex);
        return SWD_ERR_PLATFORM;
    }

    /* Give kernel time to register device */
    usleep(100000);

    detect_resolution();
    g_initialized = 1;
    pthread_mutex_unlock(&g_init_mutex);
    return SWD_OK;
}

void sw_desktop_shutdown(void) {
    pthread_mutex_lock(&g_init_mutex);
    if (g_uinput_fd >= 0) {
        ioctl(g_uinput_fd, UI_DEV_DESTROY);
        close(g_uinput_fd);
        g_uinput_fd = -1;
    }
    g_initialized = 0;
    g_screen_w = 0;
    g_screen_h = 0;
    pthread_mutex_unlock(&g_init_mutex);
}

#define ENSURE_INIT() do { if (!g_initialized) { int rc = sw_desktop_init(); if (rc != SWD_OK) return rc; } } while(0)

static void screen_to_abs(int x, int y, int *ax, int *ay) {
    *ax = (x * 32767) / g_screen_w;
    *ay = (y * 32767) / g_screen_h;
}

static void move_abs(int x, int y) {
    int ax, ay;
    screen_to_abs(x, y, &ax, &ay);
    emit(g_uinput_fd, EV_ABS, ABS_X, ax);
    emit(g_uinput_fd, EV_ABS, ABS_Y, ay);
    syn(g_uinput_fd);
}

static int linux_btn(int click_type) {
    switch (click_type) {
        case SWD_CLICK_RIGHT:  return BTN_RIGHT;
        case SWD_CLICK_MIDDLE: return BTN_MIDDLE;
        default:               return BTN_LEFT;
    }
}

/* --- Mouse --- */

int sw_desktop_mouse_move(int x, int y) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);
    move_abs(x, y);
    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

int sw_desktop_click(int x, int y, int click_type) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    int btn = linux_btn(click_type);
    move_abs(x, y);

    emit(g_uinput_fd, EV_KEY, btn, 1);
    syn(g_uinput_fd);
    emit(g_uinput_fd, EV_KEY, btn, 0);
    syn(g_uinput_fd);

    if (click_type == SWD_CLICK_DOUBLE) {
        usleep(50000);
        emit(g_uinput_fd, EV_KEY, btn, 1);
        syn(g_uinput_fd);
        emit(g_uinput_fd, EV_KEY, btn, 0);
        syn(g_uinput_fd);
    }

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

int sw_desktop_mouse_down(int x, int y, int button) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);
    move_abs(x, y);
    emit(g_uinput_fd, EV_KEY, linux_btn(button), 1);
    syn(g_uinput_fd);
    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

int sw_desktop_mouse_up(int x, int y, int button) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);
    move_abs(x, y);
    emit(g_uinput_fd, EV_KEY, linux_btn(button), 0);
    syn(g_uinput_fd);
    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

int sw_desktop_drag(int x1, int y1, int x2, int y2, int button) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    int btn = linux_btn(button);
    move_abs(x1, y1);

    /* Button down */
    emit(g_uinput_fd, EV_KEY, btn, 1);
    syn(g_uinput_fd);

    /* Move to destination in steps for smooth drag */
    int steps = 10;
    for (int i = 1; i <= steps; i++) {
        int cx = x1 + (x2 - x1) * i / steps;
        int cy = y1 + (y2 - y1) * i / steps;
        move_abs(cx, cy);
        usleep(10000);
    }

    /* Button up */
    emit(g_uinput_fd, EV_KEY, btn, 0);
    syn(g_uinput_fd);

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

int sw_desktop_get_mouse_pos(int *x, int *y) {
    /* Linux doesn't have a direct uinput way to query cursor position.
     * Use /dev/input/mice or xinput — for now use xdotool as fallback */
    FILE *fp = popen("xdotool getmouselocation --shell 2>/dev/null", "r");
    if (!fp) return SWD_ERR_PLATFORM;

    *x = 0; *y = 0;
    char buf[64];
    while (fgets(buf, sizeof(buf), fp)) {
        int val;
        if (sscanf(buf, "X=%d", &val) == 1) *x = val;
        else if (sscanf(buf, "Y=%d", &val) == 1) *y = val;
    }
    int ret = pclose(fp);
    return (ret == 0) ? SWD_OK : SWD_ERR_PLATFORM;
}

/* --- Keyboard --- */

/* Check if text is pure ASCII */
static int is_ascii(const char *text, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if ((unsigned char)text[i] > 127) return 0;
    }
    return 1;
}

/* Type ASCII via uinput (fast, no subprocess) */
static int type_ascii_uinput(const char *text, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        int key = -1;
        int shift = 0;

        if (c >= 'a' && c <= 'z') {
            key = KEY_A + (c - 'a');
        } else if (c >= 'A' && c <= 'Z') {
            key = KEY_A + (c - 'A');
            shift = 1;
        } else if (c >= '1' && c <= '9') {
            key = KEY_1 + (c - '1');
        } else if (c == '0') {
            key = KEY_0;
        } else {
            switch (c) {
                case ' ':  key = KEY_SPACE; break;
                case '\n': key = KEY_ENTER; break;
                case '\t': key = KEY_TAB; break;
                case '.':  key = KEY_DOT; break;
                case ',':  key = KEY_COMMA; break;
                case '-':  key = KEY_MINUS; break;
                case '=':  key = KEY_EQUAL; break;
                case '/':  key = KEY_SLASH; break;
                case '\\': key = KEY_BACKSLASH; break;
                case ';':  key = KEY_SEMICOLON; break;
                case '\'': key = KEY_APOSTROPHE; break;
                case '[':  key = KEY_LEFTBRACE; break;
                case ']':  key = KEY_RIGHTBRACE; break;
                case '`':  key = KEY_GRAVE; break;
                case '!':  key = KEY_1; shift = 1; break;
                case '@':  key = KEY_2; shift = 1; break;
                case '#':  key = KEY_3; shift = 1; break;
                case '$':  key = KEY_4; shift = 1; break;
                case '%':  key = KEY_5; shift = 1; break;
                case '^':  key = KEY_6; shift = 1; break;
                case '&':  key = KEY_7; shift = 1; break;
                case '*':  key = KEY_8; shift = 1; break;
                case '(':  key = KEY_9; shift = 1; break;
                case ')':  key = KEY_0; shift = 1; break;
                case '_':  key = KEY_MINUS; shift = 1; break;
                case '+':  key = KEY_EQUAL; shift = 1; break;
                case '{':  key = KEY_LEFTBRACE; shift = 1; break;
                case '}':  key = KEY_RIGHTBRACE; shift = 1; break;
                case ':':  key = KEY_SEMICOLON; shift = 1; break;
                case '"':  key = KEY_APOSTROPHE; shift = 1; break;
                case '<':  key = KEY_COMMA; shift = 1; break;
                case '>':  key = KEY_DOT; shift = 1; break;
                case '?':  key = KEY_SLASH; shift = 1; break;
                case '|':  key = KEY_BACKSLASH; shift = 1; break;
                case '~':  key = KEY_GRAVE; shift = 1; break;
                default: continue;
            }
        }

        if (key >= 0) {
            if (shift) {
                emit(g_uinput_fd, EV_KEY, KEY_LEFTSHIFT, 1);
                syn(g_uinput_fd);
            }
            emit(g_uinput_fd, EV_KEY, key, 1);
            syn(g_uinput_fd);
            emit(g_uinput_fd, EV_KEY, key, 0);
            syn(g_uinput_fd);
            if (shift) {
                emit(g_uinput_fd, EV_KEY, KEY_LEFTSHIFT, 0);
                syn(g_uinput_fd);
            }
        }
    }
    return SWD_OK;
}

/* Type Unicode via ydotool (Wayland-safe) or xdotool (X11 fallback) */
static int type_unicode_subprocess(const char *text, uint32_t len) {
    /* Shell-safe: escape single quotes */
    size_t cmd_size = len * 4 + 128;
    char *cmd = (char *)malloc(cmd_size);
    if (!cmd) return SWD_ERR_PLATFORM;

    /* Try ydotool first (works on Wayland), fall back to xdotool */
    const char *tool = "ydotool type --";
    if (system("command -v ydotool >/dev/null 2>&1") != 0) {
        tool = "xdotool type --clearmodifiers --";
    }

    size_t pos = 0;
    pos += snprintf(cmd + pos, cmd_size - pos, "%s '", tool);

    for (uint32_t i = 0; i < len; i++) {
        if (text[i] == '\'') {
            if (pos + 4 < cmd_size) {
                cmd[pos++] = '\'';
                cmd[pos++] = '\\';
                cmd[pos++] = '\'';
                cmd[pos++] = '\'';
            }
        } else {
            if (pos + 1 < cmd_size)
                cmd[pos++] = text[i];
        }
    }

    if (pos + 2 < cmd_size) {
        cmd[pos++] = '\'';
        cmd[pos] = '\0';
    }

    int ret = system(cmd);
    free(cmd);
    return (ret == 0) ? SWD_OK : SWD_ERR_PLATFORM;
}

int sw_desktop_type_text(const char *text, uint32_t len) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);
    int rc;
    if (is_ascii(text, len)) {
        rc = type_ascii_uinput(text, len);
    } else {
        rc = type_unicode_subprocess(text, len);
    }
    pthread_mutex_unlock(&g_op_mutex);
    return rc;
}

static int linux_keycode(const char *name) {
    if (strcmp(name, "return") == 0 || strcmp(name, "enter") == 0)       return KEY_ENTER;
    if (strcmp(name, "escape") == 0 || strcmp(name, "esc") == 0)         return KEY_ESC;
    if (strcmp(name, "tab") == 0)            return KEY_TAB;
    if (strcmp(name, "space") == 0)          return KEY_SPACE;
    if (strcmp(name, "delete") == 0 || strcmp(name, "backspace") == 0)   return KEY_BACKSPACE;
    if (strcmp(name, "forward-delete") == 0) return KEY_DELETE;
    if (strcmp(name, "up") == 0 || strcmp(name, "arrow-up") == 0)        return KEY_UP;
    if (strcmp(name, "down") == 0 || strcmp(name, "arrow-down") == 0)    return KEY_DOWN;
    if (strcmp(name, "left") == 0 || strcmp(name, "arrow-left") == 0)    return KEY_LEFT;
    if (strcmp(name, "right") == 0 || strcmp(name, "arrow-right") == 0)  return KEY_RIGHT;
    if (strcmp(name, "home") == 0)           return KEY_HOME;
    if (strcmp(name, "end") == 0)            return KEY_END;
    if (strcmp(name, "page-up") == 0 || strcmp(name, "pageup") == 0)     return KEY_PAGEUP;
    if (strcmp(name, "page-down") == 0 || strcmp(name, "pagedown") == 0) return KEY_PAGEDOWN;
    /* Modifier keys */
    if (strcmp(name, "shift") == 0)          return KEY_LEFTSHIFT;
    if (strcmp(name, "control") == 0 || strcmp(name, "ctrl") == 0)       return KEY_LEFTCTRL;
    if (strcmp(name, "alt") == 0 || strcmp(name, "option") == 0)         return KEY_LEFTALT;
    if (strcmp(name, "meta") == 0 || strcmp(name, "super") == 0 || strcmp(name, "command") == 0 || strcmp(name, "cmd") == 0) return KEY_LEFTMETA;
    /* Letter keys */
    if (strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'z') {
        return KEY_A + (name[0] - 'a');
    }
    /* F-keys */
    if (strcmp(name, "f1") == 0)  return KEY_F1;
    if (strcmp(name, "f2") == 0)  return KEY_F2;
    if (strcmp(name, "f3") == 0)  return KEY_F3;
    if (strcmp(name, "f4") == 0)  return KEY_F4;
    if (strcmp(name, "f5") == 0)  return KEY_F5;
    if (strcmp(name, "f6") == 0)  return KEY_F6;
    if (strcmp(name, "f7") == 0)  return KEY_F7;
    if (strcmp(name, "f8") == 0)  return KEY_F8;
    if (strcmp(name, "f9") == 0)  return KEY_F9;
    if (strcmp(name, "f10") == 0) return KEY_F10;
    if (strcmp(name, "f11") == 0) return KEY_F11;
    if (strcmp(name, "f12") == 0) return KEY_F12;
    return -1;
}

int sw_desktop_press_key(const char *key_name) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    int kc = linux_keycode(key_name);
    if (kc < 0) { pthread_mutex_unlock(&g_op_mutex); return SWD_ERR_PLATFORM; }

    emit(g_uinput_fd, EV_KEY, kc, 1);
    syn(g_uinput_fd);
    emit(g_uinput_fd, EV_KEY, kc, 0);
    syn(g_uinput_fd);

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

int sw_desktop_key_combo(uint32_t modifiers, const char *key_name) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);

    int kc = linux_keycode(key_name);
    if (kc < 0) { pthread_mutex_unlock(&g_op_mutex); return SWD_ERR_PLATFORM; }

    /* Press modifiers */
    if (modifiers & SWD_MOD_SHIFT) { emit(g_uinput_fd, EV_KEY, KEY_LEFTSHIFT, 1); syn(g_uinput_fd); }
    if (modifiers & SWD_MOD_CTRL)  { emit(g_uinput_fd, EV_KEY, KEY_LEFTCTRL, 1);  syn(g_uinput_fd); }
    if (modifiers & SWD_MOD_ALT)   { emit(g_uinput_fd, EV_KEY, KEY_LEFTALT, 1);   syn(g_uinput_fd); }
    if (modifiers & SWD_MOD_META)  { emit(g_uinput_fd, EV_KEY, KEY_LEFTMETA, 1);  syn(g_uinput_fd); }

    /* Press and release key */
    emit(g_uinput_fd, EV_KEY, kc, 1);
    syn(g_uinput_fd);
    emit(g_uinput_fd, EV_KEY, kc, 0);
    syn(g_uinput_fd);

    /* Release modifiers (reverse order) */
    if (modifiers & SWD_MOD_META)  { emit(g_uinput_fd, EV_KEY, KEY_LEFTMETA, 0);  syn(g_uinput_fd); }
    if (modifiers & SWD_MOD_ALT)   { emit(g_uinput_fd, EV_KEY, KEY_LEFTALT, 0);   syn(g_uinput_fd); }
    if (modifiers & SWD_MOD_CTRL)  { emit(g_uinput_fd, EV_KEY, KEY_LEFTCTRL, 0);  syn(g_uinput_fd); }
    if (modifiers & SWD_MOD_SHIFT) { emit(g_uinput_fd, EV_KEY, KEY_LEFTSHIFT, 0); syn(g_uinput_fd); }

    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

/* --- Scroll --- */

int sw_desktop_scroll(int x, int y, int amount) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);
    move_abs(x, y);
    emit(g_uinput_fd, EV_REL, REL_WHEEL, -amount);
    syn(g_uinput_fd);
    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

int sw_desktop_scroll_h(int x, int y, int amount) {
    ENSURE_INIT();
    pthread_mutex_lock(&g_op_mutex);
    move_abs(x, y);
    emit(g_uinput_fd, EV_REL, REL_HWHEEL, amount);
    syn(g_uinput_fd);
    pthread_mutex_unlock(&g_op_mutex);
    return SWD_OK;
}

/* --- Screen info --- */

int sw_desktop_get_screen_size(int *width, int *height) {
    detect_resolution();
    *width  = g_screen_w;
    *height = g_screen_h;
    return SWD_OK;
}

/* --- Window management (X11) --- */

#include <dlfcn.h>

/* X11 types (avoid requiring Xlib headers at compile time — dlopen instead) */
typedef unsigned long XWindow;
typedef unsigned long XAtom;
typedef void *XDisplay;

static void *g_x11_lib = NULL;
static XDisplay g_x11_display = NULL;

/* Lazy X11 loader */
static int ensure_x11(void) {
    if (g_x11_display) return SWD_OK;
    g_x11_lib = dlopen("libX11.so.6", RTLD_LAZY);
    if (!g_x11_lib) g_x11_lib = dlopen("libX11.so", RTLD_LAZY);
    if (!g_x11_lib) return SWD_ERR_PLATFORM;

    XDisplay (*XOpenDisplay)(const char *) = dlsym(g_x11_lib, "XOpenDisplay");
    if (!XOpenDisplay) return SWD_ERR_PLATFORM;

    g_x11_display = XOpenDisplay(NULL);
    return g_x11_display ? SWD_OK : SWD_ERR_PLATFORM;
}

/* X11 function typedefs */
typedef XAtom (*fn_XInternAtom)(XDisplay, const char *, int);
typedef int (*fn_XGetWindowProperty)(XDisplay, XWindow, XAtom, long, long, int, XAtom, XAtom *, int *, unsigned long *, unsigned long *, unsigned char **);
typedef int (*fn_XFree)(void *);
typedef XWindow (*fn_XDefaultRootWindow)(XDisplay);
typedef int (*fn_XMoveResizeWindow)(XDisplay, XWindow, int, int, unsigned int, unsigned int);
typedef int (*fn_XFlush)(XDisplay);
typedef int (*fn_XGetWindowAttributes)(XDisplay, XWindow, void *);
typedef int (*fn_XFetchName)(XDisplay, XWindow, char **);

/* Helper: get X11 property */
static unsigned char *x11_get_property(XWindow win, const char *prop_name,
                                       unsigned long *nitems_out) {
    fn_XInternAtom pXInternAtom = dlsym(g_x11_lib, "XInternAtom");
    fn_XGetWindowProperty pXGetWindowProperty = dlsym(g_x11_lib, "XGetWindowProperty");
    fn_XDefaultRootWindow pXDefaultRootWindow = dlsym(g_x11_lib, "XDefaultRootWindow");
    if (!pXInternAtom || !pXGetWindowProperty) return NULL;

    XAtom prop = pXInternAtom(g_x11_display, prop_name, 0);
    XAtom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    (void)pXDefaultRootWindow;
    int rc = pXGetWindowProperty(g_x11_display, win, prop,
                                  0, 1024, 0, 0 /* AnyPropertyType */,
                                  &actual_type, &actual_format,
                                  &nitems, &bytes_after, &data);
    if (rc != 0 || !data) return NULL;
    if (nitems_out) *nitems_out = nitems;
    return data;
}

int sw_desktop_get_focused_window(sw_desktop_window_t *out) {
    if (ensure_x11() != SWD_OK) return SWD_ERR_PLATFORM;
    memset(out, 0, sizeof(*out));

    fn_XDefaultRootWindow pXDefaultRootWindow = dlsym(g_x11_lib, "XDefaultRootWindow");
    fn_XFree pXFree = dlsym(g_x11_lib, "XFree");
    fn_XFetchName pXFetchName = dlsym(g_x11_lib, "XFetchName");
    if (!pXDefaultRootWindow || !pXFree) return SWD_ERR_PLATFORM;

    XWindow root = pXDefaultRootWindow(g_x11_display);

    /* _NET_ACTIVE_WINDOW on root */
    unsigned long nitems = 0;
    unsigned char *data = x11_get_property(root, "_NET_ACTIVE_WINDOW", &nitems);
    if (!data || nitems < 1) { if (data) pXFree(data); return SWD_ERR_NOT_FOUND; }

    XWindow active = ((XWindow *)data)[0];
    pXFree(data);
    if (!active) return SWD_ERR_NOT_FOUND;

    out->window_id = (uint64_t)active;
    out->focused = 1;

    /* Get title via _NET_WM_NAME or XFetchName */
    data = x11_get_property(active, "_NET_WM_NAME", &nitems);
    if (data && nitems > 0) {
        strncpy(out->title, (char *)data, sizeof(out->title) - 1);
        pXFree(data);
    } else {
        if (data) pXFree(data);
        char *name = NULL;
        if (pXFetchName && pXFetchName(g_x11_display, active, &name) && name) {
            strncpy(out->title, name, sizeof(out->title) - 1);
            pXFree(name);
        }
    }

    /* Get PID via _NET_WM_PID */
    data = x11_get_property(active, "_NET_WM_PID", &nitems);
    if (data && nitems > 0) {
        out->pid = (int32_t)(((unsigned long *)data)[0]);
        pXFree(data);
    } else {
        if (data) pXFree(data);
    }

    /* Get geometry */
    /* XGetGeometry: Display, Window, *root_return, *x, *y, *w, *h, *border_w, *depth */
    typedef int (*fn_XGetGeometry)(XDisplay, XWindow, XWindow *, int *, int *, unsigned int *, unsigned int *, unsigned int *, unsigned int *);
    fn_XGetGeometry pXGetGeometry = dlsym(g_x11_lib, "XGetGeometry");
    typedef int (*fn_XTranslateCoordinates)(XDisplay, XWindow, XWindow, int, int, int *, int *, XWindow *);
    fn_XTranslateCoordinates pXTranslateCoords = dlsym(g_x11_lib, "XTranslateCoordinates");

    if (pXGetGeometry) {
        XWindow root_ret;
        int gx, gy;
        unsigned int gw, gh, border_w, depth;
        pXGetGeometry(g_x11_display, active, &root_ret, &gx, &gy, &gw, &gh, &border_w, &depth);
        out->width = (int)gw;
        out->height = (int)gh;

        /* Translate to root coordinates */
        if (pXTranslateCoords) {
            int rx, ry;
            XWindow child;
            pXTranslateCoords(g_x11_display, active, root, 0, 0, &rx, &ry, &child);
            out->x = rx;
            out->y = ry;
        } else {
            out->x = gx;
            out->y = gy;
        }
    }

    return SWD_OK;
}

int sw_desktop_set_window_frame(uint64_t window_id, int x, int y, int w, int h) {
    if (ensure_x11() != SWD_OK) return SWD_ERR_PLATFORM;

    XWindow win = (XWindow)window_id;
    fn_XMoveResizeWindow pXMoveResizeWindow = dlsym(g_x11_lib, "XMoveResizeWindow");
    fn_XFlush pXFlush = dlsym(g_x11_lib, "XFlush");
    fn_XInternAtom pXInternAtom = dlsym(g_x11_lib, "XInternAtom");
    if (!pXMoveResizeWindow || !pXFlush) return SWD_ERR_PLATFORM;

    /* Remove maximized state first */
    if (pXInternAtom) {
        typedef int (*fn_XSendEvent)(XDisplay, XWindow, int, long, void *);
        fn_XSendEvent pXSendEvent = dlsym(g_x11_lib, "XSendEvent");
        fn_XDefaultRootWindow pXDefaultRootWindow = dlsym(g_x11_lib, "XDefaultRootWindow");

        if (pXSendEvent && pXDefaultRootWindow) {
            XWindow root = pXDefaultRootWindow(g_x11_display);
            XAtom wm_state = pXInternAtom(g_x11_display, "_NET_WM_STATE", 0);
            XAtom max_h = pXInternAtom(g_x11_display, "_NET_WM_STATE_MAXIMIZED_HORZ", 0);
            XAtom max_v = pXInternAtom(g_x11_display, "_NET_WM_STATE_MAXIMIZED_VERT", 0);

            /* XClientMessageEvent — remove maximized */
            struct {
                int type;       /* 33 = ClientMessage */
                unsigned long serial;
                int send_event;
                XDisplay display;
                XWindow window;
                XAtom message_type;
                int format;
                union { long l[5]; } data;
            } ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = 33; /* ClientMessage */
            ev.window = win;
            ev.message_type = wm_state;
            ev.format = 32;
            ev.data.l[0] = 0; /* _NET_WM_STATE_REMOVE */
            ev.data.l[1] = (long)max_h;
            ev.data.l[2] = (long)max_v;

            pXSendEvent(g_x11_display, root, 0,
                        (1L << 19) | (1L << 20) /* SubstructureRedirectMask | SubstructureNotifyMask */,
                        &ev);
        }
    }

    pXMoveResizeWindow(g_x11_display, win, x, y, (unsigned int)w, (unsigned int)h);
    pXFlush(g_x11_display);
    return SWD_OK;
}

int sw_desktop_get_usable_rect(int *x, int *y, int *w, int *h) {
    if (ensure_x11() != SWD_OK) return SWD_ERR_PLATFORM;

    fn_XDefaultRootWindow pXDefaultRootWindow = dlsym(g_x11_lib, "XDefaultRootWindow");
    fn_XFree pXFree = dlsym(g_x11_lib, "XFree");
    if (!pXDefaultRootWindow || !pXFree) return SWD_ERR_PLATFORM;

    XWindow root = pXDefaultRootWindow(g_x11_display);

    unsigned long nitems = 0;
    unsigned char *data = x11_get_property(root, "_NET_WORKAREA", &nitems);
    if (data && nitems >= 4) {
        long *vals = (long *)data;
        *x = (int)vals[0];
        *y = (int)vals[1];
        *w = (int)vals[2];
        *h = (int)vals[3];
        pXFree(data);
        return SWD_OK;
    }
    if (data) pXFree(data);

    /* Fallback: full screen */
    detect_resolution();
    *x = 0;
    *y = 0;
    *w = g_screen_w;
    *h = g_screen_h;
    return SWD_OK;
}

int sw_desktop_find_window_by_app(const char *app_name, sw_desktop_window_t *out) {
    if (ensure_x11() != SWD_OK) return SWD_ERR_PLATFORM;
    memset(out, 0, sizeof(*out));

    fn_XDefaultRootWindow pXDefaultRootWindow = dlsym(g_x11_lib, "XDefaultRootWindow");
    fn_XFree pXFree = dlsym(g_x11_lib, "XFree");
    fn_XFetchName pXFetchName = dlsym(g_x11_lib, "XFetchName");
    if (!pXDefaultRootWindow || !pXFree) return SWD_ERR_PLATFORM;

    XWindow root = pXDefaultRootWindow(g_x11_display);

    /* Get _NET_CLIENT_LIST */
    unsigned long nitems = 0;
    unsigned char *data = x11_get_property(root, "_NET_CLIENT_LIST", &nitems);
    if (!data || nitems == 0) { if (data) pXFree(data); return SWD_ERR_NOT_FOUND; }

    XWindow *clients = (XWindow *)data;
    int found = 0;

    for (unsigned long i = 0; i < nitems && !found; i++) {
        XWindow win = clients[i];

        /* Try _NET_WM_NAME first, then WM_NAME */
        unsigned long name_nitems = 0;
        unsigned char *name_data = x11_get_property(win, "_NET_WM_NAME", &name_nitems);
        char *name = NULL;
        int free_name = 0;

        if (name_data && name_nitems > 0) {
            name = (char *)name_data;
        } else {
            if (name_data) pXFree(name_data);
            name_data = NULL;
            if (pXFetchName) {
                char *fetched = NULL;
                pXFetchName(g_x11_display, win, &fetched);
                if (fetched) {
                    name = fetched;
                    free_name = 1;
                }
            }
        }

        if (name && strcasestr(name, app_name)) {
            out->window_id = (uint64_t)win;
            strncpy(out->title, name, sizeof(out->title) - 1);

            /* Get PID */
            unsigned long pid_nitems = 0;
            unsigned char *pid_data = x11_get_property(win, "_NET_WM_PID", &pid_nitems);
            if (pid_data && pid_nitems > 0) {
                out->pid = (int32_t)(((unsigned long *)pid_data)[0]);
                pXFree(pid_data);
            } else {
                if (pid_data) pXFree(pid_data);
            }

            /* Get geometry */
            typedef int (*fn_XGetGeometry)(XDisplay, XWindow, XWindow *, int *, int *, unsigned int *, unsigned int *, unsigned int *, unsigned int *);
            fn_XGetGeometry pXGetGeometry = dlsym(g_x11_lib, "XGetGeometry");
            if (pXGetGeometry) {
                XWindow root_ret;
                int gx, gy;
                unsigned int gw, gh, bw, depth;
                pXGetGeometry(g_x11_display, win, &root_ret, &gx, &gy, &gw, &gh, &bw, &depth);
                out->x = gx;
                out->y = gy;
                out->width = (int)gw;
                out->height = (int)gh;
            }

            found = 1;
        }

        if (free_name && name) pXFree(name);
        else if (name_data) pXFree(name_data);
    }

    pXFree(data);
    return found ? SWD_OK : SWD_ERR_NOT_FOUND;
}

int sw_desktop_tile_app(const char *app_name, int position) {
    sw_desktop_window_t win;
    int rc = sw_desktop_find_window_by_app(app_name, &win);
    if (rc != SWD_OK) return rc;
    return sw_desktop_tile_window(win.window_id, position);
}

/* ============================================================
 * Windows — SendInput
 * ============================================================ */
#elif defined(_WIN32)

#include <windows.h>

static CRITICAL_SECTION g_init_cs;
static CRITICAL_SECTION g_op_cs;
static int g_cs_initialized = 0;
static int g_initialized = 0;

static void ensure_cs(void) {
    if (!g_cs_initialized) {
        InitializeCriticalSection(&g_init_cs);
        InitializeCriticalSection(&g_op_cs);
        g_cs_initialized = 1;
    }
}

int sw_desktop_check_permission(void) {
    /* Windows SendInput doesn't require special permissions unless UIPI blocks it */
    return SWD_OK;
}

int sw_desktop_init(void) {
    ensure_cs();
    EnterCriticalSection(&g_init_cs);
    if (!g_initialized) {
        /* Enable per-monitor DPI awareness for correct coordinates */
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        g_initialized = 1;
    }
    LeaveCriticalSection(&g_init_cs);
    return SWD_OK;
}

void sw_desktop_shutdown(void) {
    ensure_cs();
    EnterCriticalSection(&g_init_cs);
    g_initialized = 0;
    LeaveCriticalSection(&g_init_cs);
}

#define ENSURE_INIT() do { if (!g_initialized && sw_desktop_init() != SWD_OK) return SWD_ERR_INIT; } while(0)

/* Virtual screen dimensions for multi-monitor */
static void get_virtual_screen(int *vx, int *vy, int *vw, int *vh) {
    *vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    *vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    *vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    *vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

static void screen_to_abs(int x, int y, LONG *dx, LONG *dy) {
    int vx, vy, vw, vh;
    get_virtual_screen(&vx, &vy, &vw, &vh);
    *dx = ((x - vx) * 65535) / vw;
    *dy = ((y - vy) * 65535) / vh;
}

/* --- Mouse --- */

int sw_desktop_mouse_move(int x, int y) {
    ENSURE_INIT();
    ensure_cs();
    EnterCriticalSection(&g_op_cs);

    INPUT inp = {0};
    inp.type = INPUT_MOUSE;
    screen_to_abs(x, y, &inp.mi.dx, &inp.mi.dy);
    inp.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESKTOP | MOUSEEVENTF_MOVE;
    SendInput(1, &inp, sizeof(INPUT));

    LeaveCriticalSection(&g_op_cs);
    return SWD_OK;
}

static DWORD win_btn_down(int button) {
    switch (button) {
        case SWD_CLICK_RIGHT:  return MOUSEEVENTF_RIGHTDOWN;
        case SWD_CLICK_MIDDLE: return MOUSEEVENTF_MIDDLEDOWN;
        default:               return MOUSEEVENTF_LEFTDOWN;
    }
}

static DWORD win_btn_up(int button) {
    switch (button) {
        case SWD_CLICK_RIGHT:  return MOUSEEVENTF_RIGHTUP;
        case SWD_CLICK_MIDDLE: return MOUSEEVENTF_MIDDLEUP;
        default:               return MOUSEEVENTF_LEFTUP;
    }
}

int sw_desktop_click(int x, int y, int click_type) {
    ENSURE_INIT();
    ensure_cs();
    EnterCriticalSection(&g_op_cs);

    LONG dx, dy;
    screen_to_abs(x, y, &dx, &dy);

    DWORD down_flag = win_btn_down(click_type);
    DWORD up_flag   = win_btn_up(click_type);
    DWORD abs_flags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESKTOP;

    INPUT inputs[5];
    int count = 0;

    /* Move */
    memset(&inputs[count], 0, sizeof(INPUT));
    inputs[count].type = INPUT_MOUSE;
    inputs[count].mi.dx = dx;
    inputs[count].mi.dy = dy;
    inputs[count].mi.dwFlags = abs_flags | MOUSEEVENTF_MOVE;
    count++;

    /* Down */
    memset(&inputs[count], 0, sizeof(INPUT));
    inputs[count].type = INPUT_MOUSE;
    inputs[count].mi.dx = dx;
    inputs[count].mi.dy = dy;
    inputs[count].mi.dwFlags = abs_flags | down_flag;
    count++;

    /* Up */
    memset(&inputs[count], 0, sizeof(INPUT));
    inputs[count].type = INPUT_MOUSE;
    inputs[count].mi.dx = dx;
    inputs[count].mi.dy = dy;
    inputs[count].mi.dwFlags = abs_flags | up_flag;
    count++;

    SendInput(count, inputs, sizeof(INPUT));

    if (click_type == SWD_CLICK_DOUBLE) {
        INPUT clicks[2];
        memset(&clicks[0], 0, sizeof(INPUT));
        clicks[0].type = INPUT_MOUSE;
        clicks[0].mi.dx = dx;
        clicks[0].mi.dy = dy;
        clicks[0].mi.dwFlags = abs_flags | down_flag;

        memset(&clicks[1], 0, sizeof(INPUT));
        clicks[1].type = INPUT_MOUSE;
        clicks[1].mi.dx = dx;
        clicks[1].mi.dy = dy;
        clicks[1].mi.dwFlags = abs_flags | up_flag;

        SendInput(2, clicks, sizeof(INPUT));
    }

    LeaveCriticalSection(&g_op_cs);
    return SWD_OK;
}

int sw_desktop_mouse_down(int x, int y, int button) {
    ENSURE_INIT();
    ensure_cs();
    EnterCriticalSection(&g_op_cs);

    LONG dx, dy;
    screen_to_abs(x, y, &dx, &dy);
    DWORD abs_flags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESKTOP;

    INPUT inputs[2];
    memset(inputs, 0, sizeof(inputs));

    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dx = dx;
    inputs[0].mi.dy = dy;
    inputs[0].mi.dwFlags = abs_flags | MOUSEEVENTF_MOVE;

    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dx = dx;
    inputs[1].mi.dy = dy;
    inputs[1].mi.dwFlags = abs_flags | win_btn_down(button);

    SendInput(2, inputs, sizeof(INPUT));

    LeaveCriticalSection(&g_op_cs);
    return SWD_OK;
}

int sw_desktop_mouse_up(int x, int y, int button) {
    ENSURE_INIT();
    ensure_cs();
    EnterCriticalSection(&g_op_cs);

    LONG dx, dy;
    screen_to_abs(x, y, &dx, &dy);

    INPUT inp = {0};
    inp.type = INPUT_MOUSE;
    inp.mi.dx = dx;
    inp.mi.dy = dy;
    inp.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESKTOP | win_btn_up(button);
    SendInput(1, &inp, sizeof(INPUT));

    LeaveCriticalSection(&g_op_cs);
    return SWD_OK;
}

int sw_desktop_drag(int x1, int y1, int x2, int y2, int button) {
    ENSURE_INIT();

    /* Down at start */
    sw_desktop_mouse_down(x1, y1, button);

    /* Move to destination in steps */
    int steps = 10;
    for (int i = 1; i <= steps; i++) {
        int cx = x1 + (x2 - x1) * i / steps;
        int cy = y1 + (y2 - y1) * i / steps;
        sw_desktop_mouse_move(cx, cy);
        Sleep(10);
    }

    /* Up at end */
    sw_desktop_mouse_up(x2, y2, button);
    return SWD_OK;
}

int sw_desktop_get_mouse_pos(int *x, int *y) {
    POINT pt;
    if (!GetCursorPos(&pt)) return SWD_ERR_PLATFORM;
    *x = pt.x;
    *y = pt.y;
    return SWD_OK;
}

/* --- Keyboard --- */

int sw_desktop_type_text(const char *text, uint32_t len) {
    ENSURE_INIT();
    ensure_cs();
    EnterCriticalSection(&g_op_cs);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, (int)len, NULL, 0);
    if (wlen <= 0) { LeaveCriticalSection(&g_op_cs); return SWD_ERR_PLATFORM; }

    WCHAR *wtext = (WCHAR *)malloc(wlen * sizeof(WCHAR));
    if (!wtext) { LeaveCriticalSection(&g_op_cs); return SWD_ERR_PLATFORM; }
    MultiByteToWideChar(CP_UTF8, 0, text, (int)len, wtext, wlen);

    INPUT *inputs = (INPUT *)calloc(wlen * 2, sizeof(INPUT));
    if (!inputs) { free(wtext); LeaveCriticalSection(&g_op_cs); return SWD_ERR_PLATFORM; }

    for (int i = 0; i < wlen; i++) {
        inputs[i*2].type = INPUT_KEYBOARD;
        inputs[i*2].ki.wScan = wtext[i];
        inputs[i*2].ki.dwFlags = KEYEVENTF_UNICODE;

        inputs[i*2+1].type = INPUT_KEYBOARD;
        inputs[i*2+1].ki.wScan = wtext[i];
        inputs[i*2+1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    }

    SendInput(wlen * 2, inputs, sizeof(INPUT));

    free(inputs);
    free(wtext);
    LeaveCriticalSection(&g_op_cs);
    return SWD_OK;
}

static WORD win_keycode(const char *name) {
    if (strcmp(name, "return") == 0 || strcmp(name, "enter") == 0)       return VK_RETURN;
    if (strcmp(name, "escape") == 0 || strcmp(name, "esc") == 0)         return VK_ESCAPE;
    if (strcmp(name, "tab") == 0)            return VK_TAB;
    if (strcmp(name, "space") == 0)          return VK_SPACE;
    if (strcmp(name, "delete") == 0 || strcmp(name, "backspace") == 0)   return VK_BACK;
    if (strcmp(name, "forward-delete") == 0) return VK_DELETE;
    if (strcmp(name, "up") == 0 || strcmp(name, "arrow-up") == 0)        return VK_UP;
    if (strcmp(name, "down") == 0 || strcmp(name, "arrow-down") == 0)    return VK_DOWN;
    if (strcmp(name, "left") == 0 || strcmp(name, "arrow-left") == 0)    return VK_LEFT;
    if (strcmp(name, "right") == 0 || strcmp(name, "arrow-right") == 0)  return VK_RIGHT;
    if (strcmp(name, "home") == 0)           return VK_HOME;
    if (strcmp(name, "end") == 0)            return VK_END;
    if (strcmp(name, "page-up") == 0 || strcmp(name, "pageup") == 0)     return VK_PRIOR;
    if (strcmp(name, "page-down") == 0 || strcmp(name, "pagedown") == 0) return VK_NEXT;
    /* Modifier keys */
    if (strcmp(name, "shift") == 0)          return VK_SHIFT;
    if (strcmp(name, "control") == 0 || strcmp(name, "ctrl") == 0)       return VK_CONTROL;
    if (strcmp(name, "alt") == 0 || strcmp(name, "option") == 0)         return VK_MENU;
    if (strcmp(name, "meta") == 0 || strcmp(name, "super") == 0 || strcmp(name, "command") == 0 || strcmp(name, "cmd") == 0) return VK_LWIN;
    /* Letter keys */
    if (strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'z') {
        return 'A' + (name[0] - 'a');  /* VK_A..VK_Z = 'A'..'Z' */
    }
    /* F-keys */
    if (strcmp(name, "f1") == 0)  return VK_F1;
    if (strcmp(name, "f2") == 0)  return VK_F2;
    if (strcmp(name, "f3") == 0)  return VK_F3;
    if (strcmp(name, "f4") == 0)  return VK_F4;
    if (strcmp(name, "f5") == 0)  return VK_F5;
    if (strcmp(name, "f6") == 0)  return VK_F6;
    if (strcmp(name, "f7") == 0)  return VK_F7;
    if (strcmp(name, "f8") == 0)  return VK_F8;
    if (strcmp(name, "f9") == 0)  return VK_F9;
    if (strcmp(name, "f10") == 0) return VK_F10;
    if (strcmp(name, "f11") == 0) return VK_F11;
    if (strcmp(name, "f12") == 0) return VK_F12;
    return 0;
}

int sw_desktop_press_key(const char *key_name) {
    ENSURE_INIT();
    ensure_cs();
    EnterCriticalSection(&g_op_cs);

    WORD vk = win_keycode(key_name);
    if (vk == 0) { LeaveCriticalSection(&g_op_cs); return SWD_ERR_PLATFORM; }

    INPUT inputs[2];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));

    LeaveCriticalSection(&g_op_cs);
    return SWD_OK;
}

int sw_desktop_key_combo(uint32_t modifiers, const char *key_name) {
    ENSURE_INIT();
    ensure_cs();
    EnterCriticalSection(&g_op_cs);

    WORD vk = win_keycode(key_name);
    if (vk == 0) { LeaveCriticalSection(&g_op_cs); return SWD_ERR_PLATFORM; }

    /* Build input array: modifier downs + key down + key up + modifier ups */
    INPUT inputs[10];
    int count = 0;
    memset(inputs, 0, sizeof(inputs));

    /* Modifier downs */
    if (modifiers & SWD_MOD_SHIFT) { inputs[count].type = INPUT_KEYBOARD; inputs[count].ki.wVk = VK_SHIFT;   count++; }
    if (modifiers & SWD_MOD_CTRL)  { inputs[count].type = INPUT_KEYBOARD; inputs[count].ki.wVk = VK_CONTROL; count++; }
    if (modifiers & SWD_MOD_ALT)   { inputs[count].type = INPUT_KEYBOARD; inputs[count].ki.wVk = VK_MENU;    count++; }
    if (modifiers & SWD_MOD_META)  { inputs[count].type = INPUT_KEYBOARD; inputs[count].ki.wVk = VK_LWIN;    count++; }

    /* Key down + up */
    inputs[count].type = INPUT_KEYBOARD; inputs[count].ki.wVk = vk; count++;
    inputs[count].type = INPUT_KEYBOARD; inputs[count].ki.wVk = vk; inputs[count].ki.dwFlags = KEYEVENTF_KEYUP; count++;

    /* Modifier ups (reverse) */
    if (modifiers & SWD_MOD_META)  { inputs[count].type = INPUT_KEYBOARD; inputs[count].ki.wVk = VK_LWIN;    inputs[count].ki.dwFlags = KEYEVENTF_KEYUP; count++; }
    if (modifiers & SWD_MOD_ALT)   { inputs[count].type = INPUT_KEYBOARD; inputs[count].ki.wVk = VK_MENU;    inputs[count].ki.dwFlags = KEYEVENTF_KEYUP; count++; }
    if (modifiers & SWD_MOD_CTRL)  { inputs[count].type = INPUT_KEYBOARD; inputs[count].ki.wVk = VK_CONTROL; inputs[count].ki.dwFlags = KEYEVENTF_KEYUP; count++; }
    if (modifiers & SWD_MOD_SHIFT) { inputs[count].type = INPUT_KEYBOARD; inputs[count].ki.wVk = VK_SHIFT;   inputs[count].ki.dwFlags = KEYEVENTF_KEYUP; count++; }

    SendInput(count, inputs, sizeof(INPUT));

    LeaveCriticalSection(&g_op_cs);
    return SWD_OK;
}

/* --- Scroll --- */

int sw_desktop_scroll(int x, int y, int amount) {
    ENSURE_INIT();
    sw_desktop_mouse_move(x, y);

    ensure_cs();
    EnterCriticalSection(&g_op_cs);

    INPUT inp = {0};
    inp.type = INPUT_MOUSE;
    inp.mi.dwFlags = MOUSEEVENTF_WHEEL;
    inp.mi.mouseData = (DWORD)(-amount * WHEEL_DELTA);
    SendInput(1, &inp, sizeof(INPUT));

    LeaveCriticalSection(&g_op_cs);
    return SWD_OK;
}

int sw_desktop_scroll_h(int x, int y, int amount) {
    ENSURE_INIT();
    sw_desktop_mouse_move(x, y);

    ensure_cs();
    EnterCriticalSection(&g_op_cs);

    INPUT inp = {0};
    inp.type = INPUT_MOUSE;
    inp.mi.dwFlags = MOUSEEVENTF_HWHEEL;
    inp.mi.mouseData = (DWORD)(amount * WHEEL_DELTA);
    SendInput(1, &inp, sizeof(INPUT));

    LeaveCriticalSection(&g_op_cs);
    return SWD_OK;
}

/* --- Screen info --- */

int sw_desktop_get_screen_size(int *width, int *height) {
    /* Return virtual screen (all monitors combined) */
    *width  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    *height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return SWD_OK;
}

/* --- Window management (Win32) --- */

int sw_desktop_get_focused_window(sw_desktop_window_t *out) {
    memset(out, 0, sizeof(*out));

    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return SWD_ERR_NOT_FOUND;

    out->window_id = (uint64_t)(uintptr_t)hwnd;
    out->focused = 1;

    /* Title */
    GetWindowTextA(hwnd, out->title, sizeof(out->title));

    /* PID */
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    out->pid = (int32_t)pid;

    /* App name from process */
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        char path[MAX_PATH];
        DWORD pathLen = MAX_PATH;
        if (QueryFullProcessImageNameA(proc, 0, path, &pathLen)) {
            /* Extract filename without extension */
            char *slash = strrchr(path, '\\');
            char *name = slash ? slash + 1 : path;
            char *dot = strrchr(name, '.');
            size_t len = dot ? (size_t)(dot - name) : strlen(name);
            if (len >= sizeof(out->app_name)) len = sizeof(out->app_name) - 1;
            memcpy(out->app_name, name, len);
            out->app_name[len] = '\0';
        }
        CloseHandle(proc);
    }

    /* Position/size */
    RECT rect;
    if (GetWindowRect(hwnd, &rect)) {
        out->x = rect.left;
        out->y = rect.top;
        out->width = rect.right - rect.left;
        out->height = rect.bottom - rect.top;
    }

    return SWD_OK;
}

int sw_desktop_set_window_frame(uint64_t window_id, int x, int y, int w, int h) {
    HWND hwnd = (HWND)(uintptr_t)window_id;
    if (!IsWindow(hwnd)) return SWD_ERR_NOT_FOUND;

    /* Un-maximize first */
    if (IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    return SWD_OK;
}

int sw_desktop_get_usable_rect(int *x, int *y, int *w, int *h) {
    HWND hwnd = GetForegroundWindow();
    HMONITOR mon;

    if (hwnd) {
        mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    } else {
        POINT pt = {0, 0};
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(mon, &mi)) {
        /* Fallback */
        *x = 0;
        *y = 0;
        *w = GetSystemMetrics(SM_CXSCREEN);
        *h = GetSystemMetrics(SM_CYSCREEN);
        return SWD_OK;
    }

    *x = mi.rcWork.left;
    *y = mi.rcWork.top;
    *w = mi.rcWork.right - mi.rcWork.left;
    *h = mi.rcWork.bottom - mi.rcWork.top;
    return SWD_OK;
}

/* Case-insensitive substring search (avoids shlwapi.h dependency) */
static int win32_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return 0;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (_strnicmp(haystack, needle, nlen) == 0) return 1;
    }
    return 0;
}

/* EnumWindows callback for find_window_by_app */
typedef struct {
    const char *target;
    sw_desktop_window_t *out;
    int found;
} find_win_ctx_t;

static BOOL CALLBACK find_win_cb(HWND hwnd, LPARAM lParam) {
    find_win_ctx_t *ctx = (find_win_ctx_t *)lParam;
    if (!IsWindowVisible(hwnd)) return TRUE;

    char title[256];
    GetWindowTextA(hwnd, title, sizeof(title));

    /* Match by window title */
    if (title[0] && win32_strcasestr(title, ctx->target)) {
        ctx->out->window_id = (uint64_t)(uintptr_t)hwnd;
        strncpy(ctx->out->title, title, sizeof(ctx->out->title) - 1);

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        ctx->out->pid = (int32_t)pid;

        RECT rect;
        if (GetWindowRect(hwnd, &rect)) {
            ctx->out->x = rect.left;
            ctx->out->y = rect.top;
            ctx->out->width = rect.right - rect.left;
            ctx->out->height = rect.bottom - rect.top;
        }

        ctx->found = 1;
        return FALSE; /* stop */
    }

    /* Match by process name */
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        char path[MAX_PATH];
        DWORD pathLen = MAX_PATH;
        if (QueryFullProcessImageNameA(proc, 0, path, &pathLen)) {
            char *slash = strrchr(path, '\\');
            char *name = slash ? slash + 1 : path;
            if (win32_strcasestr(name, ctx->target)) {
                ctx->out->window_id = (uint64_t)(uintptr_t)hwnd;
                strncpy(ctx->out->title, title, sizeof(ctx->out->title) - 1);
                strncpy(ctx->out->app_name, name, sizeof(ctx->out->app_name) - 1);
                ctx->out->pid = (int32_t)pid;

                RECT rect;
                if (GetWindowRect(hwnd, &rect)) {
                    ctx->out->x = rect.left;
                    ctx->out->y = rect.top;
                    ctx->out->width = rect.right - rect.left;
                    ctx->out->height = rect.bottom - rect.top;
                }

                ctx->found = 1;
                CloseHandle(proc);
                return FALSE;
            }
        }
        CloseHandle(proc);
    }

    return TRUE;
}

int sw_desktop_find_window_by_app(const char *app_name, sw_desktop_window_t *out) {
    memset(out, 0, sizeof(*out));
    find_win_ctx_t ctx = { .target = app_name, .out = out, .found = 0 };
    EnumWindows(find_win_cb, (LPARAM)&ctx);
    return ctx.found ? SWD_OK : SWD_ERR_NOT_FOUND;
}

int sw_desktop_tile_app(const char *app_name, int position) {
    sw_desktop_window_t win;
    int rc = sw_desktop_find_window_by_app(app_name, &win);
    if (rc != SWD_OK) return rc;
    return sw_desktop_tile_window(win.window_id, position);
}

/* ============================================================
 * Unsupported platform — stub
 * ============================================================ */
#else

int  sw_desktop_init(void)                                      { return SWD_ERR_PLATFORM; }
void sw_desktop_shutdown(void)                                  { }
int  sw_desktop_mouse_move(int x, int y)                        { (void)x; (void)y; return SWD_ERR_PLATFORM; }
int  sw_desktop_click(int x, int y, int t)                      { (void)x; (void)y; (void)t; return SWD_ERR_PLATFORM; }
int  sw_desktop_mouse_down(int x, int y, int b)                 { (void)x; (void)y; (void)b; return SWD_ERR_PLATFORM; }
int  sw_desktop_mouse_up(int x, int y, int b)                   { (void)x; (void)y; (void)b; return SWD_ERR_PLATFORM; }
int  sw_desktop_drag(int x1, int y1, int x2, int y2, int b)     { (void)x1; (void)y1; (void)x2; (void)y2; (void)b; return SWD_ERR_PLATFORM; }
int  sw_desktop_get_mouse_pos(int *x, int *y)                   { (void)x; (void)y; return SWD_ERR_PLATFORM; }
int  sw_desktop_type_text(const char *t, uint32_t l)             { (void)t; (void)l; return SWD_ERR_PLATFORM; }
int  sw_desktop_press_key(const char *k)                         { (void)k; return SWD_ERR_PLATFORM; }
int  sw_desktop_key_combo(uint32_t m, const char *k)             { (void)m; (void)k; return SWD_ERR_PLATFORM; }
int  sw_desktop_scroll(int x, int y, int a)                      { (void)x; (void)y; (void)a; return SWD_ERR_PLATFORM; }
int  sw_desktop_scroll_h(int x, int y, int a)                    { (void)x; (void)y; (void)a; return SWD_ERR_PLATFORM; }
int  sw_desktop_get_screen_size(int *w, int *h)                  { (void)w; (void)h; return SWD_ERR_PLATFORM; }
int  sw_desktop_check_permission(void)                           { return SWD_ERR_PLATFORM; }
int  sw_desktop_get_focused_window(sw_desktop_window_t *o)       { (void)o; return SWD_ERR_PLATFORM; }
int  sw_desktop_set_window_frame(uint64_t i, int x, int y, int w, int h) { (void)i; (void)x; (void)y; (void)w; (void)h; return SWD_ERR_PLATFORM; }
int  sw_desktop_get_usable_rect(int *x, int *y, int *w, int *h) { (void)x; (void)y; (void)w; (void)h; return SWD_ERR_PLATFORM; }
int  sw_desktop_find_window_by_app(const char *n, sw_desktop_window_t *o) { (void)n; (void)o; return SWD_ERR_PLATFORM; }
int  sw_desktop_tile_app(const char *n, int p) { (void)n; (void)p; return SWD_ERR_PLATFORM; }

#endif
