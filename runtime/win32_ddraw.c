#include "runtime.h"
#include "gl_renderer.h"
#include "hd_assets.h"
#include "imports.h"
#include <pthread.h>

#define MAX_DD_SURFACES 2048
static DDrawSurface g_surfaces[MAX_DD_SURFACES];
static pthread_mutex_t g_dd_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_dd_width = 800;
static uint32_t g_dd_height = 600;
static uint32_t g_dd_bpp = 32;
static int g_cursor_erased_this_frame = 0;
static int g_cursor_drawn_this_frame  = 0;
static int g_cursor_erase_streak      = 0;
static int g_active_cursor_x0 = -1000, g_active_cursor_y0 = -1000;
static int g_active_cursor_x1 = -1000, g_active_cursor_y1 = -1000;

DDrawSurface *ddraw_get_surface(uint32_t handle) {
    if (handle >= 1 && handle < MAX_DD_SURFACES && g_surfaces[handle].in_use) {
        return &g_surfaces[handle];
    }
    return NULL;
}

static void fill_pixelformat(uint32_t pf_va, uint32_t bpp) {
    memset(g_mem + pf_va, 0, 32);
    mem_w32(pf_va + 0, 32);       /* dwSize */
    mem_w32(pf_va + 4, 0x40u);    /* DDPF_RGB */
    mem_w32(pf_va + 12, bpp);     /* dwRGBBitCount */
    if (bpp == 16) {
        mem_w32(pf_va + 16, 0x0000F800u); /* R */
        mem_w32(pf_va + 20, 0x000007E0u); /* G */
        mem_w32(pf_va + 24, 0x0000001Fu); /* B */
        mem_w32(pf_va + 28, 0x00000000u); /* A */
    } else {
        mem_w32(pf_va + 16, 0x00FF0000u); /* R */
        mem_w32(pf_va + 20, 0x0000FF00u); /* G */
        mem_w32(pf_va + 24, 0x000000FFu); /* B */
        mem_w32(pf_va + 28, 0x00000000u); /* A */
    }
}

static void fill_surf_desc(uint32_t desc_va, DDrawSurface *s, int include_ptr) {
    uint32_t sz = mem_r32(desc_va);
    if (sz < 108) sz = 108;
    memset(g_mem + desc_va, 0, sz);
    mem_w32(desc_va + 0, sz);
    mem_w32(desc_va + 4, 0x100Fu | (include_ptr ? 0x0000u : 0u)); /* DDSD_CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT */
    mem_w32(desc_va + 8, s->height);
    mem_w32(desc_va + 12, s->width);
    mem_w32(desc_va + 16, s->pitch);
    if (s->attached_handle) {
        mem_w32(desc_va + 20, 1); /* dwBackBufferCount */
        mem_w32(desc_va + 4, mem_r32(desc_va + 4) | 0x20u);
    }
    if (include_ptr) {
        mem_w32(desc_va + 36, s->pixels_va); /* lpSurface */
    }
    if (s->has_src_colorkey) {
        mem_w32(desc_va + 4, mem_r32(desc_va + 4) | 0x10000u);
        mem_w32(desc_va + 64, s->src_ck_low);
        mem_w32(desc_va + 68, s->src_ck_high);
    }
    fill_pixelformat(desc_va + 72, s->bpp);
    mem_w32(desc_va + 104, s->caps); /* ddsCaps.dwCaps */
}

static uint32_t alloc_surface(uint32_t w, uint32_t h, uint32_t bpp, uint32_t caps, int is_primary, int is_back) {
    pthread_mutex_lock(&g_dd_mtx);
    for (uint32_t i = 1; i < MAX_DD_SURFACES; i++) {
        if (!g_surfaces[i].in_use) {
            DDrawSurface *s = &g_surfaces[i];
            memset(s, 0, sizeof(*s));
            s->in_use = 1;
            s->refcount = 1;
            s->width = w ? w : g_dd_width;
            s->height = h ? h : g_dd_height;
            s->bpp = bpp ? bpp : g_dd_bpp;
            s->pitch = ((s->width * (s->bpp / 8u)) + 15u) & ~15u;
            s->caps = caps;
            s->is_primary = is_primary;
            s->is_backbuffer = is_back;
            s->hd_asset_id = -2; /* unchecked */
            uint32_t byte_sz = s->pitch * s->height;
            s->pixels_va = guest_alloc(byte_sz + 64u);
            if (!s->pixels_va) {
                memset(s, 0, sizeof(*s));
                pthread_mutex_unlock(&g_dd_mtx);
                return 0;
            }
            memset(g_mem + s->pixels_va, 0, byte_sz);
            s->obj_va = com_create_obj(COM_IFACE_DDRAW_SURFACE, 48, i);
            if (!s->obj_va) {
                uint32_t pva = s->pixels_va;
                memset(s, 0, sizeof(*s));
                pthread_mutex_unlock(&g_dd_mtx);
                guest_free(pva);
                return 0;
            }
            pthread_mutex_unlock(&g_dd_mtx);
            return i;
        }
    }
    pthread_mutex_unlock(&g_dd_mtx);
    fprintf(stderr, "FATAL: out of DDrawSurface slots!\n");
    return 0;
}

void ddraw_surface_add_text(DDrawSurface *s, const GlTextDraw *td) {
    if (!s || !td || !td->utf8[0]) return;
    if (s->num_text_draws >= MAX_SURFACE_TEXT_DRAWS) {
        /* Shift oldest entry out */
        memmove(&s->text_draws[0], &s->text_draws[1],
                (MAX_SURFACE_TEXT_DRAWS - 1) * sizeof(GlTextDraw));
        s->num_text_draws = MAX_SURFACE_TEXT_DRAWS - 1;
    }
    s->text_draws[s->num_text_draws++] = *td;
}

static void evict_texts_in_rect(DDrawSurface *dst, int x0, int y0, int x1, int y1) {
    if (!dst || dst->num_text_draws <= 0) return;
    if (x0 <= 0 && y0 <= 0 && x1 >= (int)dst->width && y1 >= (int)dst->height) {
        dst->num_text_draws = 0;
        return;
    }
    int out = 0;
    for (int i = 0; i < dst->num_text_draws; i++) {
        const GlTextDraw *td = &dst->text_draws[i];
        int ty0 = td->y;
        int ty1 = td->y + (td->font_height > 0 ? td->font_height : 16);
        int tx0 = td->x;
        int tx1 = td->x + (int)strlen(td->utf8) * 10;
        /* Keep text draw if its bounding rect does NOT intersect the overwritten area */
        if (tx1 <= x0 || tx0 >= x1 || ty1 <= y0 || ty0 >= y1) {
            if (out != i) dst->text_draws[out] = *td;
            out++;
        }
    }
    dst->num_text_draws = out;
}

/* ---- Dialog choice detection (16:10 / 16:9 bottom band) ----
 * Dialog choices are lines of the game's bitmap font in one column at the bottom (e.g.
 * x=286, y=522/546/570); with three or more options the first one starts above the
 * inventory band (y=530..579) that the widescreen layouts show. Every Font::DrawText line
 * starting at y >= 495 is remembered on the screen surface it was unlocked into and
 * forgotten when an opaque blit / colour fill paints over its start. Replaces the old pixel
 * heuristic (black y=500..524 + lit pixels), which mistook the inventory of dark rooms
 * (golem room l/019) for dialog choices and never saw choices drawn over a lit room edge. */
typedef struct { int active, x, y, h; } FontLineCtx;
static FontLineCtx g_font_line;

static void record_bottom_line(DDrawSurface *s, int x, int y, int h) {
    if (!s || y < 495 || y >= 600) return;
    for (int i = 0; i < s->num_bottom_lines; i++) {
        if (s->bottom_lines[i].x == x && s->bottom_lines[i].y == y) return;
    }
    if (s->num_bottom_lines >= (int)(sizeof(s->bottom_lines) / sizeof(s->bottom_lines[0]))) {
        memmove(&s->bottom_lines[0], &s->bottom_lines[1],
                (size_t)(s->num_bottom_lines - 1) * sizeof(s->bottom_lines[0]));
        s->num_bottom_lines--;
    }
    s->bottom_lines[s->num_bottom_lines].x = (int16_t)x;
    s->bottom_lines[s->num_bottom_lines].y = (int16_t)y;
    s->bottom_lines[s->num_bottom_lines].h = (int16_t)(h > 0 ? h : 16);
    s->num_bottom_lines++;
}

/* A line is gone once its start (first 32 px, full height) is painted over. */
static void evict_bottom_lines(DDrawSurface *dst, int x0, int y0, int x1, int y1) {
    if (!dst || dst->num_bottom_lines <= 0) return;
    int out = 0;
    for (int i = 0; i < dst->num_bottom_lines; i++) {
        int lx = dst->bottom_lines[i].x, ly = dst->bottom_lines[i].y, lh = dst->bottom_lines[i].h;
        int covered = (x0 <= lx && x1 >= lx + 32 && y0 <= ly && y1 >= ly + lh);
        if (!covered) dst->bottom_lines[out++] = dst->bottom_lines[i];
    }
    dst->num_bottom_lines = out;
}

/* Choices: >= 2 lines in one column, at least one starting above the inventory band.
   Single lines (hover labels) never count. */
static int bottom_choices_visible(const DDrawSurface *s) {
    if (!s) return 0;
    for (int i = 0; i < s->num_bottom_lines; i++) {
        int n = 0, above = 0;
        for (int j = 0; j < s->num_bottom_lines; j++) {
            int dx = s->bottom_lines[j].x - s->bottom_lines[i].x;
            if (dx < -2 || dx > 2) continue;
            n++;
            if (s->bottom_lines[j].y < 530) above = 1; /* INV_BAND_Y0 in gl_renderer.c */
        }
        if (n >= 2 && above) return 1;
    }
    return 0;
}

/* Track last full-scene background surface to trigger smooth room crossfades */
static uint32_t g_last_bg_surface_va = 0;
static int g_last_bg_asset_id = -1;

#include <sys/stat.h>
#include <dirent.h>

/* Rolling auto-save backup of <game dir>/mha2.sav save files on room transitions (throttled to 180s) */
static void maybe_backup_savegames(void) {
    static uint32_t s_last_backup_ms = 0;
    uint32_t now = SDL_GetTicks();
    if (s_last_backup_ms != 0 && (now - s_last_backup_ms) < 180000u) return;
    s_last_backup_ms = now;
    char src_dir[1100], dst_dir[1200];
    snprintf(src_dir, sizeof(src_dir), "%s/mha2.sav", g_game_dir);
    snprintf(dst_dir, sizeof(dst_dir), "%s/autosave_backup", src_dir);
    DIR *d = opendir(src_dir);
    if (!d) return;
    mkdir(dst_dir, 0755);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".sav") != 0) continue;
        char sp[1400], dp[1500];
        snprintf(sp, sizeof(sp), "%s/%s", src_dir, ent->d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dst_dir, ent->d_name);
        FILE *fi = fopen(sp, "rb");
        if (!fi) continue;
        FILE *fo = fopen(dp, "wb");
        if (fo) {
            uint8_t buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) fwrite(buf, 1, n, fo);
            fclose(fo);
        }
        fclose(fi);
    }
    closedir(d);
}

/* Cached decoded HD cursor RGBA so we never re-decode PNG per frame */
static int g_hd_cur_cached_id = -1;
static HdImageRGBA g_hd_cur_cached_img = { NULL, 0, 0 };

/* Reusable premultiplied RGBA patch buffer for 1x unmatched blits & Lock/Unlock diffs */
static uint32_t *g_patch_rgba_buf = NULL;
static size_t g_patch_rgba_cap = 0;

/* Lock/Unlock snapshot buffers for direct CPU writes to primary(0) / backbuffer(1) */
static uint8_t *g_lock_snap[2] = { NULL, NULL };
static size_t g_lock_snap_sz[2] = { 0, 0 };
static int g_lock_snap_valid[2] = { 0, 0 };

/* Active Font::DrawText (0x00486c40) interception context for native 4K vector typography */
typedef struct FontDrawCtx {
    int active;
    uint32_t font_va;
    int text_x0;
    int text_y0;
    int text_w;
    int text_h;
    int clip_x0, clip_y0, clip_x1, clip_y1;
    char utf8[256];
} FontDrawCtx;
static FontDrawCtx g_font_ctx;

#define MAX_FONT_COLOR_CACHE 16
typedef struct FontColorEntry {
    uint32_t font_va;
    uint32_t fill_rgb;
    uint32_t outline_rgb;
} FontColorEntry;
static FontColorEntry g_font_color_cache[MAX_FONT_COLOR_CACHE];
static int g_font_color_cache_count = 0;

/* Tracks the most recent DirectDraw-backed HD sprite blit so an immediately-following
   BlitBoth(desc, NULL, tbl2) call suppresses its redundant 1x semi-transparent edge ring
   (our composited 4K HD sprite already contains both tbl1 + tbl2!). */
typedef struct LastDdHdBlit {
    int valid;
    int slot;
    int ux0, uy0;
    int fw, fh;
} LastDdHdBlit;
static LastDdHdBlit g_last_dd_hd_blit;

static inline int surf_spr_slot(const DDrawSurface *dst) {
    return (dst && dst->is_backbuffer) ? 1 : 0;
}

static uint32_t *ensure_patch_rgba_buf(size_t n_pixels) {
    if (n_pixels > g_patch_rgba_cap) {
        size_t new_cap = n_pixels < (256u * 256u) ? (256u * 256u) : n_pixels;
        uint32_t *nb = (uint32_t *)realloc(g_patch_rgba_buf, new_cap * sizeof(uint32_t));
        if (!nb) return NULL;
        g_patch_rgba_buf = nb;
        g_patch_rgba_cap = new_cap;
    }
    return g_patch_rgba_buf;
}

