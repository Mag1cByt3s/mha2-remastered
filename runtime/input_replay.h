#ifndef MHA2_INPUT_REPLAY_H
#define MHA2_INPUT_REPLAY_H
/*
 * Input record / replay for regression & smoke testing (--record / --replay).
 *
 * Scripts are line-based text (see docs/REPLAY.md). Event times are milliseconds
 * since the last sync point ("wait room <prefix>", "wait menu"), so replays stay
 * aligned regardless of loading speed. Mouse positions are guest 800x600
 * coordinates injected after window mapping (aspect/resolution independent);
 * keys and text are pushed through SDL so they take the real input path.
 */
#include <SDL2/SDL.h>

int  input_replay_init(const char *record_path, const char *replay_path, const char *out_dir);
void input_replay_shutdown(void);

/* Replay running: real keyboard/mouse/controller input is ignored. */
int  input_replay_active(void);
/* Called at the top of every SDL event pump: syncs and injects due events. */
void input_replay_poll(void);
/* True for events injected by the replay engine (tagged windowID). */
int  input_replay_is_injected(const SDL_Event *ev);

/* Recording hooks (no-ops unless --record). Mouse coords are guest coords;
 * button: -1 = move, 0 = left, 1 = right, 2 = middle. */
void input_replay_record_event(const SDL_Event *ev);
void input_replay_record_mouse(float gx, float gy, int button, int down);

/* Quick save / quick load (default Ctrl+S / Ctrl+O): drive the game's own save screen
 * into the dedicated quick slot 9. No-ops with a toast when not possible right now. */
void input_replay_quick_save(void);
void input_replay_quick_load(void);

/* Guest input entry points shared by the SDL path and the replay engine
 * (runtime/win32_user_gdi.c). */
void win32_input_mouse_move(float gx, float gy);
void win32_input_mouse_pos(float *gx, float *gy);
void win32_input_mouse_button(float gx, float gy, int button, int down);
void win32_input_text(const char *utf8);

#endif
