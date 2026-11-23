#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hd_assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#define MAX_HD_ASSETS 512
#define MAX_CACHE_ENTRIES 1024
#define HD_CACHE_MAGIC 0x31434448u /* "HDC1" */

typedef struct HdCacheDiskEntry {
    char rel_name[512];
    int64_t mtime;
    int64_t fsize;
    int32_t orig_w;
    int32_t orig_h;
    uint8_t thumb[HD_THUMB_BYTES];
} HdCacheDiskEntry;

static HdCacheDiskEntry *g_disk_cache = NULL;
static int g_disk_cache_count = 0;
static HdCacheDiskEntry *g_new_cache = NULL;
static int g_new_cache_count = 0;
static int g_cache_dirty = 0;

static const HdCacheDiskEntry *find_in_disk_cache(const char *rel, int64_t mtime, int64_t fsize) {
    for (int i = 0; i < g_disk_cache_count; i++) {
        if (g_disk_cache[i].mtime == mtime && g_disk_cache[i].fsize == fsize &&
            strcmp(g_disk_cache[i].rel_name, rel) == 0) {
            return &g_disk_cache[i];
        }
    }
    return NULL;
}

static void load_disk_cache(const char *hd_dir) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/.hd_index.cache", hd_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint32_t magic = 0, count = 0;
    if (fread(&magic, 4, 1, f) == 1 && fread(&count, 4, 1, f) == 1 &&
        magic == HD_CACHE_MAGIC && count > 0 && count <= MAX_CACHE_ENTRIES) {
        g_disk_cache = (HdCacheDiskEntry *)malloc(count * sizeof(HdCacheDiskEntry));
        if (g_disk_cache && fread(g_disk_cache, sizeof(HdCacheDiskEntry), count, f) == count) {
            g_disk_cache_count = (int)count;
        } else {
            free(g_disk_cache);
            g_disk_cache = NULL;
        }
    }
    fclose(f);
}

static void save_disk_cache(const char *hd_dir) {
    if (!g_new_cache || g_new_cache_count <= 0) return;
    char path[1100];
    snprintf(path, sizeof(path), "%s/.hd_index.cache", hd_dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t magic = HD_CACHE_MAGIC;
    uint32_t count = (uint32_t)g_new_cache_count;
    fwrite(&magic, 4, 1, f);
    fwrite(&count, 4, 1, f);
    fwrite(g_new_cache, sizeof(HdCacheDiskEntry), count, f);
    fclose(f);
}

typedef struct HdAssetEntry {
    char full_path[1024];
    char rel_name[512];
    int orig_w;
    int orig_h;
    uint8_t thumb[HD_THUMB_BYTES];
    int thumb_valid;
    int thumb_luma_mean;
} HdAssetEntry;

static HdAssetEntry g_assets[MAX_HD_ASSETS];
static int g_asset_count = 0;

/* Single active decoded full-res RGBA cache (avoids holding hundreds of 4K PNGs in RAM) */
static int g_cached_id = -1;
static HdImageRGBA g_cached_img = { NULL, 0, 0 };

static int has_image_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return (strcasecmp(dot, ".png") == 0 ||
            strcasecmp(dot, ".webp") == 0 ||
            strcasecmp(dot, ".jpg") == 0 ||
            strcasecmp(dot, ".jpeg") == 0 ||
            strcasecmp(dot, ".bmp") == 0);
}

/* Decode an image file using FFmpeg libavformat+libavcodec+libswscale into RGBA
 * (if out_w/out_h == 0, keeps native resolution; else scales to out_w x out_h). */
