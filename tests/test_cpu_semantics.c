/*
 * x86 semantic tests for runtime/cpu.h helpers used by lifted code.
 *
 * div/idiv (8/16/32): randomized comparison against an independent __int128
 * reference; every #DE case (zero divisor, quotient overflow, INT_MIN / -1) must
 * abort with a "#DE" report instead of trapping in the host or truncating.
 * FIST/FISTP m16/m32/m64 and the __ftol patch: bit-exact comparison against the
 * host's real x87 FISTP in all four rounding modes (incl. NaN/Inf/out-of-range
 * "integer indefinite" results).
 */
#include "cpu.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }

/* Interesting operands: random, small, boundaries */
static uint64_t pick(int bits) {
    uint64_t mask = bits == 64 ? ~0ull : ((1ull << bits) - 1u);
    switch (rnd() % 6u) {
    case 0: return rnd() & mask;
    case 1: return (rnd() % 17u) & mask;
    case 2: return mask;                                  /* -1 / max */
    case 3: return (1ull << (bits - 1)) & mask;           /* INT_MIN */
    case 4: return ((1ull << (bits - 1)) - 1u) & mask;    /* INT_MAX */
    default: return (rnd() >> (rnd() % (unsigned)bits)) & mask;
    }
}

static int64_t sx(uint64_t v, int bits) {
    return bits == 64 ? (int64_t)v : (int64_t)(v << (64 - bits)) >> (64 - bits);
}

/* Returns 1 if the reference expects #DE, else fills q/r. */
static int reference(int bits, int sgn, uint64_t num, uint64_t div, uint64_t *q, uint64_t *r) {
    uint64_t dmask = (1ull << bits) - 1u;
    if (div == 0) return 1;
    if (!sgn) {
        unsigned __int128 n = num, d = div;
        unsigned __int128 qq = n / d;
        if (qq > dmask) return 1;
        *q = (uint64_t)qq;
        *r = (uint64_t)(n % d);
    } else {
        __int128 n = sx(num, 2 * bits), d = sx(div, bits);
        __int128 qq = n / d;
        __int128 lim = (__int128)1 << (bits - 1);
        if (qq < -lim || qq >= lim) return 1;
        *q = (uint64_t)qq & dmask;
        *r = (uint64_t)(n % d) & dmask;
    }
    return 0;
}

static void load(CpuCtx *c, int bits, uint64_t num) {
    memset(c, 0, sizeof(*c));
    c->eax = 0xA5A5A5A5u;
    c->edx = 0x5A5A5A5Au;
    if (bits == 8) w16(&c->eax, (uint16_t)num);
    else if (bits == 16) { w16(&c->eax, (uint16_t)num); w16(&c->edx, (uint16_t)(num >> 16)); }
    else { c->eax = (uint32_t)num; c->edx = (uint32_t)(num >> 32); }
}

static void exec(CpuCtx *c, int bits, int sgn, uint64_t div) {
    if (bits == 8)  { if (sgn) alu_idiv8(c, (uint8_t)div);  else alu_div8(c, (uint8_t)div); }
    if (bits == 16) { if (sgn) alu_idiv16(c, (uint16_t)div); else alu_div16(c, (uint16_t)div); }
    if (bits == 32) { if (sgn) alu_idiv32(c, (uint32_t)div); else alu_div32(c, (uint32_t)div); }
}

static void result(const CpuCtx *c, int bits, uint64_t *q, uint64_t *r) {
    if (bits == 8)  { *q = c->eax & 0xFFu; *r = (c->eax >> 8) & 0xFFu; }
    if (bits == 16) { *q = c->eax & 0xFFFFu; *r = c->edx & 0xFFFFu; }
    if (bits == 32) { *q = c->eax; *r = c->edx; }
}

/* Run one #DE case in a child; expect SIGABRT and a #DE report. */
static void expect_de(int bits, int sgn, uint64_t num, uint64_t div) {
    int pfd[2];
    if (pipe(pfd) != 0) { g_fail = 1; return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDERR_FILENO);
        runtime_mem_init();
        crash_handler_install();
        CpuCtx c;
        load(&c, bits, num);
        exec(&c, bits, sgn, div);
        _exit(0); /* reached only if #DE was NOT raised */
    }
    close(pfd[1]);
    char buf[8192];
    size_t len = 0;
    ssize_t n;
    while (len < sizeof(buf) - 1 && (n = read(pfd[0], buf + len, sizeof(buf) - 1 - len)) > 0) len += (size_t)n;
    buf[len] = 0;
    close(pfd[0]);
    int st;
    waitpid(pid, &st, 0);
    if (!(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT && strstr(buf, "#DE"))) {
        g_fail = 1;
        printf("  FAIL: %s%d 0x%llx / 0x%llx expected #DE abort (status 0x%x)\n%s\n", sgn ? "idiv" : "div",
               bits, (unsigned long long)num, (unsigned long long)div, st, buf);
    }
}

