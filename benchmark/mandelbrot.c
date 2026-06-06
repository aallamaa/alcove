/* mandelbrot.c — escape-count sum twin of mandelbrot.alc; identical doubles. */
#include <stdio.h>

static long mb(double cr, double ci) {
  double zr = 0.0, zi = 0.0;
  long i = 0;
  while (i < 255 && (zr * zr + zi * zi) <= 4.0) {
    double nzr = (zr * zr - zi * zi) + cr;
    double nzi = (2.0 * (zr * zi)) + ci;
    zr = nzr;
    zi = nzi;
    i++;
  }
  return i;
}

int main(void) {
  long w = 300, h = 200, s = 0;
  for (long py = 0; py < h; py++)
    for (long px = 0; px < w; px++) {
      double cr = -2.5 + (3.5 / (double)w) * (double)px;
      double ci = -1.25 + (2.5 / (double)h) * (double)py;
      s += mb(cr, ci);
    }
  printf("%ld\n", s);
  return 0;
}
