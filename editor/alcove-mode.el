;;; alcove-mode.el --- Major mode for alcove (Arc/Clojure-flavoured Lisp-1)

;; Drop this file in ~/.emacs.d/ and add to ~/.emacs.d/init.el:
;;   (add-to-list 'load-path "~/.emacs.d/")
;;   (autoload 'alcove-mode "alcove-mode" "Major mode for alcove files." t)
;;   (add-to-list 'auto-mode-alist '("\\.alc\\'" . alcove-mode))

;;; Code:

(require 'lisp-mode)

(defgroup alcove nil
  "Major mode for alcove."
  :prefix "alcove-"
  :group 'languages)

(defconst alcove-special-forms
  '("def" "defmacro" "fn" "macroexpand-1" "eval" "apply"
    "if" "do" "let" "with" "when" "while" "for" "each" "case" "repeat"
    "quote" "and" "or" "not" "no" "in" "is" "iso"
    "=" "persist" "forget" "unpersist" "ispersistent")
  "Special forms / control flow / binding constructs.")

(defconst alcove-builtins
  '(;; arithmetic / numeric
    "+" "-" "*" "/" "**" "mod" "abs" "min" "max" "odd"
    "sqrt" "sqrt-int" "exp" "expt" "random"
    ;; bitwise / shifts
    "bit-and" "bit-or" "bit-xor" "bit-not" "<<" ">>"
    ;; comparison
    "<" ">" "<=" ">="
    ;; list / sequence
    "cons" "car" "cdr" "list" "length" "nth" "reverse" "append"
    "map" "filter" "reduce" "any?" "all?"
    ;; vec / tensor
    "vec" "vector" "vec-ref" "vec-set!" "vec-len"
    "vec-dot" "vec-axpy!" "vec-scale!" "vec-add!"
    "vec-fill!" "vec-relu!" "vec-argmax" "vec-max" "vec-copy!"
    "vec-push!" "vec-pop!" "vec-shift!" "vec-unshift!"
    ;; containers
    "hash-map" "deque" "get" "contains?" "keys" "vals" "count"
    "assoc!" "dissoc!"
    "push-left!" "push-right!" "pop-left!" "pop-right!"
    "peek-left" "peek-right"
    ;; blobs / bytes
    "make-blob" "blob-len" "blob-ref" "blob->string"
    "string->blob" "read-bytes"
    ;; i/o
    "pr" "prn" "print" "println"
    ;; persistence
    "savedb" "loaddb"
    ;; ffi / meta
    "ffi-fn" "time" "exit" "quit"
    "doc" "help" "source" "dir" "disasm" "inspect" "web?" "sleep-ms"
    ;; redis / RESP hooks
    "redis-defcmd" "redis-undefcmd" "redis-cmds"
    "redis-count" "redis-keys" "redis-type" "redis-get"
    "redis-flush" "redis-port")
  "Built-in functions.")

(defconst alcove-predicates
  '("number?" "string?" "symbol?" "pair?" "fn?"
    "vec?" "blob?" "dict?" "deque?")
  "Type-predicate functions (?-suffix convention).")

(defconst alcove-constants
  '("nil" "t")
  "Singletons.")

(defun alcove--symbol-regexp (names)
  "Build a regexp matching any of NAMES as a whole alcove symbol.
Uses Emacs's `\\_<` / `\\_>` symbol-boundary anchors, which respect
the syntax table — so identifiers like `vec-set!` / `any?` / `+` /
`<=` are matched as single tokens because the syntax table (below)
classifies their punctuation as symbol constituents."
  (concat "\\_<\\(" (regexp-opt names) "\\)\\_>"))

(defvar alcove-font-lock-keywords
  `(;; Tier 0: char literals (#\\x or #\\name) — must match before the
    ;; reader-macro alcove-vec-literal eats the '#'.
    ("\\(#\\\\\\(?:\\sw+\\|.\\)\\)" 1 font-lock-constant-face)
    ;; Tier 1: vec-literal opener  #[
    ("\\(#\\[\\)" 1 font-lock-builtin-face)
    ;; Tier 2: quote / quasiquote / unquote reader chars
    ("\\([`',]\\)" 1 font-lock-preprocessor-face)
    ;; Tier 3: special forms
    (,(alcove--symbol-regexp alcove-special-forms)
     1 font-lock-keyword-face)
    ;; Tier 4: predicates (?-suffix)
    (,(alcove--symbol-regexp alcove-predicates)
     1 font-lock-function-name-face)
    ;; Tier 5: builtins
    (,(alcove--symbol-regexp alcove-builtins)
     1 font-lock-function-name-face)
    ;; Tier 6: constants
    (,(alcove--symbol-regexp alcove-constants)
     1 font-lock-constant-face)
    ;; Tier 7: defined name in (def NAME ...)
    ("(\\(?:def\\|defmacro\\)[[:space:]]+\\([^[:space:]()]+\\)"
     1 font-lock-variable-name-face)
    ;; Numbers: optional sign, hex/decimal/scientific
    ("\\_<-?0x[0-9A-Fa-f]+\\_>"           . font-lock-constant-face)
    ("\\_<-?[0-9]+\\(?:\\.[0-9]*\\)?\\(?:[eE][-+]?[0-9]+\\)?\\_>"
     . font-lock-constant-face)
    ("\\_<-?\\.[0-9]+\\(?:[eE][-+]?[0-9]+\\)?\\_>"
     . font-lock-constant-face)))

(defvar alcove-mode-syntax-table
  (let ((tbl (copy-syntax-table lisp-mode-syntax-table)))
    ;; Treat the Lisp-1 punctuation that's legal inside identifiers as
    ;; symbol constituents instead of punctuation; otherwise font-lock
    ;; matches at the wrong boundaries and `forward-sexp` mis-parses
    ;; `vec-set!` as three tokens.
    (modify-syntax-entry ?! "_" tbl)
    (modify-syntax-entry ?? "_" tbl)
    (modify-syntax-entry ?- "_" tbl)
    (modify-syntax-entry ?+ "_" tbl)
    (modify-syntax-entry ?* "_" tbl)
    (modify-syntax-entry ?/ "_" tbl)
    (modify-syntax-entry ?< "_" tbl)
    (modify-syntax-entry ?> "_" tbl)
    (modify-syntax-entry ?= "_" tbl)
    (modify-syntax-entry ?& "_" tbl)
    (modify-syntax-entry ?| "_" tbl)
    (modify-syntax-entry ?^ "_" tbl)
    (modify-syntax-entry ?~ "_" tbl)
    (modify-syntax-entry ?% "_" tbl)
    ;; comment: ; ... \n
    (modify-syntax-entry ?\; "<" tbl)
    (modify-syntax-entry ?\n ">" tbl)
    tbl)
  "Syntax table for `alcove-mode'.")

;;;###autoload
(define-derived-mode alcove-mode prog-mode "Alcove"
  "Major mode for editing alcove source."
  :syntax-table alcove-mode-syntax-table
  (setq-local comment-start ";")
  (setq-local comment-start-skip ";+[[:space:]]*")
  (setq-local comment-end "")
  (setq-local font-lock-defaults
              '(alcove-font-lock-keywords nil nil nil nil))
  ;; lisp-mode's indent function works well enough for an Arc/Clojure-
  ;; flavoured Lisp-1; users who want clojure-mode-style indenting can
  ;; (setq-local lisp-indent-function 'clojure-indent-function) etc.
  (setq-local indent-line-function 'lisp-indent-line))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.alc\\'" . alcove-mode))

(provide 'alcove-mode)
;;; alcove-mode.el ends here
