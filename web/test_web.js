/* test_web.js — smoke test for the wasm (Emscripten) build under node.
 *
 * The web build drops JIT / FFI / RESP / readline, so the full test.alc suite
 * doesn't apply; instead this exercises the core language through
 * alcove_web_eval and checks results — focused on the value-model / tagged-
 * pointer / float / vector paths that have historically miscompiled on the
 * wasm/clang backend (the "vec-ref: bad args" and `(- 1)`-as-float Mario bugs
 * the SAFE_FLAGS=-fno-strict-aliasing exists to prevent). A regression here is
 * exactly what `make test-all` (native only) cannot catch.
 *
 * Run:  make test-web   (builds web/ then `node web/test_web.js`)
 */
'use strict';
const path = require('path');

const STRIP = (s) => s.replace(/\x1B\[[0-9;]*m/g, ''); // drop ANSI color

async function run(coreFile, name, cases) {
  const factory = require(path.join(__dirname, coreFile));
  let out = [];
  const Module = await factory({ print: (s) => out.push(s), printErr: () => {} });
  const ev = (src) => {
    out = [];
    Module.ccall('alcove_web_eval', 'number', ['string'], [src]);
    return STRIP(out.join('\n')).trim();
  };
  let pass = 0, fail = 0;
  for (const [src, want] of cases) {
    const got = ev(src);
    // alcove_web_eval follows the script convention of NOT echoing a nil
    // result, whereas the native REPL prints "nil" — so a native "nil" maps to
    // empty output on the web side. That's a print convention, not a divergence.
    const ok = got === want || (want === 'nil' && got === '');
    if (ok) pass++;
    else { fail++; console.log(`  FAIL [${name}]: ${JSON.stringify(src)} => ${JSON.stringify(got)}, want ${JSON.stringify(want)}`); }
  }
  console.log(`${name}: ${pass} passed, ${fail} failed`);
  return fail;
}

// Battery is GENERATED from native alcove/adder output by gen_web_battery.py
// (sources: web/web_exprs_{lisp,adder}.txt). Regenerate: make gen-web-battery.
// Each expected value IS the native result run through the same print code, so
// any wasm divergence — success OR error path — is a backend miscompile.
const { LISP, ADDER } = require('./web_battery.js');

(async () => {
  let fail = 0;
  fail += await run('alcove-core.js', 'wasm/alcove', LISP);
  fail += await run('adder-core.js', 'wasm/adder', ADDER);
  if (fail) { console.log(`>>> WASM SMOKE FAILED (${fail})`); process.exit(1); }
  console.log('>>> ALL WASM SMOKE TESTS PASSED');
  process.exit(0);
})().catch((e) => { console.error(e); process.exit(2); });
