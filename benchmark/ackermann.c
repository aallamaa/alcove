#include <stdio.h>
static long long ack(long long m, long long n) {
  if (m == 0) return n + 1;
  if (n == 0) return ack(m - 1, 1);
  return ack(m - 1, ack(m, n - 1));
}
int main(void) { volatile int m = 3, n = 9; printf("%lld\n", ack(m, n)); return 0; }
