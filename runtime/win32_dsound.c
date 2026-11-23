#include "runtime.h"
#include "gl_renderer.h"
#include "imports.h"
#include <pthread.h>

typedef struct DSoundData {
    int in_use;
    int refcount;
    uint32_t va;
    uint32_t size;
} DSoundData;

typedef struct DSoundBuf {
    int in_use;
    int refcount;
    uint32_t obj_va;
    int data_id;          /* index into g_ds_data[] (refcounted shared PCM) */
    uint32_t data_va;     /* cached guest-accessible PCM ring buffer VA */
    uint32_t size;        /* buffer size in bytes */
    uint32_t freq;        /* sample rate Hz */
    uint16_t channels;    /* 1 or 2 */
    uint16_t bits;        /* 8 or 16 */
    uint32_t block_align; /* channels * (bits/8) */

    int playing;
    int looping;
    double play_frame;    /* fractional frame index */
    int32_t volume_mb;    /* DirectSound volume: 0 (max) .. -10000 (mute) */
    int32_t pan_mb;       /* DirectSound pan: -10000 (left) .. 0 .. +10000 (right) */
    float gain_l;
    float gain_r;
    float cur_gain_l;     /* smoothed gain to prevent pops/clicks */
    float cur_gain_r;
} DSoundBuf;

#define MAX_DS_BUFS 256
#define MIX_RATE    48000

static DSoundData g_ds_data[MAX_DS_BUFS];
static DSoundBuf g_ds_bufs[MAX_DS_BUFS];
static int g_ds_obj_refcount = 0;
static SDL_AudioDeviceID g_audio_dev = 0;
static pthread_mutex_t g_ds_mtx = PTHREAD_MUTEX_INITIALIZER;

static int alloc_ds_data(uint32_t size) {
    for (int i = 1; i < MAX_DS_BUFS; i++) {
        if (!g_ds_data[i].in_use) {
            uint32_t va = guest_alloc(size + 64u);
            if (!va) return 0;
            g_ds_data[i].in_use = 1;
            g_ds_data[i].refcount = 1;
            g_ds_data[i].va = va;
            g_ds_data[i].size = size;
            return i;
        }
    }
    return 0;
}

static void release_ds_data(int data_id) {
    if (data_id < 1 || data_id >= MAX_DS_BUFS) return;
    DSoundData *d = &g_ds_data[data_id];
    if (!d->in_use) return;
    d->refcount--;
    if (d->refcount <= 0) {
        if (d->va) guest_free(d->va);
        memset(d, 0, sizeof(*d));
    }
}

/* ---- Real-Time Mastering Biquad EQ (Low-Shelf + High-Shelf) + M/S Stereo Widener ---- */
typedef struct Biquad {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} Biquad;

static Biquad g_eq_ls_l, g_eq_ls_r, g_eq_hs_l, g_eq_hs_r;
static int g_eq_last_mode = -1;

static void biquad_set_shelf(Biquad *q, int high_shelf, float freq_hz, float gain_db) {
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * 3.14159265358979323846f * freq_hz / (float)MIX_RATE;
    float cw = cosf(w0);
    float sw = sinf(w0);
    float alpha = sw * 0.70710678f; /* S = 1.0 smooth shelf slope */
    float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;
    float b0, b1, b2, a0, a1, a2;
    if (!high_shelf) {
        b0 =    A * ((A + 1.0f) - (A - 1.0f) * cw + two_sqrtA_alpha);
        b1 =  2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
        b2 =    A * ((A + 1.0f) - (A - 1.0f) * cw - two_sqrtA_alpha);
        a0 =        (A + 1.0f) + (A - 1.0f) * cw + two_sqrtA_alpha;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cw);
        a2 =        (A + 1.0f) + (A - 1.0f) * cw - two_sqrtA_alpha;
    } else {
        b0 =    A * ((A + 1.0f) + (A - 1.0f) * cw + two_sqrtA_alpha);
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
        b2 =    A * ((A + 1.0f) + (A - 1.0f) * cw - two_sqrtA_alpha);
        a0 =        (A + 1.0f) - (A - 1.0f) * cw + two_sqrtA_alpha;
        a1 =  2.0f * ((A - 1.0f) - (A + 1.0f) * cw);
        a2 =        (A + 1.0f) - (A - 1.0f) * cw - two_sqrtA_alpha;
    }
    float inv = 1.0f / a0;
    q->b0 = b0 * inv; q->b1 = b1 * inv; q->b2 = b2 * inv;
    q->a1 = a1 * inv; q->a2 = a2 * inv;
}

