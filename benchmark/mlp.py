# MLP benchmark: 64-32-10 ReLU classifier on the UCI optdigits dataset,
# trained with cross-entropy + SGD for 5 epochs. Same architecture as
# the alcove comparison (mlp.alc). Pure stdlib — no numpy, so the inner
# loops run at interpreted-Python speed (mirrors how mlp-baseline.alc
# fares without tensor ops).
import math, struct, random, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "examples", "mlp", "digits.bin")
with open(DATA, "rb") as f:
    raw = f.read()

# header: u32 magic, u32 n_train, u32 n_test, u32 n_features, u32 n_classes
_, N_TRAIN, N_TEST, N_FEATURES, N_CLASSES = struct.unpack("<5I", raw[:20])

off = 20
def slice_fvec(off, n):
    return [b / 16.0 for b in raw[off:off + n]]

train_X_off = off
train_y_off = train_X_off + N_TRAIN * N_FEATURES
test_X_off  = train_y_off + N_TRAIN
test_y_off  = test_X_off  + N_TEST * N_FEATURES

train_X = [slice_fvec(train_X_off + k * N_FEATURES, N_FEATURES) for k in range(N_TRAIN)]
train_y = list(raw[train_y_off:train_y_off + N_TRAIN])
test_X  = [slice_fvec(test_X_off  + k * N_FEATURES, N_FEATURES) for k in range(N_TEST)]
test_y  = list(raw[test_y_off:test_y_off  + N_TEST])

HIDDEN = 32
LR     = 0.05
EPOCHS = 5

def randf():
    return random.random() - 0.5

def init_mat(rows, cols, scale):
    return [[randf() * 2.0 * scale for _ in range(cols)] for _ in range(rows)]

W1 = init_mat(HIDDEN,    N_FEATURES, 0.125)
b1 = [0.0] * HIDDEN
W2 = init_mat(N_CLASSES, HIDDEN,     0.177)
b2 = [0.0] * N_CLASSES

hidden_pre = [0.0] * HIDDEN
hidden     = [0.0] * HIDDEN
logits     = [0.0] * N_CLASSES
probs      = [0.0] * N_CLASSES
d_out      = [0.0] * N_CLASSES
d_hidden   = [0.0] * HIDDEN

def dot(a, b):
    s = 0.0
    for i in range(len(a)):
        s += a[i] * b[i]
    return s

def forward(x):
    for j in range(HIDDEN):
        hidden_pre[j] = dot(W1[j], x) + b1[j]
    for j in range(HIDDEN):
        hidden[j] = hidden_pre[j] if hidden_pre[j] > 0.0 else 0.0
    for k in range(N_CLASSES):
        logits[k] = dot(W2[k], hidden) + b2[k]

def softmax_logits():
    m = max(logits)
    z = 0.0
    for k in range(N_CLASSES):
        p = math.exp(logits[k] - m)
        probs[k] = p
        z += p
    inv = 1.0 / z
    for k in range(N_CLASSES):
        probs[k] *= inv

def predict(x):
    forward(x)
    best = 0
    bestv = logits[0]
    for k in range(1, N_CLASSES):
        if logits[k] > bestv:
            bestv = logits[k]
            best = k
    return best

def step(x, target):
    forward(x)
    softmax_logits()
    # d_out = probs - one_hot(target)
    for k in range(N_CLASSES):
        d_out[k] = probs[k]
    d_out[target] -= 1.0
    # d_hidden = sum_k W2[k] * d_out[k]
    for j in range(HIDDEN):
        d_hidden[j] = 0.0
    for k in range(N_CLASSES):
        a = d_out[k]
        row = W2[k]
        for j in range(HIDDEN):
            d_hidden[j] += a * row[j]
    # ReLU mask
    for j in range(HIDDEN):
        if not (hidden_pre[j] > 0.0):
            d_hidden[j] = 0.0
    # W2 -= LR * outer(d_out, hidden); b2 -= LR * d_out
    for k in range(N_CLASSES):
        a = -LR * d_out[k]
        row = W2[k]
        for j in range(HIDDEN):
            row[j] += a * hidden[j]
        b2[k] -= LR * d_out[k]
    # W1 -= LR * outer(d_hidden, x); b1 -= LR * d_hidden
    for j in range(HIDDEN):
        a = -LR * d_hidden[j]
        row = W1[j]
        for i in range(N_FEATURES):
            row[i] += a * x[i]
        b1[j] -= LR * d_hidden[j]

def accuracy(X, y):
    correct = 0
    for k in range(len(X)):
        if predict(X[k]) == y[k]:
            correct += 1
    return correct

for _ in range(EPOCHS):
    for k in range(N_TRAIN):
        step(train_X[k], train_y[k])

print(accuracy(test_X, test_y))
