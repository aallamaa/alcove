parser:
	$(CC) -Wall -W  -g3 -o alcove  alcove.c -lm
speed:
	$(CC) -Wall -W  -O3 -o alcove  alcove.c -lm
# Single-threaded build: plain ++/-- refcounts instead of __sync atomics.
# Correctness is identical as long as nothing threads the interpreter.
mono:
	$(CC) -Wall -W  -O3 -DALCOVE_SINGLE_THREADED=1 -o alcove alcove.c -lm
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

.PHONY: parser speed mono test benchmark benchmark-mono benchmark-compare clean
