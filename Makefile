# Auto-detect host arch. JIT supports arm64 and amd64; on anything else
# the jit/jit-mono targets fall back to a non-JIT build.
ARCH         := $(shell uname -m)
JIT_OK       := $(filter aarch64 arm64 x86_64 amd64,$(ARCH))
PREFIX       ?= $(HOME)/.local
BINDIR       ?= $(PREFIX)/bin
# alcove relies on tagged-pointer / union type punning (exp_t values, the
# fixnum tag, the content/bc union). That technically violates C's strict-
# aliasing rule; at -O2/-O3 a type-based-alias-analysis pass can reorder or
# elide those punned loads. It's latent on x86-64 but the clang/wasm backend
# miscompiles it (symptom: "vec-ref: bad args" in the web Mario demo, an
# inline call result read back as the wrong tag). -fno-strict-aliasing makes
# the punning well-defined — the same flag CPython and most interpreters use.
SAFE_FLAGS   := -fno-strict-aliasing
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
  RL_BREW_ARCH_OK := $(shell if [ "$(UNAME_S)" != "Darwin" ]; then \
                         echo yes; \
                       elif [ -n "$(RL_BREW)" ] && [ -f "$(RL_BREW)/lib/libreadline.dylib" ]; then \
                         file "$(RL_BREW)/lib/libreadline.dylib" | grep -Eq '$(ARCH)|universal' && echo yes; \
                       fi)
  ifneq ($(wildcard /usr/include/readline/readline.h),)
    RL_FLAGS := -DALCOVE_READLINE=1
    RL_LIBS  := -lreadline
    RL_OK    := yes
  else ifneq ($(RL_BREW),)
  ifeq ($(RL_BREW_ARCH_OK),yes)
  ifneq ($(wildcard $(RL_BREW)/include/readline/readline.h),)
    RL_FLAGS := -DALCOVE_READLINE=1 -I$(RL_BREW)/include
    RL_LIBS  := -L$(RL_BREW)/lib -lreadline
    RL_OK    := yes
  endif
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
  # Export the executable's own symbols to the dynamic table so an empty-lib
  # (ffi-fn "" ...) — dlopen(NULL) — can resolve alcove's own functions
  # (the FFI self-test fixtures the test suite binds to). macOS exports by
  # default; Linux needs -rdynamic. Harmless where unneeded.
  FFI_FLAGS += -rdynamic
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
	$(CC) -Wall -W $(SAFE_FLAGS) -g3 -o alcove  alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)
speed:
	$(CC) -Wall -W $(SAFE_FLAGS) -O3 -o alcove  alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)
# Explicit non-JIT release build — alias of `speed`. Use this when you
# want to opt out of the JIT path (e.g. for A/B comparison).
nojit: speed
# Single-threaded build: plain ++/-- refcounts instead of __sync atomics.
# Correctness is identical as long as nothing threads the interpreter.
mono:
	$(CC) -Wall -W $(SAFE_FLAGS) -O3 -DALCOVE_SINGLE_THREADED=1 -o alcove alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)
# JIT build. Auto-picks the arm64 or amd64 backend based on `uname -m`.
# On unsupported architectures, JIT_FLAGS is empty and you get a plain
# bytecode build (with a warning).
jit:
ifeq ($(JIT_OK),)
	@echo "warning: no JIT backend for $(ARCH); building bytecode-only."
endif
	$(CC) -Wall -W $(SAFE_FLAGS) -O3 $(JIT_FLAGS) -o alcove  alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)
# JIT + single-threaded refcount — the fastest build. Pair as long as
# nothing threads the interpreter.
jit-mono:
ifeq ($(JIT_OK),)
	@echo "warning: no JIT backend for $(ARCH); building bytecode-only."
endif
	$(CC) -Wall -W $(SAFE_FLAGS) -O3 $(JIT_FLAGS) -DALCOVE_SINGLE_THREADED=1 -o alcove alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)

