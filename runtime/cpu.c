#include "runtime.h"
#include "imports.h"
#include <pthread.h>

char g_game_dir[1024] = "game_files";
volatile int g_quit_requested = 0;
int g_debug_log = 0;

#define MAX_THREAD_SLOTS 36u
static uint32_t g_com_vtables[32];
static uint64_t g_thread_slot_mask = 0;
static pthread_mutex_t g_thread_slot_mtx = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_lock(&g_thread_slot_mtx);
    uint32_t slot = MAX_THREAD_SLOTS;
    for (uint32_t i = 0; i < MAX_THREAD_SLOTS; i++) {
        if ((g_thread_slot_mask & (1ULL << i)) == 0) {
            g_thread_slot_mask |= (1ULL << i);
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_thread_slot_mtx);
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
    pthread_mutex_lock(&g_thread_slot_mtx);
    g_thread_slot_mask &= ~(1ULL << slot);
    pthread_mutex_unlock(&g_thread_slot_mtx);
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
