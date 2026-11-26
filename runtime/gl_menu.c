/*
 * Interactive F1 graphics/post-FX/assets settings menu, split out of gl_renderer.c.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gl_internal.h"
#include "hd_assets.h"
#include "input_bindings.h"

/* ============================================================================
 * Interactive 2-Tab In-Game Graphics & Remaster FX Settings Menu (F1)
 * ============================================================================ */

enum MenuRowId {
    MROW_DISPLAY_MODE = 0,
    MROW_DISPLAY_INDEX,
    MROW_RESOLUTION,
    MROW_ASPECT_MODE,
    MROW_AMBIENT_BARS,
    MROW_SHADER,
    MROW_SHARPEN,
    MROW_VSYNC,
    MROW_FPS_CAP,
    MROW_HUD,
    MROW_GAME_SPEED,
    MROW_SCREENSHOT,
    /* Tab 2: Post-FX & Color */
    MROW_BLOOM,
    MROW_COLOR_GRADE,
    MROW_SILHOUETTE_AA,
    MROW_AA_STRENGTH,
    MROW_CLARITY,
    MROW_FILMIC,
    MROW_DEBAND,
    MROW_ATMOSPHERE,
    MROW_VIGNETTE,
    MROW_TRANSITIONS,
    /* Tab 3: Assets, Typography & Audio */
    MROW_HD_ASSETS,
    MROW_HD_SPRITES,
    MROW_HD_TEXT,
    MROW_FONT_STYLE,
    MROW_TEXT_BACKDROP,
    MROW_DECOUPLED_CURSOR,
    MROW_VIDEO_ENHANCE,
    MROW_HOTSPOT_HINTS,
    MROW_AUDIO_REVERB,
    MROW_AUDIO_EQ,
    MROW_VOLUME,
    MROW_OVERLAY_TEXT,
    MROW_BIND_FIRST,                               /* 4. CONTROLS: one row per BindAction */
    MROW_BIND_LAST = MROW_BIND_FIRST + ACT_COUNT - 1,
    MROW_BIND_RESET,
    MROW_CLOSE,
    MROW_COUNT
};

static const int g_tab0_rows[] = {
    MROW_DISPLAY_MODE,
    MROW_DISPLAY_INDEX,
    MROW_RESOLUTION,
    MROW_ASPECT_MODE,
    MROW_AMBIENT_BARS,
    MROW_SHADER,
    MROW_SHARPEN,
    MROW_VSYNC,
    MROW_FPS_CAP,
    MROW_HUD,
    MROW_GAME_SPEED,
    MROW_SCREENSHOT,
    MROW_CLOSE
};
#define NUM_TAB0_ROWS ((int)(sizeof(g_tab0_rows) / sizeof(g_tab0_rows[0])))

static const int g_tab1_rows[] = {
    MROW_SILHOUETTE_AA,
    MROW_AA_STRENGTH,
    MROW_CLARITY,
    MROW_FILMIC,
    MROW_BLOOM,
    MROW_COLOR_GRADE,
    MROW_ATMOSPHERE,
    MROW_DEBAND,
    MROW_VIGNETTE,
    MROW_TRANSITIONS,
    MROW_CLOSE
};
#define NUM_TAB1_ROWS ((int)(sizeof(g_tab1_rows) / sizeof(g_tab1_rows[0])))

static const int g_tab2_rows[] = {
    MROW_HD_ASSETS,
    MROW_HD_SPRITES,
    MROW_HD_TEXT,
    MROW_FONT_STYLE,
    MROW_TEXT_BACKDROP,
    MROW_DECOUPLED_CURSOR,
    MROW_VIDEO_ENHANCE,
    MROW_HOTSPOT_HINTS,
    MROW_AUDIO_REVERB,
    MROW_AUDIO_EQ,
    MROW_VOLUME,
    MROW_OVERLAY_TEXT,
    MROW_CLOSE
};
#define NUM_TAB2_ROWS ((int)(sizeof(g_tab2_rows) / sizeof(g_tab2_rows[0])))

#define NUM_TAB3_ROWS (ACT_COUNT + 2)
static int g_tab3_rows[NUM_TAB3_ROWS];

static void init_tab3_rows(void) {
    for (int a = 0; a < ACT_COUNT; a++) g_tab3_rows[a] = MROW_BIND_FIRST + a;
    g_tab3_rows[ACT_COUNT] = MROW_BIND_RESET;
    g_tab3_rows[ACT_COUNT + 1] = MROW_CLOSE;
}

#define MENU_TABS 4
/* Key capture for rebinding (-1 = idle): next key press becomes the binding */
static int g_capture_action = -1;
static int g_capture_slot = 0;

typedef struct ResPreset {
    int w;
    int h;
    const char *label;
} ResPreset;

static const ResPreset g_res_presets[] = {
    {  800,  600, "800x600 (4:3 Native)" },
    { 1024,  768, "1024x768 (4:3)" },
    { 1280,  720, "1280x720 (16:9)" },
    { 1280,  800, "1280x800 (16:10)" },
    { 1280,  960, "1280x960 (4:3)" },
    { 1440,  900, "1440x900 (16:10)" },
    { 1600, 1000, "1600x1000 (16:10)" },
    { 1600, 1200, "1600x1200 (4:3)" },
    { 1920, 1080, "1920x1080 (16:9)" },
    { 1920, 1200, "1920x1200 (16:10)" },
    { 2560, 1440, "2560x1440 (16:9)" },
    { 2560, 1600, "2560x1600 (16:10)" },
};
#define NUM_RES_PRESETS ((int)(sizeof(g_res_presets) / sizeof(g_res_presets[0])))

static const int g_fps_presets[] = { 60, 120, 144, 165, 240, 0 };
#define NUM_FPS_PRESETS ((int)(sizeof(g_fps_presets) / sizeof(g_fps_presets[0])))

