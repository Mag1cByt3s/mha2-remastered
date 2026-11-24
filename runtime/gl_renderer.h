#ifndef MHA2_GL_RENDERER_H
#define MHA2_GL_RENDERER_H

#include <stdint.h>
#include <SDL2/SDL.h>

enum GlShaderMode {
    SHADER_BICUBIC_CAS = 0,
    SHADER_LANCZOS_CAS = 1,
    SHADER_EDGE_SMOOTH = 2,
    SHADER_BILINEAR    = 3,
    SHADER_NEAREST     = 4,
    SHADER_CRT         = 5,
    SHADER_FSR1        = 6,
    SHADER_COUNT
};

enum GlVsyncMode {
    VSYNC_ADAPTIVE = 0,
    VSYNC_ON       = 1,
    VSYNC_UNCAPPED = 2,
    VSYNC_COUNT
};

enum GlAspectMode {
    ASPECT_AUTO  = 0,
    ASPECT_16_10 = 1,
    ASPECT_16_9  = 2,
    ASPECT_4_3   = 3,
    ASPECT_COUNT
};

enum GlBloomLevel {
    BLOOM_OFF    = 0,
    BLOOM_SUBTLE = 1,
    BLOOM_MEDIUM = 2,
    BLOOM_RICH   = 3,
    BLOOM_COUNT
};

enum GlGradeMode {
    GRADE_ORIGINAL   = 0,
    GRADE_REMASTERED = 1,
    GRADE_WARM       = 2,
    GRADE_DAYLIGHT   = 3,
    GRADE_COUNT
};

enum GlVignetteLevel {
    VIGNETTE_OFF    = 0,
    VIGNETTE_SUBTLE = 1,
    VIGNETTE_MEDIUM = 2,
    VIGNETTE_COUNT
};

enum GlAaMode {
    AA_OFF   = 0,
    AA_FXAA  = 1,
    AA_SMAA  = 2,
    AA_MULTI = 3,
    AA_COUNT
};

enum GlFontStyle {
    FONT_SANS  = 0,
    FONT_SERIF = 1,
    FONT_STYLE_COUNT
};

enum GlReverbMode {
    REVERB_OFF    = 0,
    REVERB_SUBTLE = 1,
    REVERB_RICH   = 2,
    REVERB_COUNT
};

enum GlAudioEqMode {
    EQ_OFF    = 0,
    EQ_WARM   = 1,
    EQ_STUDIO = 2,
    EQ_COUNT
};

enum GlGameSpeed {
    SPEED_1X  = 0,
    SPEED_15X = 1,
    SPEED_2X  = 2,
    SPEED_COUNT
};

#define MAX_SURFACE_TEXT_DRAWS 64
#define MAX_FRAME_TEXT_DRAWS   128
#define MAX_FRAME_SPRITE_DRAWS 192

typedef struct GlSpriteDraw {
    int sprite_id;          /* index in hd_sprites table */
    int dst_x;              /* destination X in 800x600 game space */
    int dst_y;              /* destination Y in 800x600 game space */
    int dst_w;              /* destination width in game space (supports perspective scaling) */
    int dst_h;              /* destination height in game space */
    int clip_x0;            /* visible clip rect in game space */
    int clip_y0;
    int clip_x1;
    int clip_y1;
    float uv_x0;            /* normalized UV sub-rect within HD sprite texture (0..1) */
    float uv_y0;
    float uv_x1;
    float uv_y1;
    uint32_t surf_handle;   /* source surface handle for dedup/update */
} GlSpriteDraw;

typedef struct GlTextDraw {
    int x;
    int y;
    int clip_x0;
    int clip_y0;
    int clip_x1;
    int clip_y1;
    int font_height;        /* original Win32 logical pixel height */
    int font_weight;        /* >= 600 = bold */
    uint32_t text_color;    /* COLORREF 0x00BBGGRR */
    uint32_t bk_color;      /* COLORREF 0x00BBGGRR */
    int bk_mode;            /* 1 = TRANSPARENT, 2 = OPAQUE */
    char utf8[256];
} GlTextDraw;

