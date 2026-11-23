#include "runtime.h"
#include "imports.h"
#include <sys/mman.h>
#include <pthread.h>

uint8_t *g_mem = NULL;
char g_game_dir[1024] = "game_files";
volatile int g_quit_requested = 0;
int g_debug_log = 0;

/* ---- Segregated size-class free-list, thread-safe coalescing guest heap allocator ---- */
typedef struct HeapBlock {
    uint32_t size;      /* usable payload size in bytes */
    uint32_t is_free;   /* 1 = free, 0 = allocated */
    uint32_t next;      /* guest addr of next HeapBlock (0 if end) */
    uint32_t prev;      /* guest addr of prev HeapBlock (0 if head) */
} HeapBlock;

/* Stored in the first 8 bytes of a free block's payload (payload >= 16 bytes) */
typedef struct FreeLinks {
    uint32_t next_free;
    uint32_t prev_free;
} FreeLinks;

#define HEAP_HDR_SIZE    16u
#define HEAP_NUM_BINS    24u
#define MAX_THREAD_SLOTS 36u
static pthread_mutex_t g_heap_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_heap_head = 0;
static uint32_t g_free_bins[HEAP_NUM_BINS];
static uint32_t g_bin_mask = 0;
static uint32_t g_static_bump = GUEST_COM_VTABLES_BASE;
static uint32_t g_com_vtables[32];
static uint64_t g_thread_slot_mask = 0;

static inline FreeLinks *free_links(uint32_t block_va) {
    return (FreeLinks *)(g_mem + block_va + HEAP_HDR_SIZE);
}

/* Bin k covers [16 << k, 32 << k) bytes; bin 23 covers >= 128 MB */
static inline uint32_t size_to_bin(uint32_t sz) {
    if (sz < 32u) return 0u;
    uint32_t b = 27u - (uint32_t)__builtin_clz(sz); /* clz(32)=26 -> b=1 */
    return (b < HEAP_NUM_BINS) ? b : (HEAP_NUM_BINS - 1u);
}

static inline void free_list_insert(uint32_t block_va, uint32_t sz) {
    uint32_t b = size_to_bin(sz);
    FreeLinks *fl = free_links(block_va);
    fl->prev_free = 0;
    fl->next_free = g_free_bins[b];
    if (g_free_bins[b]) {
        free_links(g_free_bins[b])->prev_free = block_va;
    }
    g_free_bins[b] = block_va;
    g_bin_mask |= (1u << b);
}

static inline void free_list_remove(uint32_t block_va, uint32_t sz) {
    uint32_t b = size_to_bin(sz);
    FreeLinks *fl = free_links(block_va);
    if (fl->prev_free) {
        free_links(fl->prev_free)->next_free = fl->next_free;
    } else {
        g_free_bins[b] = fl->next_free;
        if (!g_free_bins[b]) g_bin_mask &= ~(1u << b);
    }
    if (fl->next_free) {
        free_links(fl->next_free)->prev_free = fl->prev_free;
    }
    fl->prev_free = 0;
    fl->next_free = 0;
}

void runtime_mem_init(void) {
    g_mem = (uint8_t *)mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_mem == MAP_FAILED) {
        perror("mmap guest memory");
        exit(1);
    }

    /* Initialize single free block covering GUEST_HEAP_BASE..GUEST_HEAP_END */
    g_heap_head = GUEST_HEAP_BASE;
    memset(g_free_bins, 0, sizeof(g_free_bins));
    g_bin_mask = 0;
    HeapBlock *hb = (HeapBlock *)(g_mem + g_heap_head);
    hb->size = (GUEST_HEAP_END - GUEST_HEAP_BASE) - HEAP_HDR_SIZE;
    hb->is_free = 1;
    hb->next = 0;
    hb->prev = 0;
    free_list_insert(g_heap_head, hb->size);
}