static int    g_menu_open = 0;
static int    g_menu_tab = 0;   /* 0 = Display & Scaling, 1 = Remaster FX & Assets */
static int    g_menu_sel = 0;   /* index within active tab */
static int    g_menu_dirty = 1;
static GLuint g_menu_tex = 0;
static int    g_menu_tex_w = 0, g_menu_tex_h = 0;
static int    g_menu_last_draw_w = 0, g_menu_last_draw_h = 0;
/* Cached menu panel rect in SDL window coordinates for mouse hit-testing */
static float  g_menu_win_x = 0.0f, g_menu_win_y = 0.0f, g_menu_win_w = 0.0f, g_menu_win_h = 0.0f;
static float  g_menu_tabs_top_frac = 0.10f;
static float  g_menu_tabs_bot_frac = 0.17f;
static float  g_menu_rows_top_frac = 0.19f;
static float  g_menu_row_h_frac = 0.060f;

static int menu_active_row_count(void) {
    if (g_menu_tab == 0) return NUM_TAB0_ROWS;
    if (g_menu_tab == 1) return NUM_TAB1_ROWS;
    if (g_menu_tab == 3) return NUM_TAB3_ROWS;
    return NUM_TAB2_ROWS;
}

static int menu_active_row_id(int idx) {
    if (g_menu_tab == 0) {
        if (idx < 0 || idx >= NUM_TAB0_ROWS) return MROW_CLOSE;
        return g_tab0_rows[idx];
    }
    if (g_menu_tab == 1) {
        if (idx < 0 || idx >= NUM_TAB1_ROWS) return MROW_CLOSE;
        return g_tab1_rows[idx];
    }
    if (g_menu_tab == 3) {
        if (idx < 0 || idx >= NUM_TAB3_ROWS) return MROW_CLOSE;
        if (g_tab3_rows[NUM_TAB3_ROWS - 1] != MROW_CLOSE) init_tab3_rows();
        return g_tab3_rows[idx];
    }
    if (idx < 0 || idx >= NUM_TAB2_ROWS) return MROW_CLOSE;
    return g_tab2_rows[idx];
}

int gl_menu_is_capturing_key(void) {
    return g_menu_open && g_capture_action >= 0;
}

void gl_menu_mark_dirty(void) {
    g_menu_dirty = 1;
}

int gl_menu_needs_redraw(void) {
    return g_menu_open && g_menu_dirty;
}

void gl_menu_shutdown(void) {
    if (g_menu_tex) {
        glDeleteTextures(1, &g_menu_tex);
        g_menu_tex = 0;
    }
    g_menu_tex_w = g_menu_tex_h = 0;
    g_menu_dirty = 1;
}

int gl_menu_is_open(void) {
    return g_menu_open;
}

void gl_menu_toggle(void) {
    g_menu_open = !g_menu_open;
    g_capture_action = -1;
    g_menu_dirty = 1;
    if (g_menu_open) {
        SDL_ShowCursor(SDL_ENABLE);
    } else {
        SDL_ShowCursor(g_os_cursor_hidden ? SDL_DISABLE : SDL_ENABLE);
    }
    gl_renderer_redraw_if_needed(1);
}

static int find_current_res_index(void) {
    int cw = g_cfg.init_win_w, ch = g_cfg.init_win_h;
    if (g_sdl_window && !g_fullscreen) {
        SDL_GetWindowSize(g_sdl_window, &cw, &ch);
    }
    int best = 3; /* 1280x800 */
    int best_dist = 1 << 30;
    for (int i = 0; i < NUM_RES_PRESETS; i++) {
        int d = abs(g_res_presets[i].w - cw) + abs(g_res_presets[i].h - ch);
        if (d < best_dist) { best_dist = d; best = i; }
    }
    return best;
}

static int find_current_fps_index(void) {
    for (int i = 0; i < NUM_FPS_PRESETS; i++) {
        if (g_fps_presets[i] == g_cfg.max_fps) return i;
    }
    return 4; /* 240 */
}

