import sys
sys.setrecursionlimit(100000)
def fact(n):
    if n < 2:
        return 1
    return n * fact(n-1)
def loop(k):
    while k > 0:
        fact(19)
        k -= 1
    return k
print(loop(100000))
