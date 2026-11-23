/*
 * Original 2005 game (mha2.exe) bugfixes & hot-path native replacements.
 *
 * Installed into g_code_table[] after init_code_table() so calls to these
 * addresses (emitted via g_code_table by recomp/lift.py's HOOKABLE_FUNCS)
 * execute clean, bug-free native C instead of the buggy/slow 2005 x86 code.
 */
#include "runtime.h"
#include <math.h>
#include <unistd.h>

/* 0x004443f0: void __fastcall MetaSpinLock::Lock(uint8_t *out_acquired)
 * Original bug: non-atomic DAT_0051b3b8++ with 200x Sleep(1) and forced
 * lock-steal (DAT_0051b3b8 = 0) while another thread is still in the CS.
 * Fix: true atomic CAS on DAT_0051b3b8 (-1 -> 0) with zero forced lock-steal. */
static void patch_004443f0_metalock(CpuCtx *c) {
    uint32_t tid = c->thread_id ? c->thread_id : 1000u;
    uint32_t out_acq = c->ecx;
    uint32_t *owner_ptr = (uint32_t *)(g_mem + 0x00527824u);
    int32_t  *lock_ptr  = (int32_t  *)(g_mem + 0x0051b3b8u);

    if (__atomic_load_n(owner_ptr, __ATOMIC_ACQUIRE) == tid) {
        if (out_acq) mem_w8(out_acq, 0);
        c->esp += 4u;
        return;
    }
    if (out_acq) mem_w8(out_acq, 1);
    for (;;) {
        int32_t expected = -1;
        if (__atomic_compare_exchange_n(lock_ptr, &expected, 0, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            __atomic_store_n(owner_ptr, tid, __ATOMIC_RELEASE);
            break;
        }
        usleep(200);
    }
    c->esp += 4u;
}

/* 0x00444230: int __thiscall ObjMutex::TryLock(void *this, uint32_t timeout_ms, uint32_t *out_status)
 * Original bug: uses the non-atomic meta-spinlock with forced lock-steal and 10ms Sleep polling.
 * Fix: atomic CAS directly on *(uint32_t *)(this + 4) with 1ms polling and zero lock-steal. */
static void patch_00444230_trylock(CpuCtx *c) {
    uint32_t this_va    = c->ecx;
    uint32_t timeout_ms = arg_u32(c, 0);
    uint32_t status_va  = arg_u32(c, 1);
    uint32_t tid        = c->thread_id ? c->thread_id : 1000u;

    if (status_va) mem_w32(status_va, 2u);
    if (!this_va || this_va >= GUEST_MEM_SIZE - 8u) {
        stdcall_ret(c, 2, 0u);
        return;
    }

    uint32_t *slot = (uint32_t *)(g_mem + this_va + 4u);
    if (__atomic_load_n(slot, __ATOMIC_ACQUIRE) == tid) {
        stdcall_ret(c, 2, 0u);
        return;
    }

    if (status_va) mem_w32(status_va, 3u);
    uint32_t expected = 0u;
    if (__atomic_compare_exchange_n(slot, &expected, tid, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        stdcall_ret(c, 2, this_va);
        return;
    }

    uint32_t start = SDL_GetTicks();
    do {
        usleep(1000);
        expected = 0u;
        if (__atomic_compare_exchange_n(slot, &expected, tid, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            if (status_va) mem_w32(status_va, 0u);
            stdcall_ret(c, 2, this_va);
            return;
        }
    } while ((uint32_t)(SDL_GetTicks() - start) < timeout_ms);

    if (status_va) mem_w32(status_va, 4u);
    stdcall_ret(c, 2, 0u);
}

/* 0x004a7a5e: double __cdecl ceil(double x)
 * Called inside the inner per-span loop of all 8 scaled RLE blitters (0x48dba0 et al.).
 * Replaces 40+ emulated x87/bit-mask instructions with native hardware ceil(). */
static void patch_004a7a5e_ceil(CpuCtx *c) {
    double x = mem_rf64(c->esp + 4u);
    fpu_push(c, (long double)ceil(x));
    c->esp += 4u;
}

/* 0x004a71cc: int64_t __ftol(void) (pops ST(0), returns truncated int64 in EDX:EAX)
 * Called inside all 8 scaled RLE blitters and across the entire engine.
 * Replaces 15 emulated x87 FPU-CW save/modify/restore instructions with native truncl(). */
static void patch_004a71cc_ftol(CpuCtx *c) {
    long double v = fpu_st(c, 0);
    fpu_pop(c);
    int64_t iv = (int64_t)truncl(v);
    c->eax = (uint32_t)(uint64_t)iv;
    c->edx = (uint32_t)((uint64_t)iv >> 32);
    c->esp += 4u;
}

static void install_patch(uint32_t va, RecompFn fn) {
    if (va >= GUEST_TEXT_START && va < GUEST_TEXT_START + GUEST_TEXT_SIZE) {
        g_code_table[va - GUEST_TEXT_START] = fn;
    }
}

void game_patches_install(void) {
    install_patch(0x004443f0u, patch_004443f0_metalock);
    install_patch(0x00444230u, patch_00444230_trylock);
    install_patch(0x004a7a5eu, patch_004a7a5e_ceil);
    install_patch(0x004a71ccu, patch_004a71cc_ftol);
}

/* ============================================================================
 * Read-Only Engine Clickable Hotspot & Scene-Graph Inspector (Spacebar QoL)
 * ============================================================================ */

static inline int hs_va_ok(uint32_t va, uint32_t sz) {
    return g_mem != NULL && va >= 0x00400000u && sz <= GUEST_MEM_SIZE && va <= GUEST_MEM_SIZE - sz;
}

static uint32_t hs_resolve_1d_bucket(uint32_t b0, uint32_t b1, uint32_t blo, uint32_t bhi, uint32_t g) {
    if ((int32_t)g < 0) return 0;
    if (g == 0u) return hs_va_ok(b0, 12u) ? b0 : 0u;
    if (g == 1u) return hs_va_ok(b1, 12u) ? b1 : 0u;

    uint32_t base2d = 0, slot = 0;
    if (g < 0x100u) {
        if ((g & 0xFFFF0000u) != 0u) return 0u;
        base2d = blo;
        slot = g;
    } else {
        if ((g & 0xFF0000FFu) != 0u) return 0u;
        base2d = bhi;
        slot = g >> 8;
    }
    if (!hs_va_ok(base2d, 12u)) return 0u;
    int32_t scnt = (int32_t)mem_r32(base2d);
    uint32_t sarr = mem_r32(base2d + 8u);
    if (scnt <= 0 || slot >= (uint32_t)scnt || sarr < 0x00400000u || sarr > GUEST_MEM_SIZE - 12u) {
        return 0u;
    }
    if (slot > (GUEST_MEM_SIZE - 12u - sarr) / 12u) return 0u;
    uint32_t b1d = sarr + slot * 12u;
    return hs_va_ok(b1d, 12u) ? b1d : 0u;
}

static void hs_resolve_id(uint32_t *g, int32_t *idx, int max_depth) {
    for (int d = 0; d < max_depth && ((*g & 0xF0000000u) == 0x80000000u); d++) {
        if (*idx < 0) {
            *g = 0u;
            *idx = -1;
            return;
        }
        uint32_t k = *g & 0x7FFFFFFFu;
        uint32_t b1d = hs_resolve_1d_bucket(0x00527760u, 0x0052776cu, 0x00527784u, 0x00527778u, k);
        if (!b1d) {
            *g = 0u;
            *idx = -1;
            return;
        }
        int32_t cnt = (int32_t)mem_r32(b1d);
        uint32_t data = mem_r32(b1d + 8u);
        if (cnt <= 0 || *idx >= cnt || data < 0x00400000u || data > GUEST_MEM_SIZE - 8u ||
            (uint32_t)(*idx) > (GUEST_MEM_SIZE - 8u - data) / 8u) {
            *g = 0u;
            *idx = -1;
            return;
        }
        uint32_t entry = data + (uint32_t)(*idx) * 8u;
        *g = mem_r32(entry);
        *idx = (int32_t)mem_r32(entry + 4u);
    }
}

static uint32_t hs_lookup_ptr_table(uint32_t b0, uint32_t b1, uint32_t blo, uint32_t bhi,
                                    uint32_t g, int32_t idx) {
    hs_resolve_id(&g, &idx, 2);
    if (idx < 0) return 0u;
    uint32_t b1d = hs_resolve_1d_bucket(b0, b1, blo, bhi, g);
    if (!b1d) return 0u;
    int32_t cnt = (int32_t)mem_r32(b1d);
    uint32_t data = mem_r32(b1d + 8u);
    if (cnt <= 0 || idx >= cnt || data < 0x00400000u || data > GUEST_MEM_SIZE - 4u ||
        (uint32_t)idx > (GUEST_MEM_SIZE - 4u - data) / 4u) {
        return 0u;
    }
    return mem_r32(data + (uint32_t)idx * 4u);
}

static uint32_t hs_get_props_off(uint32_t vt) {
    switch (vt) {
        case 0x005070f4u: return 0xa8u;  /* Hotspot */
        case 0x00507fc8u: return 0x214u; /* Walker */
        case 0x0050815cu: return 0x1bcu; /* Prop */
        case 0x004fe9e8u:
        case 0x004fef84u:
        case 0x00501a2cu:
        case 0x00501c2cu:
        case 0x005030d8u: return 0x1d8u; /* AnimObj */
        default:          return 0u;
    }
}

static inline void hs_read_prop(uint32_t obj, uint32_t props_off, int slot,
                                uint32_t *out_g, int32_t *out_idx) {
    uint32_t p = obj + props_off + (uint32_t)slot * 8u;
    *out_g = mem_r32(p);
    *out_idx = (int32_t)mem_r32(p + 4u);
}

static void hs_cp1252_to_utf8(uint32_t str_va, char *dst, size_t dst_sz) {
    dst[0] = '\0';
    if (!hs_va_ok(str_va, 1u) || dst_sz < 4u) return;

    char raw[240];
    size_t rlen = 0;
    while (rlen + 1u < sizeof(raw) && hs_va_ok(str_va + (uint32_t)rlen, 1u)) {
        uint8_t ch = mem_r8(str_va + (uint32_t)rlen);
        if (ch == 0u) break;
        raw[rlen++] = (char)ch;
    }
    raw[rlen] = '\0';

    size_t s0 = 0, s1 = rlen;
    while (s0 < s1 && ((uint8_t)raw[s0] == ' ' || (uint8_t)raw[s0] == '\t' ||
                       (uint8_t)raw[s0] == '\r' || (uint8_t)raw[s0] == '\n')) s0++;
    while (s1 > s0 && ((uint8_t)raw[s1 - 1u] == ' ' || (uint8_t)raw[s1 - 1u] == '\t' ||
                       (uint8_t)raw[s1 - 1u] == '\r' || (uint8_t)raw[s1 - 1u] == '\n')) s1--;

    size_t j = 0;
    for (size_t i = s0; i < s1 && j + 4u < dst_sz; i++) {
        uint8_t ch = (uint8_t)raw[i];
        if (ch < 0x80u) {
            dst[j++] = (char)ch;
            continue;
        }
        uint32_t cp = ch;
        if      (ch == 0x80u) cp = 0x20ACu;
        else if (ch == 0x82u) cp = 0x201Au;
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
    dst[j] = '\0';
}

static void hs_lookup_label(uint32_t obj, uint32_t props_off, uint32_t hs,
                            char *dst, size_t dst_sz) {
    dst[0] = '\0';
    uint32_t sg = 0u;
    int32_t sidx = -1;

    hs_read_prop(obj, props_off, 1, &sg, &sidx);
    hs_resolve_id(&sg, &sidx, 3);

    if (sidx < 0) {
        uint32_t lg = 0u;
        int32_t lidx = -1;
        hs_read_prop(obj, props_off, 9, &lg, &lidx);
        uint32_t spr = hs_lookup_ptr_table(0x00527be8u, 0x00527bf4u, 0x00527c0cu, 0x00527c00u, lg, lidx);
        if (hs_va_ok(spr, 4u)) {
            uint32_t spoff = hs_get_props_off(mem_r32(spr));
            if (spoff && hs_va_ok(spr, spoff + 25u * 8u)) {
                hs_read_prop(spr, spoff, 1, &sg, &sidx);
                hs_resolve_id(&sg, &sidx, 3);
            }
        }
    }

    if (sidx < 0 && hs && hs != obj && hs_va_ok(hs, 0xa8u + 25u * 8u)) {
        hs_read_prop(hs, 0xa8u, 1, &sg, &sidx);
        hs_resolve_id(&sg, &sidx, 3);
    }

    if (sidx < 0) return;
    uint32_t sl = hs_lookup_ptr_table(0x005277f0u, 0x005277fcu, 0x00527814u, 0x00527808u, sg, sidx);
    if (!hs_va_ok(sl, 0x10u)) return;

    int32_t nlines = (int32_t)mem_r32(sl + 0x0cu);
    uint32_t arr = mem_r32(sl + 0x08u);
    if (nlines <= 0) return;
    if (nlines > 32) nlines = 32;
    if (!hs_va_ok(arr, (uint32_t)nlines * 4u)) return;

    for (int32_t i = 0; i < nlines; i++) {
        uint32_t line_va = mem_r32(arr + (uint32_t)i * 4u);
        if (!line_va) continue;
        hs_cp1252_to_utf8(line_va, dst, dst_sz);
        if (dst[0] != '\0') return;
    }
}

typedef struct HsWalkCtx {
    GameHotspot *out;
    int max_out;
    int count;
    struct {
        uint32_t g;
        int32_t  i;
        uint32_t hs_va;
        int      from_sprite;
    } keys[64];
    int32_t clip_l, clip_t, clip_r, clip_b;
    uint32_t skip_ui1, skip_ui2;
    uint32_t layer_mask;
    uint32_t player;
    uint32_t hotspots_ptr;
    int32_t  hotspot_count;
    int      is_location;
} HsWalkCtx;

static uint32_t hs_resolve_hotspot(const HsWalkCtx *ctx, uint32_t obj, uint32_t vt, uint32_t poff) {
    if (vt == 0x005070f4u) return obj;
    uint32_t hs = 0u;
    uint32_t g0 = 0u;
    int32_t i0 = -1;
    hs_read_prop(obj, poff, 0, &g0, &i0);
    hs_resolve_id(&g0, &i0, 2);
    if (i0 >= 0 && i0 < ctx->hotspot_count && ctx->hotspots_ptr &&
        hs_va_ok(ctx->hotspots_ptr + (uint32_t)i0 * 4u, 4u)) {
        hs = mem_r32(ctx->hotspots_ptr + (uint32_t)i0 * 4u);
    }
    if (!hs) {
        uint32_t g2 = 0u;
        int32_t i2 = -1;
        hs_read_prop(obj, poff, 2, &g2, &i2);
        hs = hs_lookup_ptr_table(0x00527af0u, 0x00527afcu, 0x00527b14u, 0x00527b08u, g2, i2);
    }
    if (hs && (!hs_va_ok(hs, 0xa8u + 25u * 8u) || mem_r32(hs) != 0x005070f4u)) {
        return 0u;
    }
    return hs;
}

static int hs_has_any_verb(uint32_t hs) {
    if (!hs || !hs_va_ok(hs, 0xa8u + 25u * 8u)) return 0;
    for (int slot = 10; slot < 25; slot++) {
        uint32_t vg = 0u;
        int32_t vi = -1;
        hs_read_prop(hs, 0xa8u, slot, &vg, &vi);
        hs_resolve_id(&vg, &vi, 2);
        if (vi >= 0) return 1;
    }
    return 0;
}

static int hs_prop_has_dialog(uint32_t obj, uint32_t poff) {
    if (!obj || !poff || !hs_va_ok(obj, poff + 25u * 8u)) return 0;
    for (int slot = 19; slot <= 20; slot++) {
        uint32_t dg = 0u;
        int32_t di = -1;
        hs_read_prop(obj, poff, slot, &dg, &di);
        if (hs_lookup_ptr_table(0x005282e0u, 0x005282ecu, 0x00528304u, 0x005282f8u, dg, di) != 0u) {
            return 1;
        }
    }
    return 0;
}

static int hs_classify_kind(uint32_t obj, uint32_t vt, uint32_t poff, uint32_t hs) {
    if (hs && (int32_t)mem_r32(hs + 0x80u) > 0) return 1; /* Exit */
    if (vt == 0x00507fc8u) return 2; /* Walker NPC */
    uint32_t g3 = 0u;
    int32_t i3 = -1;
    hs_read_prop(obj, poff, 3, &g3, &i3);
    hs_resolve_id(&g3, &i3, 2);
    if (i3 == 3) return 2;
    if (hs && hs != obj) {
        hs_read_prop(hs, 0xa8u, 3, &g3, &i3);
        hs_resolve_id(&g3, &i3, 2);
        if (i3 == 3) return 2;
    }
    if (hs_prop_has_dialog(obj, poff) || (hs && hs != obj && hs_prop_has_dialog(hs, 0xa8u))) {
        return 2;
    }
    return 0;
}

static int hs_clip_and_merge(const HsWalkCtx *ctx, int32_t sl, int32_t st, int32_t sr, int32_t sb,
                             int32_t *ux0, int32_t *uy0, int32_t *ux1, int32_t *uy1,
                             int64_t *best_area, int32_t *best_cx, int32_t *best_cy) {
    if (sl < ctx->clip_l) sl = ctx->clip_l;
    if (st < ctx->clip_t) st = ctx->clip_t;
    if (sr > ctx->clip_r) sr = ctx->clip_r;
    if (sb > ctx->clip_b) sb = ctx->clip_b;
    if (sr <= sl || sb <= st) return 0;
    if (*ux1 <= *ux0) {
        *ux0 = sl; *uy0 = st; *ux1 = sr; *uy1 = sb;
    } else {
        if (sl < *ux0) *ux0 = sl;
        if (st < *uy0) *uy0 = st;
        if (sr > *ux1) *ux1 = sr;
        if (sb > *uy1) *uy1 = sb;
    }
    int64_t area = (int64_t)(sr - sl) * (int64_t)(sb - st);
    if (area > *best_area) {
        *best_area = area;
        *best_cx = (sl + sr) / 2;
        *best_cy = (st + sb) / 2;
    }
    return 1;
}

static int hs_compute_bounds(const HsWalkCtx *ctx, uint32_t obj, uint32_t vt,
                             int32_t abs_x, int32_t abs_y,
                             int *out_x0, int *out_y0, int *out_x1, int *out_y1,
                             int *out_ax, int *out_ay) {
    int is_scaled = (vt != 0x005070f4u) &&
                    !(mem_r32(obj + 0x58u) == 0u && mem_r32(obj + 0x5cu) == 0x3ff00000u);
    double sc = 1.0;
    if (is_scaled) {
        sc = mem_rf64(obj + 0x58u);
        if (!(sc > 0.01 && sc <= 20.0)) {
            is_scaled = 0;
            sc = 1.0;
        }
    }

    int32_t ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;
    int64_t best_area = -1;
    int32_t best_cx = 0, best_cy = 0;

    uint32_t shape = mem_r32(obj + 0x54u);
    uint32_t shape_vt = hs_va_ok(shape, 0x20u) ? mem_r32(shape) : 0u;
    if (shape_vt == 0x00503d88u) {
        int32_t l = (int32_t)mem_r32(shape + 0x08u);
        int32_t t = (int32_t)mem_r32(shape + 0x0cu);
        int32_t r = (int32_t)mem_r32(shape + 0x10u);
        int32_t b = (int32_t)mem_r32(shape + 0x14u);
        int32_t sl = is_scaled ? (abs_x + (int32_t)floor((double)l * sc + 0.5)) : (abs_x + l);
        int32_t st = is_scaled ? (abs_y + (int32_t)floor((double)t * sc + 0.5)) : (abs_y + t);
        int32_t sr = is_scaled ? (abs_x + (int32_t)floor((double)r * sc + 0.5)) : (abs_x + r);
        int32_t sb = is_scaled ? (abs_y + (int32_t)floor((double)b * sc + 0.5)) : (abs_y + b);
        hs_clip_and_merge(ctx, sl, st, sr, sb, &ux0, &uy0, &ux1, &uy1, &best_area, &best_cx, &best_cy);
    } else if (shape_vt == 0x00503dd0u && hs_va_ok(shape, 0x28u)) {
        int32_t rcnt = (int32_t)mem_r32(shape + 0x20u);
        uint32_t rarr = mem_r32(shape + 0x24u);
        if (rcnt > 512) rcnt = 512;
        if (rcnt > 0 && hs_va_ok(rarr, (uint32_t)rcnt * 16u)) {
            for (int32_t i = 0; i < rcnt; i++) {
                uint32_t rp = rarr + (uint32_t)i * 16u;
                int32_t l = (int32_t)mem_r32(rp + 0x00u);
                int32_t t = (int32_t)mem_r32(rp + 0x04u);
                int32_t r = (int32_t)mem_r32(rp + 0x08u);
                int32_t b = (int32_t)mem_r32(rp + 0x0cu);
                int32_t sl = is_scaled ? (abs_x + (int32_t)floor((double)l * sc + 0.5)) : (abs_x + l);
                int32_t st = is_scaled ? (abs_y + (int32_t)floor((double)t * sc + 0.5)) : (abs_y + t);
                int32_t sr = is_scaled ? (abs_x + (int32_t)floor((double)r * sc + 0.5)) : (abs_x + r);
                int32_t sb = is_scaled ? (abs_y + (int32_t)floor((double)b * sc + 0.5)) : (abs_y + b);
                hs_clip_and_merge(ctx, sl, st, sr, sb, &ux0, &uy0, &ux1, &uy1, &best_area, &best_cx, &best_cy);
            }
        } else {
            int32_t l = (int32_t)mem_r32(shape + 0x08u);
            int32_t t = (int32_t)mem_r32(shape + 0x0cu);
            int32_t r = (int32_t)mem_r32(shape + 0x10u);
            int32_t b = (int32_t)mem_r32(shape + 0x14u);
            hs_clip_and_merge(ctx, abs_x + l, abs_y + t, abs_x + r, abs_y + b,
                              &ux0, &uy0, &ux1, &uy1, &best_area, &best_cx, &best_cy);
        }
    } else {
        int32_t w  = (int32_t)mem_r32(obj + 0x44u);
        int32_t h  = (int32_t)mem_r32(obj + 0x48u);
        int32_t ox = (int32_t)mem_r32(obj + 0x4cu);
        int32_t oy = (int32_t)mem_r32(obj + 0x50u);
        int32_t sl, st, sr, sb;
        if (!is_scaled) {
            sl = abs_x + ox;
            st = abs_y + oy;
            sr = sl + w;
            sb = st + h;
        } else {
            sl = abs_x + (int32_t)floor((double)ox * sc + 0.001);
            st = abs_y + (int32_t)floor((double)oy * sc + 0.001);
            sr = sl + (int32_t)ceil((double)w * sc - 0.001) + 1;
            sb = st + (int32_t)ceil((double)h * sc - 0.001) + 1;
        }
        hs_clip_and_merge(ctx, sl, st, sr, sb, &ux0, &uy0, &ux1, &uy1, &best_area, &best_cx, &best_cy);
    }

    if (best_area <= 0 || ux1 <= ux0 || uy1 <= uy0) return 0;
    *out_x0 = ux0;
    *out_y0 = uy0;
    *out_x1 = ux1;
    *out_y1 = uy1;
    *out_ax = best_cx;
    *out_ay = best_cy;
    return 1;
}

static void hs_walk_container(HsWalkCtx *ctx, uint32_t container,
                              int32_t parent_x, int32_t parent_y, int depth) {
    if (depth > 6 || !hs_va_ok(container, 0xb4u)) return;
    int steps = 0;
    for (uint32_t child = mem_r32(container + 0x7cu);
         child && steps < 2048;
         child = mem_r32(child + 0x04u), steps++) {
        if (!hs_va_ok(child, 0x78u)) break;
        if (child == ctx->skip_ui1 || child == ctx->skip_ui2 || child == ctx->player) continue;
        if (mem_r8(child + 0x28u) == 0u || mem_r8(child + 0x35u) != 0u ||
            (mem_r32(child + 0x38u) & 1u) == 0u) {
            continue;
        }

        uint32_t vt = mem_r32(child);
        if (vt == 0x00507d3cu || vt == 0x004fd9dcu) {
            if (hs_va_ok(child, 0xb4u) && mem_r8(child + 0xb0u) == 0u) {
                hs_walk_container(ctx, child,
                                  parent_x + (int32_t)mem_r32(child + 0x3cu),
                                  parent_y + (int32_t)mem_r32(child + 0x40u),
                                  depth + 1);
            }
            continue;
        }

        if ((mem_r32(child + 0x60u) & ctx->layer_mask) == 0u) continue;
        uint32_t poff = hs_get_props_off(vt);
        if (!poff || !hs_va_ok(child, poff + 25u * 8u)) continue;

        uint32_t hs = hs_resolve_hotspot(ctx, child, vt, poff);
        char label[128];
        hs_lookup_label(child, poff, hs, label, sizeof(label));

        if (ctx->is_location) {
            if (!hs) continue;
            if (label[0] == '\0' && (int32_t)mem_r32(hs + 0x80u) <= 0 && !hs_has_any_verb(hs)) {
                continue;
            }
        } else {
            if (!hs && label[0] == '\0') continue;
        }

        int32_t abs_x = parent_x + (int32_t)mem_r32(child + 0x3cu);
        int32_t abs_y = parent_y + (int32_t)mem_r32(child + 0x40u);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0, ax = 0, ay = 0;
        if (!hs_compute_bounds(ctx, child, vt, abs_x, abs_y, &x0, &y0, &x1, &y1, &ax, &ay)) {
            continue;
        }

        int kind = hs_classify_kind(child, vt, poff, hs);
        int is_sprite = (vt != 0x005070f4u);

        uint32_t kg = 0xFFFFFFFFu;
        int32_t ki = -1;
        hs_read_prop(child, poff, 2, &kg, &ki);
        hs_resolve_id(&kg, &ki, 2);
        if (ki < 0 && hs && hs != child && hs_va_ok(hs, 0xa8u + 25u * 8u)) {
            hs_read_prop(hs, 0xa8u, 2, &kg, &ki);
            hs_resolve_id(&kg, &ki, 2);
        }
        uint32_t key_hs = (ki >= 0) ? 0u : (hs ? hs : child);
        if (ki < 0) kg = 0xFFFFFFFFu;

        int dup = -1;
        for (int k = 0; k < ctx->count; k++) {
            if (ki >= 0) {
                if (ctx->keys[k].i == ki && ctx->keys[k].g == kg) { dup = k; break; }
            } else if (ctx->keys[k].i < 0 && ctx->keys[k].hs_va == key_hs) {
                dup = k; break;
            }
        }

        if (dup >= 0) {
            GameHotspot *dst = &ctx->out[dup];
            if (dst->label[0] == '\0' && label[0] != '\0') {
                snprintf(dst->label, sizeof(dst->label), "%s", label);
            }
            if (kind == 1 || (kind == 2 && dst->kind == 0)) dst->kind = kind;
            if (!ctx->keys[dup].from_sprite && is_sprite) {
                dst->x = ax; dst->y = ay;
                dst->x0 = x0; dst->y0 = y0; dst->x1 = x1; dst->y1 = y1;
                ctx->keys[dup].from_sprite = 1;
            }
        } else if (ctx->count < ctx->max_out && ctx->count < 64) {
            int idx = ctx->count++;
            ctx->keys[idx].g = kg;
            ctx->keys[idx].i = ki;
            ctx->keys[idx].hs_va = key_hs;
            ctx->keys[idx].from_sprite = is_sprite;
            GameHotspot *dst = &ctx->out[idx];
            dst->x = ax; dst->y = ay;
            dst->x0 = x0; dst->y0 = y0; dst->x1 = x1; dst->y1 = y1;
            dst->kind = kind;
            snprintf(dst->label, sizeof(dst->label), "%s", label);
        }
    }
}

int game_collect_room_hotspots(GameHotspot *out, int max_out) {
    if (!out || max_out <= 0 || !g_mem || win32_avi_is_playing()) return 0;

    uint32_t mg = mem_r32(0x00527850u);
    if (!hs_va_ok(mg, 0x100u) || mem_r8(mg + 0x00bcu) != 0u) return 0;

    uint32_t scene = mem_r32(mg + 0xc0u);
    if (!hs_va_ok(scene, 0xd0u)) return 0;
    uint32_t scene_vt = mem_r32(scene);
    int is_location = (scene == mem_r32(mg + 0xc4u) && scene_vt == 0x00505054u);
    int is_minigame = (scene == mem_r32(mg + 0xc8u) && scene_vt == 0x005052acu);
    if (!is_location && !is_minigame) return 0;

    if (mem_r32(scene + 0xa4u) != 0u || (mem_r32(scene + 0xa0u) & 0x000ff000u) != 0u) return 0;
    uint32_t dlg = mem_r32(scene + 0x6cu);
    if (hs_va_ok(dlg, 0x84u) && mem_r32(dlg + 0x80u) != 0u) return 0;

    uint32_t rd = mem_r32(scene + 0x1cu);
    if (!hs_va_ok(rd, 0x130u) || mem_r32(rd) != 0x00507b30u) return 0;
    if (mem_r8(rd + 0xb0u) != 0u || mem_r8(rd + 0x28u) == 0u ||
        mem_r8(rd + 0x35u) != 0u || (mem_r32(rd + 0x38u) & 1u) == 0u) {
        return 0;
    }

    uint32_t scroll_root = mem_r32(rd + 0x114u);
    if (!hs_va_ok(scroll_root, 0xb4u)) return 0;
    if (mem_r8(scroll_root + 0xb0u) != 0u || mem_r8(scroll_root + 0x28u) == 0u ||
        mem_r8(scroll_root + 0x35u) != 0u || (mem_r32(scroll_root + 0x38u) & 1u) == 0u) {
        return 0;
    }

    int32_t rd_x = 0, rd_y = 0;
    uint32_t p = rd;
    for (int guard = 0; p && guard < 32 && hs_va_ok(p, 0x44u); guard++) {
        rd_x += (int32_t)mem_r32(p + 0x3cu);
        rd_y += (int32_t)mem_r32(p + 0x40u);
        p = mem_r32(p + 0x0cu);
    }

    int32_t clip_l = rd_x + (int32_t)mem_r32(rd + 0xb8u);
    int32_t clip_t = rd_y + (int32_t)mem_r32(rd + 0xbcu);
    int32_t clip_r = rd_x + (int32_t)mem_r32(rd + 0xc0u);
    int32_t clip_b = rd_y + (int32_t)mem_r32(rd + 0xc4u);
    if (clip_l < 0) clip_l = 0;
    if (clip_t < 0) clip_t = 0;
    if (clip_r > 800) clip_r = 800;
    if (clip_b > 600) clip_b = 600;
    if (clip_l >= clip_r || clip_t >= clip_b) return 0;

    HsWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    ctx.max_out = (max_out > 64) ? 64 : max_out;
    ctx.clip_l = clip_l;
    ctx.clip_t = clip_t;
    ctx.clip_r = clip_r;
    ctx.clip_b = clip_b;
    ctx.skip_ui1 = mem_r32(rd + 0x118u);
    ctx.skip_ui2 = mem_r32(rd + 0x11cu);
    ctx.layer_mask = mem_r32(0x0051ac78u);
    ctx.player = mem_r32(0x0052761cu);
    ctx.hotspots_ptr = mem_r32(rd + 0xd4u);
    ctx.hotspot_count = (int32_t)mem_r32(rd + 0xd8u);
    ctx.is_location = is_location;

    int32_t sr_x = rd_x + (int32_t)mem_r32(scroll_root + 0x3cu);
    int32_t sr_y = rd_y + (int32_t)mem_r32(scroll_root + 0x40u);
    hs_walk_container(&ctx, scroll_root, sr_x, sr_y, 0);
    return ctx.count;
}

int game_is_main_menu_active(void) {
    if (!g_mem || win32_avi_is_playing()) return 0;
    uint32_t mg = mem_r32(0x00527850u);
    if (!hs_va_ok(mg, 0x100u) || mem_r8(mg + 0x00bcu) != 0u) return 0;
    uint32_t scene = mem_r32(mg + 0xc0u);
    if (!hs_va_ok(scene, 0x30u)) return 0;
    if (scene != mem_r32(mg + 0xccu) || mem_r32(scene) != 0x00505504u) return 0;
    if (mem_r32(scene + 0x24u) != 0x00010100u) return 0;
    uint32_t rd = mem_r32(scene + 0x1cu);
    if (!hs_va_ok(rd, 0x40u)) return 0;
    if (mem_r8(rd + 0x28u) == 0u || mem_r8(rd + 0x35u) != 0u ||
        (mem_r32(rd + 0x38u) & 1u) == 0u) {
        return 0;
    }
    return 1;
}
