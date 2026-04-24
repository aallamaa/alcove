def sieve(xs):
    if not xs: return []
    p = xs[0]
    return [p] + sieve([x for x in xs[1:] if x % p != 0])
print(len(sieve(list(range(2, 1001)))))
