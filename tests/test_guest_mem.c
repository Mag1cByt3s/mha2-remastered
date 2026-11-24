/*
 * Tests for runtime/guest_mem.c (guest address space + heap) and
 * runtime/crash_handler.c (fault classification & reporting).
 *
 * Fault scenarios run in forked children; the parent asserts the terminating
 * signal and that the crash report on stderr names the right cause/region.
 */
#include "cpu.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail = 1; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- heap allocator ---------------- */

static uint32_t rng_state = 12345u;
static uint32_t rng(void) { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }

static void test_heap(void) {
    printf("[heap] randomized alloc/realloc/free integrity\n");
    enum { N = 4096 };
    static uint32_t va[N], sz[N];
    uint32_t first_big = guest_alloc(256u * 1024u * 1024u);
    CHECK(first_big != 0, "initial 256 MiB alloc failed");
    guest_free(first_big);

    for (int round = 0; round < 40; round++) {
        for (int i = 0; i < N; i++) {
            uint32_t op = rng() % 4u;
            if (va[i] && op == 0) {
                for (uint32_t k = 0; k < sz[i]; k += 97u)
                    CHECK(mem_r8(va[i] + k) == (uint8_t)(i ^ k), "corruption in block %d", i);
                guest_free(va[i]);
                va[i] = 0;
            } else if (va[i] && op == 1) {
                uint32_t nsz = 1u + rng() % ((rng() & 7u) ? 512u : 300000u);
                uint32_t nva = guest_realloc(va[i], nsz);
                CHECK(nva != 0, "realloc failed");
                uint32_t keep = nsz < sz[i] ? nsz : sz[i];
                for (uint32_t k = 0; k < keep; k += 97u)
                    CHECK(mem_r8(nva + k) == (uint8_t)(i ^ k), "realloc lost data in block %d", i);
                va[i] = nva;
                sz[i] = nsz;
                for (uint32_t k = 0; k < sz[i]; k += 97u) mem_w8(va[i] + k, (uint8_t)(i ^ k));
            } else if (!va[i]) {
                sz[i] = 1u + rng() % ((rng() & 7u) ? 512u : 300000u);
                va[i] = guest_alloc(sz[i]);
                CHECK(va[i] != 0, "alloc(%u) failed", sz[i]);
                CHECK((va[i] & 7u) == 0, "alloc not 8-byte aligned: 0x%08x", va[i]);
                CHECK(va[i] >= GUEST_HEAP_BASE && va[i] + sz[i] <= GUEST_HEAP_END, "alloc outside heap");
                CHECK(guest_alloc_size(va[i]) >= sz[i], "alloc_size too small");
                for (uint32_t k = 0; k < sz[i]; k += 97u) mem_w8(va[i] + k, (uint8_t)(i ^ k));
            }
        }
    }
    /* overlap check */
    for (int i = 0; i < N; i++)
        for (int j = i + 1; j < N && va[i]; j++)
            if (va[j]) CHECK(va[i] + sz[i] <= va[j] || va[j] + sz[j] <= va[i], "blocks %d/%d overlap", i, j);
    for (int i = 0; i < N; i++) if (va[i]) guest_free(va[i]);

    /* full coalescing: after freeing everything, a huge block must fit again */
    uint32_t big = guest_alloc(256u * 1024u * 1024u);
    CHECK(big == first_big, "heap did not fully coalesce (got 0x%08x, want 0x%08x)", big, first_big);
    guest_free(big);
    CHECK(guest_alloc(0x20000000u) == 0, "oversized request should fail");
}

/* ---------------- fault scenarios (run in child) ---------------- */

static CpuCtx g_ctx;

static volatile int g_depth_limit = 0x7fffffff; /* volatile: defeats -Winfinite-recursion */
static int recurse(int n) {
    volatile char pad[4096];
    pad[0] = (char)n;
    if (n >= g_depth_limit) return pad[0];
    return recurse(n + 1) + pad[0];
}

