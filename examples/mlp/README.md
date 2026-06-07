# MLP digit classifier in alcove

A tiny multi-layer perceptron that learns to recognize handwritten
digits, written in pure alcove. Trains from scratch on the UCI
*Optical Recognition of Handwritten Digits* dataset (the same data
behind `sklearn.datasets.load_digits()`), reports test accuracy on a
held-out split, and finishes in well under a second.

```
$ make
==================================================
 TENSOR OPS (mat-vec! / mat-vec-t! / vec-ger! …)
==================================================
MLP 64-32-10 on UCI optdigits (tensor ops)
train=3823 test=1797 lr=0.05 epochs=5

epoch 0:  test acc 1687/1797  (19 ms cumulative)
…
epoch 4:  test acc 1709/1797  (108 ms cumulative)

final train acc: 3741/3823    (97.9%)
final test  acc: 1709/1797    (95.1%)
```

## How to run

You need Python 3 and a built `alcove` binary at the repo root
(`make jit` from `../..`).

```sh
make data        # download optdigits.{tra,tes} → digits.bin   (Python)
make db          # digits.bin → digits.dump                    (alcove)
make train       # train + report accuracy                     (alcove)
make benchmark   # side-by-side: per-element loops vs tensor ops
make             # shorthand for `make train`

make clean       # remove digits.dump
make distclean   # also remove digits.bin and the .tra/.tes downloads
```

`make data` is the only step that touches the network; `digits.bin`
caches the result so re-running is offline. `digits.dump` then
caches the parsed-into-alcove-vecs form so training boots in 25 ms.

## What the pipeline does

```
                    prepare-data.py            build-db.alc
optdigits.tra ─┐
               ├─► digits.bin (365 KB) ─────► digits.dump (3.7 MB) ─► mlp.alc
optdigits.tes ─┘                                ▲
                                                │
                                          savedb of 8 vars:
                                          N-TRAIN, N-TEST,
                                          N-FEATURES, N-CLASSES,
                                          train-X, train-y,
                                          test-X,  test-y
```

| step          | does                                                  | time |
|---------------|-------------------------------------------------------|------|
| `make data`   | downloads two CSVs, packs to a tight little-endian binary | ~1 s (one-shot) |
| `make db`     | alcove reads `digits.bin` via `read-bytes`, slices into per-example vecs, `savedb`s them | ~150 ms |
| `make train`  | `loaddb` brings the 8 vars into the global env, then 5 epochs of SGD | ~900 ms |

## Network

```
input (64 floats, pixels 0..16 / 16) ─► [W1 32×64 + b1 32]
                                          │
                                          ▼
                                        ReLU
                                          │
                                          ▼
                                      [W2 10×32 + b2 10]
                                          │
                                          ▼
                                       softmax
                                          │
                                          ▼
                                      argmax → predicted digit
```

- Loss: cross-entropy on the softmax output.
- Optimizer: per-example SGD, learning rate 0.05, 5 epochs.
- No regularization, no augmentation — the dataset is clean.
- Weight init: uniform in `[-1/√fan_in, 1/√fan_in)` per layer.

## The interesting bit

Two implementations live side by side so the speedup story is
explicit:

- **`mlp-baseline.alc`** — hand-rolled per-element loops in alcove,
  each multiply allocates an `EXP_FLOAT`.
- **`mlp.alc`** — same math, but each layer is a single fused C call on
  flat row-major weight matrices: `mat-vec!` (forward `W·x+b`),
  `mat-vec-t!` (input gradient `Wᵀ·g`), `vec-ger!` (rank-1 weight
  update), all working on the underlying vec storage in raw doubles.

```
                            5-epoch time      per-epoch       test acc
baseline (per-elem loops)   30.8 s            6.2 s           95.0%
tensor ops (per-row dots)   0.88 s            176 ms          95.3%
fused dense layers         0.11 s             22 ms           95.1%
                            ──── ~280× ────►
```

Accuracy is identical within run-to-run noise (random init re-seeds
from wall-clock microseconds, so each invocation picks a slightly
different starting point — three runs each spread by ~30 examples
either way).

## Files

| file                  | what                                          |
|-----------------------|-----------------------------------------------|
| `Makefile`            | data / db / train / benchmark targets         |
| `prepare-data.py`     | UCI download + binary packer                  |
| `build-db.alc`        | binary → vec-of-vec → `savedb`                |
| `mlp.alc`             | tensor-ops MLP (the demo)                    |
| `mlp-baseline.alc`    | per-element-loops MLP (for benchmark comparison) |
| `digits.bin`          | generated; 365 KB packed dataset              |
| `digits.dump`         | generated; 3.7 MB alcove savedb               |
| `optdigits.tra/.tes`  | generated; UCI download cache                 |

## Alcove features this exercises

This demo was the forcing function for several language additions:

- **`(read-bytes "path")`** — slurp a file into a blob. Alcove had no
  file I/O before this; needed for `make db`.
- **vec persistence** — `savedb`/`loaddb` now round-trip arbitrary
  `vec` (including nested vec-of-vec). Built on the existing
  `__DUMP__` dispatch, so heterogeneous element types Just Work.
- **`istrue` honors container emptiness** — `(no v)` is now true iff
  the blob/vec/dict/list is empty, mirroring how nil works for
  lists. Previously every container was unconditionally falsy.
- **tensor bulk ops** — `vec-dot`, `vec-axpy!`, `vec-scale!`,
  `vec-add!`, `vec-copy!`, `vec-fill!`, `vec-relu!`, `vec-argmax`,
  `vec-max`, … Each operates on a `vec` of numeric elements (float or
  fixnum), does the math in raw C doubles, and writes results back —
  *in place* for the `!` ops, with no per-element interpreter dispatch.
- **fused dense-layer ops** — `mat-vec!` (in-place `W·x + bias`),
  `mat-vec-t!` (transposed `Wᵀ·v`, the input-gradient kernel), and
  `vec-ger!` (rank-1 update `A += α·u·vᵀ`, the weight-update kernel).
  A whole layer's forward or backward pass becomes one C call over a
  flat row-major weight matrix — ~8× over the per-row `vec-dot` loop,
  ~280× over the per-element baseline.

All of these are covered by tests in the top-level `test.alc`
(grep for `vec-`).
