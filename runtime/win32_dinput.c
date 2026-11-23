#include "runtime.h"
#include "imports.h"

typedef struct DiEvent {
    uint32_t ofs;
    uint32_t data;
    uint32_t ts;
    uint32_t seq;
} DiEvent;

#define DI_EV_CAP 512
static DiEvent g_di_q[DI_EV_CAP];
static int g_di_head = 0, g_di_tail = 0;
static uint32_t g_di_seq = 1;
static int g_rel_x = 0, g_rel_y = 0, g_rel_z = 0;
static uint8_t g_btn[4] = {0, 0, 0, 0};

static void di_push(uint32_t ofs, uint32_t data) {
    int next = (g_di_tail + 1) % DI_EV_CAP;
    if (next == g_di_head) g_di_head = (g_di_head + 1) % DI_EV_CAP;
    g_di_q[g_di_tail].ofs = ofs;
    g_di_q[g_di_tail].data = data;
    g_di_q[g_di_tail].ts = SDL_GetTicks();
    g_di_q[g_di_tail].seq = g_di_seq++;
    g_di_tail = next;
}

void dinput_push_mouse_move(int dx, int dy, int abs_x, int abs_y) {
    (void)abs_x; (void)abs_y;
    g_rel_x += dx;
    g_rel_y += dy;
    if (dx != 0) di_push(0u /* DIMOFS_X */, (uint32_t)(int32_t)dx);
    if (dy != 0) di_push(4u /* DIMOFS_Y */, (uint32_t)(int32_t)dy);
}

void dinput_push_mouse_button(int btn, int down) {
    if (btn < 0 || btn >= 4) return;
    g_btn[btn] = down ? 0x80u : 0x00u;
    di_push(12u + (uint32_t)btn /* DIMOFS_BUTTON0..3 */, down ? 0x80u : 0x00u);
}

void dinput_push_mouse_wheel(int dz) {
    g_rel_z += dz;
    if (dz != 0) di_push(8u /* DIMOFS_Z */, (uint32_t)(int32_t)dz);
}

int win32_dinput_dispatch(CpuCtx *c, uint32_t import_id) {
    if (import_id == IMP_DINPUT_DLL_DirectInputCreateA) {
        uint32_t ppdi = arg_u32(c, 2);
        uint32_t obj = com_create_obj(COM_IFACE_DINPUT, 12, 1);
        if (ppdi) mem_w32(ppdi, obj);
        if (g_debug_log) fprintf(stderr, "[DINPUT] DirectInputCreateA -> obj=0x%08x\n", obj);
        stdcall_ret(c, 4, 0);
        return 1;
    }
    return 0;
}

