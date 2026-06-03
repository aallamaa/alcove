#include <stdio.h>
static int safe(int col, int *qs, int row) {
  for (int o = 0; o < row; o++) { int placed = qs[o]; int d = row - o;
    if (col == placed || col - d == placed || col + d == placed) return 0; }
  return 1;
}
static long long solve(int n, int row, int *qs) {
  if (row >= n) return 1;
  long long count = 0;
  for (int col = 0; col < n; col++)
    if (safe(col, qs, row)) { qs[row] = col; count += solve(n, row + 1, qs); }
  return count;
}
int main(void) { volatile int n = 10; int qs[64] = {0}; printf("%lld\n", solve(n, 0, qs)); return 0; }
