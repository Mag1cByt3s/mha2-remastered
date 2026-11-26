/*
 * Input record / replay engine (--record <file>, --replay <file>, --replay-out <dir>).
 * Script format: docs/REPLAY.md.
 */
#include "runtime.h"
#include "gl_renderer.h"
#include "hd_assets.h"
#include "input_replay.h"
#include "input_bindings.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_WAIT_MS      90000u
#define CLICK_HOLD_MS        120u
#define TAP_HOLD_MS          80u
#define HOVER_SETTLE_MS      250u  /* the game only accepts clicks on an already-hovered target */
#define REC_MOVE_INTERVAL_MS 30u
#define QUICK_SLOT           9    /* dedicated quick save slot (mha9.sav) */

typedef enum {
    RE_KEY, RE_TEXT, RE_MOVE, RE_BUTTON, RE_SHOT, RE_HASH,
    RE_EXPECT_ROOM, RE_EXPECT_MENU, RE_QUIT, RE_WAIT_ROOM, RE_WAIT_MENU, RE_WAIT_VIDEO, RE_LOG, RE_HOTSPOTS, RE_CLICKSPOT, RE_SHOTFULL, RE_EXPECT_HOTSPOTS, RE_WAIT_LEAVE, RE_SAVECHECK, RE_TOAST, RE_QUICKSAVE, RE_QUICKLOAD, RE_PAD, RE_WAIT_INTERACTIVE,
} ReplayKind;

typedef struct {
    ReplayKind kind;
    uint32_t ms;          /* due time relative to the current sync segment */
    uint32_t timeout_ms;  /* waits */
    int line;
    int a, b;             /* key: sym/down; button: index/down */
    float x, y;
    char s[96];
} ReplayEvent;

static ReplayEvent *g_ev;
static int g_ev_count, g_ev_cap, g_ev_pos;
static int g_replaying;
static uint32_t g_seg_start;      /* SDL ticks when the current segment began */
static uint32_t g_wait_start;
static int g_wait_armed;
static uint16_t g_inj_mod;
static char g_out_dir[512] = "screenshots/replay";

static FILE *g_rec;
static uint32_t g_rec_seg_start, g_rec_last_move;
static char g_rec_room[64];
static int g_rec_menu, g_rec_video;
static float g_rec_mx = -1.0f, g_rec_my = -1.0f;

/* ---------------- helpers ---------------- */

static void norm_room(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src);
    size_t n = strlen(dst);
    if (n > 0 && n + 1 < cap && dst[n - 1] != '/') { dst[n] = '/'; dst[n + 1] = 0; }
}

static void mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *q = tmp + 1; *q; q++) {
        if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
    }
    mkdir(tmp, 0755);
}

static const char *cur_room(void) {
    const char *r = hd_sprites_room_prefix();
    return r ? r : "";
}

/* Main menu: exact scene-graph detector, or the main-menu background as the badge
 * renderer uses it (the detector misses some idle menu states). */
static int menu_active(void) {
    return game_is_main_menu_active() || strcmp(cur_room(), "x/001/") == 0;
}

static ReplayEvent *push_ev(ReplayKind k, uint32_t ms, int line) {
    if (g_ev_count == g_ev_cap) {
        int ncap = g_ev_cap ? g_ev_cap * 2 : 256;
        ReplayEvent *n = (ReplayEvent *)realloc(g_ev, (size_t)ncap * sizeof(*n));
        if (!n) return NULL;
        g_ev = n;
        g_ev_cap = ncap;
    }
    ReplayEvent *e = &g_ev[g_ev_count++];
    memset(e, 0, sizeof(*e));
    e->kind = k;
    e->ms = ms;
    e->line = line;
    return e;
}

static int parse_button(const char *s) {
    if (!strcmp(s, "left")) return 0;
    if (!strcmp(s, "right")) return 1;
    if (!strcmp(s, "middle")) return 2;
    return -1;
}

static void inject_key(SDL_Keycode k, int down);
void input_replay_quick_save(void);
void input_replay_quick_load(void);
static void quick_slot_claim(void);

/* Built-in macros (quick save/load) run on the replay engine but must never exit the
 * game: a failing step aborts the macro, restores input and shows a toast instead. */
static int g_macro;
static int g_pending_macro; /* 1 = quick save, 2 = quick load, started after the current event */
static char g_macro_fail_toast[96];
static char g_macro_origin_room[64];
static time_t g_macro_save_mtime;
static float g_macro_cursor_x = 400.0f, g_macro_cursor_y = 300.0f;

/* A macro started while a --replay script runs (e.g. an injected Ctrl+S) stashes the
 * script and resumes it afterwards with a fresh segment clock. */
static struct {
    int active;
    ReplayEvent *ev;
    int count, cap, pos, replaying;
    uint16_t mod;
} g_stash;

static void reset_events(void) {
    g_ev_count = g_ev_pos = 0;
    g_replaying = 0;
    g_wait_armed = 0;
    g_seg_start = 0;
}