/* ---------------- x87 integer conversion vs real hardware ---------------- */

static uint64_t hw_fistp(long double v, int bits, unsigned rc) {
    uint16_t cw_old, cw_new;
    __asm__ volatile("fnstcw %0" : "=m"(cw_old));
    cw_new = (uint16_t)((cw_old & ~0x0C00u) | (rc << 10));
    __asm__ volatile("fldcw %0" : : "m"(cw_new));
    uint64_t out = 0;
    if (bits == 16) { int16_t r; __asm__ volatile("fistps %0" : "=m"(r) : "t"(v) : "st"); out = (uint16_t)r; }
    if (bits == 32) { int32_t r; __asm__ volatile("fistpl %0" : "=m"(r) : "t"(v) : "st"); out = (uint32_t)r; }
    if (bits == 64) { int64_t r; __asm__ volatile("fistpll %0" : "=m"(r) : "t"(v) : "st"); out = (uint64_t)r; }
    __asm__ volatile("fnclex");
    __asm__ volatile("fldcw %0" : : "m"(cw_old));
    return out;
}

static long double pick_ld(void) {
    static const long double edge[] = {
        0.0L, -0.0L, 0.5L, -0.5L, 1.5L, -1.5L, 2.5L, -2.5L, 32767.5L, -32768.5L, 32768.0L, -32769.0L,
        2147483647.5L, -2147483648.5L, 2147483648.0L, -2147483649.0L, 0x1p63L, -0x1p63L,
        0x1p63L - 1.0L, -0x1p63L - 1.0L, 0x1p64L, 1e300L, -1e300L,
    };
    switch (rnd() % 5u) {
    case 0: return edge[rnd() % (sizeof(edge) / sizeof(edge[0]))];
    case 1: return (rnd() & 1) ? __builtin_infl() : -__builtin_infl();
    case 2: return __builtin_nanl("");
    case 3: return ldexpl((long double)(int64_t)rnd(), -(int)(rnd() % 70u));
    default: return ldexpl((long double)(int64_t)rnd() / 0x1p63L, (int)(rnd() % 70u)) + (long double)(rnd() % 4u) * 0.25L;
    }
}

static void test_fist(void) {
    int checked = 0, bad = 0;
    CpuCtx c;
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < 400000; i++) {
        long double v = pick_ld();
        unsigned rc = (unsigned)(rnd() & 3u);
        int bits = 16 << (rnd() % 3u);
        c.fpu_cw = (uint16_t)(0x037Fu | (rc << 10));
        uint64_t mask = bits == 64 ? ~0ull : ((1ull << bits) - 1u);
        uint64_t got = fpu_to_int(&c, v, bits) & mask, want = hw_fistp(v, bits, rc) & mask;
        if (got != want && bad++ < 10)
            printf("  FAIL: fistp m%d rc=%u v=%La -> 0x%llx, x87 hw 0x%llx\n", bits, rc, v,
                   (unsigned long long)got, (unsigned long long)want);
        /* __ftol patch path: truncate + 64-bit indefinite */
        uint64_t ftol = x87_ld_to_int(truncl(v), 64), hw_ftol = hw_fistp(v, 64, 3u);
        if (ftol != hw_ftol && bad++ < 10)
            printf("  FAIL: __ftol v=%La -> 0x%llx, x87 hw 0x%llx\n", v, (unsigned long long)ftol,
                   (unsigned long long)hw_ftol);
        checked += 2;
    }
    if (bad) g_fail = 1;
    printf("[x87] %d FIST/__ftol conversions checked bit-exact against host x87 FISTP (%d mismatches)\n",
           checked, bad);
}

/* ---------------- flag-free helpers vs flag-setting helpers ---------------- */

