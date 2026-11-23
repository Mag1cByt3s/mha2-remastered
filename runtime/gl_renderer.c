#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include "gl_renderer.h"
#include "hd_assets.h"
#include <SDL2/SDL_opengl.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>

ModernConfig g_cfg = {
    .truecolor_32bit    = 1,
    .shader_mode        = SHADER_BICUBIC_CAS,
    .vsync_mode         = VSYNC_ADAPTIVE,
    .max_fps            = 240,
    .start_fullscreen   = 0,
    .init_win_w         = 1280,
    .init_win_h         = 800,
    .show_hud           = 0,
    .sharpen_strength   = 0.65f,
    .master_volume      = 1.0f,
    .muted              = 0,
    .hd_vector_text     = 1,
    .hd_assets_enabled  = 1,
    .hd_sprites_enabled = 1,
    .decoupled_cursor   = 0,
    .aspect_mode        = ASPECT_AUTO,
    .ambient_bars       = 1,
    .bloom_level        = BLOOM_SUBTLE,
    .color_grade        = GRADE_REMASTERED,
    .silhouette_aa      = AA_FXAA,
    .aa_strength        = 0.75f,
    .clarity_strength   = 0.30f,
    .filmic_strength    = 0.25f,
    .deband_dither      = 1,
    .atmosphere_fx      = 1,
    .font_style         = FONT_SANS,
    .text_backdrop      = 0,
    .audio_reverb       = REVERB_SUBTLE,
    .audio_eq           = EQ_WARM,
    .game_speed         = SPEED_1X,
    .hotspot_hints      = 1,
    .vignette_level     = VIGNETTE_SUBTLE,
    .scene_transitions  = 1,
    .video_enhance      = 1,
    .display_index      = 0,
    .hd_assets_dir      = "hd_assets",
};

static char g_ini_path[512] = "mha2_settings.ini";

static int clamp_i(int v, int lo, int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

void gl_config_load(const char *path) {
    if (path && path[0]) snprintf(g_ini_path, sizeof(g_ini_path), "%s", path);
    FILE *f = fopen(g_ini_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        /* Trim trailing whitespace on key & val */
        char *ke = eq - 1;
        while (ke >= p && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';
        while (*val == ' ' || *val == '\t') val++;
        size_t vl = strlen(val);
        while (vl > 0 && (val[vl - 1] == '\n' || val[vl - 1] == '\r' || val[vl - 1] == ' ' || val[vl - 1] == '\t')) {
            val[--vl] = '\0';
        }
        if (strcmp(p, "fullscreen") == 0) g_cfg.start_fullscreen = atoi(val) ? 1 : 0;
        else if (strcmp(p, "win_width") == 0) g_cfg.init_win_w = clamp_i(atoi(val), 640, 7680);
        else if (strcmp(p, "win_height") == 0) g_cfg.init_win_h = clamp_i(atoi(val), 450, 4320);
        else if (strcmp(p, "aspect_mode") == 0) g_cfg.aspect_mode = clamp_i(atoi(val), 0, ASPECT_COUNT - 1);
        else if (strcmp(p, "ambient_bars") == 0) g_cfg.ambient_bars = atoi(val) ? 1 : 0;
        else if (strcmp(p, "shader_mode") == 0) g_cfg.shader_mode = clamp_i(atoi(val), 0, SHADER_COUNT - 1);
        else if (strcmp(p, "sharpen_strength") == 0) {
            float s = (float)atof(val);
            if (s < 0.0f) s = 0.0f;
            if (s > 1.0f) s = 1.0f;
            g_cfg.sharpen_strength = s;
        }
        else if (strcmp(p, "vsync_mode") == 0) g_cfg.vsync_mode = clamp_i(atoi(val), 0, VSYNC_COUNT - 1);
        else if (strcmp(p, "max_fps") == 0) g_cfg.max_fps = clamp_i(atoi(val), 0, 500);
        else if (strcmp(p, "show_hud") == 0) g_cfg.show_hud = atoi(val) ? 1 : 0;
        else if (strcmp(p, "master_volume") == 0) {
            float v = (float)atof(val);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.5f) v = 1.5f;
            g_cfg.master_volume = v;
        }
        else if (strcmp(p, "muted") == 0) g_cfg.muted = atoi(val) ? 1 : 0;
        else if (strcmp(p, "hd_assets_enabled") == 0) g_cfg.hd_assets_enabled = atoi(val) ? 1 : 0;
        else if (strcmp(p, "hd_sprites_enabled") == 0) g_cfg.hd_sprites_enabled = atoi(val) ? 1 : 0;
        else if (strcmp(p, "hd_vector_text") == 0) g_cfg.hd_vector_text = atoi(val) ? 1 : 0;
        else if (strcmp(p, "decoupled_cursor") == 0) g_cfg.decoupled_cursor = atoi(val) ? 1 : 0;
        else if (strcmp(p, "bloom_level") == 0) g_cfg.bloom_level = clamp_i(atoi(val), 0, BLOOM_COUNT - 1);
        else if (strcmp(p, "color_grade") == 0) g_cfg.color_grade = clamp_i(atoi(val), 0, GRADE_COUNT - 1);
        else if (strcmp(p, "silhouette_aa") == 0) g_cfg.silhouette_aa = clamp_i(atoi(val), 0, AA_COUNT - 1);
        else if (strcmp(p, "aa_strength") == 0) {
            float s = (float)atof(val);
            if (s < 0.0f) s = 0.0f;
            if (s > 1.0f) s = 1.0f;
            g_cfg.aa_strength = s;
        }
        else if (strcmp(p, "clarity_strength") == 0) {
            float s = (float)atof(val);
            if (s < 0.0f) s = 0.0f;
            if (s > 1.0f) s = 1.0f;
            g_cfg.clarity_strength = s;
        }
        else if (strcmp(p, "filmic_strength") == 0) {
            float s = (float)atof(val);
            if (s < 0.0f) s = 0.0f;
            if (s > 1.0f) s = 1.0f;
            g_cfg.filmic_strength = s;
        }
        else if (strcmp(p, "filmic_tonemap") == 0) g_cfg.filmic_strength = atoi(val) ? 0.25f : 0.0f;
        else if (strcmp(p, "deband_dither") == 0) g_cfg.deband_dither = atoi(val) ? 1 : 0;
        else if (strcmp(p, "atmosphere_fx") == 0) g_cfg.atmosphere_fx = atoi(val) ? 1 : 0;
        else if (strcmp(p, "font_style") == 0) g_cfg.font_style = clamp_i(atoi(val), 0, FONT_STYLE_COUNT - 1);
        else if (strcmp(p, "text_backdrop") == 0) g_cfg.text_backdrop = atoi(val) ? 1 : 0;
        else if (strcmp(p, "audio_reverb") == 0) g_cfg.audio_reverb = clamp_i(atoi(val), 0, REVERB_COUNT - 1);
        else if (strcmp(p, "audio_eq") == 0) g_cfg.audio_eq = clamp_i(atoi(val), 0, EQ_COUNT - 1);
        else if (strcmp(p, "game_speed") == 0) g_cfg.game_speed = clamp_i(atoi(val), 0, SPEED_COUNT - 1);
        else if (strcmp(p, "hotspot_hints") == 0) g_cfg.hotspot_hints = atoi(val) ? 1 : 0;
        else if (strcmp(p, "vignette_level") == 0) g_cfg.vignette_level = clamp_i(atoi(val), 0, VIGNETTE_COUNT - 1);
        else if (strcmp(p, "scene_transitions") == 0) g_cfg.scene_transitions = atoi(val) ? 1 : 0;
        else if (strcmp(p, "video_enhance") == 0) g_cfg.video_enhance = atoi(val) ? 1 : 0;
        else if (strcmp(p, "display_index") == 0) g_cfg.display_index = clamp_i(atoi(val), 0, 15);
    }
    fclose(f);
}

static int g_fullscreen; /* forward decl for config save */

void gl_config_save(const char *path) {
    const char *p = (path && path[0]) ? path : g_ini_path;
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "# Moorhuhn Adventure 2 — Remaster v5 Graphics & Audio Settings\n");
    fprintf(f, "fullscreen=%d\n", g_sdl_window ? (g_fullscreen ? 1 : 0) : (g_cfg.start_fullscreen ? 1 : 0));
    fprintf(f, "win_width=%d\n", g_cfg.init_win_w);
    fprintf(f, "win_height=%d\n", g_cfg.init_win_h);
    fprintf(f, "aspect_mode=%d\n", g_cfg.aspect_mode);
    fprintf(f, "ambient_bars=%d\n", g_cfg.ambient_bars);
    fprintf(f, "shader_mode=%d\n", g_cfg.shader_mode);
    fprintf(f, "sharpen_strength=%.2f\n", g_cfg.sharpen_strength);
    fprintf(f, "vsync_mode=%d\n", g_cfg.vsync_mode);
    fprintf(f, "max_fps=%d\n", g_cfg.max_fps);
    fprintf(f, "show_hud=%d\n", g_cfg.show_hud);
    fprintf(f, "master_volume=%.2f\n", g_cfg.master_volume);
    fprintf(f, "muted=%d\n", g_cfg.muted);
    fprintf(f, "hd_assets_enabled=%d\n", g_cfg.hd_assets_enabled);
    fprintf(f, "hd_sprites_enabled=%d\n", g_cfg.hd_sprites_enabled);
    fprintf(f, "hd_vector_text=%d\n", g_cfg.hd_vector_text);
    fprintf(f, "decoupled_cursor=%d\n", g_cfg.decoupled_cursor);
    fprintf(f, "bloom_level=%d\n", g_cfg.bloom_level);
    fprintf(f, "color_grade=%d\n", g_cfg.color_grade);
    fprintf(f, "silhouette_aa=%d\n", g_cfg.silhouette_aa);
    fprintf(f, "aa_strength=%.2f\n", g_cfg.aa_strength);
    fprintf(f, "clarity_strength=%.2f\n", g_cfg.clarity_strength);
    fprintf(f, "filmic_strength=%.2f\n", g_cfg.filmic_strength);
    fprintf(f, "deband_dither=%d\n", g_cfg.deband_dither);
    fprintf(f, "atmosphere_fx=%d\n", g_cfg.atmosphere_fx);
    fprintf(f, "font_style=%d\n", g_cfg.font_style);
    fprintf(f, "text_backdrop=%d\n", g_cfg.text_backdrop);
    fprintf(f, "audio_reverb=%d\n", g_cfg.audio_reverb);
    fprintf(f, "audio_eq=%d\n", g_cfg.audio_eq);
    fprintf(f, "game_speed=%d\n", g_cfg.game_speed);
    fprintf(f, "hotspot_hints=%d\n", g_cfg.hotspot_hints);
    fprintf(f, "vignette_level=%d\n", g_cfg.vignette_level);
    fprintf(f, "scene_transitions=%d\n", g_cfg.scene_transitions);
    fprintf(f, "video_enhance=%d\n", g_cfg.video_enhance);
    fprintf(f, "display_index=%d\n", g_cfg.display_index);
    fclose(f);
}

/* ---- Dynamic GL 3.3 function pointers (no GLEW/GLAD needed) ---- */
static PFNGLCREATESHADERPROC            p_glCreateShader;
static PFNGLSHADERSOURCEPROC            p_glShaderSource;
static PFNGLCOMPILESHADERPROC           p_glCompileShader;
static PFNGLGETSHADERIVPROC             p_glGetShaderiv;
static PFNGLGETSHADERINFOLOGPROC        p_glGetShaderInfoLog;
static PFNGLDELETESHADERPROC            p_glDeleteShader;
static PFNGLCREATEPROGRAMPROC           p_glCreateProgram;
static PFNGLATTACHSHADERPROC            p_glAttachShader;
static PFNGLLINKPROGRAMPROC             p_glLinkProgram;
static PFNGLGETPROGRAMIVPROC            p_glGetProgramiv;
static PFNGLGETPROGRAMINFOLOGPROC       p_glGetProgramInfoLog;
static PFNGLDELETEPROGRAMPROC           p_glDeleteProgram;
static PFNGLUSEPROGRAMPROC              p_glUseProgram;
static PFNGLGETUNIFORMLOCATIONPROC      p_glGetUniformLocation;
static PFNGLUNIFORM1IPROC               p_glUniform1i;
static PFNGLUNIFORM1FPROC               p_glUniform1f;
static PFNGLUNIFORM2FPROC               p_glUniform2f;
static PFNGLUNIFORM4FPROC               p_glUniform4f;
static PFNGLGENVERTEXARRAYSPROC         p_glGenVertexArrays;
static PFNGLBINDVERTEXARRAYPROC         p_glBindVertexArray;
static PFNGLDELETEVERTEXARRAYSPROC      p_glDeleteVertexArrays;
static PFNGLACTIVETEXTUREPROC           p_glActiveTexture;
static PFNGLGENERATEMIPMAPPROC          p_glGenerateMipmap;
static PFNGLGENFRAMEBUFFERSPROC         p_glGenFramebuffers;
static PFNGLDELETEFRAMEBUFFERSPROC      p_glDeleteFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC         p_glBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC    p_glFramebufferTexture2D;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC  p_glCheckFramebufferStatus;
static PFNGLBLENDFUNCSEPARATEPROC       p_glBlendFuncSeparate;

static int load_gl_procs(void) {
#define LOAD(fn) do { p_##fn = (typeof(p_##fn))SDL_GL_GetProcAddress(#fn); if (!p_##fn) return 0; } while (0)
    LOAD(glCreateShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glDeleteShader);
    LOAD(glCreateProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glGetProgramiv);
    LOAD(glGetProgramInfoLog);
    LOAD(glDeleteProgram);
    LOAD(glUseProgram);
    LOAD(glGetUniformLocation);
    LOAD(glUniform1i);
    LOAD(glUniform1f);
    LOAD(glUniform2f);
    LOAD(glUniform4f);
    LOAD(glGenVertexArrays);
    LOAD(glBindVertexArray);
    LOAD(glDeleteVertexArrays);
    LOAD(glActiveTexture);
    LOAD(glGenerateMipmap);
    LOAD(glGenFramebuffers);
    LOAD(glDeleteFramebuffers);
    LOAD(glBindFramebuffer);
    LOAD(glFramebufferTexture2D);
    LOAD(glCheckFramebufferStatus);
    LOAD(glBlendFuncSeparate);
#undef LOAD
    return 1;
}

static SDL_GLContext g_gl_ctx = NULL;
static pthread_t g_gl_thread;
static int g_gl_thread_valid = 0;
static int g_gl_active = 0;
static int g_has_frame = 0;
static int g_fullscreen = 0;
static GLuint g_vao = 0;
static GLuint g_prog_upscale = 0;
static GLuint g_prog_sidebar = 0;
static GLuint g_prog_osd = 0;
static GLuint g_prog_solid = 0;
static GLuint g_prog_bloom_ext = 0;
static GLuint g_prog_bloom_blur = 0;
static GLuint g_prog_post = 0;
static GLuint g_frame_tex = 0;
static int g_frame_w = 0, g_frame_h = 0, g_frame_bpp = 0;
static int g_game_w = 800, g_game_h = 600;

/* ---- v5 Offscreen Post-Processing FBOs & Scene Crossfade State ---- */
static GLuint g_scene_fbo[2] = { 0, 0 };
static GLuint g_scene_tex[2] = { 0, 0 };
static int g_scene_cur_idx = 0;
static int g_scene_fbo_w = 0, g_scene_fbo_h = 0;
static int g_scene_has_rendered = 0;
static GLuint g_bloom_fbo[2] = { 0, 0 };
static GLuint g_bloom_tex[2] = { 0, 0 };
static int g_bloom_w = 0, g_bloom_h = 0;
static int g_post_fbos_ok = 0;

/* ---- v5 Persistent 4K GPU DirectDraw Canvas FBOs (0=primary, 1=backbuffer) ---- */
#define HD_CANVAS_SCALE 4
static GLuint g_hd_canvas_fbo[2] = { 0, 0 };
static GLuint g_hd_canvas_tex[2] = { 0, 0 };
static int g_hd_canvas_gw = 0, g_hd_canvas_gh = 0;
static int g_hd_canvas_tw = 0, g_hd_canvas_th = 0;
static int g_hd_canvas_ready = 0;
static int g_hd_canvas_present_slot = 0;
static int g_hd_canvas_has_draws = 0;
static GLuint g_patch_tex = 0;
static int g_patch_tex_w = 0, g_patch_tex_h = 0;

/* Procedural clean-room 3D gold "REMASTERED" Main Menu title overlay */
static int    g_bg_is_main_menu = 0;
static GLuint g_remastered_badge_tex = 0;
static int    g_remastered_badge_ink_w = 0;
static int    g_remastered_badge_ink_h = 0;
static int    g_remastered_badge_pad = 0;
static int    g_remastered_badge_tried = 0;

/* Smooth GPU crossfade transition timer */
static int g_trans_pending = 0;
static int g_trans_active = 0;
static uint64_t g_trans_start_ns = 0;
static const uint64_t TRANS_DURATION_NS = 210000000ull; /* 210 ms cubic ease-out */

/* ---- v5 LRU GL Texture Cache for 4x HD Sprites & Animations ---- */
#define MAX_SPRITE_GL_CACHE 512
typedef struct SpriteGlEntry {
    int in_use;
    int sprite_id;
    uint32_t last_used;
    GLuint tex;
    int tex_w;
    int tex_h;
} SpriteGlEntry;
static SpriteGlEntry g_spr_gl_cache[MAX_SPRITE_GL_CACHE];
static uint32_t g_spr_gl_tick = 1;
static int g_spr_gl_resident = 0;

/* Cached viewport in window coordinates & active widescreen layout */
enum ActiveLayoutMode {
    LAYOUT_4_3   = 0, /* 800x600 full framebuffer */
    LAYOUT_16_10 = 1, /* 800x500: 800x450 room + 50px bottom inventory dock + top overlay */
    LAYOUT_16_9  = 2, /* 800x450: 800x450 room + top & bottom overlays */
};
static int   g_scene_is_widescreen = 0;
static int   g_cur_layout = LAYOUT_4_3;
static float g_vp_win_x = 0.0f, g_vp_win_y = 0.0f, g_vp_win_w = 800.0f, g_vp_win_h = 600.0f;
static uint8_t g_top_bar_mask[800 * 75]; /* non-black mask of y=0..74 for smart click routing */
static uint8_t g_bot_bar_mask[800 * 50]; /* non-black mask of y=537..586 for 16:9 inventory hits */
static int     g_bot_bar_dialog_mode = 0; /* 1 when multi-line dialog choices span y=500..599 */
static int     g_top_bar_has_buttons = 0; /* 1 when top corners contain minigame Exit/Help controls */

/* ---- v3 HD Background Replacement State ---- */
static GLuint g_bg_orig_tex = 0;
static GLuint g_bg_hd_tex = 0;
static GLuint g_bg_wide_tex = 0;
static int g_bg_wide_w = 0, g_bg_wide_h = 0, g_bg_wide_has = 0;
static int g_bg_hd_id = -1;
static int g_bg_hd_w = 0, g_bg_hd_h = 0;
static int g_bg_orig_w = 0, g_bg_orig_h = 0, g_bg_orig_bpp = 0;
static int g_bg_dst_x = 0, g_bg_dst_y = 0;
static const void *g_bg_orig_ptr = NULL;
static int g_bg_active = 0;

/* ---- v3 Native High-Res Vector Text Cache ---- */
static GlTextDraw g_frame_texts[MAX_FRAME_TEXT_DRAWS];
static int g_frame_text_count = 0;
static char g_ttf_font_path[1024] = "";
static char g_game_ttf_path[1024] = "";
static int  g_game_ttf_is_bold_file = 0;
static char g_serif_ttf_path[1024] = "";
static int  g_serif_ttf_is_bold_file = 0;

#define MAX_FONT_CACHE 32
typedef struct FontCacheEntry {
    int px_size;
    int bold;
    TTF_Font *font;
} FontCacheEntry;
static FontCacheEntry g_font_cache[MAX_FONT_CACHE];

#define MAX_CANVAS_TEXT_CACHE 128
typedef struct CanvasTextEntry {
    int in_use;
    uint32_t last_used;
    int px_size;
    int outline_px;
    uint32_t fill_rgb;
    uint32_t outline_rgb;
    char utf8[256];
    GLuint tex;
    int tex_w;
    int tex_h;
} CanvasTextEntry;
static CanvasTextEntry g_canvas_text_cache[MAX_CANVAS_TEXT_CACHE];
static uint32_t g_canvas_text_tick = 1;

#define MAX_TEXT_TEX_CACHE 96
typedef struct TextTexEntry {
    int in_use;
    uint32_t last_used;
    int px_size;
    int bold;
    uint32_t text_color;
    uint32_t bk_color;
    int bk_mode;
    char utf8[256];
    GLuint tex;
    int tex_w;
    int tex_h;
} TextTexEntry;
static TextTexEntry g_text_cache[MAX_TEXT_TEX_CACHE];
static uint32_t g_text_cache_tick = 1;

/* ---- v3 Decoupled High-Refresh Cursor State ---- */
static GLuint g_cursor_tex = 0;
static int g_cursor_w = 0, g_cursor_h = 0;         /* logical game-space cursor dims */
static int g_cursor_tex_w = 0, g_cursor_tex_h = 0; /* GL texture pixel dims (1x or 4x) */
static int g_cursor_hot_x = 0, g_cursor_hot_y = 0;
static uint64_t g_cursor_hash = 0;
static int g_cursor_ready = 0;
static float g_live_mouse_x = 400.0f, g_live_mouse_y = 300.0f;
static int g_live_mouse_visible = 0;
static int g_mouse_moved_since_draw = 0;

/* OSD / HUD state */
static TTF_Font *g_osd_font = NULL;
static GLuint g_hud_tex = 0;
static int g_hud_w = 0, g_hud_h = 0;
static uint32_t g_hud_last_update = 0;

static GLuint g_toast_tex = 0;
static int g_toast_w = 0, g_toast_h = 0;
static uint32_t g_toast_until = 0;
static char g_toast_text[256] = "";
static int g_toast_dirty = 0;

/* Frame pacing & FPS stats */
static uint64_t g_last_present_ns = 0;
static uint32_t g_fps_window_start = 0;
static int g_fps_frames = 0;
static float g_cur_fps = 0.0f;
static float g_cur_ms = 0.0f;

/* Fallback SDL_Renderer (used when SDL_VIDEODRIVER=dummy) */
static SDL_Texture *g_fb_tex = NULL;
static int g_fb_w = 0, g_fb_h = 0;
static uint32_t g_fb_fmt = 0;

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *gl_aa_name(int mode) {
    switch (mode) {
        case AA_FXAA:  return "FXAA 3.11";
        case AA_SMAA:  return "SMAA 1x (Crisp)";
        case AA_MULTI: return "SMAA + 3x Edge AA";
        default:       return "OFF";
    }
}

static const char *gl_font_style_name(int style) {
    return (style == FONT_SERIF) ? "Adventure Serif" : "Clean Sans";
}

static const char *gl_reverb_name(int mode) {
    switch (mode) {
        case REVERB_SUBTLE: return "Subtle Castle";
        case REVERB_RICH:   return "Rich Hall";
        default:            return "OFF";
    }
}

static const char *gl_audio_eq_name(int mode) {
    switch (mode) {
        case EQ_WARM:   return "Warm Remaster";
        case EQ_STUDIO: return "Studio Crisp";
        default:        return "OFF";
    }
}

static const char *gl_speed_name(int sp) {
    switch (sp) {
        case SPEED_15X: return "1.5x Fast";
        case SPEED_2X:  return "2.0x Turbo";
        default:        return "1.0x Normal";
    }
}

/* ---- QoL, Showcase, Dialog Log & 4K Screenshot State ---- */
static volatile int g_fast_walk_held = 0;
static volatile int g_hotspot_active = 0;
static int g_compare_split_on = 0;
static volatile int g_screenshot_pending = 0;
static void gl_dialog_log_draw(int draw_w, int draw_h);

void gl_renderer_set_fast_walk(int active) {
    g_fast_walk_held = active ? 1 : 0;
}

float gl_renderer_get_speed_mult(void) {
    if (g_fast_walk_held) return 2.0f;
    if (g_cfg.game_speed == SPEED_15X) return 1.5f;
    if (g_cfg.game_speed == SPEED_2X)  return 2.0f;
    return 1.0f;
}

void gl_renderer_set_hotspot_highlight(int active) {
    int nv = active ? 1 : 0;
    if (nv != g_hotspot_active) {
        g_hotspot_active = nv;
        gl_renderer_redraw_if_needed(1);
    }
}

void gl_renderer_toggle_compare_split(void) {
    g_compare_split_on = !g_compare_split_on;
    gl_renderer_show_toast("Before/After Split-Screen: %s",
                           g_compare_split_on ? "ON (Move Mouse to Wipe)" : "OFF");
    gl_renderer_redraw_if_needed(1);
}

void gl_renderer_request_screenshot(void) {
    g_screenshot_pending = 1;
    gl_renderer_redraw_if_needed(1);
}

