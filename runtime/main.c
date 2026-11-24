#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include "input_replay.h"
#include <sys/resource.h>
#include "gl_renderer.h"
#include "hd_assets.h"
#include "imports.h"
#include "funcs.h"
#include "version.h"
#include <sys/stat.h>
#include <sched.h>

static cpu_set_t g_orig_cpu_mask;
static int g_has_orig_cpu_mask = 0;

void runtime_unpin_current_thread(void) {
    if (g_has_orig_cpu_mask) {
        sched_setaffinity(0, sizeof(g_orig_cpu_mask), &g_orig_cpu_mask);
    }
}

/* Shared data location for split runtime / asset-pack installs (docs/INSTALL.md):
 * $XDG_DATA_HOME/mha2-remastered/<sub>, default ~/.local/share/mha2-remastered/<sub>.
 * Used only when game_files/ or hd_assets/ are not next to the working directory. */
static int xdg_data_path(const char *sub, char *buf, size_t cap) {
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    int n;
    if (xdg && xdg[0]) n = snprintf(buf, cap, "%s/mha2-remastered/%s", xdg, sub);
    else if (home && home[0]) n = snprintf(buf, cap, "%s/.local/share/mha2-remastered/%s", home, sub);
    else return 0;
    return n > 0 && (size_t)n < cap;
}

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void print_help(const char *prog) {
    printf("Moorhuhn Adventure 2: Der Fluch des Goldes — Native Linux Remastered Engine\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --game-dir <dir>         Directory containing mha2.dat / mha2.exe (default: game_files,\n");
    printf("                           else ~/.local/share/mha2-remastered/game_files)\n");
    printf("  -f, --fullscreen         Start in borderless fullscreen\n");
    printf("  --width <W> --height <H> Initial window size (default: 1280x800)\n");
    printf("  --aspect <mode>          Widescreen mode: auto (default), 16:10, 16:9, 4:3\n");
    printf("  --no-ambient-bars        Use pure black sidebars on 4:3 menus instead of ambient blur\n");
    printf("  --shader <name>          Upscaler: bicubic (default), lanczos, edge, bilinear, nearest, crt\n");
    printf("  --vsync <mode>           Frame pacing: adaptive (default), on, off\n");
    printf("  --fps <N>                FPS cap when --vsync off (default: 240)\n");
    printf("  --16bit                  Use original 16-bit RGB565 instead of 32-bit True Color\n");
    printf("  --hd-dir <dir>           Directory containing HD PNG replacements (default: hd_assets)\n");
    printf("  --no-hd-assets           Disable HD PNG background override\n");
    printf("  --no-hd-sprites          Disable HD RGBA foreground sprite override\n");
    printf("  --no-hd-text             Disable native monitor-DPI vector text rendering\n");
    printf("  --no-hw-cursor           Disable high-refresh decoupled cursor rendering\n");
    printf("  --no-bloom               Disable emissive bloom post-processing\n");
    printf("  --grade <mode>           Color grading: original, remastered (default), warm, daylight\n");
    printf("  --no-transitions         Disable smooth room crossfade transitions\n");
    printf("  --hud                    Show live performance/upscaler HUD on startup\n");
    printf("  --debug                  Verbose Win32/DirectX logging\n");
    printf("  --strict-mem             Debug: fault on guest NULL-page access (0x0-0xFFFF)\n");
    printf("  --record <file>          Record keyboard/mouse input to a replay script\n");
    printf("  --replay <file>          Drive the game from a replay script (docs/REPLAY.md)\n");
    printf("  --replay-out <dir>       Output directory for replay screenshots (default: screenshots/replay)\n\n");
    printf("In-Game Hotkeys:\n");
    printf("  F1                       Open/Close Interactive Graphics & Settings Menu\n");
    printf("  F2                       Toggle Split-Screen Before / After Comparison\n");
    printf("  F3                       Toggle live stats HUD\n");
    printf("  F4                       Cycle VSync / high-refresh-rate mode\n");
    printf("  F5                       Toggle Native HD Vector Text & Subtitles\n");
    printf("  F6                       Toggle HD Asset Pack Override\n");
    printf("  F7 / F8 / F9             Master volume down / up / mute\n");
    printf("  F10                      Toggle High-Refresh Decoupled Cursor\n");
    printf("  F11 or Alt+Enter         Toggle borderless fullscreen\n");
    printf("  F12                      Cycle Widescreen Aspect (Auto / 16:10 / 16:9 / 4:3)\n");
    printf("  Ctrl+L                   Open/Close Dialog & Clue History Log\n");
}

const char *mha2_version(void) {
    return MHA2_VERSION;
}

