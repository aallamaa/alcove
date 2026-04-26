def safe(c, qs, offset=1):
    for q in qs:
        if c == q or c + offset == q or c - offset == q:
            return False
        offset += 1
    return True

def solve(n, qs=()):
    if len(qs) == n:
        return 1
    total = 0
    for c in range(1, n + 1):
        if safe(c, qs):
            total += solve(n, (c,) + qs)
    return total

print(solve(10))
