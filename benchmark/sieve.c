#include <stdio.h>
#include <stdlib.h>
typedef struct N { long long v; struct N *next; } N;
static int is_prime_given(N *acc, long long i) {
  for (; acc; acc = acc->next) if (i % acc->v == 0) return 0;
  return 1;
}
int main(void) {
  volatile int n = 5000; N *acc = NULL;
  for (long long i = 2; i <= n; i++)
    if (is_prime_given(acc, i)) { N *x = malloc(sizeof *x); x->v = i; x->next = acc; acc = x; }
  int cnt = 0; for (N *p = acc; p; p = p->next) cnt++;
  printf("%d\n", cnt); return 0;
}
