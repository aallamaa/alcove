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

/* ---- Callback demos: take a C function pointer the caller supplies via
   alcove's (ffi-callback ...). These let C code call back into an alcove
   lambda. See 05-callbacks.alc. ---- */

/* Call fn(a, b) once and return the result. */
long mylib_apply2(long (*fn)(long, long), long a, long b) { return fn(a, b); }

/* Call fn(i) for i in 0..n-1 in a C loop, summing — the shape of any C API
   that drives a user callback (qsort, map, event loops). */
long mylib_sum_map(long (*fn)(long), long n) {
  long s = 0;
  for (long i = 0; i < n; i++) s += fn(i);
  return s;
}

/* Floating-point callback: returns fn(x) + 1.0. */
double mylib_apply_d(double (*fn)(double), double x) { return fn(x) + 1.0; }

/* ---- By-value struct demos (ffi-struct / ffi-pack / ffi-unpack). See
   06-structs.alc. ---- */

typedef struct { double x, y; } mylib_point;

/* Struct passed by value → scalar. */
double mylib_pt_norm2(mylib_point p) { return p.x * p.x + p.y * p.y; }

/* Scalars → struct returned by value. */
mylib_point mylib_pt_make(double x, double y) {
  mylib_point p = {x, y};
  return p;
}

/* Two structs by value → struct by value. */
mylib_point mylib_pt_add(mylib_point a, mylib_point b) {
  mylib_point r = {a.x + b.x, a.y + b.y};
  return r;
}

/* Nested struct (a struct field inside a struct). */
typedef struct { mylib_point a, b; } mylib_seg;
double mylib_seg_len2(mylib_seg s) {
  double dx = s.b.x - s.a.x, dy = s.b.y - s.a.y;
  return dx * dx + dy * dy;
}
