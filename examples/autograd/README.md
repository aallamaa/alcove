# Autograd in alcove

A small **reverse-mode automatic differentiation** engine, written in alcove on
top of the F64 tensor ops and the fused dense-layer kernels (`mat-vec!`,
`mat-vec-t!`, `vec-ger!`). You write the forward pass; `(backward loss)` fills in
every gradient by walking the computation graph in reverse.

```
examples/autograd/
  autograd.alc        the engine (~130 lines): a `tensor` type + ops + backward
  test-autograd.alc   gradient check (analytic vs finite-difference) + train test
  mlp-autograd.alc    the 64-32-10 digit MLP with FULLY AUTOMATIC backprop
```

## The idea

A `tensor` wraps its data (an F64 vec — a vector or a flat row-major matrix), a
`grad` vec, a `backward` thunk, and the list of input tensors it came from. Each
op builds a new tensor and records how to push its gradient back into its inputs:

| op                         | forward            | backward                          |
|----------------------------|--------------------|-----------------------------------|
| `(t+ a b)`                 | a + b              | da += dy, db += dy                |
| `(t-matvec W x)`           | W·x                | dW += dy⊗x (`vec-ger!`), dx += Wᵀ·dy (`mat-vec-t!`) |
| `(t-relu x)`               | max(0, x)          | dx += dy where x>0                |
| `(t-softmax-ce logits t)`  | −log softmax(t)    | d(logits) += softmax − onehot     |

`(backward loss)` does a post-order DFS to get a reverse-topological order, seeds
`dL/dL = 1`, and runs each node's `backward` so a gradient is complete before it
propagates. Gradients accumulate in place (`vec-add!`), so a value used in several
places gets the sum of its contributions.

## Run it

```sh
make -C ../mlp data                                  # one-time: fetch digits.bin
./alcove --noload examples/autograd/test-autograd.alc   # gradient check (AUTOGRAD: OK)
./alcove --noload examples/autograd/mlp-autograd.alc    # train the digit MLP
```

The gradient check compares every op's analytic gradient against a central
finite difference — if reverse-mode and numeric agree to ~1e-3 on random inputs,
the backward rules are right.

## Two ways to train the same net

- **`examples/mlp/mlp.alc`** — hand-codes the backward pass with `mat-vec-t!` /
  `vec-ger!`. Fastest (~22 ms/epoch); you write the gradients.
- **`mlp-autograd.alc`** — writes only `logits = W2·relu(W1·x + b1) + b2` and lets
  `backward` do the rest (~220 ms/epoch). You write no gradient code at all.

Both reach ~95% test accuracy on UCI optdigits. Autograd trades speed (it rebuilds
the graph each step) for never hand-deriving a gradient.

## What this exercises

The engine is pure alcove — it leans entirely on language features the numeric
work added: the fused dense kernels, in-place F64 tensor ops, `defstruct`, growable
vecs (`vec-push!`), and identity sets. Building it also surfaced (and fixed) two
interpreter bugs: a tail-call trampoline that re-evaluated a computed-list argument,
and `each` running its body once over an empty list. Both now have regression tests
in the top-level `test.alc`.