static void menu_row_label_and_value(int row, char *label, size_t lsz, char *val, size_t vsz) {
    val[0] = '\0';
    switch (row) {
        case MROW_DISPLAY_MODE:
            snprintf(label, lsz, "Display Mode");
            snprintf(val, vsz, "%s", g_fullscreen ? "Borderless Fullscreen" : "Windowed");
            break;
        case MROW_DISPLAY_INDEX: {
            int nd = SDL_GetNumVideoDisplays();
            if (nd < 1) nd = 1;
            int di = clamp_i(g_cfg.display_index, 0, nd - 1);
            snprintf(label, lsz, "Target Monitor");
            snprintf(val, vsz, "Display %d / %d", di + 1, nd);
            break;
        }
        case MROW_RESOLUTION: {
            snprintf(label, lsz, "Window Resolution");
            int ri = find_current_res_index();
            if (g_fullscreen) {
                int dw = 0, dh = 0;
                if (g_sdl_window) SDL_GL_GetDrawableSize(g_sdl_window, &dw, &dh);
                snprintf(val, vsz, "%dx%d (Win: %dx%d)", dw, dh, g_res_presets[ri].w, g_res_presets[ri].h);
            } else {
                snprintf(val, vsz, "%s", g_res_presets[ri].label);
            }
            break;
        }
        case MROW_ASPECT_MODE:
            snprintf(label, lsz, "Aspect / Widescreen");
            snprintf(val, vsz, "%s", gl_aspect_name(g_cfg.aspect_mode));
            break;
        case MROW_AMBIENT_BARS:
            snprintf(label, lsz, "4:3 Menu Sidebars");
            snprintf(val, vsz, "%s", g_cfg.ambient_bars ? "Ambient Blur" : "Pure Black");
            break;
        case MROW_SHADER:
            snprintf(label, lsz, "Upscaling Shader");
            snprintf(val, vsz, "%s", gl_shader_name(g_cfg.shader_mode));
            break;
        case MROW_SHARPEN:
            snprintf(label, lsz, "CAS Sharpening");
            snprintf(val, vsz, "%d%%", (int)(g_cfg.sharpen_strength * 100.0f + 0.5f));
            break;
        case MROW_VSYNC:
            snprintf(label, lsz, "Frame Pacing / VSync");
            snprintf(val, vsz, "%s", gl_vsync_name(g_cfg.vsync_mode));
            break;
        case MROW_FPS_CAP:
            snprintf(label, lsz, "High-FPS Mode Cap");
            if (g_cfg.max_fps <= 0) snprintf(val, vsz, "Unlimited");
            else snprintf(val, vsz, "%d FPS", g_cfg.max_fps);
            break;
        case MROW_HUD:
            snprintf(label, lsz, "Performance Stats HUD");
            snprintf(val, vsz, "%s", g_cfg.show_hud ? "ON" : "OFF");
            break;
        case MROW_GAME_SPEED:
            snprintf(label, lsz, "Game Speed (Hold Shift=2x)");
            snprintf(val, vsz, "%s", gl_speed_name(g_cfg.game_speed));
            break;
        case MROW_SCREENSHOT:
            snprintf(label, lsz, "Capture 4K Screenshot (PrtSc)");
            snprintf(val, vsz, "Press Enter");
            break;
        case MROW_BLOOM:
            snprintf(label, lsz, "Emissive Bloom Glow");
            snprintf(val, vsz, "%s", gl_bloom_name(g_cfg.bloom_level));
            break;
        case MROW_COLOR_GRADE:
            snprintf(label, lsz, "Color Grading & Vibrance");
            snprintf(val, vsz, "%s", gl_grade_name(g_cfg.color_grade));
            break;
        case MROW_SILHOUETTE_AA:
            snprintf(label, lsz, "Anti-Aliasing Mode");
            snprintf(val, vsz, "%s", gl_aa_name(g_cfg.silhouette_aa));
            break;
        case MROW_AA_STRENGTH:
            snprintf(label, lsz, "Anti-Aliasing Strength");
            snprintf(val, vsz, "%d%%", (int)(g_cfg.aa_strength * 100.0f + 0.5f));
            break;
        case MROW_CLARITY:
            snprintf(label, lsz, "Local Contrast (Clarity)");
            snprintf(val, vsz, "%d%%", (int)(g_cfg.clarity_strength * 100.0f + 0.5f));
            break;
        case MROW_FILMIC:
            snprintf(label, lsz, "Filmic HDR Tone-Map");
            snprintf(val, vsz, "%d%%", (int)(g_cfg.filmic_strength * 100.0f + 0.5f));
            break;
        case MROW_DEBAND:
            snprintf(label, lsz, "Debanding & Dither");
            snprintf(val, vsz, "%s", g_cfg.deband_dither ? "ON" : "OFF");
            break;
        case MROW_ATMOSPHERE:
            snprintf(label, lsz, "Living Atmosphere FX");
            snprintf(val, vsz, "%s", g_cfg.atmosphere_fx ? "ON (Flicker+Dust)" : "OFF");
            break;
        case MROW_FONT_STYLE:
            snprintf(label, lsz, "Game Font Style");
            snprintf(val, vsz, "%s", gl_font_style_name(g_cfg.font_style));
            break;
        case MROW_TEXT_BACKDROP:
            snprintf(label, lsz, "Text Readability Backing");
            snprintf(val, vsz, "%s", g_cfg.text_backdrop ? "Soft Backing" : "Classic Outline");
            break;
        case MROW_VIGNETTE:
            snprintf(label, lsz, "Cinematic Vignette");
            snprintf(val, vsz, "%s", gl_vignette_name(g_cfg.vignette_level));
            break;
        case MROW_TRANSITIONS:
            snprintf(label, lsz, "Scene Transitions");
            snprintf(val, vsz, "%s", g_cfg.scene_transitions ? "Smooth Fade" : "Instant");
            break;
        case MROW_VIDEO_ENHANCE:
            snprintf(label, lsz, "Cutscene Deblock + Sharpen");
            snprintf(val, vsz, "%s", g_cfg.video_enhance ? "ON" : "OFF");
            break;
        case MROW_HOTSPOT_HINTS:
            snprintf(label, lsz, "Hotspot Highlight (Space/H)");
            snprintf(val, vsz, "%s", g_cfg.hotspot_hints ? "ON" : "OFF");
            break;
        case MROW_AUDIO_REVERB:
            snprintf(label, lsz, "Audio Ambience (Reverb)");
            snprintf(val, vsz, "%s", gl_reverb_name(g_cfg.audio_reverb));
            break;
        case MROW_AUDIO_EQ:
            snprintf(label, lsz, "Audio Mastering EQ + Wide");
            snprintf(val, vsz, "%s", gl_audio_eq_name(g_cfg.audio_eq));
            break;
        case MROW_HD_ASSETS:
            snprintf(label, lsz, "HD Backgrounds (4K/2K)");
            if (g_cfg.hd_assets_enabled) snprintf(val, vsz, "ON (%d indexed)", hd_assets_count());
            else snprintf(val, vsz, "OFF (Original)");
            break;
        case MROW_HD_SPRITES:
            snprintf(label, lsz, "HD Sprites & Characters");
            if (g_cfg.hd_sprites_enabled) snprintf(val, vsz, "ON (%d indexed)", hd_sprites_count());
            else snprintf(val, vsz, "OFF (Original)");
            break;
        case MROW_HD_TEXT:
            snprintf(label, lsz, "Native HD Vector Text");
            snprintf(val, vsz, "%s", g_cfg.hd_vector_text ? "ON (Monitor DPI)" : "OFF (Original)");
            break;
        case MROW_DECOUPLED_CURSOR:
            snprintf(label, lsz, "High-Refresh Cursor");
            snprintf(val, vsz, "%s", g_cfg.decoupled_cursor ? "ON (Decoupled)" : "OFF (Software)");
            break;
        case MROW_VOLUME:
            snprintf(label, lsz, "Master Volume");
            if (g_cfg.muted) snprintf(val, vsz, "Muted");
            else snprintf(val, vsz, "%d%%", (int)(g_cfg.master_volume * 100.0f + 0.5f));
            break;
        case MROW_CLOSE:
            snprintf(label, lsz, "Close Settings Menu");
            {
                char keys[48];
                bindings_describe(ACT_SETTINGS_MENU, keys, sizeof(keys));
                snprintf(val, vsz, "[ %s / Esc ]", keys);
            }
            break;
        case MROW_OVERLAY_TEXT:
            snprintf(label, lsz, "Overlay Text Size (Log, Labels, Toasts)");
            snprintf(val, vsz, "%d%%", g_cfg.overlay_text_scale);
            break;
        case MROW_BIND_RESET:
            snprintf(label, lsz, "Reset All Controls to Defaults");
            snprintf(val, vsz, "Press Enter");
            break;
        default:
            if (row >= MROW_BIND_FIRST && row <= MROW_BIND_LAST) {
                int a = row - MROW_BIND_FIRST;
                snprintf(label, lsz, "%s", bindings_action_label((BindAction)a));
                if (g_capture_action == a) {
                    snprintf(val, vsz, "Press key for %s slot...", g_capture_slot ? "2nd" : "1st");
                } else {
                    char k0[40], k1[40]; /* "k0  /  k1" must fit the 96-byte value cell */
                    bindings_format_chord(bindings_key((BindAction)a, 0), k0, sizeof(k0));
                    bindings_format_chord(bindings_key((BindAction)a, 1), k1, sizeof(k1));
                    snprintf(val, vsz, "%s%s%s", k0[0] ? k0 : "-", k1[0] ? "  /  " : "", k1);
                }
                break;
            }
            if (lsz > 0) label[0] = '\0';
            break;
    }
}

