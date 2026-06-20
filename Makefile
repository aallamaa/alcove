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
# Tune codegen for the EXACT host CPU (AVX-512 / FMA / etc. where present).
# You recompile per machine, so a host-specific binary is fine — and this is
# self-adapting: it targets whatever the build host supports. Apple clang on
# arm64 (Apple Silicon) REJECTS -march=native, so there we fall back to the arm
# idiom -mcpu=native (then -mcpu=apple-m1) — without it a Mac build runs untuned
# and benchmarks noticeably below an equivalently-tuned x86 build. The probe
# picks the first candidate the compiler accepts, blank if none (older toolchains
# / odd targets still build). Override with `make MARCH='-march=x86-64-v3'` for a
# portable baseline, or `make MARCH=` to disable. Native (non-emcc) builds only.
ifeq ($(origin MARCH),undefined)
  # Auto-detect: on arm64 try -mcpu fallbacks after -march=native.
  ifneq (,$(filter arm64 aarch64,$(ARCH)))
    MARCH_TRY  := -march=native -mcpu=native -mcpu=apple-m1
  else
    MARCH_TRY  := -march=native
  endif
  MARCH      := $(strip $(shell for f in $(MARCH_TRY); do \
                  if $(CC) $$f -xc -S -o /dev/null /dev/null >/dev/null 2>&1; \
                  then echo $$f; break; fi; done))
else
  # Explicit override (incl. empty to disable): validate it compiles, blank if not.
  MARCH      := $(shell $(CC) $(MARCH) -xc -S -o /dev/null /dev/null >/dev/null 2>&1 && printf '%s' '$(MARCH)')
endif
ifneq ($(JIT_OK),)
  JIT_FLAGS  := -DALCOVE_JIT=1
else
  JIT_FLAGS  :=
endif

# Detect platform → choose the right install command in dependency hints.
UNAME_S      := $(shell uname -s)
# Shared-library extension for native modules: .dylib on macOS, .so elsewhere
# (matches the (dylib-suffix) builtin and require's explicit-extension handling).
NATIVE_EXT   := $(if $(filter Darwin,$(UNAME_S)),dylib,so)
# Link flags for a loadable native module (dlopen'd via require). Linux allows
# undefined symbols in a .so and resolves them at dlopen time against the host
# (built -rdynamic); macOS's linker REJECTS undefined symbols in a .dylib at link
# time, so it needs -undefined dynamic_lookup to defer resolution to load (the
# host exports its symbols there too via -rdynamic/-export_dynamic). Linux-neutral.
MODULE_LDFLAGS := -shared -fPIC
ifeq ($(UNAME_S),Darwin)
  MODULE_LDFLAGS += -undefined dynamic_lookup
endif
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
	$(CC) -Wall -W $(SAFE_FLAGS) -O3 $(MARCH) -o alcove  alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)
# Explicit non-JIT release build — alias of `speed`. Use this when you
# want to opt out of the JIT path (e.g. for A/B comparison).
nojit: speed
# Single-threaded build: plain ++/-- refcounts instead of __sync atomics.
# Correctness is identical as long as nothing threads the interpreter.
mono:
	$(CC) -Wall -W $(SAFE_FLAGS) -O3 $(MARCH) -DALCOVE_SINGLE_THREADED=1 -o alcove alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)
# JIT build. Auto-picks the arm64 or amd64 backend based on `uname -m`.
# On unsupported architectures, JIT_FLAGS is empty and you get a plain
# bytecode build (with a warning).
jit:
ifeq ($(JIT_OK),)
	@echo "warning: no JIT backend for $(ARCH); building bytecode-only."
endif
	$(CC) -Wall -W $(SAFE_FLAGS) -O3 $(MARCH) $(JIT_FLAGS) -o alcove  alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)
# JIT + single-threaded refcount — the fastest build. Pair as long as
# nothing threads the interpreter.
jit-mono:
ifeq ($(JIT_OK),)
	@echo "warning: no JIT backend for $(ARCH); building bytecode-only."
endif
	$(CC) -Wall -W $(SAFE_FLAGS) -O3 $(MARCH) $(JIT_FLAGS) -DALCOVE_SINGLE_THREADED=1 -o alcove alcove.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)

# Opt-in observability metrics. The metrics registry + RESP auto-instrumentation
# (counter!/gauge!/metric/metrics + resp.connections/.commands/.errors) compile
# out entirely unless built with -DALCOVE_METRICS, so the default binary carries
# zero per-command atomic-bump overhead on the RESP hot path. error-code and
# leveled logging are always shipped (no passive cost); only metrics are gated.
# Reuse the canonical jit/adder recipes, just adding -DALCOVE_METRICS to the
# JIT flags — so the compile/link line stays single-sourced (no drift if FFI/
# readline/SAFE_FLAGS handling changes). $(JIT_FLAGS) expands in this parent, so
# the sub-make inherits the same arch-resolved flags plus the metrics define.
alcove-with-metrics:
	$(MAKE) jit JIT_FLAGS="$(JIT_FLAGS) -DALCOVE_METRICS"
adder-with-metrics:
	$(MAKE) adder JIT_FLAGS="$(JIT_FLAGS) -DALCOVE_METRICS"

