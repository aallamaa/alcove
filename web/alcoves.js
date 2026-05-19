// alcoves.js — browser wrapper around alcoves-core.{js,wasm}.
// Exposes AlcoveScript.eval(src), using the same runtime as alcoves.

(function (global) {
  "use strict";

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
    let out = "";
    let open = false;
    let last = 0;
    SGR_RE.lastIndex = 0;
    let m;
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

  let userSink = null;

  function emit(str, kind) {
    if (userSink) {
      userSink(str, kind);
      return;
    }
    const el = document.getElementById("alcoves-output");
    if (el) el.insertAdjacentHTML("beforeend", ansiToHtml(str) + "\n");
    (kind === "stderr" ? console.error : console.log)(stripAnsi(str));
  }

  function resolveCoreUrl() {
    const scripts = document.getElementsByTagName("script");
    for (let i = scripts.length - 1; i >= 0; i--) {
      const src = scripts[i].src;
      if (src && /alcoves\.js(\?|$)/.test(src)) {
        return src.replace(/alcoves\.js(\?.*)?$/, "alcoves-core.js");
      }
    }
    return "alcoves-core.js";
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
    if (typeof global.createAlcoveScriptModule !== "function") {
      throw new Error("createAlcoveScriptModule not exported by alcoves-core.js");
    }
    Module = await global.createAlcoveScriptModule({
      print: (s) => emit(s, "stdout"),
      printErr: (s) => emit(s, "stderr"),
    });
    alcoveWebEval = Module.cwrap("alcove_web_eval", "number", ["string"]);
    return Module;
  })();

  const AlcoveScript = {
    ready,
    setOutput(fn) {
      userSink = typeof fn === "function" ? fn : null;
    },
    ansiToHtml,
    stripAnsi,
    async eval(src) {
      await ready;
      alcoveWebEval(src);
    },
  };

  global.AlcoveScript = AlcoveScript;

  async function runInlineBlocks() {
    await ready;
    const blocks = document.querySelectorAll('script[type="text/alcoves"]');
    for (const b of blocks) {
      if (b.src) {
        try {
          const r = await fetch(b.src);
          if (!r.ok) {
            emit(`alcoves: fetch ${b.src} -> ${r.status}`, "stderr");
            continue;
          }
          alcoveWebEval(await r.text());
        } catch (e) {
          emit("alcoves: " + e.message, "stderr");
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
