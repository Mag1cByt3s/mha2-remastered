/*
 * Rebindable remaster hotkeys & controller buttons (see input_bindings.h).
 */
#include "input_bindings.h"
#include <string.h>
#include <strings.h>

typedef struct {
    const char *id;
    const char *label;
    int hold;
    KeyChord def_key[BIND_SLOTS];
    int def_pad[BIND_SLOTS];
} ActionInfo;

#define K(k) { (int32_t)(k), 0 }
#define KC(k) { (int32_t)(k), BIND_MOD_CTRL }
#define NOKEY { 0, 0 }
#define NOPAD (-1)

static const ActionInfo k_actions[ACT_COUNT] = {
    [ACT_SETTINGS_MENU]    = { "settings_menu", "Settings Menu", 0, { K(SDLK_F1), NOKEY }, { SDL_CONTROLLER_BUTTON_BACK, NOPAD } },
    [ACT_COMPARE_SPLIT]    = { "compare_split", "Before/After Split-Screen", 0, { K(SDLK_F2), NOKEY }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, NOPAD } },
    [ACT_PERF_HUD]         = { "perf_hud", "Performance HUD", 0, { K(SDLK_F3), NOKEY }, { NOPAD, NOPAD } },
    [ACT_VSYNC_CYCLE]      = { "vsync_cycle", "Cycle VSync / FPS Mode", 0, { K(SDLK_F4), NOKEY }, { NOPAD, NOPAD } },
    [ACT_HD_TEXT]          = { "hd_text", "Toggle HD Vector Text", 0, { K(SDLK_F5), NOKEY }, { NOPAD, NOPAD } },
    [ACT_HD_ASSETS]        = { "hd_assets", "Toggle HD Assets", 0, { K(SDLK_F6), NOKEY }, { NOPAD, NOPAD } },
    [ACT_VOLUME_DOWN]      = { "volume_down", "Volume Down", 0, { K(SDLK_F7), NOKEY }, { NOPAD, NOPAD } },
    [ACT_VOLUME_UP]        = { "volume_up", "Volume Up", 0, { K(SDLK_F8), NOKEY }, { NOPAD, NOPAD } },
    [ACT_MUTE]             = { "mute", "Mute / Unmute", 0, { K(SDLK_F9), NOKEY }, { NOPAD, NOPAD } },
    [ACT_DECOUPLED_CURSOR] = { "decoupled_cursor", "High-Refresh Cursor", 0, { K(SDLK_F10), NOKEY }, { NOPAD, NOPAD } },
    [ACT_FULLSCREEN]       = { "fullscreen", "Toggle Fullscreen", 0, { K(SDLK_F11), NOKEY }, { NOPAD, NOPAD } },
    [ACT_ASPECT_CYCLE]     = { "aspect_cycle", "Cycle Widescreen Aspect", 0, { K(SDLK_F12), NOKEY }, { NOPAD, NOPAD } },
    [ACT_DIALOG_LOG]       = { "dialog_log", "Dialog & Clue Log", 0, { KC(SDLK_l), NOKEY }, { NOPAD, NOPAD } },
    [ACT_SCREENSHOT]       = { "screenshot", "4K Screenshot", 0, { KC(SDLK_p), K(SDLK_PRINTSCREEN) }, { NOPAD, NOPAD } },
    [ACT_QUICK_SAVE]       = { "quick_save", "Quick Save (Slot 9)", 0, { KC(SDLK_s), NOKEY }, { SDL_CONTROLLER_BUTTON_DPAD_UP, NOPAD } },
    [ACT_QUICK_LOAD]       = { "quick_load", "Quick Load (Slot 9)", 0, { KC(SDLK_o), NOKEY }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, NOPAD } },
    [ACT_HOTSPOTS]         = { "hotspots", "Show Hotspots (hold)", 1, { K(SDLK_SPACE), K(SDLK_h) }, { SDL_CONTROLLER_BUTTON_X, NOPAD } },
    [ACT_FAST_WALK]        = { "fast_walk", "Fast Walk 2x (hold)", 1, { K(SDLK_LSHIFT), K(SDLK_RSHIFT) }, { SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_LEFTSHOULDER } },
};

static KeyChord g_key[ACT_COUNT][BIND_SLOTS];
static int g_pad[ACT_COUNT][BIND_SLOTS];
static int g_init;

static void ensure_init(void) {
    if (!g_init) bindings_reset_defaults();
}

void bindings_reset_defaults(void) {
    for (int a = 0; a < ACT_COUNT; a++) {
        for (int s = 0; s < BIND_SLOTS; s++) {
            g_key[a][s] = k_actions[a].def_key[s];
            g_pad[a][s] = k_actions[a].def_pad[s];
        }
    }
    g_init = 1;
}

