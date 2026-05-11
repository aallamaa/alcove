#!/usr/bin/env python3
"""Download UCI Optical Recognition of Handwritten Digits, pack into
a tight binary blob the alcove loader can slurp via read-bytes.

Layout (all little-endian):
  uint32  magic       = 0x44494754  ("DIGT")
  uint32  n_train
  uint32  n_test
  uint32  n_features  = 64
  uint32  n_classes   = 10
  n_train * 64 bytes  train_X (each pixel 0..16)
  n_train bytes       train_y (label 0..9)
  n_test  * 64 bytes  test_X
  n_test  bytes       test_y
"""

import os
import struct
import sys
import urllib.request

BASE = "https://archive.ics.uci.edu/ml/machine-learning-databases/optdigits/"
FILES = {
    "optdigits.tra": "train",
    "optdigits.tes": "test",
}


def fetch(name: str, dest: str) -> None:
    if os.path.exists(dest):
        return
    url = BASE + name
    print(f"  fetching {url}", file=sys.stderr)
    urllib.request.urlretrieve(url, dest)


def parse(path: str):
    X, y = [], []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            cols = [int(c) for c in line.split(",")]
            if len(cols) != 65:
                raise SystemExit(f"{path}: expected 65 cols, got {len(cols)}")
            X.append(cols[:64])
            y.append(cols[64])
    return X, y


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    os.chdir(here)
    for name in FILES:
        fetch(name, name)

    train_X, train_y = parse("optdigits.tra")
    test_X, test_y = parse("optdigits.tes")
    print(f"  train: {len(train_X)} examples", file=sys.stderr)
    print(f"  test : {len(test_X)} examples", file=sys.stderr)

    with open("digits.bin", "wb") as f:
        f.write(struct.pack("<IIIII",
                            0x44494754,
                            len(train_X), len(test_X), 64, 10))
        for row in train_X:
            f.write(bytes(row))
        f.write(bytes(train_y))
        for row in test_X:
            f.write(bytes(row))
        f.write(bytes(test_y))

    size = os.path.getsize("digits.bin")
    print(f"  wrote digits.bin ({size} bytes)", file=sys.stderr)


if __name__ == "__main__":
    main()