# Adder build: the full JIT runtime + the .adr front end, emitted as the local
# adder binary. adder.c #includes alcove.c; adfmt.c (the `adder fmt` formatter)
# is a SEPARATE TU linked in (-DADFMT_NO_MAIN suppresses its standalone main) so
# its generic helpers don't collide with the engine's.
adder:
ifeq ($(JIT_OK),)
	@echo "warning: no JIT backend for $(ARCH); building bytecode-only."
endif
	$(CC) -Wall -W $(SAFE_FLAGS) -O3 $(MARCH) $(JIT_FLAGS) -DADFMT_NO_MAIN -c adfmt.c -o adfmt.o
	$(CC) -Wall -W $(SAFE_FLAGS) -O3 $(MARCH) $(JIT_FLAGS) -o adder  adder.c adfmt.o $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(print_dep_hints)

# Standalone formatter binary (handy for piping; `adder fmt` is the shipped CLI).
adfmt:
	$(CC) -Wall -W $(SAFE_FLAGS) -O2 -o adfmt adfmt.c

# Adder-formatter gate: the formatter must be meaning-preserving (transpiling
# original vs formatted through adr.h yields identical s-exprs) + idempotent over
# the .adr corpus, and convert an Alcove s-expr file to indented Adder. See
# tools/adfmt_test.sh. (Distinct from `fmt-check`, the C clang-format gate.)
adfmt-test: adder
	sh tools/adfmt_test.sh

# Regenerate test.adr from test.alc (shared engine suite, transpiled via
# alc2adr.py) + test_adder_extra.adr (adder-syntax-only tests). Keeps the
# adder test corpus in lockstep with alcove's instead of drifting as a
# hand-maintained subset. The file rule fires only when a source is newer.
test.adr: test.alc test_adder_extra.adr alc2adr.py gen_test_adr.py
	python3 gen_test_adr.py
gen-test-adr:
	python3 gen_test_adr.py

# Regenerate the wasm smoke-test battery (web/web_battery.js) from native
# alcove/adder output over web/web_exprs_{lisp,adder}.txt. Builds the two
# native binaries first so the expected values are current.
gen-web-battery: jit adder
	python3 gen_web_battery.py
web/web_battery.js: web/web_exprs_lisp.txt web/web_exprs_adder.txt gen_web_battery.py
	@$(MAKE) -s gen-web-battery

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
# Full interpreter (jit variant) under ASan + UBSan. The cross-variant `test-all`
# runs the evaluator/VM/JIT WITHOUT sanitizers — only the isolated *-test C
# harnesses get them — so a memory-safety/UB bug in the hot path would pass CI.
# This closes that hole: build alcove + adder with -fsanitize=address,undefined
# (UBSan set to abort via -fno-sanitize-recover) and run the full test.alc /
# test.adr. Leak detection is OFF (the interpreter intentionally retains the
# global env + exp_t arena blocks at exit; per-allocation leaks are covered by
# the *-test unit harnesses). Fails on any sanitizer report, a non-zero exit, or
# a non-"0 failed" result.
ASAN_BUILD := -fno-strict-aliasing -g -O1 -fsanitize=address,undefined \
              -fno-sanitize-recover=all