# Adder build: the full JIT runtime + the .adr front end, emitted
# as the local adder binary. adder.c #includes alcove.c.
adder:
ifeq ($(JIT_OK),)
	@echo "warning: no JIT backend for $(ARCH); building bytecode-only."
endif
	$(CC) -Wall -W $(SAFE_FLAGS) -O3 $(JIT_FLAGS) -o adder  adder.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)

# Backwards-compatible aliases for the old target names.
als: adder
alcoves: adder

install: jit adder
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 alcove "$(DESTDIR)$(BINDIR)/alcove"
	install -m 755 adder "$(DESTDIR)$(BINDIR)/adder"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/alcove" "$(DESTDIR)$(BINDIR)/adder"
# -Os removed

# Print just the dependency status without rebuilding. Handy when a user
# wonders "why don't I have a colored REPL / why does ffi-fn fail?".
deps:
	@echo "build dependencies on $(UNAME_S) ($(ARCH)):"
	@echo "  JIT backend     : $(if $(JIT_OK),enabled ($(ARCH)),disabled — bytecode-only on $(ARCH))"
	@echo "  libreadline     : $(if $(filter yes,$(RL_OK)),enabled,DISABLED — install with: $(PKG_RL))"
	@echo "  libffi          : $(if $(filter yes,$(FFI_OK)),enabled,DISABLED — install with: $(PKG_FFI))"

# Quick check: debug build, run the full suite, and HARD-FAIL on any test
# failure or crash (test.alc prints "TEST RESULT: N passed, M failed";
# M must be 0). Then the ffi examples. For the cross-variant matrix use
# `make test-all`.
test: parser
	@tmp=/tmp/alcove-test.$$$$.out; \
	./alcove --noload test.alc 2>&1 | tee "$$tmp"; \
	res=$$(sed 's/\x1b\[[0-9;]*m//g' "$$tmp" | grep 'TEST RESULT'); \
	rm -f "$$tmp"; \
	echo ""; \
	case "$$res" in \
	  *" 0 failed") echo ">>> $$res — OK";; \
	  "") echo ">>> test.alc produced no TEST RESULT line (crash/early exit)"; exit 1;; \
	  *) echo ">>> $$res — FAILURES"; exit 1;; \
	esac
ifeq ($(FFI_OK),yes)
	@echo
	@echo "=== ffi examples ==="
	@$(MAKE) -C ffi-examples run >/dev/null 2>&1 \
	 && echo "ffi-examples: ok" \
	 || (echo "ffi-examples: FAILED" && exit 1)
endif

# Cross-variant matrix: build and run the full suite against every core
# build variant (parser/speed/mono/jit/jit-mono) for the native `alcove`,
# AND the same five variants of the adder front end `adder`
# (running test.adr + a new-features.adr crash check) — so JIT- and
# threading-specific regressions are caught on both binaries. Prints one
# line per variant and exits non-zero if ANY variant fails to build,
# crashes, or reports a non-zero failed count. Restores ./alcove (jit) and
# ./adder (als) at the end.
TEST_VARIANTS := parser speed mono jit jit-mono
# Per-variant compiler flags, matching the parser/speed/mono/jit/jit-mono
# targets. Used to build adder.c (which #includes alcove.c) in each config.
ALS_SPECS := "parser:-g3" "speed:-O3" "mono:-O3 -DALCOVE_SINGLE_THREADED=1" \
             "jit:-O3 $(JIT_FLAGS)" "jit-mono:-O3 $(JIT_FLAGS) -DALCOVE_SINGLE_THREADED=1"
