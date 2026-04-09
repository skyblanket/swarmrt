/*
 * swarmrt_desktop.h — Cross-platform desktop automation API
 *
 * macOS:   CoreGraphics (CGEvent*)
 * Linux:   /dev/uinput (kernel-level, X11 + Wayland)
 * Windows: SendInput (Win32)
 */

#ifndef SWARMRT_DESKTOP_H
#define SWARMRT_DESKTOP_H

#include <stdint.h>

/* Return codes */
#define SWD_OK             0
#define SWD_ERR_INIT      -1
#define SWD_ERR_PLATFORM  -2
#define SWD_ERR_PERMISSION -3

/* Click / button types */
#define SWD_CLICK_LEFT    0
#define SWD_CLICK_RIGHT   1
#define SWD_CLICK_DOUBLE  2
#define SWD_CLICK_MIDDLE  3

/* Modifier flags (bitwise OR) */
#define SWD_MOD_SHIFT     (1 << 0)
#define SWD_MOD_CTRL      (1 << 1)
#define SWD_MOD_ALT       (1 << 2)
#define SWD_MOD_META      (1 << 3)  /* Cmd on macOS, Win on Windows, Super on Linux */

/* Lifecycle (lazy, idempotent, thread-safe) */
int  sw_desktop_init(void);
void sw_desktop_shutdown(void);

/* Mouse */
int sw_desktop_mouse_move(int x, int y);
int sw_desktop_click(int x, int y, int click_type);
int sw_desktop_mouse_down(int x, int y, int button);
int sw_desktop_mouse_up(int x, int y, int button);
int sw_desktop_drag(int x1, int y1, int x2, int y2, int button);
int sw_desktop_get_mouse_pos(int *x, int *y);

/* Keyboard */
int sw_desktop_type_text(const char *text, uint32_t len);
int sw_desktop_press_key(const char *key_name);
int sw_desktop_key_combo(uint32_t modifiers, const char *key_name);

/* Scroll */
int sw_desktop_scroll(int x, int y, int amount);
int sw_desktop_scroll_h(int x, int y, int amount);

/* Screen info */
int sw_desktop_get_screen_size(int *width, int *height);

/* Permission check (macOS TCC, Linux uinput access) */
int sw_desktop_check_permission(void);

/* ---- Window management & tiling ---- */

/* Window info */
typedef struct {
    uint64_t window_id;
    int32_t  pid;
    char     title[256];
    char     app_name[128];
    int      x, y, width, height;
    int      focused;
} sw_desktop_window_t;

/* Tiling positions */
#define SWD_TILE_LEFT_HALF        0
#define SWD_TILE_RIGHT_HALF       1
#define SWD_TILE_TOP_HALF         2
#define SWD_TILE_BOTTOM_HALF      3
#define SWD_TILE_TOP_LEFT         4
#define SWD_TILE_TOP_RIGHT        5
#define SWD_TILE_BOTTOM_LEFT      6
#define SWD_TILE_BOTTOM_RIGHT     7
#define SWD_TILE_LEFT_THIRD       8
#define SWD_TILE_CENTER_THIRD     9
#define SWD_TILE_RIGHT_THIRD      10
#define SWD_TILE_LEFT_TWO_THIRDS  11
#define SWD_TILE_RIGHT_TWO_THIRDS 12
#define SWD_TILE_MAXIMIZE         13
#define SWD_TILE_CENTER           14

#define SWD_ERR_NOT_FOUND         -4

/* Window management */
int sw_desktop_get_focused_window(sw_desktop_window_t *out);
int sw_desktop_set_window_frame(uint64_t window_id, int x, int y, int w, int h);
int sw_desktop_get_usable_rect(int *x, int *y, int *w, int *h);
int sw_desktop_find_window_by_app(const char *app_name, sw_desktop_window_t *out);

/* Tiling (convenience — computes rect from position, calls set_window_frame) */
int sw_desktop_tile_focused(int position);
int sw_desktop_tile_window(uint64_t window_id, int position);
int sw_desktop_tile_app(const char *app_name, int position);

#endif /* SWARMRT_DESKTOP_H */
