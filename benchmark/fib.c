/* C port of fib.alc / fib.py — native baseline for the benchmark suite. */
#include <stdio.h>
static long long fib(long long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
int main(void) { volatile int n = 33; printf("%lld\n", fib(n)); return 0; }
