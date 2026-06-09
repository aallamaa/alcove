# mnist_baseline.py — the SAME 784-128-10 per-sample-SGD MNIST training as
# mnist.alc, in Python+NumPy. The twin exists for output verification and an
# honest wall-clock comparison: both do identical math per step (forward,
# softmax-CE, manual backward, SGD), one sample at a time.
#
#   python3 examples/autograd/mnist_baseline.py
import struct
import time
import numpy as np

DIR = "examples/autograd/mnist/"


def load_idx(path, magic):
    with open(path, "rb") as f:
        b = f.read()
    m, n = struct.unpack(">II", b[:8])
    assert m == magic, path
    off = 16 if magic == 2051 else 8
    return np.frombuffer(b, dtype=np.uint8, offset=off), n


train_x, N_TRAIN = load_idx(DIR + "train-images-idx3-ubyte", 2051)
train_y, _ = load_idx(DIR + "train-labels-idx1-ubyte", 2049)
test_x, N_TEST = load_idx(DIR + "t10k-images-idx3-ubyte", 2051)
test_y, _ = load_idx(DIR + "t10k-labels-idx1-ubyte", 2049)
FEAT, CLS, HIDDEN, LR, EPOCHS = 784, 10, 128, 0.03, 3

rng = np.random.default_rng(1)
W1 = (rng.random((HIDDEN, FEAT)) - 0.5) * 2 * 0.0357
b1 = np.zeros(HIDDEN)
W2 = (rng.random((CLS, HIDDEN)) - 0.5) * 2 * 0.0884
b2 = np.zeros(CLS)


def step(x, t):
    global W1, b1, W2, b2
    h = W1 @ x + b1
    hr = np.maximum(h, 0.0)
    z = W2 @ hr + b2
    p = np.exp(z - z.max())
    p /= p.sum()
    dz = p.copy()
    dz[t] -= 1.0
    dW2 = np.outer(dz, hr)
    db2 = dz
    dh = (W2.T @ dz) * (h > 0)
    dW1 = np.outer(dh, x)
    db1 = dh
    W1 -= LR * dW1
    b1 -= LR * db1
    W2 -= LR * dW2
    b2 -= LR * db2


def accuracy():
    correct = 0
    for k in range(N_TEST):
        x = test_x[k * FEAT:(k + 1) * FEAT] / 255.0
        hr = np.maximum(W1 @ x + b1, 0.0)
        if int(np.argmax(W2 @ hr + b2)) == int(test_y[k]):
            correct += 1
    return correct


print(f"MNIST {FEAT}-{HIDDEN}-{CLS} MLP, python+numpy per-sample SGD")
print(f"train={N_TRAIN} test={N_TEST} lr={LR} epochs={EPOCHS}\n")
t0 = time.time()
for epoch in range(1, EPOCHS + 1):
    for k in range(N_TRAIN):
        step(train_x[k * FEAT:(k + 1) * FEAT] / 255.0, int(train_y[k]))
    ms = int((time.time() - t0) * 1000)
    print(f"epoch {epoch}:  test acc {accuracy()}/{N_TEST}  ({ms} ms cumulative)")
    LR *= 0.5  # halve the step size each epoch (mirrors mnist.alc)
final = accuracy()
print(f"\nfinal test accuracy: {final}/{N_TEST} ({100.0 * final / N_TEST:.1f}%)")
