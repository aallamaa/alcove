#!/bin/sh
# Download MNIST (the real 60k/10k handwritten-digit dataset) into
# examples/autograd/mnist/ as the four uncompressed IDX files mnist.alc reads.
# ~11 MB download, ~55 MB on disk. Mirror: the PyTorch/OSSCI S3 bucket
# (yann.lecun.com has required auth for years).
set -e
cd "$(dirname "$0")"
mkdir -p mnist
BASE=https://ossci-datasets.s3.amazonaws.com/mnist
for f in train-images-idx3-ubyte train-labels-idx1-ubyte \
         t10k-images-idx3-ubyte t10k-labels-idx1-ubyte; do
  if [ -f "mnist/$f" ]; then
    echo "mnist/$f already present"
  else
    echo "fetching $f.gz ..."
    curl -fsSL "$BASE/$f.gz" | gunzip > "mnist/$f"
  fi
done
echo "done — run:  ./alcove --noload examples/autograd/mnist.alc"