static void save_clean_screenshot_png(int vx, int vy, int vw, int vh) {
    if (vw <= 0 || vh <= 0) return;
    size_t row_bytes = (size_t)vw * 3u;
    uint8_t *raw = (uint8_t *)malloc(row_bytes * (size_t)vh);
    uint8_t *flipped = (uint8_t *)malloc(row_bytes * (size_t)vh);
    if (!raw || !flipped) { free(raw); free(flipped); return; }

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(vx, vy, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, raw);
    for (int y = 0; y < vh; y++) {
        memcpy(flipped + (size_t)y * row_bytes, raw + (size_t)(vh - 1 - y) * row_bytes, row_bytes);
    }
    free(raw);

    mkdir("screenshots", 0755);
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char fname[256];
    snprintf(fname, sizeof(fname), "screenshots/mha2_%04d%02d%02d_%02d%02d%02d.png",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int saved = 0;
    if (codec && ctx && frame && pkt) {
        ctx->width = vw;
        ctx->height = vh;
        ctx->pix_fmt = AV_PIX_FMT_RGB24;
        ctx->time_base = (AVRational){1, 1};
        if (avcodec_open2(ctx, codec, NULL) == 0) {
            frame->format = AV_PIX_FMT_RGB24;
            frame->width = vw;
            frame->height = vh;
            av_image_fill_arrays(frame->data, frame->linesize, flipped, AV_PIX_FMT_RGB24, vw, vh, 1);
            if (avcodec_send_frame(ctx, frame) == 0 && avcodec_receive_packet(ctx, pkt) == 0) {
                FILE *f = fopen(fname, "wb");
                if (f) {
                    fwrite(pkt->data, 1, (size_t)pkt->size, f);
                    fclose(f);
                    saved = 1;
                }
            }
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    free(flipped);

    if (saved) {
        gl_renderer_show_toast("4K Screenshot saved: %s", fname);
    } else {
        gl_renderer_show_toast("Screenshot failed!");
    }
}

const char *gl_shader_name(int mode) {
    switch (mode) {
        case SHADER_BICUBIC_CAS: return "Bicubic + CAS (Remaster)";
        case SHADER_LANCZOS_CAS: return "Lanczos3 + CAS";
        case SHADER_EDGE_SMOOTH: return "Edge-Smooth (xBR-style)";
        case SHADER_BILINEAR:    return "Bilinear";
        case SHADER_NEAREST:     return "Crisp Nearest";
        case SHADER_CRT:         return "Subtle CRT";
        case SHADER_FSR1:        return "AMD FSR 1.0 (EASU + RCAS)";
        default:                 return "Unknown";
    }
}

const char *gl_vsync_name(int mode) {
    switch (mode) {
        case VSYNC_ADAPTIVE: return "Adaptive VSync";
        case VSYNC_ON:       return "VSync On";
        case VSYNC_UNCAPPED: return "High-FPS Cap";
        default:             return "Unknown";
    }
}

const char *gl_aspect_name(int mode) {
    switch (mode) {
        case ASPECT_AUTO:  return "Auto (Smart)";
        case ASPECT_16_10: return "16:10 Smart";
        case ASPECT_16_9:  return "16:9 Cinema";
        case ASPECT_4_3:   return "4:3 Original";
        default:           return "Unknown";
    }
}

const char *gl_bloom_name(int mode) {
    switch (mode) {
        case BLOOM_OFF:    return "Off";
        case BLOOM_SUBTLE: return "Subtle (Torch Glow)";
        case BLOOM_MEDIUM: return "Medium";
        case BLOOM_RICH:   return "Rich Cinematic";
        default:           return "Unknown";
    }
}

const char *gl_grade_name(int mode) {
    switch (mode) {
        case GRADE_ORIGINAL:   return "Original (Neutral)";
        case GRADE_REMASTERED: return "Remastered Rich";
        case GRADE_WARM:       return "Cinematic Warm";
        case GRADE_DAYLIGHT:   return "Crisp Daylight";
        default:               return "Unknown";
    }
}

const char *gl_vignette_name(int mode) {
    switch (mode) {
        case VIGNETTE_OFF:    return "Off";
        case VIGNETTE_SUBTLE: return "Subtle";
        case VIGNETTE_MEDIUM: return "Medium";
        default:              return "Unknown";
    }
}

/* ---- GLSL 330 Shaders (Upscaling + Native HD Background + Widescreen Compositor) ---- */
static const char *VS_FULLSCREEN =
    "#version 330 core\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "  vec2 pos = vec2((gl_VertexID == 1) ? 3.0 : -1.0,\n"
    "                  (gl_VertexID == 2) ? 3.0 : -1.0);\n"
    "  vUV = vec2((pos.x + 1.0) * 0.5, 1.0 - (pos.y + 1.0) * 0.5);\n"
    "  gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char *FS_SIDEBAR =
    "#version 330 core\n"
    "uniform sampler2D uTex;\n"
    "in vec2 vUV;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "  vec3 col = vec3(0.0);\n"
    "  float wsum = 0.0;\n"
    "  for (int y = -2; y <= 2; y++) {\n"
    "    for (int x = -2; x <= 2; x++) {\n"
    "      float w = exp(-0.18 * float(x*x + y*y));\n"
    "      vec2 uv = clamp(vUV + vec2(float(x), float(y)) * vec2(0.018, 0.024), 0.01, 0.99);\n"
    "      col += texture(uTex, uv).rgb * w;\n"
    "      wsum += w;\n"
    "    }\n"
    "  }\n"
    "  col /= wsum;\n"
    "  vec2 d = vUV * 2.0 - 1.0;\n"
    "  float vig = clamp(1.0 - 0.35 * dot(d, d), 0.35, 1.0);\n"
    "  FragColor = vec4(col * 0.24 * vig, 1.0);\n"
    "}\n";

static const char *FS_UPSCALE =
    "#version 330 core\n"
    "uniform sampler2D uTex;\n"
    "uniform sampler2D uBgOrigTex;\n"
    "uniform sampler2D uBgHdTex;\n"
    "uniform vec2 uSrcSize;\n"
    "uniform vec2 uTexSize;\n"
    "uniform vec2 uDstSize;\n"
    "uniform vec4 uBgRect; /* x0, y0, w, h in uSrcSize pixels */\n"
    "uniform int uMode;\n"
    "uniform float uSharp;\n"
    "uniform int uHasHdBg;\n"
    "uniform int uUseHdCanvas;\n"
    "uniform int uLayout; /* 0=4:3 (800x600), 1=16:10 (800x500), 2=16:9 (800x450) */\n"
    "uniform int uDialogBar; /* 1 when multi-line dialog choices occupy y=500..599 */\n"
    "in vec2 vUV;\n"
    "out vec4 FragColor;\n"
    "\n"
    "vec3 sampleTex(vec2 uv) {\n"
    "  return texture(uTex, clamp(uv, vec2(0.5)/uTexSize, vec2(1.0) - vec2(0.5)/uTexSize)).rgb;\n"
    "}\n"
    "\n"
    "vec4 catmullWeights(float f) {\n"
    "  float f2 = f * f;\n"
    "  float f3 = f2 * f;\n"
    "  return 0.5 * vec4(\n"
    "    -f + 2.0*f2 - f3,\n"
    "    2.0 - 5.0*f2 + 3.0*f3,\n"
    "    f + 4.0*f2 - 3.0*f3,\n"
    "    -f2 + f3\n"
    "  );\n"
    "}\n"
    "\n"
    "vec3 sampleBicubic(vec2 uv) {\n"
    "  vec2 p = uv * uSrcSize - 0.5;\n"
    "  vec2 f = fract(p);\n"
    "  vec2 base = (floor(p) + 0.5) / uSrcSize;\n"
    "  vec2 inv = 1.0 / uSrcSize;\n"
    "  vec4 wx = catmullWeights(f.x);\n"
    "  vec4 wy = catmullWeights(f.y);\n"
    "  vec3 col = vec3(0.0);\n"
    "  for (int y = -1; y <= 2; y++) {\n"
    "    vec3 row = vec3(0.0);\n"
    "    for (int x = -1; x <= 2; x++) {\n"
    "      row += sampleTex(base + vec2(float(x), float(y)) * inv) * wx[x + 1];\n"
    "    }\n"
    "    col += row * wy[y + 1];\n"
    "  }\n"
    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"
    "\n"
    "float sinc(float x) {\n"
    "  if (abs(x) < 1e-5) return 1.0;\n"
    "  float px = 3.14159265 * x;\n"
    "  return sin(px) / px;\n"
    "}\n"
    "float lanczos3(float x) {\n"
    "  if (abs(x) >= 3.0) return 0.0;\n"
    "  return sinc(x) * sinc(x / 3.0);\n"
    "}\n"
    "vec3 sampleLanczos3(vec2 uv) {\n"
    "  vec2 p = uv * uSrcSize - 0.5;\n"
    "  vec2 f = fract(p);\n"
    "  vec2 base = (floor(p) + 0.5) / uSrcSize;\n"
    "  vec2 inv = 1.0 / uSrcSize;\n"
    "  float wx[6]; float wy[6];\n"
    "  float sx = 0.0, sy = 0.0;\n"
    "  for (int i = 0; i < 6; i++) {\n"
    "    wx[i] = lanczos3(float(i - 2) - f.x); sx += wx[i];\n"
    "    wy[i] = lanczos3(float(i - 2) - f.y); sy += wy[i];\n"
    "  }\n"
    "  vec3 col = vec3(0.0);\n"
    "  for (int y = 0; y < 6; y++) {\n"
    "    vec3 row = vec3(0.0);\n"
    "    for (int x = 0; x < 6; x++) {\n"
    "      row += sampleTex(base + vec2(float(x - 2), float(y - 2)) * inv) * wx[x];\n"
    "    }\n"
    "    col += row * wy[y];\n"
    "  }\n"
    "  return clamp(col / (sx * sy), 0.0, 1.0);\n"
    "}\n"
    "\n"
    "/* AMD FidelityFX-style Contrast Adaptive Sharpening (CAS) */\n"
    "vec3 applyCAS(vec3 c, vec2 uv, float sharpness) {\n"
    "  vec2 inv = 1.0 / uSrcSize;\n"
    "  vec3 n = sampleTex(uv + vec2(0.0, -inv.y));\n"
    "  vec3 s = sampleTex(uv + vec2(0.0,  inv.y));\n"
    "  vec3 w = sampleTex(uv + vec2(-inv.x, 0.0));\n"
    "  vec3 e = sampleTex(uv + vec2( inv.x, 0.0));\n"
    "  vec3 mn = min(c, min(min(n, s), min(w, e)));\n"
    "  vec3 mx = max(c, max(max(n, s), max(w, e)));\n"
    "  vec3 amp = clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0);\n"
    "  amp = sqrt(amp);\n"
    "  float peak = -1.0 / mix(8.0, 5.0, sharpness);\n"
    "  vec3 wt = amp * peak;\n"
    "  vec3 outc = (c + (n + s + w + e) * wt) / (1.0 + 4.0 * wt);\n"
    "  return clamp(outc, 0.0, 1.0);\n"
    "}\n"
    "\n"
    "/* 4x4 Catmull-Rom supersampling + native-res FidelityFX CAS for 4K canvas */\n"
    "vec3 sampleHdCanvas(vec2 uv) {\n"
    "  vec2 p = uv * uTexSize - 0.5;\n"
    "  vec2 f = fract(p);\n"
    "  vec2 base = (floor(p) + 0.5) / uTexSize;\n"
    "  vec2 inv = 1.0 / uTexSize;\n"
    "  vec4 wx = catmullWeights(f.x);\n"
    "  vec4 wy = catmullWeights(f.y);\n"
    "  vec3 col = vec3(0.0);\n"
    "  for (int y = -1; y <= 2; y++) {\n"
    "    vec3 row = vec3(0.0);\n"
    "    for (int x = -1; x <= 2; x++) {\n"
    "      row += sampleTex(base + vec2(float(x), float(y)) * inv) * wx[x + 1];\n"
    "    }\n"
    "    col += row * wy[y + 1];\n"
    "  }\n"
    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"
    "\n"
    "vec3 applyCASNative(vec3 c, vec2 uv, vec2 dispStep, float sharpness) {\n"
    "  vec3 n1 = sampleTex(uv + vec2(0.0, -dispStep.y));\n"
    "  vec3 s1 = sampleTex(uv + vec2(0.0,  dispStep.y));\n"
    "  vec3 w1 = sampleTex(uv + vec2(-dispStep.x, 0.0));\n"
    "  vec3 e1 = sampleTex(uv + vec2( dispStep.x, 0.0));\n"
    "  vec2 s2step = dispStep * 2.0;\n"
    "  vec3 n2 = sampleTex(uv + vec2(0.0, -s2step.y));\n"
    "  vec3 s2 = sampleTex(uv + vec2(0.0,  s2step.y));\n"
    "  vec3 w2 = sampleTex(uv + vec2(-s2step.x, 0.0));\n"
    "  vec3 e2 = sampleTex(uv + vec2( s2step.x, 0.0));\n"
    "  vec3 mn = min(c, min(min(n1, s1), min(w1, e1)));\n"
    "  vec3 mx = max(c, max(max(n1, s1), max(w1, e1)));\n"
    "  vec3 amp = clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0);\n"
    "  amp = sqrt(amp);\n"
    "  vec3 d1 = c - 0.25 * (n1 + s1 + w1 + e1);\n"
    "  vec3 d2 = c - 0.25 * (n2 + s2 + w2 + e2);\n"
    "  vec3 outc = c + (d1 * 1.8 + d2 * 1.5) * (amp * clamp(sharpness, 0.0, 1.0));\n"
    "  return clamp(outc, 0.0, 1.0);\n"
    "}\n"
    "\n"
    "/* Edge-directed smooth upscaler for 2D sprites & hand-painted art */\n"
    "vec3 sampleEdgeSmooth(vec2 uv) {\n"
    "  vec2 inv = 1.0 / uSrcSize;\n"
    "  vec3 c  = sampleBicubic(uv);\n"
    "  vec3 nw = sampleTex(uv + vec2(-inv.x, -inv.y));\n"
    "  vec3 ne = sampleTex(uv + vec2( inv.x, -inv.y));\n"
    "  vec3 sw = sampleTex(uv + vec2(-inv.x,  inv.y));\n"
    "  vec3 se = sampleTex(uv + vec2( inv.x,  inv.y));\n"
    "  float d1 = length(nw - se);\n"
    "  float d2 = length(ne - sw);\n"
    "  float w = clamp(abs(d1 - d2) * 2.5, 0.0, 0.45);\n"
    "  vec3 diag = (d1 < d2) ? 0.5 * (nw + se) : 0.5 * (ne + sw);\n"
    "  vec3 blended = mix(c, diag, w * 0.55);\n"
    "  return applyCAS(blended, uv, uSharp * 0.85);\n"
    "}\n"
    "\n"
    "/* AMD FidelityFX FSR 1.0 — EASU (12-tap Edge-Adaptive Spatial Upsampling) */\n"
    "float fsrLuma(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
    "\n"
    "void fsrEasuSet(inout vec2 dir, inout float len, float w,\n"
    "                float lA, float lB, float lC, float lD, float lE) {\n"
    "  float lenX = max(abs(lD - lC), abs(lC - lB));\n"
    "  float dirX = lD - lB;\n"
    "  dir.x += dirX * w;\n"
    "  lenX = clamp(abs(dirX) / max(lenX, 1e-4), 0.0, 1.0);\n"
    "  lenX *= lenX;\n"
    "  len += lenX * w;\n"
    "  float lenY = max(abs(lE - lC), abs(lC - lA));\n"
    "  float dirY = lE - lA;\n"
    "  dir.y += dirY * w;\n"
    "  lenY = clamp(abs(dirY) / max(lenY, 1e-4), 0.0, 1.0);\n"
    "  lenY *= lenY;\n"
    "  len += lenY * w;\n"
    "}\n"
    "\n"
    "void fsrEasuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir,\n"
    "                vec2 len2, float lob, float clp, vec3 c) {\n"
    "  vec2 v = vec2(off.x * dir.x + off.y * dir.y,\n"
    "                off.x * (-dir.y) + off.y * dir.x);\n"
    "  v *= len2;\n"
    "  float d2 = min(dot(v, v), clp);\n"
    "  float wB = 0.4 * d2 - 1.0;\n"
    "  float wA = lob * d2 - 1.0;\n"
    "  wB *= wB; wA *= wA;\n"
    "  wB = 1.5625 * wB - 0.5625;\n"
    "  float w = wB * wA;\n"
    "  aC += c * w;\n"
    "  aW += w;\n"
    "}\n"
    "\n"
    "vec3 sampleFsrEasu(vec2 uv, vec2 texSize) {\n"
    "  vec2 pp = uv * texSize - 0.5;\n"
    "  vec2 fp = floor(pp);\n"
    "  vec2 f  = pp - fp;\n"
    "  vec2 inv = 1.0 / texSize;\n"
    "  vec2 p0 = (fp + 0.5) * inv;\n"
    "  /* 12-tap FSR 1.0 EASU footprint */\n"
    "  vec3 b = sampleTex(p0 + vec2( 0.0, -1.0) * inv);\n"
    "  vec3 c = sampleTex(p0 + vec2( 1.0, -1.0) * inv);\n"
    "  vec3 e = sampleTex(p0 + vec2(-1.0,  0.0) * inv);\n"
    "  vec3 f0 = sampleTex(p0);\n"
    "  vec3 g = sampleTex(p0 + vec2( 1.0,  0.0) * inv);\n"
    "  vec3 h = sampleTex(p0 + vec2( 2.0,  0.0) * inv);\n"
    "  vec3 i = sampleTex(p0 + vec2(-1.0,  1.0) * inv);\n"
    "  vec3 j = sampleTex(p0 + vec2( 0.0,  1.0) * inv);\n"
    "  vec3 k = sampleTex(p0 + vec2( 1.0,  1.0) * inv);\n"
    "  vec3 l = sampleTex(p0 + vec2( 2.0,  1.0) * inv);\n"
    "  vec3 n = sampleTex(p0 + vec2( 0.0,  2.0) * inv);\n"
    "  vec3 o = sampleTex(p0 + vec2( 1.0,  2.0) * inv);\n"
    "  float bL = fsrLuma(b), cL = fsrLuma(c), eL = fsrLuma(e), fL = fsrLuma(f0);\n"
    "  float gL = fsrLuma(g), hL = fsrLuma(h), iL = fsrLuma(i), jL = fsrLuma(j);\n"
    "  float kL = fsrLuma(k), lL = fsrLuma(l), nL = fsrLuma(n), oL = fsrLuma(o);\n"
    "  vec2 dir = vec2(0.0);\n"
    "  float len = 0.0;\n"
    "  fsrEasuSet(dir, len, (1.0 - f.x) * (1.0 - f.y), bL, eL, fL, gL, jL);\n"
    "  fsrEasuSet(dir, len,        f.x  * (1.0 - f.y), cL, fL, gL, hL, kL);\n"
    "  fsrEasuSet(dir, len, (1.0 - f.x) *        f.y , fL, iL, jL, kL, nL);\n"
    "  fsrEasuSet(dir, len,        f.x  *        f.y , gL, jL, kL, lL, oL);\n"
    "  float dirR = dot(dir, dir);\n"
    "  if (dirR < 1.0 / 32768.0) {\n"
    "    dir = vec2(1.0, 0.0);\n"
    "  } else {\n"
    "    dir *= inversesqrt(dirR);\n"
    "  }\n"
    "  len = len * 0.5;\n"
    "  len *= len;\n"
    "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
    "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
    "  float lob = 0.5 - 0.29 * len;\n"
    "  float clp = 1.0 / lob;\n"
    "  vec3 min4 = min(min(f0, g), min(j, k));\n"
    "  vec3 max4 = max(max(f0, g), max(j, k));\n"
    "  vec3 aC = vec3(0.0);\n"
    "  float aW = 0.0;\n"
    "  fsrEasuTap(aC, aW, vec2( 0.0, -1.0) - f, dir, len2, lob, clp, b);\n"
    "  fsrEasuTap(aC, aW, vec2( 1.0, -1.0) - f, dir, len2, lob, clp, c);\n"
    "  fsrEasuTap(aC, aW, vec2(-1.0,  0.0) - f, dir, len2, lob, clp, e);\n"
    "  fsrEasuTap(aC, aW, vec2( 0.0,  0.0) - f, dir, len2, lob, clp, f0);\n"
    "  fsrEasuTap(aC, aW, vec2( 1.0,  0.0) - f, dir, len2, lob, clp, g);\n"
    "  fsrEasuTap(aC, aW, vec2( 2.0,  0.0) - f, dir, len2, lob, clp, h);\n"
    "  fsrEasuTap(aC, aW, vec2(-1.0,  1.0) - f, dir, len2, lob, clp, i);\n"
    "  fsrEasuTap(aC, aW, vec2( 0.0,  1.0) - f, dir, len2, lob, clp, j);\n"
    "  fsrEasuTap(aC, aW, vec2( 1.0,  1.0) - f, dir, len2, lob, clp, k);\n"
    "  fsrEasuTap(aC, aW, vec2( 2.0,  1.0) - f, dir, len2, lob, clp, l);\n"
    "  fsrEasuTap(aC, aW, vec2( 0.0,  2.0) - f, dir, len2, lob, clp, n);\n"
    "  fsrEasuTap(aC, aW, vec2( 1.0,  2.0) - f, dir, len2, lob, clp, o);\n"
    "  return clamp(aC / max(aW, 1e-4), min4, max4);\n"
    "}\n"
    "\n"
    "/* AMD FidelityFX FSR 1.0 — Authentic per-channel RCAS + 2-scale display-pixel detail recovery */\n"
    "vec3 applyFsrRcas(vec3 e, vec2 uv, vec2 stepUV, float sharpness) {\n"
    "  vec3 b1 = sampleTex(uv + vec2(0.0, -stepUV.y));\n"
    "  vec3 d1 = sampleTex(uv + vec2(-stepUV.x, 0.0));\n"
    "  vec3 f1 = sampleTex(uv + vec2( stepUV.x, 0.0));\n"
    "  vec3 h1 = sampleTex(uv + vec2(0.0,  stepUV.y));\n"
    "  vec2 s2 = stepUV * 2.0;\n"
    "  vec3 b2 = sampleTex(uv + vec2(0.0, -s2.y));\n"
    "  vec3 d2 = sampleTex(uv + vec2(-s2.x, 0.0));\n"
    "  vec3 f2 = sampleTex(uv + vec2( s2.x, 0.0));\n"
    "  vec3 h2 = sampleTex(uv + vec2(0.0,  s2.y));\n"
    "  float bL = fsrLuma(b1), dL = fsrLuma(d1), eL = fsrLuma(e), fL = fsrLuma(f1), hL = fsrLuma(h1);\n"
    "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
    "  float rangeL = max(max(max(bL, dL), max(eL, fL)), hL) -\n"
    "                 min(min(min(bL, dL), min(eL, fL)), hL);\n"
    "  nz = clamp(abs(nz) / max(rangeL, 1e-4), 0.0, 1.0);\n"
    "  nz = -0.35 * nz + 1.0;\n"
    "  vec3 mn4 = min(min(b1, d1), min(f1, h1));\n"
    "  vec3 mx4 = max(max(b1, d1), max(f1, h1));\n"
    "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1e-4));\n"
    "  vec3 hitMax = (1.0 - max(mx4, e)) / max(4.0 * (1.0 - mn4), vec3(1e-4));\n"
    "  vec3 lobeRGB = max(-hitMin, -hitMax);\n"
    "  float lobe = max(-0.22, min(lobeRGB.r, min(lobeRGB.g, lobeRGB.b))) * clamp(sharpness, 0.0, 1.0) * nz;\n"
    "  vec3 rcas1 = (lobe * (b1 + d1 + f1 + h1) + e) / (4.0 * lobe + 1.0);\n"
    "  vec3 amp = sqrt(clamp(min(min(mn4, e), 1.0 - max(mx4, e)) / max(max(mx4, e), vec3(1e-4)), 0.0, 1.0));\n"
    "  vec3 detail2 = (e - 0.25 * (b2 + d2 + f2 + h2)) * (amp * clamp(sharpness, 0.0, 1.0) * nz * 1.65);\n"
    "  vec3 outc = rcas1 + detail2;\n"
    "  vec3 lo = min(mn4, e) - vec3(0.06);\n"
    "  vec3 hi = max(mx4, e) + vec3(0.06);\n"
    "  return clamp(outc, max(lo, vec3(0.0)), min(hi, vec3(1.0)));\n"
    "}\n"
    "\n"
    "vec3 upscaleAt(vec2 uv) {\n"
    "  if (uMode == 0) return applyCAS(sampleBicubic(uv), uv, uSharp);\n"
    "  if (uMode == 1) return applyCAS(sampleLanczos3(uv), uv, uSharp * 0.55);\n"
    "  if (uMode == 2) return sampleEdgeSmooth(uv);\n"
    "  if (uMode == 3) return sampleTex(uv);\n"
    "  if (uMode == 4) {\n"
    "    vec2 p = (floor(uv * uSrcSize) + 0.5) / uSrcSize;\n"
    "    return sampleTex(p);\n"
    "  }\n"
    "  if (uMode == 6) {\n"
    "    vec3 easu = sampleFsrEasu(uv, uSrcSize);\n"
    "    return (uSharp > 0.005) ? applyFsrRcas(easu, uv, 1.0 / uSrcSize, uSharp) : easu;\n"
    "  }\n"
    "  return applyCAS(sampleBicubic(uv), uv, 0.4);\n"
    "}\n"
    "\n"
    "float bgDiffAt(vec2 texelPos) {\n"
    "  vec2 rel = texelPos - uBgRect.xy;\n"
    "  if (rel.x < 0.0 || rel.y < 0.0 || rel.x >= uBgRect.z || rel.y >= uBgRect.w) return 1.0;\n"
    "  vec2 c  = (clamp(texelPos, vec2(0.0), uSrcSize - 1.0) + 0.5) / uSrcSize;\n"
    "  vec2 bc = (clamp(rel, vec2(0.0), uBgRect.zw - 1.0) + 0.5) / uBgRect.zw;\n"
    "  vec3 a = texture(uTex, c).rgb;\n"
    "  vec3 b = texture(uBgOrigTex, bc).rgb;\n"
    "  vec3 d = abs(a - b);\n"
    "  return max(d.r, max(d.g, d.b));\n"
    "}\n"
    "\n"
    "/* Strict exact-match background mask with pinhole protection */\n"
    "float fgMaskAt(vec2 tp) {\n"
    "  float dc = bgDiffAt(tp);\n"
    "  float m  = smoothstep(0.002, 0.008, dc);\n"
    "  if (m > 0.99) return 1.0;\n"
    "  float n = step(0.006, bgDiffAt(tp + vec2( 0.0, -1.0)));\n"
    "  float s = step(0.006, bgDiffAt(tp + vec2( 0.0,  1.0)));\n"
    "  float w = step(0.006, bgDiffAt(tp + vec2(-1.0,  0.0)));\n"
    "  float e = step(0.006, bgDiffAt(tp + vec2( 1.0,  0.0)));\n"
    "  if (n + s + w + e >= 3.0) return 1.0;\n"
    "  return m;\n"
    "}\n"
    "\n"
    "/* Composite non-black HUD/subtitle pixels with subtle soft drop shadow */\n"
    "vec3 overlayHudStrip(vec3 baseCol, vec2 hudPx, float minY, float maxY) {\n"
    "  vec2 uv = hudPx / uSrcSize;\n"
    "  vec3 hc = sampleTex(uv);\n"
    "  float luma = max(hc.r, max(hc.g, hc.b));\n"
    "  float a = smoothstep(0.025, 0.09, luma);\n"
    "  float halo = 0.0;\n"
    "  for (int oy = -2; oy <= 2; oy++) {\n"
    "    for (int ox = -2; ox <= 2; ox++) {\n"
    "      float sy = clamp(hudPx.y + float(oy), minY, maxY);\n"
    "      vec3 sc = sampleTex(vec2(hudPx.x + float(ox), sy) / uSrcSize);\n"
    "      halo = max(halo, max(sc.r, max(sc.g, sc.b)));\n"
    "    }\n"
    "  }\n"
    "  float haloA = smoothstep(0.03, 0.14, halo) * 0.68;\n"
    "  vec3 outc = mix(baseCol, vec3(0.02, 0.02, 0.04), haloA * (1.0 - a));\n"
    "  return mix(outc, hc, a);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "  vec2 dispSize = (uLayout == 1) ? vec2(800.0, 500.0) : ((uLayout == 2) ? vec2(800.0, 450.0) : uSrcSize);\n"
    "  vec2 dispPos  = vUV * dispSize;\n"
    "  vec2 srcPos;\n"
    "  bool inRoom = true;\n"
    "  bool inDock = false;\n"
    "\n"
    "  if (uDialogBar != 0 && uLayout != 0 && dispPos.y >= dispSize.y - 100.0) {\n"
    "    inRoom = false;\n"
    "    inDock = false;\n"
    "    srcPos = vec2(dispPos.x, 500.0 + (dispPos.y - (dispSize.y - 100.0)));\n"
    "  } else if (uLayout == 1) {\n"
    "    if (dispPos.y < 450.0) {\n"
    "      srcPos = vec2(dispPos.x, dispPos.y + 75.0);\n"
    "    } else {\n"
    "      inRoom = false;\n"
    "      inDock = true;\n"
    "      srcPos = vec2(dispPos.x, (dispPos.y - 450.0) + 537.0);\n"
    "    }\n"
    "  } else if (uLayout == 2) {\n"
    "    srcPos = vec2(dispPos.x, dispPos.y + 75.0);\n"
    "  } else {\n"
    "    srcPos = dispPos;\n"
    "  }\n"
    "\n"
    "  vec2 srcUV = srcPos / uSrcSize;\n"
    "  vec3 col;\n"
    "  if (uUseHdCanvas != 0) {\n"
    "    /* 4K canvas sampling: true 12-tap directional FSR 1.0 EASU for FSR mode, Catmull-Rom otherwise */\n"
    "    col = (uMode == 6) ? sampleFsrEasu(srcUV, uTexSize) : sampleHdCanvas(srcUV);\n"
    "    if (uSharp > 0.005 && uMode != 3 && uMode != 4) {\n"
    "      vec2 dispStep = (dispSize / max(uDstSize, vec2(1.0))) / uSrcSize;\n"
    "      if (uMode == 6) {\n"
    "        col = applyFsrRcas(col, srcUV, dispStep, uSharp);\n"
    "      } else {\n"
    "        col = applyCASNative(col, srcUV, dispStep, uSharp);\n"
    "      }\n"
    "    }\n"
    "  } else {\n"
    "    col = upscaleAt(srcUV);\n"
    "    /* Seamless Native-Resolution HD Background Compositing (when HD Sprites is OFF) */\n"
    "    if (uHasHdBg != 0 && inRoom &&\n"
    "        srcPos.x >= uBgRect.x && srcPos.x < uBgRect.x + uBgRect.z &&\n"
    "        srcPos.y >= uBgRect.y && srcPos.y < uBgRect.y + uBgRect.w) {\n"
    "      vec2 fp = srcPos - 0.5;\n"
    "      vec2 t0 = floor(fp);\n"
    "      vec2 f  = fract(fp);\n"
    "      float m00 = fgMaskAt(t0 + vec2(0.0, 0.0));\n"
    "      float m10 = fgMaskAt(t0 + vec2(1.0, 0.0));\n"
    "      float m01 = fgMaskAt(t0 + vec2(0.0, 1.0));\n"
    "      float m11 = fgMaskAt(t0 + vec2(1.0, 1.0));\n"
    "      float fgMask = mix(mix(m00, m10, f.x), mix(m01, m11, f.x), f.y);\n"
    "      vec2 bgUV = clamp((srcPos - uBgRect.xy) / uBgRect.zw, 0.0, 1.0);\n"
    "      vec3 hdCol = texture(uBgHdTex, bgUV).rgb;\n"
    "      col = mix(hdCol, col, fgMask);\n"
    "    }\n"
    "  }\n"
    "\n"
    "  /* Subtle separator line above the 16:10 dedicated inventory dock */\n"
    "  if (inDock && dispPos.y < 451.0) {\n"
    "    col = mix(col, vec3(0.32, 0.26, 0.12), 0.75);\n"
    "  }\n"
    "\n"
    "  /* Widescreen top bar overlay (subtitles, score, minigame Exit/Help buttons from y=0..74) */\n"
    "  if (uLayout != 0 && dispPos.y < 75.0) {\n"
    "    col = overlayHudStrip(col, vec2(dispPos.x, dispPos.y), 0.5, 74.5);\n"
    "  }\n"
    "\n"
    "  /* 16:9 Cinema bottom inventory overlay (items & arrows from y=537..586) */\n"
    "  if (uLayout == 2 && uDialogBar == 0 && dispPos.y >= 400.0) {\n"
    "    vec2 invPx = vec2(dispPos.x, 537.0 + (dispPos.y - 400.0));\n"
    "    col = overlayHudStrip(col, invPx, 537.5, 586.5);\n"
    "  }\n"
    "\n"
    "  if (uMode == 5) {\n"
    "    float scan = 0.90 + 0.10 * sin(vUV.y * dispSize.y * 3.14159265 * 2.0);\n"
    "    vec2 d = vUV * 2.0 - 1.0;\n"
    "    float vig = 1.0 - 0.14 * dot(d, d);\n"
    "    col *= scan * vig;\n"
    "  }\n"
    "  FragColor = vec4(col, 1.0);\n"
    "}\n";

static const char *VS_OSD =
    "#version 330 core\n"
    "uniform vec4 uRect; /* x0, y0, x1, y1 in NDC */\n"
    "uniform vec4 uUVRect; /* u0, v0, u1, v1 normalized */\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "  vec2 corners[4] = vec2[4](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));\n"
    "  vec2 c = corners[gl_VertexID];\n"
    "  vUV = vec2(mix(uUVRect.x, uUVRect.z, c.x), mix(uUVRect.y, uUVRect.w, c.y));\n"
    "  vec2 pos = vec2(mix(uRect.x, uRect.z, c.x), mix(uRect.y, uRect.w, c.y));\n"
    "  gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char *FS_OSD =
    "#version 330 core\n"
    "uniform sampler2D uTex;\n"
    "uniform float uAlpha;\n"
    "uniform float uSoftEdge;\n"
    "in vec2 vUV;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "  vec4 t = texture(uTex, vUV);\n"
    "  if (uSoftEdge > 0.5 && t.a > 0.004 && t.a < 0.992) {\n"
    "    vec2 ts = 1.0 / vec2(textureSize(uTex, 0));\n"
    "    vec4 nL = texture(uTex, vUV + vec2(-ts.x, 0.0));\n"
    "    vec4 nR = texture(uTex, vUV + vec2( ts.x, 0.0));\n"
    "    vec4 nU = texture(uTex, vUV + vec2(0.0, -ts.y));\n"
    "    vec4 nD = texture(uTex, vUV + vec2(0.0,  ts.y));\n"
    "    float aAvg = 0.50 * t.a + 0.125 * (nL.a + nR.a + nU.a + nD.a);\n"
    "    t.a = mix(t.a, smoothstep(0.05, 0.95, aAvg), 0.72);\n"
    "    /* Defringe dark cutout rims by lifting semi-transparent edge RGB toward opaque interior */\n"
    "    float wL = smoothstep(0.55, 0.98, nL.a);\n"
    "    float wR = smoothstep(0.55, 0.98, nR.a);\n"
    "    float wU = smoothstep(0.55, 0.98, nU.a);\n"
    "    float wD = smoothstep(0.55, 0.98, nD.a);\n"
    "    float wSum = wL + wR + wU + wD;\n"
    "    if (wSum > 0.01 && t.a < 0.88) {\n"
    "      vec3 interior = (nL.rgb * wL + nR.rgb * wR + nU.rgb * wU + nD.rgb * wD) / wSum;\n"
    "      float lift = (1.0 - t.a) * 0.65;\n"
    "      t.rgb = mix(t.rgb, max(t.rgb, interior), lift);\n"
    "    }\n"
    "  }\n"
    "  FragColor = vec4(t.rgb, t.a * uAlpha);\n"
    "}\n";

static const char *FS_SOLID =
    "#version 330 core\n"
    "uniform vec4 uColor;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "  FragColor = uColor;\n"
    "}\n";

/* ---- v5 Bloom Bright-Pass Extract (warm-weighted for torches, candles, gold) ---- */
static const char *FS_BLOOM_EXTRACT =
    "#version 330 core\n"
    "uniform sampler2D uScene;\n"
    "uniform vec2 uTexelSize;\n"
    "in vec2 vUV;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "  vec2 uv = vec2(vUV.x, 1.0 - vUV.y);\n"
    "  vec3 c = (texture(uScene, uv + uTexelSize * vec2(-0.5, -0.5)).rgb +\n"
    "            texture(uScene, uv + uTexelSize * vec2( 0.5, -0.5)).rgb +\n"
    "            texture(uScene, uv + uTexelSize * vec2(-0.5,  0.5)).rgb +\n"
    "            texture(uScene, uv + uTexelSize * vec2( 0.5,  0.5)).rgb) * 0.25;\n"
    "  float luma = dot(c, vec3(0.32, 0.56, 0.12));\n"
    "  float warmth = max(0.0, (c.r * 0.65 + c.g * 0.35) - c.b);\n"
    "  float bright = luma + warmth * 0.24;\n"
    "  float thresh = 0.72;\n"
    "  float knee = 0.14;\n"
    "  float soft = clamp(bright - thresh + knee, 0.0, 2.0 * knee);\n"
    "  soft = (soft * soft) / (4.0 * knee + 1e-4);\n"
    "  float contrib = max(bright - thresh, soft) / max(bright, 1e-4);\n"
    "  FragColor = vec4(c * clamp(contrib * 1.45, 0.0, 2.0), 1.0);\n"
    "}\n";

/* ---- v5 Separable 9-Tap Gaussian Blur for Bloom ---- */
static const char *FS_BLOOM_BLUR =
    "#version 330 core\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uDir;\n"
    "in vec2 vUV;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "  vec2 uv = vec2(vUV.x, 1.0 - vUV.y);\n"
    "  vec3 sum = texture(uTex, uv).rgb * 0.227027;\n"
    "  sum += (texture(uTex, uv + uDir * 1.384615).rgb + texture(uTex, uv - uDir * 1.384615).rgb) * 0.316216;\n"
    "  sum += (texture(uTex, uv + uDir * 3.230769).rgb + texture(uTex, uv - uDir * 3.230769).rgb) * 0.070270;\n"
    "  FragColor = vec4(sum, 1.0);\n"
    "}\n";

/* ---- v5 Final Remaster Post-Processing Compositor ----
 * Combines: Scene Crossfade, Video Deblock+Sharpen, Sprite Silhouette AA,
 * Emissive Bloom, Color Grading/Vibrance, Vignette, and Anti-Banding Dithering. */
static const char *FS_POST =
    "#version 330 core\n"
    "uniform sampler2D uScene;\n"
    "uniform sampler2D uPrevScene;\n"
    "uniform sampler2D uBloom;\n"
    "uniform sampler2D uOrigTex;\n"
    "uniform vec2 uInvSize;\n"
    "uniform float uTransAlpha;\n"
    "uniform float uBloomStrength;\n"
    "uniform int uGradeMode;\n"
    "uniform int uSilhouetteAA;\n"
    "uniform float uAAStrength;\n"
    "uniform float uClarity;\n"
    "uniform float uFilmic;\n"
    "uniform int uDeband;\n"
    "uniform int uAtmosphere;\n"
    "uniform float uTime;\n"
    "uniform float uVignette;\n"
    "uniform int uVideoEnhance;\n"
    "uniform float uCompareSplit;\n"
    "uniform int uLayout;\n"
    "uniform int uDialogBar;\n"
    "in vec2 vUV;\n"
    "out vec4 FragColor;\n"
    "\n"
    "float luma3(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
    "float max3(vec3 v) { return max(v.r, max(v.g, v.b)); }\n"
    "\n"
    "/* SMAA 1x color-aware edge detector with local contrast adaptation */\n"
    "vec2 smaaEdgeAt(vec2 uv, float thr) {\n"
    "  vec3 c  = texture(uScene, uv).rgb;\n"
    "  vec3 cL = texture(uScene, uv + vec2(-uInvSize.x,  0.0)).rgb;\n"
    "  vec3 cT = texture(uScene, uv + vec2( 0.0, -uInvSize.y)).rgb;\n"
    "  vec3 cR = texture(uScene, uv + vec2( uInvSize.x,  0.0)).rgb;\n"
    "  vec3 cB = texture(uScene, uv + vec2( 0.0,  uInvSize.y)).rgb;\n"
    "  float dL = max3(abs(c - cL));\n"
    "  float dT = max3(abs(c - cT));\n"
    "  vec2 e = step(vec2(thr), vec2(dL, dT));\n"
    "  if (e.x + e.y < 0.5) return vec2(0.0);\n"
    "  float dR = max3(abs(c - cR));\n"
    "  float dB = max3(abs(c - cB));\n"
    "  float maxD = max(max(dL, dT), max(dR, dB));\n"
    "  return e * step(vec2(maxD * 0.5), vec2(dL, dT));\n"
    "}\n"
    "\n"
    "float smaaArea(float d1, float d2, float c1, float c2) {\n"
    "  float len = max(d1 + d2, 1.0);\n"
    "  if (c1 > 0.5 && c2 > 0.5) {\n"
    "    /* Z-shape staircase: linear coverage ramp across span */\n"
    "    float t = (d1 < d2) ? (0.5 - d1 / len) : (0.5 - d2 / len);\n"
    "    return clamp(t, 0.0, 0.5);\n"
    "  } else if (c1 > 0.5) {\n"
    "    return clamp(0.5 - d1 / len, 0.0, 0.5);\n"
    "  } else if (c2 > 0.5) {\n"
    "    return clamp(0.5 - d2 / len, 0.0, 0.5);\n"
    "  }\n"
    "  return 0.0;\n"
    "}\n"
    "\n"
    "vec3 applySmaa1x(vec3 c, vec2 uv, float strength) {\n"
    "  float thr = mix(0.14, 0.045, strength);\n"
    "  vec2 eC = smaaEdgeAt(uv, thr);\n"
    "  vec2 eR = smaaEdgeAt(uv + vec2(uInvSize.x, 0.0), thr);\n"
    "  vec2 eB = smaaEdgeAt(uv + vec2(0.0, uInvSize.y), thr);\n"
    "  vec4 weights = vec4(0.0); /* top, bottom, left, right */\n"
    "  /* Horizontal edge (top of pixel): search left/right up to 16px each */\n"
    "  if (eC.y > 0.5) {\n"
    "    float dL = 0.0, dR = 0.0;\n"
    "    for (int i = 1; i <= 16; i++) {\n"
    "      if (smaaEdgeAt(uv - vec2(float(i) * uInvSize.x, 0.0), thr).y < 0.5) break;\n"
    "      dL = float(i);\n"
    "    }\n"
    "    for (int i = 1; i <= 16; i++) {\n"
    "      if (smaaEdgeAt(uv + vec2(float(i) * uInvSize.x, 0.0), thr).y < 0.5) break;\n"
    "      dR = float(i);\n"
    "    }\n"
    "    float cL = smaaEdgeAt(uv + vec2(-(dL + 1.0) * uInvSize.x, -uInvSize.y), thr).x;\n"
    "    float cR = smaaEdgeAt(uv + vec2( (dR + 1.0) * uInvSize.x, -uInvSize.y), thr).x;\n"
    "    float cLb = smaaEdgeAt(uv + vec2(-(dL + 1.0) * uInvSize.x, 0.0), thr).x;\n"
    "    float cRb = smaaEdgeAt(uv + vec2( (dR + 1.0) * uInvSize.x, 0.0), thr).x;\n"
    "    weights.x = smaaArea(dL, dR, max(cL, cLb), max(cR, cRb));\n"
    "  }\n"
    "  /* Bottom horizontal edge */\n"
    "  if (eB.y > 0.5) {\n"
    "    vec2 uvB = uv + vec2(0.0, uInvSize.y);\n"
    "    float dL = 0.0, dR = 0.0;\n"
    "    for (int i = 1; i <= 16; i++) {\n"
    "      if (smaaEdgeAt(uvB - vec2(float(i) * uInvSize.x, 0.0), thr).y < 0.5) break;\n"
    "      dL = float(i);\n"
    "    }\n"
    "    for (int i = 1; i <= 16; i++) {\n"
    "      if (smaaEdgeAt(uvB + vec2(float(i) * uInvSize.x, 0.0), thr).y < 0.5) break;\n"
    "      dR = float(i);\n"
    "    }\n"
    "    float cL = smaaEdgeAt(uvB + vec2(-(dL + 1.0) * uInvSize.x, -uInvSize.y), thr).x;\n"
    "    float cR = smaaEdgeAt(uvB + vec2( (dR + 1.0) * uInvSize.x, -uInvSize.y), thr).x;\n"
    "    weights.y = smaaArea(dL, dR, cL, cR);\n"
    "  }\n"
    "  /* Vertical edge (left of pixel): search up/down up to 16px each */\n"
    "  if (eC.x > 0.5) {\n"
    "    float dU = 0.0, dD = 0.0;\n"
    "    for (int i = 1; i <= 16; i++) {\n"
    "      if (smaaEdgeAt(uv - vec2(0.0, float(i) * uInvSize.y), thr).x < 0.5) break;\n"
    "      dU = float(i);\n"
    "    }\n"
    "    for (int i = 1; i <= 16; i++) {\n"
    "      if (smaaEdgeAt(uv + vec2(0.0, float(i) * uInvSize.y), thr).x < 0.5) break;\n"
    "      dD = float(i);\n"
    "    }\n"
    "    float cU = smaaEdgeAt(uv + vec2(-uInvSize.x, -(dU + 1.0) * uInvSize.y), thr).y;\n"
    "    float cD = smaaEdgeAt(uv + vec2(-uInvSize.x,  (dD + 1.0) * uInvSize.y), thr).y;\n"
    "    float cUr = smaaEdgeAt(uv + vec2(0.0, -(dU + 1.0) * uInvSize.y), thr).y;\n"
    "    float cDr = smaaEdgeAt(uv + vec2(0.0,  (dD + 1.0) * uInvSize.y), thr).y;\n"
    "    weights.z = smaaArea(dU, dD, max(cU, cUr), max(cD, cDr));\n"
    "  }\n"
    "  /* Right vertical edge */\n"
    "  if (eR.x > 0.5) {\n"
    "    vec2 uvR = uv + vec2(uInvSize.x, 0.0);\n"
    "    float dU = 0.0, dD = 0.0;\n"
    "    for (int i = 1; i <= 16; i++) {\n"
    "      if (smaaEdgeAt(uvR - vec2(0.0, float(i) * uInvSize.y), thr).x < 0.5) break;\n"
    "      dU = float(i);\n"
    "    }\n"
    "    for (int i = 1; i <= 16; i++) {\n"
    "      if (smaaEdgeAt(uvR + vec2(0.0, float(i) * uInvSize.y), thr).x < 0.5) break;\n"
    "      dD = float(i);\n"
    "    }\n"
    "    float cU = smaaEdgeAt(uvR + vec2(-uInvSize.x, -(dU + 1.0) * uInvSize.y), thr).y;\n"
    "    float cD = smaaEdgeAt(uvR + vec2(-uInvSize.x,  (dD + 1.0) * uInvSize.y), thr).y;\n"
    "    weights.w = smaaArea(dU, dD, cU, cD);\n"
    "  }\n"
    "  weights *= strength;\n"
    "  float wSum = weights.x + weights.y + weights.z + weights.w;\n"
    "  if (wSum < 1e-4) return c;\n"
    "  vec3 colH = mix(c, texture(uScene, uv + vec2(0.0, (weights.y - weights.x) * uInvSize.y)).rgb,\n"
    "                  clamp(weights.x + weights.y, 0.0, 1.0));\n"
    "  vec3 colV = mix(c, texture(uScene, uv + vec2((weights.w - weights.z) * uInvSize.x, 0.0)).rgb,\n"
    "                  clamp(weights.z + weights.w, 0.0, 1.0));\n"
    "  float hW = weights.x + weights.y;\n"
    "  float vW = weights.z + weights.w;\n"
    "  return (colH * hW + colV * vW) / max(hW + vW, 1e-4);\n"
    "}\n"
    "\n"
    "/* Game-pixel-scale (~3 display px) structure-tensor tangent AA for baked-in 2005 1x staircases */\n"
    "vec3 applyEdgeGuided3xAA(vec3 c, vec2 uv, float strength) {\n"
    "  vec2 s = uInvSize * 2.6;\n"
    "  float lTL = luma3(texture(uScene, uv + vec2(-s.x, -s.y)).rgb);\n"
    "  float lTC = luma3(texture(uScene, uv + vec2( 0.0, -s.y)).rgb);\n"
    "  float lTR = luma3(texture(uScene, uv + vec2( s.x, -s.y)).rgb);\n"
    "  float lML = luma3(texture(uScene, uv + vec2(-s.x,  0.0)).rgb);\n"
    "  float lMR = luma3(texture(uScene, uv + vec2( s.x,  0.0)).rgb);\n"
    "  float lBL = luma3(texture(uScene, uv + vec2(-s.x,  s.y)).rgb);\n"
    "  float lBC = luma3(texture(uScene, uv + vec2( 0.0,  s.y)).rgb);\n"
    "  float lBR = luma3(texture(uScene, uv + vec2( s.x,  s.y)).rgb);\n"
    "  float gx = (lTR + 2.0 * lMR + lBR) - (lTL + 2.0 * lML + lBL);\n"
    "  float gy = (lBL + 2.0 * lBC + lBR) - (lTL + 2.0 * lTC + lTR);\n"
    "  float g2 = gx * gx + gy * gy;\n"
    "  if (g2 < 0.012) return c;\n"
    "  /* Local 2-scale coherence check: ignore isotropic texture/grain, target clean curved/diagonal edges */\n"
    "  vec2 grad = vec2(gx, gy) * inversesqrt(g2);\n"
    "  vec2 tang = vec2(-grad.y, grad.x);\n"
    "  vec2 step1 = tang * uInvSize * 1.6;\n"
    "  vec2 step2 = tang * uInvSize * 3.2;\n"
    "  vec3 tP1 = texture(uScene, uv + step1).rgb;\n"
    "  vec3 tM1 = texture(uScene, uv - step1).rgb;\n"
    "  vec3 tP2 = texture(uScene, uv + step2).rgb;\n"
    "  vec3 tM2 = texture(uScene, uv - step2).rgb;\n"
    "  float wP1 = exp(-dot(tP1 - c, tP1 - c) * 18.0) * 0.28;\n"
    "  float wM1 = exp(-dot(tM1 - c, tM1 - c) * 18.0) * 0.28;\n"
    "  float wP2 = exp(-dot(tP2 - c, tP2 - c) * 24.0) * 0.14;\n"
    "  float wM2 = exp(-dot(tM2 - c, tM2 - c) * 24.0) * 0.14;\n"
    "  float wC  = 0.36;\n"
    "  vec3 smoothCol = (c * wC + tP1 * wP1 + tM1 * wM1 + tP2 * wP2 + tM2 * wM2) /\n"
    "                   (wC + wP1 + wM1 + wP2 + wM2);\n"
    "  /* Gate by directional contrast vs tangent consistency */\n"
    "  float edgeGate = smoothstep(0.012, 0.09, g2);\n"
    "  return mix(c, smoothCol, edgeGate * clamp(strength, 0.0, 1.0) * 0.85);\n"
    "}\n"
    "\n"
    "vec3 sampleSceneFX(vec2 uv) {\n"
    "  vec3 c = texture(uScene, uv).rgb;\n"
    "  if (uVideoEnhance != 0) {\n"
    "    /* Multi-radius bilateral deblock + adaptive detail recovery for 2005 FMV cutscenes */\n"
    "    vec3 sum = c * 0.28;\n"
    "    float wsum = 0.28;\n"
    "    vec2 offs[8] = vec2[8](\n"
    "      vec2(-1.2, 0.0), vec2(1.2, 0.0), vec2(0.0, -1.2), vec2(0.0, 1.2),\n"
    "      vec2(-2.4, -2.4), vec2(2.4, -2.4), vec2(-2.4, 2.4), vec2(2.4, 2.4));\n"
    "    for (int i = 0; i < 8; i++) {\n"
    "      vec3 s = texture(uScene, uv + offs[i] * uInvSize).rgb;\n"
    "      float d = length(s - c);\n"
    "      float w = exp(-d * d * 38.0) * (i < 4 ? 0.12 : 0.06);\n"
    "      sum += s * w;\n"
    "      wsum += w;\n"
    "    }\n"
    "    vec3 deb = sum / wsum;\n"
    "    vec3 b1 = 0.25 * (texture(uScene, uv + vec2(-uInvSize.x, 0.0)).rgb +\n"
    "                      texture(uScene, uv + vec2( uInvSize.x, 0.0)).rgb +\n"
    "                      texture(uScene, uv + vec2(0.0, -uInvSize.y)).rgb +\n"
    "                      texture(uScene, uv + vec2(0.0,  uInvSize.y)).rgb);\n"
    "    vec3 b2 = 0.25 * (texture(uScene, uv + vec2(-2.0 * uInvSize.x, 0.0)).rgb +\n"
    "                      texture(uScene, uv + vec2( 2.0 * uInvSize.x, 0.0)).rgb +\n"
    "                      texture(uScene, uv + vec2(0.0, -2.0 * uInvSize.y)).rgb +\n"
    "                      texture(uScene, uv + vec2(0.0,  2.0 * uInvSize.y)).rgb);\n"
    "    c = clamp(deb + (deb - b1) * 0.45 + (deb - b2) * 0.25, 0.0, 1.0);\n"
    "  } else if (uSilhouetteAA == 3 && uAAStrength > 0.005) {\n"
    "    float aaS = clamp(uAAStrength, 0.0, 1.0);\n"
    "    c = applySmaa1x(c, uv, aaS);\n"
    "    c = applyEdgeGuided3xAA(c, uv, aaS);\n"
    "  } else if (uSilhouetteAA == 2 && uAAStrength > 0.005) {\n"
    "    c = applySmaa1x(c, uv, clamp(uAAStrength, 0.0, 1.0));\n"
    "  } else if (uSilhouetteAA == 1 && uAAStrength > 0.005) {\n"
    "    /* Full FXAA 3.11 Quality edge-tracing AA (with live uAAStrength 0..1 control) */\n"
    "    float aaS = clamp(uAAStrength, 0.0, 1.0);\n"
    "    float lM  = luma3(c);\n"
    "    float lN  = luma3(texture(uScene, uv + vec2( 0.0, -uInvSize.y)).rgb);\n"
    "    float lS  = luma3(texture(uScene, uv + vec2( 0.0,  uInvSize.y)).rgb);\n"
    "    float lW  = luma3(texture(uScene, uv + vec2(-uInvSize.x,  0.0)).rgb);\n"
    "    float lE  = luma3(texture(uScene, uv + vec2( uInvSize.x,  0.0)).rgb);\n"
    "    float lMin = min(lM, min(min(lN, lS), min(lW, lE)));\n"
    "    float lMax = max(lM, max(max(lN, lS), max(lW, lE)));\n"
    "    float range = lMax - lMin;\n"
    "    float edgeMin = mix(0.06, 0.022, aaS);\n"
    "    float edgeRel = mix(0.15, 0.062, aaS);\n"
    "    if (range >= max(edgeMin, lMax * edgeRel)) {\n"
    "      float lNW = luma3(texture(uScene, uv + vec2(-uInvSize.x, -uInvSize.y)).rgb);\n"
    "      float lNE = luma3(texture(uScene, uv + vec2( uInvSize.x, -uInvSize.y)).rgb);\n"
    "      float lSW = luma3(texture(uScene, uv + vec2(-uInvSize.x,  uInvSize.y)).rgb);\n"
    "      float lSE = luma3(texture(uScene, uv + vec2( uInvSize.x,  uInvSize.y)).rgb);\n"
    "      float lNS = lN + lS;\n"
    "      float lWE = lW + lE;\n"
    "      float subpixRcp = 1.0 / range;\n"
    "      float subpixA = 2.0 * (lNS + lWE) + (lNW + lNE + lSW + lSE);\n"
    "      float subpixB = clamp(abs(subpixA * (1.0 / 12.0) - lM) * subpixRcp, 0.0, 1.0);\n"
    "      float subpixC = (-2.0 * subpixB + 3.0) * subpixB * subpixB;\n"
    "      float subpixOff = subpixC * subpixC * (0.85 * aaS);\n"
    "      float edgeH = abs(-2.0 * lW + lNW + lSW) + abs(-2.0 * lM + lNS) * 2.0 + abs(-2.0 * lE + lNE + lSE);\n"
    "      float edgeV = abs(-2.0 * lN + lNW + lNE) + abs(-2.0 * lM + lWE) * 2.0 + abs(-2.0 * lS + lSW + lSE);\n"
    "      bool isHorz = (edgeH >= edgeV);\n"
    "      float l1 = isHorz ? lN : lW;\n"
    "      float l2 = isHorz ? lS : lE;\n"
    "      float g1 = abs(l1 - lM);\n"
    "      float g2 = abs(l2 - lM);\n"
    "      bool is1Steep = (g1 >= g2);\n"
    "      float gradScaled = 0.25 * max(g1, g2);\n"
    "      float stepLen = isHorz ? uInvSize.y : uInvSize.x;\n"
    "      float lLocalAvg;\n"
    "      if (is1Steep) {\n"
    "        stepLen = -stepLen;\n"
    "        lLocalAvg = 0.5 * (l1 + lM);\n"
    "      } else {\n"
    "        lLocalAvg = 0.5 * (l2 + lM);\n"
    "      }\n"
    "      vec2 curUV = uv;\n"
    "      if (isHorz) curUV.y += stepLen * 0.5; else curUV.x += stepLen * 0.5;\n"
    "      vec2 off = isHorz ? vec2(uInvSize.x, 0.0) : vec2(0.0, uInvSize.y);\n"
    "      vec2 uv1 = curUV - off;\n"
    "      vec2 uv2 = curUV + off;\n"
    "      float lEnd1 = luma3(texture(uScene, uv1).rgb) - lLocalAvg;\n"
    "      float lEnd2 = luma3(texture(uScene, uv2).rgb) - lLocalAvg;\n"
    "      bool done1 = abs(lEnd1) >= gradScaled;\n"
    "      bool done2 = abs(lEnd2) >= gradScaled;\n"
    "      if (!done1) uv1 -= off;\n"
    "      if (!done2) uv2 += off;\n"
    "      float steps[9] = float[9](1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 4.0, 8.0);\n"
    "      for (int i = 0; i < 9; i++) {\n"
    "        if (done1 && done2) break;\n"
    "        if (!done1) {\n"
    "          lEnd1 = luma3(texture(uScene, uv1).rgb) - lLocalAvg;\n"
    "          done1 = abs(lEnd1) >= gradScaled;\n"
    "        }\n"
    "        if (!done2) {\n"
    "          lEnd2 = luma3(texture(uScene, uv2).rgb) - lLocalAvg;\n"
    "          done2 = abs(lEnd2) >= gradScaled;\n"
    "        }\n"
    "        if (!done1) uv1 -= off * steps[i];\n"
    "        if (!done2) uv2 += off * steps[i];\n"
    "      }\n"
    "      float dist1 = isHorz ? (uv.x - uv1.x) : (uv.y - uv1.y);\n"
    "      float dist2 = isHorz ? (uv2.x - uv.x) : (uv2.y - uv.y);\n"
    "      bool isDir1 = (dist1 < dist2);\n"
    "      float distMin = min(dist1, dist2);\n"
    "      float spanLen = dist1 + dist2;\n"
    "      float pixOff = -distMin / max(spanLen, 1e-5) + 0.5;\n"
    "      bool isMLtAvg = (lM < lLocalAvg);\n"
    "      bool goodSpan = ((isDir1 ? lEnd1 : lEnd2) < 0.0) != isMLtAvg;\n"
    "      float finalOff = max(goodSpan ? pixOff : 0.0, subpixOff);\n"
    "      vec2 aaUV = uv;\n"
    "      if (isHorz) aaUV.y += finalOff * stepLen; else aaUV.x += finalOff * stepLen;\n"
    "      c = mix(c, texture(uScene, aaUV).rgb, aaS);\n"
    "    }\n"
    "  }\n"
    "  return c;\n"
    "}\n"
    "\n"
    "vec3 applyColorGrade(vec3 c, int mode) {\n"
    "  if (mode == 0) return c;\n"
    "  float l = luma3(c);\n"
    "  if (mode == 1) {\n"
    "    /* Remastered Rich: shadow-safe S-curve + natural vibrance */\n"
    "    float maxc = max(c.r, max(c.g, c.b));\n"
    "    float minc = min(c.r, min(c.g, c.b));\n"
    "    float sat = (maxc > 1e-4) ? ((maxc - minc) / maxc) : 0.0;\n"
    "    float vib = 0.14 * (1.0 - sat * 0.7);\n"
    "    c = mix(vec3(l), c, 1.0 + vib);\n"
    "    vec3 sc = c * c * (3.0 - 2.0 * c);\n"
    "    c = mix(c, sc, 0.16);\n"
    "    c = pow(max(c, vec3(0.0)), vec3(0.97));\n"
    "  } else if (mode == 2) {\n"
    "    /* Cinematic Warm: golden torchlight highlights + deep cool shadows */\n"
    "    vec3 shad = vec3(0.95, 0.98, 1.05);\n"
    "    vec3 high = vec3(1.06, 1.01, 0.92);\n"
    "    c *= mix(shad, high, smoothstep(0.12, 0.82, l));\n"
    "    c = mix(vec3(l), c, 1.12);\n"
    "    vec3 sc = c * c * (3.0 - 2.0 * c);\n"
    "    c = mix(c, sc, 0.22);\n"
    "  } else if (mode == 3) {\n"
    "    /* Crisp Daylight: clean neutral-cool clarity */\n"
    "    c *= vec3(0.98, 1.00, 1.04);\n"
    "    c = mix(vec3(l), c, 1.08);\n"
    "    c = pow(max(c, vec3(0.0)), vec3(0.95));\n"
    "  }\n"
    "  return clamp(c, 0.0, 1.0);\n"
    "}\n"
    "\n"
    "/* Wide-radius midtone local contrast ('Clarity') for 3D depth on pre-rendered art */\n"
    "vec3 applyClarity(vec3 c, vec2 uv, float strength) {\n"
    "  vec2 s1 = uInvSize * 12.0;\n"
    "  vec2 s2 = uInvSize * 26.0;\n"
    "  vec3 blur = 0.125 * (\n"
    "    texture(uScene, uv + vec2(-s1.x, -s1.y)).rgb + texture(uScene, uv + vec2(s1.x, -s1.y)).rgb +\n"
    "    texture(uScene, uv + vec2(-s1.x,  s1.y)).rgb + texture(uScene, uv + vec2(s1.x,  s1.y)).rgb +\n"
    "    texture(uScene, uv + vec2(-s2.x,   0.0)).rgb + texture(uScene, uv + vec2(s2.x,   0.0)).rgb +\n"
    "    texture(uScene, uv + vec2(  0.0, -s2.y)).rgb + texture(uScene, uv + vec2( 0.0,  s2.y)).rgb);\n"
    "  float l = luma3(c);\n"
    "  float midMask = 0.25 + 0.75 * (4.0 * l * (1.0 - l));\n"
    "  vec3 diff = clamp((c - blur) * (strength * 0.85), vec3(-0.11), vec3(0.11));\n"
    "  return clamp(c + diff * midMask, 0.0, 1.0);\n"
    "}\n"
    "\n"
    "/* ACES-inspired filmic HDR highlight roll-off (adjustable 0..100% strength) */\n"
    "vec3 applyFilmicToneMap(vec3 c, float strength) {\n"
    "  float s = clamp(strength, 0.0, 1.0);\n"
    "  vec3 lin = pow(max(c, vec3(0.0)), vec3(2.2)) * mix(1.0, 1.32, s);\n"
    "  vec3 num = lin * (2.51 * lin + 0.03);\n"
    "  vec3 den = lin * (2.43 * lin + 0.59) + 0.14;\n"
    "  vec3 mapped = clamp(num / den, 0.0, 1.0);\n"
    "  return mix(c, pow(mapped, vec3(1.0 / 2.2)), s);\n"
    "}\n"
    "\n"
    "/* Subtle procedural drifting warm dust motes for living room atmosphere */\n"
    "float dustLayer(vec2 p, float t, float scale) {\n"
    "  vec2 gp = p * scale + vec2(sin(t * 0.25 + p.y * 3.0) * 0.35, -t * 0.18);\n"
    "  vec2 cell = floor(gp);\n"
    "  vec2 f = fract(gp);\n"
    "  float h1 = fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453);\n"
    "  float h2 = fract(sin(dot(cell, vec2(269.5, 183.3))) * 43758.5453);\n"
    "  float h3 = fract(sin(dot(cell, vec2(419.2, 371.9))) * 43758.5453);\n"
    "  if (h3 < 0.84) return 0.0;\n"
    "  vec2 center = vec2(0.2 + 0.6 * h1, 0.2 + 0.6 * h2);\n"
    "  float d = length(f - center);\n"
    "  float pulse = 0.45 + 0.55 * sin(t * (1.2 + h1 * 1.8) + h2 * 6.283);\n"
    "  return smoothstep(0.085, 0.012, d) * pulse;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "  vec2 uv = vec2(vUV.x, 1.0 - vUV.y);\n"
    "  vec3 col = sampleSceneFX(uv);\n"
    "  if (uDeband != 0) {\n"
    "    /* Proper wide-radius mpv-style 2-ring jittered debanding (smooths real 12-35px 8-bit shadow/sky bands) */\n"
    "    float h1 = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
    "    float h2 = fract(52.9829189 * fract(dot(gl_FragCoord.xy + vec2(3.1, 7.7), vec2(0.06711056, 0.00583715))));\n"
    "    float ang = h1 * 6.2831853;\n"
    "    vec2 rot1 = vec2(cos(ang), sin(ang));\n"
    "    vec2 rot2 = vec2(-rot1.y, rot1.x);\n"
    "    float r1 = 7.0 + 7.0 * h2;\n"
    "    float r2 = 18.0 + 14.0 * h1;\n"
    "    vec3 avg1 = 0.25 * (\n"
    "      texture(uScene, uv + rot1 * uInvSize * r1).rgb +\n"
    "      texture(uScene, uv - rot1 * uInvSize * r1).rgb +\n"
    "      texture(uScene, uv + rot2 * uInvSize * r1).rgb +\n"
    "      texture(uScene, uv - rot2 * uInvSize * r1).rgb);\n"
    "    vec3 avg2 = 0.25 * (\n"
    "      texture(uScene, uv + (rot1 + rot2) * 0.7071 * uInvSize * r2).rgb +\n"
    "      texture(uScene, uv - (rot1 + rot2) * 0.7071 * uInvSize * r2).rgb +\n"
    "      texture(uScene, uv + (rot1 - rot2) * 0.7071 * uInvSize * r2).rgb +\n"
    "      texture(uScene, uv - (rot1 - rot2) * 0.7071 * uInvSize * r2).rgb);\n"
    "    float d1 = max3(abs(avg1 - col));\n"
    "    float d2 = max3(abs(avg2 - col));\n"
    "    /* Smoothly blend only where local gradient is flat (< 12/255), keeping all real edges untouched */\n"
    "    float w1 = 1.0 - smoothstep(0.016, 0.048, d1);\n"
    "    float w2 = 1.0 - smoothstep(0.012, 0.038, d2);\n"
    "    col = mix(col, avg1, w1 * 0.65);\n"
    "    col = mix(col, avg2, w2 * 0.45);\n"
    "  }\n"
    "  if (uClarity > 0.005) {\n"
    "    col = applyClarity(col, uv, clamp(uClarity, 0.0, 1.0));\n"
    "  }\n"
    "  if (uTransAlpha < 0.999) {\n"
    "    vec3 prev = texture(uPrevScene, uv).rgb;\n"
    "    col = mix(prev, col, uTransAlpha);\n"
    "  }\n"
    "  vec3 blRaw = (uBloomStrength > 0.001 || uAtmosphere != 0) ? texture(uBloom, uv).rgb : vec3(0.0);\n"
    "  if (uBloomStrength > 0.001) {\n"
    "    float flick = (uAtmosphere != 0)\n"
    "      ? (1.0 + 0.08 * sin(uTime * 7.3) + 0.05 * sin(uTime * 13.1 + 1.4) + 0.03 * sin(uTime * 21.7))\n"
    "      : 1.0;\n"
    "    vec3 bl = blRaw * (uBloomStrength * flick);\n"
    "    col = 1.0 - (1.0 - col) * exp(-bl * 1.15);\n"
    "  }\n"
    "  if (uAtmosphere != 0) {\n"
    "    /* Subtle warm drifting dust motes (attenuated in dark corners, gently lit near bloom) */\n"
    "    float dm = dustLayer(vUV * vec2(1.6, 1.0), uTime, 9.0) * 0.65 +\n"
    "               dustLayer(vUV * vec2(1.6, 1.0) + vec2(3.7, 1.9), uTime * 0.72, 15.0) * 0.45;\n"
    "    float lightBoost = 0.28 + clamp(luma3(blRaw) * 2.5, 0.0, 0.85);\n"
    "    float edgeFade = smoothstep(0.0, 0.12, vUV.y) * smoothstep(1.0, 0.85, vUV.y);\n"
    "    col += vec3(1.0, 0.91, 0.72) * (dm * 0.075 * lightBoost * edgeFade);\n"
    "  }\n"
    "  col = applyColorGrade(col, uGradeMode);\n"
    "  if (uFilmic > 0.005) {\n"
    "    col = applyFilmicToneMap(col, uFilmic);\n"
    "  }\n"
    "  if (uVignette > 0.001) {\n"
    "    vec2 d = (vUV - 0.5) * vec2(1.15, 1.0);\n"
    "    float v = 1.0 - smoothstep(0.42, 0.95, length(d)) * uVignette;\n"
    "    col *= v;\n"
    "  }\n"
    "  if (uDeband != 0) {\n"
    "    /* Triangular-PDF interleaved gradient noise dithering */\n"
    "    float n1 = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
    "    float n2 = fract(52.9829189 * fract(dot(gl_FragCoord.xy + vec2(1.7, 2.3), vec2(0.06711056, 0.00583715))));\n"
    "    col += (n1 + n2 - 1.0) * (0.75 / 255.0);\n"
    "  }\n"
    "  if (uCompareSplit > 0.01) {\n"
    "    if (abs(vUV.x - uCompareSplit) < uInvSize.x * 1.5) {\n"
    "      col = vec3(0.96, 0.82, 0.36);\n"
    "    } else if (vUV.x < uCompareSplit) {\n"
    "      vec2 dSize = (uLayout == 1) ? vec2(800.0, 500.0) : ((uLayout == 2) ? vec2(800.0, 450.0) : vec2(800.0, 600.0));\n"
    "      vec2 dPos = vUV * dSize;\n"
    "      vec2 oPos = dPos;\n"
    "      if (uDialogBar != 0 && uLayout != 0 && dPos.y >= dSize.y - 100.0) {\n"
    "        oPos.y = 500.0 + (dPos.y - (dSize.y - 100.0));\n"
    "      } else if (uLayout == 1) {\n"
    "        oPos.y = (dPos.y < 450.0) ? (dPos.y + 75.0) : ((dPos.y - 450.0) + 537.0);\n"
    "      } else if (uLayout == 2) {\n"
    "        oPos.y = dPos.y + 75.0;\n"
    "      }\n"
    "      vec2 oUV = clamp((floor(oPos) + 0.5) / vec2(800.0, 600.0), vec2(0.0), vec2(1.0));\n"
    "      col = texture(uOrigTex, oUV).rgb;\n"
    "    }\n"
    "  }\n"
    "  FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[GL] Shader compile error: %s\n", log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(const char *vs_src, const char *fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, vs);
    p_glAttachShader(p, fs);
    p_glLinkProgram(p);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "[GL] Program link error: %s\n", log);
        p_glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void apply_vsync_mode(void) {
    if (!g_gl_active) return;
    if (g_cfg.vsync_mode == VSYNC_ADAPTIVE) {
        if (SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(1);
    } else if (g_cfg.vsync_mode == VSYNC_ON) {
        SDL_GL_SetSwapInterval(1);
    } else {
        SDL_GL_SetSwapInterval(0);
    }
}

static void init_osd_font(void) {
    if (!g_ttf_font_path[0]) {
        const char *env = getenv("MHA2_FONT_PATH");
        const char *paths[] = {
            env,
            "fonts/DejaVuSans.ttf",
            "/run/current-system/sw/share/X11/fonts/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            NULL
        };
        for (int i = 0; i < 5; i++) {
            if (!paths[i] || !paths[i][0]) continue;
            TTF_Font *f = TTF_OpenFont(paths[i], 18);
            if (f) {
                snprintf(g_ttf_font_path, sizeof(g_ttf_font_path), "%s", paths[i]);
                g_osd_font = f;
                break;
            }
        }
    } else if (!g_osd_font) {
        g_osd_font = TTF_OpenFont(g_ttf_font_path, 18);
    }

    if (!g_game_ttf_path[0]) {
        const char *env_game = getenv("MHA2_GAME_FONT_PATH");
        const char *gpaths[] = {
            env_game,
            "fonts/DejaVuSans-Bold.ttf",
            "/run/current-system/sw/share/X11/fonts/Arial_Bold.ttf",
            "/run/current-system/sw/share/X11/fonts/calibrib.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
            g_ttf_font_path,
            NULL
        };
        for (int i = 0; i < 7; i++) {
            if (!gpaths[i] || !gpaths[i][0]) continue;
            TTF_Font *tf = TTF_OpenFont(gpaths[i], 32);
            if (tf) {
                TTF_CloseFont(tf);
                snprintf(g_game_ttf_path, sizeof(g_game_ttf_path), "%s", gpaths[i]);
                g_game_ttf_is_bold_file = (strstr(gpaths[i], "Bold") != NULL || strstr(gpaths[i], "calibrib") != NULL);
                break;
            }
        }
    }

    if (!g_serif_ttf_path[0]) {
        const char *spaths[] = {
            "fonts/DejaVuSerif-Bold.ttf",
            "/run/current-system/sw/share/X11/fonts/Georgia_Bold.ttf",
            "/run/current-system/sw/share/X11/fonts/DejaVuSerif-Bold.ttf",
            "/run/current-system/sw/share/X11/fonts/FreeSerifBold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf",
            "/usr/share/fonts/TTF/DejaVuSerif-Bold.ttf",
            g_game_ttf_path,
            NULL
        };
        for (int i = 0; i < 7; i++) {
            if (!spaths[i] || !spaths[i][0]) continue;
            TTF_Font *tf = TTF_OpenFont(spaths[i], 32);
            if (tf) {
                TTF_CloseFont(tf);
                snprintf(g_serif_ttf_path, sizeof(g_serif_ttf_path), "%s", spaths[i]);
                g_serif_ttf_is_bold_file = (strstr(spaths[i], "Bold") != NULL);
                break;
            }
        }
    }
}

static TTF_Font *get_native_font(int px_size, int bold) {
    if (px_size < 8) px_size = 8;
    if (px_size > 200) px_size = 200;
    for (int i = 0; i < MAX_FONT_CACHE; i++) {
        if (g_font_cache[i].font && g_font_cache[i].px_size == px_size && g_font_cache[i].bold == bold) {
            return g_font_cache[i].font;
        }
    }
    if (!g_ttf_font_path[0]) init_osd_font();
    const char *path = g_ttf_font_path;
    if (bold == 3 && g_serif_ttf_path[0]) path = g_serif_ttf_path;
    else if (bold == 2 && g_game_ttf_path[0]) path = g_game_ttf_path;
    if (!path || !path[0]) return g_osd_font;

    TTF_Font *f = TTF_OpenFont(path, px_size);
    if (!f) return g_osd_font;
    if (bold == 1 || (bold == 2 && !g_game_ttf_is_bold_file) || (bold == 3 && !g_serif_ttf_is_bold_file)) {
        TTF_SetFontStyle(f, TTF_STYLE_BOLD);
    }

    /* Store in free slot or evict slot 0 */
    int slot = 0;
    for (int i = 0; i < MAX_FONT_CACHE; i++) {
        if (!g_font_cache[i].font) { slot = i; break; }
    }
    if (g_font_cache[slot].font) TTF_CloseFont(g_font_cache[slot].font);
    g_font_cache[slot].px_size = px_size;
    g_font_cache[slot].bold = bold;
    g_font_cache[slot].font = f;
    return f;
}

static void flush_canvas_text_cache(void) {
    for (int i = 0; i < MAX_CANVAS_TEXT_CACHE; i++) {
        if (g_canvas_text_cache[i].in_use && g_canvas_text_cache[i].tex) {
            glDeleteTextures(1, &g_canvas_text_cache[i].tex);
        }
        g_canvas_text_cache[i].in_use = 0;
        g_canvas_text_cache[i].tex = 0;
    }
}

static CanvasTextEntry *get_or_render_outlined_text_tex(const char *utf8, int px_size, int outline_px,
                                                        uint32_t fill_rgb, uint32_t outline_rgb) {
    if (!utf8 || !utf8[0]) return NULL;
    g_canvas_text_tick++;
    int lru_slot = 0;
    uint32_t lru_tick = 0xFFFFFFFFu;
    for (int i = 0; i < MAX_CANVAS_TEXT_CACHE; i++) {
        CanvasTextEntry *e = &g_canvas_text_cache[i];
        if (e->in_use && e->px_size == px_size && e->outline_px == outline_px &&
            e->fill_rgb == fill_rgb && e->outline_rgb == outline_rgb &&
            strcmp(e->utf8, utf8) == 0) {
            e->last_used = g_canvas_text_tick;
            return e;
        }
        if (!e->in_use) {
            lru_slot = i;
            lru_tick = 0;
        } else if (e->last_used < lru_tick) {
            lru_tick = e->last_used;
            lru_slot = i;
        }
    }

    int font_slot = (g_cfg.font_style == FONT_SERIF) ? 3 : 2;
    TTF_Font *f = get_native_font(px_size, font_slot);
    if (!f) return NULL;

    SDL_Color out_c = {
        (uint8_t)((outline_rgb >> 16) & 0xFFu),
        (uint8_t)((outline_rgb >> 8) & 0xFFu),
        (uint8_t)(outline_rgb & 0xFFu),
        255
    };
    SDL_Color fill_c = {
        (uint8_t)((fill_rgb >> 16) & 0xFFu),
        (uint8_t)((fill_rgb >> 8) & 0xFFu),
        (uint8_t)(fill_rgb & 0xFFu),
        255
    };

    TTF_SetFontOutline(f, outline_px);
    SDL_Surface *out_s = TTF_RenderUTF8_Blended(f, utf8, out_c);
    TTF_SetFontOutline(f, 0);
    SDL_Surface *fill_s = TTF_RenderUTF8_Blended(f, utf8, fill_c);
    if (!fill_s) {
        if (out_s) SDL_FreeSurface(out_s);
        return NULL;
    }

    int pad_sh = (px_size >= 80) ? 4 : 2;
    int tw = (out_s ? out_s->w : fill_s->w) + pad_sh;
    int th = (out_s ? out_s->h : fill_s->h) + pad_sh;
    SDL_Surface *rgba = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!rgba) {
        if (out_s) SDL_FreeSurface(out_s);
        SDL_FreeSurface(fill_s);
        return NULL;
    }
    SDL_FillRect(rgba, NULL, SDL_MapRGBA(rgba->format, 0, 0, 0, 0));

    if (g_cfg.text_backdrop) {
        /* Subtle rounded dark readability backing behind hover tooltips & dialog lines */
        int r = th / 4;
        if (r > 14) r = 14;
        uint32_t *px = (uint32_t *)rgba->pixels;
        int pitch_px = rgba->pitch / 4;
        for (int y = 0; y < th; y++) {
            for (int x = 0; x < tw; x++) {
                int dx = 0, dy = 0;
                if (x < r) dx = r - x;
                else if (x >= tw - r) dx = x - (tw - 1 - r);
                if (y < r) dy = r - y;
                else if (y >= th - r) dy = y - (th - 1 - r);
                if (dx * dx + dy * dy <= r * r) {
                    px[y * pitch_px + x] = (120u << 24) | (16u << 16) | (10u << 8) | 8u;
                }
            }
        }
    }

    if (out_s) {
        /* Subtle soft drop shadow for legibility over bright backgrounds */
        SDL_SetSurfaceBlendMode(out_s, SDL_BLENDMODE_BLEND);
        SDL_SetSurfaceAlphaMod(out_s, g_cfg.text_backdrop ? 185 : 145);
        SDL_Rect sh_r = { pad_sh, pad_sh, out_s->w, out_s->h };
        SDL_BlitSurface(out_s, NULL, rgba, &sh_r);

        /* Crisp full-opacity vector outline */
        SDL_SetSurfaceAlphaMod(out_s, 255);
        SDL_Rect out_r = { 0, 0, out_s->w, out_s->h };
        SDL_BlitSurface(out_s, NULL, rgba, &out_r);
        SDL_FreeSurface(out_s);
    }

    SDL_SetSurfaceBlendMode(fill_s, SDL_BLENDMODE_BLEND);
    SDL_Rect fill_r = { outline_px, outline_px, fill_s->w, fill_s->h };
    SDL_BlitSurface(fill_s, NULL, rgba, &fill_r);
    SDL_FreeSurface(fill_s);

    CanvasTextEntry *slot = &g_canvas_text_cache[lru_slot];
    if (slot->in_use && slot->tex) glDeleteTextures(1, &slot->tex);
    memset(slot, 0, sizeof(*slot));

    glGenTextures(1, &slot->tex);
    glBindTexture(GL_TEXTURE_2D, slot->tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
    p_glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    slot->in_use = 1;
    slot->last_used = g_canvas_text_tick;
    slot->px_size = px_size;
    slot->outline_px = outline_px;
    slot->fill_rgb = fill_rgb;
    slot->outline_rgb = outline_rgb;
    snprintf(slot->utf8, sizeof(slot->utf8), "%s", utf8);
    slot->tex_w = tw;
    slot->tex_h = th;
    SDL_FreeSurface(rgba);
    return slot;
}

static TextTexEntry *get_or_render_text_tex(const GlTextDraw *td, int native_px) {
    int bold = (td->font_weight >= 600) ? 1 : 0;
    g_text_cache_tick++;
    int lru_slot = 0;
    uint32_t lru_tick = 0xFFFFFFFFu;

    for (int i = 0; i < MAX_TEXT_TEX_CACHE; i++) {
        TextTexEntry *e = &g_text_cache[i];
        if (e->in_use && e->px_size == native_px && e->bold == bold &&
            e->text_color == td->text_color && e->bk_mode == td->bk_mode &&
            (td->bk_mode != 2 || e->bk_color == td->bk_color) &&
            strcmp(e->utf8, td->utf8) == 0) {
            e->last_used = g_text_cache_tick;
            return e;
        }
        if (!e->in_use) {
            lru_slot = i;
            lru_tick = 0;
        } else if (e->last_used < lru_tick) {
            lru_tick = e->last_used;
            lru_slot = i;
        }
    }

    TTF_Font *f = get_native_font(native_px, bold);
    if (!f || !td->utf8[0]) return NULL;

    SDL_Color fg = {
        (uint8_t)(td->text_color & 0xFFu),
        (uint8_t)((td->text_color >> 8) & 0xFFu),
        (uint8_t)((td->text_color >> 16) & 0xFFu),
        255
    };
    SDL_Surface *txt = TTF_RenderUTF8_Blended(f, td->utf8, fg);
    if (!txt) return NULL;

    SDL_Surface *rgba = SDL_CreateRGBSurfaceWithFormat(0, txt->w, txt->h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!rgba) { SDL_FreeSurface(txt); return NULL; }
    if (td->bk_mode == 2 /* OPAQUE */) {
        SDL_FillRect(rgba, NULL, SDL_MapRGBA(rgba->format,
            (uint8_t)(td->bk_color & 0xFFu),
            (uint8_t)((td->bk_color >> 8) & 0xFFu),
            (uint8_t)((td->bk_color >> 16) & 0xFFu), 255));
    } else {
        SDL_FillRect(rgba, NULL, SDL_MapRGBA(rgba->format, 0, 0, 0, 0));
    }
    SDL_SetSurfaceBlendMode(txt, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(txt, NULL, rgba, NULL);
    SDL_FreeSurface(txt);

    TextTexEntry *slot = &g_text_cache[lru_slot];
    if (slot->in_use && slot->tex) glDeleteTextures(1, &slot->tex);
    memset(slot, 0, sizeof(*slot));

    glGenTextures(1, &slot->tex);
    glBindTexture(GL_TEXTURE_2D, slot->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rgba->w, rgba->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);

    slot->in_use = 1;
    slot->last_used = g_text_cache_tick;
    slot->px_size = native_px;
    slot->bold = bold;
    slot->text_color = td->text_color;
    slot->bk_color = td->bk_color;
    slot->bk_mode = td->bk_mode;
    snprintf(slot->utf8, sizeof(slot->utf8), "%s", td->utf8);
    slot->tex_w = rgba->w;
    slot->tex_h = rgba->h;
    SDL_FreeSurface(rgba);
    return slot;
}

static GLuint render_pill_texture(const char *text, int *out_w, int *out_h) {
    if (!g_osd_font || !text || !text[0]) return 0;
    SDL_Color fg = { 245, 232, 185, 255 };
    SDL_Surface *txt = TTF_RenderUTF8_Blended(g_osd_font, text, fg);
    if (!txt) return 0;
    int pad_x = 14, pad_y = 7;
    int tw = txt->w + pad_x * 2;
    int th = txt->h + pad_y * 2;
    SDL_Surface *pill = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!pill) { SDL_FreeSurface(txt); return 0; }
    SDL_FillRect(pill, NULL, SDL_MapRGBA(pill->format, 14, 14, 20, 205));
    SDL_Rect top = {0, 0, tw, 2}, bot = {0, th - 2, tw, 2};
    SDL_Rect lft = {0, 0, 2, th}, rgt = {tw - 2, 0, 2, th};
    Uint32 border = SDL_MapRGBA(pill->format, 212, 175, 55, 220);
    SDL_FillRect(pill, &top, border);
    SDL_FillRect(pill, &bot, border);
    SDL_FillRect(pill, &lft, border);
    SDL_FillRect(pill, &rgt, border);
    SDL_Rect dst = { pad_x, pad_y, txt->w, txt->h };
    SDL_BlitSurface(txt, NULL, pill, &dst);
    SDL_FreeSurface(txt);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pill->pixels);
    SDL_FreeSurface(pill);
    *out_w = tw;
    *out_h = th;
    return tex;
}

static void destroy_post_fbos(void) {
    if (p_glDeleteFramebuffers) {
        if (g_scene_fbo[0]) { p_glDeleteFramebuffers(2, g_scene_fbo); g_scene_fbo[0] = g_scene_fbo[1] = 0; }
        if (g_bloom_fbo[0]) { p_glDeleteFramebuffers(2, g_bloom_fbo); g_bloom_fbo[0] = g_bloom_fbo[1] = 0; }
    }
    if (g_scene_tex[0]) { glDeleteTextures(2, g_scene_tex); g_scene_tex[0] = g_scene_tex[1] = 0; }
    if (g_bloom_tex[0]) { glDeleteTextures(2, g_bloom_tex); g_bloom_tex[0] = g_bloom_tex[1] = 0; }
    g_scene_fbo_w = g_scene_fbo_h = 0;
    g_bloom_w = g_bloom_h = 0;
    g_post_fbos_ok = 0;
    g_scene_has_rendered = 0;
    g_trans_active = 0;
    g_trans_pending = 0;
}

static int ensure_post_fbos(int vw, int vh) {
    if (vw <= 0 || vh <= 0) return 0;
    if (g_post_fbos_ok && g_scene_fbo_w == vw && g_scene_fbo_h == vh) return 1;
    destroy_post_fbos();

    p_glGenFramebuffers(2, g_scene_fbo);
    glGenTextures(2, g_scene_tex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, g_scene_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vw, vh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo[i]);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_scene_tex[i], 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
            destroy_post_fbos();
            return 0;
        }
    }

    int bw = vw / 4; if (bw < 64) bw = 64;
    int bh = vh / 4; if (bh < 48) bh = 48;
    p_glGenFramebuffers(2, g_bloom_fbo);
    glGenTextures(2, g_bloom_tex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, g_bloom_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, bw, bh, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_bloom_fbo[i]);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_bloom_tex[i], 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            /* Fallback to RGBA8 bloom buffer if RGBA16F isn't renderable */
            glBindTexture(GL_TEXTURE_2D, g_bloom_tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
                destroy_post_fbos();
                return 0;
            }
        }
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_scene_fbo_w = vw;
    g_scene_fbo_h = vh;
    g_bloom_w = bw;
    g_bloom_h = bh;
    g_scene_cur_idx = 0;
    g_scene_has_rendered = 0;
    g_post_fbos_ok = 1;
    return 1;
}

