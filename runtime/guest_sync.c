/*
 * Atomic replacements for the original game's hand-rolled guest spinlocks.
 *
 * The 2005 binary guards all ObjMutex state with an InterlockedIncrement-style
 * spinlock counter (-1 = free, 0 = held) plus an owner-TID word, implemented with
 * NON-atomic `mov/inc/mov` sequences and a forced lock-steal after 200 Sleep(1)
 * retries. recomp/lift.py (ATOMIC_COUNTER_GLOBALS) lifts every inline inc/dec as an
 * atomic RMW and every forced steal as guest_spin_acquire(); the native patches in
 * runtime/game_patches.c use guest_owner_lock_enter()/leave() with the identical
 * protocol, so lifted and native paths interoperate.
 *
 * Kept free of SDL/runtime dependencies so tests/ can link it directly.
 */
#include "cpu.h"
#include <time.h>
#include <unistd.h>

static uint32_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void guest_spin_acquire(uint32_t counter_va) {
    uint32_t *cnt = (uint32_t *)(g_mem + counter_va);
    uint32_t start = 0;
    int warned = 0;
    for (uint32_t spins = 0;; spins++) {
        if (__atomic_add_fetch(cnt, 1u, __ATOMIC_SEQ_CST) == 0u) return;
        __atomic_sub_fetch(cnt, 1u, __ATOMIC_SEQ_CST);
        if (spins == 0) {
            start = monotonic_ms();
        } else if (!warned && (uint32_t)(monotonic_ms() - start) > 5000u) {
            fprintf(stderr, "[sync] WARNING: guest spinlock 0x%08x still contended after 5s (counter=%d)\n",
                    counter_va, (int32_t)__atomic_load_n(cnt, __ATOMIC_RELAXED));
            warned = 1;
        }
        usleep(spins < 32u ? 100u : 1000u);
    }
}

int guest_owner_lock_enter(uint32_t counter_va, uint32_t owner_va, uint32_t tid) {
    uint32_t *owner = (uint32_t *)(g_mem + owner_va);
    if (__atomic_load_n(owner, __ATOMIC_ACQUIRE) == tid) return 0;
    guest_spin_acquire(counter_va);
    __atomic_store_n(owner, tid, __ATOMIC_RELEASE);
    return 1;
}

void guest_owner_lock_leave(uint32_t counter_va, uint32_t owner_va, int held) {
    if (!held) return;
    __atomic_store_n((uint32_t *)(g_mem + owner_va), 0u, __ATOMIC_RELEASE);
    __atomic_sub_fetch((uint32_t *)(g_mem + counter_va), 1u, __ATOMIC_SEQ_CST);
}
