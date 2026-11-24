#ifndef MHA2_CPU_H
#define MHA2_CPU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* 1.5 GiB flat 32-bit guest address space (demand-paged via mmap; all VAs < 0x7FFFFFFF).
 * The full 4 GiB window (+ guard for 8-byte accesses at 0xFFFFFFFF) is reserved
 * PROT_NONE so wild guest pointers fault (runtime/crash_handler.c) instead of
 * touching unrelated host memory; only GUEST_MEM_SIZE is committed read/write. */
#define GUEST_MEM_SIZE   (1536u * 1024u * 1024u)
#define GUEST_RESERVE_SIZE    ((size_t)1 << 32 | (size_t)0x10000u)
#define GUEST_NULL_GUARD_SIZE 0x10000u  /* protected under --strict-mem */
#define GUEST_TEXT_START 0x00401000u
#define GUEST_TEXT_END   0x004fd000u
#define GUEST_TEXT_SIZE  (GUEST_TEXT_END - GUEST_TEXT_START)

/* Synthetic address spaces for HLE dispatch */
#define HLE_COM_BASE     0xFE000000u
#define HLE_WIN32_BASE   0xFF000000u
#define HLE_RET_SENTINEL 0xFD000000u

/* Reserved guest memory layout */
#define GUEST_COM_VTABLES_BASE 0x00800000u  /* 64 KB for COM vtables & static strings */
#define GUEST_TEB_BASE         0x00900000u  /* 64 KB for thread TEBs (4 KB per thread) */
#define GUEST_HEAP_BASE        0x01000000u  /* ~1.42 GB virtual guest heap & surface/audio buffers */
#define GUEST_HEAP_END         0x5A000000u
#define GUEST_STACK_BASE       0x5B000000u  /* 2 MB stacks per thread up to 0x5F800000 */
#define GUEST_STACK_SIZE       (2u * 1024u * 1024u)
#define GUEST_HOST_THREAD_STACK ((size_t)32u * 1024u * 1024u) /* native C stack per guest thread */

extern uint8_t *g_mem;

typedef struct CpuCtx {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t cf, zf, sf, of, pf, df;
    uint32_t teb_addr;
    uint32_t thread_id;

    /* x87 FPU */
    long double st[8];
    int fpu_top;
    uint16_t fpu_sw;
    uint16_t fpu_cw;
} CpuCtx;

typedef void (*RecompFn)(CpuCtx *ctx);

extern RecompFn g_code_table[GUEST_TEXT_SIZE];

void dispatch_call(CpuCtx *ctx, uint32_t addr);
void win32_dispatch(CpuCtx *ctx, uint32_t import_id);
void com_dispatch(CpuCtx *ctx, uint32_t iface_id, uint32_t slot);
uint32_t guest_call_stdcall(CpuCtx *ctx, uint32_t fn_addr, int nargs, const uint32_t *args);
uint32_t guest_call_cdecl(CpuCtx *ctx, uint32_t fn_addr, int nargs, const uint32_t *args);

/* Blocking acquire of an original-game InterlockedIncrement-style spinlock counter
 * (-1 = free, 0 = held). Emitted by recomp/lift.py (ATOMIC_COUNTER_GLOBALS) in place
 * of the original forced lock-steal; implemented in runtime/guest_sync.c. */
void guest_spin_acquire(uint32_t counter_va);
/* Re-entrant owner-TID lock on top of a spinlock counter (original MetaSpinLock).
 * enter() returns 1 if this call acquired it (and must pass that to leave()). */
int guest_owner_lock_enter(uint32_t counter_va, uint32_t owner_va, uint32_t tid);
void guest_owner_lock_leave(uint32_t counter_va, uint32_t owner_va, int held);

/* Fatal signal reporter (runtime/crash_handler.c). thread_init() registers the
 * calling thread's guest context and alternate signal stack. */
void crash_handler_install(void);
void crash_handler_thread_init(CpuCtx *ctx);
void crash_handler_thread_exit(void);
/* Guest address space & heap allocator (runtime/guest_mem.c) */
void     runtime_mem_init(void);
/* Protect the guest NULL page (debug: --strict-mem). Returns 0 on success. */
int      runtime_mem_protect_null_page(void);
uint32_t guest_alloc(uint32_t size);
uint32_t guest_realloc(uint32_t addr, uint32_t new_size);
void     guest_free(uint32_t addr);
uint32_t guest_alloc_size(uint32_t addr);
uint32_t guest_alloc_static(const void *data, uint32_t size);
uint32_t guest_alloc_cstr(const char *s);

/* Memory accessors */
static inline void *mem_ptr(uint32_t addr) {
    return (void *)(g_mem + addr);
}

