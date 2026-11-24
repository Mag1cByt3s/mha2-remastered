/*
 * Tests for runtime/input_bindings.c (rebindable hotkeys / controller buttons).
 */
#include "input_bindings.h"
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail = 1; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void test_defaults(void) {
    bindings_reset_defaults();
    CHECK(bindings_match_key(SDLK_F1, 0, NULL) == ACT_SETTINGS_MENU, "F1 -> settings menu");
    CHECK(bindings_match_key(SDLK_F1, KMOD_LSHIFT, NULL) == ACT_SETTINGS_MENU, "Shift does not block F1 (fast walk held)");
    CHECK(bindings_match_key(SDLK_F4, KMOD_LALT, NULL) == -1, "Alt+F4 must not cycle vsync");
    CHECK(bindings_match_key(SDLK_l, KMOD_LCTRL, NULL) == ACT_DIALOG_LOG, "Ctrl+L -> dialog log");
    CHECK(bindings_match_key(SDLK_l, 0, NULL) == -1, "plain L (savegame names) is not a hotkey");
    CHECK(bindings_match_key(SDLK_l, KMOD_LALT | KMOD_LCTRL, NULL) == -1, "Ctrl+Alt+L is not Ctrl+L");
    int slot = -1;
    CHECK(bindings_match_key(SDLK_PRINTSCREEN, 0, &slot) == ACT_SCREENSHOT && slot == 1, "PrintScreen -> screenshot slot 2");
    CHECK(bindings_match_key(SDLK_s, KMOD_RCTRL, NULL) == ACT_QUICK_SAVE, "Ctrl+S -> quick save");
    CHECK(bindings_match_key(SDLK_SPACE, KMOD_LCTRL, NULL) == ACT_HOTSPOTS, "hold actions ignore modifiers");
    CHECK(bindings_match_key_release(SDLK_h, &slot) == ACT_HOTSPOTS && slot == 1, "H release -> hotspots slot 2");
    CHECK(bindings_match_key_release(SDLK_F1, NULL) == -1, "press actions have no release");
    CHECK(bindings_match_pad(SDL_CONTROLLER_BUTTON_DPAD_UP, NULL) == ACT_QUICK_SAVE, "D-pad up -> quick save");
    CHECK(bindings_match_pad(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, &slot) == ACT_FAST_WALK && slot == 1, "LB -> fast walk");
    CHECK(bindings_match_pad(SDL_CONTROLLER_BUTTON_A, NULL) == -1, "A stays the fixed left click");
}

static void test_rebind(void) {
    bindings_reset_defaults();
    KeyChord c = { SDLK_l, BIND_MOD_CTRL };
    int displaced = bindings_set_key(ACT_QUICK_SAVE, 0, c);
    CHECK(displaced == ACT_DIALOG_LOG, "rebinding Ctrl+L displaces the dialog log (got %d)", displaced);
    CHECK(bindings_key(ACT_DIALOG_LOG, 0)->key == 0, "dialog log unbound after conflict");
    CHECK(bindings_match_key(SDLK_l, KMOD_LCTRL, NULL) == ACT_QUICK_SAVE, "Ctrl+L now quick save");
    CHECK(bindings_match_key(SDLK_s, KMOD_LCTRL, NULL) == -1, "old Ctrl+S binding replaced");

    KeyChord esc = { SDLK_ESCAPE, 0 };
    CHECK(bindings_set_key(ACT_MUTE, 0, esc) == -2, "Escape is reserved");
    KeyChord altf4 = { SDLK_F4, BIND_MOD_ALT };
    CHECK(bindings_set_key(ACT_MUTE, 0, altf4) == -2, "Alt+F4 is reserved");
    CHECK(bindings_match_key(SDLK_F9, 0, NULL) == ACT_MUTE, "refused rebind keeps the old binding");

    /* hold action keys collide with press chords on the key alone */
    KeyChord h = { SDLK_h, BIND_MOD_CTRL };
    displaced = bindings_set_key(ACT_PERF_HUD, 0, h);
    CHECK(displaced == ACT_HOTSPOTS, "Ctrl+H displaces hold-H (got %d)", displaced);
    CHECK(bindings_key(ACT_HOTSPOTS, 1)->key == 0, "hotspots slot 2 cleared");

    bindings_set_pad(ACT_QUICK_LOAD, 0, SDL_CONTROLLER_BUTTON_X);
    CHECK(bindings_match_pad(SDL_CONTROLLER_BUTTON_X, NULL) == ACT_QUICK_LOAD, "X -> quick load");
    CHECK(bindings_pad(ACT_HOTSPOTS, 0) == -1, "X removed from hotspots");
}