test-asan:
	$(CC) -Wall -W $(ASAN_BUILD) $(MARCH) $(JIT_FLAGS) -o alcove_asan alcove.c \
	  $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	$(CC) -Wall -W $(ASAN_BUILD) $(MARCH) $(JIT_FLAGS) -DALCOVE_ALS -o adder_asan \
	  adder.c $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	@ok=1; \
	for spec in "alcove_asan test.alc" "adder_asan test.adr"; do \
	  set -- $$spec; bin=$$1; f=$$2; \
	  out=$$(ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
	         ./$$bin --noload $$f 2>&1); rc=$$?; \
	  res=$$(echo "$$out" | sed 's/\x1b\[[0-9;]*m//g' | grep 'TEST RESULT'); \
	  if echo "$$out" | grep -qiE 'AddressSanitizer|runtime error:|LeakSanitizer'; then \
	    echo "  SANITIZER REPORT [$$bin]:"; \
	    echo "$$out" | grep -iE 'Sanitizer|runtime error:' | sed 's/^/    /' | head -8; ok=0; \
	  elif [ $$rc -ne 0 ]; then echo "  NON-ZERO EXIT [$$bin] ($$rc): $$res"; ok=0; \
	  else case "$$res" in *" 0 failed") echo "  OK [$$bin] — $$res (ASan+UBSan clean)";; \
	    *) echo "  FAIL [$$bin] — $$res"; ok=0;; esac; fi; \
	done; \
	rm -f alcove_asan adder_asan; \
	[ $$ok -eq 1 ] || { echo "==> test-asan FAILED"; exit 1; }
	@echo "==> test-asan PASSED (hot path clean under ASan+UBSan)"

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
	printf '\n=== test.adr freshness (generated from test.alc) ===\n'; \
	gadr=/tmp/test.adr.check.$$$$; \
	if python3 gen_test_adr.py -o "$$gadr" >/dev/null 2>&1 && \
	   diff -q "$$gadr" test.adr >/dev/null 2>&1; then \
	  echo "  OK — test.adr matches gen_test_adr.py(test.alc + extra)"; \
	else \
	  echo "  STALE — test.adr is out of sync; run 'make gen-test-adr' and commit"; \
	  ok=0; \
	fi; rm -f "$$gadr"; \
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
	    af=0; for i in 1 2 3 4; do "$$abin" --noload examples/adder/new-features.adr >/dev/null 2>&1; \
	      rc=$$?; [ $$rc -ge 128 ] && af=1; done; \
	    [ $$af -eq 0 ] || { echo "  CRASH in new-features.adr run"; ok=0; }; \
	  else echo "  BUILD FAILED:"; sed 's/^/    /' "$$bld"; ok=0; fi; \
	done; \
	rm -f "$$abin" "$$bld"; \
	wbld=/tmp/alcove-web.$$$$; \
	if command -v $(EMCC) >/dev/null 2>&1 && command -v node >/dev/null 2>&1; then \
	  printf '\n=== wasm/emcc smoke (alcove + adder) ===\n'; \
	  if $(MAKE) -s web >"$$wbld" 2>&1 && node web/test_web.js >>"$$wbld" 2>&1; then \
	    grep -E 'passed,' "$$wbld" | sed 's/^/  OK — /'; \
	  else echo "  WASM SMOKE FAILED:"; sed 's/^/    /' "$$wbld" | tail -20; ok=0; fi; \
	else printf '\n=== wasm/emcc smoke: skipped (need emcc + node) ===\n'; fi; \
	rm -f "$$wbld"; \
	printf '\n=== embedding example (examples/embed, C host #includes alcove.c) ===\n'; \
	ebin=/tmp/alcove-embed.$$$$; \
	if $(CC) $(SAFE_FLAGS) -O2 $(MARCH) -I. -o "$$ebin" examples/embed/host.c -lm >"$$bld" 2>&1; then \
	  if "$$ebin" | grep -q '(\* from-c 10) \[from-c=42\] => 420'; then \
	    echo "  OK — embed host built and ran"; \
	  else echo "  EMBED RAN WRONG:"; "$$ebin" | sed 's/^/    /'; ok=0; fi; \
	else echo "  EMBED BUILD FAILED:"; sed 's/^/    /' "$$bld"; ok=0; fi; \
	rm -f "$$ebin"; \
	printf '\n=== native module (examples/embed/nativemod.$(NATIVE_EXT) via require) ===\n'; \
	if [ "$(FFI_OK)" = yes ]; then \
	  nso=/tmp/alcove-nativemod.$$$$.$(NATIVE_EXT); \
	  if $(CC) $(SAFE_FLAGS) -O2 $(MARCH) $(MODULE_LDFLAGS) -I. -o "$$nso" examples/embed/nativemod.c >"$$bld" 2>&1 \
	     && ./alcove --noload -e "(require \"$$nso\") (prn (nm/add 20 22))" 2>/dev/null | grep -q 42; then \
	    echo "  OK — native module built, required, and called"; \
	  else echo "  NATIVE MODULE FAILED:"; sed 's/^/    /' "$$bld"; ok=0; fi; \
	  rm -f "$$nso"; \
	else echo "  skipped (no libffi → alcove not built -rdynamic)"; fi; \
	printf '\n=== custom type persistence (savedb a foreign object, reload in a fresh process) ===\n'; \
	if [ "$(FFI_OK)" = yes ]; then \
	  cso=/tmp/alcove-ct.$$$$.$(NATIVE_EXT); cdb=/tmp/alcove-ctdb.$$$$; \
	  if $(CC) $(SAFE_FLAGS) -O2 $(MARCH) $(MODULE_LDFLAGS) -I. -o "$$cso" examples/embed/nativemod.c >"$$bld" 2>&1 \
	     && ./alcove --noload -e "(require \"$$cso\") (= c (nm/counter 41)) (nm/inc! c) (persist (quote c)) (savedb \"$$cdb\")" >/dev/null 2>&1 \
	     && ./alcove --noload -e "(loaddb \"$$cdb\") (prn (nm/get c))" 2>/dev/null | grep -q 42; then \
	    echo "  OK — foreign object persisted + auto-reloaded its module on load"; \
	  else echo "  CUSTOM TYPE PERSIST FAILED:"; sed 's/^/    /' "$$bld"; ok=0; fi; \
	  rm -f "$$cso" "$$cdb"; \
	else echo "  skipped (no libffi)"; fi; \
	printf '\n=== script exit code (file/-e error → non-zero) ===\n'; \
	./alcove --noload -e '(prn (+ 1 2))' >/dev/null 2>&1; gc=$$?; \
	./alcove --noload -e '(this_is_unbound)'   >/dev/null 2>&1; bc=$$?; \
	if [ $$gc -eq 0 ] && [ $$bc -ne 0 ]; then echo "  OK — good exits 0, error exits $$bc"; \
	else echo "  EXIT-CODE WRONG: good=$$gc (want 0), error=$$bc (want nonzero)"; ok=0; fi; \
	printf '\n=== error caret (source line + ^ under the offending form) ===\n'; \
	cf=$$(printf '(println 1)\n(+ 2 undefined_caret_zz)\n' | ./alcove --noload /dev/stdin 2>&1 | sed 's/\x1b\[[0-9;]*m//g'); \
	ce=$$(./alcove --noload -e '(+ 2 undefined_caret_zz)' 2>&1 | sed 's/\x1b\[[0-9;]*m//g'); \
	if echo "$$cf" | grep -q ':2:' && echo "$$cf" | grep -qF '(+ 2 undefined_caret_zz)' && echo "$$cf" | grep -qE '^[[:space:]]*\^' \
	   && echo "$$ce" | grep -qF '(+ 2 undefined_caret_zz)' && echo "$$ce" | grep -qE '^[[:space:]]*\^'; then \
	  echo "  OK — file error shows src:line + caret, -e shows source line + caret"; \
	else echo "  CARET MISSING:"; echo "$$cf" | sed 's/^/    /'; echo "    --"; echo "$$ce" | sed 's/^/    /'; ok=0; fi; \
	printf '\n=== precise error location (compiled fn: inner line, not the call site) ===\n'; \
	pl=$$(printf '(def toto (x)\n  (/ 1 x))\n(def test (x)\n  (toto (* x 2)))\n(test 0.)\n' | ./alcove --noload /dev/stdin 2>&1 | sed 's/\x1b\[[0-9;]*m//g'); \
	if echo "$$pl" | grep -q ':2:' && echo "$$pl" | grep -qF '(/ 1 x)'; then \
	  echo "  OK — error points at (/ 1 x) on line 2 (the bytecode pc->line table), not the line-5 call"; \
	else echo "  PRECISE LOC WRONG:"; echo "$$pl" | sed 's/^/    /'; ok=0; fi; \
	printf '\n=== sandbox: --safe + RESP client-callback isolation (tools/test_safe.sh) ===\n'; \
	so=$$(sh tools/test_safe.sh 2>&1); \
	if echo "$$so" | tail -1 | grep -q "SAFE: OK"; then \
	  echo "  OK — host-escape refused via direct/apply/map/compiled + RESP callbacks; def/compute allowed; allow-unsafe grants"; \
	else echo "  SANDBOX FAILED:"; echo "$$so" | sed 's/^/    /'; ok=0; fi; \
	printf '\n=== error backtrace (call chain on uncaught error) ===\n'; \
	bt=$$(printf '(def c (x) (/ x 0))\n(def b (x) (+ 1 (c x)))\n(def a () (+ 1 (b 5)))\n(a)\n' | ./alcove --noload /dev/stdin 2>&1 | sed 's/\x1b\[[0-9;]*m//g'); \
	if echo "$$bt" | grep -qi 'backtrace' && echo "$$bt" | grep -qE '^[[:space:]]+c$$' && echo "$$bt" | grep -qE '^[[:space:]]+b$$' && echo "$$bt" | grep -qE '^[[:space:]]+a$$'; then \
	  echo "  OK — uncaught error prints the c<-b<-a call chain"; \
	else echo "  BACKTRACE WRONG:"; echo "$$bt" | sed 's/^/    /'; ok=0; fi; \
	btd=/tmp/alcove-bt.$$$$; mkdir -p "$$btd"; \
	printf '(prn "init ran")\n' > "$$btd/.init.alc"; \
	bt2=$$(printf '(def c (x) (/ x 0))\n(def b (x) (+ 1 (c x)))\n(def a () (+ 1 (b 5)))\n(a)\n' | (cd "$$btd" && exec "$(CURDIR)/alcove" --noload /dev/stdin) 2>&1 | sed 's/\x1b\[[0-9;]*m//g'); \
	rm -rf "$$btd"; \
	if echo "$$bt2" | grep -qi 'backtrace' && echo "$$bt2" | grep -qE '^[[:space:]]+c$$'; then \
	  echo "  OK — backtrace survives a loaded .init.alc (reader EOF must not poison the capture)"; \
	else echo "  BACKTRACE-WITH-INIT WRONG:"; echo "$$bt2" | sed 's/^/    /'; ok=0; fi; \
	$(MAKE) -s jit >/dev/null 2>&1; $(MAKE) -s adder >/dev/null 2>&1; \
	printf '\n=== script identity (*args* + shebang, both dialects) ===\n'; \
	sd=/tmp/alcove-args.$$$$.d; mkdir -p "$$sd"; \
	sa="$$sd/s.alc"; printf '#!/usr/bin/env alcove\n(pr (length *args*) ":" (first *args*))\n' > "$$sa"; \
	out=$$( (cd "$$sd" && exec "$(CURDIR)/alcove" --noload "$$sa" one two) 2>&1 | sed 's/\x1b\[[0-9;]*m//g'); \
	if [ "$$out" = "2:one" ]; then echo "  OK — alcove script receives *args*, shebang line ignored"; \
	else echo "  SCRIPT ARGS WRONG (alcove): '$$out'"; ok=0; fi; \
	sb="$$sd/s.adr"; printf '#!/usr/bin/env adder\npr (length *args*) ":" (first *args*)\n' > "$$sb"; \
	out=$$( (cd "$$sd" && exec "$(CURDIR)/adder" --noload "$$sb" uno dos) 2>&1 | sed 's/\x1b\[[0-9;]*m//g'); \
	if [ "$$out" = "2:uno" ]; then echo "  OK — adder script receives *args*, shebang line ignored"; \
	else echo "  SCRIPT ARGS WRONG (adder): '$$out'"; ok=0; fi; \
	rm -rf "$$sd"; \
	printf '\n=== LSP server (tools/lsp.alc over real JSON-RPC framing) ===\n'; \
	if command -v python3 >/dev/null 2>&1; then \
	  lout=$$(python3 tools/test_lsp.py 2>&1 | tail -1); \
	  if [ "$$lout" = "LSP: OK" ]; then \
	    echo "  OK — initialize/diagnostics/completion/hover, both dialects"; \
	  else echo "  LSP FAILED: $$lout"; ok=0; fi; \
	else echo "  (skipped — no python3)"; fi; \
	printf '\n=== debugger (--debug / (break): breakpoints, bt, locals, p, step/next) ===\n'; \
	if command -v python3 >/dev/null 2>&1; then \
	  dout=$$(python3 tools/test_debug.py 2>&1 | tail -1); \
	  if [ "$$dout" = "DEBUG: OK" ]; then \
	    echo "  OK — fn/line breakpoints, full backtrace, frame/locals/p, step+next, (break)"; \
	  else echo "  DEBUG FAILED: $$dout"; ok=0; fi; \
	else echo "  (skipped — no python3)"; fi; \
	printf '\n=== programmable REPL (lib/repl.adr hooks over a real pty) ===\n'; \
	if command -v python3 >/dev/null 2>&1; then \
	  rout=$$(ALCOVE_PATH=lib python3 tools/test_repl_pty.py 2>&1 | tail -1); \
	  if [ "$$rout" = "REPL: OK" ]; then \
	    echo "  OK — prompt/output-hook/transcript/errlog/notebook/bind-key/fallback, both dialects"; \
	  else echo "  REPL FAILED: $$rout"; ok=0; fi; \
	else echo "  (skipped — no python3)"; fi; \
	printf '\n=== adder error caret (maps generated line back to Adder source) ===\n'; \
	ac=$$(printf '= x 1\n\n+ x undefined_adr_zz\n' | ./adder --noload /dev/stdin 2>&1 | sed 's/\x1b\[[0-9;]*m//g'); \
	if echo "$$ac" | grep -q ':3:' && echo "$$ac" | grep -qF '+ x undefined_adr_zz' && echo "$$ac" | grep -qE '^[[:space:]]*\^'; then \
	  echo "  OK — .adr error reports the Adder source line (3) + caret, not the generated line"; \
	else echo "  ADDER CARET WRONG:"; echo "$$ac" | sed 's/^/    /'; ok=0; fi; \
	printf '\n=== AST-vs-VM equivalence sweep (tools/equiv_sweep.alc) ===\n'; \
	es=$$(./alcove --noload tools/equiv_sweep.alc 2>&1 | sed 's/\x1b\[[0-9;]*m//g'); \
	echo "$$es" | grep "EQUIV SWEEP:" | grep -vE "OK|FAIL" | sed 's/^/  /'; \
	if echo "$$es" | grep -q "EQUIV SWEEP: OK"; then echo "  OK — AST and bytecode VM agree on every compiled form"; \
	else echo "  EQUIV SWEEP MISMATCH:"; echo "$$es" | grep MISMATCH | sed 's/^/    /'; ok=0; fi; \
	printf '\n=== recursion/closure compiled-vs-interpreted differential ===\n'; \
	rc=/tmp/alcove-recur-c.$$$$; ri=/tmp/alcove-recur-i.$$$$; \
	./alcove --noload tools/recur_battery.alc 2>&1 | sed 's/\x1b\[[0-9;]*m//g' >"$$rc"; \
	./alcove --interpret --noload tools/recur_battery.alc 2>&1 | sed 's/\x1b\[[0-9;]*m//g' >"$$ri"; \
	if diff -q "$$rc" "$$ri" >/dev/null && grep -q "RECUR BATTERY DONE" "$$rc"; then \
	  echo "  OK — bytecode VM == AST tree-walker on $$(grep -c "	" "$$rc") recursion/closure results"; \
	else echo "  RECUR DIFFERENTIAL MISMATCH:"; diff "$$rc" "$$ri" | sed 's/^/    /' | head -20; ok=0; fi; \
	rm -f "$$rc" "$$ri"; \
	printf '\n=== top-level error spew gate (uncounted errors must not creep in) ===\n'; \
	printf '%s\n' \
	  "  The assert/* helpers count failures, but a top-level form that ERRORS" \
	  "  OUTSIDE an assert prints to stderr and is NOT counted (exactly how the" \
	  "  old assert-as-function bug hid ~40 broken tests). A known set of" \
	  "  intentional error-recovery tests (div0, arity, non-numeric vec, ...)" \
	  "  spews to stderr by design; these baselines pin that set. A regression" \
	  "  that adds NEW uncounted error output trips this gate even though" \
	  "  'TEST RESULT' still says 0 failed. If you intentionally add/remove a" \
	  "  recovery test, update the baseline below."; \
	ALC_BASE=29; ADR_BASE=30; \
	ae=$$(./alcove --noload test.alc 2>&1 1>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -cE '^test\.alc:[0-9]+:'); \
	if [ "$$ae" -le "$$ALC_BASE" ]; then echo "  OK — alcove top-level errors: $$ae (baseline $$ALC_BASE)"; \
	else echo "  ALCOVE STDERR REGRESSION: $$ae top-level errors > baseline $$ALC_BASE — a new uncounted error crept in:"; \
	  ./alcove --noload test.alc 2>&1 1>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -E '^test\.alc:[0-9]+:' | sed 's/^/    /' | tail -40; ok=0; fi; \
	de=$$(./adder --noload test.adr 2>&1 1>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -cE '^test\.adr:[0-9]+:'); \
	if [ "$$de" -le "$$ADR_BASE" ]; then echo "  OK — adder top-level errors: $$de (baseline $$ADR_BASE)"; \
	else echo "  ADDER STDERR REGRESSION: $$de top-level errors > baseline $$ADR_BASE — a new uncounted error crept in:"; \
	  ./adder --noload test.adr 2>&1 1>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -E '^test\.adr:[0-9]+:' | sed 's/^/    /' | tail -40; ok=0; fi; \
	printf '\n'; \
	if [ $$ok -eq 1 ]; then echo "==> ALL VARIANTS PASSED"; \
	else echo "==> VARIANT FAILURES (see above)"; exit 1; fi