static inline uint8_t mem_r8(uint32_t addr) {
    return g_mem[addr];
}
static inline uint16_t mem_r16(uint32_t addr) {
    uint16_t v;
    memcpy(&v, g_mem + addr, 2);
    return v;
}
static inline uint32_t mem_r32(uint32_t addr) {
    uint32_t v;
    memcpy(&v, g_mem + addr, 4);
    return v;
}
static inline uint64_t mem_r64(uint32_t addr) {
    uint64_t v;
    memcpy(&v, g_mem + addr, 8);
    return v;
}
static inline float mem_rf32(uint32_t addr) {
    float v;
    memcpy(&v, g_mem + addr, 4);
    return v;
}
static inline double mem_rf64(uint32_t addr) {
    double v;
    memcpy(&v, g_mem + addr, 8);
    return v;
}
static inline long double mem_rf80(uint32_t addr) {
    long double v = 0.0L;
    memcpy(&v, g_mem + addr, 10);
    return v;
}

static inline void mem_w8(uint32_t addr, uint8_t v) {
    g_mem[addr] = v;
}
static inline void mem_w16(uint32_t addr, uint16_t v) {
    memcpy(g_mem + addr, &v, 2);
}
static inline void mem_w32(uint32_t addr, uint32_t v) {
    memcpy(g_mem + addr, &v, 4);
}
static inline void mem_w64(uint32_t addr, uint64_t v) {
    memcpy(g_mem + addr, &v, 8);
}
static inline void mem_wf32(uint32_t addr, float v) {
    memcpy(g_mem + addr, &v, 4);
}
static inline void mem_wf64(uint32_t addr, double v) {
    memcpy(g_mem + addr, &v, 8);
}
static inline void mem_wf80(uint32_t addr, long double v) {
    memcpy(g_mem + addr, &v, 10);
}

/* Push / Pop */
static inline void push32(CpuCtx *c, uint32_t v) {
    c->esp -= 4;
    mem_w32(c->esp, v);
}
static inline uint32_t pop32(CpuCtx *c) {
    uint32_t v = mem_r32(c->esp);
    c->esp += 4;
    return v;
}
static inline void push16(CpuCtx *c, uint16_t v) {
    c->esp -= 2;
    mem_w16(c->esp, v);
}
static inline uint16_t pop16(CpuCtx *c) {
    uint16_t v = mem_r16(c->esp);
    c->esp += 2;
    return v;
}

/* Sub-register accessors */
static inline uint8_t r8_lo(uint32_t r) { return (uint8_t)(r & 0xFFu); }
static inline uint8_t r8_hi(uint32_t r) { return (uint8_t)((r >> 8) & 0xFFu); }
static inline uint16_t r16(uint32_t r)  { return (uint16_t)(r & 0xFFFFu); }
static inline void w8_lo(uint32_t *r, uint8_t v) { *r = (*r & ~0xFFu) | (uint32_t)v; }
static inline void w8_hi(uint32_t *r, uint8_t v) { *r = (*r & ~0xFF00u) | ((uint32_t)v << 8); }
static inline void w16(uint32_t *r, uint16_t v)  { *r = (*r & ~0xFFFFu) | (uint32_t)v; }

/* Parity helper */
static inline uint32_t parity8(uint32_t v) {
    return (uint32_t)(!__builtin_parity(v & 0xFFu));
}