static void destroy_hd_canvas_fbos(void) {
    if (p_glDeleteFramebuffers && g_hd_canvas_fbo[0]) {
        p_glDeleteFramebuffers(2, g_hd_canvas_fbo);
        g_hd_canvas_fbo[0] = g_hd_canvas_fbo[1] = 0;
    }
    if (g_hd_canvas_tex[0]) {
        glDeleteTextures(2, g_hd_canvas_tex);
        g_hd_canvas_tex[0] = g_hd_canvas_tex[1] = 0;
    }
    if (g_patch_tex) {
        glDeleteTextures(1, &g_patch_tex);
        g_patch_tex = 0;
    }
    g_patch_tex_w = g_patch_tex_h = 0;
    g_hd_canvas_gw = g_hd_canvas_gh = 0;
    g_hd_canvas_tw = g_hd_canvas_th = 0;
    g_hd_canvas_ready = 0;
    g_hd_canvas_has_draws = 0;
}

static int ensure_hd_canvas_fbos(int game_w, int game_h) {
    if (game_w <= 0 || game_h <= 0) return 0;
    if (g_hd_canvas_ready && g_hd_canvas_gw == game_w && g_hd_canvas_gh == game_h) return 1;
    destroy_hd_canvas_fbos();

    int tw = game_w * HD_CANVAS_SCALE;
    int th = game_h * HD_CANVAS_SCALE;
    p_glGenFramebuffers(2, g_hd_canvas_fbo);
    glGenTextures(2, g_hd_canvas_tex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, g_hd_canvas_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_hd_canvas_fbo[i]);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_hd_canvas_tex[i], 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
            destroy_hd_canvas_fbos();
            return 0;
        }
        glViewport(0, 0, tw, th);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGenTextures(1, &g_patch_tex);
    g_hd_canvas_gw = game_w;
    g_hd_canvas_gh = game_h;
    g_hd_canvas_tw = tw;
    g_hd_canvas_th = th;
    g_hd_canvas_ready = 1;
    return 1;
}

