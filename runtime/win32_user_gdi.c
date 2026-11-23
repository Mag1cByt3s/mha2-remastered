#include "runtime.h"
#include "gl_renderer.h"
#include "imports.h"
#include <ctype.h>

SDL_Window   *g_sdl_window   = NULL;
SDL_Renderer *g_sdl_renderer = NULL;
int           g_win_w        = 800;
int           g_win_h        = 600;

static uint32_t g_wndproc_va  = 0;
static uint32_t g_main_hwnd   = 0x00010001u;
static uint32_t g_wnd_userdata = 0;

/* ---- Win32 Message Queue ---- */
typedef struct WinMsg {
    uint32_t hwnd;
    uint32_t msg;
    uint32_t wparam;
    uint32_t lparam;
    uint32_t time;
    int32_t  pt_x;
    int32_t  pt_y;
} WinMsg;

#define MSG_Q_CAP 512
static WinMsg g_msg_q[MSG_Q_CAP];
static int g_msg_head = 0, g_msg_tail = 0;
int g_mouse_x = 0, g_mouse_y = 0;
int g_os_cursor_hidden = 0;
static float g_mouse_fx = 0.0f, g_mouse_fy = 0.0f;

static void push_msg(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    /* Coalesce consecutive WM_MOUSEMOVE events (just like real USER32) so
       500-1000Hz mice never overflow the original game's 9-msg/frame PeekMessage cap. */
    if (msg == 0x0200u && g_msg_head != g_msg_tail) {
        int prev = (g_msg_tail + MSG_Q_CAP - 1) % MSG_Q_CAP;
        if (g_msg_q[prev].msg == 0x0200u && g_msg_q[prev].hwnd == hwnd) {
            g_msg_q[prev].wparam = wp;
            g_msg_q[prev].lparam = lp;
            g_msg_q[prev].time = SDL_GetTicks();
            g_msg_q[prev].pt_x = g_mouse_x;
            g_msg_q[prev].pt_y = g_mouse_y;
            return;
        }
    }
    int next = (g_msg_tail + 1) % MSG_Q_CAP;
    if (next == g_msg_head) return; /* drop if full */
    WinMsg *m = &g_msg_q[g_msg_tail];
    m->hwnd = hwnd;
    m->msg = msg;
    m->wparam = wp;
    m->lparam = lp;
    m->time = SDL_GetTicks();
    m->pt_x = g_mouse_x;
    m->pt_y = g_mouse_y;
    g_msg_tail = next;
}

static int pop_msg(WinMsg *out, int remove) {
    if (g_msg_head == g_msg_tail) return 0;
    *out = g_msg_q[g_msg_head];
    if (remove) g_msg_head = (g_msg_head + 1) % MSG_Q_CAP;
    return 1;
}

static uint32_t sdl_key_to_vk(SDL_Keycode sym) {
    if (sym >= 'a' && sym <= 'z') return (uint32_t)(sym - 'a' + 'A');
    if (sym >= '0' && sym <= '9') return (uint32_t)sym;
    if (sym >= SDLK_F1 && sym <= SDLK_F12) return 0x70u + (uint32_t)(sym - SDLK_F1);
    switch (sym) {
        case SDLK_BACKSPACE: return 0x08;
        case SDLK_TAB:       return 0x09;
        case SDLK_RETURN:    return 0x0D;
        case SDLK_KP_ENTER:  return 0x0D;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:    return 0x10;
        case SDLK_LCTRL:
        case SDLK_RCTRL:     return 0x11;
        case SDLK_LALT:
        case SDLK_RALT:      return 0x12;
        case SDLK_PAUSE:     return 0x13;
        case SDLK_CAPSLOCK:  return 0x14;
        case SDLK_ESCAPE:    return 0x1B;
        case SDLK_SPACE:     return 0x20;
        case SDLK_PAGEUP:    return 0x21;
        case SDLK_PAGEDOWN:  return 0x22;
        case SDLK_END:       return 0x23;
        case SDLK_HOME:      return 0x24;
        case SDLK_LEFT:      return 0x25;
        case SDLK_UP:        return 0x26;
        case SDLK_RIGHT:     return 0x27;
        case SDLK_DOWN:      return 0x28;
        case SDLK_INSERT:    return 0x2D;
        case SDLK_DELETE:    return 0x2E;
        case SDLK_MINUS:     return 0xBD;
        case SDLK_EQUALS:    return 0xBB;
        case SDLK_PERIOD:    return 0xBE;
        case SDLK_COMMA:     return 0xBC;
        default: return 0;
    }
}

static SDL_GameController *g_gamepad = NULL;
static uint32_t g_gamepad_last_ms = 0;

static void gamepad_ensure_open(void) {
    if (g_gamepad && SDL_GameControllerGetAttached(g_gamepad)) return;
    if (g_gamepad) {
        SDL_GameControllerClose(g_gamepad);
        g_gamepad = NULL;
    }
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (SDL_IsGameController(i)) {
            g_gamepad = SDL_GameControllerOpen(i);
            if (g_gamepad) {
                const char *nm = SDL_GameControllerName(g_gamepad);
                gl_renderer_show_toast("Controller Connected: %s", nm ? nm : "Gamepad");
                break;
            }
        }
    }
}

static void gamepad_update_cursor(void) {
    if (!g_gamepad || !SDL_GameControllerGetAttached(g_gamepad) || gl_menu_is_open()) return;
    uint32_t now = SDL_GetTicks();
    if (g_gamepad_last_ms == 0) { g_gamepad_last_ms = now; return; }
    uint32_t dt_ms = now - g_gamepad_last_ms;
    if (dt_ms < 4) return;
    if (dt_ms > 50) dt_ms = 50;
    g_gamepad_last_ms = now;

    float ax = (float)SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
    float ay = (float)SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
    float mag = sqrtf(ax * ax + ay * ay);
    if (mag < 0.18f) return;
    float norm = (mag - 0.18f) / 0.82f;
    if (norm > 1.0f) norm = 1.0f;
    float speed = (0.35f + 0.65f * norm * norm) * 460.0f * ((float)dt_ms * 0.001f);
    float nx = g_mouse_fx + (ax / mag) * speed;
    float ny = g_mouse_fy + (ay / mag) * speed;
    gl_renderer_clamp_gamepad_cursor(&nx, &ny);
    int gx = (int)(nx + 0.5f);
    int gy = (int)(ny + 0.5f);
    int dx = gx - g_mouse_x;
    int dy = gy - g_mouse_y;
    g_mouse_fx = nx; g_mouse_fy = ny;
    g_mouse_x = gx;  g_mouse_y = gy;
    gl_renderer_set_live_mouse(nx, ny, g_os_cursor_hidden);
    if (dx != 0 || dy != 0) {
        dinput_push_mouse_move(dx, dy, g_mouse_x, g_mouse_y);
        uint32_t lp = ((uint32_t)(uint16_t)g_mouse_y << 16) | (uint16_t)g_mouse_x;
        push_msg(g_main_hwnd, 0x0200u /* WM_MOUSEMOVE */, 0, lp);
    }
}

