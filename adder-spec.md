# Adder Specification

## 1. Goal

Adder is a whitespace-sensitive surface syntax for Lisp.

It allows code like:

```python
def assert(txt a b):
  pr txt
  if (iso a b):
    prn "Passed"
    prn "Failed"
```

to be read as:

```lisp
(def assert (txt a b)
  (pr txt)
  (if (iso a b)
      (prn "Passed")
      (prn "Failed")))
```

The language remains homoiconic because the source code is read into ordinary Lisp data structures before macro expansion or evaluation.

The reader produces forms such as:

```python
[
  Symbol("def"),
  Symbol("assert"),
  [Symbol("txt"), Symbol("a"), Symbol("b")],
  [Symbol("pr"), Symbol("txt")],
  [
    Symbol("if"),
    [Symbol("iso"), Symbol("a"), Symbol("b")],
    [Symbol("prn"), "Passed"],
    [Symbol("prn"), "Failed"]
  ]
]
```

The syntax is therefore not Python. It is a different reader notation for Lisp forms.

---

## 2. Core Principle

The syntax must follow this rule:

```text
Source text -> reader -> Lisp forms -> macro expansion -> evaluation/compilation
```

Not:

```text
Source text -> opaque Python-like AST -> compiler
```

This is essential.

`def`, `if`, `macro`, `let`, `quote`, and other constructs are not parser-level AST node types. They are symbols in normal lists.

For example:

```python
if (iso a b):
  prn "yes"
  prn "no"
```

is not parsed as:

```python
IfNode(condition=..., then=..., else=...)
```

It is parsed as:

```python
[
  Symbol("if"),
  [Symbol("iso"), Symbol("a"), Symbol("b")],
  [Symbol("prn"), "yes"],
  [Symbol("prn"), "no"]
]
```

---

## 3. Lexical Elements

### 3.1 Symbols

A bare word is a symbol.

```python
foo
```

reads as:

```lisp
foo
```

Examples:

```python
pr txt
```

reads as:

```lisp
(pr txt)
```

with internal representation:

```python
[Symbol("pr"), Symbol("txt")]
```

Symbols may include letters, digits, and common Lisp operator characters:

```text
+ - * / = < > <= >= != ? ! _ - . :
```

Examples:

```python
+ a b
iso a b
empty? xs
user.name
http:get url
```

---

### 3.2 Strings

Double-quoted text is a string literal.

```python
prn "Passed"
```

reads as:

```lisp
(prn "Passed")
```

Internal representation:

```python
[Symbol("prn"), "Passed"]
```

Escapes are preserved:

```python
prn "\t: \x1B[92mPassed\x1B[39m"
```

reads as:

```lisp
(prn "\t: \x1B[92mPassed\x1B[39m")
```

---

### 3.3 Numbers

Integer and floating-point literals read as numbers.

```python
+ 1 2
```

reads as:

```lisp
(+ 1 2)
```

Examples:

```python
42
-10
3.14
-0.5
```

---

### 3.4 Booleans and Nil

Suggested literals:

```python
true
false
nil
```

Recommended mapping for Common Lisp-style output:

```text
true  -> t
false -> nil
nil   -> nil
```

---

## 4. Forms

### 4.1 Every Non-Empty Line Is a Form

A normal line:

```python
pr txt
```

reads as:

```lisp
(pr txt)
```

A line is split into top-level tokens by spaces.

So:

```python
+ a b
```

reads as:

```lisp
(+ a b)
```

Internal representation:

```python
[Symbol("+"), Symbol("a"), Symbol("b")]
```

---

### 4.2 A Colon Opens a Block

A line ending with `:` opens a block.

```python
foo a b:
  bar c
  baz d
```

reads as:

```lisp
(foo a b
  (bar c)
  (baz d))
```

Internal representation:

```python
[
  Symbol("foo"),
  Symbol("a"),
  Symbol("b"),
  [Symbol("bar"), Symbol("c")],
  [Symbol("baz"), Symbol("d")]
]
```

So the rule is:

```text
line:
  indented-form-1
  indented-form-2
```

becomes:

```lisp
(line indented-form-1 indented-form-2)
```

The colon does not mean Python control flow. It means:

```text
append the following indented forms to this list
```

---

### 4.3 Indentation Closes Forms

Indentation determines when a block ends.

This:

```python
foo:
  bar
  baz
qux
```

reads as:

```lisp
(foo
  (bar)
  (baz))

(qux)
```

The dedent before `qux` closes the `foo` form.

---

### 4.4 More Indentation Means Nested Forms

This:

```python
foo:
  bar:
    baz
```

reads as:

```lisp
(foo
  (bar
    (baz)))
```

Internal representation:

```python
[
  Symbol("foo"),
  [
    Symbol("bar"),
    [Symbol("baz")]
  ]
]
```