static void macro_end(void) {
    g_macro = 0;
    reset_events();
    if (g_stash.active) {
        free(g_ev);
        g_ev = g_stash.ev;
        g_ev_count = g_stash.count;
        g_ev_cap = g_stash.cap;
        g_ev_pos = g_stash.pos;
        g_replaying = g_stash.replaying;
        g_inj_mod = g_stash.mod;
        g_stash.active = 0;
        g_seg_start = 0; /* restart the script's segment clock at the next poll */
    }
}

static void replay_fail(const ReplayEvent *e, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (g_macro) {
        fprintf(stderr, "[macro] aborted: %s\n", msg);
        gl_renderer_show_toast("%s", g_macro_fail_toast);
        /* left stranded in the save screen: return to the game */
        if (strcmp(cur_room(), "x/006/") == 0 && strncmp(g_macro_origin_room, "l/", 2) == 0) {
            inject_key(SDLK_ESCAPE, 1);
            inject_key(SDLK_ESCAPE, 0);
        }
        macro_end();
        return;
    }
    fprintf(stderr, "[replay] FAIL (script line %d): %s\n", e ? e->line : 0, msg);
    fflush(stderr);
    _exit(3);
}

/* ---------------- script parsing ---------------- */

static int parse_script_stream(FILE *f, const char *path) {
    char line[512];
    int ln = 0;
    uint32_t prev = 0;
    while (fgets(line, sizeof(line), f)) {
        ln++;
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        char *end = p + strlen(p);
        while (end > p && isspace((unsigned char)end[-1])) *--end = 0;
        if (!*p || *p == '#') continue;

        if (!strncmp(p, "wait ", 5)) {
            char what[16] = "", arg[64] = "";
            float tmo = 0.0f;
            int n = sscanf(p + 5, "%15s %63s %f", what, arg, &tmo);
            ReplayEvent *e = NULL;
            if (n >= 1 && (!strcmp(what, "menu") || !strcmp(what, "video") || !strcmp(what, "interactive"))) {
                e = push_ev(!strcmp(what, "menu") ? RE_WAIT_MENU : !strcmp(what, "video") ? RE_WAIT_VIDEO
                                                                 : RE_WAIT_INTERACTIVE, 0, ln);
                if (e && n >= 2) e->timeout_ms = (uint32_t)(atof(arg) * 1000.0);
            } else if (n >= 2 && (!strcmp(what, "room") || !strcmp(what, "leave"))) {
                e = push_ev(!strcmp(what, "room") ? RE_WAIT_ROOM : RE_WAIT_LEAVE, 0, ln);
                if (e) { norm_room(e->s, sizeof(e->s), arg); if (n >= 3) e->timeout_ms = (uint32_t)(tmo * 1000.0f); }
            }
            if (!e) { fprintf(stderr, "[replay] %s:%d: bad wait: %s\n", path, ln, p); return -1; }
            if (!e->timeout_ms) e->timeout_ms = DEFAULT_WAIT_MS;
            prev = 0;
            continue;
        }

        /* "<ms> cmd ..." absolute within segment, or "+<ms> cmd ..." relative to previous */
        int rel = (*p == '+');
        char *num_end = NULL;
        unsigned long t = strtoul(p + rel, &num_end, 10);
        if (num_end == p + rel || !isspace((unsigned char)*num_end)) {
            fprintf(stderr, "[replay] %s:%d: expected time: %s\n", path, ln, p);
            return -1;
        }
        uint32_t ms = rel ? prev + (uint32_t)t : (uint32_t)t;
        char *cmd = num_end;
        while (isspace((unsigned char)*cmd)) cmd++;
        char *args = cmd;
        while (*args && !isspace((unsigned char)*args)) args++;
        if (*args) *args++ = 0;
        while (isspace((unsigned char)*args)) args++;

        int ok = 1;
        ReplayEvent *e;
        if (!strcmp(cmd, "key") || !strcmp(cmd, "tap")) {
            int is_tap = !strcmp(cmd, "tap");
            int down = 1;
            const char *name = args;
            if (!is_tap) {
                if (!strncmp(args, "down ", 5)) name = args + 5;
                else if (!strncmp(args, "up ", 3)) { name = args + 3; down = 0; }
                else ok = 0;
            }
            SDL_Keycode k = ok ? SDL_GetKeyFromName(name) : SDLK_UNKNOWN;
            if (k == SDLK_UNKNOWN) ok = 0;
            if (ok && (e = push_ev(RE_KEY, ms, ln))) { e->a = (int)k; e->b = is_tap ? 1 : down; }
            if (ok && is_tap && (e = push_ev(RE_KEY, ms + TAP_HOLD_MS, ln))) { e->a = (int)k; e->b = 0; ms += TAP_HOLD_MS; }
        } else if (!strcmp(cmd, "pad") || !strcmp(cmd, "padtap")) {
            /* controller button: "pad dpup down", "padtap x" (SDL GameController names) */
            int is_tap = !strcmp(cmd, "padtap");
            char bn[32] = "", st[8] = "down";
            int n = sscanf(args, "%31s %7s", bn, st);
            int b = n >= 1 ? (int)SDL_GameControllerGetButtonFromString(bn) : -1;
            ok = b >= 0 && (is_tap || n == 2) && (!strcmp(st, "down") || !strcmp(st, "up"));
            if (ok && (e = push_ev(RE_PAD, ms, ln))) { e->a = b; e->b = is_tap ? 1 : !strcmp(st, "down"); }
            if (ok && is_tap && (e = push_ev(RE_PAD, ms + TAP_HOLD_MS, ln))) { e->a = b; e->b = 0; ms += TAP_HOLD_MS; }
        } else if (!strcmp(cmd, "text")) {
            if ((e = push_ev(RE_TEXT, ms, ln))) snprintf(e->s, sizeof(e->s), "%s", args);
        } else if (!strcmp(cmd, "move")) {
            float x, y;
            ok = sscanf(args, "%f %f", &x, &y) == 2;
            if (ok && (e = push_ev(RE_MOVE, ms, ln))) { e->x = x; e->y = y; }
        } else if (!strcmp(cmd, "button") || !strcmp(cmd, "click")) {
            char b1[16] = "", b2[16] = "";
            float x = -1.0f, y = -1.0f;
            int down = 1, btn;
            if (!strcmp(cmd, "button")) {
                int n = sscanf(args, "%15s %15s %f %f", b1, b2, &x, &y);
                btn = parse_button(b1);
                down = !strcmp(b2, "down");
                ok = n >= 2 && btn >= 0 && (down || !strcmp(b2, "up")) && (n == 2 || n == 4);
                if (ok && (e = push_ev(RE_BUTTON, ms, ln))) { e->a = btn; e->b = down; e->x = x; e->y = y; }
            } else {
                int n = sscanf(args, "%f %f %15s", &x, &y, b1);
                btn = n >= 3 ? parse_button(b1) : 0;
                ok = n >= 2 && btn >= 0;
                if (ok && (e = push_ev(RE_MOVE, ms, ln))) { e->x = x; e->y = y; }
                if (ok && (e = push_ev(RE_BUTTON, ms + HOVER_SETTLE_MS, ln))) { e->a = btn; e->b = 1; e->x = x; e->y = y; }
                if (ok && (e = push_ev(RE_BUTTON, ms + HOVER_SETTLE_MS + CLICK_HOLD_MS, ln))) { e->a = btn; e->b = 0; e->x = x; e->y = y; }
                ms += HOVER_SETTLE_MS + CLICK_HOLD_MS;
            }
        } else if (!strcmp(cmd, "shot") || !strcmp(cmd, "shotfull") || !strcmp(cmd, "hash") || !strcmp(cmd, "log")) {
            ReplayKind k = !strcmp(cmd, "shot") ? RE_SHOT : !strcmp(cmd, "shotfull") ? RE_SHOTFULL :
                           !strcmp(cmd, "hash") ? RE_HASH : RE_LOG;
            ok = *args != 0;
            if (ok && (e = push_ev(k, ms, ln))) snprintf(e->s, sizeof(e->s), "%s", args);
        } else if (!strcmp(cmd, "hotspots")) {
            push_ev(RE_HOTSPOTS, ms, ln);
        } else if (!strcmp(cmd, "clickspot")) {
            ok = *args != 0;
            /* phase 0: locate + hover, 1: press (after hover settles), 2: release */
            for (int ph = 0; ok && ph < 3; ph++) {
                uint32_t at = ms + (ph >= 1 ? HOVER_SETTLE_MS : 0u) + (ph == 2 ? CLICK_HOLD_MS : 0u);
                if ((e = push_ev(RE_CLICKSPOT, at, ln))) { snprintf(e->s, sizeof(e->s), "%s", args); e->b = ph; }
            }
            ms += HOVER_SETTLE_MS + CLICK_HOLD_MS;
        } else if (!strcmp(cmd, "expect")) {
            if (!strncmp(args, "room ", 5)) {
                if ((e = push_ev(RE_EXPECT_ROOM, ms, ln))) norm_room(e->s, sizeof(e->s), args + 5);
            } else if (!strcmp(args, "menu")) {
                push_ev(RE_EXPECT_MENU, ms, ln);
            } else if (!strncmp(args, "hotspots ", 9)) {
                if ((e = push_ev(RE_EXPECT_HOTSPOTS, ms, ln))) e->a = atoi(args + 9);
            } else {
                ok = 0;
            }
        } else if (!strcmp(cmd, "quicksave") || !strcmp(cmd, "quickload")) {
            push_ev(!strcmp(cmd, "quicksave") ? RE_QUICKSAVE : RE_QUICKLOAD, ms, ln);
        } else if (!strcmp(cmd, "toast")) {
            ok = *args != 0;
            if (ok && (e = push_ev(RE_TOAST, ms, ln))) snprintf(e->s, sizeof(e->s), "%s", args);
        } else if (!strcmp(cmd, "savecheck")) {
            ok = atoi(args) >= 1 && atoi(args) <= 9;
            if (ok && (e = push_ev(RE_SAVECHECK, ms, ln))) e->a = atoi(args);
        } else if (!strcmp(cmd, "quit")) {
            push_ev(RE_QUIT, ms, ln);
        } else {
            ok = 0;
        }
        if (!ok) {
            fprintf(stderr, "[replay] %s:%d: cannot parse: %s %s\n", path, ln, cmd, args);
            return -1;
        }
        prev = ms;
    }
    return 0;
}