static inline float biquad_step(Biquad *q, float x) {
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static void update_master_eq(int mode) {
    if (mode == g_eq_last_mode) return;
    g_eq_last_mode = mode;
    float ls_db = (mode == EQ_STUDIO) ? 2.2f : 3.0f;
    float hs_db = (mode == EQ_STUDIO) ? 4.2f : 2.2f;
    float hs_hz = (mode == EQ_STUDIO) ? 6200.0f : 7200.0f;
    biquad_set_shelf(&g_eq_ls_l, 0, 115.0f, ls_db);
    biquad_set_shelf(&g_eq_ls_r, 0, 115.0f, ls_db);
    biquad_set_shelf(&g_eq_hs_l, 1, hs_hz, hs_db);
    biquad_set_shelf(&g_eq_hs_r, 1, hs_hz, hs_db);
}

static void update_gains(DSoundBuf *b) {
    float vol = (b->volume_mb <= -10000) ? 0.0f : powf(10.0f, (float)b->volume_mb / 2000.0f);
    if (vol > 1.0f) vol = 1.0f;
    float gl = vol, gr = vol;
    if (b->pan_mb < 0) {
        gr *= powf(10.0f, (float)b->pan_mb / 2000.0f);
    } else if (b->pan_mb > 0) {
        gl *= powf(10.0f, -(float)b->pan_mb / 2000.0f);
    }
    b->gain_l = gl;
    b->gain_r = gr;
}

static inline float read_sample(const uint8_t *base, uint32_t total_frames, int64_t frame,
                                int ch_idx, uint16_t channels, uint16_t bits, int looping) {
    if (frame < 0) frame = looping ? (frame + total_frames) : 0;
    if (frame >= (int64_t)total_frames) frame = looping ? (frame % total_frames) : (int64_t)(total_frames - 1);
    uint32_t idx = (uint32_t)frame;
    if (bits == 16) {
        int16_t s;
        memcpy(&s, base + (idx * channels + (uint32_t)ch_idx) * 2u, 2);
        return (float)s / 32768.0f;
    } else {
        uint8_t u = base[idx * channels + (uint32_t)ch_idx];
        return ((float)u - 128.0f) / 128.0f;
    }
}

/* 4-point Catmull-Rom cubic interpolation for artifact-free 22kHz -> 48kHz upsampling */
static inline float sample_cubic(const uint8_t *base, uint32_t total_frames, double pos,
                                 int ch_idx, uint16_t channels, uint16_t bits, int looping) {
    int64_t i = (int64_t)floor(pos);
    float t = (float)(pos - (double)i);
    float y0 = read_sample(base, total_frames, i - 1, ch_idx, channels, bits, looping);
    float y1 = read_sample(base, total_frames, i,     ch_idx, channels, bits, looping);
    float y2 = read_sample(base, total_frames, i + 1, ch_idx, channels, bits, looping);
    float y3 = read_sample(base, total_frames, i + 2, ch_idx, channels, bits, looping);
    float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    float a1 =        y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float a2 = -0.5f * y0             + 0.5f * y2;
    float a3 = y1;
    return ((a0 * t + a1) * t + a2) * t + a3;
}

/* Subtle Schroeder Stereo Castle Reverb (4 lowpass-damped comb filters + 2 allpass diffusers)
   for warm spatial ambience on 2005 music & SFX — zero heap allocations */
#define RV_COMBS 4
#define RV_APS   2
static const int k_comb_len_l[RV_COMBS] = { 1617, 1791, 1987, 2179 };
static const int k_comb_len_r[RV_COMBS] = { 1649, 1823, 2017, 2213 };
static const int k_ap_len_l[RV_APS]     = { 347, 557 };
static const int k_ap_len_r[RV_APS]     = { 373, 587 };
static float g_comb_l[RV_COMBS][2300];
static float g_comb_r[RV_COMBS][2300];
static int   g_comb_idx_l[RV_COMBS], g_comb_idx_r[RV_COMBS];
static float g_comb_lp_l[RV_COMBS],  g_comb_lp_r[RV_COMBS];
static float g_ap_l[RV_APS][640];
static float g_ap_r[RV_APS][640];
static int   g_ap_idx_l[RV_APS], g_ap_idx_r[RV_APS];
static float g_cur_master = 1.0f;

static void SDLCALL dsound_audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    static int audio_unpinned = 0;
    if (!audio_unpinned) {
        audio_unpinned = 1;
        runtime_unpin_current_thread();
    }
    float *out = (float *)stream;
    int nframes = len / 8; /* stereo float32 */
    memset(out, 0, (size_t)len);

    int muted = __atomic_load_n(&g_cfg.muted, __ATOMIC_RELAXED);
    float master_vol = 1.0f;
    __atomic_load(&g_cfg.master_volume, &master_vol, __ATOMIC_RELAXED);
    float target_master = muted ? 0.0f : master_vol;

    pthread_mutex_lock(&g_ds_mtx);
    for (int i = 1; i < MAX_DS_BUFS; i++) {
        DSoundBuf *b = &g_ds_bufs[i];
        if (!b->in_use || !b->playing || !b->data_va || b->block_align == 0) continue;
        uint32_t total_frames = b->size / b->block_align;
        if (total_frames == 0) continue;
        const uint8_t *base = g_mem + b->data_va;
        double step = (double)b->freq / (double)MIX_RATE;
        double pos = b->play_frame;
        float gl = b->cur_gain_l;
        float gr = b->cur_gain_r;
        float tgl = b->gain_l;
        float tgr = b->gain_r;

        for (int f = 0; f < nframes; f++) {
            if (pos >= (double)total_frames) {
                if (b->looping) {
                    pos = fmod(pos, (double)total_frames);
                } else {
                    b->playing = 0;
                    pos = 0.0;
                    break;
                }
            }
            /* Smooth gain ramp (~5ms) to eliminate clicks */
            gl += (tgl - gl) * 0.015f;
            gr += (tgr - gr) * 0.015f;
            float sl = sample_cubic(base, total_frames, pos, 0, b->channels, b->bits, b->looping);
            float sr = (b->channels >= 2)
                ? sample_cubic(base, total_frames, pos, 1, b->channels, b->bits, b->looping)
                : sl;
            out[f * 2 + 0] += sl * gl;
            out[f * 2 + 1] += sr * gr;
            pos += step;
        }
        b->play_frame = pos;
        b->cur_gain_l = gl;
        b->cur_gain_r = gr;
    }
    pthread_mutex_unlock(&g_ds_mtx);

    int rev_mode = __atomic_load_n(&g_cfg.audio_reverb, __ATOMIC_RELAXED);
    float rev_wet = (rev_mode == REVERB_RICH) ? 0.26f : ((rev_mode == REVERB_SUBTLE) ? 0.13f : 0.0f);
    float rev_fb  = (rev_mode == REVERB_RICH) ? 0.78f : 0.70f;
    float damp    = 0.35f;

    int eq_mode = __atomic_load_n(&g_cfg.audio_eq, __ATOMIC_RELAXED);
    if (eq_mode != EQ_OFF) update_master_eq(eq_mode);
    float stereo_width = (eq_mode == EQ_STUDIO) ? 1.24f : ((eq_mode == EQ_WARM) ? 1.16f : 1.0f);

    for (int f = 0; f < nframes; f++) {
        /* Smooth master volume / mute anti-pop ramping */
        g_cur_master += (target_master - g_cur_master) * 0.004f;
        float dl = out[f * 2 + 0] * g_cur_master;
        float dr = out[f * 2 + 1] * g_cur_master;

        if (eq_mode != EQ_OFF) {
            dl = biquad_step(&g_eq_hs_l, biquad_step(&g_eq_ls_l, dl));
            dr = biquad_step(&g_eq_hs_r, biquad_step(&g_eq_ls_r, dr));
            float mid  = 0.5f * (dl + dr);
            float side = 0.5f * (dl - dr) * stereo_width;
            dl = mid + side;
            dr = mid - side;
        }

        if (rev_wet > 0.001f) {
            float inp = (dl + dr) * 0.25f;
            float wl = 0.0f, wr = 0.0f;
            for (int c = 0; c < RV_COMBS; c++) {
                int il = g_comb_idx_l[c];
                float yl = g_comb_l[c][il];
                g_comb_lp_l[c] = yl * (1.0f - damp) + g_comb_lp_l[c] * damp;
                g_comb_l[c][il] = inp + g_comb_lp_l[c] * rev_fb;
                if (++il >= k_comb_len_l[c]) il = 0;
                g_comb_idx_l[c] = il;
                wl += yl;

                int ir = g_comb_idx_r[c];
                float yr = g_comb_r[c][ir];
                g_comb_lp_r[c] = yr * (1.0f - damp) + g_comb_lp_r[c] * damp;
                g_comb_r[c][ir] = inp + g_comb_lp_r[c] * rev_fb;
                if (++ir >= k_comb_len_r[c]) ir = 0;
                g_comb_idx_r[c] = ir;
                wr += yr;
            }
            for (int a = 0; a < RV_APS; a++) {
                int il = g_ap_idx_l[a];
                float bl = g_ap_l[a][il];
                float ol = -wl + bl;
                g_ap_l[a][il] = wl + bl * 0.5f;
                if (++il >= k_ap_len_l[a]) il = 0;
                g_ap_idx_l[a] = il;
                wl = ol;

                int ir = g_ap_idx_r[a];
                float br = g_ap_r[a][ir];
                float or_ = -wr + br;
                g_ap_r[a][ir] = wr + br * 0.5f;
                if (++ir >= k_ap_len_r[a]) ir = 0;
                g_ap_idx_r[a] = ir;
                wr = or_;
            }
            dl = dl * (1.0f - rev_wet * 0.35f) + wl * rev_wet;
            dr = dr * (1.0f - rev_wet * 0.35f) + wr * rev_wet;
        }

        /* Gentle soft-clipping limiter */
        if (dl > 0.85f || dl < -0.85f) dl = tanhf(dl);
        if (dr > 0.85f || dr < -0.85f) dr = tanhf(dr);
        out[f * 2 + 0] = dl;
        out[f * 2 + 1] = dr;
    }
}

