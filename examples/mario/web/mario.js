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

  // alcove's print_node writes terminal SGR colour escapes (ESC[NNm). The
  // browser renders those as literal `␛[92m...` garbage in a <pre>, so we
  // translate them into <span style="color:…"> on the way out. Mirrors the
  // converter in ../../../web/alcove.js (the REPL shim).
  const SGR_FG = {
    30: "#555", 31: "#d33", 32: "#3a3", 33: "#b80",
    34: "#37d", 35: "#a3a", 36: "#0aa", 37: "#bbb",
    90: "#888", 91: "#f55", 92: "#5d5", 93: "#dc4",
    94: "#6af", 95: "#f6f", 96: "#5cc", 97: "#eee",
  };
  const SGR_RE = /\x1B\[([0-9;]*)m/g;

  function escapeHtml(s) {
    return s.replace(/[&<>"']/g, (c) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;",
      '"': "&quot;", "'": "&#39;",
    }[c]));
  }

  function ansiToHtml(s) {
    let out = "", open = false, last = 0, m;
    SGR_RE.lastIndex = 0;
    while ((m = SGR_RE.exec(s)) !== null) {
      out += escapeHtml(s.slice(last, m.index));
      const params = m[1].split(";").filter(Boolean);
      const code = params.length ? parseInt(params[params.length - 1], 10) : 0;
      if (open) { out += "</span>"; open = false; }
      if (code !== 0 && code !== 39 && SGR_FG[code]) {
        out += `<span style="color:${SGR_FG[code]}">`;
        open = true;
      }
      last = SGR_RE.lastIndex;
    }
    out += escapeHtml(s.slice(last));
    if (open) out += "</span>";
    return out;
  }

  function stripAnsi(s) {
    return s.replace(/\x1B\[[0-9;]*[A-Za-z]/g, "");
  }

  function emit(s, kind) {
    const el = document.getElementById("alcove-output");
    if (el) {
      el.insertAdjacentHTML("beforeend", ansiToHtml(s) + "\n");
      // Pin to bottom so the latest line is always visible. We always
      // scroll because emit() only runs when new text was just appended.
      el.scrollTop = el.scrollHeight;
    }
    (kind === "stderr" ? console.error : console.log)(stripAnsi(s));
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
