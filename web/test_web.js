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
    if (got === want) pass++;
    else { fail++; console.log(`  FAIL [${name}]: ${JSON.stringify(src)} => ${JSON.stringify(got)}, want ${JSON.stringify(want)}`); }
  }
  console.log(`${name}: ${pass} passed, ${fail} failed`);
  return fail;
}

// Core Lisp battery (alcove-core). Expected values match NATIVE ./alcove output
// exactly — a divergence means the wasm/clang backend miscompiled something.
const LISP = [
  ['(+ 1 2)', '3'],
  ['(* 6 7)', '42'],
  ['(- 1)', '-1'],                       // fixnum negate — wasm float-tag bug
  ['(- 5 8)', '-3'],
  ['(< 2 3)', 't'],
  ['(/ 10.0 4)', '2.5'],                 // float path
  ['(+ 1.5 2.5)', '4'],                  // native prints whole floats w/o .0
  ['(if (> 3 2) "yes" "no")', '"yes"'],
  ['(vec-ref #[10 20 30] 1)', '20'],     // vector read — the Mario "bad args" bug
  ['(vec-len #[5 6 7 8])', '4'],
  ['#[1 2 3]', '#[1 2 3]'],
  ['#b"hi"', '#b"hi"'],                   // blob literal round-trip
  ['(blob->string #b"ok")', '"ok"'],
  ['#{1 2 3}', '#{1 3 2}'],              // set literal (bucket order)
  ['(set-has? #{1 2 3} 2)', 't'],
  ['(get {:a 1 :b 2} :b)', '2'],         // dict
  ['((fn (x) (* x x)) 9)', '81'],        // lambda apply
  ['(reduce + 0 (range 1 101))', '5050'], // seq ops
  ['42 # a comment', '42'],              // # comment
];

// Adder battery (adder-core) — same engine, paren-free prefix surface syntax.
const ADDER = [
  ['+ 1 2', '3'],
  ['vec-ref #[10 20 30] 1', '20'],
  ['{:a 1, :b 2}', '{:a 1, :b 2}'],      // map literal round-trip + comma sep
  ['get {:a 1} :a', '1'],
];

(async () => {
  let fail = 0;
  fail += await run('alcove-core.js', 'wasm/alcove', LISP);
  fail += await run('adder-core.js', 'wasm/adder', ADDER);
  if (fail) { console.log(`>>> WASM SMOKE FAILED (${fail})`); process.exit(1); }
  console.log('>>> ALL WASM SMOKE TESTS PASSED');
  process.exit(0);
})().catch((e) => { console.error(e); process.exit(2); });
