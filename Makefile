# Auto-detect host arch. JIT supports arm64 and amd64; on anything else
# the jit/jit-mono targets fall back to a non-JIT build.
ARCH         := $(shell uname -m)
JIT_OK       := $(filter aarch64 arm64 x86_64 amd64,$(ARCH))
ifneq ($(JIT_OK),)
  JIT_FLAGS  := -DALCOVE_JIT=1
else
  JIT_FLAGS  :=
endif

# Detect platform → choose the right install command in dependency hints.
UNAME_S      := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  PKG_RL     := brew install readline
  PKG_FFI    := brew install libffi
else ifneq ($(wildcard /etc/debian_version),)
  PKG_RL     := sudo apt install libreadline-dev
  PKG_FFI    := sudo apt install libffi-dev
else ifneq ($(wildcard /etc/fedora-release),)
  PKG_RL     := sudo dnf install readline-devel
  PKG_FFI    := sudo dnf install libffi-devel
else ifneq ($(wildcard /etc/arch-release),)
  PKG_RL     := sudo pacman -S readline
  PKG_FFI    := sudo pacman -S libffi
else
  PKG_RL     := install libreadline-dev (or your distro's equivalent)
  PKG_FFI    := install libffi-dev (or your distro's equivalent)
endif

# Auto-detect libreadline for REPL line editing + tab completion. Try
# pkg-config first (covers macOS Homebrew + most Linux distros), fall
# back to a header sniff. macOS's libedit doesn't ship the header at
# all, so brew's readline is the supported path on Darwin.
RL_PC_OK := $(shell pkg-config --exists readline 2>/dev/null && echo yes)
ifeq ($(RL_PC_OK),yes)
  RL_FLAGS := -DALCOVE_READLINE=1 $(shell pkg-config --cflags readline)
  RL_LIBS  := $(shell pkg-config --libs readline)
  RL_OK    := yes
else
  RL_BREW  := $(shell command -v brew >/dev/null 2>&1 && brew --prefix readline 2>/dev/null)
  ifneq ($(wildcard /usr/include/readline/readline.h),)
    RL_FLAGS := -DALCOVE_READLINE=1
    RL_LIBS  := -lreadline
    RL_OK    := yes
  else ifneq ($(RL_BREW),)
  ifneq ($(wildcard $(RL_BREW)/include/readline/readline.h),)
    RL_FLAGS := -DALCOVE_READLINE=1 -I$(RL_BREW)/include
    RL_LIBS  := -L$(RL_BREW)/lib -lreadline
    RL_OK    := yes
  endif
  endif
endif
RL_OK    := $(or $(RL_OK),no)

# Auto-detect libffi for the (ffi-fn ...) builtin. pkg-config is the
# universal answer (works on macOS Homebrew, Debian/Ubuntu, Fedora,
# Arch, MSYS2). Header sniff is the fallback for systems without
# pkg-config installed.
FFI_PC_OK := $(shell pkg-config --exists libffi 2>/dev/null && echo yes)
ifeq ($(FFI_PC_OK),yes)
  FFI_FLAGS := -DALCOVE_FFI=1 $(shell pkg-config --cflags libffi)
  FFI_LIBS  := $(shell pkg-config --libs libffi)
  FFI_OK    := yes
else
  FFI_HDR_OK := $(shell test -f /usr/include/ffi.h \
                       -o -f /usr/include/x86_64-linux-gnu/ffi.h \
                       -o -f /usr/include/aarch64-linux-gnu/ffi.h \
                       && echo yes)
  ifeq ($(FFI_HDR_OK),yes)
    FFI_FLAGS := -DALCOVE_FFI=1
    FFI_LIBS  := -lffi
    FFI_OK    := yes
  endif
endif
FFI_OK    := $(or $(FFI_OK),no)

# dlopen/dlsym live in libdl on Linux but in libSystem on macOS — the
# pkg-config path doesn't include -ldl, so add it ourselves on Linux.
ifeq ($(FFI_OK),yes)
  ifeq ($(UNAME_S),Linux)
    FFI_LIBS += -ldl
  endif
endif

# Reusable hint snippet — prints a one-shot summary of what's missing
# and the exact command to fix it. Cheap if everything's present (no
# `@echo` at all), nudging without nagging.
define print_dep_hints
	@if [ "$(RL_OK)" != "yes" ] || [ "$(FFI_OK)" != "yes" ]; then \
	  echo ""; \
	  echo "  optional features disabled:"; \
	  if [ "$(RL_OK)" != "yes" ]; then \
	    echo "    libreadline (REPL line editing, history, paren-match, color)"; \
	    echo "       install: $(PKG_RL)"; \
	  fi; \
	  if [ "$(FFI_OK)" != "yes" ]; then \
	    echo "    libffi (the (ffi-fn ...) builtin for calling C libraries)"; \
	    echo "       install: $(PKG_FFI)"; \
	  fi; \
	  echo ""; \
	fi
endef

# Default goal: JIT release build (auto-arch). Explicit opt-outs:
#   make nojit    — release without JIT (atomic refcounts)
#   make parser   — debug build (-g3, no JIT)
#   make speed    — same as nojit (kept for back-compat)
.DEFAULT_GOAL := jit

parser:
	$(CC) -Wall -W  -g3 -o alcove  alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)
speed:
	$(CC) -Wall -W  -O3 -o alcove  alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)
