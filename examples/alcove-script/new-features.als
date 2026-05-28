# new-features.als — examples for cond, match, try/finally,
# destructuring params, and generators.
# Run: alcoves --noload new-features.als   ← preferred (uses als.h with elif/else)
#  or: python3 als.py new-features.als | alcove --noload  ← if/elif/else won't work
#
# ALS block syntax notes:
#   - `if/elif/else` work in block style via alcoves (als.h); als.py lacks elif.
#   - `match` and `cond` take FLAT alternating (pattern result) pairs.
#     ALS wraps each block-body line into a list, breaking the flat protocol.
#     Write `match`/`cond` inline (all on one line), or use if/elif/else.

= even? (fn (x) (is (mod x 2) 0))

prn "=== 1. cond — flat multi-arm conditional ==="

# Inline cond: all arms on one line
def classify (n):
  cond (< n 0) "negative" (is n 0) "zero" (< n 10) "small" (< n 100) "medium" "large"

prn (classify -5)    # negative
prn (classify 0)     # zero
prn (classify 7)     # small
prn (classify 42)    # medium
prn (classify 999)   # large

# Equivalent using if/elif/else blocks (idiomatic ALS for multi-arm)
def classify2 (n):
  if (< n 0):
    "negative"
  elif (is n 0):
    "zero"
  elif (< n 10):
    "small"
  elif (< n 100):
    "medium"
  else:
    "large"

prn (classify2 7)    # small
prn (classify2 42)   # medium


prn "=== 2. match — structural pattern matching ==="

# Literal match — all inline since match needs flat pairs
def day-name (n):
  match n 0 "Sun" 1 "Mon" 2 "Tue" 3 "Wed" 4 "Thu" 5 "Fri" 6 "Sat" "?"

prn (day-name 3)   # Wed
prn (day-name 9)   # ?

# Capture binding — variables in pattern position bind in result scope
def describe-list (xs):
  match xs nil "empty" (list h) (str "singleton: " h) (list h1 h2) (str "pair: " h1 " " h2) (cons h _) (str "starts with: " h)

prn (describe-list nil)              # empty
prn (describe-list (list 42))        # singleton: 42
prn (describe-list (list 1 2))       # pair: 1 2
prn (describe-list (list 1 2 3 4))   # starts with: 1

# Guard predicate (? pred)
def classify-num (x):
  match x (? (fn (n) (< n 0))) "negative" (? even?) "even" "odd"

prn (classify-num -5)   # negative
prn (classify-num 4)    # even
prn (classify-num 3)    # odd

# Symbol literal (quote sym)
def sym-kind (s):
  match s (quote if) "control" (quote def) "binding" (quote fn) "binding" _ "other"

