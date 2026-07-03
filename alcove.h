#ifndef ALCOVE_H
#define ALCOVE_H

/* Release version — printed by `alcove --version` and exported here so a C
   embedder can compile-time-check what it's building against. */
#define ALCOVE_VERSION "0.3.0"

/* Embedding API/ABI version. Bump this whenever a change could break a
   separately-compiled consumer of this header: the exp_t/env_t layout, the
   signature/semantics of an exported function (alcove_register_cmd, make_*,
   refexp, the dump/load hooks…), or the calling convention a native module
   relies on. A native module SHOULD export
       int alcove_module_abi(void) { return ALCOVE_API_VERSION; }
   so the host can REFUSE a mismatch at (require) time instead of crashing on a
   silently-incompatible layout. See examples/embed/. */
#define ALCOVE_API_VERSION 1

#include "char.h"
#include <stdint.h>
#include <stdio.h> /* FILE — used in the dump/load declarations below; keeps
                      alcove.h self-contained for native-module authors who
                      #include it on its own. */
#include "mpsc.h"
/* Structure definition */

/* Single source of truth for the value type tags AND their display names
   (inspect_type_name in builtins_stdlib.h expands the same list, so a new type
   gets a name automatically — no more "?" drift). Columns: MEMBER, value-anchor
   (only EXP_SYMBOL is pinned, to 1), "inspect name". ORDER IS LOAD-BEARING:
   isatom / the self-evaluating container range depend on it — append, never
   reorder. EXP_MAXSIZE (the sentinel) is appended to the enum by hand. */
#define ALC_EXP_TYPES(X)                                                       \
  /* ---- atoms (everything down to EXP_ERROR) ---- */                         \
  X(EXP_SYMBOL, = 1, "symbol")                                                 \
  X(EXP_NUMBER, , "number")                                                    \
  X(EXP_FLOAT, , "float")                                                      \
  X(EXP_STRING, , "string")                                                    \
  X(EXP_CHAR, , "char")                                                        \
  X(EXP_BOOLEAN, , "boolean")                                                  \
  X(EXP_VECTOR, , "vector")                                                    \
  X(EXP_ERROR, , "error")                                                      \
  /* ---- non-atoms ---- */                                                    \
  X(EXP_PAIR, , "pair")                                                        \
  X(EXP_LAMBDA, , "lambda")                                                    \
  X(EXP_INTERNAL, , "builtin")                                                 \
  X(EXP_MACRO, , "macro")                                                      \
  X(EXO_MACROINTERNAL, , "macro-builtin")                                      \
  X(EXP_FFI, , "ffi") /* libffi-backed C-function callable; ptr → alc_ffi_t */ \
  /* ---- "circular" tags: point to a previously-allocated exp ---- */         \
  X(EXP_TREE, , "tree")                                                        \
  X(EXP_PAIR_CIRCULAR, , "pair-circular")                                      \
  /* ---- Clojure-flavored mutable containers (appended for db.dump stability, \
     self-evaluating; see isatom) ---- */                                      \
  X(EXP_BLOB, , "blob") /* binary-safe bytes; ptr → alc_blob_t (flex-array) */ \
  X(EXP_DICT, , "dict") /* hash-map; ptr → dict_t* */                          \
  X(EXP_LIST, , "deque") /* doubly-linked deque; ptr → alc_list_t */           \
  X(EXP_SET, , "set")    /* hash set; ptr → dict_t* canonical keys */          \
  X(EXP_HAMT, , "hamt")  /* persistent map; ptr → hamt_t* */                   \
  /* ---- exact non-integer numerics: inside the self-eval BLOB.. range and    \
     BEFORE EXP_CONT so isatom stays one contiguous bound ---- */              \
  X(EXP_RATIONAL, , "rational") /* int64 num/den (den>0, reduced) */           \
  X(EXP_DECIMAL, , "decimal")   /* bounded base-10: coeff + scale */           \
  X(EXP_CONT, , "continuation") /* call/cc escape token; id in meta */         \
  X(EXP_PORT, , "port")         /* buffered stream; ptr → alc_port_t */        \
  X(EXP_WEAK, , "weak") /* weak ref; ptr → target (borrowed), see weak.h */
enum {
#define X(name, anchor, dispname) name anchor,
  ALC_EXP_TYPES(X)
#undef X
      EXP_MAXSIZE /* should always be the last */
} exptype_t;

/* ---------------- Pointer tagging ----------------
   On 64-bit systems malloc returns pointers aligned to at least 8 bytes, so
   the low 3 bits of any real exp_t* are zero. We reuse those bits as a type
   tag and store small immediates (fixnums, chars) directly in the pointer
   slot instead of heap-allocating a wrapper. This removes the per-integer
   exp_t alloc + refcount round-trip that used to dominate arithmetic.

     tag 000 -> heap exp_t* (or NULL)
     tag 001 -> fixnum   (61-bit signed, value in bits 63..3)
     tag 010 -> char     (Unicode codepoint in bits 63..3)

   Floats, strings, symbols, pairs, lambdas, macros, errors stay on the
   heap — their payload doesn't fit in 61 bits or needs shared structure. */

#define TAG_MASK ((uintptr_t)0x7)
#define TAG_PTR ((uintptr_t)0x0)
#define TAG_FIX ((uintptr_t)0x1)
#define TAG_CHAR ((uintptr_t)0x2)

#define TAG(e) ((uintptr_t)(e) & TAG_MASK)
#define is_ptr(e) ((e) != NULL && TAG(e) == TAG_PTR)
#define is_imm(e) ((e) != NULL && TAG(e) != TAG_PTR)

#define MAKE_FIX(v) ((exp_t *)((((uintptr_t)(int64_t)(v)) << 3) | TAG_FIX))
#define FIX_VAL(e) ((int64_t)((intptr_t)(e)) >> 3) /* arithmetic shift */
/* True if the signed value v survives a round-trip through the fixnum tag
   (i.e. fits the (pointer_width - 3)-bit tagged range: 61-bit on 64-bit
   targets, 29-bit on wasm32). Use this to decide fixnum-vs-float promotion
   — it is pointer-width-correct, unlike an open-coded `<< 3 >> 3` check that
   assumes 64-bit uintptr_t and zero-extends the sign on 32-bit. */
#define FIX_FITS(v) (FIX_VAL(MAKE_FIX(v)) == (int64_t)(v))
/* Coerce a numeric exp_t (fixnum or float) to double without heap allocation.
   Caller must ensure (e) is either isnumber or isfloat. */
#define TO_DOUBLE(e) (isnumber(e) ? (double)FIX_VAL(e) : (e)->f)
#define MAKE_CHAR(c) ((exp_t *)((((uintptr_t)(uint32_t)(c)) << 3) | TAG_CHAR))
#define CHAR_VAL(e) ((uint32_t)((uintptr_t)(e) >> 3))

/* ---------------- Refcount atomicity ----------------
   alcove is single-threaded today, but the refcount paths use GCC/Clang
   __sync_* atomics in case threading is ever added. Atomics aren't free
   on arm64 (ldxr/stxr pairs, no store-buffer forwarding).

   Define ALCOVE_SINGLE_THREADED=1 at compile time (e.g. via
   `make mono`, or `cc -DALCOVE_SINGLE_THREADED=1 ...`) to use plain
   pre-increment/pre-decrement instead. Both variants return the NEW
   value so all call sites match either expansion. */

#ifndef ALCOVE_SINGLE_THREADED
#define ALCOVE_SINGLE_THREADED 0
#endif

#if ALCOVE_SINGLE_THREADED
#define REFCOUNT_INC(p) (++(*(p)))
#define REFCOUNT_DEC(p) (--(*(p)))
/* alcove_global_gen bumps invalidate every gcache reader: the bump must
   happen-before any subsequent dict mutation a reader could observe.
   Mono build: plain ++ is fine (single thread, single happens-before). */
#define GEN_BUMP() (++alcove_global_gen)
/* Mono build doesn't need TLS at all — the macro expands away. */
#define ALCOVE_TLS
#else
/* Refcount inc is RELAXED: a thread that increments already holds a reference,
   so a happens-before to the object's construction exists via that reference —
   no barrier needed (the standard Arc/shared_ptr pattern). Dec is RELEASE so
   all of this thread's writes-through-the-object are ordered before the count
   drop; on the 1->0 transition the macro issues an ACQUIRE fence so the freeing
   thread sees every other thread's prior writes before it tears the object
   down. This is strictly cheaper than the old full-barrier __sync on every dec
   (arm64: ldxr/stxr without the dmb), and behaviorally identical to it. */
#define REFCOUNT_INC(p) __atomic_add_fetch((p), 1, __ATOMIC_RELAXED)
#define REFCOUNT_DEC(p)                                                        \
  __extension__({                                                              \
    int _rc_new = __atomic_sub_fetch((p), 1, __ATOMIC_RELEASE);                \
    if (_rc_new <= 0)                                                          \
      __atomic_thread_fence(__ATOMIC_ACQUIRE);                                 \
    _rc_new;                                                                   \
  })
/* GEN_BUMP stays a FULL barrier (not relaxed): the bump must happen-before the
   dict mutation that follows it, so a gcache reader on another thread that
   loads the new gen is guaranteed to also see that write. Relaxing it would be
   a real (subtle) bug — leave it as __sync. */