typedef struct ModernConfig {
    int truecolor_32bit;    /* 1 = prefer 32-bit XRGB8888 Mode 888 */
    int shader_mode;        /* enum GlShaderMode */
    int vsync_mode;         /* enum GlVsyncMode */
    int max_fps;            /* 0 = monitor/uncapped, >0 = target FPS cap in uncapped mode */
    int start_fullscreen;   /* 1 = start in borderless fullscreen */
    int init_win_w;         /* default 1280 */
    int init_win_h;         /* default 960 */
    int show_hud;           /* F3 toggle */
    float sharpen_strength; /* 0.0 .. 1.0 (default 0.65) */
    float master_volume;    /* 0.0 .. 1.5 (default 1.0) */
    int muted;              /* F9 toggle */
    int hd_vector_text;     /* F5 toggle: render GDI TextOutA natively at monitor DPI */
    int hd_assets_enabled;  /* F6 toggle: composite HD PNG backgrounds from hd_assets/ */
    int hd_sprites_enabled; /* F1 toggle: composite HD RGBA sprites/characters/icons/cursors */
    int decoupled_cursor;   /* F10 toggle: render game cursor at full refresh rate in GL */
    int aspect_mode;        /* F12 / F1: enum GlAspectMode (Auto / 16:10 / 16:9 / 4:3) */
    int ambient_bars;       /* F1: blurred ambient sidebars on 4:3 menus instead of black */
    int bloom_level;        /* F1: enum GlBloomLevel (Off / Subtle / Medium / Rich) */
    int color_grade;        /* F1: enum GlGradeMode (Original / Remastered / Warm / Daylight) */
    int silhouette_aa;      /* F1: enum GlAaMode (Off / FXAA 3.11 / SMAA 1x) */
    float aa_strength;      /* F1: 0.0 .. 1.0 (default 0.75) */
    float clarity_strength; /* F1: 0.0 .. 1.0 local contrast ("Clarity", default 0.30) */
    float filmic_strength;  /* F1: 0.0 .. 1.0 ACES-style filmic HDR strength (default 0.25) */
    int deband_dither;      /* F1: 8-bit gradient debanding + triangular dither (default 1) */
    int atmosphere_fx;      /* F1: subtle living torch flicker + drifting dust motes (default 1) */
    int font_style;         /* F1: enum GlFontStyle (Clean Sans / Adventure Serif) */
    int text_backdrop;      /* F1: 0=Classic Outline, 1=Soft Shadow + Subtle Backing */
    int audio_reverb;       /* F1: enum GlReverbMode (Off / Subtle Castle / Rich Hall) */
    int audio_eq;           /* F1: enum GlAudioEqMode (Off / Warm Remaster / Studio Crisp) */
    int game_speed;         /* F1: enum GlGameSpeed (1.0x / 1.5x / 2.0x) */
    int hotspot_hints;      /* F1: 1=allow Space/H hotspot highlighter */
    int vignette_level;     /* F1: enum GlVignetteLevel (Off / Subtle / Medium) */
    int scene_transitions;  /* F1: smooth GPU crossfade on room/menu switches */
    int video_enhance;      /* F1: GPU deblocking + sharpening on FMV cutscenes */
    int display_index;      /* F1: target monitor index for multi-monitor fullscreen (0=Display 1) */
    int overlay_text_scale; /* F1: % size of remaster overlay text (dialog log, hotspot labels, toasts) */
    char hd_assets_dir[512];/* default "hd_assets" */
} ModernConfig;

extern ModernConfig g_cfg;

/* Persistent Settings INI */
void gl_config_load(const char *path);
void gl_config_save(const char *path);

int  gl_renderer_init(const char *title, int w, int h);
void gl_renderer_shutdown(void);
void gl_renderer_present_pixels(const void *pixels, int w, int h, int pitch, int bpp);
void gl_renderer_present_rgb24(const uint8_t *rgb, int w, int h, int stride);
void gl_renderer_redraw_if_needed(int force);

/* Scene Aspect, Transitions & HD Background Override */
void gl_renderer_set_scene_widescreen(int is_widescreen);
void gl_renderer_trigger_transition(void);
void gl_renderer_set_hd_background(int asset_id, const void *orig_pixels,
                                   int w, int h, int pitch, int bpp,
                                   int dst_x, int dst_y);
void gl_renderer_clear_hd_background(void);

/* Persistent 4K GPU DirectDraw Canvas (mirrors primary=0 & backbuffer=1 in exact painter's order) */
void gl_canvas_colorfill(int slot, int game_w, int game_h,
                         int x0, int y0, int x1, int y1,
                         float r, float g, float b, float a);
