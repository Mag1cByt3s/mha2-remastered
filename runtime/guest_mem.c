/*
 * Guest address space & heap allocator.
 *
 * Reserves the full 32-bit guest window PROT_NONE (wild pointers fault into
 * runtime/crash_handler.c), commits GUEST_MEM_SIZE read/write, and implements the
 * thread-safe 24-bin segregated free-list guest heap (64-step best-fit + same-bin
 * first-fit fallback) plus the static bump allocator for COM vtables/strings.
 *
 * Kept free of SDL/runtime dependencies so tests/ can link it directly.
 */
#include "cpu.h"
#include <sys/mman.h>
#include <pthread.h>

uint8_t *g_mem = NULL;

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
static pthread_mutex_t g_heap_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_heap_head = 0;
static uint32_t g_free_bins[HEAP_NUM_BINS];
static uint32_t g_bin_mask = 0;
static uint32_t g_static_bump = GUEST_COM_VTABLES_BASE;

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
    /* Reserve the whole 32-bit window inaccessible, then commit the used range. */
    g_mem = (uint8_t *)mmap(NULL, GUEST_RESERVE_SIZE, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) {
        perror("mmap guest address space reservation");
        exit(1);
    }
    if (mprotect(g_mem, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect guest memory");
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

int runtime_mem_protect_null_page(void) {
    return mprotect(g_mem, GUEST_NULL_GUARD_SIZE, PROT_NONE);
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