# Embedding demo: a plain C host that #includes alcove.c with ALCOVE_NO_MAIN and
# drives the engine via alcove_init / alcove_eval_string / alcove_register_cmd.
embed-example:
	$(CC) $(SAFE_FLAGS) -O2 $(MARCH) -I. -o examples/embed/host examples/embed/host.c -lm
	@./examples/embed/host
# Native-module demo: compile examples/embed/nativemod.c into a .so and load it
# with (require ...). Needs an FFI-enabled alcove (built -rdynamic so the module
# resolves alcove_register_cmd / make_* at dlopen). Builds the JIT alcove first.
native-module-example: jit
	$(CC) $(SAFE_FLAGS) -O2 $(MARCH) $(MODULE_LDFLAGS) -I. -o examples/embed/nativemod.$(NATIVE_EXT) examples/embed/nativemod.c
	@./alcove --noload -e '(require "examples/embed/nativemod.$(NATIVE_EXT)") (prn (nm/add 20 22)) (prn (nm/scale 4)) (prn (nm/greet "world"))'
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

# Build the wasm modules and smoke-test them under node, asserting results
# match native ./alcove output (catches wasm/clang-backend miscompiles that the
# native test-all can't — the fixnum-tag / vec-ref class of bug). NODE is
# overridable: under setup-emsdk in CI, bare `node` on PATH isn't directly
# runnable, so we pass NODE="$EMSDK_NODE".
NODE ?= node
test-web: web
	$(NODE) web/test_web.js

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
	  -sSTACK_SIZE=8388608 \
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
	  -sSTACK_SIZE=8388608 \
	  -o web/adder-core.js adder.c -lm

