#include <stdio.h>
static volatile long long sink = 0;
static long long countdown(long long n) { return n > 0 ? countdown(n - 1) : n; }
static long long loop(long long k) { while (k > 0) { sink = countdown(1000); k--; } return k; }
int main(void) { volatile int k = 10000; printf("%lld\n", loop(k)); return 0; }