/* ALU helpers */
static inline uint8_t alu_add8(CpuCtx *c, uint8_t a, uint8_t b) {
    uint32_t r = (uint32_t)a + (uint32_t)b;
    uint8_t res = (uint8_t)r;
    c->cf = (r >> 8) & 1u;
    c->of = (((a ^ res) & (b ^ res)) >> 7) & 1u;
    c->sf = (res >> 7) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint16_t alu_add16(CpuCtx *c, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a + (uint32_t)b;
    uint16_t res = (uint16_t)r;
    c->cf = (r >> 16) & 1u;
    c->of = (((a ^ res) & (b ^ res)) >> 15) & 1u;
    c->sf = (res >> 15) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint32_t alu_add32(CpuCtx *c, uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a + (uint64_t)b;
    uint32_t res = (uint32_t)r;
    c->cf = (uint32_t)((r >> 32) & 1u);
    c->of = (((a ^ res) & (b ^ res)) >> 31) & 1u;
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}

static inline uint8_t alu_adc8(CpuCtx *c, uint8_t a, uint8_t b) {
    uint32_t r = (uint32_t)a + (uint32_t)b + (c->cf & 1u);
    uint8_t res = (uint8_t)r;
    c->cf = (r >> 8) & 1u;
    c->of = (((a ^ res) & (b ^ res)) >> 7) & 1u;
    c->sf = (res >> 7) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint16_t alu_adc16(CpuCtx *c, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a + (uint32_t)b + (c->cf & 1u);
    uint16_t res = (uint16_t)r;
    c->cf = (r >> 16) & 1u;
    c->of = (((a ^ res) & (b ^ res)) >> 15) & 1u;
    c->sf = (res >> 15) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint32_t alu_adc32(CpuCtx *c, uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a + (uint64_t)b + (uint64_t)(c->cf & 1u);
    uint32_t res = (uint32_t)r;
    c->cf = (uint32_t)((r >> 32) & 1u);
    c->of = (((a ^ res) & (b ^ res)) >> 31) & 1u;
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}

static inline uint8_t alu_sub8(CpuCtx *c, uint8_t a, uint8_t b) {
    uint8_t res = (uint8_t)(a - b);
    c->cf = (a < b);
    c->of = (((a ^ b) & (a ^ res)) >> 7) & 1u;
    c->sf = (res >> 7) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint16_t alu_sub16(CpuCtx *c, uint16_t a, uint16_t b) {
    uint16_t res = (uint16_t)(a - b);
    c->cf = (a < b);
    c->of = (((a ^ b) & (a ^ res)) >> 15) & 1u;
    c->sf = (res >> 15) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint32_t alu_sub32(CpuCtx *c, uint32_t a, uint32_t b) {
    uint32_t res = a - b;
    c->cf = (a < b);
    c->of = (((a ^ b) & (a ^ res)) >> 31) & 1u;
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}

static inline uint8_t alu_sbb8(CpuCtx *c, uint8_t a, uint8_t b) {
    uint32_t cin = c->cf & 1u;
    uint32_t r = (uint32_t)a - (uint32_t)b - cin;
    uint8_t res = (uint8_t)r;
    c->cf = (r >> 31) & 1u;
    c->of = (((a ^ b) & (a ^ res)) >> 7) & 1u;
    c->sf = (res >> 7) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint16_t alu_sbb16(CpuCtx *c, uint16_t a, uint16_t b) {
    uint32_t cin = c->cf & 1u;
    uint32_t r = (uint32_t)a - (uint32_t)b - cin;
    uint16_t res = (uint16_t)r;
    c->cf = (r >> 31) & 1u;
    c->of = (((a ^ b) & (a ^ res)) >> 15) & 1u;
    c->sf = (res >> 15) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint32_t alu_sbb32(CpuCtx *c, uint32_t a, uint32_t b) {
    uint64_t cin = c->cf & 1u;
    uint64_t r = (uint64_t)a - (uint64_t)b - cin;
    uint32_t res = (uint32_t)r;
    c->cf = (uint32_t)((r >> 63) & 1u);
    c->of = (((a ^ b) & (a ^ res)) >> 31) & 1u;
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}

static inline uint8_t alu_inc8(CpuCtx *c, uint8_t a) {
    uint8_t res = (uint8_t)(a + 1u);
    c->of = (a == 0x7Fu);
    c->sf = (res >> 7) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint16_t alu_inc16(CpuCtx *c, uint16_t a) {
    uint16_t res = (uint16_t)(a + 1u);
    c->of = (a == 0x7FFFu);
    c->sf = (res >> 15) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint32_t alu_inc32(CpuCtx *c, uint32_t a) {
    uint32_t res = a + 1u;
    c->of = (a == 0x7FFFFFFFu);
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}

static inline uint8_t alu_dec8(CpuCtx *c, uint8_t a) {
    uint8_t res = (uint8_t)(a - 1u);
    c->of = (a == 0x80u);
    c->sf = (res >> 7) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint16_t alu_dec16(CpuCtx *c, uint16_t a) {
    uint16_t res = (uint16_t)(a - 1u);
    c->of = (a == 0x8000u);
    c->sf = (res >> 15) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint32_t alu_dec32(CpuCtx *c, uint32_t a) {
    uint32_t res = a - 1u;
    c->of = (a == 0x80000000u);
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}

static inline uint8_t alu_neg8(CpuCtx *c, uint8_t a) {
    uint8_t res = (uint8_t)(0u - a);
    c->cf = (a != 0);
    c->of = (a == 0x80u);
    c->sf = (res >> 7) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint16_t alu_neg16(CpuCtx *c, uint16_t a) {
    uint16_t res = (uint16_t)(0u - a);
    c->cf = (a != 0);
    c->of = (a == 0x8000u);
    c->sf = (res >> 15) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint32_t alu_neg32(CpuCtx *c, uint32_t a) {
    uint32_t res = 0u - a;
    c->cf = (a != 0);
    c->of = (a == 0x80000000u);
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}

static inline uint8_t alu_logic8(CpuCtx *c, uint8_t res) {
    c->cf = 0; c->of = 0;
    c->sf = (res >> 7) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint16_t alu_logic16(CpuCtx *c, uint16_t res) {
    c->cf = 0; c->of = 0;
    c->sf = (res >> 15) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint32_t alu_logic32(CpuCtx *c, uint32_t res) {
    c->cf = 0; c->of = 0;
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}

/* Shifts & Rotates */
static inline uint8_t alu_shl8(CpuCtx *c, uint8_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint8_t res = (cnt < 8) ? (uint8_t)(a << cnt) : 0;
    c->cf = (cnt <= 8) ? ((a >> (8 - cnt)) & 1u) : 0;
    if (cnt == 1) c->of = ((res >> 7) & 1u) ^ c->cf;
    c->sf = (res >> 7) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint16_t alu_shl16(CpuCtx *c, uint16_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint16_t res = (cnt < 16) ? (uint16_t)(a << cnt) : 0;
    c->cf = (cnt <= 16) ? ((a >> (16 - cnt)) & 1u) : 0;
    if (cnt == 1) c->of = ((res >> 15) & 1u) ^ c->cf;
    c->sf = (res >> 15) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint32_t alu_shl32(CpuCtx *c, uint32_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint32_t res = a << cnt;
    c->cf = (a >> (32 - cnt)) & 1u;
    if (cnt == 1) c->of = ((res >> 31) & 1u) ^ c->cf;
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}

static inline uint8_t alu_shr8(CpuCtx *c, uint8_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint8_t res = (cnt < 8) ? (uint8_t)(a >> cnt) : 0;
    c->cf = (cnt <= 8) ? ((a >> (cnt - 1)) & 1u) : 0;
    if (cnt == 1) c->of = (a >> 7) & 1u;
    c->sf = (res >> 7) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint16_t alu_shr16(CpuCtx *c, uint16_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint16_t res = (cnt < 16) ? (uint16_t)(a >> cnt) : 0;
    c->cf = (cnt <= 16) ? ((a >> (cnt - 1)) & 1u) : 0;
    if (cnt == 1) c->of = (a >> 15) & 1u;
    c->sf = (res >> 15) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint32_t alu_shr32(CpuCtx *c, uint32_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint32_t res = a >> cnt;
    c->cf = (a >> (cnt - 1)) & 1u;
    if (cnt == 1) c->of = (a >> 31) & 1u;
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}

static inline uint8_t alu_sar8(CpuCtx *c, uint8_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    int8_t sa = (int8_t)a;
    uint8_t res = (cnt < 8) ? (uint8_t)(sa >> cnt) : (uint8_t)(sa >> 7);
    c->cf = (cnt < 8) ? (((uint32_t)sa >> (cnt - 1)) & 1u) : ((a >> 7) & 1u);
    if (cnt == 1) c->of = 0;
    c->sf = (res >> 7) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint16_t alu_sar16(CpuCtx *c, uint16_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    int16_t sa = (int16_t)a;
    uint16_t res = (cnt < 16) ? (uint16_t)(sa >> cnt) : (uint16_t)(sa >> 15);
    c->cf = (cnt < 16) ? (((uint32_t)sa >> (cnt - 1)) & 1u) : ((a >> 15) & 1u);
    if (cnt == 1) c->of = 0;
    c->sf = (res >> 15) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint32_t alu_sar32(CpuCtx *c, uint32_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    int32_t sa = (int32_t)a;
    uint32_t res = (uint32_t)(sa >> cnt);
    c->cf = (a >> (cnt - 1)) & 1u;
    if (cnt == 1) c->of = 0;
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}

static inline uint32_t alu_rol32(CpuCtx *c, uint32_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint32_t res = (a << cnt) | (a >> (32 - cnt));
    c->cf = res & 1u;
    if (cnt == 1) c->of = ((res >> 31) & 1u) ^ c->cf;
    return res;
}
static inline uint32_t alu_ror32(CpuCtx *c, uint32_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint32_t res = (a >> cnt) | (a << (32 - cnt));
    c->cf = (res >> 31) & 1u;
    if (cnt == 1) c->of = ((res >> 31) ^ (res >> 30)) & 1u;
    return res;
}
static inline uint8_t alu_rol8(CpuCtx *c, uint8_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint32_t r = cnt & 7u;
    uint8_t res = r ? (uint8_t)((a << r) | (a >> (8u - r))) : a;
    c->cf = res & 1u;
    if (cnt == 1) c->of = ((res >> 7) & 1u) ^ c->cf;
    return res;
}
static inline uint8_t alu_ror8(CpuCtx *c, uint8_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint32_t r = cnt & 7u;
    uint8_t res = r ? (uint8_t)((a >> r) | (a << (8u - r))) : a;
    c->cf = (res >> 7) & 1u;
    if (cnt == 1) c->of = ((res >> 7) ^ (res >> 6)) & 1u;
    return res;
}
static inline uint16_t alu_rol16(CpuCtx *c, uint16_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint32_t r = cnt & 15u;
    uint16_t res = r ? (uint16_t)((a << r) | (a >> (16u - r))) : a;
    c->cf = res & 1u;
    if (cnt == 1) c->of = ((res >> 15) & 1u) ^ c->cf;
    return res;
}
static inline uint16_t alu_ror16(CpuCtx *c, uint16_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint32_t r = cnt & 15u;
    uint16_t res = r ? (uint16_t)((a >> r) | (a << (16u - r))) : a;
    c->cf = (res >> 15) & 1u;
    if (cnt == 1) c->of = ((res >> 15) ^ (res >> 14)) & 1u;
    return res;
}
static inline uint32_t alu_rcr32(CpuCtx *c, uint32_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint32_t cf = c->cf & 1u;
    uint32_t res = a;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t new_cf = res & 1u;
        res = (res >> 1) | (cf << 31);
        cf = new_cf;
    }
    c->cf = cf;
    if (cnt == 1) c->of = ((res >> 31) ^ (res >> 30)) & 1u;
    return res;
}
static inline uint32_t alu_rcl32(CpuCtx *c, uint32_t a, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return a;
    uint32_t cf = c->cf & 1u;
    uint32_t res = a;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t new_cf = (res >> 31) & 1u;
        res = (res << 1) | cf;
        cf = new_cf;
    }
    c->cf = cf;
    if (cnt == 1) c->of = ((res >> 31) & 1u) ^ cf;
    return res;
}
static inline uint32_t alu_shld32(CpuCtx *c, uint32_t dst, uint32_t src, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return dst;
    uint32_t res = (dst << cnt) | (src >> (32 - cnt));
    c->cf = (dst >> (32 - cnt)) & 1u;
    if (cnt == 1) c->of = ((res >> 31) & 1u) ^ ((dst >> 31) & 1u);
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}
static inline uint32_t alu_shrd32(CpuCtx *c, uint32_t dst, uint32_t src, uint32_t cnt) {
    cnt &= 31u;
    if (cnt == 0) return dst;
    uint32_t res = (dst >> cnt) | (src << (32 - cnt));
    c->cf = (dst >> (cnt - 1)) & 1u;
    if (cnt == 1) c->of = ((res >> 31) & 1u) ^ ((dst >> 31) & 1u);
    c->sf = (res >> 31) & 1u;
    c->zf = (res == 0);
    c->pf = parity8(res);
    return res;
}

/* Mul / Div */
static inline void alu_mul8(CpuCtx *c, uint8_t b) {
    uint16_t r = (uint16_t)((uint8_t)c->eax) * (uint16_t)b;
    w16(&c->eax, r);
    c->cf = c->of = ((r >> 8) != 0);
}
static inline void alu_mul16(CpuCtx *c, uint16_t b) {
    uint32_t r = (uint32_t)((uint16_t)c->eax) * (uint32_t)b;
    w16(&c->eax, (uint16_t)r);
    w16(&c->edx, (uint16_t)(r >> 16));
    c->cf = c->of = ((r >> 16) != 0);
}
static inline void alu_mul32(CpuCtx *c, uint32_t b) {
    uint64_t r = (uint64_t)c->eax * (uint64_t)b;
    c->eax = (uint32_t)r;
    c->edx = (uint32_t)(r >> 32);
    c->cf = c->of = (c->edx != 0);
}
static inline void alu_imul1_8(CpuCtx *c, uint8_t b) {
    int16_t r = (int16_t)(int8_t)c->eax * (int16_t)(int8_t)b;
    w16(&c->eax, (uint16_t)r);
    c->cf = c->of = (r != (int16_t)(int8_t)r);
}
static inline void alu_imul1_16(CpuCtx *c, uint16_t b) {
    int32_t r = (int32_t)(int16_t)c->eax * (int32_t)(int16_t)b;
    w16(&c->eax, (uint16_t)r);
    w16(&c->edx, (uint16_t)((uint32_t)r >> 16));
    c->cf = c->of = (r != (int32_t)(int16_t)r);
}
static inline void alu_imul1_32(CpuCtx *c, uint32_t b) {
    int64_t r = (int64_t)(int32_t)c->eax * (int64_t)(int32_t)b;
    c->eax = (uint32_t)r;
    c->edx = (uint32_t)((uint64_t)r >> 32);
    c->cf = c->of = (r != (int64_t)(int32_t)r);
}
static inline uint16_t alu_imul2_16(CpuCtx *c, uint16_t a, uint16_t b) {
    int32_t r = (int32_t)(int16_t)a * (int32_t)(int16_t)b;
    c->cf = c->of = (r != (int32_t)(int16_t)r);
    return (uint16_t)r;
}
static inline uint32_t alu_imul2_32(CpuCtx *c, uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    c->cf = c->of = (r != (int64_t)(int32_t)r);
    return (uint32_t)r;
}
/* Flag-free ALU result helpers, emitted by recomp/lift.py when flag liveness
 * analysis proves none of the instruction's flag outputs is ever read. Results must
 * match the alu_* helpers exactly (verified in tests/test_cpu_semantics.c). */
#define NF_SHIFTS(B, UT, ST)                                                           \
    static inline UT nf_shl##B(UT a, uint32_t n) {                                     \
        n &= 31u; return (n < B) ? (UT)((uint32_t)a << n) : (UT)0; }                    \
    static inline UT nf_shr##B(UT a, uint32_t n) {                                     \
        n &= 31u; return (n < B) ? (UT)((uint32_t)a >> n) : (UT)0; }                    \
    static inline UT nf_sar##B(UT a, uint32_t n) {                                     \
        n &= 31u; return (UT)((int32_t)(ST)a >> (n < B ? n : (uint32_t)(B - 1))); }
#define NF_ARITH(B, UT)                                                               \
    static inline UT nf_add##B(UT a, UT b) { return (UT)(a + b); }                    \
    static inline UT nf_sub##B(UT a, UT b) { return (UT)(a - b); }                    \
    static inline UT nf_inc##B(UT a) { return (UT)(a + 1u); }                         \
    static inline UT nf_dec##B(UT a) { return (UT)(a - 1u); }                         \
    static inline UT nf_neg##B(UT a) { return (UT)(0u - a); }
NF_ARITH(8, uint8_t)
NF_ARITH(16, uint16_t)
NF_ARITH(32, uint32_t)
#undef NF_ARITH
static inline uint16_t nf_imul2_16(uint16_t a, uint16_t b) { return (uint16_t)((uint32_t)a * (uint32_t)b); }
static inline uint32_t nf_imul2_32(uint32_t a, uint32_t b) { return a * b; }
NF_SHIFTS(8, uint8_t, int8_t)
NF_SHIFTS(16, uint16_t, int16_t)
#undef NF_SHIFTS
static inline uint32_t nf_shl32(uint32_t a, uint32_t n) { return a << (n & 31u); }
static inline uint32_t nf_shr32(uint32_t a, uint32_t n) { return a >> (n & 31u); }
static inline uint32_t nf_sar32(uint32_t a, uint32_t n) { return (uint32_t)((int32_t)a >> (n & 31u)); }

/* Debug lift mode (MHA2_LIFT_FLAGCHECK=1 make lift): flags the liveness analysis
 * declared dead are poisoned with 2 and every real flag read checks for poison,
 * so a wrong elimination aborts at the reading instruction. */
#define FLAG_POISON 2u
static inline void flag_check(CpuCtx *c, uint32_t mask, uint32_t va) {
    if (((mask & 1u) && c->cf > 1u) || ((mask & 2u) && c->zf > 1u) || ((mask & 4u) && c->sf > 1u) ||
        ((mask & 8u) && c->of > 1u) || ((mask & 16u) && c->pf > 1u)) {
        fprintf(stderr, "FATAL: flag liveness violation: poisoned flag read at guest 0x%08x "
                "(mask=%u cf=%u zf=%u sf=%u of=%u pf=%u)\n", va, mask, c->cf, c->zf, c->sf, c->of, c->pf);
        abort();
    }
}

/* div/idiv raise #DE on x86 for a zero divisor or a quotient that does not fit the
 * destination; the 2005 code never handles it, so report it like Windows would
 * crash (runtime/crash_handler.c) instead of continuing with a truncated result. */
__attribute__((noreturn, cold)) void guest_divide_error(CpuCtx *c, int bits, int is_signed,
                                                        uint64_t dividend_hi, uint64_t dividend_lo,
                                                        uint32_t divisor);

static inline void alu_div8(CpuCtx *c, uint8_t b) {
    uint16_t num = (uint16_t)c->eax;
    if (__builtin_expect(b == 0 || num / b > 0xFFu, 0)) guest_divide_error(c, 8, 0, 0, num, b);
    w8_lo(&c->eax, (uint8_t)(num / b));
    w8_hi(&c->eax, (uint8_t)(num % b));
}
static inline void alu_div16(CpuCtx *c, uint16_t b) {
    uint32_t num = ((uint32_t)(uint16_t)c->edx << 16) | (uint16_t)c->eax;
    if (__builtin_expect(b == 0 || num / b > 0xFFFFu, 0)) guest_divide_error(c, 16, 0, 0, num, b);
    w16(&c->eax, (uint16_t)(num / b));
    w16(&c->edx, (uint16_t)(num % b));
}
static inline void alu_div32(CpuCtx *c, uint32_t b) {
    uint64_t num = ((uint64_t)c->edx << 32) | c->eax;
    if (__builtin_expect(b == 0 || c->edx >= b, 0)) guest_divide_error(c, 32, 0, c->edx, c->eax, b);
    c->eax = (uint32_t)(num / b);
    c->edx = (uint32_t)(num % b);
}
static inline void alu_idiv8(CpuCtx *c, uint8_t b) {
    int16_t num = (int16_t)(uint16_t)c->eax;
    int8_t d = (int8_t)b;
    if (__builtin_expect(d == 0, 0)) guest_divide_error(c, 8, 1, 0, (uint16_t)num, b);
    int16_t q = (int16_t)(num / d);
    if (__builtin_expect(q < -128 || q > 127, 0)) guest_divide_error(c, 8, 1, 0, (uint16_t)num, b);
    w8_lo(&c->eax, (uint8_t)q);
    w8_hi(&c->eax, (uint8_t)(num % d));
}
static inline void alu_idiv16(CpuCtx *c, uint16_t b) {
    int32_t num = (int32_t)(((uint32_t)(uint16_t)c->edx << 16) | (uint16_t)c->eax);
    int16_t d = (int16_t)b;
    if (__builtin_expect(d == 0, 0)) guest_divide_error(c, 16, 1, 0, (uint32_t)num, b);
    int64_t q = (int64_t)num / d; /* 64-bit: INT32_MIN / -1 must not trap on the host */
    if (__builtin_expect(q < -32768 || q > 32767, 0)) guest_divide_error(c, 16, 1, 0, (uint32_t)num, b);
    w16(&c->eax, (uint16_t)q);
    w16(&c->edx, (uint16_t)((int64_t)num % d));
}
static inline void alu_idiv32(CpuCtx *c, uint32_t b) {
    int64_t num = (int64_t)(((uint64_t)c->edx << 32) | c->eax);
    int32_t d = (int32_t)b;
    /* d == -1 with num == INT64_MIN would trap in the host division itself */
    if (__builtin_expect(d == 0 || (d == -1 && num == INT64_MIN), 0))
        guest_divide_error(c, 32, 1, c->edx, c->eax, b);
    int64_t q = num / d;
    if (__builtin_expect(q < INT32_MIN || q > INT32_MAX, 0)) guest_divide_error(c, 32, 1, c->edx, c->eax, b);
    c->eax = (uint32_t)q;
    c->edx = (uint32_t)(num % d);
}

/* x87 FPU helpers */
static inline long double *fpu_st_ptr(CpuCtx *c, int i) {
    return &c->st[(c->fpu_top + i) & 7];
}
static inline long double fpu_st(CpuCtx *c, int i) {
    return c->st[(c->fpu_top + i) & 7];
}
static inline void fpu_push(CpuCtx *c, long double v) {
    c->fpu_top = (c->fpu_top - 1) & 7;
    c->st[c->fpu_top] = v;
}
static inline long double fpu_pop(CpuCtx *c) {
    long double v = c->st[c->fpu_top];
    c->fpu_top = (c->fpu_top + 1) & 7;
    return v;
}
static inline void fpu_cmp(CpuCtx *c, long double a, long double b) {
    c->fpu_sw &= (uint16_t)~0x4500u; /* clear C3, C2, C0 */
    if (isnan(a) || isnan(b)) {
        c->fpu_sw |= 0x4500u; /* C3=1, C2=1, C0=1 */
    } else if (a > b) {
        /* C3=0, C2=0, C0=0 */
    } else if (a < b) {
        c->fpu_sw |= 0x0100u; /* C0=1 */
    } else {
        c->fpu_sw |= 0x4000u; /* C3=1 */
    }
}
static inline uint16_t fpu_get_sw(CpuCtx *c) {
    return (uint16_t)((c->fpu_sw & ~0x3800u) | ((c->fpu_top & 7) << 11));
}
static inline long double fpu_round_ld(CpuCtx *c, long double v) {
    uint16_t rc = (c->fpu_cw >> 10) & 3u;
    switch (rc) {
        case 0: return rintl(v);
        case 1: return floorl(v);
        case 2: return ceill(v);
        case 3: return truncl(v);
    }
    return v;
}
/* x87 integer conversion semantics shared by FIST/FISTP and the __ftol patch:
 * NaN or a rounded value outside the destination range yields the "integer
 * indefinite" value (only the sign bit set), never a wrapped/truncated result. */
static inline uint64_t x87_int_indefinite(int bits) {
    return (uint64_t)1 << (bits - 1);
}
static inline uint64_t x87_ld_to_int(long double rounded, int bits) {
    long double lim = (long double)((uint64_t)1 << (bits - 1)); /* 2^(bits-1) exactly */
    if (!(rounded >= -lim && rounded < lim)) return x87_int_indefinite(bits);
    return (uint64_t)(int64_t)rounded;
}
/* FIST/FISTP m16/m32/m64: round ST(0) per FPU control word RC, then convert. */
static inline uint64_t fpu_to_int(CpuCtx *c, long double v, int bits) {
    return x87_ld_to_int(fpu_round_ld(c, v), bits);
}
static inline void fpu_save(CpuCtx *c, uint32_t addr) {
    memset(g_mem + addr, 0, 108);
    mem_w32(addr + 0, c->fpu_cw);
    mem_w32(addr + 4, fpu_get_sw(c));
    /* Save 8 doubles in slots 28..92 (custom compact layout matching fpu_rstor) */
    for (int i = 0; i < 8; i++) {
        double d = (double)fpu_st(c, i);
        mem_wf64(addr + 28 + i * 8, d);
    }
    /* Intel SDM: FSAVE/FNSAVE re-initializes the FPU after saving state */
    c->fpu_cw = 0x037Fu;
    c->fpu_sw = 0;
    c->fpu_top = 0;
}
static inline void fpu_rstor(CpuCtx *c, uint32_t addr) {
    c->fpu_cw = (uint16_t)mem_r32(addr + 0);
    c->fpu_sw = (uint16_t)mem_r32(addr + 4);
    c->fpu_top = (c->fpu_sw >> 11) & 7;
    for (int i = 0; i < 8; i++) {
        *fpu_st_ptr(c, i) = (long double)mem_rf64(addr + 28 + i * 8);
    }
}

/* String instructions */
static inline void str_movsb(CpuCtx *c) {
    int32_t step = c->df ? -1 : 1;
    mem_w8(c->edi, mem_r8(c->esi));
    c->esi += (uint32_t)step; c->edi += (uint32_t)step;
}
static inline void str_movsw(CpuCtx *c) {
    int32_t step = c->df ? -2 : 2;
    mem_w16(c->edi, mem_r16(c->esi));
    c->esi += (uint32_t)step; c->edi += (uint32_t)step;
}
static inline void str_movsd(CpuCtx *c) {
    int32_t step = c->df ? -4 : 4;
    mem_w32(c->edi, mem_r32(c->esi));
    c->esi += (uint32_t)step; c->edi += (uint32_t)step;
}
static inline void rep_movsb(CpuCtx *c) {
    if (!c->df && c->ecx > 0) {
        memmove(g_mem + c->edi, g_mem + c->esi, c->ecx);
        c->esi += c->ecx; c->edi += c->ecx; c->ecx = 0;
    } else {
        while (c->ecx) { str_movsb(c); c->ecx--; }
    }
}
static inline void rep_movsw(CpuCtx *c) {
    while (c->ecx) { str_movsw(c); c->ecx--; }
}
static inline void rep_movsd(CpuCtx *c) {
    if (!c->df && c->ecx > 0) {
        uint32_t n = c->ecx * 4u;
        memmove(g_mem + c->edi, g_mem + c->esi, n);
        c->esi += n; c->edi += n; c->ecx = 0;
    } else {
        while (c->ecx) { str_movsd(c); c->ecx--; }
    }
}
static inline void str_stosb(CpuCtx *c) {
    int32_t step = c->df ? -1 : 1;
    mem_w8(c->edi, (uint8_t)c->eax);
    c->edi += (uint32_t)step;
}
static inline void str_stosw(CpuCtx *c) {
    int32_t step = c->df ? -2 : 2;
    mem_w16(c->edi, (uint16_t)c->eax);
    c->edi += (uint32_t)step;
}
static inline void str_stosd(CpuCtx *c) {
    int32_t step = c->df ? -4 : 4;
    mem_w32(c->edi, c->eax);
    c->edi += (uint32_t)step;
}
static inline void rep_stosb(CpuCtx *c) {
    if (!c->df && c->ecx > 0) {
        memset(g_mem + c->edi, (uint8_t)c->eax, c->ecx);
        c->edi += c->ecx; c->ecx = 0;
    } else {
        while (c->ecx) { str_stosb(c); c->ecx--; }
    }
}
static inline void rep_stosw(CpuCtx *c) {
    while (c->ecx) { str_stosw(c); c->ecx--; }
}
static inline void rep_stosd(CpuCtx *c) {
    while (c->ecx) { str_stosd(c); c->ecx--; }
}
static inline void repe_cmpsb(CpuCtx *c) {
    int32_t step = c->df ? -1 : 1;
    while (c->ecx) {
        alu_sub8(c, mem_r8(c->esi), mem_r8(c->edi));
        c->esi += (uint32_t)step; c->edi += (uint32_t)step;
        c->ecx--;
        if (!c->zf) break;
    }
}
static inline void repe_cmpsw(CpuCtx *c) {
    int32_t step = c->df ? -2 : 2;
    while (c->ecx) {
        alu_sub16(c, mem_r16(c->esi), mem_r16(c->edi));
        c->esi += (uint32_t)step; c->edi += (uint32_t)step;
        c->ecx--;
        if (!c->zf) break;
    }
}
static inline void repe_cmpsd(CpuCtx *c) {
    int32_t step = c->df ? -4 : 4;
    while (c->ecx) {
        alu_sub32(c, mem_r32(c->esi), mem_r32(c->edi));
        c->esi += (uint32_t)step; c->edi += (uint32_t)step;
        c->ecx--;
        if (!c->zf) break;
    }
}
static inline void repne_scasb(CpuCtx *c) {
    int32_t step = c->df ? -1 : 1;
    while (c->ecx) {
        alu_sub8(c, (uint8_t)c->eax, mem_r8(c->edi));
        c->edi += (uint32_t)step;
        c->ecx--;
        if (c->zf) break;
    }
}
static inline void repe_scasb(CpuCtx *c) {
    int32_t step = c->df ? -1 : 1;
    while (c->ecx) {
        alu_sub8(c, (uint8_t)c->eax, mem_r8(c->edi));
        c->edi += (uint32_t)step;
        c->ecx--;
        if (!c->zf) break;
    }
}

/* SAHF */
static inline void alu_sahf(CpuCtx *c) {
    uint8_t ah = (uint8_t)((c->eax >> 8) & 0xFFu);
    c->sf = (ah >> 7) & 1u;
    c->zf = (ah >> 6) & 1u;
    c->pf = (ah >> 2) & 1u;
    c->cf = (ah >> 0) & 1u;
}
static inline void alu_lahf(CpuCtx *c) {
    uint8_t ah = (uint8_t)((c->sf << 7) | (c->zf << 6) | (c->pf << 2) | 0x02u | (c->cf & 1u));
    w8_hi(&c->eax, ah);
}

#endif /* MHA2_CPU_H */
