#ifndef MHA2_HD_ASSETS_H
#define MHA2_HD_ASSETS_H

#include <stdint.h>

#define HD_THUMB_W 16
#define HD_THUMB_H 12
#define HD_THUMB_BYTES (HD_THUMB_W * HD_THUMB_H * 3)

typedef struct HdImageRGBA {
    uint8_t *rgba;
    int width;
    int height;
} HdImageRGBA;

/* Scan hd_dir recursively for .png/.jpg/.jpeg replacement assets.
 * Returns number of assets indexed. */
int hd_assets_init(const char *hd_dir);
void hd_assets_shutdown(void);
int hd_assets_count(void);

/* Match an 800x600 (or full-frame) guest surface against indexed HD assets.
 * Returns asset index >= 0 on match, or -1 if no match. */
int hd_assets_match_pixels(const void *pixels, int w, int h, int pitch, int bpp);

/* Load full-resolution RGBA pixels for asset index (cached LRU in memory).
 * Returns pointer valid until next hd_assets_get_rgba/shutdown call, or NULL. */
const HdImageRGBA *hd_assets_get_rgba(int asset_id);

/* Return relative asset name (e.g. "x/001/img/objBg.png") for HUD/toast display. */
const char *hd_assets_get_name(int asset_id);
/* Absolute path of the HD file (e.g. <hd_dir>/m/017/img/objBg.webp), "" if invalid */
const char *hd_assets_get_path(int asset_id);

/* Decode optional 16:9 widescreen extended sibling (*_wide.webp / *_wide.png) for 4:3 menus.
 * Returns 0 on success (caller frees via hd_sprites_free_rgba), or -1 if none exists. */
int hd_assets_decode_wide_rgba(int asset_id, HdImageRGBA *out);

/* ---- Native HD Foreground Sprites, Characters, Inventory & Cursors (hd_assets/sprites/) ---- */
int hd_sprites_count(void);
void hd_sprites_set_room_prefix(const char *bg_rel_name);
/* Active room prefix derived from the last HD background (e.g. "l/006/"), "" if none. */
const char *hd_sprites_room_prefix(void);
uint32_t hd_sprites_get_room_gen(void);

/* Match a color-keyed sprite frame rect inside a guest DirectDraw surface against indexed HD sprites.
 * Returns sprite index >= 0 on match, or -1 if no match. */
int hd_sprites_match_rect(const void *pixels, int surf_w, int surf_h, int pitch, int bpp,
                          int sx0, int sy0, int sw, int sh,
                          int use_ck, uint32_t ck_lo, uint32_t ck_hi);

/* Match a decoded 1x RGBA buffer (e.g. from the engine's software IT3 RLE blitter) against indexed HD sprites. */
int hd_sprites_match_rgba(const uint8_t *rgba, int w, int h);

/* Decode full-resolution RGBA pixels for a sprite index (returns newly allocated buffer; caller frees
 * via hd_sprites_free_rgba, or NULL on error). */
int hd_sprites_decode_rgba(int sprite_id, HdImageRGBA *out);
void hd_sprites_free_rgba(HdImageRGBA *img);
const char *hd_sprites_get_name(int sprite_id);
void hd_sprites_get_orig_dims(int sprite_id, int *out_w, int *out_h);

/* Discover distinct candidate frame cell sizes (cw, ch) for a multi-frame strip surface
 * from active-room indexed sprites (returns count stored in out_cw/out_ch, <= max_out). */
int hd_sprites_find_strip_cell(int surf_w, int surf_h, int *out_cw, int *out_ch, int max_out);

#endif /* MHA2_HD_ASSETS_H */
