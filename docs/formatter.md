# Adder Formatter (`adder fmt`)

`adder fmt` is the opinionated code formatter for the Adder and Alcove dialects. It is designed to format Adder and Alcove code to a consistent style, making codebases uniform and readable.

## Guarantees

The formatter provides two strong correctness guarantees:
1. **Meaning-Preserving**: Transpiling the original and the formatted source yields identical s-expressions, guaranteeing that formatting changes layout only and never changes execution behavior.
2. **Idempotency**: Formatting a file twice produces identical output to formatting it once (i.e., `fmt(fmt(x)) == fmt(x)`). The formatted file is a fixed point.

---

## Command-Line Usage

```sh
adder fmt [options] [files...]
```

If no files are specified (or if `-` is passed), the formatter reads from standard input and writes the formatted result to standard output.

### Options

* **`-w` / `--write`**: Rewrite the specified files in place (instead of printing the formatted result to standard output).
* **`--check`**: Exit with a non-zero code if any of the specified files are not already formatted. This is used as a CI gate to enforce formatting without altering files. It prints the paths of any files that need formatting.
* **`--infix`**: (Default) Format appropriate operator applications into infix style (e.g., `(n < 0)` instead of `(< n 0)`), which reads more naturally.
* **`--no-infix`**: Preserve prefix operators (e.g. `(< n 0)`). Useful in performance-sensitive JIT loops, as unhinted prefix comparisons compile to native JIT loops more easily without de-optimizations.
* **`--alcove`**: Force the formatter to treat the input files as Alcove `.alc` syntax (enabling multi-statement-per-line splits and `;;` comments) even if they lack a `.alc` extension.

---

## Formatting Rules

`adder fmt` reformats code by enforcing the following styling decisions:
* **Indentation**: Standard Lisp indentation (2 spaces per depth level, standard function-argument indentation).
* **Comments**: Preserves both `;;` (Lisp-style) and `#` (Python-style shebang / single line) comments, keeping comments aligned with surrounding code.
* **Blank Lines**: Standardizes blank lines between forms and functions to avoid excessive spacing.
* **Parentheses Clamping**: Trailing parentheses are clamped onto the last line of the form (e.g., no isolated hanging closing parentheses on their own line).

---

## Continuous Integration (CI)

Formatting is enforced in the project's CI pipeline using the `adfmt-test` suite. 
This is wired via the `Makefile` target:

```sh
make adfmt-test
```

This runs `tools/adfmt_test.sh` to:
1. Assert meaning-preservation and idempotency over the checked-in `.adr` corpus.
2. Run conversion tests from Alcove s-expression files to indented Adder code.
3. Validate that compiling and running formatted test suites produces identical evaluation results to the original files.
