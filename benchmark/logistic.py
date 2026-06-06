# logistic.py — logistic map twin of logistic.alc.
x = 0.3
for n in range(50000000):
    x = 2.5 * (x * (1.0 - x))
print(f"{x:g}")