static void destroy_sprite_gl_cache(void) {
    for (int i = 0; i < MAX_SPRITE_GL_CACHE; i++) {
        if (g_spr_gl_cache[i].in_use && g_spr_gl_cache[i].tex) {
            glDeleteTextures(1, &g_spr_gl_cache[i].tex);
        }
        g_spr_gl_cache[i].in_use = 0;
        g_spr_gl_cache[i].tex = 0;
        g_spr_gl_cache[i].sprite_id = -1;
    }
    g_spr_gl_resident = 0;
}

static SpriteGlEntry *get_or_upload_sprite_tex(int sprite_id) {
    if (sprite_id < 0) return NULL;
    g_spr_gl_tick++;
    int lru_slot = 0;
    uint32_t lru_tick = 0xFFFFFFFFu;
    for (int i = 0; i < MAX_SPRITE_GL_CACHE; i++) {
        SpriteGlEntry *e = &g_spr_gl_cache[i];
        if (e->in_use && e->sprite_id == sprite_id) {
            e->last_used = g_spr_gl_tick;
            return e;
        }
        if (!e->in_use) {
            lru_slot = i;
            lru_tick = 0;
        } else if (e->last_used < lru_tick) {
            lru_tick = e->last_used;
            lru_slot = i;
        }
    }
    HdImageRGBA img = { NULL, 0, 0 };
    if (hd_sprites_decode_rgba(sprite_id, &img) != 0 || !img.rgba) return NULL;
    GLint max_tex = 16384;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    if (img.width <= 0 || img.height <= 0 || img.width > max_tex || img.height > max_tex) {
        free(img.rgba);
        return NULL;
    }

    SpriteGlEntry *slot = &g_spr_gl_cache[lru_slot];
    int was_used = slot->in_use;
    if (slot->in_use && slot->tex) glDeleteTextures(1, &slot->tex);
    memset(slot, 0, sizeof(*slot));

    glGenTextures(1, &slot->tex);
    glBindTexture(GL_TEXTURE_2D, slot->tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba);
    p_glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    slot->in_use = 1;
    slot->sprite_id = sprite_id;
    slot->last_used = g_spr_gl_tick;
    slot->tex_w = img.width;
    slot->tex_h = img.height;
    if (!was_used) g_spr_gl_resident++;
    hd_sprites_free_rgba(&img);
    return slot;
}