uint32_t guest_alloc(uint32_t size) {
    if (size > 0x10000000u) {
        fprintf(stderr, "FATAL: guest_alloc(%u) request too large!\n", size);
        return 0;
    }
    if (size == 0) size = 16;
    size = (size + 15u) & ~15u;

    pthread_mutex_lock(&g_heap_mtx);
    uint32_t start_bin = size_to_bin(size);
    uint32_t cur = 0;

    /* 1. Best-fit scan (bounded to 64 steps, falling back to first-fit in the same bin) */
    uint32_t scan = g_free_bins[start_bin];
    uint32_t best = 0;
    uint32_t best_sz = 0xFFFFFFFFu;
    int steps = 0;
    while (scan && (steps < 64 || !best)) {
        HeapBlock *hb = (HeapBlock *)(g_mem + scan);
        if (hb->size >= size && hb->size < best_sz) {
            best = scan;
            best_sz = hb->size;
            if (best_sz == size || steps >= 64) break;
        }
        scan = free_links(scan)->next_free;
        steps++;
    }
    if (best) {
        cur = best;
    } else if (start_bin + 1u < HEAP_NUM_BINS) {
        /* 2. O(1) jump to the smallest non-empty larger bin (every block is >= size) */
        uint32_t hi_mask = g_bin_mask & (~0u << (start_bin + 1u));
        if (hi_mask) {
            uint32_t b = (uint32_t)__builtin_ctz(hi_mask);
            cur = g_free_bins[b];
        }
    }

    if (cur) {
        HeapBlock *hb = (HeapBlock *)(g_mem + cur);
        free_list_remove(cur, hb->size);
        if (hb->size >= size + HEAP_HDR_SIZE + 32u) {
            /* Split block */
            uint32_t next_va = cur + HEAP_HDR_SIZE + size;
            HeapBlock *nhb = (HeapBlock *)(g_mem + next_va);
            nhb->size = hb->size - size - HEAP_HDR_SIZE;
            nhb->is_free = 1;
            nhb->next = hb->next;
            nhb->prev = cur;
            if (hb->next) {
                ((HeapBlock *)(g_mem + hb->next))->prev = next_va;
            }
            hb->size = size;
            hb->next = next_va;
            free_list_insert(next_va, nhb->size);
        }
        hb->is_free = 0;
        uint32_t payload = cur + HEAP_HDR_SIZE;
        pthread_mutex_unlock(&g_heap_mtx);
        return payload;
    }
    pthread_mutex_unlock(&g_heap_mtx);
    fprintf(stderr, "FATAL: guest_alloc(%u) out of guest heap memory!\n", size);
    return 0;
}

void guest_free(uint32_t addr) {
    if (addr < GUEST_HEAP_BASE + HEAP_HDR_SIZE || addr >= GUEST_HEAP_END) return;
    pthread_mutex_lock(&g_heap_mtx);
    uint32_t cur = addr - HEAP_HDR_SIZE;
    HeapBlock *hb = (HeapBlock *)(g_mem + cur);
    if (hb->is_free) {
        pthread_mutex_unlock(&g_heap_mtx);
        return;
    }
    hb->is_free = 1;

    /* Coalesce with next */
    if (hb->next) {
        uint32_t next_va = hb->next;
        HeapBlock *nhb = (HeapBlock *)(g_mem + next_va);
        if (nhb->is_free) {
            free_list_remove(next_va, nhb->size);
            hb->size += HEAP_HDR_SIZE + nhb->size;
            hb->next = nhb->next;
            if (hb->next) {
                ((HeapBlock *)(g_mem + hb->next))->prev = cur;
            }
        }
    }
    /* Coalesce with prev */
    if (hb->prev) {
        uint32_t prev_va = hb->prev;
        HeapBlock *phb = (HeapBlock *)(g_mem + prev_va);
        if (phb->is_free) {
            free_list_remove(prev_va, phb->size);
            phb->size += HEAP_HDR_SIZE + hb->size;
            phb->next = hb->next;
            if (hb->next) {
                ((HeapBlock *)(g_mem + hb->next))->prev = prev_va;
            }
            free_list_insert(prev_va, phb->size);
            pthread_mutex_unlock(&g_heap_mtx);
            return;
        }
    }
    free_list_insert(cur, hb->size);
    pthread_mutex_unlock(&g_heap_mtx);
}

uint32_t guest_alloc_size(uint32_t addr) {
    if (addr < GUEST_HEAP_BASE + HEAP_HDR_SIZE || addr >= GUEST_HEAP_END) return 0;
    pthread_mutex_lock(&g_heap_mtx);
    HeapBlock *hb = (HeapBlock *)(g_mem + addr - HEAP_HDR_SIZE);
    uint32_t sz = hb->is_free ? 0u : hb->size;
    pthread_mutex_unlock(&g_heap_mtx);
    return sz;
}

uint32_t guest_realloc(uint32_t addr, uint32_t new_size) {
    if (addr == 0) return guest_alloc(new_size);
    if (new_size == 0) { guest_free(addr); return 0; }
    uint32_t old_sz = guest_alloc_size(addr);
    if (old_sz >= new_size) return addr;
    uint32_t naddr = guest_alloc(new_size);
    if (!naddr) return 0;
    memcpy(g_mem + naddr, g_mem + addr, old_sz);
    guest_free(addr);
    return naddr;
}

uint32_t guest_alloc_static(const void *data, uint32_t size) {
    pthread_mutex_lock(&g_heap_mtx);
    uint32_t va = (g_static_bump + 15u) & ~15u;
    if (size > GUEST_TEB_BASE || va > GUEST_TEB_BASE - size) {
        pthread_mutex_unlock(&g_heap_mtx);
        fprintf(stderr, "FATAL: guest_alloc_static(%u) out of static region!\n", size);
        return 0;
    }
    g_static_bump = va + size;
    if (data && size) memcpy(g_mem + va, data, size);
    pthread_mutex_unlock(&g_heap_mtx);
    return va;
}

