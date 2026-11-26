/*
 * Page reader for books, letters and notices (hotspot key, default Space/H, on a page).
 *
 * Which pages: a .txt next to the matched HD background, written locally by
 * tools/extract_page_text.py from the player's own game files:
 * <hd_dir>/m/017/img/objBg.webp -> <hd_dir>/m/017/img/objBg.txt
 *   - text: shown as a clean text box over the dimmed close-up. One paragraph per line,
 *     an empty line adds space, "---" separates pages; a two-page spread gets two columns.
 *   - empty file: the OCR was not reliable; only the zoom view is offered.
 * Zoom view (Tab from the text box, or directly for empty files): the 4K page image
 * magnified; mouse / arrow keys / D-pad pan, wheel / +/- / LB/RB zoom.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gl_internal.h"
#include "hd_assets.h"
#include "input_bindings.h"
#include "text_wrap.h"

#define READER_MAX_TEXT   16384
#define READER_MAX_PAGES  8
#define READER_MAX_LINES  512

static int    g_reader_open = 0;
static int    g_reader_dirty = 1;
static GLuint g_reader_tex = 0, g_reader_dim_tex = 0;
static int    g_reader_tw = 0, g_reader_th = 0, g_reader_dw = 0, g_reader_dh = 0;
static int    g_text_asset = -2;            /* asset id g_text belongs to (-2: none loaded) */
static char  *g_text = NULL;                /* page text of g_text_asset, NULL: no page, "": zoom only */
static int    g_zoom_mode = 0;              /* 0: text box, 1: zoom view */
static float  g_zoom = 2.0f;                /* magnification relative to the fitted page */
static float  g_pan_x = 0.5f, g_pan_y = 0.5f;  /* view centre, 0..1 across the image */
static GLuint g_hint_tex = 0;
static int    g_hint_w = 0, g_hint_h = 0, g_hint_mode = -1;