int gl_renderer_init(const char *title, int w, int h) {
    if (g_sdl_window) return g_gl_active ? 1 : 0;
    int init_w = g_cfg.init_win_w > 0 ? g_cfg.init_win_w : (w > 0 ? w : 1280);
    int init_h = g_cfg.init_win_h > 0 ? g_cfg.init_win_h : (h > 0 ? h : 960);

    const char *vdrv = SDL_GetCurrentVideoDriver();
    int try_gl = !(vdrv && strcmp(vdrv, "dummy") == 0);

    if (try_gl) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

        Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (g_cfg.start_fullscreen) {
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            g_fullscreen = 1;
        }
        g_sdl_window = SDL_CreateWindow(
            title ? title : "Moorhuhn Adventure 2: Der Fluch des Goldes [Remastered v3]",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, init_w, init_h, flags);
        if (g_sdl_window) {
            g_gl_ctx = SDL_GL_CreateContext(g_sdl_window);
            if (g_gl_ctx && load_gl_procs()) {
                g_prog_upscale    = link_program(VS_FULLSCREEN, FS_UPSCALE);
                g_prog_sidebar    = link_program(VS_FULLSCREEN, FS_SIDEBAR);
                g_prog_osd        = link_program(VS_OSD, FS_OSD);
                g_prog_solid      = link_program(VS_OSD, FS_SOLID);
                g_prog_bloom_ext  = link_program(VS_FULLSCREEN, FS_BLOOM_EXTRACT);
                g_prog_bloom_blur = link_program(VS_FULLSCREEN, FS_BLOOM_BLUR);
                g_prog_post       = link_program(VS_FULLSCREEN, FS_POST);
                if (g_prog_upscale && g_prog_sidebar && g_prog_osd && g_prog_solid &&
                    g_prog_bloom_ext && g_prog_bloom_blur && g_prog_post) {
                    p_glGenVertexArrays(1, &g_vao);
                    glGenTextures(1, &g_frame_tex);
                    glGenTextures(1, &g_bg_orig_tex);
                    glGenTextures(1, &g_bg_hd_tex);
                    glGenTextures(1, &g_bg_wide_tex);
                    glGenTextures(1, &g_cursor_tex);
                    g_gl_thread = pthread_self();
                    g_gl_thread_valid = 1;
                    g_gl_active = 1;
                    apply_vsync_mode();
                    init_osd_font();
                    SDL_StartTextInput();
                    fprintf(stderr, "[GL] OpenGL 3.3 Core Remaster v5 initialized (%s, %s, Aspect:%s, Bloom:%s, Grade:%s, HD-Bg:%d, HD-Sprites:%d)\n",
                            gl_shader_name(g_cfg.shader_mode), gl_vsync_name(g_cfg.vsync_mode),
                            gl_aspect_name(g_cfg.aspect_mode),
                            gl_bloom_name(g_cfg.bloom_level),
                            gl_grade_name(g_cfg.color_grade),
                            hd_assets_count(), hd_sprites_count());
                    gl_renderer_show_toast("Remastered v5 | Press F1 for Graphics & Remaster FX Menu | F11:Fullscreen | F12:Aspect");
                    return 1;
                }
            }
            if (g_gl_ctx) { SDL_GL_DeleteContext(g_gl_ctx); g_gl_ctx = NULL; }
            SDL_DestroyWindow(g_sdl_window);
            g_sdl_window = NULL;
        }
    }

    g_sdl_window = SDL_CreateWindow(
        title ? title : "Moorhuhn Adventure 2: Der Fluch des Goldes",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w > 0 ? w : 800, h > 0 ? h : 600, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (g_sdl_window) {
        g_sdl_renderer = SDL_CreateRenderer(g_sdl_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!g_sdl_renderer) g_sdl_renderer = SDL_CreateRenderer(g_sdl_window, -1, SDL_RENDERER_SOFTWARE);
        if (g_sdl_renderer) SDL_RenderSetLogicalSize(g_sdl_renderer, 800, 600);
        SDL_StartTextInput();
    }
    return 0;
}

/* ---- Interactive F1 Graphics & Settings Menu State (forward decl) ---- */
static void gl_menu_mark_dirty(void);
static void gl_menu_draw(int draw_w, int draw_h);
static void gl_menu_shutdown(void);
static int  gl_menu_needs_redraw(void);
static void gl_dialog_log_shutdown(void);

void gl_renderer_shutdown(void) {
    if (g_gl_active && g_gl_thread_valid && !pthread_equal(pthread_self(), g_gl_thread)) {
        return;
    }
    for (int i = 0; i < MAX_TEXT_TEX_CACHE; i++) {
        if (g_text_cache[i].in_use && g_text_cache[i].tex) {
            glDeleteTextures(1, &g_text_cache[i].tex);
        }
        g_text_cache[i].in_use = 0;
        g_text_cache[i].tex = 0;
    }
    for (int i = 0; i < MAX_CANVAS_TEXT_CACHE; i++) {
        if (g_canvas_text_cache[i].in_use && g_canvas_text_cache[i].tex) {
            glDeleteTextures(1, &g_canvas_text_cache[i].tex);
        }
        g_canvas_text_cache[i].in_use = 0;
        g_canvas_text_cache[i].tex = 0;
    }
    for (int i = 0; i < MAX_FONT_CACHE; i++) {
        if (g_font_cache[i].font) {
            TTF_CloseFont(g_font_cache[i].font);
            g_font_cache[i].font = NULL;
        }
    }
    destroy_sprite_gl_cache();
    destroy_hd_canvas_fbos();
    destroy_post_fbos();
    gl_menu_shutdown();
    gl_dialog_log_shutdown();
    if (g_hud_tex) { glDeleteTextures(1, &g_hud_tex); g_hud_tex = 0; }
    if (g_toast_tex) { glDeleteTextures(1, &g_toast_tex); g_toast_tex = 0; }
    if (g_frame_tex) { glDeleteTextures(1, &g_frame_tex); g_frame_tex = 0; }
    if (g_bg_orig_tex) { glDeleteTextures(1, &g_bg_orig_tex); g_bg_orig_tex = 0; }
    if (g_bg_hd_tex) { glDeleteTextures(1, &g_bg_hd_tex); g_bg_hd_tex = 0; }
    if (g_bg_wide_tex) { glDeleteTextures(1, &g_bg_wide_tex); g_bg_wide_tex = 0; g_bg_wide_has = 0; }
    if (g_remastered_badge_tex) { glDeleteTextures(1, &g_remastered_badge_tex); g_remastered_badge_tex = 0; }
    g_remastered_badge_ink_w = g_remastered_badge_ink_h = g_remastered_badge_pad = 0;
    g_remastered_badge_tried = 0;
    g_bg_is_main_menu = 0;
    if (g_cursor_tex) { glDeleteTextures(1, &g_cursor_tex); g_cursor_tex = 0; }
    if (g_vao && p_glDeleteVertexArrays) { p_glDeleteVertexArrays(1, &g_vao); g_vao = 0; }
    if (g_prog_upscale && p_glDeleteProgram) { p_glDeleteProgram(g_prog_upscale); g_prog_upscale = 0; }
    if (g_prog_sidebar && p_glDeleteProgram) { p_glDeleteProgram(g_prog_sidebar); g_prog_sidebar = 0; }
    if (g_prog_osd && p_glDeleteProgram) { p_glDeleteProgram(g_prog_osd); g_prog_osd = 0; }
    if (g_prog_solid && p_glDeleteProgram) { p_glDeleteProgram(g_prog_solid); g_prog_solid = 0; }
    if (g_prog_bloom_ext && p_glDeleteProgram) { p_glDeleteProgram(g_prog_bloom_ext); g_prog_bloom_ext = 0; }
    if (g_prog_bloom_blur && p_glDeleteProgram) { p_glDeleteProgram(g_prog_bloom_blur); g_prog_bloom_blur = 0; }
    if (g_prog_post && p_glDeleteProgram) { p_glDeleteProgram(g_prog_post); g_prog_post = 0; }
    if (g_osd_font) { TTF_CloseFont(g_osd_font); g_osd_font = NULL; }
    if (g_gl_ctx) { SDL_GL_DeleteContext(g_gl_ctx); g_gl_ctx = NULL; }
    g_gl_thread_valid = 0;
    g_gl_active = 0;
    g_has_frame = 0;
    g_bg_active = 0;
    g_cursor_ready = 0;
}

int gl_renderer_is_active(void) { return g_gl_active; }

int gl_renderer_is_gl_thread(void) {
    return g_gl_active && g_gl_thread_valid && pthread_equal(pthread_self(), g_gl_thread);
}

void gl_renderer_show_toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_toast_text, sizeof(g_toast_text), fmt, ap);
    va_end(ap);
    g_toast_until = SDL_GetTicks() + 2600u;
    g_toast_dirty = 1;
}

void gl_renderer_cycle_shader(void) {
    g_cfg.shader_mode = (g_cfg.shader_mode + 1) % SHADER_COUNT;
    gl_config_save(NULL);
    gl_menu_mark_dirty();
    gl_renderer_show_toast("Upscaler: %s", gl_shader_name(g_cfg.shader_mode));
    gl_renderer_redraw_if_needed(1);
}

void gl_renderer_cycle_vsync(void) {
    g_cfg.vsync_mode = (g_cfg.vsync_mode + 1) % VSYNC_COUNT;
    apply_vsync_mode();
    gl_config_save(NULL);
    gl_menu_mark_dirty();
    gl_renderer_show_toast("Frame Pacing: %s", gl_vsync_name(g_cfg.vsync_mode));
    gl_renderer_redraw_if_needed(1);
}

void gl_renderer_cycle_aspect(void) {
    g_cfg.aspect_mode = (g_cfg.aspect_mode + 1) % ASPECT_COUNT;
    gl_config_save(NULL);
    gl_menu_mark_dirty();
    gl_renderer_show_toast("Aspect / Widescreen: %s", gl_aspect_name(g_cfg.aspect_mode));
    gl_renderer_redraw_if_needed(1);
}

void gl_renderer_toggle_fullscreen(void) {
    if (!g_sdl_window) return;
    g_fullscreen = !g_fullscreen;
    SDL_SetWindowFullscreen(g_sdl_window, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!g_fullscreen && g_cfg.init_win_w >= 640 && g_cfg.init_win_h >= 450) {
        SDL_SetWindowSize(g_sdl_window, g_cfg.init_win_w, g_cfg.init_win_h);
        SDL_SetWindowPosition(g_sdl_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    gl_config_save(NULL);
    gl_menu_mark_dirty();
    gl_renderer_show_toast("%s", g_fullscreen ? "Borderless Fullscreen" : "Windowed Mode");
    gl_renderer_redraw_if_needed(1);
}

void gl_renderer_toggle_hud(void) {
    g_cfg.show_hud = !g_cfg.show_hud;
    gl_config_save(NULL);
    gl_menu_mark_dirty();
    gl_renderer_show_toast("Stats HUD: %s", g_cfg.show_hud ? "ON" : "OFF");
    gl_renderer_redraw_if_needed(1);
}

void gl_renderer_toggle_hd_text(void) {
    g_cfg.hd_vector_text = !g_cfg.hd_vector_text;
    gl_config_save(NULL);
    gl_menu_mark_dirty();
    gl_renderer_show_toast("Native HD Vector Text: %s", g_cfg.hd_vector_text ? "ON (Monitor DPI)" : "OFF (800x600 Original)");
    gl_renderer_redraw_if_needed(1);
}

void gl_renderer_toggle_hd_assets(void) {
    g_cfg.hd_assets_enabled = !g_cfg.hd_assets_enabled;
    gl_config_save(NULL);
    gl_menu_mark_dirty();
    if (g_cfg.hd_assets_enabled && g_bg_active && g_bg_hd_id >= 0) {
        gl_renderer_show_toast("HD Assets: ON (%s %dx%d)", hd_assets_get_name(g_bg_hd_id), g_bg_hd_w, g_bg_hd_h);
    } else if (g_cfg.hd_assets_enabled) {
        gl_renderer_show_toast("HD Assets: ON (%d indexed in '%s')", hd_assets_count(), g_cfg.hd_assets_dir);
    } else {
        gl_renderer_show_toast("HD Assets: OFF (Original 800x600)");
    }
    gl_renderer_redraw_if_needed(1);
}

void gl_renderer_toggle_decoupled_cursor(void) {
    g_cfg.decoupled_cursor = !g_cfg.decoupled_cursor;
    gl_config_save(NULL);
    gl_menu_mark_dirty();
    gl_renderer_show_toast("High-Refresh Decoupled Cursor: %s", g_cfg.decoupled_cursor ? "ON" : "OFF");
    gl_renderer_redraw_if_needed(1);
}

void gl_renderer_set_scene_widescreen(int is_widescreen) {
    g_scene_is_widescreen = is_widescreen ? 1 : 0;
}

void gl_renderer_trigger_transition(void) {
    if (g_cfg.scene_transitions && g_scene_has_rendered) {
        g_trans_pending = 1;
    }
}

/* ---- HD Background Activation (supports both 800x600 menus and 800x450 rooms!) ---- */
void gl_renderer_set_hd_background(int asset_id, const void *orig_pixels,
                                   int w, int h, int pitch, int bpp,
                                   int dst_x, int dst_y) {
    if (!g_gl_active || !pthread_equal(pthread_self(), g_gl_thread)) return;
    if (asset_id < 0 || !orig_pixels || w <= 0 || h <= 0) {
        g_bg_active = 0;
        g_bg_is_main_menu = 0;
        return;
    }

    int need_orig_upload = (g_bg_hd_id != asset_id) || (g_bg_orig_ptr != orig_pixels);
    if (g_bg_hd_id != asset_id) {
        const HdImageRGBA *hd = hd_assets_get_rgba(asset_id);
        if (!hd || !hd->rgba) {
            g_bg_active = 0;
            g_bg_is_main_menu = 0;
            return;
        }
        glBindTexture(GL_TEXTURE_2D, g_bg_hd_tex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, hd->width, hd->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, hd->rgba);
        p_glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_bg_hd_id = asset_id;
        g_bg_hd_w = hd->width;
        g_bg_hd_h = hd->height;

        /* Load optional 16:9 widescreen extended sibling (*_wide.webp) for 4:3 menus */
        g_bg_wide_has = 0;
        HdImageRGBA wimg;
        memset(&wimg, 0, sizeof(wimg));
        if (hd_assets_decode_wide_rgba(asset_id, &wimg) == 0 && wimg.rgba && wimg.width > 0 && wimg.height > 0) {
            /* Flip vertically once to match bottom-up FBO orientation used by FS_POST */
            size_t row_bytes = (size_t)wimg.width * 4u;
            uint8_t *tmp_row = (uint8_t *)malloc(row_bytes);
            if (tmp_row) {
                for (int y = 0; y < wimg.height / 2; y++) {
                    uint8_t *r0 = wimg.rgba + (size_t)y * row_bytes;
                    uint8_t *r1 = wimg.rgba + (size_t)(wimg.height - 1 - y) * row_bytes;
                    memcpy(tmp_row, r0, row_bytes);
                    memcpy(r0, r1, row_bytes);
                    memcpy(r1, tmp_row, row_bytes);
                }
                free(tmp_row);
            }
            glBindTexture(GL_TEXTURE_2D, g_bg_wide_tex);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, wimg.width, wimg.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, wimg.rgba);
            p_glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g_bg_wide_w = wimg.width;
            g_bg_wide_h = wimg.height;
            g_bg_wide_has = 1;
            hd_sprites_free_rgba(&wimg);
        }

        if (g_cfg.hd_assets_enabled) {
            gl_renderer_show_toast("HD Background: %s (%dx%d)", hd_assets_get_name(asset_id), g_bg_hd_w, g_bg_hd_h);
        }
    }

    if (need_orig_upload) {
        glBindTexture(GL_TEXTURE_2D, g_bg_orig_tex);
        int bps = bpp / 8;
        glPixelStorei(GL_UNPACK_ALIGNMENT, bps == 4 ? 4 : 2);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / bps);
        GLenum fmt  = (bpp == 16) ? GL_RGB : GL_BGRA;
        GLenum type = (bpp == 16) ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_INT_8_8_8_8_REV;
        GLint  ifmt = (bpp == 16) ? GL_RGB8 : GL_RGBA8;
        if (g_bg_orig_w != w || g_bg_orig_h != h || g_bg_orig_bpp != bpp) {
            glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, orig_pixels);
            g_bg_orig_w = w; g_bg_orig_h = h; g_bg_orig_bpp = bpp;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, type, orig_pixels);
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_bg_orig_ptr = orig_pixels;
    }
    const char *aname = hd_assets_get_name(asset_id);
    g_bg_is_main_menu = (aname && strncmp(aname, "x/001/", 6) == 0 && strstr(aname, "objBg") != NULL) ? 1 : 0;
    g_bg_dst_x = dst_x;
    g_bg_dst_y = dst_y;
    g_bg_active = 1;
}

void gl_renderer_clear_hd_background(void) {
    g_bg_active = 0;
    g_bg_is_main_menu = 0;
}

/* ---- Persistent 4K GPU DirectDraw Canvas Mirror (Exact Painter's Z-Order) ---- */
static void canvas_draw_tex_quad(int slot, int game_w, int game_h, GLuint tex,
                                 int dx0, int dy0, int dx1, int dy1,
                                 float u0, float v0, float u1, float v1,
                                 int blend_mode /* 0=opaque, 1=straight-alpha, 2=premul-alpha */,
                                 float alpha) {
    if (!g_gl_active || !tex || !pthread_equal(pthread_self(), g_gl_thread)) return;
    if (dx1 <= dx0 || dy1 <= dy0) return;
    if (!ensure_hd_canvas_fbos(game_w, game_h)) return;

    float x0 = (2.0f * (float)dx0 / (float)g_hd_canvas_gw) - 1.0f;
    float x1 = (2.0f * (float)dx1 / (float)g_hd_canvas_gw) - 1.0f;
    float y0 = (2.0f * (float)dy0 / (float)g_hd_canvas_gh) - 1.0f;
    float y1 = (2.0f * (float)dy1 / (float)g_hd_canvas_gh) - 1.0f;

    p_glBindFramebuffer(GL_FRAMEBUFFER, g_hd_canvas_fbo[slot & 1]);
    glViewport(0, 0, g_hd_canvas_tw, g_hd_canvas_th);
    glDisable(GL_SCISSOR_TEST);
    if (blend_mode == 1) {
        glEnable(GL_BLEND);
        p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    } else if (blend_mode == 2) {
        glEnable(GL_BLEND);
        p_glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    } else {
        glDisable(GL_BLEND);
    }

    p_glUseProgram(g_prog_osd);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    p_glUniform1i(p_glGetUniformLocation(g_prog_osd, "uTex"), 0);
    p_glUniform4f(p_glGetUniformLocation(g_prog_osd, "uRect"), x0, y0, x1, y1);
    p_glUniform4f(p_glGetUniformLocation(g_prog_osd, "uUVRect"), u0, v0, u1, v1);
    p_glUniform1f(p_glGetUniformLocation(g_prog_osd, "uAlpha"), alpha);
    p_glUniform1f(p_glGetUniformLocation(g_prog_osd, "uSoftEdge"), (blend_mode == 1) ? 1.0f : 0.0f);
    p_glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisable(GL_BLEND);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_hd_canvas_has_draws = 1;
}

void gl_canvas_colorfill(int slot, int game_w, int game_h,
                         int x0_i, int y0_i, int x1_i, int y1_i,
                         float r, float g, float b, float a) {
    if (!g_gl_active || !pthread_equal(pthread_self(), g_gl_thread)) return;
    if (x1_i <= x0_i || y1_i <= y0_i) return;
    if (!ensure_hd_canvas_fbos(game_w, game_h)) return;

    float x0 = (2.0f * (float)x0_i / (float)g_hd_canvas_gw) - 1.0f;
    float x1 = (2.0f * (float)x1_i / (float)g_hd_canvas_gw) - 1.0f;
    float y0 = (2.0f * (float)y0_i / (float)g_hd_canvas_gh) - 1.0f;
    float y1 = (2.0f * (float)y1_i / (float)g_hd_canvas_gh) - 1.0f;

    p_glBindFramebuffer(GL_FRAMEBUFFER, g_hd_canvas_fbo[slot & 1]);
    glViewport(0, 0, g_hd_canvas_tw, g_hd_canvas_th);
    glDisable(GL_SCISSOR_TEST);
    if (a < 0.999f) {
        glEnable(GL_BLEND);
        p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    } else {
        glDisable(GL_BLEND);
    }

    p_glUseProgram(g_prog_solid);
    p_glUniform4f(p_glGetUniformLocation(g_prog_solid, "uRect"), x0, y0, x1, y1);
    p_glUniform4f(p_glGetUniformLocation(g_prog_solid, "uUVRect"), 0.0f, 0.0f, 1.0f, 1.0f);
    p_glUniform4f(p_glGetUniformLocation(g_prog_solid, "uColor"), r, g, b, a);
    p_glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisable(GL_BLEND);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_hd_canvas_has_draws = 1;
}

static float badge_clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static float badge_smoothstep(float e0, float e1, float x) {
    float t = badge_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static void badge_sample_gold_env(float ev, float *out_r, float *out_g, float *out_b) {
    static const float pos[6] = { 0.00f, 0.22f, 0.48f, 0.65f, 0.86f, 1.00f };
    static const float col[6][3] = {
        { 1.00f, 0.97f, 0.84f },
        { 0.98f, 0.91f, 0.72f },
        { 0.88f, 0.76f, 0.52f },
        { 0.95f, 0.85f, 0.62f },
        { 0.84f, 0.70f, 0.45f },
        { 0.66f, 0.52f, 0.28f },
    };
    ev = badge_clampf(ev, 0.0f, 1.0f);
    for (int i = 0; i < 5; i++) {
        if (ev <= pos[i + 1]) {
            float t = (ev - pos[i]) / (pos[i + 1] - pos[i]);
            *out_r = col[i][0] + (col[i + 1][0] - col[i][0]) * t;
            *out_g = col[i][1] + (col[i + 1][1] - col[i][1]) * t;
            *out_b = col[i][2] + (col[i + 1][2] - col[i][2]) * t;
            return;
        }
    }
    *out_r = col[5][0];
    *out_g = col[5][1];
    *out_b = col[5][2];
}

/* Exact O(n) 1D squared Euclidean distance transform (Felzenszwalb-Huttenlocher) */
static void badge_edt_1d(const float *f, float *d, int n, int *v, double *z) {
    if (n <= 0) return;
    int k = 0;
    v[0] = 0;
    z[0] = -1e20;
    z[1] = 1e20;
    for (int q = 1; q < n; q++) {
        double fq = (double)f[q];
        double qq = (double)q * (double)q;
        double s;
        for (;;) {
            int vk = v[k];
            double fvk = (double)f[vk];
            double vkvk = (double)vk * (double)vk;
            s = ((fq + qq) - (fvk + vkvk)) / (2.0 * (double)(q - vk));
            if (s > z[k] || k == 0) break;
            k--;
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e20;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < (double)q) k++;
        double dx = (double)q - (double)v[k];
        d[q] = (float)(dx * dx + (double)f[v[k]]);
    }
}

static void badge_edt_2d(float *grid, int w, int h) {
    int max_dim = (w > h) ? w : h;
    float *src = (float *)malloc((size_t)max_dim * sizeof(float));
    float *dst = (float *)malloc((size_t)max_dim * sizeof(float));
    int   *v   = (int   *)malloc((size_t)max_dim * sizeof(int));
    double *z  = (double *)malloc((size_t)(max_dim + 1) * sizeof(double));
    if (!src || !dst || !v || !z) {
        free(src); free(dst); free(v); free(z);
        return;
    }

    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) src[y] = grid[y * w + x];
        badge_edt_1d(src, dst, h, v, z);
        for (int y = 0; y < h; y++) grid[y * w + x] = dst[y];
    }
    for (int y = 0; y < h; y++) {
        float *row = grid + y * w;
        memcpy(src, row, (size_t)w * sizeof(float));
        badge_edt_1d(src, dst, w, v, z);
        for (int x = 0; x < w; x++) row[x] = sqrtf(fmaxf(0.0f, dst[x]));
    }

    free(src);
    free(dst);
    free(v);
    free(z);
}