uint32_t guest_alloc_cstr(const char *s) {
    uint32_t len = (uint32_t)strlen(s) + 1u;
    return guest_alloc_static(s, len);
}

uint32_t com_get_vtable(uint32_t iface_id, uint32_t num_slots) {
    if (iface_id < 32 && g_com_vtables[iface_id]) {
        return g_com_vtables[iface_id];
    }
    uint32_t va = guest_alloc_static(NULL, num_slots * 4u);
    if (!va) return 0;
    for (uint32_t i = 0; i < num_slots; i++) {
        mem_w32(va + i * 4u, HLE_COM_BASE | (iface_id << 12) | i);
    }
    if (iface_id < 32) g_com_vtables[iface_id] = va;
    return va;
}

uint32_t com_create_obj(uint32_t iface_id, uint32_t num_slots, uint32_t handle_val) {
    uint32_t vtbl = com_get_vtable(iface_id, num_slots);
    uint32_t obj = guest_alloc(16);
    if (!obj) return 0;
    mem_w32(obj + 0, vtbl);
    mem_w32(obj + 4, handle_val);
    mem_w32(obj + 8, iface_id);
    return obj;
}

uint32_t com_obj_handle(uint32_t obj_va) {
    if (!obj_va) return 0;
    return mem_r32(obj_va + 4);
}

