# Deep Architectural, Memory Safety, and Language Audit of Alcove & Adder

**Audit Target**: `alcove` (Core Lisp Engine & Runtime) & `adder` (Whitespace-Sensitive Surface Syntax Transpiler & Tooling)  
**Date**: August 5, 2026  
**Auditors**: Expert Agents in Language Design, Lisp Systems, Memory Safety, and Systems Tooling  

---

## Executive Summary

A deep audit of the **Alcove / Adder** language system was conducted across four distinct domain perspectives:
1. **Language Design & Surface Syntax** (`adder-spec.md`, `adr.h`, `adr.py`, `adfmt.c`, `alc2adr.py`)
2. **Lisp Core Engine, Evaluator & JIT** (`alcove.c`, `alcove.h`, `reader.c`, `builtins_*.h`, `compiler_impl.h`, `jit_*.h`)
3. **Memory Safety & Garbage Collection** (`gc.h`, `vector.h`, `dict.h`, `epoch.c`, `mpsc.h`, `lfkv.c`, `weak.h`)
4. **Tooling, REPL & Systems** (`adfmt.c`, `debugger.h`, `ffi.h`, `persist.h`, `adder.c`)

The audit identified **18 major findings**, including critical memory safety vulnerabilities (Heap Out-of-Bounds Reads, Use-After-Free during vector type promotion, integer overflows in hashtable rehashing, and epoch thread cleanup hazards), evaluator flaws (loss of macro closure environments, TCO bypass in `try`, unbounded macro recursion), surface syntax desynchronizations between Python and C transpilers, and REPL multi-line continuation oversights.

---

## 1. High & Critical Severity Vulnerabilities