# Code-coverage SIGNAL for the hot path (evaluator / VM / JIT + core fragments).
# alcove.c is one TU that #includes every fragment, so a single gcov-instrumented
# build + one gcov pass gives per-fragment line coverage. Exercises the whole
# test.alc suite plus the differential gates (equiv_sweep, recur_battery), then
# reports the trust-critical fragments and a weighted aggregate. Answers "are we
# testing the evaluator enough?" — it is a SIGNAL, never a pass/fail gate (a
# threshold would just be flaky). Writes .gcov files for the CI artifact.
# Needs gcov (ships with gcc); no gcovr/lcov dependency.
coverage:
	rm -f *.gcno *.gcda *.gcov alcove_cov
	$(CC) -Wall -W $(SAFE_FLAGS) -O0 -g --coverage -fprofile-update=atomic \
	  $(MARCH) $(JIT_FLAGS) -o alcove_cov alcove.c \
	  $(RL_FLAGS) $(FFI_FLAGS) -lm $(FFI_LIBS) $(RL_LIBS)
	-./alcove_cov --noload test.alc >/dev/null 2>&1
	-./alcove_cov --noload tools/equiv_sweep.alc >/dev/null 2>&1
	-./alcove_cov --noload tools/recur_battery.alc >/dev/null 2>&1
	@gcov -o . alcove_cov-alcove >/dev/null 2>&1 || true
	@echo "==> code coverage — hot path (evaluator / VM / JIT / core fragments):"
	@sh tools/cov_report.sh alcove_cov-alcove
	@echo "    (signal only, not a gate; .gcov files written for the CI artifact)"
	@rm -f alcove_cov

