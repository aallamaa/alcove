#include <stdio.h>
static long long tak(long long x, long long y, long long z) {
  return !(y < x) ? z : tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y));
}
int main(void) { volatile int x = 24, y = 16, z = 8; printf("%lld\n", tak(x, y, z)); return 0; }
