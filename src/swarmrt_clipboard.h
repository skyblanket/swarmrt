/*
 * swarmrt_clipboard.h — Cross-platform clipboard API (text-only)
 *
 * macOS:   NSPasteboard via ObjC runtime (pure C)
 * Linux:   wl-copy/wl-paste (Wayland) or xclip (X11)
 * Windows: OpenClipboard / SetClipboardData (Win32)
 */

#ifndef SWARMRT_CLIPBOARD_H
#define SWARMRT_CLIPBOARD_H

#include <stdint.h>

/* Return codes (shared with swarmrt_desktop.h) */
#ifndef SWD_OK
#define SWD_OK             0
#define SWD_ERR_INIT      -1
#define SWD_ERR_PLATFORM  -2
#define SWD_ERR_PERMISSION -3
#endif

/*
 * Get clipboard text content.
 * Returns SWD_OK on success. Caller must free(*out_text).
 * *out_len is set to the byte length (excluding null terminator).
 */
int sw_clipboard_get_text(char **out_text, uint32_t *out_len);

/*
 * Set clipboard text content.
 * text must be UTF-8, len is byte length.
 */
int sw_clipboard_set_text(const char *text, uint32_t len);

/*
 * Check if clipboard has text content.
 * Returns 1 if available, 0 if not.
 */
int sw_clipboard_has_text(void);

/*
 * Clear the clipboard.
 */
int sw_clipboard_clear(void);

#endif /* SWARMRT_CLIPBOARD_H */