clean:
	rm -f alcove adder mpsc_test mpsc_test_tsan alcove_cov
	rm -f *.gcno *.gcda *.gcov
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

# ---------------------------------------------------------------------------
# Parser / tokenizer tests.
#   make parser-test  — unit tests + bounded deterministic fuzz, under ASan
#   make fuzz         — coverage-guided libFuzzer run (needs clang)
# parser_test.c #includes alcove.c, so it sees the reader internals directly
# and feeds inputs through fmemopen exactly like the real -e / web paths.
parser-test:
	$(CC) -Wall -W $(SAFE_FLAGS) -g -O1 -fsanitize=address,undefined \
	  $(JIT_FLAGS) -o parser_test parser_test.c $(FFI_FLAGS) -lm $(FFI_LIBS)
	./parser_test

# Coverage-guided fuzzing. clang + libFuzzer; runs 60s by default. Override:
#   make fuzz FUZZ_ARGS='-max_total_time=600 corpus/'
# libFuzzer ships with clang only; auto-pick an installed clang (the distro may
# only have versioned binaries like clang-19). Override with `make fuzz CLANG=...`.
FUZZ_ARGS ?= -max_total_time=60
CLANG ?= $(shell command -v clang || command -v clang-19 || command -v clang-18 || command -v clang-17)
fuzz:
	@[ -n "$(CLANG)" ] || { echo "no clang found — install clang for libFuzzer"; exit 1; }
	$(CLANG) -DPARSER_LIBFUZZER -g -O1 -fsanitize=fuzzer,address,undefined \
	  $(JIT_FLAGS) -o parser_fuzz parser_test.c $(FFI_FLAGS) -lm $(FFI_LIBS)
	./parser_fuzz $(FUZZ_ARGS)

