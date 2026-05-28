#!/usr/bin/env python3
"""
alcove (.alc Lisp)  ->  alcove script (.als)

The inverse of als.py. It reads alcove source into the same
plain-list form model the indent reader produces, then re-emits it in
the indentation syntax. The pair round-trips:

    alc2als.py f.alc | als.py /dev/stdin   ==  f.alc  (behaviourally)

Reader fidelity notes (matched to alcove's own reader in alcove.c):
  ; ...            line comment            -> dropped (whitespace)
  "..."            string, \\ escapes next  -> kept verbatim
  'x  `x  ,x       reader macros           -> (quote x) (quasiquote x)
                                              (unquote x)
  #\\X              character literal       -> kept verbatim as #\\X
  #[a b c]         vector literal          -> (vector a b c)   (this is
                                              literally what alcove's
                                              reader expands it to)

Emission: a form is printed inline when it is short; otherwise the head
(plus its leading atom operands and the first list operand -- the
signature / cond / binding) goes on a line ending in `:`, and the
remaining operands become indented child lines. The indent reader's
block rule appends those children back as the same trailing elements,
so structure is preserved exactly.

Usage:
    python3 alc2als.py test.alc                 # -> stdout
    python3 alc2als.py test.alc -o test.als
"""

import sys

DELIM = set(' \t\r\n()[]";\'`,')


class Sym:
    __slots__ = ("t",)

    def __init__(self, t):
        self.t = t          # raw token text, emitted verbatim


class Str:
    __slots__ = ("raw",)

    def __init__(self, raw):
        self.raw = raw      # bytes between the quotes, escapes intact


class Reader:
    def __init__(self, s):
        self.s = s
        self.i = 0
        self.n = len(s)

    def skip(self):
        while self.i < self.n:
            c = self.s[self.i]
            if c in " \t\r\n":
                self.i += 1
            elif c == ";":
                while self.i < self.n and self.s[self.i] != "\n":
                    self.i += 1
            else:
                return

    def all_forms(self):
        out = []
        while True:
            self.skip()
            if self.i >= self.n:
                return out
            if self.s[self.i] == ")":
                # mirror alcove's reader: ignore a stray top-level ')'
                self.i += 1
                continue
            out.append(self.form())

    def form(self):
        c = self.s[self.i]
        if c == "(":
            return self.lst(")")
        if c == ")":
            raise SyntaxError(f"unexpected ) at {self.i}")
        if c == '"':
            return self.string()
        if c == "'":
            self.i += 1
            self.skip()
            return [Sym("quote"), self.form()]
        if c == "`":
            self.i += 1
            self.skip()
            return [Sym("quasiquote"), self.form()]
        if c == ",":
            self.i += 1
            if self.s[self.i:self.i + 1] == "@":   # ,@x unquote-splicing
                self.i += 1
                self.skip()
                return [Sym("unquote-splicing"), self.form()]
            self.skip()
            return [Sym("unquote"), self.form()]
        if c == "#":
            nxt = self.s[self.i + 1:self.i + 2]
            if nxt == "\\":                       # #\X char literal
                tok = self.s[self.i:self.i + 3]
                self.i += 3
                return Sym(tok)
            if nxt == "[":                        # #[..] vector literal
                self.i += 2
                v = [Sym("vector")]
                while True:
                    self.skip()
                    if self.i < self.n and self.s[self.i] == "]":
                        self.i += 1
                        return v
                    v.append(self.form())
        return self.atom()

    def lst(self, close):
        self.i += 1                               # consume (
        items = []
        while True:
            self.skip()
            if self.i >= self.n:
                raise SyntaxError("unbalanced (")
            if self.s[self.i] == close:
                self.i += 1
                return items
            items.append(self.form())

    def string(self):
        self.i += 1                               # opening "
        start = self.i
        while self.i < self.n:
            ch = self.s[self.i]
            if ch == "\\":
                self.i += 2
                continue
            if ch == '"':
                raw = self.s[start:self.i]
                self.i += 1
                return Str(raw)
            self.i += 1
        raise SyntaxError("unterminated string")

    def atom(self):
        start = self.i
        while self.i < self.n and self.s[self.i] not in DELIM:
            self.i += 1
        return Sym(self.s[start:self.i])


# ---- emit alcove script ------------------------------------------------------

WIDTH = 78

# Forms whose tail elements are *body statements* (not data operands).
# value = how many leading elements form the header that stays on the
# `:`-line (head + signature / cond / binding / counter); the rest
# become indented child statement-lines.
HEADER_ARITY = {
    "def": 3, "defmacro": 3, "mac": 3,    # (def name (args) body...)
    "fn": 2,                              # (fn (args) body...)
    "with": 2, "each": 2,                 # (with (binds) body...)
    "let": 3,                             # (let var val body...)
    "if": 2, "when": 2, "unless": 2,      # (if cond then... )
    "while": 2,                           # (while cond body...)
    "for": 4,                             # (for v start end body...)
    "case": 2,                            # (case key v r ... default)
    "do": 1,                              # (do body...)
}


