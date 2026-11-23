#include "runtime.h"
#include "imports.h"
#include <sys/stat.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

typedef struct FfVideo {
    int in_use;
    AVFormatContext *fmt;
    AVCodecContext  *dec;
    struct SwsContext *sws;
    int vstream;
    int width;
    int height;
    double fps;
    int64_t total_frames;
    int64_t cur_frame;
    AVFrame *frame;
    AVPacket *pkt;
    uint8_t *rgb_buf;
    int rgb_stride;
    /* For AVIStreamGetFrame guest DIB */
    uint32_t dib_va;
    /* For DirectShow playback */
    int running;
    int eof;
    uint32_t start_ticks;
    uint32_t paused_elapsed;
} FfVideo;

#define MAX_VIDEOS 16
static FfVideo g_vids[MAX_VIDEOS];

static void ff_close(FfVideo *v) {
    if (!v || !v->in_use) return;
    if (v->sws) { sws_freeContext(v->sws); v->sws = NULL; }
    if (v->frame) { av_frame_free(&v->frame); }
    if (v->pkt) { av_packet_free(&v->pkt); }
    if (v->dec) { avcodec_free_context(&v->dec); }
    if (v->fmt) { avformat_close_input(&v->fmt); }
    if (v->rgb_buf) { free(v->rgb_buf); v->rgb_buf = NULL; }
    if (v->dib_va) { guest_free(v->dib_va); v->dib_va = 0; }
    v->in_use = 0;
}

static int ff_try_hd_video(const char *orig_path, char *out_path, size_t out_sz) {
    if (!g_cfg.hd_assets_enabled || !g_cfg.hd_assets_dir[0] || !orig_path) return 0;
    const char *slash = strrchr(orig_path, '/');
    const char *base = slash ? slash + 1 : orig_path;
    char stem[256];
    snprintf(stem, sizeof(stem), "%.255s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    if (!stem[0]) return 0;

    static const char *exts[] = { "mp4", "mkv", "webm", "avi" };
    struct stat st;
    for (int i = 0; i < 4; i++) {
        int n = snprintf(out_path, out_sz, "%s/videos/%s.%s", g_cfg.hd_assets_dir, stem, exts[i]);
        if (n > 0 && (size_t)n < out_sz && stat(out_path, &st) == 0 && S_ISREG(st.st_mode)) {
            return 1;
        }
    }
    return 0;
}

static int ff_open(FfVideo *v, const char *win_path) {
    char lpath[1024];
    char hd_path[1024];
    int used_hd = 0;
    resolve_game_path(win_path, lpath, sizeof(lpath), 0);
    if (ff_try_hd_video(lpath, hd_path, sizeof(hd_path))) {
        memcpy(lpath, hd_path, sizeof(lpath));
        used_hd = 1;
    }
    memset(v, 0, sizeof(*v));
    if (avformat_open_input(&v->fmt, lpath, NULL, NULL) < 0) {
        if (used_hd) {
            /* Fall back to original AVI if HD file failed to open */
            resolve_game_path(win_path, lpath, sizeof(lpath), 0);
            used_hd = 0;
            if (avformat_open_input(&v->fmt, lpath, NULL, NULL) < 0) {
                fprintf(stderr, "[AVI] avformat_open_input failed for '%s' -> '%s'\n", win_path, lpath);
                return -1;
            }
        } else {
            fprintf(stderr, "[AVI] avformat_open_input failed for '%s' -> '%s'\n", win_path, lpath);
            return -1;
        }
    }
    if (avformat_find_stream_info(v->fmt, NULL) < 0) {
        avformat_close_input(&v->fmt);
        return -1;
    }
    v->vstream = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (v->vstream < 0) {
        avformat_close_input(&v->fmt);
        return -1;
    }
    AVStream *st = v->fmt->streams[v->vstream];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&v->fmt);
        return -1;
    }
    v->dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(v->dec, st->codecpar);
    if (avcodec_open2(v->dec, codec, NULL) < 0) {
        avcodec_free_context(&v->dec);
        avformat_close_input(&v->fmt);
        return -1;
    }
    v->width = v->dec->width;
    v->height = v->dec->height;
    AVRational r = av_guess_frame_rate(v->fmt, st, NULL);
    v->fps = (r.num > 0 && r.den > 0) ? av_q2d(r) : 25.0;
    if (st->nb_frames > 0) v->total_frames = st->nb_frames;
    else if (v->fmt->duration > 0) v->total_frames = (int64_t)((double)v->fmt->duration / AV_TIME_BASE * v->fps);
    else v->total_frames = 1000;

    v->frame = av_frame_alloc();
    v->pkt = av_packet_alloc();
    v->rgb_stride = v->width * 3;
    v->rgb_buf = (uint8_t *)calloc(1, (size_t)v->rgb_stride * (size_t)v->height);
    v->sws = sws_getContext(v->width, v->height, v->dec->pix_fmt,
                            v->width, v->height, AV_PIX_FMT_RGB24,
                            SWS_LANCZOS, NULL, NULL, NULL);
    if (!v->frame || !v->pkt || !v->rgb_buf || !v->sws) {
        v->in_use = 1;
        ff_close(v);
        return -1;
    }
    v->in_use = 1;
    v->cur_frame = -1;
    fprintf(stderr, "[AVI] Opened %s'%s' (%dx%d @ %.2f fps, ~%lld frames)\n",
            used_hd ? "HD cutscene " : "",
            lpath, v->width, v->height, v->fps, (long long)v->total_frames);
    return 0;
}