static uint8_t *decode_image_ffmpeg(const char *path, int dst_w, int dst_h,
                                    enum AVPixelFormat dst_fmt, int *out_w, int *out_h) {
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) != 0) return NULL;
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return NULL;
    }

    int vidx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vidx = (int)i;
            break;
        }
    }
    if (vidx < 0) {
        avformat_close_input(&fmt);
        return NULL;
    }

    const AVCodec *codec = avcodec_find_decoder(fmt->streams[vidx]->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmt);
        return NULL;
    }
    AVCodecContext *cc = avcodec_alloc_context3(codec);
    if (!cc || avcodec_parameters_to_context(cc, fmt->streams[vidx]->codecpar) < 0 ||
        avcodec_open2(cc, codec, NULL) < 0) {
        if (cc) avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return NULL;
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();
    uint8_t  *out_buf = NULL;

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vidx) {
            if (avcodec_send_packet(cc, pkt) == 0 && avcodec_receive_frame(cc, frm) == 0) {
                int tw = (dst_w > 0) ? dst_w : frm->width;
                int th = (dst_h > 0) ? dst_h : frm->height;
                int bpp = (dst_fmt == AV_PIX_FMT_RGBA) ? 4 : 3;
                out_buf = (uint8_t *)malloc((size_t)tw * (size_t)th * (size_t)bpp);
                if (out_buf) {
                    struct SwsContext *sws = sws_getContext(
                        frm->width, frm->height, (enum AVPixelFormat)frm->format,
                        tw, th, dst_fmt,
                        (dst_w > 0) ? SWS_AREA : SWS_LANCZOS, NULL, NULL, NULL);
                    if (sws) {
                        uint8_t *dst_data[4] = { out_buf, NULL, NULL, NULL };
                        int dst_linesize[4] = { tw * bpp, 0, 0, 0 };
                        sws_scale(sws, (const uint8_t *const *)frm->data, frm->linesize,
                                  0, frm->height, dst_data, dst_linesize);
                        sws_freeContext(sws);
                        if (out_w) *out_w = frm->width;
                        if (out_h) *out_h = frm->height;
                    } else {
                        free(out_buf);
                        out_buf = NULL;
                    }
                }
                av_packet_unref(pkt);
                break;
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    return out_buf;
}

static void scan_dir_recursive(const char *root_dir, const char *rel_prefix) {
    char cur_dir[1024];
    if (rel_prefix && rel_prefix[0]) {
        snprintf(cur_dir, sizeof(cur_dir), "%s/%s", root_dir, rel_prefix);
    } else {
        snprintf(cur_dir, sizeof(cur_dir), "%s", root_dir);
    }

    DIR *d = opendir(cur_dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_asset_count < MAX_HD_ASSETS) {
        if (ent->d_name[0] == '.') continue;
        char full[1024], rel[512];
        int fn = snprintf(full, sizeof(full), "%s/%s", cur_dir, ent->d_name);
        if (fn < 0 || fn >= (int)sizeof(full)) continue;
        int rn;
        if (rel_prefix && rel_prefix[0]) {
            rn = snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, ent->d_name);
        } else {
            rn = snprintf(rel, sizeof(rel), "%s", ent->d_name);
        }
        if (rn < 0 || rn >= (int)sizeof(rel)) continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if ((!rel_prefix || !rel_prefix[0]) &&
                (strcmp(ent->d_name, "sprites") == 0 || strcmp(ent->d_name, "videos") == 0)) {
                continue;
            }
            scan_dir_recursive(root_dir, rel);
        } else if (S_ISREG(st.st_mode) && has_image_ext(ent->d_name)) {
            /* Skip optional 16:9 widescreen wing extensions (*_wide.webp / *_wide.png) from 4:3 matching */
            if (strstr(ent->d_name, "_wide.") != NULL) continue;
            int64_t mtime = (int64_t)st.st_mtime;
            int64_t fsize = (int64_t)st.st_size;
            int nw = 0, nh = 0;
            uint8_t thumb_buf[HD_THUMB_BYTES];
            const HdCacheDiskEntry *ce = find_in_disk_cache(rel, mtime, fsize);
            if (ce) {
                nw = ce->orig_w;
                nh = ce->orig_h;
                memcpy(thumb_buf, ce->thumb, HD_THUMB_BYTES);
            } else {
                uint8_t *thumb = decode_image_ffmpeg(full, HD_THUMB_W, HD_THUMB_H, AV_PIX_FMT_RGB24, &nw, &nh);
                if (!thumb) continue;
                memcpy(thumb_buf, thumb, HD_THUMB_BYTES);
                free(thumb);
                g_cache_dirty = 1;
            }

            if (g_new_cache && g_new_cache_count < MAX_CACHE_ENTRIES) {
                HdCacheDiskEntry *ne = &g_new_cache[g_new_cache_count++];
                memset(ne, 0, sizeof(*ne));
                snprintf(ne->rel_name, sizeof(ne->rel_name), "%s", rel);
                ne->mtime = mtime;
                ne->fsize = fsize;
                ne->orig_w = nw;
                ne->orig_h = nh;
                memcpy(ne->thumb, thumb_buf, HD_THUMB_BYTES);
            }

            /* Index both 16:9 gameplay rooms (>= 800x450) and 4:3 menus (>= 800x600) */
            if (nw < 800 || nh < 450) {
                continue;
            }

            HdAssetEntry *e = &g_assets[g_asset_count++];
            snprintf(e->full_path, sizeof(e->full_path), "%s", full);
            snprintf(e->rel_name, sizeof(e->rel_name), "%s", rel);
            e->orig_w = nw;
            e->orig_h = nh;
            memcpy(e->thumb, thumb_buf, HD_THUMB_BYTES);
            e->thumb_valid = 1;
            int sum = 0;
            for (int i = 0; i < HD_THUMB_BYTES; i++) sum += thumb_buf[i];
            e->thumb_luma_mean = sum / HD_THUMB_BYTES;
        }
    }
    closedir(d);
}

static void hd_sprites_init(const char *hd_dir);
static void hd_sprites_shutdown(void);