static int parse_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    int rc = parse_script_stream(f, path);
    fclose(f);
    return rc;
}

/* ---------------- injection ---------------- */

/* Injected key/text events carry the real window ID (SDL3/sdl2-compat dereferences the
 * window of pushed text events), so they are recognized by a FIFO of signatures
 * instead; SDL keeps pushed events in order. */
#define INJ_SIG_CAP 256u
typedef struct { uint32_t type; int32_t key; uint32_t text_hash; } InjSig;
static InjSig g_inj_sig[INJ_SIG_CAP];
static uint32_t g_inj_head, g_inj_tail;

static InjSig inj_sig_of(const SDL_Event *ev) {
    InjSig s = { ev->type, 0, 0 };
    if (ev->type == SDL_KEYDOWN || ev->type == SDL_KEYUP) {
        s.key = ev->key.keysym.sym;
    } else if (ev->type == SDL_CONTROLLERBUTTONDOWN || ev->type == SDL_CONTROLLERBUTTONUP) {
        s.key = ev->cbutton.button;
    } else if (ev->type == SDL_TEXTINPUT) {
        uint32_t h = 2166136261u;
        for (const char *p = ev->text.text; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        s.text_hash = h;
    }
    return s;
}

static void inj_push_sig(const SDL_Event *ev) {
    if (g_inj_tail - g_inj_head >= INJ_SIG_CAP) g_inj_head++; /* drop oldest, never block */
    g_inj_sig[g_inj_tail++ % INJ_SIG_CAP] = inj_sig_of(ev);
}

static void inject_key(SDL_Keycode k, int down) {
    uint16_t bit = 0;
    switch (k) {
    case SDLK_LCTRL: bit = KMOD_LCTRL; break;
    case SDLK_RCTRL: bit = KMOD_RCTRL; break;
    case SDLK_LSHIFT: bit = KMOD_LSHIFT; break;
    case SDLK_RSHIFT: bit = KMOD_RSHIFT; break;
    case SDLK_LALT: bit = KMOD_LALT; break;
    case SDLK_RALT: bit = KMOD_RALT; break;
    default: break;
    }
    if (down) g_inj_mod |= bit;
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.windowID = gl_renderer_window_id();
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.sym = k;
    e.key.keysym.scancode = SDL_GetScancodeFromKey(k);
    e.key.keysym.mod = g_inj_mod;
    inj_push_sig(&e);
    SDL_PushEvent(&e);
    if (!down) g_inj_mod &= (uint16_t)~bit;
}

static void inject_text(const char *s) {
    /* Direct WM_CHAR path: sdl2-compat cannot push SDL_TEXTINPUT events (SDL3 text
     * events hold a pointer), it crashes inside SDL_PushEvent. */
    win32_input_text(s);
    if (g_rec) fprintf(g_rec, "%u text %s\n", SDL_GetTicks() - g_rec_seg_start, s);
}

/* Move like a real mouse: the game drops implausibly large single DirectInput deltas
 * (a ~350px jump is ignored), so travel in <= 48px steps from the current position. */
#define MOVE_STEP_PX 48.0f
static void inject_move_to(float x, float y) {
    float cx, cy;
    win32_input_mouse_pos(&cx, &cy);
    float dx = x - cx, dy = y - cy;
    float dist = fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy);
    int steps = (int)ceilf(dist / MOVE_STEP_PX);
    if (steps < 1) steps = 1;
    for (int i = 1; i <= steps; i++)
        win32_input_mouse_move(cx + dx * (float)i / (float)steps, cy + dy * (float)i / (float)steps);
}

