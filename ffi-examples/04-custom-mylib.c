/* Minimal C library to demo FFI to user-built shared objects.
   Build with:
     cc -shared -fPIC -O2 -o libmylib.so 04-custom-mylib.c
   Then call from alcove (see 04-custom-mylib.alc). */

#include <stddef.h>
#include <stdio.h>

/* Simple add. */
long mylib_add(long a, long b) { return a + b; }

/* Fibonacci — same shape as alcove's benchmark, but in C. */
long mylib_fib(long n) {
  if (n < 2) return n;
  return mylib_fib(n - 1) + mylib_fib(n - 2);
}

/* String length (same as libc strlen, just demoing user functions). */
long mylib_strlen(const char *s) {
  long n = 0;
  while (*s++) n++;
  return n;
}

/* Counter with state — module-private, mutable across calls. */
static long mylib_counter = 0;
long mylib_inc(void)  { return ++mylib_counter; }
long mylib_get(void)  { return mylib_counter; }
void mylib_reset(void){ mylib_counter = 0; }

/* Returns a static string — owned by the lib, alcove will copy it. */
const char *mylib_greet(void) { return "hello from C!"; }