### 1.1 Out-of-Bounds Heap Read on Trailing Escape Backslash
* **Components**: Transpiler & Formatter ([`adr.h:426, 538`](file:///home/kader/Code/alcove/adr.h#L426), [`adfmt.c:157, 240`](file:///home/kader/Code/alcove/adfmt.c#L157))
* **Mechanism**: When parsing string or blob literals (`"..."` or `#b"..."`), if an input string ends with an unclosed backslash `\` as the last byte of the buffer (`r->i == r->n - 1`), `r->i += 2` advances `r->i` to `r->n + 1`. The loop condition `r->i < r->n` exits, but atom creation reads length `(r->n + 1) - start`, attempting a heap out-of-bounds read 1 byte past the allocated source buffer.
* **Remediation**: Guard index increments to ensure `r->i + 1 < r->n` before skipping escape pairs.

```c
/* Fix for string escape scanning in adr.h and adfmt.c */
if (r->s[r->i] == '\\') {
  if (r->i + 1 < r->n) r->i += 2;
  else r->i++;
  continue;
}
```

---

### 1.2 Use-After-Free & Uninitialized Read in Vector Promotion
* **Component**: Vector Engine ([`vector.h:1597-1605`](file:///home/kader/Code/alcove/vector.h#L1597-L1605))
* **Mechanism**: In `vecpushcmd`, `vexp->vec_win.end++` is incremented *before* calling `vec_set_boxed`. If `vec_set_boxed` requires type promotion (e.g. pushing a symbol to an `I64`/`F64` vector via `vec_promote_to_gen`), `live = vexp->vec_win.end - start` includes the uninitialized new slot. `vec_promote_to_gen` treats the garbage bytes as an existing element. When `vec_set_boxed` later overwrites the slot, `unrefexp` dereferences arbitrary garbage pointers, leading to Use-After-Free / arbitrary heap dereference crashes.
* **Remediation**: Increment `vexp->vec_win.end` **only after** `vec_set_boxed` successfully stores the element.

---

### 1.3 Epoch Reclamation Registration Exhaustion & Use-After-Free Race
* **Component**: Lock-Free Concurrency ([`epoch.c:24-40, 66-75`](file:///home/kader/Code/alcove/epoch.c#L24-L40))
* **Mechanism**: When thread count exceeds `EPOCH_MAX_THREADS` (128), `epoch_register` returns `-1`. Unregistered threads calling `epoch_retire` execute `freer(ptr)` immediately rather than deferring until a quiescent epoch. If concurrent reader threads in active reactors are accessing `ptr`, calling `freer(ptr)` causes a Use-After-Free data race on `lfkv.c` lock-free reads.
* **Remediation**: Reuse thread slots upon thread termination and fallback to global quiescent epoch synchronization for unregistered workers.

---

### 1.4 Loss of Macro Lexical Closure Environment
* **Component**: Macro Engine ([`debugger.h:648-669`](file:///home/kader/Code/alcove/debugger.h#L648-L669), [`alcove.c:3892`](file:///home/kader/Code/alcove/alcove.c#L3892))
* **Mechanism**: `defmacrocmd` captures the defining lexical environment into `fn->next->meta`. However, `expandmacro` instantiates macro evaluation using `env_t *newenv = make_env(NULL);`, ignoring both the captured environment and caller environment. Macros referencing lexically closed variables from their scope fail or look up wrong global symbols.
* **Remediation**: Pass `(fn->next && fn->next->meta) ? (env_t *)fn->next->meta : env` to `make_env` inside `expandmacro`.

---

### 1.5 TCO Exception Bypass in `try` Blocks
* **Component**: Core Evaluator ([`builtins_stdlib.h:2821-2870`](file:///home/kader/Code/alcove/builtins_stdlib.h#L2821-L2870))
* **Mechanism**: `trycmd` does not clear `in_tail_position = 0` during evaluation of its body. If `try` is invoked in a tail position, body sub-calls inherit `in_tail_position = 1` and emit `FLAG_TAIL`. `trycmd` receives `FLAG_TAIL` as a return value and passes it upward, executing the tail call *outside* the dynamic extent of the `try` handler. Exceptions in the tail call completely bypass the catch block.
* **Remediation**: Save and clear `in_tail_position = 0` before evaluating the body in `trycmd`, restoring it afterwards.

---

### 1.6 Unchecked `mprotect` and Page Size Hardcoding in JIT
* **Component**: JIT Compiler ([`jit_common.h:352-355`](file:///home/kader/Code/alcove/jit_common.h#L352-L355))
* **Mechanism**: `jit_write_end` calls `mprotect` without checking the return value. Furthermore, `size_t pagesz = 4096;` hardcodes page size instead of querying `sysconf(_SC_PAGESIZE)`. On 64KB-page ARM64 Linux systems, page alignment calculations fail, causing `mprotect` to fail silently and execution of executable memory to hit SIGSEGV/SIGBUS.
* **Remediation**: Dynamically query `sysconf(_SC_PAGESIZE)` and assert successful return from `mprotect`.

---

## 2. Medium Severity Findings

### 2.1 Transpiler Desynchronization (`adr.py` vs `adr.h`)
* **Components**: Transpilers ([`adr.py:380-426`](file:///home/kader/Code/alcove/adr.py#L380-L426) vs [`adr.h:895-943`](file:///home/kader/Code/alcove/adr.h#L895-L943))
* **Issue**: `adr.py` completely lacks the `if_stack`, `elif`/`else` clause attachment, and `do` body wrapping specified in Spec §8 and implemented in `adr.h`. `else:` in `adr.py` produces an orphaned top-level `(else ...)` form. In addition, `foo(a b)` in `adr.py` transpiles to `(foo (a b))` instead of `(foo a b)`.
* **Remediation**: Synchronize `adr.py` with `adr.h` logic.

### 2.2 Off-Side Rule Tab Calculation Bug
* **Components**: Surface Parser ([`adr.h:868`](file:///home/kader/Code/alcove/adr.h#L868), [`adr.py:389`](file:///home/kader/Code/alcove/adr.py#L389), [`adfmt.c:485`](file:///home/kader/Code/alcove/adfmt.c#L485))
* **Issue**: Indentation counts 1 tab (`\t`) as 1 space column. Mixing 4 spaces and 1 tab treats the tabbed line as less indented (`1 < 4`), prematurely terminating block parsing.
* **Remediation**: Expand tabs to 4-space boundaries before evaluating leading indentation.

### 2.3 REPL Multi-Line Continuation Ignores Brackets and Braces
* **Components**: REPL ([`debugger.h:1176-1205`](file:///home/kader/Code/alcove/debugger.h#L1176-L1205))
* **Issue**: `rl_paren_depth` only counts `(` and `)`, ignoring `[` / `]` and `{` / `}`. Multi-line vector literals `#[1, 2,` or dicts `{:a 1,` immediately trigger single-line parse errors in the REPL instead of prompting for continuation lines.
* **Remediation**: Update `rl_paren_depth` to track `[`/`]` and `{`/`}`.

### 2.4 Unbounded Macro Expansion Stack Overflow
* **Components**: Evaluator & Compiler ([`compiler_impl.h:1563`](file:///home/kader/Code/alcove/compiler_impl.h#L1563), [`debugger.h:672`](file:///home/kader/Code/alcove/debugger.h#L672))
* **Issue**: Recursive or mutually recursive macros expand indefinitely until C stack exhaustion (SIGSEGV).
* **Remediation**: Add a thread-local depth counter (`ALCOVE_MACRO_MAX_DEPTH = 256`) and raise an error upon exceeding the limit.

### 2.5 Hashtable Rehash Integer Overflow
* **Component**: Dictionary Engine ([`dict.h:180-204, 244`](file:///home/kader/Code/alcove/dict.h#L180-L204))
* **Issue**: `d->ht[0].size * 2` overflows `unsigned int` when capacity reaches $2^{31}$, causing rehash failure while `used` count continues growing.
* **Remediation**: Add overflow bounds check `size <= UINT_MAX / 2` before doubling hashtable size.

### 2.6 Missing REX Prefixes in x86-64 JIT
* **Component**: JIT Compiler ([`jit_amd64.h:157-199`](file:///home/kader/Code/alcove/jit_amd64.h#L157-L199))
* **Issue**: Instruction encoders for `idiv`, `cmovz`, `call_reg`, and `jmp_reg` omit `REX.B` / `REX.R` prefixes for extended registers `r8`..`r11`, emitting instructions targeting `rax` instead.
* **Remediation**: Add `X64_REXB` and `X64_REXR` byte masks when register index $\ge 8$.

---

## 3. Low Severity & Ergonomic Findings

1. **Host Endianness Persistence Dependency** ([`persist.h:308-367`](file:///home/kader/Code/alcove/persist.h#L308-L367)): Raw binary serialization writes native endianness; little-endian conversion should be enforced for cross-architecture `.dump` portability.
2. **Deserialization Unbounded Allocations** ([`persist.h:628`](file:///home/kader/Code/alcove/persist.h#L628)): `load_vec_v2` lacks maximum element cap checks before allocation.
3. **UTF-8 Lead/Continuation Byte Error Handling** ([`reader.c:404-417`](file:///home/kader/Code/alcove/reader.c#L404-L417)): Invalid multi-byte sequences fall back to consuming raw truncated lead bytes as tokens.
4. **Intermediate Error Swallowing in `when`/`unless`** ([`builtins_stdlib.h:2989`](file:///home/kader/Code/alcove/builtins_stdlib.h#L2989)): Intermediate errors in body forms are unref'd without short-circuiting execution.
5. **Infinite Loop on Circular Cons Cells in `count`** ([`builtins_dict.h:170`](file:///home/kader/Code/alcove/builtins_dict.h#L170)): `(count circular-list)` hangs in an un-guarded while loop.

---

## 4. Comprehensive Audit Matrix

| Category | Component | Subsystem | Severity | Impact Summary |
| :--- | :--- | :--- | :--- | :--- |
| **Memory Safety** | `adr.h` / `adfmt.c` | Transpiler / Formatter | **HIGH** | Out-of-Bounds Heap Read on trailing escape backslash |
| **Memory Safety** | `vector.h` | Vector Engine | **HIGH** | Use-After-Free / UMR during vector type promotion |
| **Concurrency** | `epoch.c` | Lock-Free Memory | **HIGH** | Registration overflow UAF race in epoch retirement |
| **Lisp Core** | `debugger.h` | Macro Evaluator | **HIGH** | Macro closure environment discarded during expansion |
| **Lisp Core** | `builtins_stdlib.h` | Evaluator / TCO | **HIGH** | `try` block TCO bypass allows uncaught exceptions |
| **Compiler/JIT** | `jit_common.h` | JIT Memory Guard | **HIGH** | Unchecked `mprotect` & hardcoded page size |
| **Language Design**| `adr.py` / `adr.h` | Transpilers | **MEDIUM** | Python/C transpiler logic desynchronization |
| **Language Design**| `adr.h` / `adfmt.c` | Off-side Parser | **MEDIUM** | Tab vs space indentation calculation bug |
| **Tooling** | `debugger.h` | REPL | **MEDIUM** | Bracket/brace multi-line continuation prompt failure |
| **Lisp Core** | `compiler_impl.h` | Macro Engine | **MEDIUM** | Unbounded recursive macro expansion (stack overflow) |
| **Memory Safety** | `dict.h` | Dictionary | **MEDIUM** | Hashtable doubling integer overflow |
| **Compiler/JIT** | `jit_amd64.h` | JIT Codegen | **MEDIUM** | Missing REX prefixes on `r8..r11` register instructions |
| **Tooling** | `persist.h` | Serialization | **LOW** | Host endianness dependency & uncapped vector alloc |
| **Lisp Core** | `reader.c` | S-Expr Reader | **LOW** | Truncated UTF-8 sequence error handling |
| **Lisp Core** | `builtins_stdlib.h` | Control Flow | **LOW** | Intermediate error swallowing in `when`/`unless` |
| **Lisp Core** | `builtins_dict.h` | Builtin Functions | **LOW** | Infinite loop in `count` on circular lists |

---

## 5. Actionable Roadmap & Recommendations

1. **Apply Memory Safety Patches**:
   - Apply guards in `adr.h` and `adfmt.c` for escape backslash index bounds.
   - Defer `vexp->vec_win.end++` in `vector.h` until after `vec_set_boxed` completes.
   - Update `epoch.c` thread slot reuse and fallback synchronization.
2. **Fix Evaluator & Macro Correctness**:
   - Bind `captured` environment in `expandmacro`.
   - Clear `in_tail_position` during `trycmd` body evaluation.
   - Enforce `ALCOVE_MACRO_MAX_DEPTH` limit.
3. **Synchronize Adder Syntax & Tooling**:
   - Port `if_stack`, `elif`/`else` attachment, and `foo(a b)` call logic from `adr.h` to `adr.py`.
   - Update tab expansion in off-side parser.
   - Add bracket/brace depth tracking to `rl_paren_depth` in `debugger.h`.
4. **Harden JIT Engine**:
   - Dynamically fetch page size with `sysconf(_SC_PAGESIZE)` and check `mprotect` return code.
   - Add missing `REX.B`/`REX.R` masks in `jit_amd64.h`.
