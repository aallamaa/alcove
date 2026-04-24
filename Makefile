# Auto-detect host arch. JIT supports arm64 and amd64; on anything else
# the jit/jit-mono targets fall back to a non-JIT build.
ARCH         := $(shell uname -m)
JIT_OK       := $(filter aarch64 arm64 x86_64 amd64,$(ARCH))
ifneq ($(JIT_OK),)
  JIT_FLAGS  := -DALCOVE_JIT=1
else
  JIT_FLAGS  :=
endif

# Default goal: JIT release build (auto-arch). Explicit opt-outs:
#   make nojit    — release without JIT (atomic refcounts)
#   make parser   — debug build (-g3, no JIT)
#   make speed    — same as nojit (kept for back-compat)
.DEFAULT_GOAL := jit

parser:
	$(CC) -Wall -W  -g3 -o alcove  alcove.c -lm
speed:
	$(CC) -Wall -W  -O3 -o alcove  alcove.c -lm
# Explicit non-JIT release build — alias of `speed`. Use this when you
# want to opt out of the JIT path (e.g. for A/B comparison).
nojit: speed
# Single-threaded build: plain ++/-- refcounts instead of __sync atomics.
# Correctness is identical as long as nothing threads the interpreter.
mono:
	$(CC) -Wall -W  -O3 -DALCOVE_SINGLE_THREADED=1 -o alcove alcove.c -lm
# JIT build. Auto-picks the arm64 or amd64 backend based on `uname -m`.
# On unsupported architectures, JIT_FLAGS is empty and you get a plain
# bytecode build (with a warning).
jit:
ifeq ($(JIT_OK),)
	@echo "warning: no JIT backend for $(ARCH); building bytecode-only."
endif
	$(CC) -Wall -W  -O3 $(JIT_FLAGS) -o alcove  alcove.c -lm
# JIT + single-threaded refcount — the fastest build. Pair as long as
# nothing threads the interpreter.
jit-mono:
ifeq ($(JIT_OK),)
	@echo "warning: no JIT backend for $(ARCH); building bytecode-only."
endif
	$(CC) -Wall -W  -O3 $(JIT_FLAGS) -DALCOVE_SINGLE_THREADED=1 -o alcove alcove.c -lm
# -Os removed
test: parser
	./alcove test.alc
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

.PHONY: parser speed nojit mono jit jit-mono test benchmark benchmark-mono benchmark-jit benchmark-compare clean
