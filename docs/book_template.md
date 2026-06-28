# The Alcove & Adder Reference Manual: A Technical Deep Dive

Welcome to the definitive programmer's reference and architecture manual for the **Alcove** and **Adder** languages. This book is generated dynamically: every single code block is executed by the actual compiled `alcove` and `adder` binaries, verifying both compiler correctness and execution output in real-time.

---

## Chapter 1: VM Architecture & Memory Design

### 1.1 One VM, Two Symmetrical Dialects
Alcove and Adder are two surface syntaxes compiling to the same register-based bytecode Virtual Machine:
*   **Alcove** represents the classic Lisp-style S-expression syntax.
*   **Adder** is a Python-inspired block-based syntax that compiles down to the exact same S-expressions and runs on the same VM.

Because they compile to the same underlying S-expressions, they share the same global namespace, the same FFI bindings, the same garbage collection behaviors, and can transparently import (`require`) and call each other.

### 1.2 Pointer Tagging Layout
On 64-bit platforms, allocations returned by `malloc` are aligned to 8 bytes, which leaves the lowest 3 bits of a pointer address as `000`. Alcove uses this alignment to store immediate values (fixnums and characters) directly inside the pointer variable (`exp_t *`) itself, skipping the heap:
*   `000` (`TAG_PTR`): Standard heap-allocated node (e.g. lists, dicts, vectors).
*   `001` (`TAG_FIX`): 61-bit signed immediate integer.
*   `010` (`TAG_CHAR`): Unicode character immediate.

```
+-------------------------------------------------------------+---+---+---+
|    (TAG_PTR)          Pointer Address (61 bits)             | 0 | 0 | 0 |
+-------------------------------------------------------------+---+---+---+
|    (TAG_FIX)          Signed Integer Value (61 bits)        | 0 | 0 | 1 |
+-------------------------------------------------------------+---+---+---+
|    (TAG_CHAR)         Unicode Character Code (61 bits)      | 0 | 1 | 0 |
+-------------------------------------------------------------+---+---+---+
```

This immediate value tagging avoids memory allocation, refcount tracking, and collection overhead.

### 1.3 Memory Management & Teardown
Alcove uses a fast-path reference counting model. To handle cycles formed by local closures capturing their parent frames, the VM implements a cycle-breaking pass (`env_break_self_cycle`) on environment destruction. For user-defined structures, `(heap-stats)` is provided to track live allocation cells.

---

## Chapter 2: Language Dialect Syntax Comparisons

### 2.1 Symmetrical Comparison
Let's see how identical expressions are written in Lisp (Alcove) vs Python-style (Adder).

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(+ 1 2)
(is 1 1)
```

#### Adder
<!-- exec: adder -->
```
1 + 2
1 is 1
```

### 2.2 Infix Notation Rewriting Mechanics
Unlike traditional Lisp, Alcove features infix rewriting. 
*   **Compile-time**: An expression `(A op B)` where `op` is a known operator is compiled as `(op A B)` if `A` is statically non-callable (e.g., a literal, symbol, or type-hinted parameter).
*   **Runtime**: If the compiler cannot determine callability statically, it compiles a generic call. At runtime, the evaluator and VM dispatch `(A op B)` as `(op A B)` if `A` evaluates to a non-callable value.

---

## Chapter 3: Deep Dive into Core Data Types

### 3.1 Numbers Tower
Alcove supports 61-bit fixnums, IEEE-754 double floats, exact fractional Ratios, and fixed-precision Decimals.

#### Alcove (Lisp)
<!-- exec: alcove -->
```
(/ 1 3)
(rational? 1/3)
(decimal "1.25")
(decimal? (decimal "1.25"))
```

#### Adder
<!-- exec: adder -->
```
1/3
rational?(1/3)
decimal("1.25")
decimal?(decimal("1.25"))
```

### 3.2 Native Collections
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

### 3.3 Stateful Generators
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

---

## Chapter 4: Advanced Scopes, Control Flows, & Recursion

### 4.1 Local Bindings & Assignment
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

### 4.2 Conditionals & Pattern Matching
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

### 4.3 Loops & Recursion
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
  while (i < 3):
    prn(i)
    i = i + 1
```

### 4.4 Escape Continuations (call/cc & defc)
Escape continuations allow capturing the current control flow, enabling early returns, custom aborts, and exception handling.

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
    if (x is val):
      return(t)
  nil
search(42 list(10 42 100))
```

---

## Chapter 5: Metaprogramming (Macros)

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

---

## Chapter 6: Polymorphism (Structs & Multimethods)

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
p = point(3 4)
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

## Chapter 7: High-Performance JIT Compiler

The VM contains an integrated JIT compiler that generates native instructions (AMD64/ARM64) at runtime.
It accelerates recursion patterns like Ackerman loops, simple step loops, and Fibonacci layouts:
*   **W^X Compliance**: Pages are mapped Write-only during compilation, then switched to Read/Execute before invocation.
*   **Fast Call paths**: Skips frame creation overhead for tight numeric loops.

---

## Chapter 8: Systems Integration & FFI

### 8.1 Foreign Function Interface (FFI)
Alcove supports loading shared libraries (.so / .dylib) dynamically, mapping C structures, and exporting Lisp procedures as C callback function pointers.

### 8.2 Safe Mode Sandbox
Invoking `./alcove --safe` enables a restrictive sandbox:
*   Blocks all raw filesystem and network calls.
*   Bypasses to unsafe builtins (like FFI or shell command gates) throw an immediate security exception.
