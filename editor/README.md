# Editor support for `.alc`

Syntax highlighting for vim and emacs. Both highlight special forms,
builtins, predicates, tensor-op mutators, RESP / persistence commands,
literal syntax (`#[...]`, `#\char`, `0xHEX`, scientific floats), strings,
`;` comments, and quote / quasiquote reader chars.

## Vim

```sh
mkdir -p ~/.vim/syntax ~/.vim/ftdetect
cp alcove.vim          ~/.vim/syntax/alcove.vim
cp ftdetect-alcove.vim ~/.vim/ftdetect/alcove.vim
```

That's it — open any `.alc` file and vim picks up the filetype + colours.

If you use Neovim, the same files work under `~/.config/nvim/syntax/` and
`~/.config/nvim/ftdetect/`.

## Emacs

```sh
mkdir -p ~/.emacs.d
cp alcove-mode.el ~/.emacs.d/alcove-mode.el
```

Then add to your `~/.emacs.d/init.el` (or `~/.emacs`):

```elisp
(add-to-list 'load-path "~/.emacs.d/")
(autoload 'alcove-mode "alcove-mode" "Major mode for alcove files." t)
(add-to-list 'auto-mode-alist '("\\.alc\\'" . alcove-mode))
```

Restart emacs (or `M-x eval-buffer` on `init.el`) and `.alc` files open
in `alcove-mode`.

## What gets highlighted

| group                | examples                                                          |
|----------------------|-------------------------------------------------------------------|
| special forms        | `def`, `defmacro`, `fn`, `if`, `do`, `let`, `for`, `when`, `=`    |
| builtins / operators | `+`, `-`, `*`, `<`, `vec-dot`, `vec-axpy!`, `cons`, `map`         |
| predicates           | `number?`, `vec?`, `pair?`, `any?`                                |
| constants            | `nil`, `t`                                                        |
| literals             | strings, chars `#\x`, numbers (incl. `-0x1f`, `2.5e-3`), `#[...]` |
| reader chars         | `'`, `` ` ``, `,`                                                 |
| defined name (emacs) | the symbol after `def` / `defmacro` gets a distinct face          |

The vim syntax also extends `iskeyword` for the buffer so `\<vec-set!\>`,
`\<any?\>`, etc. work as single-token searches via `*` / `#`.

## Language server (completion, hover docs, live diagnostics)

`tools/lsp.alc` is a Language Server Protocol server **written in Alcove
itself** — one process serves both dialects (`.alc` in-process; `.adr`
checked through the `adder` binary so error lines point into your Adder
source). Capabilities: live syntax diagnostics, completion for all
builtins (with their docstrings) plus your globals, and hover docs.

The server command is:

```sh
alcove --noload --noinit /path/to/alcove/tools/lsp.alc
```

(If `adder` isn't on your PATH, point at it: `ALCOVE_LSP_ADDER=/path/to/adder`.)

### Neovim (0.11+)

```lua
vim.filetype.add { extension = { alc = "alcove", adr = "adder" } }
vim.lsp.config("alcove", {
  cmd = { "alcove", "--noload", "--noinit",
          vim.fn.expand("~/Code/alcove/tools/lsp.alc") },
  filetypes = { "alcove", "adder" },
})
vim.lsp.enable("alcove")
```

### Emacs (eglot, built in since 29)

```elisp
(add-to-list 'auto-mode-alist '("\\.adr\\'" . alcove-mode))
(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               '(alcove-mode . ("alcove" "--noload" "--noinit"
                                "~/Code/alcove/tools/lsp.alc"))))
```

### Vim (classic, via vim-lsp)

With [prabirshrestha/vim-lsp](https://github.com/prabirshrestha/vim-lsp)
(and the syntax files above installed):

```vim
au BufRead,BufNewFile *.adr set filetype=alcove
if executable('alcove')
  au User lsp_setup call lsp#register_server({
        \ 'name': 'alcove-lsp',
        \ 'cmd': {server_info->['alcove', '--noload', '--noinit',
        \         expand('~/Code/alcove/tools/lsp.alc')]},
        \ 'allowlist': ['alcove'],
        \ })
endif
```

`:LspHover` on a builtin, `:LspDocumentDiagnostics` for the error list;
completion arrives through omnifunc (`<C-x><C-o>`). (coc.nvim users:
declare the same command under `languageserver` in `coc-settings.json`.)

### Helix

In `~/.config/helix/languages.toml`:

```toml
[language-server.alcove-lsp]
command = "alcove"
args = ["--noload", "--noinit", "/home/you/Code/alcove/tools/lsp.alc"]

[[language]]
name = "alcove"
scope = "source.alcove"
file-types = ["alc", "adr"]
comment-token = ";"
language-servers = ["alcove-lsp"]
```

### Sublime Text

Install the **LSP** package, then in its settings:

```json
{ "clients": { "alcove-lsp": {
    "enabled": true,
    "command": ["alcove", "--noload", "--noinit",
                "/home/you/Code/alcove/tools/lsp.alc"],
    "selector": "source.alcove",
    "file_patterns": ["*.alc", "*.adr"] } } }
```

### VS Code

Any generic LSP client extension works (e.g. "LSP Client" / custom
`languageServerExample`); set the command above for file patterns
`*.alc` and `*.adr`.

### Verify it's working (any editor)

1. `python3 tools/test_lsp.py` from the repo root — the server itself
   must print `LSP: OK` (19 protocol checks; also a `make test-all` gate).
2. Open an `.alc` buffer, type `(]` — a diagnostic should appear on that
   line within a keystroke, and clear when you delete it.
3. Hover (or `K` / eldoc) on `vec-dot` — its docstring appears.
4. Trigger completion after `(vec-` — the builtin list, with docs.
5. Open an `.adr` buffer, type `bad ]` — the diagnostic points at YOUR
   Adder line (the server maps through the transpiler). If .adr
   diagnostics stay silent, the server can't find the `adder` binary —
   set `ALCOVE_LSP_ADDER=/path/to/adder` in the editor's environment.

In Emacs, `M-x eglot-events-buffer` shows the raw JSON-RPC traffic and
the `*EGLOT … stderr*` buffer shows the server's own log (handler errors
are reported there rather than killing the session).