#define GEN_BUMP() __sync_add_and_fetch(&alcove_global_gen, 1)
/* TLS storage class for hot per-thread variables (exp_freelist, the
   bump pointers, current_shard, ...). The "initial-exec" model emits
   a single mrs+ldr on arm64 and a single %fs:offset load on x86_64,
   avoiding the __tls_get_addr indirect call that the default
   "global-dynamic" model uses. We can use it because alcove is always
   built as a single executable (no dlopen of TLS variables from
   plugins). The "initial-exec" attribute is a clang/gcc extension;
   wrap it in __has_attribute so older compilers fall back gracefully. */
#if defined(__has_attribute) && __has_attribute(tls_model)
#define ALCOVE_TLS __thread __attribute__((tls_model("initial-exec")))
#else
#define ALCOVE_TLS __thread
#endif
#endif

/* Marks a function as run-rarely so the compiler places it off the hot path
   (its own .text.unlikely section) and never inlines it into a hot caller —
   used only on cold compiler/disasm helpers to keep them from perturbing the
   layout/inlining of the VM hot loop. No-op on compilers without the attr. */
#if defined(__has_attribute) && __has_attribute(cold)
#define ALCOVE_COLD __attribute__((cold, noinline))
#else
#define ALCOVE_COLD
#endif

/* Single source of truth for the error codes AND their machine-readable names
   (error_code_name in builtins_log.h expands the same list into its switch, so
   a new code can't be added to the enum without getting a stable name).
   Columns: MEMBER, value-anchor (empty = sequential; the =1 / =256 gaps are
   explicit), "code-name". The 4 PARSING_* codes deliberately share
   "parse-error". ERROR_CONT_ESCAPE is NOT in this list — it is not a real error
   (a call/cc escape token), so it has no code-name and is appended to the enum
   by hand, falling through to error_code_name's "error" default. */
/* Columns: MEMBER, value-anchor, "code-name", "description" — the last is
   what (error-codes) reports next to each class. */
#define ALC_ERRORS(X)                                                          \
  X(EXP_ERROR_PARSING_MACROCHAR, = 1, "parse-error",                           \
    "the reader could not parse the source text")                              \
  X(EXP_ERROR_PARSING_ILLEGAL_CHAR, , "parse-error",                           \
    "the reader could not parse the source text")                              \
  X(EXP_ERROR_PARSING_EOF, , "parse-error",                                    \
    "the reader could not parse the source text")                              \
  X(EXP_ERROR_PARSING_ESCAPE, , "parse-error",                                 \
    "the reader could not parse the source text")                              \
  X(EXP_ERROR_INVALID_KEY_UPDATE, = 256, "invalid-key-update",                 \
    "rebinding a reserved/builtin name")                                       \
  X(EXP_ERROR_BODY_NOT_LIST, , "body-not-list",                                \
    "def/fn body is not a list of forms")                                      \
  X(EXP_ERROR_PARAM_NOT_LIST, , "param-not-list",                              \
    "def/fn parameter spec is not a list")                                     \
  X(EXP_ERROR_MISSING_NAME, , "missing-name",                                  \
    "def/defmacro without a name symbol")                                      \
  X(ERROR_ILLEGAL_VALUE, , "illegal-value",                                    \
    "wrong type or value for the operation (the general class)")               \
  X(ERROR_DIV_BY0, , "div-by-zero",                                            \
    "division or modulo by zero")                                              \
  X(ERROR_MISSING_PARAMETER, , "missing-parameter",                            \
    "wrong number of arguments in a call")                                     \
  X(ERROR_UNBOUND_VARIABLE, , "unbound-variable",                              \
    "reference to a name with no binding")                                     \
  X(ERROR_NUMBER_EXPECTED, , "number-expected",                                \
    "a numeric argument was required")                                         \
  X(ERROR_INDEX_OUT_OF_RANGE, , "index-out-of-range",                          \
    "index past the end (or negative) for an erroring accessor")               \
  X(ERROR_USER, , "user-error",                                                \
    "(raise ...) errors; error-code may also be your custom symbol")
enum {
#define X(name, anchor, codename, desc) name anchor,
  ALC_ERRORS(X)
#undef X
      ERROR_CONT_ESCAPE, /* not a real error: a call/cc escape token in flight,
                            carrying its continuation id in `meta` and the
                            payload value in `next`; caught by the matching
                            call/cc frame */
} exp_error_t;

/* Type predicates — all tag-aware. is_ptr() guards every heap deref so a
   tagged immediate passed to any of these returns 0 cleanly. */
#define isnumber(e) (TAG(e) == TAG_FIX)
#define ischar(e) (TAG(e) == TAG_CHAR)
#define issymbol(e) (is_ptr(e) && (e)->type == EXP_SYMBOL)
#define isfloat(e) (is_ptr(e) && (e)->type == EXP_FLOAT)
#define isstring(e) (is_ptr(e) && (e)->type == EXP_STRING)
#define ispair(e) (is_ptr(e) && (e)->type == EXP_PAIR)
#define islambda(e) (is_ptr(e) && (e)->type == EXP_LAMBDA)
#define isinternal(e) (is_ptr(e) && (e)->type == EXP_INTERNAL)
#define ismacro(e) (is_ptr(e) && (e)->type == EXP_MACRO)
#define iserror(e) (is_ptr(e) && (e)->type == EXP_ERROR)
/* A call/cc escape token in flight: an EXP_ERROR whose flags ==
   ERROR_CONT_ESCAPE. It is NOT a catchable error — it belongs to the matching
   call/cc frame and must pass through try/handler/finally untouched. Defined
   here (not in builtins_stdlib.h) so the bytecode VM in alcove.c can route on
   it too. */
#define is_cont_escape(e) (iserror(e) && (e)->flags == ERROR_CONT_ESCAPE)
#define isffi(e) (is_ptr(e) && (e)->type == EXP_FFI)
#define isblob(e) (is_ptr(e) && (e)->type == EXP_BLOB)
#define isdict(e) (is_ptr(e) && (e)->type == EXP_DICT)
#define islist(e) (is_ptr(e) && (e)->type == EXP_LIST)
#define isset(e) (is_ptr(e) && (e)->type == EXP_SET)
#define ishamt(e) (is_ptr(e) && (e)->type == EXP_HAMT)
#define iscont(e) (is_ptr(e) && (e)->type == EXP_CONT)
#define isport(e) (is_ptr(e) && (e)->type == EXP_PORT)
#define isvector(e) (is_ptr(e) && (e)->type == EXP_VECTOR)
/* Self-evaluating: tagged immediates, scalar atoms (≤ EXP_VECTOR), and
   the appended Clojure-style containers (blob/dict/list). The container
   range is contiguous (BLOB..LIST) so the bounds check stays a single
   comparison pair instead of three OR'd equalities. */
#define isatom(e)                                                              \
  (is_imm(e) ||                                                                \
   (is_ptr(e) && ((e)->type <= EXP_VECTOR ||                                   \
                  ((e)->type >= EXP_BLOB && (e)->type <= EXP_DECIMAL) ||       \
                  (e)->type >= EXP_MAXSIZE)))

/* Helper for fast-path refcounting */
#define is_immortal(e)                                                         \
  (!is_ptr(e) || (e) == nil_singleton || (e) == true_singleton ||              \
   (e) == gen_done_singleton)

#define car(e) ((is_ptr(e) && (e)->type == EXP_PAIR) ? (e)->content : NULL)
#define cdr(e) ((is_ptr(e) && (e)->type == EXP_PAIR) ? (e)->next : NULL)
#define cadr(e) car(cdr(e))
#define cddr(e) cdr(cdr(e))
#define cdddr(e) cdr(cdr(cdr(e)))
#define caddr(e) car(cdr(cdr(e)))
#define cadddr(e) car(cdr(cdr(cdr(e))))
#define expfloat double
struct keyval_t;
struct exp_t;
struct env_t;
typedef struct exp_t *lispCmd(struct exp_t *e, struct env_t *env);
/* "Values" builtin ABI (fast path): receives the already-EVALuated args as an
   owned argv (the callee must unrefexp each, like vm_invoke_values' contract)
   and returns an owned result. No call form is synthesized, so there is no `e`
   to forget freeing — this sidesteps the consume-e footgun entirely. A builtin
   registers BOTH a normal lispCmd (for the AST / apply / map paths) and an
   optional lispCmdV; the compiled fast path calls the lispCmdV when present.
   TWO obligations on the implementor:
     1. VALIDATE nargs — the VM passes the raw call arity (a wrong-arity
   compiled call reaches you as-is); index argv only after checking nargs.
     2. CONSUME every argv ref exactly once (unrefexp), on every path.
   The lispCmdV and the lispCmd MUST be behaviorally + ownership equivalent —
   the AST path runs one, the compiled path the other; nothing enforces parity.
 */
typedef struct exp_t *lispCmdV(int nargs, struct exp_t **argv,
                               struct env_t *env);

#define FLAG_TAILREC 1
/* Internal cmd is tail-aware: evaluate will expose in_tail_position to
   it. Set on the EXP_INTERNAL via the lispProc flags column. */
#define FLAG_TAIL_AWARE 2
/* Lambda body has been compiled to bytecode. invoke() runs the VM
   dispatch loop instead of the AST walker. e->bc points to a bytecode_t. */
#define FLAG_COMPILED 4
/* This exp is reachable from more than one shard (e.g., it lives in the
   global env, or was promoted-to-shared by a write barrier). The C
   refcount macros are atomic regardless, but the JIT inlines plain
   load/add/store on nref for speed — JIT'd code MUST test this bit
   before any inline refop and deopt to the bytecode interpreter when
   set, so the atomic macros run instead. Cleared on fresh exps. */
