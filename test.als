def assert (txt a b):
  pr txt
  if (iso a b):
    prn "\t: \x1B[92mPassed\x1B[39m"
    prn "\t: \x1B[91mFailed\x1B[39m"

assert "This will Pass" 1 1

assert "This will Fail" 1 0

assert "addition" 3 (+ 1 2)

assert " (+) is 0" 0 (+)

assert "cons test " (cons 'f '(a b)) '(f a b)

= x '(a b)

assert "(cons 'f x) -> (f a b)" '(f a b) (cons 'f x)

assert "(cons 'a nil) -> (a)" (cons 'a nil) '(a)

assert "(car '(a b c)) -> a" (car '(a b c)) 'a

assert "(cdr '(a b c)) -> (b c)" (cdr '(a b c)) '(b c)

assert "(list 'a 1 " foo " '(b))  -> (a 1 " foo " (b))" (list 'a 1 "foo" '(b)):
  '(a 1 "foo" (b))

assert "(cons 'a (cons 1 (cons " foo " (cons '(b) nil)))) -> (a 1 " foo " (b))" (cons 'a (cons 1 (cons "foo" (cons '(b) nil)))):
  '(a 1 "foo" (b))

= x '(a b)

assert "(= (car x) 'z) -> z" (= (car x) 'z) 'z

assert "and x -> (z b)" x '(z b)

def average (x y):
  / (+ x y) 2

assert "(average 2 4) -> 3" (average 2 4) 3

assert "((fn (x y) (/ (+ x y) 2)) 2 4) -> 3" ((fn (x y) (/ (+ x y) 2)) 2 4) 3

assert "(\"foo\" 0) -> #\\f" ("foo" 0) #\f

= s "foo"

assert "(= (s 0) #\m) ->#\m " (= (s 0) #\m) #\m

assert " s -> moo" s "moo"

assert "(let x 1 (+ x (* x 2))) -> 3" (let x 1 (+ x (* x 2))) 3

assert "(with (x 3 y 4)\n       (sqrt (+ (expt x 2) (expt y 2)))) \n       -> 5" (with (x 3 y 4) (sqrt (+ (expt x 2) (expt y 2)))):
  5.0

assert "(if (odd 1) 'a 'b) -> a" (if (odd 1) 'a 'b) 'a

assert "(if (odd 2) 'a 'b) -> b" (if (odd 2) 'a 'b) 'b

def mylen (xs):
  if (no xs):
    0
    + 1 (mylen (cdr xs))

assert "mylen nil -> 0" (mylen nil) 0

assert "(mylen '(a b)) -> 2 " (mylen '(a b)) 2

assert "(is 'a 'a) -> t" (is 'a 'a) t

assert "(is " foo " " foo ") -> t" (is "foo" "foo") t

assert "(let x (list 'a) (is x x)) -> t" (let x (list 'a) (is x x)) t

assert "(is (list 'a) (list 'a)) -> nil" (is (list 'a) (list 'a)) nil

assert "(iso (list 'a) (list 'a)) ->t" (iso (list 'a) (list 'a)) t

assert "(let x 'a (in x 'a 'b 'c)) -> t" (let x 'a (in x 'a 'b 'c)) t

def runforsum (n):
  let s 0:
    for i 1 n:
      = s (+ s 1)

assert "first  (runforsum 5) -> 5" (runforsum 5) 5

assert "second (runforsum 5) -> 5" (runforsum 5) 5

assert "anon-fn arith" ((fn (x y) (+ (* x 2) y)) 3 4) 10

def dbl (x):
  + x x

assert "AST calls compiled" (let x 7 (dbl x)) 14

def sumtwo (a b):
  + a b

assert "compiled call chain" (sumtwo (dbl 5) (dbl 6)) 22

def seven (a b c d e f g):
  + a (+ b (+ c (+ d (+ e (+ f g)))))

assert "7 params spill" (seven 1 2 3 4 5 6 7) 28

assert "vm < returns t" (< 3 5) t

assert "vm < returns nil" (< 5 3) nil

def vmerr (x):
  / x 0

vmerr 5

assert "recovery after VM error" 42 42

def inc (n):
  = n (+ n 1)

assert "(= param val) returns val" (inc 5) 6

assert "let basic" (let x 3 (+ x 4)) 7

assert "let uses caller scope" ((fn (y) (let x 10 (+ x y))) 5) 15

assert "nested let shadows" (let x 1 (let x 2 x)) 2

assert "with 2 bindings" (with (a 3 b 4) (+ a b)) 7

assert "with shadows" (let a 1 (with (a 9) a)) 9

assert "= into let slot" (let c 10 (= c 20) c) 20

def lastsq (n):
  for i 1 n:
    * i i

assert "for returns last body val" (lastsq 5) 25

def forempty (n):
  for i 5 1:
    * n n

assert "for empty range nil" (forempty 7) nil

def fsum (n):
  let s 0:
    for i 1 n:
      = s (+ s i)

assert "compiled fsum 10 -> 55" (fsum 10) 55

assert "compiled fsum 100 -> 5050" (fsum 100) 5050

def even? (n):
  if (< n 1):
    t
    odd? (- n 1)

def odd? (n):
  if (< n 1):
    nil
    even? (- n 1)

assert "mutual even? 1000" (even? 1000) t

assert "mutual odd? 1001" (odd? 1001) t

assert "mutual deep 100k" (even? 100000) t

def consit (a b):
  cons a b

assert "vm cons returns pair" (iso (consit 1 (list 2 3)) (list 1 2 3)) t

def carit (xs):
  car xs

def cdrit (xs):
  cdr xs

assert "vm car" (carit (list 10 20 30)) 10

assert "vm cdr iso" (iso (cdrit (list 10 20 30)) (list 20 30)) t

def listit (a b c):
  list a b (* c 2)

assert "vm list 3" (iso (listit 1 2 5) (list 1 2 10)) t

def myrev (xs acc):
  if (no xs):
    acc
    myrev (cdr xs) (cons (car xs) acc)

assert "vm tail-recursive reverse" (iso (myrev (list 1 2 3 4 5) (list)) (list 5 4 3 2 1)):
  t

def addone (n):
  + n 1

assert "JIT addone fix" (addone 41) 42

addone 'sym

assert "JIT addone after deopt" (addone 100) 101

def idheap (x):
  x

assert "JIT identity heap string safe" (idheap "hello") "hello"

assert "JIT identity heap list safe" (iso (idheap (list 1 2)) (list 1 2)) t

def cdn (n):
  if (> n 0):
    cdn (- n 1)
    n

assert "JIT loop fix to 0" (cdn 1000) 0

cdn 'sym

assert "JIT loop after deopt" (cdn 500) 0

def jitbuild (i n acc):
  if (> i n):
    acc
    jitbuild (+ i 1) n (cons i acc)

assert "JIT build inc cons" (jitbuild 1 5 (list)) (list 5 4 3 2 1)

assert "JIT build keeps acc" (jitbuild 1 3 (list 'z)) (list 3 2 1 'z)

def addb (a b):
  + b 1

assert "JIT addb slot1 fix" (addb 'ignored 41) 42

addb 0 'sym

assert "JIT addb slot1 after deopt" (addb 0 100) 101

def doublen (n):
  * n 2

assert "JIT doublen fix" (doublen 21) 42

doublen 'sym

assert "JIT doublen after deopt" (doublen 100) 200

def triplen (n):
  * n 3

assert "JIT triplen 0" (triplen 0) 0

assert "JIT triplen negative K" ((fn (n) (* n -4)) 7) -28

def addbump (x):
  + x 1

def lpcall (k):
  if (> k 0):
    do:
      addbump 99
      lpcall (- k 1)
    k

assert "JIT loop+call to 0" (lpcall 100) 0

lpcall 'sym

assert "JIT loop+call after deopt" (lpcall 50) 0

def jfib (n):
  if (< n 2):
    n
    + (jfib (- n 1)) (jfib (- n 2))

assert "JIT fib base 0" (jfib 0) 0

assert "JIT fib base 1" (jfib 1) 1

assert "JIT fib 10" (jfib 10) 55

assert "JIT fib 20" (jfib 20) 6765

assert "JIT fib 30" (jfib 30) 832040

jfib 'sym

assert "JIT fib after deopt" (jfib 15) 610

def make-adder (n):
  fn (x):
    + x n

= add5 (make-adder 5)

= add10 (make-adder 10)

assert "closure: add5 100" (add5 100) 105

assert "closure: add10 100" (add10 100) 110

def make-counter (dummy):
  let n 0:
    fn (d):
      = n (+ n 1)

= ca (make-counter 0)

= cb (make-counter 0)

ca 0

ca 0

ca 0

assert "closure: counter ca = 4" (ca 0) 4

assert "closure: counter cb = 1" (cb 0) 1

def f2 (n):
  let m 10:
    fn (d):
      + n m

assert "closure: nested let cap" ((f2 5) 0) 15

def zhello ():
  + 1 2

assert "0-arg def returns 3" (zhello) 3

assert "mod 17 5" (mod 17 5) 2

assert "abs -42" (abs -42) 42

assert "max 1 5 3" (max 1 5 3) 5

assert "min 1 5 -2" (min 1 5 -2) -2

assert "length list" (length (list 1 2 3 4 5)) 5

assert "nth 2 list" (nth 2 (list 10 20 30 40)) 30

assert "reverse" (iso (reverse (list 1 2 3)) (list 3 2 1)) t

assert "append two lists" (iso (append (list 1 2) (list 3 4)) (list 1 2 3 4)):
  t

assert "number? fixnum" (number? 42) t

assert "string? \"hi\"" (string? "hi") t

assert "symbol? 'a" (symbol? 'a) t

assert "pair? list" (pair? (list 1 2)) t

assert "fn? lambda" (fn? (fn (x) x)) t

assert "fn? builtin" (fn? +) t

assert "fn? non-fn" (fn? 42) nil

assert "map square" (iso (map (fn (x) (* x x)) (list 1 2 3)) (list 1 4 9)) t

assert "filter > 2" (iso (filter (fn (x) (> x 2)) (list 1 2 3 4)) (list 3 4)):
  t

assert "reduce sum" (reduce (fn (a b) (+ a b)) 0 (list 1 2 3 4 5)) 15

assert "apply 3 args" (apply (fn (a b c) (+ a (+ b c))) (list 10 20 30)) 60

assert "any? has match" (any? (fn (x) (> x 3)) (list 1 2 3 4)) t

assert "any? no match" (any? (fn (x) (> x 100)) (list 1 2 3)) nil

assert "any? empty" (any? (fn (x) (> x 0)) (list)) nil

assert "all? all match" (all? (fn (x) (> x 0)) (list 1 2 3)) t

assert "all? one fails" (all? (fn (x) (> x 0)) (list 1 0 3)) nil

assert "all? empty" (all? (fn (x) (> x 0)) (list)) t

def jit-sieve-fn (i n):
  def jit-sieve-up (j acc):
    if (> j n):
      acc
      if (any? (fn (d) (is (mod j d) 0)) acc):
        jit-sieve-up (+ j 1) acc
        jit-sieve-up (+ j 1) (cons j acc)
  length (jit-sieve-up i (list))

assert "any?-in-recursion: π(100)" (jit-sieve-fn 2 100) 25

assert "if cond-style: 1st clause" (if t 'a 'b 'c) 'a

assert "if cond-style: 2nd clause" (if nil 'a t 'b 'c) 'b

assert "if cond-style: fallback" (if nil 'a nil 'b 'c) 'c

assert "if 2-arg true" (if 1 'yes) 'yes

assert "if 2-arg false -> nil" (if nil 'yes) nil

def compiled-if-ladder (x):
  if (is x 1):
    'one
    is x 2
    'two
    'other

assert "compiled if cond-style: 1st clause" (compiled-if-ladder 1) 'one

assert "compiled if cond-style: 2nd clause" (compiled-if-ladder 2) 'two

assert "compiled if cond-style: fallback" (compiled-if-ladder 3) 'other

= sbuf "abcdef"

= i 2

= (sbuf i) #\Z

assert "string-set with var index" sbuf "abZdef"

assert "nth on non-list -> nil-ish" (no (no (nth 0 5))) nil

= sread "abcdef"

assert "string read by var, literal idx" (sread 2) #\c

= ridx 4

assert "string read by var, computed idx" (sread ridx) #\e

def sget (s n):
  s n

assert "string read inside def body" (sget "hello" 1) #\e

assert "(+) is 0" (+) 0

assert "(*) is 1" (*) 1

assert "(+ a) is a" (+ 7) 7

assert "(- 5) negates" (- 5) -5

assert "(- a b c)" (- 10 1 2 3) 4

assert "(/ a b)" (/ 12 3) 4

assert "(/ a b c)" (/ 60 3 5) 4

assert "mod" (mod 10 3) 1

assert "abs neg" (abs -7) 7

assert "abs pos" (abs 7) 7

assert "max" (max 3 1 7 2 5) 7

assert "min" (min 3 1 7 2 5) 1

assert "max single" (max 9) 9

assert "min single" (min 9) 9

assert "expt int^int small" (expt 2 10) 1024

assert "expt 0^0 = 1" (expt 0 0) 1

assert "expt 1^N = 1" (expt 1 100) 1

assert "expt N^0 = 1" (expt 99 0) 1

assert "** alias" (** 3 4) 81

assert "expt int overflow → float" (iso (expt 2 80) (** 2 80)) t

assert "sqrt-int" (sqrt-int 100) 10

assert "sqrt-int floor" (sqrt-int 99) 9

assert "odd t" (odd 3) t

assert "odd nil" (odd 4) nil

assert "bit-and" (bit-and 12 10) 8

assert "bit-or" (bit-or 12 10) 14

assert "bit-xor" (bit-xor 12 10) 6

assert "& alias" (& 12 10) 8

assert "| alias" (| 12 10) 14

assert "^ alias" (^ 12 10) 6

assert "~ 0 -> -1" (~ 0) -1

assert "~ 5" (~ 5) -6

assert "<< basic" (<< 1 8) 256

assert ">> basic" (>> 256 4) 16

assert ">> sign-preserving" (>> -8 1) -4

assert "<< then >>" (>> (<< 7 4) 4) 7

assert "(** 2 16) via shift" (<< 1 16) 65536

assert "< 0-arg vacuous" (<) t

assert "< 1-arg vacuous" (< 5) t

assert "< chained ok" (< 1 2 3 4 5) t

assert "< chained break" (< 1 2 2 4) nil

assert "< chained break late" (< 1 2 3 2) nil

assert "> chained" (> 5 4 3 2 1) t

assert "<= chained equal ok" (<= 1 2 2 3) t

assert ">= chained equal ok" (>= 5 4 4 3) t

assert "string cmp <" (< "abc" "abd") t

assert "char cmp <" (< #\a #\b) t

assert "char cmp =>" (>= #\b #\a) t

def compiled-lt3 (a b c):
  < a b c

assert "compiled chained < ok" (compiled-lt3 1 2 3) t

assert "compiled chained < break" (compiled-lt3 1 3 2) nil

def compiled-slt (a b):
  < a b

assert "compiled string cmp <" (compiled-slt "abc" "abd") t

def compiled-clt (a b):
  < a b

assert "compiled char cmp <" (compiled-clt #\a #\b) t

assert "is symbol" (is 'a 'a) t

assert "is fixnum" (is 5 5) t

assert "is char" (is #\x #\x) t

assert "is mixed types -> nil" (is 5 'a) nil

assert "iso list" (iso '(1 2) '(1 2)) t

assert "iso unequal list" (iso '(1 2) '(1 3)) nil

assert "iso nested" (iso '((a) (b c)) '((a) (b c))) t

assert "iso strings" (iso "foo" "foo") t

assert "in match first" (in 'a 'a 'b 'c) t

assert "in match middle" (in 'b 'a 'b 'c) t

assert "in match none" (in 'z 'a 'b 'c) nil

assert "no nil" (no nil) t

assert "no empty list" (no (list)) t

assert "no nonempty" (no (list 1)) nil

assert "no non-empty string" (no "x") nil

assert "no 0 -> t (alcove deviates from arc)" (no 0) t

assert "if ladder 1st" (if t 1 t 2 t 3 99) 1

assert "if ladder 2nd" (if nil 1 t 2 t 3 99) 2

assert "if ladder 3rd" (if nil 1 nil 2 t 3 99) 3

assert "if ladder fallback" (if nil 1 nil 2 nil 3 99) 99

assert "if no fallback no match" (if nil 1 nil 2) nil

def evenc? (n):
  case (mod n 2):
    0
    t
    nil

def evensum (n acc):
  if (< n 1):
    acc
    evensum (- n 1) (if (evenc? n) (+ acc n) acc)

assert "case+if mutual: sum evens 100" (evensum 100 0) 2550

def whenchain (n):
  when (> n 0):
    whenchain (- n 1)

assert "when tail-rec deep" (whenchain 1000) nil

def andchain (n):
  and (> n 0) (andchain (- n 1))

assert "and tail-rec to nil" (andchain 1000) nil

def orchain (n acc):
  or (is n 0) (orchain (- n 1) (+ acc 1))

assert "or tail-rec returns t" (orchain 1000 0) t

assert "let shadow inner" (let x 10 (let x 20 x)) 20

assert "let inner doesn't leak" (let x 10 (let y 5 (+ x y))) 15

assert "with two bindings" (with (a 3 b 4) (+ a b)) 7

assert "with three bindings" (with (a 1 b 2 c 3) (+ a b c)) 6

assert "let-in-call binding" ((fn (x) (let y 1 (+ x y))) 5) 6

assert "length empty" (length nil) 0

assert "length 3" (length (list 1 2 3)) 3

assert "length list w/ nil" (length (list nil)) 1

def lenof (x):
  length x

assert "length string in compiled body" (lenof "abc") 3

assert "length list in compiled body" (lenof (list 1 2 3 4)) 4

assert "length nil in compiled body" (lenof nil) 0

assert "length vector in compiled body" (lenof (vector 10 20 30)) 3

assert "nth 0" (nth 0 (list 'a 'b 'c)) 'a

assert "nth 2" (nth 2 (list 'a 'b 'c)) 'c

assert "nth out-of-range -> nil" (nth 99 (list 'a 'b 'c)) nil

assert "reverse empty -> nil" (reverse nil) nil

assert "reverse 3" (iso (reverse (list 1 2 3)) (list 3 2 1)) t

assert "append nil nil" (append nil nil) nil

assert "append empty cons" (iso (append nil (list 1)) (list 1)) t

assert "append two non-empty" (iso (append (list 1) (list 2 3)) (list 1 2 3)):
  t

assert "append three" (iso (append (list 1) (list 2) (list 3)) (list 1 2 3)) t

def caught (e):
  no (no e)

assert "nth on int → error obj" (caught (nth 0 5)) t

assert "map on int → error obj" (caught (map prn 5)) t

assert "filter on int → error obj" (caught (filter odd 5)) t

assert "reduce on int → error obj" (caught (reduce + 0 5)) t

assert "any? on int → error obj" (caught (any? odd 5)) t

assert "all? on int → error obj" (caught (all? odd 5)) t

assert "append on int → error obj" (caught (append (list 1) 99)) t

assert "reverse on int → error obj" (caught (reverse 7)) t

def double (x):
  * x 2

assert "map basic" (iso (map double (list 1 2 3)) (list 2 4 6)) t

assert "map empty" (map double nil) nil

assert "filter odd" (iso (filter odd (list 1 2 3 4 5)) (list 1 3 5)) t

assert "filter all-pass" (iso (filter (fn (x) t) (list 1 2)) (list 1 2)) t

assert "filter all-fail" (filter (fn (x) nil) (list 1 2)) nil

assert "reduce sum" (reduce + 0 (list 1 2 3 4 5)) 15

assert "reduce empty" (reduce + 99 nil) 99

assert "reduce non-arith" (reduce (fn (a x) (cons x a)) nil (list 1 2 3)):
  '(3 2 1)

assert "any? has match" (any? odd (list 2 4 5 6)) t

assert "any? no match" (any? odd (list 2 4 6)) nil

assert "any? empty -> nil" (any? odd nil) nil

assert "all? all match" (all? odd (list 1 3 5)) t

assert "all? one fails" (all? odd (list 1 3 4)) nil

assert "all? empty -> t" (all? odd nil) t

= v (vec 5 0)

assert "vec len" (vec-len v) 5

assert "vec init" (vec-ref v 0) 0

vec-set! v 2 'x

assert "vec-set! / vec-ref" (vec-ref v 2) 'x

assert "vec-set! doesn't touch others" (vec-ref v 1) 0

assert "vec n=0 ok" (vec-len (vec 0 nil)) 0

assert "vec n too large -> error" (caught (vec (** 2 50) 0)) t

def compiled-mkvec (n):
  vec n 0

assert "compiled vec n too large -> error" (caught (compiled-mkvec (** 2 50))):
  t

assert "number? int" (number? 5) t

assert "number? float" (number? 1.5) t

assert "number? string" (number? "5") nil

assert "string? str" (string? "x") t

assert "string? symbol" (string? 'x) nil

assert "symbol? sym" (symbol? 'x) t

assert "symbol? str" (symbol? "x") nil

assert "pair? pair" (pair? (list 1)) t

assert "pair? nil" (pair? nil) nil

assert "fn? lambda" (fn? (fn (x) x)) t

assert "fn? builtin" (fn? +) t

assert "fn? not" (fn? 5) nil

assert "string read literal idx" ("abcdef" 0) #\a

assert "string read idx 5" ("abcdef" 5) #\f

= sread2 "qwerty"

assert "string read by var" (sread2 3) #\r

= ridx2 4

assert "string read by computed idx" (sread2 ridx2) #\t

= sw "hello"

= (sw 0) #\J

assert "string-set literal idx" sw "Jello"

= si 4

= (sw si) #\Y

assert "string-set computed idx" sw "JellY"

= sx "abc"

assert "string-set OOB -> error" (caught (= (sx 99) #\Z)) t

def need3 (a b c):
  + a b c

assert "correct arity" (need3 1 2 3) 6

assert "too few args -> error" (caught (need3 1 2)) t

assert "too many args -> error" (caught (need3 1 2 3 4)) t

def tail-arity-target (a b):
  b

def tail-arity-few (x):
  tail-arity-target x

tail-arity-few 9

assert "tail call wrong arity recovers" 42 42

def self-tail-few (a b):
  if (< a 1):
    b
    self-tail-few 0

self-tail-few 1 42

assert "self-tail wrong arity recovers" 42 42

def recfib (n):
  if (< n 2):
    n
    + (recfib (- n 1)) (recfib (- n 2))

assert "recfib 10" (recfib 10) 55

assert "recfib 20" (recfib 20) 6765

def recfact (n):
  if (< n 2):
    1
    * n (recfact (- n 1))

assert "recfact 5" (recfact 5) 120

assert "recfact 10" (recfact 10) 3628800

assert "recfact 12" (recfact 12) 479001600

def recack (m n):
  if (is m 0):
    + n 1
    if (is n 0):
      recack (- m 1) 1
      recack (- m 1) (recack m (- n 1))

assert "ack(2,3) = 9" (recack 2 3) 9

assert "ack(3,3) = 61" (recack 3 3) 61

assert "ack(3,5) = 253" (recack 3 5) 253

def rectak (x y z):
  if (no (< y x)):
    z
    rectak (rectak (- x 1) y z) (rectak (- y 1) z x) (rectak (- z 1) x y)

assert "tak(8 4 0) = 1" (rectak 8 4 0) 1

assert "tak(6 3 1) = 3" (rectak 6 3 1) 3

def helperA (n):
  - n 1

def myfib2 (n):
  if (< n 2):
    n
    + (helperA (- n 1)) (helperA (- n 2))

assert "JIT shape match w/ other callee 10" (myfib2 10) 15

assert "JIT shape match w/ other callee 5" (myfib2 5) 5

= zforget 99

assert "z bound" zforget 99

forget 'zforget

assert "forget unbinds" (caught zforget) t

= zkeep 7

persist 'zkeep

assert "ispersistent t" (ispersistent 'zkeep) t

unpersist 'zkeep

assert "unpersist clears flag" (ispersistent 'zkeep) nil

assert "unpersist doesn't unbind" zkeep 7

forget 'zkeep

= xs1 (list 1 2 3)

= (car xs1) 'A

assert "= (car x) replaces head" (iso xs1 '(A 2 3)) t

= xs2 (list 1 2 3)

= (cdr xs2) (list 99)

assert "= (cdr x) replaces tail" (iso xs2 '(1 99)) t

defmacro my-when (cond body):
  quasiquote (if (unquote cond) (unquote body) nil)

assert "macro my-when t" (my-when t 42) 42

assert "macro my-when nil" (my-when nil 42) nil

assert "eval quoted" (eval '(+ 2 3)) 5

assert "apply +" (apply + (list 1 2 3 4)) 10

assert "apply list" (iso (apply list (list 1 2 3)) (list 1 2 3)) t

def counter-mk ():
  = cnt 0
  def counter-inc ():
    = cnt (+ cnt 1)
    cnt

counter-mk()

counter-inc()

counter-inc()

counter-inc()

assert "global counter via def" cnt 3

assert "doc returns nil" (doc cons) nil

assert "doc on quoted" (doc 'reverse) nil

assert "doc on string" (doc "vec") nil

assert "doc on unknown" (doc never-existed) nil

= rt 1234

persist 'rt

savedb "/tmp/alcove-test-roundtrip.db"

forget 'rt

assert "after forget rt is gone" (caught rt) t

loaddb "/tmp/alcove-test-roundtrip.db"

assert "loaddb restores rt" rt 1234

forget 'rt

def countdownr (n):
  if (< n 1):
    'done
    countdownr (- n 1)

assert "countdown 1k tail-rec" (countdownr 1000) 'done

assert "countdown 100k tail-rec" (countdownr 100000) 'done

def evenq (n):
  if (< n 1):
    t
    oddq (- n 1)

def oddq (n):
  if (< n 1):
    nil
    evenq (- n 1)

assert "evenq 10000" (evenq 10000) t

assert "oddq 10001" (oddq 10001) t

assert "char literal" (is #\A #\A) t

assert "char from string" (is ("foo" 0) #\f) t

assert "(list) is nil" (list) nil

assert "(and) is t" (and) t

assert "(or) is nil" (or) nil

assert "(do)" (do) nil

= mutbuf "hello"

def upcase-first (s):
  = (s 0) #\H
  s

upcase-first mutbuf

assert "mutation persists in buf" mutbuf "Hello"

assert "1<<59 fits" (<< 1 59) 576460752303423488

assert "end-of-suite sentinel" 'reached 'reached

= sread6 "abcdef"

def t6_sget1 (n):
  sread6 n

assert "ticket6: var-string in def body" (t6_sget1 2) #\c

def t6_sget2 (s n):
  s n

assert "ticket6: passed-string in def" (t6_sget2 "hello" 1) #\e

def t6_sget3 (n):
  let _ 1:
    sread6 n

assert "ticket6: var-string in let body" (t6_sget3 4) #\e

def t6_lit (n):
  "abcdef" n

assert "ticket6: literal string in def" (t6_lit 3) #\d

def t6_mix (n):
  let a (sread6 n):
    let b (sread6 (+ n 1)):
      a

assert "ticket6: mixed reads in def" (t6_mix 0) #\a

= acc7 0

for y 0 2:
  for x 0 1:
    = acc7 (+ acc7 1)
  = acc7 (+ acc7 100)

assert "ticket7: nested for, multi-form outer body" acc7 (+ (* 3 2) (* 3 100))

= log7 (list)

for i 1 3:
  = log7 (cons (list 'in i) log7)
  for j 1 2:
    = log7 (cons (list 'inner i j) log7)
  = log7 (cons (list 'out i) log7)

assert "ticket7: outer body forms after nested for run" (length log7) 12

= n7 0

for k 1 5:
  prn ""
  = n7 (+ n7 1)

assert "ticket7: for after a NIL-returning builtin" n7 5

= s8a 0

do:
  = s8a (+ s8a 1)
  = s8a (+ s8a 10)
  = s8a (+ s8a 100)

assert "ticket8: do with mid-list comment" s8a 111

= s8b 0

def f8 ():
  = s8b (+ s8b 1)
  = s8b (+ s8b 10)
  = s8b (+ s8b 100)

f8()

assert "ticket8: def body with comment" s8b 111

= s8c 0

def f8c ():
  = s8c (+ s8c 1)

f8c()

assert "ticket8: leading comment in def body" s8c 1

= s8d 0

def f8d ():
  let _ 1:
    = s8d (+ s8d 1)

f8d()

assert "ticket8: comment inside let inside def" s8d 1

def step8 ():
  pr ""
  pr ""
  for i 1 3:
    pr ""

step8()

assert "ticket8: arkanoid-shaped step" 'ok 'ok

def alc001b-lit ():
  42

assert "alcove-001 B: def with literal body" (alc001b-lit) 42

def alc001b-nil ():
  nil

assert "alcove-001 B: def with nil body" (alc001b-nil) nil

def alc001b-sym (x):
  x

assert "alcove-001 B: def with bare-symbol body" (alc001b-sym 7) 7

assert "alcove-001 B: anon fn with literal body" ((fn () 99)) 99

def alc001a-pollute (a b c d e f):
  + a b c d e f

alc001a-pollute 1 2 3 4 5 6

def alc001a-let ():
  let x 1:
    let y 2:
      let z 3:
        + x y z

assert "alcove-001 A: nested let after pollution" (alc001a-let) 6

def alc001a-with ():
  with (p 10 q 20 r 30):
    + p q r

assert "alcove-001 A: with after pollution" (alc001a-with) 60

def alc001a-for ():
  let s 0:
    for i 1 5:
      = s (+ s i)
    s

assert "alcove-001 A: for-body after pollution" (alc001a-for) 15

def alc001a-outer (m):
  let n 100:
    + m n

assert "alcove-001 A: outer-symbol lookup through let body" (alc001a-outer 7):
  107

assert "alcove-002: (or)  -> nil" (or) nil

assert "alcove-002: (and) -> t" (and) t

assert "alcove-002: (when t)   no body -> nil" (when t) nil

assert "alcove-002: (when nil) no body -> nil" (when nil) nil

assert "alcove-002: (when t 42) -> 42" (when t 42) 42

assert "alcove-002: (when t 1 2 3) -> 3" (when t 1 2 3) 3

assert "alcove-002: (when t (+ 1 2)) -> 3" (when t (+ 1 2)) 3

assert "alcove-002: (when nil 42) -> nil" (when nil 42) nil

def alc002-helper (n):
  n

def alc002-and-leak ():
  when t:
    and (alc002-helper nil) 7

assert "alcove-002: tail-leak through (and) earlier arg" (alc002-and-leak) nil

def alc002-or-leak ():
  when t:
    or (alc002-helper nil) 7

assert "alcove-002: tail-leak through (or) earlier arg" (alc002-or-leak) 7

def vec->list (v):
  let xs (list):
    do:
      for i 0 (- (vec-len v) 1):
        = xs (cons (vec-ref v i) xs)
      reverse xs

= vd-a (vec 4 0)

vec-set! vd-a 0 1

vec-set! vd-a 1 2

vec-set! vd-a 2 3

vec-set! vd-a 3 4

= vd-b (vec 4 0)

vec-set! vd-b 0 5

vec-set! vd-b 1 6

vec-set! vd-b 2 7

vec-set! vd-b 3 8

assert "vec-dot: integer elements" (vec-dot vd-a vd-b) 70.0

= vd-c (vec 4 0.0)

vec-set! vd-c 0 1.0

vec-set! vd-c 1 2.0

vec-set! vd-c 2 3.0

vec-set! vd-c 3 4.0

= vd-d (vec 4 0.0)

vec-set! vd-d 0 0.5

vec-set! vd-d 1 1.5

vec-set! vd-d 2 -1.0

vec-set! vd-d 3 2.0

assert "vec-dot: float elements, exact sum" (vec-dot vd-c vd-d) 8.5

assert "vec-dot: empty vec is 0" (vec-dot (vec 0 0) (vec 0 0)) 0.0

= e1 (vec 3 0)

vec-set! e1 0 1

= e2 (vec 3 0)

vec-set! e2 1 1

assert "vec-dot: orthogonal vecs" (vec-dot e1 e2) 0.0

= ax-y (vec 4 0)

for i 0 3:
  vec-set! ax-y i 10

= ax-x (vec 4 0)

for i 0 3:
  vec-set! ax-x i (+ i 1)

vec-axpy! ax-y 3 ax-x

assert "vec-axpy!: integer scale + integer vecs" (vec->list ax-y):
  list 13.0 16.0 19.0 22.0

= ax-y2 (vec 3 7)

= ax-x2 (vec 3 99)

vec-axpy! ax-y2 0 ax-x2

assert "vec-axpy!: a=0 preserves y" (vec->list ax-y2) (list 7.0 7.0 7.0)

= ax-y3 (vec 3 0)

vec-set! ax-y3 0 10

vec-set! ax-y3 1 20

vec-set! ax-y3 2 30

= ax-x3 (vec 3 0)

vec-set! ax-x3 0 1

vec-set! ax-x3 1 2

vec-set! ax-x3 2 3

vec-axpy! ax-y3 -2 ax-x3

assert "vec-axpy!: negative scalar" (vec->list ax-y3) (list 8.0 16.0 24.0)

= ax-y4 (vec 2 1)

= ax-x4 (vec 2 1)

assert "vec-axpy!: returns y" (iso (vec-axpy! ax-y4 1 ax-x4) ax-y4) t

= sc-v (vec 4 0)

vec-set! sc-v 0 1

vec-set! sc-v 1 2

vec-set! sc-v 2 3

vec-set! sc-v 3 4

vec-scale! sc-v 5

assert "vec-scale!: integer scale" (vec->list sc-v) (list 5.0 10.0 15.0 20.0)

= sc-v2 (vec 3 0)

vec-set! sc-v2 0 2.0

vec-set! sc-v2 1 4.0

vec-set! sc-v2 2 8.0

vec-scale! sc-v2 0.5

assert "vec-scale!: halving" (vec->list sc-v2) (list 1.0 2.0 4.0)

= ad-y (vec 4 0)

for i 0 3:
  vec-set! ad-y i (+ i 1)

= ad-x (vec 4 0)

for i 0 3:
  vec-set! ad-x i (* i 10)

vec-add! ad-y ad-x

assert "vec-add!: element-wise sum" (vec->list ad-y) (list 1.0 12.0 23.0 34.0)

= fl-v (vec 5 99)

vec-fill! fl-v -1.5

assert "vec-fill!: float fill" (vec->list fl-v):
  list -1.5 -1.5 -1.5 -1.5 -1.5

= rl-v (vec 5 0)

vec-set! rl-v 0 -3

vec-set! rl-v 1 0

vec-set! rl-v 2 4

vec-set! rl-v 3 -0.5

vec-set! rl-v 4 2.5

vec-relu! rl-v

assert "vec-relu!: zeros negatives, preserves non-negatives" (vec->list rl-v):
  list 0.0 0 4 0.0 2.5

= rl-v2 (vec 3 0)

vec-set! rl-v2 0 -1

vec-set! rl-v2 1 -2

vec-set! rl-v2 2 -3

vec-relu! rl-v2

assert "vec-relu!: all-negative → all-zero" (vec->list rl-v2):
  list 0.0 0.0 0.0

= am-v (vec 5 0)

vec-set! am-v 0 1

vec-set! am-v 1 -3

vec-set! am-v 2 9

vec-set! am-v 3 7

vec-set! am-v 4 2

assert "vec-argmax: basic" (vec-argmax am-v) 2

= am-v2 (vec 4 0)

vec-set! am-v2 0 5

vec-set! am-v2 1 5

vec-set! am-v2 2 5

vec-set! am-v2 3 5

assert "vec-argmax: ties favor lowest index" (vec-argmax am-v2) 0

= mx-v (vec 5 0)

vec-set! mx-v 0 1.0

vec-set! mx-v 1 -3.0

vec-set! mx-v 2 9.0

vec-set! mx-v 3 7.0

vec-set! mx-v 4 2.0

assert "vec-max: float elements" (vec-max mx-v) 9.0

= mx-v2 (vec 3 0)

vec-set! mx-v2 0 1

vec-set! mx-v2 1 2.5

vec-set! mx-v2 2 2

assert "vec-max: mixed int/float picks the float" (vec-max mx-v2) 2.5

= cp-dst (vec 4 0)

= cp-src (vec 4 0)

vec-set! cp-src 0 10

vec-set! cp-src 1 20

vec-set! cp-src 2 30

vec-set! cp-src 3 40

vec-copy! cp-dst cp-src

assert "vec-copy!: contents match source" (vec->list cp-dst):
  list 10 20 30 40

vec-set! cp-src 0 999

assert "vec-copy!: no aliasing" (vec-ref cp-dst 0) 10

= grad-probs (vec 3 0)

vec-set! grad-probs 0 0.25

vec-set! grad-probs 1 0.5

vec-set! grad-probs 2 0.25

= grad-d (vec 3 0)

vec-copy! grad-d grad-probs

vec-set! grad-d 1 (- (vec-ref grad-d 1) 1.0)

assert "tensor combo: softmax-CE gradient" (vec->list grad-d):
  list 0.25 -0.5 0.25

= W-row0 (vec 2 0)

vec-set! W-row0 0 1

vec-set! W-row0 1 2

= W-row1 (vec 2 0)

vec-set! W-row1 0 3

vec-set! W-row1 1 4

= W-row2 (vec 2 0)

vec-set! W-row2 0 5

vec-set! W-row2 1 6

= mv-x (vec 2 0)

vec-set! mv-x 0 10

vec-set! mv-x 1 20

= mv-out (vec 3 0)

vec-set! mv-out 0 (vec-dot W-row0 mv-x)

vec-set! mv-out 1 (vec-dot W-row1 mv-x)

vec-set! mv-out 2 (vec-dot W-row2 mv-x)

assert "tensor combo: 3x2 matvec via per-row vec-dot" (vec->list mv-out):
  list 50.0 110.0 170.0

= cs-y (vec 2 0.0)

= cs-c (vec 3 0)

vec-set! cs-c 0 1

vec-set! cs-c 1 0

vec-set! cs-c 2 -1

vec-axpy! cs-y (vec-ref cs-c 0) W-row0

vec-axpy! cs-y (vec-ref cs-c 1) W-row1

vec-axpy! cs-y (vec-ref cs-c 2) W-row2

assert "tensor combo: column-weighted sum via axpy" (vec->list cs-y):
  list -4.0 -4.0

= dq (vec 0 nil)

vec-push! dq 1

vec-push! dq 2

vec-push! dq 3

assert "vec-push!: length after 3 pushes" (vec-len dq) 3

assert "vec-push!: contents" (vec->list dq) (list 1 2 3)

assert "vec-pop!: returns last" (vec-pop! dq) 3

assert "vec-pop!: length decreases" (vec-len dq) 2

assert "vec-pop!: contents after pop" (vec->list dq) (list 1 2)

vec-unshift! dq 0

assert "vec-unshift!: prepends" (vec->list dq) (list 0 1 2)

assert "vec-shift!: returns first" (vec-shift! dq) 0

assert "vec-shift!: contents after shift" (vec->list dq) (list 1 2)

= dq2 (vec 0 nil)

vec-push! dq2 10

vec-push! dq2 20

vec-push! dq2 30

assert "vec-shift! queue: first out" (vec-shift! dq2) 10

assert "vec-shift! queue: second out" (vec-shift! dq2) 20

assert "vec-shift! queue: third out" (vec-shift! dq2) 30

assert "vec-shift! queue: empty after" (vec-len dq2) 0

vec-pop! never-bound-for-pop

assert "vec-pop!: recovers after arg error" 42 42

vec-shift! never-bound-for-shift

assert "vec-shift!: recovers after arg error" 42 42

= vec-err (vector 1 'x 3)

vec-dot vec-err vec-err

assert "vec-dot: recovers after non-numeric error" 42 42

vec-relu! vec-err

assert "vec-relu!: recovers after non-numeric error" 42 42

vec-argmax vec-err

assert "vec-argmax: recovers after non-numeric error" 42 42

vec-max vec-err

assert "vec-max: recovers after non-numeric error" 42 42

vec-len 'not-a-vec

assert "vec-len: recovers after type error" 42 42

sqrt-int 'not-a-number

assert "sqrt-int: recovers after type error" 42 42

= dq4 (vec 0 nil)

for i 0 99:
  vec-push! dq4 i

assert "vec-push!: 100 pushes length" (vec-len dq4) 100

assert "vec-push!: first cell" (vec-ref dq4 0) 0

assert "vec-push!: last cell" (vec-ref dq4 99) 99

= dq5 (vec 0 nil)

for i 0 49:
  do:
    vec-push! dq5 i
    vec-shift! dq5
    vec-push! dq5 (+ i 1000)

assert "vec-push!/shift! alternation" (vec-len dq5) 50

= dqi (vec 0 0)

vec-push! dqi 1

vec-push! dqi 2

assert "vec-push! on fixnum-init vec" (vec-ref dqi 1) 2

assert "vec?: empty vec" (vec? (vec 0 0)) t

assert "vec?: populated vec" (vec? (vec 3 0)) t

assert "vec?: literal vec" (vec? (vector 1 2 3)) t

assert "vec?: nil" (vec? nil) nil

assert "vec?: list (cons-pair)" (vec? (list 1 2)) nil

assert "vec?: string" (vec? "x") nil

assert "vec?: number" (vec? 5) nil

assert "vec?: blob" (vec? (make-blob 1)) nil

assert "vec?: hash-map" (vec? (hash-map)) nil

assert "vec?: deque" (vec? (deque)) nil

assert "blob?: empty blob" (blob? (make-blob 0)) t

assert "blob?: populated blob" (blob? (make-blob 5)) t

assert "blob?: from string" (blob? (string->blob "x")) t

assert "blob?: nil" (blob? nil) nil

assert "blob?: string (not blob)" (blob? "abc") nil

assert "blob?: vec" (blob? (vec 1 0)) nil

assert "blob?: list" (blob? (list 1)) nil

redis-flush()

assert "redis-set string -> blob" (do (redis-set "alcove:test:string" "abc") (blob->string (redis-get "alcove:test:string"))):
  "abc"

assert "redis-set number -> blob" (do (redis-set "alcove:test:number" 42) (blob->string (redis-get "alcove:test:number"))):
  "42"

assert "redis-set deque type" (do (redis-set "alcove:test:list" (deque "a" (string->blob "b") 3)) (redis-type "alcove:test:list")):
  "list"

assert "redis-set hash-map type" (do (redis-set "alcove:test:hash" (hash-map "a" "b" "n" 9)) (redis-type "alcove:test:hash")):
  "hash"

assert "redis-val hash-map container" (dict? (redis-val "alcove:test:hash")) t

assert "redis-set vector type" (do (redis-set "alcove:test:vec" (vector 1 2 3)) (redis-type "alcove:test:vec")):
  "vector"

assert "redis-val vector container" (vec? (redis-val "alcove:test:vec")) t

assert "redis-val vector works with vec-dot" (vec-dot (redis-val "alcove:test:vec") (vector 1 1 1)):
  6.0

assert "redis-del removes key" (redis-del "alcove:test:string") 1

assert "redis-del missing" (redis-del "alcove:test:string") 0

redis-flush()

assert "dict?: empty hash-map" (dict? (hash-map)) t

assert "dict?: populated hash-map" (dict? (hash-map "k" 1)) t

assert "dict?: nil" (dict? nil) nil

assert "dict?: list" (dict? (list 1 2)) nil

assert "dict?: vec" (dict? (vec 1 0)) nil

assert "dict?: blob" (dict? (make-blob 1)) nil

assert "dict?: deque" (dict? (deque)) nil

assert "deque?: empty deque" (deque? (deque)) t

assert "deque?: populated deque" (deque? (deque 1 2 3)) t

assert "deque?: nil" (deque? nil) nil

assert "deque?: list (cons)" (deque? (list 1 2)) nil

assert "deque?: vec" (deque? (vec 1 0)) nil

assert "deque?: hash-map" (deque? (hash-map)) nil

assert "deque?: number" (deque? 0) nil

assert "vec?: no args" (vec?) nil

assert "blob?: no args" (blob?) nil

assert "dict?: no args" (dict?) nil

assert "deque?: no args" (deque?) nil

def bm-fib (n):
  if (< n 2):
    n
    + (bm-fib (- n 1)) (bm-fib (- n 2))

assert "bench fib 33" (bm-fib 33) 3524578

def bm-fact (n):
  if (< n 2):
    1
    * n (bm-fact (- n 1))

assert "bench fact 19" (bm-fact 19) 121645100408832000

def bm-countdown (n):
  if (> n 0):
    bm-countdown (- n 1)
    n

assert "bench countdown 1000" (bm-countdown 1000) 0

def bm-ack (m n):
  if (is m 0):
    + n 1
    if (is n 0):
      bm-ack (- m 1) 1
      bm-ack (- m 1) (bm-ack m (- n 1))

assert "bench ackermann 3 7" (bm-ack 3 7) 1021

def bm-forsum (n):
  let s 0:
    for i 1 n:
      = s (+ s 1)

assert "bench forsum 1000000" (bm-forsum 1000000) 1000000

def bm-build (i n acc):
  if (> i n):
    acc
    bm-build (+ i 1) n (cons i acc)

assert "bench listsum 1..100000" (reduce (fn (a b) (+ a b)) 0 (bm-build 1 100000 (list))):
  5000050000

def nq-safe? (c qs offset):
  if (no qs):
    t
    if (is c (car qs)):
      nil
      if (is (+ c offset) (car qs)):
        nil
        if (is (- c offset) (car qs)):
          nil
          nq-safe? c (cdr qs) (+ offset 1)

def nq-try-cols (n c qs row-acc):
  if (> c n):
    row-acc
    nq-try-cols n (+ c 1):
      qs
      if (nq-safe? c qs 1):
        + row-acc (nq-solve n (cons c qs))
        row-acc

def nq-solve (n qs):
  if (is (length qs) n):
    1
    nq-try-cols n 1 qs 0

assert "bench nqueens 10" (nq-solve 10 (list)) 724

def nqv-safe? (col qs row offset):
  if (>= offset row):
    t
    if (is col (vec-ref qs offset)):
      nil
      if (is (- col (- row offset)) (vec-ref qs offset)):
        nil
        if (is (+ col (- row offset)) (vec-ref qs offset)):
          nil
          nqv-safe? col qs row (+ offset 1)

def nqv-try-cols (n row col qs count):
  if (>= col n):
    count
    if (nqv-safe? col qs row 0):
      do:
        vec-set! qs row col
        nqv-try-cols n row (+ col 1) qs (+ count (nqv-solve n (+ row 1) qs))
      nqv-try-cols n row (+ col 1) qs count

def nqv-solve (n row qs):
  if (>= row n):
    1
    nqv-try-cols n row 0 qs 0

assert "bench nqueens-vec 10" (nqv-solve 10 0 (vec 10 0)) 724

def sv-is-prime-given (acc i):
  if (no acc):
    t
    if (is (mod i (car acc)) 0):
      nil
      sv-is-prime-given (cdr acc) i

def sv-primes-up-to (i n acc):
  if (> i n):
    acc
    if (sv-is-prime-given acc i):
      sv-primes-up-to (+ i 1) n (cons i acc)
      sv-primes-up-to (+ i 1) n acc

assert "bench sieve up to 5000" (length (sv-primes-up-to 2 5000 (list))) 669

def sf-mark-composites (i n marks):
  if (> (* i i) n):
    nil
    do:
      if (vec-ref marks i):
        sf-mark-from i (* i i) n marks
        nil
      sf-mark-composites (+ i 1) n marks

def sf-mark-from (step j n marks):
  if (> j n):
    nil
    do:
      vec-set! marks j nil
      sf-mark-from step (+ j step) n marks

def sf-count-primes (i n marks acc):
  if (> i n):
    acc
    sf-count-primes (+ i 1) n marks (if (vec-ref marks i) (+ acc 1) acc)

= sf-n 100000

= sf-marks (vec (+ sf-n 1) t)

sf-mark-composites 2 sf-n sf-marks

assert "bench sieve-fast up to 100000" (sf-count-primes 2 sf-n sf-marks 0):
  9592

def bm-tak (x y z):
  if (no (< y x)):
    z
    bm-tak (bm-tak (- x 1) y z) (bm-tak (- y 1) z x) (bm-tak (- z 1) x y)

assert "bench tak 24 16 8" (bm-tak 24 16 8) 9
