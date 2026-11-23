CC       ?= gcc
PKG_CFG  ?= pkg-config
PKG_DEPS := sdl2 SDL2_ttf libavformat libavcodec libswscale libavutil gl

SDL_CFLAGS ?= $(shell $(PKG_CFG) --cflags $(PKG_DEPS))
SDL_LIBS   ?= $(shell $(PKG_CFG) --libs   $(PKG_DEPS))

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(MAKECMDGOALS),lift)
ifeq ($(wildcard recomp/gen/dispatch.c),)
$(error Generated C sources in recomp/gen/ are missing. Run 'make lift' first)
endif
ifeq ($(strip $(SDL_LIBS)),)
$(error Failed to resolve development libraries ($(PKG_DEPS)) via '$(PKG_CFG)'. Run inside 'nix-shell' or pass SDL_CFLAGS/SDL_LIBS)
endif
endif
endif

CFLAGS_COMMON := -O2 -Iruntime -Irecomp/gen -fno-strict-aliasing -MMD -MP
CFLAGS_RT     := $(CFLAGS_COMMON) -Wall -Wextra $(SDL_CFLAGS)
CFLAGS_GEN    := $(CFLAGS_COMMON) -Wno-unused-label -Wno-unused-variable -Wno-parentheses
LDFLAGS       := $(SDL_LIBS) -lpthread -lm

RT_SRCS := \
	runtime/main.c \
	runtime/cpu.c \
	runtime/gl_renderer.c \
	runtime/hd_assets.c \
	runtime/win32_kernel.c \
	runtime/win32_user_gdi.c \
	runtime/win32_ddraw.c \
	runtime/win32_dinput.c \
	runtime/win32_dsound.c \
	runtime/win32_avi.c \
	runtime/game_patches.c

GEN_SRCS := recomp/gen/dispatch.c $(wildcard recomp/gen/chunk_*.c)

BUILD_DIR ?= build
RT_OBJS   := $(patsubst runtime/%.c,$(BUILD_DIR)/rt_%.o,$(RT_SRCS))
GEN_OBJS  := $(patsubst recomp/gen/%.c,$(BUILD_DIR)/gen_%.o,$(GEN_SRCS))
RT_DEPS   := $(RT_OBJS:.o=.d)
GEN_DEPS  := $(GEN_OBJS:.o=.d)

TARGET ?= mha2_linux

.PHONY: all clean lift

all: $(TARGET)

lift:
	python3 recomp/lift.py --standalone

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/rt_%.o: runtime/%.c runtime/cpu.h runtime/runtime.h runtime/gl_renderer.h runtime/hd_assets.h recomp/gen/imports.h recomp/gen/funcs.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_RT) -c $< -o $@

$(BUILD_DIR)/gen_%.o: recomp/gen/%.c runtime/cpu.h recomp/gen/funcs.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_GEN) -c $< -o $@

$(TARGET): $(RT_OBJS) $(GEN_OBJS)
	$(CC) $(RT_OBJS) $(GEN_OBJS) -o $@.tmp $(LDFLAGS)
	mv -f $@.tmp $@
	@echo "Built native Linux static recompilation: ./$(TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(RT_DEPS) $(GEN_DEPS)