int hd_assets_init(const char *hd_dir) {
    hd_assets_shutdown();
    if (!hd_dir || !hd_dir[0]) return 0;
    struct stat st;
    if (stat(hd_dir, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;

    av_log_set_level(AV_LOG_QUIET);
    g_disk_cache = NULL;
    g_disk_cache_count = 0;
    g_new_cache = (HdCacheDiskEntry *)malloc(MAX_CACHE_ENTRIES * sizeof(HdCacheDiskEntry));
    g_new_cache_count = 0;
    g_cache_dirty = 0;

    load_disk_cache(hd_dir);
    scan_dir_recursive(hd_dir, "");
    if (g_cache_dirty || g_new_cache_count != g_disk_cache_count) {
        save_disk_cache(hd_dir);
    }
    free(g_disk_cache); g_disk_cache = NULL; g_disk_cache_count = 0;
    free(g_new_cache);  g_new_cache = NULL;  g_new_cache_count = 0;

    if (g_asset_count > 0) {
        fprintf(stderr, "[HD] Indexed %d HD background replacement(s) from '%s'%s\n",
                g_asset_count, hd_dir, g_cache_dirty ? " (cache updated)" : " (cached)");
    }
    hd_sprites_init(hd_dir);
    return g_asset_count;
}

void hd_assets_shutdown(void) {
    hd_sprites_shutdown();
    if (g_cached_img.rgba) {
        free(g_cached_img.rgba);
        g_cached_img.rgba = NULL;
    }
    g_cached_img.width = 0;
    g_cached_img.height = 0;
    g_cached_id = -1;
    g_asset_count = 0;
}

int hd_assets_count(void) {
    return g_asset_count;
}

/* Compute fast 16x12 area-sampled RGB thumbnail of a guest surface (takes ~10 us). */
static void compute_surface_thumb(const void *pixels, int w, int h, int pitch, int bpp,
                                  uint8_t out_thumb[HD_THUMB_BYTES], int *out_mean) {
    const uint8_t *base = (const uint8_t *)pixels;
    int total_sum = 0;
    for (int ty = 0; ty < HD_THUMB_H; ty++) {
        int y0 = (ty * h) / HD_THUMB_H;
        int y1 = ((ty + 1) * h) / HD_THUMB_H;
        int ystep = (y1 - y0) > 8 ? ((y1 - y0) / 4) : 1;
        for (int tx = 0; tx < HD_THUMB_W; tx++) {
            int x0 = (tx * w) / HD_THUMB_W;
            int x1 = ((tx + 1) * w) / HD_THUMB_W;
            int xstep = (x1 - x0) > 8 ? ((x1 - x0) / 4) : 1;
            int sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int y = y0; y < y1; y += ystep) {
                const uint8_t *row = base + y * pitch;
                for (int x = x0; x < x1; x += xstep) {
                    if (bpp == 32) {
                        uint32_t px;
                        memcpy(&px, row + x * 4, 4);
                        sr += (int)((px >> 16) & 0xFFu);
                        sg += (int)((px >> 8) & 0xFFu);
                        sb += (int)(px & 0xFFu);
                    } else {
                        uint16_t px;
                        memcpy(&px, row + x * 2, 2);
                        sr += (int)(((px >> 11) & 0x1Fu) * 255u / 31u);
                        sg += (int)(((px >> 5) & 0x3Fu) * 255u / 63u);
                        sb += (int)((px & 0x1Fu) * 255u / 31u);
                    }
                    cnt++;
                }
            }
            if (cnt <= 0) cnt = 1;
            int idx = (ty * HD_THUMB_W + tx) * 3;
            uint8_t r = (uint8_t)(sr / cnt);
            uint8_t g = (uint8_t)(sg / cnt);
            uint8_t b = (uint8_t)(sb / cnt);
            out_thumb[idx + 0] = r;
            out_thumb[idx + 1] = g;
            out_thumb[idx + 2] = b;
            total_sum += r + g + b;
        }
    }
    if (out_mean) *out_mean = total_sum / HD_THUMB_BYTES;
}

int hd_assets_match_pixels(const void *pixels, int w, int h, int pitch, int bpp) {
    if (g_asset_count <= 0 || !pixels || w < 640 || h < 440 || (bpp != 16 && bpp != 32)) {
        return -1;
    }
    uint8_t thumb[HD_THUMB_BYTES];
    int mean = 0;
    compute_surface_thumb(pixels, w, h, pitch, bpp, thumb, &mean);
    /* Reject all-black / blank surfaces */
    if (mean < 5) return -1;

    float src_aspect = (float)w / (float)h;
    int best_id = -1;
    int best_sad = 1 << 30;
    for (int i = 0; i < g_asset_count; i++) {
        const HdAssetEntry *e = &g_assets[i];
        if (!e->thumb_valid || e->orig_h <= 0) continue;
        float e_aspect = (float)e->orig_w / (float)e->orig_h;
        float diff = src_aspect - e_aspect;
        if (diff < -0.08f || diff > 0.08f) continue;
        int sad = 0;
        for (int k = 0; k < HD_THUMB_BYTES; k++) {
            int d = (int)thumb[k] - (int)e->thumb[k];
            sad += (d < 0) ? -d : d;
        }
        if (sad < best_sad) {
            best_sad = sad;
            best_id = i;
        }
    }

    /* Require tight per-channel average error (< 14 / 255) */
    if (best_id >= 0 && (best_sad / HD_THUMB_BYTES) < 14) {
        return best_id;
    }
    return -1;
}

const HdImageRGBA *hd_assets_get_rgba(int asset_id) {
    if (asset_id < 0 || asset_id >= g_asset_count) return NULL;
    if (g_cached_id == asset_id && g_cached_img.rgba) {
        return &g_cached_img;
    }
    if (g_cached_img.rgba) {
        free(g_cached_img.rgba);
        g_cached_img.rgba = NULL;
    }
    g_cached_id = -1;

    const HdAssetEntry *e = &g_assets[asset_id];
    int nw = 0, nh = 0;
    uint8_t *rgba = decode_image_ffmpeg(e->full_path, 0, 0, AV_PIX_FMT_RGBA, &nw, &nh);
    if (!rgba) return NULL;

    g_cached_img.rgba = rgba;
    g_cached_img.width = nw;
    g_cached_img.height = nh;
    g_cached_id = asset_id;
    fprintf(stderr, "[HD] Activated HD background '%s' (%dx%d)\n", e->rel_name, nw, nh);
    return &g_cached_img;
}

const char *hd_assets_get_name(int asset_id) {
    if (asset_id < 0 || asset_id >= g_asset_count) return "";
    return g_assets[asset_id].rel_name;
}

int hd_assets_decode_wide_rgba(int asset_id, HdImageRGBA *out) {
    if (!out || asset_id < 0 || asset_id >= g_asset_count) return -1;
    const char *base = g_assets[asset_id].full_path;
    const char *dot = strrchr(base, '.');
    if (!dot) return -1;
    size_t stem_len = (size_t)(dot - base);
    if (stem_len + 16 >= 1024) return -1;
    static const char *exts[] = { "webp", "png", "jpg" };
    struct stat st;
    for (int i = 0; i < 3; i++) {
        char wpath[1024];
        snprintf(wpath, sizeof(wpath), "%.*s_wide.%s", (int)stem_len, base, exts[i]);
        if (stat(wpath, &st) == 0 && S_ISREG(st.st_mode)) {
            int nw = 0, nh = 0;
            uint8_t *rgba = decode_image_ffmpeg(wpath, 0, 0, AV_PIX_FMT_RGBA, &nw, &nh);
            if (!rgba || nw <= 0 || nh <= 0) {
                free(rgba);
                return -1;
            }
            out->rgba = rgba;
            out->width = nw;
            out->height = nh;
            return 0;
        }
    }
    return -1;
}

/* ============================================================================
 * Native HD Foreground Sprites, Characters, Inventory & Cursors Indexer
 * ============================================================================ */

#define MAX_HD_SPRITES     16384
#define HD_SPR_TW          24
#define HD_SPR_TH          24
#define HD_SPR_TBYTES      (HD_SPR_TW * HD_SPR_TH * 4) /* 2304 bytes RGBA */
#define HD_SPR_CACHE_MAGIC 0x35534448u /* "HDS5" */
#define HD_SPR_BUCKETS     2048

typedef struct HdSprCacheDiskEntry {
    char rel_name[256];
    int64_t mtime;
    int64_t fsize;
    int32_t hd_w;
    int32_t hd_h;
    uint8_t thumb[HD_SPR_TBYTES];
} HdSprCacheDiskEntry;

typedef struct HdSpriteEntry {
    char full_path[512];
    char rel_name[256];
    int hd_w;
    int hd_h;
    uint8_t thumb[HD_SPR_TBYTES];
    int next_in_bucket;
} HdSpriteEntry;

static HdSpriteEntry *g_sprites = NULL;
static int g_sprite_count = 0;
static int g_spr_buckets[HD_SPR_BUCKETS];
static char g_spr_room_prefix[64] = "";
static size_t g_spr_room_prefix_len = 0;
static uint32_t g_spr_room_gen = 1;

/* ---- Background Room-Sprite Pre-Decoder Thread (eliminates first-frame micro-stutter) ---- */
#define PREDEC_SLOTS 96
typedef struct PredecEntry {
    int sprite_id;
    uint8_t *rgba;
    int w, h;
} PredecEntry;

static PredecEntry     g_predec[PREDEC_SLOTS];
static pthread_mutex_t g_predec_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_predec_cv  = PTHREAD_COND_INITIALIZER;
static pthread_t       g_predec_thr;
static int             g_predec_running = 0;
static int             g_predec_stop    = 0;
static uint32_t        g_predec_target_gen = 0;
static char            g_predec_target_prefix[64] = "";

static void predec_clear_locked(void) {
    for (int i = 0; i < PREDEC_SLOTS; i++) {
        free(g_predec[i].rgba);
        g_predec[i].rgba = NULL;
        g_predec[i].sprite_id = -1;
        g_predec[i].w = g_predec[i].h = 0;
    }
}

extern void runtime_unpin_current_thread(void);

static void *predec_worker_main(void *arg) {
    (void)arg;
    runtime_unpin_current_thread();
    uint32_t last_gen = 0;
    for (;;) {
        char pref[64];
        uint32_t cur_gen;
        pthread_mutex_lock(&g_predec_mtx);
        while (!g_predec_stop && g_predec_target_gen == last_gen) {
            pthread_cond_wait(&g_predec_cv, &g_predec_mtx);
        }
        if (g_predec_stop) {
            pthread_mutex_unlock(&g_predec_mtx);
            break;
        }
        cur_gen = g_predec_target_gen;
        snprintf(pref, sizeof(pref), "%s", g_predec_target_prefix);
        predec_clear_locked();
        pthread_mutex_unlock(&g_predec_mtx);

        last_gen = cur_gen;
        size_t plen = strlen(pref);
        if (plen == 0 || !g_sprites || g_sprite_count <= 0) continue;

        int slot = 0;
        for (int i = 0; i < g_sprite_count && slot < PREDEC_SLOTS; i++) {
            pthread_mutex_lock(&g_predec_mtx);
            int abort_now = (g_predec_stop || g_predec_target_gen != cur_gen);
            pthread_mutex_unlock(&g_predec_mtx);
            if (abort_now) break;

            if (strncmp(g_sprites[i].rel_name, pref, plen) != 0) continue;
            if (g_sprites[i].hd_w > 4096 || g_sprites[i].hd_h > 4096) continue;
            int nw = 0, nh = 0;
            uint8_t *rgba = decode_image_ffmpeg(g_sprites[i].full_path, 0, 0, AV_PIX_FMT_RGBA, &nw, &nh);
            if (!rgba) continue;

            pthread_mutex_lock(&g_predec_mtx);
            if (g_predec_stop || g_predec_target_gen != cur_gen) {
                free(rgba);
                pthread_mutex_unlock(&g_predec_mtx);
                break;
            }
            g_predec[slot].sprite_id = i;
            g_predec[slot].rgba = rgba;
            g_predec[slot].w = nw;
            g_predec[slot].h = nh;
            slot++;
            pthread_mutex_unlock(&g_predec_mtx);
        }
    }
    return NULL;
}

void hd_sprites_set_room_prefix(const char *bg_rel_name) {
    char next_prefix[64] = "";
    if (bg_rel_name && bg_rel_name[0]) {
        /* Extract "<dir>/<room>/" from e.g. "l/006/img/objBg.png" -> "l/006/" */
        const char *s1 = strchr(bg_rel_name, '/');
        if (s1) {
            const char *s2 = strchr(s1 + 1, '/');
            if (s2) {
                size_t len = (size_t)(s2 - bg_rel_name + 1);
                if (len < sizeof(next_prefix)) {
                    memcpy(next_prefix, bg_rel_name, len);
                    next_prefix[len] = '\0';
                }
            }
        }
    }
    if (strcmp(g_spr_room_prefix, next_prefix) != 0) {
        snprintf(g_spr_room_prefix, sizeof(g_spr_room_prefix), "%s", next_prefix);
        g_spr_room_prefix_len = strlen(g_spr_room_prefix);
        g_spr_room_gen++;
        if (g_predec_running) {
            pthread_mutex_lock(&g_predec_mtx);
            snprintf(g_predec_target_prefix, sizeof(g_predec_target_prefix), "%s", next_prefix);
            g_predec_target_gen = g_spr_room_gen;
            pthread_cond_signal(&g_predec_cv);
            pthread_mutex_unlock(&g_predec_mtx);
        }
    }
}

uint32_t hd_sprites_get_room_gen(void) {
    return g_spr_room_gen;
}

static int sprite_allowed_in_active_room(const char *rel_name) {
    if (!rel_name) return 0;
    /* Global UI/cursors/inventory (g/) and Moorhuhn character animations (s/) are allowed in EVERY room! */
    if ((rel_name[0] == 'g' || rel_name[0] == 's') && rel_name[1] == '/') return 1;
    /* Split-background outdoor room layers (l/011, l/014, l/022, l/023, l/026, l/027, l/028, l/036)
       have no single 800x450 objBg surface, so they must ALWAYS be matchable across rooms — even when
       entering directly from another room whose prefix is still locked. */
    if (strstr(rel_name, "/objBgO") != NULL ||
        strstr(rel_name, "/objBgU") != NULL ||
        strstr(rel_name, "/mskBg.") != NULL ||
        strstr(rel_name, "/objWolken") != NULL) {
        return 1;
    }
    /* Room-specific sprites belong to the active room prefix (e.g. "l/006/", "x/001/") */
    if (g_spr_room_prefix_len > 0) {
        if (strncmp(rel_name, g_spr_room_prefix, g_spr_room_prefix_len) == 0) {
            return 1;
        }
        /* Starting rooms l/001, l/002, l/003, l/004 all share l/001/img/xobjBg.png! */
        if (strcmp(g_spr_room_prefix, "l/001/") == 0 &&
            (strncmp(rel_name, "l/002/", 6) == 0 ||
             strncmp(rel_name, "l/003/", 6) == 0 ||
             strncmp(rel_name, "l/004/", 6) == 0)) {
            return 1;
        }
        return 0;
    }
    return 0;
}

/* Fast hash lookup for sprite disk cache during startup */
static HdSprCacheDiskEntry *g_spr_dcache = NULL;
static int g_spr_dcache_count = 0;
static int *g_spr_dcache_head = NULL;
static int *g_spr_dcache_next = NULL;
static HdSprCacheDiskEntry *g_spr_ncache = NULL;
static int g_spr_ncache_count = 0;
static int g_spr_cache_dirty = 0;

static uint32_t hash_str_u32(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;
    }
    return h;
}

