/* logistic.c — logistic map twin of logistic.alc; same recurrence + order, so
   the IEEE double result is identical. */
#include <stdio.h>

int main(void) {
  double x = 0.3;
  for (long n = 0; n < 50000000L; n++)
    x = 2.5 * (x * (1.0 - x));
  printf("%g\n", x);
  return 0;
}
