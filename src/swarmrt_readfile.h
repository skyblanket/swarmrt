/*
 * SwarmRT: read a whole file into memory.
 *
 * Works on non-seekable input (pipes, /dev/stdin, process substitution):
 * the buffer grows until EOF instead of sizing it with ftell, which is -1
 * on a pipe.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_READFILE_H
#define SWARMRT_READFILE_H

#include <stdio.h>
#include <stdlib.h>

/* Read the file at path into a malloc'd NUL-terminated string.
 * Returns NULL if the file cannot be opened, read, or allocated. The caller
 * frees the result. */
static inline char *sw_read_file_all(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }

    for (;;) {
        /* Keep one byte free for the terminating NUL. */
        if (cap - len < 2) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
            cap *= 2;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        if (n == 0) break;
        len += n;
    }

    int failed = ferror(f);
    fclose(f);
    if (failed) { free(buf); return NULL; }

    buf[len] = '\0';
    return buf;
}

#endif /* SWARMRT_READFILE_H */