#define FLAG_SHARED 8

/* Small-text optimization for EXP_SYMBOL / EXP_STRING. When the text fits
   (≤ INLINE_TXT_CAP chars + NUL), it is stored INLINE in the primary union
   (`istr`, overlapping `ptr`) instead of a separate heap allocation. The
   text lives in the `ptr` union — NOT the `meta` union — specifically so a
   symbol's resolution cache (which lives in `meta`) keeps working for short
   builtins like +, car, if. Two rules the rest of the code must respect:
     1. Read the bytes via exp_text(), never `->ptr` directly, when the flag
        may be set — `->ptr` reinterprets the inline bytes as a pointer.
     2. unrefexp must NOT free(ptr) when this flag is set (there is no heap
        allocation; the bytes are in the struct) — see the free path.
   Bit 6: bits 0-3 are the FLAG_* above, bits 4-5 are the EXP_VECTOR element
   kind. Symbols/strings are never vectors, so there is no overlap. */
#define FLAG_INLINE_TXT 64
#define INLINE_TXT_CAP 7 /* 7 chars + NUL fit the 8-byte union slot */
/* Multi-arity lambda (defn): an EXP_LAMBDA whose `content` is a LIST of
   per-clause lambda values rather than a param list. The call paths
   (invoke / vm_invoke_values) intercept this flag and dispatch to the clause
   whose arity matches before any code reads content as params. Bit 7. */
#define FLAG_MULTI 128
/* EXP_INTERNAL registered via alcove_register_cmd as a NON-tail-aware builtin:
   an applicative C function that evaluates all its args (the module/embedding
   convention). The compiler may emit a real OP_CALL_GLOBAL for such a call
   (args compiled to bytecode, then a fast dispatch) instead of the OP_EVAL_AST
   tree-walk it falls back to for unflagged builtins — so a hot loop calling a
   native-module builtin (e.g. lmi/get) isn't a per-call AST re-resolve. Core
   special forms (while/each/time/…) are installed via make_internal directly,
   never get this bit, and keep the AST path. Bit 8. */
/* Set on an object that has at least one live (weak v) cell pointing at it.
   Tested only on the out-of-line free path (unrefexp_free / the gc sweep):
   when set, weak_on_target_free nulls out every weak cell in the target's
   registry chain so a later (weak-get) returns nil instead of dangling.
   Bit 10 — bits 4-5 are the vector element kind, 6/7/8/9 taken above. */
#define FLAG_WEAK_REFERENT 1024
/* Set on a container with at least one (watch! obj fn) watcher. Tested by
   the structural-mutator builtins (assoc!, push-*!, vec-set!, ...) which
   call watch_notify (watch.h) after the write. Bit 11. */
#define FLAG_WATCHED 2048
#define FLAG_APPLICATIVE 256
/* Privileged/unsafe builtin: touches the host OS, filesystem, network, FFI, or
   loads/persists code (shell, file ops, setenv, FFI, require of native modules,
   savedb/loaddb, redis command registration). Set in the lispProcList flags
   column and propagated to the EXP_INTERNAL (see the registration mask). Under
   `--safe` (g_safe_mode) the dispatch refuses these with a clear error — the
   sandbox tier the `level` reservation was a placeholder for. Bit 9 (fits the
   16-bit exp_t.flags). */
#define FLAG_UNSAFE 512

/* For EXP_VECTOR only: element kind, encoded in bits 4-5 of exp_t->flags.
   GEN keeps the old behavior — each slot is an owning exp_t* ref. I64/F64
   store raw 8-byte numerics inline, skipping per-cell heap exp_t boxing.
   Inference at construction (see make_vector / vectorcmd) picks the
   tightest kind; vec-set! of a mismatched type triggers an in-place
   promotion to GEN (see vec_promote_to_gen). Single-shard only: a vec
   that gets FLAG_SHARED must NOT be promoted or reallocated, since
   readers on another shard would observe torn metadata. */
#define VEC_KIND_GEN (0u << 4)
#define VEC_KIND_I64 (1u << 4)
#define VEC_KIND_F64 (2u << 4)
#define VEC_KIND_MASK (3u << 4)
#define vec_kind(e) ((unsigned)((e)->flags & VEC_KIND_MASK))

struct bytecode_t;

typedef struct exp_t {
  unsigned short int flags; /* 2 bytes */
  unsigned short int type;  /* 2 bytes */
  int nref;                 /* 4 bytes */
  union {                   /* 8 bytes */
    struct exp_t *content;
    struct bytecode_t *bc; /* set on compiled lambdas */
    void *ptr;
    int64_t s64;
    expfloat f;
    lispCmd *fnc;
    char istr[8]; /* FLAG_INLINE_TXT: inline symbol/string bytes,
                     overlapping `ptr`. Read via exp_text(), never
                     via `->ptr` directly, when the flag is set.
                     Lives here (not the meta union) so the symbol
                     resolution cache in `meta` stays usable. */
  };
  union { /* 8 bytes — overloaded by node role:
               - on the lambda/macro head: strdup'd name
                 in `meta` (free'd in unrefexp)
               - on the body wrapper (e->next of a
                 lambda/macro): captured env_t* in `meta`
                 for closures (released via destroy_env)
               - on cached-symbol exp_t's: keyval_t*
                 back-pointer in `meta`
               - on EXP_VECTOR: `vec_win` holds the live
                 window {start, end} into the storage
                 pointed at by `ptr`. `len = end - start`.
                 Reading `meta` on a vec yields garbage —
                 gate every `->meta` access on type. */
    struct keyval_t *meta;
    struct {
      int32_t start, end;
    } vec_win;
  };
  struct exp_t *next; /* 8 bytes */
} exp_t;              /* 32 bytes */
_Static_assert(sizeof(exp_t) == 32,
               "exp_t layout changed — audit JIT and refcount paths");

/* Text of an EXP_SYMBOL / EXP_STRING / EXP_ERROR, transparent to inlining.
   Use this everywhere a symbol or string name is read as a C string. For
   non-inline exps it is just `->ptr`; for FLAG_INLINE_TXT it returns the
   in-struct bytes. The branch is on a field already in the same cache line
   and is perfectly predictable per-exp, so the cost over a bare load is ~0. */
static inline char *exp_text(const exp_t *e) {
  return (e->flags & FLAG_INLINE_TXT) ? (char *)e->istr : (char *)e->ptr;
}

/* ---------------- Bytecode VM ----------------
   Lambda bodies that use only supported forms (fixnum arithmetic,
   comparisons, if, user calls, param/global refs) get compiled at
   def/fn time. invoke() runs the dispatch loop over the opcode array
   instead of walking the AST. Unsupported bodies stay as AST. */
/* Single source of truth for the bytecode opcodes AND their disassembler names:
   bc_opname (alcove.c) expands this list, so a new opcode can't be added
   without getting a name (a missing one printed "??"). bc_oplen / bc_disasm_one
   stay hand-written — they encode per-op operand LENGTHS and PRINT FORMATS that
   don't fit a uniform column, and bc_oplen deliberately returns 0 for
   PUSH_HANDLER. Opcodes are sequential from OP_HALT=0; OP_MAX (the count
   sentinel) is appended to the enum by hand. Operand layout documented per row.
   ORDER preserved. */
/* The per-opcode operand-layout comments contain UTF-8 (→ —), which
   clang-format mis-columns, so it oscillates the `\` continuations; the
   alignment here is hand-maintained. */