static void *quit_watchdog(void *arg) {
    (void)arg;
    sleep(20);
    fprintf(stderr, "[replay] quit watchdog: process did not exit within 20s, forcing exit\n");
    _exit(0);
    return NULL;
}

static void exec_event(const ReplayEvent *e, uint32_t t) {
    switch (e->kind) {
    case RE_KEY:
        inject_key((SDL_Keycode)e->a, e->b);
        break;
    case RE_PAD: {
        SDL_Event pe;
        memset(&pe, 0, sizeof(pe));
        pe.type = e->b ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
        pe.cbutton.button = (Uint8)e->a;
        pe.cbutton.state = e->b ? SDL_PRESSED : SDL_RELEASED;
        inj_push_sig(&pe);
        SDL_PushEvent(&pe);
        break;
    }
    case RE_TEXT:
        inject_text(e->s);
        break;
    case RE_MOVE:
        inject_move_to(e->x, e->y);
        break;
    case RE_BUTTON:
        if (e->x >= 0.0f) inject_move_to(e->x, e->y);
        win32_input_mouse_button(e->x >= 0.0f ? e->x : -1.0f, e->y, e->a, e->b);
        break;
    case RE_SHOT:
    case RE_SHOTFULL: {
        char path[700];
        mkdir_p(g_out_dir);
        snprintf(path, sizeof(path), "%s/%s.png", g_out_dir, e->s);
        gl_renderer_request_screenshot_to(path, e->kind == RE_SHOTFULL);
        fprintf(stderr, "[replay] %s %s -> %s (room=%s)\n", e->kind == RE_SHOTFULL ? "shotfull" : "shot",
                e->s, path, cur_room());
        break;
    }
    case RE_HASH: {
        uint32_t w, h;
        uint64_t hv = ddraw_last_frame_hash(&w, &h);
        fprintf(stderr, "[replay] hash %s %016llx %ux%u room=%s\n", e->s, (unsigned long long)hv, w, h, cur_room());
        break;
    }
    case RE_LOG:
        fprintf(stderr, "[replay] log %s (room=%s menu=%d detector=%d fps=%.0f)\n", e->s, cur_room(), menu_active(),
                game_is_main_menu_active(), (double)gl_renderer_current_fps());
        break;
    case RE_HOTSPOTS: {
        static const char *kinds[] = { "object", "exit", "npc" };
        GameHotspot hs[64];
        int n = game_collect_room_hotspots(hs, 64);
        fprintf(stderr, "[replay] hotspots room=%s count=%d\n", cur_room(), n);
        for (int i = 0; i < n; i++)
            fprintf(stderr, "[replay]   %-6s at %3d %3d  box %3d,%3d-%3d,%3d  \"%s\"\n",
                    hs[i].kind >= 0 && hs[i].kind <= 2 ? kinds[hs[i].kind] : "?", hs[i].x, hs[i].y,
                    hs[i].x0, hs[i].y0, hs[i].x1, hs[i].y1, hs[i].label);
        break;
    }
    case RE_CLICKSPOT: {
        /* e->b: 0 = locate the first hotspot whose label contains e->s and hover it,
         * 1 = press, 2 = release */
        static float cx = -1.0f, cy = -1.0f;
        if (e->b == 0) {
            GameHotspot hs[64];
            int n = game_collect_room_hotspots(hs, 64), hit = -1;
            for (int i = 0; i < n && hit < 0; i++)
                if (strstr(hs[i].label, e->s)) hit = i;
            if (hit < 0) { replay_fail(e, "no hotspot labelled '%s' in room '%s' (%d hotspots)", e->s, cur_room(), n); return; }
            cx = (float)hs[hit].x;
            cy = (float)hs[hit].y;
            fprintf(stderr, "[replay] clickspot \"%s\" -> %d,%d\n", hs[hit].label, hs[hit].x, hs[hit].y);
            inject_move_to(cx, cy);
        } else {
            win32_input_mouse_button(cx, cy, 0, e->b == 1);
        }
        break;
    }
    case RE_EXPECT_ROOM:
        if (strcmp(cur_room(), e->s) != 0) { replay_fail(e, "expected room '%s', current room '%s'", e->s, cur_room()); return; }
        fprintf(stderr, "[replay] expect room %s ok\n", e->s);
        break;
    case RE_TOAST:
        gl_renderer_show_toast("%s", e->s);
        break;
    case RE_QUICKSAVE:
    case RE_QUICKLOAD:
        g_pending_macro = e->kind == RE_QUICKSAVE ? 1 : 2;
        break;
    case RE_SAVECHECK: {
        struct stat st;
        char path[1200];
        snprintf(path, sizeof(path), "%s/mha2.sav/mha%d.sav", g_game_dir, e->a);
        if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_mtime < g_macro_save_mtime)
            replay_fail(e, "save slot %d was not written (%s)", e->a, path);
        else {
            fprintf(stderr, "[macro] save slot %d written (%lld bytes)\n", e->a, (long long)st.st_size);
            if (g_macro && e->a == QUICK_SLOT) quick_slot_claim();
        }
        break;
    }
    case RE_EXPECT_HOTSPOTS: {
        GameHotspot hs[64];
        int n = game_collect_room_hotspots(hs, 64);
        if (n < e->a) { replay_fail(e, "expected >= %d hotspots in room '%s', found %d", e->a, cur_room(), n); return; }
        fprintf(stderr, "[replay] expect hotspots >= %d ok (%d in %s)\n", e->a, n, cur_room());
        break;
    }
    case RE_EXPECT_MENU:
        if (!menu_active()) { replay_fail(e, "expected main menu (room '%s')", cur_room()); return; }
        fprintf(stderr, "[replay] expect menu ok\n");
        break;
    case RE_QUIT: {
        fprintf(stderr, "[replay] PASS: script completed (%d events, t=%ums, room=%s); quitting\n",
                g_ev_count, t, cur_room());
        SDL_Event q;
        memset(&q, 0, sizeof(q));
        q.type = SDL_QUIT;
        SDL_PushEvent(&q);
        pthread_t th;
        if (pthread_create(&th, NULL, quit_watchdog, NULL) == 0) pthread_detach(th);
        g_replaying = 0;
        break;
    }
    case RE_WAIT_ROOM:
    case RE_WAIT_MENU:
    case RE_WAIT_VIDEO:
    case RE_WAIT_LEAVE:
    case RE_WAIT_INTERACTIVE:
        break;
    }
}