int main(int argc, char **argv) {
    /* Save original multi-core affinity for native worker threads (HD pre-decoder & audio),
       then pin guest threads to Core 0: original 2005 Win98/XP game uses non-atomic
       refcounts across main / loading / processing threads (SingleProcAffinity). */
    CPU_ZERO(&g_orig_cpu_mask);
    if (sched_getaffinity(0, sizeof(g_orig_cpu_mask), &g_orig_cpu_mask) == 0) {
        g_has_orig_cpu_mask = 1;
    }
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(0, &mask);
    sched_setaffinity(0, sizeof(mask), &mask);

    /* The guest main thread runs on the process stack; lifted guest calls nest as
       native C calls, so allow it the same headroom as guest worker threads. */
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
        rl.rlim_cur < GUEST_HOST_THREAD_STACK) {
        rl.rlim_cur = (rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= GUEST_HOST_THREAD_STACK)
                          ? GUEST_HOST_THREAD_STACK : rl.rlim_max;
        setrlimit(RLIMIT_STACK, &rl);
    }

    /* Load persistent user settings before applying CLI overrides */
    gl_config_load("mha2_settings.ini");

    const char *exe_override = NULL;
    int strict_mem = 0;
    const char *record_path = NULL, *replay_path = NULL, *replay_out = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--strict-mem") == 0) {
            strict_mem = 1;
        } else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_path = argv[++i];
        } else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            replay_path = argv[++i];
        } else if (strcmp(argv[i], "--replay-out") == 0 && i + 1 < argc) {
            replay_out = argv[++i];
        } else if (strcmp(argv[i], "--debug") == 0) {
            g_debug_log = 1;
        } else if (strcmp(argv[i], "--game-dir") == 0 && i + 1 < argc) {
            snprintf(g_game_dir, sizeof(g_game_dir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc) {
            exe_override = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fullscreen") == 0) {
            g_cfg.start_fullscreen = 1;
        } else if (strcmp(argv[i], "--windowed") == 0) {
            g_cfg.start_fullscreen = 0;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            g_cfg.init_win_w = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            g_cfg.init_win_h = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--16bit") == 0) {
            g_cfg.truecolor_32bit = 0;
        } else if (strcmp(argv[i], "--hd-dir") == 0 && i + 1 < argc) {
            snprintf(g_cfg.hd_assets_dir, sizeof(g_cfg.hd_assets_dir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--no-hd-assets") == 0) {
            g_cfg.hd_assets_enabled = 0;
        } else if (strcmp(argv[i], "--no-hd-sprites") == 0) {
            g_cfg.hd_sprites_enabled = 0;
        } else if (strcmp(argv[i], "--no-hd-text") == 0) {
            g_cfg.hd_vector_text = 0;
        } else if (strcmp(argv[i], "--no-hw-cursor") == 0) {
            g_cfg.decoupled_cursor = 0;
        } else if (strcmp(argv[i], "--no-ambient-bars") == 0) {
            g_cfg.ambient_bars = 0;
        } else if (strcmp(argv[i], "--no-bloom") == 0) {
            g_cfg.bloom_level = BLOOM_OFF;
        } else if (strcmp(argv[i], "--no-transitions") == 0) {
            g_cfg.scene_transitions = 0;
        } else if (strcmp(argv[i], "--grade") == 0 && i + 1 < argc) {
            const char *g = argv[++i];
            if (strcmp(g, "original") == 0) g_cfg.color_grade = GRADE_ORIGINAL;
            else if (strcmp(g, "remastered") == 0) g_cfg.color_grade = GRADE_REMASTERED;
            else if (strcmp(g, "warm") == 0) g_cfg.color_grade = GRADE_WARM;
            else if (strcmp(g, "daylight") == 0) g_cfg.color_grade = GRADE_DAYLIGHT;
        } else if (strcmp(argv[i], "--aspect") == 0 && i + 1 < argc) {
            const char *a = argv[++i];
            if (strcmp(a, "auto") == 0) g_cfg.aspect_mode = ASPECT_AUTO;
            else if (strcmp(a, "16:10") == 0 || strcmp(a, "1610") == 0) g_cfg.aspect_mode = ASPECT_16_10;
            else if (strcmp(a, "16:9") == 0 || strcmp(a, "169") == 0) g_cfg.aspect_mode = ASPECT_16_9;
            else if (strcmp(a, "4:3") == 0 || strcmp(a, "43") == 0) g_cfg.aspect_mode = ASPECT_4_3;
        } else if (strcmp(argv[i], "--hud") == 0) {
            g_cfg.show_hud = 1;
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            g_cfg.max_fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--vsync") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            if (strcmp(m, "adaptive") == 0) g_cfg.vsync_mode = VSYNC_ADAPTIVE;
            else if (strcmp(m, "on") == 0) g_cfg.vsync_mode = VSYNC_ON;
            else if (strcmp(m, "off") == 0) g_cfg.vsync_mode = VSYNC_UNCAPPED;
        } else if (strcmp(argv[i], "--shader") == 0 && i + 1 < argc) {
            const char *s = argv[++i];
            if (strcmp(s, "bicubic") == 0) g_cfg.shader_mode = SHADER_BICUBIC_CAS;
            else if (strcmp(s, "lanczos") == 0) g_cfg.shader_mode = SHADER_LANCZOS_CAS;
            else if (strcmp(s, "edge") == 0) g_cfg.shader_mode = SHADER_EDGE_SMOOTH;
            else if (strcmp(s, "bilinear") == 0) g_cfg.shader_mode = SHADER_BILINEAR;
            else if (strcmp(s, "nearest") == 0) g_cfg.shader_mode = SHADER_NEAREST;
            else if (strcmp(s, "crt") == 0) g_cfg.shader_mode = SHADER_CRT;
            else if (strcmp(s, "fsr") == 0 || strcmp(s, "fsr1") == 0) g_cfg.shader_mode = SHADER_FSR1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
    }

    if (!file_exists(g_game_dir)) {
        char xdg[1024];
        if (file_exists("mha2.dat")) {
            snprintf(g_game_dir, sizeof(g_game_dir), ".");
        } else if (xdg_data_path("game_files", xdg, sizeof(xdg)) && file_exists(xdg)) {
            snprintf(g_game_dir, sizeof(g_game_dir), "%s", xdg);
        }
    }
    if (!file_exists(g_cfg.hd_assets_dir)) {
        char xdg[1024];
        if (xdg_data_path("hd_assets", xdg, sizeof(g_cfg.hd_assets_dir)) && file_exists(xdg))
            memcpy(g_cfg.hd_assets_dir, xdg, strlen(xdg) + 1); /* fits: length bounded above */
    }

    char exe_path[1280];
    if (exe_override) {
        snprintf(exe_path, sizeof(exe_path), "%s", exe_override);
    } else {
        snprintf(exe_path, sizeof(exe_path), "%s/mha2.exe", g_game_dir);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "ERROR: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    runtime_mem_init();
    crash_handler_install();
    if ((record_path || replay_path) && input_replay_init(record_path, replay_path, replay_out) != 0) {
        return 2;
    }
    if (runtime_load_pe(exe_path) != 0) {
        fprintf(stderr, "ERROR: failed to load PE data from '%s'\n", exe_path);
        return 1;
    }

    init_code_table();
    game_patches_install();
    ddraw_install_rle_hooks();
    hd_assets_init(g_cfg.hd_assets_dir);
    win32_kernel_init();
    win32_user_gdi_init();
    win32_dsound_init();

    CpuCtx main_ctx;
    memset(&main_ctx, 0, sizeof(main_ctx));
    main_ctx.thread_id = guest_alloc_thread_slot(&main_ctx.teb_addr, &main_ctx.esp);
    main_ctx.fpu_cw = 0x037Fu;
    crash_handler_thread_init(&main_ctx);
    if (strict_mem) {
        if (runtime_mem_protect_null_page() == 0)
            fprintf(stderr, "[MHA2] --strict-mem: guest NULL page 0x0-0x%X is protected\n", GUEST_NULL_GUARD_SIZE - 1u);
        else
            perror("[MHA2] --strict-mem: mprotect");
    }

    fprintf(stderr, "[MHA2] Launching Remastered Engine %s (%s, %s, %s, HD-Bg:%d, HD-Sprites:%d, game_dir='%s')...\n",
            MHA2_VERSION,
            g_cfg.truecolor_32bit ? "32-bit TrueColor" : "16-bit RGB565",
            gl_shader_name(g_cfg.shader_mode),
            gl_vsync_name(g_cfg.vsync_mode),
            hd_assets_count(),
            hd_sprites_count(),
            g_game_dir);
    const char *update_notice = getenv("MHA2_UPDATE_NOTICE"); /* set by play.sh after tools/update.sh */
    if (update_notice && update_notice[0]) gl_renderer_set_startup_notice(update_notice);

    push32(&main_ctx, HLE_RET_SENTINEL);
    dispatch_call(&main_ctx, MHA2_ENTRY_VA);

    gl_config_save("mha2_settings.ini");
    input_replay_shutdown();
    win32_dsound_shutdown();
    gl_renderer_shutdown();
    hd_assets_shutdown();
    SDL_Quit();
    return 0;
}
