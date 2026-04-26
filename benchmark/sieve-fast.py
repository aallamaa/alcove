# Sieve of Eratosthenes — true mark-composites algorithm using a list.
# Mirrors sieve-fast.alc.
def primes_up_to(n):
    marks = [True] * (n + 1)
    i = 2
    while i * i <= n:
        if marks[i]:
            j = i * i
            while j <= n:
                marks[j] = False
                j += i
        i += 1
    cnt = 0
    i = 2
    while i <= n:
        if marks[i]:
            cnt += 1
        i += 1
    return cnt
print(primes_up_to(100000))
