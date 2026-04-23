import sys
sys.setrecursionlimit(100000)
def countdown(n):
    if n > 0:
        return countdown(n-1)
    return n
def loop(k):
    while k > 0:
        countdown(1000)
        k -= 1
    return k
print(loop(1000))