void input_replay_poll(void) {
    uint32_t now = SDL_GetTicks();

    if (g_rec) {
        const char *room = cur_room();
        if (room[0] && strcmp(room, g_rec_room) != 0) {
            snprintf(g_rec_room, sizeof(g_rec_room), "%s", room);
            fprintf(g_rec, "wait room %s\n", room);
            g_rec_seg_start = now;
        }
        int menu = menu_active();
        if (menu && !g_rec_menu) {
            fprintf(g_rec, "wait menu\n");
            g_rec_seg_start = now;
        }
        g_rec_menu = menu;
        int video = win32_avi_is_playing();
        if (video && !g_rec_video) {
            fprintf(g_rec, "wait video\n");
            g_rec_seg_start = now;
        }
        g_rec_video = video;
    }

    if (!g_replaying) return;
    if (!g_seg_start) g_seg_start = now ? now : 1u;

    while (g_replaying && g_ev_pos < g_ev_count) {
        ReplayEvent *e = &g_ev[g_ev_pos];
        if (e->kind == RE_WAIT_ROOM || e->kind == RE_WAIT_MENU || e->kind == RE_WAIT_VIDEO ||
            e->kind == RE_WAIT_LEAVE || e->kind == RE_WAIT_INTERACTIVE) {
            const char *what = e->kind == RE_WAIT_ROOM ? "room" : e->kind == RE_WAIT_MENU ? "menu" :
                               e->kind == RE_WAIT_LEAVE ? "leave" :
                               e->kind == RE_WAIT_INTERACTIVE ? "interactive" : "video";
            if (!g_wait_armed) { g_wait_armed = 1; g_wait_start = now; }
            const char *room = cur_room();
            GameHotspot hsw[64];
            int met = e->kind == RE_WAIT_INTERACTIVE ? game_collect_room_hotspots(hsw, 64) > 0 :
                      e->kind == RE_WAIT_ROOM ? strcmp(room, e->s) == 0 :
                      e->kind == RE_WAIT_LEAVE ? (room[0] && strcmp(room, e->s) != 0) :
                      e->kind == RE_WAIT_MENU ? menu_active() : win32_avi_is_playing();
            if (!met) {
                if (now - g_wait_start > e->timeout_ms)
                    replay_fail(e, "timed out after %us waiting for %s %s (current room '%s')",
                                e->timeout_ms / 1000u, what, e->s, cur_room());
                return;
            }
            fprintf(stderr, "[replay] reached %s %s after %ums\n", what, e->s, now - g_wait_start);
            g_wait_armed = 0;
            g_seg_start = now;
            g_ev_pos++;
            continue;
        }
        uint32_t t = now - g_seg_start;
        if (t < e->ms) return;
        /* Running late (blocked frame, earlier absolute time): shift the rest of the
         * segment so relative spacing survives, e.g. a click keeps its hover delay. */
        if (t > e->ms) {
            g_seg_start += t - e->ms;
            t = e->ms;
        }
        exec_event(e, t);
        if (!g_replaying) return; /* quit, or a macro step failed and reset the engine */
        g_ev_pos++;
        if (g_pending_macro) {
            int which = g_pending_macro;
            g_pending_macro = 0;
            if (which == 1) input_replay_quick_save();
            else input_replay_quick_load();
            return;
        }
    }
    if (g_replaying && g_ev_pos >= g_ev_count) {
        if (g_macro) {
            fprintf(stderr, "[macro] done\n");
            macro_end();
            return;
        }
        fprintf(stderr, "[replay] script finished (%d events); real input re-enabled\n", g_ev_count);
        g_replaying = 0;
    }
}