const char *bindings_action_label(BindAction a) {
    return (a >= 0 && a < ACT_COUNT) ? k_actions[a].label : "?";
}

const char *bindings_action_id(BindAction a) {
    return (a >= 0 && a < ACT_COUNT) ? k_actions[a].id : "?";
}

int bindings_is_hold(BindAction a) {
    return (a >= 0 && a < ACT_COUNT) ? k_actions[a].hold : 0;
}

uint8_t bindings_mods_from_sdl(uint16_t m) {
    return (uint8_t)(((m & KMOD_CTRL) ? BIND_MOD_CTRL : 0u) | ((m & KMOD_SHIFT) ? BIND_MOD_SHIFT : 0u) |
                     ((m & KMOD_ALT) ? BIND_MOD_ALT : 0u));
}

static int is_modifier_key(SDL_Keycode k) {
    return k == SDLK_LCTRL || k == SDLK_RCTRL || k == SDLK_LSHIFT || k == SDLK_RSHIFT ||
           k == SDLK_LALT || k == SDLK_RALT || k == SDLK_LGUI || k == SDLK_RGUI;
}

int bindings_match_key(SDL_Keycode sym, uint16_t sdl_mod, int *out_slot) {
    ensure_init();
    uint8_t held = bindings_mods_from_sdl(sdl_mod);
    for (int a = 0; a < ACT_COUNT; a++) {
        for (int s = 0; s < BIND_SLOTS; s++) {
            const KeyChord *c = &g_key[a][s];
            if (!c->key || c->key != (int32_t)sym) continue;
            if (!k_actions[a].hold) {
                if ((held & c->mods) != c->mods) continue;                     /* required held */
                uint8_t strict = (uint8_t)(BIND_MOD_CTRL | BIND_MOD_ALT) & (uint8_t)~c->mods;
                if (held & strict) continue;                                    /* no extra Ctrl/Alt */
            }
            if (out_slot) *out_slot = s;
            return a;
        }
    }
    return -1;
}

int bindings_match_key_release(SDL_Keycode sym, int *out_slot) {
    ensure_init();
    for (int a = 0; a < ACT_COUNT; a++) {
        if (!k_actions[a].hold) continue;
        for (int s = 0; s < BIND_SLOTS; s++) {
            if (g_key[a][s].key && g_key[a][s].key == (int32_t)sym) {
                if (out_slot) *out_slot = s;
                return a;
            }
        }
    }
    return -1;
}

int bindings_match_pad(int button, int *out_slot) {
    ensure_init();
    if (button < 0) return -1;
    for (int a = 0; a < ACT_COUNT; a++) {
        for (int s = 0; s < BIND_SLOTS; s++) {
            if (g_pad[a][s] == button) {
                if (out_slot) *out_slot = s;
                return a;
            }
        }
    }
    return -1;
}

const KeyChord *bindings_key(BindAction a, int slot) {
    static const KeyChord none = { 0, 0 };
    ensure_init();
    if (a < 0 || a >= ACT_COUNT || slot < 0 || slot >= BIND_SLOTS) return &none;
    return &g_key[a][slot];
}

int bindings_pad(BindAction a, int slot) {
    ensure_init();
    if (a < 0 || a >= ACT_COUNT || slot < 0 || slot >= BIND_SLOTS) return -1;
    return g_pad[a][slot];
}

static int chord_reserved(KeyChord c) {
    if (c.key == SDLK_ESCAPE) return 1;
    if (c.key == SDLK_F4 && (c.mods & BIND_MOD_ALT)) return 1;
    if ((c.key == SDLK_RETURN || c.key == SDLK_KP_ENTER) && (c.mods & BIND_MOD_ALT)) return 1;
    return 0;
}

int bindings_set_key(BindAction a, int slot, KeyChord c) {
    ensure_init();
    if (a < 0 || a >= ACT_COUNT || slot < 0 || slot >= BIND_SLOTS) return -1;
    if (c.key && chord_reserved(c)) return -2;
    if (k_actions[a].hold) c.mods = 0; /* hold actions trigger on the key alone */
    int displaced = -1;
    if (c.key) {
        for (int o = 0; o < ACT_COUNT; o++) {
            for (int s = 0; s < BIND_SLOTS; s++) {
                if (o == (int)a && s == slot) continue;
                KeyChord *x = &g_key[o][s];
                /* hold keys collide on the key alone; press chords on key + modifiers */
                int same = x->key == c.key &&
                           (k_actions[o].hold || k_actions[a].hold || x->mods == c.mods);
                if (same) {
                    x->key = 0;
                    x->mods = 0;
                    if (o != (int)a) displaced = o;
                }
            }
        }
    }
    g_key[a][slot] = c;
    return displaced;
}

