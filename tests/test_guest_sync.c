/*
 * Stress test for runtime/guest_sync.c (atomic MetaSpinLock replacement).
 *
 * Threads mix every acquire path the recompiled game now uses on the same
 * counter/owner words:
 *   A) the exact inline sequence emitted by recomp/lift.py for ObjMutex methods:
 *      atomic inc -> (fail) atomic dec, Sleep, atomic inc ... and, after the retry
 *      budget is exhausted, guest_spin_acquire() where the original force-stole;
 *   B) guest_owner_lock_enter()/leave() (native MetaSpinLock/TryLock patches);
 *   C) re-entrant nesting via the owner-TID check.
 * Verifies mutual exclusion, zero lost updates, and a clean final state
 * (counter == -1, owner == 0). A simulation of the ORIGINAL non-atomic protocol
 * runs first to demonstrate that the harness detects the 2005 race.
 */
#include "cpu.h"
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

uint8_t *g_mem;

#define CNT_VA   0x0051b3b8u
#define OWNER_VA 0x00527824u
#define THREADS  8
#define ITERS    4000
#define LEGACY_ITERS 400

static uint32_t g_iters;
#define RETRIES  200

static volatile uint32_t g_shared;   /* protected by the lock, updated non-atomically */
static int g_in_cs;                  /* atomic occupancy probe */
static int g_violations;
static int g_legacy;                 /* 1 = simulate original non-atomic protocol */

static uint32_t *cnt(void) { return (uint32_t *)(g_mem + CNT_VA); }

static void critical_section(uint32_t i) {
    if (__atomic_add_fetch(&g_in_cs, 1, __ATOMIC_SEQ_CST) != 1)
        __atomic_add_fetch(&g_violations, 1, __ATOMIC_RELAXED);
    uint32_t v = g_shared;
    if ((i & 63u) == 0) usleep(3000);   /* occasionally hold long enough to exhaust RETRIES */
    else if ((i & 7u) == 0) sched_yield();
    g_shared = v + 1u;
    __atomic_sub_fetch(&g_in_cs, 1, __ATOMIC_SEQ_CST);
}

/* Original 2005 RMW: separate load / modify / store (simulated with relaxed atomics). */
static uint32_t legacy_rmw(int delta) {
    uint32_t v = __atomic_load_n(cnt(), __ATOMIC_RELAXED);
    sched_yield();                      /* widen the window the way a preemption would */
    v += (uint32_t)delta;
    __atomic_store_n(cnt(), v, __ATOMIC_RELAXED);
    return v;
}

static uint32_t rmw(int delta) {
    if (g_legacy) return legacy_rmw(delta);
    return delta > 0 ? __atomic_add_fetch(cnt(), 1u, __ATOMIC_SEQ_CST)
                     : __atomic_sub_fetch(cnt(), 1u, __ATOMIC_SEQ_CST);
}

/* Path A: shape of the lifted inline acquire in sub_004447a0 & co. */
static void inline_acquire(uint32_t tid) {
    if (rmw(+1) != 0u) {
        int tries = RETRIES;
        for (;;) {
            rmw(-1);
            if (--tries == 0) {
                if (g_legacy) __atomic_store_n(cnt(), 0u, __ATOMIC_RELAXED); /* forced steal */
                else guest_spin_acquire(CNT_VA);
                break;
            }
            usleep(10);
            if (rmw(+1) == 0u) break;
        }
    }
    __atomic_store_n((uint32_t *)(g_mem + OWNER_VA), tid, __ATOMIC_RELEASE);
}

static void inline_release(void) {
    __atomic_store_n((uint32_t *)(g_mem + OWNER_VA), 0u, __ATOMIC_RELEASE);
    rmw(-1);
}

static void *worker(void *arg) {
    uint32_t tid = (uint32_t)(uintptr_t)arg;
    for (uint32_t i = 0; i < g_iters; i++) {
        switch ((i + tid) % 3u) {
        case 0:
            inline_acquire(tid);
            critical_section(i);
            inline_release();
            break;
        case 1: {
            if (g_legacy) { inline_acquire(tid); critical_section(i); inline_release(); break; }
            int held = guest_owner_lock_enter(CNT_VA, OWNER_VA, tid);
            if (!held) __atomic_add_fetch(&g_violations, 1, __ATOMIC_RELAXED);
            critical_section(i);
            guest_owner_lock_leave(CNT_VA, OWNER_VA, held);
            break;
        }
        default: {
            if (g_legacy) { inline_acquire(tid); critical_section(i); inline_release(); break; }
            int outer = guest_owner_lock_enter(CNT_VA, OWNER_VA, tid);
            int inner = guest_owner_lock_enter(CNT_VA, OWNER_VA, tid); /* re-entrant: must be 0 */
            if (!outer || inner) __atomic_add_fetch(&g_violations, 1, __ATOMIC_RELAXED);
            critical_section(i);
            guest_owner_lock_leave(CNT_VA, OWNER_VA, inner);
            guest_owner_lock_leave(CNT_VA, OWNER_VA, outer);
            break;
        }
        }
    }
    return NULL;
}

static int run(int legacy, int iters_check) {
    g_legacy = legacy;
    g_iters = legacy ? LEGACY_ITERS : ITERS;
    g_shared = 0;
    g_in_cs = 0;
    g_violations = 0;
    *cnt() = 0xFFFFFFFFu;
    *(uint32_t *)(g_mem + OWNER_VA) = 0;

    pthread_t th[THREADS];
    for (uintptr_t t = 0; t < THREADS; t++) pthread_create(&th[t], NULL, worker, (void *)(t + 1000u));
    for (int t = 0; t < THREADS; t++) pthread_join(th[t], NULL);

    uint32_t expected = THREADS * g_iters;
    int lost = (int)(expected - g_shared);
    int32_t final_cnt = (int32_t)*cnt();
    uint32_t final_owner = *(uint32_t *)(g_mem + OWNER_VA);
    printf("[%s] shared=%u/%u lost=%d exclusion_violations=%d final_counter=%d final_owner=%u\n",
           legacy ? "legacy-2005" : "atomic-fixed", g_shared, expected, lost, g_violations,
           final_cnt, final_owner);
    if (!iters_check) return 0;
    return (lost == 0 && g_violations == 0 && final_cnt == -1 && final_owner == 0) ? 0 : 1;
}

int main(void) {
    g_mem = calloc(1, 0x00600000u);
    if (!g_mem) return 2;
    run(1, 0);                      /* informational: shows the original protocol races */
    int rc = 0;
    for (int round = 0; round < 3 && rc == 0; round++) rc = run(0, 1);
    printf(rc == 0 ? "PASS test_guest_sync\n" : "FAIL test_guest_sync\n");
    free(g_mem);
    return rc;
}