// clang-format off
#define OPCODE_LIST(X)                                                           \
  X(HALT)                                                                        \
  X(RET)                                                                         \
  X(POP)                                                                         \
  X(DUP) /* dup top-of-stack (fresh ref); compile_and/or/case keep the tested    \
            value across a popping branch */                                     \
  X(EVAL_AST)    /* u8 idx → push EVAL(consts[idx], env). Escape hatch: a      \
                    non-tail-aware builtin call with no native opcode is stored  \
                    as    its raw form and tree-walked, so the enclosing lambda                        \
                    still    compiles (and keeps its tail call). */                                       \
  X(LOAD_FIX)    /* int16 imm       → push MAKE_FIX(imm) */                      \
  X(LOAD_CONST)  /* u8 idx          → push refexp(consts[idx]) */                \
  X(LOAD_SLOT)   /* u8 idx          → push refexp(inline_vals[idx]) */           \
  X(LOAD_GLOBAL) /* u8 idx (symbol) → lookup consts[idx] in env, push */         \
  X(STORE_SLOT)  /* u8 idx          → pop → inline_vals[idx] (unref old) */      \
  X(BIND_SLOT)   /* u8 idx          → pop → inline_vals[idx], bump n_inline */   \
  X(BIND_SLOT_NAMED) /* u8 idx, u8 name_const → like BIND_SLOT but also sets   \
                        inline_keys[idx]=exp_text(consts[name_const]) so the     \
                        let/with/for local is resolvable BY NAME (an EVAL_AST    \
                        sub-form needs that; plain BIND_SLOT leaves a NULL key   \
                        invisible to symbolic lookup). */                        \
  X(UNBIND_SLOT) /* u8 idx          → unref + NULL inline_vals[idx] (and key)  \
                  */                                                             \
  X(ADD)                                                                         \
  X(SUB)                                                                         \
  X(MUL)                                                                         \
  X(DIV)                                                                         \
  X(MOD)                                                                         \
  X(LT)                                                                          \
  X(GT)                                                                          \
  X(LE)                                                                          \
  X(GE)                                                                          \
  X(IS)                                                                          \
  X(ISO)                                                                         \
  X(NOT)                                                                         \
  X(JUMP) /* int16 off relative to end of operand */                             \
  X(BR_IF_FALSE)                                                                 \
  X(BR_IF_TRUE)                                                                  \
  X(CALL)        /* u8 nargs        → [fn, a0..aN-1] → result */                 \
  X(CALL_GLOBAL) /* u8 const_idx, u8 nargs → fused LOAD_GLOBAL+CALL */           \
  X(TAIL_SELF)   /* u8 nargs        → rebind inline slots, PC=0 */               \
  X(TAIL_CALL)   /* u8 nargs        → [fn, a0..aN-1]; reuse env, jump to fn */   \
  X(CONS)        /* pop b, pop a → push (cons a b) */                            \
  X(CAR)         /* pop pair     → push car */                                   \
  X(CDR)         /* pop pair     → push cdr */                                   \
  X(LIST)        /* u8 n         → pop n values → push list */                   \
  /* Fused "local ± small const" / "local < const" superinstructions: collapse  \
     LOAD_SLOT+LOAD_FIX+OP into one dispatch (peephole for (- n 1), (< n 2)).    \
   */                                                                            \
  X(SLOT_ADD_FIX) /* u8 slot, i16 imm → push inline_vals[slot] + imm */          \
  X(SLOT_SUB_FIX) /* u8 slot, i16 imm → push inline_vals[slot] - imm */          \
  X(SLOT_LT_FIX)  /* u8 slot, i16 imm → push (inline_vals[slot] <  imm) */       \
  X(SLOT_LE_FIX)  /* u8 slot, i16 imm → push (inline_vals[slot] <= imm) */       \
  X(SLOT_GT_FIX)  /* u8 slot, i16 imm → push (inline_vals[slot] >  imm) */       \
  X(SLOT_GE_FIX)  /* u8 slot, i16 imm → push (inline_vals[slot] >= imm) */       \
  X(SLOT_IS_FIX)  /* u8 slot, i16 imm → push (inline_vals[slot] is imm): a     \
                     fixnum-immediate `is` is a tagged-bit compare (two fixnums  \
                     equal iff identical bits; a non-fixnum slot never bit-      \
                     equals a fixnum, so yields nil). */                         \
  /* Slot-vs-slot compare — fuses LOAD_SLOT+LOAD_SLOT+<cmp> (compile_for). */    \
  X(SLOT_LE_SLOT) /* u8 slot_a, u8 slot_b → push (slot_a <= slot_b) */           \
  /* Vector ops — direct opcodes so vec-heavy loops stay in the VM (else the   \
     compiler bails to AST and deep recursion overflows the C stack). */         \
  X(VEC_REF)  /* pop i, pop v → push v[i] */                                     \
  X(VEC_SET)  /* pop val, pop i, pop v → v[i]=val, push val */                   \
  X(VEC_LEN)  /* pop v        → push v->len (fixnum) */                          \
  X(VEC_NEW)  /* pop init, pop n → push (vec n init) */                          \
  X(SQRT_INT) /* pop n        → push (sqrt-int n) */                             \
  X(ABS)      /* pop a        → push (abs a) — fixnum/float, FIXMIN→float */     \
  X(NMAX)     /* pop b, pop a → push numeric max (value-preserving) */           \
  X(NMIN)     /* pop b, pop a → push numeric min (value-preserving) */           \
  X(LENGTH)   /* pop list     → push (length list) — walk cons chain */          \
  X(PUSH_HANDLER) /* u8 handler_idx, u8 finally_idx → push a try handler onto  \
                     the heap handler stack (idx into the const pool; sentinel   \
                     0xff = none). Emitted by compile_try ONLY for a tail-       \
                     position try, whose body then trampolines;                  \
                     handler/finally run at error-time / OP_RET-time (see                                            \
                     vm_run). */                                                 \
  X(SETQ_DYN) /* u8 idx (symbol) → pop v; setq_store_symbol — nearest binding  \
                 else top-level; push v back (setq returns the value) */         \
  X(STORE_FREE) /* u8 idx (symbol) → pop v; assign_store_symbol — `=`/`setf` \
                   to a non-slot (free/global): nearest binding else CURRENT                          \
                   env (differs from SETQ_DYN, matching updatebang); push v                            \
                   back */
// clang-format on
typedef enum {
#define X(n) OP_##n,
  OPCODE_LIST(X)
#undef X
      OP_MAX
} alc_op;

/* Mutable random-access array. Held by exp_t->ptr when type==EXP_VECTOR.
   Storage layout: an 8-byte header followed by `cap` cells of 8 bytes.
   Cell payload type depends on vec_kind(exp_t):
     VEC_KIND_GEN: cells are owning exp_t* (heterogeneous fallback)
     VEC_KIND_I64: cells are raw int64_t (no boxing)
     VEC_KIND_F64: cells are raw double  (no boxing)
   The live window into the storage is on the parent exp_t — vec_win.start
   and vec_win.end. len = end - start. cap >= end always. Pop-front and
   pop-back move the window without shifting; only push beyond cap (or
   front-grow when start==0) reallocates.

   The cells are NOT inline as a flex-array — that lets the JIT mark-from
   shape compute &cell[i] = (ptr) + sizeof(alc_vec_t) + 8*i with the same
   +8 offset the old {int64_t len; data[]} layout used, so the existing
   JIT instruction streams keep their address arithmetic intact. */
typedef struct {
  int32_t cap;     /* cells allocated (always 8 bytes each) */
  int32_t hdr_pad; /* reserved; keeps payload 8-byte aligned + named so
                      future use of this slot is documented */
} alc_vec_t;

/* Cell accessors. e is the exp_t*, i is 0-based logical index (within the
   window [0, vec_len(e))). All three kinds share the same byte layout
   (8B/cell at offset sizeof(alc_vec_t)). */
#define vec_len(e) ((int64_t)((e)->vec_win.end - (e)->vec_win.start))
#define vec_cap(e) ((int64_t)((alc_vec_t *)(e)->ptr)->cap)
#define vec_base(e) ((char *)(e)->ptr + sizeof(alc_vec_t))
#define vec_gen_at(e, i)                                                       \
  (((struct exp_t **)vec_base(e))[(e)->vec_win.start + (i)])
#define vec_i64_at(e, i) (((int64_t *)vec_base(e))[(e)->vec_win.start + (i)])
#define vec_f64_at(e, i) (((double *)vec_base(e))[(e)->vec_win.start + (i)])
/* Raw double* to the first cell of a VEC_KIND_F64 window. Equivalent to
   &vec_f64_at(v,0); prefer this for pointer-arithmetic bulk ops where
   `cells[i]` reads/writes are clearer than repeated vec_f64_at calls. */
#define VEC_F64_CELLS(v) ((double *)vec_base(v) + (v)->vec_win.start)

/* Inline binding slots per env. The bytecode VM uses slot indices to
   bypass the per-name dict lookup; bytecode_t.param_keys mirrors the
   shape so vm_invoke_values can memcpy them in. Defined here (above
   bytecode_t) so the array dimensions can be expressed symbolically
   instead of hardcoding "6" and racing the value below. */
#define ENV_INLINE_SLOTS 6

/* Global-resolution cache slot. One per consts[] entry; lazily allocated.
   Stores the last lookup result for an OP_LOAD_GLOBAL symbol along with
   the generation it was cached at. Mutations to global bindings bump
   alcove_global_gen; stale entries are detected and re-looked-up. */
typedef struct gcache_entry {
  struct exp_t *val; /* not refcounted by us — global env keeps it alive */
  uint64_t gen;
} gcache_entry;

/* One entry of a bytecode's pc→source-location table: "from this code offset
   on, the source line/col is this." Built at compile time, read ONLY on the
   error path (cold) to point a runtime error at the precise failing form —
   never touched by the VM dispatch loop, so it adds zero per-instruction cost.
 */
typedef struct bc_loc_t {
  int pc;   /* code offset where this form's emitted ops begin */
  int line; /* 1-based source line (raw reader line; mapped at display time) */
  int col;  /* 1-based column, or 0 */
} bc_loc_t;

typedef struct bytecode_t {
  struct exp_t *content; /* original arguments list of the compiled lambda */
  uint8_t *code;
  int ncode;
  exp_t **consts; /* owned refs — unref on free */
  int nconsts;
  gcache_entry *gcache; /* lazily allocated, sized = nconsts */
  /* Cached parameter info from compile_lambda — lets vm_invoke_values
     skip the per-call walk over fn->content (a cons-list of param
     symbols). nparams is the canonical arity; param_keys[i] is the
     borrowed string ptr from the i-th param symbol's ->ptr. */
  uint8_t nparams;
  char *param_keys[ENV_INLINE_SLOTS];
  /* Optional type-annotation hints (TYPE_HINT_* codes; 0 = none), parallel to
     param_keys; ret_hint is the declared return type. Recorded from def-time
     annotations (def f (x :int) :f64 ...). Phase 2: the JIT reads these to emit
     a guarded specialized fast-path for shapes it would otherwise reject — the
     guard still deopts on a wrong hint, so they only ever affect speed. */
  uint8_t param_hints[ENV_INLINE_SLOTS];
  uint8_t ret_hint;
  /* Name of the function this bytecode belongs to (borrowed from
     fn->meta). NULL for anonymous lambdas. JIT shape matchers compare
     this against the callee symbol of recursive CALL_GLOBAL ops to
     reject "same shape, different callee" misfires that would otherwise
     silently rewrite (def myfib (n) (... (otherfn ...) (otherfn ...)))
     as iterative-fib. */
  const char *self_name;
  /* Optional native-code fast path. When set, vm_invoke_values calls
     this directly instead of running the dispatch loop. Returns the
     result exp_t* (NULL signals deopt → caller falls back to vm_run).
     Populated by jit_compile() when ALCOVE_JIT is enabled and the
     bytecode matches a recognized shape. */
  exp_t *(*jit)(struct env_t *env);
  void *jit_mem; /* mmap'd page; freed via munmap on bytecode_free */
  size_t jit_size;
  /* Set for closures (lambdas compiled with a captured env). A closure's
     free variables live in the captured env chain, not the global env, so
     the gcache (keyed on alcove_global_gen, which only tracks GLOBAL
     mutations) would serve stale values. With this set, OP_LOAD_GLOBAL /
     OP_CALL_GLOBAL always do a fresh lookup and never cache. */
  uint8_t no_gcache;
  /* pc→source-location table (ascending pc), for precise runtime-error
     locations. NULL when line tracking was off at compile time. Owned. */
  bc_loc_t *locs;
  int nlocs;
} bytecode_t;