static uint32_t hash_dims_u32(int w, int h) {
    uint32_t k = ((uint32_t)w * 2654435761u) ^ ((uint32_t)h * 40503u);
    return k & (HD_SPR_BUCKETS - 1);
}

static void compute_rgba_sprite_thumb(const uint8_t *rgba, int w, int h, uint8_t out[HD_SPR_TBYTES]) {
    /* Detect whether the sprite has an opaque tbl1 core (alpha >= 245).
       If yes, measure coverage & RGB on the opaque core so DirectDraw tbl1-only surfaces
       and composited tbl1+tbl2 4K HD PNGs produce identical thumbnails!
       If no (pure semi-transparent tbl2-only sprite like ghosts/glows), fall back to alpha >= 48. */
    int has_core = 0;
    int sy_step = h > 64 ? (h / 32) : 1;
    int sx_step = w > 64 ? (w / 32) : 1;
    for (int y = 0; y < h && !has_core; y += sy_step) {
        const uint8_t *row = rgba + (size_t)y * (size_t)w * 4u;
        for (int x = 0; x < w; x += sx_step) {
            if (row[(size_t)x * 4u + 3u] >= 245u) {
                has_core = 1;
                break;
            }
        }
    }
    /* Use 160u core threshold (instead of 245u) so 1px 4K Gaussian-AA silhouette rims on
       non-tbl2 props (invAbacus, invToteFledermaus, objVase, xobjKerze, objPortrait) stay
       matched to their 1-bit 1x originals while still rejecting low-alpha tbl2 edge halos. */
    uint8_t a_thr = has_core ? 160u : 48u;

    for (int ty = 0; ty < HD_SPR_TH; ty++) {
        int y0 = (ty * h) / HD_SPR_TH;
        int y1 = ((ty + 1) * h) / HD_SPR_TH;
        if (y1 <= y0) y1 = y0 + 1;
        int ystep = (y1 - y0) > 32 ? ((y1 - y0) / 16) : 1;
        for (int tx = 0; tx < HD_SPR_TW; tx++) {
            int x0 = (tx * w) / HD_SPR_TW;
            int x1 = ((tx + 1) * w) / HD_SPR_TW;
            if (x1 <= x0) x1 = x0 + 1;
            int xstep = (x1 - x0) > 32 ? ((x1 - x0) / 16) : 1;
            int sr = 0, sg = 0, sb = 0, ocnt = 0, acnt = 0, tcnt = 0;
            for (int y = y0; y < y1; y += ystep) {
                int cy = (y < h) ? y : (h - 1);
                const uint8_t *row = rgba + (size_t)cy * (size_t)w * 4u;
                for (int x = x0; x < x1; x += xstep) {
                    int cx = (x < w) ? x : (w - 1);
                    const uint8_t *p = row + (size_t)cx * 4u;
                    tcnt++;
                    if (p[3] >= a_thr) {
                        acnt++;
                        sr += p[0];
                        sg += p[1];
                        sb += p[2];
                        ocnt++;
                    }
                }
            }
            int idx = (ty * HD_SPR_TW + tx) * 4;
            if (ocnt > 0) {
                out[idx + 0] = (uint8_t)(sr / ocnt);
                out[idx + 1] = (uint8_t)(sg / ocnt);
                out[idx + 2] = (uint8_t)(sb / ocnt);
            } else {
                out[idx + 0] = 0;
                out[idx + 1] = 0;
                out[idx + 2] = 0;
            }
            out[idx + 3] = (uint8_t)((tcnt > 0) ? ((acnt * 255) / tcnt) : 0);
        }
    }
}

