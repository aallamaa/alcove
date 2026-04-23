parser:
	$(CC) -Wall -W  -g3 -o alcove  alcove.c -lm
speed:
	$(CC) -Wall -W  -O3 -o alcove  alcove.c -lm
# Single-threaded build: plain ++/-- refcounts instead of __sync atomics.
# Correctness is identical as long as nothing threads the interpreter.
mono:
	$(CC) -Wall -W  -O3 -DALCOVE_SINGLE_THREADED=1 -o alcove alcove.c -lm
# arm64-only JIT (proof-of-concept). Compiles a narrow set of lambda
# shapes (constant return, (+ n K), (- n K) for fixnum K in u12 range)
# directly to native code. Falls back to bytecode for everything else.
jit:
	$(CC) -Wall -W  -O3 -DALCOVE_JIT=1 -o alcove alcove.c -lm
# -Os removed
test: parser
	./alcove test.alc
benchmark: speed
	./benchmark/run.sh
# Build mono and run the bench suite against it.
benchmark-mono: mono
	./benchmark/run.sh
# Rebuild both variants and compare numbers side-by-side.
benchmark-compare:
	@./benchmark/compare.sh
clean:
	rm -f alcove

.PHONY: parser speed mono jit test benchmark benchmark-mono benchmark-compare clean