/* Resolve a lambda's params list, accounting for the union overload
   on `content`/`bc`. After compile_lambda runs, fn->bc occupies the
   same slot as fn->content, and the original params chain has been
   migrated to bc->content. Use this helper everywhere a caller needs
   the params of a possibly-compiled lambda — touching fn->content
   directly on a FLAG_COMPILED lambda dereferences a bytecode_t* as
   an exp_t* and corrupts memory. */
static inline struct exp_t *lambda_params(struct exp_t *fn) {
  return (fn->flags & FLAG_COMPILED) ? fn->bc->content : fn->content;
}

extern uint64_t alcove_global_gen;

void bytecode_free(bytecode_t *bc);
void disasm_bytecode(bytecode_t *bc); /* opcode-by-opcode dump for debugging */
/* Optional type-annotation codes (JIT speculation hints). 0 = unannotated.
   Single source of truth: this X-macro list generates the enum below AND the
   code→keyword / keyword→code maps in alcove.c, so a new hint can't be added to
   one and forgotten in another. (code, keyword) per row. */
#define ALC_TYPE_HINTS(X)                                                      \
  X(TYPE_HINT_INT, ":int") /* tagged fixnum */                                 \
  X(TYPE_HINT_F64, ":f64") /* double */                                        \
  X(TYPE_HINT_VEC_F64, ":vec-f64")                                             \
  X(TYPE_HINT_VEC_I64, ":vec-i64")
enum {
  TYPE_HINT_NONE = 0,
#define X(code, kw) code,
  ALC_TYPE_HINTS(X)
#undef X
};
/* param_hints (ENV_INLINE_SLOTS TYPE_HINT_* codes, or NULL) + ret_hint are the
   def-time type annotations, recorded into the bytecode for the JIT. */
int compile_lambda(exp_t *fn, int is_closure, const uint8_t *param_hints,
                   uint8_t ret_hint);        /* 1 on success, 0 on fallback */
exp_t *vm_run(exp_t *fn, struct env_t *env); /* runs bytecode; returns owned */
#ifdef ALCOVE_JIT
int jit_compile(bytecode_t *bc); /* 1 if JIT'd, 0 otherwise */
#endif

typedef struct exp_tfunc {
  unsigned short int flags;
  /* clone / clone_flag: RESERVED. The accessor macros __CLONE__ /
     __CLONE_FLAG__ exist, but the engine does not currently invoke either (no
     deep-copy path calls them) — they are part of the registration struct for
     forward compatibility, so a module may set them without effect today. */
  exp_t *(*clone)(exp_t *this);      /* clone exp_t */
  exp_t *(*clone_flag)(exp_t *this); /* clone exp_t and flag as persistent*/
  exp_t *(*load)(exp_t *e,
                 FILE *stream); /* load object as serialized data from stream */
  exp_t *(*dump)(exp_t *this,
                 FILE *stream); /* serialized object this to stream */
  /* Custom (foreign) types only: destroy frees the C payload (this->ptr) and
     unrefs any exp_t it owns, at refcount zero — the exp_t shell is reclaimed
     by the host. print writes a representation (optional; a default #<name@ptr>
     is used when NULL). Built-in types leave these NULL. */
  void (*destroy)(exp_t *this);
  void (*print)(exp_t *this);
} exp_tfunc;
/* Capacity of the exp_tfuncList dispatch table. Built-in types occupy ids
   1..EXP_MAXSIZE-1; custom types registered by native modules
   (alcove_register_type) get ids EXP_MAXSIZE..ALCOVE_TYPE_CAP-1. The 2-byte
   exp_t->type field holds any of these with room to spare. */
#define ALCOVE_TYPE_CAP 256

#define __CLONE__(e)                                                           \
  (exp_tfuncList[e->type] && exp_tfuncList[e->type]->clone                     \
       ? exp_tfuncList[e->type]->clone(e)                                      \
       : NULL)
#define __CLONE_FLAG__(e)                                                      \
  (exp_tfuncList[e->type] && exp_tfuncList[e->type]->clone_flag                \
       ? exp_tfuncList[e->type]->clone_flag(e)                                 \
       : NULL)
/* Tag-aware type discriminator: returns the logical type for both heap
   exp_t and tagged immediates. Used to dispatch into exp_tfuncList. */
#define TYPEOF_E(e)                                                            \
  (is_ptr(e) ? (int)(e)->type                                                  \
             : (TAG(e) == TAG_FIX ? (int)EXP_NUMBER                            \
                                  : (TAG(e) == TAG_CHAR ? (int)EXP_CHAR : 0)))

#define __LOAD__(e, s)                                                         \
  (exp_tfuncList[TYPEOF_E(e)] && exp_tfuncList[TYPEOF_E(e)]->load              \
       ? exp_tfuncList[TYPEOF_E(e)]->load(e, s)                                \
       : NULL)
#define __DUMP__(e, s)                                                         \
  (exp_tfuncList[TYPEOF_E(e)] && exp_tfuncList[TYPEOF_E(e)]->dump              \
       ? exp_tfuncList[TYPEOF_E(e)]->dump(e, s)                                \
       : NULL)
#define __DUMPABLE__(e)                                                        \
  (exp_tfuncList[TYPEOF_E(e)] && exp_tfuncList[TYPEOF_E(e)]->dump ? 1 : 0)

typedef struct token_t {
  int size;
  int maxsize;
  char *data;
} token_t;

#define TOKENMINSIZE 32

typedef struct keyval_t {
  int64_t timestamp; /* updated if persistant data */
  void *key;
  union {
    void *raw;
    exp_t *val;
    int64_t s64;
  };
  struct keyval_t *next;
} keyval_t;

/* Key Value Hash Table*/
typedef struct kvht_t {
  keyval_t **table;
  unsigned long size;
  unsigned long sizemask;
  unsigned long used;
} kvht_t;

#define DICT_KVHT_INITIAL_SIZE 32

typedef struct dict_t {
  void *meta;
  kvht_t ht[2];
} dict_t;

/* How many function-parameter bindings an env_t holds inline before
   spilling to the dict. Sized for the common recursive-function case
   where all params fit inline, so invoke() skips the dict/table/keyval
   allocs entirely. Tune up if deeper arities become common.
   Defined above (near bytecode_t.param_keys) so both arrays share the
   same constant. */

typedef struct env_t {
  struct env_t *root;
  exp_t *callingfnc;
  dict_t *d;    /* lazy — used for let/with and inline-slot overflow */
  int nref;     /* refcount */
  int n_inline; /* number of filled inline slots */
  /* Set when a lambda/macro captures THIS env as its closure scope (the only
     way a self-closure cycle can form). destroy_env skips the cycle-breaker
     scan when it's clear — the common case (no closure ever bound here), which
     is otherwise a per-frame-teardown cost on all deep recursion. */
  int has_closure;
  /* Inline bindings for function-invocation params. Keys BORROW from
     the lambda header symbol's ptr; caller (invoke) must hold a ref to
     the lambda while the env is live. Vals own a ref. */
  char *inline_keys[ENV_INLINE_SLOTS];
  exp_t *inline_vals[ENV_INLINE_SLOTS];
} env_t;

/* shard_t — bundle of per-worker runtime state. Today there is exactly
   one shard (`main_shard`); the multithreaded reactor will spawn one
   shard per worker, each with its own env_arena. Routing make_env /
   destroy_env through `current_shard` (TLS) keeps allocators thread-
   local and avoids contention on the bump pointer. Single-threaded
   builds resolve `current_shard` to the same `main_shard` every time,
   so the indirection is one extra TLS load per env op — well under
   noise on the benchmark suite. */
#define ENV_ARENA_SLOTS 8192
typedef struct shard_t {
  env_t *arena;     /* base of arena array (pointer to static storage) */
  env_t *arena_sp;  /* bump pointer; LIFO rolled back on destroy */
  env_t *arena_end; /* arena + ENV_ARENA_SLOTS */
  /* Cross-thread inbox: peer shards enqueue messages, this shard's
     reactor drains on each wake. No producer exists today; Step 2.5
     (cross-shard ops) wires up the first one. Initialized by
     shard_runtime_init, torn down by shard_runtime_destroy. */
  mpsc_queue_t inbox;
  alc_wake_t wake;
  int runtime_ready; /* 0 until shard_runtime_init succeeds; -1 on failure */
  /* RESP keyspace — lock-free open-addressed hash with atomic exp_t *
     slots. Today every shard's `kv` field points to the same global
     `g_resp_kv` (created lazily in resp.c on first SET); all reactors
     share one keyspace and rely on CAS + epoch reclamation to stay
     correct under concurrency. The pointer lives on shard_t to keep
     the eventual partitioning option open without touching call sites. */
  struct lfkv *kv;
  int64_t db_last_sweep_us; /* TTL eviction sweep gate */
} shard_t;
extern shard_t main_shard;
extern ALCOVE_TLS shard_t *current_shard;