/* Generate crisp 4K-scale clean-room 3D brushed champagne-gold "REMASTERED" texture
 * (premultiplied RGBA8) using SDL2_ttf outlines and exact 2D signed distance fields. */
static uint8_t *build_remastered_badge_rgba(int *out_w, int *out_h,
                                            int *out_ink_w, int *out_ink_h, int *out_pad) {
    static const char k_text[] = "REMASTERED";
    if (!g_game_ttf_path[0]) init_osd_font();
    const char *fpath = g_game_ttf_path[0] ? g_game_ttf_path : g_ttf_font_path;
    if (!fpath[0]) return NULL;

    TTF_Font *font = TTF_OpenFont(fpath, 260);
    if (!font) return NULL;
    if (!g_game_ttf_is_bold_file) {
        TTF_SetFontStyle(font, TTF_STYLE_BOLD);
    }

#ifdef SDL_PIXELFORMAT_RGBA32
    const Uint32 fmt = SDL_PIXELFORMAT_RGBA32;
#else
    const Uint32 fmt = SDL_PIXELFORMAT_ABGR8888;
#endif
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *gsurf[10];
    int gx0[10], gx1[10];
    int gy_min = 0x7fffffff, gy_max = -1;
    memset(gsurf, 0, sizeof(gsurf));

    int ok = 1;
    for (int i = 0; i < 10; i++) {
        char buf[2] = { k_text[i], '\0' };
        SDL_Surface *raw = TTF_RenderUTF8_Blended(font, buf, white);
        if (!raw) { ok = 0; break; }
        SDL_Surface *conv = SDL_ConvertSurfaceFormat(raw, fmt, 0);
        SDL_FreeSurface(raw);
        if (!conv) { ok = 0; break; }
        gsurf[i] = conv;

        int lx0 = conv->w, lx1 = -1;
        const uint8_t *px = (const uint8_t *)conv->pixels;
        for (int y = 0; y < conv->h; y++) {
            const uint8_t *row = px + y * conv->pitch;
            for (int x = 0; x < conv->w; x++) {
                if (row[x * 4 + 3] >= 16) {
                    if (x < lx0) lx0 = x;
                    if (x > lx1) lx1 = x;
                    if (y < gy_min) gy_min = y;
                    if (y > gy_max) gy_max = y;
                }
            }
        }
        if (lx1 < lx0) { ok = 0; break; }
        gx0[i] = lx0;
        gx1[i] = lx1;
    }
    TTF_CloseFont(font);

    if (!ok || gy_max < gy_min) {
        for (int i = 0; i < 10; i++) {
            if (gsurf[i]) SDL_FreeSurface(gsurf[i]);
        }
        return NULL;
    }

    int ink_h = gy_max - gy_min + 1;
    int gaps[9];
    int ink_w = 0;
    for (int i = 0; i < 10; i++) {
        ink_w += (gx1[i] - gx0[i] + 1);
        if (i < 9) {
            float gf = 0.140f;
            if (i == 2 || i == 3) gf = 0.105f;       /* M->A, A->S optical tighten */
            else if (i == 4 || i == 5) gf = 0.110f;  /* S->T, T->E overhang tighten */
            gaps[i] = (int)roundf((float)ink_h * gf);
            ink_w += gaps[i];
        }
    }

    int pad = (int)roundf((float)ink_h * 0.34f);
    int tex_w = ink_w + 2 * pad;
    int tex_h = ink_h + 2 * pad;
    size_t npix = (size_t)tex_w * (size_t)tex_h;

    float *cov   = (float *)calloc(npix, sizeof(float));
    float *d_in  = (float *)malloc(npix * sizeof(float));
    float *d_out = (float *)malloc(npix * sizeof(float));
    float *hgt   = (float *)malloc(npix * sizeof(float));
    uint8_t *rgba = (uint8_t *)calloc(npix * 4u, 1u);

    if (!cov || !d_in || !d_out || !hgt || !rgba) {
        for (int i = 0; i < 10; i++) {
            if (gsurf[i]) SDL_FreeSurface(gsurf[i]);
        }
        free(cov); free(d_in); free(d_out); free(hgt); free(rgba);
        return NULL;
    }

    int pen_x = pad;
    for (int i = 0; i < 10; i++) {
        SDL_Surface *s = gsurf[i];
        const uint8_t *px = (const uint8_t *)s->pixels;
        int sy0 = gy_min - 1; if (sy0 < 0) sy0 = 0;
        int sy1 = gy_max + 1; if (sy1 >= s->h) sy1 = s->h - 1;
        int sx0 = gx0[i] - 1; if (sx0 < 0) sx0 = 0;
        int sx1 = gx1[i] + 1; if (sx1 >= s->w) sx1 = s->w - 1;
        for (int sy = sy0; sy <= sy1; sy++) {
            int dy = pad + (sy - gy_min);
            if (dy < 0 || dy >= tex_h) continue;
            const uint8_t *row = px + sy * s->pitch;
            for (int sx = sx0; sx <= sx1; sx++) {
                int dx = pen_x + (sx - gx0[i]);
                if (dx < 0 || dx >= tex_w) continue;
                float a = (float)row[sx * 4 + 3] / 255.0f;
                size_t di = (size_t)dy * (size_t)tex_w + (size_t)dx;
                if (a > cov[di]) cov[di] = a;
            }
        }
        pen_x += (gx1[i] - gx0[i] + 1);
        if (i < 9) pen_x += gaps[i];
        SDL_FreeSurface(gsurf[i]);
        gsurf[i] = NULL;
    }

    for (size_t i = 0; i < npix; i++) {
        d_in[i]  = (cov[i] < 0.5f)  ? 0.0f : 1e9f;
        d_out[i] = (cov[i] >= 0.5f) ? 0.0f : 1e9f;
    }
    badge_edt_2d(d_in, tex_w, tex_h);
    badge_edt_2d(d_out, tex_w, tex_h);

    float bw = 0.072f * (float)ink_h;
    if (bw < 1.0f) bw = 1.0f;
    for (size_t i = 0; i < npix; i++) {
        if (cov[i] >= 0.5f) {
            d_in[i] = fmaxf(0.0f, d_in[i] - 0.5f + (cov[i] - 0.5f));
            d_out[i] = 0.0f;
        } else {
            d_out[i] = fmaxf(0.0f, d_out[i] - 0.5f + (0.5f - cov[i]));
            d_in[i] = 0.0f;
        }
        float u = badge_clampf(d_in[i] / bw, 0.0f, 1.0f);
        hgt[i] = 1.0f - (1.0f - u) * (1.0f - u);
    }

    /* Key upper-left light + lower-right warm treasure bounce (pre-normalized) */
    const float l1x = -0.32f, l1y = -0.82f, l1z = 0.48f;
    const float l1_len = sqrtf(l1x * l1x + l1y * l1y + l1z * l1z);
    const float kx = l1x / l1_len, ky = l1y / l1_len, kz = l1z / l1_len;
    float hx = kx, hy = ky, hz = kz + 1.0f;
    float h_len = sqrtf(hx * hx + hy * hy + hz * hz);
    hx /= h_len; hy /= h_len; hz /= h_len;

    const float l2x = 0.25f, l2y = 0.78f, l2z = 0.57f;
    const float l2_len = sqrtf(l2x * l2x + l2y * l2y + l2z * l2z);
    const float bx = l2x / l2_len, by = l2y / l2_len, bz = l2z / l2_len;

    const float sigma_g = 0.088f * (float)ink_h;
    const float sigma_s = 0.070f * (float)ink_h;
    const float inv_2sg2 = 1.0f / (2.0f * sigma_g * sigma_g);
    const float inv_2ss2 = 1.0f / (2.0f * sigma_s * sigma_s);
    const int sh_dx = (int)roundf(0.020f * (float)ink_h);
    const int sh_dy = (int)roundf(0.048f * (float)ink_h);
    const float border_w = fmaxf(1.8f, 0.027f * (float)ink_h);

    for (int y = 0; y < tex_h; y++) {
        int ym = (y > 0) ? (y - 1) : 0;
        int yp = (y + 1 < tex_h) ? (y + 1) : (tex_h - 1);
        float ty = badge_clampf((float)(y - pad) / (float)ink_h, 0.0f, 1.0f);
        /* Fine horizontal brushed-metal texture */
        float brush = 0.011f * sinf((float)y * 1.45f) + 0.005f * sinf((float)y * 3.7f);

        for (int x = 0; x < tex_w; x++) {
            size_t idx = (size_t)y * (size_t)tex_w + (size_t)x;
            float dout = d_out[idx];

            /* Layer 1: Soft warm golden ambient aura (smoothly attenuated before padding edge) */
            float ga = 0.18f * expf(-(dout * dout) * inv_2sg2);
            float pr = 1.00f * ga;
            float pg = 0.78f * ga;
            float pb = 0.34f * ga;
            float pa = ga;

            /* Layer 2: Soft offset dark drop shadow */
            int sx = x - sh_dx; if (sx < 0) sx = 0; else if (sx >= tex_w) sx = tex_w - 1;
            int sy = y - sh_dy; if (sy < 0) sy = 0; else if (sy >= tex_h) sy = tex_h - 1;
            float dsh = d_out[(size_t)sy * (size_t)tex_w + (size_t)sx];
            float sa = 0.84f * expf(-(dsh * dsh) * inv_2ss2);
            if (sa > 0.001f) {
                float isa = 1.0f - sa;
                pr = 0.03f * sa + pr * isa;
                pg = 0.015f * sa + pg * isa;
                pb = 0.004f * sa + pb * isa;
                pa = sa + pa * isa;
            }

            /* Layer 3: Crisp dark carved bronze outline */
            float oa = 1.0f - badge_smoothstep(border_w - 0.8f, border_w + 1.0f, dout);
            if (oa > 0.001f) {
                float ioa = 1.0f - oa;
                pr = 0.06f * oa + pr * ioa;
                pg = 0.035f * oa + pg * ioa;
                pb = 0.01f * oa + pb * ioa;
                pa = oa + pa * ioa;
            }

            /* Layer 4: 3D chiseled brushed champagne-gold interior */
            float ca = cov[idx];
            if (ca > 0.001f) {
                int xm = (x > 0) ? (x - 1) : 0;
                int xp = (x + 1 < tex_w) ? (x + 1) : (tex_w - 1);
                float gx = 0.5f * (hgt[(size_t)y * (size_t)tex_w + (size_t)xp] -
                                   hgt[(size_t)y * (size_t)tex_w + (size_t)xm]);
                float gy = 0.5f * (hgt[(size_t)yp * (size_t)tex_w + (size_t)x] -
                                   hgt[(size_t)ym * (size_t)tex_w + (size_t)x]);
                float nx = -gx * 2.6f, ny = -gy * 2.6f, nz = 1.0f;
                float nlen = sqrtf(nx * nx + ny * ny + nz * nz);
                nx /= nlen; ny /= nlen; nz /= nlen;

                float tx = badge_clampf((float)(x - pad) / (float)ink_w, 0.0f, 1.0f);
                float ev = badge_clampf(ty + ny * 0.34f + nx * 0.08f, 0.0f, 1.0f);
                float mr = 0.0f, mg = 0.0f, mb = 0.0f;
                badge_sample_gold_env(ev, &mr, &mg, &mb);

                float diag = 0.97f + 0.06f * cosf((tx * 0.85f + ty * 0.35f - 0.55f) * 3.14159f);
                float mod = diag + brush;
                mr *= mod; mg *= mod; mb *= mod;

                float diff1 = fmaxf(0.0f, nx * kx + ny * ky + nz * kz);
                float spec1 = powf(fmaxf(0.0f, nx * hx + ny * hy + nz * hz), 24.0f);
                float diff2 = fmaxf(0.0f, nx * bx + ny * by + nz * bz);

                float rim_mask = 1.0f - nz * nz; /* emphasizes beveled chamfers vs flat face */
                float lift = 0.92f + 0.16f * diff1;
                mr = mr * lift + 0.50f * spec1 * 1.00f + 0.16f * diff2 * rim_mask * 0.98f;
                mg = mg * lift + 0.50f * spec1 * 0.97f + 0.16f * diff2 * rim_mask * 0.80f;
                mb = mb * lift + 0.50f * spec1 * 0.88f + 0.16f * diff2 * rim_mask * 0.42f;

                /* Subtle bevel crest catchlight near outer edge */
                float crease = 1.0f - badge_smoothstep(0.0f, 1.8f, d_in[idx]);
                mr = mr * (1.0f - 0.20f * crease) + 0.20f * crease * 0.35f;
                mg = mg * (1.0f - 0.20f * crease) + 0.20f * crease * 0.26f;
                mb = mb * (1.0f - 0.20f * crease) + 0.20f * crease * 0.12f;

                mr = badge_clampf(mr, 0.0f, 1.0f);
                mg = badge_clampf(mg, 0.0f, 1.0f);
                mb = badge_clampf(mb, 0.0f, 1.0f);

                float ica = 1.0f - ca;
                pr = mr * ca + pr * ica;
                pg = mg * ca + pg * ica;
                pb = mb * ca + pb * ica;
                pa = ca + pa * ica;
            }

            /* Wide smooth border fade over half of padding so zero rectangular halo edge is visible */
            int ex = x < (tex_w - 1 - x) ? x : (tex_w - 1 - x);
            int ey = y < (tex_h - 1 - y) ? y : (tex_h - 1 - y);
            int em = ex < ey ? ex : ey;
            float fade_end = fmaxf(8.0f, (float)pad * 0.45f);
            if ((float)em < fade_end) {
                float ef = badge_smoothstep(2.0f, fade_end, (float)em);
                pr *= ef; pg *= ef; pb *= ef; pa *= ef;
            }

            int r8 = (int)roundf(badge_clampf(pr, 0.0f, 1.0f) * 255.0f);
            int g8 = (int)roundf(badge_clampf(pg, 0.0f, 1.0f) * 255.0f);
            int b8 = (int)roundf(badge_clampf(pb, 0.0f, 1.0f) * 255.0f);
            int a8 = (int)roundf(badge_clampf(pa, 0.0f, 1.0f) * 255.0f);
            rgba[idx * 4u + 0u] = (uint8_t)r8;
            rgba[idx * 4u + 1u] = (uint8_t)g8;
            rgba[idx * 4u + 2u] = (uint8_t)b8;
            rgba[idx * 4u + 3u] = (uint8_t)a8;
        }
    }

    free(cov);
    free(d_in);
    free(d_out);
    free(hgt);

    *out_w = tex_w;
    *out_h = tex_h;
    *out_ink_w = ink_w;
    *out_ink_h = ink_h;
    *out_pad = pad;
    return rgba;
}

static void ensure_remastered_badge_tex(void) {
    if (g_remastered_badge_tex || g_remastered_badge_tried) return;
    g_remastered_badge_tried = 1;

    int tw = 0, th = 0, iw = 0, ih = 0, pad = 0;
    uint8_t *rgba = build_remastered_badge_rgba(&tw, &th, &iw, &ih, &pad);
    if (!rgba) return;

    glGenTextures(1, &g_remastered_badge_tex);
    glBindTexture(GL_TEXTURE_2D, g_remastered_badge_tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    p_glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(rgba);

    g_remastered_badge_ink_w = iw;
    g_remastered_badge_ink_h = ih;
    g_remastered_badge_pad = pad;
}

static void get_remastered_badge_game_rect(float *qx0, float *qy0, float *qx1, float *qy1) {
    float target_ink_h = 52.0f;
    float target_ink_w = target_ink_h * ((float)g_remastered_badge_ink_w / (float)g_remastered_badge_ink_h);
    if (target_ink_w > 504.0f) {
        float sc = 504.0f / target_ink_w;
        target_ink_w = 504.0f;
        target_ink_h *= sc;
    }
    float pad_g = target_ink_h * ((float)g_remastered_badge_pad / (float)g_remastered_badge_ink_h);
    const float cx = 400.0f;
    const float cy = 472.5f;
    *qx0 = cx - target_ink_w * 0.5f - pad_g;
    *qx1 = cx + target_ink_w * 0.5f + pad_g;
    *qy0 = cy - target_ink_h * 0.5f - pad_g;
    *qy1 = cy + target_ink_h * 0.5f + pad_g;
}

void gl_canvas_blit_hd_bg(int slot, int game_w, int game_h,
                          int dx0, int dy0, int dx1, int dy1,
                          float u0, float v0, float u1, float v1) {
    if (!g_bg_active || !g_bg_hd_tex) return;
    canvas_draw_tex_quad(slot, game_w, game_h, g_bg_hd_tex,
                         dx0, dy0, dx1, dy1, u0, v0, u1, v1, 0, 1.0f);

    if (g_bg_is_main_menu && game_w == 800 && game_h == 600 &&
        g_gl_active && pthread_equal(pthread_self(), g_gl_thread) &&
        ensure_hd_canvas_fbos(game_w, game_h)) {
        ensure_remastered_badge_tex();
        if (g_remastered_badge_tex) {
            float qx0, qy0, qx1, qy1;
            get_remastered_badge_game_rect(&qx0, &qy0, &qx1, &qy1);
            if ((float)dx1 > qx0 && (float)dx0 < qx1 && (float)dy1 > qy0 && (float)dy0 < qy1) {
                int scx0 = dx0 < 0 ? 0 : dx0;
                int scy0 = dy0 < 0 ? 0 : dy0;
                int scx1 = dx1 > g_hd_canvas_gw ? g_hd_canvas_gw : dx1;
                int scy1 = dy1 > g_hd_canvas_gh ? g_hd_canvas_gh : dy1;
                if (scx1 > scx0 && scy1 > scy0) {
                    float x0 = (2.0f * qx0 / (float)g_hd_canvas_gw) - 1.0f;
                    float x1 = (2.0f * qx1 / (float)g_hd_canvas_gw) - 1.0f;
                    float y0 = (2.0f * qy0 / (float)g_hd_canvas_gh) - 1.0f;
                    float y1 = (2.0f * qy1 / (float)g_hd_canvas_gh) - 1.0f;

                    p_glBindFramebuffer(GL_FRAMEBUFFER, g_hd_canvas_fbo[slot & 1]);
                    glViewport(0, 0, g_hd_canvas_tw, g_hd_canvas_th);
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(scx0 * HD_CANVAS_SCALE, scy0 * HD_CANVAS_SCALE,
                              (scx1 - scx0) * HD_CANVAS_SCALE, (scy1 - scy0) * HD_CANVAS_SCALE);
                    glEnable(GL_BLEND);
                    p_glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

                    p_glUseProgram(g_prog_osd);
                    p_glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, g_remastered_badge_tex);
                    p_glUniform1i(p_glGetUniformLocation(g_prog_osd, "uTex"), 0);
                    p_glUniform4f(p_glGetUniformLocation(g_prog_osd, "uRect"), x0, y0, x1, y1);
                    p_glUniform4f(p_glGetUniformLocation(g_prog_osd, "uUVRect"), 0.0f, 0.0f, 1.0f, 1.0f);
                    p_glUniform1f(p_glGetUniformLocation(g_prog_osd, "uAlpha"), 1.0f);
                    p_glUniform1f(p_glGetUniformLocation(g_prog_osd, "uSoftEdge"), 0.0f);
                    p_glBindVertexArray(g_vao);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                    glDisable(GL_BLEND);
                    glDisable(GL_SCISSOR_TEST);
                    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
                }
            }
        }
    }
}

void gl_canvas_blit_hd_sprite(int slot, int game_w, int game_h, int sprite_id,
                              int dx0, int dy0, int dx1, int dy1,
                              float u0, float v0, float u1, float v1,
                              int blend_alpha, float alpha) {
    if (!g_gl_active || !pthread_equal(pthread_self(), g_gl_thread)) return;
    SpriteGlEntry *se = get_or_upload_sprite_tex(sprite_id);
    if (!se || !se->tex) return;
    int bm = (blend_alpha || alpha < 0.999f) ? 1 : 0;
    canvas_draw_tex_quad(slot, game_w, game_h, se->tex,
                         dx0, dy0, dx1, dy1, u0, v0, u1, v1, bm, alpha);
}

void gl_canvas_blit_rgba_patch(int slot, int game_w, int game_h,
                               const uint32_t *rgba_premul, int pw, int ph,
                               int dx0, int dy0, int dx1, int dy1,
                               int blend_alpha) {
    if (!g_gl_active || !rgba_premul || pw <= 0 || ph <= 0 || !pthread_equal(pthread_self(), g_gl_thread)) return;
    if (!ensure_hd_canvas_fbos(game_w, game_h)) return;

    glBindTexture(GL_TEXTURE_2D, g_patch_tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (g_patch_tex_w != pw || g_patch_tex_h != ph) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_premul);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_patch_tex_w = pw;
        g_patch_tex_h = ph;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, rgba_premul);
    }

    canvas_draw_tex_quad(slot, game_w, game_h, g_patch_tex,
                         dx0, dy0, dx1, dy1, 0.0f, 0.0f, 1.0f, 1.0f,
                         blend_alpha ? 2 : 0, 1.0f);
}

void gl_canvas_blit_canvas(int dst_slot, int src_slot, int game_w, int game_h,
                           int dx0, int dy0, int dx1, int dy1,
                           int sx0, int sy0, int sx1, int sy1) {
    if (!g_gl_active || !g_hd_canvas_ready || !pthread_equal(pthread_self(), g_gl_thread)) return;
    if ((dst_slot & 1) == (src_slot & 1)) return;
    float u0 = (float)sx0 / (float)g_hd_canvas_gw;
    float v0 = (float)sy0 / (float)g_hd_canvas_gh;
    float u1 = (float)sx1 / (float)g_hd_canvas_gw;
    float v1 = (float)sy1 / (float)g_hd_canvas_gh;
    canvas_draw_tex_quad(dst_slot, game_w, game_h, g_hd_canvas_tex[src_slot & 1],
                         dx0, dy0, dx1, dy1, u0, v0, u1, v1, 0, 1.0f);
}

void gl_canvas_draw_vector_text(int slot, int game_w, int game_h,
                                const char *utf8,
                                int text_x0, int text_y0, int text_w, int text_h,
                                int clip_x0, int clip_y0, int clip_x1, int clip_y1,
                                uint32_t fill_rgb, uint32_t outline_rgb) {
    if (!g_gl_active || !utf8 || !utf8[0] || !pthread_equal(pthread_self(), g_gl_thread)) return;
    if (!ensure_hd_canvas_fbos(game_w, game_h)) return;
    if (text_w <= 0 || text_h <= 0) return;

    /* Render native 4K vector typography using exact unclipped (text_x0, text_y0, text_w, text_h)
       from Font::MeasureText so dirty-rect partial redraws NEVER shift, squish, or morph text,
       and every rendered pixel stays strictly inside the game's dirty-rect erase bounds! */
    int px_size = (text_h >= 24) ? ((text_h * HD_CANVAS_SCALE * 82) / 100)
                                 : ((text_h * HD_CANVAS_SCALE * 78) / 100);
    if (px_size < 32) px_size = 32;
    if (px_size > 160) px_size = 160;
    int outline_px = (px_size >= 80) ? 5 : 3;

    CanvasTextEntry *te = get_or_render_outlined_text_tex(utf8, px_size, outline_px, fill_rgb, outline_rgb);
    if (!te || !te->tex || te->tex_w <= 0 || te->tex_h <= 0) return;

    float gw = (float)te->tex_w / (float)HD_CANVAS_SCALE;
    float gh = (float)te->tex_h / (float)HD_CANVAS_SCALE;

    /* Strictly contain inside [text_w x text_h] so mha2.exe's dirty-rect eraser removes 100% of pixels */
    if (gh > (float)text_h) {
        float sc = (float)text_h / gh;
        gw *= sc;
        gh = (float)text_h;
    }
    if (gw > (float)text_w) {
        float sc = (float)text_w / gw;
        gw = (float)text_w;
        if (sc < 0.85f) {
            gh *= (sc / 0.85f);
        }
    }

    /* Left-align bottom dialog choices (y >= 495) to match the original game's column alignment;
       center top subtitles, scores, and hover labels inside their measured span. */
    float dx0 = (text_y0 >= 495)
        ? (float)text_x0
        : ((float)text_x0 + ((float)text_w - gw) * 0.5f);
    float dx1 = dx0 + gw;
    float dy0 = (float)text_y0 + ((float)text_h - gh) * 0.5f;
    float dy1 = dy0 + gh;

    /* Intersect scissor with BOTH caller's clip_rect AND exact text bounding box */
    int sc_x0 = clip_x0 > text_x0 ? clip_x0 : text_x0;
    int sc_y0 = clip_y0 > text_y0 ? clip_y0 : text_y0;
    int sc_x1 = clip_x1 < (text_x0 + text_w) ? clip_x1 : (text_x0 + text_w);
    int sc_y1 = clip_y1 < (text_y0 + text_h) ? clip_y1 : (text_y0 + text_h);
    if (sc_x0 < 0) sc_x0 = 0;
    if (sc_y0 < 0) sc_y0 = 0;
    if (sc_x1 > g_hd_canvas_gw) sc_x1 = g_hd_canvas_gw;
    if (sc_y1 > g_hd_canvas_gh) sc_y1 = g_hd_canvas_gh;
    if (sc_x1 <= sc_x0 || sc_y1 <= sc_y0) return;

    float x0 = (2.0f * dx0 / (float)g_hd_canvas_gw) - 1.0f;
    float x1 = (2.0f * dx1 / (float)g_hd_canvas_gw) - 1.0f;
    float y0 = (2.0f * dy0 / (float)g_hd_canvas_gh) - 1.0f;
    float y1 = (2.0f * dy1 / (float)g_hd_canvas_gh) - 1.0f;

    p_glBindFramebuffer(GL_FRAMEBUFFER, g_hd_canvas_fbo[slot & 1]);
    glViewport(0, 0, g_hd_canvas_tw, g_hd_canvas_th);
    glEnable(GL_SCISSOR_TEST);
    glScissor(sc_x0 * HD_CANVAS_SCALE, sc_y0 * HD_CANVAS_SCALE,
              (sc_x1 - sc_x0) * HD_CANVAS_SCALE, (sc_y1 - sc_y0) * HD_CANVAS_SCALE);
    glEnable(GL_BLEND);
    p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    p_glUseProgram(g_prog_osd);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, te->tex);
    p_glUniform1i(p_glGetUniformLocation(g_prog_osd, "uTex"), 0);
    p_glUniform4f(p_glGetUniformLocation(g_prog_osd, "uRect"), x0, y0, x1, y1);
    p_glUniform4f(p_glGetUniformLocation(g_prog_osd, "uUVRect"), 0.0f, 0.0f, 1.0f, 1.0f);
    p_glUniform1f(p_glGetUniformLocation(g_prog_osd, "uAlpha"), 1.0f);
    p_glUniform1f(p_glGetUniformLocation(g_prog_osd, "uSoftEdge"), 0.0f);
    p_glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_hd_canvas_has_draws = 1;
}