static void load_sprite_disk_cache(const char *hd_dir) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/.hd_sprite_index.cache", hd_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint32_t magic = 0, count = 0;
    if (fread(&magic, 4, 1, f) == 1 && fread(&count, 4, 1, f) == 1 &&
        magic == HD_SPR_CACHE_MAGIC && count > 0 && count <= MAX_HD_SPRITES) {
        g_spr_dcache = (HdSprCacheDiskEntry *)malloc((size_t)count * sizeof(HdSprCacheDiskEntry));
        g_spr_dcache_head = (int *)malloc(4096 * sizeof(int));
        g_spr_dcache_next = (int *)malloc((size_t)count * sizeof(int));
        if (g_spr_dcache && g_spr_dcache_head && g_spr_dcache_next &&
            fread(g_spr_dcache, sizeof(HdSprCacheDiskEntry), count, f) == count) {
            g_spr_dcache_count = (int)count;
            for (int i = 0; i < 4096; i++) g_spr_dcache_head[i] = -1;
            for (int i = 0; i < g_spr_dcache_count; i++) {
                uint32_t b = hash_str_u32(g_spr_dcache[i].rel_name) & 4095u;
                g_spr_dcache_next[i] = g_spr_dcache_head[b];
                g_spr_dcache_head[b] = i;
            }
        } else {
            free(g_spr_dcache); g_spr_dcache = NULL;
            free(g_spr_dcache_head); g_spr_dcache_head = NULL;
            free(g_spr_dcache_next); g_spr_dcache_next = NULL;
        }
    }
    fclose(f);
}

static const HdSprCacheDiskEntry *find_sprite_in_dcache(const char *rel, int64_t mtime, int64_t fsize) {
    if (!g_spr_dcache || !g_spr_dcache_head) return NULL;
    uint32_t b = hash_str_u32(rel) & 4095u;
    for (int i = g_spr_dcache_head[b]; i >= 0; i = g_spr_dcache_next[i]) {
        if (g_spr_dcache[i].mtime == mtime && g_spr_dcache[i].fsize == fsize &&
            strcmp(g_spr_dcache[i].rel_name, rel) == 0) {
            return &g_spr_dcache[i];
        }
    }
    return NULL;
}

