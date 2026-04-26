import sys
sys.setrecursionlimit(50000)
def is_prime_given(acc, i):
    while acc:
        if i % acc[0] == 0: return False
        acc = acc[1]
    return True
def primes_up_to(n):
    acc = None
    i = 2
    while i <= n:
        if is_prime_given(acc, i):
            acc = (i, acc)
        i += 1
    cnt = 0
    while acc:
        cnt += 1
        acc = acc[1]
    return cnt
print(primes_up_to(5000))
