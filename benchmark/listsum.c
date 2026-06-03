#include <stdio.h>
#include <stdlib.h>
typedef struct N { long long v; struct N *next; } N;
int main(void) {
  volatile int n = 100000; N *acc = NULL;
  for (int i = 1; i <= n; i++) { N *x = malloc(sizeof *x); x->v = i; x->next = acc; acc = x; }
  long long s = 0; for (N *p = acc; p; p = p->next) s += p->v;
  printf("%lld\n", s); return 0;
}
