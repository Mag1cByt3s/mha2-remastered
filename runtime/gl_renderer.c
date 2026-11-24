#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include "gl_renderer.h"
#include "hd_assets.h"
#include "gl_internal.h"
#include "input_bindings.h"
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
    .overlay_text_scale = 100,
    .hd_assets_dir      = "hd_assets",
};

static char g_ini_path[512] = "mha2_settings.ini";

int clamp_i(int v, int lo, int hi) {
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
        if (bindings_ini_parse(p, val)) continue;
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
        else if (strcmp(p, "overlay_text_scale") == 0) g_cfg.overlay_text_scale = clamp_i(atoi(val), 100, 200);
        else if (strcmp(p, "display_index") == 0) g_cfg.display_index = clamp_i(atoi(val), 0, 15);
    }
    fclose(f);
}


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
    fprintf(f, "overlay_text_scale=%d\n", g_cfg.overlay_text_scale);
    fprintf(f, "display_index=%d\n", g_cfg.display_index);
    bindings_ini_write(f);
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
int g_fullscreen = 0;
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

/* GPU cache of decoded HD backgrounds. Some screens blit from two different background
 * surfaces in the same frame (e.g. the m/012 picture-difference minigame alternates
 * objBg1/objBg2); with a single texture every switch re-decoded a 3200x1800 WebP and
 * rebuilt its mipmaps on the GL thread (~3 fps, 100% CPU). g_bg_hd_tex/g_bg_wide_tex
 * always alias the textures of the active entry. */
#define BG_TEX_CACHE_SIZE 4
typedef struct {
    int asset_id;               /* -1 = empty */
    GLuint hd_tex, wide_tex;
    int hd_w, hd_h, wide_w, wide_h, wide_has;
    uint64_t last_use;
} BgTexEntry;
static BgTexEntry g_bg_cache[BG_TEX_CACHE_SIZE] = {
    { .asset_id = -1 }, { .asset_id = -1 }, { .asset_id = -1 }, { .asset_id = -1 },
};
static uint64_t g_bg_cache_clock = 0;

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
TTF_Font *g_osd_font = NULL;
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

const char *gl_aa_name(int mode) {
    switch (mode) {
        case AA_FXAA:  return "FXAA 3.11";
        case AA_SMAA:  return "SMAA 1x (Crisp)";
        case AA_MULTI: return "SMAA + 3x Edge AA";
        default:       return "OFF";
    }
}

const char *gl_font_style_name(int style) {
    return (style == FONT_SERIF) ? "Adventure Serif" : "Clean Sans";
}

const char *gl_reverb_name(int mode) {
    switch (mode) {
        case REVERB_SUBTLE: return "Subtle Castle";
        case REVERB_RICH:   return "Rich Hall";
        default:            return "OFF";
    }
}

const char *gl_audio_eq_name(int mode) {
    switch (mode) {
        case EQ_WARM:   return "Warm Remaster";
        case EQ_STUDIO: return "Studio Crisp";
        default:        return "OFF";
    }
}

