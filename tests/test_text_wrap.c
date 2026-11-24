/*
 * Tests for runtime/text_wrap.c (dialog log word wrap). Needs a TTF font:
 * $MHA2_FONT_PATH (set by shell.nix) or fonts/DejaVuSans.ttf; skipped if none.
 */
#include "text_wrap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail = 1; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    const char *path = getenv("MHA2_FONT_PATH");
    if (!path) path = "fonts/DejaVuSans.ttf";
    if (TTF_Init() != 0) { printf("SKIP test_text_wrap (TTF_Init failed)\n"); return 0; }
    TTF_Font *f = TTF_OpenFont(path, 28);
    if (!f) { printf("SKIP test_text_wrap (no font at %s)\n", path); return 0; }

    static char out[64][256];
    int owner[64];
    const char *text = "Vielleicht sollte ich eine schöne Tasse heiße Milch zu mir nehmen, "
                       "denn es ist jetzt genau ein Jahr her, dass ich den Pharao vertrieben habe.";
    int widths[] = { 120, 260, 400, 800, 4000 };
    for (int wi = 0; wi < 5; wi++) {
        int max_w = widths[wi];
        int n = wrap_text_lines(f, text, max_w, out, owner, 7, 64);
        CHECK(n >= 1, "w=%d produced no lines", max_w);
        /* every line fits unless it is a single over-wide word; words are never split
         * unless over-wide, and joining the lines with spaces restores the text */
        char joined[1024] = "";
        for (int i = 0; i < n; i++) {
            int tw = 0;
            TTF_SizeUTF8(f, out[i], &tw, NULL);
            CHECK(tw <= max_w || strchr(out[i], ' ') == NULL, "w=%d line %d too wide (%d): '%s'", max_w, i, tw, out[i]);
            CHECK(owner[i] == 7, "owner tag");
            CHECK(out[i][0] != ' ' && out[i][strlen(out[i]) - 1] != ' ', "w=%d line %d has edge spaces", max_w, i);
            if (i) strcat(joined, " ");
            strcat(joined, out[i]);
        }
        if (max_w >= 260) CHECK(strcmp(joined, text) == 0, "w=%d join mismatch: '%s'", max_w, joined);
        printf("[wrap] max_w=%4d -> %2d line(s)\n", max_w, n);
    }
    CHECK(wrap_text_lines(f, text, 4000, out, owner, 0, 64) == 1, "fits on one line when wide");
    CHECK(wrap_text_lines(f, text, 100, out, owner, 0, 3) == 3, "cap respected");
    CHECK(wrap_text_lines(f, "   ", 300, out, owner, 0, 64) == 0, "blank text -> no lines");
    TTF_CloseFont(f);
    TTF_Quit();
    printf(g_fail ? "FAIL test_text_wrap\n" : "PASS test_text_wrap\n");
    return g_fail;
}
