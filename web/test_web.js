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

// Core Lisp battery (alcove-core). Expected values match NATIVE ./alcove output
// exactly — a divergence means the wasm/clang backend miscompiled something.
// Battery auto-generated from NATIVE ./alcove + ./adder output (see
// /tmp/gen_web_battery.py); each expected value IS the native result, so
// any wasm divergence — success OR error path — is a backend miscompile.
const LISP = [
  ["(+ 1 2 3 4)", "10"],
  ["(* 2 3 4)", "24"],
  ["(- 10 3 2)", "5"],
  ["(- 5)", "-5"],
  ["(/ 100 5 2)", "10"],
  ["(mod 17 5)", "2"],
  ["(abs -9)", "9"],
  ["(min 3 1 2)", "1"],
  ["(max 3 1 2)", "3"],
  ["(/ 7.0 2)", "3.5"],
  ["(+ 0.1 0.2)", "0.3"],
  ["(int 3.9)", "3"],
  ["(float 4)", "4"],
  ["(expt 2 10)", "1024"],
  ["(sqrt-int 99)", "9"],
  ["(< 1 2 3)", "t"],
  ["(<= 2 2 3)", "t"],
  ["(= 5 5)", "Error: Error invalid key in ="],
  ["(> 9 1)", "t"],
  ["(and t t nil)", "nil"],
  ["(or nil 7)", "7"],
  ["(not nil)", "Error: Error unbound variable not"],
  ["(if (> 3 2) :yes :no)", ":yes"],
  ["(str \"a\" 1 :k)", "\"a1:k\""],
  ["(substr \"hello\" 1 4)", "\"ell\""],
  ["(upper \"abc\")", "Error: Error unbound variable upper"],
  ["(split \"a,b,c\" \",\")", "Error: Error unbound variable split"],
  ["(join \"-\" (list \"x\" \"y\"))", "Error: Error unbound variable join"],
  ["(replace \"aaa\" \"a\" \"b\")", "Error: Error unbound variable replace"],
  ["(list 1 2 3)", "(1 2 3)"],
  ["(length (list 1 2 3 4))", "4"],
  ["(nth (list 'a 'b 'c) 1)", "nil"],
  ["(reverse (list 1 2 3))", "(3 2 1)"],
  ["(map (fn (x) (* x x)) (list 1 2 3))", "(1 4 9)"],
  ["(filter (fn (x) (> x 2)) (list 1 2 3 4))", "(3 4)"],
  ["(reduce + 0 (range 1 11))", "55"],
  ["(sort (list 3 1 2))", "(1 2 3)"],
  ["(append (list 1 2) (list 3 4))", "(1 2 3 4)"],
  ["(range 0 5)", "(0 1 2 3 4)"],
  ["(car (list 1 2))", "1"],
  ["(cdr (list 1 2 3))", "(2 3)"],
  ["(cons 0 (list 1 2))", "(0 1 2)"],
  ["#[1 2 3]", "#[1 2 3]"],
  ["(vec-ref #[10 20 30] 2)", "30"],
  ["(vec-len #[1 2 3 4 5])", "5"],
  ["(vec-dot #[1 2 3] #[4 5 6])", "32"],
  ["(get {:a 1 :b 2} :a)", "1"],
  ["(keys {:x 1})", "(:x)"],
  ["(contains? {:k 9} :k)", "t"],
  ["(set-has? #{1 2 3} 3)", "t"],
  ["(set->list (set-union #{1} #{2}))", "(1 2)"],
  ["(hamt-get (hamt-assoc (hamt) :k 42) :k)", "42"],
  ["#b\"abc\"", "#b\"abc\""],
  ["(blob-len #b\"abcd\")", "4"],
  ["(blob->string (string->blob \"yo\"))", "\"yo\""],
  ["(msgpack-decode (msgpack-encode 12345))", "12345"],
  ["(blob->string (msgpack-encode \"x\"))", "Error: blob->string: invalid UTF-8 at offset 0"],
  ["(cond ((> 1 2) :a) (t :b))", "t"],
  ["(let ((x 5) (y 6)) (+ x y))", "Error: Error unbound variable x"],
  ["(do (+ 1 1) (* 3 3))", "9"],
  ["(case 2 (1 'one) (2 'two) (t 'other))", "t"],
  ["((fn (n) (if (< n 2) 1 (* n ((fn (m) m) n)))) 5)", "25"],
  ["(reduce + 0 (map (fn (x) (* x 2)) (range 1 6)))", "30"],
  ["(num? 5)", "Error: Error unbound variable num?"],
  ["(str? \"x\")", "Error: Error unbound variable str?"],
  ["(list? (list 1))", "t"],
  ["(vec? #[1])", "t"],
  ["(blob? #b\"a\")", "t"],
  ["(set? #{1})", "t"],
  ["(dict? {:a 1})", "t"],
  ["(nil? nil)", "Error: Error unbound variable nil?"],
  ["'(1 2 3)", "(1 2 3)"],
  ["(eval (list (quote +) 1 2))", "3"],
];
const ADDER = [
  ["+ 1 2", "3"],
  ["* 3 4", "12"],
  ["vec-ref #[5 6 7] 0", "5"],
  ["{:a 1, :b 2}", "{:a 1, :b 2}"],
  ["get {:k 8} :k", "8"],
  ["map (fn (x) (* x x)) (list 1 2 3)", "(1 4 9)"],
  ["reduce + 0 (range 1 11)", "55"],
  ["str \"a\" \"b\" \"c\"", "\"abc\""],
  ["set-has? #{1 2 3} 2", "t"],
  ["`(a ,(+ 1 2) ,@(list 4 5))", "(a 3 4 5)"],
];

(async () => {
  let fail = 0;
  fail += await run('alcove-core.js', 'wasm/alcove', LISP);
  fail += await run('adder-core.js', 'wasm/adder', ADDER);
  if (fail) { console.log(`>>> WASM SMOKE FAILED (${fail})`); process.exit(1); }
  console.log('>>> ALL WASM SMOKE TESTS PASSED');
  process.exit(0);
})().catch((e) => { console.error(e); process.exit(2); });