static void menu_apply_delta(int row_id, int dir) {
    if (dir == 0) dir = 1;
    switch (row_id) {
        case MROW_DISPLAY_MODE:
            gl_renderer_toggle_fullscreen();
            return;
        case MROW_DISPLAY_INDEX: {
            int nd = SDL_GetNumVideoDisplays();
            if (nd < 1) nd = 1;
            g_cfg.display_index = (g_cfg.display_index + dir + nd) % nd;
            if (g_sdl_window) {
                SDL_Rect b;
                if (SDL_GetDisplayBounds(g_cfg.display_index, &b) == 0) {
                    int was_fs = g_fullscreen;
                    if (was_fs) SDL_SetWindowFullscreen(g_sdl_window, 0);
                    int ww = g_cfg.init_win_w, wh = g_cfg.init_win_h;
                    SDL_GetWindowSize(g_sdl_window, &ww, &wh);
                    SDL_SetWindowPosition(g_sdl_window, b.x + (b.w - ww) / 2, b.y + (b.h - wh) / 2);
                    if (was_fs) SDL_SetWindowFullscreen(g_sdl_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
            }
            gl_renderer_show_toast("Target Monitor: Display %d / %d", g_cfg.display_index + 1, nd);
            break;
        }
        case MROW_RESOLUTION: {
            int ri = (find_current_res_index() + dir + NUM_RES_PRESETS) % NUM_RES_PRESETS;
            g_cfg.init_win_w = g_res_presets[ri].w;
            g_cfg.init_win_h = g_res_presets[ri].h;
            if (g_sdl_window) {
                if (g_fullscreen) {
                    g_fullscreen = 0;
                    SDL_SetWindowFullscreen(g_sdl_window, 0);
                }
                SDL_SetWindowSize(g_sdl_window, g_cfg.init_win_w, g_cfg.init_win_h);
                SDL_SetWindowPosition(g_sdl_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            }
            gl_renderer_show_toast("Resolution: %s", g_res_presets[ri].label);
            break;
        }
        case MROW_ASPECT_MODE:
            g_cfg.aspect_mode = (g_cfg.aspect_mode + dir + ASPECT_COUNT) % ASPECT_COUNT;
            gl_renderer_show_toast("Aspect / Widescreen: %s", gl_aspect_name(g_cfg.aspect_mode));
            break;
        case MROW_AMBIENT_BARS:
            g_cfg.ambient_bars = !g_cfg.ambient_bars;
            gl_renderer_show_toast("4:3 Menu Sidebars: %s", g_cfg.ambient_bars ? "Ambient Blur" : "Pure Black");
            break;
        case MROW_SHADER:
            g_cfg.shader_mode = (g_cfg.shader_mode + dir + SHADER_COUNT) % SHADER_COUNT;
            gl_renderer_show_toast("Upscaler: %s", gl_shader_name(g_cfg.shader_mode));
            break;
        case MROW_SHARPEN: {
            int pct = (int)(g_cfg.sharpen_strength * 100.0f + 0.5f);
            pct = ((pct + 5) / 10) * 10 + dir * 10;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            g_cfg.sharpen_strength = (float)pct / 100.0f;
            break;
        }
        case MROW_VSYNC:
            g_cfg.vsync_mode = (g_cfg.vsync_mode + dir + VSYNC_COUNT) % VSYNC_COUNT;
            apply_vsync_mode();
            gl_renderer_show_toast("Frame Pacing: %s", gl_vsync_name(g_cfg.vsync_mode));
            break;
        case MROW_FPS_CAP: {
            int fi = (find_current_fps_index() + dir + NUM_FPS_PRESETS) % NUM_FPS_PRESETS;
            g_cfg.max_fps = g_fps_presets[fi];
            break;
        }
        case MROW_HUD:
            gl_renderer_toggle_hud();
            return;
        case MROW_GAME_SPEED:
            g_cfg.game_speed = (g_cfg.game_speed + dir + SPEED_COUNT) % SPEED_COUNT;
            gl_renderer_show_toast("Game Speed: %s", gl_speed_name(g_cfg.game_speed));
            break;
        case MROW_SCREENSHOT:
            gl_menu_toggle();
            gl_renderer_request_screenshot();
            return;
        case MROW_BLOOM:
            g_cfg.bloom_level = (g_cfg.bloom_level + dir + BLOOM_COUNT) % BLOOM_COUNT;
            gl_renderer_show_toast("Emissive Bloom: %s", gl_bloom_name(g_cfg.bloom_level));
            break;
        case MROW_COLOR_GRADE:
            g_cfg.color_grade = (g_cfg.color_grade + dir + GRADE_COUNT) % GRADE_COUNT;
            gl_renderer_show_toast("Color Grading: %s", gl_grade_name(g_cfg.color_grade));
            break;
        case MROW_SILHOUETTE_AA:
            g_cfg.silhouette_aa = (g_cfg.silhouette_aa + dir + AA_COUNT) % AA_COUNT;
            gl_renderer_show_toast("Anti-Aliasing: %s", gl_aa_name(g_cfg.silhouette_aa));
            break;
        case MROW_AA_STRENGTH: {
            int pct = (int)(g_cfg.aa_strength * 100.0f + 0.5f);
            pct = ((pct + 5) / 10) * 10 + dir * 10;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            g_cfg.aa_strength = (float)pct / 100.0f;
            break;
        }
        case MROW_CLARITY: {
            int pct = (int)(g_cfg.clarity_strength * 100.0f + 0.5f);
            pct = ((pct + 5) / 10) * 10 + dir * 10;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            g_cfg.clarity_strength = (float)pct / 100.0f;
            break;
        }
        case MROW_FILMIC: {
            int pct = (int)(g_cfg.filmic_strength * 100.0f + 0.5f);
            pct = ((pct + 5) / 10) * 10 + dir * 10;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            g_cfg.filmic_strength = (float)pct / 100.0f;
            break;
        }
        case MROW_DEBAND:
            g_cfg.deband_dither = !g_cfg.deband_dither;
            gl_renderer_show_toast("Debanding & Dither: %s", g_cfg.deband_dither ? "ON" : "OFF");
            break;
        case MROW_ATMOSPHERE:
            g_cfg.atmosphere_fx = !g_cfg.atmosphere_fx;
            gl_renderer_show_toast("Living Atmosphere FX: %s", g_cfg.atmosphere_fx ? "ON" : "OFF");
            break;
        case MROW_FONT_STYLE:
            g_cfg.font_style = (g_cfg.font_style + dir + FONT_STYLE_COUNT) % FONT_STYLE_COUNT;
            flush_canvas_text_cache();
            gl_renderer_show_toast("Game Font Style: %s", gl_font_style_name(g_cfg.font_style));
            break;
        case MROW_TEXT_BACKDROP:
            g_cfg.text_backdrop = !g_cfg.text_backdrop;
            flush_canvas_text_cache();
            gl_renderer_show_toast("Text Backing: %s", g_cfg.text_backdrop ? "Soft Backing" : "Classic Outline");
            break;
        case MROW_VIGNETTE:
            g_cfg.vignette_level = (g_cfg.vignette_level + dir + VIGNETTE_COUNT) % VIGNETTE_COUNT;
            gl_renderer_show_toast("Cinematic Vignette: %s", gl_vignette_name(g_cfg.vignette_level));
            break;
        case MROW_TRANSITIONS:
            g_cfg.scene_transitions = !g_cfg.scene_transitions;
            gl_renderer_show_toast("Scene Transitions: %s", g_cfg.scene_transitions ? "Smooth Fade" : "Instant");
            break;
        case MROW_VIDEO_ENHANCE:
            g_cfg.video_enhance = !g_cfg.video_enhance;
            gl_renderer_show_toast("Cutscene Deblock + Sharpen: %s", g_cfg.video_enhance ? "ON" : "OFF");
            break;
        case MROW_HOTSPOT_HINTS:
            g_cfg.hotspot_hints = !g_cfg.hotspot_hints;
            {
                char keys[64];
                bindings_describe(ACT_HOTSPOTS, keys, sizeof(keys));
                if (g_cfg.hotspot_hints) gl_renderer_show_toast("Hotspot Highlight: ON (Hold %s)", keys);
                else gl_renderer_show_toast("Hotspot Highlight: OFF");
            }
            break;
        case MROW_AUDIO_REVERB: {
            int rev = (g_cfg.audio_reverb + dir + REVERB_COUNT) % REVERB_COUNT;
            __atomic_store_n(&g_cfg.audio_reverb, rev, __ATOMIC_RELAXED);
            gl_renderer_show_toast("Audio Ambience: %s", gl_reverb_name(rev));
            break;
        }
        case MROW_AUDIO_EQ: {
            int eq = (g_cfg.audio_eq + dir + EQ_COUNT) % EQ_COUNT;
            __atomic_store_n(&g_cfg.audio_eq, eq, __ATOMIC_RELAXED);
            gl_renderer_show_toast("Audio Mastering EQ: %s", gl_audio_eq_name(eq));
            break;
        }
        case MROW_HD_ASSETS:
            gl_renderer_toggle_hd_assets();
            return;
        case MROW_HD_SPRITES:
            g_cfg.hd_sprites_enabled = !g_cfg.hd_sprites_enabled;
            gl_renderer_show_toast("HD Sprites & Characters: %s", g_cfg.hd_sprites_enabled ? "ON" : "OFF");
            break;
        case MROW_HD_TEXT:
            gl_renderer_toggle_hd_text();
            return;
        case MROW_DECOUPLED_CURSOR:
            gl_renderer_toggle_decoupled_cursor();
            return;
        case MROW_VOLUME:
            if (g_cfg.muted && dir > 0) {
                __atomic_store_n(&g_cfg.muted, 0, __ATOMIC_RELAXED);
            } else {
                float vol = g_cfg.master_volume + (float)dir * 0.1f;
                if (vol <= 0.01f) {
                    vol = 0.0f;
                    __atomic_store(&g_cfg.master_volume, &vol, __ATOMIC_RELAXED);
                    __atomic_store_n(&g_cfg.muted, 1, __ATOMIC_RELAXED);
                } else {
                    if (vol > 1.5f) vol = 1.5f;
                    __atomic_store(&g_cfg.master_volume, &vol, __ATOMIC_RELAXED);
                    __atomic_store_n(&g_cfg.muted, 0, __ATOMIC_RELAXED);
                }
            }
            break;
        case MROW_CLOSE:
            gl_menu_toggle();
            return;
        case MROW_OVERLAY_TEXT: {
            static const int steps[] = { 100, 125, 150, 175, 200 };
            int idx = 0;
            for (int i = 0; i < 5; i++) if (steps[i] <= g_cfg.overlay_text_scale) idx = i;
            idx = (idx + (dir > 0 ? 1 : 4)) % 5;
            g_cfg.overlay_text_scale = steps[idx];
            gl_dialog_log_mark_dirty();
            gl_page_reader_mark_dirty();
            gl_renderer_show_toast("Overlay Text Size: %d%%", g_cfg.overlay_text_scale);
            break;
        }
        case MROW_BIND_RESET:
            bindings_reset_defaults();
            gl_renderer_show_toast("Controls reset to defaults");
            break;
        default:
            if (row_id >= MROW_BIND_FIRST && row_id <= MROW_BIND_LAST) {
                /* Enter / right / click: rebind 1st slot; left / right-click: 2nd slot */
                g_capture_action = row_id - MROW_BIND_FIRST;
                g_capture_slot = (dir < 0) ? 1 : 0;
                g_menu_dirty = 1;
                gl_renderer_redraw_if_needed(1);
                return;
            }
            break;
    }
    gl_config_save(NULL);
    g_menu_dirty = 1;
    gl_renderer_redraw_if_needed(1);
}

static void menu_switch_tab(int new_tab) {
    new_tab = ((new_tab % MENU_TABS) + MENU_TABS) % MENU_TABS;
    g_capture_action = -1;
    if (new_tab == g_menu_tab) return;
    g_menu_tab = new_tab;
    int cnt = menu_active_row_count();
    if (g_menu_sel >= cnt) g_menu_sel = cnt - 1;
    g_menu_dirty = 1;
    gl_renderer_redraw_if_needed(1);
}

static int menu_hit_test(int win_x, int win_y, int *out_dir, int *out_tab_hit) {
    if (out_tab_hit) *out_tab_hit = -1;
    if (g_menu_win_w <= 1.0f || g_menu_win_h <= 1.0f) return -2; /* outside */
    float rx = ((float)win_x - g_menu_win_x) / g_menu_win_w;
    float ry = ((float)win_y - g_menu_win_y) / g_menu_win_h;
    if (rx < 0.0f || rx > 1.0f || ry < 0.0f || ry > 1.0f) return -2; /* outside */
    if (ry >= g_menu_tabs_top_frac && ry <= g_menu_tabs_bot_frac) {
        if (out_tab_hit) {
            int t = (int)(rx * (float)MENU_TABS);
            *out_tab_hit = t < 0 ? 0 : (t >= MENU_TABS ? MENU_TABS - 1 : t);
        }
        return -1;
    }
    int cnt = menu_active_row_count();
    int row = (int)((ry - g_menu_rows_top_frac) / g_menu_row_h_frac);
    if (row < 0 || row >= cnt) return -1;
    if (out_dir) {
        *out_dir = (rx >= 0.52f && rx < 0.74f) ? -1 : 1;
    }
    return row;
}

static void menu_capture_key(const SDL_Event *ev) {
    SDL_Keycode sym = ev->key.keysym.sym;
    int a = g_capture_action;
    if (ev->key.windowID == GL_MENU_PAD_SYNTH) { /* controller navigation: B / Start cancel */
        if (sym == SDLK_ESCAPE) g_capture_action = -1;
        g_menu_dirty = 1;
        gl_renderer_redraw_if_needed(1);
        return;
    }
    if (sym == SDLK_ESCAPE) {
        g_capture_action = -1;
    } else if (sym == SDLK_BACKSPACE || sym == SDLK_DELETE) {
        KeyChord none = { 0, 0 };
        bindings_set_key((BindAction)a, g_capture_slot, none);
        g_capture_action = -1;
        gl_config_save(NULL);
    } else {
        int mod_only = (sym == SDLK_LCTRL || sym == SDLK_RCTRL || sym == SDLK_LALT || sym == SDLK_RALT ||
                        sym == SDLK_LSHIFT || sym == SDLK_RSHIFT || sym == SDLK_LGUI || sym == SDLK_RGUI);
        if (mod_only && !bindings_is_hold((BindAction)a)) return; /* wait for the chord's main key */
        KeyChord c = { (int32_t)sym, mod_only ? 0 : bindings_mods_from_sdl(ev->key.keysym.mod) };
        int r = bindings_set_key((BindAction)a, g_capture_slot, c);
        char name[64];
        bindings_format_chord(&c, name, sizeof(name));
        if (r == -2) {
            gl_renderer_show_toast("%s is reserved", name);
        } else {
            if (r >= 0) gl_renderer_show_toast("%s moved from \"%s\"", name, bindings_action_label((BindAction)r));
            gl_config_save(NULL);
        }
        g_capture_action = -1;
    }
    g_menu_dirty = 1;
    gl_renderer_redraw_if_needed(1);
}

int gl_menu_handle_event(const SDL_Event *ev) {
    if (!g_menu_open || !ev) return 0;
    if (g_capture_action >= 0) {
        if (ev->type == SDL_KEYDOWN) menu_capture_key(ev);
        return 1; /* swallow everything else (mouse, text) while capturing */
    }
    int cnt = menu_active_row_count();
    switch (ev->type) {
        case SDL_KEYDOWN: {
            SDL_Keycode sym = ev->key.keysym.sym;
            if (sym == SDLK_ESCAPE) {
                gl_menu_toggle();
                return 1;
            }
            if (sym == SDLK_TAB || sym == SDLK_e) {
                menu_switch_tab(g_menu_tab + 1);
                return 1;
            }
            if (sym == SDLK_q) {
                menu_switch_tab(g_menu_tab - 1);
                return 1;
            }
            if (sym == SDLK_UP || sym == SDLK_w) {
                g_menu_sel = (g_menu_sel - 1 + cnt) % cnt;
                g_menu_dirty = 1;
                gl_renderer_redraw_if_needed(1);
                return 1;
            }
            if (sym == SDLK_DOWN || sym == SDLK_s) {
                g_menu_sel = (g_menu_sel + 1) % cnt;
                g_menu_dirty = 1;
                gl_renderer_redraw_if_needed(1);
                return 1;
            }
            if (sym == SDLK_LEFT || sym == SDLK_a) {
                menu_apply_delta(menu_active_row_id(g_menu_sel), -1);
                return 1;
            }
            if (sym == SDLK_RIGHT || sym == SDLK_d || sym == SDLK_RETURN || sym == SDLK_SPACE) {
                menu_apply_delta(menu_active_row_id(g_menu_sel), 1);
                return 1;
            }
            return 1;
        }
        case SDL_MOUSEMOTION: {
            int row = menu_hit_test(ev->motion.x, ev->motion.y, NULL, NULL);
            if (row >= 0 && row != g_menu_sel) {
                g_menu_sel = row;
                g_menu_dirty = 1;
                gl_renderer_redraw_if_needed(1);
            }
            return 1;
        }
        case SDL_MOUSEBUTTONDOWN: {
            if (ev->button.button == SDL_BUTTON_LEFT || ev->button.button == SDL_BUTTON_RIGHT) {
                int dir = 1, tab_hit = -1;
                int row = menu_hit_test(ev->button.x, ev->button.y, &dir, &tab_hit);
                if (tab_hit >= 0) {
                    menu_switch_tab(tab_hit);
                } else if (row >= 0) {
                    g_menu_sel = row;
                    if (ev->button.button == SDL_BUTTON_RIGHT) dir = -1;
                    menu_apply_delta(menu_active_row_id(row), dir);
                } else if (row == -2) {
                    gl_menu_toggle();
                }
            }
            return 1;
        }
        case SDL_MOUSEWHEEL: {
            if (ev->wheel.y != 0) {
                menu_apply_delta(menu_active_row_id(g_menu_sel), (ev->wheel.y > 0) ? 1 : -1);
            }
            return 1;
        }
        default:
            return 0;
    }
}

static void render_menu_texture(int draw_w, int draw_h) {
    float scale = (float)draw_h / 960.0f;
    if (scale < 0.65f) scale = 0.65f;
    if (scale > 1.65f) scale = 1.65f;

    int pw = (int)(690.0f * scale);
    int ph = (int)(670.0f * scale);
    if (pw > draw_w - 40) pw = draw_w - 40;
    if (ph > draw_h - 40) ph = draw_h - 40;
    if (pw < 380) pw = 380;
    if (ph < 400) ph = 400;

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, pw, ph, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!surf) return;

    /* Solid dark obsidian backdrop + golden border */
    SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 12, 14, 22, 244));
    Uint32 gold = SDL_MapRGBA(surf->format, 214, 178, 64, 248);
    Uint32 gold_dim = SDL_MapRGBA(surf->format, 120, 96, 36, 220);
    int bw = (int)(2.0f * scale + 0.5f); if (bw < 2) bw = 2;
    SDL_Rect b_top = {0, 0, pw, bw}, b_bot = {0, ph - bw, pw, bw};
    SDL_Rect b_lft = {0, 0, bw, ph}, b_rgt = {pw - bw, 0, bw, ph};
    SDL_FillRect(surf, &b_top, gold);
    SDL_FillRect(surf, &b_bot, gold);
    SDL_FillRect(surf, &b_lft, gold);
    SDL_FillRect(surf, &b_rgt, gold);

    int hdr_h = (int)(ph * 0.095f);
    SDL_Rect hdr_bg = {bw, bw, pw - bw * 2, hdr_h};
    SDL_FillRect(surf, &hdr_bg, SDL_MapRGBA(surf->format, 20, 20, 30, 248));

    TTF_Font *f_title = get_native_font((int)(20.0f * scale + 0.5f), 1);
    TTF_Font *f_tab   = get_native_font((int)(15.0f * scale + 0.5f), 1);
    TTF_Font *f_row   = get_native_font((int)(16.0f * scale + 0.5f), 0);
    TTF_Font *f_val   = get_native_font((int)(16.0f * scale + 0.5f), 1);
    TTF_Font *f_foot  = get_native_font((int)(13.0f * scale + 0.5f), 0);

    SDL_Color col_gold   = { 242, 206, 98, 255 };
    SDL_Color col_label  = { 225, 228, 236, 255 };
    SDL_Color col_val    = { 250, 232, 165, 255 };
    SDL_Color col_sel    = { 255, 245, 210, 255 };
    SDL_Color col_dim    = { 155, 162, 178, 255 };

    if (f_title) {
        char title[96];
        const char *ver = mha2_version();
        int ver_len = (int)strcspn(ver, "-"); /* "v5.5.0-3-gabc1234-dirty" -> "v5.5.0" */
        if (ver[0] != 'v') { ver = "dev"; ver_len = 3; } /* no release tag in the history */
        snprintf(title, sizeof(title), "REMASTER %.*s  -  GRAPHICS & ENGINE SETTINGS", ver_len, ver);
        SDL_Surface *t = TTF_RenderUTF8_Blended(f_title, title, col_gold);
        if (t) {
            SDL_Rect dst = { (pw - t->w) / 2, bw + (hdr_h - t->h) / 2, t->w, t->h };
            SDL_BlitSurface(t, NULL, surf, &dst);
            SDL_FreeSurface(t);
        }
    }

    /* 3-Tab Bar */
    int pad_x = (int)(26.0f * scale);
    int tabs_y = bw + hdr_h + (int)(6.0f * scale);
    int tabs_h = (int)(38.0f * scale);
    int tab_gap = (int)(8.0f * scale);
    int tab_w = (pw - pad_x * 2 - tab_gap * (MENU_TABS - 1)) / MENU_TABS;
    g_menu_tabs_top_frac = (float)tabs_y / (float)ph;
    g_menu_tabs_bot_frac = (float)(tabs_y + tabs_h) / (float)ph;

    const char *tab_names[MENU_TABS] = { "1. DISPLAY", "2. POST-FX", "3. ASSETS & TEXT", "4. CONTROLS" };
    for (int t = 0; t < MENU_TABS; t++) {
        SDL_Rect tr = { pad_x + t * (tab_w + tab_gap), tabs_y, tab_w, tabs_h };
        int act = (t == g_menu_tab);
        SDL_FillRect(surf, &tr, act ? SDL_MapRGBA(surf->format, 76, 60, 24, 248)
                                    : SDL_MapRGBA(surf->format, 24, 27, 40, 244));
        SDL_Rect tb = { tr.x, tr.y + tr.h - 3, tr.w, 3 };
        SDL_FillRect(surf, &tb, act ? gold : gold_dim);
        if (f_tab) {
            SDL_Surface *ts = TTF_RenderUTF8_Blended(f_tab, tab_names[t], act ? col_gold : col_dim);
            if (ts) {
                SDL_Rect dst = { tr.x + (tr.w - ts->w) / 2, tr.y + (tr.h - ts->h) / 2, ts->w, ts->h };
                SDL_BlitSurface(ts, NULL, surf, &dst);
                SDL_FreeSurface(ts);
            }
        }
    }

    int rows_top = tabs_y + tabs_h + (int)(12.0f * scale);
    int foot_h   = (int)(38.0f * scale);
    int avail_h  = ph - rows_top - foot_h - bw;
    int cnt      = menu_active_row_count();
    int row_h    = avail_h / cnt;
    g_menu_rows_top_frac = (float)rows_top / (float)ph;
    g_menu_row_h_frac    = (float)row_h / (float)ph;

    for (int i = 0; i < cnt; i++) {
        int row_id = menu_active_row_id(i);
        int ry = rows_top + i * row_h;
        if (i == g_menu_sel) {
            SDL_Rect hl = { pad_x - 10, ry + 2, pw - (pad_x - 10) * 2, row_h - 4 };
            SDL_FillRect(surf, &hl, SDL_MapRGBA(surf->format, 82, 64, 24, 248));
            SDL_Rect hl_l = { hl.x, hl.y, 4, hl.h };
            SDL_FillRect(surf, &hl_l, gold);
        } else if (i % 2 == 1) {
            SDL_Rect alt = { pad_x - 10, ry + 2, pw - (pad_x - 10) * 2, row_h - 4 };
            SDL_FillRect(surf, &alt, SDL_MapRGBA(surf->format, 24, 27, 40, 244));
        }

        char lbl[96], val[96], val_fmt[112];
        menu_row_label_and_value(row_id, lbl, sizeof(lbl), val, sizeof(val));
        if (row_id == MROW_CLOSE) {
            snprintf(val_fmt, sizeof(val_fmt), "%s", val);
        } else {
            snprintf(val_fmt, sizeof(val_fmt), "<  %s  >", val);
        }

        if (f_row && lbl[0]) {
            SDL_Surface *tl = TTF_RenderUTF8_Blended(f_row, lbl, (i == g_menu_sel) ? col_sel : col_label);
            if (tl) {
                SDL_Rect dst = { pad_x, ry + (row_h - tl->h) / 2, tl->w, tl->h };
                SDL_BlitSurface(tl, NULL, surf, &dst);
                SDL_FreeSurface(tl);
            }
        }
        if (f_val && val_fmt[0]) {
            SDL_Surface *tv = TTF_RenderUTF8_Blended(f_val, val_fmt, (i == g_menu_sel) ? col_gold : col_val);
            if (tv) {
                SDL_Rect dst = { pw - pad_x - tv->w, ry + (row_h - tv->h) / 2, tv->w, tv->h };
                SDL_BlitSurface(tv, NULL, surf, &dst);
                SDL_FreeSurface(tv);
            }
        }
    }

    if (f_foot) {
        const char *hint = (g_capture_action >= 0)
            ? "Press the new key or chord   |   Esc = cancel   |   Backspace = unbind this slot"
            : (g_menu_tab == 3)
            ? "Enter / Click = rebind 1st key   |   Left / Right-Click = 2nd key   |   Controller: pad_* in mha2_settings.ini"
            : "Tab / Click Tabs to switch page   |   Click / Scroll / Arrows to adjust   |   Auto-Saved (mha2_settings.ini)";
        SDL_Surface *tf = TTF_RenderUTF8_Blended(f_foot, hint, col_dim);
        if (tf) {
            /* Shrink hints wider than the panel (long CONTROLS hint, larger text sizes) */
            int max_w = pw - 2 * (bw + 8);
            int w = tf->w, h = tf->h;
            if (max_w > 0 && w > max_w) { h = h * max_w / w; w = max_w; }
            SDL_Rect dst = { (pw - w) / 2, ph - foot_h + (foot_h - h) / 2 - bw, w, h };
            if (w == tf->w) SDL_BlitSurface(tf, NULL, surf, &dst);
            else SDL_BlitScaled(tf, NULL, surf, &dst);
            SDL_FreeSurface(tf);
        }
    }

    if (g_menu_tex) glDeleteTextures(1, &g_menu_tex);
    glGenTextures(1, &g_menu_tex);
    glBindTexture(GL_TEXTURE_2D, g_menu_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
    SDL_FreeSurface(surf);

    g_menu_tex_w = pw;
    g_menu_tex_h = ph;
    g_menu_last_draw_w = draw_w;
    g_menu_last_draw_h = draw_h;
    g_menu_dirty = 0;
}

void gl_menu_draw(int draw_w, int draw_h) {
    if (g_menu_dirty || !g_menu_tex || g_menu_last_draw_w != draw_w || g_menu_last_draw_h != draw_h) {
        render_menu_texture(draw_w, draw_h);
    }
    if (!g_menu_tex) return;
    int px = (draw_w - g_menu_tex_w) / 2;
    int py = (draw_h - g_menu_tex_h) / 2;
    draw_osd_quad(g_menu_tex, px, py, g_menu_tex_w, g_menu_tex_h, draw_w, draw_h, 1.0f);

    int win_w = draw_w, win_h = draw_h;
    if (g_sdl_window) SDL_GetWindowSize(g_sdl_window, &win_w, &win_h);
    float sx = (draw_w > 0) ? ((float)win_w / (float)draw_w) : 1.0f;
    float sy = (draw_h > 0) ? ((float)win_h / (float)draw_h) : 1.0f;
    g_menu_win_x = (float)px * sx;
    g_menu_win_y = (float)py * sy;
    g_menu_win_w = (float)g_menu_tex_w * sx;
    g_menu_win_h = (float)g_menu_tex_h * sy;
}