test-all:
	@ok=1; bld=/tmp/alcove-bld.$$$$; \
	for V in $(TEST_VARIANTS); do \
	  printf '\n=== alcove/%s ===\n' "$$V"; \
	  if $(MAKE) -s $$V >"$$bld" 2>&1; then :; \
	  else echo "  BUILD FAILED:"; sed 's/^/    /' "$$bld"; ok=0; continue; fi; \
	  res=$$(./alcove --noload test.alc 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep 'TEST RESULT'); \
	  case "$$res" in \
	    *" 0 failed") echo "  OK — $$res";; \
	    "") echo "  CRASH / early exit — no TEST RESULT line"; ok=0;; \
	    *) echo "  FAILURES — $$res"; ok=0;; \
	  esac; \
	done; \
	abin=/tmp/adder-test.$$$$; \
	for spec in $(ALS_SPECS); do \
	  aname=$${spec%%:*}; aflags=$${spec#*:}; \
	  printf '\n=== adder/%s (adder front end) ===\n' "$$aname"; \
	  if $(CC) -Wall -W $(SAFE_FLAGS) $$aflags -o "$$abin" adder.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS) >"$$bld" 2>&1; then \
	    res=$$("$$abin" --noload test.adr 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep 'TEST RESULT'); \
	    case "$$res" in \
	      *" 0 failed") echo "  OK — $$res (test.adr)";; \
	      "") echo "  CRASH / early exit — no TEST RESULT line"; ok=0;; \
	      *) echo "  FAILURES — $$res"; ok=0;; \
	    esac; \
	    af=0; for i in 1 2 3 4; do "$$abin" --noload examples/adder/new-features.adr >/dev/null 2>&1 || af=1; done; \
	    [ $$af -eq 0 ] || { echo "  CRASH in new-features.adr run"; ok=0; }; \
	  else echo "  BUILD FAILED:"; sed 's/^/    /' "$$bld"; ok=0; fi; \
	done; \
	rm -f "$$abin" "$$bld"; \
	$(MAKE) -s jit >/dev/null 2>&1; $(MAKE) -s adder >/dev/null 2>&1; \
	printf '\n'; \
	if [ $$ok -eq 1 ]; then echo "==> ALL VARIANTS PASSED"; \
	else echo "==> VARIANT FAILURES (see above)"; exit 1; fi
benchmark: speed
	./benchmark/run.sh
# Build mono and run the bench suite against it.
benchmark-mono: mono
	./benchmark/run.sh
# Run the bench against the fastest build (jit + mono refcount).
benchmark-jit: jit-mono
	./benchmark/run.sh
# MLP-only: baseline (per-element interpreter loops) vs tensor ops, on
# the UCI optdigits dataset. The main `make benchmark` already runs the
# tensor-ops side against Python via benchmark/mlp.{alc,py}; this
# target is the side-by-side internal comparison.
benchmark-mlp:
	@$(MAKE) -C examples/mlp benchmark
# Self-contained C RESP client benchmark — randomised-key SET/GET sweep
# against any RESP server (redis, alcove -r). Optional; redis-benchmark
# already covers the same workload via `benchmark/resp-bench.sh`.
benchmark/resp-bench-c: benchmark/resp-bench.c
	$(CC) -O3 -Wall -W -pthread -o $@ $<
# Rebuild both variants and compare numbers side-by-side.
benchmark-compare:
	@./benchmark/compare.sh
# MPSC primitive (mpsc.h) torture test — Step 2.1 of the multithreading
# rollout. Builds standalone, no alcove dependency.
mpsc-test:
	$(CC) -O2 -Wall -W -pthread -o mpsc_test mpsc_test.c
	./mpsc_test
# Same test under ThreadSanitizer. Catches data races the plain build
# can hide. Slower (~10x) and needs Apple Clang's libclang_rt or gcc's
# libtsan installed; skip silently if it can't link.
mpsc-test-tsan:
	$(CC) -O1 -g -Wall -W -pthread -fsanitize=thread -o mpsc_test_tsan mpsc_test.c \
	  && ./mpsc_test_tsan \
	  || echo "tsan unavailable on this toolchain — skipping"

