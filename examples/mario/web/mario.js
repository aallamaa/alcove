// mario.js — bootstrap that loads alcove (WASM), registers the gfx-* JS
// builtins, fetches ../mario.alc, and hands it to alcove_web_eval.
//
// Page layout assumed:
//   <canvas id="canvas"></canvas>
//   <pre id="alcove-output"></pre>      (optional — captures prn/print output)
//   <script src="mario_gfx.js"></script>
//   <script src="mario.js"></script>
//
// The canvas needs focus to receive keyboard events; we click it on load.

(function () {
  "use strict";

  function emit(s, kind) {
    const el = document.getElementById("alcove-output");
    if (el) el.appendChild(document.createTextNode(s + "\n"));
    (kind === "stderr" ? console.error : console.log)(s);
  }

  async function loadAlcoveCore(url) {
    return new Promise((resolve, reject) => {
      const s = document.createElement("script");
      s.src = url;
      s.onload = resolve;
      s.onerror = () => reject(new Error("failed to load " + url));
      document.head.appendChild(s);
    });
  }

  async function main() {
    await loadAlcoveCore("mario-core.js");
    if (typeof createMarioModule !== "function") {
      emit("createMarioModule not exported by mario-core.js", "stderr");
      return;
    }

    const Module = await createMarioModule({
      print:    (s) => emit(s, "stdout"),
      printErr: (s) => emit(s, "stderr"),
    });

    // The Asyncify runtime helper is needed by gfx_sleep_ms.
    if (Module.Asyncify) window.Asyncify = Module.Asyncify;

    // Register all gfx-* builtins (defined in mario_gfx.js).
    window.marioGfxRegister(Module);

    // Fetch the game source. `make web` copies mario.alc into web/ so the
    // page works regardless of which directory you serve from (a server
    // rooted at web/ can't traverse up with ../).
    const r = await fetch("mario.alc");
    if (!r.ok) {
      emit("mario.js: fetch mario.alc failed (" + r.status +
           "). Did `make web` run? It copies mario.alc into web/.", "stderr");
      return;
    }
    const src = await r.text();

    // Give the canvas keyboard focus so arrow keys reach our listener
    // without an extra click. Most browsers require a user gesture before
    // resuming an AudioContext, so we don't auto-start audio here.
    const canvas = document.getElementById("canvas");
    if (canvas) {
      canvas.tabIndex = 0;
      canvas.focus();
    }

    // `async: true` is critical — when Asyncify suspends inside (sleep-ms N)
    // the WASM stack unwinds back to JS with a pending Promise. Without
    // awaiting that Promise, cwrap returns immediately with a garbage value
    // and the WASM stack stays suspended forever (game loop never runs).
    const evalFn = Module.cwrap("alcove_web_eval", "number", ["string"], { async: true });
    await evalFn(src);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", main);
  } else {
    main();
  }
})();
