def build(i, n, acc):
    while i <= n:
        acc = [i, acc]
        i += 1
    return acc
def reduce_add(xs, acc):
    while xs:
        acc += xs[0]
        xs = xs[1]
    return acc
xs = build(1, 100000, None)
print(reduce_add(xs, 0))