uint32_t guest_alloc_thread_slot(uint32_t *out_teb, uint32_t *out_stack_top) {
    pthread_mutex_lock(&g_heap_mtx);
    uint32_t slot = MAX_THREAD_SLOTS;
    for (uint32_t i = 0; i < MAX_THREAD_SLOTS; i++) {
        if ((g_thread_slot_mask & (1ULL << i)) == 0) {
            g_thread_slot_mask |= (1ULL << i);
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_heap_mtx);
    if (slot >= MAX_THREAD_SLOTS) {
        fprintf(stderr, "FATAL: guest_alloc_thread_slot exhausted (%u slots)!\n", MAX_THREAD_SLOTS);
        return 0;
    }
    uint32_t teb = GUEST_TEB_BASE + slot * 4096u;
    uint32_t stack_top = GUEST_STACK_BASE + (slot + 1u) * GUEST_STACK_SIZE - 256u;
    memset(g_mem + teb, 0, 4096);
    mem_w32(teb + 0x00, 0xFFFFFFFFu); /* SEH chain end */
    mem_w32(teb + 0x04, stack_top);
    mem_w32(teb + 0x08, stack_top - GUEST_STACK_SIZE + 256u);
    mem_w32(teb + 0x18, teb);         /* Self pointer */
    mem_w32(teb + 0x20, 1000u);       /* PID */
    mem_w32(teb + 0x24, 1000u + slot);/* TID */
    if (out_teb) *out_teb = teb;
    if (out_stack_top) *out_stack_top = stack_top;
    return 1000u + slot;
}

void guest_free_thread_slot(uint32_t tid) {
    if (tid < 1000u || tid >= 1000u + MAX_THREAD_SLOTS) return;
    uint32_t slot = tid - 1000u;
    pthread_mutex_lock(&g_heap_mtx);
    g_thread_slot_mask &= ~(1ULL << slot);
    pthread_mutex_unlock(&g_heap_mtx);
}

/* ---- PE Loader ---- */
int runtime_load_pe(const char *exe_path) {
    FILE *f = fopen(exe_path, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open PE image '%s'\n", exe_path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);

    uint32_t e_lfanew;
    memcpy(&e_lfanew, buf + 0x3c, 4);
    uint16_t n_sec, opt_size;
    memcpy(&n_sec, buf + e_lfanew + 6, 2);
    memcpy(&opt_size, buf + e_lfanew + 20, 2);
    uint32_t image_base, hdr_size;
    memcpy(&image_base, buf + e_lfanew + 24 + 28, 4);
    memcpy(&hdr_size, buf + e_lfanew + 24 + 60, 4);

    /* Copy PE headers */
    memcpy(g_mem + image_base, buf, hdr_size);

    /* Copy sections */
    uint32_t sec_off = e_lfanew + 24 + opt_size;
    for (int i = 0; i < n_sec; i++) {
        uint8_t *s = buf + sec_off + i * 40;
        uint32_t vsz, vaddr, rsz, roff;
        memcpy(&vsz, s + 8, 4);
        memcpy(&vaddr, s + 12, 4);
        memcpy(&rsz, s + 16, 4);
        memcpy(&roff, s + 20, 4);
        uint32_t copy_len = rsz < vsz ? rsz : vsz;
        if (roff + copy_len <= (uint32_t)sz) {
            memcpy(g_mem + image_base + vaddr, buf + roff, copy_len);
        }
    }
    free(buf);

    /* Populate IAT slots with synthetic Win32 HLE addresses */
    for (uint32_t i = 0; i < MHA2_IMPORT_COUNT; i++) {
        mem_w32(g_imports[i].iat_va, HLE_WIN32_BASE + g_imports[i].id);
    }
    return 0;
}

/* ---- Call Dispatchers ---- */
void win32_dispatch(CpuCtx *c, uint32_t import_id) {
    if (win32_kernel_dispatch(c, import_id)) return;
    if (win32_user_gdi_dispatch(c, import_id)) return;
    if (win32_ddraw_dispatch(c, import_id)) return;
    if (win32_dinput_dispatch(c, import_id)) return;
    if (win32_dsound_dispatch(c, import_id)) return;
    if (win32_avi_dispatch(c, import_id)) return;

    const char *dll = (import_id < MHA2_IMPORT_COUNT) ? g_imports[import_id].dll : "?";
    const char *name = (import_id < MHA2_IMPORT_COUNT) ? g_imports[import_id].name : "?";
    fprintf(stderr, "UNIMPLEMENTED Win32 import #%u: %s!%s (ret=0x%08x)\n",
            import_id, dll, name, mem_r32(c->esp));
    stdcall_ret(c, 0, 0);
}

void com_dispatch(CpuCtx *c, uint32_t iface_id, uint32_t slot) {
    switch (iface_id) {
        case COM_IFACE_DDRAW:
        case COM_IFACE_DDRAW_SURFACE:
        case COM_IFACE_DDRAW_CLIPPER:
            com_ddraw_dispatch(c, iface_id, slot);
            return;
        case COM_IFACE_DINPUT:
        case COM_IFACE_DINPUT_DEV:
            com_dinput_dispatch(c, iface_id, slot);
            return;
        case COM_IFACE_DSOUND:
        case COM_IFACE_DSOUND_BUF:
            com_dsound_dispatch(c, iface_id, slot);
            return;
        case COM_IFACE_DSHOW_GRAPH:
        case COM_IFACE_DSHOW_CTRL:
        case COM_IFACE_DSHOW_EVENT:
        case COM_IFACE_DSHOW_VIDWIN:
        case COM_IFACE_DSHOW_BASVID:
            com_dshow_dispatch(c, iface_id, slot);
            return;
        default:
            fprintf(stderr, "FATAL: unknown COM iface %u slot %u\n", iface_id, slot);
            abort();
    }
}

void dispatch_call(CpuCtx *c, uint32_t addr) {
    if (addr >= HLE_WIN32_BASE) {
        win32_dispatch(c, addr - HLE_WIN32_BASE);
        return;
    }
    if (addr >= HLE_COM_BASE && addr < HLE_WIN32_BASE) {
        uint32_t rel = addr - HLE_COM_BASE;
        com_dispatch(c, rel >> 12, rel & 0xFFFu);
        return;
    }
    if (addr >= GUEST_TEXT_START && addr < GUEST_TEXT_END) {
        RecompFn fn = g_code_table[addr - GUEST_TEXT_START];
        if (fn) {
            fn(c);
            return;
        }
    }
    fprintf(stderr, "FATAL: indirect call to unlifted address 0x%08x (ret=0x%08x, esp=0x%08x)\n",
            addr, mem_r32(c->esp), c->esp);
    abort();
}

uint32_t guest_call_stdcall(CpuCtx *c, uint32_t fn_addr, int nargs, const uint32_t *args) {
    /* Save registers that caller expects preserved across Win32 API calls */
    uint32_t s_ebx = c->ebx, s_esi = c->esi, s_edi = c->edi, s_ebp = c->ebp;
    for (int i = nargs - 1; i >= 0; i--) {
        push32(c, args[i]);
    }
    push32(c, HLE_RET_SENTINEL);
    dispatch_call(c, fn_addr);
    uint32_t ret = c->eax;
    c->ebx = s_ebx; c->esi = s_esi; c->edi = s_edi; c->ebp = s_ebp;
    return ret;
}

uint32_t guest_call_cdecl(CpuCtx *c, uint32_t fn_addr, int nargs, const uint32_t *args) {
    uint32_t s_ebx = c->ebx, s_esi = c->esi, s_edi = c->edi, s_ebp = c->ebp;
    for (int i = nargs - 1; i >= 0; i--) {
        push32(c, args[i]);
    }
    push32(c, HLE_RET_SENTINEL);
    dispatch_call(c, fn_addr);
    c->esp += (uint32_t)nargs * 4u;
    uint32_t ret = c->eax;
    c->ebx = s_ebx; c->esi = s_esi; c->edi = s_edi; c->ebp = s_ebp;
    return ret;
}