void win32_dsound_init(void) {
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = MIX_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 512; /* ~10.6 ms low-latency buffer */
    want.callback = dsound_audio_callback;
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_dev != 0) {
        SDL_PauseAudioDevice(g_audio_dev, 0);
    } else {
        fprintf(stderr, "WARN: SDL_OpenAudioDevice failed: %s (continuing muted)\n", SDL_GetError());
    }
}

void win32_dsound_shutdown(void) {
    if (g_audio_dev != 0) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
}

static DSoundBuf *get_buf(uint32_t h) {
    if (h >= 1 && h < MAX_DS_BUFS && g_ds_bufs[h].in_use) return &g_ds_bufs[h];
    return NULL;
}

int win32_dsound_dispatch(CpuCtx *c, uint32_t import_id) {
    if (import_id == IMP_DSOUND_DLL_Ordinal_1) {
        /* DirectSoundCreate(lpGuid, ppDS, pUnkOuter) */
        uint32_t ppds = arg_u32(c, 1);
        uint32_t obj = com_create_obj(COM_IFACE_DSOUND, 16, 1);
        g_ds_obj_refcount = 1;
        if (ppds) mem_w32(ppds, obj);
        if (g_debug_log) fprintf(stderr, "[DSOUND] DirectSoundCreate -> obj=0x%08x\n", obj);
        stdcall_ret(c, 3, 0);
        return 1;
    }
    return 0;
}

