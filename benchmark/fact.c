#include <stdio.h>
static volatile long long sink = 0;
static long long fact(long long n) { return n < 2 ? 1 : n * fact(n - 1); }
static long long loop(long long k) { while (k > 0) { sink = fact(19); k--; } return k; }
int main(void) { volatile int k = 100000; printf("%lld\n", loop(k)); return 0; }
