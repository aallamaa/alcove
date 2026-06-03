#include <stdio.h>
static int safe(int c, int *qs, int len) {
  int offset = 1;
  for (int k = 0; k < len; k++) { int q = qs[k];
    if (c == q || c + offset == q || c - offset == q) return 0; offset++; }
  return 1;
}
static long long solve(int n, int *qs, int len) {
  if (len == n) return 1;
  long long total = 0;
  for (int c = 1; c <= n; c++)
    if (safe(c, qs, len)) {
      int nq[64]; nq[0] = c; for (int k = 0; k < len; k++) nq[k + 1] = qs[k];
      total += solve(n, nq, len + 1);
    }
  return total;
}
int main(void) { volatile int n = 10; int qs[64]; printf("%lld\n", solve(n, qs, 0)); return 0; }