static int ff_decode_next_frame(FfVideo *v) {
    if (!v || !v->in_use || v->eof) return 0;
    int flushed = 0;
    int consec_err = 0;
    while (1) {
        int r = avcodec_receive_frame(v->dec, v->frame);
        if (r == 0) {
            uint8_t *dst[4] = { v->rgb_buf, NULL, NULL, NULL };
            int dst_linesize[4] = { v->rgb_stride, 0, 0, 0 };
            sws_scale(v->sws, (const uint8_t *const *)v->frame->data, v->frame->linesize,
                      0, v->height, dst, dst_linesize);
            v->cur_frame++;
            return 1;
        }
        if (r == AVERROR_EOF || r != AVERROR(EAGAIN)) {
            v->eof = 1;
            return 0;
        }
        if (av_read_frame(v->fmt, v->pkt) < 0) {
            if (!flushed) {
                flushed = 1;
                int sr = avcodec_send_packet(v->dec, NULL);
                if (sr == 0 || sr == AVERROR(EAGAIN)) {
                    continue;
                }
            }
            v->eof = 1;
            return 0;
        }
        if (v->pkt->stream_index == v->vstream) {
            int sr = avcodec_send_packet(v->dec, v->pkt);
            if (sr < 0 && sr != AVERROR(EAGAIN)) {
                if (++consec_err > 64) {
                    av_packet_unref(v->pkt);
                    v->eof = 1;
                    return 0;
                }
            } else {
                consec_err = 0;
            }
        }
        av_packet_unref(v->pkt);
    }
}

static FfVideo *alloc_vid(uint32_t *out_idx) {
    for (uint32_t i = 1; i < MAX_VIDEOS; i++) {
        if (!g_vids[i].in_use) {
            if (out_idx) *out_idx = i;
            return &g_vids[i];
        }
    }
    return NULL;
}

static FfVideo *get_vid(uint32_t idx) {
    if (idx >= 1 && idx < MAX_VIDEOS && g_vids[idx].in_use) return &g_vids[idx];
    return NULL;
}

int win32_avi_is_playing(void) {
    for (uint32_t i = 1; i < MAX_VIDEOS; i++) {
        if (!g_vids[i].in_use) continue;
        if (g_vids[i].dib_va != 0) return 1;              /* Active AVIFile/DrawDib playback */
        if (g_vids[i].running && !g_vids[i].eof) return 1;/* Active DirectShow playback */
    }
    return 0;
}

/* ---- DirectShow COM ---- */
static uint32_t g_dshow_vid_idx = 0;

uint32_t com_dshow_create_graph(void) {
    uint32_t idx = 0;
    FfVideo *v = alloc_vid(&idx);
    if (!v) return 0;
    memset(v, 0, sizeof(*v));
    v->in_use = 1;
    g_dshow_vid_idx = idx;
    return com_create_obj(COM_IFACE_DSHOW_GRAPH, 24, idx);
}