prn (sym-kind 'if)    # control
prn (sym-kind 'map)   # other

# Cons pattern
def safe-head (xs):
  match xs (cons h _) h _ nil

prn (safe-head (list 10 20 30))   # 10
prn (safe-head nil)               # nil

# Nested list patterns — 2D point classification
def quadrant (p):
  match p (list 0 0) "origin" (list 0 y) (str "y-axis y=" y) (list x 0) (str "x-axis x=" x) (list x y) (cond (and (> x 0) (> y 0)) "Q1" (and (< x 0) (> y 0)) "Q2" (and (< x 0) (< y 0)) "Q3" "Q4") _ "not a 2d point"

prn (quadrant (list 0 0))    # origin
prn (quadrant (list 0 5))    # y-axis y=5
prn (quadrant (list 3 4))    # Q1
prn (quadrant (list -1 -2))  # Q3
prn (quadrant "hi")          # not a 2d point

# match inside a function — FizzBuzz using guard predicates
def fizzbuzz (n):
  match n (? (fn (x) (is (mod x 15) 0))) "FizzBuzz" (? (fn (x) (is (mod x 3) 0))) "Fizz" (? (fn (x) (is (mod x 5) 0))) "Buzz" (str n)

for-each! x (range! 1 16):
  pr (fizzbuzz x)
  pr " "
prn ""   # 1 2 Fizz 4 Buzz Fizz 7 8 Fizz Buzz 11 Fizz 13 14 FizzBuzz

# Recursive match — tree depth
def tree-depth (tree):
  match tree (cons h t) (+ 1 (max (tree-depth h) (tree-depth t))) _ 0

prn (tree-depth nil)                          # 0
prn (tree-depth (list 1 2))                   # 1
prn (tree-depth (cons (list 1 2) (list 3)))   # 2


prn "=== 3. try / finally ==="

# Catch an error
prn (try (/ 1 0) (fn (e) (str "caught: " (error-message e))))
# caught: Illegal Division by 0

# Body succeeds — handler not called
prn (try (* 6 7) (fn (e) "err"))   # 42

# finally always runs; its value is discarded
def safe-div (a b):
  try (/ a b) (fn (e) -1) (prn "  [cleanup]")

prn (safe-div 10 2)   # prints [cleanup] then 5
prn (safe-div 10 0)   # prints [cleanup] then -1

# nil handler: propagate error, cleanup still runs
def with-cleanup (label thunk):
  try (thunk) nil (prn (str "  [" label " done]"))

with-cleanup "ok" (fn () (* 3 7))    # [ok done]

# Handler can use match to classify errors (match written inline — flat pairs)
def robust-div (x):
  try (/ 100 x) (fn (e) (match (error-message e) (? (fn (s) (string-contains? s "Division"))) 0 e))

prn (robust-div 4)    # 25
prn (robust-div 0)    # 0 — div-by-zero silently returns 0

# Retry pattern
def try-twice (thunk fallback):
  try (thunk) (fn (_) (try (thunk) (fn (_) (fallback))))

= attempts 0
def flaky ():
  = attempts (+ attempts 1)
  if (< attempts 2) (/ 1 0) attempts

prn (try-twice flaky (fn () "gave up"))   # 2


prn "=== 4. destructuring params ==="

def swap ((x y)):
  list y x

prn (swap (list 10 20))   # (20 10)

def vec2-add ((ax ay) (bx by)):
  list (+ ax bx) (+ ay by)

prn (vec2-add (list 1 2) (list 3 4))   # (4 6)

def dot2 ((ax ay) (bx by)):
  + (* ax bx) (* ay by)

prn (dot2 (list 3 4) (list 1 2))   # 11

# Rest params inside a destructured list
def first-two ((a b . rest)):
  list a b (length rest)

prn (first-two (list 10 20 30 40))   # (10 20 2)

# Missing elements → nil
def opt-pair ((a b)):
  list a b

prn (opt-pair (list 42))     # (42 nil)
prn (opt-pair (list 1 2))    # (1 2)

# Multiple destructured params — distance squared
def dist-sq ((ax ay) (bx by)):
  let dx (- ax bx):
    let dy (- ay by):
      + (* dx dx) (* dy dy)

prn (dist-sq (list 0 0) (list 3 4))   # 25


prn "=== 5. generators ==="

# range! constructors
prn (collect! (range! 5))           # (0 1 2 3 4)
prn (collect! (range! 2 10 3))      # (2 5 8)
prn (collect! (range! 10 0 -2))     # (10 8 6 4 2)

# iter! wraps an existing list
= g (iter! (list "a" "b" "c"))
prn (next! g)           # a
prn (next! g)           # b
prn (next! g)           # c
prn (done? (next! g))   # t — exhausted

# *done* sentinel
prn (done? (*done*))                  # t
prn (done? (next! (range! 0 0)))      # t — empty range
prn (iso (*done*) (*done*))           # t — unique identity

# map! — lazy transformation, no intermediate list
prn (collect! (map! (fn (x) (* x x)) (range! 1 6)))   # (1 4 9 16 25)

# filter! — lazy filtering
prn (collect! (filter! even? (range! 10)))   # (0 2 4 6 8)
prn (collect! (filter! odd (range! 10)))     # (1 3 5 7 9)

# pipeline: odd squares, all lazy
prn (collect! (filter! odd (map! (fn (x) (* x x)) (range! 10))))
# (1 9 25 49 81)

# for-each! — consume with side effects
for-each! x (range! 1 6):
  pr (* x x)
  pr " "
prn ""   # 1 4 9 16 25

# sum via for-each!
def gen-sum (gen):
  = total 0
  for-each! x gen:
    = total (+ total x)
  total

prn (gen-sum (range! 1 101))   # 5050

# count via gen-sum + map!
def gen-count (gen):
  gen-sum (map! (fn (_) 1) gen)

prn (gen-count (filter! odd (range! 100)))   # 50

# zip two generators
def zip! (g1 g2):
  fn ():
    = a (next! g1)
    = b (next! g2)
    if (or (done? a) (done? b)) (*done*) (list a b)

= zg (zip! (range! 1 4) (iter! (list "a" "b" "c")))
prn (next! zg)          # (1 a)
prn (next! zg)          # (2 b)
prn (next! zg)          # (3 c)
prn (done? (next! zg))  # t

# Stateful generator via closure — no yield keyword needed
def counter (start step):
  = n start
  fn ():
    = v n
    = n (+ n step)
    v

= c (counter 0 5)
prn (c)   # 0
prn (c)   # 5
prn (c)   # 10

# Fibonacci generator
def fib-gen ():
  = a 0
  = b 1
  fn ():
    = v a
    = next-b (+ a b)
    = a b
    = b next-b
    v

= fibs (fib-gen)
def take-gen (n gen):
  = acc (list)
  = i 0
  while (< i n):
    = v (next! gen)
    if (done? v):
      = i n
      = acc (cons v acc)
    = i (+ i 1)
  reverse acc

prn (take-gen 10 fibs)   # (0 1 1 2 3 5 8 13 21 34)

# Running sum
def running-sum (gen):
  = acc 0
  fn ():
    = v (next! gen)
    if (done? v):
      (*done*)
      = acc (+ acc v)
      acc

prn (collect! (running-sum (range! 1 6)))   # (1 3 6 10 15)


# ──────────────────────────────────────────────────────────────────────────
# 6. vector literals — #[...] works in Alcove Script (lowered to (vector ...))
# ──────────────────────────────────────────────────────────────────────────
prn #[1 2 3]                          # #[1 2 3]
prn (vec-ref #[10 20 30] 1)           # 20
prn (vec-len #[5 6 7 8])              # 4
prn #[#[1 2] #[3 4]]                  # nested: #[#[1 2] #[3 4]]
= grid #[#[1 2 3] #[4 5 6]]
prn (vec-ref (vec-ref grid 1) 2)      # 6
# `#` still starts a comment everywhere else:
prn "comments still work"             # this trailing text is ignored
