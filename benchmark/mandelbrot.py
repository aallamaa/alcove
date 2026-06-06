# mandelbrot.py — escape-count sum twin of mandelbrot.alc.
def mb(cr, ci):
    zr = 0.0
    zi = 0.0
    i = 0
    while i < 255 and (zr * zr + zi * zi) <= 4.0:
        nzr = (zr * zr - zi * zi) + cr
        nzi = (2.0 * (zr * zi)) + ci
        zr = nzr
        zi = nzi
        i += 1
    return i


w, h, s = 300, 200, 0
for py in range(h):
    for px in range(w):
        cr = -2.5 + (3.5 / w) * px
        ci = -1.25 + (2.5 / h) * py
        s += mb(cr, ci)
print(s)
