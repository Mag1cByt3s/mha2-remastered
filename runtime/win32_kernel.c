#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include "gl_renderer.h"
#include "hd_assets.h"
#include "imports.h"
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <fnmatch.h>

static __thread uint32_t tl_last_error = 0;

void win32_set_last_error(uint32_t err) { tl_last_error = err; }
uint32_t win32_get_last_error(void) { return tl_last_error; }

/* ---- Case-insensitive path resolution into g_game_dir ---- */
static int find_ci_component(const char *dir, const char *name, char *matched, size_t msz) {
    DIR *d = opendir(dir[0] ? dir : ".");
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcasecmp(de->d_name, name) == 0) {
            snprintf(matched, msz, "%s", de->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

void resolve_game_path(const char *win_path, char *out_linux, size_t out_sz, int for_write) {
    char clean[1024];
    size_t j = 0;
    const char *p = win_path ? win_path : "";

    /* Strip leading drive letter like C:\ */
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        p += 2;
        if (*p == '\\' || *p == '/') p++;
    }
    while (*p == '\\' || *p == '/') p++;
    /* Strip leading .\ or ./ */
    while (p[0] == '.' && (p[1] == '\\' || p[1] == '/')) p += 2;

    for (; *p && j + 1 < sizeof(clean); p++) {
        clean[j++] = (*p == '\\') ? '/' : *p;
    }
    clean[j] = '\0';

    /* If clean already starts with g_game_dir/, avoid double-prefixing */
    char cur[1024];
    size_t gdl = strlen(g_game_dir);
    if (strncmp(clean, g_game_dir, gdl) == 0 && (clean[gdl] == '/' || clean[gdl] == '\0')) {
        snprintf(cur, sizeof(cur), "%s", clean);
        snprintf(out_linux, out_sz, "%s", cur);
        return;
    }

    snprintf(cur, sizeof(cur), "%s", g_game_dir);
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", clean);
    char *saveptr = NULL;
    char *tok = strtok_r(tmp, "/", &saveptr);
    while (tok) {
        if (strcmp(tok, ".") == 0) {
            tok = strtok_r(NULL, "/", &saveptr);
            continue;
        }
        char matched[256];
        char next_path[1400];
        if (find_ci_component(cur, tok, matched, sizeof(matched))) {
            snprintf(next_path, sizeof(next_path), "%s/%s", cur, matched);
        } else {
            if (for_write) {
                /* Ensure parent directory exists */
                mkdir(cur, 0755);
            }
            snprintf(next_path, sizeof(next_path), "%s/%s", cur, tok);
        }
        snprintf(cur, sizeof(cur), "%.*s", (int)(sizeof(cur) - 1), next_path);
        tok = strtok_r(NULL, "/", &saveptr);
    }
    snprintf(out_linux, out_sz, "%s", cur);
}

/* ---- Handle Table ---- */
enum HandleType {
    HT_NONE = 0,
    HT_FILE,
    HT_EVENT,
    HT_SEMAPHORE,
    HT_MUTEX,
    HT_THREAD,
    HT_FIND,
};

typedef struct FindState {
    char dir_path[1024];
    char pattern[256];
    DIR *dir;
} FindState;

typedef struct SyncObj {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    int refcount;
    int manual_reset;
    int signaled;
    int count;
    int max_count;
    int owner_tid;
    int lock_depth;
} SyncObj;

static void sync_unref(SyncObj *so) {
    if (!so) return;
    if (__atomic_sub_fetch(&so->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        pthread_mutex_destroy(&so->mtx);
        pthread_cond_destroy(&so->cond);
        free(so);
    }
}

typedef struct ThreadObj {
    pthread_t pt;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    int refcount;
    uint32_t entry_va;
    uint32_t param_va;
    uint32_t teb_va;
    uint32_t stack_top;
    uint32_t tid;
    int suspended;
    int finished;
    uint32_t exit_code;
} ThreadObj;

static void thread_unref(ThreadObj *to) {
    if (!to) return;
    if (__atomic_sub_fetch(&to->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        if (to->tid) guest_free_thread_slot(to->tid);
        pthread_mutex_destroy(&to->mtx);
        pthread_cond_destroy(&to->cond);
        free(to);
    }
}

typedef struct WinHandle {
    enum HandleType type;
    int fd;
    SyncObj *sync;
    ThreadObj *thr;
    FindState *find;
} WinHandle;

#define MAX_HANDLES 4096
static WinHandle g_handles[MAX_HANDLES];
static pthread_mutex_t g_hdl_mtx = PTHREAD_MUTEX_INITIALIZER;

static uint32_t alloc_handle(enum HandleType t) {
    pthread_mutex_lock(&g_hdl_mtx);
    for (uint32_t i = 0x100; i < MAX_HANDLES; i++) {
        if (g_handles[i].type == HT_NONE) {
            memset(&g_handles[i], 0, sizeof(WinHandle));
            g_handles[i].type = t;
            g_handles[i].fd = -1;
            pthread_mutex_unlock(&g_hdl_mtx);
            return i;
        }
    }
    pthread_mutex_unlock(&g_hdl_mtx);
    return 0;
}

static WinHandle *get_handle(uint32_t h) {
    if (h >= MAX_HANDLES || g_handles[h].type == HT_NONE) return NULL;
    return &g_handles[h];
}

/* ---- Critical Sections ---- */
#define MAX_CS 1024
static pthread_mutex_t g_cs_pool[MAX_CS];
static int g_cs_used[MAX_CS];
static pthread_mutex_t g_cs_alloc_mtx = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *get_or_init_cs(uint32_t cs_va) {
    uint32_t id = mem_r32(cs_va);
    if (id >= 1 && id < MAX_CS && g_cs_used[id]) {
        return &g_cs_pool[id];
    }
    pthread_mutex_lock(&g_cs_alloc_mtx);
    id = mem_r32(cs_va);
    if (id >= 1 && id < MAX_CS && g_cs_used[id]) {
        pthread_mutex_unlock(&g_cs_alloc_mtx);
        return &g_cs_pool[id];
    }
    for (uint32_t i = 1; i < MAX_CS; i++) {
        if (!g_cs_used[i]) {
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&g_cs_pool[i], &attr);
            pthread_mutexattr_destroy(&attr);
            g_cs_used[i] = 1;
            mem_w32(cs_va, i);
            pthread_mutex_unlock(&g_cs_alloc_mtx);
            return &g_cs_pool[i];
        }
    }
    pthread_mutex_unlock(&g_cs_alloc_mtx);
    return NULL;
}

static __thread ThreadObj *tl_thread_obj = NULL;

/* ---- Thread entry trampoline ---- */
static void *guest_thread_main(void *arg) {
    ThreadObj *to = (ThreadObj *)arg;
    tl_thread_obj = to;
    pthread_mutex_lock(&to->mtx);
    while (to->suspended) {
        pthread_cond_wait(&to->cond, &to->mtx);
    }
    pthread_mutex_unlock(&to->mtx);

    CpuCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.esp = to->stack_top;
    ctx.teb_addr = to->teb_va;
    ctx.thread_id = to->tid;
    ctx.fpu_cw = 0x037Fu;
    crash_handler_thread_init(&ctx);

    uint32_t a[1] = { to->param_va };
    uint32_t ret = guest_call_stdcall(&ctx, to->entry_va, 1, a);
    crash_handler_thread_exit();

    pthread_mutex_lock(&to->mtx);
    to->exit_code = ret;
    to->finished = 1;
    pthread_cond_broadcast(&to->cond);
    pthread_mutex_unlock(&to->mtx);
    tl_thread_obj = NULL;
    thread_unref(to);
    return NULL;
}

/* ---- Timing helpers ---- */
static uint64_t g_start_ns = 0;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static pthread_mutex_t g_vclock_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_vclock_last_real = 0;
static uint64_t g_vclock_acc_ns = 0;

static uint64_t guest_elapsed_ns(void) {
    uint64_t real = now_ns();
    pthread_mutex_lock(&g_vclock_mtx);
    if (g_vclock_last_real == 0) {
        g_vclock_last_real = g_start_ns ? g_start_ns : real;
    }
    if (real > g_vclock_last_real) {
        uint64_t delta = real - g_vclock_last_real;
        float mult = gl_renderer_get_speed_mult();
        if (mult != 1.0f) {
            g_vclock_acc_ns += (uint64_t)((double)delta * (double)mult);
        } else {
            g_vclock_acc_ns += delta;
        }
        g_vclock_last_real = real;
    }
    uint64_t res = g_vclock_acc_ns;
    pthread_mutex_unlock(&g_vclock_mtx);
    return res;
}

static void make_abs_timeout(struct timespec *ts, uint32_t ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000u;
    ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

/* ---- Static guest strings for CRT ---- */
static uint32_t g_cmdline_va = 0;
static uint32_t g_env_a_va = 0;
static uint32_t g_env_w_va = 0;
static uint32_t g_tls_next = 1;
static pthread_mutex_t g_tls_mtx = PTHREAD_MUTEX_INITIALIZER;

void win32_kernel_init(void) {
    g_start_ns = now_ns();
    g_cmdline_va = guest_alloc_cstr("mha2.exe");
    static const char env_a[] = "PATH=C:\\WINDOWS\0TEMP=.\0\0";
    g_env_a_va = guest_alloc_static(env_a, sizeof(env_a));
    uint16_t env_w[64];
    size_t n = sizeof(env_a);
    for (size_t i = 0; i < n; i++) env_w[i] = (uint8_t)env_a[i];
    g_env_w_va = guest_alloc_static(env_w, (uint32_t)(n * 2));

    /* Standard handles 1, 2, 3 */
    g_handles[1].type = HT_FILE; g_handles[1].fd = 0;
    g_handles[2].type = HT_FILE; g_handles[2].fd = 1;
    g_handles[3].type = HT_FILE; g_handles[3].fd = 2;
}

static void fill_find_data(uint32_t dst_va, const char *full_path, const char *name) {
    memset(g_mem + dst_va, 0, 320);
    struct stat st;
    uint32_t attrs = 0x80; /* FILE_ATTRIBUTE_NORMAL */
    uint64_t fsz = 0;
    if (stat(full_path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) attrs = 0x10;
        fsz = (uint64_t)st.st_size;
        uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
        mem_w32(dst_va + 4, (uint32_t)ft);
        mem_w32(dst_va + 8, (uint32_t)(ft >> 32));
        mem_w32(dst_va + 12, (uint32_t)ft);
        mem_w32(dst_va + 16, (uint32_t)(ft >> 32));
        mem_w32(dst_va + 20, (uint32_t)ft);
        mem_w32(dst_va + 24, (uint32_t)(ft >> 32));
    }
    mem_w32(dst_va + 0, attrs);
    mem_w32(dst_va + 28, (uint32_t)(fsz >> 32));
    mem_w32(dst_va + 32, (uint32_t)fsz);
    snprintf((char *)(g_mem + dst_va + 44), 260, "%s", name);
}

static int find_next_match(FindState *fs, uint32_t dst_va) {
    struct dirent *de;
    while ((de = readdir(fs->dir)) != NULL) {
        if (fnmatch(fs->pattern, de->d_name, FNM_CASEFOLD) == 0) {
            char full[1280];
            snprintf(full, sizeof(full), "%s/%s", fs->dir_path, de->d_name);
            fill_find_data(dst_va, full, de->d_name);
            return 1;
        }
    }
    return 0;
}

/* Copy a NUL-terminated guest string into buf (bounded, printable-only). */
static void guest_cstr_copy(uint32_t va, char *buf, size_t cap) {
    size_t n = 0;
    if (va >= 0x00010000u && va < GUEST_MEM_SIZE) {
        for (; n + 1 < cap && va + n < GUEST_MEM_SIZE; n++) {
            uint8_t ch = mem_r8(va + (uint32_t)n);
            if (!ch) break;
            buf[n] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
        }
    }
    buf[n] = 0;
}

/* Diagnose a guest RaiseException before aborting. The only raiser in mha2.exe is
 * MSVC _CxxThrowException (0x004a8a2b), reached from the inlined STL _Xlen/_Xran
 * helpers (std::length_error / std::out_of_range / std::logic_error) — i.e. guest
 * programming errors. Decodes code 0xE06D7363's ThrowInfo -> CatchableTypeArray ->
 * TypeDescriptor names and calls the object's virtual what() (vtable slot 1). */
static void report_guest_exception(CpuCtx *c) {
    uint32_t code = arg_u32(c, 0), flags = arg_u32(c, 1);
    uint32_t nargs = arg_u32(c, 2), args = arg_u32(c, 3);
    fprintf(stderr, "FATAL: guest RaiseException(code=0x%08x, flags=0x%x, nargs=%u) from 0x%08x\n",
            code, flags, nargs, mem_r32(c->esp));
    if (code != 0xE06D7363u || nargs < 3u || !args || args >= GUEST_MEM_SIZE - 12u) return;

    uint32_t magic = mem_r32(args), obj = mem_r32(args + 4u), ti = mem_r32(args + 8u);
    fprintf(stderr, "  MSVC C++ exception (magic 0x%08x), object=0x%08x, ThrowInfo=0x%08x\n", magic, obj, ti);
    /* _CxxThrowException uses an EBP frame: [ebp+4] = return address in the thrower */
    if (c->ebp >= GUEST_STACK_BASE && c->ebp < GUEST_MEM_SIZE - 8u)
        fprintf(stderr, "  thrown from guest 0x%08x\n", mem_r32(c->ebp + 4u));
    if (ti && ti < GUEST_MEM_SIZE - 16u) {
        uint32_t cta = mem_r32(ti + 12u);
        uint32_t n = (cta && cta < GUEST_MEM_SIZE - 4u) ? mem_r32(cta) : 0u;
        for (uint32_t k = 0; k < n && k < 8u; k++) {
            uint32_t ct = mem_r32(cta + 4u + k * 4u);
            uint32_t td = (ct && ct < GUEST_MEM_SIZE - 8u) ? mem_r32(ct + 4u) : 0u;
            char name[128];
            guest_cstr_copy(td ? td + 8u : 0u, name, sizeof(name));
            fprintf(stderr, "  %s %s\n", k == 0 ? "type:     " : "catchable:", name[0] ? name : "?");
        }
    }
    if (obj >= 0x00010000u && obj < GUEST_MEM_SIZE - 4u) {
        uint32_t vtbl = mem_r32(obj);
        uint32_t what_fn = (vtbl >= 0x00400000u && vtbl < GUEST_MEM_SIZE - 8u) ? mem_r32(vtbl + 4u) : 0u;
        if (what_fn >= GUEST_TEXT_START && what_fn < GUEST_TEXT_END && g_code_table[what_fn - GUEST_TEXT_START]) {
            uint32_t s_ecx = c->ecx;
            c->ecx = obj; /* __thiscall const char *exception::what() const */
            uint32_t msg_va = guest_call_stdcall(c, what_fn, 0, NULL);
            c->ecx = s_ecx;
            char msg[256];
            guest_cstr_copy(msg_va, msg, sizeof(msg));
            fprintf(stderr, "  what():   \"%s\"\n", msg);
        }
    }
}

int win32_kernel_dispatch(CpuCtx *c, uint32_t import_id) {
    switch (import_id) {
        /* ---- Memory / Heap ---- */
        case IMP_KERNEL32_DLL_HeapCreate:
            stdcall_ret(c, 3, 0x00100001u); return 1;
        case IMP_KERNEL32_DLL_HeapDestroy:
            stdcall_ret(c, 1, 1); return 1;
        case IMP_KERNEL32_DLL_HeapAlloc: {
            uint32_t flags = arg_u32(c, 1);
            uint32_t sz = arg_u32(c, 2);
            uint32_t p = guest_alloc(sz);
            if (p && (flags & 0x08u)) memset(g_mem + p, 0, sz);
            stdcall_ret(c, 3, p); return 1;
        }
        case IMP_KERNEL32_DLL_HeapReAlloc: {
            uint32_t flags = arg_u32(c, 1);
            uint32_t ptr = arg_u32(c, 2);
            uint32_t sz = arg_u32(c, 3);
            uint32_t old_sz = guest_alloc_size(ptr);
            uint32_t p = guest_realloc(ptr, sz);
            if (p && (flags & 0x08u) && sz > old_sz) {
                memset(g_mem + p + old_sz, 0, sz - old_sz);
            }
            stdcall_ret(c, 4, p); return 1;
        }
        case IMP_KERNEL32_DLL_HeapFree:
            guest_free(arg_u32(c, 2));
            stdcall_ret(c, 3, 1); return 1;
        case IMP_KERNEL32_DLL_HeapSize:
            stdcall_ret(c, 3, guest_alloc_size(arg_u32(c, 2))); return 1;
        case IMP_KERNEL32_DLL_VirtualAlloc: {
            uint32_t addr = arg_u32(c, 0);
            uint32_t sz = arg_u32(c, 1);
            uint32_t p = addr ? addr : guest_alloc(sz);
            if (p && !addr) memset(g_mem + p, 0, sz);
            stdcall_ret(c, 4, p); return 1;
        }
        case IMP_KERNEL32_DLL_VirtualFree: {
            uint32_t addr = arg_u32(c, 0);
            uint32_t type = arg_u32(c, 2);
            if (type & 0x8000u) guest_free(addr);
            stdcall_ret(c, 3, 1); return 1;
        }
        case IMP_KERNEL32_DLL_IsBadReadPtr:
        case IMP_KERNEL32_DLL_IsBadWritePtr:
        case IMP_KERNEL32_DLL_IsBadCodePtr: {
            uint32_t p = arg_u32(c, 0);
            int nargs = (import_id == IMP_KERNEL32_DLL_IsBadCodePtr) ? 1 : 2;
            stdcall_ret(c, nargs, (p == 0 || p >= GUEST_MEM_SIZE) ? 1 : 0);
            return 1;
        }

        /* ---- Process / Module / Env / Version ---- */
        case IMP_KERNEL32_DLL_GetVersion:
            stdcall_ret(c, 0, 0x0A280105u); return 1; /* WinXP 5.1.2600 */
        case IMP_KERNEL32_DLL_GetVersionExA: {
            uint32_t p = arg_u32(c, 0);
            if (p) {
                uint32_t sz = mem_r32(p);
                memset(g_mem + p + 4, 0, (sz > 4 && sz <= 256) ? sz - 4 : 144);
                mem_w32(p + 4, 5);    /* Major */
                mem_w32(p + 8, 1);    /* Minor */
                mem_w32(p + 12, 2600);/* Build */
                mem_w32(p + 16, 2);   /* VER_PLATFORM_WIN32_NT */
            }
            stdcall_ret(c, 1, 1); return 1;
        }
        case IMP_KERNEL32_DLL_GetCommandLineA:
            stdcall_ret(c, 0, g_cmdline_va); return 1;
        case IMP_KERNEL32_DLL_GetStartupInfoA: {
            uint32_t p = arg_u32(c, 0);
            if (p) {
                memset(g_mem + p, 0, 68);
                mem_w32(p + 0, 68);
                mem_w32(p + 44, 1);  /* STARTF_USESHOWWINDOW */
                mem_w16(p + 48, 1);  /* SW_SHOWNORMAL */
            }
            stdcall_ret(c, 1, 0); return 1;
        }
        case IMP_KERNEL32_DLL_GetModuleHandleA:
            stdcall_ret(c, 1, 0x00400000u); return 1;
        case IMP_KERNEL32_DLL_GetModuleFileNameA: {
            uint32_t buf = arg_u32(c, 1);
            uint32_t bsz = arg_u32(c, 2);
            const char *nm = "C:\\mha2\\mha2.exe";
            uint32_t len = (uint32_t)snprintf((char *)(g_mem + buf), bsz, "%s", nm);
            stdcall_ret(c, 3, len); return 1;
        }
        case IMP_KERNEL32_DLL_GetCurrentProcess:
            stdcall_ret(c, 0, 0xFFFFFFFFu); return 1;
        case IMP_KERNEL32_DLL_GetCurrentThreadId:
            stdcall_ret(c, 0, c->thread_id ? c->thread_id : 1000u); return 1;
        case IMP_KERNEL32_DLL_GetEnvironmentStrings:
            stdcall_ret(c, 0, g_env_a_va); return 1;
        case IMP_KERNEL32_DLL_GetEnvironmentStringsW:
            stdcall_ret(c, 0, g_env_w_va); return 1;
        case IMP_KERNEL32_DLL_FreeEnvironmentStringsA:
        case IMP_KERNEL32_DLL_FreeEnvironmentStringsW:
            stdcall_ret(c, 1, 1); return 1;
        case IMP_KERNEL32_DLL_GetEnvironmentVariableA:
            stdcall_ret(c, 3, 0); return 1;
        case IMP_KERNEL32_DLL_SetEnvironmentVariableA:
            stdcall_ret(c, 2, 1); return 1;
        case IMP_KERNEL32_DLL_SetHandleCount:
            stdcall_ret(c, 1, arg_u32(c, 0)); return 1;
        case IMP_KERNEL32_DLL_GetStdHandle: {
            uint32_t which = arg_u32(c, 0);
            uint32_t h = (which == (uint32_t)-10) ? 1 : (which == (uint32_t)-11) ? 2 : 3;
            stdcall_ret(c, 1, h); return 1;
        }
        case IMP_KERNEL32_DLL_SetStdHandle:
            stdcall_ret(c, 2, 1); return 1;
        case IMP_KERNEL32_DLL_LoadLibraryA: {
            const char *name = arg_cstr(c, 0);
            if (g_debug_log) fprintf(stderr, "[KERNEL32] LoadLibraryA(%s)\n", name ? name : "NULL");
            stdcall_ret(c, 1, 0x00400000u); return 1;
        }
        case IMP_KERNEL32_DLL_GetProcAddress: {
            const char *name = arg_cstr(c, 1);
            if (g_debug_log) fprintf(stderr, "[KERNEL32] GetProcAddress(%s)\n", name ? name : "NULL");
            /* Check if name matches any known import */
            if (name && arg_u32(c, 1) > 0xFFFFu) {
                for (uint32_t i = 0; i < MHA2_IMPORT_COUNT; i++) {
                    if (strcmp(g_imports[i].name, name) == 0) {
                        stdcall_ret(c, 2, HLE_WIN32_BASE + g_imports[i].id);
                        return 1;
                    }
                }
            }
            stdcall_ret(c, 2, 0); return 1;
        }
        case IMP_KERNEL32_DLL_ExitProcess:
        case IMP_KERNEL32_DLL_TerminateProcess: {
            uint32_t code = (import_id == IMP_KERNEL32_DLL_TerminateProcess)
                            ? arg_u32(c, 1) : arg_u32(c, 0);
            win32_dsound_shutdown();
            hd_assets_shutdown();
            /* OpenGL/SDL teardown must only run from the thread owning the GL context */
            if (!gl_renderer_is_active() || gl_renderer_is_gl_thread()) {
                gl_renderer_shutdown();
                SDL_Quit();
            }
            exit((int)code);
        }
        case IMP_KERNEL32_DLL_OutputDebugStringA: {
            const char *s = arg_cstr(c, 0);
            if (s && g_debug_log) fprintf(stderr, "[DBG] %s", s);
            stdcall_ret(c, 1, 0); return 1;
        }
        case IMP_KERNEL32_DLL_GetLastError:
            stdcall_ret(c, 0, win32_get_last_error()); return 1;
        case IMP_KERNEL32_DLL_SetLastError:
            win32_set_last_error(arg_u32(c, 0));
            stdcall_ret(c, 1, 0); return 1;
        case IMP_KERNEL32_DLL_SetUnhandledExceptionFilter:
            stdcall_ret(c, 1, 0); return 1;
        case IMP_KERNEL32_DLL_UnhandledExceptionFilter:
            stdcall_ret(c, 1, 1); return 1;
        case IMP_KERNEL32_DLL_RaiseException:
            /* No guest SEH unwinding: report precisely, then abort() so the crash
             * reporter prints the guest call stack (runtime/crash_handler.c). */
            report_guest_exception(c);
            abort();
        case IMP_KERNEL32_DLL_RtlUnwind:
            stdcall_ret(c, 4, 0); return 1;

        /* ---- Atomics & TLS ---- */
        case IMP_KERNEL32_DLL_InterlockedIncrement: {
            uint32_t p = arg_u32(c, 0);
            int32_t v = __atomic_add_fetch((int32_t *)(g_mem + p), 1, __ATOMIC_SEQ_CST);
            stdcall_ret(c, 1, (uint32_t)v); return 1;
        }
        case IMP_KERNEL32_DLL_InterlockedDecrement: {
            uint32_t p = arg_u32(c, 0);
            int32_t v = __atomic_sub_fetch((int32_t *)(g_mem + p), 1, __ATOMIC_SEQ_CST);
            stdcall_ret(c, 1, (uint32_t)v); return 1;
        }
        case IMP_KERNEL32_DLL_TlsAlloc: {
            pthread_mutex_lock(&g_tls_mtx);
            uint32_t idx = g_tls_next++;
            pthread_mutex_unlock(&g_tls_mtx);
            stdcall_ret(c, 0, idx); return 1;
        }
        case IMP_KERNEL32_DLL_TlsGetValue: {
            uint32_t idx = arg_u32(c, 0);
            uint32_t val = (idx < 64 && c->teb_addr) ? mem_r32(c->teb_addr + 0xE10u + idx * 4u) : 0;
            stdcall_ret(c, 1, val); return 1;
        }
        case IMP_KERNEL32_DLL_TlsSetValue: {
            uint32_t idx = arg_u32(c, 0);
            uint32_t val = arg_u32(c, 1);
            if (idx < 64 && c->teb_addr) mem_w32(c->teb_addr + 0xE10u + idx * 4u, val);
            stdcall_ret(c, 2, 1); return 1;
        }

        /* ---- Critical Sections ---- */
        case IMP_KERNEL32_DLL_InitializeCriticalSection: {
            uint32_t cs = arg_u32(c, 0);
            mem_w32(cs, 0);
            get_or_init_cs(cs);
            stdcall_ret(c, 1, 0); return 1;
        }
        case IMP_KERNEL32_DLL_EnterCriticalSection: {
            pthread_mutex_t *m = get_or_init_cs(arg_u32(c, 0));
            if (m) pthread_mutex_lock(m);
            stdcall_ret(c, 1, 0); return 1;
        }
        case IMP_KERNEL32_DLL_LeaveCriticalSection: {
            pthread_mutex_t *m = get_or_init_cs(arg_u32(c, 0));
            if (m) pthread_mutex_unlock(m);
            stdcall_ret(c, 1, 0); return 1;
        }
        case IMP_KERNEL32_DLL_DeleteCriticalSection: {
            uint32_t cs = arg_u32(c, 0);
            pthread_mutex_lock(&g_cs_alloc_mtx);
            uint32_t id = mem_r32(cs);
            if (id >= 1 && id < MAX_CS && g_cs_used[id]) {
                pthread_mutex_destroy(&g_cs_pool[id]);
                g_cs_used[id] = 0;
                mem_w32(cs, 0);
            }
            pthread_mutex_unlock(&g_cs_alloc_mtx);
            stdcall_ret(c, 1, 0); return 1;
        }

        /* ---- Threads & Sync ---- */
        case IMP_KERNEL32_DLL_CreateThread: {
            uint32_t entry = arg_u32(c, 2);
            uint32_t param = arg_u32(c, 3);
            uint32_t flags = arg_u32(c, 4);
            uint32_t tid_ptr = arg_u32(c, 5);

            uint32_t h = alloc_handle(HT_THREAD);
            if (!h) {
                win32_set_last_error(8u /* ERROR_NOT_ENOUGH_MEMORY */);
                stdcall_ret(c, 6, 0); return 1;
            }
            ThreadObj *to = (ThreadObj *)calloc(1, sizeof(ThreadObj));
            if (!to) {
                pthread_mutex_lock(&g_hdl_mtx);
                g_handles[h].type = HT_NONE;
                pthread_mutex_unlock(&g_hdl_mtx);
                stdcall_ret(c, 6, 0); return 1;
            }
            pthread_mutex_init(&to->mtx, NULL);
            pthread_cond_init(&to->cond, NULL);
            to->refcount = 2; /* 1 for handle, 1 for guest_thread_main */
            to->entry_va = entry;
            to->param_va = param;
            to->suspended = (flags & 0x4u) ? 1 : 0;
            to->tid = guest_alloc_thread_slot(&to->teb_va, &to->stack_top);
            if (!to->tid) {
                pthread_mutex_destroy(&to->mtx);
                pthread_cond_destroy(&to->cond);
                free(to);
                pthread_mutex_lock(&g_hdl_mtx);
                g_handles[h].type = HT_NONE;
                pthread_mutex_unlock(&g_hdl_mtx);
                stdcall_ret(c, 6, 0); return 1;
            }
            pthread_mutex_lock(&g_hdl_mtx);
            g_handles[h].thr = to;
            pthread_mutex_unlock(&g_hdl_mtx);
            if (tid_ptr) mem_w32(tid_ptr, to->tid);
            /* Explicit host stack: lifted guest calls nest as native C calls, so don't
             * depend on RLIMIT_STACK-derived defaults (2 MiB with `ulimit -s unlimited`). */
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, GUEST_HOST_THREAD_STACK);
            int pc_rc = pthread_create(&to->pt, &attr, guest_thread_main, to);
            pthread_attr_destroy(&attr);
            if (pc_rc != 0) {
                pthread_mutex_lock(&g_hdl_mtx);
                g_handles[h].thr = NULL;
                g_handles[h].type = HT_NONE;
                pthread_mutex_unlock(&g_hdl_mtx);
                guest_free_thread_slot(to->tid);
                pthread_mutex_destroy(&to->mtx);
                pthread_cond_destroy(&to->cond);
                free(to);
                stdcall_ret(c, 6, 0); return 1;
            }
            if (g_debug_log) fprintf(stderr, "[KERNEL32] CreateThread(entry=0x%08x, param=0x%08x, flags=0x%x) -> h=0x%x tid=%u\n", entry, param, flags, h, to->tid);
            stdcall_ret(c, 6, h); return 1;
        }
        case IMP_KERNEL32_DLL_ResumeThread: {
            uint32_t h = arg_u32(c, 0);
            pthread_mutex_lock(&g_hdl_mtx);
            WinHandle *wh = get_handle(h);
            ThreadObj *to = (wh && wh->type == HT_THREAD) ? wh->thr : NULL;
            if (to) __atomic_add_fetch(&to->refcount, 1, __ATOMIC_ACQ_REL);
            pthread_mutex_unlock(&g_hdl_mtx);
            uint32_t prev = 0;
            if (to) {
                pthread_mutex_lock(&to->mtx);
                prev = (uint32_t)to->suspended;
                to->suspended = 0;
                pthread_cond_broadcast(&to->cond);
                pthread_mutex_unlock(&to->mtx);
                thread_unref(to);
            }
            stdcall_ret(c, 1, prev); return 1;
        }
        case IMP_KERNEL32_DLL_GetExitCodeThread: {
            uint32_t h = arg_u32(c, 0);
            uint32_t out = arg_u32(c, 1);
            pthread_mutex_lock(&g_hdl_mtx);
            WinHandle *wh = get_handle(h);
            ThreadObj *to = (wh && wh->type == HT_THREAD) ? wh->thr : NULL;
            if (to) __atomic_add_fetch(&to->refcount, 1, __ATOMIC_ACQ_REL);
            pthread_mutex_unlock(&g_hdl_mtx);
            if (to && out) {
                pthread_mutex_lock(&to->mtx);
                uint32_t code = to->finished ? to->exit_code : 259u /* STILL_ACTIVE */;
                pthread_mutex_unlock(&to->mtx);
                mem_w32(out, code);
                thread_unref(to);
                stdcall_ret(c, 2, 1); return 1;
            }
            if (to) thread_unref(to);
            stdcall_ret(c, 2, 0); return 1;
        }
        case IMP_KERNEL32_DLL_ExitThread: {
            uint32_t code = arg_u32(c, 0);
            if (tl_thread_obj) {
                ThreadObj *to = tl_thread_obj;
                tl_thread_obj = NULL;
                pthread_mutex_lock(&to->mtx);
                to->exit_code = code;
                to->finished = 1;
                pthread_cond_broadcast(&to->cond);
                pthread_mutex_unlock(&to->mtx);
                thread_unref(to);
            }
            pthread_exit(NULL);
            return 1;
        }
        case IMP_KERNEL32_DLL_CreateEventA: {
            uint32_t h = alloc_handle(HT_EVENT);
            if (!h) { stdcall_ret(c, 4, 0); return 1; }
            SyncObj *so = (SyncObj *)calloc(1, sizeof(SyncObj));
            pthread_mutex_init(&so->mtx, NULL);
            pthread_cond_init(&so->cond, NULL);
            so->refcount = 1;
            so->manual_reset = arg_u32(c, 1) ? 1 : 0;
            so->signaled = arg_u32(c, 2) ? 1 : 0;
            g_handles[h].sync = so;
            stdcall_ret(c, 4, h); return 1;
        }
        case IMP_KERNEL32_DLL_ResetEvent: {
            pthread_mutex_lock(&g_hdl_mtx);
            WinHandle *wh = get_handle(arg_u32(c, 0));
            SyncObj *so = (wh && wh->type == HT_EVENT) ? wh->sync : NULL;
            if (so) __atomic_add_fetch(&so->refcount, 1, __ATOMIC_ACQ_REL);
            pthread_mutex_unlock(&g_hdl_mtx);
            if (so) {
                pthread_mutex_lock(&so->mtx);
                so->signaled = 0;
                pthread_mutex_unlock(&so->mtx);
                sync_unref(so);
                stdcall_ret(c, 1, 1); return 1;
            }
            stdcall_ret(c, 1, 0); return 1;
        }
        case IMP_KERNEL32_DLL_CreateSemaphoreA: {
            uint32_t h = alloc_handle(HT_SEMAPHORE);
            if (!h) { stdcall_ret(c, 4, 0); return 1; }
            SyncObj *so = (SyncObj *)calloc(1, sizeof(SyncObj));
            pthread_mutex_init(&so->mtx, NULL);
            pthread_cond_init(&so->cond, NULL);
            so->refcount = 1;
            so->count = arg_i32(c, 1);
            so->max_count = arg_i32(c, 2);
            g_handles[h].sync = so;
            stdcall_ret(c, 4, h); return 1;
        }
        case IMP_KERNEL32_DLL_ReleaseSemaphore: {
            pthread_mutex_lock(&g_hdl_mtx);
            WinHandle *wh = get_handle(arg_u32(c, 0));
            SyncObj *so = (wh && wh->type == HT_SEMAPHORE) ? wh->sync : NULL;
            if (so) __atomic_add_fetch(&so->refcount, 1, __ATOMIC_ACQ_REL);
            pthread_mutex_unlock(&g_hdl_mtx);
            int32_t rel = arg_i32(c, 1);
            uint32_t prev_ptr = arg_u32(c, 2);
            if (so) {
                pthread_mutex_lock(&so->mtx);
                if (prev_ptr) mem_w32(prev_ptr, (uint32_t)so->count);
                so->count += rel;
                if (so->count > so->max_count) so->count = so->max_count;
                pthread_cond_broadcast(&so->cond);
                pthread_mutex_unlock(&so->mtx);
                sync_unref(so);
                stdcall_ret(c, 3, 1); return 1;
            }
            stdcall_ret(c, 3, 0); return 1;
        }
        case IMP_KERNEL32_DLL_CreateMutexA: {
            uint32_t h = alloc_handle(HT_MUTEX);
            if (!h) { stdcall_ret(c, 3, 0); return 1; }
            SyncObj *so = (SyncObj *)calloc(1, sizeof(SyncObj));
            pthread_mutex_init(&so->mtx, NULL);
            pthread_cond_init(&so->cond, NULL);
            so->refcount = 1;
            if (arg_u32(c, 1)) {
                so->owner_tid = (int)(c->thread_id ? c->thread_id : 1000u);
                so->lock_depth = 1;
            }
            g_handles[h].sync = so;
            stdcall_ret(c, 3, h); return 1;
        }
        case IMP_KERNEL32_DLL_ReleaseMutex: {
            pthread_mutex_lock(&g_hdl_mtx);
            WinHandle *wh = get_handle(arg_u32(c, 0));
            SyncObj *so = (wh && wh->type == HT_MUTEX) ? wh->sync : NULL;
            if (so) __atomic_add_fetch(&so->refcount, 1, __ATOMIC_ACQ_REL);
            pthread_mutex_unlock(&g_hdl_mtx);
            if (so) {
                pthread_mutex_lock(&so->mtx);
                if (so->lock_depth > 0) {
                    so->lock_depth--;
                    if (so->lock_depth == 0) {
                        so->owner_tid = 0;
                        pthread_cond_signal(&so->cond);
                    }
                }
                pthread_mutex_unlock(&so->mtx);
                sync_unref(so);
                stdcall_ret(c, 1, 1); return 1;
            }
            stdcall_ret(c, 1, 0); return 1;
        }
        case IMP_KERNEL32_DLL_WaitForSingleObject: {
            uint32_t h = arg_u32(c, 0);
            uint32_t ms = arg_u32(c, 1);
            pthread_mutex_lock(&g_hdl_mtx);
            WinHandle *wh = get_handle(h);
            enum HandleType htype = wh ? wh->type : HT_NONE;
            SyncObj *so = (wh && (htype == HT_EVENT || htype == HT_SEMAPHORE || htype == HT_MUTEX)) ? wh->sync : NULL;
            ThreadObj *to = (wh && htype == HT_THREAD) ? wh->thr : NULL;
            if (so) __atomic_add_fetch(&so->refcount, 1, __ATOMIC_ACQ_REL);
            if (to) __atomic_add_fetch(&to->refcount, 1, __ATOMIC_ACQ_REL);
            pthread_mutex_unlock(&g_hdl_mtx);
            if (htype == HT_NONE) { stdcall_ret(c, 2, 0xFFFFFFFFu); return 1; }

            if (htype == HT_THREAD && to) {
                pthread_mutex_lock(&to->mtx);
                uint32_t res = 0;
                if (!to->finished) {
                    if (ms == 0) res = 0x102u;
                    else if (ms == 0xFFFFFFFFu) {
                        while (!to->finished) pthread_cond_wait(&to->cond, &to->mtx);
                    } else {
                        struct timespec ts;
                        make_abs_timeout(&ts, ms);
                        while (!to->finished) {
                            if (pthread_cond_timedwait(&to->cond, &to->mtx, &ts) == ETIMEDOUT) {
                                res = 0x102u; break;
                            }
                        }
                    }
                }
                pthread_mutex_unlock(&to->mtx);
                thread_unref(to);
                stdcall_ret(c, 2, res); return 1;
            }
            if (htype == HT_SEMAPHORE && so) {
                pthread_mutex_lock(&so->mtx);
                uint32_t res = 0;
                if (so->count <= 0) {
                    if (ms == 0) res = 0x102u;
                    else if (ms == 0xFFFFFFFFu) {
                        while (so->count <= 0) pthread_cond_wait(&so->cond, &so->mtx);
                    } else {
                        struct timespec ts;
                        make_abs_timeout(&ts, ms);
                        while (so->count <= 0) {
                            if (pthread_cond_timedwait(&so->cond, &so->mtx, &ts) == ETIMEDOUT) {
                                res = 0x102u; break;
                            }
                        }
                    }
                }
                if (res == 0 && so->count > 0) so->count--;
                pthread_mutex_unlock(&so->mtx);
                sync_unref(so);
                stdcall_ret(c, 2, res); return 1;
            }
            if (htype == HT_EVENT && so) {
                pthread_mutex_lock(&so->mtx);
                uint32_t res = 0;
                if (!so->signaled) {
                    if (ms == 0) res = 0x102u;
                    else if (ms == 0xFFFFFFFFu) {
                        while (!so->signaled) pthread_cond_wait(&so->cond, &so->mtx);
                    } else {
                        struct timespec ts;
                        make_abs_timeout(&ts, ms);
                        while (!so->signaled) {
                            if (pthread_cond_timedwait(&so->cond, &so->mtx, &ts) == ETIMEDOUT) {
                                res = 0x102u; break;
                            }
                        }
                    }
                }
                if (res == 0 && !so->manual_reset) so->signaled = 0;
                pthread_mutex_unlock(&so->mtx);
                sync_unref(so);
                stdcall_ret(c, 2, res); return 1;
            }
            if (htype == HT_MUTEX && so) {
                int mytid = (int)(c->thread_id ? c->thread_id : 1000u);
                pthread_mutex_lock(&so->mtx);
                uint32_t res = 0;
                if (so->owner_tid != 0 && so->owner_tid != mytid) {
                    if (ms == 0) res = 0x102u;
                    else if (ms == 0xFFFFFFFFu) {
                        while (so->owner_tid != 0 && so->owner_tid != mytid)
                            pthread_cond_wait(&so->cond, &so->mtx);
                    } else {
                        struct timespec ts;
                        make_abs_timeout(&ts, ms);
                        while (so->owner_tid != 0 && so->owner_tid != mytid) {
                            if (pthread_cond_timedwait(&so->cond, &so->mtx, &ts) == ETIMEDOUT) {
                                res = 0x102u; break;
                            }
                        }
                    }
                }
                if (res == 0) {
                    so->owner_tid = mytid;
                    so->lock_depth++;
                }
                pthread_mutex_unlock(&so->mtx);
                sync_unref(so);
                stdcall_ret(c, 2, res); return 1;
            }
            if (so) sync_unref(so);
            if (to) thread_unref(to);
            stdcall_ret(c, 2, 0); return 1;
        }

        /* ---- Timers ---- */
        case IMP_KERNEL32_DLL_GetTickCount: {
            uint64_t ms = guest_elapsed_ns() / 1000000ull;
            stdcall_ret(c, 0, (uint32_t)(ms + 1000u)); return 1;
        }
        case IMP_KERNEL32_DLL_QueryPerformanceFrequency: {
            uint32_t p = arg_u32(c, 0);
            if (p) mem_w64(p, 1000000000ull);
            stdcall_ret(c, 1, 1); return 1;
        }
        case IMP_KERNEL32_DLL_QueryPerformanceCounter: {
            uint32_t p = arg_u32(c, 0);
            if (p) mem_w64(p, guest_elapsed_ns());
            stdcall_ret(c, 1, 1); return 1;
        }
        case IMP_KERNEL32_DLL_Sleep: {
            uint32_t ret_va = mem_r32(c->esp);
            uint32_t ms = arg_u32(c, 0);
            /* Original game bugfix (0x00453cd0): loading screen sleeps (250 - p1 - p2) ms
               every tick (50..250ms artificial stall). Cap to 4ms for snappy room loads. */
            if (ret_va == 0x00453f87u && ms > 4u) {
                ms = 4u;
            }
            if (ms == 0 || (ms <= 4 && c->thread_id == 1000u && gl_renderer_is_active())) {
                /* High-refresh-rate frame pacing is handled smoothly by OpenGL VSync / pacer */
                sched_yield();
            } else {
                usleep(ms * 1000u);
            }
            stdcall_ret(c, 1, 0); return 1;
        }

        /* ---- File I/O ---- */
        case IMP_KERNEL32_DLL_CreateFileA: {
            const char *fname = arg_cstr(c, 0);
            uint32_t access = arg_u32(c, 1);
            uint32_t disp = arg_u32(c, 4);
            int write_mode = (access & 0x40000000u) != 0;
            int read_mode  = (access & 0x80000000u) != 0;
            int oflags = (read_mode && write_mode) ? O_RDWR : (write_mode ? O_WRONLY : O_RDONLY);
            if (disp == 1) oflags |= O_CREAT | O_EXCL;        /* CREATE_NEW */
            else if (disp == 2) oflags |= O_CREAT | O_TRUNC;  /* CREATE_ALWAYS */
            else if (disp == 4) oflags |= O_CREAT;            /* OPEN_ALWAYS */
            else if (disp == 5) oflags |= O_TRUNC;            /* TRUNCATE_EXISTING */

            char lpath[1024];
            resolve_game_path(fname, lpath, sizeof(lpath), write_mode || (oflags & O_CREAT));
            int fd = open(lpath, oflags, 0644);
            if (g_debug_log) fprintf(stderr, "[FILE] CreateFileA('%s' -> '%s', acc=0x%x, disp=%u) -> fd=%d\n", fname ? fname : "", lpath, access, disp, fd);
            if (fd < 0) {
                win32_set_last_error(errno == ENOENT ? 2u : 5u);
                stdcall_ret(c, 7, 0xFFFFFFFFu); return 1;
            }
            uint32_t h = alloc_handle(HT_FILE);
            if (!h) {
                close(fd);
                win32_set_last_error(4u /* ERROR_TOO_MANY_OPEN_FILES */);
                stdcall_ret(c, 7, 0xFFFFFFFFu); return 1;
            }
            g_handles[h].fd = fd;
            win32_set_last_error(0);
            stdcall_ret(c, 7, h); return 1;
        }
        case IMP_KERNEL32_DLL_ReadFile: {
            uint32_t ret_va = mem_r32(c->esp);
            WinHandle *wh = get_handle(arg_u32(c, 0));
            uint32_t buf = arg_u32(c, 1);
            uint32_t n = arg_u32(c, 2);
            uint32_t out_n = arg_u32(c, 3);
            if (!wh || wh->type != HT_FILE || wh->fd < 0) { stdcall_ret(c, 5, 0); return 1; }
            ssize_t r = read(wh->fd, g_mem + buf, n);
            if (r < 0) { stdcall_ret(c, 5, 0); return 1; }
            if (out_n) mem_w32(out_n, (uint32_t)r);
            /* Original game bugfix (0x0044a200): savegame/encrypted loader loops forever
               on truncated files because it never checks bytesRead == 0 at EOF. */
            if (r == 0 && n > 0 && (ret_va == 0x0044a286u || ret_va == 0x0044a2aeu)) {
                stdcall_ret(c, 5, 0); return 1;
            }
            stdcall_ret(c, 5, 1); return 1;
        }
        case IMP_KERNEL32_DLL_WriteFile: {
            WinHandle *wh = get_handle(arg_u32(c, 0));
            uint32_t buf = arg_u32(c, 1);
            uint32_t n = arg_u32(c, 2);
            uint32_t out_n = arg_u32(c, 3);
            if (!wh || wh->type != HT_FILE || wh->fd < 0) { stdcall_ret(c, 5, 0); return 1; }
            ssize_t w = write(wh->fd, g_mem + buf, n);
            if (w < 0) { stdcall_ret(c, 5, 0); return 1; }
            if (out_n) mem_w32(out_n, (uint32_t)w);
            stdcall_ret(c, 5, 1); return 1;
        }
        case IMP_KERNEL32_DLL_SetFilePointer: {
            WinHandle *wh = get_handle(arg_u32(c, 0));
            int32_t dist = arg_i32(c, 1);
            uint32_t high_ptr = arg_u32(c, 2);
            uint32_t method = arg_u32(c, 3);
            if (!wh || wh->type != HT_FILE || wh->fd < 0) { stdcall_ret(c, 4, 0xFFFFFFFFu); return 1; }
            off_t off = dist;
            if (high_ptr) off |= ((off_t)(int32_t)mem_r32(high_ptr)) << 32;
            int whence = (method == 0) ? SEEK_SET : (method == 1) ? SEEK_CUR : SEEK_END;
            off_t res = lseek(wh->fd, off, whence);
            if (res < 0) { stdcall_ret(c, 4, 0xFFFFFFFFu); return 1; }
            if (high_ptr) mem_w32(high_ptr, (uint32_t)((uint64_t)res >> 32));
            stdcall_ret(c, 4, (uint32_t)res); return 1;
        }
        case IMP_KERNEL32_DLL_SetEndOfFile: {
            WinHandle *wh = get_handle(arg_u32(c, 0));
            if (wh && wh->type == HT_FILE && wh->fd >= 0) {
                off_t cur = lseek(wh->fd, 0, SEEK_CUR);
                if (cur >= 0 && ftruncate(wh->fd, cur) == 0) {
                    stdcall_ret(c, 1, 1); return 1;
                }
            }
            stdcall_ret(c, 1, 0); return 1;
        }
        case IMP_KERNEL32_DLL_GetFileSize: {
            uint32_t ret_va = mem_r32(c->esp);
            WinHandle *wh = get_handle(arg_u32(c, 0));
            uint32_t high_ptr = arg_u32(c, 1);
            if (!wh || wh->type != HT_FILE || wh->fd < 0) {
                stdcall_ret(c, 2, (ret_va == 0x0044a236u) ? 0u : 0xFFFFFFFFu); return 1;
            }
            struct stat st;
            if (fstat(wh->fd, &st) != 0) {
                stdcall_ret(c, 2, (ret_va == 0x0044a236u) ? 0u : 0xFFFFFFFFu); return 1;
            }
            /* Original game bugfix (0x0044a200): reject absurd sizes before operator_new(size+0x20) */
            if (ret_va == 0x0044a236u && (st.st_size < 0 || st.st_size > 0x08000000)) {
                stdcall_ret(c, 2, 0u); return 1;
            }
            if (high_ptr) mem_w32(high_ptr, (uint32_t)((uint64_t)st.st_size >> 32));
            stdcall_ret(c, 2, (uint32_t)st.st_size); return 1;
        }
        case IMP_KERNEL32_DLL_GetFileType: {
            uint32_t h = arg_u32(c, 0);
            if (h <= 3) { stdcall_ret(c, 1, 2u /* FILE_TYPE_CHAR */); return 1; }
            WinHandle *wh = get_handle(h);
            stdcall_ret(c, 1, (wh && wh->type == HT_FILE) ? 1u /* FILE_TYPE_DISK */ : 0u);
            return 1;
        }
        case IMP_KERNEL32_DLL_FlushFileBuffers: {
            WinHandle *wh = get_handle(arg_u32(c, 0));
            if (wh && wh->type == HT_FILE && wh->fd >= 3) fsync(wh->fd);
            stdcall_ret(c, 1, 1); return 1;
        }
        case IMP_KERNEL32_DLL_CloseHandle: {
            uint32_t h = arg_u32(c, 0);
            pthread_mutex_lock(&g_hdl_mtx);
            WinHandle *wh = get_handle(h);
            int ok = 0;
            int fd_to_close = -1;
            SyncObj *so_to_unref = NULL;
            ThreadObj *to_detach = NULL;
            if (wh && h >= 0x100) {
                ok = 1;
                if (wh->type == HT_FILE && wh->fd >= 0) fd_to_close = wh->fd;
                so_to_unref = wh->sync;
                to_detach = wh->thr;
                wh->type = HT_NONE;
                wh->sync = NULL;
                wh->thr = NULL;
                wh->fd = -1;
            }
            pthread_mutex_unlock(&g_hdl_mtx);
            if (fd_to_close >= 0) close(fd_to_close);
            if (so_to_unref) sync_unref(so_to_unref);
            if (to_detach) {
                pthread_detach(to_detach->pt);
                thread_unref(to_detach);
            }
            stdcall_ret(c, 1, ok ? 1u : 0u); return 1;
        }
        case IMP_KERNEL32_DLL_GetFileAttributesA: {
            const char *fn = arg_cstr(c, 0);
            char lpath[1024];
            resolve_game_path(fn, lpath, sizeof(lpath), 0);
            struct stat st;
            if (stat(lpath, &st) != 0) {
                win32_set_last_error(2);
                stdcall_ret(c, 1, 0xFFFFFFFFu); return 1;
            }
            stdcall_ret(c, 1, S_ISDIR(st.st_mode) ? 0x10u : 0x80u); return 1;
        }
        case IMP_KERNEL32_DLL_CreateDirectoryA: {
            const char *fn = arg_cstr(c, 0);
            char lpath[1024];
            resolve_game_path(fn, lpath, sizeof(lpath), 1);
            int r = mkdir(lpath, 0755);
            stdcall_ret(c, 2, (r == 0 || errno == EEXIST) ? 1 : 0); return 1;
        }
        case IMP_KERNEL32_DLL_GetCurrentDirectoryA: {
            uint32_t bsz = arg_u32(c, 0);
            uint32_t buf = arg_u32(c, 1);
            const char *cwd = "C:\\mha2";
            uint32_t len = (uint32_t)snprintf((char *)(g_mem + buf), bsz, "%s", cwd);
            stdcall_ret(c, 2, len); return 1;
        }
        case IMP_KERNEL32_DLL_GetFullPathNameA: {
            const char *fn = arg_cstr(c, 0);
            uint32_t bsz = arg_u32(c, 1);
            uint32_t buf = arg_u32(c, 2);
            uint32_t fpart = arg_u32(c, 3);
            uint32_t len = (uint32_t)snprintf((char *)(g_mem + buf), bsz, "%s", fn ? fn : "");
            if (fpart) {
                const char *s = (const char *)(g_mem + buf);
                const char *sl = strrchr(s, '\\');
                const char *sl2 = strrchr(s, '/');
                if (sl2 > sl) sl = sl2;
                mem_w32(fpart, sl ? (buf + (uint32_t)(sl - s) + 1u) : buf);
            }
            stdcall_ret(c, 4, len); return 1;
        }
        case IMP_KERNEL32_DLL_GetDriveTypeA:
            stdcall_ret(c, 1, 3u /* DRIVE_FIXED */); return 1;
        case IMP_KERNEL32_DLL_CompareFileTime: {
            uint64_t a = mem_r64(arg_u32(c, 0));
            uint64_t b = mem_r64(arg_u32(c, 1));
            stdcall_ret(c, 2, (a < b) ? 0xFFFFFFFFu : (a > b ? 1u : 0u)); return 1;
        }
        case IMP_KERNEL32_DLL_FindFirstFileA: {
            const char *pat = arg_cstr(c, 0);
            uint32_t dst = arg_u32(c, 1);
            char clean[1024];
            size_t j = 0;
            for (const char *p = pat ? pat : ""; *p && j + 1 < sizeof(clean); p++)
                clean[j++] = (*p == '\\') ? '/' : *p;
            clean[j] = '\0';
            char *sl = strrchr(clean, '/');
            char dir_win[1024], mask[256];
            if (sl) {
                *sl = '\0';
                snprintf(dir_win, sizeof(dir_win), "%s", clean);
                snprintf(mask, sizeof(mask), "%s", sl + 1);
            } else {
                dir_win[0] = '\0';
                snprintf(mask, sizeof(mask), "%.*s", (int)(sizeof(mask) - 1), clean);
            }
            char ldir[1024];
            resolve_game_path(dir_win, ldir, sizeof(ldir), 0);
            DIR *d = opendir(ldir);
            if (!d) {
                win32_set_last_error(2);
                stdcall_ret(c, 2, 0xFFFFFFFFu); return 1;
            }
            FindState *fs = (FindState *)calloc(1, sizeof(FindState));
            snprintf(fs->dir_path, sizeof(fs->dir_path), "%s", ldir);
            snprintf(fs->pattern, sizeof(fs->pattern), "%s", mask);
            fs->dir = d;
            if (!find_next_match(fs, dst)) {
                closedir(d); free(fs);
                win32_set_last_error(2);
                stdcall_ret(c, 2, 0xFFFFFFFFu); return 1;
            }
            /* Original game bugfix (0x00447820): startup data\g directory probe jumps past
               FindClose when FILE_ATTRIBUTE_DIRECTORY (0x10) is set, leaking DIR* forever. */
            uint32_t ret_va = mem_r32(c->esp);
            if (ret_va >= 0x00447820u && ret_va < 0x00448600u && dst && (mem_r32(dst) & 0x10u)) {
                closedir(d); free(fs);
                stdcall_ret(c, 2, 0x000000FEu); return 1;
            }
            uint32_t h = alloc_handle(HT_FIND);
            if (!h) {
                closedir(d); free(fs);
                win32_set_last_error(4u);
                stdcall_ret(c, 2, 0xFFFFFFFFu); return 1;
            }
            g_handles[h].find = fs;
            stdcall_ret(c, 2, h); return 1;
        }
        case IMP_KERNEL32_DLL_FindNextFileA: {
            WinHandle *wh = get_handle(arg_u32(c, 0));
            uint32_t dst = arg_u32(c, 1);
            if (!wh || wh->type != HT_FIND || !wh->find) { stdcall_ret(c, 2, 0); return 1; }
            int ok = find_next_match(wh->find, dst);
            if (!ok) win32_set_last_error(18); /* ERROR_NO_MORE_FILES */
            stdcall_ret(c, 2, ok ? 1u : 0u); return 1;
        }
        case IMP_KERNEL32_DLL_FindClose: {
            uint32_t h = arg_u32(c, 0);
            FindState *fs = NULL;
            pthread_mutex_lock(&g_hdl_mtx);
            WinHandle *wh = get_handle(h);
            if (wh && wh->type == HT_FIND && wh->find) {
                fs = wh->find;
                wh->find = NULL;
                wh->type = HT_NONE;
            }
            pthread_mutex_unlock(&g_hdl_mtx);
            if (fs) {
                if (fs->dir) closedir(fs->dir);
                free(fs);
            }
            stdcall_ret(c, 1, 1); return 1;
        }

        /* ---- Codepage / Locale / String Conversion ---- */
        case IMP_KERNEL32_DLL_GetACP:
        case IMP_KERNEL32_DLL_GetOEMCP:
            stdcall_ret(c, 0, 1252u); return 1;
        case IMP_KERNEL32_DLL_GetCPInfo: {
            uint32_t p = arg_u32(c, 1);
            if (p) {
                memset(g_mem + p, 0, 20);
                mem_w32(p + 0, 1);
                mem_w8(p + 4, '?');
            }
            stdcall_ret(c, 2, 1); return 1;
        }
        case IMP_KERNEL32_DLL_MultiByteToWideChar: {
            uint32_t src_va = arg_u32(c, 2);
            int32_t  src_len = arg_i32(c, 3);
            uint32_t dst_va = arg_u32(c, 4);
            int32_t  dst_len = arg_i32(c, 5);
            const uint8_t *s = g_mem + src_va;
            int32_t n = (src_len < 0) ? (int32_t)strlen((const char *)s) + 1 : src_len;
            if (dst_len == 0) { stdcall_ret(c, 6, (uint32_t)n); return 1; }
            int32_t m = n < dst_len ? n : dst_len;
            for (int32_t i = 0; i < m; i++) mem_w16(dst_va + (uint32_t)i * 2u, s[i]);
            stdcall_ret(c, 6, (uint32_t)m); return 1;
        }
        case IMP_KERNEL32_DLL_WideCharToMultiByte: {
            uint32_t src_va = arg_u32(c, 2);
            int32_t  src_len = arg_i32(c, 3);
            uint32_t dst_va = arg_u32(c, 4);
            int32_t  dst_len = arg_i32(c, 5);
            int32_t n = src_len;
            if (n < 0) {
                n = 0;
                while (mem_r16(src_va + (uint32_t)n * 2u) != 0) n++;
                n++;
            }
            if (dst_len == 0) { stdcall_ret(c, 8, (uint32_t)n); return 1; }
            int32_t m = n < dst_len ? n : dst_len;
            for (int32_t i = 0; i < m; i++) {
                uint16_t ch = mem_r16(src_va + (uint32_t)i * 2u);
                mem_w8(dst_va + (uint32_t)i, ch < 256 ? (uint8_t)ch : '?');
            }
            stdcall_ret(c, 8, (uint32_t)m); return 1;
        }
        case IMP_KERNEL32_DLL_GetStringTypeA: {
            uint32_t src_va = arg_u32(c, 2);
            int32_t  n = arg_i32(c, 3);
            uint32_t dst_va = arg_u32(c, 4);
            if (n < 0) n = (int32_t)strlen((const char *)(g_mem + src_va)) + 1;
            for (int32_t i = 0; i < n; i++) {
                unsigned char ch = g_mem[src_va + i];
                uint16_t t = 0;
                if (isupper(ch)) t |= 0x0001;
                if (islower(ch)) t |= 0x0002;
                if (isdigit(ch)) t |= 0x0004;
                if (isspace(ch)) t |= 0x0008;
                if (ispunct(ch)) t |= 0x0010;
                if (iscntrl(ch)) t |= 0x0020;
                if (ch == ' ' || ch == '\t') t |= 0x0040;
                if (isxdigit(ch)) t |= 0x0080;
                if (isalpha(ch)) t |= 0x0100;
                mem_w16(dst_va + (uint32_t)i * 2u, t);
            }
            stdcall_ret(c, 5, 1); return 1;
        }
        case IMP_KERNEL32_DLL_GetStringTypeW: {
            uint32_t src_va = arg_u32(c, 1);
            int32_t  n = arg_i32(c, 2);
            uint32_t dst_va = arg_u32(c, 3);
            for (int32_t i = 0; i < n; i++) {
                uint16_t ch = mem_r16(src_va + (uint32_t)i * 2u);
                uint16_t t = 0;
                if (ch < 128) {
                    if (isupper(ch)) t |= 0x0001;
                    if (islower(ch)) t |= 0x0002;
                    if (isdigit(ch)) t |= 0x0004;
                    if (isspace(ch)) t |= 0x0008;
                    if (ispunct(ch)) t |= 0x0010;
                    if (iscntrl(ch)) t |= 0x0020;
                    if (ch == ' ' || ch == '\t') t |= 0x0040;
                    if (isxdigit(ch)) t |= 0x0080;
                    if (isalpha(ch)) t |= 0x0100;
                }
                mem_w16(dst_va + (uint32_t)i * 2u, t);
            }
            stdcall_ret(c, 4, 1); return 1;
        }
        case IMP_KERNEL32_DLL_LCMapStringA: {
            uint32_t flags = arg_u32(c, 1);
            uint32_t src = arg_u32(c, 2);
            int32_t  sn = arg_i32(c, 3);
            uint32_t dst = arg_u32(c, 4);
            int32_t  dn = arg_i32(c, 5);
            if (sn < 0) sn = (int32_t)strlen((const char *)(g_mem + src)) + 1;
            if (dn == 0) { stdcall_ret(c, 6, (uint32_t)sn); return 1; }
            int32_t m = sn < dn ? sn : dn;
            for (int32_t i = 0; i < m; i++) {
                unsigned char ch = g_mem[src + i];
                if (flags & 0x100u) ch = (unsigned char)tolower(ch);
                else if (flags & 0x200u) ch = (unsigned char)toupper(ch);
                g_mem[dst + i] = ch;
            }
            stdcall_ret(c, 6, (uint32_t)m); return 1;
        }
        case IMP_KERNEL32_DLL_LCMapStringW: {
            uint32_t flags = arg_u32(c, 1);
            uint32_t src = arg_u32(c, 2);
            int32_t  sn = arg_i32(c, 3);
            uint32_t dst = arg_u32(c, 4);
            int32_t  dn = arg_i32(c, 5);
            if (sn < 0) { sn = 0; while (mem_r16(src + (uint32_t)sn * 2u)) sn++; sn++; }
            if (dn == 0) { stdcall_ret(c, 6, (uint32_t)sn); return 1; }
            int32_t m = sn < dn ? sn : dn;
            for (int32_t i = 0; i < m; i++) {
                uint16_t ch = mem_r16(src + (uint32_t)i * 2u);
                if (ch < 128) {
                    if (flags & 0x100u) ch = (uint16_t)tolower(ch);
                    else if (flags & 0x200u) ch = (uint16_t)toupper(ch);
                }
                mem_w16(dst + (uint32_t)i * 2u, ch);
            }
            stdcall_ret(c, 6, (uint32_t)m); return 1;
        }
        case IMP_KERNEL32_DLL_CompareStringA: {
            uint32_t flags = arg_u32(c, 1);
            const char *s1 = arg_cstr(c, 2);
            int32_t n1 = arg_i32(c, 3);
            const char *s2 = arg_cstr(c, 4);
            int32_t n2 = arg_i32(c, 5);
            if (n1 < 0) n1 = (int32_t)strlen(s1);
            if (n2 < 0) n2 = (int32_t)strlen(s2);
            int m = n1 < n2 ? n1 : n2;
            int cmp = (flags & 1u) ? strncasecmp(s1, s2, (size_t)m) : strncmp(s1, s2, (size_t)m);
            if (cmp == 0) cmp = n1 - n2;
            stdcall_ret(c, 6, cmp < 0 ? 1u : (cmp > 0 ? 3u : 2u)); return 1;
        }
        case IMP_KERNEL32_DLL_CompareStringW: {
            uint32_t flags = arg_u32(c, 1);
            uint32_t s1 = arg_u32(c, 2);
            int32_t n1 = arg_i32(c, 3);
            uint32_t s2 = arg_u32(c, 4);
            int32_t n2 = arg_i32(c, 5);
            if (n1 < 0) { n1 = 0; while (mem_r16(s1 + (uint32_t)n1 * 2u)) n1++; }
            if (n2 < 0) { n2 = 0; while (mem_r16(s2 + (uint32_t)n2 * 2u)) n2++; }
            int m = n1 < n2 ? n1 : n2;
            int cmp = 0;
            for (int i = 0; i < m; i++) {
                uint16_t a = mem_r16(s1 + (uint32_t)i * 2u);
                uint16_t b = mem_r16(s2 + (uint32_t)i * 2u);
                if (flags & 1u) { if (a < 128) a = (uint16_t)tolower(a); if (b < 128) b = (uint16_t)tolower(b); }
                if (a != b) { cmp = (int)a - (int)b; break; }
            }
            if (cmp == 0) cmp = n1 - n2;
            stdcall_ret(c, 6, cmp < 0 ? 1u : (cmp > 0 ? 3u : 2u)); return 1;
        }

        /* ---- SHELL32 & OLE32 ---- */
        case IMP_SHELL32_DLL_ShellExecuteA: {
            const char *file = arg_cstr(c, 2);
            fprintf(stderr, "[SHELL32] ShellExecuteA(%s)\n", file ? file : "");
            stdcall_ret(c, 6, 33u); return 1;
        }
        case IMP_OLE32_DLL_CoInitialize:
            stdcall_ret(c, 1, 0); return 1;
        case IMP_OLE32_DLL_CoUninitialize:
            stdcall_ret(c, 0, 0); return 1;
        case IMP_OLE32_DLL_CoCreateInstance: {
            uint32_t clsid_va = arg_u32(c, 0);
            uint32_t ppv_va   = arg_u32(c, 4);
            uint32_t d1 = mem_r32(clsid_va);
            if (g_debug_log) fprintf(stderr, "[OLE32] CoCreateInstance(clsid.d1=0x%08x)\n", d1);
            if (d1 == 0xe436ebb3u) {
                /* CLSID_FilterGraph -> DirectShow AVI player */
                uint32_t obj = com_dshow_create_graph();
                if (ppv_va) mem_w32(ppv_va, obj);
                stdcall_ret(c, 5, 0); return 1;
            }
            /* For CLSID_DirectSound8 (0x3901cc3f) and CLSID_OverlayMixer (0xcd8743a1),
               return E_NOINTERFACE so the game cleanly uses DirectSound 7 / standard render! */
            if (ppv_va) mem_w32(ppv_va, 0);
            stdcall_ret(c, 5, 0x80040154u /* REGDB_E_CLASSNOTREG */); return 1;
        }
    }
    return 0;
}