int input_replay_active(void) {
    return g_replaying;
}

int input_replay_is_injected(const SDL_Event *ev) {
    if (g_inj_head == g_inj_tail) return 0;
    InjSig *h = &g_inj_sig[g_inj_head % INJ_SIG_CAP];
    InjSig sig = inj_sig_of(ev);
    if (sig.type != h->type || sig.key != h->key || sig.text_hash != h->text_hash) return 0;
    g_inj_head++;
    return 1;
}

/* ---------------- recording ---------------- */

void input_replay_record_event(const SDL_Event *ev) {
    if (!g_rec) return;
    uint32_t t = SDL_GetTicks() - g_rec_seg_start;
    if ((ev->type == SDL_KEYDOWN && !ev->key.repeat) || ev->type == SDL_KEYUP) {
        const char *name = SDL_GetKeyName(ev->key.keysym.sym);
        if (name && *name)
            fprintf(g_rec, "%u key %s %s\n", t, ev->type == SDL_KEYDOWN ? "down" : "up", name);
    } else if (ev->type == SDL_TEXTINPUT) {
        fprintf(g_rec, "%u text %s\n", t, ev->text.text);
    }
}

void input_replay_record_mouse(float gx, float gy, int button, int down) {
    if (!g_rec) return;
    uint32_t now = SDL_GetTicks();
    uint32_t t = now - g_rec_seg_start;
    static const char *names[] = { "left", "right", "middle" };
    if (button < 0) {
        if (now - g_rec_last_move < REC_MOVE_INTERVAL_MS || (gx == g_rec_mx && gy == g_rec_my)) return;
        g_rec_last_move = now;
        fprintf(g_rec, "%u move %.1f %.1f\n", t, gx, gy);
    } else if (button <= 2) {
        fprintf(g_rec, "%u button %s %s %.1f %.1f\n", t, names[button], down ? "down" : "up", gx, gy);
    }
    g_rec_mx = gx;
    g_rec_my = gy;
}