void gl_canvas_flip(void) {
    GLuint tf = g_hd_canvas_fbo[0]; g_hd_canvas_fbo[0] = g_hd_canvas_fbo[1]; g_hd_canvas_fbo[1] = tf;
    GLuint tt = g_hd_canvas_tex[0]; g_hd_canvas_tex[0] = g_hd_canvas_tex[1]; g_hd_canvas_tex[1] = tt;
}

void gl_canvas_set_present_slot(int slot) {
    g_hd_canvas_present_slot = slot & 1;
}

/* ---- Native Vector Text Queue ---- */
void gl_renderer_set_frame_texts(const GlTextDraw *draws, int count) {
    if (!draws || count <= 0) {
        g_frame_text_count = 0;
        return;
    }
    if (count > MAX_FRAME_TEXT_DRAWS) count = MAX_FRAME_TEXT_DRAWS;
    memcpy(g_frame_texts, draws, (size_t)count * sizeof(GlTextDraw));
    g_frame_text_count = count;
}

/* ---- Decoupled Hardware-Rate Cursor (1x or 4x HD RGBA) ---- */
void gl_renderer_set_cursor(const uint32_t *rgba, int tex_w, int tex_h,
                            int game_w, int game_h,
                            int hot_x, int hot_y, uint64_t hash) {
    if (!g_gl_active || !pthread_equal(pthread_self(), g_gl_thread)) return;
    if (!rgba || tex_w <= 0 || tex_h <= 0 || game_w <= 0 || game_h <= 0) return;
    g_cursor_w = game_w;
    g_cursor_h = game_h;
    g_cursor_hot_x = hot_x;
    g_cursor_hot_y = hot_y;
    g_cursor_ready = 1;
    if (g_cursor_hash == hash && g_cursor_tex_w == tex_w && g_cursor_tex_h == tex_h) return;

    glBindTexture(GL_TEXTURE_2D, g_cursor_tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (tex_w > game_w) {
        p_glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_cursor_tex_w = tex_w;
    g_cursor_tex_h = tex_h;
    g_cursor_hash = hash;
}

void gl_renderer_set_live_mouse(float gx, float gy, int visible) {
    if (gx != g_live_mouse_x || gy != g_live_mouse_y || visible != g_live_mouse_visible) {
        g_live_mouse_x = gx;
        g_live_mouse_y = gy;
        g_live_mouse_visible = visible;
        g_mouse_moved_since_draw = 1;
    }
}

void gl_renderer_clear_cursor(void) {
    g_cursor_ready = 0;
}

static int resolve_active_layout(int draw_w, int draw_h, int src_bpp) {
    if (!g_scene_is_widescreen || src_bpp == 24 || g_game_w != 800 || g_game_h != 600) {
        return LAYOUT_4_3;
    }
    if (g_cfg.aspect_mode == ASPECT_4_3)   return LAYOUT_4_3;
    if (g_cfg.aspect_mode == ASPECT_16_10) return LAYOUT_16_10;
    if (g_cfg.aspect_mode == ASPECT_16_9)  return LAYOUT_16_9;
    /* ASPECT_AUTO: pick exact match for monitor/window aspect ratio */
    float cur_aspect = (float)draw_w / (float)(draw_h > 0 ? draw_h : 1);
    if (cur_aspect >= 1.69f) return LAYOUT_16_9;
    if (cur_aspect >= 1.48f) return LAYOUT_16_10;
    return LAYOUT_4_3;
}

static void layout_display_size(int layout, float *out_w, float *out_h) {
    *out_w = (float)(g_game_w > 0 ? g_game_w : 800);
    if (layout == LAYOUT_16_10)      *out_h = 500.0f;
    else if (layout == LAYOUT_16_9)  *out_h = 450.0f;
    else                             *out_h = (float)(g_game_h > 0 ? g_game_h : 600);
}

static float game_to_display_y(float gy, int layout) {
    if (layout == LAYOUT_4_3) return gy;
    if (gy < 75.0f) return gy; /* top subtitle / minigame HUD overlay */
    if (g_bot_bar_dialog_mode && gy >= 500.0f) {
        /* Unified 1:1 100px dialog choice band (y=500..599 -> bottom 100px, zero dead zone!) */
        float dlg_top = (layout == LAYOUT_16_10) ? 400.0f : 350.0f;
        return dlg_top + (gy - 500.0f);
    }
    if (gy < 525.0f) return gy - 75.0f; /* 800x450 widescreen room */
    float dy = gy - 537.0f;
    if (dy < 0.0f) dy = 0.0f;
    if (dy > 49.0f) dy = 49.0f;
    return (layout == LAYOUT_16_10) ? (450.0f + dy) : (400.0f + dy);
}

static void update_hud_bar_masks(const void *pixels, int w, int h, int pitch, int bpp) {
    if (!pixels || w != 800 || h != 600 || (bpp != 16 && bpp != 32)) {
        memset(g_top_bar_mask, 0, sizeof(g_top_bar_mask));
        memset(g_bot_bar_mask, 0, sizeof(g_bot_bar_mask));
        g_top_bar_has_buttons = 0;
        g_bot_bar_dialog_mode = 0;
        return;
    }
    const uint8_t *base = (const uint8_t *)pixels;
    int top_corner_lit = 0;
    for (int y = 0; y < 75; y++) {
        const uint8_t *row = base + y * pitch;
        uint8_t *dst = g_top_bar_mask + y * 800;
        if (bpp == 32) {
            const uint32_t *p = (const uint32_t *)row;
            for (int x = 0; x < 800; x++) {
                uint32_t px = p[x] & 0xFFFFFFu;
                uint32_t r = (px >> 16) & 0xFFu, g = (px >> 8) & 0xFFu, b = px & 0xFFu;
                uint8_t lit = (r > 18 || g > 18 || b > 18) ? 1u : 0u;
                dst[x] = lit;
                if (lit && y < 70 && (x < 120 || x > 680)) top_corner_lit++;
            }
        } else {
            const uint16_t *p = (const uint16_t *)row;
            for (int x = 0; x < 800; x++) {
                uint8_t lit = (p[x] != 0) ? 1u : 0u;
                dst[x] = lit;
                if (lit && y < 70 && (x < 120 || x > 680)) top_corner_lit++;
            }
        }
    }
    g_top_bar_has_buttons = (top_corner_lit >= 60) ? 1 : 0;
    int black_500_524 = 0;
    int lit_500_599 = 0;
    for (int y = 500; y < 525; y++) {
        const uint8_t *row = base + y * pitch;
        if (bpp == 32) {
            const uint32_t *p = (const uint32_t *)row;
            for (int x = 0; x < 800; x++) {
                uint32_t px = p[x] & 0xFFFFFFu;
                uint32_t r = (px >> 16) & 0xFFu, g = (px >> 8) & 0xFFu, b = px & 0xFFu;
                if (r <= 10 && g <= 10 && b <= 10) black_500_524++;
                if (r > 18 || g > 18 || b > 18) lit_500_599++;
            }
        } else {
            const uint16_t *p = (const uint16_t *)row;
            for (int x = 0; x < 800; x++) {
                if (p[x] == 0) black_500_524++;
                else lit_500_599++;
            }
        }
    }
    for (int y = 525; y < 600; y++) {
        const uint8_t *row = base + y * pitch;
        uint8_t *dst = (y >= 537 && y < 587) ? (g_bot_bar_mask + (y - 537) * 800) : NULL;
        if (bpp == 32) {
            const uint32_t *p = (const uint32_t *)row;
            for (int x = 0; x < 800; x++) {
                uint32_t px = p[x] & 0xFFFFFFu;
                uint32_t r = (px >> 16) & 0xFFu, g = (px >> 8) & 0xFFu, b = px & 0xFFu;
                uint8_t lit = (r > 18 || g > 18 || b > 18) ? 1u : 0u;
                if (dst) dst[x] = lit;
                lit_500_599 += lit;
            }
        } else {
            const uint16_t *p = (const uint16_t *)row;
            for (int x = 0; x < 800; x++) {
                uint8_t lit = (p[x] != 0) ? 1u : 0u;
                if (dst) dst[x] = lit;
                lit_500_599 += lit;
            }
        }
    }
    /* Solid black dialog box across y=500..524 (>82.5% pure black) with visible dialog text in y=500..599 */
    g_bot_bar_dialog_mode = (black_500_524 >= 16500 && lit_500_599 >= 60) ? 1 : 0;
}

static int mask_hit_near(const uint8_t *mask, int w, int h, int cx, int cy, int rad) {
    int y0 = cy - rad; if (y0 < 0) y0 = 0;
    int y1 = cy + rad; if (y1 >= h) y1 = h - 1;
    int x0 = cx - rad; if (x0 < 0) x0 = 0;
    int x1 = cx + rad; if (x1 >= w) x1 = w - 1;
    for (int y = y0; y <= y1; y++) {
        const uint8_t *row = mask + y * w;
        for (int x = x0; x <= x1; x++) {
            if (row[x]) return 1;
        }
    }
    return 0;
}

static void compute_viewport(int draw_w, int draw_h, int layout, int *vx, int *vy, int *vw, int *vh) {
    float disp_w, disp_h;
    layout_display_size(layout, &disp_w, &disp_h);
    float target_aspect = disp_w / (disp_h > 0.0f ? disp_h : 600.0f);
    float cur_aspect = (float)draw_w / (float)(draw_h > 0 ? draw_h : 1);
    int w, h;
    if (cur_aspect > target_aspect) {
        h = draw_h;
        w = (int)((float)draw_h * target_aspect + 0.5f);
    } else {
        w = draw_w;
        h = (int)((float)draw_w / target_aspect + 0.5f);
    }
    *vw = w; *vh = h;
    *vx = (draw_w - w) / 2;
    *vy = (draw_h - h) / 2;

    int win_w = draw_w, win_h = draw_h;
    if (g_sdl_window) SDL_GetWindowSize(g_sdl_window, &win_w, &win_h);
    float sx = (draw_w > 0) ? ((float)win_w / (float)draw_w) : 1.0f;
    float sy = (draw_h > 0) ? ((float)win_h / (float)draw_h) : 1.0f;
    g_vp_win_x = (float)(*vx) * sx;
    g_vp_win_y = (float)(*vy) * sy;
    g_vp_win_w = (float)(*vw) * sx;
    g_vp_win_h = (float)(*vh) * sy;
    g_cur_layout = layout;
}

void gl_window_to_game_coords_f(int win_x, int win_y, float *out_gx, float *out_gy) {
    if (!g_gl_active || g_vp_win_w <= 1.0f || g_vp_win_h <= 1.0f) {
        *out_gx = (float)win_x;
        *out_gy = (float)win_y;
        return;
    }
    float nx = ((float)win_x - g_vp_win_x) / g_vp_win_w;
    float ny = ((float)win_y - g_vp_win_y) / g_vp_win_h;
    if (nx < 0.0f) nx = 0.0f; else if (nx > 1.0f) nx = 1.0f;
    if (ny < 0.0f) ny = 0.0f; else if (ny > 1.0f) ny = 1.0f;

    float disp_w, disp_h;
    layout_display_size(g_cur_layout, &disp_w, &disp_h);
    float dx = nx * (disp_w - 1.0f);
    float dy = ny * (disp_h - 1.0f);
    *out_gx = dx;

    if (g_cur_layout == LAYOUT_4_3) {
        *out_gy = dy;
    } else if (g_bot_bar_dialog_mode && dy >= ((g_cur_layout == LAYOUT_16_10) ? 400.0f : 350.0f)) {
        float dlg_top = (g_cur_layout == LAYOUT_16_10) ? 400.0f : 350.0f;
        *out_gy = 500.0f + (dy - dlg_top);
    } else if (g_cur_layout == LAYOUT_16_10) {
        if (dy >= 450.0f) {
            /* Dedicated bottom 50px inventory dock -> game y = 537..586 */
            *out_gy = 537.0f + (dy - 450.0f);
        } else if (g_top_bar_has_buttons && dy < 70.0f && (dx < 120.0f || dx > 680.0f) &&
                   mask_hit_near(g_top_bar_mask, 800, 75, (int)dx, (int)dy, 8)) {
            /* Minigame top-corner Exit/Help button overlay */
            *out_gy = dy;
        } else {
            *out_gy = 75.0f + dy;
        }
    } else { /* LAYOUT_16_9 */
        if (dy >= 400.0f && mask_hit_near(g_bot_bar_mask, 800, 50, (int)dx, (int)(dy - 400.0f), 10)) {
            *out_gy = 537.0f + (dy - 400.0f);
        } else if (g_top_bar_has_buttons && dy < 70.0f && (dx < 120.0f || dx > 680.0f) &&
                   mask_hit_near(g_top_bar_mask, 800, 75, (int)dx, (int)dy, 8)) {
            *out_gy = dy;
        } else {
            *out_gy = 75.0f + dy;
        }
    }
}

void gl_renderer_clamp_gamepad_cursor(float *nx, float *ny) {
    float max_x = (g_game_w > 0) ? (float)(g_game_w - 1) : 799.0f;
    float max_y = (g_game_h > 0) ? (float)(g_game_h - 1) : 599.0f;
    float min_y = 0.0f;
    if (g_gl_active && g_cur_layout != LAYOUT_4_3) {
        min_y = g_top_bar_has_buttons ? 0.0f : 75.0f;
        max_y = g_bot_bar_dialog_mode ? 599.0f : 586.0f;
    }
    if (nx) {
        if (*nx < 0.0f) *nx = 0.0f;
        else if (*nx > max_x) *nx = max_x;
    }
    if (ny) {
        if (*ny < min_y) *ny = min_y;
        else if (*ny > max_y) *ny = max_y;
    }
}

void gl_window_to_game_coords(int win_x, int win_y, int *out_gx, int *out_gy) {
    float fx, fy;
    gl_window_to_game_coords_f(win_x, win_y, &fx, &fy);
    *out_gx = (int)(fx + 0.5f);
    *out_gy = (int)(fy + 0.5f);
}

static void draw_osd_quad_uv(GLuint tex, float x_px, float y_px, float w_px, float h_px,
                             float u0, float v0, float u1, float v1,
                             int draw_w, int draw_h, float alpha) {
    if (!tex || draw_w <= 0 || draw_h <= 0) return;
    float x0 = (2.0f * x_px / (float)draw_w) - 1.0f;
    float x1 = (2.0f * (x_px + w_px) / (float)draw_w) - 1.0f;
    float y0 = 1.0f - (2.0f * y_px / (float)draw_h);
    float y1 = 1.0f - (2.0f * (y_px + h_px) / (float)draw_h);

    glViewport(0, 0, draw_w, draw_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glUseProgram(g_prog_osd);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    p_glUniform1i(p_glGetUniformLocation(g_prog_osd, "uTex"), 0);
    p_glUniform4f(p_glGetUniformLocation(g_prog_osd, "uRect"), x0, y0, x1, y1);
    p_glUniform4f(p_glGetUniformLocation(g_prog_osd, "uUVRect"), u0, v0, u1, v1);
    p_glUniform1f(p_glGetUniformLocation(g_prog_osd, "uAlpha"), alpha);
    p_glUniform1f(p_glGetUniformLocation(g_prog_osd, "uSoftEdge"), 0.0f);
    p_glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

static void draw_osd_quad_f(GLuint tex, float x_px, float y_px, float w_px, float h_px,
                            int draw_w, int draw_h, float alpha) {
    draw_osd_quad_uv(tex, x_px, y_px, w_px, h_px, 0.0f, 0.0f, 1.0f, 1.0f, draw_w, draw_h, alpha);
}

static void draw_osd_quad(GLuint tex, int x_px, int y_px, int w_px, int h_px,
                          int draw_w, int draw_h, float alpha) {
    draw_osd_quad_f(tex, (float)x_px, (float)y_px, (float)w_px, (float)h_px, draw_w, draw_h, alpha);
}

static void gl_draw_frame_common(int src_w, int src_h, int src_bpp) {
    uint32_t now_ms = SDL_GetTicks();
    uint64_t now_ns_frame = mono_ns();
    g_fps_frames++;
    if (g_fps_window_start == 0) g_fps_window_start = now_ms;
    uint32_t dt = now_ms - g_fps_window_start;
    if (dt >= 400) {
        g_cur_fps = (float)g_fps_frames * 1000.0f / (float)dt;
        g_cur_ms = (g_cur_fps > 0.01f) ? (1000.0f / g_cur_fps) : 0.0f;
        g_fps_frames = 0;
        g_fps_window_start = now_ms;
    }

    int draw_w = 800, draw_h = 600;
    SDL_GL_GetDrawableSize(g_sdl_window, &draw_w, &draw_h);
    int layout = resolve_active_layout(draw_w, draw_h, src_bpp);
    int vx, vy, vw, vh;
    compute_viewport(draw_w, draw_h, layout, &vx, &vy, &vw, &vh);

    float disp_w, disp_h;
    layout_display_size(layout, &disp_w, &disp_h);
    float sx = (float)vw / disp_w;
    float sy = (float)vh / disp_h;

    int is_video = (src_bpp == 24);
    if (is_video) {
        g_trans_pending = 0;
        g_trans_active = 0;
    }

    int use_hd_canvas = (g_cfg.hd_assets_enabled && g_cfg.hd_sprites_enabled &&
                         g_hd_canvas_ready && g_hd_canvas_has_draws &&
                         g_hd_canvas_gw == src_w && g_hd_canvas_gh == src_h && !is_video);

    int use_hd_bg = (!use_hd_canvas && g_cfg.hd_assets_enabled && g_bg_active && g_bg_hd_id >= 0 &&
                     g_bg_orig_w > 0 && g_bg_orig_h > 0 &&
                     g_bg_orig_w <= src_w && g_bg_orig_h <= src_h && !is_video);

    int use_post_pipe = ensure_post_fbos(vw, vh);

    /* Trigger zero-copy FBO swap for smooth scene crossfade transition */
    if (use_post_pipe && g_trans_pending && g_cfg.scene_transitions && g_scene_has_rendered && !is_video) {
        g_scene_cur_idx ^= 1;
        g_trans_pending = 0;
        g_trans_active = 1;
        g_trans_start_ns = now_ns_frame;
    } else if (!g_cfg.scene_transitions) {
        g_trans_pending = 0;
        g_trans_active = 0;
    }

    float trans_alpha = 1.0f;
    if (g_trans_active) {
        float t = (float)(now_ns_frame - g_trans_start_ns) / (float)TRANS_DURATION_NS;
        if (t >= 1.0f) {
            g_trans_active = 0;
            trans_alpha = 1.0f;
        } else {
            if (t < 0.0f) t = 0.0f;
            float inv = 1.0f - t;
            trans_alpha = 1.0f - inv * inv * inv; /* cubic ease-out */
        }
    }

    /* ---- Pass 0: Render Upscaled Scene OR Native 4K DirectDraw Canvas into Scene FBO ---- */
    if (use_post_pipe) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo[g_scene_cur_idx]);
        glViewport(0, 0, vw, vh);
    } else {
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, draw_w, draw_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glViewport(vx, vy, vw, vh);
    }

    p_glUseProgram(g_prog_upscale);
    p_glActiveTexture(GL_TEXTURE0);
    if (use_hd_canvas) {
        glBindTexture(GL_TEXTURE_2D, g_hd_canvas_tex[g_hd_canvas_present_slot & 1]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, g_frame_tex);
        GLint filt = (g_cfg.shader_mode == SHADER_NEAREST) ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    if (use_hd_bg) {
        p_glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_bg_orig_tex);
        p_glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g_bg_hd_tex);
        p_glActiveTexture(GL_TEXTURE0);
    }

    p_glUniform1i(p_glGetUniformLocation(g_prog_upscale, "uTex"), 0);
    p_glUniform1i(p_glGetUniformLocation(g_prog_upscale, "uBgOrigTex"), 1);
    p_glUniform1i(p_glGetUniformLocation(g_prog_upscale, "uBgHdTex"), 2);
    p_glUniform2f(p_glGetUniformLocation(g_prog_upscale, "uSrcSize"), (float)src_w, (float)src_h);
    p_glUniform2f(p_glGetUniformLocation(g_prog_upscale, "uTexSize"),
                  (float)(use_hd_canvas ? g_hd_canvas_tw : src_w),
                  (float)(use_hd_canvas ? g_hd_canvas_th : src_h));
    p_glUniform2f(p_glGetUniformLocation(g_prog_upscale, "uDstSize"), (float)vw, (float)vh);
    p_glUniform4f(p_glGetUniformLocation(g_prog_upscale, "uBgRect"),
                  (float)g_bg_dst_x, (float)g_bg_dst_y, (float)g_bg_orig_w, (float)g_bg_orig_h);
    p_glUniform1i(p_glGetUniformLocation(g_prog_upscale, "uMode"), g_cfg.shader_mode);
    p_glUniform1f(p_glGetUniformLocation(g_prog_upscale, "uSharp"), g_cfg.sharpen_strength);
    p_glUniform1i(p_glGetUniformLocation(g_prog_upscale, "uHasHdBg"), use_hd_bg ? 1 : 0);
    p_glUniform1i(p_glGetUniformLocation(g_prog_upscale, "uUseHdCanvas"), use_hd_canvas ? 1 : 0);
    int dialog_bar = (layout != LAYOUT_4_3 && !is_video && g_bot_bar_dialog_mode) ? 1 : 0;
    p_glUniform1i(p_glGetUniformLocation(g_prog_upscale, "uLayout"), layout);
    p_glUniform1i(p_glGetUniformLocation(g_prog_upscale, "uDialogBar"), dialog_bar);

    p_glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* Classic / non-HD-canvas fallback: composite clean-room "REMASTERED" onto Main Menu scene */
    if (!use_hd_canvas && !is_video && src_w == 800 && src_h == 600 && layout == LAYOUT_4_3 &&
        (game_is_main_menu_active() || (use_hd_bg && g_bg_is_main_menu))) {
        ensure_remastered_badge_tex();
        if (g_remastered_badge_tex) {
            float qx0, qy0, qx1, qy1;
            get_remastered_badge_game_rect(&qx0, &qy0, &qx1, &qy1);
            float x0 = (2.0f * qx0 / 800.0f) - 1.0f;
            float x1 = (2.0f * qx1 / 800.0f) - 1.0f;
            float y0 = 1.0f - (2.0f * qy0 / 600.0f);
            float y1 = 1.0f - (2.0f * qy1 / 600.0f);

            glEnable(GL_BLEND);
            p_glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
            p_glUseProgram(g_prog_osd);
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_remastered_badge_tex);
            p_glUniform1i(p_glGetUniformLocation(g_prog_osd, "uTex"), 0);
            p_glUniform4f(p_glGetUniformLocation(g_prog_osd, "uRect"), x0, y0, x1, y1);
            p_glUniform4f(p_glGetUniformLocation(g_prog_osd, "uUVRect"), 0.0f, 0.0f, 1.0f, 1.0f);
            p_glUniform1f(p_glGetUniformLocation(g_prog_osd, "uAlpha"), 1.0f);
            p_glUniform1f(p_glGetUniformLocation(g_prog_osd, "uSoftEdge"), 0.0f);
            p_glBindVertexArray(g_vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisable(GL_BLEND);
        }
    }

    if (use_post_pipe) {
        g_scene_has_rendered = 1;

        /* ---- Pass 1: Optional Quarter-Res Emissive Bloom Pyramid ---- */
        float bloom_strength = 0.0f;
        if (!is_video) {
            if (g_cfg.bloom_level == BLOOM_SUBTLE)      bloom_strength = 0.25f;
            else if (g_cfg.bloom_level == BLOOM_MEDIUM) bloom_strength = 0.50f;
            else if (g_cfg.bloom_level == BLOOM_RICH)   bloom_strength = 0.85f;
        }
        if (bloom_strength > 0.001f) {
            /* Bright-pass extract -> g_bloom_fbo[0] */
            p_glBindFramebuffer(GL_FRAMEBUFFER, g_bloom_fbo[0]);
            glViewport(0, 0, g_bloom_w, g_bloom_h);
            p_glUseProgram(g_prog_bloom_ext);
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_scene_tex[g_scene_cur_idx]);
            p_glUniform1i(p_glGetUniformLocation(g_prog_bloom_ext, "uScene"), 0);
            p_glUniform2f(p_glGetUniformLocation(g_prog_bloom_ext, "uTexelSize"),
                          1.0f / (float)vw, 1.0f / (float)vh);
            p_glBindVertexArray(g_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            /* 3 iterations of progressive separable H + V Gaussian blur for wide soft halos */
            p_glUseProgram(g_prog_bloom_blur);
            p_glUniform1i(p_glGetUniformLocation(g_prog_bloom_blur, "uTex"), 0);
            static const float k_bloom_steps[3] = { 1.0f, 2.2f, 4.0f };
            for (int iter = 0; iter < 3; iter++) {
                float step = k_bloom_steps[iter];
                /* Horizontal: 0 -> 1 */
                p_glBindFramebuffer(GL_FRAMEBUFFER, g_bloom_fbo[1]);
                glViewport(0, 0, g_bloom_w, g_bloom_h);
                glBindTexture(GL_TEXTURE_2D, g_bloom_tex[0]);
                p_glUniform2f(p_glGetUniformLocation(g_prog_bloom_blur, "uDir"),
                              step / (float)g_bloom_w, 0.0f);
                glDrawArrays(GL_TRIANGLES, 0, 3);

                /* Vertical: 1 -> 0 */
                p_glBindFramebuffer(GL_FRAMEBUFFER, g_bloom_fbo[0]);
                glViewport(0, 0, g_bloom_w, g_bloom_h);
                glBindTexture(GL_TEXTURE_2D, g_bloom_tex[1]);
                p_glUniform2f(p_glGetUniformLocation(g_prog_bloom_blur, "uDir"),
                              0.0f, step / (float)g_bloom_h);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
        }

        /* ---- Pass 2: Final Backbuffer Composite (Ambient Bars + Remaster Post-FX) ---- */
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, draw_w, draw_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        int has_wide_wings = (layout == LAYOUT_4_3 && !is_video && g_bg_active && g_cfg.hd_assets_enabled &&
                              g_bg_wide_has && g_bg_wide_tex && g_bg_wide_w > g_bg_wide_h &&
                              vw < draw_w - 4 && g_cfg.aspect_mode != ASPECT_4_3);

        if (layout == LAYOUT_4_3 && g_cfg.ambient_bars && vw < draw_w - 4 && g_prog_sidebar && !has_wide_wings) {
            p_glUseProgram(g_prog_sidebar);
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_frame_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            p_glUniform1i(p_glGetUniformLocation(g_prog_sidebar, "uTex"), 0);
            p_glBindVertexArray(g_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        if (has_wide_wings) {
            /* Seamless 16:9 / 16:10 widescreen extended wings aligned 1:1 with the centered 4:3 menu */
            float wide_aspect = (float)g_bg_wide_w / (float)g_bg_wide_h;
            int wide_vw = (int)roundf((float)vw * wide_aspect / (4.0f / 3.0f));
            int wide_vx = (draw_w - wide_vw) / 2;
            if (g_cfg.aspect_mode == ASPECT_16_10) {
                int sc_w = (int)roundf((float)vh * (16.0f / 10.0f));
                if (sc_w > draw_w) sc_w = draw_w;
                int sc_x = (draw_w - sc_w) / 2;
                glEnable(GL_SCISSOR_TEST);
                glScissor(sc_x, vy, sc_w, vh);
            }
            glViewport(wide_vx, vy, wide_vw, vh);
            p_glUseProgram(g_prog_post);
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_bg_wide_tex);
            p_glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g_bg_wide_tex);
            p_glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, g_bloom_tex[0]);
            p_glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, g_bg_wide_tex);
            p_glActiveTexture(GL_TEXTURE0);
            p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uScene"), 0);
            p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uPrevScene"), 1);
            p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uBloom"), 2);
            p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uOrigTex"), 3);
            p_glUniform2f(p_glGetUniformLocation(g_prog_post, "uInvSize"), 1.0f / (float)wide_vw, 1.0f / (float)vh);
            p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uTransAlpha"), 1.0f);
            p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uBloomStrength"), 0.0f);
            p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uGradeMode"), g_cfg.color_grade);
            p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uSilhouetteAA"), 0);
            p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uAAStrength"), 0.0f);
            p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uClarity"), g_cfg.clarity_strength);
            p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uFilmic"), g_cfg.filmic_strength);
            p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uDeband"), g_cfg.deband_dither ? 1 : 0);
            p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uAtmosphere"), 0);
            p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uTime"), (float)(SDL_GetTicks() % 3600000u) * 0.001f);
            p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uVignette"), 0.0f);
            p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uVideoEnhance"), 0);
            p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uCompareSplit"), 0.0f);
            p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uLayout"), LAYOUT_4_3);
            p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uDialogBar"), 0);
            p_glBindVertexArray(g_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            if (g_cfg.aspect_mode == ASPECT_16_10) {
                glDisable(GL_SCISSOR_TEST);
            }
        }

        float vig_strength = 0.0f;
        if (!is_video && !has_wide_wings) {
            if (g_cfg.vignette_level == VIGNETTE_SUBTLE)      vig_strength = 0.18f;
            else if (g_cfg.vignette_level == VIGNETTE_MEDIUM) vig_strength = 0.36f;
        }

        glViewport(vx, vy, vw, vh);
        p_glUseProgram(g_prog_post);
        p_glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_scene_tex[g_scene_cur_idx]);
        p_glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_scene_tex[1 - g_scene_cur_idx]);
        p_glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g_bloom_tex[0]);
        p_glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, g_frame_tex);
        p_glActiveTexture(GL_TEXTURE0);

        p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uScene"), 0);
        p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uPrevScene"), 1);
        p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uBloom"), 2);
        p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uOrigTex"), 3);
        p_glUniform2f(p_glGetUniformLocation(g_prog_post, "uInvSize"), 1.0f / (float)vw, 1.0f / (float)vh);
        p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uTransAlpha"), trans_alpha);
        p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uBloomStrength"), bloom_strength);
        p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uGradeMode"), is_video ? GRADE_ORIGINAL : g_cfg.color_grade);
        p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uSilhouetteAA"), (!is_video) ? g_cfg.silhouette_aa : 0);
        p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uAAStrength"), g_cfg.aa_strength);
        p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uClarity"), (!is_video) ? g_cfg.clarity_strength : 0.0f);
        p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uFilmic"), (!is_video) ? g_cfg.filmic_strength : 0.0f);
        p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uDeband"), g_cfg.deband_dither ? 1 : 0);
        p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uAtmosphere"), (!is_video && g_cfg.atmosphere_fx) ? 1 : 0);
        p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uTime"), (float)(SDL_GetTicks() % 3600000u) * 0.001f);
        p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uVignette"), vig_strength);
        p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uVideoEnhance"), (is_video && g_cfg.video_enhance) ? 1 : 0);
        float split_x = 0.0f;
        if (g_compare_split_on && !is_video) {
            split_x = g_live_mouse_x / 800.0f;
            if (split_x < 0.06f) split_x = 0.06f;
            if (split_x > 0.94f) split_x = 0.94f;
        }
        p_glUniform1f(p_glGetUniformLocation(g_prog_post, "uCompareSplit"), split_x);
        p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uLayout"), layout);
        p_glUniform1i(p_glGetUniformLocation(g_prog_post, "uDialogBar"), dialog_bar);

        p_glBindVertexArray(g_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    /* Clean 4K PNG screenshot capture (before HUD / F1 menu / cursor!) */
    if (g_screenshot_pending) {
        g_screenshot_pending = 0;
        save_clean_screenshot_png(vx, vy, vw, vh);
    }

    /* ---- Pass 2.5: Hotspot Highlighter Overlay (hold Space/H or Gamepad X) ---- */
    static int g_hotspot_logged_press = 0;
    if (!g_hotspot_active) {
        g_hotspot_logged_press = 0;
    } else if (g_cfg.hotspot_hints && !is_video && !gl_menu_is_open()) {
        GameHotspot hotspots[64];
        int hcount = game_collect_room_hotspots(hotspots, 64);
        if (g_debug_log && !g_hotspot_logged_press) {
            g_hotspot_logged_press = 1;
            fprintf(stderr, "[HOTSPOT] Found %d clickable hotspot(s):\n", hcount);
            for (int i = 0; i < hcount; i++) {
                fprintf(stderr, "  #%02d (%3d,%3d) [%d:%d-%d:%d] kind=%d label='%s'\n",
                        i, hotspots[i].x, hotspots[i].y,
                        hotspots[i].x0, hotspots[i].y0, hotspots[i].x1, hotspots[i].y1,
                        hotspots[i].kind, hotspots[i].label);
            }
        }
        if (hcount > 0) {
            float t = (float)(SDL_GetTicks() % 60000u) * 0.001f;
            float pulse = 0.65f + 0.35f * sinf(t * 7.0f);
            float rad = 11.0f * sy * (0.85f + 0.25f * pulse);
            float ir = rad * 0.42f;

            glViewport(0, 0, draw_w, draw_h);
            glEnable(GL_BLEND);
            p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            p_glUseProgram(g_prog_solid);
            p_glBindVertexArray(g_vao);
            GLint loc_rect  = p_glGetUniformLocation(g_prog_solid, "uRect");
            GLint loc_color = p_glGetUniformLocation(g_prog_solid, "uColor");

            for (int i = 0; i < hcount; i++) {
                const GameHotspot *h = &hotspots[i];
                float ry = game_to_display_y((float)h->y, layout);
                if (ry < 0.0f || ry > disp_h) continue;
                float scx = (float)vx + (float)h->x * sx;
                float scy = (float)vy + (disp_h - ry) * sy;

                float r_out = 1.0f, g_out = 0.84f, b_out = 0.22f, a_out = 0.34f * pulse;
                float r_in  = 1.0f, g_in  = 0.96f, b_in  = 0.55f, a_in  = 0.94f;
                if (h->kind == 1) { /* Exit */
                    r_out = 0.22f; g_out = 0.88f; b_out = 1.0f; a_out = 0.36f * pulse;
                    r_in  = 0.72f; g_in  = 0.98f; b_in  = 1.0f;
                } else if (h->kind == 2) { /* Character / NPC */
                    r_out = 1.0f; g_out = 0.62f; b_out = 0.18f; a_out = 0.36f * pulse;
                    r_in  = 1.0f; g_in  = 0.86f; b_in  = 0.50f;
                }

                float x0 = (2.0f * (scx - rad) / (float)draw_w) - 1.0f;
                float x1 = (2.0f * (scx + rad) / (float)draw_w) - 1.0f;
                float y0 = (2.0f * (scy - rad) / (float)draw_h) - 1.0f;
                float y1 = (2.0f * (scy + rad) / (float)draw_h) - 1.0f;
                p_glUniform4f(loc_rect, x0, y0, x1, y1);
                p_glUniform4f(loc_color, r_out, g_out, b_out, a_out);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                float ix0 = (2.0f * (scx - ir) / (float)draw_w) - 1.0f;
                float ix1 = (2.0f * (scx + ir) / (float)draw_w) - 1.0f;
                float iy0 = (2.0f * (scy - ir) / (float)draw_h) - 1.0f;
                float iy1 = (2.0f * (scy + ir) / (float)draw_h) - 1.0f;
                p_glUniform4f(loc_rect, ix0, iy0, ix1, iy1);
                p_glUniform4f(loc_color, r_in, g_in, b_in, a_in);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            glDisable(GL_BLEND);

            int px_size = (int)(13.0f * sy + 0.5f);
            if (px_size < 14) px_size = 14;
            if (px_size > 34) px_size = 34;
            int outline = (px_size >= 22) ? 3 : 2;
            float vp_top = (float)(draw_h - vy - vh);

            for (int i = 0; i < hcount; i++) {
                const GameHotspot *h = &hotspots[i];
                if (h->label[0] == '\0') continue;
                float ry = game_to_display_y((float)h->y, layout);
                if (ry < 0.0f || ry > disp_h) continue;
                float scx = (float)vx + (float)h->x * sx;

                uint32_t fill_rgb = (h->kind == 1) ? 0xA8F6FFu : ((h->kind == 2) ? 0xFFD080u : 0xFFF29Cu);
                CanvasTextEntry *te = get_or_render_outlined_text_tex(h->label, px_size, outline, fill_rgb, 0x000000u);
                if (!te || !te->tex) continue;

                int lx = (int)(scx - 0.5f * (float)te->tex_w);
                int ly = (int)(vp_top + ry * sy - rad - (float)te->tex_h - 3.0f);
                int min_x = vx + 4;
                int max_x = vx + vw - te->tex_w - 4;
                int min_y = (int)vp_top + 4;
                int max_y = (int)vp_top + vh - te->tex_h - 4;
                if (max_x < min_x) max_x = min_x;
                if (max_y < min_y) max_y = min_y;
                if (lx < min_x) lx = min_x; else if (lx > max_x) lx = max_x;
                if (ly < min_y) ly = min_y; else if (ly > max_y) ly = max_y;

                draw_osd_quad(te->tex, lx, ly, te->tex_w, te->tex_h, draw_w, draw_h, 0.96f);
            }
        }
    }

    /* ---- Pass 3: Native Monitor-DPI Vector Text & Subtitles (Post-FX: always razor-sharp!) ---- */
    if (g_cfg.hd_vector_text && g_frame_text_count > 0 && !is_video) {
        for (int i = 0; i < g_frame_text_count; i++) {
            const GlTextDraw *td = &g_frame_texts[i];
            int native_px = (int)((float)td->font_height * 0.85f * sy + 0.5f);
            TextTexEntry *te = get_or_render_text_tex(td, native_px);
            if (!te || !te->tex) continue;

            float tx = (float)vx + (float)td->x * sx;
            float ty = (float)vy + game_to_display_y((float)td->y, layout) * sy;

            int cx0 = vx + (int)((float)td->clip_x0 * sx);
            int cy0 = vy + (int)(game_to_display_y((float)td->clip_y0, layout) * sy);
            int cx1 = vx + (int)((float)td->clip_x1 * sx + 0.5f);
            int cy1 = vy + (int)(game_to_display_y((float)td->clip_y1, layout) * sy + 0.5f);
            if (cx0 < vx) cx0 = vx;
            if (cy0 < vy) cy0 = vy;
            if (cx1 > vx + vw) cx1 = vx + vw;
            if (cy1 > vy + vh) cy1 = vy + vh;
            if (cx1 > cx0 && cy1 > cy0) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(cx0, draw_h - cy1, cx1 - cx0, cy1 - cy0);
                draw_osd_quad_f(te->tex, tx, ty, (float)te->tex_w, (float)te->tex_h, draw_w, draw_h, 1.0f);
                glDisable(GL_SCISSOR_TEST);
            }
        }
    }

    /* ---- Pass 4: High-Framerate Decoupled Hardware-Rate Cursor (F10) ---- */
    if (!gl_menu_is_open() && g_cfg.decoupled_cursor && g_cursor_ready &&
        g_live_mouse_visible && g_cursor_tex && !is_video) {
        float disp_my = game_to_display_y(g_live_mouse_y, layout);
        float cx = (float)vx + (g_live_mouse_x - (float)g_cursor_hot_x) * sx;
        float cy = (float)vy + (disp_my - (float)g_cursor_hot_y) * sy;
        float cw = (float)g_cursor_w * sx;
        float ch = (float)g_cursor_h * sy;
        glEnable(GL_SCISSOR_TEST);
        glScissor(vx, vy, vw, vh);
        draw_osd_quad_f(g_cursor_tex, cx, cy, cw, ch, draw_w, draw_h, 1.0f);
        glDisable(GL_SCISSOR_TEST);
    }

    /* ---- Pass 5: Optional Stats HUD (F3) ---- */
    if (g_cfg.show_hud) {
        if (!g_hud_tex || (now_ms - g_hud_last_update) >= 350) {
            g_hud_last_update = now_ms;
            if (g_hud_tex) { glDeleteTextures(1, &g_hud_tex); g_hud_tex = 0; }
            const char *lstr = (layout == LAYOUT_16_10) ? "16:10" : ((layout == LAYOUT_16_9) ? "16:9" : "4:3");
            char buf[384];
            snprintf(buf, sizeof(buf),
                     "%.0f FPS (%.1fms) | %s %dx%d | %s | HD-Bg:%s | HD-4K:%s(%d) | Bloom:%s | Vol %d%% | F1:Menu",
                     g_cur_fps, g_cur_ms, lstr, vw, vh,
                     gl_shader_name(g_cfg.shader_mode),
                     (g_cfg.hd_assets_enabled && g_bg_active && !is_video) ? "ON" : (g_cfg.hd_assets_enabled ? "RDY" : "OFF"),
                     use_hd_canvas ? "ON" : "OFF",
                     g_spr_gl_resident,
                     g_cfg.bloom_level ? "ON" : "OFF",
                     g_cfg.muted ? 0 : (int)(g_cfg.master_volume * 100.0f + 0.5f));
            g_hud_tex = render_pill_texture(buf, &g_hud_w, &g_hud_h);
        }
        draw_osd_quad(g_hud_tex, 14, 14, g_hud_w, g_hud_h, draw_w, draw_h, 0.95f);
    }

    /* ---- Pass 6: Auto-fading Toast ---- */
    if (now_ms < g_toast_until) {
        if (g_toast_dirty) {
            g_toast_dirty = 0;
            if (g_toast_tex) { glDeleteTextures(1, &g_toast_tex); g_toast_tex = 0; }
            g_toast_tex = render_pill_texture(g_toast_text, &g_toast_w, &g_toast_h);
        }
        float rem = (float)(g_toast_until - now_ms) / 500.0f;
        float alpha = rem < 1.0f ? rem : 1.0f;
        draw_osd_quad(g_toast_tex, (draw_w - g_toast_w) / 2, draw_h - g_toast_h - 24,
                      g_toast_w, g_toast_h, draw_w, draw_h, alpha);
    }

    /* ---- Pass 7: Interactive Graphics Menu (F1) & Dialog History Log (L) ---- */
    if (gl_menu_is_open()) {
        gl_menu_draw(draw_w, draw_h);
    } else if (gl_renderer_is_dialog_log_open()) {
        gl_dialog_log_draw(draw_w, draw_h);
    }

    g_mouse_moved_since_draw = 0;
    SDL_GL_SwapWindow(g_sdl_window);

    /* High-precision frame pacing when in Uncapped/High-FPS mode */
    if (g_cfg.vsync_mode == VSYNC_UNCAPPED && g_cfg.max_fps > 0) {
        uint64_t target_ns = 1000000000ull / (uint64_t)g_cfg.max_fps;
        uint64_t now_ns = mono_ns();
        if (g_last_present_ns != 0 && now_ns < g_last_present_ns + target_ns) {
            uint64_t wait_ns = (g_last_present_ns + target_ns) - now_ns;
            struct timespec req = { (time_t)(wait_ns / 1000000000ull), (long)(wait_ns % 1000000000ull) };
            clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
        }
        g_last_present_ns = mono_ns();
    } else {
        g_last_present_ns = mono_ns();
    }
}

