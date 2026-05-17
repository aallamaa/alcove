# alcove script demo -- transpiles to alcove
# (run: python3 als.py examples/alcove-script/demo.als | ./alcove --noload)

def assert (txt a b):
  pr txt
  if (iso a b):
    prn "\t: \x1B[92mPassed\x1B[39m"
    prn "\t: \x1B[91mFailed\x1B[39m"

def square(x):
  * x x

def fib(n):
  if (< n 2):
    n
    + (fib (- n 1)) (fib (- n 2))

def main ():
  prn "hello"
  prn "world"
  assert "square 5 = 25" (square 5) 25
  assert "fib 10 = 55"   (fib 10)   55
  # alcove `let` takes ONE binding; multiple bindings use `with`
  with (x 10 y 20):
    prn (+ x y)
  for i 1 5:
    pr i
  prn ""

main()
