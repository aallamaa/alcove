// alcove.js — browser wrapper around alcove-core.{js,wasm}.
//
// API (after the module loads):
//   Alcove.eval(src)        async; runs `src` through alcove. Output streams
//                           to the DOM sink as it's produced.
//   Alcove.ready            promise that resolves when the runtime is up.
//   Alcove.setOutput(fn)    install a custom output sink. fn(str, "stdout"|"stderr")
//                           receives the raw stream with ANSI codes stripped.
//                           Default sink renders ANSI colour into <pre id="alcove-output">
//                           and mirrors a stripped copy to console.log/error.
//
// Auto-run: after DOMContentLoaded + runtime ready, every
//   <script type="text/alcove">...</script>
// in document order is passed to Alcove.eval. External-src forms are also
// fetched and evaluated.
//
// Load order: include this file with a normal <script src>. It dynamically
// imports alcove-core.js from the same directory (window.location-relative).

(function (global) {
  "use strict";

  // ---- ANSI → HTML / plain --------------------------------------------
  // alcove's print_node uses a handful of SGR colour codes (3x/9x fg,
  // 39 reset). We translate those into <span> with inline CSS so the
  // REPL colouring carries over into the browser. Anything we don't
  // recognise is dropped quietly.
  // Foreground SGR codes. Values tuned to be readable on both light
  // and dark backgrounds (we don't control where output lands).
  const SGR_FG = {
    30: "#555", 31: "#d33", 32: "#3a3", 33: "#b80",
    34: "#37d", 35: "#a3a", 36: "#0aa", 37: "#bbb",
    90: "#888", 91: "#f55", 92: "#5d5", 93: "#dc4",
    94: "#6af", 95: "#f6f", 96: "#5cc", 97: "#eee",
  };
  // Token regex: matches a single SGR sequence ESC[<digits>m, where
  // the parameter list may contain `;` to chain multiple codes
  // (e.g. ESC[1;31m). We only honour the *last* colour parameter
  // since alcove never combines bold+colour in one sequence.
  const SGR_RE = /\x1B\[([0-9;]*)m/g;

  function escapeHtml(s) {
    return s.replace(/[&<>"']/g, (c) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;",
      '"': "&quot;", "'": "&#39;",
    }[c]));
  }

  function ansiToHtml(s) {
    let out = "";
    let open = false;
    let last = 0;
    SGR_RE.lastIndex = 0;
    let m;
    while ((m = SGR_RE.exec(s)) !== null) {
      out += escapeHtml(s.slice(last, m.index));
      // Take the final numeric code in the (possibly semicolon-joined) list.
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

  // ---- Output routing --------------------------------------------------
  let userSink = null;

  function defaultDomSink(str, kind) {
    const el = document.getElementById("alcove-output");
    if (el) {
      el.insertAdjacentHTML("beforeend", ansiToHtml(str) + "\n");
    }
  }

  function emit(str, kind) {
    if (userSink) {
      /* Hand the user the raw stream (with ANSI codes intact). Callers
         can ansiToHtml it for color, or stripAnsi it for plain text —
         both are exposed on the Alcove object below. */
      userSink(str, kind);
    } else {
      defaultDomSink(str, kind);
      (kind === "stderr" ? console.error : console.log)(stripAnsi(str));
    }
  }

  // ---- Module bootstrap ------------------------------------------------
  function resolveCoreUrl() {
    const scripts = document.getElementsByTagName("script");
    for (let i = scripts.length - 1; i >= 0; i--) {
      const src = scripts[i].src;
      if (src && /alcove\.js(\?|$)/.test(src)) {
        return src.replace(/alcove\.js(\?.*)?$/, "alcove-core.js");
      }
    }
    return "alcove-core.js";
  }

  function loadCoreScript() {
    const url = resolveCoreUrl();
    return new Promise((resolve, reject) => {
      const s = document.createElement("script");
      s.src = url;
      s.onload = resolve;
      s.onerror = () => reject(new Error("failed to load " + url));
      document.head.appendChild(s);
    });
  }

  let Module = null;
  let alcoveWebEval = null;

  const ready = (async function () {
    await loadCoreScript();
    if (typeof global.createAlcoveModule !== "function") {
      throw new Error("createAlcoveModule not exported by alcove-core.js");
    }
    Module = await global.createAlcoveModule({
      print: (s) => emit(s, "stdout"),
      printErr: (s) => emit(s, "stderr"),
    });
    alcoveWebEval = Module.cwrap("alcove_web_eval", "number", ["string"]);
    return Module;
  })();

  // ---- Public API ------------------------------------------------------
  const Alcove = {
    ready,
    setOutput(fn) {
      userSink = typeof fn === "function" ? fn : null;
    },
    /* Helpers for sinks: convert ANSI SGR sequences into safe HTML, or
       drop them entirely. Exposed because setOutput now passes the raw
       stream — pages can pick their rendering style. */
    ansiToHtml,
    stripAnsi,
    async eval(src) {
      await ready;
      alcoveWebEval(src);
    },
  };

  global.Alcove = Alcove;

  // ---- Auto-run <script type="text/alcove"> ----------------------------
  async function runInlineBlocks() {
    await ready;
    const blocks = document.querySelectorAll('script[type="text/alcove"]');
    for (const b of blocks) {
      if (b.src) {
        try {
          const r = await fetch(b.src);
          if (!r.ok) {
            emit(`alcove: fetch ${b.src} → ${r.status}`, "stderr");
            continue;
          }
          alcoveWebEval(await r.text());
        } catch (e) {
          emit("alcove: " + e.message, "stderr");
        }
      } else {
        alcoveWebEval(b.textContent || "");
      }
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", runInlineBlocks);
  } else {
    runInlineBlocks();
  }
})(typeof window !== "undefined" ? window : globalThis);
