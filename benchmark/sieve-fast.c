#include <stdio.h>
#include <stdlib.h>
int main(void) {
  volatile int N = 100000; char *m = malloc((size_t)N + 1);
  for (int i = 0; i <= N; i++) m[i] = 1;
  for (long long i = 2; i * i <= N; i++)
    if (m[i]) for (long long j = i * i; j <= N; j += i) m[j] = 0;
  int cnt = 0; for (int i = 2; i <= N; i++) if (m[i]) cnt++;
  printf("%d\n", cnt); return 0;
}