# WebAssembly build via Emscripten. Produces web/alcove-core.{js,wasm}
# and web/adder-core.{js,wasm}, wrapped by web/alcove.js and
# web/adder.js. Excludes JIT, FFI, readline, and the RESP server
# (all unavailable in the browser). See the runnable demos at
# web/index.html and web/learn.html.
EMCC ?= emcc
web:
	@command -v $(EMCC) >/dev/null 2>&1 || \
	  (echo "emcc not found. Install Emscripten from https://emscripten.org"; \
	   exit 1)
	$(EMCC) -O2 -Wall -W $(SAFE_FLAGS) \
	  -DALCOVE_WEB=1 -UALCOVE_JIT -UALCOVE_FFI -UALCOVE_READLINE \
	  -sNO_EXIT_RUNTIME=1 \
	  -sALLOW_MEMORY_GROWTH=1 \
	  -sMODULARIZE=1 \
	  -sEXPORT_NAME=createAlcoveModule \
	  -sEXPORTED_FUNCTIONS=_main,_alcove_web_eval,_alcove_register_cmd,_alcove_arg_int,_alcove_arg_string,_alcove_make_int \
	  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,addFunction,UTF8ToString \
	  -sALLOW_TABLE_GROWTH=1 \
	  -o web/alcove-core.js alcove.c -lm
	$(EMCC) -O2 -Wall -W $(SAFE_FLAGS) \
	  -DALCOVE_WEB=1 -UALCOVE_JIT -UALCOVE_FFI -UALCOVE_READLINE \
	  -sNO_EXIT_RUNTIME=1 \
	  -sALLOW_MEMORY_GROWTH=1 \
	  -sMODULARIZE=1 \
	  -sEXPORT_NAME=createAdderModule \
	  -sEXPORTED_FUNCTIONS=_main,_alcove_web_eval,_alcove_register_cmd,_alcove_arg_int,_alcove_arg_string,_alcove_make_int \
	  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,addFunction,UTF8ToString \
	  -sALLOW_TABLE_GROWTH=1 \
	  -o web/adder-core.js adder.c -lm

clean:
	rm -f alcove adder mpsc_test mpsc_test_tsan
	rm -f web/alcove-core.js web/alcove-core.wasm web/adder-core.js web/adder-core.wasm

# ---------------------------------------------------------------------------
# Formatting & static analysis (clang-format + clang-tidy).
#   make fmt        — reformat the whole tree in place (one-time churn)
#   make fmt-check  — fail if anything is mis-formatted, change nothing
#   make tidy       — run clang-tidy on the main TU (config: .clang-tidy)
#   make hooks      — install the staged-diff pre-commit gate
# Day-to-day, the pre-commit hook checks only the lines you touch, so the
# existing whole-file LLVM drift never blocks a commit. `fmt-check` over the
# whole tree will fail until `make fmt` is run once across everything.
FMT       ?= clang-format
TIDY      ?= clang-tidy
FMT_FILES := $(wildcard *.c *.h)
# Mirror how we actually build alcove.c (JIT + FFI + readline) so the
# analyzer sees the same code the compiler does. adder.c just #includes
# alcove.c, so one TU covers both.
TIDY_CFLAGS := -std=gnu11 $(JIT_FLAGS) -DALCOVE_FFI=1 -DALCOVE_READLINE=1 \
               $(shell pkg-config --cflags libffi 2>/dev/null) \
               $(shell pkg-config --cflags readline 2>/dev/null)

fmt:
	$(FMT) -i $(FMT_FILES)

fmt-check:
	$(FMT) --dry-run --Werror $(FMT_FILES)

tidy:
	$(TIDY) alcove.c -- $(TIDY_CFLAGS)

hooks:
	git config core.hooksPath .githooks
	@echo "pre-commit hook installed (core.hooksPath=.githooks)."
	@echo "It formats + lints only the lines you stage."

.PHONY: parser speed nojit mono jit jit-mono adder als alcoves install uninstall deps test test-all benchmark benchmark-mlp benchmark-mono benchmark-jit benchmark-compare mpsc-test mpsc-test-tsan web clean fmt fmt-check tidy hooks
