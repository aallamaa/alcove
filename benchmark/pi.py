# pi.py — π via a telescoped Leibniz series; the Python twin of pi.alc.
acc = 0.0
k = 1
while k < 40000000:
    acc += 4.0 / k - 4.0 / (k + 2)
    k += 4
print(f"{acc:g}")