static uint32_t g_fast_walk_mask = 0; /* bit0: LShift, bit1: RShift, bit2: Pad Y, bit3: Pad LB */
static uint32_t g_hotspot_mask   = 0; /* bit0: Space, bit1: H, bit2: Pad X */

static void clear_held_qol_keys(void) {
    if (g_fast_walk_mask) {
        g_fast_walk_mask = 0;
        gl_renderer_set_fast_walk(0);
    }
    if (g_hotspot_mask) {
        g_hotspot_mask = 0;
        gl_renderer_set_hotspot_highlight(0);
    }
}

void win32_pump_sdl_events(void) {
    static int g_pad_init = 0;
    if (!g_pad_init) { g_pad_init = 1; gamepad_ensure_open(); }
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_CONTROLLERDEVICEADDED) {
            gamepad_ensure_open();
            continue;
        }
        if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            g_fast_walk_mask &= ~(4u | 8u);
            g_hotspot_mask   &= ~4u;
            gl_renderer_set_fast_walk(g_fast_walk_mask != 0);
            gl_renderer_set_hotspot_highlight(g_hotspot_mask != 0);
            if (g_gamepad && !SDL_GameControllerGetAttached(g_gamepad)) {
                SDL_GameControllerClose(g_gamepad);
                g_gamepad = NULL;
                gl_renderer_show_toast("Controller Disconnected");
            }
            continue;
        }
        if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP) {
            int down = (ev.type == SDL_CONTROLLERBUTTONDOWN);
            Uint8 btn = ev.cbutton.button;
            if (down && btn == SDL_CONTROLLER_BUTTON_BACK) {
                clear_held_qol_keys();
                gl_menu_toggle();
                continue;
            }
            if (gl_menu_is_open()) {
                if (down) {
                    SDL_Event synth;
                    memset(&synth, 0, sizeof(synth));
                    synth.type = SDL_KEYDOWN;
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) synth.key.keysym.sym = SDLK_UP;
                    else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) synth.key.keysym.sym = SDLK_DOWN;
                    else if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) synth.key.keysym.sym = SDLK_LEFT;
                    else if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) synth.key.keysym.sym = SDLK_RIGHT;
                    else if (btn == SDL_CONTROLLER_BUTTON_A) synth.key.keysym.sym = SDLK_RETURN;
                    else if (btn == SDL_CONTROLLER_BUTTON_B || btn == SDL_CONTROLLER_BUTTON_START) synth.key.keysym.sym = SDLK_ESCAPE;
                    else if (btn == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) synth.key.keysym.sym = SDLK_q;
                    else if (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) synth.key.keysym.sym = SDLK_e;
                    if (synth.key.keysym.sym) gl_menu_handle_event(&synth);
                }
                continue;
            }
            uint32_t lp = ((uint32_t)(uint16_t)g_mouse_y << 16) | (uint16_t)g_mouse_x;
            if (btn == SDL_CONTROLLER_BUTTON_A) {
                dinput_push_mouse_button(0, down);
                push_msg(g_main_hwnd, down ? 0x0201u : 0x0202u, down ? 1u : 0u, lp);
            } else if (btn == SDL_CONTROLLER_BUTTON_B) {
                dinput_push_mouse_button(1, down);
                push_msg(g_main_hwnd, down ? 0x0204u : 0x0205u, down ? 2u : 0u, lp);
            } else if (btn == SDL_CONTROLLER_BUTTON_X) {
                if (down) g_hotspot_mask |= 4u; else g_hotspot_mask &= ~4u;
                gl_renderer_set_hotspot_highlight(g_hotspot_mask != 0);
            } else if (btn == SDL_CONTROLLER_BUTTON_Y) {
                if (down) g_fast_walk_mask |= 4u; else g_fast_walk_mask &= ~4u;
                gl_renderer_set_fast_walk(g_fast_walk_mask != 0);
            } else if (btn == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                if (down) g_fast_walk_mask |= 8u; else g_fast_walk_mask &= ~8u;
                gl_renderer_set_fast_walk(g_fast_walk_mask != 0);
            } else if (down && btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                gl_renderer_toggle_compare_split();
            } else if (btn == SDL_CONTROLLER_BUTTON_START) {
                push_msg(g_main_hwnd, down ? 0x0100u : 0x0101u, 0x1Bu, down ? 1u : 0xC0000001u);
                if (down) push_msg(g_main_hwnd, 0x0102u, 0x1Bu, 1u);
            }
            continue;
        }
        if (ev.type == SDL_QUIT) {
            g_quit_requested = 1;
            push_msg(g_main_hwnd, 0x0010u /* WM_CLOSE */, 0, 0);
            push_msg(g_main_hwnd, 0x0012u /* WM_QUIT */, 0, 0);
            continue;
        }
        if (ev.type == SDL_KEYDOWN) {
            SDL_Keycode sym = ev.key.keysym.sym;
            if (sym == SDLK_F4 && (ev.key.keysym.mod & KMOD_ALT)) {
                g_quit_requested = 1;
                push_msg(g_main_hwnd, 0x0010u /* WM_CLOSE */, 0, 0);
                push_msg(g_main_hwnd, 0x0012u /* WM_QUIT */, 0, 0);
                continue;
            }
            if (sym == SDLK_F1) {
                clear_held_qol_keys();
                gl_menu_toggle();
                continue;
            }
            if (sym == SDLK_F11 || (sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                gl_renderer_toggle_fullscreen();
                continue;
            }
        }
        if (gl_menu_is_open()) {
            if (gl_menu_handle_event(&ev)) {
                continue;
            }
            /* Block all game keyboard/mouse input while F1 graphics menu is open */
            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP || ev.type == SDL_TEXTINPUT ||
                ev.type == SDL_MOUSEMOTION || ev.type == SDL_MOUSEBUTTONDOWN ||
                ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_MOUSEWHEEL) {
                continue;
            }
        }
        if (gl_renderer_is_dialog_log_open()) {
            if (ev.type == SDL_KEYDOWN &&
                ((ev.key.keysym.sym == SDLK_l && (ev.key.keysym.mod & KMOD_CTRL)) ||
                 ev.key.keysym.sym == SDLK_ESCAPE)) {
                gl_renderer_toggle_dialog_log();
                continue;
            }
            if (ev.type == SDL_MOUSEBUTTONDOWN) {
                gl_renderer_toggle_dialog_log();
                continue;
            }
            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP || ev.type == SDL_TEXTINPUT ||
                ev.type == SDL_MOUSEMOTION || ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_MOUSEWHEEL) {
                continue;
            }
        }
        switch (ev.type) {
            case SDL_KEYDOWN: {
                SDL_Keycode sym = ev.key.keysym.sym;
                if (sym == SDLK_l && (ev.key.keysym.mod & KMOD_CTRL)) {
                    clear_held_qol_keys();
                    gl_renderer_toggle_dialog_log();
                    break;
                }
                if (sym == SDLK_F2) { gl_renderer_toggle_compare_split(); break; }
                if (sym == SDLK_PRINTSCREEN || (sym == SDLK_p && (ev.key.keysym.mod & KMOD_CTRL))) {
                    gl_renderer_request_screenshot();
                    break;
                }
                if (sym == SDLK_LSHIFT) {
                    g_fast_walk_mask |= 1u;
                    gl_renderer_set_fast_walk(g_fast_walk_mask != 0);
                } else if (sym == SDLK_RSHIFT) {
                    g_fast_walk_mask |= 2u;
                    gl_renderer_set_fast_walk(g_fast_walk_mask != 0);
                }
                if (sym == SDLK_SPACE) {
                    g_hotspot_mask |= 1u;
                    gl_renderer_set_hotspot_highlight(g_hotspot_mask != 0);
                } else if (sym == SDLK_h) {
                    g_hotspot_mask |= 2u;
                    gl_renderer_set_hotspot_highlight(g_hotspot_mask != 0);
                }
                if (sym == SDLK_F3) { gl_renderer_toggle_hud(); break; }
                if (sym == SDLK_F4) { gl_renderer_cycle_vsync(); break; }
                if (sym == SDLK_F5) { gl_renderer_toggle_hd_text(); break; }
                if (sym == SDLK_F6) { gl_renderer_toggle_hd_assets(); break; }
                if (sym == SDLK_F10) { gl_renderer_toggle_decoupled_cursor(); break; }
                if (sym == SDLK_F12) { gl_renderer_cycle_aspect(); break; }
                if (sym == SDLK_F7 || sym == SDLK_F8) {
                    float vol = g_cfg.master_volume + ((sym == SDLK_F8) ? 0.1f : -0.1f);
                    if (vol < 0.0f) vol = 0.0f;
                    if (vol > 1.5f) vol = 1.5f;
                    __atomic_store(&g_cfg.master_volume, &vol, __ATOMIC_RELAXED);
                    __atomic_store_n(&g_cfg.muted, 0, __ATOMIC_RELAXED);
                    gl_config_save(NULL);
                    gl_renderer_show_toast("Volume: %d%%", (int)(vol * 100.0f + 0.5f));
                    break;
                }
                if (sym == SDLK_F9) {
                    int muted = !g_cfg.muted;
                    __atomic_store_n(&g_cfg.muted, muted, __ATOMIC_RELAXED);
                    gl_config_save(NULL);
                    gl_renderer_show_toast("Audio: %s", muted ? "Muted" : "Unmuted");
                    break;
                }
                uint32_t vk = sdl_key_to_vk(sym);
                if (vk) {
                    push_msg(g_main_hwnd, 0x0100u /* WM_KEYDOWN */, vk, 1);
                    if (vk == 0x08 || vk == 0x0D || vk == 0x1B || vk == 0x09) {
                        push_msg(g_main_hwnd, 0x0102u /* WM_CHAR */, vk, 1);
                    }
                }
                break;
            }
            case SDL_KEYUP: {
                SDL_Keycode sym = ev.key.keysym.sym;
                if (sym == SDLK_LSHIFT) {
                    g_fast_walk_mask &= ~1u;
                    gl_renderer_set_fast_walk(g_fast_walk_mask != 0);
                } else if (sym == SDLK_RSHIFT) {
                    g_fast_walk_mask &= ~2u;
                    gl_renderer_set_fast_walk(g_fast_walk_mask != 0);
                }
                if (sym == SDLK_SPACE) {
                    g_hotspot_mask &= ~1u;
                    gl_renderer_set_hotspot_highlight(g_hotspot_mask != 0);
                } else if (sym == SDLK_h) {
                    g_hotspot_mask &= ~2u;
                    gl_renderer_set_hotspot_highlight(g_hotspot_mask != 0);
                }
                uint32_t vk = sdl_key_to_vk(sym);
                if (vk) push_msg(g_main_hwnd, 0x0101u /* WM_KEYUP */, vk, 0xC0000001u);
                break;
            }
            case SDL_TEXTINPUT: {
                const uint8_t *p = (const uint8_t *)ev.text.text;
                while (*p) {
                    uint32_t cp = 0;
                    if (*p < 0x80) { cp = *p++; }
                    else if ((*p & 0xE0) == 0xC0 && p[1]) {
                        cp = ((*p & 0x1Fu) << 6) | (p[1] & 0x3Fu);
                        p += 2;
                    } else { p++; continue; }
                    if (cp >= 0x20 && cp <= 0xFF) {
                        push_msg(g_main_hwnd, 0x0102u /* WM_CHAR */, cp, 1);
                    }
                }
                break;
            }
            case SDL_MOUSEMOTION: {
                float fx, fy;
                gl_window_to_game_coords_f(ev.motion.x, ev.motion.y, &fx, &fy);
                int gx = (int)(fx + 0.5f);
                int gy = (int)(fy + 0.5f);
                int dx = gx - g_mouse_x;
                int dy = gy - g_mouse_y;
                g_mouse_fx = fx;
                g_mouse_fy = fy;
                g_mouse_x = gx;
                g_mouse_y = gy;
                gl_renderer_set_live_mouse(fx, fy, g_os_cursor_hidden);
                dinput_push_mouse_move(dx, dy, g_mouse_x, g_mouse_y);
                uint32_t lp = ((uint32_t)(uint16_t)g_mouse_y << 16) | (uint16_t)g_mouse_x;
                push_msg(g_main_hwnd, 0x0200u /* WM_MOUSEMOVE */, 0, lp);
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                int down = (ev.type == SDL_MOUSEBUTTONDOWN);
                float fx, fy;
                gl_window_to_game_coords_f(ev.button.x, ev.button.y, &fx, &fy);
                int gx = (int)(fx + 0.5f);
                int gy = (int)(fy + 0.5f);
                g_mouse_fx = fx;
                g_mouse_fy = fy;
                g_mouse_x = gx;
                g_mouse_y = gy;
                gl_renderer_set_live_mouse(fx, fy, g_os_cursor_hidden);
                uint32_t lp = ((uint32_t)(uint16_t)g_mouse_y << 16) | (uint16_t)g_mouse_x;
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    dinput_push_mouse_button(0, down);
                    push_msg(g_main_hwnd, down ? 0x0201u : 0x0202u, down ? 1u : 0u, lp);
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    dinput_push_mouse_button(1, down);
                    push_msg(g_main_hwnd, down ? 0x0204u : 0x0205u, down ? 2u : 0u, lp);
                } else if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    dinput_push_mouse_button(2, down);
                    push_msg(g_main_hwnd, down ? 0x0207u : 0x0208u, down ? 16u : 0u, lp);
                }
                break;
            }
            case SDL_MOUSEWHEEL: {
                int dz = ev.wheel.y * 120;
                dinput_push_mouse_wheel(dz);
                uint32_t wp = ((uint32_t)(uint16_t)(int16_t)dz) << 16;
                uint32_t lp = ((uint32_t)(uint16_t)g_mouse_y << 16) | (uint16_t)g_mouse_x;
                push_msg(g_main_hwnd, 0x020Au /* WM_MOUSEWHEEL */, wp, lp);
                break;
            }
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    push_msg(g_main_hwnd, 0x001Cu /* WM_ACTIVATEAPP */, 1, 0);
                    push_msg(g_main_hwnd, 0x0006u /* WM_ACTIVATE */, 1, 0);
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    clear_held_qol_keys();
                    push_msg(g_main_hwnd, 0x0006u /* WM_ACTIVATE */, 0, 0);
                    push_msg(g_main_hwnd, 0x001Cu /* WM_ACTIVATEAPP */, 0, 0);
                } else if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                           ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                           ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                    gl_renderer_redraw_if_needed(1);
                }
                break;
        }
    }
    gamepad_update_cursor();
    gl_renderer_redraw_if_needed(0);
}