# Data/code builders whose nested calls read well as :-blocks. A
# trailing run of these as arguments is laddered into indented children.
LADDER_HEADS = {"list", "cons", "append", "quasiquote"}


# Head-symbol aliases emitted purely for readability in .als output:
# `= a 1` reads oddly in indented code, so we print `setf a 1`. `setf`
# is a real exact synonym of `=` in the runtime + compiler, so the
# output runs everywhere (including nested, e.g. `prn (setf x 5)`).
# Applied ONLY in operator/head position — never to a quoted `'=` or an
# `=` passed as a value (e.g. `(map = xs ys)`).
HEAD_ALIAS = {"=": "setf"}


def head_tok(head):
    """Render a form's head, applying readability aliases. Use only for
    the first element of a call form."""
    if isinstance(head, Sym) and head.t in HEAD_ALIAS:
        return HEAD_ALIAS[head.t]
    return expr(head)


def joined(f, hi):
    """Space-join f[0:hi] for a call form, aliasing the head symbol."""
    return " ".join([head_tok(f[0])] + [expr(e) for e in f[1:hi]])


def tok(x):
    if not isinstance(x, Str):
        return x.t
    # the .als file is read line-by-line, so a real newline inside a
    # string literal must travel as the \n escape (alcove's reader
    # turns it back into a newline -> identical string value).
    return '"' + x.raw.replace("\n", "\\n").replace("\r", "\\r") + '"'


def is_quote(f):
    return (isinstance(f, list) and len(f) == 2
            and isinstance(f[0], Sym) and f[0].t == "quote")


def expr(f):
    """Render `f` in argument position: lists keep their parens, a
    (quote x) collapses to 'x."""
    if not isinstance(f, list):
        return tok(f)
    if len(f) == 0:
        return "()"            # empty list / empty param list — () reads as nil
    if is_quote(f):
        return "'" + expr(f[1])
    return "(" + joined(f, len(f)) + ")"


def stmt(f, indent, lines):
    """Render `f` in statement position (top level or a block child):
    drop the outer parens, open a `:`-block for body-bearing forms."""
    pad = " " * indent
    if not isinstance(f, list):
        lines.append(pad + tok(f))
        return
    if len(f) == 0:
        lines.append(pad + "()")
        return
    if is_quote(f):
        lines.append(pad + "'" + expr(f[1]))
        return
    head = f[0]
    if len(f) == 1 and isinstance(head, Sym):
        lines.append(pad + head_tok(head) + "()")    # no-arg call
        return

    # body-bearing form -> header line + indented child statements
    if isinstance(head, Sym) and head.t in HEADER_ARITY:
        ha = HEADER_ARITY[head.t]
        if len(f) > ha:
            header = joined(f, ha)
            lines.append(f"{pad}{header}:")
            for child in f[ha:]:
                stmt(child, indent + 2, lines)
            return

    # list/data builders: ladder a trailing run of nested builder calls
    # into :-blocks (so macro templates read like indented forms). Only
    # a *suffix* is laddered, so the block rule re-appends the children
    # in order -> behaviourally identical.
    if isinstance(head, Sym) and head.t in LADDER_HEADS:
        k = len(f)
        while (k > 1 and isinstance(f[k - 1], list) and f[k - 1]
               and isinstance(f[k - 1][0], Sym)
               and f[k - 1][0].t in LADDER_HEADS):
            k -= 1
        if k < len(f):  # at least one trailing builder child to ladder
            header = joined(f, k)
            lines.append(f"{pad}{header}:")
            for child in f[k:]:
                stmt(child, indent + 2, lines)
            return

    # plain statement: unwrapped head + operands on one line
    line = joined(f, len(f))
    if len(line) + indent <= WIDTH:
        lines.append(pad + line)
        return
    # too long: split off a header + indented children (still a faithful
    # round trip -- the block rule re-appends them in order).
    k = 1
    while k < len(f) and not isinstance(f[k], list):
        k += 1
    if k < len(f) and isinstance(f[k], list):
        k += 1
    if k >= len(f):
        lines.append(pad + line)
        return
    header = joined(f, k)
    lines.append(f"{pad}{header}:")
    for child in f[k:]:
        stmt(child, indent + 2, lines)


def convert(src):
    forms = Reader(src).all_forms()
    lines = []
    for f in forms:
        stmt(f, 0, lines)
        lines.append("")             # blank line between top-level forms
    return "\n".join(lines).rstrip() + "\n"


def main(argv):
    if len(argv) < 2:
        sys.stderr.write("usage: python3 alc2als.py in.alc [-o out.als]\n")
        return 2
    out = argv[argv.index("-o") + 1] if "-o" in argv else None
    with open(argv[1]) as fh:
        result = convert(fh.read())
    if out:
        open(out, "w").write(result)
    else:
        sys.stdout.write(result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