# Explicit non-JIT release build — alias of `speed`. Use this when you
# want to opt out of the JIT path (e.g. for A/B comparison).
nojit: speed
# Single-threaded build: plain ++/-- refcounts instead of __sync atomics.
# Correctness is identical as long as nothing threads the interpreter.
mono:
	$(CC) -Wall -W  -O3 -DALCOVE_SINGLE_THREADED=1 -o alcove alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)
# JIT build. Auto-picks the arm64 or amd64 backend based on `uname -m`.
# On unsupported architectures, JIT_FLAGS is empty and you get a plain
# bytecode build (with a warning).
jit:
ifeq ($(JIT_OK),)
	@echo "warning: no JIT backend for $(ARCH); building bytecode-only."
endif
	$(CC) -Wall -W  -O3 $(JIT_FLAGS) -o alcove  alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)
# JIT + single-threaded refcount — the fastest build. Pair as long as
# nothing threads the interpreter.
jit-mono:
ifeq ($(JIT_OK),)
	@echo "warning: no JIT backend for $(ARCH); building bytecode-only."
endif
	$(CC) -Wall -W  -O3 $(JIT_FLAGS) -DALCOVE_SINGLE_THREADED=1 -o alcove alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)
# -Os removed

# Print just the dependency status without rebuilding. Handy when a user
# wonders "why don't I have a colored REPL / why does ffi-fn fail?".
deps:
	@echo "build dependencies on $(UNAME_S) ($(ARCH)):"
	@echo "  JIT backend     : $(if $(JIT_OK),enabled ($(ARCH)),disabled — bytecode-only on $(ARCH))"
	@echo "  libreadline     : $(if $(filter yes,$(RL_OK)),enabled,DISABLED — install with: $(PKG_RL))"
	@echo "  libffi          : $(if $(filter yes,$(FFI_OK)),enabled,DISABLED — install with: $(PKG_FFI))"

test: parser
	./alcove test.alc
ifeq ($(FFI_OK),yes)
	@echo
	@echo "=== ffi examples ==="
	@$(MAKE) -C ffi-examples run >/dev/null 2>&1 \
	 && echo "ffi-examples: ok" \
	 || (echo "ffi-examples: FAILED" && exit 1)
endif
benchmark: speed
	./benchmark/run.sh
# Build mono and run the bench suite against it.
benchmark-mono: mono
	./benchmark/run.sh
# Run the bench against the fastest build (jit + mono refcount).
benchmark-jit: jit-mono
	./benchmark/run.sh
# Rebuild both variants and compare numbers side-by-side.
benchmark-compare:
	@./benchmark/compare.sh
clean:
	rm -f alcove

.PHONY: parser speed nojit mono jit jit-mono deps test benchmark benchmark-mono benchmark-jit benchmark-compare clean