/* Load (once per background) the .txt next to the active HD background. */
static const char *active_page_text(void) {
    int id = ddraw_active_bg_asset();
    if (id == g_text_asset) return g_text;
    g_text_asset = id;
    free(g_text);
    g_text = NULL;
    const char *img = hd_assets_get_path(id);
    const char *dot = strrchr(img, '.');
    if (!img[0] || !dot) return NULL;
    char path[1100];
    snprintf(path, sizeof(path), "%.*s.txt", (int)(dot - img), img);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = (char *)malloc(READER_MAX_TEXT);
    size_t n = buf ? fread(buf, 1, READER_MAX_TEXT - 1, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = '\0';   /* empty file: zoom-only page */
    g_text = buf;
    return g_text;
}

int gl_renderer_page_reader_available(void) {
    return active_page_text() != NULL;
}

int gl_renderer_is_page_reader_open(void) {
    return g_reader_open;
}

void gl_renderer_toggle_page_reader(void) {
    g_reader_open = !g_reader_open && gl_renderer_page_reader_available();
    if (g_reader_open) {
        g_zoom_mode = (g_text[0] == '\0');
        g_pan_x = g_pan_y = 0.5f;
    }
    g_reader_dirty = 1;
    gl_renderer_redraw_if_needed(1);
}

int gl_renderer_page_reader_zoom_mode(void) {
    return g_reader_open && g_zoom_mode;
}

/* Text box <-> zoom view (only pages with text have both). */
void gl_renderer_page_reader_switch_view(void) {
    if (!g_reader_open || !g_text || !g_text[0]) return;
    g_zoom_mode = !g_zoom_mode;
    gl_renderer_redraw_if_needed(1);
}

void gl_renderer_page_reader_zoom(int steps) {
    for (; steps > 0; steps--) g_zoom *= 1.25f;
    for (; steps < 0; steps++) g_zoom /= 1.25f;
    if (g_zoom < 1.0f) g_zoom = 1.0f;
    if (g_zoom > 5.0f) g_zoom = 5.0f;
    gl_renderer_redraw_if_needed(1);
}

/* Pan: absolute (pointer position, 0..1 across the window) or relative steps. */
void gl_renderer_page_reader_pan(float x, float y, int relative) {
    g_pan_x = relative ? g_pan_x + x : x;
    g_pan_y = relative ? g_pan_y + y : y;
    g_pan_x = g_pan_x < 0.0f ? 0.0f : (g_pan_x > 1.0f ? 1.0f : g_pan_x);
    g_pan_y = g_pan_y < 0.0f ? 0.0f : (g_pan_y > 1.0f ? 1.0f : g_pan_y);
    gl_renderer_redraw_if_needed(1);
}

/* Per frame: close when the page goes away, announce the key once per opened page. */
void gl_page_reader_poll(void) {
    static int announced = -2;
    if (active_page_text() == NULL) {
        g_reader_open = 0;
        announced = -2;
        return;
    }
    if (announced != g_text_asset) {
        announced = g_text_asset;
        char keys[64];
        bindings_describe(ACT_HOTSPOTS, keys, sizeof(keys));
        gl_renderer_show_toast(g_text[0] ? "Press %s to read the text" : "Press %s to zoom in on the page", keys);
        g_reader_dirty = 1;
    }
}

/* Split g_text into pages (lines of "---") and word-wrap each paragraph to col_w.
 * Returns the text height in pixels (tallest column for a two-page spread). */
typedef struct ReaderLayout {
    int  npages, ncols;
    int  count[READER_MAX_PAGES];
    char (*line)[256];
    int  *page;     /* page index of each line */
    int  *blank;    /* 1: paragraph spacer */
    int  *heading;  /* 1: all-caps heading line (drawn bold) */
    int  nlines;
} ReaderLayout;

static int layout_text(ReaderLayout *L, TTF_Font *font, int col_w, int line_h) {
    L->npages = 1;
    L->nlines = 0;
    memset(L->count, 0, sizeof(L->count));
    const char *p = g_text;
    while (p && *p && L->nlines < READER_MAX_LINES) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char para[2048];
        if (len >= sizeof(para)) len = sizeof(para) - 1;
        memcpy(para, p, len);
        para[len] = '\0';
        if (len && para[len - 1] == '\r') para[--len] = '\0';
        p = e ? e + 1 : p + strlen(p);
        if (strcmp(para, "---") == 0) {
            if (L->npages < READER_MAX_PAGES) L->npages++;
            continue;
        }
        int pg = L->npages - 1;
        if (len == 0) {
            L->blank[L->nlines] = 1;
            L->line[L->nlines][0] = '\0';
            L->page[L->nlines++] = pg;
            L->count[pg]++;
            continue;
        }
        int owner[64], heading = len < 64;
        for (const char *c = para; *c && heading; c++)
            if (*c >= 'a' && *c <= 'z') heading = 0;
        int n = wrap_text_lines(font, para, col_w, L->line + L->nlines, owner, 0,
                                READER_MAX_LINES - L->nlines < 64 ? READER_MAX_LINES - L->nlines : 64);
        for (int i = 0; i < n; i++) {
            L->blank[L->nlines + i] = 0;
            L->heading[L->nlines + i] = heading;
            L->page[L->nlines + i] = pg;
        }
        L->nlines += n;
        L->count[pg] += n;
    }
    /* height per column: 2 pages side by side, otherwise one column with page gaps */
    int h = 0;
    if (L->ncols == 2) {
        for (int pg = 0; pg < L->npages; pg++)
            if (L->count[pg] * line_h > h) h = L->count[pg] * line_h;
    } else {
        h = L->nlines * line_h + (L->npages - 1) * line_h;
    }
    return h;
}

static void blit_text(SDL_Surface *dst, TTF_Font *font, const char *s, SDL_Color c, int x, int y) {
    SDL_Surface *ts = TTF_RenderUTF8_Blended(font, s, c);
    if (!ts) return;
    SDL_Rect dr = { x, y, ts->w, ts->h };
    SDL_BlitSurface(ts, NULL, dst, &dr);
    SDL_FreeSurface(ts);
}