static void dshow_step_playback(FfVideo *v) {
    if (!v || !v->running || v->eof || !v->fmt) return;
    win32_pump_sdl_events();
    uint32_t elapsed = SDL_GetTicks() - v->start_ticks;
    int64_t target_frame = (int64_t)((double)elapsed * v->fps / 1000.0);
    int decoded = 0;
    while (v->cur_frame < target_frame && !v->eof) {
        if (!ff_decode_next_frame(v)) break;
        decoded = 1;
    }
    if (decoded) {
        ddraw_present_rgb24(v->rgb_buf, v->width, v->height, v->rgb_stride);
    }
}

void com_dshow_dispatch(CpuCtx *c, uint32_t iface_id, uint32_t slot) {
    uint32_t this_va = arg_u32(c, 0);
    uint32_t vid_idx = com_obj_handle(this_va);
    FfVideo *v = get_vid(vid_idx);

    if (iface_id == COM_IFACE_DSHOW_GRAPH) {
        switch (slot) {
            case 0: { /* QueryInterface(this, riid, ppv) */
                uint32_t iid_va = arg_u32(c, 1);
                uint32_t ppv = arg_u32(c, 2);
                uint32_t d1 = iid_va ? mem_r32(iid_va) : 0;
                uint32_t obj = 0;
                if (d1 == 0x56a868b1u) obj = com_create_obj(COM_IFACE_DSHOW_CTRL, 20, vid_idx);
                else if (d1 == 0x56a868b6u) obj = com_create_obj(COM_IFACE_DSHOW_EVENT, 20, vid_idx);
                else if (d1 == 0x56a868b4u) obj = com_create_obj(COM_IFACE_DSHOW_VIDWIN, 48, vid_idx);
                else if (d1 == 0x56a868b5u) obj = com_create_obj(COM_IFACE_DSHOW_BASVID, 48, vid_idx);
                else if (d1 == 0x56a868a9u) obj = this_va;
                if (ppv) mem_w32(ppv, obj);
                stdcall_ret(c, 3, obj ? 0u : 0x80004002u); return;
            }
            case 1: stdcall_ret(c, 1, 2); return;
            case 2: /* Release */
                if (v) ff_close(v);
                if (this_va) guest_free(this_va);
                stdcall_ret(c, 1, 0); return;
            case 13: { /* RenderFile(this, lpcwstrFile, lpcwstrPlayList) -> +0x34 */
                uint32_t wpath_va = arg_u32(c, 1);
                char apath[512];
                int i = 0;
                while (wpath_va && i < 511) {
                    uint16_t ch = mem_r16(wpath_va + (uint32_t)i * 2u);
                    if (!ch) break;
                    apath[i++] = (char)(ch < 128 ? ch : '?');
                }
                apath[i] = '\0';
                if (v) {
                    ff_close(v);
                    if (ff_open(v, apath) == 0) {
                        stdcall_ret(c, 3, 0); return;
                    }
                    v->in_use = 1;
                }
                stdcall_ret(c, 3, 0x80040218u /* VFW_E_CANNOT_RENDER */); return;
            }
            case 15: /* SetLogFile(this, hFile) -> +0x3c */
                stdcall_ret(c, 2, 0); return;
            default:
                stdcall_ret(c, 1, 0); return;
        }
    }

    if (iface_id == COM_IFACE_DSHOW_CTRL) {
        /* IDispatch (slots 0..6) + Run(7), Pause(8), Stop(9), GetState(10) */
        switch (slot) {
            case 0: { uint32_t ppv = arg_u32(c, 2); if (ppv) mem_w32(ppv, this_va); stdcall_ret(c, 3, 0); return; }
            case 1: stdcall_ret(c, 1, 2); return;
            case 2:
                if (this_va) guest_free(this_va);
                stdcall_ret(c, 1, 0); return;
            case 7: /* Run(this) */
                if (v) {
                    if (!v->running) {
                        v->start_ticks = SDL_GetTicks() - v->paused_elapsed;
                        v->running = 1;
                    }
                    dshow_step_playback(v);
                }
                stdcall_ret(c, 1, 0); return;
            case 8: /* Pause(this) */
                if (v && v->running) {
                    v->paused_elapsed = SDL_GetTicks() - v->start_ticks;
                    v->running = 0;
                }
                stdcall_ret(c, 1, 0); return;
            case 9: /* Stop(this) */
                if (v) { v->running = 0; v->eof = 1; v->paused_elapsed = 0; }
                stdcall_ret(c, 1, 0); return;
            case 10: { /* GetState(this, msTimeout, pfs) */
                uint32_t pfs = arg_u32(c, 2);
                if (v) dshow_step_playback(v);
                if (pfs) mem_w32(pfs, (v && v->running && !v->eof) ? 2u : 0u);
                stdcall_ret(c, 3, 0); return;
            }
            default:
                stdcall_ret(c, 1, 0); return;
        }
    }

    if (iface_id == COM_IFACE_DSHOW_EVENT) {
        /* IMediaEventEx: IDispatch(0..6) + GetEventHandle(7), GetEvent(8), WaitForCompletion(9),
           CancelDefaultHandling(10), RestoreDefaultHandling(11), FreeEventParams(12),
           SetNotifyWindow(13), SetNotifyFlags(14), GetNotifyFlags(15) */
        switch (slot) {
            case 0: { uint32_t ppv = arg_u32(c, 2); if (ppv) mem_w32(ppv, this_va); stdcall_ret(c, 3, 0); return; }
            case 1: stdcall_ret(c, 1, 2); return;
            case 2:
                if (this_va) guest_free(this_va);
                stdcall_ret(c, 1, 0); return;
            case 7: { /* GetEventHandle(this, hEvent) */
                uint32_t ph = arg_u32(c, 1);
                if (ph) mem_w32(ph, 0);
                stdcall_ret(c, 2, 0); return;
            }
            case 8: { /* GetEvent(this, lEventCode, lParam1, lParam2, msTimeout) -> +0x20 */
                uint32_t pec = arg_u32(c, 1);
                uint32_t p1  = arg_u32(c, 2);
                uint32_t p2  = arg_u32(c, 3);
                if (v) dshow_step_playback(v);
                if (!v || v->eof) {
                    if (pec) mem_w32(pec, 1u /* EC_COMPLETE */);
                    if (p1) mem_w32(p1, 0);
                    if (p2) mem_w32(p2, 0);
                    stdcall_ret(c, 5, 0); return;
                }
                stdcall_ret(c, 5, 0x80004004u /* E_ABORT: no event yet */); return;
            }
            case 9: { /* WaitForCompletion(this, msTimeout, pEvCode) -> +0x24 */
                uint32_t ms = arg_u32(c, 1);
                uint32_t pec = arg_u32(c, 2);
                if (v) dshow_step_playback(v);
                if (!v || v->eof) {
                    if (pec) mem_w32(pec, 1u /* EC_COMPLETE */);
                    stdcall_ret(c, 3, 0); return;
                }
                if (ms > 0 && ms < 100) SDL_Delay(ms);
                else if (ms >= 100) SDL_Delay(16);
                if (v) dshow_step_playback(v);
                if (pec) mem_w32(pec, (v && v->eof) ? 1u : 0u);
                stdcall_ret(c, 3, (v && v->eof) ? 0u : 0x80004004u); return;
            }
            case 10: /* CancelDefaultHandling(this, lEvCode) */
            case 11: /* RestoreDefaultHandling(this, lEvCode) */
                stdcall_ret(c, 2, 0); return;
            case 12: /* FreeEventParams(this, lEvCode, lParam1, lParam2) */
                stdcall_ret(c, 4, 0); return;
            case 13: /* SetNotifyWindow(this, hwnd, lMsg, lInstanceData) */
                stdcall_ret(c, 4, 0); return;
            case 14: /* SetNotifyFlags(this, lNoNotifyFlags) */
            case 15: /* GetNotifyFlags(this, lplNoNotifyFlags) */
                stdcall_ret(c, 2, 0); return;
            default:
                stdcall_ret(c, 1, 0); return;
        }
    }

    if (iface_id == COM_IFACE_DSHOW_VIDWIN) {
        /* IVideoWindow vtable layout:
           0..2   IUnknown: QueryInterface(3), AddRef(1), Release(1)
           3..6   IDispatch: GetTypeInfoCount(2), GetTypeInfo(4), GetIDsOfNames(6), Invoke(9)
           7..36  1-param put_/get_ property methods (2 args each, e.g. +0x24 put_WindowStyle,
                  +0x34 put_AutoShow, +0x4c put_Visible, +0x74 put_Owner, +0x7c put_MessageDrain)
           37     +0x94 SetWindowForeground(this, Focus) [2 args]
           38     +0x98 NotifyOwnerMessage(this, hwnd, uMsg, wParam, lParam) [5 args]
           39     +0x9c SetWindowPosition(this, Left, Top, Width, Height) [5 args]
           40     +0xa0 GetWindowPosition(this, pLeft, pTop, pWidth, pHeight) [5 args]
           41     +0xa4 GetMinIdealImageSize(this, pWidth, pHeight) [3 args]
           42     +0xa8 GetMaxIdealImageSize(this, pWidth, pHeight) [3 args]
           43     +0xac GetRestorePosition(this, pLeft, pTop, pWidth, pHeight) [5 args]
           44..45 +0xb0/+0xb4 HideCursor / IsCursorHidden [2 args] */
        uint32_t vw = (v && v->width > 0) ? (uint32_t)v->width : 800u;
        uint32_t vh = (v && v->height > 0) ? (uint32_t)v->height : 600u;
        switch (slot) {
            case 0: { uint32_t ppv = arg_u32(c, 2); if (ppv) mem_w32(ppv, this_va); stdcall_ret(c, 3, 0); return; }
            case 1: stdcall_ret(c, 1, 2); return;
            case 2:
                if (this_va) guest_free(this_va);
                stdcall_ret(c, 1, 0); return;
            case 3: stdcall_ret(c, 2, 0x80004001u); return;
            case 4: stdcall_ret(c, 4, 0x80004001u); return;
            case 5: stdcall_ret(c, 6, 0x80004001u); return;
            case 6: stdcall_ret(c, 9, 0x80004001u); return;
            case 37: /* SetWindowForeground(this, Focus) */
                stdcall_ret(c, 2, 0); return;
            case 38: /* NotifyOwnerMessage(this, hwnd, uMsg, wParam, lParam) */
            case 39: /* SetWindowPosition(this, Left, Top, Width, Height) */
                stdcall_ret(c, 5, 0); return;
            case 40: /* GetWindowPosition(this, pLeft, pTop, pWidth, pHeight) */
            case 43: { /* GetRestorePosition(this, pLeft, pTop, pWidth, pHeight) */
                uint32_t pl = arg_u32(c, 1), pt = arg_u32(c, 2), pw = arg_u32(c, 3), ph = arg_u32(c, 4);
                if (pl) mem_w32(pl, 0);
                if (pt) mem_w32(pt, 0);
                if (pw) mem_w32(pw, vw);
                if (ph) mem_w32(ph, vh);
                stdcall_ret(c, 5, 0); return;
            }
            case 41: /* GetMinIdealImageSize(this, pWidth, pHeight) */
            case 42: { /* GetMaxIdealImageSize(this, pWidth, pHeight) */
                uint32_t pw = arg_u32(c, 1), ph = arg_u32(c, 2);
                if (pw) mem_w32(pw, vw);
                if (ph) mem_w32(ph, vh);
                stdcall_ret(c, 3, 0); return;
            }
            default: /* All 1-param put_/get_ property methods take (this, val) = 2 args */
                stdcall_ret(c, 2, 0); return;
        }
    }

    if (iface_id == COM_IFACE_DSHOW_BASVID) {
        /* IBasicVideo vtable layout:
           0..2   IUnknown, 3..6 IDispatch
           10,11  +0x28/+0x2c get_VideoWidth / get_VideoHeight [2 args]
           13,15  get_SourceLeft / get_SourceWidth (+0x3c) [2 args]
           17,19  get_SourceTop / get_SourceHeight (+0x4c) [2 args]
           21,23  get_DestinationLeft / get_DestinationWidth (+0x5c) [2 args]
           25,27  get_DestinationTop / get_DestinationHeight (+0x6c) [2 args]
           28,29  SetSourcePosition / GetSourcePosition [5 args]
           30     SetDefaultSourcePosition [1 arg]
           31,32  SetDestinationPosition / GetDestinationPosition [5 args]
           33     SetDefaultDestinationPosition [1 arg]
           34     +0x88 GetVideoSize(this, pWidth, pHeight) [3 args]
           35     GetVideoPaletteEntries [5 args]
           36     GetCurrentImage [3 args]
           37,38  IsUsingDefaultSource / IsUsingDefaultDestination [1 arg] */
        uint32_t vw = (v && v->width > 0) ? (uint32_t)v->width : 800u;
        uint32_t vh = (v && v->height > 0) ? (uint32_t)v->height : 600u;
        switch (slot) {
            case 0: { uint32_t ppv = arg_u32(c, 2); if (ppv) mem_w32(ppv, this_va); stdcall_ret(c, 3, 0); return; }
            case 1: stdcall_ret(c, 1, 2); return;
            case 2:
                if (this_va) guest_free(this_va);
                stdcall_ret(c, 1, 0); return;
            case 3: stdcall_ret(c, 2, 0x80004001u); return;
            case 4: stdcall_ret(c, 4, 0x80004001u); return;
            case 5: stdcall_ret(c, 6, 0x80004001u); return;
            case 6: stdcall_ret(c, 9, 0x80004001u); return;
            case 10: /* get_VideoWidth */
            case 15: /* get_SourceWidth */
            case 23: { /* get_DestinationWidth */
                uint32_t pw = arg_u32(c, 1);
                if (pw) mem_w32(pw, vw);
                stdcall_ret(c, 2, 0); return;
            }
            case 11: /* get_VideoHeight */
            case 19: /* get_SourceHeight */
            case 27: { /* get_DestinationHeight */
                uint32_t ph = arg_u32(c, 1);
                if (ph) mem_w32(ph, vh);
                stdcall_ret(c, 2, 0); return;
            }
            case 13: /* get_SourceLeft */
            case 17: /* get_SourceTop */
            case 21: /* get_DestinationLeft */
            case 25: { /* get_DestinationTop */
                uint32_t pv = arg_u32(c, 1);
                if (pv) mem_w32(pv, 0);
                stdcall_ret(c, 2, 0); return;
            }
            case 28: /* SetSourcePosition(this, Left, Top, Width, Height) */
            case 31: /* SetDestinationPosition(this, Left, Top, Width, Height) */
                stdcall_ret(c, 5, 0); return;
            case 29: /* GetSourcePosition(this, pLeft, pTop, pWidth, pHeight) */
            case 32: { /* GetDestinationPosition(this, pLeft, pTop, pWidth, pHeight) */
                uint32_t pl = arg_u32(c, 1), pt = arg_u32(c, 2), pw = arg_u32(c, 3), ph = arg_u32(c, 4);
                if (pl) mem_w32(pl, 0);
                if (pt) mem_w32(pt, 0);
                if (pw) mem_w32(pw, vw);
                if (ph) mem_w32(ph, vh);
                stdcall_ret(c, 5, 0); return;
            }
            case 30: /* SetDefaultSourcePosition */
            case 33: /* SetDefaultDestinationPosition */
            case 37: /* IsUsingDefaultSource */
            case 38: /* IsUsingDefaultDestination */
                stdcall_ret(c, 1, 0); return;
            case 34: { /* GetVideoSize(this, pWidth, pHeight) */
                uint32_t pw = arg_u32(c, 1), ph = arg_u32(c, 2);
                if (pw) mem_w32(pw, vw);
                if (ph) mem_w32(ph, vh);
                stdcall_ret(c, 3, 0); return;
            }
            case 35: /* GetVideoPaletteEntries(this, StartIndex, Entries, pRetrieved, pPalette) */
                stdcall_ret(c, 5, 0); return;
            case 36: /* GetCurrentImage(this, pBufferSize, pDIBImage) */
                stdcall_ret(c, 3, 0x80004001u); return;
            default:
                stdcall_ret(c, 2, 0); return;
        }
    }
}

