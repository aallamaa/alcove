#!/usr/bin/env python3
"""
Adder -> alcove transpiler.

Implements the reader described in adder-spec.md:

    source text  ->  reader  ->  Lisp forms  ->  emit alcove

The reader is homoiconic. `def`, `if`, `let`, `quote`, `macro` are NOT
parser node types -- they are ordinary symbols in ordinary lists. The
reader only turns whitespace/colon structure into nested lists; it does
not know what any of those symbols mean.

Reader rules (see spec sections 4-6, 16-18):

  * A bare word is a symbol; "..." a string; 42 / 3.14 a number.
  * Inline (...) are normal Lisp lists, may nest.
  * `name(args)` is reader sugar for the symbol `name` followed by the
    list `(args)` -- so `def f(a b):` reads as `(def f (a b) ...)` and
    the canonical spaced form `def f (a b):` reads identically.
    The empty form `name()` is the no-arg call `(name)`.
  * A line becomes a list of its top-level forms:
        - one atom        -> that atom itself (a bare value, not a call)
        - one list        -> that list as-is
        - many forms      -> (form form ...)
    A no-arg call is therefore written `(qux)` or `qux()`, never bare
    `qux` (which is just the value of the symbol `qux`). A line that
    opens a block is always wrapped, so `do:` is `(do ...)`.
  * A line ending in `:` opens a block; the more-indented lines below
    are appended as further elements of that line's list.
  * `'x` reads as (quote x).  `# ...` is a comment.

alcove target tweaks (spec section 19, adapted to alcove not CL):
  true -> t,  false -> nil,  nil -> nil
  macro -> defmacro   (head position only)
  setf is the assignment word (built-in alias of =); `set` is NOT remapped,
  so it stays the set constructor — (set 1 2 3) builds a set, as in Alcove.
  def / fn / do / if / let / while / for : already native to alcove.

Usage:
    python3 adr.py prog.adr               # print alcove to stdout
    python3 adr.py prog.adr -o prog.alc   # write a file
    python3 adr.py prog.adr | ./alcove    # run it
"""

import sys


# --- form model ------------------------------------------------------------

class Sym:
    """A symbol. Distinct from str so strings can print with quotes."""
    __slots__ = ("name",)

    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return f"Sym({self.name!r})"


class Str:
    """A string literal. `raw` is the bytes between the quotes, with
    escape sequences left exactly as written (the spec requires escapes
    be preserved; alcove's own reader interprets \\t, \\x1B, ...)."""
    __slots__ = ("raw",)

    def __init__(self, raw):
        self.raw = raw


# --- inline reader: one physical line of text -> list of forms -------------