static void render_panel(int draw_w, int draw_h) {
    init_osd_font();
    int pw = (draw_w * 82) / 100, ph_max = (draw_h * 90) / 100;
    if (pw > draw_h * 3 / 2) pw = draw_h * 3 / 2;   /* keep lines short enough to read */
    int pad = draw_h / 30 + 8;

    ReaderLayout L;
    memset(&L, 0, sizeof(L));
    L.line  = (char (*)[256])calloc(READER_MAX_LINES, 256);
    L.page  = (int *)calloc(READER_MAX_LINES, sizeof(int));
    L.blank = (int *)calloc(READER_MAX_LINES, sizeof(int));
    L.heading = (int *)calloc(READER_MAX_LINES, sizeof(int));
    if (!L.line || !L.page || !L.blank || !L.heading) goto out;

    /* count pages once to pick the column layout */
    int pages = 1;
    for (const char *q = g_text; q && (q = strstr(q, "\n---")) != NULL; q += 4)
        if (q[4] == '\n' || q[4] == '\r' || q[4] == '\0') pages++;
    if (strncmp(g_text, "---", 3) == 0) pages++;
    L.ncols = (pages == 2) ? 2 : 1;

    int base = (draw_h * 23) / 1000;   /* ~37 px at 1600p, ~25 px at 1080p */
    if (base < 16) base = 16;
    int fsz = (base * g_cfg.overlay_text_scale) / 100, line_h = 0, hint_h = 0;
    TTF_Font *font = NULL, *hint_font = NULL;
    int col_gap = pad * 2;
    for (;; fsz -= 2) {   /* shrink until the page fits on screen */
        font = get_native_font(fsz, 0);
        if (!font) font = g_osd_font;
        if (!font) goto out;
        line_h = TTF_FontLineSkip(font) + fsz / 6;
        int col_w = (L.ncols == 2) ? (pw - 2 * pad - col_gap) / 2 : pw - 2 * pad;
        int text_h = layout_text(&L, font, col_w, line_h);
        hint_font = get_native_font(fsz * 2 / 3 > 12 ? fsz * 2 / 3 : 12, 0);
        if (!hint_font) hint_font = font;
        hint_h = TTF_FontLineSkip(hint_font);
        if (text_h + 2 * pad + hint_h + pad / 2 <= ph_max || fsz <= 14) {
            ph_max = text_h + 2 * pad + hint_h + pad / 2;
            break;
        }
    }
    int ph = ph_max;

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, pw, ph, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) goto out;
    SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 238, 228, 204, 250));   /* paper */
    Uint32 edge = SDL_MapRGBA(surf->format, 120, 86, 44, 255);
    SDL_Rect b[4] = { { 0, 0, pw, 3 }, { 0, ph - 3, pw, 3 }, { 0, 0, 3, ph }, { pw - 3, 0, 3, ph } };
    for (int i = 0; i < 4; i++) SDL_FillRect(surf, &b[i], edge);

    SDL_Color ink = { 34, 26, 18, 255 };
    TTF_Font *bold = get_native_font(fsz, 1);
    int col_w = (L.ncols == 2) ? (pw - 2 * pad - col_gap) / 2 : pw - 2 * pad;
    int y = pad, cur_page = 0;
    for (int i = 0; i < L.nlines; i++) {
        int pg = L.page[i];
        int x = pad;
        if (L.ncols == 2) {
            if (pg != cur_page) { cur_page = pg; y = pad; }
            x = pad + pg * (col_w + col_gap);
        } else if (pg != cur_page) {   /* page break: thin rule */
            cur_page = pg;
            SDL_Rect rule = { pad + col_w / 4, y + line_h / 2 - 1, col_w / 2, 2 };
            SDL_FillRect(surf, &rule, SDL_MapRGBA(surf->format, 150, 120, 80, 200));
            y += line_h;
        }
        TTF_Font *f = (L.heading[i] && bold) ? bold : font;
        if (!L.blank[i]) blit_text(surf, f, L.line[i], ink, x, y);
        y += L.blank[i] ? line_h / 2 : line_h;
    }
    if (L.ncols == 2) {   /* gutter between the two pages */
        SDL_Rect g = { pw / 2 - 1, pad, 2, ph - 2 * pad - hint_h };
        SDL_FillRect(surf, &g, SDL_MapRGBA(surf->format, 150, 120, 80, 160));
    }
    char keys[64], hint[128];
    bindings_describe(ACT_HOTSPOTS, keys, sizeof(keys));
    snprintf(hint, sizeof(hint), "Tab: zoom view   |   %s, Esc or click: close", keys);
    int hw = 0;
    TTF_SizeUTF8(hint_font, hint, &hw, NULL);
    blit_text(surf, hint_font, hint, (SDL_Color){ 110, 88, 60, 255 }, pw - pad - hw, ph - pad / 2 - hint_h);

    if (!g_reader_tex) glGenTextures(1, &g_reader_tex);
    glBindTexture(GL_TEXTURE_2D, g_reader_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
    SDL_FreeSurface(surf);
    g_reader_tw = pw;
    g_reader_th = ph;
out:
    free(L.line);
    free(L.page);
    free(L.blank);
    free(L.heading);
}

