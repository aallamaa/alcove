# `call/cc` — escape continuations

alcove provides `call/cc` (call-with-current-continuation) in its **escape**
form: one-shot, upward-only continuations. This is the subset that covers the
overwhelming majority of real uses — early return, non-local exit, breaking
out of nested loops, exception-style bailout — without the cost and risk of
full re-entrant continuations.

```lisp
(call/cc (fn (k) ... (k value) ...))
```

`call/cc` calls your function with one argument, the **continuation** `k`.
Invoking `(k value)` immediately abandons whatever computation is in progress
and makes the whole `(call/cc …)` form return `value`. If `k` is never
invoked, `call/cc` returns the normal result of the function.

```lisp
(call/cc (fn (k) (+ 1 2)))         ; → 3      (k unused: normal return)
(call/cc (fn (k) (k 42) 999))      ; → 42     (999 is never reached)
(call/cc (fn (k) (+ 1 (k 100) 2))) ; → 100    (the + never completes)
```

## Semantics and limits

- **Escape-only (upward, one-shot).** `k` is valid only during the dynamic
  extent of its `call/cc`. You may invoke it to *escape* outward to that
  `call/cc`; you may not use it to *resume* a computation later or re-enter
  it. Invoking `k` after its `call/cc` has already returned is an error.
- **Not a full re-entrant continuation.** alcove's evaluator is a recursive
  C tree-walker plus a bytecode VM; capturing and resuming arbitrary control
  state would require capturing the C stack (a CPS or stack-copying rewrite).
  This `call/cc` is equivalent to Scheme's `call/ec` (escape continuation) /
  `call-with-escape-continuation`.
- **How it works.** Invoking `(k v)` produces an internal escape token that
  propagates upward exactly like an error value (the same mechanism as
  `try`), so it threads cleanly through `if`, `do`, `while`, `for`, recursion,
  and higher-order calls like `map`. The matching `call/cc` frame catches its
  own token and yields the payload. No `setjmp` is involved.
- **The argument must be a function.** `(call/cc 5)` is an error (catchable
  with `try`).

## Relationship to `try`

`try` and `call/cc` share the same propagation substrate but read
differently:

- `try` catches **errors** raised below it.
- `call/cc` gives **you** a handle (`k`) you choose when to fire, returning a
  value of your choosing to a point you chose.

Use `try` for "something went wrong, recover"; use `call/cc` for "I want to
return early from here with this value".

## Idioms

### Early return / guard

```lisp
(def clamp (x lo hi)
  (call/cc (fn (ret)
    (do (if (< x lo) (ret lo))
        (if (> x hi) (ret hi))
        x))))
(clamp  5 0 10)  ; → 5
(clamp -3 0 10)  ; → 0
(clamp 99 0 10)  ; → 10
```

### Find-first (abandon the search on a hit)

```lisp
(def find-first (pred xs)
  (call/cc (fn (return)
    (do (def go (ys)
          (if (no ys) nil
            (if (pred ys) (return (car ys)) (go (cdr ys)))))
        (go xs)))))
(find-first (fn (l) (> (car l) 3)) '(1 2 5 0 9))  ; → 5
```

### Break out of a nested loop

```lisp
(def find-pair (n target)
  (call/cc (fn (found)
    (do (= i 1)
        (while (<= i n)
          (do (= j 1)
              (while (<= j n)
                (do (if (is (+ i j) target) (found (list i j)))
                    (= j (+ j 1))))
              (= i (+ i 1))))
        nil))))
(find-pair 5 7)  ; → (2 5)   ; both inner and outer loops abandoned at once
```

### Exception-style bailout with a marker

```lisp
(def safe-div (a b k) (if (is b 0) (k 'div-by-zero) (/ a b)))
(call/cc (fn (k) (safe-div 10 2 k)))  ; → 5
(call/cc (fn (k) (safe-div 10 0 k)))  ; → div-by-zero
```

### Short-circuit a fold

```lisp
; sum a list, but bail to 0 as soon as a negative element appears
(def sum-nonneg (xs)
  (call/cc (fn (bail)
    (do (def go (ys acc)
          (if (no ys) acc
            (if (< (car ys) 0) (bail 0) (go (cdr ys) (+ acc (car ys))))))
        (go xs 0)))))
(sum-nonneg '(3 4 5))   ; → 12
(sum-nonneg '(3 -1 5))  ; → 0
```

### Escape from inside a higher-order function

```lisp
; map normally visits every element; the continuation cuts it short
(call/cc (fn (k)
  (do (map (fn (x) (if (> x 2) (k x))) '(1 2 3 4)) -1)))  ; → 3
```

### Nested `call/cc`

The inner body can fire the inner *or* the outer continuation:

```lisp
(call/cc (fn (outer)
  (+ 1 (call/cc (fn (inner) (outer 100))))))  ; → 100  (skips the + 1)

(call/cc (fn (outer)
  (+ 1 (call/cc (fn (inner) (inner 100))))))  ; → 101  (inner returns 100 to the +)
```

## `defc` — early-return functions without the boilerplate

Most uses of `call/cc` are "define a function that can return early." `defc`
captures exactly that: `(defc name (params…) body…)` is sugar for
`(def name (params) (call/cc (fn (return) body…)))`, binding the escape
continuation to `return`. So `(return v)` exits the function immediately with
`v`:

```lisp
(defc clamp (x lo hi)
  (if (< x lo) (return lo))
  (if (> x hi) (return hi))
  x)                                ; falls through to x if neither guard fires
(clamp 99 0 10)                     ; → 10

(defc find-first (pred xs)
  (each x xs (if (pred x) (return x)))
  nil)                              ; → nil if nothing matched
```

`return` is bound in the body (an anaphoric capture). The param list takes the
same forms as `def` (list pattern, or a bare symbol for rest args), and
`(return v)` accepts any `v` including the falsy `0`/`nil`. If `return` is
never fired, the function returns its last body form. In Adder, the `:`-block
form works too (use indented blocks, not inline `cond: expr`):

```python
defc clamp (x lo hi):
  if (< x lo):
    return lo
  if (> x hi):
    return hi
  x
```

## Notes

- The escape value can be any alcove value, including a list or other
  structure: `(call/cc (fn (k) (k (list 1 2 3))))` → `(1 2 3)`.
- A continuation captured by a closure works as expected — the closure can
  carry `k` and fire it from wherever it is eventually called, as long as
  that happens before the originating `call/cc` returns.
- See the `20g`/`20h` blocks in `test.alc` for the full regression suite.