---

## 5. Inline Parentheses

Inline parentheses are normal Lisp forms.

```python
if (iso a b):
  prn "Passed"
  prn "Failed"
```

reads as:

```lisp
(if (iso a b)
    (prn "Passed")
    (prn "Failed"))
```

Internal representation:

```python
[
  Symbol("if"),
  [Symbol("iso"), Symbol("a"), Symbol("b")],
  [Symbol("prn"), "Passed"],
  [Symbol("prn"), "Failed"]
]
```

Parentheses can be nested:

```python
prn (+ (* 2 x) 1)
```

reads as:

```lisp
(prn (+ (* 2 x) 1))
```

---

## 6. Function-Call Sugar

To make definitions pleasant, the reader may support this sugar:

```python
name(arg1 arg2 arg3)
```

which reads as:

```lisp
(name arg1 arg2 arg3)
```

So:

```python
assert(txt a b)
```

reads as:

```lisp
(assert txt a b)
```

This is reader sugar only.

Therefore:

```python
def assert(txt a b):
  body
```

reads as:

```lisp
(def assert (txt a b)
  body)
```

This rule is special only in shape, not semantics. The macro system does not see the surface syntax. It sees only the final list.

---

## 7. Definitions

A function definition is written:

```python
def name(args):
  body...
```

Example:

```python
def assert(txt a b):
  pr txt
  if (iso a b):
    prn "Passed"
    prn "Failed"
```

reads as:

```lisp
(def assert (txt a b)
  (pr txt)
  (if (iso a b)
      (prn "Passed")
      (prn "Failed")))
```

Internal representation:

```python
[
  Symbol("def"),
  Symbol("assert"),
  [Symbol("txt"), Symbol("a"), Symbol("b")],
  [Symbol("pr"), Symbol("txt")],
  [
    Symbol("if"),
    [Symbol("iso"), Symbol("a"), Symbol("b")],
    [Symbol("prn"), "Passed"],
    [Symbol("prn"), "Failed"]
  ]
]
```

The reader does not need to know what `def` means. It only converts syntax to forms.

The compiler or Lisp backend may then map:

```lisp
(def assert (txt a b) ...)
```

to:

```lisp
(defun assert (txt a b) ...)
```

if targeting Common Lisp.

---

## 8. Conditionals

A conditional is written:

```python
if condition:
  then-expression
  else-expression
```

Example:

```python
if (iso a b):
  prn "Passed"
  prn "Failed"
```

reads as:

```lisp
(if (iso a b)
    (prn "Passed")
    (prn "Failed"))
```

The reader does not enforce that `if` has exactly two or three arguments. It simply produces a list.

So this:

```python
if condition:
  then
```

reads as:

```lisp
(if condition
    (then))
```

And this:

```python
if condition:
  a
  b
  c
```

reads as:

```lisp
(if condition
    (a)
    (b)
    (c))
```

Whether that is valid depends on the target Lisp semantics.

---

## 9. Sequential Blocks

A block under a form becomes multiple arguments to that form.

Example:

```python
do:
  prn "one"
  prn "two"
  prn "three"
```

reads as:

```lisp
(do
  (prn "one")
  (prn "two")
  (prn "three"))
```

For function bodies:

```python
def main():
  prn "hello"
  prn "world"
```

reads as:

```lisp
(def main ()
  (prn "hello")
  (prn "world"))
```

The meaning of multiple body expressions belongs to `def`, not the reader.

---

## 10. Quote

Quote should preserve code as data.

Suggested syntax:

```python
quote:
  if (iso a b):
    prn "Passed"
    prn "Failed"
```

reads as:

```lisp
(quote
  (if (iso a b)
      (prn "Passed")
      (prn "Failed")))
```

You may also support shorthand:

```python
'foo
```

reads as:

```lisp
(quote foo)
```

And:

```python
'(a b c)
```

reads as:

```lisp
(quote (a b c))
```

But for the first version of the language, explicit `quote` is simpler and cleaner.

---

## 11. Macros

Macros are normal forms that receive unevaluated forms.

Example:

```python
macro when(cond body):
  list 'if cond body nil
```

reads as:

```lisp
(macro when (cond body)
  (list (quote if) cond body nil))
```

Then:

```python
when (> x 10):
  prn "large"
```

reads first as:

```lisp
(when (> x 10)
  (prn "large"))
```

Macro expansion can transform it into:

```lisp
(if (> x 10)
    (prn "large")
    nil)
```

This is the essential homoiconic property:

```text
code is represented as lists
macros receive lists
macros return lists
the returned lists are evaluated/compiled
```

---

## 12. Let Bindings

A `let` can be written using direct Lisp-compatible syntax:

```python
let ((x 10) (y 20)):
  + x y
```