void bindings_set_pad(BindAction a, int slot, int button) {
    ensure_init();
    if (a < 0 || a >= ACT_COUNT || slot < 0 || slot >= BIND_SLOTS) return;
    if (button >= 0) {
        for (int o = 0; o < ACT_COUNT; o++)
            for (int s = 0; s < BIND_SLOTS; s++)
                if (g_pad[o][s] == button) g_pad[o][s] = -1;
    }
    g_pad[a][slot] = button;
}

void bindings_format_chord(const KeyChord *c, char *buf, size_t cap) {
    if (!cap) return;
    buf[0] = 0;
    if (!c || !c->key) return;
    const char *name = SDL_GetKeyName((SDL_Keycode)c->key);
    snprintf(buf, cap, "%s%s%s%s", (c->mods & BIND_MOD_CTRL) ? "Ctrl+" : "",
             (c->mods & BIND_MOD_ALT) ? "Alt+" : "", (c->mods & BIND_MOD_SHIFT) ? "Shift+" : "",
             (name && *name) ? name : "?");
}

void bindings_describe(BindAction a, char *buf, size_t cap) {
    char k0[48], k1[48];
    bindings_format_chord(bindings_key(a, 0), k0, sizeof(k0));
    bindings_format_chord(bindings_key(a, 1), k1, sizeof(k1));
    if (!k0[0] && !k1[0]) snprintf(buf, cap, "unbound");
    else snprintf(buf, cap, "%s%s%s", k0[0] ? k0 : k1, (k0[0] && k1[0]) ? "/" : "", (k0[0] && k1[0]) ? k1 : "");
}

int bindings_parse_chord(const char *s, KeyChord *out) {
    KeyChord c = { 0, 0 };
    if (!s) return -1;
    while (*s == ' ') s++;
    if (!*s) { *out = c; return 0; } /* explicitly unbound */
    for (;;) {
        if (!strncasecmp(s, "Ctrl+", 5)) { c.mods |= BIND_MOD_CTRL; s += 5; }
        else if (!strncasecmp(s, "Alt+", 4)) { c.mods |= BIND_MOD_ALT; s += 4; }
        else if (!strncasecmp(s, "Shift+", 6) && s[6]) { c.mods |= BIND_MOD_SHIFT; s += 6; }
        else break;
    }
    SDL_Keycode k = SDL_GetKeyFromName(s);
    if (k == SDLK_UNKNOWN) return -1;
    if (is_modifier_key(k)) c.mods = 0;
    c.key = (int32_t)k;
    *out = c;
    return 0;
}

int bindings_ini_parse(const char *key, const char *val) {
    ensure_init();
    int is_pad = !strncmp(key, "pad_", 4);
    if (!is_pad && strncmp(key, "bind_", 5) != 0) return 0;
    const char *stem = key + (is_pad ? 4 : 5);
    for (int a = 0; a < ACT_COUNT; a++) {
        size_t n = strlen(k_actions[a].id);
        if (strncmp(stem, k_actions[a].id, n) != 0) continue;
        int slot;
        if (stem[n] == 0) slot = 0;
        else if (!strcmp(stem + n, "_2")) slot = 1;
        else continue;
        if (is_pad) {
            int b = (*val == 0) ? -1 : (int)SDL_GameControllerGetButtonFromString(val);
            g_pad[a][slot] = b; /* SDL_CONTROLLER_BUTTON_INVALID == -1 */
        } else {
            KeyChord c;
            if (bindings_parse_chord(val, &c) == 0 && !chord_reserved(c)) g_key[a][slot] = c;
        }
        return 1;
    }
    return 0;
}

void bindings_ini_write(FILE *f) {
    ensure_init();
    fprintf(f, "# Remaster hotkeys (F1 -> 4. CONTROLS). Chords like Ctrl+S, F1, Space; empty = unbound.\n");
    for (int a = 0; a < ACT_COUNT; a++) {
        char b0[64], b1[64];
        bindings_format_chord(&g_key[a][0], b0, sizeof(b0));
        bindings_format_chord(&g_key[a][1], b1, sizeof(b1));
        fprintf(f, "bind_%s=%s\n", k_actions[a].id, b0);
        fprintf(f, "bind_%s_2=%s\n", k_actions[a].id, b1);
    }
    fprintf(f, "# Controller buttons (a b x y back start leftshoulder rightshoulder dpup dpdown dpleft dpright ...)\n");
    for (int a = 0; a < ACT_COUNT; a++) {
        for (int s = 0; s < BIND_SLOTS; s++) {
            const char *bn = g_pad[a][s] >= 0
                ? SDL_GameControllerGetStringForButton((SDL_GameControllerButton)g_pad[a][s]) : "";
            fprintf(f, "pad_%s%s=%s\n", k_actions[a].id, s ? "_2" : "", bn ? bn : "");
        }
    }
}