void gl_renderer_redraw_if_needed(int force) {
    if (!g_gl_active || !g_has_frame || !pthread_equal(pthread_self(), g_gl_thread)) return;
    if (!force) {
        int want_redraw = 0;
        if (g_trans_active) want_redraw = 1;
        if (!want_redraw && g_cfg.decoupled_cursor && g_cursor_ready && g_mouse_moved_since_draw) want_redraw = 1;
        if (!want_redraw && gl_menu_needs_redraw()) want_redraw = 1;
        if (!want_redraw && (g_cfg.show_hud || g_hotspot_active || g_cfg.atmosphere_fx || SDL_GetTicks() < g_toast_until)) {
            uint64_t now = mono_ns();
            if (now - g_last_present_ns >= 16000000ull) want_redraw = 1;
        }
        if (!want_redraw) return;
        /* Throttle non-forced redraws to max_fps (or 240Hz cap) so we never spin CPU */
        uint64_t min_interval = (g_cfg.max_fps > 0) ? (1000000000ull / (uint64_t)g_cfg.max_fps) : 4000000ull;
        if (mono_ns() - g_last_present_ns < min_interval) return;
    }
    gl_draw_frame_common(g_frame_w, g_frame_h, g_frame_bpp);
}

void gl_renderer_present_pixels(const void *pixels, int w, int h, int pitch, int bpp) {
    if (!pixels || w <= 0 || h <= 0) return;
    g_game_w = w;
    g_game_h = h;

    if (!g_gl_active) {
        if (!g_sdl_renderer) return;
        uint32_t fmt = (bpp == 16) ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_XRGB8888;
        if (!g_fb_tex || g_fb_w != w || g_fb_h != h || g_fb_fmt != fmt) {
            if (g_fb_tex) SDL_DestroyTexture(g_fb_tex);
            g_fb_tex = SDL_CreateTexture(g_sdl_renderer, fmt, SDL_TEXTUREACCESS_STREAMING, w, h);
            g_fb_w = w; g_fb_h = h; g_fb_fmt = fmt;
            SDL_RenderSetLogicalSize(g_sdl_renderer, w, h);
        }
        if (!g_fb_tex) return;
        SDL_UpdateTexture(g_fb_tex, NULL, pixels, pitch);
        SDL_RenderClear(g_sdl_renderer);
        SDL_RenderCopy(g_sdl_renderer, g_fb_tex, NULL, NULL);
        SDL_RenderPresent(g_sdl_renderer);
        return;
    }

    glBindTexture(GL_TEXTURE_2D, g_frame_tex);
    int bps = bpp / 8;
    glPixelStorei(GL_UNPACK_ALIGNMENT, bps == 4 ? 4 : 2);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / bps);

    GLenum fmt  = (bpp == 16) ? GL_RGB : GL_BGRA;
    GLenum type = (bpp == 16) ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_INT_8_8_8_8_REV;
    GLint  ifmt = (bpp == 16) ? GL_RGB8 : GL_RGBA8;

    if (g_frame_w != w || g_frame_h != h || g_frame_bpp != bpp) {
        glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, pixels);
        g_frame_w = w; g_frame_h = h; g_frame_bpp = bpp;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, type, pixels);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g_has_frame = 1;
    update_hud_bar_masks(pixels, w, h, pitch, bpp);

    gl_draw_frame_common(w, h, bpp);
}

void gl_renderer_present_rgb24(const uint8_t *rgb, int w, int h, int stride) {
    if (!rgb || w <= 0 || h <= 0) return;

    if (!g_gl_active) {
        if (!g_sdl_renderer) return;
        if (!g_fb_tex || g_fb_w != w || g_fb_h != h || g_fb_fmt != SDL_PIXELFORMAT_RGB24) {
            if (g_fb_tex) SDL_DestroyTexture(g_fb_tex);
            g_fb_tex = SDL_CreateTexture(g_sdl_renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);
            g_fb_w = w; g_fb_h = h; g_fb_fmt = SDL_PIXELFORMAT_RGB24;
        }
        if (!g_fb_tex) return;
        SDL_UpdateTexture(g_fb_tex, NULL, rgb, stride);
        SDL_RenderClear(g_sdl_renderer);
        SDL_RenderCopy(g_sdl_renderer, g_fb_tex, NULL, NULL);
        SDL_RenderPresent(g_sdl_renderer);
        return;
    }

    glBindTexture(GL_TEXTURE_2D, g_frame_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 3);
    if (g_frame_w != w || g_frame_h != h || g_frame_bpp != 24) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
        g_frame_w = w; g_frame_h = h; g_frame_bpp = 24;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g_has_frame = 1;

    gl_draw_frame_common(w, h, 24);
}

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
    MROW_CLOSE
};
#define NUM_TAB2_ROWS ((int)(sizeof(g_tab2_rows) / sizeof(g_tab2_rows[0])))

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
    if (idx < 0 || idx >= NUM_TAB2_ROWS) return MROW_CLOSE;
    return g_tab2_rows[idx];
}

static void gl_menu_mark_dirty(void) {
    g_menu_dirty = 1;
}

static int gl_menu_needs_redraw(void) {
    return g_menu_open && g_menu_dirty;
}

static void gl_menu_shutdown(void) {
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
            snprintf(val, vsz, "[ F1 / Esc ]");
            break;
        default:
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
            gl_renderer_show_toast("Hotspot Highlight: %s", g_cfg.hotspot_hints ? "ON (Hold Space/H)" : "OFF");
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
        default:
            break;
    }
    gl_config_save(NULL);
    g_menu_dirty = 1;
    gl_renderer_redraw_if_needed(1);
}

static void menu_switch_tab(int new_tab) {
    new_tab = ((new_tab % 3) + 3) % 3;
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
        if (out_tab_hit) *out_tab_hit = (rx < 0.333f) ? 0 : ((rx < 0.667f) ? 1 : 2);
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

int gl_menu_handle_event(const SDL_Event *ev) {
    if (!g_menu_open || !ev) return 0;
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
        SDL_Surface *t = TTF_RenderUTF8_Blended(f_title, "REMASTER v5  -  GRAPHICS & ENGINE SETTINGS", col_gold);
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
    int tab_w = (pw - pad_x * 2 - tab_gap * 2) / 3;
    g_menu_tabs_top_frac = (float)tabs_y / (float)ph;
    g_menu_tabs_bot_frac = (float)(tabs_y + tabs_h) / (float)ph;

    const char *tab_names[3] = { "1. DISPLAY", "2. POST-FX", "3. ASSETS & TEXT" };
    for (int t = 0; t < 3; t++) {
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
        const char *hint = "Tab / Click Tabs to switch page   |   Click / Scroll / Arrows to adjust   |   Auto-Saved (mha2_settings.ini)";
        SDL_Surface *tf = TTF_RenderUTF8_Blended(f_foot, hint, col_dim);
        if (tf) {
            SDL_Rect dst = { (pw - tf->w) / 2, ph - foot_h + (foot_h - tf->h) / 2 - bw, tf->w, tf->h };
            SDL_BlitSurface(tf, NULL, surf, &dst);
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

static void gl_menu_draw(int draw_w, int draw_h) {
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

/* ---- In-Game Dialog & Clue History Overlay (L key) ---- */
#define DIALOG_LOG_CAP 32
static char   g_dialog_log[DIALOG_LOG_CAP][256];
static int    g_dialog_log_count = 0;
static int    g_dialog_log_open  = 0;
static int    g_dialog_log_dirty = 1;
static GLuint g_dialog_log_tex   = 0;
static int    g_dialog_log_tw = 0, g_dialog_log_th = 0;

void gl_renderer_log_dialog(const char *utf8) {
    if (!utf8) return;
    size_t len = strlen(utf8);
    if (len < 12) return; /* ignore short scores & 1-word hover labels */
    int has_space = 0;
    for (size_t i = 0; i < len; i++) {
        if (utf8[i] == ' ') { has_space = 1; break; }
    }
    if (!has_space) return;
    /* Deduplicate against recent 6 entries (dirty-rect redraws repaint the same line) */
    int check = g_dialog_log_count < 6 ? g_dialog_log_count : 6;
    for (int i = 0; i < check; i++) {
        if (strcmp(g_dialog_log[g_dialog_log_count - 1 - i], utf8) == 0) return;
    }
    if (g_dialog_log_count < DIALOG_LOG_CAP) {
        snprintf(g_dialog_log[g_dialog_log_count], sizeof(g_dialog_log[0]), "%s", utf8);
        g_dialog_log_count++;
    } else {
        memmove(g_dialog_log[0], g_dialog_log[1], (DIALOG_LOG_CAP - 1) * sizeof(g_dialog_log[0]));
        snprintf(g_dialog_log[DIALOG_LOG_CAP - 1], sizeof(g_dialog_log[0]), "%s", utf8);
    }
    g_dialog_log_dirty = 1;
}

void gl_renderer_toggle_dialog_log(void) {
    g_dialog_log_open = !g_dialog_log_open;
    g_dialog_log_dirty = 1;
    gl_renderer_redraw_if_needed(1);
}

int gl_renderer_is_dialog_log_open(void) {
    return g_dialog_log_open;
}

static void gl_dialog_log_draw(int draw_w, int draw_h) {
    if (!g_dialog_log_open || draw_w <= 0 || draw_h <= 0) return;
    if (g_dialog_log_dirty || !g_dialog_log_tex) {
        init_osd_font();
        int pw = (draw_w * 72) / 100;
        int ph = (draw_h * 74) / 100;
        if (pw < 560) pw = 560;
        if (ph < 380) ph = 380;
        if (pw > draw_w - 40) pw = draw_w - 40;
        if (ph > draw_h - 40) ph = draw_h - 40;
        SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, pw, ph, 32, SDL_PIXELFORMAT_RGBA32);
        if (!surf) return;
        SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 14, 18, 30, 238));
        /* Gold border */
        Uint32 gold = SDL_MapRGBA(surf->format, 235, 192, 72, 245);
        SDL_Rect top_b = { 0, 0, pw, 3 }, bot_b = { 0, ph - 3, pw, 3 };
        SDL_Rect lft_b = { 0, 0, 3, ph }, rgt_b = { pw - 3, 0, 3, ph };
        SDL_FillRect(surf, &top_b, gold); SDL_FillRect(surf, &bot_b, gold);
        SDL_FillRect(surf, &lft_b, gold); SDL_FillRect(surf, &rgt_b, gold);

        int fsz = (ph >= 700) ? 22 : 16;
        TTF_Font *ft_title = get_native_font(fsz + 4, 1);
        TTF_Font *ft_body  = get_native_font(fsz, 0);
        if (!ft_title) ft_title = g_osd_font;
        if (!ft_body)  ft_body  = g_osd_font;

        if (ft_title) {
            SDL_Color cg = { 248, 212, 98, 255 };
            SDL_Surface *ts = TTF_RenderUTF8_Blended(ft_title, "DIALOG & CLUE HISTORY  (Press Ctrl+L or Esc to close)", cg);
            if (ts) {
                SDL_Rect dr = { (pw - ts->w) / 2, 18, ts->w, ts->h };
                SDL_BlitSurface(ts, NULL, surf, &dr);
                SDL_FreeSurface(ts);
            }
        }
        int y = 64;
        int line_h = fsz + 14;
        if (ft_body) {
            if (g_dialog_log_count == 0) {
                SDL_Color cm = { 175, 185, 205, 255 };
                SDL_Surface *ts = TTF_RenderUTF8_Blended(ft_body, "No dialog lines recorded yet. Talk to characters or examine objects!", cm);
                if (ts) {
                    SDL_Rect dr = { 28, y + 12, ts->w, ts->h };
                    SDL_BlitSurface(ts, NULL, surf, &dr);
                    SDL_FreeSurface(ts);
                }
            } else {
                int max_lines = (ph - y - 24) / line_h;
                int start = (g_dialog_log_count > max_lines) ? (g_dialog_log_count - max_lines) : 0;
                for (int i = start; i < g_dialog_log_count && y + line_h < ph - 14; i++) {
                    SDL_Color cw = (i == g_dialog_log_count - 1)
                        ? (SDL_Color){ 255, 238, 165, 255 }
                        : (SDL_Color){ 224, 230, 242, 255 };
                    SDL_Surface *ts = TTF_RenderUTF8_Blended(ft_body, g_dialog_log[i], cw);
                    if (ts) {
                        SDL_Rect sr = { 0, 0, ts->w > (pw - 56) ? (pw - 56) : ts->w, ts->h };
                        SDL_Rect dr = { 28, y, sr.w, sr.h };
                        SDL_BlitSurface(ts, &sr, surf, &dr);
                        SDL_FreeSurface(ts);
                    }
                    y += line_h;
                }
            }
        }
        if (g_dialog_log_tex) glDeleteTextures(1, &g_dialog_log_tex);
        glGenTextures(1, &g_dialog_log_tex);
        glBindTexture(GL_TEXTURE_2D, g_dialog_log_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
        SDL_FreeSurface(surf);
        g_dialog_log_tw = pw;
        g_dialog_log_th = ph;
        g_dialog_log_dirty = 0;
    }
    if (g_dialog_log_tex) {
        draw_osd_quad(g_dialog_log_tex, (draw_w - g_dialog_log_tw) / 2, (draw_h - g_dialog_log_th) / 2,
                      g_dialog_log_tw, g_dialog_log_th, draw_w, draw_h, 1.0f);
    }
}

static void gl_dialog_log_shutdown(void) {
    if (g_dialog_log_tex) {
        glDeleteTextures(1, &g_dialog_log_tex);
        g_dialog_log_tex = 0;
    }
    g_dialog_log_dirty = 1;
}