void gl_canvas_blit_hd_bg(int slot, int game_w, int game_h,
                          int dx0, int dy0, int dx1, int dy1,
                          float u0, float v0, float u1, float v1);
void gl_canvas_blit_hd_sprite(int slot, int game_w, int game_h, int sprite_id,
                              int dx0, int dy0, int dx1, int dy1,
                              float u0, float v0, float u1, float v1,
                              int blend_alpha, float alpha);
void gl_canvas_blit_rgba_patch(int slot, int game_w, int game_h,
                               const uint32_t *rgba_premul, int pw, int ph,
                               int dx0, int dy0, int dx1, int dy1,
                               int blend_alpha);
void gl_canvas_blit_canvas(int dst_slot, int src_slot, int game_w, int game_h,
                           int dx0, int dy0, int dx1, int dy1,
                           int sx0, int sy0, int sx1, int sy1);
void gl_canvas_draw_vector_text(int slot, int game_w, int game_h,
                                const char *utf8,
                                int text_x0, int text_y0, int text_w, int text_h,
                                int clip_x0, int clip_y0, int clip_x1, int clip_y1,
                                uint32_t fill_rgb, uint32_t outline_rgb);
void gl_canvas_flip(void);
void gl_canvas_set_present_slot(int slot);

/* Native High-Res Vector Text & Subtitles */
void gl_renderer_set_frame_texts(const GlTextDraw *draws, int count);

/* High-Framerate Decoupled Hardware-Rate Cursor (supports 1x or 4x HD cursor textures) */
void gl_renderer_set_cursor(const uint32_t *rgba, int tex_w, int tex_h,
                            int game_w, int game_h,
                            int hot_x, int hot_y, uint64_t hash);
void gl_renderer_set_live_mouse(float gx, float gy, int visible);
void gl_renderer_clear_cursor(void);

/* Live Hotkey Controls & In-Game Graphics Menu (F1) */
void gl_renderer_cycle_shader(void);
void gl_renderer_cycle_vsync(void);
void gl_renderer_cycle_aspect(void);
void gl_renderer_toggle_fullscreen(void);
void gl_renderer_toggle_hud(void);
void gl_renderer_toggle_hd_text(void);
void gl_renderer_toggle_hd_assets(void);
void gl_renderer_toggle_decoupled_cursor(void);
void gl_renderer_show_toast(const char *fmt, ...);

int  gl_menu_is_open(void);
void gl_menu_toggle(void);
int  gl_menu_handle_event(const SDL_Event *ev);
/* 4. CONTROLS tab waits for a key to bind: raw key events must go to the menu first */
int  gl_menu_is_capturing_key(void);
/* windowID marking menu key events synthesized from controller buttons */
#define GL_MENU_PAD_SYNTH 0xFFFFFFFFu

void gl_window_to_game_coords(int win_x, int win_y, int *out_gx, int *out_gy);
void gl_window_to_game_coords_f(int win_x, int win_y, float *out_gx, float *out_gy);
void gl_renderer_clamp_gamepad_cursor(float *nx, float *ny);
int  gl_renderer_is_active(void);
int  gl_renderer_is_gl_thread(void);
const char *gl_shader_name(int mode);
const char *gl_vsync_name(int mode);
const char *gl_aspect_name(int mode);
const char *gl_bloom_name(int mode);
const char *gl_grade_name(int mode);
const char *gl_vignette_name(int mode);

/* QoL, Showcase & Capture APIs */
void  gl_renderer_set_fast_walk(int active);
float gl_renderer_get_speed_mult(void);
void  gl_renderer_set_hotspot_highlight(int active);
void  gl_renderer_toggle_compare_split(void);
void  gl_renderer_request_screenshot(void);
/* SDL window ID of the game window (0 before init); used to tag injected input. */
uint32_t gl_renderer_window_id(void);
/* Screenshot of the next frame to an explicit PNG path (replay `shot` / `shotfull`):
 * full_frame = 0 captures the clean game image, 1 the final frame incl. overlays. */
void  gl_renderer_request_screenshot_to(const char *path, int full_frame);
void  gl_renderer_log_dialog(const char *utf8);
void  gl_renderer_toggle_dialog_log(void);
int   gl_renderer_is_dialog_log_open(void);

#endif /* MHA2_GL_RENDERER_H */