void com_dsound_dispatch(CpuCtx *c, uint32_t iface_id, uint32_t slot) {
    uint32_t this_va = arg_u32(c, 0);

    if (iface_id == COM_IFACE_DSOUND) {
        switch (slot) {
            case 0: { uint32_t ppv = arg_u32(c, 2); if (ppv) mem_w32(ppv, this_va); stdcall_ret(c, 3, 0); return; }
            case 1: stdcall_ret(c, 1, (uint32_t)(++g_ds_obj_refcount)); return;
            case 2: {
                uint32_t rc = 0;
                if (g_ds_obj_refcount > 0) {
                    g_ds_obj_refcount--;
                    rc = (uint32_t)g_ds_obj_refcount;
                    if (g_ds_obj_refcount == 0 && this_va) guest_free(this_va);
                }
                stdcall_ret(c, 1, rc); return;
            }
            case 3: { /* CreateSoundBuffer(this, lpcDSBufferDesc, lplpDirectSoundBuffer, pUnkOuter) */
                uint32_t desc_va = arg_u32(c, 1);
                uint32_t out_va  = arg_u32(c, 2);
                uint32_t flags   = mem_r32(desc_va + 4);
                uint32_t bsz     = mem_r32(desc_va + 8);
                uint32_t wfx_va  = mem_r32(desc_va + 16);
                int is_primary   = (flags & 1u) != 0;

                pthread_mutex_lock(&g_ds_mtx);
                uint32_t h = 0;
                for (uint32_t i = 1; i < MAX_DS_BUFS; i++) {
                    if (!g_ds_bufs[i].in_use) { h = i; break; }
                }
                if (h) {
                    DSoundBuf *b = &g_ds_bufs[h];
                    memset(b, 0, sizeof(*b));
                    b->in_use = 1;
                    b->refcount = 1;
                    b->channels = wfx_va ? mem_r16(wfx_va + 2) : 2;
                    b->freq = wfx_va ? mem_r32(wfx_va + 4) : 22050;
                    b->bits = wfx_va ? mem_r16(wfx_va + 14) : 16;
                    if (b->channels == 0) b->channels = 2;
                    if (b->freq == 0) b->freq = 22050;
                    if (b->bits != 8 && b->bits != 16) b->bits = 16;
                    b->block_align = b->channels * (b->bits / 8u);
                    b->size = (bsz > 0) ? bsz : (is_primary ? 16384u : 4096u);
                    b->data_id = alloc_ds_data(b->size);
                    if (!b->data_id) {
                        memset(b, 0, sizeof(*b));
                        h = 0;
                        if (out_va) mem_w32(out_va, 0);
                    } else {
                        b->data_va = g_ds_data[b->data_id].va;
                        memset(g_mem + b->data_va, b->bits == 8 ? 0x80 : 0, b->size);
                        b->volume_mb = 0;
                        b->pan_mb = 0;
                        update_gains(b);
                        b->cur_gain_l = b->gain_l;
                        b->cur_gain_r = b->gain_r;
                        b->obj_va = com_create_obj(COM_IFACE_DSOUND_BUF, 24, h);
                        if (!b->obj_va) {
                            release_ds_data(b->data_id);
                            memset(b, 0, sizeof(*b));
                            h = 0;
                        }
                        if (out_va) mem_w32(out_va, h ? b->obj_va : 0);
                    }
                } else {
                    if (out_va) mem_w32(out_va, 0);
                }
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 4, h ? 0u : 0x8007000Eu); return;
            }
            case 4: { /* GetCaps(this, lpDSCaps) */
                uint32_t caps = arg_u32(c, 1);
                if (caps) {
                    uint32_t sz = mem_r32(caps);
                    if (sz > 4 && sz <= 128) memset(g_mem + caps + 4, 0, sz - 4);
                    mem_w32(caps + 4, 0x00000FFFu);
                    mem_w32(caps + 8, 8000);
                    mem_w32(caps + 12, 48000);
                    mem_w32(caps + 16, 1);
                    mem_w32(caps + 20, 64);
                    mem_w32(caps + 24, 64);
                    mem_w32(caps + 28, 64);
                }
                stdcall_ret(c, 2, 0); return;
            }
            case 5: { /* DuplicateSoundBuffer(this, lpDsbOriginal, lplpDsbDuplicate) */
                uint32_t orig_obj = arg_u32(c, 1);
                uint32_t out_va   = arg_u32(c, 2);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *orig = get_buf(com_obj_handle(orig_obj));
                uint32_t h = 0;
                if (orig) {
                    for (uint32_t i = 1; i < MAX_DS_BUFS; i++) {
                        if (!g_ds_bufs[i].in_use) { h = i; break; }
                    }
                    if (h) {
                        DSoundBuf *b = &g_ds_bufs[h];
                        *b = *orig;
                        b->refcount = 1;
                        b->playing = 0;
                        b->play_frame = 0.0;
                        if (b->data_id >= 1 && b->data_id < MAX_DS_BUFS && g_ds_data[b->data_id].in_use) {
                            g_ds_data[b->data_id].refcount++;
                        }
                        b->obj_va = com_create_obj(COM_IFACE_DSOUND_BUF, 24, h);
                        if (!b->obj_va) {
                            release_ds_data(b->data_id);
                            memset(b, 0, sizeof(*b));
                            h = 0;
                        }
                        if (out_va) mem_w32(out_va, h ? b->obj_va : 0);
                    }
                }
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 3, h ? 0u : 0x80070057u); return;
            }
            case 6: /* SetCooperativeLevel(this, hwnd, dwLevel) */
                stdcall_ret(c, 3, 0); return;
            case 10: /* Initialize(this, pcGuid) */
                stdcall_ret(c, 2, 0); return;
            default:
                fprintf(stderr, "[DSOUND] Unhandled IDirectSound slot %u\n", slot);
                stdcall_ret(c, 1, 0); return;
        }
    }

    if (iface_id == COM_IFACE_DSOUND_BUF) {
        uint32_t h = com_obj_handle(this_va);
        switch (slot) {
            case 0: { uint32_t ppv = arg_u32(c, 2); if (ppv) mem_w32(ppv, this_va); stdcall_ret(c, 3, 0); return; }
            case 1: {
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                uint32_t rc = b ? (uint32_t)(++b->refcount) : 1u;
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 1, rc); return;
            }
            case 2: {
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                uint32_t rc = 0;
                if (b) {
                    b->refcount--;
                    rc = (uint32_t)b->refcount;
                    if (b->refcount <= 0) {
                        b->playing = 0;
                        release_ds_data(b->data_id);
                        b->data_id = 0;
                        b->data_va = 0;
                        b->in_use = 0;
                        guest_free(this_va);
                    }
                }
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 1, rc); return;
            }
            case 4: { /* GetCurrentPosition(this, lpdwPlay, lpdwWrite) */
                uint32_t pplay = arg_u32(c, 1);
                uint32_t pwrite = arg_u32(c, 2);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                if (b && b->size > 0) {
                    uint32_t byte_pos = ((uint32_t)b->play_frame * b->block_align) % b->size;
                    uint32_t write_pos = (byte_pos + 2048u) % b->size;
                    if (pplay) mem_w32(pplay, byte_pos);
                    if (pwrite) mem_w32(pwrite, write_pos);
                }
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 3, 0); return;
            }
            case 5: { /* GetFormat(this, lpwfx, dwSize, lpdwWritten) */
                uint32_t wfx = arg_u32(c, 1);
                uint32_t wr  = arg_u32(c, 3);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                if (b && wfx) {
                    mem_w16(wfx + 0, 1); /* WAVE_FORMAT_PCM */
                    mem_w16(wfx + 2, b->channels);
                    mem_w32(wfx + 4, b->freq);
                    mem_w32(wfx + 8, b->freq * b->block_align);
                    mem_w16(wfx + 12, (uint16_t)b->block_align);
                    mem_w16(wfx + 14, b->bits);
                    mem_w16(wfx + 16, 0);
                }
                if (wr) mem_w32(wr, 18);
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 4, 0); return;
            }
            case 6: { /* GetVolume */
                uint32_t out = arg_u32(c, 1);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                if (b && out) mem_w32(out, (uint32_t)b->volume_mb);
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 2, 0); return;
            }
            case 7: { /* GetPan */
                uint32_t out = arg_u32(c, 1);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                if (b && out) mem_w32(out, (uint32_t)b->pan_mb);
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 2, 0); return;
            }
            case 8: { /* GetFrequency */
                uint32_t out = arg_u32(c, 1);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                if (b && out) mem_w32(out, b->freq);
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 2, 0); return;
            }
            case 9: { /* GetStatus(this, lpdwStatus) */
                uint32_t out = arg_u32(c, 1);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                uint32_t st = 0;
                if (b && b->playing) {
                    st |= 1u; /* DSBSTATUS_PLAYING */
                    if (b->looping) st |= 4u; /* DSBSTATUS_LOOPING */
                }
                if (out) mem_w32(out, st);
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 2, 0); return;
            }
            case 11: { /* Lock(this, dwWriteCursor, dwWriteBytes, lplpvAudioPtr1, lpdwAudioBytes1, lplpvAudioPtr2, lpdwAudioBytes2, dwFlags) */
                uint32_t cursor = arg_u32(c, 1);
                uint32_t bytes  = arg_u32(c, 2);
                uint32_t pp1 = arg_u32(c, 3), pl1 = arg_u32(c, 4);
                uint32_t pp2 = arg_u32(c, 5), pl2 = arg_u32(c, 6);
                uint32_t flags = arg_u32(c, 7);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                if (b && b->data_va && b->size > 0) {
                    if (flags & 2u /* DSBLOCK_ENTIREBUFFER */) { cursor = 0; bytes = b->size; }
                    if (flags & 1u /* DSBLOCK_FROMWRITECURSOR */) {
                        cursor = ((uint32_t)b->play_frame * b->block_align) % b->size;
                    }
                    cursor %= b->size;
                    if (bytes > b->size) bytes = b->size;
                    uint32_t first = b->size - cursor;
                    if (first >= bytes) {
                        if (pp1) mem_w32(pp1, b->data_va + cursor);
                        if (pl1) mem_w32(pl1, bytes);
                        if (pp2) mem_w32(pp2, 0);
                        if (pl2) mem_w32(pl2, 0);
                    } else {
                        if (pp1) mem_w32(pp1, b->data_va + cursor);
                        if (pl1) mem_w32(pl1, first);
                        if (pp2) mem_w32(pp2, b->data_va);
                        if (pl2) mem_w32(pl2, bytes - first);
                    }
                }
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 8, 0); return;
            }
            case 12: { /* Play(this, dwReserved1, dwReserved2, dwFlags) */
                uint32_t flags = arg_u32(c, 3);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                if (b) {
                    b->playing = 1;
                    b->looping = (flags & 1u) ? 1 : 0;
                }
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 4, 0); return;
            }
            case 13: { /* SetCurrentPosition(this, dwNewPosition) */
                uint32_t pos = arg_u32(c, 1);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                if (b && b->block_align > 0 && b->size > 0) {
                    b->play_frame = (double)((pos % b->size) / b->block_align);
                }
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 2, 0); return;
            }
            case 14: /* SetFormat(this, lpcfxFormat) */
                stdcall_ret(c, 2, 0); return;
            case 15: { /* SetVolume(this, lVolume) */
                int32_t vol = arg_i32(c, 1);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                if (b) { b->volume_mb = vol; update_gains(b); }
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 2, 0); return;
            }
            case 16: { /* SetPan(this, lPan) */
                int32_t pan = arg_i32(c, 1);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                if (b) { b->pan_mb = pan; update_gains(b); }
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 2, 0); return;
            }
            case 17: { /* SetFrequency(this, dwFrequency) */
                uint32_t freq = arg_u32(c, 1);
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                if (b && freq >= 100 && freq <= 192000) b->freq = freq;
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 2, 0); return;
            }
            case 18: { /* Stop(this) */
                pthread_mutex_lock(&g_ds_mtx);
                DSoundBuf *b = get_buf(h);
                if (b) b->playing = 0;
                pthread_mutex_unlock(&g_ds_mtx);
                stdcall_ret(c, 1, 0); return;
            }
            case 19: /* Unlock(this, ptr1, len1, ptr2, len2) */
                stdcall_ret(c, 5, 0); return;
            case 20: /* Restore(this) */
                stdcall_ret(c, 1, 0); return;
            default:
                fprintf(stderr, "[DSOUND] Unhandled IDirectSoundBuffer slot %u\n", slot);
                stdcall_ret(c, 1, 0); return;
        }
    }
}