# JIT differential fuzzer. Generates randomized counter/leaf/float-acc shapes,
# runs each under a JIT build and a non-JIT build, and asserts (1) results are
# byte-identical jit==VM (msgpack-encoded, type+bit exact) and (2) every
# shape that should JIT does (coverage map). Catches both miscompiles and
# silent coverage regressions like the forsum/for_loop_inc drift. Builds its
# own two binaries; pass options via JITFUZZ_ARGS (e.g. --seed N --count N).
# The always-on guard is the self-gating JIT-coverage block in test.alc (run
# by every test-all variant); this is the deeper opt-in differential check.
jit-fuzz:
	python3 jit_fuzz.py $(JITFUZZ_ARGS)

# Generative whole-program differential fuzzer for the evaluator/VM/JIT. The
# reader/transpiler/msgpack are fuzzed, but the eval path only had equiv_sweep's
# fixed form-set and jit_fuzz's narrow loop shapes. eval_fuzz.py generates a
# large batch of random TERMINATING, PURE programs across the full breadth of
# constructs (arithmetic, the numeric tower, conditionals, let, lists, vectors,
# strings, HOFs, bounded recursion, quasiquote) and asserts each is
# byte-identical AST==VM (msgpack, type+bit exact), under nojit + jit + an
# ASan/UBSan build (memory-safe, no crash). Found the length vector/blob/cdr-nil
# AST divergence and the numeric-tower error-path use-after-free. Tune via
# EVALFUZZ_ARGS (e.g. --seed N --count N --no-asan).
eval-fuzz:
	python3 tools/eval_fuzz.py $(EVALFUZZ_ARGS)

# OOM-recovery gate: a failed allocation mid-computation must abort the current
# top-level form with a surfaced out-of-memory error and leave the PROCESS +
# engine usable (the next form runs), instead of exit()'ing or segfaulting.
# Exercised via the unsafe (alloc-fail-after N) fault-injection builtin, under a
# normal build AND an ASan/UBSan build (the longjmp recovery must not corrupt
# state). See tools/oom_test.sh.
oom-test:
	sh tools/oom_test.sh

# End-to-end multi-reactor RESP server under ThreadSanitizer: a 4-reactor server
# driven by concurrent clients on a shared key (+ distinct keys + a redis-defcmd
# callback) must be data-race-clean. Complements mpsc-test-tsan (the queue
# primitive alone). Skips cleanly if redis-cli or libtsan is unavailable. See
# tools/resp_tsan.sh.
resp-tsan:
	sh tools/resp_tsan.sh

# Observability gate: leveled logfmt logging (assert one logfmt line per emit on
# stderr + below-threshold silence) and RESP server auto-instrumentation metrics
# (resp.connections/.commands/.errors > 0 under driven traffic, queried via a
# metrics build's (metric …)). The metrics half builds with -DALCOVE_METRICS;
# the logging half is in the default build. Skips the server half if redis-cli is
# absent. See tools/obs_test.sh.
obs-test:
	sh tools/obs_test.sh

# Adder transpiler (adr.h) tests. adr.h is self-contained string->string, so
# this links nothing else. unit tests + bounded deterministic fuzz, under ASan.
adr-test:
	$(CC) -Wall -W $(SAFE_FLAGS) -g -O1 -fsanitize=address,undefined \
	  -I. -o adr_test adr_test.c
	./adr_test

# HAMT (hamt.h) unit + fuzz tests; includes alcove.c for the hamt_* internals.
hamt-test:
	$(CC) -Wall -W $(SAFE_FLAGS) -g -O1 -fsanitize=address,undefined \
	  $(JIT_FLAGS) -o hamt_test hamt_test.c $(FFI_FLAGS) -lm $(FFI_LIBS)
	./hamt_test