static void upload_1x_patch_to_canvas(DDrawSurface *dst, int dx0, int dy0, int dx1, int dy1,
                                      DDrawSurface *src, int sx0, int sy0, int sx1, int sy1,
                                      int use_ck, uint32_t ck_lo, uint32_t ck_hi) {
    int sw = sx1 - sx0, sh = sy1 - sy0;
    if (sw <= 0 || sh <= 0 || dx1 <= dx0 || dy1 <= dy0) return;
    uint32_t *rgba = ensure_patch_rgba_buf((size_t)sw * (size_t)sh);
    if (!rgba) return;

    for (int y = 0; y < sh; y++) {
        int sy = sy0 + y;
        const uint8_t *srow = (sy >= 0 && sy < (int)src->height)
            ? (g_mem + src->pixels_va + (uint32_t)sy * src->pitch) : NULL;
        uint32_t *drow = rgba + (size_t)y * (size_t)sw;
        for (int x = 0; x < sw; x++) {
            int sx = sx0 + x;
            uint32_t out_px = 0;
            if (srow && sx >= 0 && sx < (int)src->width) {
                if (src->bpp == 32) {
                    uint32_t px;
                    memcpy(&px, srow + sx * 4, 4);
                    uint32_t rgb = px & 0xFFFFFFu;
                    if (!(use_ck && rgb >= (ck_lo & 0xFFFFFFu) && rgb <= (ck_hi & 0xFFFFFFu))) {
                        uint8_t r = (uint8_t)((rgb >> 16) & 0xFFu);
                        uint8_t g = (uint8_t)((rgb >> 8) & 0xFFu);
                        uint8_t b = (uint8_t)(rgb & 0xFFu);
                        out_px = (0xFFu << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
                    }
                } else {
                    uint16_t px;
                    memcpy(&px, srow + sx * 2, 2);
                    if (!(use_ck && px >= ck_lo && px <= ck_hi)) {
                        uint8_t r = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
                        uint8_t g = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
                        uint8_t b = (uint8_t)((px & 0x1Fu) * 255u / 31u);
                        out_px = (0xFFu << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
                    }
                }
            }
            drow[x] = out_px;
        }
    }
    gl_canvas_blit_rgba_patch(surf_spr_slot(dst), (int)dst->width, (int)dst->height,
                              rgba, sw, sh, dx0, dy0, dx1, dy1, use_ck ? 1 : 0);
}

static void sync_screen_lock_diff_to_canvas(DDrawSurface *s) {
    if (!s || (!s->is_primary && !s->is_backbuffer) || !gl_renderer_is_active()) return;
    int slot = surf_spr_slot(s);
    if (!g_lock_snap_valid[slot] || !g_lock_snap[slot]) return;
    g_lock_snap_valid[slot] = 0;

    int W = (int)s->width, H = (int)s->height;
    size_t need = (size_t)s->pitch * (size_t)H;
    if (g_lock_snap_sz[slot] < need) return;

    const uint8_t *cur_base = g_mem + s->pixels_va;
    const uint8_t *old_base = g_lock_snap[slot];
    int bps = (int)(s->bpp / 8u);

    /* Native 4K Vector Text replacement when Unlock() finishes inside Font::DrawText (0x00486c40).
       Handled BEFORE the 1x diff early-out so even a 1px dirty-rect sliver that overlaps a 4K
       vector outline (where the 1x bitmap happened to be transparent) is seamlessly repainted! */
    if (g_font_ctx.active && g_cfg.hd_vector_text && g_font_ctx.utf8[0]) {
        uint32_t fill_rgb = 0xFFFFFFu;
        uint32_t outline_rgb = 0x000000u;
        int color_found = 0;
        for (int i = 0; i < g_font_color_cache_count; i++) {
            if (g_font_color_cache[i].font_va == g_font_ctx.font_va) {
                fill_rgb = g_font_color_cache[i].fill_rgb;
                outline_rgb = g_font_color_cache[i].outline_rgb;
                color_found = 1;
                break;
            }
        }
        if (!color_found) {
            int sx0 = g_font_ctx.clip_x0 > g_font_ctx.text_x0 ? g_font_ctx.clip_x0 : g_font_ctx.text_x0;
            int sy0 = g_font_ctx.clip_y0 > g_font_ctx.text_y0 ? g_font_ctx.clip_y0 : g_font_ctx.text_y0;
            int sx1 = g_font_ctx.clip_x1 < (g_font_ctx.text_x0 + g_font_ctx.text_w)
                      ? g_font_ctx.clip_x1 : (g_font_ctx.text_x0 + g_font_ctx.text_w);
            int sy1 = g_font_ctx.clip_y1 < (g_font_ctx.text_y0 + g_font_ctx.text_h)
                      ? g_font_ctx.clip_y1 : (g_font_ctx.text_y0 + g_font_ctx.text_h);
            if (sx0 < 0) sx0 = 0;
            if (sy0 < 0) sy0 = 0;
            if (sx1 > W) sx1 = W;
            if (sy1 > H) sy1 = H;

            int best_luma = -1;
            int br = 255, bg = 255, bb = 255;
            for (int y = sy0; y < sy1; y++) {
                const uint8_t *crow = cur_base + (size_t)y * s->pitch;
                const uint8_t *orow = old_base + (size_t)y * s->pitch;
                for (int x = sx0; x < sx1; x++) {
                    if (memcmp(crow + x * bps, orow + x * bps, (size_t)bps) == 0) continue;
                    int r, g, b;
                    if (s->bpp == 32) {
                        uint32_t px;
                        memcpy(&px, crow + x * 4, 4);
                        r = (int)((px >> 16) & 0xFFu);
                        g = (int)((px >> 8) & 0xFFu);
                        b = (int)(px & 0xFFu);
                    } else {
                        uint16_t px;
                        memcpy(&px, crow + x * 2, 2);
                        r = (int)(((px >> 11) & 0x1Fu) * 255u / 31u);
                        g = (int)(((px >> 5) & 0x3Fu) * 255u / 63u);
                        b = (int)((px & 0x1Fu) * 255u / 31u);
                    }
                    int luma = r + g + b;
                    if (luma > best_luma) {
                        best_luma = luma;
                        br = r; bg = g; bb = b;
                    }
                }
            }
            if (g_font_ctx.text_h >= 24) {
                fill_rgb = (br > 225 && bg > 225 && bb > 225) ? 0xFFFFFFu : 0xFBDA6Fu;
                outline_rgb = 0x492C08u;
            } else if (bg > br + 35 && bg > bb + 35) {
                fill_rgb = 0x78FF78u; /* fntOtherSpeakers (NPC dialog green) */
            } else if (br > bb + 70 && bg > bb + 70) {
                fill_rgb = 0xFFDB01u; /* fntYellow (highlights / score / active choices) */
            } else if (best_luma >= 250 && br < 205 && bg < 205 && bb < 205) {
                fill_rgb = 0xBDBDBDu; /* fntGray (inactive / secondary text) */
            } else {
                fill_rgb = 0xFFFFFFu; /* fntWhite (Moorhuhn dialog / hover labels) */
            }
            if (best_luma >= 250 && g_font_color_cache_count < MAX_FONT_COLOR_CACHE) {
                g_font_color_cache[g_font_color_cache_count].font_va = g_font_ctx.font_va;
                g_font_color_cache[g_font_color_cache_count].fill_rgb = fill_rgb;
                g_font_color_cache[g_font_color_cache_count].outline_rgb = outline_rgb;
                g_font_color_cache_count++;
            }
        }

        gl_renderer_log_dialog(g_font_ctx.utf8);
        gl_canvas_draw_vector_text(slot, W, H, g_font_ctx.utf8,
                                   g_font_ctx.text_x0, g_font_ctx.text_y0,
                                   g_font_ctx.text_w, g_font_ctx.text_h,
                                   g_font_ctx.clip_x0, g_font_ctx.clip_y0,
                                   g_font_ctx.clip_x1, g_font_ctx.clip_y1,
                                   fill_rgb, outline_rgb);
        return;
    }

    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++) {
        const uint8_t *crow = cur_base + (size_t)y * s->pitch;
        const uint8_t *orow = old_base + (size_t)y * s->pitch;
        if (memcmp(crow, orow, (size_t)W * (size_t)bps) == 0) continue;
        if (y < y0) y0 = y;
        if (y + 1 > y1) y1 = y + 1;
        for (int x = 0; x < W; x++) {
            if (memcmp(crow + x * bps, orow + x * bps, (size_t)bps) != 0) {
                if (x < x0) x0 = x;
                if (x + 1 > x1) x1 = x + 1;
            }
        }
    }
    if (x1 <= x0 || y1 <= y0) return;

    /* Check if this Lock/Unlock was a uniform afill darken (e.g. ESC/dialog dim overlay)
       so the dimmed scene underneath stays 100% 4K sharp! */
    int bw = x1 - x0, bh = y1 - y0;
    if (bw >= 32 && bh >= 32) {
        int step_x = bw > 16 ? bw / 16 : 1;
        int step_y = bh > 16 ? bh / 16 : 1;
        double ratio_sum = 0.0;
        int bright_cnt = 0;
        int all_match = 1;
        double ref_ratio = -1.0;
        for (int y = y0; y < y1 && all_match; y += step_y) {
            const uint8_t *crow = cur_base + (size_t)y * s->pitch;
            const uint8_t *orow = old_base + (size_t)y * s->pitch;
            for (int x = x0; x < x1 && all_match; x += step_x) {
                int nr, ng, nb, or_, og, ob;
                if (s->bpp == 32) {
                    uint32_t np, op;
                    memcpy(&np, crow + x * 4, 4); memcpy(&op, orow + x * 4, 4);
                    nr = (int)((np >> 16) & 0xFFu); ng = (int)((np >> 8) & 0xFFu); nb = (int)(np & 0xFFu);
                    or_ = (int)((op >> 16) & 0xFFu); og = (int)((op >> 8) & 0xFFu); ob = (int)(op & 0xFFu);
                } else {
                    uint16_t np, op;
                    memcpy(&np, crow + x * 2, 2); memcpy(&op, orow + x * 2, 2);
                    nr = (int)(((np >> 11) & 0x1Fu) * 255u / 31u); ng = (int)(((np >> 5) & 0x3Fu) * 255u / 63u); nb = (int)((np & 0x1Fu) * 255u / 31u);
                    or_ = (int)(((op >> 11) & 0x1Fu) * 255u / 31u); og = (int)(((op >> 5) & 0x3Fu) * 255u / 63u); ob = (int)((op & 0x1Fu) * 255u / 31u);
                }
                int osum = or_ + og + ob;
                if (osum < 96) continue;
                double r = (double)(nr + ng + nb) / (double)osum;
                if (ref_ratio < 0.0) ref_ratio = r;
                if (abs(nr - (int)(or_ * ref_ratio + 0.5)) > 6 ||
                    abs(ng - (int)(og * ref_ratio + 0.5)) > 6 ||
                    abs(nb - (int)(ob * ref_ratio + 0.5)) > 6) {
                    all_match = 0;
                    break;
                }
                ratio_sum += r;
                bright_cnt++;
            }
        }
        if (all_match && bright_cnt >= 8) {
            float avg_r = (float)(ratio_sum / (double)bright_cnt);
            if (avg_r >= 0.02f && avg_r <= 0.96f) {
                gl_canvas_colorfill(slot, W, H, x0, y0, x1, y1, 0.0f, 0.0f, 0.0f, 1.0f - avg_r);
                return;
            }
        }
    }

    uint32_t *patch = ensure_patch_rgba_buf((size_t)bw * (size_t)bh);
    if (!patch) return;
    for (int y = 0; y < bh; y++) {
        const uint8_t *crow = cur_base + (size_t)(y0 + y) * s->pitch;
        const uint8_t *orow = old_base + (size_t)(y0 + y) * s->pitch;
        uint32_t *drow = patch + (size_t)y * (size_t)bw;
        for (int x = 0; x < bw; x++) {
            int sx = x0 + x;
            if (memcmp(crow + sx * bps, orow + sx * bps, (size_t)bps) == 0) {
                drow[x] = 0;
            } else if (s->bpp == 32) {
                uint32_t px;
                memcpy(&px, crow + sx * 4, 4);
                uint8_t r = (uint8_t)((px >> 16) & 0xFFu);
                uint8_t g = (uint8_t)((px >> 8) & 0xFFu);
                uint8_t b = (uint8_t)(px & 0xFFu);
                drow[x] = (0xFFu << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
            } else {
                uint16_t px;
                memcpy(&px, crow + sx * 2, 2);
                uint8_t r = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
                uint8_t g = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
                uint8_t b = (uint8_t)((px & 0x1Fu) * 255u / 31u);
                drow[x] = (0xFFu << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
            }
        }
    }
    gl_canvas_blit_rgba_patch(slot, W, H, patch, bw, bh, x0, y0, x1, y1, 1);
}

static int is_full_sprite_frame(const DDrawSurface *src, int sx0, int sy0, int sw, int sh) {
    if (!src || sw < 16 || sh < 16) return 0;
    int W = (int)src->width;
    int H = (int)src->height;
    /* Exact full surface */
    if (sx0 == 0 && sy0 == 0 && sw == W && sh == H) return 1;
    /* Exact frame in a uniform horizontal strip (e.g. 2-frame cursors/buttons) */
    if (sy0 == 0 && sh == H && W > sw && (W % sw) == 0 && (sx0 % sw) == 0) return 1;
    /* Exact frame in a uniform vertical strip */
    if (sx0 == 0 && sw == W && H > sh && (H % sh) == 0 && (sy0 % sh) == 0) return 1;
    return 0;
}

static int lookup_surface_sprite(DDrawSurface *src, int sx0, int sy0, int sw, int sh,
                                 int use_ck, uint32_t ck_lo, uint32_t ck_hi) {
    if (!src || hd_sprites_count() <= 0 || sw < 16 || sh < 16) return -1;
    uint32_t rgen = hd_sprites_get_room_gen();
    if (src->sprite_cache_room_gen != rgen) {
        src->sprite_cache_count = 0;
        src->sprite_cache_room_gen = rgen;
    }
    for (int i = 0; i < src->sprite_cache_count; i++) {
        if (src->sprite_cache[i].sx0 == sx0 && src->sprite_cache[i].sy0 == sy0 &&
            src->sprite_cache[i].sw == sw && src->sprite_cache[i].sh == sh) {
            return src->sprite_cache[i].sprite_id;
        }
    }
    int id = hd_sprites_match_rect(g_mem + src->pixels_va, (int)src->width, (int)src->height,
                                   (int)src->pitch, (int)src->bpp,
                                   sx0, sy0, sw, sh, use_ck, ck_lo, ck_hi);
    if (g_debug_log && sw <= 64 && sh <= 64 && use_ck) {
        fprintf(stderr, "[SPR-MATCH] surf=%ux%u rect=(%d,%d %dx%d) ck=%d -> id=%d (%s)\n",
                src->width, src->height, sx0, sy0, sw, sh, use_ck, id,
                id >= 0 ? hd_sprites_get_name(id) : "MISS");
    }
    if (src->sprite_cache_count < 96) {
        int slot = src->sprite_cache_count++;
        src->sprite_cache[slot].sx0 = sx0;
        src->sprite_cache[slot].sy0 = sy0;
        src->sprite_cache[slot].sw = sw;
        src->sprite_cache[slot].sh = sh;
        src->sprite_cache[slot].sprite_id = id;
    }
    return id;
}

/* Extract cropped colorkeyed cursor pixels as RGBA8 + FNV-1a 64-bit hash (with 4x HD cursor upgrade) */
static void upload_cursor_sprite(DDrawSurface *src, int sx0, int sy0, int sw, int sh,
                                 int use_ck, uint32_t ck_lo, uint32_t ck_hi,
                                 int hot_x, int hot_y) {
    if (sw <= 0 || sh <= 0 || sw > 128 || sh > 128) return;

    if (g_cfg.hd_assets_enabled && g_cfg.hd_sprites_enabled && hd_sprites_count() > 0) {
        int spr_id = lookup_surface_sprite(src, sx0, sy0, sw, sh, use_ck, ck_lo, ck_hi);
        if (spr_id >= 0) {
            if (g_hd_cur_cached_id != spr_id) {
                hd_sprites_free_rgba(&g_hd_cur_cached_img);
                g_hd_cur_cached_id = -1;
                if (hd_sprites_decode_rgba(spr_id, &g_hd_cur_cached_img) == 0) {
                    g_hd_cur_cached_id = spr_id;
                }
            }
            if (g_hd_cur_cached_id == spr_id && g_hd_cur_cached_img.rgba) {
                int hw = g_hd_cur_cached_img.width;
                int hh = g_hd_cur_cached_img.height;
                uint64_t hd_hash = 0x4844435552534F52ull ^ ((uint64_t)(uint32_t)spr_id * 1099511628211ull);
                gl_renderer_set_cursor((const uint32_t *)g_hd_cur_cached_img.rgba,
                                       hw, hh, sw, sh, hot_x, hot_y, hd_hash);
                return;
            }
        }
    }
    uint32_t rgba[128 * 128];
    uint64_t hash = 14695981039346656037ull;
    for (int y = 0; y < sh; y++) {
        int sy = sy0 + y;
        const uint8_t *srow = (sy >= 0 && sy < (int)src->height)
            ? (g_mem + src->pixels_va + (uint32_t)sy * src->pitch) : NULL;
        for (int x = 0; x < sw; x++) {
            int sx = sx0 + x;
            uint32_t out_px = 0;
            if (srow && sx >= 0 && sx < (int)src->width) {
                if (src->bpp == 32) {
                    uint32_t px;
                    memcpy(&px, srow + sx * 4, 4);
                    uint32_t rgb = px & 0xFFFFFFu;
                    if (!(use_ck && rgb >= (ck_lo & 0xFFFFFFu) && rgb <= (ck_hi & 0xFFFFFFu))) {
                        uint8_t r = (uint8_t)((rgb >> 16) & 0xFFu);
                        uint8_t g = (uint8_t)((rgb >> 8) & 0xFFu);
                        uint8_t b = (uint8_t)(rgb & 0xFFu);
                        out_px = (0xFFu << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
                    }
                } else {
                    uint16_t px;
                    memcpy(&px, srow + sx * 2, 2);
                    if (!(use_ck && px >= ck_lo && px <= ck_hi)) {
                        uint8_t r = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
                        uint8_t g = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
                        uint8_t b = (uint8_t)((px & 0x1Fu) * 255u / 31u);
                        out_px = (0xFFu << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
                    }
                }
            }
            rgba[y * sw + x] = out_px;
            hash ^= out_px;
            hash *= 1099511628211ull;
        }
    }
    gl_renderer_set_cursor(rgba, sw, sh, sw, sh, hot_x, hot_y, hash);
}

static DDrawSurface *g_last_presented;

/* FNV-1a 64 of the last presented guest frame (replay `hash` regression probes). */
uint64_t ddraw_last_frame_hash(uint32_t *out_w, uint32_t *out_h) {
    DDrawSurface *s = g_last_presented;
    if (out_w) *out_w = s ? s->width : 0;
    if (out_h) *out_h = s ? s->height : 0;
    if (!s || !s->in_use || !s->pixels_va) return 0;
    uint32_t row = s->width * (s->bpp / 8u);
    if (row > s->pitch || (uint64_t)s->pitch * s->height > GUEST_MEM_SIZE - s->pixels_va) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < s->height; y++) {
        const uint8_t *p = g_mem + s->pixels_va + (size_t)y * s->pitch;
        for (uint32_t x = 0; x < row; x++) { h ^= p[x]; h *= 0x100000001b3ull; }
    }
    return h;
}

void ddraw_present_surface(DDrawSurface *surf) {
    if (!surf || !surf->pixels_va) return;
    /* Do not overwrite an active AVI/DirectShow fullscreen cutscene with the empty
       DirectDraw backbuffer (fixes 25Hz black-frame strobe flicker during intro/cutscenes). */
    if (win32_avi_is_playing()) return;
    g_last_presented = surf;
    if (g_cursor_drawn_this_frame) {
        g_cursor_erase_streak = 0;
    } else if (g_cursor_erased_this_frame) {
        /* Require 3 consecutive full-erase frames without any cursor blit before hiding the GL cursor,
           so single-frame dirty-rect overlaps (e.g. animated objects or hover tooltips) never cause
           the cursor to blink invisible/transparent. */
        g_cursor_erase_streak++;
        if (g_cursor_erase_streak >= 3) {
            gl_renderer_clear_cursor();
            g_cursor_erase_streak = 0;
        }
    }
    g_cursor_erased_this_frame = 0;
    g_cursor_drawn_this_frame = 0;
    gl_canvas_set_present_slot(surf_spr_slot(surf));
    gl_renderer_set_frame_texts(surf->text_draws, surf->num_text_draws);
    gl_renderer_set_dialog_choices_visible(bottom_choices_visible(surf));
    gl_renderer_present_pixels(g_mem + surf->pixels_va, (int)surf->width, (int)surf->height,
                               (int)surf->pitch, (int)surf->bpp);
}

int ddraw_active_bg_asset(void) {
    return g_last_bg_asset_id;
}

void ddraw_present_rgb24(const uint8_t *rgb, int w, int h, int stride) {
    g_last_bg_surface_va = 0;
    g_last_bg_asset_id = -1;
    hd_sprites_set_room_prefix("");
    gl_renderer_set_scene_widescreen(0);
    gl_renderer_clear_hd_background();
    gl_renderer_set_frame_texts(NULL, 0);
    gl_renderer_set_dialog_choices_visible(0);
    gl_renderer_present_rgb24(rgb, w, h, stride);
}

static void surface_colorfill(DDrawSurface *dst, int x0, int y0, int x1, int y1, uint32_t color) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)dst->width) x1 = (int)dst->width;
    if (y1 > (int)dst->height) y1 = (int)dst->height;
    if (x0 >= x1 || y0 >= y1) return;
    evict_texts_in_rect(dst, x0, y0, x1, y1);
    evict_bottom_lines(dst, x0, y0, x1, y1);
    if (dst->is_primary || dst->is_backbuffer) {
        if (gl_renderer_is_active()) {
            float r, g, b;
            if (dst->bpp == 16) {
                r = (float)((color >> 11) & 0x1Fu) / 31.0f;
                g = (float)((color >> 5) & 0x3Fu) / 63.0f;
                b = (float)(color & 0x1Fu) / 31.0f;
            } else {
                r = (float)((color >> 16) & 0xFFu) / 255.0f;
                g = (float)((color >> 8) & 0xFFu) / 255.0f;
                b = (float)(color & 0xFFu) / 255.0f;
            }
            gl_canvas_colorfill(surf_spr_slot(dst), (int)dst->width, (int)dst->height,
                                x0, y0, x1, y1, r, g, b, 1.0f);
        }
    } else {
        dst->hd_asset_id = -2;
        dst->sprite_cache_count = 0;
        dst->known_cell_w = 0;
        dst->known_cell_h = 0;
        dst->cell_probe_room_gen = 0;
    }
    for (int y = y0; y < y1; y++) {
        uint8_t *row = g_mem + dst->pixels_va + (uint32_t)y * dst->pitch;
        if (dst->bpp == 16) {
            uint16_t c16 = (uint16_t)color;
            uint16_t *p = (uint16_t *)(row + x0 * 2);
            for (int x = x0; x < x1; x++) *p++ = c16;
        } else {
            uint32_t *p = (uint32_t *)(row + x0 * 4);
            for (int x = x0; x < x1; x++) *p++ = color;
        }
    }
}

static void maybe_lock_split_room_from_sprite(int spr_id) {
    if (spr_id < 0) return;
    const char *sname = hd_sprites_get_name(spr_id);
    if (!sname || !sname[0]) return;
    /* Only lock room prefix from unique per-room split-bg anchors (never objWolken,
       which is pixel-identical across desert rooms l/022, l/027, l/028, l/036). */
    if (strstr(sname, "/objBgO") || strstr(sname, "/objBgU") || strstr(sname, "/mskBg.")) {
        uint32_t prev_gen = hd_sprites_get_room_gen();
        hd_sprites_set_room_prefix(sname);
        if (hd_sprites_get_room_gen() != prev_gen) {
            if (g_last_bg_surface_va != 0) {
                gl_renderer_trigger_transition();
                maybe_backup_savegames();
            }
            /* Use non-zero sentinel 1u (never a valid guest surface VA) so transitioning
               out of this split room into a full-bg room still triggers crossfade/backup. */
            g_last_bg_surface_va = 1u;
            g_last_bg_asset_id = -1;
            gl_renderer_clear_hd_background();
        }
    }
}

static void surface_blt(DDrawSurface *dst, int dx0, int dy0, int dx1, int dy1,
                        DDrawSurface *src, int sx0, int sy0, int sx1, int sy1,
                        int use_src_ck, uint32_t ck_lo, uint32_t ck_hi) {
    if (!dst || !src || !dst->pixels_va || !src->pixels_va) return;
    int dw = dx1 - dx0, dh = dy1 - dy0;
    int sw = sx1 - sx0, sh = sy1 - sy0;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;

    int to_screen = (dst->is_primary || dst->is_backbuffer);
    if (!to_screen) {
        dst->hd_asset_id = -2;
        dst->sprite_cache_count = 0;
        dst->known_cell_w = 0;
        dst->known_cell_h = 0;
        dst->cell_probe_room_gen = 0;
    }

    /* ---- v3 Scene Aspect Auto-Detection (16:9 800x450 gameplay vs 4:3 800x600 menus) ---- */
    if (to_screen && !use_src_ck && !src->is_primary && !src->is_backbuffer) {
        if (dx0 == 0 && dy0 == 0 && dx1 >= 800 && dy1 >= 600) {
            gl_renderer_set_scene_widescreen(0);
        } else if (dw >= 780 && dy0 >= 70 && dy1 <= 530 && dh >= 100) {
            /* Covers both single 800x450 rooms (y=75..525) and split-strip/scrolling rooms */
            gl_renderer_set_scene_widescreen(1);
        }
    }

    /* ---- v3 HD Background Replacement Hook (800x600 menus AND 800x450 gameplay rooms) ---- */
    int handled_as_full_hd_bg = 0;
    if (to_screen && !use_src_ck && src->width >= 640 && src->height >= 440 &&
        !src->is_primary && !src->is_backbuffer) {
        int is_full_bg = (dx0 <= 0 && dy0 <= 0 && dx1 >= (int)dst->width && dy1 >= (int)dst->height) ||
                         (dw >= 780 && dy0 >= 70 && dy1 <= 530 && dh >= 400);
        if (src->hd_asset_id == -2 && hd_assets_count() > 0) {
            src->hd_asset_id = hd_assets_match_pixels(
                g_mem + src->pixels_va, (int)src->width, (int)src->height,
                (int)src->pitch, (int)src->bpp);
        }
        if (is_full_bg && (src->obj_va != g_last_bg_surface_va || src->hd_asset_id != g_last_bg_asset_id)) {
            if (g_last_bg_surface_va != 0) {
                gl_renderer_trigger_transition();
                maybe_backup_savegames();
            }
            g_last_bg_surface_va = src->obj_va;
            g_last_bg_asset_id = src->hd_asset_id;
        }
        if (src->hd_asset_id >= 0) {
            int bg_x = dx0 - sx0;
            int bg_y = dy0 - sy0;
            hd_sprites_set_room_prefix(hd_assets_get_name(src->hd_asset_id));
            gl_renderer_set_hd_background(
                src->hd_asset_id, g_mem + src->pixels_va,
                (int)src->width, (int)src->height, (int)src->pitch, (int)src->bpp,
                bg_x, bg_y);
            handled_as_full_hd_bg = 1;
        } else if (is_full_bg) {
            hd_sprites_set_room_prefix("");
            gl_renderer_clear_hd_background();
        }
    }

    /* ---- v3 High-Framerate Decoupled Cursor Auto-Detection ----
     * 1) Frame ITSELF must be small (<= 128x128), immediately excluding all room props
     *    like wardrobes (192x264), tables (359x147), doors, characters, etc., while
     *    accommodating 2-frame horizontal or vertical cursor strips up to 256px.
     * 2) Tracks sprite world origin on FULL unclipped frames only (zero false positives
     *    on stationary props clipped by moving cursor dirty rects), and seamlessly handles
     *    2-frame hover animations (horiz e.g. mcArrow 98x45->49x45, vert e.g. cuHelp).
     * 3) Once confirmed as a cursor, ALL blits of that surface (including tiny clipped
     *    dirty-rect slices!) are intercepted cleanly, and we ALWAYS upload the FULL
     *    unclipped frame directly from src->pixels_va so the cursor is never chopped or
     *    flickered by overlapping dirty rects! */
    if (to_screen && !use_src_ck) {
        int full_cover = (dx0 <= g_active_cursor_x0 && dy0 <= g_active_cursor_y0 &&
                          dx1 >= g_active_cursor_x1 && dy1 >= g_active_cursor_y1);
        int fullscreen_wipe = (dw >= 640 && dh >= 400);
        if (full_cover || fullscreen_wipe) {
            g_cursor_erased_this_frame = 1;
        }
    }

    if (to_screen && use_src_ck && g_os_cursor_hidden && dw == sw && dh == sh) {
        if (src->is_cursor) {
            int fw = (src->cursor_full_w > 0 && src->cursor_full_w <= (int)src->width)
                     ? src->cursor_full_w : (int)src->width;
            int fh = (src->cursor_full_h > 0 && src->cursor_full_h <= (int)src->height)
                     ? src->cursor_full_h : (int)src->height;
            int is_2frame_horiz = (fw * 2 == (int)src->width && fh == (int)src->height);
            int is_2frame_vert  = (fw == (int)src->width && fh * 2 == (int)src->height);
            int frame_sx0 = (is_2frame_horiz && sx0 >= fw) ? fw : 0;
            int frame_sy0 = (is_2frame_vert  && sy0 >= fh) ? fh : 0;
            int cur_dx0 = dx0 - (sx0 - frame_sx0);
            int cur_dy0 = dy0 - (sy0 - frame_sy0);

            g_cursor_drawn_this_frame = 1;
            g_active_cursor_x0 = cur_dx0;
            g_active_cursor_y0 = cur_dy0;
            g_active_cursor_x1 = cur_dx0 + fw;
            g_active_cursor_y1 = cur_dy0 + fh;

            /* Only recalibrate hotspot when an unclipped full frame has a stable offset
               for 2 consecutive frames (immune to fast mouse motion & dirty-rect clips). */
            int is_full_frame = (sw == fw && sh == fh && sx0 == frame_sx0 && sy0 == frame_sy0);
            if (is_full_frame) {
                int hx = g_mouse_x - cur_dx0;
                int hy = g_mouse_y - cur_dy0;
                if (hx >= 0 && hx <= fw && hy >= 0 && hy <= fh) {
                    if (hx == src->cand_hot_x && hy == src->cand_hot_y) {
                        src->cursor_hot_x = hx;
                        src->cursor_hot_y = hy;
                    } else {
                        src->cand_hot_x = hx;
                        src->cand_hot_y = hy;
                    }
                }
            }

            /* Always upload the FULL unclipped frame (frame_sx0, frame_sy0, fw, fh) directly from
               src->pixels_va; upload_cursor_sprite hashes it so unchanged frames are free! */
            upload_cursor_sprite(src, frame_sx0, frame_sy0, fw, fh, use_src_ck, ck_lo, ck_hi,
                                 src->cursor_hot_x, src->cursor_hot_y);

            if (g_cfg.decoupled_cursor && gl_renderer_is_active()) {
                return;
            }
        } else if (src->width <= 256u && src->height <= 256u &&
                   dw >= 20 && dw <= 128 && dh >= 20 && dh <= 128) {
            /* Only track FULL unclipped frames (1-frame, 2-frame horizontal, or 2-frame vertical sheet) */
            int is_1frame       = (sx0 == 0 && sy0 == 0 && sw == (int)src->width && sh == (int)src->height);
            int is_2frame_horiz = (sy0 == 0 && sh == (int)src->height && sw * 2 == (int)src->width && (sx0 == 0 || sx0 == sw));
            int is_2frame_vert  = (sx0 == 0 && sw == (int)src->width && sh * 2 == (int)src->height && (sy0 == 0 || sy0 == sh));
            int is_full_frame   = (is_1frame || is_2frame_horiz || is_2frame_vert);
            int near_mouse = (g_mouse_x >= dx0 - 4 && g_mouse_x <= dx1 + 4 &&
                              g_mouse_y >= dy0 - 4 && g_mouse_y <= dy1 + 4);
            int in_bounds = (dx0 > 0 && dy0 > 0 && dx1 < (int)dst->width && dy1 < (int)dst->height);
            if (is_full_frame && near_mouse && in_bounds) {
                int hx = g_mouse_x - dx0;
                int hy = g_mouse_y - dy0;
                if (src->cursor_track_count == 0) {
                    src->cursor_track_count = 1;
                    src->cursor_hot_x = hx;
                    src->cursor_hot_y = hy;
                    src->cand_hot_x = hx;
                    src->cand_hot_y = hy;
                    src->last_origin_x = dx0;
                    src->last_origin_y = dy0;
                    src->last_mouse_x = g_mouse_x;
                    src->last_mouse_y = g_mouse_y;
                    src->cursor_full_w = sw;
                    src->cursor_full_h = sh;
                } else if (sw == src->cursor_full_w && sh == src->cursor_full_h) {
                    int odx = dx0 - src->last_origin_x;
                    int ody = dy0 - src->last_origin_y;
                    int mdx = g_mouse_x - src->last_mouse_x;
                    int mdy = g_mouse_y - src->last_mouse_y;
                    if ((odx != 0 || ody != 0) && (mdx != 0 || mdy != 0)) {
                        /* Full sprite frame moved identically to the mouse! */
                        if (abs(odx - mdx) <= 2 && abs(ody - mdy) <= 2) {
                            src->cursor_track_count++;
                            src->cursor_hot_x = hx;
                            src->cursor_hot_y = hy;
                            src->cand_hot_x = hx;
                            src->cand_hot_y = hy;
                            if (src->cursor_track_count >= 2) {
                                src->is_cursor = 1;
                                if (g_debug_log) {
                                    fprintf(stderr, "[CURSOR] Locked HiHz cursor (%ux%u, frame=%dx%d, hot=%d,%d)\n",
                                            src->width, src->height, sw, sh, hx, hy);
                                }
                            }
                        } else {
                            src->cursor_track_count = 1;
                        }
                        src->last_origin_x = dx0;
                        src->last_origin_y = dy0;
                        src->last_mouse_x = g_mouse_x;
                        src->last_mouse_y = g_mouse_y;
                    } else if (odx == 0 && ody == 0 && (abs(mdx) >= 2 || abs(mdy) >= 2)) {
                        /* Full frame stayed stationary while mouse moved -> NOT a cursor! */
                        src->cursor_track_count = 0;
                    }
                }
                if (src->is_cursor) {
                    g_cursor_drawn_this_frame = 1;
                    g_active_cursor_x0 = dx0; g_active_cursor_y0 = dy0;
                    g_active_cursor_x1 = dx1; g_active_cursor_y1 = dy1;
                    upload_cursor_sprite(src, sx0, sy0, sw, sh, use_src_ck, ck_lo, ck_hi,
                                         src->cursor_hot_x, src->cursor_hot_y);
                    if (g_cfg.decoupled_cursor && gl_renderer_is_active()) {
                        return;
                    }
                }
            }
        }
    }

    /* ---- v5 Persistent 4K GPU DirectDraw Canvas Mirror ----
     * Every screen blit is mirrored into g_hd_canvas_fbo[0/1] (3200x2400) in exact
     * chronological painter's order:
     *  1) Full 4K backgrounds (or dirty-rect sub-regions of them) blit at 4K with zero
     *     eviction of untouched objects (Portraits, Bonsai, Moorhuhn stay razor-sharp!).
     *  2) Matched 4x sprites (1-frame props, split backgrounds, multi-frame IT3 strips
     *     INCLUDING clipped dirty-rect slices via known_cell_w/h, and Moorhuhn animations)
     *     paint ONLY their exact UV sub-rect in exact Z-order.
     *  3) Unmatched blits (e.g. fntWhite hover text above cursor, tiny clock hands) paint
     *     their exact 1x premultiplied-alpha patch ON TOP in exact Z-order (zero flicker!).
     *  4) 1x software pixels are ALWAYS still written below for savegame thumbnails & F1 toggles. */
    if (to_screen && gl_renderer_is_active()) {
        int dslot = surf_spr_slot(dst);
        if (src->is_primary || src->is_backbuffer) {
            gl_canvas_blit_canvas(dslot, surf_spr_slot(src), (int)dst->width, (int)dst->height,
                                  dx0, dy0, dx1, dy1, sx0, sy0, sx1, sy1);
        } else if (handled_as_full_hd_bg) {
            float u0 = (float)sx0 / (float)src->width;
            float v0 = (float)sy0 / (float)src->height;
            float u1 = (float)sx1 / (float)src->width;
            float v1 = (float)sy1 / (float)src->height;
            gl_canvas_blit_hd_bg(dslot, (int)dst->width, (int)dst->height,
                                 dx0, dy0, dx1, dy1, u0, v0, u1, v1);
        } else {
            int spr_id = -1;
            float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            int hd_ux0 = dx0, hd_uy0 = dy0, hd_fw = dw, hd_fh = dh;

            if (g_cfg.hd_sprites_enabled && hd_sprites_count() > 0) {
                /* Path A: Whole surface matches an indexed 4x asset (1-frame IT3 sprites,
                   surf=0x0 Moorhuhn/character frames, split backgrounds objBgO/objBgU/objWolken,
                   save thumbnails, and JPEG prop/animation strips — works for ANY dirty-rect slice!) */
                if (src->width >= 16 && src->height >= 16) {
                    int whole_id = lookup_surface_sprite(src, 0, 0, (int)src->width, (int)src->height,
                                                         use_src_ck, ck_lo, ck_hi);
                    if (whole_id >= 0) {
                        spr_id = whole_id;
                        u0 = (float)sx0 / (float)src->width;
                        v0 = (float)sy0 / (float)src->height;
                        u1 = (float)sx1 / (float)src->width;
                        v1 = (float)sy1 / (float)src->height;
                        if (dw == sw && dh == sh) {
                            hd_ux0 = dx0 - sx0;
                            hd_uy0 = dy0 - sy0;
                            hd_fw = (int)src->width;
                            hd_fh = (int)src->height;
                        }
                    }
                }
                /* Path B1: Exact full frame inside a multi-frame IT3 strip (e.g. objBonsaiIdle 150x180) */
                if (spr_id < 0 && !(sx0 == 0 && sy0 == 0 && sw == (int)src->width && sh == (int)src->height) &&
                    is_full_sprite_frame(src, sx0, sy0, sw, sh)) {
                    spr_id = lookup_surface_sprite(src, sx0, sy0, sw, sh, use_src_ck, ck_lo, ck_hi);
                    if (spr_id >= 0) {
                        src->known_cell_w = sw;
                        src->known_cell_h = sh;
                        hd_ux0 = dx0;
                        hd_uy0 = dy0;
                        hd_fw = dw;
                        hd_fh = dh;
                    }
                }
                /* Path B1b: Auto-discover strip cell dims if a multi-frame strip's very first blit is clipped
                   (e.g. scrolling objWolken or a dirty-rect-clipped JPEG/IT3 animation strip). */
                if (spr_id < 0 && src->known_cell_w == 0 && sx0 >= 0 && sy0 >= 0) {
                    uint32_t rgen = hd_sprites_get_room_gen();
                    if (src->cell_probe_room_gen != rgen) {
                        src->cell_probe_room_gen = rgen;
                        int cand_w[8], cand_h[8];
                        int ncand = hd_sprites_find_strip_cell((int)src->width, (int)src->height,
                                                               cand_w, cand_h, 8);
                        for (int ci = 0; ci < ncand; ci++) {
                            int cw = cand_w[ci], ch = cand_h[ci];
                            int cx = (sx0 / cw) * cw;
                            int cy = (sy0 / ch) * ch;
                            if (sx1 <= cx + cw && sy1 <= cy + ch &&
                                cx + cw <= (int)src->width && cy + ch <= (int)src->height) {
                                int mid = lookup_surface_sprite(src, cx, cy, cw, ch, use_src_ck, ck_lo, ck_hi);
                                if (mid >= 0) {
                                    src->known_cell_w = cw;
                                    src->known_cell_h = ch;
                                    break;
                                }
                            }
                        }
                    }
                }
                /* Path B2: Clipped dirty-rect slice inside a known multi-frame IT3 strip! */
                if (spr_id < 0 && src->known_cell_w > 0 && src->known_cell_h > 0 && sx0 >= 0 && sy0 >= 0) {
                    int cw = src->known_cell_w;
                    int ch = src->known_cell_h;
                    int cx = (sx0 / cw) * cw;
                    int cy = (sy0 / ch) * ch;
                    if (sx1 <= cx + cw && sy1 <= cy + ch &&
                        cx + cw <= (int)src->width && cy + ch <= (int)src->height) {
                        spr_id = lookup_surface_sprite(src, cx, cy, cw, ch, use_src_ck, ck_lo, ck_hi);
                        if (spr_id >= 0) {
                            u0 = (float)(sx0 - cx) / (float)cw;
                            v0 = (float)(sy0 - cy) / (float)ch;
                            u1 = (float)(sx1 - cx) / (float)cw;
                            v1 = (float)(sy1 - cy) / (float)ch;
                            if (dw == sw && dh == sh) {
                                hd_ux0 = dx0 - (sx0 - cx);
                                hd_uy0 = dy0 - (sy0 - cy);
                                hd_fw = cw;
                                hd_fh = ch;
                            }
                        }
                    }
                }
            }

            if (spr_id >= 0) {
                /* If we matched a split outdoor background layer (objBgO/objBgU/mskBg), lock active room prefix! */
                maybe_lock_split_room_from_sprite(spr_id);
                gl_canvas_blit_hd_sprite(dslot, (int)dst->width, (int)dst->height, spr_id,
                                         dx0, dy0, dx1, dy1, u0, v0, u1, v1, use_src_ck, 1.0f);
                g_last_dd_hd_blit.valid = 1;
                g_last_dd_hd_blit.slot = dslot;
                g_last_dd_hd_blit.ux0 = hd_ux0;
                g_last_dd_hd_blit.uy0 = hd_uy0;
                g_last_dd_hd_blit.fw = hd_fw;
                g_last_dd_hd_blit.fh = hd_fh;
            } else {
                g_last_dd_hd_blit.valid = 0;
                upload_1x_patch_to_canvas(dst, dx0, dy0, dx1, dy1, src, sx0, sy0, sx1, sy1,
                                          use_src_ck, ck_lo, ck_hi);
            }
        }
    }

    /* ---- v3 Native Vector Text Propagation ---- */
    if (!use_src_ck) {
        evict_texts_in_rect(dst, dx0, dy0, dx1, dy1);
        evict_bottom_lines(dst, dx0, dy0, dx1, dy1);
    }
    if (src->num_text_draws > 0) {
        int off_x = dx0 - sx0;
        int off_y = dy0 - sy0;
        for (int i = 0; i < src->num_text_draws; i++) {
            GlTextDraw td = src->text_draws[i];
            td.x += off_x;
            td.y += off_y;
            td.clip_x0 += off_x;
            td.clip_y0 += off_y;
            td.clip_x1 += off_x;
            td.clip_y1 += off_y;
            if (td.clip_x0 < dx0) td.clip_x0 = dx0;
            if (td.clip_y0 < dy0) td.clip_y0 = dy0;
            if (td.clip_x1 > dx1) td.clip_x1 = dx1;
            if (td.clip_y1 > dy1) td.clip_y1 = dy1;
            if (td.clip_x1 > td.clip_x0 && td.clip_y1 > td.clip_y0) {
                ddraw_surface_add_text(dst, &td);
            }
        }
    }

    int same_size = (dw == sw && dh == sh);

    for (int y = 0; y < dh; y++) {
        int dy = dy0 + y;
        if (dy < 0 || dy >= (int)dst->height) continue;
        int sy = same_size ? (sy0 + y) : (sy0 + (y * sh) / dh);
        if (sy < 0 || sy >= (int)src->height) continue;

        uint8_t *drow = g_mem + dst->pixels_va + (uint32_t)dy * dst->pitch;
        const uint8_t *srow = g_mem + src->pixels_va + (uint32_t)sy * src->pitch;

        if (same_size && !use_src_ck && dst->bpp == src->bpp) {
            int cx0 = 0, cx1 = dw;
            if (dx0 + cx0 < 0) cx0 = -dx0;
            if (sx0 + cx0 < 0) cx0 = -sx0;
            if (dx0 + cx1 > (int)dst->width) cx1 = (int)dst->width - dx0;
            if (sx0 + cx1 > (int)src->width) cx1 = (int)src->width - sx0;
            if (cx1 > cx0) {
                uint32_t bps = dst->bpp / 8u;
                memmove(drow + (dx0 + cx0) * bps, srow + (sx0 + cx0) * bps, (size_t)(cx1 - cx0) * bps);
            }
            continue;
        }

        for (int x = 0; x < dw; x++) {
            int dx = dx0 + x;
            if (dx < 0 || dx >= (int)dst->width) continue;
            int sx = same_size ? (sx0 + x) : (sx0 + (x * sw) / dw);
            if (sx < 0 || sx >= (int)src->width) continue;

            if (src->bpp == 16 && dst->bpp == 16) {
                uint16_t px;
                memcpy(&px, srow + sx * 2, 2);
                if (use_src_ck && px >= ck_lo && px <= ck_hi) continue;
                memcpy(drow + dx * 2, &px, 2);
            } else if (src->bpp == 32 && dst->bpp == 32) {
                uint32_t px;
                memcpy(&px, srow + sx * 4, 4);
                if (use_src_ck && (px & 0xFFFFFFu) >= (ck_lo & 0xFFFFFFu) && (px & 0xFFFFFFu) <= (ck_hi & 0xFFFFFFu)) continue;
                memcpy(drow + dx * 4, &px, 4);
            }
        }
    }
}

/* ---- DirectDraw API Imports ---- */
int win32_ddraw_dispatch(CpuCtx *c, uint32_t import_id) {
    switch (import_id) {
        case IMP_DDRAW_DLL_DirectDrawEnumerateA: {
            uint32_t cb_va = arg_u32(c, 0);
            uint32_t ctx_va = arg_u32(c, 1);
            if (cb_va) {
                uint32_t desc_va = guest_alloc_cstr("SDL2 Native Linux Display Driver");
                uint32_t name_va = guest_alloc_cstr("display");
                uint32_t a[4] = { 0, desc_va, name_va, ctx_va };
                guest_call_stdcall(c, cb_va, 4, a);
            }
            stdcall_ret(c, 2, 0); return 1;
        }
        case IMP_DDRAW_DLL_DirectDrawCreate: {
            uint32_t ppdd = arg_u32(c, 1);
            uint32_t obj = com_create_obj(COM_IFACE_DDRAW, 32, 1);
            if (ppdd) mem_w32(ppdd, obj);
            if (g_debug_log) fprintf(stderr, "[DDRAW] DirectDrawCreate -> obj=0x%08x\n", obj);
            stdcall_ret(c, 3, 0); return 1;
        }
    }
    return 0;
}

/* ---- DirectDraw COM Methods ---- */
void com_ddraw_dispatch(CpuCtx *c, uint32_t iface_id, uint32_t slot) {
    uint32_t this_va = arg_u32(c, 0);

    if (iface_id == COM_IFACE_DDRAW) {
        switch (slot) {
            case 0: { /* QueryInterface(this, riid, ppv) */
                uint32_t ppv = arg_u32(c, 2);
                if (ppv) mem_w32(ppv, this_va);
                stdcall_ret(c, 3, 0); return;
            }
            case 1: /* AddRef */
                stdcall_ret(c, 1, 2); return;
            case 2: /* Release */
                stdcall_ret(c, 1, 1); return;
            case 4: { /* CreateClipper(this, dwFlags, lplpDDClipper, pUnkOuter) */
                uint32_t out = arg_u32(c, 2);
                uint32_t clip = com_create_obj(COM_IFACE_DDRAW_CLIPPER, 12, 1);
                if (out) mem_w32(out, clip);
                stdcall_ret(c, 4, 0); return;
            }
            case 6: { /* CreateSurface(this, lpDDSurfaceDesc, lplpDDSurface, pUnkOuter) */
                uint32_t desc_va = arg_u32(c, 1);
                uint32_t out_va  = arg_u32(c, 2);
                uint32_t flags = mem_r32(desc_va + 4);
                uint32_t h = (flags & 0x02u) ? mem_r32(desc_va + 8) : g_dd_height;
                uint32_t w = (flags & 0x04u) ? mem_r32(desc_va + 12) : g_dd_width;
                uint32_t back_cnt = (flags & 0x20u) ? mem_r32(desc_va + 20) : 0;
                uint32_t caps = (flags & 0x01u) ? mem_r32(desc_va + 104) : 0;
                int is_prim = (caps & 0x200u) != 0; /* DDSCAPS_PRIMARYSURFACE */
                uint32_t bpp = g_dd_bpp;
                if (flags & 0x1000u) {
                    uint32_t pbpp = mem_r32(desc_va + 72 + 12);
                    if (pbpp == 16 || pbpp == 32) bpp = pbpp;
                }
                if (is_prim) { w = g_dd_width; h = g_dd_height; bpp = g_dd_bpp; }
                uint32_t sh = alloc_surface(w, h, bpp, caps, is_prim, 0);
                DDrawSurface *s = ddraw_get_surface(sh);
                if (s && (flags & 0x00010000u /* DDSD_CKSRCBLT */)) {
                    s->has_src_colorkey = 1;
                    s->src_ck_low = mem_r32(desc_va + 64);
                    s->src_ck_high = mem_r32(desc_va + 68);
                }
                if (s && is_prim && (back_cnt > 0 || (caps & 0x10u /* DDSCAPS_FLIP */))) {
                    uint32_t bh = alloc_surface(w, h, bpp, 0x04u /* DDSCAPS_BACKBUFFER */, 0, 1);
                    s->attached_handle = bh;
                }
                if (out_va) mem_w32(out_va, s ? s->obj_va : 0);
                if (g_debug_log) fprintf(stderr, "[DDRAW] CreateSurface(%ux%u@%u caps=0x%x prim=%d back=%u) -> h=%u\n", w, h, bpp, caps, is_prim, s ? s->attached_handle : 0, sh);
                stdcall_ret(c, 4, s ? 0u : 0x8007000Eu); return;
            }
            case 8: { /* EnumDisplayModes(this, dwFlags, lpDDSurfaceDesc, lpContext, lpEnumModesCallback) */
                uint32_t ctx_va = arg_u32(c, 3);
                uint32_t cb_va  = arg_u32(c, 4);
                uint32_t pref_bpp = g_cfg.truecolor_32bit ? 32u : 16u;
                g_dd_bpp = pref_bpp;
                const uint32_t modes[][3] = {
                    { 800, 600, pref_bpp },
                    { 640, 480, pref_bpp },
                    { 1024, 768, pref_bpp },
                };
                uint32_t d_va = guest_alloc(128);
                for (size_t i = 0; i < sizeof(modes)/sizeof(modes[0]) && cb_va; i++) {
                    memset(g_mem + d_va, 0, 108);
                    mem_w32(d_va + 0, 108);
                    mem_w32(d_va + 4, 0x100Eu);
                    mem_w32(d_va + 8, modes[i][1]);
                    mem_w32(d_va + 12, modes[i][0]);
                    mem_w32(d_va + 16, modes[i][0] * (modes[i][2] / 8u));
                    fill_pixelformat(d_va + 72, modes[i][2]);
                    uint32_t a[2] = { d_va, ctx_va };
                    if (guest_call_stdcall(c, cb_va, 2, a) == 0) break;
                }
                guest_free(d_va);
                stdcall_ret(c, 5, 0); return;
            }
            case 10: /* FlipToGDISurface(this) */
                stdcall_ret(c, 1, 0); return;
            case 11: { /* GetCaps(this, lpDDDriverCaps, lpDDHELCaps) */
                uint32_t hw = arg_u32(c, 1);
                uint32_t hel = arg_u32(c, 2);
                uint32_t targets[2] = { hw, hel };
                for (int i = 0; i < 2; i++) {
                    if (!targets[i]) continue;
                    uint32_t sz = mem_r32(targets[i]);
                    if (sz > 4 && sz <= 512) memset(g_mem + targets[i] + 4, 0, sz - 4);
                    mem_w32(targets[i] + 4, 0xFFFFFFFFu); /* dwCaps */
                    mem_w32(targets[i] + 8, 0xFFFFFFFFu); /* dwCaps2 */
                    mem_w32(targets[i] + 12, 0xFFFFFFFFu);/* dwCKeyCaps */
                    mem_w32(targets[i] + 16, 0xFFFFFFFFu);/* dwFXCaps */
                    mem_w32(targets[i] + 36, 64u * 1024u * 1024u); /* dwVidMemTotal */
                    mem_w32(targets[i] + 40, 64u * 1024u * 1024u); /* dwVidMemFree */
                }
                stdcall_ret(c, 3, 0); return;
            }
            case 12: { /* GetDisplayMode(this, lpDDSurfaceDesc) */
                uint32_t d = arg_u32(c, 1);
                if (d) {
                    uint32_t sz = mem_r32(d);
                    if (sz < 108) sz = 108;
                    memset(g_mem + d, 0, sz);
                    mem_w32(d + 0, sz);
                    mem_w32(d + 4, 0x100Eu);
                    mem_w32(d + 8, g_dd_height);
                    mem_w32(d + 12, g_dd_width);
                    mem_w32(d + 16, g_dd_width * (g_dd_bpp / 8u));
                    fill_pixelformat(d + 72, g_dd_bpp);
                }
                stdcall_ret(c, 2, 0); return;
            }
            case 19: /* RestoreDisplayMode(this) */
                stdcall_ret(c, 1, 0); return;
            case 20: /* SetCooperativeLevel(this, hWnd, dwFlags) */
                stdcall_ret(c, 3, 0); return;
            case 21: { /* IDirectDraw2::SetDisplayMode(this, w, h, bpp, refresh, flags) */
                g_dd_width = arg_u32(c, 1);
                g_dd_height = arg_u32(c, 2);
                g_dd_bpp = arg_u32(c, 3);
                if (g_dd_width == 0) g_dd_width = 800;
                if (g_dd_height == 0) g_dd_height = 600;
                if (g_dd_bpp != 16 && g_dd_bpp != 32) g_dd_bpp = g_cfg.truecolor_32bit ? 32 : 16;
                g_win_w = (int)g_dd_width;
                g_win_h = (int)g_dd_height;
                if (!gl_renderer_is_active()) {
                    if (g_sdl_window) SDL_SetWindowSize(g_sdl_window, g_win_w, g_win_h);
                    if (g_sdl_renderer) SDL_RenderSetLogicalSize(g_sdl_renderer, g_win_w, g_win_h);
                }
                fprintf(stderr, "[DDRAW] SetDisplayMode(%ux%u @ %ubpp)\n", g_dd_width, g_dd_height, g_dd_bpp);
                stdcall_ret(c, 6, 0); return;
            }
            case 22: /* WaitForVerticalBlank(this, dwFlags, hEvent) */
                stdcall_ret(c, 3, 0); return;
            case 23: { /* GetAvailableVidMem(this, lpDDSCaps, lpdwTotal, lpdwFree) */
                uint32_t tot = arg_u32(c, 2), fr = arg_u32(c, 3);
                if (tot) mem_w32(tot, 64u * 1024u * 1024u);
                if (fr)  mem_w32(fr,  64u * 1024u * 1024u);
                stdcall_ret(c, 4, 0); return;
            }
            case 24: /* GetSurfaceFromDC(this, hdc, lpDDS) */
                stdcall_ret(c, 3, 0x80004001u); return;
            case 25: /* RestoreAllSurfaces(this) */
                stdcall_ret(c, 1, 0); return;
            case 26: /* TestCooperativeLevel(this) */
                stdcall_ret(c, 1, 0); return;
            case 27: { /* GetDeviceIdentifier(this, lpdddi, dwFlags) */
                uint32_t p = arg_u32(c, 1);
                if (p) {
                    memset(g_mem + p, 0, 1068);
                    snprintf((char *)(g_mem + p + 0), 512, "sdl2_ddraw");
                    snprintf((char *)(g_mem + p + 512), 512, "SDL2 Native Linux Display Driver");
                    mem_w32(p + 1032, 0x10DEu);
                    mem_w32(p + 1036, 0x0001u);
                }
                stdcall_ret(c, 3, 0); return;
            }
            default:
                fprintf(stderr, "[DDRAW] Unhandled IDirectDraw slot %u\n", slot);
                stdcall_ret(c, 1, 0); return;
        }
    }

    if (iface_id == COM_IFACE_DDRAW_CLIPPER) {
        switch (slot) {
            case 0: { uint32_t ppv = arg_u32(c, 2); if (ppv) mem_w32(ppv, this_va); stdcall_ret(c, 3, 0); return; }
            case 1: stdcall_ret(c, 1, 2); return;
            case 2: stdcall_ret(c, 1, 0); return;
            case 8: /* SetHWnd(this, dwFlags, hWnd) */ stdcall_ret(c, 3, 0); return;
            default: stdcall_ret(c, 1, 0); return;
        }
    }

    if (iface_id == COM_IFACE_DDRAW_SURFACE) {
        uint32_t h = com_obj_handle(this_va);
        DDrawSurface *s = ddraw_get_surface(h);
        switch (slot) {
            case 0: { /* QueryInterface */
                uint32_t ppv = arg_u32(c, 2);
                if (ppv) mem_w32(ppv, this_va);
                pthread_mutex_lock(&g_dd_mtx);
                if (s && s->in_use) s->refcount++;
                pthread_mutex_unlock(&g_dd_mtx);
                stdcall_ret(c, 3, 0); return;
            }
            case 1: { /* AddRef */
                uint32_t rc = 1u;
                pthread_mutex_lock(&g_dd_mtx);
                if (s && s->in_use) rc = (uint32_t)(++s->refcount);
                pthread_mutex_unlock(&g_dd_mtx);
                stdcall_ret(c, 1, rc); return;
            }
            case 2: /* Release */
                pthread_mutex_lock(&g_dd_mtx);
                if (s && s->in_use) {
                    s->refcount--;
                    if (s->refcount <= 0) {
                        uint32_t pva = s->pixels_va;
                        uint32_t bh = s->attached_handle;
                        uint32_t bpva = 0, bova = 0;
                        s->pixels_va = 0;
                        s->attached_handle = 0;
                        s->in_use = 0;
                        if (bh >= 1 && bh < MAX_DD_SURFACES && g_surfaces[bh].in_use) {
                            DDrawSurface *bs = &g_surfaces[bh];
                            bs->refcount--;
                            if (bs->refcount <= 0) {
                                bpva = bs->pixels_va;
                                bova = bs->obj_va;
                                bs->pixels_va = 0;
                                bs->obj_va = 0;
                                bs->in_use = 0;
                            }
                        }
                        pthread_mutex_unlock(&g_dd_mtx);
                        if (pva) guest_free(pva);
                        if (bpva) guest_free(bpva);
                        if (bova) guest_free(bova);
                        guest_free(this_va);
                        stdcall_ret(c, 1, 0); return;
                    }
                    uint32_t rc = (uint32_t)s->refcount;
                    pthread_mutex_unlock(&g_dd_mtx);
                    stdcall_ret(c, 1, rc); return;
                }
                pthread_mutex_unlock(&g_dd_mtx);
                stdcall_ret(c, 1, 0); return;
            case 5: { /* Blt(this, lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx) */
                uint32_t drc_va = arg_u32(c, 1);
                uint32_t src_obj = arg_u32(c, 2);
                uint32_t src_va = arg_u32(c, 3);
                uint32_t flags = arg_u32(c, 4);
                uint32_t fx_va = arg_u32(c, 5);
                if (s) {
                    int dx0 = 0, dy0 = 0, dx1 = (int)s->width, dy1 = (int)s->height;
                    if (drc_va) {
                        dx0 = (int32_t)mem_r32(drc_va + 0);
                        dy0 = (int32_t)mem_r32(drc_va + 4);
                        dx1 = (int32_t)mem_r32(drc_va + 8);
                        dy1 = (int32_t)mem_r32(drc_va + 12);
                    }
                    if (flags & 0x00000400u /* DDBLT_COLORFILL */) {
                        uint32_t color = fx_va ? mem_r32(fx_va + 80) : 0;
                        surface_colorfill(s, dx0, dy0, dx1, dy1, color);
                    } else if (src_obj) {
                        DDrawSurface *src = ddraw_get_surface(com_obj_handle(src_obj));
                        if (src) {
                            int sx0 = 0, sy0 = 0, sx1 = (int)src->width, sy1 = (int)src->height;
                            if (src_va) {
                                sx0 = (int32_t)mem_r32(src_va + 0);
                                sy0 = (int32_t)mem_r32(src_va + 4);
                                sx1 = (int32_t)mem_r32(src_va + 8);
                                sy1 = (int32_t)mem_r32(src_va + 12);
                            }
                            int use_ck = 0;
                            uint32_t ck_lo = 0, ck_hi = 0;
                            if ((flags & 0x00010000u /* DDBLT_KEYSRCOVERRIDE */) && fx_va) {
                                use_ck = 1;
                                ck_lo = mem_r32(fx_va + 92);
                                ck_hi = mem_r32(fx_va + 96);
                                if (ck_hi < ck_lo) ck_hi = ck_lo;
                            } else if ((flags & 0x00008000u /* DDBLT_KEYSRC */) && src->has_src_colorkey) {
                                use_ck = 1;
                                ck_lo = src->src_ck_low;
                                ck_hi = src->src_ck_high;
                            }
                            surface_blt(s, dx0, dy0, dx1, dy1, src, sx0, sy0, sx1, sy1, use_ck, ck_lo, ck_hi);
                        }
                    }
                    if (s->is_primary) {
                        win32_pump_sdl_events();
                        ddraw_present_surface(s);
                    }
                }
                stdcall_ret(c, 6, 0); return;
            }
            case 7: { /* BltFast(this, dwX, dwY, lpDDSrcSurface, lpSrcRect, dwTrans) */
                int dx0 = arg_i32(c, 1);
                int dy0 = arg_i32(c, 2);
                uint32_t src_obj = arg_u32(c, 3);
                uint32_t src_va = arg_u32(c, 4);
                uint32_t trans = arg_u32(c, 5);
                if (s && src_obj) {
                    DDrawSurface *src = ddraw_get_surface(com_obj_handle(src_obj));
                    if (src) {
                        int sx0 = 0, sy0 = 0, sx1 = (int)src->width, sy1 = (int)src->height;
                        if (src_va) {
                            sx0 = (int32_t)mem_r32(src_va + 0);
                            sy0 = (int32_t)mem_r32(src_va + 4);
                            sx1 = (int32_t)mem_r32(src_va + 8);
                            sy1 = (int32_t)mem_r32(src_va + 12);
                        }
                        int use_ck = ((trans & 1u /* DDBLTFAST_SRCCOLORKEY */) != 0) && src->has_src_colorkey;
                        surface_blt(s, dx0, dy0, dx0 + (sx1 - sx0), dy0 + (sy1 - sy0), src, sx0, sy0, sx1, sy1,
                                    use_ck, src->src_ck_low, src->src_ck_high);
                        if (s->is_primary) {
                            win32_pump_sdl_events();
                            ddraw_present_surface(s);
                        }
                    }
                }
                stdcall_ret(c, 6, 0); return;
            }
            case 11: { /* Flip(this, lpDDSurfaceTargetOverride, dwFlags) */
                if (s && s->attached_handle) {
                    DDrawSurface *back = ddraw_get_surface(s->attached_handle);
                    if (back) {
                        /* Swap backing buffers and attached vector text queues */
                        uint32_t tmp = s->pixels_va;
                        s->pixels_va = back->pixels_va;
                        back->pixels_va = tmp;

                        GlTextDraw tmp_td[MAX_SURFACE_TEXT_DRAWS];
                        int tmp_ntd = s->num_text_draws;
                        if (tmp_ntd > 0) memcpy(tmp_td, s->text_draws, (size_t)tmp_ntd * sizeof(GlTextDraw));
                        s->num_text_draws = back->num_text_draws;
                        if (back->num_text_draws > 0) memcpy(s->text_draws, back->text_draws, (size_t)back->num_text_draws * sizeof(GlTextDraw));
                        back->num_text_draws = tmp_ntd;
                        if (tmp_ntd > 0) memcpy(back->text_draws, tmp_td, (size_t)tmp_ntd * sizeof(GlTextDraw));

                        int tmp_nbl = s->num_bottom_lines;
                        __typeof__(s->bottom_lines) tmp_bl;
                        memcpy(tmp_bl, s->bottom_lines, sizeof(tmp_bl));
                        memcpy(s->bottom_lines, back->bottom_lines, sizeof(tmp_bl));
                        s->num_bottom_lines = back->num_bottom_lines;
                        memcpy(back->bottom_lines, tmp_bl, sizeof(tmp_bl));
                        back->num_bottom_lines = tmp_nbl;

                        /* Swap primary/backbuffer 4K GPU canvases in lockstep with pixel buffers */
                        gl_canvas_flip();

                        win32_pump_sdl_events();
                        ddraw_present_surface(s);
                    }
                } else if (s) {
                    win32_pump_sdl_events();
                    ddraw_present_surface(s);
                }
                stdcall_ret(c, 3, 0); return;
            }
            case 12: { /* GetAttachedSurface(this, lpDDSCaps, lplpDDAttachedSurface) */
                uint32_t out = arg_u32(c, 2);
                if (s && s->attached_handle) {
                    DDrawSurface *back = ddraw_get_surface(s->attached_handle);
                    if (back) {
                        back->refcount++;
                        if (out) mem_w32(out, back->obj_va);
                        stdcall_ret(c, 3, 0); return;
                    }
                }
                if (out) mem_w32(out, 0);
                stdcall_ret(c, 3, 0x887600FFu); return;
            }
            case 15: { /* GetCaps(this, lpDDSCaps) */
                uint32_t caps_va = arg_u32(c, 1);
                if (caps_va && s) mem_w32(caps_va, s->caps);
                stdcall_ret(c, 2, 0); return;
            }
            case 17: { /* GetDC(this, lphDC) */
                uint32_t out = arg_u32(c, 1);
                uint32_t hdc = gdi_create_surface_dc(h);
                if (out) mem_w32(out, hdc);
                stdcall_ret(c, 2, hdc ? 0u : 0x80004005u); return;
            }
            case 21: { /* GetPixelFormat(this, lpDDPixelFormat) */
                uint32_t pf = arg_u32(c, 1);
                if (pf && s) fill_pixelformat(pf, s->bpp);
                stdcall_ret(c, 2, 0); return;
            }
            case 22: { /* GetSurfaceDesc(this, lpDDSurfaceDesc) */
                uint32_t desc = arg_u32(c, 1);
                if (desc && s) fill_surf_desc(desc, s, 0);
                stdcall_ret(c, 2, 0); return;
            }
            case 24: /* IsLost(this) */
                stdcall_ret(c, 1, 0); return;
            case 25: { /* Lock(this, lpDestRect, lpDDSurfaceDesc, dwFlags, hEvent) */
                uint32_t drc = arg_u32(c, 1);
                uint32_t desc = arg_u32(c, 2);
                if (desc && s) {
                    if ((s->is_primary || s->is_backbuffer) && s->pixels_va && gl_renderer_is_active()) {
                        int slot = surf_spr_slot(s);
                        size_t need = (size_t)s->pitch * (size_t)s->height;
                        if (need > g_lock_snap_sz[slot]) {
                            uint8_t *nb = (uint8_t *)realloc(g_lock_snap[slot], need);
                            if (nb) {
                                g_lock_snap[slot] = nb;
                                g_lock_snap_sz[slot] = need;
                            }
                        }
                        if (g_lock_snap[slot] && g_lock_snap_sz[slot] >= need) {
                            memcpy(g_lock_snap[slot], g_mem + s->pixels_va, need);
                            g_lock_snap_valid[slot] = 1;
                        }
                    }
                    fill_surf_desc(desc, s, 1);
                    if (drc) {
                        int x0 = (int32_t)mem_r32(drc + 0);
                        int y0 = (int32_t)mem_r32(drc + 4);
                        uint32_t ptr = s->pixels_va + (uint32_t)y0 * s->pitch + (uint32_t)x0 * (s->bpp / 8u);
                        mem_w32(desc + 36, ptr);
                    }
                }
                stdcall_ret(c, 5, 0); return;
            }
            case 26: { /* ReleaseDC(this, hDC) */
                gdi_release_surface_dc(arg_u32(c, 1));
                if (s && s->is_primary) ddraw_present_surface(s);
                stdcall_ret(c, 2, 0); return;
            }
            case 27: /* Restore(this) */
                stdcall_ret(c, 1, 0); return;
            case 28: /* SetClipper(this, lpDDClipper) */
                stdcall_ret(c, 2, 0); return;
            case 29: { /* SetColorKey(this, dwFlags, lpDDColorKey) */
                uint32_t flags = arg_u32(c, 1);
                uint32_t ck_va = arg_u32(c, 2);
                if (s && (flags & 8u /* DDCKEY_SRCBLT */)) {
                    if (ck_va) {
                        s->has_src_colorkey = 1;
                        s->src_ck_low = mem_r32(ck_va + 0);
                        s->src_ck_high = mem_r32(ck_va + 4);
                    } else {
                        s->has_src_colorkey = 0;
                    }
                    s->sprite_cache_count = 0;
                    s->known_cell_w = 0;
                    s->known_cell_h = 0;
                    s->cell_probe_room_gen = 0;
                }
                stdcall_ret(c, 3, 0); return;
            }
            case 32: /* Unlock(this, lpSurfaceData) */
                if (s) {
                    if (!s->is_primary && !s->is_backbuffer) {
                        s->hd_asset_id = -2; /* re-check HD match on next Blt */
                        s->sprite_cache_count = 0;
                        s->known_cell_w = 0;
                        s->known_cell_h = 0;
                        s->cell_probe_room_gen = 0;
                        s->num_text_draws = 0;
                    } else {
                        if (g_font_line.active) record_bottom_line(s, g_font_line.x, g_font_line.y, g_font_line.h);
                        sync_screen_lock_diff_to_canvas(s);
                        if (s->is_primary) {
                            win32_pump_sdl_events();
                            ddraw_present_surface(s);
                        }
                    }
                }
                stdcall_ret(c, 2, 0); return;
            default:
                fprintf(stderr, "[DDRAW] Unhandled IDirectDrawSurface slot %u\n", slot);
                stdcall_ret(c, 1, 0); return;
        }
    }
}

/* ============================================================================
 * Native 4K GPU Hooks for mha2.exe's Software IT3 RLE Sprite Blitters
 * (Intercepts Moorhuhn s/ animations, surf=0x0 character animations, scaled,
 *  mirrored, and alpha-blended IT3 sprites drawn directly into locked buffers)
 * ============================================================================ */

#define RLE_MATCH_CACHE_SIZE 1024
typedef struct RleMatchCacheEntry {
    uint32_t frame_va;
    uint32_t data_va;
    uint32_t data_sig;
    uint32_t room_gen;
    int cell_w;
    int cell_h;
    int sprite_id;
} RleMatchCacheEntry;

static RleMatchCacheEntry g_rle_cache[RLE_MATCH_CACHE_SIZE];
static uint8_t *g_rle_tmp_rgba = NULL;
static size_t g_rle_tmp_cap = 0;

static DDrawSurface *find_screen_surface_by_pixels(uint32_t pixels_va, int *out_ox, int *out_oy) {
    if (!pixels_va) return NULL;
    for (uint32_t i = 1; i < MAX_DD_SURFACES; i++) {
        DDrawSurface *s = &g_surfaces[i];
        if (!s->in_use || (!s->is_primary && !s->is_backbuffer) || !s->pixels_va) continue;
        uint32_t sz = s->pitch * s->height;
        if (pixels_va >= s->pixels_va && pixels_va < s->pixels_va + sz) {
            uint32_t off = pixels_va - s->pixels_va;
            int bps = (int)(s->bpp / 8u);
            if (out_oy) *out_oy = (s->pitch > 0) ? (int)(off / s->pitch) : 0;
            if (out_ox) *out_ox = (s->pitch > 0 && bps > 0) ? (int)((off % s->pitch) / (uint32_t)bps) : 0;
            return s;
        }
    }
    return NULL;
}

static int decode_rle_frame_into_rgba(uint32_t frame_va, int cell_w, int cell_h, uint8_t *rgba) {
    if (!frame_va || frame_va >= GUEST_MEM_SIZE - 0x30u) return 0;
    uint32_t rle_w = mem_r32(frame_va + 0x00u);
    uint32_t rle_h = mem_r32(frame_va + 0x04u);
    uint32_t bpp   = mem_r32(frame_va + 0x14u);
    uint32_t has_a = mem_r32(frame_va + 0x18u);
    uint32_t data_va = mem_r32(frame_va + 0x2cu);
    if (rle_w == 0 || rle_h == 0 || data_va < GUEST_HEAP_BASE || data_va >= GUEST_MEM_SIZE - 32u) return 0;
    if (bpp != 2u && bpp != 3u && bpp != 4u) return 0;

    uint32_t pos = data_va;
    int x = 0, y = 0;
    int max_y = (int)rle_h < cell_h ? (int)rle_h : cell_h;
    while (pos < GUEST_MEM_SIZE - 8u && y < max_y) {
        uint8_t b = g_mem[pos++];
        if (b == 0) break;
        if ((b & 0x80u) == 0) {
            x += (int)b;
        } else {
            int cnt = (int)(b & 0x7Fu);
            if (cnt == 0) {
                y++;
                x = 0;
                continue;
            }
            for (int i = 0; i < cnt; i++) {
                uint8_t r = 0, g = 0, bl = 0, a = 255;
                if (bpp == 4u) {
                    if (pos + 4u >= GUEST_MEM_SIZE) { y = max_y; break; }
                    bl = g_mem[pos + 0];
                    g  = g_mem[pos + 1];
                    r  = g_mem[pos + 2];
                    pos += 4u;
                } else if (bpp == 3u) {
                    if (pos + 3u >= GUEST_MEM_SIZE) { y = max_y; break; }
                    bl = g_mem[pos + 0];
                    g  = g_mem[pos + 1];
                    r  = g_mem[pos + 2];
                    pos += 3u;
                } else {
                    if (pos + 2u >= GUEST_MEM_SIZE) { y = max_y; break; }
                    uint16_t p16 = mem_r16(pos);
                    pos += 2u;
                    r  = (uint8_t)(((p16 >> 11) & 0x1Fu) * 255u / 31u);
                    g  = (uint8_t)(((p16 >> 5) & 0x3Fu) * 255u / 63u);
                    bl = (uint8_t)((p16 & 0x1Fu) * 255u / 31u);
                }
                if (has_a) {
                    if (pos >= GUEST_MEM_SIZE) { y = max_y; break; }
                    a = g_mem[pos++];
                }
                if (x >= 0 && x < cell_w && y >= 0 && y < cell_h) {
                    size_t di = ((size_t)y * (size_t)cell_w + (size_t)x) * 4u;
                    rgba[di + 0] = r;
                    rgba[di + 1] = g;
                    rgba[di + 2] = bl;
                    rgba[di + 3] = a;
                }
                x++;
            }
        }
    }
    return 1;
}

static int lookup_rle_both_sprite(uint32_t tbl1_va, uint32_t tbl2_va, int cell_w, int cell_h) {
    if (hd_sprites_count() <= 0 || cell_w < 16 || cell_h < 16 || cell_w > 2048 || cell_h > 2048) return -1;
    uint32_t primary_va = tbl1_va ? tbl1_va : tbl2_va;
    if (!primary_va || primary_va >= GUEST_MEM_SIZE - 0x30u) return -1;

    uint32_t rle_w = mem_r32(primary_va + 0x00u);
    uint32_t rle_h = mem_r32(primary_va + 0x04u);
    uint32_t bpp   = mem_r32(primary_va + 0x14u);
    uint32_t data_va = mem_r32(primary_va + 0x2cu);
    if (rle_w == 0 || rle_h == 0 || data_va < GUEST_HEAP_BASE || data_va >= GUEST_MEM_SIZE - 32u) return -1;
    if (bpp != 2u && bpp != 3u && bpp != 4u) return -1;

    uint32_t sig = rle_w * 73856093u ^ rle_h * 19349663u ^ bpp * 83492791u ^ (tbl2_va * 2654435761u);
    for (uint32_t k = 0; k < 16u; k++) {
        sig ^= (uint32_t)g_mem[data_va + k];
        sig *= 16777619u;
    }

    uint32_t rgen = hd_sprites_get_room_gen();
    uint32_t hidx = ((primary_va >> 3) ^ (data_va >> 4) ^ (uint32_t)(cell_w * 31 + cell_h)) & (RLE_MATCH_CACHE_SIZE - 1u);
    RleMatchCacheEntry *ce = &g_rle_cache[hidx];
    if (ce->frame_va == primary_va && ce->data_va == data_va && ce->data_sig == sig &&
        ce->cell_w == cell_w && ce->cell_h == cell_h && ce->room_gen == rgen) {
        return ce->sprite_id;
    }

    size_t need = (size_t)cell_w * (size_t)cell_h * 4u;
    if (need > g_rle_tmp_cap) {
        uint8_t *nb = (uint8_t *)realloc(g_rle_tmp_rgba, need);
        if (!nb) return -1;
        g_rle_tmp_rgba = nb;
        g_rle_tmp_cap = need;
    }
    memset(g_rle_tmp_rgba, 0, need);

    int ok = 0;
    if (tbl1_va) ok |= decode_rle_frame_into_rgba(tbl1_va, cell_w, cell_h, g_rle_tmp_rgba);
    if (tbl2_va) ok |= decode_rle_frame_into_rgba(tbl2_va, cell_w, cell_h, g_rle_tmp_rgba);
    if (!ok) return -1;

    int spr_id = hd_sprites_match_rgba(g_rle_tmp_rgba, cell_w, cell_h);
    ce->frame_va = primary_va;
    ce->data_va = data_va;
    ce->data_sig = sig;
    ce->room_gen = rgen;
    ce->cell_w = cell_w;
    ce->cell_h = cell_h;
    ce->sprite_id = spr_id;
    return spr_id;
}

static int lookup_rle_frame_sprite(uint32_t frame_va, int cell_w, int cell_h) {
    return lookup_rle_both_sprite(frame_va, 0, cell_w, cell_h);
}

static void sync_rect_into_lock_snap(DDrawSurface *s, int x0, int y0, int x1, int y1, int upload_diff_first) {
    int slot = surf_spr_slot(s);
    if (!g_lock_snap_valid[slot] || !g_lock_snap[slot]) return;
    int W = (int)s->width, H = (int)s->height;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x1 <= x0 || y1 <= y0) return;

    int bps = (int)(s->bpp / 8u);
    int bw = x1 - x0, bh = y1 - y0;
    const uint8_t *cur_base = g_mem + s->pixels_va;
    uint8_t *old_base = g_lock_snap[slot];

    if (upload_diff_first) {
        uint32_t *patch = ensure_patch_rgba_buf((size_t)bw * (size_t)bh);
        if (patch) {
            int any = 0;
            for (int y = 0; y < bh; y++) {
                const uint8_t *crow = cur_base + (size_t)(y0 + y) * s->pitch;
                const uint8_t *orow = old_base + (size_t)(y0 + y) * s->pitch;
                uint32_t *drow = patch + (size_t)y * (size_t)bw;
                for (int x = 0; x < bw; x++) {
                    int sx = x0 + x;
                    if (memcmp(crow + sx * bps, orow + sx * bps, (size_t)bps) == 0) {
                        drow[x] = 0;
                    } else {
                        any = 1;
                        if (s->bpp == 32) {
                            uint32_t px;
                            memcpy(&px, crow + sx * 4, 4);
                            uint8_t r = (uint8_t)((px >> 16) & 0xFFu);
                            uint8_t g = (uint8_t)((px >> 8) & 0xFFu);
                            uint8_t b = (uint8_t)(px & 0xFFu);
                            drow[x] = (0xFFu << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
                        } else {
                            uint16_t px;
                            memcpy(&px, crow + sx * 2, 2);
                            uint8_t r = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
                            uint8_t g = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
                            uint8_t b = (uint8_t)((px & 0x1Fu) * 255u / 31u);
                            drow[x] = (0xFFu << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
                        }
                    }
                }
            }
            if (any) {
                gl_canvas_blit_rgba_patch(slot, W, H, patch, bw, bh, x0, y0, x1, y1, 1);
            }
        }
    }

    /* Update snapshot so Unlock() never overwrites this rect with 1x software pixels */
    size_t row_bytes = (size_t)bw * (size_t)bps;
    for (int y = y0; y < y1; y++) {
        memcpy(old_base + (size_t)y * s->pitch + (size_t)x0 * (size_t)bps,
               cur_base + (size_t)y * s->pitch + (size_t)x0 * (size_t)bps,
               row_bytes);
    }
}

static RecompFn g_orig_rle_fn[8];
static RecompFn g_orig_rle_both_fn = NULL;
static int g_in_rle_both = 0;
static const uint32_t g_rle_addrs[8] = {
    0x0048dba0u, 0x0048e260u,
    0x0048fc10u, 0x004902d0u,
    0x00491c70u, 0x00492310u,
    0x00493dd0u, 0x00494480u,
};

/* Master hook on Blitter::BlitBoth (0x00494b80, __thiscall(desc, tbl1, tbl2)):
   1) When tbl1 != NULL (e.g. ALL of Moorhuhn s/001..s/008 & surf=0x0 animations),
      matches tbl1+tbl2 together ONCE against our composited 4K HD sprite and renders
      in one clean 4K pass — eliminating the blurry 1x tbl2 dashed border completely!
   2) When tbl1 == NULL && tbl2 != NULL (DirectDraw-backed IT3 sprites & ALL 5 cursors
      whose tbl1 was already drawn via BltFast), suppresses the redundant blurry 1x tbl2
      ring whenever tbl1 was already drawn as our decoupled 4K cursor or 4K HD sprite! */
static void hook_rle_both(CpuCtx *c) {
    if (!g_orig_rle_both_fn) return;
    if (g_font_ctx.active && g_cfg.hd_vector_text) {
        g_orig_rle_both_fn(c);
        return;
    }

    uint32_t desc_va = mem_r32(c->esp + 4u);
    uint32_t tbl1_va = mem_r32(c->esp + 8u);
    uint32_t tbl2_va = mem_r32(c->esp + 12u);

    uint32_t dst_pix_va = 0, clip_va = 0;
    int dst_x = 0, dst_y = 0, cell_w = 0, cell_h = 0;
    int flip_x = 0, flip_y = 0;
    double scale = 1.0;
    uint32_t alpha_u = 256u;
    int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;
    int valid_desc = 0;
    if (desc_va && desc_va < GUEST_MEM_SIZE - 0x60u && (tbl1_va || tbl2_va)) {
        dst_pix_va = mem_r32(desc_va + 0x00u);
        dst_x      = (int32_t)mem_r32(desc_va + 0x08u);
        dst_y      = (int32_t)mem_r32(desc_va + 0x0cu);
        cell_w     = (int32_t)mem_r32(desc_va + 0x10u);
        cell_h     = (int32_t)mem_r32(desc_va + 0x14u);
        clip_va    = mem_r32(desc_va + 0x18u);
        flip_x     = (int)g_mem[desc_va + 0x20u];
        flip_y     = (int)g_mem[desc_va + 0x21u];
        scale      = mem_rf64(desc_va + 0x28u);
        int raw_a  = (int32_t)mem_r32(desc_va + 0x30u);
        alpha_u    = (raw_a > 0 && raw_a < 255) ? (uint32_t)raw_a : 256u;
        if (clip_va && clip_va < GUEST_MEM_SIZE - 16u) {
            clip_x0 = (int32_t)mem_r32(clip_va + 0u);
            clip_y0 = (int32_t)mem_r32(clip_va + 4u);
            clip_x1 = (int32_t)mem_r32(clip_va + 8u);
            clip_y1 = (int32_t)mem_r32(clip_va + 12u);
        }
        valid_desc = (scale >= 0.001 && cell_w > 0 && cell_h > 0);
    }

    int prev_in = g_in_rle_both;
    g_in_rle_both = 1;
    g_orig_rle_both_fn(c);
    g_in_rle_both = prev_in;

    if (!valid_desc || !gl_renderer_is_active()) return;
    int base_ox = 0, base_oy = 0;
    DDrawSurface *s = find_screen_surface_by_pixels(dst_pix_va, &base_ox, &base_oy);
    if (!s) return;

    int scaled_w = (scale == 1.0) ? cell_w : (int)((double)cell_w * scale + 0.5);
    int scaled_h = (scale == 1.0) ? cell_h : (int)((double)cell_h * scale + 0.5);
    if (scaled_w <= 0 || scaled_h <= 0) return;

    int ux0 = base_ox + dst_x;
    int uy0 = base_oy + dst_y;
    int dx0 = ux0, dy0 = uy0;
    int dx1 = ux0 + scaled_w, dy1 = uy0 + scaled_h;
    if (clip_va) {
        int cx0 = base_ox + clip_x0, cy0 = base_oy + clip_y0;
        int cx1 = base_ox + clip_x1, cy1 = base_oy + clip_y1;
        if (dx0 < cx0) dx0 = cx0;
        if (dy0 < cy0) dy0 = cy0;
        if (dx1 > cx1) dx1 = cx1;
        if (dy1 > cy1) dy1 = cy1;
    }
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > (int)s->width) dx1 = (int)s->width;
    if (dy1 > (int)s->height) dy1 = (int)s->height;
    if (dx1 <= dx0 || dy1 <= dy0) return;

    int slot = surf_spr_slot(s);

    /* Case A: tbl1 was already drawn via DirectDraw BltFast (cursor or HD prop) ->
       suppress the redundant 1x tbl2 semi-transparent ring so it never stamps blurry
       1x pixels onto the 4K canvas! */
    if (!tbl1_va && tbl2_va) {
        if (g_cfg.decoupled_cursor && g_cursor_drawn_this_frame &&
            ux0 == g_active_cursor_x0 && uy0 == g_active_cursor_y0 &&
            scaled_w == (g_active_cursor_x1 - g_active_cursor_x0) &&
            scaled_h == (g_active_cursor_y1 - g_active_cursor_y0)) {
            sync_rect_into_lock_snap(s, dx0, dy0, dx1, dy1, 0);
            return;
        }
        if (g_last_dd_hd_blit.valid && g_last_dd_hd_blit.slot == slot &&
            abs(ux0 - g_last_dd_hd_blit.ux0) <= 1 && abs(uy0 - g_last_dd_hd_blit.uy0) <= 1 &&
            abs(scaled_w - g_last_dd_hd_blit.fw) <= 1 && abs(scaled_h - g_last_dd_hd_blit.fh) <= 1) {
            g_last_dd_hd_blit.valid = 0;
            sync_rect_into_lock_snap(s, dx0, dy0, dx1, dy1, 0);
            return;
        }
    }

    int spr_id = (g_cfg.hd_sprites_enabled && hd_sprites_count() > 0)
                 ? lookup_rle_both_sprite(tbl1_va, tbl2_va, cell_w, cell_h) : -1;
    if (spr_id >= 0) {
        maybe_lock_split_room_from_sprite(spr_id);
        float fx0 = (float)(dx0 - ux0) / (float)scaled_w;
        float fy0 = (float)(dy0 - uy0) / (float)scaled_h;
        float fx1 = (float)(dx1 - ux0) / (float)scaled_w;
        float fy1 = (float)(dy1 - uy0) / (float)scaled_h;
        float u0 = flip_x ? (1.0f - fx0) : fx0;
        float u1 = flip_x ? (1.0f - fx1) : fx1;
        float v0 = flip_y ? (1.0f - fy0) : fy0;
        float v1 = flip_y ? (1.0f - fy1) : fy1;
        float alpha_f = (alpha_u >= 256u) ? 1.0f : ((float)alpha_u / 256.0f);
        gl_canvas_blit_hd_sprite(slot, (int)s->width, (int)s->height, spr_id,
                                 dx0, dy0, dx1, dy1, u0, v0, u1, v1, 1, alpha_f);
        sync_rect_into_lock_snap(s, dx0, dy0, dx1, dy1, 0);
    } else {
        sync_rect_into_lock_snap(s, dx0, dy0, dx1, dy1, 1);
    }
}

static void hook_rle_common(CpuCtx *c, int idx, int is_alpha) {
    /* When inside BlitBoth (0x00494b80) or Font::DrawText (0x00486c40), let the software
       blitter update 1x RAM only — the outer hook handles the single clean 4K pass! */
    if (g_in_rle_both || (g_font_ctx.active && g_cfg.hd_vector_text)) {
        g_orig_rle_fn[idx](c);
        return;
    }

    uint32_t desc_va  = mem_r32(c->esp + 4u);
    uint32_t frame_va = mem_r32(c->esp + 8u);
    uint32_t alpha_u  = is_alpha ? mem_r32(c->esp + 12u) : 256u;

    /* Capture descriptor fields BEFORE orig_fn runs (0x48d7c0 may normalize scale to 1.0) */
    uint32_t dst_pix_va = 0, clip_va = 0;
    int dst_x = 0, dst_y = 0, cell_w = 0, cell_h = 0;
    int flip_x = 0, flip_y = 0;
    double scale = 1.0;
    int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;
    int valid_desc = 0;
    if (desc_va && desc_va < GUEST_MEM_SIZE - 0x60u && frame_va && frame_va < GUEST_MEM_SIZE - 0x30u) {
        dst_pix_va = mem_r32(desc_va + 0x00u);
        dst_x      = (int32_t)mem_r32(desc_va + 0x08u);
        dst_y      = (int32_t)mem_r32(desc_va + 0x0cu);
        cell_w     = (int32_t)mem_r32(desc_va + 0x10u);
        cell_h     = (int32_t)mem_r32(desc_va + 0x14u);
        clip_va    = mem_r32(desc_va + 0x18u);
        flip_x     = (int)g_mem[desc_va + 0x20u];
        flip_y     = (int)g_mem[desc_va + 0x21u];
        scale      = mem_rf64(desc_va + 0x28u);
        if (clip_va && clip_va < GUEST_MEM_SIZE - 16u) {
            clip_x0 = (int32_t)mem_r32(clip_va + 0u);
            clip_y0 = (int32_t)mem_r32(clip_va + 4u);
            clip_x1 = (int32_t)mem_r32(clip_va + 8u);
            clip_y1 = (int32_t)mem_r32(clip_va + 12u);
        }
        valid_desc = (scale >= 0.001 && cell_w > 0 && cell_h > 0);
    }

    /* Always execute the original software RLE blitter so 1x RAM stays 100% accurate */
    g_orig_rle_fn[idx](c);

    if (!valid_desc || alpha_u == 0 || !gl_renderer_is_active()) return;
    int base_ox = 0, base_oy = 0;
    DDrawSurface *s = find_screen_surface_by_pixels(dst_pix_va, &base_ox, &base_oy);
    if (!s) return;

    int scaled_w = (scale == 1.0) ? cell_w : (int)((double)cell_w * scale + 0.5);
    int scaled_h = (scale == 1.0) ? cell_h : (int)((double)cell_h * scale + 0.5);
    if (scaled_w <= 0 || scaled_h <= 0) return;

    int ux0 = base_ox + dst_x;
    int uy0 = base_oy + dst_y;
    int dx0 = ux0, dy0 = uy0;
    int dx1 = ux0 + scaled_w, dy1 = uy0 + scaled_h;
    if (clip_va) {
        int cx0 = base_ox + clip_x0, cy0 = base_oy + clip_y0;
        int cx1 = base_ox + clip_x1, cy1 = base_oy + clip_y1;
        if (dx0 < cx0) dx0 = cx0;
        if (dy0 < cy0) dy0 = cy0;
        if (dx1 > cx1) dx1 = cx1;
        if (dy1 > cy1) dy1 = cy1;
    }
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > (int)s->width) dx1 = (int)s->width;
    if (dy1 > (int)s->height) dy1 = (int)s->height;
    if (dx1 <= dx0 || dy1 <= dy0) return;

    int spr_id = (g_cfg.hd_sprites_enabled && hd_sprites_count() > 0)
                 ? lookup_rle_frame_sprite(frame_va, cell_w, cell_h) : -1;
    if (spr_id >= 0) {
        maybe_lock_split_room_from_sprite(spr_id);
        float fx0 = (float)(dx0 - ux0) / (float)scaled_w;
        float fy0 = (float)(dy0 - uy0) / (float)scaled_h;
        float fx1 = (float)(dx1 - ux0) / (float)scaled_w;
        float fy1 = (float)(dy1 - uy0) / (float)scaled_h;
        float u0 = flip_x ? (1.0f - fx0) : fx0;
        float u1 = flip_x ? (1.0f - fx1) : fx1;
        float v0 = flip_y ? (1.0f - fy0) : fy0;
        float v1 = flip_y ? (1.0f - fy1) : fy1;
        float alpha_f = (alpha_u >= 256u) ? 1.0f : ((float)alpha_u / 256.0f);
        gl_canvas_blit_hd_sprite(surf_spr_slot(s), (int)s->width, (int)s->height, spr_id,
                                 dx0, dy0, dx1, dy1, u0, v0, u1, v1, 1, alpha_f);
        sync_rect_into_lock_snap(s, dx0, dy0, dx1, dy1, 0);
    } else {
        sync_rect_into_lock_snap(s, dx0, dy0, dx1, dy1, 1);
    }
}

