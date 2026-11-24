/*
 * Word wrapping for runtime-drawn overlay text (dialog log). SDL_ttf only, so
 * tests/ can link it directly.
 */
#include "text_wrap.h"
#include <stdio.h>
#include <string.h>

/* Greedy word wrap of one UTF-8 entry into lines no wider than max_w pixels.
 * Writes up to cap lines (each < 256 bytes) and tags them with entry index `who`. */
int wrap_text_lines(TTF_Font *f, const char *text, int max_w, char (*out)[256], int *owner,
                           int who, int cap) {
    int n = 0;
    const char *p = text;
    while (*p && n < cap) {
        while (*p == ' ') p++;
        if (!*p) break;
        char line[256] = "";
        size_t len = 0;
        const char *q = p;
        for (;;) {
            const char *w_end = q;
            while (*w_end && *w_end != ' ') w_end++;
            size_t wl = (size_t)(w_end - q);
            char cand[256];
            int cl = snprintf(cand, sizeof(cand), "%s%s%.*s", line, len ? " " : "", (int)wl, q);
            int tw = 0;
            if (cl >= (int)sizeof(cand) || (TTF_SizeUTF8(f, cand, &tw, NULL) == 0 && tw > max_w && len)) break;
            snprintf(line, sizeof(line), "%s", cand);
            len = strlen(line);
            q = w_end;
            while (*q == ' ') q++;
            if (!*q) break;
        }
        if (!len) { /* single word wider than the panel: hard-cut it */
            size_t wl = strcspn(p, " ");
            if (wl > 255) wl = 255;
            memcpy(line, p, wl);
            line[wl] = 0;
            q = p + wl;
        }
        snprintf(out[n], 256, "%s", line);
        owner[n] = who;
        n++;
        p = q;
    }
    return n;
}