class LineReader:
    def __init__(self, text):
        self.s = text
        self.i = 0
        self.n = len(text)

    def peek(self):
        return self.s[self.i] if self.i < self.n else ""

    def skip_ws(self):
        while self.i < self.n and self.s[self.i] in " \t":
            self.i += 1

    def read_forms(self, terminator=None):
        """Read forms until end of string, or until `terminator` (')').
        Inside a brace container ({…} map / #{…} set, both closed by '}') a
        comma separates entries and counts as whitespace; elsewhere ',' stays
        the unquote reader macro, keeping the quasiquote meta-syntax intact."""
        sep_comma = terminator == "}"
        forms = []
        while True:
            self.skip_ws()
            while sep_comma and self.i < self.n and self.s[self.i] == ",":
                self.i += 1
                self.skip_ws()
            if self.i >= self.n:
                if terminator is not None:
                    raise SyntaxError("unbalanced '(' in: " + self.s)
                return forms
            c = self.s[self.i]
            if c == terminator:
                self.i += 1
                return forms
            if c == ")":
                raise SyntaxError("unexpected ')' in: " + self.s)
            form = self.read_one()
            # call sugar: an atom immediately followed by '(' (no space)
            if (isinstance(form, Sym) and self.i < self.n
                    and self.s[self.i] == "("):
                self.i += 1
                args = self.read_forms(terminator=")")
                if args == []:
                    forms.append([form])          # name()  -> (name)
                else:
                    forms.append(form)            # name(a b) -> name (a b)
                    forms.append(args)
                continue
            forms.append(form)

    def read_one(self):
        c = self.s[self.i]
        if c == "{":
            # hash-map literal {k v, k v} -> (hash-map k v k v); read_forms
            # treats the ',' separators as whitespace. (#{…} set is below.)
            self.i += 1
            return [Sym("hash-map")] + self.read_forms(terminator="}")
        if c == "(":
            self.i += 1
            return self.read_forms(terminator=")")
        if c == '"':
            return self.read_string()
        if c == "'":
            self.i += 1
            self.skip_ws()
            return [Sym("quote"), self.read_one()]
        # vector literal #[a b c] -> (vector a b c); alcove's reader expands
        # #[...] the same way. (#\X char literals are handled in read_atom.)
        if self.s[self.i:self.i + 2] == "#[":
            self.i += 2
            return [Sym("vector")] + self.read_forms(terminator="]")
        # set literal #{a b c} -> (hash-set a b c)
        if self.s[self.i:self.i + 2] == "#{":
            self.i += 2
            return [Sym("hash-set")] + self.read_forms(terminator="}")
        # blob literal #b"..." -> (string->blob "...")
        if self.s[self.i:self.i + 3] == '#b"':
            self.i += 2                       # consume #b, leave the string
            return [Sym("string->blob"), self.read_string()]
        return self.read_atom()

    def read_string(self):
        self.i += 1  # opening "
        start = self.i
        while self.i < self.n:
            ch = self.s[self.i]
            if ch == "\\":            # keep escape pair verbatim
                self.i += 2
                continue
            if ch == '"':
                raw = self.s[start:self.i]
                self.i += 1
                return Str(raw)
            self.i += 1
        raise SyntaxError("unterminated string in: " + self.s)

    def read_atom(self):
        # alcove char literal #\X — X may itself be a delimiter byte;
        # always take exactly the two prefix chars plus one more.
        if self.s[self.i:self.i + 2] == "#\\" and self.i + 2 < self.n:
            tok = self.s[self.i:self.i + 3]
            self.i += 3
            return Sym(tok)
        start = self.i
        # `,` and `` ` `` terminate an atom too (they are reader macros), so a
        # token like `1,` splits into `1` and `,` — matching adr.h's
        # als_is_delim. Otherwise a map's `k v,` glues the comma onto the value.
        while self.i < self.n and self.s[self.i] not in ' \t()[]{}"\',`':
            self.i += 1
        tok = self.s[start:self.i]
        # alcove-target literal mapping
        if tok == "true":
            return Sym("t")
        if tok == "false":
            return Sym("nil")
        return Sym(tok)  # numbers ride through as symbols; emitted verbatim


# --- block builder: indentation + trailing colon -> tree -------------------

