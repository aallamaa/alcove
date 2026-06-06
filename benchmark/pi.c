/* pi.c — π via a telescoped Leibniz series; the native twin of pi.alc.
   Same terms in the same order, so the IEEE double result is identical. */
#include <stdio.h>

int main(void) {
  double acc = 0.0;
  for (long k = 1; k < 40000000L; k += 4)
    acc += 4.0 / (double)k - 4.0 / (double)(k + 2);
  printf("%g\n", acc);
  return 0;
}
