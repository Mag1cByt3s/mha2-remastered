#ifndef MHA2_INPUT_BINDINGS_H
#define MHA2_INPUT_BINDINGS_H
/*
 * Rebindable remaster hotkeys & controller buttons (F1 menu "4. CONTROLS",
 * persisted in mha2_settings.ini as bind_<id>, bind_<id>_2, pad_<id>, pad_<id>_2).
 *
 * Fixed, non-rebindable: Escape (game / menu cancel), Alt+F4, Alt+Enter, and the
 * controller's A / B / Start (left click / right click / Escape). Keys of "hold"
 * actions (hotspots, fast walk) are also forwarded to the game, as before.
 * SDL-only (no GL), so tests/ can link it directly.
 */
#include <SDL2/SDL.h>
#include <stdio.h>

typedef enum BindAction {
    ACT_SETTINGS_MENU = 0,
    ACT_COMPARE_SPLIT,
    ACT_PERF_HUD,
    ACT_VSYNC_CYCLE,
    ACT_HD_TEXT,
    ACT_HD_ASSETS,
    ACT_VOLUME_DOWN,
    ACT_VOLUME_UP,
    ACT_MUTE,
    ACT_DECOUPLED_CURSOR,
    ACT_FULLSCREEN,
    ACT_ASPECT_CYCLE,
    ACT_DIALOG_LOG,
    ACT_SCREENSHOT,
    ACT_QUICK_SAVE,
    ACT_QUICK_LOAD,
    ACT_HOTSPOTS,   /* hold */
    ACT_FAST_WALK,  /* hold */
    ACT_COUNT
} BindAction;

#define BIND_SLOTS     2      /* keyboard chords and controller buttons per action */
#define BIND_MOD_CTRL  1u
#define BIND_MOD_SHIFT 2u
#define BIND_MOD_ALT   4u

typedef struct KeyChord {
    int32_t key;   /* SDL_Keycode, 0 = unbound */
    uint8_t mods;  /* BIND_MOD_* required modifiers */
} KeyChord;

void        bindings_reset_defaults(void);
const char *bindings_action_label(BindAction a);   /* UI text */
const char *bindings_action_id(BindAction a);      /* ini key stem */
int         bindings_is_hold(BindAction a);

/* Keyboard. Press: required modifiers must be held and Ctrl/Alt must not be held
 * unless required (so plain "L" typed into a savegame name never opens the log).
 * Hold actions match on the key alone. Returns the action or -1; *out_slot = slot. */
int  bindings_match_key(SDL_Keycode sym, uint16_t sdl_mod, int *out_slot);
int  bindings_match_key_release(SDL_Keycode sym, int *out_slot);
/* Controller: returns the action bound to an SDL_GameControllerButton or -1. */
int  bindings_match_pad(int button, int *out_slot);

const KeyChord *bindings_key(BindAction a, int slot);
int  bindings_pad(BindAction a, int slot);  /* SDL_CONTROLLER_BUTTON_*, -1 = unbound */
/* Assign a chord; any other action/slot using the same chord is unbound. Returns the
 * displaced action (or -1). Reserved chords (Escape, Alt+F4, Alt+Enter) are refused
 * with -2. */
int  bindings_set_key(BindAction a, int slot, KeyChord c);
void bindings_set_pad(BindAction a, int slot, int button);

/* UI hint for an action's keys: "Ctrl+L", "Space/H", "unbound" */
void bindings_describe(BindAction a, char *buf, size_t cap);

/* "Ctrl+S", "F1", "Left Shift", "" (unbound) */
void bindings_format_chord(const KeyChord *c, char *buf, size_t cap);
int  bindings_parse_chord(const char *s, KeyChord *out);   /* 0 = ok */
uint8_t bindings_mods_from_sdl(uint16_t sdl_mod);

/* mha2_settings.ini integration */
int  bindings_ini_parse(const char *key, const char *val); /* 1 = key consumed */
void bindings_ini_write(FILE *f);

#endif