/* ---------------- lifecycle ---------------- */

int input_replay_init(const char *record_path, const char *replay_path, const char *out_dir) {
    if (out_dir && *out_dir) snprintf(g_out_dir, sizeof(g_out_dir), "%s", out_dir);
    /* --record together with --replay re-records the injected input (converts a
     * hand-written script into a recording; used by the recorder round-trip test). */
    if (replay_path) {
        if (parse_script(replay_path) != 0) return -1;
        g_replaying = g_ev_count > 0;
        fprintf(stderr, "[replay] loaded %d events from %s (real input ignored while replaying)\n",
                g_ev_count, replay_path);
    }
    if (record_path) {
        g_rec = fopen(record_path, "w");
        if (!g_rec) { perror(record_path); return -1; }
        setvbuf(g_rec, NULL, _IOLBF, 0);
        fprintf(g_rec, "# mha2 input script v1 (recorded)\n");
        g_rec_seg_start = SDL_GetTicks();
        fprintf(stderr, "[replay] recording input to %s\n", record_path);
    }
    return 0;
}

void input_replay_shutdown(void) {
    if (g_rec) { fclose(g_rec); g_rec = NULL; }
    free(g_ev);
    g_ev = NULL;
    g_ev_count = g_ev_cap = g_ev_pos = 0;
    g_replaying = 0;
}

/* ---------------- quick save / quick load macros ----------------
 * Drive the game's own Kapitelauswahl screen (Escape -> select slot -> Speichern /
 * Laden), so saving/loading uses the original code paths and slot thumbnails. Only
 * the dedicated quick slot is ever written; the user's other slots are untouched. */
static const int k_slot_xy[9][2] = {
    { 219, 149 }, { 400, 149 }, { 581, 149 },
    { 219, 290 }, { 400, 290 }, { 581, 290 },
    { 219, 430 }, { 400, 430 }, { 581, 430 },
};
#define BTN_SPEICHERN_X 339
#define BTN_LADEN_X     491
#define BTN_ROW_Y       566

static void quick_slot_path(char *buf, size_t cap) {
    snprintf(buf, cap, "%s/mha2.sav/mha%d.sav", g_game_dir, QUICK_SLOT);
}

/* Slot ownership: the quick slot is only overwritten if it is empty or still holds the
 * exact file a previous quick save wrote (hash recorded in <game dir>/mha2_quicksave.id),
 * so a manual save the player made in that slot is never destroyed. */
static int file_hash(const char *path, uint64_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t h = 0xcbf29ce484222325ull;
    int ch;
    while ((ch = fgetc(f)) != EOF) { h ^= (uint8_t)ch; h *= 0x100000001b3ull; }
    fclose(f);
    *out = h;
    return 0;
}

static void quick_id_path(char *buf, size_t cap) {
    snprintf(buf, cap, "%s/mha2_quicksave.id", g_game_dir);
}

static int quick_slot_is_ours(void) {
    char path[1200], idp[1200];
    struct stat st;
    quick_slot_path(path, sizeof(path));
    if (stat(path, &st) != 0 || st.st_size <= 0) return 1; /* free slot */
    uint64_t h, rec = 0;
    quick_id_path(idp, sizeof(idp));
    FILE *f = fopen(idp, "r");
    int have = f && fscanf(f, "%llx", (unsigned long long *)&rec) == 1;
    if (f) fclose(f);
    return have && file_hash(path, &h) == 0 && h == rec;
}

static void quick_slot_claim(void) {
    char path[1200], idp[1200];
    uint64_t h;
    quick_slot_path(path, sizeof(path));
    quick_id_path(idp, sizeof(idp));
    if (file_hash(path, &h) != 0) return;
    FILE *f = fopen(idp, "w");
    if (f) { fprintf(f, "%016llx\n", (unsigned long long)h); fclose(f); }
}