/* ---- GDI Objects & SDL_ttf Font Engine ---- */
typedef struct GdiFont {
    int in_use;
    TTF_Font *ttf;
    int height;
    int weight;
    char face[64];
} GdiFont;

typedef struct GdiDC {
    int in_use;
    uint32_t surface_handle; /* 0 = memory/screen DC, >0 = DDrawSurface handle */
    uint32_t font_handle;
    uint32_t text_color;     /* COLORREF 0x00BBGGRR */
    uint32_t bk_color;       /* COLORREF 0x00BBGGRR */
    uint32_t bk_mode;        /* 1 = TRANSPARENT, 2 = OPAQUE */
} GdiDC;

#define MAX_GDI_FONTS 256
#define MAX_GDI_DCS   256
static GdiFont g_fonts[MAX_GDI_FONTS];
static GdiDC   g_dcs[MAX_GDI_DCS];
static char    g_ttf_path[1024] = "";

static void find_system_ttf(void) {
    const char *env = getenv("MHA2_FONT_PATH");
    if (env && env[0]) {
        FILE *f = fopen(env, "rb");
        if (f) { fclose(f); snprintf(g_ttf_path, sizeof(g_ttf_path), "%s", env); return; }
    }
    const char *candidates[] = {
        "fonts/DejaVuSans.ttf",
        "/run/current-system/sw/share/X11/fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) { fclose(f); snprintf(g_ttf_path, sizeof(g_ttf_path), "%s", candidates[i]); return; }
    }
}