static void save_sprite_disk_cache(const char *hd_dir) {
    if (!g_spr_ncache || g_spr_ncache_count <= 0) return;
    char path[1100];
    snprintf(path, sizeof(path), "%s/.hd_sprite_index.cache", hd_dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t magic = HD_SPR_CACHE_MAGIC;
    uint32_t count = (uint32_t)g_spr_ncache_count;
    fwrite(&magic, 4, 1, f);
    fwrite(&count, 4, 1, f);
    fwrite(g_spr_ncache, sizeof(HdSprCacheDiskEntry), count, f);
    fclose(f);
}

static void scan_sprites_recursive(const char *root_dir, const char *rel_prefix) {
    char cur_dir[1024];
    if (rel_prefix && rel_prefix[0]) {
        snprintf(cur_dir, sizeof(cur_dir), "%s/%s", root_dir, rel_prefix);
    } else {
        snprintf(cur_dir, sizeof(cur_dir), "%s", root_dir);
    }

    DIR *d = opendir(cur_dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_sprite_count < MAX_HD_SPRITES) {
        if (ent->d_name[0] == '.') continue;
        char full[512], rel[256];
        int fn = snprintf(full, sizeof(full), "%s/%s", cur_dir, ent->d_name);
        if (fn < 0 || fn >= (int)sizeof(full)) continue;
        int rn;
        if (rel_prefix && rel_prefix[0]) {
            rn = snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, ent->d_name);
        } else {
            rn = snprintf(rel, sizeof(rel), "%s", ent->d_name);
        }
        if (rn < 0 || rn >= (int)sizeof(rel)) continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_sprites_recursive(root_dir, rel);
        } else if (S_ISREG(st.st_mode) && has_image_ext(ent->d_name)) {
            /* Skip optional 16:9 widescreen wing extensions (*_wide.webp / *_wide.png) from 4:3 matching */
            if (strstr(ent->d_name, "_wide.") != NULL) continue;
            int64_t mtime = (int64_t)st.st_mtime;
            int64_t fsize = (int64_t)st.st_size;
            int nw = 0, nh = 0;
            uint8_t tbuf[HD_SPR_TBYTES];
            const HdSprCacheDiskEntry *ce = find_sprite_in_dcache(rel, mtime, fsize);
            if (ce) {
                nw = ce->hd_w;
                nh = ce->hd_h;
                memcpy(tbuf, ce->thumb, HD_SPR_TBYTES);
            } else {
                uint8_t *rgba = decode_image_ffmpeg(full, 0, 0, AV_PIX_FMT_RGBA, &nw, &nh);
                if (!rgba || nw < 16 || nh < 16) {
                    free(rgba);
                    continue;
                }
                compute_rgba_sprite_thumb(rgba, nw, nh, tbuf);
                free(rgba);
                g_spr_cache_dirty = 1;
            }

            if (g_spr_ncache && g_spr_ncache_count < MAX_HD_SPRITES) {
                HdSprCacheDiskEntry *ne = &g_spr_ncache[g_spr_ncache_count++];
                memset(ne, 0, sizeof(*ne));
                snprintf(ne->rel_name, sizeof(ne->rel_name), "%s", rel);
                ne->mtime = mtime;
                ne->fsize = fsize;
                ne->hd_w = nw;
                ne->hd_h = nh;
                memcpy(ne->thumb, tbuf, HD_SPR_TBYTES);
            }

            int idx = g_sprite_count++;
            HdSpriteEntry *e = &g_sprites[idx];
            snprintf(e->full_path, sizeof(e->full_path), "%s", full);
            snprintf(e->rel_name, sizeof(e->rel_name), "%s", rel);
            e->hd_w = nw;
            e->hd_h = nh;
            memcpy(e->thumb, tbuf, HD_SPR_TBYTES);
            uint32_t b = hash_dims_u32(nw, nh);
            e->next_in_bucket = g_spr_buckets[b];
            g_spr_buckets[b] = idx;
        }
    }
    closedir(d);
}

static void hd_sprites_init(const char *hd_dir) {
    char spr_dir[1024];
    snprintf(spr_dir, sizeof(spr_dir), "%s/sprites", hd_dir);
    struct stat st;
    if (stat(spr_dir, &st) != 0 || !S_ISDIR(st.st_mode)) return;

    g_sprites = (HdSpriteEntry *)malloc(MAX_HD_SPRITES * sizeof(HdSpriteEntry));
    if (!g_sprites) return;
    g_sprite_count = 0;
    for (int i = 0; i < HD_SPR_BUCKETS; i++) g_spr_buckets[i] = -1;

    g_spr_dcache = NULL;
    g_spr_dcache_count = 0;
    g_spr_dcache_head = NULL;
    g_spr_dcache_next = NULL;
    g_spr_ncache = (HdSprCacheDiskEntry *)malloc(MAX_HD_SPRITES * sizeof(HdSprCacheDiskEntry));
    g_spr_ncache_count = 0;
    g_spr_cache_dirty = 0;

    load_sprite_disk_cache(hd_dir);
    scan_sprites_recursive(spr_dir, "");
    if (g_spr_cache_dirty || g_spr_ncache_count != g_spr_dcache_count) {
        save_sprite_disk_cache(hd_dir);
    }

    free(g_spr_dcache); g_spr_dcache = NULL; g_spr_dcache_count = 0;
    free(g_spr_dcache_head); g_spr_dcache_head = NULL;
    free(g_spr_dcache_next); g_spr_dcache_next = NULL;
    free(g_spr_ncache); g_spr_ncache = NULL; g_spr_ncache_count = 0;

    if (g_sprite_count > 0) {
        fprintf(stderr, "[HD] Indexed %d HD RGBA sprite(s) from '%s'%s\n",
                g_sprite_count, spr_dir, g_spr_cache_dirty ? " (cache updated)" : " (cached)");
        for (int i = 0; i < PREDEC_SLOTS; i++) {
            g_predec[i].sprite_id = -1;
            g_predec[i].rgba = NULL;
        }
        g_predec_stop = 0;
        if (pthread_create(&g_predec_thr, NULL, predec_worker_main, NULL) == 0) {
            g_predec_running = 1;
        }
    }
}