reads as:

```lisp
(let ((x 10) (y 20))
  (+ x y))
```

This is recommended for the first version because it maps directly to Lisp.

An alternative indentation-only form is possible:

```python
let:
  bindings:
    x 10
    y 20
  + x y
```

which reads as:

```lisp
(let
  (bindings
    (x 10)
    (y 20))
  (+ x y))
```

Then a macro can rewrite it.

---

## 13. Lists and Data

Inline list:

```python
quote (1 2 3)
```

reads as:

```lisp
(quote (1 2 3))
```

Function call:

```python
list 1 2 3
```

reads as:

```lisp
(list 1 2 3)
```

Quoted code:

```python
quote:
  + 1 2
```

reads as:

```lisp
(quote
  (+ 1 2))
```

That means code and data use the same representation.

---

## 14. Comments

Suggested comment syntax:

```python
# this is a comment
```

Comments are ignored by the reader.

Example:

```python
def main():
  # print greeting
  prn "hello"
```

reads as:

```lisp
(def main ()
  (prn "hello"))
```

Comments inside strings are preserved:

```python
prn "# not a comment"
```

reads as:

```lisp
(prn "# not a comment")
```

---

## 15. Multiple Top-Level Forms

A file may contain several top-level forms.

```python
def square(x):
  * x x

def main():
  prn (square 5)
```

reads as:

```lisp
(def square (x)
  (* x x))

(def main ()
  (prn (square 5)))
```

The reader returns a list of top-level forms:

```python
[
  [Symbol("def"), Symbol("square"), [Symbol("x")], [Symbol("*"), Symbol("x"), Symbol("x")]],
  [Symbol("def"), Symbol("main"), [], [Symbol("prn"), [Symbol("square"), 5]]]
]
```

---

## 16. Conversion Rules to Lisp

### 16.1 Basic Line

Adder:

```python
foo a b
```

Lisp:

```lisp
(foo a b)
```

---

### 16.2 Block

Adder:

```python
foo a:
  bar b
  baz c
```

Lisp:

```lisp
(foo a
  (bar b)
  (baz c))
```

---

### 16.3 Nested Block

Adder:

```python
foo:
  bar:
    baz
```

Lisp:

```lisp
(foo
  (bar
    (baz)))
```

---

### 16.4 Inline Form

Adder:

```python
prn (+ 1 2)
```

Lisp:

```lisp
(prn (+ 1 2))
```

---

### 16.5 Function Definition

Adder:

```python
def add(a b):
  + a b
```

Generic Lisp:

```lisp
(def add (a b)
  (+ a b))
```

Common Lisp target:

```lisp
(defun add (a b)
  (+ a b))
```

---

### 16.6 If

Adder:

```python
if (> x 0):
  prn "positive"
  prn "not positive"
```

Lisp:

```lisp
(if (> x 0)
    (prn "positive")
    (prn "not positive"))
```

---

### 16.7 Function Call Sugar

A name glued directly to `(...)` is a CALL of any arity (matching §6):

```python
foo(bar baz)
```

reads as:

```lisp
(foo bar baz)
```

This is uniform with the no-arg case (`foo()` → `(foo)`) and with nesting
(`foo(bar(baz))` → `(foo (bar baz))`).

The one exception is **definition position**: directly after a binder keyword
(`def`/`defn`/`defc`/`defmacro`/`macro`), or when the name is itself
`fn`/`lambda`, the glued `(...)` is a PARAMETER LIST, not a call:

```python
def foo(a b):
```

is interpreted as:

```lisp
(def foo (a b) ...)
```

The spaced form is equivalent and always reads as a parameter list, so it
remains available when you prefer maximum homoiconicity:

```python
def foo (a b):
  body
```

```lisp
(def foo (a b)
  body)
```

Note: the call sugar applies to a name glued to `(...)`. A form whose first
argument must be a literal list — e.g. `let`'s binding list — must use the
spaced form (`let ((x 10) (y 20)): ...`); writing `let((x 10) (y 20))` would
be read as the call `(let (x 10) (y 20))`.

---

## 17. Recommended Canonical Syntax

To preserve maximum homoiconicity, the canonical syntax should be:

```python
def assert (txt a b):
  pr txt
  if (iso a b):
    prn "Passed"
    prn "Failed"
```

instead of:

```python
def assert(txt a b):
  ...
```

Because:

```python
def assert (txt a b):
```

maps directly to:

```lisp
(def assert (txt a b) ...)
```

No special function-definition sugar is needed.

However, the Python-like version can still be accepted as reader sugar:

```python
def assert(txt a b):
```

and normalized to:

```python
def assert (txt a b):
```

before macro expansion.

---

## 18. Reader Algorithm

The reader works in five phases.

### Phase 1: Remove comments and blank lines

