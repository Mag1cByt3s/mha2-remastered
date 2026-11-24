/*
 * Ctrl+L dialog & clue history overlay, split out of gl_renderer.c.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gl_internal.h"
#include "input_bindings.h"
#include "text_wrap.h"

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

void gl_dialog_log_draw(int draw_w, int draw_h) {
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

        int fsz = (((ph >= 700) ? 22 : 16) * g_cfg.overlay_text_scale) / 100;
        TTF_Font *ft_title = get_native_font(fsz + 4, 1);
        TTF_Font *ft_body  = get_native_font(fsz, 0);
        if (!ft_title) ft_title = g_osd_font;
        if (!ft_body)  ft_body  = g_osd_font;

        int y = 64;
        if (ft_title) {
            SDL_Color cg = { 248, 212, 98, 255 };
            SDL_Color chint = { 175, 185, 205, 255 };
            char keys[64], title[160], hint[96];
            bindings_describe(ACT_DIALOG_LOG, keys, sizeof(keys));
            snprintf(hint, sizeof(hint), "(Press %s or Esc to close)", keys);
            snprintf(title, sizeof(title), "DIALOG & CLUE HISTORY  %s", hint);
            SDL_Surface *ts = TTF_RenderUTF8_Blended(ft_title, title, cg);
            SDL_Surface *hs = NULL;
            if (ts && ts->w > pw - 40) { /* large text: heading + hint on its own line */
                SDL_FreeSurface(ts);
                ts = TTF_RenderUTF8_Blended(ft_title, "DIALOG & CLUE HISTORY", cg);
                hs = ft_body ? TTF_RenderUTF8_Blended(ft_body, hint, chint) : NULL;
            }
            int ty = 18;
            if (ts) {
                SDL_Rect dr = { (pw - ts->w) / 2, ty, ts->w, ts->h };
                SDL_BlitSurface(ts, NULL, surf, &dr);
                ty += ts->h + 4;
                SDL_FreeSurface(ts);
            }
            if (hs) {
                SDL_Rect dr = { (pw - hs->w) / 2, ty, hs->w, hs->h };
                SDL_BlitSurface(hs, NULL, surf, &dr);
                ty += hs->h + 4;
                SDL_FreeSurface(hs);
            }
            if (ty + 14 > y) y = ty + 14; /* body starts below the actual title block */
        }
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
                /* Word-wrap every entry to the panel width, then show the newest wrapped
                 * lines that fit (entries used to be clipped at the right edge). */
                int max_lines = (ph - y - 24) / line_h;
                int wrap_w = pw - 56;
                char (*lines)[256] = (char (*)[256])calloc((size_t)DIALOG_LOG_CAP * 8u, 256);
                int *owner = (int *)calloc((size_t)DIALOG_LOG_CAP * 8u, sizeof(int));
                int nl = 0;
                for (int i = 0; lines && owner && i < g_dialog_log_count; i++)
                    nl += wrap_text_lines(ft_body, g_dialog_log[i], wrap_w, lines + nl, owner + nl, i,
                                          DIALOG_LOG_CAP * 8 - nl);
                int start = (nl > max_lines) ? (nl - max_lines) : 0;
                for (int k = start; k < nl && y + line_h < ph - 14; k++) {
                    SDL_Color cw = (owner[k] == g_dialog_log_count - 1)
                        ? (SDL_Color){ 255, 238, 165, 255 }
                        : (SDL_Color){ 224, 230, 242, 255 };
                    SDL_Surface *ts = TTF_RenderUTF8_Blended(ft_body, lines[k], cw);
                    if (ts) {
                        SDL_Rect sr = { 0, 0, ts->w > wrap_w ? wrap_w : ts->w, ts->h };
                        SDL_Rect dr = { 28, y, sr.w, sr.h };
                        SDL_BlitSurface(ts, &sr, surf, &dr);
                        SDL_FreeSurface(ts);
                    }
                    y += line_h;
                }
                free(lines);
                free(owner);
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

void gl_dialog_log_mark_dirty(void) {
    g_dialog_log_dirty = 1;
}

void gl_dialog_log_shutdown(void) {
    if (g_dialog_log_tex) {
        glDeleteTextures(1, &g_dialog_log_tex);
        g_dialog_log_tex = 0;
    }
    g_dialog_log_dirty = 1;
}