/* Lifecycle. shard_main is the future pthread entrypoint; today the
   main thread calls it directly with &main_shard for N=1. */
int shard_runtime_init(shard_t *sh);
void shard_runtime_destroy(shard_t *sh);
int shard_main(shard_t *sh, int port);
/* Hot loops (make_env / destroy_env) cache `current_shard` once and inline the
   bounds check `env >= sh->arena && env < sh->arena_end` — the per-iteration
   TLS reload was a measurable hit on env-heavy benchmarks. */

typedef struct lispProc {
  char *name;
  int arity; /* reserved — not yet enforced. -1 = variadic. */
  int flags; /* FLAG_TAIL_AWARE / FLAG_APPLICATIVE / FLAG_UNSAFE; see alcove.h.
                FLAG_UNSAFE marks the security tier the old `level` field
                reserved — gated under --safe (see invoke_internal). */
  const char *doc; /* one-line help string; NULL = undocumented. */
  lispCmd *cmd;
} lispProc;

/* Functions declaration */

int64_t gettimeusec();

/* memory management */
exp_t *refexp(exp_t *e);
void *memalloc(size_t count, size_t size);

/* ALCOVE_SAFE — opt-in runtime type assertions, INDEPENDENT of NDEBUG, so a
   fully-optimized build can still be run with the checks on to validate the
   "we statically know this object's type here" assumptions that the fast paths
   below rely on. Off by default → SAFE_ASSERT compiles to nothing (zero cost).
   Build a checked binary with `cc -DALCOVE_SAFE=1 …`. */
#ifndef ALCOVE_SAFE
#define ALCOVE_SAFE 0
#endif
#if ALCOVE_SAFE
#include <assert.h>
#define SAFE_ASSERT(c) assert(c)
#else
#define SAFE_ASSERT(c) ((void)0)
#endif

/* unrefexp fast path — the inline wrapper is defined just after the immortal
   singletons it references (is_immortal). unrefexp runs on every value drop;
   keeping the hot part inline (immediate/singleton skip + the live decrement)
   and calling out only when a refcount actually reaches 0 avoids a function
   call for the overwhelmingly common cases (drop a fixnum, or decrement a
   still-shared object). unrefexp_free assumes ret<=0; SAFE_ASSERT verifies the
   object invariants under -DALCOVE_SAFE=1. NOT static: the inline unrefexp()
   below calls it, so a native module (which includes only alcove.h) must
   resolve it against the host binary at dlopen (-rdynamic) rather than need a
   local definition. */
int unrefexp_free(exp_t *e, int ret);
static inline int unrefexp(exp_t *e);

/* env management and exception handling inside env*/
env_t *ref_env(env_t *env);
env_t *make_env(env_t *rootenv);
void *destroy_env(env_t *env);
exp_t *set_return_point(env_t *env);

/* dict management */
dict_t *create_dict();
int destroy_dict(dict_t *d);
int dump_dict(dict_t *d, FILE *stream);
unsigned int bernstein_hash(unsigned char *key, int len);
unsigned int
bernstein_hash_z(const char *key); /* fused strlen+hash, C-strings */
unsigned int bernstein_uhash(unsigned char *key, int len);
keyval_t *set_get_keyval_dict(dict_t *d, char *key, exp_t *val);
exp_t *set_keyval_dict_timestamp(dict_t *d, char *key, int64_t timestamp);
int64_t get_keyval_dict_timestamp(dict_t *d, char *key);
int del_keyval_dict(dict_t *d, char *key);

/* lisp */
exp_t *error(int errnum, exp_t *id, env_t *env, const char *err_message, ...);
static exp_t *
make_nil(); /* fresh heap pair (content=next=NULL) — for builders */
#define MAKE_TYPED(name, t, p)                                                 \
  exp_t *(name) = make_nil();                                                  \
  (name)->type = (t);                                                          \
  (name)->ptr = (void *)(p)

#define INIT_TYPED(name, t, p)                                                 \
  (name) = make_nil();                                                         \
  (name)->type = (t);                                                          \
  (name)->ptr = (void *)(p)
exp_t *make_char(uint32_t c);
exp_t *make_node(exp_t *node);
exp_t *make_internal(lispCmd *cmd, int flags);
/* Runtime registration of a name → C function as an alcove builtin.
   Returns 0 on success, -1 if called before alcove's init has set up
   the reserved-symbol dict. The web build's JS shim uses this with
   Module.addFunction to inject browser-side builtins.

   OWNERSHIP CONTRACT — the `fn` you register MUST consume its call form:
   `unrefexp(e)` exactly once before returning (after reading every arg). The
   interpreter passes `e` with one ref it expects you to release, exactly as
   every core builtin does (e.g. conscmd). This always mattered; it became
   load-bearing once non-tail-aware builtins (FLAG_APPLICATIVE) compile to a
   real OP_CALL_GLOBAL — the bytecode VM then synthesizes a FRESH call form per
   call, so forgetting unrefexp(e) leaks one form per call (unbounded in a hot
   loop). tail_aware!=0 builtins are special forms and are exempt from the
   FLAG_APPLICATIVE fast path (but still must consume e). See
   examples/embed/nativemod.c. */
int alcove_register_cmd(const char *name, lispCmd *fn, int tail_aware);
/* Attach a "values" fast-path implementation (lispCmdV) to an
   already-registered non-tail-aware builtin. The compiled fast path then calls
   `fnv` directly with the evaluated args (no synthesized call form). The normal
   lispCmd stays as the fallback for the AST / apply / map paths. Returns 0 on
   success, -1 if `name` isn't a registered internal. */
int alcove_set_cmd_values(const char *name, lispCmdV *fnv);
/* Helpers for embedders implementing builtins outside the alcove TU
   (e.g. JS-side builtins in the WASM build). Evaluate the Nth (0-indexed)
   argument of the in-flight call and return it as a C int / C string;
   wrap a C int as a fixnum exp_t* for return.
   alcove_arg_string CAVEATS: the returned char* points into a static
   round-robin of 4 buffers of 1024 bytes — a string longer than 1023 bytes is
   silently truncated, and the 5th alcove_arg_string call within one builtin
   reuses the 1st buffer (so copy the string if you need it past 4 calls or one
   builtin). For full-fidelity / long strings, EVAL the arg yourself and read
   exp_text(). */
int alcove_arg_int(exp_t *e, env_t *env, int n);
const char *alcove_arg_string(exp_t *e, env_t *env, int n);
exp_t *alcove_make_int(int v);
/* C embedding entry points (see the alcove_init comment in alcove.c and
   examples/embed/). alcove_init() brings the engine up and returns the global
   env (IDEMPOTENT — a second call returns the same env). alcove_eval_string()
   evaluates s-expressions and returns the last value as an OWNED ref the caller
   must unrefexp (or an error exp_t — test iserror; returns an error, not nil,
   if the engine isn't initialized or fmemopen fails). Build a host by defining
   ALCOVE_NO_MAIN before #include "alcove.c" so this TU's main() is omitted.
   A native module (loaded by (require "x.so")) instead exports
       int alcove_module_init(void);   // return 0 on success, nonzero to fail
   which registers its builtins via alcove_register_cmd; on a nonzero return
   the host reports a generic load failure, so the module should print its own
   diagnostic (it has no env_t to call error() with). */
env_t *alcove_init(void);
exp_t *alcove_eval_string(const char *src);
/* Custom (foreign) object types — a native module defines its own value type
   with dump/load serialization. alcove_register_type(name, ops) reserves a
   2-byte runtime type id and copies `ops` (its dump/load/destroy/print);
   idempotent — re-registering the same name returns the same id. `name` should
   be module-qualified (e.g. "mymod/matrix"); it's the DURABLE identity used in
   db.dump (the runtime id is per-process). Returns 0 if the table is full.
   alcove_make_foreign(id, ptr) builds a value of that type carrying `ptr` (the
   module's C struct, freed by the type's destroy hook at refcount zero).
   alcove_foreign_ptr / alcove_is_foreign read it back. */
unsigned short alcove_register_type(const char *name, const exp_tfunc *ops);
exp_t *alcove_make_foreign(unsigned short type_id, void *ptr);
void *alcove_foreign_ptr(const exp_t *e);
int alcove_is_foreign(const exp_t *e, unsigned short type_id);
exp_t *make_tree(exp_t *root, exp_t *node1);

/* Value constructors for embedders/modules. OWNERSHIP: make_string /
   make_symbol / make_floatf return a fresh HEAP value (nref=1) you must
   unrefexp when done (or hand to a sink — e.g. set_get_keyval_dict / a builtin
   return — that takes it). make_integeri / make_char / alcove_make_int return a
   TAGGED IMMEDIATE (no heap, never refcounted — do NOT unref). nil/t
   (NIL_EXP/TRUE_EXP) are immortal singletons (no unref). See
   examples/embed/README.md. */