Input:

```python
def main():
  # greeting
  prn "hello"
```

Output lines:

```python
def main():
  prn "hello"
```

---

### Phase 2: Track indentation

Each line becomes:

```python
Line(indent=0, text="def main()", opens_block=True)
Line(indent=2, text='prn "hello"', opens_block=False)
```

---

### Phase 3: Parse each line into an inline form

```python
def main()
```

becomes:

```python
[Symbol("def"), Symbol("main"), []]
```

And:

```python
prn "hello"
```

becomes:

```python
[Symbol("prn"), "hello"]
```

---

### Phase 4: Attach indented forms to parent forms

Input:

```python
def main():
  prn "hello"
```

Tree:

```python
[
  Symbol("def"),
  Symbol("main"),
  [],
  [Symbol("prn"), "hello"]
]
```

---

### Phase 5: Emit Lisp

Tree:

```python
[
  Symbol("def"),
  Symbol("main"),
  [],
  [Symbol("prn"), "hello"]
]
```

Generic Lisp:

```lisp
(def main ()
  (prn "hello"))
```

Common Lisp backend:

```lisp
(defun main ()
  (prn "hello"))
```

---

## 19. Target Lisp Mapping

The reader produces generic Lisp-like forms.

Then a backend can map generic names to target Lisp names.

Suggested mappings for Common Lisp:

```text
def      -> defun
macro    -> defmacro
fn       -> lambda
true     -> t
false    -> nil
nil      -> nil
do       -> progn
set      -> setf
```

Example source:

```python
def hello(name):
  prn "Hello"
  prn name
```

Generic output:

```lisp
(def hello (name)
  (prn "Hello")
  (prn name))
```

Common Lisp output:

```lisp
(defun hello (name)
  (prn "Hello")
  (prn name))
```

---

## 20. Homoiconicity Requirements

To keep the language homoiconic, the implementation must obey these rules.

### Rule 1: The reader outputs plain data

Good:

```python
[Symbol("if"), condition, then_form, else_form]
```

Bad:

```python
IfNode(condition, then_form, else_form)
```

---

### Rule 2: Macros receive the same forms the reader outputs

A macro should receive:

```python
[Symbol("when"), [Symbol(">"), Symbol("x"), 0], [Symbol("prn"), "large"]]
```

not an opaque compiler object.

---

### Rule 3: Macros return forms

A macro expansion should return:

```python
[Symbol("if"), [Symbol(">"), Symbol("x"), 0], [Symbol("prn"), "large"], None]
```

which then gets compiled or evaluated.

---

### Rule 4: Reader sugar disappears before macro expansion

Source:

```python
def assert(txt a b):
  ...
```

must become:

```python
[Symbol("def"), Symbol("assert"), [Symbol("txt"), Symbol("a"), Symbol("b")], ...]
```

before macros see it.

---

### Rule 5: Code and quoted code have the same representation

This:

```python
quote:
  + 1 2
```

must produce data equivalent to:

```python
[Symbol("+"), 1, 2]
```

not a string, not a special AST object.

---

## 21. Full Example

Source:

```python
def assert(txt a b):
  pr txt
  if (iso a b):
    prn "\t: \x1B[92mPassed\x1B[39m"
    prn "\t: \x1B[91mFailed\x1B[39m"
```

Reader output:

```python
[
  Symbol("def"),
  Symbol("assert"),
  [Symbol("txt"), Symbol("a"), Symbol("b")],
  [Symbol("pr"), Symbol("txt")],
  [
    Symbol("if"),
    [Symbol("iso"), Symbol("a"), Symbol("b")],
    [Symbol("prn"), "\t: \x1B[92mPassed\x1B[39m"],
    [Symbol("prn"), "\t: \x1B[91mFailed\x1B[39m"]
  ]
]
```

Generic Lisp output:

```lisp
(def assert (txt a b)
  (pr txt)
  (if (iso a b)
      (prn "\t: \x1B[92mPassed\x1B[39m")
      (prn "\t: \x1B[91mFailed\x1B[39m")))
```

Common Lisp-style output:

```lisp
(defun assert (txt a b)
  (pr txt)
  (if (iso a b)
      (prn "\t: \x1B[92mPassed\x1B[39m")
      (prn "\t: \x1B[91mFailed\x1B[39m")))
```

---

## 22. Summary

The language should be specified as:

```text
Adder is an indentation-sensitive reader syntax for Lisp forms.

A line becomes a list.
A colon appends an indented block to that list.
Parentheses create inline forms.
Bare words are symbols.
Strings are strings.
Numbers are numbers.
Macros operate on the same lists produced by the reader.
```

The most important design sentence is:

> The reader may be Python-like, but the language core is Lisp-like data.

That is how the language keeps homoiconicity.