static void child_body(int scenario) {
    runtime_mem_init();
    crash_handler_install();
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.thread_id = 1000;
    g_ctx.eax = 0xDEADBEEFu;
    g_ctx.esp = GUEST_STACK_BASE + 0x1000u;
    crash_handler_thread_init(&g_ctx);
    volatile uint32_t sink = 0;
    volatile int zero = 0;
    switch (scenario) {
    case 0: sink = mem_r32(0x70000000u); break;                  /* beyond committed */
    case 1: sink = mem_r32(0xFFFFFFFEu); break;                  /* straddles 4 GiB */
    case 2: runtime_mem_protect_null_page(); sink = mem_r32(0x10u); break;
    case 3: sink = mem_r32(0x10u); mem_w32(GUEST_MEM_SIZE - 4u, 1u); _exit(0); /* must NOT fault */
    case 4: sink = (uint32_t)(100 / zero); break;                /* SIGFPE */
    case 5: sink = (uint32_t)recurse(0); break;                  /* host stack overflow */
    case 6: abort();
    }
    (void)sink;
    _exit(0);
}

typedef struct { const char *name; int want_sig; const char *want_text; } Scenario;

static void run_scenario(int idx, const Scenario *s) {
    int pfd[2];
    if (pipe(pfd) != 0) { perror("pipe"); g_fail = 1; return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDERR_FILENO);
        child_body(idx);
    }
    close(pfd[1]);
    static char buf[16384];
    size_t len = 0;
    ssize_t r;
    while (len < sizeof(buf) - 1 && (r = read(pfd[0], buf + len, sizeof(buf) - 1 - len)) > 0) len += (size_t)r;
    buf[len] = 0;
    close(pfd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    int sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
    int ok_sig = (sig == s->want_sig) && (s->want_sig || (WIFEXITED(st) && WEXITSTATUS(st) == 0));
    int ok_txt = !s->want_text || strstr(buf, s->want_text) != NULL;
    printf("[fault] %-34s signal=%-2d %s\n", s->name, sig, ok_sig && ok_txt ? "ok" : "FAIL");
    if (!ok_sig || !ok_txt) {
        g_fail = 1;
        printf("  expected signal %d and text \"%s\"; child stderr:\n%s\n", s->want_sig,
               s->want_text ? s->want_text : "", buf);
    }
    if (getenv("MHA2_TEST_VERBOSE")) printf("%s\n", buf);
    if (idx == 0 && !strstr(buf, "eax=deadbeef")) {
        g_fail = 1;
        printf("  FAIL: guest register dump missing\n");
    }
}

int main(void) {
    /* keep mha2_crash.log out of the source tree */
    char tmpl[] = "/tmp/mha2_test_XXXXXX";
    if (mkdtemp(tmpl) && chdir(tmpl) != 0) perror("chdir");

    runtime_mem_init();
    test_heap();

    static const Scenario sc[] = {
        { "wild pointer beyond 1.5 GiB",       SIGSEGV, "outside committed guest memory" },
        { "4-byte read straddling 4 GiB",      SIGSEGV, "guest VA 0xfffffffe" },
        { "--strict-mem NULL page read",       SIGSEGV, "guest NULL page" },
        { "NULL page readable by default",     0,       NULL },
        { "integer divide by zero",            SIGFPE,  "integer divide by zero" },
        { "host stack overflow (altstack)",    SIGSEGV, "MHA2 CRASH" },
        { "abort() backtrace",                 SIGABRT, "host backtrace" },
    };
    for (int i = 0; i < (int)(sizeof(sc) / sizeof(sc[0])); i++) run_scenario(i, &sc[i]);

    FILE *f = fopen("mha2_crash.log", "r");
    CHECK(f != NULL, "mha2_crash.log not written");
    if (f) fclose(f);

    printf(g_fail ? "FAIL test_guest_mem\n" : "PASS test_guest_mem\n");
    return g_fail;
}
