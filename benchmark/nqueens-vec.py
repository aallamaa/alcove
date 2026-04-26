# N-queens with a fixed-size list (mirroring alcove's vector).
# Same algorithm as nqueens-vec.alc.
def safe(col, qs, row, offset=0):
    while offset < row:
        placed = qs[offset]
        if col == placed: return False
        d = row - offset
        if col - d == placed: return False
        if col + d == placed: return False
        offset += 1
    return True

def try_cols(n, row, col, qs, count):
    while col < n:
        if safe(col, qs, row):
            qs[row] = col
            count += solve(n, row + 1, qs)
        col += 1
    return count

def solve(n, row, qs):
    if row >= n:
        return 1
    return try_cols(n, row, 0, qs, 0)

qs = [0] * 10
print(solve(10, 0, qs))