static void hd_sprites_shutdown(void) {
    if (g_predec_running) {
        pthread_mutex_lock(&g_predec_mtx);
        g_predec_stop = 1;
        pthread_cond_signal(&g_predec_cv);
        pthread_mutex_unlock(&g_predec_mtx);
        pthread_join(g_predec_thr, NULL);
        g_predec_running = 0;
        pthread_mutex_lock(&g_predec_mtx);
        predec_clear_locked();
        pthread_mutex_unlock(&g_predec_mtx);
    }
    free(g_sprites);
    g_sprites = NULL;
    g_sprite_count = 0;
    for (int i = 0; i < HD_SPR_BUCKETS; i++) g_spr_buckets[i] = -1;
}

int hd_sprites_count(void) {
    return g_sprite_count;
}

static int match_sprite_thumb_internal(const uint8_t thumb[HD_SPR_TBYTES], int sw, int sh) {
    int target_scales[2] = { 4, 2 };
    int is_font_strip  = (sw >= 200 && sh <= 40);
    int is_small_prop  = (sw <= 64 || sh <= 64);
    int max_mean_alpha = is_small_prop ? 28 : 18;
    int max_cell_da    = is_small_prop ? 256 : 200;
    int max_mean_rgb   = is_font_strip ? 40 : (is_small_prop ? 22 : 18);
    int max_cell_drgb  = is_font_strip ? 112 : (is_small_prop ? 160 : 96);

    int probe_opaque = 0;
    int probe_faint  = 0;
    for (int c = 0; c < HD_SPR_TW * HD_SPR_TH; c++) {
        uint8_t a0 = thumb[c * 4 + 3];
        if (a0 >= 48) probe_opaque++;
        if (a0 >= 12) probe_faint++;
    }
    if (probe_opaque == 0 && probe_faint == 0) return -1;
    const int min_cell_a = (probe_opaque <= 2) ? 12 : 48;
    const int min_shared = (probe_opaque <= 2) ? 1 : 2;

    int best_id = -1;
    int best_score = 1 << 30;
    for (int si = 0; si < 2; si++) {
        int tw = sw * target_scales[si];
        int th = sh * target_scales[si];
        for (int idx = g_spr_buckets[hash_dims_u32(tw, th)]; idx >= 0; idx = g_sprites[idx].next_in_bucket) {
            const HdSpriteEntry *e = &g_sprites[idx];
            if (e->hd_w != tw || e->hd_h != th) continue;
            if (!sprite_allowed_in_active_room(e->rel_name)) continue;
            int alpha_sad = 0;
            int rgb_sad = 0;
            int shared_cells = 0;
            int cand_opaque = 0;
            int max_da = 0;
            int max_drgb = 0;
            for (int c = 0; c < HD_SPR_TW * HD_SPR_TH; c++) {
                int o = c * 4;
                int a0 = thumb[o + 3];
                int a1 = e->thumb[o + 3];
                if (a1 >= 48) cand_opaque++;
                int da = a0 - a1;
                if (da < 0) da = -da;
                alpha_sad += da;
                if (da > max_da) max_da = da;
                if (a0 >= min_cell_a && a1 >= min_cell_a) {
                    int dr = (int)thumb[o + 0] - (int)e->thumb[o + 0];
                    int dg = (int)thumb[o + 1] - (int)e->thumb[o + 1];
                    int db = (int)thumb[o + 2] - (int)e->thumb[o + 2];
                    int cdiff = ((dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db)) / 3;
                    rgb_sad += cdiff;
                    if (cdiff > max_drgb) max_drgb = cdiff;
                    shared_cells++;
                }
            }
            int mean_alpha = alpha_sad / (HD_SPR_TW * HD_SPR_TH);
            if (mean_alpha >= max_mean_alpha || max_da >= max_cell_da) continue;
            if (shared_cells < min_shared) continue;
            if (probe_opaque <= 2 && cand_opaque > 3) continue;
            int mean_rgb = rgb_sad / shared_cells;
            if (mean_rgb >= max_mean_rgb || max_drgb >= max_cell_drgb) continue;
            /* Un-truncated fixed-point SAD + peak-cell penalty so nearly-identical puzzle sprites
               (e.g. Safe rotary wheel digits 0-9, dice, cards, locks) never tie or mismatch! */
            int score = (rgb_sad * 256) / shared_cells +
                        (alpha_sad * 128) / (HD_SPR_TW * HD_SPR_TH) +
                        max_drgb * 4;
            if (score < best_score) {
                best_score = score;
                best_id = idx;
            }
        }
        if (best_id >= 0) break; /* prefer 4x over 2x if found */
    }
    return best_id;
}

static int has_candidate_for_dims(int sw, int sh) {
    int target_scales[2] = { 4, 2 };
    for (int si = 0; si < 2; si++) {
        int tw = sw * target_scales[si];
        int th = sh * target_scales[si];
        for (int idx = g_spr_buckets[hash_dims_u32(tw, th)]; idx >= 0; idx = g_sprites[idx].next_in_bucket) {
            if (g_sprites[idx].hd_w == tw && g_sprites[idx].hd_h == th &&
                sprite_allowed_in_active_room(g_sprites[idx].rel_name)) {
                return 1;
            }
        }
    }
    return 0;
}

int hd_sprites_find_strip_cell(int surf_w, int surf_h, int *out_cw, int *out_ch, int max_out) {
    if (g_sprite_count <= 0 || !g_sprites || surf_w < 16 || surf_h < 16 || !out_cw || !out_ch || max_out <= 0) {
        return 0;
    }
    int found = 0;
    for (int i = 0; i < g_sprite_count; i++) {
        const HdSpriteEntry *e = &g_sprites[i];
        if (!sprite_allowed_in_active_room(e->rel_name)) continue;
        for (int scale = 4; scale >= 2; scale -= 2) {
            if ((e->hd_w % scale) != 0 || (e->hd_h % scale) != 0) continue;
            int cw = e->hd_w / scale;
            int ch = e->hd_h / scale;
            if (cw < 16 || ch < 16) continue;
            if ((cw < surf_w || ch < surf_h) && (surf_w % cw) == 0 && (surf_h % ch) == 0) {
                int dup = 0;
                for (int k = 0; k < found; k++) {
                    if (out_cw[k] == cw && out_ch[k] == ch) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    out_cw[found] = cw;
                    out_ch[found] = ch;
                    found++;
                    if (found >= max_out) return found;
                }
                break;
            }
        }
    }
    return found;
}

