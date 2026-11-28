#ifndef MHA2_GL_INTERNAL_H
#define MHA2_GL_INTERNAL_H
/*
 * Private interface shared by the renderer translation units:
 *   gl_renderer.c   - GL context, frame composition, 4K canvas, cursor, text, post-FX
 *   gl_shaders.c    - GLSL 330 core shader sources
 *   gl_menu.c       - interactive F1 settings menu
 *   gl_dialog_log.c - Ctrl+L dialog & clue history overlay
 *   gl_page_reader.c - Space page reader for books, letters and notices
 * Not part of the public renderer API (gl_renderer.h); do not include elsewhere.
 */
#include "runtime.h"
#include "gl_renderer.h"
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_ttf.h>

/* ---- gl_shaders.c ---- */
extern const char *const VS_FULLSCREEN;
extern const char *const FS_SIDEBAR;
extern const char *const FS_UPSCALE;
extern const char *const VS_OSD;
extern const char *const FS_OSD;
extern const char *const FS_SOLID;
extern const char *const FS_BLOOM_EXTRACT;
extern const char *const FS_BLOOM_BLUR;
extern const char *const FS_POST;

/* ---- gl_renderer.c (shared helpers & state) ---- */
extern int g_fullscreen;
extern TTF_Font *g_osd_font;
int  clamp_i(int v, int lo, int hi);
void draw_osd_quad(GLuint tex, int x_px, int y_px, int w_px, int h_px,
                   int draw_w, int draw_h, float alpha);
int  gl_toast_visible(void);   /* a toast occupies the bottom centre right now */
void draw_osd_quad_uv(GLuint tex, float x_px, float y_px, float w_px, float h_px,
                      float u0, float v0, float u1, float v1,
                      int draw_w, int draw_h, float alpha);
GLuint gl_bg_texture(int asset_id, int *w, int *h);   /* cached 4K background texture */
void init_osd_font(void);
TTF_Font *get_native_font(int px_size, int bold);
void flush_canvas_text_cache(void);
void apply_vsync_mode(void);
const char *gl_aa_name(int mode);
const char *gl_audio_eq_name(int mode);
const char *gl_font_style_name(int style);
const char *gl_reverb_name(int mode);
const char *gl_speed_name(int sp);

/* ---- gl_menu.c ---- */
void gl_menu_mark_dirty(void);
void gl_menu_draw(int draw_w, int draw_h);
void gl_menu_shutdown(void);
int  gl_menu_needs_redraw(void);

/* ---- gl_dialog_log.c ---- */
void gl_dialog_log_draw(int draw_w, int draw_h);
void gl_dialog_log_shutdown(void);
void gl_dialog_log_mark_dirty(void);  /* re-render (e.g. text size changed) */

/* ---- gl_page_reader.c ---- */
void gl_page_reader_poll(void);       /* per frame: page change, key hint toast */
void gl_page_reader_draw(int draw_w, int draw_h);
void gl_page_reader_draw_hint(int draw_w, int draw_h);  /* bottom hint while closed */
void gl_page_reader_shutdown(void);
void gl_page_reader_mark_dirty(void);

#endif