/* ---- AVIFIL32 + MSVFW32 Imports ---- */
int win32_avi_dispatch(CpuCtx *c, uint32_t import_id) {
    switch (import_id) {
        case IMP_AVIFIL32_DLL_AVIFileInit:
            stdcall_ret(c, 0, 0); return 1;
        case IMP_AVIFIL32_DLL_AVIFileExit:
            stdcall_ret(c, 0, 0); return 1;
        case IMP_AVIFIL32_DLL_AVIFileOpenA: {
            uint32_t ppfile = arg_u32(c, 0);
            const char *fname = arg_cstr(c, 1);
            uint32_t idx = 0;
            FfVideo *v = alloc_vid(&idx);
            if (!v || ff_open(v, fname ? fname : "") != 0) {
                if (ppfile) mem_w32(ppfile, 0);
                stdcall_ret(c, 4, 0x8004406Au /* AVIERR_FILEREAD */); return 1;
            }
            if (ppfile) mem_w32(ppfile, idx);
            stdcall_ret(c, 4, 0); return 1;
        }
        case IMP_AVIFIL32_DLL_AVIFileInfoA: {
            FfVideo *v = get_vid(arg_u32(c, 0));
            uint32_t pfi = arg_u32(c, 1);
            uint32_t sz = arg_u32(c, 2);
            if (v && pfi) {
                memset(g_mem + pfi, 0, sz < 108 ? sz : 108);
                mem_w32(pfi + 8, 1);                       /* dwStreams */
                mem_w32(pfi + 16, (uint32_t)v->width);     /* dwWidth */
                mem_w32(pfi + 20, (uint32_t)v->height);    /* dwHeight */
                mem_w32(pfi + 24, 1000);                   /* dwScale */
                mem_w32(pfi + 28, (uint32_t)(v->fps * 1000.0)); /* dwRate */
                mem_w32(pfi + 32, (uint32_t)v->total_frames);   /* dwLength */
            }
            stdcall_ret(c, 3, v ? 0u : 0x80044066u); return 1;
        }
        case IMP_AVIFIL32_DLL_AVIFileGetStream: {
            uint32_t h = arg_u32(c, 0);
            uint32_t ppavi = arg_u32(c, 1);
            uint32_t fcc = arg_u32(c, 2);
            if (get_vid(h) && (fcc == 0 || fcc == 0x73646976u /* 'vids' */)) {
                if (ppavi) mem_w32(ppavi, h);
                stdcall_ret(c, 4, 0); return 1;
            }
            if (ppavi) mem_w32(ppavi, 0);
            stdcall_ret(c, 4, 0x80044068u); return 1;
        }
        case IMP_AVIFIL32_DLL_AVIFileRelease: {
            FfVideo *v = get_vid(arg_u32(c, 0));
            if (v) ff_close(v);
            stdcall_ret(c, 1, 0); return 1;
        }
        case IMP_AVIFIL32_DLL_AVIStreamLength: {
            FfVideo *v = get_vid(arg_u32(c, 0));
            stdcall_ret(c, 1, v ? (uint32_t)v->total_frames : 0u); return 1;
        }
        case IMP_AVIFIL32_DLL_AVIStreamSampleToTime: {
            FfVideo *v = get_vid(arg_u32(c, 0));
            int32_t sample = arg_i32(c, 1);
            uint32_t ms = (v && v->fps > 0.0) ? (uint32_t)((double)sample * 1000.0 / v->fps) : 0u;
            stdcall_ret(c, 2, ms); return 1;
        }
        case IMP_AVIFIL32_DLL_AVIStreamGetFrameOpen: {
            uint32_t h = arg_u32(c, 0);
            FfVideo *v = get_vid(h);
            if (!v) { stdcall_ret(c, 2, 0); return 1; }
            uint32_t stride = ((uint32_t)v->width * 3u + 3u) & ~3u;
            /* Only the 40-byte BITMAPINFOHEADER is read by the guest; DrawDibDraw
               presents v->rgb_buf directly with zero 23MB guest copy/BGR-swap! */
            if (!v->dib_va) v->dib_va = guest_alloc(64u);
            if (v->dib_va) {
                memset(g_mem + v->dib_va, 0, 40);
                mem_w32(v->dib_va + 0, 40);
                mem_w32(v->dib_va + 4, (uint32_t)v->width);
                mem_w32(v->dib_va + 8, (uint32_t)(-v->height)); /* top-down */
                mem_w16(v->dib_va + 12, 1);
                mem_w16(v->dib_va + 14, 24);
                mem_w32(v->dib_va + 20, stride * (uint32_t)v->height);
            }
            stdcall_ret(c, 2, h); return 1;
        }
        case IMP_AVIFIL32_DLL_AVIStreamGetFrame: {
            FfVideo *v = get_vid(arg_u32(c, 0));
            int32_t lpos = arg_i32(c, 1);
            if (!v || !v->dib_va) { stdcall_ret(c, 2, 0); return 1; }
            while (v->cur_frame < lpos && !v->eof) {
                if (!ff_decode_next_frame(v)) break;
            }
            stdcall_ret(c, 2, v->dib_va); return 1;
        }
        case IMP_AVIFIL32_DLL_AVIStreamGetFrameClose: {
            FfVideo *v = get_vid(arg_u32(c, 0));
            if (v && v->dib_va) { guest_free(v->dib_va); v->dib_va = 0; }
            stdcall_ret(c, 1, 0); return 1;
        }
        case IMP_MSVFW32_DLL_DrawDibOpen:
            stdcall_ret(c, 0, 0x5001u); return 1;
        case IMP_MSVFW32_DLL_DrawDibClose:
            stdcall_ret(c, 1, 1); return 1;
        case IMP_MSVFW32_DLL_DrawDibDraw: {
            uint32_t lpbi = arg_u32(c, 6);
            uint32_t lpbits = arg_u32(c, 7);
            if (lpbi) {
                /* Fast zero-copy path: match active FfVideo by its BITMAPINFOHEADER VA */
                FfVideo *matched = NULL;
                for (uint32_t i = 1; i < MAX_VIDEOS; i++) {
                    if (g_vids[i].in_use && g_vids[i].dib_va == lpbi && g_vids[i].rgb_buf) {
                        matched = &g_vids[i];
                        break;
                    }
                }
                if (matched) {
                    ddraw_present_rgb24(matched->rgb_buf, matched->width, matched->height, matched->rgb_stride);
                } else if (lpbi <= GUEST_MEM_SIZE - 40u) {
                    int w = (int32_t)mem_r32(lpbi + 4);
                    int32_t h_raw = (int32_t)mem_r32(lpbi + 8);
                    if (w > 0 && w <= 16384 && h_raw != 0 && h_raw != INT32_MIN) {
                        int top_down = (h_raw < 0);
                        int h = top_down ? -h_raw : h_raw;
                        uint32_t bi_size = mem_r32(lpbi);
                        uint32_t bits_va = lpbits ? lpbits :
                            ((bi_size <= GUEST_MEM_SIZE - lpbi) ? (lpbi + bi_size) : GUEST_MEM_SIZE);
                        uint64_t stride = (((uint64_t)w * 3u) + 3u) & ~3ull;
                        uint64_t total_bytes = (uint64_t)h * stride;
                        if (h <= 16384 && bits_va < GUEST_MEM_SIZE &&
                            total_bytes <= (uint64_t)GUEST_MEM_SIZE - bits_va) {
                            uint8_t *rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
                            if (rgb) {
                                for (int y = 0; y < h; y++) {
                                    int sy = top_down ? y : (h - 1 - y);
                                    const uint8_t *s = g_mem + bits_va + (size_t)sy * (size_t)stride;
                                    uint8_t *d = rgb + (size_t)y * (size_t)w * 3;
                                    for (int x = 0; x < w; x++) {
                                        d[x*3+0] = s[x*3+2];
                                        d[x*3+1] = s[x*3+1];
                                        d[x*3+2] = s[x*3+0];
                                    }
                                }
                                ddraw_present_rgb24(rgb, w, h, w * 3);
                                free(rgb);
                            }
                        }
                    }
                }
            }
            stdcall_ret(c, 13, 1); return 1;
        }
        case IMP_MSVFW32_DLL_MCIWndCreateA:
            /* cdecl variadic on Win32! Return 0 so it uses DirectShow/AVIFile */
            cdecl_ret(c, 0); return 1;
    }
    return 0;
}