void com_dinput_dispatch(CpuCtx *c, uint32_t iface_id, uint32_t slot) {
    uint32_t this_va = arg_u32(c, 0);

    if (iface_id == COM_IFACE_DINPUT) {
        switch (slot) {
            case 0: { uint32_t ppv = arg_u32(c, 2); if (ppv) mem_w32(ppv, this_va); stdcall_ret(c, 3, 0); return; }
            case 1: stdcall_ret(c, 1, 2); return;
            case 2: stdcall_ret(c, 1, 1); return;
            case 3: { /* CreateDevice(this, rguid, lplpDirectInputDevice, pUnkOuter) */
                uint32_t out = arg_u32(c, 2);
                uint32_t dev = com_create_obj(COM_IFACE_DINPUT_DEV, 20, 1);
                if (out) mem_w32(out, dev);
                if (g_debug_log) fprintf(stderr, "[DINPUT] CreateDevice -> dev=0x%08x\n", dev);
                stdcall_ret(c, 4, 0); return;
            }
            case 4: { /* EnumDevices(this, dwDevType, lpCallback, pvRef, dwFlags) */
                uint32_t cb_va = arg_u32(c, 2);
                uint32_t ref   = arg_u32(c, 3);
                if (cb_va) {
                    /* DIDEVICEINSTANCEA is 580 bytes */
                    uint32_t inst = guest_alloc(580);
                    memset(g_mem + inst, 0, 580);
                    mem_w32(inst + 0, 580);
                    /* GUID_SysMouse */
                    static const uint8_t guid_mouse[16] = {
                        0xE0,0x2E,0x1D,0x6F, 0xD5,0xD5, 0xCF,0x11, 0xBF,0xC7, 0x44,0x45,0x53,0x54,0x00,0x00
                    };
                    memcpy(g_mem + inst + 4, guid_mouse, 16);
                    memcpy(g_mem + inst + 20, guid_mouse, 16);
                    mem_w32(inst + 36, 0x00000102u /* DIDEVTYPE_MOUSE | DIDEVTYPEMOUSE_TRADITIONAL */);
                    snprintf((char *)(g_mem + inst + 40), 260, "SDL2 Mouse");
                    snprintf((char *)(g_mem + inst + 300), 260, "SDL2 Mouse");
                    uint32_t a[2] = { inst, ref };
                    guest_call_stdcall(c, cb_va, 2, a);
                    guest_free(inst);
                }
                stdcall_ret(c, 5, 0); return;
            }
            default:
                fprintf(stderr, "[DINPUT] Unhandled IDirectInput slot %u\n", slot);
                stdcall_ret(c, 1, 0); return;
        }
    }

    if (iface_id == COM_IFACE_DINPUT_DEV) {
        switch (slot) {
            case 0: { uint32_t ppv = arg_u32(c, 2); if (ppv) mem_w32(ppv, this_va); stdcall_ret(c, 3, 0); return; }
            case 1: stdcall_ret(c, 1, 2); return;
            case 2: stdcall_ret(c, 1, 0); return;
            case 3: { /* GetCapabilities(this, lpDIDevCaps) */
                uint32_t caps = arg_u32(c, 1);
                if (caps) {
                    uint32_t sz = mem_r32(caps);
                    if (sz > 4 && sz <= 64) memset(g_mem + caps + 4, 0, sz - 4);
                    mem_w32(caps + 4, 1); /* DIDC_ATTACHED */
                    mem_w32(caps + 8, 2); /* DIDEVTYPE_MOUSE */
                    mem_w32(caps + 12, 3);/* dwAxes */
                    mem_w32(caps + 16, 3);/* dwButtons */
                }
                stdcall_ret(c, 2, 0); return;
            }
            case 5: /* GetProperty(this, rguidProp, pdiph) */
                stdcall_ret(c, 3, 0); return;
            case 6: /* SetProperty(this, rguidProp, pdiph) */
                stdcall_ret(c, 3, 0); return;
            case 7: /* Acquire(this) */
                stdcall_ret(c, 1, 0); return;
            case 8: /* Unacquire(this) */
                stdcall_ret(c, 1, 0); return;
            case 9: { /* GetDeviceState(this, cbData, lpvData) */
                win32_pump_sdl_events();
                uint32_t cb = arg_u32(c, 1);
                uint32_t dst = arg_u32(c, 2);
                if (dst && cb >= 16) {
                    mem_w32(dst + 0, (uint32_t)(int32_t)g_rel_x);
                    mem_w32(dst + 4, (uint32_t)(int32_t)g_rel_y);
                    mem_w32(dst + 8, (uint32_t)(int32_t)g_rel_z);
                    g_rel_x = g_rel_y = g_rel_z = 0;
                    for (int i = 0; i < 4; i++) mem_w8(dst + 12u + (uint32_t)i, g_btn[i]);
                }
                stdcall_ret(c, 3, 0); return;
            }
            case 10: { /* GetDeviceData(this, cbObjectData, rgdod, pdwInOut, dwFlags) */
                win32_pump_sdl_events();
                uint32_t cb = arg_u32(c, 1);
                uint32_t rgdod = arg_u32(c, 2);
                uint32_t pdw = arg_u32(c, 3);
                uint32_t flags = arg_u32(c, 4);
                uint32_t max_n = pdw ? mem_r32(pdw) : 0;
                uint32_t count = 0;
                int peek = (flags & 1u /* DIGDD_PEEK */) != 0;
                int idx = g_di_head;
                while (idx != g_di_tail && count < max_n) {
                    if (rgdod && cb >= 16) {
                        uint32_t dst = rgdod + count * cb;
                        mem_w32(dst + 0, g_di_q[idx].ofs);
                        mem_w32(dst + 4, g_di_q[idx].data);
                        mem_w32(dst + 8, g_di_q[idx].ts);
                        mem_w32(dst + 12, g_di_q[idx].seq);
                    }
                    idx = (idx + 1) % DI_EV_CAP;
                    count++;
                }
                if (!peek) g_di_head = idx;
                if (pdw) mem_w32(pdw, count);
                stdcall_ret(c, 5, 0); return;
            }
            case 11: /* SetDataFormat(this, lpdf) */
                stdcall_ret(c, 2, 0); return;
            case 12: /* SetEventNotification(this, hEvent) */
                stdcall_ret(c, 2, 0); return;
            case 13: /* SetCooperativeLevel(this, hwnd, dwFlags) */
                stdcall_ret(c, 3, 0); return;
            case 17: /* Poll(this) */
                win32_pump_sdl_events();
                stdcall_ret(c, 1, 0); return;
            default:
                fprintf(stderr, "[DINPUT] Unhandled IDirectInputDevice slot %u\n", slot);
                stdcall_ret(c, 1, 0); return;
        }
    }
}