static int quick_ready(const char *what) {
    if (g_macro) return 0; /* a macro is already driving the game */
    const char *room = cur_room();
    if (strncmp(room, "l/", 2) != 0 || win32_avi_is_playing() || gl_menu_is_open() ||
        gl_renderer_is_dialog_log_open() || gl_renderer_is_page_reader_open()) {
        gl_renderer_show_toast("%s: only available while exploring a room", what);
        fprintf(stderr, "[macro] rejected %s: not in a room (room '%s')\n", what, room);
        return 0;
    }
    GameHotspot hs[64];
    if (game_collect_room_hotspots(hs, 64) <= 0) { /* cutscene, dialog, transition */
        gl_renderer_show_toast("%s: not available during cutscenes or dialogs", what);
        fprintf(stderr, "[macro] rejected %s: scene not interactive (room '%s')\n", what, room);
        return 0;
    }
    /* restored at the end: otherwise the cursor is left on the save screen's button
     * row, i.e. on the room's inventory bar, which swallows the next click */
    win32_input_mouse_pos(&g_macro_cursor_x, &g_macro_cursor_y);
    return 1;
}

static void start_macro(const char *text, const char *fail_toast) {
    if (g_replaying) {
        g_stash.active = 1;
        g_stash.ev = g_ev;
        g_stash.count = g_ev_count;
        g_stash.cap = g_ev_cap;
        g_stash.pos = g_ev_pos; /* triggering key event was injected by an earlier poll */
        g_stash.mod = g_inj_mod;
        g_inj_mod = 0;
        g_stash.replaying = g_replaying;
        g_ev = NULL;
        g_ev_cap = 0;
    }
    reset_events();
    FILE *f = fmemopen((void *)text, strlen(text), "r");
    if (!f || parse_script_stream(f, "<macro>") != 0) {
        if (f) fclose(f);
        macro_end();
        gl_renderer_show_toast("%s", fail_toast);
        return;
    }
    fclose(f);
    snprintf(g_macro_fail_toast, sizeof(g_macro_fail_toast), "%s", fail_toast);
    snprintf(g_macro_origin_room, sizeof(g_macro_origin_room), "%s", cur_room());
    g_macro = 1;
    g_replaying = g_ev_count > 0;
}

void input_replay_quick_save(void) {
    if (g_macro) return;
    if (!quick_slot_is_ours()) {
        gl_renderer_show_toast("Quick Save: slot %d holds a manual save - not overwriting it", QUICK_SLOT);
        fprintf(stderr, "[macro] rejected Quick Save: slot %d holds a save not written by quick save\n", QUICK_SLOT);
        return;
    }
    if (!quick_ready("Quick Save")) return;
    const int *xy = k_slot_xy[QUICK_SLOT - 1];
    char buf[2048];
    int n = snprintf(buf, sizeof(buf),
                     "0 tap Escape\n"
                     "wait room x/006 10\n"
                     "900 click %d %d\n"
                     "+900 click %d %d\n"
                     "+600 tap Backspace\n", xy[0], xy[1], BTN_SPEICHERN_X, BTN_ROW_Y);
    for (int i = 0; i < 19; i++) n += snprintf(buf + n, sizeof(buf) - (size_t)n, "+30 tap Backspace\n");
    snprintf(buf + n, sizeof(buf) - (size_t)n,
             "+200 text Quicksave\n"
             "+300 tap Return\n"
             "+1200 savecheck %d\n"
             "+200 tap Escape\n"
             "wait leave x/006 15\n"
             "300 move %.0f %.0f\n"
             "+0 toast Quick Saved (Slot %d)\n", QUICK_SLOT, g_macro_cursor_x, g_macro_cursor_y, QUICK_SLOT);
    g_macro_save_mtime = time(NULL);
    fprintf(stderr, "[macro] quick save -> slot %d (room %s)\n", QUICK_SLOT, cur_room());
    start_macro(buf, "Quick Save failed");
}

void input_replay_quick_load(void) {
    char path[1200];
    struct stat st;
    quick_slot_path(path, sizeof(path));
    if (g_macro) return;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        char keys[64];
        bindings_describe(ACT_QUICK_SAVE, keys, sizeof(keys));
        gl_renderer_show_toast("No Quick Save yet (%s)", keys);
        fprintf(stderr, "[macro] rejected Quick Load: no quick save in slot %d\n", QUICK_SLOT);
        return;
    }
    if (!quick_ready("Quick Load")) return;
    const int *xy = k_slot_xy[QUICK_SLOT - 1];
    char buf[512];
    snprintf(buf, sizeof(buf),
             "0 tap Escape\n"
             "wait room x/006 10\n"
             "900 click %d %d\n"
             "+900 click %d %d\n"
             "wait leave x/006 30\n"
             "800 move %.0f %.0f\n"
             "+0 toast Quick Loaded (Slot %d)\n", xy[0], xy[1], BTN_LADEN_X, BTN_ROW_Y,
             g_macro_cursor_x, g_macro_cursor_y, QUICK_SLOT);
    fprintf(stderr, "[macro] quick load <- slot %d (room %s)\n", QUICK_SLOT, cur_room());
    start_macro(buf, "Quick Load failed");
}