/* Bottom hint pill for the zoom view (re-rendered when the text/zoom-only kind changes). */
static void draw_zoom_hint(int draw_w, int draw_h) {
    static int hint_draw_h = 0;
    int kind = g_text[0] != '\0';
    if (!g_hint_tex || g_hint_mode != kind || hint_draw_h != draw_h) {
        hint_draw_h = draw_h;
        char keys[64], hint[192];
        bindings_describe(ACT_HOTSPOTS, keys, sizeof(keys));
        snprintf(hint, sizeof(hint), "Mouse/arrows: pan   |   Wheel/+/-: zoom   |   %s%s, Esc or click: close",
                 kind ? "Tab: text   |   " : "", keys);
        TTF_Font *f = get_native_font((draw_h * 15) / 1000 > 13 ? (draw_h * 15) / 1000 : 13, 0);
        if (!f) f = g_osd_font;
        SDL_Surface *ts = f ? TTF_RenderUTF8_Blended(f, hint, (SDL_Color){ 235, 228, 210, 255 }) : NULL;
        if (!ts) return;
        int pad = ts->h / 2;
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ts->w + 2 * pad, ts->h + pad, 32, SDL_PIXELFORMAT_RGBA32);
        if (s) {
            SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 0, 0, 0, 170));
            SDL_SetSurfaceBlendMode(ts, SDL_BLENDMODE_BLEND);
            SDL_Rect dr = { pad, pad / 2, ts->w, ts->h };
            SDL_BlitSurface(ts, NULL, s, &dr);
            if (!g_hint_tex) glGenTextures(1, &g_hint_tex);
            glBindTexture(GL_TEXTURE_2D, g_hint_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
            g_hint_w = s->w;
            g_hint_h = s->h;
            g_hint_mode = kind;
            SDL_FreeSurface(s);
        }
        SDL_FreeSurface(ts);
    }
    draw_osd_quad(g_hint_tex, (draw_w - g_hint_w) / 2, draw_h - g_hint_h - draw_h / 40,
                  g_hint_w, g_hint_h, draw_w, draw_h, 1.0f);
}

/* Zoom view: the 4K page magnified around the pan centre, black around it. */
static void draw_zoom(int draw_w, int draw_h) {
    int iw = 0, ih = 0;
    GLuint tex = gl_bg_texture(g_text_asset, &iw, &ih);
    draw_osd_quad(g_reader_dim_tex, 0, 0, draw_w, draw_h, draw_w, draw_h, 1.0f);
    if (!tex || iw <= 0 || ih <= 0) return;
    float fit = (float)draw_w / (float)iw;
    if ((float)ih * fit > (float)draw_h) fit = (float)draw_h / (float)ih;
    float scale = fit * g_zoom;                 /* screen px per image px */
    float vis_w = (float)draw_w / scale / (float)iw, vis_h = (float)draw_h / scale / (float)ih;
    if (vis_w > 1.0f) vis_w = 1.0f;
    if (vis_h > 1.0f) vis_h = 1.0f;
    float u0 = g_pan_x * (1.0f - vis_w), v0 = g_pan_y * (1.0f - vis_h);
    float w = vis_w * (float)iw * scale, h = vis_h * (float)ih * scale;
    draw_osd_quad_uv(tex, ((float)draw_w - w) / 2.0f, ((float)draw_h - h) / 2.0f, w, h,
                     u0, v0, u0 + vis_w, v0 + vis_h, draw_w, draw_h, 1.0f);
    draw_zoom_hint(draw_w, draw_h);
}

void gl_page_reader_draw(int draw_w, int draw_h) {
    if (!g_reader_open || !g_text || draw_w <= 0 || draw_h <= 0) return;
    if (!g_reader_dim_tex) {
        static const uint8_t black[4] = { 0, 0, 0, 255 };
        glGenTextures(1, &g_reader_dim_tex);
        glBindTexture(GL_TEXTURE_2D, g_reader_dim_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    }
    if (g_zoom_mode) {
        draw_zoom(draw_w, draw_h);
        return;
    }
    if (g_reader_dirty || !g_reader_tex || draw_w != g_reader_dw || draw_h != g_reader_dh) {
        render_panel(draw_w, draw_h);
        g_reader_dw = draw_w;
        g_reader_dh = draw_h;
        g_reader_dirty = 0;
    }
    draw_osd_quad(g_reader_dim_tex, 0, 0, draw_w, draw_h, draw_w, draw_h, 0.55f);
    if (g_reader_tex)
        draw_osd_quad(g_reader_tex, (draw_w - g_reader_tw) / 2, (draw_h - g_reader_th) / 2,
                      g_reader_tw, g_reader_th, draw_w, draw_h, 1.0f);
}

void gl_page_reader_mark_dirty(void) {
    g_reader_dirty = 1;
    g_hint_mode = -1;
}

void gl_page_reader_shutdown(void) {
    if (g_reader_tex) { glDeleteTextures(1, &g_reader_tex); g_reader_tex = 0; }
    if (g_reader_dim_tex) { glDeleteTextures(1, &g_reader_dim_tex); g_reader_dim_tex = 0; }
    if (g_hint_tex) { glDeleteTextures(1, &g_hint_tex); g_hint_tex = 0; }
    g_hint_mode = -1;
    free(g_text);
    g_text = NULL;
    g_text_asset = -2;
    g_reader_open = 0;
    g_reader_dirty = 1;
}