def strip_comment(line):
    """Remove a `#` comment, but not a `#` inside a string literal."""
    out = []
    in_str = False
    i = 0
    while i < len(line):
        c = line[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
        else:
            # Char literal #\X: copy `#\` and the value byte verbatim so a
            # value of `#`, `"`, `;`, or space isn't read as a comment/string.
            if c == "#" and line[i + 1:i + 2] == "\\":
                out.append(c)
                if i + 1 < len(line):
                    out.append(line[i + 1])
                if i + 2 < len(line):
                    out.append(line[i + 2])
                i += 3
                continue
            # A bare `;` (a `#\;` char literal was handled above) is the
            # *Alcove* line-comment char. Adder's comment char is `#`, but a
            # `;` must not leak into the transpiled s-expr — there it starts a
            # comment that eats the rest of the line including a closing paren,
            # leaving the form unterminated and silently dropped. Treat it as a
            # comment here too, matching the alcove reader's unconditional `;`.
            if c == ";":
                break
            # A line comment is `#` followed by a space, tab, or end of line.
            # `#!` comments too (shebang scripts). A `#` glued to any other
            # char is a dispatch token (#[ #{ #b"…) and survives to the
            # reader. One rule, no per-token exceptions.
            if c == "#" and line[i + 1:i + 2] in (" ", "\t", "", "!"):
                break
            out.append(c)
            if c == '"':
                in_str = True
        i += 1
    return "".join(out)


def opens_block(text):
    """True if `text` ends with a block-opening colon (depth 0, not in a
    string). Returns (flag, text_without_colon)."""
    if not text.endswith(":"):
        return False, text
    # make sure that final ':' isn't inside an open string literal
    in_str = False
    i = 0
    while i < len(text) - 1:
        c = text[i]
        if c == "\\" and in_str:
            i += 2
            continue
        if c == '"':
            in_str = not in_str
        i += 1
    if in_str:
        return False, text
    return True, text[:-1].rstrip()


def inline_colon(text):
    """Index of a standalone inline-block ':' — a colon followed by whitespace,
    outside any string — at which `head: body` splits into a head form and a
    one-line inline body (Pythonic `if cond: stmt`), or -1. A ':' glued to a
    constituent (`:keyword`) has no following space and is left alone; a
    trailing ':' is the block opener (opens_block); strings are skipped."""
    in_str = False
    i = 0
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == ":" and i + 1 < len(text) and text[i + 1] in " \t":
            return i if text[i + 1:].strip() else -1
        i += 1
    return -1


def line_to_list(text):
    """A line's text -> the list it denotes, per the spec line rule."""
    forms = LineReader(text).read_forms()
    if len(forms) == 1:
        return forms[0]           # lone atom -> value; lone list -> as-is
    return forms                  # many forms -> (f f ...)


HEAD_MAP = {"macro": "defmacro"}


def read_program(src):
    """Whole source -> list of top-level forms."""
    roots = []
    # stack entries: (indent, list_to_append_children_into)
    stack = []
    for raw in src.split("\n"):
        text = strip_comment(raw).rstrip()
        if text.strip() == "":
            continue
        indent = len(raw) - len(raw.lstrip(" \t"))
        body = text.strip()
        block, body = opens_block(body)

        # inline block `head: body` (no trailing ':'): parse head and the
        # one-line body separately and nest the body inside the head form, so
        # an unparenthesized body stays grouped — `if c: return y` ->
        # (if c (return y)), not (if c return y).
        icolon = -1 if block else inline_colon(body)
        if icolon >= 0:
            node = line_to_list(body[:icolon])
            if not isinstance(node, list):
                node = [node]
            node.append(line_to_list(body[icolon + 1:].strip()))
        else:
            node = line_to_list(body)
            # a block opener must be a list so children can be appended;
            # `do:` / `quote:` / a lone-atom head all wrap up here.
            if block and not isinstance(node, list):
                node = [node]

        # head-symbol remap (macro -> defmacro, set -> =)
        if (isinstance(node, list) and node
                and isinstance(node[0], Sym) and node[0].name in HEAD_MAP):
            node[0] = Sym(HEAD_MAP[node[0].name])

        while stack and indent <= stack[-1][0]:
            stack.pop()

        if stack:
            stack[-1][1].append(node)
        else:
            roots.append(node)

        if block:
            stack.append((indent, node))

    return roots


# --- emitter: forms -> formatted alcove Lisp -------------------------------

WIDTH = 72


def atom_str(a):
    if isinstance(a, Str):
        return '"' + a.raw + '"'
    if isinstance(a, Sym):
        return a.name
    raise TypeError(repr(a))


def flat(form):
    if isinstance(form, list):
        return "(" + " ".join(flat(x) for x in form) + ")"
    return atom_str(form)


def fmt(form, indent=0):
    if not isinstance(form, list):
        return atom_str(form)
    one = flat(form)
    if len(one) + indent <= WIDTH and "\n" not in one:
        return one
    if not form:
        return "()"
    pad = " " * (indent + 2)
    # keep the operator and any leading non-list operands on line 1
    # (covers `(def name (args)`, `(if (cond)`, `(let (binds)` ...)
    k = 1
    while k < len(form) and not isinstance(form[k], list):
        k += 1
    if k < len(form) and isinstance(form[k], list):
        k += 1  # keep the first list operand (signature / binding / cond)
    header = "(" + " ".join(flat(x) for x in form[:k])
    lines = [header]
    for child in form[k:]:
        lines.append(pad + fmt(child, indent + 2))
    return "\n".join(lines) + ")"


def transpile(src):
    return "\n\n".join(fmt(f) for f in read_program(src)) + "\n"


# --- cli -------------------------------------------------------------------

def main(argv):
    if len(argv) < 2:
        print(__doc__.strip().split("Usage:")[1].strip(), file=sys.stderr)
        return 2
    inp = argv[1]
    out = None
    if "-o" in argv:
        out = argv[argv.index("-o") + 1]
    with open(inp) as f:
        src = f.read()
    try:
        result = transpile(src)
    except SyntaxError as e:
        print(f"; adder read error: {e}", file=sys.stderr)
        return 1
    if out:
        with open(out, "w") as f:
            f.write(result)
    else:
        sys.stdout.write(result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