static void test_format_parse(void) {
    bindings_reset_defaults();
    int n = 0;
    for (int a = 0; a < ACT_COUNT; a++) {
        for (int s = 0; s < BIND_SLOTS; s++) {
            const KeyChord *c = bindings_key((BindAction)a, s);
            char buf[64];
            KeyChord back;
            bindings_format_chord(c, buf, sizeof(buf));
            CHECK(bindings_parse_chord(buf, &back) == 0, "parse '%s'", buf);
            CHECK(back.key == c->key && back.mods == c->mods, "round trip '%s'", buf);
            n++;
        }
    }
    KeyChord c;
    CHECK(bindings_parse_chord("ctrl+alt+Delete", &c) == 0 && c.key == SDLK_DELETE &&
          c.mods == (BIND_MOD_CTRL | BIND_MOD_ALT), "case-insensitive modifiers");
    CHECK(bindings_parse_chord("NoSuchKey", &c) != 0, "unknown key rejected");
    printf("[bindings] %d chord format/parse round trips\n", n);
}

static void test_ini_roundtrip(void) {
    bindings_reset_defaults();
    KeyChord q = { SDLK_q, BIND_MOD_CTRL | BIND_MOD_SHIFT };
    bindings_set_key(ACT_QUICK_SAVE, 1, q);
    bindings_set_key(ACT_HD_TEXT, 0, (KeyChord){ 0, 0 });
    bindings_set_pad(ACT_PERF_HUD, 0, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    KeyChord want_k[ACT_COUNT][BIND_SLOTS];
    int want_p[ACT_COUNT][BIND_SLOTS];
    for (int a = 0; a < ACT_COUNT; a++)
        for (int s = 0; s < BIND_SLOTS; s++) {
            want_k[a][s] = *bindings_key((BindAction)a, s);
            want_p[a][s] = bindings_pad((BindAction)a, s);
        }
    FILE *f = tmpfile();
    bindings_ini_write(f);
    rewind(f);
    bindings_reset_defaults();
    char line[256];
    int lines = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        CHECK(bindings_ini_parse(line, eq + 1) == 1, "ini key '%s' consumed", line);
        lines++;
    }
    fclose(f);
    for (int a = 0; a < ACT_COUNT; a++)
        for (int s = 0; s < BIND_SLOTS; s++) {
            const KeyChord *k = bindings_key((BindAction)a, s);
            CHECK(k->key == want_k[a][s].key && k->mods == want_k[a][s].mods, "ini key %s/%d", bindings_action_id(a), s);
            CHECK(bindings_pad((BindAction)a, s) == want_p[a][s], "ini pad %s/%d", bindings_action_id(a), s);
        }
    CHECK(bindings_ini_parse("shader_mode", "1") == 0, "foreign ini keys are not consumed");
    printf("[bindings] ini round trip: %d entries\n", lines);
}

int main(void) {
    test_defaults();
    test_rebind();
    test_format_parse();
    test_ini_roundtrip();
    printf(g_fail ? "FAIL test_input_bindings\n" : "PASS test_input_bindings\n");
    return g_fail;
}
