CC       ?= gcc
PKG_CFG  ?= pkg-config
PKG_DEPS := sdl2 SDL2_ttf libavformat libavcodec libswscale libavutil gl

SDL_CFLAGS ?= $(shell $(PKG_CFG) --cflags $(PKG_DEPS))
SDL_LIBS   ?= $(shell $(PKG_CFG) --libs   $(PKG_DEPS))

ifeq ($(filter clean lift test,$(MAKECMDGOALS)),)
ifeq ($(wildcard recomp/gen/dispatch.c),)
$(error Generated C sources in recomp/gen/ are missing. Run 'make lift' first)
endif
ifeq ($(strip $(SDL_LIBS)),)
$(error Failed to resolve development libraries ($(PKG_DEPS)) via '$(PKG_CFG)'. Run inside 'nix-shell' or pass SDL_CFLAGS/SDL_LIBS)
endif
endif

CFLAGS_COMMON := -O2 -Iruntime -Irecomp/gen -fno-strict-aliasing -MMD -MP
CFLAGS_RT     := $(CFLAGS_COMMON) -Wall -Wextra $(SDL_CFLAGS)
CFLAGS_GEN    := $(CFLAGS_COMMON) -Wno-unused-label -Wno-unused-variable -Wno-parentheses
# -rdynamic exports sub_<VA> symbols so crash backtraces name the guest functions
LDFLAGS       := $(SDL_LIBS) -lpthread -lm -rdynamic

RT_SRCS := \
	runtime/main.c \
	runtime/cpu.c \
	runtime/guest_mem.c \
	runtime/gl_renderer.c \
	runtime/gl_shaders.c \
	runtime/gl_menu.c \
	runtime/gl_dialog_log.c \
	runtime/hd_assets.c \
	runtime/win32_kernel.c \
	runtime/win32_user_gdi.c \
	runtime/input_replay.c \
	runtime/input_bindings.c \
	runtime/text_wrap.c \
	runtime/win32_ddraw.c \
	runtime/win32_dinput.c \
	runtime/win32_dsound.c \
	runtime/win32_avi.c \
	runtime/game_patches.c \
	runtime/guest_sync.c \
	runtime/crash_handler.c

GEN_SRCS := recomp/gen/dispatch.c $(wildcard recomp/gen/chunk_*.c)

BUILD_DIR ?= build
RT_OBJS   := $(patsubst runtime/%.c,$(BUILD_DIR)/rt_%.o,$(RT_SRCS))
GEN_OBJS  := $(patsubst recomp/gen/%.c,$(BUILD_DIR)/gen_%.o,$(GEN_SRCS))
RT_DEPS   := $(RT_OBJS:.o=.d)
GEN_DEPS  := $(GEN_OBJS:.o=.d)

TARGET ?= mha2_linux

.PHONY: all clean lift test regress

all: $(TARGET)

lift:
	python3 recomp/lift.py --standalone

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/rt_%.o: runtime/%.c runtime/cpu.h runtime/runtime.h runtime/gl_renderer.h runtime/gl_internal.h runtime/hd_assets.h recomp/gen/imports.h recomp/gen/funcs.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_RT) -c $< -o $@

$(BUILD_DIR)/gen_%.o: recomp/gen/%.c runtime/cpu.h recomp/gen/funcs.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_GEN) -c $< -o $@

$(TARGET): $(RT_OBJS) $(GEN_OBJS)
	$(CC) $(RT_OBJS) $(GEN_OBJS) -o $@.tmp $(LDFLAGS)
	mv -f $@.tmp $@
	@echo "Built native Linux static recompilation: ./$(TARGET)"

# Standalone runtime unit/stress tests (no SDL, no recomp/gen/ required)
TESTS := $(BUILD_DIR)/test_guest_sync $(BUILD_DIR)/test_guest_mem $(BUILD_DIR)/test_cpu_semantics $(BUILD_DIR)/test_input_bindings $(BUILD_DIR)/test_text_wrap
TEST_CFLAGS := -O2 -g -Wall -Wextra -Iruntime

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; $$t; done

$(BUILD_DIR)/test_guest_sync: tests/test_guest_sync.c runtime/guest_sync.c runtime/cpu.h | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) tests/test_guest_sync.c runtime/guest_sync.c -o $@ -lpthread

$(BUILD_DIR)/test_guest_mem: tests/test_guest_mem.c runtime/guest_mem.c runtime/crash_handler.c runtime/cpu.h | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -rdynamic tests/test_guest_mem.c runtime/guest_mem.c runtime/crash_handler.c -o $@ -lpthread

$(BUILD_DIR)/test_cpu_semantics: tests/test_cpu_semantics.c runtime/guest_mem.c runtime/crash_handler.c runtime/cpu.h | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -rdynamic tests/test_cpu_semantics.c runtime/guest_mem.c runtime/crash_handler.c -o $@ -lpthread -lm

# Replay-driven gameplay smoke/regression runs (headless, sandboxed; docs/REPLAY.md)
regress: $(TARGET)
	tools/regress.sh

$(BUILD_DIR)/test_input_bindings: tests/test_input_bindings.c runtime/input_bindings.c runtime/input_bindings.h | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $(SDL_CFLAGS) tests/test_input_bindings.c runtime/input_bindings.c -o $@ $(SDL_LIBS)

$(BUILD_DIR)/test_text_wrap: tests/test_text_wrap.c runtime/text_wrap.c runtime/text_wrap.h | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $(SDL_CFLAGS) tests/test_text_wrap.c runtime/text_wrap.c -o $@ $(SDL_LIBS)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(RT_DEPS) $(GEN_DEPS)
