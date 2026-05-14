" Vim syntax file for alcove (Arc/Clojure-flavoured Lisp-1).
" Place at ~/.vim/syntax/alcove.vim; pair with ~/.vim/ftdetect/alcove.vim.
"
" Highlights special forms, builtins, predicates, tensor-op mutators,
" RESP and persistence commands, literal syntaxes (#[...], #\char,
" 0xHEX, scientific floats), strings, ; comments, and quote/quasiquote
" reader chars.

if exists("b:current_syntax")
  finish
endif

syntax case match

" Extend the keyword character set so `\<vec-set!\>`, `\<any?\>`,
" `\<+\>`, `\<bit-and\>` etc. match as single tokens. Without this,
" `-`, `!`, `?`, `<`, `>`, `*`, `/`, `+`, `^`, `&`, `|`, `~` would
" split symbols at every special char.
setlocal iskeyword=33,35-39,42-47,48-57,60-63,64-90,94-122,126,_

" ---------- comments ----------
syntax match alcoveComment ";.*$"

" ---------- numbers ----------
" Order matters: hex before generic; signs are part of the token so the
" math operators don't get partially-eaten.
syntax match alcoveNumber "\<-\=0x[0-9A-Fa-f]\+\>"
syntax match alcoveNumber "\<-\=\d\+\(\.\d*\)\=\([eE][-+]\=\d\+\)\=\>"
syntax match alcoveNumber "\<-\=\.\d\+\([eE][-+]\=\d\+\)\=\>"

" ---------- strings ----------
syntax region alcoveString start=+"+ skip=+\\.+ end=+"+

" ---------- chars (#\a, #\space, #\<single-codepoint>) ----------
syntax match alcoveChar "#\\\S"
syntax match alcoveChar "#\\\w\+\>"

" ---------- vec literal opener — #[ stays distinct from ( ----------
syntax match alcoveVecLit "#\["
syntax match alcoveVecLit "\]"

" ---------- quote / quasiquote / unquote ----------
" Just the reader chars; their argument is highlighted normally.
syntax match alcoveQuote "[`',]"

" ---------- special forms (control / binding / macro) ----------
syntax keyword alcoveSpecial def defmacro fn macroexpand-1 eval apply
syntax keyword alcoveSpecial if do let with when while for each case repeat
syntax keyword alcoveSpecial quote and or not no in is iso
syntax keyword alcoveSpecial = persist forget unpersist ispersistent

" ---------- arithmetic / bitwise / comparison ----------
" Plain alphanumeric ops use `syntax keyword`. Punctuation operators
" (+ - * / < > etc.) go through `syntax match` instead, because
" `syntax keyword` shares the parser with vimscript itself — a `|`
" or `~` on the keyword line is interpreted as a command separator
" and runs `:~` on whatever buffer is being loaded. The match form
" sidesteps that by quoting the whole pattern.
syntax keyword alcoveBuiltin mod abs min max odd
syntax keyword alcoveBuiltin sqrt sqrt-int exp expt random
syntax keyword alcoveBuiltin bit-and bit-or bit-xor bit-not
" Punctuation operators — anchored on whitespace/paren so `(+ 1 2)`
" gets the `+` highlighted but identifiers like `vec-set!` don't
" accidentally have their `!` re-coloured.
syntax match alcoveBuiltin "\%(^\|\s\|[()]\)\@<=\%(\*\*\|<<\|>>\|<=\|>=\|[-+*/<>=^&|~]\)\%(\s\|[()]\|$\)\@="

" ---------- list / sequence ops ----------
syntax keyword alcoveBuiltin cons car cdr list length nth reverse append
syntax keyword alcoveBuiltin map filter reduce any? all?

" ---------- vec / tensor ops ----------
syntax keyword alcoveBuiltin vec vector vec-ref vec-set! vec-len
syntax keyword alcoveBuiltin vec-dot vec-axpy! vec-scale! vec-add!
syntax keyword alcoveBuiltin vec-fill! vec-relu! vec-argmax vec-max
syntax keyword alcoveBuiltin vec-copy!
syntax keyword alcoveBuiltin vec-push! vec-pop! vec-shift! vec-unshift!

" ---------- Clojure-style containers ----------
syntax keyword alcoveBuiltin hash-map deque get contains? keys vals count
syntax keyword alcoveBuiltin assoc! dissoc!
syntax keyword alcoveBuiltin push-left! push-right! pop-left! pop-right!
syntax keyword alcoveBuiltin peek-left peek-right

" ---------- blobs / bytes ----------
syntax keyword alcoveBuiltin make-blob blob-len blob-ref blob->string
syntax keyword alcoveBuiltin string->blob read-bytes

" ---------- predicates ----------
syntax keyword alcovePredicate number? string? symbol? pair? fn?
syntax keyword alcovePredicate vec? blob? dict? deque?

" ---------- I/O ----------
syntax keyword alcoveBuiltin pr prn print println

" ---------- persistence / db ----------
syntax keyword alcoveBuiltin savedb loaddb

" ---------- FFI / meta ----------
syntax keyword alcoveBuiltin ffi-fn time exit quit
syntax keyword alcoveBuiltin doc help source dir disasm inspect web?
syntax keyword alcoveBuiltin sleep-ms

" ---------- redis / RESP server hooks ----------
syntax keyword alcoveBuiltin redis-defcmd redis-undefcmd redis-cmds
syntax keyword alcoveBuiltin redis-count redis-keys redis-type redis-get
syntax keyword alcoveBuiltin redis-flush redis-port

" ---------- singletons ----------
syntax keyword alcoveConstant nil t

" ---------- parens — kept as a delimiter, neutral colour ----------
syntax match alcoveParen "[()]"

" ---------- highlight links ----------
highlight default link alcoveComment    Comment
highlight default link alcoveNumber     Number
highlight default link alcoveString     String
highlight default link alcoveChar       Character
highlight default link alcoveQuote      Special
highlight default link alcoveVecLit     Special
highlight default link alcoveSpecial    Statement
highlight default link alcoveBuiltin    Function
highlight default link alcovePredicate  Function
highlight default link alcoveConstant   Constant
highlight default link alcoveParen      Delimiter

let b:current_syntax = "alcove"
" Lispy indenting + paren-match come for free from the default
" filetype machinery; load lisp.vim's lispwords for sensible auto-indent.
setlocal lispwords=def,defmacro,fn,let,with,when,while,for,each,case,if,do,repeat,let*
setlocal lisp