static void test_nf_helpers(void) {
    CpuCtx c;
    memset(&c, 0, sizeof(c));
    int bad = 0, checked = 0;
#define CMP2(B, UT, OP)                                                                  \
    { UT x = (UT)rnd(), y = (UT)rnd(); c.cf = (uint32_t)(rnd() & 1u);                    \
      if ((UT)nf_##OP##B(x, y) != (UT)alu_##OP##B(&c, x, y) && bad++ < 10)               \
          { printf("  FAIL: nf_" #OP #B "(0x%x, 0x%x)\n", (unsigned)x, (unsigned)y); } checked++; }
#define CMP1(B, UT, OP)                                                                  \
    { UT x = (UT)rnd();                                                                  \
      if ((UT)nf_##OP##B(x) != (UT)alu_##OP##B(&c, x) && bad++ < 10)                     \
          { printf("  FAIL: nf_" #OP #B "(0x%x)\n", (unsigned)x); } checked++; }
#define CMPSH(B, UT, OP)                                                                 \
    { UT x = (UT)rnd(); uint32_t n = (uint32_t)(rnd() % 64u);                            \
      if ((UT)nf_##OP##B(x, n) != (UT)alu_##OP##B(&c, x, n) && bad++ < 10)               \
          { printf("  FAIL: nf_" #OP #B "(0x%x, %u)\n", (unsigned)x, n); } checked++; }
    for (int i = 0; i < 200000; i++) {
        CMP2(8, uint8_t, add) CMP2(16, uint16_t, add) CMP2(32, uint32_t, add)
        CMP2(8, uint8_t, sub) CMP2(16, uint16_t, sub) CMP2(32, uint32_t, sub)
        CMP1(8, uint8_t, inc) CMP1(16, uint16_t, inc) CMP1(32, uint32_t, inc)
        CMP1(8, uint8_t, dec) CMP1(16, uint16_t, dec) CMP1(32, uint32_t, dec)
        CMP1(8, uint8_t, neg) CMP1(16, uint16_t, neg) CMP1(32, uint32_t, neg)
        CMPSH(8, uint8_t, shl) CMPSH(16, uint16_t, shl) CMPSH(32, uint32_t, shl)
        CMPSH(8, uint8_t, shr) CMPSH(16, uint16_t, shr) CMPSH(32, uint32_t, shr)
        CMPSH(8, uint8_t, sar) CMPSH(16, uint16_t, sar) CMPSH(32, uint32_t, sar)
        { uint16_t x = (uint16_t)rnd(), y = (uint16_t)rnd();
          if (nf_imul2_16(x, y) != alu_imul2_16(&c, x, y) && bad++ < 10) { printf("  FAIL: nf_imul2_16\n"); } checked++; }
        { uint32_t x = (uint32_t)rnd(), y = (uint32_t)rnd();
          if (nf_imul2_32(x, y) != alu_imul2_32(&c, x, y) && bad++ < 10) { printf("  FAIL: nf_imul2_32\n"); } checked++; }
    }
#undef CMP2
#undef CMP1
#undef CMPSH
    if (bad) g_fail = 1;
    printf("[nf] %d flag-free results checked against alu_* helpers (%d mismatches)\n", checked, bad);
}

int main(void) {
    char tmpl[] = "/tmp/mha2_test_XXXXXX";
    if (mkdtemp(tmpl) && chdir(tmpl) != 0) perror("chdir");
    runtime_mem_init();

    int checked = 0, de_cases = 0;
    for (int bits = 8; bits <= 32; bits *= 2) {
        for (int sgn = 0; sgn <= 1; sgn++) {
            int de_budget = 6;
            for (int i = 0; i < 200000; i++) {
                uint64_t num = pick(2 * bits), div = pick(bits);
                uint64_t rq = 0, rr = 0;
                if (reference(bits, sgn, num, div, &rq, &rr)) {
                    if (de_budget-- > 0) { expect_de(bits, sgn, num, div); de_cases++; }
                    continue;
                }
                CpuCtx c;
                load(&c, bits, num);
                exec(&c, bits, sgn, div);
                uint64_t q = 0, r = 0;
                result(&c, bits, &q, &r);
                if (q != rq || r != rr) {
                    if (g_fail++ < 10)
                        printf("  FAIL: %s%d 0x%llx / 0x%llx = q 0x%llx r 0x%llx, want q 0x%llx r 0x%llx\n",
                               sgn ? "idiv" : "div", bits, (unsigned long long)num, (unsigned long long)div,
                               (unsigned long long)q, (unsigned long long)r, (unsigned long long)rq,
                               (unsigned long long)rr);
                }
                checked++;
            }
        }
    }
    /* Explicit boundary #DE cases, including the ones that trap in naive host code */
    expect_de(32, 0, 0x100000000ull, 1);                 /* div32 quotient overflow */
    expect_de(32, 1, 0x8000000000000000ull, 0xFFFFFFFFu); /* idiv32 INT64_MIN / -1 */
    expect_de(16, 1, 0x80000000ull, 0xFFFFu);             /* idiv16 INT32_MIN / -1 */
    expect_de(8, 1, 0x8000u, 0xFFu);                      /* idiv8  -32768 / -1 */
    expect_de(32, 0, 5, 0);                               /* div32 by zero */
    de_cases += 5;

    test_fist();
    test_nf_helpers();
    printf("[div] %d results checked against __int128 reference, %d #DE cases\n", checked, de_cases);
    printf(g_fail ? "FAIL test_cpu_semantics\n" : "PASS test_cpu_semantics\n");
    return g_fail != 0;
}
