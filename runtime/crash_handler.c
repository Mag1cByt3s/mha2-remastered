/*
 * Fatal signal reporter for the recompiled guest.
 *
 * Classifies faults against the reserved 4 GiB guest window (null page, wild
 * pointer beyond committed memory, image/heap/stack), dumps the faulting guest
 * thread's CpuCtx, and prints a symbolized host backtrace — lifted functions are
 * named sub_<guest VA>, so the backtrace doubles as a guest call stack. The report
 * goes to stderr and mha2_crash.log, then the signal is re-raised with its default
 * action so exit codes and core dumps are unchanged.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cpu.h"
#include <execinfo.h>
#include <stdarg.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#define ALTSTACK_SIZE (128u * 1024u)
#define CRASH_LOG     "mha2_crash.log"

static __thread CpuCtx *tl_guest_ctx;
static __thread void *tl_altstack;
static volatile sig_atomic_t g_in_crash;

static void out(int fd_log, const char *s, size_t n) {
    ssize_t r = write(STDERR_FILENO, s, n);
    if (fd_log >= 0) r = write(fd_log, s, n);
    (void)r;
}

static void outf(int fd_log, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(int fd_log, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) out(fd_log, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

static const char *guest_region(uint32_t va) {
    if (va < GUEST_NULL_GUARD_SIZE) return "guest NULL page (null-pointer dereference)";
    if (va >= GUEST_MEM_SIZE) return "outside committed guest memory (wild pointer)";
    if (va >= GUEST_STACK_BASE) return "guest thread stacks";
    if (va >= GUEST_HEAP_BASE) return "guest heap";
    if (va >= GUEST_TEB_BASE && va < GUEST_HEAP_BASE) return "guest TEBs";
    if (va >= 0x00400000u && va < GUEST_COM_VTABLES_BASE) return "guest PE image";
    return "guest low memory";
}

static const char *sig_cause(int sig, int code) {
    switch (sig) {
    case SIGSEGV: return code == SEGV_ACCERR ? "access violation (protected page)" : "access violation (unmapped)";
    case SIGBUS:  return "bus error";
    case SIGILL:  return "illegal instruction";
    case SIGABRT: return "abort()";
    case SIGFPE:
        if (code == FPE_INTDIV) return "integer divide by zero (#DE)";
        if (code == FPE_INTOVF) return "integer overflow (#DE)";
        return "floating-point exception";
    default: return "fatal signal";
    }
}

static void crash_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    if (g_in_crash) _exit(128 + sig); /* fault inside the reporter */
    g_in_crash = 1;

    int fd = open(CRASH_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    outf(fd, "\n==== MHA2 CRASH: signal %d (%s) ====\n", sig, sig_cause(sig, si->si_code));

    uintptr_t fault = (uintptr_t)si->si_addr;
    uintptr_t base = (uintptr_t)g_mem;
    if (si->si_code <= 0) {
        outf(fd, "signal sent by pid %d (not a hardware fault)\n", (int)si->si_pid);
    } else if (sig != SIGABRT) {
        if (g_mem && fault >= base && fault < base + GUEST_RESERVE_SIZE) {
            uint64_t off = (uint64_t)(fault - base);
            if (off > 0xFFFFFFFFull)
                outf(fd, "fault: guest VA 0x%08x + %llu bytes past 4 GiB (wrapped access)\n",
                     0xFFFFFFFFu, (unsigned long long)(off - 0xFFFFFFFFull));
            else
                outf(fd, "fault: guest VA 0x%08x — %s\n", (uint32_t)off, guest_region((uint32_t)off));
        } else {
            outf(fd, "fault: host address %p (not guest memory)\n", si->si_addr);
        }
    }

    CpuCtx *c = tl_guest_ctx;
    if (c) {
        outf(fd, "guest thread %u: eax=%08x ecx=%08x edx=%08x ebx=%08x\n",
             c->thread_id, c->eax, c->ecx, c->edx, c->ebx);
        outf(fd, "  esp=%08x ebp=%08x esi=%08x edi=%08x  cf=%u zf=%u sf=%u of=%u fpu_top=%d\n",
             c->esp, c->ebp, c->esi, c->edi, c->cf, c->zf, c->sf, c->of, c->fpu_top);
        if (c->esp >= GUEST_STACK_BASE && c->esp <= GUEST_MEM_SIZE - 32u) {
            outf(fd, "  [esp] =");
            for (int i = 0; i < 8; i++) outf(fd, " %08x", mem_r32(c->esp + (uint32_t)i * 4u));
            outf(fd, "\n");
        }
    } else {
        outf(fd, "(not a guest thread)\n");
    }

    void *frames[64];
    int n = backtrace(frames, 64);
    outf(fd, "host backtrace (sub_XXXXXXXX = lifted guest function at that VA):\n");
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    if (fd >= 0) {
        backtrace_symbols_fd(frames, n, fd);
        close(fd);
    }
    outf(-1, "crash report written to %s\n", CRASH_LOG);

    signal(sig, SIG_DFL);
    raise(sig);
}

void guest_divide_error(CpuCtx *c, int bits, int is_signed, uint64_t dividend_hi,
                        uint64_t dividend_lo, uint32_t divisor) {
    if (bits == 32)
        fprintf(stderr, "FATAL: guest %s%d divide error (#DE): edx:eax=%08llx:%08llx / 0x%08x (%s)\n",
                is_signed ? "idiv" : "div", bits, (unsigned long long)dividend_hi,
                (unsigned long long)dividend_lo, divisor, divisor ? "quotient overflow" : "divide by zero");
    else
        fprintf(stderr, "FATAL: guest %s%d divide error (#DE): dividend=0x%llx / 0x%x (%s)\n",
                is_signed ? "idiv" : "div", bits, (unsigned long long)dividend_lo, divisor,
                divisor ? "quotient overflow" : "divide by zero");
    if (c && !tl_guest_ctx) tl_guest_ctx = c;
    abort();
}

void crash_handler_thread_init(CpuCtx *ctx) {
    tl_guest_ctx = ctx;
    if (!tl_altstack) {
        tl_altstack = malloc(ALTSTACK_SIZE);
        if (tl_altstack) {
            stack_t ss = { .ss_sp = tl_altstack, .ss_size = ALTSTACK_SIZE, .ss_flags = 0 };
            sigaltstack(&ss, NULL);
        }
    }
}

void crash_handler_thread_exit(void) {
    tl_guest_ctx = NULL;
    if (tl_altstack) {
        stack_t ss = { .ss_sp = NULL, .ss_size = 0, .ss_flags = SS_DISABLE };
        sigaltstack(&ss, NULL);
        free(tl_altstack);
        tl_altstack = NULL;
    }
}

void crash_handler_install(void) {
    /* Pre-load libgcc's unwinder so backtrace() never allocates inside the handler. */
    void *warm[1];
    backtrace(warm, 1);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
}
