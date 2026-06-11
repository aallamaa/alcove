#!/usr/bin/env python3
"""test_lsp.py — end-to-end check of tools/lsp.alc over real JSON-RPC framing.

Spawns `alcove --noload --noinit tools/lsp.alc`, then: initialize → didOpen a
broken document (expect a diagnostic on the right line) → fix it (expect the
diagnostic cleared) → completion (expect builtins with docs) → hover (expect a
docstring) → shutdown/exit. If an `adder` binary is available, also opens an
.adr document and expects the diagnostic line mapped to the ADDER source.

Run from the repo root:  python3 tools/test_lsp.py
Self-gating: prints "LSP: OK" and exits 0 only if every check passes.
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
passed = 0
failed = 0


def check(name, ok):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
        print(f"FAIL: {name}")


class Server:
    def __init__(self):
        env = dict(os.environ)
        env["ALCOVE_LSP_ADDER"] = os.path.join(ROOT, "adder")
        self.p = subprocess.Popen(
            [os.path.join(ROOT, "alcove"), "--noload", "--noinit",
             os.path.join(ROOT, "tools", "lsp.alc")],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, cwd=ROOT, env=env)
        self.buf = b""

    def send(self, msg):
        body = json.dumps(msg).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n%s" % (len(body), body))
        self.p.stdin.flush()

    def recv(self, timeout_msgs=1):
        # read headers
        headers = b""
        while not headers.endswith(b"\r\n\r\n"):
            c = self.p.stdout.read(1)
            if not c:
                return None
            headers += c
        length = int([l.split(b":")[1] for l in headers.split(b"\r\n")
                      if l.lower().startswith(b"content-length")][0])
        body = b""
        while len(body) < length:
            body += self.p.stdout.read(length - len(body))
        return json.loads(body)

    def request(self, id_, method, params):
        self.send({"jsonrpc": "2.0", "id": id_, "method": method,
                   "params": params})

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})


srv = Server()

# 1. initialize
srv.request(1, "initialize", {"capabilities": {}})
r = srv.recv()
check("initialize id", r and r.get("id") == 1)
caps = (r or {}).get("result", {}).get("capabilities", {})
check("full sync", caps.get("textDocumentSync") == 1)
check("hover capability", caps.get("hoverProvider") is True)
check("completion capability", "completionProvider" in caps)
srv.notify("initialized", {})

# 2. didOpen with a syntax error on line 2 → diagnostic
URI = "file:///tmp/x.alc"
srv.notify("textDocument/didOpen",
           {"textDocument": {"uri": URI, "languageId": "alcove", "version": 1,
                             "text": "(+ 1 2)\n(]"}})
d = srv.recv()
check("diagnostics published", d and d.get("method") ==
      "textDocument/publishDiagnostics")
diags = (d or {}).get("params", {}).get("diagnostics", [])
check("one diagnostic", len(diags) == 1)
check("diagnostic on line 2 (0-based 1)",
      diags and diags[0]["range"]["start"]["line"] == 1)
check("diagnostic has message", diags and len(diags[0]["message"]) > 3)

# 3. fix the document → diagnostics cleared
srv.notify("textDocument/didChange",
           {"textDocument": {"uri": URI, "version": 2},
            "contentChanges": [{"text": "(+ 1 2)\n(* 3 4)"}]})
d = srv.recv()
check("diagnostics cleared",
      d and d["params"]["diagnostics"] == [])

# 4. completion → builtins present, documented
srv.request(2, "textDocument/completion",
            {"textDocument": {"uri": URI},
             "position": {"line": 0, "character": 1}})
r = srv.recv()
items = (r or {}).get("result", [])
labels = {i["label"]: i for i in items}
check("completion has many items", len(items) > 300)
check("completion has map", "map" in labels)
check("map is documented", "documentation" in labels.get("map", {})
      and "apply fn" in labels["map"]["documentation"])
check("globals appear (*args*)", "*args*" in labels)

# 5. hover over `vec-dot` in "(vec-dot a b)"
srv.notify("textDocument/didChange",
           {"textDocument": {"uri": URI, "version": 3},
            "contentChanges": [{"text": "(vec-dot a b)"}]})
srv.recv()  # diagnostics for v3
srv.request(3, "textDocument/hover",
            {"textDocument": {"uri": URI},
             "position": {"line": 0, "character": 3}})
r = srv.recv()
hv = (r or {}).get("result") or {}
check("hover returns docstring",
      "dot product" in hv.get("contents", {}).get("value", "").lower()
      or "vec-dot" in hv.get("contents", {}).get("value", ""))

# 6. unknown request → MethodNotFound error
srv.request(4, "workspace/symbol", {"query": "x"})
r = srv.recv()
check("unknown request errors", r and r.get("error", {}).get("code") == -32601)

# 7. Adder document (only when an adder binary is around)
adder = os.path.join(ROOT, "adder")
if os.path.exists(adder):
    AURI = "file:///tmp/x.adr"
    srv.notify("textDocument/didOpen",
               {"textDocument": {"uri": AURI, "languageId": "adder",
                                 "version": 1,
                                 "text": "def f (x):\n  + x 1\nbad ]"}})
    d = srv.recv()
    adiags = (d or {}).get("params", {}).get("diagnostics", [])
    check("adder diagnostic present", len(adiags) == 1)
    check("adder diagnostic line mapped to source (0-based 2)",
          adiags and adiags[0]["range"]["start"]["line"] == 2)
else:
    print("note: no ./adder binary — Adder document checks skipped")

# 8. shutdown / exit
srv.request(9, "shutdown", {})
r = srv.recv()
check("shutdown returns null", r and r.get("result") is None)
srv.notify("exit", {})
check("server exits 0", srv.p.wait(timeout=10) == 0)

print(f"lsp tests: {passed} passed, {failed} failed")
print("LSP: OK" if failed == 0 else "LSP: FAILED")
sys.exit(1 if failed else 0)
