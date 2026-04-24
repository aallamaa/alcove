.PHONY: all run clean

all: libmylib.so

libmylib.so: 04-custom-mylib.c
	cc -shared -fPIC -O2 -o $@ $<

run: libmylib.so
	@echo "=== 01-libm-math.alc ==="
	@../alcove --noload 01-libm-math.alc
	@echo "=== 02-libc-strings.alc ==="
	@../alcove --noload 02-libc-strings.alc
	@echo "=== 03-time-syscalls.alc ==="
	@../alcove --noload 03-time-syscalls.alc
	@echo "=== 04-custom-mylib.alc ==="
	@../alcove --noload 04-custom-mylib.alc

clean:
	rm -f libmylib.so