# dict_t (dict.h) hash-table unit + fuzz tests; includes alcove.c.
dict-test:
	$(CC) -Wall -W $(SAFE_FLAGS) -g -O1 -fsanitize=address,undefined \
	  $(JIT_FLAGS) -o dict_test dict_test.c $(FFI_FLAGS) -lm $(FFI_LIBS)
	./dict_test

# blob (blob.h) binary-safety unit + fuzz tests; includes alcove.c.
blob-test:
	$(CC) -Wall -W $(SAFE_FLAGS) -g -O1 -fsanitize=address,undefined \
	  $(JIT_FLAGS) -o blob_test blob_test.c $(FFI_FLAGS) -lm $(FFI_LIBS)
	./blob_test

# vector (vector.h) unit + fuzz tests; includes alcove.c.
vector-test:
	$(CC) -Wall -W $(SAFE_FLAGS) -g -O1 -fsanitize=address,undefined \
	  $(JIT_FLAGS) -o vector_test vector_test.c $(FFI_FLAGS) -lm $(FFI_LIBS)
	./vector_test

# set (set.h) unit + fuzz tests; includes alcove.c.
set-test:
	$(CC) -Wall -W $(SAFE_FLAGS) -g -O1 -fsanitize=address,undefined \
	  $(JIT_FLAGS) -o set_test set_test.c $(FFI_FLAGS) -lm $(FFI_LIBS)
	./set_test

# MsgPack (msgpack.h) round-trip + decoder-fuzz tests; includes alcove.c.
msgpack-test:
	$(CC) -Wall -W $(SAFE_FLAGS) -g -O1 -fsanitize=address,undefined \
	  $(JIT_FLAGS) -o msgpack_test msgpack_test.c $(FFI_FLAGS) -lm $(FFI_LIBS)
	./msgpack_test

# utf8.h is libc-only, so its test links nothing else (the most isolated TU).
utf8-test:
	$(CC) -Wall -W $(SAFE_FLAGS) -g -O1 -fsanitize=address,undefined \
	  -I. -o utf8_test utf8_test.c
	./utf8_test

# Coverage-guided fuzzing of the Adder transpiler (clang + libFuzzer).
adr-fuzz:
	@[ -n "$(CLANG)" ] || { echo "no clang found — install clang for libFuzzer"; exit 1; }
	$(CLANG) -DADR_LIBFUZZER -g -O1 -fsanitize=fuzzer,address,undefined \
	  -I. -o adr_fuzz adr_test.c
	./adr_fuzz $(FUZZ_ARGS)

# Coverage-guided libFuzzer over the MsgPack binary decoder (untrusted bytes).
msgpack-fuzz:
	@[ -n "$(CLANG)" ] || { echo "no clang found"; exit 1; }
	$(CLANG) -DMSGPACK_LIBFUZZER -g -O1 -fsanitize=fuzzer,address,undefined \
	  $(JIT_FLAGS) -o msgpack_fuzz msgpack_test.c $(FFI_FLAGS) -lm $(FFI_LIBS)
	./msgpack_fuzz $(FUZZ_ARGS)

# JSON codec: unit round-trips + random-buffer decoder fuzz under ASan/UBSan,
# plus a coverage-guided libFuzzer target (json.h parses untrusted text).
json-test:
	$(CC) -Wall -W $(SAFE_FLAGS) -g -O1 -fsanitize=address,undefined \
	  $(JIT_FLAGS) -o json_test json_test.c $(FFI_FLAGS) -lm $(FFI_LIBS)
	./json_test

json-fuzz:
	@[ -n "$(CLANG)" ] || { echo "no clang found"; exit 1; }
	$(CLANG) -DJSON_LIBFUZZER -g -O1 -fsanitize=fuzzer,address,undefined \
	  $(JIT_FLAGS) -o json_fuzz json_test.c $(FFI_FLAGS) -lm $(FFI_LIBS)
	./json_fuzz $(FUZZ_ARGS)

fmt:
	$(FMT) -i $(FMT_FILES)

fmt-check:
	$(FMT) --dry-run --Werror $(FMT_FILES)

# Analyze both translation units: alcove.c (the core) and adder.c (which adds
# ALCOVE_ALS + #includes adr.h), so the include-only headers — adr.h in
# particular — are checked in real caller context, not standalone.
tidy:
	$(TIDY) alcove.c -- $(TIDY_CFLAGS)
	$(TIDY) adder.c  -- -DALCOVE_ALS=1 $(TIDY_CFLAGS)

hooks:
	git config core.hooksPath .githooks
	@echo "pre-commit hook installed (core.hooksPath=.githooks)."
	@echo "It formats + lints only the lines you stage."

.PHONY: parser speed nojit mono jit jit-mono adder embed-example native-module-example als alcoves gen-test-adr gen-web-battery jit-fuzz eval-fuzz oom-test resp-tsan obs-test adfmt-test coverage alcove-with-metrics adder-with-metrics adfmt install uninstall deps test test-asan test-all benchmark benchmark-mlp benchmark-mono benchmark-jit benchmark-compare mpsc-test mpsc-test-tsan web clean fmt fmt-check tidy parser-test fuzz adr-test adr-fuzz msgpack-fuzz hamt-test dict-test blob-test set-test vector-test msgpack-test utf8-test test-web hooks
