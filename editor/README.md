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
