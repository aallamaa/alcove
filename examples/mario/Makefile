# Cross-platform build for the SDL2 graphics shim used by mario.alc.
#
# macOS  → libmariogfx.dylib + libmariogfx.so symlink (alcove FFI dlopens
#          the .so name on every platform)
# Linux  → libmariogfx.so
#
# Requires SDL2 + SDL2_mixer development files, plus curl (used by
# `make run` to download the theme.ogg music track on first launch).
#   macOS:   brew install sdl2 sdl2_mixer curl
#   Debian:  sudo apt install libsdl2-dev libsdl2-mixer-dev curl
#   Fedora:  sudo dnf install SDL2-devel SDL2_mixer-devel curl
#
# Usage:  make           # build the shim
#         make run       # build then launch the game
#         make clean

UNAME_S := $(shell uname -s)

CC      ?= cc
CFLAGS  := -O2 -Wall -Wextra -fPIC -std=c11 $(shell sdl2-config --cflags)
LDLIBS  := $(shell sdl2-config --libs) -lSDL2_mixer

SHIM_SRC := mario_gfx.c

ifeq ($(UNAME_S),Darwin)
  SHIM_LIB    := libmariogfx.dylib
  SHIM_LINK   := libmariogfx.so
  LDFLAGS     := -dynamiclib -Wl,-install_name,@rpath/$(SHIM_LIB)
else
  SHIM_LIB    := libmariogfx.so
  SHIM_LINK   :=
  LDFLAGS     := -shared
endif

ALCOVE := ../../alcove

THEME      := theme.ogg
THEME_URL  := https://dn721802.ca.archive.org/0/items/super-mario-history-anniversary/01%20Super%20Mario%20Bros.%20-%20Ground%20Theme.ogg

.PHONY: all run clean theme web web-clean

all: $(SHIM_LIB) $(SHIM_LINK)

# Web build: compiles alcove.c into web/mario-core.{js,wasm} via Emscripten.
# The gfx_* functions are NOT compiled in — they're implemented in JS
# (web/mario_gfx.js) and injected at runtime via alcove_register_cmd. This
# keeps alcove.c free of mario-specific code.
#
# -sASYNCIFY=1 lets the synchronous game loop in mario.alc yield to the
# browser inside (gfx-sleep-ms 16); see web/mario_gfx.js for the JS side.
EMCC ?= emcc
WEB_OUT := web/mario-core.js
web: $(WEB_OUT)

$(WEB_OUT): ../../alcove.c ../../alcove.h ../../builtins.h web/mario.html web/mario.js web/mario_gfx.js mario.alc $(THEME)
	@command -v $(EMCC) >/dev/null 2>&1 || \
	  (echo "emcc not found. Install Emscripten from https://emscripten.org"; \
	   exit 1)
	$(EMCC) -O2 -Wall -W \
	  -DALCOVE_WEB=1 -UALCOVE_JIT -UALCOVE_FFI -UALCOVE_READLINE \
	  -sASYNCIFY=1 \
	  -sNO_EXIT_RUNTIME=1 \
	  -sALLOW_MEMORY_GROWTH=1 \
	  -sALLOW_TABLE_GROWTH=1 \
	  -sMODULARIZE=1 \
	  -sEXPORT_NAME=createMarioModule \
	  -sEXPORTED_FUNCTIONS=_main,_alcove_web_eval,_alcove_register_cmd,_alcove_arg_int,_alcove_arg_string,_alcove_make_int \
	  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,addFunction,UTF8ToString \
	  -o $(WEB_OUT) ../../alcove.c -lm
	@cp mario.alc web/mario.alc
	@cp $(THEME) web/$(THEME)
	@echo
	@echo "Built $(WEB_OUT). To run:"
	@echo "  python3 -m http.server -d web 8000"
	@echo "  open http://localhost:8000/mario.html"

web-clean:
	rm -f web/mario-core.js web/mario-core.wasm web/mario.alc web/$(THEME)

$(THEME):
	@command -v curl >/dev/null 2>&1 || \
	  (echo "curl is required to download $(THEME). Install it:"; \
	   echo "  macOS:  brew install curl"; \
	   echo "  Debian: sudo apt install curl"; \
	   echo "  Fedora: sudo dnf install curl"; \
	   exit 1)
	@echo "Note: $(THEME) is the 'Ground Theme' from Super Mario Bros."
	@echo "      (composed by Koji Kondo), copyrighted by Nintendo Co., Ltd."
	@echo "      This Makefile fetches it from a third-party host for local"
	@echo "      playback only. See README.md > Legal notice. Press Ctrl-C"
	@echo "      within 3 seconds to cancel."
	@sleep 3
	@echo "Downloading $(THEME)..."
	@curl -fL --retry 3 -o $(THEME) "$(THEME_URL)" || \
	  (rm -f $(THEME); echo "Failed to download $(THEME) from $(THEME_URL)"; exit 1)

theme: $(THEME)

$(SHIM_LIB): $(SHIM_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

ifneq ($(SHIM_LINK),)
$(SHIM_LINK): $(SHIM_LIB)
	ln -sf $(SHIM_LIB) $(SHIM_LINK)
endif

run: all $(THEME)
	$(ALCOVE) --noload --noinit mario.alc

clean:
	rm -f libmariogfx.dylib libmariogfx.so
