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

# Auto-detect libreadline for REPL line editing + tab completion. The
# project still builds and runs without it (just the plain stdin loop);
# we only enable when the dev header is available.
# Linux:        /usr/include/readline/readline.h (system pkg)
# macOS+brew:   $(brew --prefix readline)/include/readline/readline.h
#               (Apple's libedit doesn't ship the header at all)
RL_BREW := $(shell command -v brew >/dev/null 2>&1 && brew --prefix readline 2>/dev/null)
RL_OK   := no
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

# Auto-detect libffi for the (ffi-fn ...) builtin. Without it, ffi-fn
# is unavailable but everything else still builds (e.g. cross-compiling
# for arm64 without libffi installed).
# Linux:        /usr/include/{,x86_64-linux-gnu/,aarch64-linux-gnu/}ffi.h
# macOS+brew:   $(brew --prefix libffi)/include/ffi.h (Apple ships libffi
#               as a system lib but no header in the SDK, so brew's libffi
#               is the supported path on Darwin)
FFI_BREW := $(shell command -v brew >/dev/null 2>&1 && brew --prefix libffi 2>/dev/null)
FFI_OK   := no
ifneq ($(or $(wildcard /usr/include/ffi.h),$(wildcard /usr/include/x86_64-linux-gnu/ffi.h),$(wildcard /usr/include/aarch64-linux-gnu/ffi.h)),)
  FFI_FLAGS := -DALCOVE_FFI=1
  FFI_LIBS  := -lffi -ldl
  FFI_OK    := yes
else ifneq ($(FFI_BREW),)
ifneq ($(wildcard $(FFI_BREW)/include/ffi.h),)
  FFI_FLAGS := -DALCOVE_FFI=1 -I$(FFI_BREW)/include
  FFI_LIBS  := -L$(FFI_BREW)/lib -lffi
  FFI_OK    := yes
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