exp_t *make_string(char *str, int length);
exp_t *make_symbol(char *str, int length);

/* Canonical singletons — allocated once at main() startup; refexp /
   unrefexp short-circuit on them so they never reach 0. */
extern exp_t *nil_singleton;
extern exp_t *true_singleton;
extern exp_t *gen_done_singleton; /* generator exhaustion sentinel */
#define NIL_EXP (nil_singleton)
#define TRUE_EXP (true_singleton)
#define GEN_DONE (gen_done_singleton)
#define isgen_done(e) ((e) == gen_done_singleton)

/* unrefexp (see the forward decl above). is_immortal folds the immediate/NULL
   and three-singleton skips into one test; a live object's decrement returns
   inline; only an object whose count hits 0 (or a double-free, ret<0) reaches
   the out-of-line unrefexp_free. */
static inline int unrefexp(exp_t *e) {
  if (is_immortal(e))
    return is_ptr(e) ? 1 : 0;
  int ret = REFCOUNT_DEC(&e->nref);
  if (ret > 0)
    return ret;
  return unrefexp_free(e, ret);
}
exp_t *make_quote(exp_t *node);
exp_t *make_integer(char *str);
exp_t *make_integeri(int64_t i);
exp_t *make_float(char *str);
exp_t *make_floatf(expfloat f);
size_t loadtype(FILE *stream, unsigned short int *type);
size_t dumptype(FILE *stream, unsigned short int *type);
size_t loadsize_t(FILE *stream, size_t *len);
size_t dumpsize_t(FILE *stream, size_t *len);
exp_t *load_exp_t(FILE *stream);
exp_t *dump_exp_t(exp_t *e, FILE *stream);
exp_t *load_char(exp_t *e, FILE *stream);
exp_t *dump_char(exp_t *e, FILE *stream);
char *load_str(char **pptr, FILE *stream);
exp_t *load_string(exp_t *e, FILE *stream);
char *dump_str(char *ptr, FILE *stream);
exp_t *dump_string(exp_t *e, FILE *stream);
exp_t *load_number(exp_t *e, FILE *stream);
exp_t *dump_number(exp_t *e, FILE *stream);
exp_t *load_float(exp_t *e, FILE *stream);
exp_t *dump_float(exp_t *e, FILE *stream);
exp_t *load_rational(exp_t *e, FILE *stream);
exp_t *dump_rational(exp_t *e, FILE *stream);
exp_t *load_decimal(exp_t *e, FILE *stream);
exp_t *dump_decimal(exp_t *e, FILE *stream);
exp_t *load_symbol(exp_t *e, FILE *stream);
exp_t *dump_symbol(exp_t *e, FILE *stream);
exp_t *load_pair(exp_t *e, FILE *stream);
exp_t *dump_pair(exp_t *e, FILE *stream);
exp_t *load_lambda(exp_t *e, FILE *stream);
exp_t *dump_lambda(exp_t *e, FILE *stream);
exp_t *load_macro(exp_t *e, FILE *stream);
exp_t *dump_macro(exp_t *e, FILE *stream);
exp_t *make_atom(char *str, int length);
exp_t *callmacrochar(FILE *stream, unsigned char x);
exp_t *lookup(exp_t *e, env_t *env);
exp_t *updatebang(exp_t *key, env_t *env, exp_t *val);
exp_t *escapereader(FILE *stream, token_t **ptoken, int lastchar);
exp_t *reader(FILE *stream, unsigned char clmacro, int keepwspace);
exp_t *evaluate(exp_t *e, env_t *env);
/* EVAL() — canonical borrowed→owned wrapper. evaluate() consumes its input ref,
   so calling it on a borrowed car/cadr/caddr pointer causes a premature free.
   Prefer EVAL(e, env) over evaluate(refexp(e), env) at call sites. */
#define EVAL(e, env) evaluate(refexp(e), env)
void tree_add_node(exp_t *tree, exp_t *node);
void pair_add_node(exp_t *pair, exp_t *node);
void print_node(exp_t *node);
int istrue(exp_t *e);
int isequal(exp_t *cur1, exp_t *cur2);
int isoequal(exp_t *cur1, exp_t *cur2);
exp_t *letcmd(exp_t *e, env_t *env);
exp_t *withcmd(exp_t *e, env_t *env);
exp_t *var2env(exp_t *e, exp_t *var, exp_t *val, env_t *env, int evalexp);
exp_t *invoke(exp_t *e, exp_t *fn, env_t *env);
exp_t *expandmacro(exp_t *e, exp_t *fn, env_t *env);
exp_t *invokemacro(exp_t *e, exp_t *fn, env_t *env);

/* lisp command */
exp_t *quotecmd(exp_t *e, env_t *env);
exp_t *ifcmd(exp_t *e, env_t *env);
exp_t *equalcmd(exp_t *e, env_t *env);
exp_t *persistcmd(exp_t *e, env_t *env);
exp_t *ispersistentcmd(exp_t *e, env_t *env);
exp_t *forgetcmd(exp_t *e, env_t *env);
exp_t *savedbcmd(exp_t *e, env_t *env);
exp_t *cmpcmd(exp_t *e, env_t *env);
exp_t *pluscmd(exp_t *e, env_t *env);
exp_t *multiplycmd(exp_t *e, env_t *env);
exp_t *minuscmd(exp_t *e, env_t *env);
exp_t *dividecmd(exp_t *e, env_t *env);
exp_t *sqrtcmd(exp_t *e, env_t *env);
exp_t *expcmd(exp_t *e, env_t *env);
exp_t *exptcmd(exp_t *e, env_t *env);
exp_t *prcmd(exp_t *e, env_t *env);
exp_t *prncmd(exp_t *e, env_t *env);
exp_t *oddcmd(exp_t *e, env_t *env);
exp_t *docmd(exp_t *e, env_t *env);
exp_t *whencmd(exp_t *e, env_t *env);
exp_t *whilecmd(exp_t *e, env_t *env);
exp_t *repeatcmd(exp_t *e, env_t *env);
exp_t *withtimelimitcmd(exp_t *e, env_t *env);
extern const char doc_with_time_limit[];
exp_t *withmemlimitcmd(exp_t *e, env_t *env);
extern const char doc_with_memory_limit[];
exp_t *heapstatscmd(exp_t *e, env_t *env);
extern const char doc_heap_stats[];
exp_t *gccyclescmd(exp_t *e, env_t *env); /* gc.h */
extern const char doc_gc_cycles[];
exp_t *weakcmd(exp_t *e, env_t *env); /* weak.h */
extern const char doc_weak[];
exp_t *weakgetcmd(exp_t *e, env_t *env);
extern const char doc_weak_get[];
exp_t *weakpcmd(exp_t *e, env_t *env);
extern const char doc_weakp[];
exp_t *lforcmd(exp_t *e, env_t *env); /* comprehensions.h */
extern const char doc_lfor[];
exp_t *sforcmd(exp_t *e, env_t *env);
extern const char doc_sfor[];
exp_t *dforcmd(exp_t *e, env_t *env);
extern const char doc_dfor[];
exp_t *gforcmd(exp_t *e, env_t *env);
extern const char doc_gfor[];
exp_t *watchcmd(exp_t *e, env_t *env); /* watch.h */
extern const char doc_watch[];
exp_t *unwatchcmd(exp_t *e, env_t *env);
extern const char doc_unwatch[];
exp_t *watchedpcmd(exp_t *e, env_t *env);
extern const char doc_watchedp[];
exp_t *setvalidatorcmd(exp_t *e, env_t *env);
extern const char doc_set_validator[];
exp_t *raisecmd(exp_t *e, env_t *env); /* builtins_log.h */
extern const char doc_raise[];
exp_t *errorcodescmd(exp_t *e, env_t *env);
extern const char doc_error_codes[];
exp_t *allocfailaftercmd(exp_t *e, env_t *env);
extern const char doc_alloc_fail_after[];
exp_t *andcmd(exp_t *e, env_t *env);
exp_t *orcmd(exp_t *e, env_t *env);
exp_t *nocmd(exp_t *e, env_t *env);
exp_t *iscmd(exp_t *e, env_t *env);
exp_t *isntcmd(exp_t *e, env_t *env);
exp_t *isocmd(exp_t *e, env_t *env);
exp_t *incmd(exp_t *e, env_t *env);
exp_t *casecmd(exp_t *e, env_t *env);
exp_t *forcmd(exp_t *e, env_t *env);
exp_t *eachcmd(exp_t *e, env_t *env);
exp_t *timecmd(exp_t *e, env_t *env);
exp_t *inspectcmd(exp_t *e, env_t *env);
exp_t *disasmcmd(exp_t *e, env_t *env);
exp_t *dircmd(exp_t *e, env_t *env);
/* stdlib batch */
exp_t *modcmd(exp_t *e, env_t *env);
exp_t *abscmd(exp_t *e, env_t *env);
exp_t *maxcmd(exp_t *e, env_t *env);
exp_t *mincmd(exp_t *e, env_t *env);
exp_t *lengthcmd(exp_t *e, env_t *env);
exp_t *nthcmd(exp_t *e, env_t *env);
exp_t *seqcmd(exp_t *e, env_t *env);
exp_t *firstcmd(exp_t *e, env_t *env);
exp_t *restcmd(exp_t *e, env_t *env);
exp_t *conjcmd(exp_t *e, env_t *env);
exp_t *intocmd(exp_t *e, env_t *env);
exp_t *typeofcmd(exp_t *e, env_t *env);
exp_t *defstructcmd(exp_t *e, env_t *env);
exp_t *defmulticmd(exp_t *e, env_t *env);
exp_t *defmethodcmd(exp_t *e, env_t *env);
exp_t *reversecmd(exp_t *e, env_t *env);
exp_t *appendcmd(exp_t *e, env_t *env);
exp_t *numberpcmd(exp_t *e, env_t *env);
exp_t *charpcmd(exp_t *e, env_t *env);
exp_t *yespcmd(exp_t *e, env_t *env);
exp_t *zeropcmd(exp_t *e, env_t *env);
exp_t *stringpcmd(exp_t *e, env_t *env);
exp_t *symbolpcmd(exp_t *e, env_t *env);
exp_t *pairpcmd(exp_t *e, env_t *env);
exp_t *fnpcmd(exp_t *e, env_t *env);
exp_t *exitcmd(exp_t *e, env_t *env);
exp_t *randomcmd(exp_t *e, env_t *env);
exp_t *applycmd(exp_t *e, env_t *env);
exp_t *mapcmd(exp_t *e, env_t *env);
exp_t *filtercmd(exp_t *e, env_t *env);
exp_t *reducecmd(exp_t *e, env_t *env);
exp_t *anypcmd(exp_t *e, env_t *env);
exp_t *allpcmd(exp_t *e, env_t *env);
exp_t *ffipcmd(exp_t *e, env_t *env);
exp_t *ffifncmd(exp_t *e, env_t *env);
exp_t *ffivfncmd(exp_t *e, env_t *env);
exp_t *fficallbackcmd(exp_t *e, env_t *env);
exp_t *ffistructcmd(exp_t *e, env_t *env);
exp_t *ffipackcmd(exp_t *e, env_t *env);
exp_t *ffiunpackcmd(exp_t *e, env_t *env);
/* Persistent/immutable map (HAMT) */
exp_t *hamtcmd(exp_t *e, env_t *env);
exp_t *hamtassoccmd(exp_t *e, env_t *env);
exp_t *hamtgetcmd(exp_t *e, env_t *env);
exp_t *hamtdissoccmd(exp_t *e, env_t *env);
exp_t *hamtcountcmd(exp_t *e, env_t *env);
exp_t *hamtcontainspcmd(exp_t *e, env_t *env);
exp_t *hamtkeyscmd(exp_t *e, env_t *env);
exp_t *hamtvalscmd(exp_t *e, env_t *env);
exp_t *hamtlistcmd(exp_t *e, env_t *env);
exp_t *hamtmergecmd(exp_t *e, env_t *env);
exp_t *hamtpcmd(exp_t *e, env_t *env);
/* Escape continuations */
exp_t *callcccmd(exp_t *e, env_t *env);
/* MessagePack codec */
exp_t *msgpackencodecmd(exp_t *e, env_t *env);
exp_t *msgpackdecodecmd(exp_t *e, env_t *env);
exp_t *sourcecmd(exp_t *e, env_t *env);
exp_t *veccmd(exp_t *e, env_t *env);
exp_t *vecrefcmd(exp_t *e, env_t *env);
exp_t *vecsetcmd(exp_t *e, env_t *env);
exp_t *veclencmd(exp_t *e, env_t *env);
exp_t *sqrtintcmd(exp_t *e, env_t *env);
exp_t *loaddbcmd(exp_t *e, env_t *env);
int loaddb_from_file(env_t *env); /* shared with main() for auto-load */
/* lisp macro */
exp_t *defcmd(exp_t *e, env_t *env);
exp_t *defncmd(exp_t *e, env_t *env);
exp_t *expandmacrocmd(exp_t *e, env_t *env);
exp_t *defmacrocmd(exp_t *e, env_t *env);
exp_t *defccmd(exp_t *e, env_t *env);
exp_t *fncmd(exp_t *e, env_t *env);
exp_t *conscmd(exp_t *e, env_t *env);
exp_t *evalcmd(exp_t *e, env_t *env);
exp_t *carcmd(exp_t *e, env_t *env);
exp_t *cdrcmd(exp_t *e, env_t *env);
exp_t *listcmd(exp_t *e, env_t *env);

