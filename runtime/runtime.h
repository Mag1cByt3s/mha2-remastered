#ifndef MHA2_RUNTIME_H
#define MHA2_RUNTIME_H

#include "cpu.h"
#include "gl_renderer.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

/* COM Interface IDs for synthetic vtables (0xFE000000 | (iface << 12) | slot) */
enum ComIfaceId {
    COM_IFACE_DDRAW         = 1,
    COM_IFACE_DDRAW_SURFACE = 2,
    COM_IFACE_DDRAW_CLIPPER = 3,
    COM_IFACE_DINPUT        = 4,
    COM_IFACE_DINPUT_DEV    = 5,
    COM_IFACE_DSOUND        = 6,
    COM_IFACE_DSOUND_BUF    = 7,
    COM_IFACE_DSHOW_GRAPH   = 8,
    COM_IFACE_DSHOW_CTRL    = 9,
    COM_IFACE_DSHOW_EVENT   = 10,
    COM_IFACE_DSHOW_VIDWIN  = 11,
    COM_IFACE_DSHOW_BASVID  = 12,
};

/* Global game directory (e.g. "game_files") */
extern char g_game_dir[1024];
extern volatile int g_quit_requested;
extern int g_debug_log;

/* PE loading & code table (cpu.c); guest memory & heap allocator live in cpu.h / guest_mem.c */
int  runtime_load_pe(const char *exe_path);
void init_code_table(void);

uint32_t com_get_vtable(uint32_t iface_id, uint32_t num_slots);
uint32_t com_create_obj(uint32_t iface_id, uint32_t num_slots, uint32_t handle_val);
uint32_t com_obj_handle(uint32_t obj_va);

uint32_t guest_alloc_thread_slot(uint32_t *out_teb, uint32_t *out_stack_top);
void     guest_free_thread_slot(uint32_t tid);
void     runtime_unpin_current_thread(void);

/* Path resolution (win32_kernel.c) */
void resolve_game_path(const char *win_path, char *out_linux, size_t out_sz, int for_write);
void win32_kernel_init(void);
void win32_set_last_error(uint32_t err);
uint32_t win32_get_last_error(void);
int  win32_kernel_dispatch(CpuCtx *c, uint32_t import_id);

/* User32 / GDI32 / SDL2 Window (win32_user_gdi.c) */
extern SDL_Window   *g_sdl_window;
extern SDL_Renderer *g_sdl_renderer;
extern int           g_win_w;
extern int           g_win_h;
extern int           g_mouse_x;
extern int           g_mouse_y;
extern int           g_os_cursor_hidden;

void win32_user_gdi_init(void);
void win32_pump_sdl_events(void);
int  win32_user_gdi_dispatch(CpuCtx *c, uint32_t import_id);
uint32_t gdi_create_surface_dc(uint32_t surface_handle);
void     gdi_release_surface_dc(uint32_t hdc);

/* DirectDraw (win32_ddraw.c) */
typedef struct DDrawSurface {
    int in_use;
    int refcount;
    uint32_t obj_va;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    uint32_t caps;
    uint32_t pixels_va;
    uint32_t attached_handle; /* backbuffer handle for primary */
    int has_src_colorkey;
    uint32_t src_ck_low;
    uint32_t src_ck_high;
    int is_primary;
    int is_backbuffer;

    /* v3/v5 Remaster metadata */
    int hd_asset_id;          /* -2=unchecked, -1=none, >=0=HD background asset index */
    int known_cell_w;         /* locked uniform frame cell width for multi-frame IT3 strips */
    int known_cell_h;         /* locked uniform frame cell height for multi-frame IT3 strips */
    uint32_t cell_probe_room_gen;
    uint32_t sprite_cache_room_gen;
    int sprite_cache_count;
    struct {
        int sx0, sy0, sw, sh;
        int sprite_id;        /* -1=none, >=0=matched HD sprite index */
    } sprite_cache[96];
    GlTextDraw text_draws[MAX_SURFACE_TEXT_DRAWS];
    int num_text_draws;

    /* Cursor sprite auto-detection */
    int is_cursor;
    int cursor_track_count;
    int last_origin_x;
    int last_origin_y;
    int last_sx0;
    int last_sy0;
    int last_mouse_x;
    int last_mouse_y;
    int cursor_hot_x;
    int cursor_hot_y;
    int cand_hot_x;
    int cand_hot_y;
    int cursor_full_w;
    int cursor_full_h;
} DDrawSurface;

DDrawSurface *ddraw_get_surface(uint32_t handle);
void ddraw_surface_add_text(DDrawSurface *s, const GlTextDraw *td);
void ddraw_present_surface(DDrawSurface *surf);
uint64_t ddraw_last_frame_hash(uint32_t *out_w, uint32_t *out_h);
void ddraw_present_rgb24(const uint8_t *rgb, int w, int h, int stride);
void ddraw_install_rle_hooks(void);
void game_patches_install(void);

typedef struct GameHotspot {
    int x, y;          /* marker anchor point in 800x600 screen coords (inside clickable region) */
    int x0, y0, x1, y1;/* clipped bounding box in 800x600 screen coords */
    int kind;          /* 0 = Object/Item, 1 = Exit, 2 = Character/NPC */
    char label[128];   /* UTF-8 hover name from engine string table (may be empty) */
} GameHotspot;
int  game_collect_room_hotspots(GameHotspot *out, int max_out);
int  game_is_main_menu_active(void);
int  win32_ddraw_dispatch(CpuCtx *c, uint32_t import_id);
void com_ddraw_dispatch(CpuCtx *c, uint32_t iface_id, uint32_t slot);

/* DirectInput (win32_dinput.c) */
void dinput_push_mouse_move(int dx, int dy, int abs_x, int abs_y);
void dinput_push_mouse_button(int btn, int down);
void dinput_push_mouse_wheel(int dz);
int  win32_dinput_dispatch(CpuCtx *c, uint32_t import_id);
void com_dinput_dispatch(CpuCtx *c, uint32_t iface_id, uint32_t slot);

/* DirectSound (win32_dsound.c) */
void win32_dsound_init(void);
void win32_dsound_shutdown(void);
int  win32_dsound_dispatch(CpuCtx *c, uint32_t import_id);
void com_dsound_dispatch(CpuCtx *c, uint32_t iface_id, uint32_t slot);

/* AVI / DirectShow / VFW (win32_avi.c) */
int  win32_avi_is_playing(void);
int  win32_avi_dispatch(CpuCtx *c, uint32_t import_id);
uint32_t com_dshow_create_graph(void);
void com_dshow_dispatch(CpuCtx *c, uint32_t iface_id, uint32_t slot);

/* Helper to read stdcall args on guest stack:
 * On entry to win32/COM function: [esp+0] = retaddr, [esp+4] = arg0, [esp+8] = arg1, ... */
static inline uint32_t arg_u32(CpuCtx *c, int idx) {
    return mem_r32(c->esp + 4u + (uint32_t)idx * 4u);
}
static inline int32_t arg_i32(CpuCtx *c, int idx) {
    return (int32_t)arg_u32(c, idx);
}
static inline const char *arg_cstr(CpuCtx *c, int idx) {
    uint32_t a = arg_u32(c, idx);
    return a ? (const char *)(g_mem + a) : NULL;
}
static inline void stdcall_ret(CpuCtx *c, int nargs, uint32_t retval) {
    c->eax = retval;
    c->esp += 4u + (uint32_t)nargs * 4u;
}
static inline void cdecl_ret(CpuCtx *c, uint32_t retval) {
    c->eax = retval;
    c->esp += 4u;
}

#endif /* MHA2_RUNTIME_H */