int hd_sprites_match_rgba(const uint8_t *rgba, int w, int h) {
    if (g_sprite_count <= 0 || !g_sprites || !rgba || w < 16 || h < 16) return -1;
    if (!has_candidate_for_dims(w, h)) return -1;
    uint8_t thumb[HD_SPR_TBYTES];
    compute_rgba_sprite_thumb(rgba, w, h, thumb);
    return match_sprite_thumb_internal(thumb, w, h);
}

int hd_sprites_match_rect(const void *pixels, int surf_w, int surf_h, int pitch, int bpp,
                          int sx0, int sy0, int sw, int sh,
                          int use_ck, uint32_t ck_lo, uint32_t ck_hi) {
    if (g_sprite_count <= 0 || !g_sprites || !pixels) return -1;
    if (sw < 16 || sh < 16 || sx0 < 0 || sy0 < 0 || sx0 + sw > surf_w || sy0 + sh > surf_h) return -1;
    if (bpp != 16 && bpp != 32) return -1;
    if (!has_candidate_for_dims(sw, sh)) return -1;

    /* Compute 8x8 color-key-aware RGBA thumbnail of the source rect */
    uint8_t thumb[HD_SPR_TBYTES];
    const uint8_t *base = (const uint8_t *)pixels;
    int total_opaque = 0;
    for (int ty = 0; ty < HD_SPR_TH; ty++) {
        int y0 = sy0 + (ty * sh) / HD_SPR_TH;
        int y1 = sy0 + ((ty + 1) * sh) / HD_SPR_TH;
        if (y1 <= y0) y1 = y0 + 1;
        int ystep = (y1 - y0) > 32 ? ((y1 - y0) / 16) : 1;
        for (int tx = 0; tx < HD_SPR_TW; tx++) {
            int x0 = sx0 + (tx * sw) / HD_SPR_TW;
            int x1 = sx0 + ((tx + 1) * sw) / HD_SPR_TW;
            if (x1 <= x0) x1 = x0 + 1;
            int xstep = (x1 - x0) > 32 ? ((x1 - x0) / 16) : 1;
            int sr = 0, sg = 0, sb = 0, ocnt = 0, tcnt = 0;
            for (int y = y0; y < y1; y += ystep) {
                const uint8_t *row = base + y * pitch;
                for (int x = x0; x < x1; x += xstep) {
                    tcnt++;
                    if (bpp == 32) {
                        uint32_t px;
                        memcpy(&px, row + x * 4, 4);
                        uint32_t rgb = px & 0xFFFFFFu;
                        if (use_ck && rgb >= (ck_lo & 0xFFFFFFu) && rgb <= (ck_hi & 0xFFFFFFu)) continue;
                        sr += (int)((rgb >> 16) & 0xFFu);
                        sg += (int)((rgb >> 8) & 0xFFu);
                        sb += (int)(rgb & 0xFFu);
                        ocnt++;
                    } else {
                        uint16_t px;
                        memcpy(&px, row + x * 2, 2);
                        if (use_ck && px >= ck_lo && px <= ck_hi) continue;
                        sr += (int)(((px >> 11) & 0x1Fu) * 255u / 31u);
                        sg += (int)(((px >> 5) & 0x3Fu) * 255u / 63u);
                        sb += (int)((px & 0x1Fu) * 255u / 31u);
                        ocnt++;
                    }
                }
            }
            int tidx = (ty * HD_SPR_TW + tx) * 4;
            if (ocnt > 0) {
                thumb[tidx + 0] = (uint8_t)(sr / ocnt);
                thumb[tidx + 1] = (uint8_t)(sg / ocnt);
                thumb[tidx + 2] = (uint8_t)(sb / ocnt);
                total_opaque += ocnt;
            } else {
                thumb[tidx + 0] = 0;
                thumb[tidx + 1] = 0;
                thumb[tidx + 2] = 0;
            }
            thumb[tidx + 3] = (uint8_t)((tcnt > 0) ? ((ocnt * 255) / tcnt) : 0);
        }
    }
    if (total_opaque == 0) return -1;
    return match_sprite_thumb_internal(thumb, sw, sh);
}

int hd_sprites_decode_rgba(int sprite_id, HdImageRGBA *out) {
    if (!out || !g_sprites || sprite_id < 0 || sprite_id >= g_sprite_count) return -1;
    if (g_predec_running) {
        pthread_mutex_lock(&g_predec_mtx);
        for (int i = 0; i < PREDEC_SLOTS; i++) {
            if (g_predec[i].sprite_id == sprite_id && g_predec[i].rgba) {
                out->rgba = g_predec[i].rgba;
                out->width = g_predec[i].w;
                out->height = g_predec[i].h;
                g_predec[i].rgba = NULL;
                g_predec[i].sprite_id = -1;
                pthread_mutex_unlock(&g_predec_mtx);
                return 0;
            }
        }
        pthread_mutex_unlock(&g_predec_mtx);
    }
    int nw = 0, nh = 0;
    uint8_t *rgba = decode_image_ffmpeg(g_sprites[sprite_id].full_path, 0, 0, AV_PIX_FMT_RGBA, &nw, &nh);
    if (!rgba) return -1;
    out->rgba = rgba;
    out->width = nw;
    out->height = nh;
    return 0;
}

void hd_sprites_free_rgba(HdImageRGBA *img) {
    if (!img) return;
    free(img->rgba);
    img->rgba = NULL;
    img->width = 0;
    img->height = 0;
}

const char *hd_sprites_get_name(int sprite_id) {
    if (!g_sprites || sprite_id < 0 || sprite_id >= g_sprite_count) return "";
    return g_sprites[sprite_id].rel_name;
}

void hd_sprites_get_orig_dims(int sprite_id, int *out_w, int *out_h) {
    if (!g_sprites || sprite_id < 0 || sprite_id >= g_sprite_count) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }
    if (out_w) *out_w = g_sprites[sprite_id].hd_w;
    if (out_h) *out_h = g_sprites[sprite_id].hd_h;
}
