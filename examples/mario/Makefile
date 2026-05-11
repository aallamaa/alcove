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

.PHONY: all run clean theme

all: $(SHIM_LIB) $(SHIM_LINK)

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