const char *gl_speed_name(int sp) {
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

static char g_screenshot_path[512]; /* explicit target (replay `shot`), else timestamped */

uint32_t gl_renderer_window_id(void) {
    return g_sdl_window ? SDL_GetWindowID(g_sdl_window) : 0u;
}

void gl_renderer_request_screenshot(void) {
    g_screenshot_path[0] = 0;
    g_screenshot_pending = 1;
    gl_renderer_redraw_if_needed(1);
}

static volatile int g_screenshot_full_pending = 0;

void gl_renderer_request_screenshot_to(const char *path, int full_frame) {
    snprintf(g_screenshot_path, sizeof(g_screenshot_path), "%s", path ? path : "");
    if (full_frame) g_screenshot_full_pending = 1;
    else g_screenshot_pending = 1;
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

    char fname[512];
    int explicit_path = g_screenshot_path[0] != 0;
    if (explicit_path) {
        snprintf(fname, sizeof(fname), "%s", g_screenshot_path);
        g_screenshot_path[0] = 0;
    } else {
        mkdir("screenshots", 0755);
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        snprintf(fname, sizeof(fname), "screenshots/mha2_%04d%02d%02d_%02d%02d%02d.png",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    }

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

    if (explicit_path) {
        /* replay capture: no toast, it would bleed into the next captured frame */
        if (!saved) fprintf(stderr, "[GL] screenshot to '%s' failed\n", fname);
    } else if (saved) {
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

void apply_vsync_mode(void) {
    if (!g_gl_active) return;
    if (g_cfg.vsync_mode == VSYNC_ADAPTIVE) {
        if (SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(1);
    } else if (g_cfg.vsync_mode == VSYNC_ON) {
        SDL_GL_SetSwapInterval(1);
    } else {
        SDL_GL_SetSwapInterval(0);
    }
}

void init_osd_font(void) {
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

TTF_Font *get_native_font(int px_size, int bold) {
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

void flush_canvas_text_cache(void) {
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
    TTF_Font *font = g_osd_font;
    if (g_cfg.overlay_text_scale > 100) {
        TTF_Font *scaled = get_native_font((18 * g_cfg.overlay_text_scale) / 100, 0);
        if (scaled) font = scaled;
    }
    SDL_Surface *txt = TTF_RenderUTF8_Blended(font, text, fg);
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
    for (int i = 0; i < BG_TEX_CACHE_SIZE; i++) {
        BgTexEntry *e = &g_bg_cache[i];
        if (e->hd_tex) glDeleteTextures(1, &e->hd_tex);
        if (e->wide_tex) glDeleteTextures(1, &e->wide_tex);
        memset(e, 0, sizeof(*e));
        e->asset_id = -1;
    }
    g_bg_hd_tex = 0;
    g_bg_wide_tex = 0;
    g_bg_wide_has = 0;
    g_bg_hd_id = -1;
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

static void upload_bg_texture(GLuint tex, const HdImageRGBA *img) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img->width, img->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->rgba);
    p_glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* Return the cache entry holding asset_id, decoding/uploading it into the least recently
 * used slot on a miss. NULL if the asset cannot be decoded. GL thread only. */
static BgTexEntry *bg_cache_acquire(int asset_id) {
    BgTexEntry *victim = &g_bg_cache[0];
    for (int i = 0; i < BG_TEX_CACHE_SIZE; i++) {
        BgTexEntry *e = &g_bg_cache[i];
        if (e->asset_id == asset_id) {
            e->last_use = ++g_bg_cache_clock;
            return e;
        }
        if (e->asset_id < 0 || (victim->asset_id >= 0 && e->last_use < victim->last_use)) victim = e;
    }

    const HdImageRGBA *hd = hd_assets_get_rgba(asset_id);
    if (!hd || !hd->rgba) return NULL;
    if (!victim->hd_tex) glGenTextures(1, &victim->hd_tex);
    upload_bg_texture(victim->hd_tex, hd);
    victim->asset_id = asset_id;
    victim->hd_w = hd->width;
    victim->hd_h = hd->height;
    victim->last_use = ++g_bg_cache_clock;

    /* Optional 16:9 widescreen extended sibling (*_wide.webp) for 4:3 menus */
    victim->wide_has = 0;
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
        if (!victim->wide_tex) glGenTextures(1, &victim->wide_tex);
        upload_bg_texture(victim->wide_tex, &wimg);
        victim->wide_w = wimg.width;
        victim->wide_h = wimg.height;
        victim->wide_has = 1;
    }
    hd_sprites_free_rgba(&wimg);

    if (g_cfg.hd_assets_enabled) {
        gl_renderer_show_toast("HD Background: %s (%dx%d)", hd_assets_get_name(asset_id), victim->hd_w, victim->hd_h);
    }
    return victim;
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
        BgTexEntry *e = bg_cache_acquire(asset_id);
        if (!e) {
            g_bg_active = 0;
            g_bg_is_main_menu = 0;
            return;
        }
        g_bg_hd_tex = e->hd_tex;
        g_bg_hd_w = e->hd_w;
        g_bg_hd_h = e->hd_h;
        g_bg_wide_tex = e->wide_tex;
        g_bg_wide_w = e->wide_w;
        g_bg_wide_h = e->wide_h;
        g_bg_wide_has = e->wide_has;
        g_bg_hd_id = asset_id;
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

void draw_osd_quad(GLuint tex, int x_px, int y_px, int w_px, int h_px,
                          int draw_w, int draw_h, float alpha) {
    draw_osd_quad_f(tex, (float)x_px, (float)y_px, (float)w_px, (float)h_px, draw_w, draw_h, alpha);
}

float gl_renderer_current_fps(void) {
    return g_cur_fps;
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
    } else if (g_cfg.hotspot_hints && !is_video && !gl_menu_is_open() && !g_bot_bar_dialog_mode) {
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

            int px_size = (int)(13.0f * sy * (float)g_cfg.overlay_text_scale / 100.0f + 0.5f);
            if (px_size < 14) px_size = 14;
            if (px_size > 68) px_size = 68;
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

    /* Full composited frame incl. overlays/HUD/menus (replay `shotfull`) */
    if (g_screenshot_full_pending) {
        g_screenshot_full_pending = 0;
        save_clean_screenshot_png(0, 0, draw_w, draw_h);
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

