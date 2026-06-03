#include <stdio.h>
int main(void) { volatile int n = 10000000; long long s = 0;
  for (int i = 1; i <= n; i++) s = s + 1; printf("%lld\n", s); return 0; }