static void hook_rle_0(CpuCtx *c) { hook_rle_common(c, 0, 0); }
static void hook_rle_1(CpuCtx *c) { hook_rle_common(c, 1, 1); }
static void hook_rle_2(CpuCtx *c) { hook_rle_common(c, 2, 0); }
static void hook_rle_3(CpuCtx *c) { hook_rle_common(c, 3, 1); }
static void hook_rle_4(CpuCtx *c) { hook_rle_common(c, 4, 0); }
static void hook_rle_5(CpuCtx *c) { hook_rle_common(c, 5, 1); }
static void hook_rle_6(CpuCtx *c) { hook_rle_common(c, 6, 0); }
static void hook_rle_7(CpuCtx *c) { hook_rle_common(c, 7, 1); }

static RecompFn g_orig_font_draw_fn = NULL;

static void font_cp1252_to_utf8(const uint8_t *src, int len, char *dst, size_t dst_sz,
                                int *out_s0, int *out_s1) {
    int s0 = 0, s1 = len;
    while (s0 < s1 && src[s0] == ' ') s0++;
    while (s1 > s0 && (src[s1 - 1] == ' ' || src[s1 - 1] == '\0')) s1--;
    if (out_s0) *out_s0 = s0;
    if (out_s1) *out_s1 = s1;
    size_t j = 0;
    for (int i = s0; i < s1 && j + 4 < dst_sz; i++) {
        uint8_t ch = src[i];
        if (ch == 0) break;
        if (ch < 0x80u) {
            dst[j++] = (char)ch;
        } else {
            uint32_t cp = ch;
            if (ch == 0x80u) cp = 0x20ACu;
            else if (ch == 0x84u) cp = 0x201Eu;
            else if (ch == 0x85u) cp = 0x2026u;
            else if (ch == 0x91u) cp = 0x2018u;
            else if (ch == 0x92u) cp = 0x2019u;
            else if (ch == 0x93u) cp = 0x201Cu;
            else if (ch == 0x94u) cp = 0x201Du;
            else if (ch == 0x96u) cp = 0x2013u;
            else if (ch == 0x97u) cp = 0x2014u;
            if (cp < 0x800u) {
                dst[j++] = (char)(0xC0u | (cp >> 6));
                dst[j++] = (char)(0x80u | (cp & 0x3Fu));
            } else {
                dst[j++] = (char)(0xE0u | (cp >> 12));
                dst[j++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                dst[j++] = (char)(0x80u | (cp & 0x3Fu));
            }
        }
    }
    dst[j] = '\0';
}

static int font_line_height(uint32_t font_va) {
    int line_h = 16;
    if (font_va && font_va < GUEST_MEM_SIZE - 0x24u) {
        uint32_t fhdr_va = mem_r32(font_va + 0x1cu);
        if (fhdr_va && fhdr_va < GUEST_MEM_SIZE - 0x18u) {
            int h = (int32_t)mem_r32(fhdr_va + 0x04u);
            if (h >= 10 && h <= 64) line_h = h;
        }
    }
    return line_h;
}

static void hook_font_draw_text_impl(CpuCtx *c);

/* Font::DrawText(this=font, clip, str, x, y, len): remember the line for the screen-surface
   Unlock that writes it (dialog choice detection), with HD vector text on or off. */
static void hook_font_draw_text(CpuCtx *c) {
    if (!g_orig_font_draw_fn) return;
    FontLineCtx prev = g_font_line;
    g_font_line.active = 1;
    g_font_line.x = (int32_t)mem_r32(c->esp + 16u);
    g_font_line.y = (int32_t)mem_r32(c->esp + 20u);
    g_font_line.h = font_line_height(c->ecx);
    hook_font_draw_text_impl(c);
    g_font_line = prev;
}

static void hook_font_draw_text_impl(CpuCtx *c) {
    if (!g_cfg.hd_vector_text || !gl_renderer_is_active()) {
        g_orig_font_draw_fn(c);
        return;
    }

    uint32_t font_va = c->ecx;
    uint32_t clip_va = mem_r32(c->esp + 8u);
    uint32_t str_va  = mem_r32(c->esp + 12u);
    int draw_x       = (int32_t)mem_r32(c->esp + 16u);
    int draw_y       = (int32_t)mem_r32(c->esp + 20u);
    int str_len      = (int32_t)mem_r32(c->esp + 24u);

    if (!str_va || str_len <= 0 || str_va >= GUEST_MEM_SIZE - 4u) {
        g_orig_font_draw_fn(c);
        return;
    }
    if (str_len > 240) str_len = 240;

    int s0 = 0, s1 = 0;
    char utf8[256];
    font_cp1252_to_utf8(g_mem + str_va, str_len, utf8, sizeof(utf8), &s0, &s1);
    if (!utf8[0] || s1 <= s0) {
        g_orig_font_draw_fn(c);
        return;
    }

    int line_h = font_line_height(font_va);

    /* Call Font::MeasureText (0x00486ad0) on a scratch CpuCtx copy to get the exact,
       dirty-rect-invariant unclipped pixel span [text_x0 .. text_x0 + text_w]! */
    int lead_w = 0, text_w = 0;
    if (font_va && g_code_table[0x00486ad0u - GUEST_TEXT_START]) {
        if (s0 > 0) {
            CpuCtx tmp = *c;
            tmp.ecx = font_va;
            uint32_t a[2] = { str_va, (uint32_t)s0 };
            lead_w = (int)guest_call_stdcall(&tmp, 0x00486ad0u, 2, a);
        }
        CpuCtx tmp = *c;
        tmp.ecx = font_va;
        uint32_t a[2] = { str_va + (uint32_t)s0, (uint32_t)(s1 - s0) };
        text_w = (int)guest_call_stdcall(&tmp, 0x00486ad0u, 2, a);
    }
    if (text_w <= 0) {
        g_orig_font_draw_fn(c);
        return;
    }

    int cx0 = 0, cy0 = 0, cx1 = (int)g_dd_width, cy1 = (int)g_dd_height;
    if (clip_va && clip_va < GUEST_MEM_SIZE - 16u) {
        cx0 = (int32_t)mem_r32(clip_va + 0u);
        cy0 = (int32_t)mem_r32(clip_va + 4u);
        cx1 = (int32_t)mem_r32(clip_va + 8u);
        cy1 = (int32_t)mem_r32(clip_va + 12u);
    }

    FontDrawCtx prev = g_font_ctx;
    g_font_ctx.active = 1;
    g_font_ctx.font_va = font_va;
    g_font_ctx.text_x0 = draw_x + lead_w;
    g_font_ctx.text_y0 = draw_y;
    g_font_ctx.text_w = text_w;
    g_font_ctx.text_h = line_h;
    g_font_ctx.clip_x0 = cx0;
    g_font_ctx.clip_y0 = cy0;
    g_font_ctx.clip_x1 = cx1;
    g_font_ctx.clip_y1 = cy1;
    snprintf(g_font_ctx.utf8, sizeof(g_font_ctx.utf8), "%s", utf8);

    g_orig_font_draw_fn(c);

    g_font_ctx = prev;
}

void ddraw_install_rle_hooks(void) {
    static const RecompFn hooks[8] = {
        hook_rle_0, hook_rle_1, hook_rle_2, hook_rle_3,
        hook_rle_4, hook_rle_5, hook_rle_6, hook_rle_7,
    };
    for (int i = 0; i < 8; i++) {
        uint32_t off = g_rle_addrs[i] - GUEST_TEXT_START;
        g_orig_rle_fn[i] = g_code_table[off];
        if (g_orig_rle_fn[i]) {
            g_code_table[off] = hooks[i];
        }
    }
    uint32_t both_off = 0x00494b80u - GUEST_TEXT_START;
    g_orig_rle_both_fn = g_code_table[both_off];
    if (g_orig_rle_both_fn) {
        g_code_table[both_off] = hook_rle_both;
    }
    uint32_t font_off = 0x00486c40u - GUEST_TEXT_START;
    g_orig_font_draw_fn = g_code_table[font_off];
    if (g_orig_font_draw_fn) {
        g_code_table[font_off] = hook_font_draw_text;
    }
}