/* ---------------- Clojure-style containers ----------------
   Three new exp_t types that can be Lisp values AND used by RESP.
   All three are mutable; mutation ops end in `!` per Scheme/alcove
   convention even though Clojure proper drops the bang.

   EXP_BLOB — binary-safe byte buffer. Stored as alc_blob_t (flexible
   array), so size and bytes share one alloc. Differs from EXP_STRING
   only in NUL-byte safety: EXP_STRING uses strlen everywhere.

   EXP_DICT — wraps alcove's own dict_t*. Keys are strings (printed
   form of whatever the user passed: keyword `:a` hashes as ":a",
   string "a" hashes as "a", so the two are distinct keys, matching
   Clojure semantics).

   EXP_LIST — doubly-linked node chain with head AND tail pointers,
   so push/pop on either end is O(1). EXP_PAIR is singly-linked; an
   RPOP on a long pair-list would be O(n). */

typedef struct {
  size_t len;
  char bytes[]; /* flex-array: header + payload in one alloc */
} alc_blob_t;

/* EXP_RATIONAL — exact fraction. den > 0, gcd(num,den) == 1. Defined here (not
   numeric.h) because print.h / equality run before that fragment is #included.
   Operations are in numeric.h. */
typedef struct {
  int64_t num; /* sign lives here */
  int64_t den; /* always > 0 */
} alc_rat_t;

/* EXP_DECIMAL — exact bounded base-10 number, value = coef * 10^(-scale).
   coef is a signed 128-bit integer bounded to <= 28 significant digits
   (rust_decimal class); scale is the count of fractional digits (0..28). The
   stored form is normalized (trailing fractional zeros trimmed; coef==0 has
   scale 0), so structural equality is value equality. Operations in numeric.h.
 */
typedef struct {
  __int128 coef;
  int32_t scale;
} alc_dec_t;
int dec_to_str(alc_dec_t *d,
               char *buf); /* defined in numeric.h; used by print.h */

typedef struct alc_listnode {
  struct exp_t *val; /* owned ref */
  struct alc_listnode *prev, *next;
} alc_listnode_t;

typedef struct {
  alc_listnode_t *head, *tail;
  long len;
} alc_list_t;

/* EXP_HAMT — persistent/immutable map (Hash Array Mapped Trie). Nodes are
   reference-counted and shared across map versions (structural sharing); the
   trie is an acyclic DAG so refcounting reclaims it without cycles. A node
   with bitmap==0 is a hash-collision bucket; otherwise each present slot
   holds a key/value entry (slot.key != NULL) or a child node. */
typedef struct hamt_node hamt_node;
typedef struct {
  struct exp_t *key; /* entry key; NULL ⇒ this slot holds a child node */
  union {
    struct exp_t *val;
    hamt_node *child;
  };
} hamt_slot;
struct hamt_node {
  int nref;
  uint32_t bitmap; /* present slots; 0 ⇒ collision bucket */
  int n;           /* slot count */
  hamt_slot slots[];
};
typedef struct {
  hamt_node *root; /* owned; NULL ⇒ empty map */
  int64_t count;
} hamt_t;

exp_t *make_blob(const char *bytes, size_t len);
exp_t *make_vector(int64_t n, exp_t *fill);
exp_t *make_dict_exp(void); /* fresh empty dict_t wrapped in exp_t */
exp_t *make_list_exp(void); /* fresh empty alc_list_t wrapped in exp_t */
size_t blob_len(exp_t *e);
const char *blob_bytes(exp_t *e);

/* Token */
token_t *tokenize(int v);
void freetoken(token_t *token);
void tokenadd(token_t *token, int v);
void tokenappend(token_t *token, char *src, int len);

#define true 1
#define false 0

/* Parser */
#define PARSER_TERMMACROMODE 4

/* --- Extracted from alcove.c --- */
struct compiler_env;
typedef struct compiler_t {
  uint8_t *code;
  int ncode;
  int code_cap;
  exp_t **consts;
  int nconsts;
  int consts_cap;
  /* slot_names[0..nparams) = lambda params; slot_names[nparams..nslots)
     = let/with-bound names. Scope-managed as a stack. */
  char *slot_names[ENV_INLINE_SLOTS];
  int nparams;
  const uint8_t
      *param_hints;      /* borrowed: per-param TYPE_HINT_* (or NULL). Lets
                            compile_expr treat a hinted param as a known
                            non-callable value for infix->prefix rewriting. */
  int nslots;            /* current total: nparams + active let/with bindings */
  int nlet_depth;        /* let/with/for nesting depth of the cursor */
  int capture_unsafe;    /* 1 if the lambda body may create an env-capturing
                            closure (fn/lambda/def/... anywhere) or push a try
                            handler — disables OP_TAIL_SELF inside a let body,
                            since TAIL_SELF mutates env->inline_vals in place.
                            Set once by a body pre-pass in compile_lambda. */
  const char *self_name; /* for self-tail-call detection; NULL in anon fn */
  int failed;
  /* Accumulating pc→source-location table (see bc_loc_t). Appended to as forms
     compile; transferred to bytecode_t at the end of compile_lambda. */
  bc_loc_t *locs;
  int nlocs;
  int locs_cap;
  int last_loc_line; /* last line recorded, to coalesce runs on the same line */
} compiler_t;

typedef struct {
  FILE *fp;
  char *path;
  char mode;   /* 'r' | 'w' | 'a' */
  int closed;
} alc_port_t;

#endif /* ALCOVE_H */
