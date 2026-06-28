# The Alcove & Adder Programming Manual: A Deep Dive into High-Performance Lisp and Python-Like Dialect VM

Welcome to the definitive guide to **Alcove** and **Adder**. This book is built programmatically: every single code block is executed by the actual compiled `alcove` and `adder` binaries, verifying both syntax and runtime output in real-time.

---

## Chapter 1: Introduction & Architecture

### 1.1 One VM, Two Dialects
Alcove and Adder are two complementary frontends compiling to the same register-based bytecode virtual machine. 
*   **Alcove** represents the classic Lisp-style S-expression syntax.
*   **Adder** is a Python-inspired block-based syntax that compiles down to the exact same S-expressions and runs on the same VM.

Because they compile to the same underlying S-expressions, they share the same global namespace, the same FFI bindings, the same garbage collection behaviors, and can transparently import (`require`) and call each other.

### 1.2 Pointer Tagging Layout
On 64-bit platforms, allocations returned by `malloc` are aligned to 8 bytes, which leaves the lowest 3 bits of a pointer address as `000`. Alcove uses this alignment to store immediate values (fixnums and characters) directly inside the pointer variable (`exp_t *`) itself, skipping the heap:
*   `000` (`TAG_PTR`): Standard heap-allocated node (e.g. lists, dicts, vectors).
*   `001` (`TAG_FIX`): 61-bit signed immediate integer.
*   `010` (`TAG_CHAR`): Unicode character immediate.

This immediate value tagging avoids memory allocation, refcount tracking, and collection overhead.

### 1.3 Memory Management & Teardown
Alcove uses a fast-path reference counting model. To handle cycles formed by local closures capturing their parent frames, the VM implements a cycle-breaking pass (`env_break_self_cycle`) on environment destruction. For user-defined structures, `(heap-stats)` is provided to track live allocation cells.

---

## Chapter 2: Data Types & Collections

### 2.1 Numbers and Characters
Alcove features fixnums, floats, ratios (exact fractions), and decimals.

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(+ 1 2)
(/ 1 3)
(rational? 1/3)
(decimal "1.25")
#\A
(char? #\A)
```

#### Adder
<!-- exec: adder -->
```
1 + 2
1/3
rational?(1/3)
decimal("1.25")
#\A
char?(#\A)
```

### 2.2 Collections
Alcove provides multiple native collections:
*   **Lists**: Linked cons cells.
*   **Vectors**: Flat arrays with double-ended queue operations.
*   **Hash Maps**: Mutable hash tables.
*   **Persistent Maps**: Immutable Hash Array Mapped Tries (`hamt`).
*   **Blobs**: Byte arrays.

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(list 1 2 3)
(vec 10 20 30)
(hash-map "name" "alcove" "version" 2)
(hamt "a" 1 "b" 2)
(make-blob "hello")
```

#### Adder
<!-- exec: adder -->
```
list(1 2 3)
vec(10 20 30)
hash-map("name" "adder" "version" 2)
hamt("a" 1 "b" 2)
make-blob("hello")
```

---

## Chapter 3: Control Flow & Pattern Matching

### 3.1 Local Bindings & Assignment
We can bind variables locally using `let` or `=` assignment.

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(let (x 10 y 20) (+ x y))
```

#### Adder
<!-- exec: adder -->
```
let (x 10 y 20):
  x + y
```

### 3.2 Conditionals & Pattern Matching
We can use `if`, `cond`, `case`, and structural pattern matching `match`.

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(cond (is 1 2) "no" (is 1 1) "yes")

(match (list 1 2) (list x y) (+ x y) "fallback")
```

#### Adder
<!-- exec: adder -->
```
cond:
  1 is 2
  "no"
  1 is 1
  "yes"

match list(1 2):
  list(x y)
  x + y
  _
  "fallback"
```

### 3.3 Loops
Supported loop constructs include `while`, `repeat`, `for`, and `each`.

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(let (i 0)
  (while (< i 3)
    (prn i)
    (= i (+ i 1))))
```

#### Adder
<!-- exec: adder -->
```
let i 0:
  while i < 3:
    prn(i)
    i = i + 1
```

---

## Chapter 4: Functions & Polymorphism

### 4.1 Functions and Multi-arity
Functions are defined via `def` (single-arity) or `defn` (multi-arity).

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(defn area
  ((r) (* 3.14 r r))
  ((w h) (* w h)))
(area 5)
(area 4 5)
```

#### Adder
<!-- exec: adder -->
```
defn area:
  ((r) (* 3.14 r r))
  ((w h) (* w h))
area(5)
area(4 5)
```

### 4.2 Escape Continuations
Continuations can be captured via `call/cc` or early returns via `defc`.

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(defc search (val lst)
  (each x lst
    (if (is x val) (return t)))
  nil)
(search 42 (list 10 42 100))
```

#### Adder
<!-- exec: adder -->
```
defc search (val lst):
  each x lst:
    if x is val:
      return(t)
  nil
search(42 list(10 42 100))
```

### 4.3 Macros
Macros are defined using `defmacro`. They manipulate S-expressions and expand at compile time.

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(defmacro when (cond body)
  (list 'if cond body nil))
(macroexpand-1 '(when t (prn "hello")))
```

#### Adder
<!-- exec: adder -->
```
defmacro when (cond body):
  list('if cond body nil)
macroexpand-1('(when t prn("hello")))
```

### 4.4 Structs & Multimethods
Records are defined via `defstruct`. Custom type polymorphism is supported via `defmulti` and `defmethod`.

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(defstruct point x y)
(= p (point 3 4))
(point-x p)
(point? p)

(defmulti greet type-of)
(defmethod greet 'point (p) "hello point!")
(defmethod greet 'int (n) "hello int!")
(greet p)
(greet 42)
```

#### Adder
<!-- exec: adder -->
```
defstruct point x y
setf p (point 3 4)
point-x p
point? p

defmulti greet type-of
defmethod greet 'point (p):
  "hello point!"
defmethod greet 'int (n):
  "hello int!"
greet(p)
greet(42)
```

---

## Chapter 5: Advanced Systems

### 5.1 Stateful Generators
Generators provide stateful yield-style lazy sequence iteration.

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(setf g (gen-range 1 5))
(gen-next! g)
(gen-next! g)
```

#### Adder
<!-- exec: adder -->
```
g = gen-range(1 5)
gen-next!(g)
gen-next!(g)
```

### 5.2 JSON Serialization
JSON encoding and decoding is built-in.

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(json-encode (hash-map "name" "alcove" "score" 99))
(json-decode "{\"name\":\"alcove\",\"score\":99}")
```

#### Adder
<!-- exec: adder -->
```
json-encode(hash-map("name" "adder" "score" 99))
json-decode("{\"name\":\"adder\",\"score\":99}")
```
