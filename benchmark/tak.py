import sys
sys.setrecursionlimit(20000)

def tak(x, y, z):
    if not (y < x):
        return z
    return tak(tak(x - 1, y, z),
               tak(y - 1, z, x),
               tak(z - 1, x, y))

print(tak(24, 16, 8))
