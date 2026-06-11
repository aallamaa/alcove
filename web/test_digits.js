// test_digits.js — validates the draw-a-digit page's wasm pipeline (unpack
// u8-quantized weights → classify) against real MNIST test images: the exact
// alcove forms web/digits.html evaluates. Needs the dataset
// (sh examples/autograd/get-mnist.sh) and a built wasm core (make web);
// skips cleanly when either is absent.   Run: node web/test_digits.js
const fs = require('fs');
const path = require('path');
const ROOT = path.join(__dirname, '..');
(async () => {
  const core = path.join(__dirname, 'alcove-core.js');
  const imgPath = path.join(ROOT, 'examples/autograd/mnist/t10k-images-idx3-ubyte');
  const labPath = path.join(ROOT, 'examples/autograd/mnist/t10k-labels-idx1-ubyte');
  for (const need of [core, imgPath]) {
    if (!fs.existsSync(need)) {
      console.log(`test_digits: skipped (missing ${need})`);
      process.exit(0);
    }
  }
  const factory = require(core);
  let buf = "";
  const Module = await factory({ print: (s) => { buf += s + "\n"; },
                                 printErr: (s) => { buf += s + "\n"; } });
  const ev = (src) => Module.ccall('alcove_web_eval', 'number', ['string'], [src]);
  const W = JSON.parse(fs.readFileSync(path.join(__dirname, 'mnist-weights.json'), 'utf8'));
  ev(`(def unpack (hex n s)
        (let v (vec n 0.0)
          (do (vec-from-blob! v (hex-decode hex) 0 s)
              (let ones (vec n 1.0)
                (vec-axpy! v (- 0.0 (* 128.0 s)) ones))
              v)))
      (def classify (hex)
        (do (vec-from-blob! x-in (hex-decode hex) 0 0.00392156862745098)
            (mat-vec! h w1 x-in b1)
            (vec-relu! h)
            (mat-vec! o w2 h b2)
            (vec-softmax! o)
            o))
      (= x-in (vec 784 0.0)) (= h (vec 128 0.0)) (= o (vec 10 0.0))`);
  for (const k of ["w1", "b1", "w2", "b2"]) {
    const t = W[k];
    ev(`(= ${k} (unpack "${t.hex}" ${t.rows * t.cols} ${t.scale}))`);
  }
  const img = fs.readFileSync(imgPath);
  const lab = fs.readFileSync(labPath);
  let right = 0;
  const n = 50;
  const t0 = Date.now();
  for (let k = 0; k < n; k++) {
    const px = img.subarray(16 + k * 784, 16 + (k + 1) * 784);
    const hex = Buffer.from(px).toString('hex');
    buf = "";
    ev(`(prn (classify "${hex}"))`);
    const probs = (buf.replace(/\x1B\[[0-9;]*m/g, "")
                      .match(/-?[0-9.]+(?:e-?[0-9]+)?/g) || []).map(Number);
    let best = 0;
    for (let d = 1; d < 10; d++) if (probs[d] > probs[best]) best = d;
    if (best === lab[8 + k]) right++;
  }
  const ms = Date.now() - t0;
  console.log(`test_digits: ${right}/${n} MNIST test images correct, ` +
              `${(ms / n).toFixed(1)} ms/classify`);
  process.exit(right >= 47 ? 0 : 1);
})();