static TTF_Font *get_dc_font(GdiDC *dc) {
    uint32_t fh = dc ? dc->font_handle : 0;
    if (fh >= 0x2000 && fh < 0x2000 + MAX_GDI_FONTS && g_fonts[fh - 0x2000].in_use) {
        return g_fonts[fh - 0x2000].ttf;
    }
    /* Lazily open default 16px font in slot 0 */
    if (!g_fonts[0].in_use && g_ttf_path[0]) {
        g_fonts[0].ttf = TTF_OpenFont(g_ttf_path, 14);
        g_fonts[0].height = 16;
        g_fonts[0].in_use = 1;
    }
    return g_fonts[0].ttf;
}

static void cp1252_to_utf8(const uint8_t *src, int len, char *dst, size_t dst_sz) {
    size_t j = 0;
    for (int i = 0; i < len && j + 4 < dst_sz; i++) {
        uint8_t ch = src[i];
        if (ch == 0) break;
        if (ch < 0x80) {
            dst[j++] = (char)ch;
        } else {
            /* Standard ISO-8859-1 / Windows-1252 range 0xA0..0xFF (umlauts äöüßÄÖÜ etc.) */
            uint32_t cp = ch;
            if (ch == 0x80) cp = 0x20AC;
            else if (ch == 0x84) cp = 0x201E;
            else if (ch == 0x93) cp = 0x201C;
            else if (ch == 0x94) cp = 0x201D;
            else if (ch == 0x96) cp = 0x2013;
            else if (ch == 0x97) cp = 0x2014;
            if (cp < 0x800) {
                dst[j++] = (char)(0xC0 | (cp >> 6));
                dst[j++] = (char)(0x80 | (cp & 0x3F));
            } else {
                dst[j++] = (char)(0xE0 | (cp >> 12));
                dst[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                dst[j++] = (char)(0x80 | (cp & 0x3F));
            }
        }
    }
    dst[j] = '\0';
}

uint32_t gdi_create_surface_dc(uint32_t surface_handle) {
    for (int i = 1; i < MAX_GDI_DCS; i++) {
        if (!g_dcs[i].in_use) {
            g_dcs[i].in_use = 1;
            g_dcs[i].surface_handle = surface_handle;
            g_dcs[i].font_handle = 0;
            g_dcs[i].text_color = 0x00FFFFFFu;
            g_dcs[i].bk_color = 0x00000000u;
            g_dcs[i].bk_mode = 2; /* OPAQUE */
            return 0x3000u + (uint32_t)i;
        }
    }
    return 0;
}

void gdi_release_surface_dc(uint32_t hdc) {
    if (hdc >= 0x3001 && hdc < 0x3000 + MAX_GDI_DCS) {
        g_dcs[hdc - 0x3000].in_use = 0;
    }
}

static GdiDC *get_dc(uint32_t hdc) {
    if (hdc >= 0x3001 && hdc < 0x3000 + MAX_GDI_DCS && g_dcs[hdc - 0x3000].in_use) {
        return &g_dcs[hdc - 0x3000];
    }
    return &g_dcs[0];
}

void win32_user_gdi_init(void) {
    if (TTF_Init() != 0) {
        fprintf(stderr, "WARN: TTF_Init failed: %s\n", TTF_GetError());
    }
    find_system_ttf();
    g_dcs[0].in_use = 1;
    g_dcs[0].text_color = 0x00FFFFFFu;
    g_dcs[0].bk_color = 0x00000000u;
    g_dcs[0].bk_mode = 2;
}

static void ensure_sdl_window(const char *title, int w, int h) {
    if (w <= 0) w = 800;
    if (h <= 0) h = 600;
    g_win_w = w;
    g_win_h = h;
    if (!g_sdl_window) {
        gl_renderer_init(title, w, h);
    }
}

int win32_user_gdi_dispatch(CpuCtx *c, uint32_t import_id) {
    switch (import_id) {
        /* ---- USER32 ---- */
        case IMP_USER32_DLL_RegisterClassExA: {
            uint32_t wc = arg_u32(c, 0);
            if (wc) {
                g_wndproc_va = mem_r32(wc + 8);
                if (g_debug_log) fprintf(stderr, "[USER32] RegisterClassExA(wndproc=0x%08x)\n", g_wndproc_va);
            }
            stdcall_ret(c, 1, 0xC001u); return 1;
        }
        case IMP_USER32_DLL_CreateWindowExA: {
            const char *title = arg_cstr(c, 2);
            int w = arg_i32(c, 6);
            int h = arg_i32(c, 7);
            if (w <= 0 || w > 4096) w = 800;
            if (h <= 0 || h > 4096) h = 600;
            ensure_sdl_window(title, w, h);
            if (g_debug_log) fprintf(stderr, "[USER32] CreateWindowExA('%s', %dx%d)\n", title ? title : "", w, h);
            /* Send WM_CREATE */
            if (g_wndproc_va) {
                uint32_t a[4] = { g_main_hwnd, 0x0001u, 0, 0 };
                guest_call_stdcall(c, g_wndproc_va, 4, a);
            }
            stdcall_ret(c, 12, g_main_hwnd); return 1;
        }
        case IMP_USER32_DLL_DestroyWindow: {
            uint32_t hwnd = arg_u32(c, 0);
            if (hwnd && g_wndproc_va) {
                uint32_t a[4] = { hwnd, 0x0002u /* WM_DESTROY */, 0, 0 };
                guest_call_stdcall(c, g_wndproc_va, 4, a);
            }
            stdcall_ret(c, 1, 1); return 1;
        }
        case IMP_USER32_DLL_ShowWindow:
            if (arg_i32(c, 1) == 0 && g_sdl_window) {
                SDL_HideWindow(g_sdl_window);
            }
            stdcall_ret(c, 2, 1); return 1;
        case IMP_USER32_DLL_UpdateWindow:
            stdcall_ret(c, 1, 1); return 1;
        case IMP_USER32_DLL_ValidateRect:
            stdcall_ret(c, 2, 1); return 1;
        case IMP_USER32_DLL_SetWindowPos:
            stdcall_ret(c, 7, 1); return 1;
        case IMP_USER32_DLL_SetForegroundWindow:
        case IMP_USER32_DLL_SetActiveWindow:
        case IMP_USER32_DLL_SetFocus:
            stdcall_ret(c, 1, g_main_hwnd); return 1;
        case IMP_USER32_DLL_GetParent:
            stdcall_ret(c, 1, 0); return 1;
        case IMP_USER32_DLL_SetWindowLongA: {
            int idx = arg_i32(c, 1);
            uint32_t val = arg_u32(c, 2);
            uint32_t prev = 0;
            if (idx == -21 /* GWL_USERDATA */) { prev = g_wnd_userdata; g_wnd_userdata = val; }
            else if (idx == -4 /* GWL_WNDPROC */) { prev = g_wndproc_va; g_wndproc_va = val; }
            stdcall_ret(c, 3, prev); return 1;
        }
        case IMP_USER32_DLL_GetClientRect: {
            uint32_t rc = arg_u32(c, 1);
            if (rc) {
                mem_w32(rc + 0, 0);
                mem_w32(rc + 4, 0);
                mem_w32(rc + 8, (uint32_t)g_win_w);
                mem_w32(rc + 12, (uint32_t)g_win_h);
            }
            stdcall_ret(c, 2, 1); return 1;
        }
        case IMP_USER32_DLL_GetWindowPlacement: {
            uint32_t wp = arg_u32(c, 1);
            if (wp) {
                memset(g_mem + wp, 0, 44);
                mem_w32(wp + 0, 44);
                mem_w32(wp + 8, 1);
                mem_w32(wp + 28, 0);
                mem_w32(wp + 32, 0);
                mem_w32(wp + 36, (uint32_t)g_win_w);
                mem_w32(wp + 40, (uint32_t)g_win_h);
            }
            stdcall_ret(c, 2, 1); return 1;
        }
        case IMP_USER32_DLL_SetWindowPlacement:
            stdcall_ret(c, 2, 1); return 1;
        case IMP_USER32_DLL_ClientToScreen:
            stdcall_ret(c, 2, 1); return 1;
        case IMP_USER32_DLL_LoadIconA:
        case IMP_USER32_DLL_LoadCursorA:
            stdcall_ret(c, 2, 0x00020001u); return 1;
        case IMP_USER32_DLL_ShowCursor: {
            int show = arg_i32(c, 0);
            g_os_cursor_hidden = show ? 0 : 1;
            if (show) gl_renderer_clear_cursor();
            gl_renderer_set_live_mouse(g_mouse_fx, g_mouse_fy, g_os_cursor_hidden);
            SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
            stdcall_ret(c, 1, show ? 0u : 0xFFFFFFFFu); return 1;
        }
        case IMP_USER32_DLL_SetCursorPos:
            g_mouse_x = arg_i32(c, 0);
            g_mouse_y = arg_i32(c, 1);
            g_mouse_fx = (float)g_mouse_x;
            g_mouse_fy = (float)g_mouse_y;
            gl_renderer_set_live_mouse(g_mouse_fx, g_mouse_fy, g_os_cursor_hidden);
            stdcall_ret(c, 2, 1); return 1;
        case IMP_USER32_DLL_GetSystemMetrics: {
            int idx = arg_i32(c, 0);
            uint32_t val = 0;
            if (idx == 0) val = (uint32_t)g_win_w;
            else if (idx == 1) val = (uint32_t)g_win_h;
            stdcall_ret(c, 1, val); return 1;
        }
        case IMP_USER32_DLL_GetDC:
            stdcall_ret(c, 1, gdi_create_surface_dc(0)); return 1;
        case IMP_USER32_DLL_ReleaseDC:
            gdi_release_surface_dc(arg_u32(c, 1));
            stdcall_ret(c, 2, 1); return 1;
        case IMP_USER32_DLL_PostQuitMessage:
            g_quit_requested = 1;
            push_msg(g_main_hwnd, 0x0012u, arg_u32(c, 0), 0);
            stdcall_ret(c, 1, 0); return 1;
        case IMP_USER32_DLL_PeekMessageA: {
            win32_pump_sdl_events();
            uint32_t msg_va = arg_u32(c, 0);
            uint32_t remove = arg_u32(c, 4) & 1u;
            WinMsg m;
            if (pop_msg(&m, (int)remove)) {
                mem_w32(msg_va + 0, m.hwnd);
                mem_w32(msg_va + 4, m.msg);
                mem_w32(msg_va + 8, m.wparam);
                mem_w32(msg_va + 12, m.lparam);
                mem_w32(msg_va + 16, m.time);
                mem_w32(msg_va + 20, (uint32_t)m.pt_x);
                mem_w32(msg_va + 24, (uint32_t)m.pt_y);
                stdcall_ret(c, 5, 1); return 1;
            }
            stdcall_ret(c, 5, 0); return 1;
        }
        case IMP_USER32_DLL_TranslateMessage:
            stdcall_ret(c, 1, 1); return 1;
        case IMP_USER32_DLL_DispatchMessageA: {
            uint32_t msg_va = arg_u32(c, 0);
            uint32_t ret = 0;
            if (msg_va && g_wndproc_va) {
                uint32_t a[4] = {
                    mem_r32(msg_va + 0),
                    mem_r32(msg_va + 4),
                    mem_r32(msg_va + 8),
                    mem_r32(msg_va + 12),
                };
                ret = guest_call_stdcall(c, g_wndproc_va, 4, a);
            }
            stdcall_ret(c, 1, ret); return 1;
        }
        case IMP_USER32_DLL_SendMessageA: {
            uint32_t hwnd = arg_u32(c, 0);
            uint32_t msg  = arg_u32(c, 1);
            uint32_t wp   = arg_u32(c, 2);
            uint32_t lp   = arg_u32(c, 3);
            uint32_t ret = 0;
            if (hwnd == g_main_hwnd && g_wndproc_va) {
                uint32_t a[4] = { hwnd, msg, wp, lp };
                ret = guest_call_stdcall(c, g_wndproc_va, 4, a);
            }
            stdcall_ret(c, 4, ret); return 1;
        }
        case IMP_USER32_DLL_DefWindowProcA: {
            uint32_t msg = arg_u32(c, 1);
            if (msg == 0x0010u /* WM_CLOSE */) {
                g_quit_requested = 1;
                push_msg(g_main_hwnd, 0x0012u, 0, 0);
            }
            stdcall_ret(c, 4, 0); return 1;
        }
        case IMP_USER32_DLL_MessageBoxA: {
            const char *text = arg_cstr(c, 1);
            const char *cap  = arg_cstr(c, 2);
            fprintf(stderr, "[MessageBoxA] %s: %s\n", cap ? cap : "", text ? text : "");
            stdcall_ret(c, 4, 1u /* IDOK */); return 1;
        }

        /* ---- GDI32 ---- */
        case IMP_GDI32_DLL_GetStockObject:
            stdcall_ret(c, 1, 0x4000u + arg_u32(c, 0)); return 1;
        case IMP_GDI32_DLL_CreateCompatibleDC:
            stdcall_ret(c, 1, gdi_create_surface_dc(0)); return 1;
        case IMP_GDI32_DLL_DeleteDC:
            gdi_release_surface_dc(arg_u32(c, 0));
            stdcall_ret(c, 1, 1); return 1;
        case IMP_GDI32_DLL_DeleteObject: {
            uint32_t h = arg_u32(c, 0);
            if (h >= 0x2001 && h < 0x2000 + MAX_GDI_FONTS) {
                GdiFont *f = &g_fonts[h - 0x2000];
                if (f->in_use) {
                    if (f->ttf) TTF_CloseFont(f->ttf);
                    f->ttf = NULL;
                    f->in_use = 0;
                }
            }
            stdcall_ret(c, 1, 1); return 1;
        }
        case IMP_GDI32_DLL_SelectObject: {
            GdiDC *dc = get_dc(arg_u32(c, 0));
            uint32_t obj = arg_u32(c, 1);
            uint32_t prev = dc->font_handle;
            if (obj >= 0x2000 && obj < 0x2000 + MAX_GDI_FONTS) {
                dc->font_handle = obj;
            }
            stdcall_ret(c, 2, prev); return 1;
        }
        case IMP_GDI32_DLL_SetTextColor: {
            GdiDC *dc = get_dc(arg_u32(c, 0));
            uint32_t prev = dc->text_color;
            dc->text_color = arg_u32(c, 1);
            stdcall_ret(c, 2, prev); return 1;
        }
        case IMP_GDI32_DLL_GetTextColor:
            stdcall_ret(c, 1, get_dc(arg_u32(c, 0))->text_color); return 1;
        case IMP_GDI32_DLL_SetBkColor: {
            GdiDC *dc = get_dc(arg_u32(c, 0));
            uint32_t prev = dc->bk_color;
            dc->bk_color = arg_u32(c, 1);
            stdcall_ret(c, 2, prev); return 1;
        }
        case IMP_GDI32_DLL_GetBkColor:
            stdcall_ret(c, 1, get_dc(arg_u32(c, 0))->bk_color); return 1;
        case IMP_GDI32_DLL_SetBkMode: {
            GdiDC *dc = get_dc(arg_u32(c, 0));
            uint32_t prev = dc->bk_mode;
            dc->bk_mode = arg_u32(c, 1);
            stdcall_ret(c, 2, prev); return 1;
        }
        case IMP_GDI32_DLL_GetBkMode:
            stdcall_ret(c, 1, get_dc(arg_u32(c, 0))->bk_mode); return 1;

        case IMP_GDI32_DLL_EnumFontFamiliesA: {
            uint32_t cb_va = arg_u32(c, 2);
            uint32_t lparam = arg_u32(c, 3);
            static const char *font_names[] = {
                "Arial", "Times New Roman", "Courier New", "Verdana",
                "Tahoma", "Comic Sans MS", "MS Sans Serif", "Trebuchet MS", NULL
            };
            uint32_t elf_va = guest_alloc(128);
            uint32_t ntm_va = guest_alloc(128);
            int res = 1;
            for (int i = 0; font_names[i] && cb_va; i++) {
                memset(g_mem + elf_va, 0, 128);
                memset(g_mem + ntm_va, 0, 128);
                mem_w32(elf_va + 0, 16); /* lfHeight */
                mem_w32(elf_va + 16, 400); /* lfWeight */
                snprintf((char *)(g_mem + elf_va + 28), 32, "%s", font_names[i]);
                mem_w32(ntm_va + 0, 16); /* tmHeight */
                mem_w32(ntm_va + 4, 13); /* tmAscent */
                mem_w32(ntm_va + 8, 3);  /* tmDescent */
                mem_w32(ntm_va + 20, 8); /* tmAveCharWidth */
                mem_w32(ntm_va + 24, 14);/* tmMaxCharWidth */
                uint32_t a[4] = { elf_va, ntm_va, 4u /* TRUETYPE_FONTTYPE */, lparam };
                res = (int)guest_call_stdcall(c, cb_va, 4, a);
                if (res == 0) break;
            }
            guest_free(elf_va);
            guest_free(ntm_va);
            stdcall_ret(c, 4, (uint32_t)res); return 1;
        }
        case IMP_GDI32_DLL_CreateFontIndirectA: {
            uint32_t lf_va = arg_u32(c, 0);
            int32_t lf_h = lf_va ? (int32_t)mem_r32(lf_va + 0) : 16;
            int32_t lf_w = lf_va ? (int32_t)mem_r32(lf_va + 16) : 400;
            const char *face = lf_va ? (const char *)(g_mem + lf_va + 28) : "Arial";
            int ptsz = lf_h < 0 ? -lf_h : (lf_h > 0 ? lf_h : 16);
            if (ptsz < 6) ptsz = 6;
            if (ptsz > 128) ptsz = 128;
            /* Scale slightly to match Windows GDI pixel height */
            int ttf_sz = (ptsz * 85) / 100;
            if (ttf_sz < 6) ttf_sz = 6;

            for (int i = 1; i < MAX_GDI_FONTS; i++) {
                if (!g_fonts[i].in_use) {
                    g_fonts[i].in_use = 1;
                    g_fonts[i].height = ptsz;
                    g_fonts[i].weight = lf_w;
                    snprintf(g_fonts[i].face, sizeof(g_fonts[i].face), "%s", face);
                    g_fonts[i].ttf = g_ttf_path[0] ? TTF_OpenFont(g_ttf_path, ttf_sz) : NULL;
                    if (g_fonts[i].ttf && lf_w >= 600) {
                        TTF_SetFontStyle(g_fonts[i].ttf, TTF_STYLE_BOLD);
                    }
                    stdcall_ret(c, 1, 0x2000u + (uint32_t)i); return 1;
                }
            }
            stdcall_ret(c, 1, 0); return 1;
        }
        case IMP_GDI32_DLL_GetTextMetricsA: {
            GdiDC *dc = get_dc(arg_u32(c, 0));
            uint32_t tm_va = arg_u32(c, 1);
            TTF_Font *f = get_dc_font(dc);
            if (tm_va) {
                memset(g_mem + tm_va, 0, 56);
                int h = f ? TTF_FontHeight(f) : 16;
                int asc = f ? TTF_FontAscent(f) : 13;
                int desc = f ? -TTF_FontDescent(f) : 3;
                if (desc < 0) desc = 0;
                int w = 8;
                if (f) TTF_SizeUTF8(f, "n", &w, NULL);
                mem_w32(tm_va + 0, (uint32_t)h);
                mem_w32(tm_va + 4, (uint32_t)asc);
                mem_w32(tm_va + 8, (uint32_t)desc);
                mem_w32(tm_va + 20, (uint32_t)w);
                mem_w32(tm_va + 24, (uint32_t)(w * 2));
                mem_w32(tm_va + 28, 400);
                mem_w8(tm_va + 48, 32);
                mem_w8(tm_va + 49, 255);
                mem_w8(tm_va + 50, '?');
                mem_w8(tm_va + 51, ' ');
            }
            stdcall_ret(c, 2, 1); return 1;
        }
        case IMP_GDI32_DLL_GetTextExtentPoint32A: {
            GdiDC *dc = get_dc(arg_u32(c, 0));
            uint32_t str_va = arg_u32(c, 1);
            int32_t len = arg_i32(c, 2);
            uint32_t sz_va = arg_u32(c, 3);
            TTF_Font *f = get_dc_font(dc);
            char u8[1024];
            if (len < 0) len = 0;
            if (len > 500) len = 500;
            cp1252_to_utf8(g_mem + str_va, len, u8, sizeof(u8));
            int w = len * 8, h = 16;
            if (f) {
                if (u8[0]) TTF_SizeUTF8(f, u8, &w, &h);
                else { w = 0; h = TTF_FontHeight(f); }
            }
            if (sz_va) {
                mem_w32(sz_va + 0, (uint32_t)w);
                mem_w32(sz_va + 4, (uint32_t)h);
            }
            stdcall_ret(c, 4, 1); return 1;
        }
        case IMP_GDI32_DLL_TextOutA: {
            GdiDC *dc = get_dc(arg_u32(c, 0));
            int32_t x = arg_i32(c, 1);
            int32_t y = arg_i32(c, 2);
            uint32_t str_va = arg_u32(c, 3);
            int32_t len = arg_i32(c, 4);
            TTF_Font *f = get_dc_font(dc);
            DDrawSurface *ds = dc->surface_handle ? ddraw_get_surface(dc->surface_handle) : NULL;
            if (f && ds && ds->pixels_va && len > 0) {
                char u8[1024];
                if (len > 500) len = 500;
                cp1252_to_utf8(g_mem + str_va, len, u8, sizeof(u8));
                if (u8[0]) {
                    /* Record vector text draw for Native Monitor-DPI OpenGL rendering (F5) */
                    GdiFont *gf = (dc->font_handle >= 0x2001 && dc->font_handle < 0x2000 + MAX_GDI_FONTS &&
                                   g_fonts[dc->font_handle - 0x2000].in_use)
                        ? &g_fonts[dc->font_handle - 0x2000] : NULL;
                    GlTextDraw td;
                    memset(&td, 0, sizeof(td));
                    td.x = x;
                    td.y = y;
                    td.clip_x0 = 0;
                    td.clip_y0 = 0;
                    td.clip_x1 = (int)ds->width;
                    td.clip_y1 = (int)ds->height;
                    td.font_height = gf ? gf->height : 16;
                    td.font_weight = gf ? gf->weight : 400;
                    td.text_color = dc->text_color;
                    td.bk_color = dc->bk_color;
                    td.bk_mode = (int)dc->bk_mode;
                    snprintf(td.utf8, sizeof(td.utf8), "%.255s", u8);
                    ddraw_surface_add_text(ds, &td);

                    /* If Native HD Vector Text is active, skip blurry 800x600 software rasterization */
                    if (g_cfg.hd_vector_text && gl_renderer_is_active()) {
                        stdcall_ret(c, 5, 1); return 1;
                    }

                    SDL_Color fg = {
                        (uint8_t)(dc->text_color & 0xFF),
                        (uint8_t)((dc->text_color >> 8) & 0xFF),
                        (uint8_t)((dc->text_color >> 16) & 0xFF),
                        255
                    };
                    SDL_Surface *ts = TTF_RenderUTF8_Blended(f, u8, fg);
                    if (ts) {
                        SDL_Surface *rgba = SDL_ConvertSurfaceFormat(ts, SDL_PIXELFORMAT_ARGB8888, 0);
                        SDL_FreeSurface(ts);
                        if (rgba) {
                            uint8_t bg_r = (uint8_t)(dc->bk_color & 0xFF);
                            uint8_t bg_g = (uint8_t)((dc->bk_color >> 8) & 0xFF);
                            uint8_t bg_b = (uint8_t)((dc->bk_color >> 16) & 0xFF);
                            for (int sy = 0; sy < rgba->h; sy++) {
                                int dy = y + sy;
                                if (dy < 0 || dy >= (int)ds->height) continue;
                                const uint32_t *srow = (const uint32_t *)((const uint8_t *)rgba->pixels + sy * rgba->pitch);
                                uint8_t *drow = g_mem + ds->pixels_va + (uint32_t)dy * ds->pitch;
                                for (int sx = 0; sx < rgba->w; sx++) {
                                    int dx = x + sx;
                                    if (dx < 0 || dx >= (int)ds->width) continue;
                                    uint32_t px = srow[sx];
                                    uint32_t a = px >> 24;
                                    uint32_t sr = (px >> 16) & 0xFF;
                                    uint32_t sg = (px >> 8) & 0xFF;
                                    uint32_t sb = px & 0xFF;
                                    if (dc->bk_mode == 2 /* OPAQUE */) {
                                        sr = (sr * a + bg_r * (255 - a)) / 255;
                                        sg = (sg * a + bg_g * (255 - a)) / 255;
                                        sb = (sb * a + bg_b * (255 - a)) / 255;
                                        a = 255;
                                    }
                                    if (a == 0) continue;
                                    if (ds->bpp == 16) {
                                        uint16_t dp;
                                        memcpy(&dp, drow + dx * 2, 2);
                                        uint32_t dr = ((dp >> 11) & 0x1F) * 255 / 31;
                                        uint32_t dg = ((dp >> 5) & 0x3F) * 255 / 63;
                                        uint32_t db = (dp & 0x1F) * 255 / 31;
                                        uint32_t rr = (sr * a + dr * (255 - a)) / 255;
                                        uint32_t rg = (sg * a + dg * (255 - a)) / 255;
                                        uint32_t rb = (sb * a + db * (255 - a)) / 255;
                                        uint16_t out = (uint16_t)(((rr >> 3) << 11) | ((rg >> 2) << 5) | (rb >> 3));
                                        memcpy(drow + dx * 2, &out, 2);
                                    } else if (ds->bpp == 32) {
                                        uint32_t dp;
                                        memcpy(&dp, drow + dx * 4, 4);
                                        uint32_t dr = (dp >> 16) & 0xFF;
                                        uint32_t dg = (dp >> 8) & 0xFF;
                                        uint32_t db = dp & 0xFF;
                                        uint32_t rr = (sr * a + dr * (255 - a)) / 255;
                                        uint32_t rg = (sg * a + dg * (255 - a)) / 255;
                                        uint32_t rb = (sb * a + db * (255 - a)) / 255;
                                        uint32_t out = 0xFF000000u | (rr << 16) | (rg << 8) | rb;
                                        memcpy(drow + dx * 4, &out, 4);
                                    }
                                }
                            }
                            SDL_FreeSurface(rgba);
                        }
                    }
                }
            }
            stdcall_ret(c, 5, 1); return 1;
        }
    }
    return 0;
}
