/*
   Copyright (c) 2012 Abdelkader ALLAM abdelkader.allam@gmail.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
/* On macOS, _XOPEN_SOURCE puts the system headers into strict-POSIX mode, which
   hides the BSD/Darwin extensions this codebase relies on (vasprintf/asprintf,
   timegm, MAP_ANONYMOUS, MAP_JIT, pthread_jit_write_protect_np, INADDR_LOOPBACK).
   _DARWIN_C_SOURCE re-exposes them — the Darwin analogue of _GNU_SOURCE. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h> /* LLONG_MIN for hex literal overflow guard */
#include <locale.h> /* setlocale: make readline UTF-8 / multibyte aware */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>   /* clock_gettime(CLOCK_MONOTONIC) for (now-ms) */
#include <unistd.h> /* isatty for the readline REPL gate; needed even
                          when ALCOVE_JIT is off. */
#include <setjmp.h> /* OOM recovery: longjmp from a failed alloc to the eval boundary */
#include <signal.h> /* REPL Ctrl-C: cancel the current input instead of exiting */
#include <sys/resource.h> /* getrlimit(RLIMIT_STACK) for the stack-overflow guard */
/* Adder front end: a string->string transpiler that turns the
   whitespace/`:`-block surface syntax into ordinary alcove s-expressions
   before they reach reader(). Included UNCONDITIONALLY (not just in the adder
   build) because (require)/(load) are dialect-aware by file extension — a .adr
   module is transpiled via als_to_sexpr in BOTH the alcove and adder binaries,
   so an .alc program can require a .adr one and vice versa. adr.h is
   self-contained (its only ALCOVE_ALS* token is its own include guard). */
#include "adr.h"
#ifdef ALCOVE_ALS
#define ALCOVE_PROGNAME "adder"
#else
#define ALCOVE_PROGNAME "alcove"
#endif
#ifdef ALCOVE_WEB
#include <emscripten/emscripten.h>
#endif
#ifdef ALCOVE_JIT
#include <stddef.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <pthread.h>
#endif
#endif
// #include <jemalloc/jemalloc.h>
#include "alcove.h"
/* Pull in the lock-free keyspace decls early so the unified savedb
   writer (defined ~line 2500, well before the #include "lfkv.c" at
   the bottom) can call lfkv_foreach / lfkv_set / lfkv_set_expiry. */
#include "lfkv.h"

/* Multi-threaded build needs pthread for the FFI cache mutex (and future
   shard primitives). Header guards make this benign next to the
   macOS+JIT include above. */
#if !ALCOVE_SINGLE_THREADED
#include <pthread.h>
#endif

int toeval = 1;
dict_t *reserved_symbol = NULL;
/* name → docstring for user (def f (args) "doc" body...) functions, queried
   by the `doc` builtin after the builtin table misses. Lazily created. */
dict_t *user_doc = NULL;
/* Numbered keyspace databases (redis-style). Selected per-thread by
   (with-db n ...); db 0 is the shared main keyspace the RESP server uses,
   dbs 1..ALC_NDB-1 are separate keyspaces. The selector is thread-local so
   a REPL thread's (with-db ...) never perturbs server reactor threads
   (which always see db 0). resp.c (included below) reads this in the single
   db-aware chokepoint resp_kv_current(). */
#define ALC_NDB 16
ALCOVE_TLS int alcove_kv_db = 0;
/* Per-type dispatch table (dump/load/destroy/print/clone). Built-ins fill ids
   1..EXP_MAXSIZE-1 in alcove_init; custom module types fill EXP_MAXSIZE.. via
   alcove_register_type. Sized to ALCOVE_TYPE_CAP so a 2-byte type id is always
   in range. */
exp_tfunc *exp_tfuncList[ALCOVE_TYPE_CAP];
/* Durable metadata for each custom type id (>= EXP_MAXSIZE): the registered
   (module-qualified) name and the module spec to re-(require) on db load.
   Indexed by type id. name==NULL means the slot is unused. */
static struct {
  char *name; /* module-qualified type name — the persistent identity */
  char
      *module_spec; /* what (require) loaded the module (for db auto-require) */
} g_custom_types[ALCOVE_TYPE_CAP];
static unsigned short g_next_type_id =
    0; /* lazily set to EXP_MAXSIZE on first use */
/* The module spec currently being loaded by load_native_module, so a type
   registered inside its alcove_module_init records where it came from. */
static const char *g_current_module_spec = NULL;
/* --safe: disable db-load auto-(require) of a missing custom type's module. */
static int g_safe_mode = 0;
/* --interpret: force every lambda body to run on the AST tree-walker (skip the
   bytecode compiler). Used to differential-test compiled-vs-interpreted on the
   same source (run twice, diff). Deep tail recursion has no TCO in this mode,
   so keep test depths bounded. */
static int g_no_compile = 0;
/* Load-scoped: dump-session custom type id → this process's id (0 =
   unresolved). Built by alcove_load_unified from the v3 type table; read by
   load_exp_t. Declared here (before load_exp_t) since load_exp_t precedes the
   dump code. */
static unsigned short g_type_remap[ALCOVE_TYPE_CAP];
/* The db.dump format version being loaded; alcove_load_unified sets it before
   any load fn runs (the initializer is just a safe default). Defined here so
   load_exp_t and persist.h both see it. Kept in sync with ALCOVE_DUMP_VERSION.
 */
static int alcove_load_dump_version = 3;

/* Canonical singletons — pointer set at main() startup. */
exp_t *nil_singleton = NULL;
exp_t *true_singleton = NULL;
exp_t *gen_done_singleton = NULL;
static exp_t *alc_cstr_to_key(const char *k);
static int set_insert_value(dict_t *d, exp_t *v);
static void alc_list_push_right(
    alc_list_t *l,
    exp_t *val); /* defined far below; used by load_deque_value */
static int
is_reserved_name(const char *name); /* defined below; used by updatebang/setq */

/* Reject assigning to a bare reserved-name symbol (= / setf / setq). Only a
   symbol LHS triggers it — place forms ((vec i), (s i), (car x)) and string
   keys pass through. On a hit, build the error into ERRLV and run FAIL
   (cleanup + return/propagate). `env` must be in scope at the use site. */
#define REJECT_RESERVED_ASSIGN(SYM, ERRLV, FAIL)                               \
  do {                                                                         \
    if (issymbol(SYM) && is_reserved_name(exp_text(SYM))) {                    \
      (ERRLV) = error(ERROR_ILLEGAL_VALUE, NULL, env,                          \
                      "cannot assign to reserved name '%s'", exp_text(SYM));   \
      FAIL;                                                                    \
    }                                                                          \
  } while (0)

/* Global env handle for the readline tab-completion callback (which
   takes no user-data param). Set in main() before the REPL loop. */
struct env_t *g_global_env = NULL;

/* Set by invoke's body loop / ifcmd's selected-branch eval. When true,
   evaluate returns a trampoline marker instead of recursing into
   invoke, giving us O(1) C stack for tail-recursive code. __thread:
   each evaluator stack belongs to its own thread; the tail flag is
   strictly call-site reentrant state. */
static ALCOVE_TLS int in_tail_position = 0;

/* ---- Call backtrace (for error stack traces) ------------------------------
   A lightweight dynamic call stack of function NAMES, pushed/popped at the two
   call dispatchers (invoke for the AST path, vm_invoke_values for the VM path).
   The env chain can't serve this — it's the LEXICAL parent (a top-level fn
   parents straight to global), not the dynamic caller. When an error is first
   created, error() snapshots the live stack into g_error_bt; it is rendered at
   top level if the error goes uncaught, and cleared when a handler catches it.
   Tail calls reuse a frame (no push), so the trace collapses tail recursion —
   standard for a TCO language. */
#define ALC_BT_MAX 128
static ALCOVE_TLS const char *g_callstack[ALC_BT_MAX];
static ALCOVE_TLS int g_calldepth = 0; /* may exceed ALC_BT_MAX; only the
                                          first ALC_BT_MAX names are kept */
static ALCOVE_TLS const char *g_error_bt[ALC_BT_MAX];
static ALCOVE_TLS int g_error_bt_n = 0; /* >0 once an error snapshot is live */
static ALCOVE_TLS int g_error_bt_more =
    0; /* frames beyond ALC_BT_MAX, elided */
static inline void bt_push(const char *name) {
  if (g_calldepth >= 0 && g_calldepth < ALC_BT_MAX)
    g_callstack[g_calldepth] = name ? name : "<anonymous>";
  g_calldepth++;
}
static inline void bt_pop(void) {
  if (g_calldepth > 0)
    g_calldepth--;
}
/* Snapshot the live call stack into g_error_bt (called by error() at the error
   site, where the depth is deepest). Only captures once per error episode — the
   first error() wins; cleared at top level / on catch so the next is fresh. */
static inline void bt_capture(void) {
  if (g_error_bt_n != 0)
    return;
  int n = g_calldepth < ALC_BT_MAX ? g_calldepth : ALC_BT_MAX;
  for (int i = 0; i < n; i++)
    g_error_bt[i] = g_callstack[i];
  g_error_bt_n = n;
  g_error_bt_more = g_calldepth - n;
  if (n == 0)
    g_error_bt_n = -1; /* sentinel: captured-but-empty (don't re-capture) */
}
static inline void bt_clear(void) {
  g_error_bt_n = 0;
  g_error_bt_more = 0;
}

/* ---- Debugger (gdb-style, runs on the AST tree-walker) --------------------
   Opt-in via --debug (which also forces the AST walker, like --interpret, so
   every call frame is a live, inspectable env_t) or the (break) builtin. All
   state is thread-local and every hook is gated on g_debug, so a non-debug run
   is byte-identical and pays at most one never-taken branch in evaluate().
   Source lines come from g_track_lines: when set, the reader stamps each list
   form's start line into the pair's otherwise-unused `meta` slot (lambda/macro
   heads, symbols and vectors use `meta`; a plain list-form pair never does) —
   read it back via form_line(). */
static ALCOVE_TLS int g_track_lines =
    1; /* reader stamps each list form's line+col
          into its pair meta (on by default — feeds
          precise error locations + the debugger) */
/* Precise source position of a raised error (raw reader line/col), 0 = unknown.
   The AST path fills it in error() from the offending form; the VM path fills
   it from the bytecode pc→loc table in RUNTIME_ERR. The top-level renderer
   prefers it over g_form_line (the enclosing top-level form only). Reset per
   form. */
static ALCOVE_TLS int g_err_line = 0;
static ALCOVE_TLS int g_err_col = 0;
static ALCOVE_TLS int g_debug = 0;    /* debugger active (hooks live) */
static ALCOVE_TLS int g_dbg_mode = 0; /* 0 run, 1 step-into, 2 next/step-over */
static ALCOVE_TLS int g_dbg_next_depth =
    0;                               /* for `next`: stop when depth <= this */
static ALCOVE_TLS int g_dbg_sel = 0; /* frame selected for locals / p */
static ALCOVE_TLS int g_dbg_active =
    0; /* inside the debug REPL: suppress the hook
          so `p <expr>` / locals don't re-stop */
static ALCOVE_TLS int g_try_depth =
    0; /* AST (try ...) body nesting: an error here
          is about to be caught — don't break on it */
static ALCOVE_TLS int g_dbg_evaluating =
    0; /* set only while a top-level user form is
          evaluating, so break-on-error never fires
          during parsing/loading */
static ALCOVE_TLS int g_dbg_color =
    0; /* colorize debug output (a tty stderr) */
static ALCOVE_TLS env_t *g_dbg_complete_env =
    NULL; /* frame env for tab-completion */
static ALCOVE_TLS int g_dbg_in_error_break =
    0; /* in the break-on-raise prompt: the
          `return <expr>` recovery is allowed */
static ALCOVE_TLS exp_t *g_dbg_replace =
    NULL; /* `return <expr>` value (owned) — error()
             hands it back in place of the error */
static ALCOVE_TLS int g_dbg_did_replace = 0;
static exp_t *dbg_error_break(exp_t *err,
                              env_t *env); /* returns err, or a replacement
                                              value if the user `return`ed */
static char *dbg_read_command(env_t *frame_env); /* readline (tty) or getline */
/* ANSI colors for the debug REPL, suppressed when stderr isn't a tty (so piped
   output — and the test harness — stays plain). dc() returns the code or "". */
#define DBGC_HDR "\x1B[1;96m" /* header (break / ready) */
#define DBGC_FN "\x1B[93m"    /* function name */
#define DBGC_NUM "\x1B[92m"   /* line numbers */
#define DBGC_FORM "\x1B[36m"  /* source form */
#define DBGC_VAR "\x1B[94m"   /* local variable name */
#define DBGC_SEL "\x1B[95m"   /* selected-frame marker */
#define DBGC_ERR "\x1B[91m"   /* error text */
#define DBGC_RST "\x1B[0m"
static inline const char *dc(const char *code) {
  return g_dbg_color ? code : "";
}
typedef struct {
  const char *name; /* function name (borrowed from fn->meta) */
  env_t *env;       /* most-local env seen executing in this frame */
  exp_t *form;      /* form last about to evaluate in this frame */
  int line;         /* its source line, or 0 */
} dbg_frame_t;
static ALCOVE_TLS dbg_frame_t g_dbg_frames[ALC_BT_MAX];
static ALCOVE_TLS int g_dbg_depth = 0; /* may exceed ALC_BT_MAX; array capped */
#define DBG_BP_MAX 32
static ALCOVE_TLS char *g_dbg_bp_fn[DBG_BP_MAX]; /* break-on-function names */
static ALCOVE_TLS int g_dbg_nbp_fn = 0;
static ALCOVE_TLS int g_dbg_bp_line[DBG_BP_MAX]; /* break-on-line numbers */
static ALCOVE_TLS int g_dbg_nbp_line = 0;
/* Source position stamped on a list form, packed into the pair's `meta` union
   (unused on plain list-form pairs): line in the low 20 bits, column in the
   next 12 — so it fits a 32-bit pointer slot (wasm32). 0 = unknown. The reader
   packs via FORM_LOC_PACK; form_line/form_col unpack. Lines past ~1M / cols
   past ~4095 clamp (cosmetic only — used for error display, never semantics).
 */
#define FORM_LOC_LINE_MAX 0xFFFFF /* 2^20 - 1 */
#define FORM_LOC_COL_MAX 0xFFF    /* 2^12 - 1 */
#define FORM_LOC_PACK(line, col)                                               \
  ((uintptr_t)(((uint32_t)((line) > FORM_LOC_LINE_MAX ? FORM_LOC_LINE_MAX      \
                                                      : (line))) |             \
               ((uint32_t)((col) > FORM_LOC_COL_MAX ? FORM_LOC_COL_MAX         \
                                                    : (col))                   \
                << 20)))
static inline int form_line(exp_t *e) {
  return (e && is_ptr(e) && e->type == EXP_PAIR)
             ? (int)((uintptr_t)e->meta & 0xFFFFF)
             : 0;
}
static inline int form_col(exp_t *e) {
  return (e && is_ptr(e) && e->type == EXP_PAIR)
             ? (int)(((uintptr_t)e->meta >> 20) & 0xFFF)
             : 0;
}

/* ---- RESP user-callback read-only guard -----------------------------------
   `alcove -r --threads N` (N>1) runs N reactor pthreads that serve RESP
   concurrently. User commands (redis-defcmd) run their lambda on a reactor
   thread against the SHARED global Lisp env. A callback that mutates a global
   binding — (def ...), (= global ...), (persist ...), registering more
   commands — would race on the shared env dict (non-atomic refcounts, dict
   rehash). Rather than serialize all callbacks (which would kill the
   multi-thread throughput that --threads exists for), we REFUSE the
   global-mutating operations with a clear error while a callback runs under
   multiple reactors. Keyspace-only callbacks (lfkv.c is lock-free + epoch
   safe) stay fully parallel.

   g_resp_cb_guard is per-reactor-thread (TLS): set while that thread is inside
   a user callback AND we are multi-reactor. g_resp_multi is set once at server
   start, true iff >1 reactor. In mono builds ALCOVE_TLS is empty and both are
   plain globals left at 0 — the guard is never set, so zero behavioural
   change.

   KNOWN LIMITS: the guard catches global def/=/persist/forget/unpersist and
   command (un)registration. It does NOT catch in-place mutation of a shared
   object the callback reaches (e.g. mutating a container stored in a captured
   closure var) nor reassignment of a captured closure var that lives outside
   the callback's own frame — those remain the caller's responsibility and are
   documented in doc_redis_defcmd. */
static ALCOVE_TLS int g_resp_cb_guard = 0;
static int g_resp_multi = 0;
/* Set (TLS) while executing a RESP client's command callback, on ANY reactor
   count — the security-sandbox signal: invoke_internal refuses FLAG_UNSAFE
   (host-escape) builtins while it is set, so code a network client triggers
   can't reach the OS/FS/FFI/code-loading. Distinct from g_resp_cb_guard (which
   stays multi-reactor-only for the global-write thread-safety refusal). */
static ALCOVE_TLS int g_in_client_cmd = 0;

/* ---- VM handler stack (try in tail position) ------------------------------
   `try` is value-based and not tail-aware, so the AST evaluator (trycmd) must
   regain control after the body to inspect the result / run the handler — which
   keeps a C frame alive per level and blows the C stack on try-per-level deep
   recursion. To keep TCO, a compiled `try` IN TAIL POSITION instead pushes its
   {handler-form, finally-form, env} onto this heap stack (OP_PUSH_HANDLER) and
   compiles its body in tail position, so the body's recursive call trampolines
   (OP_TAIL_SELF/OP_TAIL_CALL) with O(1) C stack. Handlers legitimately
   ACCUMULATE O(n) for a try-per-level recursion (each is live until its
   protected body completes) — that O(n) is inherent and now lives on the heap,
   not the C stack.

   Each vm_run records the stack depth at entry (handler_base) and owns only the
   handlers pushed above it; on its final OP_RET it unwinds back to that depth,
   running each popped try's finally. Innermost (top) handler catches first; a
   handler that re-raises is caught by the next-out. is_cont_escape tokens are
   NEVER caught — they pass through (finally still runs).

   Per-level env state: a try-per-level self-recursion uses OP_TAIL_SELF, which
   DESTRUCTIVELY reuses the one env (rebinding its inline slots in place each
   trampoline hop). So storing the env pointer would make every level's handler/
   finally see only the LAST (base-case) param values. Instead each entry
   SNAPSHOTS the env's inline slots (values + keys + count) at push time, plus a
   ref on the root chain (the root is stable — self-tail-call keeps the same
   env, top-level defs root at the global env). At eval time we rebuild a
   short-lived env from the snapshot. The O(n) snapshots are the inherent O(n)
   cost of a try-per-level recursion — now on the heap handler stack, not the C
   stack.

   __thread: each evaluator stack is per-thread, like in_tail_position. */
typedef struct {
  exp_t *handler_form; /* raw AST of the handler expr (NULL = no-catch try);
                          evaluated lazily on the error path, like trycmd */
  exp_t *finally_form; /* raw AST of the finally expr, or NULL */
  env_t *root;         /* OWNED ref on the snapshot env's parent chain */
  int n_inline;        /* snapshotted inline-slot count */
  /* Borrowed pointers into the param symbols' ->ptr. Safe because
     OP_PUSH_HANDLER only runs on a TAIL-position try, and a cross-function tail
     call uses the in-place env-rebind (which would free the old fn) ONLY for
     compiled targets that captured the GLOBAL env — i.e. top-level defs, whose
     param symbols are held alive by the permanent global binding for the
     program's lifetime. Local closures take the vm_invoke_values fallback
     (their own vm_run, own handler_base), so their snapshots are consumed
     before the closure can die. */
  char *inline_keys[ENV_INLINE_SLOTS];
  exp_t *inline_vals[ENV_INLINE_SLOTS]; /* OWNED refs (snapshot) */
} vm_handler_t;
static ALCOVE_TLS vm_handler_t *g_handlers = NULL;
static ALCOVE_TLS int g_handler_sp = 0;  /* live entries */
static ALCOVE_TLS int g_handler_cap = 0; /* allocated entries */

/* Push a handler entry, SNAPSHOTTING env's inline slots so the handler/finally
   see this try's bindings even after OP_TAIL_SELF mutates the env in place.
   forms are borrowed (const-pool, outlive the call). Returns 0 on OOM (no entry
   pushed, no refs taken). */
static int vm_handler_push(exp_t *handler_form, exp_t *finally_form,
                           env_t *env) {
  if (g_handler_sp >= g_handler_cap) {
    int ncap = g_handler_cap ? g_handler_cap * 2 : 16;
    vm_handler_t *nh = realloc(g_handlers, (size_t)ncap * sizeof(*nh));
    if (!nh)
      return 0;
    g_handlers = nh;
    g_handler_cap = ncap;
  }
  vm_handler_t *h = &g_handlers[g_handler_sp];
  h->handler_form = handler_form;
  h->finally_form = finally_form;
  h->root = ref_env(env->root); /* root is stable across the trampoline */
  int n = env->n_inline;
  if (n > ENV_INLINE_SLOTS)
    n = ENV_INLINE_SLOTS; /* d-overflow bindings resolve via root/dict */
  h->n_inline = n;
  for (int i = 0; i < n; i++) {
    h->inline_keys[i] = env->inline_keys[i];
    h->inline_vals[i] = refexp(env->inline_vals[i]);
  }
  g_handler_sp++;
  return 1;
}

/* Build a short-lived env from a handler entry's snapshot. Caller must
   destroy_env it. Kept separate so handler-eval-and-apply can happen within one
   temp-env scope (a handler/finally closure captures this env; it must outlive
   the apply, then be torn down LIFO-correctly). */
static env_t *vm_handler_make_env(const vm_handler_t *h) {
  env_t *e = make_env(h->root); /* takes its own ref on root */
  e->n_inline = h->n_inline;
  for (int i = 0; i < h->n_inline; i++) {
    e->inline_keys[i] = h->inline_keys[i];
    e->inline_vals[i] = refexp(h->inline_vals[i]);
  }
  return e;
}

/* Builtin registration. `arity` and `level` are reserved fields (the
   first for future per-call arity checks, the second for sandbox tiers);
   nothing reads them today. `doc` is a one-line help string colocated
   with each cmd's definition (search for `static const char doc_<name>[]`
   near the corresponding cmd function). Use LISPCMD_TAIL for control-
   flow forms (if, do) that need FLAG_TAIL_AWARE so evaluate() exposes
   in_tail_position to them. */
#define LISPCMD(name, fn, doc) {name, -1, 0, doc, fn}
#define LISPCMD_TAIL(name, fn, doc) {name, -1, FLAG_TAIL_AWARE, doc, fn}
/* *_UNSAFE: the builtin touches the host OS / filesystem / FFI / persistence /
   code-loading, so it is refused under --safe (g_safe_mode) at the single
   invoke_internal() gate. UNSAFE = non-applicative (special-form-shaped, e.g.
   shell/load/require); APP_UNSAFE = applicative (e.g. delete-file/setenv). */
#define LISPCMD_UNSAFE(name, fn, doc) {name, -1, FLAG_UNSAFE, doc, fn}
#define LISPCMD_APP_UNSAFE(name, fn, doc)                                      \
  {name, -1, FLAG_APPLICATIVE | FLAG_UNSAFE, doc, fn}
/* LISPCMD_APP: an APPLICATIVE builtin — evaluates ALL of its arguments in
   applicative order, returns a value, and does NOT inspect its unevaluated
   arg forms or depend on in_tail_position. The compiler emits a real
   OP_CALL_GLOBAL for these (callee via gcache, args from slots/gcache)
   instead of the OP_EVAL_AST tree-walk, which re-resolves every name by
   string each call. Use ONLY for builtins that meet that contract — special
   forms (quote/if/for/=/let/…), macros, and anything that reads its raw form
   must stay LISPCMD/LISPCMD_TAIL. alc_apply_n quote-protects symbol/list arg
   VALUES, so storing/forwarding arbitrary values stays correct. */
#define LISPCMD_APP(name, fn, doc) {name, -1, FLAG_APPLICATIVE, doc, fn}

#include "builtins.h"

lispProc lispProcList[] = {
    /* Special forms / control flow */
    LISPCMD("quote", quotecmd, doc_quote),
    LISPCMD("quasiquote", quasiquotecmd, doc_quasiquote),
    LISPCMD_TAIL("if", ifcmd, doc_if),
    LISPCMD_TAIL("do", docmd, doc_do),
    LISPCMD_TAIL("when", whencmd, doc_when),
    LISPCMD_TAIL("unless", unlesscmd, doc_unless),
    LISPCMD("while", whilecmd, doc_while),
    LISPCMD("repeat", repeatcmd, doc_repeat),
    LISPCMD("with-time-limit", withtimelimitcmd, doc_with_time_limit),
    LISPCMD("with-memory-limit", withmemlimitcmd, doc_with_memory_limit),
    LISPCMD("heap-stats", heapstatscmd, doc_heap_stats),
    LISPCMD("gc-cycles", gccyclescmd, doc_gc_cycles),
    /* observability (builtins_log.h) */
    LISPCMD("error-code", errorcodecmd, doc_error_code),
    LISPCMD("log!", logemitcmd, doc_log_emit),
    LISPCMD("log-debug", logdebugcmd, doc_log_debug),
    LISPCMD("log-info", loginfocmd, doc_log_info),
    LISPCMD("log-warn", logwarncmd, doc_log_warn),
    LISPCMD("log-error", logerrorcmd, doc_log_error),
    LISPCMD("log-level", loglevelcmd, doc_log_level),
    LISPCMD("set-log-level", setloglevelcmd, doc_set_log_level),
#ifdef ALCOVE_METRICS /* metrics opt-in: make alcove-with-metrics */
    LISPCMD("counter!", counterbangcmd, doc_counter_bang),
    LISPCMD("gauge!", gaugebangcmd, doc_gauge_bang),
    LISPCMD("metric", metriccmd, doc_metric),
    LISPCMD("metrics", metricscmd, doc_metrics),
#endif
    LISPCMD_UNSAFE("alloc-fail-after", allocfailaftercmd, doc_alloc_fail_after),
    LISPCMD_TAIL("and", andcmd, doc_and),
    LISPCMD_TAIL("or", orcmd, doc_or),
    LISPCMD_TAIL("case", casecmd, doc_case),
    LISPCMD_TAIL("cond", condcmd, doc_cond),
    LISPCMD_TAIL("match", matchcmd, doc_match),
    LISPCMD("for", forcmd, doc_for),
    LISPCMD("each", eachcmd, doc_each),
    LISPCMD_TAIL("for-gen", forgencmd, doc_forgen),
    LISPCMD_TAIL("for-each!", forgencmd, doc_forgen), /* canonical !-suffix */
    LISPCMD_TAIL("let", letcmd, doc_let),
    LISPCMD_TAIL("let*", letstar_cmd, doc_letstar),
    LISPCMD_TAIL("with", withcmd, doc_with),
    /* Generators — gen-* names kept for compat; !-suffix are the preferred
       forms. Convention: ! = operates in the stateful/mutable iterator domain;
                   ? = predicate; no suffix = pure eager list operations. */
    LISPCMD("*gen-done*", gendone_cmd, doc_gendone),
    LISPCMD("*done*", gendone_cmd, doc_gendone), /* preferred short name */
    LISPCMD("gen-done?", gendonep_cmd, doc_gendonep),
    LISPCMD("done?", gendonep_cmd, doc_gendonep), /* preferred */
    LISPCMD("gen-list", genlist_cmd, doc_genlist),
    LISPCMD("iter!", genlist_cmd, doc_genlist), /* preferred */
    LISPCMD("gen-range", genrange_cmd, doc_genrange),
    LISPCMD("range!", genrange_cmd, doc_genrange), /* preferred */
    LISPCMD("gen-next!", gennext_cmd, doc_gennext),
    LISPCMD("next!", gennext_cmd, doc_gennext), /* preferred */
    LISPCMD("gen-collect", gencollect_cmd, doc_gencollect),
    LISPCMD("collect!", gencollect_cmd, doc_gencollect), /* preferred */
    LISPCMD("gen-map", genmap_cmd, doc_genmap),
    LISPCMD("map!", genmap_cmd, doc_genmap), /* preferred */
    LISPCMD("gen-filter", genfilter_cmd, doc_genfilter),
    LISPCMD("filter!", genfilter_cmd, doc_genfilter), /* preferred */
    /* Comparison / equality */
    LISPCMD("=", equalcmd, doc_eq),
    LISPCMD("setf", equalcmd,
            doc_setf), /* exact synonym of = (readable head) */
    LISPCMD("<", cmpcmd, doc_lt),
    LISPCMD(">", cmpcmd, doc_gt),
    LISPCMD("<=", cmpcmd, doc_le),
    LISPCMD(">=", cmpcmd, doc_ge),
    LISPCMD("is", iscmd, doc_is),           /* has inline OP handling */
    LISPCMD_APP("eq", iscmd, doc_is),       /* alias of is (no inline path) */
    LISPCMD_APP("eq?", iscmd, doc_is),      /* alias of is (no inline path) */
    LISPCMD_APP("isnt", isntcmd, doc_isnt), /* complement of is */
    LISPCMD("iso", isocmd, doc_iso),        /* has inline OP handling */
    LISPCMD_APP("in", incmd, doc_in),
    LISPCMD("no", nocmd, doc_no),         /* has inline OP handling */
    LISPCMD_APP("not", nocmd, doc_no),    /* alias of no (no inline path) */
    LISPCMD_APP("yes", yespcmd, doc_yes), /* complement of no */
    /* Arithmetic */
    LISPCMD("+", pluscmd, doc_plus),
    LISPCMD("*", multiplycmd, doc_mul),
    LISPCMD("-", minuscmd, doc_minus),
    LISPCMD("/", dividecmd, doc_div),
    LISPCMD_APP("rational", rationalcmd, doc_rational),
    LISPCMD_APP("numerator", numeratorcmd, doc_numerator),
    LISPCMD_APP("denominator", denominatorcmd, doc_denominator),
    LISPCMD_APP("rational?", rationalpcmd, doc_rationalp),
    LISPCMD_APP("decimal", decimalcmd, doc_decimal),
    LISPCMD_APP("decimal?", decimalpcmd, doc_decimalp),
    LISPCMD("mod", modcmd, doc_mod),
    LISPCMD("abs", abscmd, doc_abs),
    LISPCMD("max", maxcmd, doc_max),
    LISPCMD("min", mincmd, doc_min),
    LISPCMD_APP("odd", oddcmd, doc_odd),
    LISPCMD_APP("sqrt", sqrtcmd, doc_sqrt),
    LISPCMD("sqrt-int", sqrtintcmd, doc_sqrtint), /* has inline OP_SQRT_INT */
    LISPCMD_APP("exp", expcmd, doc_exp),
    LISPCMD_APP("expt", exptcmd, doc_expt),
    LISPCMD_APP("**", exptcmd, doc_expt), /* Python-ish alias */
    LISPCMD_APP("random", randomcmd, doc_random),
    LISPCMD_APP("round", roundcmd, doc_round),
    LISPCMD_APP("floor", floorcmd, doc_floor),
    LISPCMD_APP("ceil", ceilcmd, doc_ceil),
    LISPCMD_APP("truncate", truncatecmd, doc_truncate),
    LISPCMD_APP("log", logcmd, doc_log),
    LISPCMD_APP("sin", sincmd, doc_sin),
    LISPCMD_APP("cos", coscmd, doc_cos),
    LISPCMD_APP("tan", tancmd, doc_tan),
    LISPCMD_APP("float", floatcmd, doc_float),
    LISPCMD_APP("int", intcmd, doc_int),
    /* Bitwise — int-only. C-style spelling + Lisp-style aliases. */
    LISPCMD_APP("bit-and", bitandcmd, doc_bitand),
    LISPCMD_APP("band", bitandcmd, doc_bitand),
    LISPCMD_APP("&", bitandcmd, doc_bitand),
    LISPCMD_APP("bit-or", bitorcmd, doc_bitor),
    LISPCMD_APP("|", bitorcmd, doc_bitor),
    LISPCMD_APP("bit-xor", bitxorcmd, doc_bitxor),
    LISPCMD_APP("^", bitxorcmd, doc_bitxor),
    LISPCMD_APP("bit-not", bitnotcmd, doc_bitnot),
    LISPCMD_APP("~", bitnotcmd, doc_bitnot),
    LISPCMD_APP("<<", shlcmd, doc_shl),
    LISPCMD_APP(">>", shrcmd, doc_shr),
    /* Pairs and lists */
    LISPCMD("cons", conscmd, doc_cons),
    LISPCMD("car", carcmd, doc_car),
    LISPCMD("cdr", cdrcmd, doc_cdr),
    LISPCMD("list", listcmd, doc_list),
    LISPCMD("length", lengthcmd, doc_length),
    LISPCMD_APP("nth", nthcmd, doc_nth),
    LISPCMD_APP("seq", seqcmd, doc_seq),
    LISPCMD_APP("first", firstcmd, doc_first),
    LISPCMD_APP("rest", restcmd, doc_rest),
    LISPCMD_APP("conj", conjcmd, doc_conj),
    LISPCMD_APP("into", intocmd, doc_into),
    LISPCMD_APP("type-of", typeofcmd, doc_typeof),
    LISPCMD("defstruct", defstructcmd, doc_defstruct),
    LISPCMD("defmulti", defmulticmd, doc_defmulti),
    LISPCMD("defmethod", defmethodcmd, doc_defmethod),
    LISPCMD_APP("reverse", reversecmd, doc_reverse),
    LISPCMD("append", appendcmd, doc_append), /* has inline OP handling */
    /* Vectors — O(1) random-access array */
    LISPCMD_APP("vec", veccmd, doc_vec),
    LISPCMD_APP("vec-ref", vecrefcmd, doc_vecref),
    LISPCMD_APP("vec-set!", vecsetcmd, doc_vecset),
    LISPCMD_APP("vec-len", veclencmd, doc_veclen),
    /* Tensor bulk ops — read each element as a double, do the math in
       raw C, write fresh EXP_FLOATs back. ~100x faster than the
       interpreted equivalent for MLP-style inner loops. Applicative
       (LISPCMD_APP): compile to OP_CALL_GLOBAL so a hot for-loop calling
       them is not a per-call OP_EVAL_AST tree-walk (the MLP's dominant
       cost — see benchmark/mlp.alc). */
    LISPCMD_APP("vec-dot", vecdotcmd, doc_vecdot),
    LISPCMD_APP("vec-axpy!", vecaxpycmd, doc_vecaxpy),
    LISPCMD_APP("vec-scale!", vecscalecmd, doc_vecscale),
    LISPCMD_APP("vec-add!", vecaddcmd, doc_vecadd),
    LISPCMD_APP("vec-count-le!", veccountlecmd, doc_veccountle),
    LISPCMD_APP("vec-copy!", veccopycmd, doc_veccopy),
    LISPCMD_APP("vec-fill!", vecfillcmd, doc_vecfill),
    LISPCMD_APP("vec-relu!", vecrelucmd, doc_vecrelu),
    LISPCMD_APP("vec-argmax", vecargmaxcmd, doc_vecargmax),
    LISPCMD_APP("vec-max", vecmaxcmd, doc_vecmax),
    LISPCMD_APP("vec-mul!", vecmulcmd, doc_vecmul),
    LISPCMD_APP("vec-sub!", vecsubcmd, doc_vecsub),
    LISPCMD_APP("vec-sum", vecsumcmd, doc_vecsum),
    LISPCMD_APP("vec-min", vecmincmd, doc_vecmin),
    LISPCMD_APP("vec-argmin", vecargmincmd, doc_vecargmin),
    LISPCMD_APP("vec-exp!", vecexpcmd, doc_vecexp),
    LISPCMD_APP("vec-sigmoid!", vecsigmoidcmd, doc_vecsigmoid),
    LISPCMD_APP("vec-tanh!", vectanhcmd, doc_vectanh),
    LISPCMD_APP("vec-softmax!", vecsoftmaxcmd, doc_vecsoftmax),
    LISPCMD_APP("mat-vec", matveccmd, doc_matvec),
    LISPCMD_APP("mat-vec!", matvecbangcmd, doc_matvecbang),
    LISPCMD_APP("mat-vec-t!", matvectbangcmd, doc_matvectbang),
    LISPCMD_APP("vec-ger!", vecgercmd, doc_vecger),
    LISPCMD_APP("vec-from-blob!", vecfromblobcmd, doc_vecfromblob),
    LISPCMD_APP("mat-mul", matmulcmd, doc_matmul),
    /* Deque ops on vec — amortised O(1) push/pop at both ends via the
       cap/start/end window. Growth: 1.5x on realloc; slide-left when
       start >= cap/4 instead of reallocating; recenter on unshift-grow. */
    LISPCMD_APP("vec-push!", vecpushcmd, doc_vecpush),
    LISPCMD_APP("vec-pop!", vecpopcmd, doc_vecpop),
    LISPCMD_APP("vec-unshift!", vecunshiftcmd, doc_vecunshift),
    LISPCMD_APP("vec-shift!", vecshiftcmd, doc_vecshift),
    /* Functions and binding */
    LISPCMD("def", defcmd, doc_def),
    LISPCMD("defn", defncmd, doc_defn),
    LISPCMD("fn", fncmd, doc_fn),
    LISPCMD("defc", defccmd, doc_defc),
    LISPCMD("defmacro", defmacrocmd, doc_defmacro),
    LISPCMD("macroexpand-1", expandmacrocmd, doc_macroexpand),
    LISPCMD_UNSAFE("eval", evalcmd, doc_eval),
    LISPCMD("apply", applycmd, doc_apply),
    LISPCMD("setq", setqcmd, doc_setq),
    /* Higher-order */
    LISPCMD("map", mapcmd, doc_map),
    LISPCMD("filter", filtercmd, doc_filter),
    LISPCMD("reduce", reducecmd, doc_reduce),
    LISPCMD("any?", anypcmd, doc_any),
    LISPCMD("all?", allpcmd, doc_all),
    LISPCMD("sort", sortcmd, doc_sort),
    LISPCMD("sort-by", sortbycmd, doc_sortby),
    /* List utilities */
    LISPCMD("take", takecmd, doc_take),
    LISPCMD("drop", dropcmd, doc_drop),
    LISPCMD("range", rangecmd, doc_range),
    LISPCMD("zip", zipcmd, doc_zip),
    LISPCMD("flatten", flattencmd, doc_flatten),
    LISPCMD("gensym", gensymcmd, doc_gensym),
    LISPCMD("with-gensyms", withgensymscmd, doc_withgensyms),
    /* Error handling */
    LISPCMD("error?", errorpcmd, doc_errorp),
    LISPCMD("error-message", errormessagecmd, doc_errormessage),
    LISPCMD("try", trycmd, doc_try),
    LISPCMD("call/cc", callcccmd, doc_callcc),
    /* Predicates */
    LISPCMD_APP("number?", numberpcmd, doc_numberp),
    LISPCMD_APP("zero?", zeropcmd, doc_zerop),
    LISPCMD_APP("char?", charpcmd, doc_charp),
    LISPCMD_APP("string?", stringpcmd, doc_stringp),
    LISPCMD_APP("symbol?", symbolpcmd, doc_symbolp),
    LISPCMD_APP("pair?", pairpcmd, doc_pairp),
    LISPCMD_APP("list?", listpcmd, doc_listp),
    LISPCMD_APP("null?", nullpcmd, doc_nullp),
    LISPCMD_APP("nil?", nullpcmd, doc_nullp), /* alias of null? */
    LISPCMD_APP("fn?", fnpcmd, doc_fnp),
    LISPCMD_APP("vec?", vecpcmd, doc_vecp),
    LISPCMD_APP("blob?", blobpcmd, doc_blobp),
    LISPCMD_APP("dict?", dictpcmd, doc_dictp),
    LISPCMD_APP("deque?", dequepcmd, doc_dequep),
    LISPCMD_APP("set?", setpcmd, doc_setp),
    /* Introspection (return testable values, not printed) */
    LISPCMD_APP("compiled?", compiledpcmd, doc_compiledp),
    LISPCMD_APP("jit?", jitpcmd, doc_jitp),
    LISPCMD_APP("inline?", inlinepcmd, doc_inlinep),
    LISPCMD_APP("exp-flags", expflagscmd, doc_expflags),
    LISPCMD("backtrace", backtracecmd, doc_backtrace),
    LISPCMD_UNSAFE("break", breakcmd, doc_break),
    LISPCMD_UNSAFE("allow-unsafe", allowunsafecmd, doc_allow_unsafe),
    /* I/O */
    LISPCMD("pr", prcmd, doc_pr),
    LISPCMD("print", prcmd, doc_pr),
    LISPCMD("prn", prncmd, doc_prn),
    LISPCMD("println", prncmd, doc_prn),
    LISPCMD("epr", eprcmd, doc_epr),
    LISPCMD("eprn", eprncmd, doc_eprn),
    LISPCMD_UNSAFE("read-line", readlinecmd, doc_readline),
    LISPCMD_APP_UNSAFE("getenv", getenvcmd, doc_getenv),
    LISPCMD_APP_UNSAFE("setenv", setenvcmd, doc_setenv),
    LISPCMD_APP_UNSAFE("delete-file", deletefilecmd, doc_deletefile),
    LISPCMD_APP_UNSAFE("rename-file", renamefilecmd, doc_renamefile),
    LISPCMD_APP_UNSAFE("make-dir", makedircmd, doc_makedir),
    LISPCMD_APP_UNSAFE("list-dir", listdircmd, doc_listdir),
    LISPCMD_APP_UNSAFE("file-info", fileinfocmd, doc_fileinfo),
    LISPCMD_UNSAFE("shell", shellcmd, doc_shell),
    LISPCMD_APP("json-encode", jsonencodecmd, doc_jsonencode),
    LISPCMD_APP("json-decode", jsondecodecmd, doc_jsondecode),
    LISPCMD_APP("base64-encode", base64encodecmd, doc_base64encode),
    LISPCMD_APP("base64-decode", base64decodecmd, doc_base64decode),
    LISPCMD_APP("hex-encode", hexencodecmd, doc_hexencode),
    LISPCMD_APP("hex-decode", hexdecodecmd, doc_hexdecode),
    LISPCMD("group-by", groupbycmd, doc_groupby),
    LISPCMD_APP("frequencies", frequenciescmd, doc_frequencies),
    LISPCMD_APP("partition", partitioncmd, doc_partition),
    LISPCMD_APP("interleave", interleavecmd, doc_interleave),
    LISPCMD("max-by", maxbycmd, doc_maxby),
    LISPCMD("min-by", minbycmd, doc_minby),
    LISPCMD_APP("starts-with?", startswithcmd, doc_startswith),
    LISPCMD_APP("ends-with?", endswithcmd, doc_endswith),
    LISPCMD_APP("string-repeat", stringrepeatcmd, doc_stringrepeat),
    LISPCMD_APP("string-pad-left", stringpadleftcmd, doc_stringpadleft),
    LISPCMD_APP("string-pad-right", stringpadrightcmd, doc_stringpadright),
    LISPCMD_APP("re-match", rematchcmd, doc_rematch),
    LISPCMD_APP("re-find", refindcmd, doc_refind),
    LISPCMD_APP("re-find-all", refindallcmd, doc_refindall),
    LISPCMD_APP("re-replace", rereplacecmd, doc_rereplace),
    LISPCMD_APP("re-split", resplitcmd, doc_resplit),
    /* Strings and whole-file I/O */
    LISPCMD_APP("str", strcmd, doc_str),
    LISPCMD_APP("format", strcmd, doc_str),
    LISPCMD_APP("fmt", fmtcmd, doc_fmt),
    LISPCMD_APP("string-buf", stringbufcmd, doc_stringbuf),
    LISPCMD_APP("string-set!", stringsetcmd, doc_stringset),
    LISPCMD_APP("string-fill!", stringfillcmd, doc_stringfill),
    LISPCMD_APP("string-copy!", stringcopycmd, doc_stringcopy),
    LISPCMD_APP("substr", substrcmd, doc_substr),
    LISPCMD_APP("string-append", stringappendcmd, doc_stringappend),
    LISPCMD_APP("string-concat", stringappendcmd, doc_stringappend),
    LISPCMD_APP("string-split", stringsplitcmd, doc_stringsplit),
    LISPCMD_APP("string-join", stringjoincmd, doc_stringjoin),
    LISPCMD_APP("string-trim", stringtrimcmd, doc_stringtrim),
    LISPCMD_APP("string-upcase", stringupcasecmd, doc_stringupcase),
    LISPCMD_APP("string-downcase", stringdowncasecmd, doc_stringdowncase),
    LISPCMD_APP("string-contains?", stringcontainspcmd, doc_stringcontainsp),
    LISPCMD_APP("string-index", stringindexcmd, doc_stringindex),
    LISPCMD_APP("string-replace", stringreplacecmd, doc_stringreplace),
    LISPCMD_APP_UNSAFE("read-string", readstringcmd, doc_readstring),
    LISPCMD_APP_UNSAFE("open", opencmd, doc_open),
    LISPCMD_APP_UNSAFE("close", closecmd, doc_close),
    LISPCMD_APP_UNSAFE("write", writecmd, doc_write),
    LISPCMD_APP_UNSAFE("eof?", eofpcmd, doc_eofp),
    LISPCMD_APP_UNSAFE("port?", portpcmd, doc_portp),
    LISPCMD_APP_UNSAFE("write-string", writestringcmd, doc_writestring),
    LISPCMD_APP_UNSAFE("append-string", appendstringcmd, doc_appendstring),
    LISPCMD_APP_UNSAFE("read-lines", readlinescmd, doc_readlines),
    LISPCMD_APP_UNSAFE("file-exists?", fileexistspcmd, doc_fileexistsp),
    LISPCMD_APP_UNSAFE("write-bytes", writebytescmd, doc_writebytes),
    LISPCMD_UNSAFE("load", loadcmd, doc_load),
    LISPCMD_UNSAFE("require", requirecmd, doc_require),
    LISPCMD("ns", nscmd, doc_ns),
    /* Persistence */
    LISPCMD("persist", persistcmd, doc_persist),
    LISPCMD("forget", forgetcmd, doc_forget),
    LISPCMD("unpersist", unpersistcmd, doc_unpersist),
    LISPCMD_UNSAFE("savedb", savedbcmd, doc_savedb),
    LISPCMD_UNSAFE("loaddb", loaddbcmd, doc_loaddb),
    LISPCMD("ispersistent", ispersistentcmd, doc_ispersistent),
    /* Introspection / utilities */
    LISPCMD("inspect", inspectcmd, doc_inspect),
    LISPCMD("disasm", disasmcmd, doc_disasm),
    LISPCMD_UNSAFE("source", sourcecmd, doc_source),
    LISPCMD("dir", dircmd, doc_dir),
    LISPCMD("time", timecmd, doc_time),
    LISPCMD("web?", webpcmd, doc_webp),
    LISPCMD("platform", platformcmd, doc_platform),
    LISPCMD("dialect", dialectcmd, doc_dialect),
    /* Programmable REPL: bind a key to a handler, and edit the live input line
       from inside that handler. */
    LISPCMD("bind-key", bindkeycmd, doc_bindkey),
    LISPCMD("repl-line", repllinecmd, doc_replline),
    LISPCMD("repl-point", replpointcmd, doc_replpoint),
    LISPCMD("repl-end", replendcmd, doc_replend),
    LISPCMD("repl-goto", replgotocmd, doc_replgoto),
    LISPCMD("repl-insert", replinsertcmd, doc_replinsert),
    LISPCMD("repl-delete", repldeletecmd, doc_repldelete),
    LISPCMD("repl-replace-line", replreplacelinecmd, doc_replreplaceline),
    LISPCMD("repl-refresh", replrefreshcmd, doc_replrefresh),
    LISPCMD("repl-completions", replcompletionscmd, doc_replcompletions),
    LISPCMD("arch", archcmd, doc_arch),
    LISPCMD_UNSAFE("dylib-suffix", dylibsuffixcmd, doc_dylibsuffix),
    LISPCMD("now-ms", nowmscmd, doc_nowms),
    LISPCMD_UNSAFE("sleep-ms", sleepmscmd, doc_sleepms),
    LISPCMD_UNSAFE("exit", exitcmd, doc_exit),
    LISPCMD_UNSAFE("quit", exitcmd, doc_exit),
    /* Help / discovery */
    LISPCMD("doc", doccmd, doc_doc),
    LISPCMD("docstring", docstringcmd, doc_docstring),
    LISPCMD("help", helpcmd, doc_help),
    LISPCMD_APP("builtins", builtinscmd, doc_builtins),
    LISPCMD_APP_UNSAFE("globals", globalscmd, doc_globals),
    LISPCMD_APP("check-syntax", checksyntaxcmd, doc_checksyntax),
    LISPCMD_APP("read-string-sexpr", readstringsexprcmd, doc_read_string_sexpr),
    LISPCMD_APP("read-all-string", readallstringcmd, doc_read_all_string),
    LISPCMD_APP("adder->sexpr", addertosexprcmd, doc_adder_to_sexpr),
    LISPCMD_UNSAFE("read-stdin", readstdincmd, doc_readstdin),
    LISPCMD("flush", flushcmd, doc_flush),
    /* File I/O & Filesystem */
    LISPCMD("dir-exists?", direxistspcmd, doc_direxistsp),
    LISPCMD("path-join", pathjoincmd, doc_pathjoin),
    LISPCMD("path-dirname", pathdirnamecmd, doc_pathdirname),
    LISPCMD("path-basename", pathbasenamecmd, doc_pathbasename),
    /* Networking */
    LISPCMD_UNSAFE("resolve-host", resolvehostcmd, doc_resolvehost),
    LISPCMD_UNSAFE("tcp-connect", tcpconnectcmd, doc_tcpconnect),
    LISPCMD_UNSAFE("tcp-send", tcpsendcmd, doc_tcpsend),
    LISPCMD_UNSAFE("tcp-recv", tcprecvcmd, doc_tcprecv),
    LISPCMD_UNSAFE("tcp-close", tcpclosecmd, doc_tcpclose),
    /* Date & Time */
    LISPCMD("format-time", formattimecmd, doc_formattime),
    LISPCMD("parse-time", parsetimecmd, doc_parsetime),
    /* Sequence / Itertools */
    LISPCMD("take-while", takewhilecmd, doc_takewhile),
    LISPCMD("drop-while", dropwhilecmd, doc_dropwhile),
    /* FFI */
    LISPCMD("ffi?", ffipcmd, doc_ffip),
    LISPCMD_UNSAFE("ffi-fn", ffifncmd, doc_ffifn),
    LISPCMD_UNSAFE("ffi-vfn", ffivfncmd, doc_ffivfn),
    LISPCMD_UNSAFE("ffi-callback", fficallbackcmd, doc_fficallback),
    LISPCMD_UNSAFE("ffi-struct", ffistructcmd, doc_ffistruct),
    LISPCMD_UNSAFE("ffi-pack", ffipackcmd, doc_ffipack),
    LISPCMD_UNSAFE("ffi-unpack", ffiunpackcmd, doc_ffiunpack),
    /* Clojure-style hash-maps (EXP_DICT) */
    LISPCMD("hash-map", hashmapcmd, doc_hashmap),
    LISPCMD("assoc!", assocbangcmd, doc_assocbang),
    LISPCMD("dissoc!", dissocbangcmd, doc_dissocbang),
    LISPCMD("get", getcmd, doc_get),
    LISPCMD("contains?", containspcmd, doc_containsp),
    LISPCMD("keys", keyscmd, doc_keys),
    LISPCMD("vals", valscmd, doc_vals),
    LISPCMD("count", countcmd, doc_count),
    /* Doubly-linked deques (EXP_LIST) — Redis-list shaped */
    LISPCMD("deque", dequecmd, doc_deque),
    LISPCMD("push-right!", pushrightbangcmd, doc_pushrightbang),
    LISPCMD("push-left!", pushleftbangcmd, doc_pushleftbang),
    LISPCMD("pop-right!", poprightbangcmd, doc_poprightbang),
    LISPCMD("pop-left!", popleftbangcmd, doc_popleftbang),
    LISPCMD("peek-left", peekleftcmd, doc_peekleft),
    LISPCMD("peek-right", peekrightcmd, doc_peekright),
    /* Hash sets */
    LISPCMD("set", setcmd, doc_set),
    LISPCMD("hash-set", hashsetcmd, doc_hashset),
    LISPCMD("set-add!", setaddbangcmd, doc_setaddbang),
    LISPCMD("set-del!", setdelbangcmd, doc_setdelbang),
    LISPCMD("set-has?", sethaspcmd, doc_sethasp),
    LISPCMD("set-union", setunioncmd, doc_setunion),
    LISPCMD("set-intersection", setintersectioncmd, doc_setintersection),
    LISPCMD("set-difference", setdifferencecmd, doc_setdifference),
    LISPCMD("set->list", setlistcmd, doc_setlist),
    /* Persistent/immutable map (EXP_HAMT) */
    LISPCMD("hamt", hamtcmd, doc_hamt),
    LISPCMD("hamt-assoc", hamtassoccmd, doc_hamtassoc),
    LISPCMD("hamt-get", hamtgetcmd, doc_hamtget),
    LISPCMD("hamt-dissoc", hamtdissoccmd, doc_hamtdissoc),
    LISPCMD("hamt-count", hamtcountcmd, doc_hamtcount),
    LISPCMD("hamt-contains?", hamtcontainspcmd, doc_hamtcontainsp),
    LISPCMD("hamt-keys", hamtkeyscmd, doc_hamtkeys),
    LISPCMD("hamt-vals", hamtvalscmd, doc_hamtvals),
    LISPCMD("hamt->list", hamtlistcmd, doc_hamtlist),
    LISPCMD("hamt-merge", hamtmergecmd, doc_hamtmerge),
    LISPCMD("hamt?", hamtpcmd, doc_hamtp),
    /* MessagePack codec */
    LISPCMD_APP("msgpack-encode", msgpackencodecmd, doc_msgpackencode),
    LISPCMD_APP("msgpack-decode", msgpackdecodecmd, doc_msgpackdecode),
    /* Binary-safe blobs (EXP_BLOB) */
    LISPCMD_APP("make-blob", makeblobcmd, doc_makeblob),
    LISPCMD_APP("blob-len", bloblencmd, doc_bloblen),
    LISPCMD_APP("blob-ref", blobrefcmd, doc_blobref),
    LISPCMD_APP("blob->string", blob2stringcmd, doc_blob2string),
    LISPCMD_APP("string->blob", string2blobcmd, doc_string2blob),
    LISPCMD_APP_UNSAFE("read-bytes", readbytescmd, doc_readbytes),
    /* Clojure-style varargs vector ctor — populates EXP_VECTOR. Same as #[...].
     */
    LISPCMD("vector", vectorcmd, doc_vector),
#ifndef ALCOVE_WEB
    /* Redis keyspace bridge. Under -R these inspect/mutate the live RESP
       server; in normal Lisp runs they use the same in-process exp_t-backed
       keyspace without opening a socket. Excluded from the web build since
       resp.c (pthread/epoll/socket) doesn't compile under emscripten. */
    LISPCMD("redis-count", rediscountcmd, doc_redis_count),
    LISPCMD("redis-keys", rediskeyscmd, doc_redis_keys),
    LISPCMD("redis-type", redistypecmd, doc_redis_type),
    LISPCMD("redis-get", redisgetcmd, doc_redis_get),
    LISPCMD("redis-val", redisvalcmd, doc_redis_val),
    LISPCMD("redis-set", redissetcmd, doc_redis_set),
    LISPCMD("redis-del", redisdelcmd, doc_redis_del),
    LISPCMD("with-db", withdbcmd, doc_withdb),
    LISPCMD("redis-flush", redisflushcmd, doc_redis_flush),
    LISPCMD_UNSAFE("redis-port", redisportcmd, doc_redis_port),
    LISPCMD_UNSAFE("redis-defcmd", rediscmddefcmd, doc_redis_defcmd),
    LISPCMD_UNSAFE("redis-undefcmd", rediscmdundefcmd, doc_redis_undefcmd),
    LISPCMD_UNSAFE("redis-cmds", rediscmdscmd, doc_redis_cmds),
#endif
};
#undef LISPCMD
#undef LISPCMD_TAIL

/* ---------------- Argument Evaluation & Cleanup Macros ----------------
   Built-in commands frequently evaluate 1-4 arguments sequentially, checking
   for errors and managing unrefexp cascades. These macros reduce boilerplate.
   All assume `e` and `env` are in scope (the standard command signature). */

#define EVAL_ARG_1(v1)                                                         \
  exp_t *v1 = NULL;                                                            \
  if (e->next) {                                                               \
    v1 = EVAL(e->next->content, env);                                          \
    if (iserror(v1)) {                                                         \
      unrefexp(e);                                                             \
      return v1;                                                               \
    }                                                                          \
  }

#define EVAL_ARG_2(v1, v2)                                                     \
  EVAL_ARG_1(v1)                                                               \
  exp_t *v2 = NULL;                                                            \
  if (e->next && e->next->next) {                                              \
    v2 = EVAL(e->next->next->content, env);                                    \
    if (iserror(v2)) {                                                         \
      unrefexp(v1);                                                            \
      unrefexp(e);                                                             \
      return v2;                                                               \
    }                                                                          \
  }

#define EVAL_ARG_3(v1, v2, v3)                                                 \
  EVAL_ARG_2(v1, v2)                                                           \
  exp_t *v3 = NULL;                                                            \
  if (e->next && e->next->next && e->next->next->next) {                       \
    v3 = EVAL(e->next->next->next->content, env);                              \
    if (iserror(v3)) {                                                         \
      unrefexp(v1);                                                            \
      unrefexp(v2);                                                            \
      unrefexp(e);                                                             \
      return v3;                                                               \
    }                                                                          \
  }

#define CLEAN_RETURN_1(v1, ret)                                                \
  do {                                                                         \
    exp_t *_alc_ret = (ret);                                                   \
    unrefexp(v1);                                                              \
    unrefexp(e);                                                               \
    return _alc_ret;                                                           \
  } while (0)
#define CLEAN_RETURN_2(v1, v2, ret)                                            \
  do {                                                                         \
    exp_t *_alc_ret = (ret);                                                   \
    unrefexp(v1);                                                              \
    unrefexp(v2);                                                              \
    unrefexp(e);                                                               \
    return _alc_ret;                                                           \
  } while (0)
#define CLEAN_RETURN_3(v1, v2, v3, ret)                                        \
  do {                                                                         \
    exp_t *_alc_ret = (ret);                                                   \
    unrefexp(v1);                                                              \
    unrefexp(v2);                                                              \
    unrefexp(v3);                                                              \
    unrefexp(e);                                                               \
    return _alc_ret;                                                           \
  } while (0)
#define CLEAN_RETURN_4(v1, v2, v3, v4, ret)                                    \
  do {                                                                         \
    exp_t *_alc_ret = (ret);                                                   \
    unrefexp(v1);                                                              \
    unrefexp(v2);                                                              \
    unrefexp(v3);                                                              \
    unrefexp(v4);                                                              \
    unrefexp(e);                                                               \
    return _alc_ret;                                                           \
  } while (0)

int64_t gettimeusec() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000000 + tv.tv_usec);
}

#pragma GCC diagnostic ignored "-Wunused-parameter"
exp_t *error(int errnum, exp_t *id, env_t *env, const char *err_message, ...) {
  exp_t *ret;
  va_list ap;
  va_start(ap, err_message);
  ret = make_nil();
  ret->type = EXP_ERROR;
  ret->flags = errnum;
  if (vasprintf((char **)&ret->ptr, err_message, ap) < 0) {
    ret->ptr = strdup("<error formatting error message>");
  }
  va_end(ap);
  ret->next = refexp(id);
  /* Precise location for the top-level renderer: the offending form's own line
     (AST path — `id` is that form). The VM path passes the lambda (no line) and
     fills g_err_line itself from the bytecode pc→loc table in RUNTIME_ERR. */
  g_err_line = form_line(id);
  g_err_col = form_col(id);
  bt_capture(); /* snapshot the call stack at the error site for a backtrace */
  /* Debugger break-on-raise: an error raised outside any (try ...) during a
     top-level form's evaluation drops into the debugger HERE — at the raise
     site, where every frame and its env are still live (locals/p work). Gated
     so it never fires while parsing/loading, inside a try, or re-entrantly from
     a `p` evaluation. Like gdb's `catch throw`: wrap in (try ...) to suppress.
   */
  if (g_debug && g_dbg_evaluating && !g_dbg_active && g_try_depth == 0 &&
      g_handler_sp == 0 && errnum != EXP_ERROR_PARSING_EOF) {
    exp_t *rep = dbg_error_break(ret, env);
    if (rep != ret) { /* user typed `return <expr>` — recover with that value */
      unrefexp(ret);  /* discard the error; the failing form yields `rep` */
      return rep;     /* ownership transfers to error()'s caller */
    }
  }
  return ret;
}
#pragma GCC diagnostic warning "-Wunused-parameter"

/* Execute an internal builtin on its call form. THE single gate for the --safe
   sandbox: a FLAG_UNSAFE builtin (OS / filesystem / FFI / persistence / code-
   loading) is refused here. Every internal-cmd execution path routes through
   this — the AST evaluator's two fnc() sites and alc_apply_n (which backs
   apply, map/filter/reduce, and the compiled OP_CALL_GLOBAL fall-through) — so
   there is no bypass. Consumes `form` exactly as fnc does (the refusal error
   keeps it via its id ref), so callers need no extra cleanup. */
static inline exp_t *invoke_internal(exp_t *fn, exp_t *form, env_t *env) {
  /* Refuse a host-escape builtin (FLAG_UNSAFE) when sandboxed: either process-
     wide under --safe, or — the main case — while executing a RESP client's
     command callback (g_in_client_cmd, armed around resp_invoke_user_cmd). So
     code a network client can trigger can't reach OS/FS/FFI/code-loading, while
     the operator's own setup/REPL code stays unrestricted. (Global mutation
     from a callback — def/=/persist — is separately refused by
     g_resp_cb_guard.)
     `(allow-unsafe "name")` from trusted init grants a specific exception. */
  if ((g_safe_mode || g_in_client_cmd) && (fn->flags & FLAG_UNSAFE)) {
    exp_t *err = error(ERROR_ILLEGAL_VALUE, form, env,
                       "operation not permitted in this context "
                       "(sandboxed: OS / filesystem / FFI / code-loading)");
    unrefexp(form);
    return err;
  }
  return fn->fnc(form, env);
}

/* Raised by global-mutating special forms when invoked from a RESP user
   callback running under multiple reactors (see g_resp_cb_guard above). */
static exp_t *resp_cb_readonly_error(env_t *env) {
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "this RESP command callback is read-only w.r.t. Lisp globals "
               "(callbacks run concurrently under --threads); mutate the "
               "keyspace, not global defs/vars");
}

/* MEMORY MANAGEMENT FUNCTIONS */
/*
There are two function, refexp and unrefexp which handle , which handle the
incrementing and decrementing of the reference count of exp_t objects.

By default, functions that return a reference to an object pass on ownership
with the reference.

Macros such car,cdr,cadr,cddr,cddr,isatom... of course borrow reference and are
not supposed to decrement the reference count of the object.

print_node only borrow ownership and does not modify objects.

When objects are passed as parameter of a *cmd function, ownership is also
transfered otherwise, ownership is borrowed.

Concurrency issues are not yet taken care of.

When the reference count of an object reaches zero when decremented by unrefexp,
unrefexp frees the object. At some point a the struct exp_tfunc will point to
the function to be called to free the object depending on its type. It is not
yet implemented.

There are 2 ways to handle an object at the end of the function, transfer the
ownership or call unrefexp.

*/

void graceful_shutdown(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  /* TODO: Trigger any graceful shutdown hooks here later */
  exit(1);
}

/* ---- OOM recovery (Tier 1.3 follow-up) ------------------------------------
   A failed allocation on the eval path used to exit() the process — fatal for
   an embedding host or server. When a recovery point is armed (a top-level
   form evaluation / alcove_eval_string), a failed alloc instead longjmps back
   there, which resets interpreter state and surfaces a CATCHABLE out-of-memory
   error; the next form runs cleanly. The partially-built allocation and its
   refs leak (the process survives — far better than dying), and global eval
   state is reset at the landing. Unarmed (during init, or in a RESP reactor —
   the experimental --threads path), a failed alloc still graceful_shutdowns. */
static ALCOVE_TLS jmp_buf g_oom_jmp;
static ALCOVE_TLS int g_oom_armed = 0;
/* Test-only fault injection (driven by the unsafe (alloc-fail-after N)
   builtin): when >= 0, count down on each guarded alloc and fail when it
   reaches 0. Only consulted while armed, so it can't fire during init. */
static ALCOVE_TLS long g_alloc_fail_after = -1;

/* Raise OOM: longjmp to the armed recovery point, else die. Never returns. */
static void oom_raise(void) {
  if (g_oom_armed) {
    g_oom_armed = 0; /* disarm before unwinding so a re-fail can't re-longjmp */
    longjmp(g_oom_jmp, 1);
  }
  graceful_shutdown("Fatal error: Out of memory");
}
/* True (once) when injected failure should trigger; only while armed. */
static int alloc_should_fail(void) {
  if (g_oom_armed && g_alloc_fail_after >= 0) {
    if (g_alloc_fail_after == 0) {
      g_alloc_fail_after = -1; /* one-shot: disable after firing */
      return 1;
    }
    g_alloc_fail_after--;
  }
  return 0;
}

void *memalloc(size_t count, size_t size) {
  if (alloc_should_fail())
    oom_raise();
  void *ptr = calloc(count, size);
  if (!ptr)
    oom_raise();
  return ptr;
}

/* Checked realloc: abort on OOM instead of returning NULL. Lets call sites use
   the `p = xrealloc(p, n)` idiom safely — a bare `p = realloc(p, n)` both leaks
   the old block and NULL-derefs on failure. Mirrors memalloc's OOM policy. */
void *xrealloc(void *ptr, size_t size) {
  if (size && alloc_should_fail())
    oom_raise();
  void *p = realloc(ptr, size);
  if (!p && size)
    oom_raise();
  return p;
}

/* ---- C-stack-overflow guard (Tier 1.1) ------------------------------------
   Deep NON-tail recursion grows the C stack — every nested vm_run / AST
   invoke_body is a frame. Unbounded it SEGFAULTs with no catchable error, which
   is fatal when embedding untrusted/buggy code or running a server. A stack-
   pointer probe at the call boundaries raises an ordinary, *catchable* error
   instead, leaving enough headroom to build + raise + unwind it. Tail calls
   reuse the frame (VM trampoline / AST tail-marker), so they never trip it —
   the probe is in BYTES, so it adapts to the real per-frame size automatically.

   g_stack_base is per-thread, captured lazily at the first guarded call (always
   shallow — a top-level form's evaluation), so RESP worker threads each stamp
   their own base. The budget is the stack rlimit less a 1/4 margin (≥2 MiB on a
   default 8 MiB stack) — comfortably more than error()'s vasprintf + backtrace
   capture need. */
/* base held as an INTEGER, not a char* — storing &probe (a local) in a
   longer-lived object trips -Wdangling-pointer, and we only ever do arithmetic
   on it, never dereference it. */
static ALCOVE_TLS uintptr_t g_stack_base = 0;
static size_t g_stack_budget = 0; /* bytes of usable depth; computed once */

static int stack_guard_exhausted(void) {
  /* __builtin_frame_address tracks the REAL frame pointer; a plain `&local`
     is relocated to a heap "fake stack" under ASan's
     detect_stack_use_after_return, which would fool the depth measurement. */
  uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
  if (!g_stack_base) {
    g_stack_base = sp;
    if (!g_stack_budget) {
      /* 8 MiB is the default main-thread stack on Linux/macOS. Only ever
         SHRINK from it: a smaller `ulimit -s` must trip the guard earlier, but
         we must NOT trust a larger/unlimited RLIMIT_STACK — ASan (and some
         libcs) report a huge or infinite limit while the real guard page is
         still at 8 MiB, and trusting it would disable the guard and let the
         process segfault. */
      size_t lim = 8u * 1024 * 1024;
      struct rlimit rl;
      if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
          rl.rlim_cur >= (1u * 1024 * 1024) && (size_t)rl.rlim_cur < lim)
        lim = (size_t)rl.rlim_cur;
      /* Reserve a 512 KiB tail so the error path (vasprintf + backtrace
         capture) always has room, then allow the rest. The shallow error path
         needs only a few KiB, so 512 KiB is still ample headroom. The deepest
         legitimate recursion in the suite — Ackermann(3,7) in the unoptimized
         AST build — peaks near 7 MiB of an 8 MiB stack: its per-frame footprint
         is larger under Apple clang's unoptimized arm64 codegen than under
         glibc/gcc, and macOS also reports RLIMIT_STACK a hair under 8 MiB, so a
         full 1 MiB reserve tripped the guard on a recursion that has real stack
         to spare. 512 KiB keeps the error-path safety while admitting it. */
      size_t margin = 512u * 1024;
      g_stack_budget = lim > margin * 2 ? lim - margin : lim / 2;
    }
    return 0;
  }
  /* stack grows down on the supported targets: deeper frame => lower sp */
  return g_stack_base > sp && (g_stack_base - sp) > g_stack_budget;
}

/* ---- runaway-computation budget (Tier 1.4) --------------------------------
   An optional wall-clock deadline that bounds a runaway loop / tail recursion
   and raises a CATCHABLE "interrupted" error, so embedding untrusted or buggy
   code can't hang the host. Off by default: g_deadline_ms == 0 makes the check
   a single predicted-not-taken branch at loop back-edges, so straight-line and
   JIT'd code pay nothing. When armed, the monotonic clock is sampled only once
   per ~1024 back-edges, so even a tight VM loop pays ~one clock read / 1024
   iterations. Per-thread (TLS) so each RESP reactor / embedding caller is
   independent. NOTE: only INTERPRETED (VM/AST) loops are bounded — a JIT'd
   numeric loop is a terminating counting shape by construction, and a runaway
   (while t ...) / infinite tail loop is not a JIT shape, so it stays in the VM
   where the check lives. */
static ALCOVE_TLS int64_t g_deadline_ms =
    0; /* 0 = unlimited; else abs mono-ms */
static ALCOVE_TLS int64_t g_chunk_ceiling =
    0; /* 0 = unlimited; else max g_exp_chunks (Tier 1.3) */
static ALCOVE_TLS uint32_t g_budget_tick = 0;
/* exp_t arena high-water: # of EXP_BUMP_CHUNK-sized chunks ever calloc'd on
   this thread. Bumped only on the rare chunk-exhaustion path (every 256
   allocations), so the hot make_nil path is untouched. Backs both (heap-stats)
   [1.2] and the memory budget [1.3]: it rises only on genuine cell
   ACCUMULATION, never on alloc/free churn. Defined here (ahead of make_nil) so
   budget_check can read it. */
static ALCOVE_TLS int64_t g_exp_chunks = 0;

static int64_t alc_monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
/* Runaway checkpoint, called at loop back-edges. 0 = keep going; 1 = time
   budget (1.4) exceeded; 2 = memory budget (1.3) exceeded. Both off by default
   → two predicted-not-taken branches, so the hot path pays nothing. The memory
   test uses g_exp_chunks (the arena high-water from heap-stats): it rises only
   on genuine cell ACCUMULATION, never on steady-state alloc/free churn, so a
   tight loop that frees what it makes is not charged. */
static int budget_check(void) {
  if (g_chunk_ceiling && g_exp_chunks > g_chunk_ceiling)
    return 2;
  if (g_deadline_ms && (++g_budget_tick & 0x3FFu) == 0 &&
      alc_monotonic_ms() >= g_deadline_ms)
    return 1;
  return 0;
}

inline exp_t *refexp(exp_t *e) {
  /* Tagged immediates (fixnum, char) and canonical singletons (nil, t)
     are immortal — skip the refcount traffic. */
  if (!(is_immortal(e)))
    REFCOUNT_INC(&e->nref);
  return e;
}

/* exp_t free-list. Hot in cons-heavy code (sieve, listsum): each cons
   make_nils a fresh node, and discarding a list of length N triggers N
   unrefexp/free pairs. malloc/free is ~50ns per round trip; the
   free-list pop/push is ~3 cycles, ~10x faster. The list re-uses each
   freed exp_t's `next` pointer as the freelist link — safe because
   unrefexp recursively releases the original next before this point.
   __thread: per-shard worker, no cross-thread alloc traffic. On a
   single-threaded run the TLS slot collapses to one backing copy.

   INVARIANT (safety, not perf): the per-thread arena + freelist are
   intentionally never reclaimed — chunks are process-lived (no tracing GC,
   see exp_bump_next below) and the RESP reactor pool is fixed-size and
   spawned once, so no thread exit ever orphans a freelist. Sound only as
   long as reactor threads live for the whole process. If dynamic worker
   threads are ever added, drain this freelist + free the chunks at thread
   exit or it becomes a genuine per-thread leak. */
static ALCOVE_TLS exp_t *exp_freelist = NULL;

/* Bump-allocator for fresh exp_t when the free-list is empty. calloc(1,
   sizeof exp_t) is ~50ns per call; chunk-allocating 256 at a time
   amortizes that to ~4ns per exp_t. The chunks themselves are never
   freed — they live for the process lifetime, which matches alcove's
   model (the interpreter exits and the OS reclaims). __thread paired
   with the freelist above so each worker bumps from its own arena. */
#define EXP_BUMP_CHUNK 256
static ALCOVE_TLS exp_t *exp_bump_next = NULL;
static ALCOVE_TLS int exp_bump_left = 0;
/* Registry of every chunk base pointer, appended on the rare chunk-alloc
   path. This is what makes the arena ENUMERABLE — (gc-cycles) in gc.h
   walks it to visit every cell without needing a root set. Same TLS
   model as the arena: per-thread bases, per-thread collection. */
static ALCOVE_TLS exp_t **exp_chunk_bases = NULL;
static ALCOVE_TLS int64_t exp_chunk_cap = 0;
/* g_exp_chunks (the arena high-water backing heap-stats + the memory budget) is
   declared earlier, ahead of budget_check. */

/* Cold free path, reached from the inline unrefexp() ONLY once a refcount has
   hit <= 0: ret == 0 → e is dead (free its payload, push to the freelist, and
   walk e->next); ret < 0 → double-free. The hot cases — dropping an immediate/
   singleton, and decrementing a still-shared object that stays > 0 — return
   inline in the wrapper and never call here. e is always a heap object.
   Recurses (through the wrapper, so immediate children are free) for e->content
   and vector/list elements. */
int unrefexp_free(exp_t *e,
                  int ret) { /* non-static: see alcove.h (native modules) */
  SAFE_ASSERT(is_ptr(e)); /* wrapper's contract — checked under ALCOVE_SAFE */
  while (1) {
    /* Detect double-free: a refcount that went negative means this exp_t
       was already freed and the caller holds a dangling pointer. Abort in
       debug builds; in production at least avoid spiralling into UB. */
    if (ret < 0) {
      REFCOUNT_INC(&e->nref); /* undo the over-decrement */
#ifdef NDEBUG
      return 0;
#else
      abort(); /* double-free — crash loudly so the caller can be found */
#endif
    }
    /* meta holds a strdup'd name for LAMBDA/MACRO, or a borrowed
       resolved-exp_t* pointer for cached SYMBOL lookups. Only free()
       in the former case — the cached pointer is borrowed. */
    if (e->meta && (e->type == EXP_LAMBDA || e->type == EXP_MACRO)) {
      free(e->meta);
    }
    if ((e->flags & FLAG_COMPILED) &&
        (e->type == EXP_LAMBDA || e->type == EXP_MACRO)) {
      bytecode_free(e->bc);
    }
    /* Closure capture lives in lambda/macro's wrapper-node meta field
       (see fncmd / defcmd / defmacrocmd). Release it BEFORE unref'ing
       the wrapper, since after that e->next may be freed. */
    if ((e->type == EXP_LAMBDA || e->type == EXP_MACRO) && e->next &&
        e->next->meta) {
      destroy_env((env_t *)e->next->meta);
      e->next->meta = NULL;
    }
    exp_t *next = e->next;
    /* Release this node's payload, dispatched on type. Contiguous enum -> a
       jump table; the hot EXP_PAIR case (every cons cell + nil) lands in
       `default` directly instead of after ~10 sequential `e->type ==` tests.
       EXP_FLOAT / EXP_INTERNAL / immediates MUST be explicit no-op cases:
       falling through to `default` would call unrefexp on their inline union
       value (a double / fn pointer), not a child. ERROR heap-allocates its
       message via vasprintf (and overloads `flags` as the error code, so it
       is never flag-tested). A container with a NULL ptr does nothing —
       equivalent to the old fall-through, since ptr aliases content. */
    switch (e->type) {
    case EXP_ERROR:
      free(e->ptr);
      break;
    case EXP_SYMBOL:
    case EXP_STRING:
      /* Free heap text only when not inline (inline bytes live in the
         struct's `ptr` union — see FLAG_INLINE_TXT). */
      if (!(e->flags & FLAG_INLINE_TXT))
        free(e->ptr);
      break;
    case EXP_FFI: {
      extern void alc_ffi_free(void *ptr); /* defined alongside ffi_call */
      alc_ffi_free(e->ptr);
      break;
    }
    case EXP_PORT: {
      alc_port_t *p = (alc_port_t *)e->ptr;
      if (p) {
        if (!p->closed && p->fp)
          fclose(p->fp);
        free(p->path);
        free(p);
      }
      break;
    }
    case EXP_VECTOR:
      if (e->ptr) {
        /* VEC_KIND_GEN cells each own a ref — walk and release. Typed
           (i64/f64) cells are raw scalars — just free the storage. */
        if (vec_kind(e) == VEC_KIND_GEN) {
          int64_t n = vec_len(e);
          for (int64_t i = 0; i < n; i++)
            unrefexp(vec_gen_at(e, i));
        }
        free(e->ptr);
      }
      break;
    case EXP_BLOB:
      free(e->ptr); /* alc_blob_t is a single flex-array alloc */
      break;
    case EXP_RATIONAL: /* alc_rat_t / alc_dec_t: single malloc, no nested refs
                        */
    case EXP_DECIMAL:
      free(e->ptr);
      break;
    case EXP_DICT:
    case EXP_SET:
      if (e->ptr)
        destroy_dict((dict_t *)e->ptr); /* unrefs every value internally */
      break;
    case EXP_HAMT:
      if (e->ptr) {
        extern void hamt_free(void *ptr); /* defined alongside the HAMT ops */
        hamt_free(e->ptr); /* unrefs the shared (refcounted) trie */
      }
      break;
    case EXP_LIST:
      if (e->ptr) {
        alc_list_t *l = (alc_list_t *)e->ptr;
        alc_listnode_t *n = l->head;
        while (n) {
          alc_listnode_t *nx = n->next;
          unrefexp(n->val);
          free(n);
          n = nx;
        }
        free(l);
      }
      break;
    case EXP_NUMBER:
    case EXP_FLOAT:
    case EXP_CHAR:
    case EXP_BOOLEAN:
    case EXP_INTERNAL:
      /* Numeric/char/bool live inline in the union; EXP_INTERNAL has no
         owned payload — nothing to release. */
      break;
    case EXP_LAMBDA:
    case EXP_MACRO:
      /* Compiled: content is unioned with bc, already released by
         bytecode_free above. AST: content is the params list — recurse. */
      if (!(e->flags & FLAG_COMPILED))
        unrefexp(e->content);
      break;
    default:
      /* Custom (foreign) module type: its destroy hook frees the C payload
         (e->ptr) + unrefs any exp_t it owns. Do NOT recurse on e->content —
         a foreign value's slot is a void* C struct, not an exp_t car. */
      if (e->type >= EXP_MAXSIZE) {
        if (exp_tfuncList[e->type] && exp_tfuncList[e->type]->destroy)
          exp_tfuncList[e->type]->destroy(e);
        break;
      }
      /* EXP_PAIR (incl. nil), EXP_TREE, EXP_PAIR_CIRCULAR: recurse on the
         child; e->next is released by the next loop iteration. */
      unrefexp(e->content);
      break;
    }

    e->next = exp_freelist;
    exp_freelist = e;
    /* Walk to the chain successor and decrement it; stop if it's still live,
       or NULL / an immortal singleton (is_immortal covers both). Otherwise
       loop to free it too (the ret < 0 guard at the top runs). */
    e = next;
    if (is_immortal(e))
      return is_ptr(e) ? 1 : 0;
    ret = REFCOUNT_DEC(&e->nref);
    if (ret > 0)
      return ret;
  }
}

/* ---- type-specialized releasers --------------------------------------------
   At a call site where the object's type is statically known, these skip the
   immortal-singleton checks AND the type switch the general unrefexp runs,
   doing only the work that type needs. Implemented as inline functions (not
   macros) so the argument is evaluated exactly once — call sites pass things
   like POP() or cur->val, which a function-like macro would evaluate 2-3×.
   Under -DALCOVE_SAFE=1 each asserts its type/refcount assumption; in a normal
   build the asserts vanish and only the minimal release remains. */

/* A number is a tagged-immediate fixnum: no heap, no refcount → pure no-op
   (a known-fixnum result needs no release at all). Marked unused: it has no
   call site today (releasing a known fixnum is a literal no-op, so callers
   just skip it), but it's kept for symmetry with the unref_<type> family so a
   known-fixnum site can stay uniform — same idiom as the jit_call_* helpers. */
__attribute__((unused)) static inline void unref_number(exp_t *e) {
  SAFE_ASSERT(isnumber(e));
  (void)e;
}

/* A float is heap with no children and is never an immortal singleton, so its
   release is just decrement-and-free — no is_immortal, no switch, no call. */
static inline void unref_float(exp_t *e) {
  SAFE_ASSERT(isfloat(e));
  int ret = REFCOUNT_DEC(&e->nref);
  SAFE_ASSERT(ret >= 0); /* double-free caught under ALCOVE_SAFE */
  if (ret == 0) {
    e->next = exp_freelist;
    exp_freelist = e;
  }
}

/* An arithmetic operand is one of the two numeric types: fixnum (immediate,
   no-op) or float (the only heap numeric → unref_float). */
static inline void unref_number_or_float(exp_t *e) {
  if (is_ptr(e))
    unref_float(e);
  else
    SAFE_ASSERT(isnumber(e));
}

/* env management*/

/* ---------------- Environment stack arena ----------------
   The common case is LIFO: every env created by make_env is destroyed
   at the end of the call/let/with/for that made it, so a bump allocator
   wins over calloc/free per function call.

   Closures complicate this. fncmd / defcmd / defmacrocmd do
     val->next->meta = (struct keyval_t *)ref_env(env)
   to keep the defining env alive while the lambda exists. When that
   captured env is an arena slot AND the lambda outlives its caller,
   destroy_env's REFCOUNT_DEC stays > 0 and the bump-pointer rollback
   at line below is SKIPPED — the slot remains live but is no longer
   on top of the shard's arena_sp. Subsequent make_env calls hand out
   arena slots above it; the captured env is never reused. The only
   cost is that the bump allocator can fragment under heavy closure use.

   What is NOT safe: assuming arena slots strictly map to nesting
   depth, or recovering an "earlier" arena slot once it's been ref'd.
   Any future change that overwrites or recycles non-top arena slots
   breaks closure correctness silently.

   Falls back to malloc() if the arena ever overflows (deep recursion
   beyond ENV_ARENA_SLOTS). */

/* Storage for the main shard's arena. The shard struct holds pointers
   into this array; spawning workers later will allocate their own
   arenas (heap or static-per-worker) and point their shard_t at them.
   ENV_ARENA_SLOTS is defined alongside shard_t in alcove.h. */
static env_t main_shard_arena[ENV_ARENA_SLOTS];
shard_t main_shard = {
    .arena = main_shard_arena,
    .arena_sp = main_shard_arena,
    .arena_end = main_shard_arena + ENV_ARENA_SLOTS,
};
/* Initial value &main_shard is a constant expression, so the TLS
   initializer is well-formed. Spawned workers overwrite this when they
   start. */
ALCOVE_TLS shard_t *current_shard = &main_shard;

/* Bring the inbox queue and wake fd online. Idempotent: re-calling on
   an already-initialized shard is a no-op. Returns 0 on success, -1 on
   failure (errno set by alc_wake_init). The reactor must call this
   before entering its select() loop. */
int shard_runtime_init(shard_t *sh) {
  if (sh->runtime_ready == 1)
    return 0;
  mpsc_init(&sh->inbox);
  if (alc_wake_init(&sh->wake) < 0) {
    sh->runtime_ready = -1;
    return -1;
  }
  sh->runtime_ready = 1;
  return 0;
}

/* Tear down inbox/wake. Caller is responsible for ensuring no producer
   will signal after this returns. */
void shard_runtime_destroy(shard_t *sh) {
  if (sh->runtime_ready != 1)
    return;
  /* Drain any leftover nodes so we don't leak. No producer exists today;
     any node here is a bug. When Step 2.5 lands, variant-specific
     cleanup (close fds, dec refcounts) belongs at the producer side. */
  int leaked = 0;
  for (mpsc_node_t *n; (n = mpsc_dequeue(&sh->inbox));) {
    free(n);
    leaked++;
  }
  assert(leaked == 0 && "stray inbox nodes — producer didn't quiesce");
  alc_wake_destroy(&sh->wake);
  sh->runtime_ready = 0;
}

/* Bumped on every operation that mutates a global binding (def, defmacro,
   persist, forget, savedb, top-level updatebang). The bytecode global-
   resolution cache compares this against its own per-slot gen to detect
   stale entries. Starts at 1 so a fresh gcache_entry{val=NULL,gen=0} is
   trivially stale. */
uint64_t alcove_global_gen = 1;

/* env_t lifecycle (make/ref/destroy + closure-cycle reclaim) lives in a
   dedicated #included fragment; lookup stays with eval. */
#include "env.h"

/* TOKEN MANAGEMENT FUNCTIONS */

inline token_t *tokenize(int v) {
  token_t *token = memalloc(1, sizeof(token_t));
  token->size = 0;
  token->maxsize = TOKENMINSIZE;
  token->data = memalloc(TOKENMINSIZE, sizeof(char));
  if (v != -1)
    token->data[token->size++] = v;
  return token;
}

inline void freetoken(token_t *token) {
  free(token->data);
  free(token);
}

inline void tokenadd(token_t *token, int v) {
  char *tmp;
  if (token->size + 1 >= token->maxsize) {
    tmp = memalloc(token->maxsize * 2, sizeof(char));
    strncpy(tmp, token->data, token->maxsize);
    token->maxsize *= 2;
    free(token->data);
    token->data = tmp;
  }
  token->data[token->size++] = v;
}

inline void tokenappend(token_t *token, char *src, int len) {
  char *tmp;
  if ((token->size + len + 1) >= token->maxsize) {
    int d = 2;
    while ((token->size + len + 1) >= (d * token->maxsize))
      d *= 2;
    tmp = memalloc(token->maxsize * d, sizeof(char));
    strncpy(tmp, token->data, token->maxsize);
    token->maxsize *= d;
    free(token->data);
    token->data = tmp; /* WAS MISSING — UAF on the next access. */
  }
  strncpy(token->data + token->size, src, len);
  token->size += len;
}

// HASH FUNCTIONS

/* The dict_t hash table (env store + dict/set substrate) lives in a
   dedicated #included fragment. */
#include "dict.h"

// see page 25 concept of "liaison immuable" et liaison "muable"

static inline exp_t *make_nil() {
  exp_t *nil_exp;
  if (exp_freelist) {
    nil_exp = exp_freelist;
    exp_freelist = nil_exp->next;
    /* Zero out the fields that could carry stale state from the
       previous tenant. memset would also work but is ~3x slower
       for a 32-byte struct on a hot path. (content and bc share the
       primary union; clearing content covers both.) */
    nil_exp->flags = 0;
    nil_exp->content = NULL;
    nil_exp->meta = NULL;
    nil_exp->next = NULL;
  } else {
    if (exp_bump_left == 0) {
      exp_bump_next = (exp_t *)calloc(EXP_BUMP_CHUNK, sizeof(exp_t));
      if (!exp_bump_next)
        oom_raise(); /* was an unchecked NULL deref → segfault on exp_t OOM */
      exp_bump_left = EXP_BUMP_CHUNK;
      g_exp_chunks++; /* rare path: per-chunk, not per-alloc */
      if (g_exp_chunks > exp_chunk_cap) {
        int64_t ncap = exp_chunk_cap ? exp_chunk_cap * 2 : 64;
        exp_t **nb =
            (exp_t **)realloc(exp_chunk_bases, (size_t)ncap * sizeof *nb);
        if (!nb)
          oom_raise();
        exp_chunk_bases = nb;
        exp_chunk_cap = ncap;
      }
      exp_chunk_bases[g_exp_chunks - 1] = exp_bump_next;
    }
    nil_exp = exp_bump_next++;
    exp_bump_left--;
  }
  nil_exp->type = EXP_PAIR;
  nil_exp->nref = 1;
  nil_exp->content = NULL;
  nil_exp->next = NULL;
  nil_exp->meta = NULL;
  return nil_exp;
}

/* UTF-8 codepoint helpers live in a dedicated (libc-only) header. */
#include "utf8.h"

inline exp_t *make_char(uint32_t c) {
  /* Tagged immediate: no heap allocation. Holds a full Unicode codepoint. */
  return MAKE_CHAR(c);
}

inline exp_t *make_node(exp_t *node) {
  exp_t *cur = make_nil();
  if (node)
    cur->content = node;
  return cur;
}

inline exp_t *make_internal(lispCmd *cmd, int flags) {
  MAKE_TYPED(cur, EXP_INTERNAL, cmd);
  cur->flags = flags;
  return cur;
}

/* Public: register a name → C function as an alcove builtin. Safe to call
   after main() has finished its init pass. Used by the web build's JS shim
   to inject browser-side implementations (Canvas, Web Audio, …) as alcove
   callables, but the API is generic — any embedder can use it. Returns 0
   on success, -1 if `reserved_symbol` hasn't been created yet. */
int alcove_register_cmd(const char *name, lispCmd *fn, int tail_aware) {
  if (!reserved_symbol || !name || !fn)
    return -1;
  /* Non-tail-aware module builtins are applicative (they eval all their args),
     so mark them FLAG_APPLICATIVE — the compiler can then emit a fast
     OP_CALL_GLOBAL for them instead of the OP_EVAL_AST tree-walk. */
  exp_t *val =
      make_internal(fn, tail_aware ? FLAG_TAIL_AWARE : FLAG_APPLICATIVE);
  set_get_keyval_dict(reserved_symbol, (char *)name, val);
  unrefexp(val);
  return 0;
}

/* Attach a values fast-path (lispCmdV) to an already-registered builtin. Stored
   in the internal's `meta` field (unused by internals; make_internal leaves it
   NULL, so a NULL meta means "no fast path"). vm_invoke_values' internal arm
   calls it directly with the evaluated argv when present. */
int alcove_set_cmd_values(const char *name, lispCmdV *fnv) {
  if (!reserved_symbol || !name || !fnv)
    return -1;
  keyval_t *kv = set_get_keyval_dict(reserved_symbol, (char *)name, NULL);
  if (!kv || !isinternal((exp_t *)kv->val))
    return -1;
  ((exp_t *)kv->val)->meta = (keyval_t *)(void *)fnv;
  return 0;
}

/* Look up a registered custom type by name; returns its id or 0 if none. */
static unsigned short custom_type_id_by_name(const char *name) {
  if (!name)
    return 0;
  for (unsigned short i = EXP_MAXSIZE; i < g_next_type_id; i++)
    if (g_custom_types[i].name && strcmp(g_custom_types[i].name, name) == 0)
      return i;
  return 0;
}

unsigned short alcove_register_type(const char *name, const exp_tfunc *ops) {
  if (!name || !ops)
    return 0;
  if (g_next_type_id < EXP_MAXSIZE)
    g_next_type_id = EXP_MAXSIZE; /* first custom id sits above the built-ins */
  /* Idempotent: a module re-(require)d, or two registrations of the same name,
     reuse the existing id (and keep live objects of that type valid). */
  unsigned short existing = custom_type_id_by_name(name);
  if (existing)
    return existing;
  if (g_next_type_id >= ALCOVE_TYPE_CAP)
    return 0; /* table full — raise ALCOVE_TYPE_CAP if you truly need >~225 */
  unsigned short id = g_next_type_id++;
  exp_tfuncList[id] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  *exp_tfuncList[id] = *ops; /* copy the dump/load/destroy/print pointers */
  g_custom_types[id].name = strdup(name);
  g_custom_types[id].module_spec =
      g_current_module_spec ? strdup(g_current_module_spec) : NULL;
  GEN_BUMP(); /* a new type may shadow nothing, but keep caches honest */
  return id;
}

exp_t *alcove_make_foreign(unsigned short type_id, void *ptr) {
  if (type_id < EXP_MAXSIZE || type_id >= ALCOVE_TYPE_CAP ||
      !exp_tfuncList[type_id])
    return NIL_EXP; /* not a registered custom type */
  exp_t *e = make_nil();
  e->type = type_id;
  e->ptr = ptr;
  return e;
}

void *alcove_foreign_ptr(const exp_t *e) {
  return (is_ptr(e) && e->type >= EXP_MAXSIZE) ? e->ptr : NULL;
}

int alcove_is_foreign(const exp_t *e, unsigned short type_id) {
  return is_ptr(e) && e->type == type_id && type_id >= EXP_MAXSIZE;
}

/* Embedder helpers — evaluate the Nth argument of an in-flight builtin
   call and return it as a plain C type. Companion to alcove_register_cmd
   so a host (e.g. the WASM build's JS shim) can implement builtins with
   `int(int,int,...)` signatures and let alcove handle the exp_t plumbing.
   N is 0-indexed. */
int alcove_arg_int(exp_t *e, env_t *env, int n) {
  exp_t *cur = e ? e->next : NULL;
  for (int i = 0; i < n && cur; i++)
    cur = cur->next;
  if (!cur || !cur->content)
    return 0;
  exp_t *val = EVAL(cur->content, env);
  if (!val)
    return 0;
  /* Accept a fixnum or a float (truncate). Never call FIX_VAL on a float —
     that would shift its heap pointer and return garbage (this is what broke
     web Mario's (gfx-music-play-file -1) when (- 1) momentarily evaluated to
     the float -1.0). Non-numbers yield 0. */
  int rv = isnumber(val) ? (int)FIX_VAL(val) : (isfloat(val) ? (int)val->f : 0);
  unrefexp(val);
  return rv;
}

/* Returns a pointer to alcove's internal string storage for the Nth arg,
   or NULL if the arg isn't a string. The pointer is stable for the
   lifetime of the alcove value (which lives at least as long as the
   builtin call) — the host should copy if it needs to outlive the call. */
const char *alcove_arg_string(exp_t *e, env_t *env, int n) {
  exp_t *cur = e ? e->next : NULL;
  for (int i = 0; i < n && cur; i++)
    cur = cur->next;
  if (!cur || !cur->content)
    return NULL;
  exp_t *val = EVAL(cur->content, env);
  if (!val)
    return NULL;
  const char *s = (is_ptr(val) && val->type == EXP_STRING)
                      ? (const char *)exp_text(val)
                      : NULL;
  /* Copy into a static round-robin buffer so the pointer is safe to
     return after we unref. Round-robin gives a few concurrent slots
     for the common pattern of reading several string args in a row. */
  static char buf[4][1024];
  static int slot = 0;
  const char *out = NULL;
  if (s) {
    int i = slot;
    slot = (slot + 1) & 3;
    strncpy(buf[i], s, sizeof(buf[i]) - 1);
    buf[i][sizeof(buf[i]) - 1] = 0;
    out = buf[i];
  }
  unrefexp(val);
  return out;
}

/* Wrap a C int as a tagged-fixnum exp_t* for return from a host-side
   builtin. The host can't compute the tagged value itself without
   knowing alcove's pointer layout, so we expose this. */
exp_t *alcove_make_int(int v) { return MAKE_FIX(v); }

void tree_add_node(exp_t *tree, exp_t *node) {
  exp_t *cur = tree;
  if ((cur = cur->content)) {
    if (cur->type == EXP_PAIR)
      pair_add_node(cur, node);
    else if (cur->type == EXP_TREE)
      tree_add_node(cur, node);
    else
      printf("ERROR IMPOSSIBLE TO ADD ");
  } else
    tree->content = make_node(node);
}

void pair_add_node(exp_t *pair, exp_t *node) {
  exp_t *cur = pair;
  if (cur->type == EXP_PAIR) {
    if (cur->next) {
      cur = cur->next;
      if (cur->type == EXP_PAIR)
        pair_add_node(cur, node);
      else if (cur->type ==
               EXP_TREE) /* was duplicate EXP_PAIR — copy-paste bug */
        tree_add_node(cur, node);
      else
        printf("ERROR UNABLE TO ADD NODE TO EXP");
    } else {
      cur = cur->next = make_node(node);
      cur->content = node; /* ??? **/
    }
  } else
    printf("ERROR IMPOSSIBLE TO ADD NODE TO NON PAIR OBJECT\n");
}

exp_t *make_tree(exp_t *root, exp_t *node1) {
  exp_t *tree = make_nil();
  tree->type = EXP_TREE;
  tree->next = refexp(root);
  if (node1)
    tree->content = refexp(node1);
  if (root)
    tree_add_node(root, tree);
  return tree;
}

/* The canonical value printer lives in a dedicated #included fragment. */
#include "print.h"

exp_t *make_string_from_token(token_t *token, int offset, int final_length) {
  exp_t *cur = make_nil();
  cur->type = EXP_STRING;
  if (final_length <= INLINE_TXT_CAP) {
    /* Short string literal: store inline, no heap bytes. */
    cur->flags |= FLAG_INLINE_TXT;
    memcpy(cur->istr, token->data + offset, final_length);
    cur->istr[final_length] = '\0';
    free(token->data);
  } else {
    char *ptr = realloc(token->data, final_length + 1);
    if (!ptr)
      oom_raise();
    if (offset > 0)
      memmove(ptr, ptr + offset, final_length);
    ptr[final_length] = '\0';
    cur->ptr = ptr;
  }
  free(token);
  return cur;
}

exp_t *make_symbol_from_token(token_t *token) {
  int n = token->size;
  exp_t *cur = make_nil();
  cur->type = EXP_SYMBOL;
  if (n <= INLINE_TXT_CAP) {
    /* Short name: store inline (no heap symbol bytes). This is the common
       case for source symbols — keywords, identifiers, operators — so it
       must inline here, not only in make_symbol. We own token->data. */
    cur->flags |= FLAG_INLINE_TXT;
    memcpy(cur->istr, token->data, n);
    cur->istr[n] = '\0';
    free(token->data);
  } else {
    /* Long name: reuse the token's own buffer as the heap symbol string. */
    char *ptr = realloc(token->data, n + 1);
    if (!ptr)
      oom_raise();
    ptr[n] = '\0';
    cur->ptr = ptr;
  }
  free(token);
  return cur;
}

static inline char *alloc_str(char *str, int length) {
  char *ptr = memalloc(length + 1, sizeof(char));
  memcpy(ptr, str, length);
  ptr[length] = '\0';
  return ptr;
}

/* Build a SYMBOL/STRING, storing short text inline in the primary union
   (no heap alloc); longer text falls back to the heap. The inline bytes
   overlap `ptr`, so readers must go through exp_text() — `meta` is left
   free for the symbol resolution cache. See FLAG_INLINE_TXT. */
static inline exp_t *make_inline_txt(char *str, int length, int type) {
  /* A negative length here means a caller truncated a size_t > INT_MAX into
     this signed int (e.g. a multi-GB blob/string). Left unchecked it would
     sign-extend in the memcpy below into a ~SIZE_MAX copy — a heap smash.
     alcove can't represent text that long anyway, so treat it as a fatal
     invariant violation, consistent with the out-of-memory convention. */
  if (length < 0)
    graceful_shutdown("Fatal error: text length overflow (size_t > INT_MAX)");
  exp_t *cur = make_nil();
  cur->type = (unsigned short)type;
  if (length <= INLINE_TXT_CAP) {
    cur->flags |= FLAG_INLINE_TXT;
    memcpy(cur->istr, str, length);
    cur->istr[length] = '\0';
  } else {
    cur->ptr = alloc_str(str, length);
  }
  return cur;
}

inline exp_t *make_string(char *str, int length) {
  return make_inline_txt(str, length, EXP_STRING);
}

/* Like make_string, but ADOPTS a heap buffer instead of copying it: the long
   case takes ownership of `buf` (no second alloc + copy), the short case copies
   inline and frees `buf`. Either way the caller must NOT free `buf` afterwards.
   `buf` must be a malloc/realloc'd buffer of at least length+1 bytes (room for
   the NUL); strings are NUL-terminated C text (length is strlen-based). Use at
   the ~dozen sites that built a buffer via str_buf_put/memalloc only to hand it
   to make_string and free it. */
static exp_t *make_string_take(char *buf, int length) {
  if (length < 0)
    graceful_shutdown("Fatal error: text length overflow (size_t > INT_MAX)");
  if (length <= INLINE_TXT_CAP) {
    exp_t *cur = make_string(buf, length); /* inline copy */
    free(buf);
    return cur;
  }
  exp_t *cur = make_nil();
  cur->type = EXP_STRING;
  buf[length] =
      '\0';       /* in-capacity (buf has >= length+1); ensure termination */
  cur->ptr = buf; /* adopt — no copy */
  return cur;
}

inline exp_t *make_symbol(char *str, int length) {
  return make_inline_txt(str, length, EXP_SYMBOL);
}

/* Replace the text bytes of an EXP_STRING/EXP_SYMBOL in place, picking
   inline vs heap storage like make_inline_txt and freeing any prior heap
   buffer. `bytes` must not alias e's storage. Used by codepoint-aware
   string index assignment, where the new codepoint can differ in byte
   width from the old one. */
static void exp_set_text(exp_t *e, const char *bytes, size_t len) {
  char *old = (e->flags & FLAG_INLINE_TXT) ? NULL : (char *)e->ptr;
  if (len <= (size_t)INLINE_TXT_CAP) {
    e->flags |= FLAG_INLINE_TXT;
    memcpy(e->istr, bytes, len);
    e->istr[len] = '\0';
  } else {
    char *nb = alloc_str((char *)bytes, (int)len);
    e->flags &= ~FLAG_INLINE_TXT;
    e->ptr = nb;
  }
  if (old)
    free(old);
}

inline exp_t *make_quote(exp_t *node) {
  exp_t *cur = make_symbol("quote", strlen("quote"));
  cur = make_node(cur);
  cur->next = make_node(node);
  return cur;
}

/* ---------------- Clojure-style container constructors ----------------
   See alcove.h for the type definitions. Each returns a fresh exp_t with
   nref=1; unrefexp's free-path knows how to drop them (see line ~365). */

exp_t *make_blob(const char *bytes, size_t len) {
#if UINTPTR_MAX <= 0xffffffff
  if (len > SIZE_MAX - sizeof(alc_blob_t))
    oom_raise();
#endif
  alc_blob_t *b = (alc_blob_t *)malloc(sizeof(alc_blob_t) + len);
  if (!b)
    oom_raise();
  b->len = len;
  if (len && bytes)
    memcpy(b->bytes, bytes, len);
  else if (len)
    memset(b->bytes, 0, len);
  MAKE_TYPED(cur, EXP_BLOB, b);
  return cur;
}

exp_t *make_dict_exp(void) {
  MAKE_TYPED(cur, EXP_DICT, create_dict());
  return cur;
}

exp_t *make_set_exp(void) {
  MAKE_TYPED(cur, EXP_SET, create_dict());
  return cur;
}

exp_t *make_list_exp(void) {
  alc_list_t *l = (alc_list_t *)memalloc(1, sizeof(alc_list_t));
  MAKE_TYPED(cur, EXP_LIST, l);
  return cur;
}

size_t blob_len(exp_t *e) {
  return (isblob(e) && e->ptr) ? ((alc_blob_t *)e->ptr)->len : 0;
}

const char *blob_bytes(exp_t *e) {
  return (isblob(e) && e->ptr) ? ((alc_blob_t *)e->ptr)->bytes : NULL;
}

inline exp_t *make_integer(char *str) {
  /* Tagged immediate: no heap allocation. Use strtoll so we can detect
     out-of-range literals — fixnum is int61, so anything outside
     [-2^60, 2^60) silently wrapped under atoll. We promote oversize
     ints to floats (matches the ARC convention; preserves magnitude
     and reports approximate value) rather than silently truncating. */
  errno = 0;
  char *end;
  long long v = strtoll(str, &end, 10);
  int64_t fix_max = ((int64_t)1 << 60) - 1;
  int64_t fix_min = -((int64_t)1 << 60);
  if (errno == ERANGE || v > fix_max || v < fix_min) {
    /* Out of fixnum range. Explicit over implicit: an integer literal that
       doesn't fit is an error, not a silent float — write a float literal
       (1e24), a rational (n/d), or a decimal (...m) if that's what you mean. */
    return error(ERROR_ILLEGAL_VALUE, NULL, NULL,
                 "integer literal out of range (exceeds the fixnum range; "
                 "write a float, rational, or decimal)");
  }
  return MAKE_FIX((int64_t)v);
}

inline exp_t *make_integeri(int64_t i) { return MAKE_FIX(i); }

inline exp_t *make_float(char *str) {
  exp_t *cur = make_nil();
  cur->type = EXP_FLOAT;
  cur->f = strtod(str, NULL);
  return cur;
}

inline exp_t *make_floatf(expfloat f) {
  exp_t *cur = make_nil();
  cur->type = EXP_FLOAT;
  cur->f = f;
  return cur;
}

/* Exact non-integer numeric types (rational, decimal). Needs make_nil,
   memalloc, FIX_FITS/FIX_VAL/MAKE_FIX and EXP_RATIONAL — all in scope here. */
#include "numeric.h"

// exp_t dump and load

size_t loadtype(FILE *stream, unsigned short int *type) {
  return fread(type, sizeof(unsigned short int), 1, stream);
}

size_t dumptype(FILE *stream, unsigned short int *type) {
  return fwrite(type, sizeof(unsigned short int), 1, stream);
}

size_t loadsize_t(FILE *stream, size_t *len) {
  return fread(len, sizeof(size_t), 1, stream);
}

size_t dumpsize_t(FILE *stream, size_t *len) {
  return fwrite(len, sizeof(size_t), 1, stream);
}

/* Sanity bound for nested-pair recursion in dump format. ~64K levels at
   the C stack costs ~4MB; well under the typical 8MB stack but blocks
   the trivially-malicious "millions of nested pairs" file. */
#define ALCOVE_LOAD_MAX_DEPTH 16384
/* __thread: depth is caller-local — a worker loading a dump shouldn't
   trip another worker's guard. */
static ALCOVE_TLS int alcove_load_depth = 0;

exp_t *load_exp_t(FILE *stream) {
  exp_t *resp = make_nil();
  if (loadtype(stream, &(resp->type)) <= 0) {
    unrefexp(resp);
    return NULL;
  }
  /* v3: a custom-type id in the body is the DUMP session's id — remap it to
     this process's id by durable name (table built in alcove_load_unified)
     BEFORE validating against exp_tfuncList. Unresolved (module absent /
     --safe) → abort this value's load. */
  if (alcove_load_dump_version >= 3 && resp->type >= EXP_MAXSIZE) {
    unsigned short cur =
        (resp->type < ALCOVE_TYPE_CAP) ? g_type_remap[resp->type] : 0;
    if (!cur) {
      unrefexp(resp);
      return NULL;
    }
    resp->type = cur;
  }
  /* Validate the type tag against the dispatch table BEFORE __LOAD__
     indexes exp_tfuncList[type]. A malicious db.dump with type=0xFFFF
     would otherwise read out-of-bounds and indirect-call whatever
     pointer-shaped bytes are there at startup. */
  if (resp->type < 1 || resp->type >= ALCOVE_TYPE_CAP ||
      !exp_tfuncList[resp->type] || !exp_tfuncList[resp->type]->load) {
    unrefexp(resp);
    return NULL;
  }
  if (++alcove_load_depth > ALCOVE_LOAD_MAX_DEPTH) {
    --alcove_load_depth;
    unrefexp(resp);
    return NULL;
  }
  exp_t *r = __LOAD__(resp, stream);
  --alcove_load_depth;
  return r;
}

/* Per-type dump/load serializers live in a dedicated #included fragment. */
#include "persist.h"

/* Source-location tracking for the reader. Only touched on the cold parse
   path (the reader's getc/ungetc sites) and the file/eval driver loops —
   NEVER in the VM hot loop or JIT. g_reader_line is 1-based and bumped on
   every consumed '\n'; g_reader_src is the current source label (a file
   path) or NULL for interactive/anonymous input. The file drivers
   save/restore both on the C stack so a nested (load …) reports the inner
   file's lines and resumes the outer file's afterwards. */
static ALCOVE_TLS int g_reader_line = 1;
/* Column (1-based) and absolute byte offset within the current source, kept in
   sync with g_reader_line by the getc/ungetc wrappers. Cold parse path only —
   never read in the VM hot loop or JIT. Used to place the error caret. */
static ALCOVE_TLS int g_reader_col = 1;
static ALCOVE_TLS long g_reader_off = 0;
/* Reader nesting depth — one C frame is consumed per opening (/[/{ via the
   callmacrochar→reader recursion, so deeply-nested untrusted source would
   otherwise overflow the C stack (a hard crash on every reader entry point:
   scripts, eval/load, the REPL, the alcove_eval_string embedding API). Capped
   like the persistence loader's ALCOVE_LOAD_MAX_DEPTH. Cold parse path only. */
static ALCOVE_TLS int g_reader_depth = 0;
#define ALCOVE_READER_MAX_DEPTH 2000
static ALCOVE_TLS const char *g_reader_src = NULL;
/* The in-memory text of the current source, retained by the drivers so the
   error caret can print the offending line. NULL when no buffer is available
   (e.g. piped stdin). Borrowed — the driver owns the storage. For the Adder
   surface this points at the ORIGINAL Adder source (not the transpiled
   s-expr), paired with g_adder_map (see als_map). */
static ALCOVE_TLS const char *g_reader_srctext = NULL;
static ALCOVE_TLS size_t g_reader_srctext_len = 0;
/* When the current source is Adder, this maps a GENERATED s-expr line back to
   the original Adder source line (als_map, from adr.h). NULL for plain s-expr
   input. With it set, g_reader_srctext points at the Adder source and the
   reader's line numbers are translated through the map before display. */
static ALCOVE_TLS als_map *g_adder_map = NULL;
/* Translate a reader (generated) line to the line to DISPLAY: the Adder source
   line when a map is active, else the reader line unchanged. */
static int display_line(int gen_line) {
  if (g_adder_map) {
    int a = als_map_lookup(g_adder_map, gen_line);
    return a > 0 ? a : gen_line;
  }
  return gen_line;
}
/* Module system (see requirecmd / nscmd):
   g_reader_dir  — directory of the file currently being loaded, so `require`
                   can resolve a sibling module relative to the requirer first.
                   NULL for interactive/stdin. Saved/restored across nested
   loads. g_current_ns  — the active namespace set by (ns name); while non-NULL
   the def* forms auto-qualify a GLOBAL binding's name to ns/name. Reset to NULL
   at the start of each loaded file (a file has no namespace until it declares
   one) and restored afterward. */
static ALCOVE_TLS char *g_reader_dir = NULL;
static ALCOVE_TLS char *g_current_ns = NULL;
/* Set to 1 when a top-level form in a SCRIPT (a file argument or -e) fails to
   parse or evaluate, so main() can exit non-zero — a runner/CI then detects the
   failure instead of seeing the old always-0 status. The interactive REPL and
   piped-stdin (REPL-mode) paths never set it: there you fix-and-continue. */
static int g_script_error = 0;
/* The namespace the most-recently-loaded file declared (its final (ns ...), or
   NULL if none) — eval_file_forms stamps this just before restoring
   g_current_ns, so `require` can read it after loading to drive :refer. */
static ALCOVE_TLS char *g_last_module_ns = NULL;

/* Binding name for a def* form: a freshly-malloc'd "<ns>/<bare>" when a
   namespace is active AND this is a GLOBAL definition (root env) AND bare isn't
   already qualified; otherwise `bare` itself, borrowed. Caller frees the result
   IFF it differs from the bare pointer it passed in (i.e. only when actually
   qualified). Restricting to the root env keeps defs nested in a let/function
   during load un-namespaced (they're locals). With no (ns ...) active this
   returns the borrowed name and allocates nothing, so the common definition
   path is exactly as cheap as before and the test suites are unchanged. */
static char *ns_qualify(const char *bare, env_t *env) {
  if (g_current_ns && env && env->root == NULL && !strchr(bare, '/')) {
    char *q = NULL;
    if (asprintf(&q, "%s/%s", g_current_ns, bare) >= 0 && q)
      return q;
  }
  return (char *)bare; /* borrowed — caller must NOT free (== input pointer) */
}
/* Start line of the top-level form currently being read/evaluated. A driver
   arms g_form_line_arm before calling reader(); reader() stamps g_form_line
   with g_reader_line at the first SIGNIFICANT byte of the form (after any
   leading whitespace/newlines/comments) and disarms. This makes the line the
   form actually begins on, not the line the previous form ended on — so a
   form preceded by blank/comment lines reports correctly. */
static ALCOVE_TLS int g_form_line = 1;
static ALCOVE_TLS int g_form_line_arm = 0;
/* Column (1-based) of the current top-level form's first significant byte —
   stamped together with g_form_line; drives the caret renderer (Alcove). */
static ALCOVE_TLS int g_form_col = 1;

/* getc/ungetc wrappers that keep g_reader_line in sync. RGETC bumps the
   line on a consumed newline; RUNGETC decrements when a newline is pushed
   back so the count stays consistent with the stream position. These wrap
   the reader's raw getc(stream) sites. */
static inline int reader_getc(FILE *s) {
  int c = getc(s);
  if (c != EOF) {
    g_reader_off++;
    if (c == '\n') {
      g_reader_line++;
      g_reader_col = 1;
    } else
      g_reader_col++;
  }
  return c;
}
static inline int reader_ungetc(int c, FILE *s) {
  if (c != EOF) {
    if (g_reader_off > 0)
      g_reader_off--;
    if (c == '\n') {
      if (g_reader_line > 1)
        g_reader_line--;
      /* column of the prior line's end isn't tracked; the next getc re-reads
         this newline and resets col=1, so any read in between is at worst off
         by a column on the caret — never out of bounds. */
    } else if (g_reader_col > 1)
      g_reader_col--;
  }
  return ungetc(c, s);
}
#define RGETC(s) reader_getc(s)
#define RUNGETC(c, s) reader_ungetc((c), (s))

/* The s-expression reader/tokenizer lives in a dedicated #included
   fragment (single TU — keeps the per-byte loop inlinable). */
#include "reader.c"

/* Prepend "<src>:<line>: " to an error's message, in place, when evaluating
   a FILE (g_reader_src != NULL). Used by the file drivers so a top-level form
   that errors — or a syntax error from the reader itself — reports where it
   came from. The error's flags/errnum and id (->next) are left untouched, so
   error?/error-message/try and the parsing-EOF sentinel all keep working; we
   only rewrite the message string. No-op for non-errors, when src is NULL, or
   if the message already begins with "<src>:" (so we never double-prefix). */
static exp_t *annotate_error_loc(exp_t *err, const char *src, int line) {
  if (!err || !iserror(err) || !src)
    return err;
  const char *msg = (const char *)err->ptr;
  if (!msg)
    return err;
  size_t srclen = strlen(src);
  if (strncmp(msg, src, srclen) == 0 && msg[srclen] == ':')
    return err; /* already annotated — don't stack prefixes */
  char *combined = NULL;
  if (asprintf(&combined, "%s:%d: %s", src, line, msg) >= 0 && combined) {
    free(err->ptr);
    err->ptr = combined;
  }
  return err;
}

/* Print the offending source line and a caret under column `col` (1-based),
   gcc/clang-style, beneath an already-printed error message. No-op (safe) when
   there is no retained source text, the line/col are out of range, or the byte
   span can't be located — so callers can fire it unconditionally. The caret
   padding mirrors leading tabs in the source so it lines up under tab indents.
   `srctext`/`len` is the in-memory source; `line`/`col` are 1-based. */
static void render_error_caret(FILE *out, const char *srctext, size_t len,
                               int line, int col) {
  if (!srctext || line < 1)
    return; /* col < 1 means "auto" — caret under the first non-blank char */
  /* Walk to the start of the target line. */
  size_t i = 0;
  int curline = 1;
  while (i < len && curline < line) {
    if (srctext[i] == '\n')
      curline++;
    i++;
  }
  if (curline != line)
    return; /* line beyond EOF — give up quietly */
  size_t ls = i;
  size_t le = ls;
  while (le < len && srctext[le] != '\n')
    le++;
  /* Drop a trailing CR so a CRLF file doesn't print a stray ^M. */
  size_t vis = le;
  if (vis > ls && srctext[vis - 1] == '\r')
    vis--;
  int linelen = (int)(vis - ls);
  if (col < 1) {
    /* auto: caret under the first non-whitespace char (used for Adder, whose
       indentation doesn't map to a generated-s-expr column). */
    col = 1;
    while (col - 1 < linelen &&
           (srctext[ls + col - 1] == ' ' || srctext[ls + col - 1] == '\t'))
      col++;
  }
  if (col - 1 > linelen)
    col = linelen + 1; /* clamp caret to just past the line */
  fprintf(out, "  %.*s\n  ", linelen, srctext + ls);
  for (int c = 0; c < col - 1; c++)
    fputc((c < linelen && srctext[ls + c] == '\t') ? '\t' : ' ', out);
  fprintf(out, "\x1B[91m^\x1B[39m\n");
}

/* Render the caret for a reader (generated) position, translating to the Adder
   source line when a map is active (where the generated column is meaningless,
   so the caret auto-targets the line's first non-blank char). */
static void render_form_caret(FILE *out, int gen_line, int gen_col) {
  if (g_adder_map)
    render_error_caret(out, g_reader_srctext, g_reader_srctext_len,
                       display_line(gen_line), 0 /* auto column */);
  else
    render_error_caret(out, g_reader_srctext, g_reader_srctext_len, gen_line,
                       gen_col);
}

/* Print the call backtrace captured at the error site (most recent call first),
   then clear it. Frame 0 is the outermost call, frame n-1 the innermost. Does
   nothing if no frames were on the stack (a top-level error). */
static void render_backtrace(FILE *out) {
  int n = g_error_bt_n;
  if (n <= 0) { /* 0 = none captured, -1 = captured-but-empty */
    bt_clear();
    return;
  }
  fprintf(out, "  backtrace (most recent call first):\n");
  for (int i = n - 1; i >= 0; i--)
    fprintf(out, "    %s\n", g_error_bt[i] ? g_error_bt[i] : "<anonymous>");
  if (g_error_bt_more > 0)
    fprintf(out, "    … (%d more frame%s)\n", g_error_bt_more,
            g_error_bt_more == 1 ? "" : "s");
  bt_clear();
}

// Syntactic sugar causes cancer of the semicolon. — Alan Perlis
// istrue borrow object reference
inline int istrue(exp_t *e) {
  if (!e)
    return 0;
  /* Tagged immediates first — cheap tag check, no deref. */
  if (isnumber(e))
    return FIX_VAL(e) != 0;
  if (ischar(e))
    return 0; /* preserve historical behavior */
  if (!is_ptr(e))
    return 0;
  /* Heap object — dispatch on type. The enum is contiguous, so this lowers
     to a jump table: the hot nil/pair case lands directly instead of after a
     dozen sequential `e->type ==` comparisons. Function-like values (lambda /
     builtin / macro / ffi / continuation) are always truthy — a callable is a
     real value, not "empty". Containers are truthy when non-empty. default
     covers immediates-as-heap (shouldn't occur), booleans, errors, and the
     internal tree/circular markers, all falsy as before. */
  switch (e->type) {
  case EXP_PAIR:
    return (e->content || e->next) ? 1 : 0; /* nil = empty pair -> 0 */
  case EXP_SYMBOL:
    return strcmp(exp_text(e), "nil") != 0;
  case EXP_STRING: {
    const char *t = exp_text(e);
    return t ? (*t != '\0') : 0;
  }
  case EXP_FLOAT:
    return e->f != 0;
  case EXP_VECTOR:
    return (e->ptr && vec_len(e) > 0);
  case EXP_BLOB:
    return (e->ptr && ((alc_blob_t *)e->ptr)->len > 0);
  case EXP_LIST:
    return (e->ptr && ((alc_list_t *)e->ptr)->len > 0);
  case EXP_SET:
  case EXP_DICT: {
    dict_t *d = (dict_t *)e->ptr;
    return (d && d->ht[0].used > 0);
  }
  case EXP_HAMT:
    return (e->ptr && ((hamt_t *)e->ptr)->count > 0);
  case EXP_LAMBDA:
  case EXP_INTERNAL:
  case EXP_MACRO:
  case EXO_MACROINTERNAL:
  case EXP_FFI:
  case EXP_CONT:
  case EXP_PORT:
    return 1;
  default:
    return 0;
  }
}

inline exp_t *lookup(exp_t *e, env_t *env) {
  keyval_t *ret;
  env_t *curenv = env;

  /* Cache fast path: symbols previously resolved into reserved_symbol
     (builtins like +, -, if, <, etc.) skip the hash lookup. On a
     symbol, meta != NULL uniquely means "cached resolution" — the
     lambda/macro meta strings only exist on their own exp_t types.
     Inlining keeps the cache: the text is in the `ptr` union, `meta`
     is untouched, so a short builtin symbol caches just like a long one.
     Read the key only after the cache miss (a hit needs no text). */
  if (e->meta) {
    return refexp((exp_t *)e->meta);
  }

  char *key = exp_text(e);
  if ((ret = set_get_keyval_dict(reserved_symbol, key, NULL))) {
    e->meta = (struct keyval_t *)ret->val;
    return refexp(ret->val);
  } else {
    if (curenv)
      do {
        /* Fast path: scan inline function-param slots first. For the
           common case (1-6 params) this beats a full hash lookup.
           Innermost-first (high idx → low): a compiled multi-let env holds
           several bindings in one env, and an inner `(let x …)` (higher slot)
           must shadow an outer one / a param of the same name. AST envs hold
           one binding per name, so direction is immaterial there. */
        int i;
        for (i = curenv->n_inline - 1; i >= 0; i--) {
          const char *k = curenv->inline_keys[i];
          if (k && strcmp(k, key) == 0)
            return refexp(curenv->inline_vals[i]);
        }
        if ((curenv->d) && (ret = set_get_keyval_dict(curenv->d, key, NULL)))
          return refexp(ret->val);
      } while ((curenv = curenv->root));
  }
  return NULL;
}

/* Scope-aware lookup used by the OP_LOAD_GLOBAL / OP_CALL_GLOBAL gcache
   sites. Identical resolution to lookup(), but reports via *global whether
   the binding came from a truly global scope (reserved builtins or the root
   env, *global=1) versus a local function/let env (*global=0).
   Only global-scope resolutions are safe to memoize in the per-bytecode
   gcache: that cache is keyed solely by alcove_global_gen and is invalidated
   only on GLOBAL mutations, NOT on per-call local-env changes. A symbol that
   is created/assigned in a local env via OP_STORE_FREE (e.g. `(= tmp ...)`
   for a non-parameter inside a function body) compiles to OP_LOAD_GLOBAL on
   read; caching that local value would serve a stale binding to a later call
   with a different env at the same gen. Manifested as "vec-ref: bad args"
   when the stale value fed a vec index (heap-layout dependent: benign on
   x86-64, corrupting on wasm). */
static exp_t *lookup_scoped(exp_t *e, env_t *env, int *global) {
  keyval_t *ret;
  env_t *curenv = env;
  *global = 1; /* reserved + root hits are global; demoted below for locals */
  if (e->meta)
    return refexp((exp_t *)e->meta);
  char *key = exp_text(e);
  if ((ret = set_get_keyval_dict(reserved_symbol, key, NULL))) {
    e->meta = (struct keyval_t *)ret->val;
    return refexp(ret->val);
  }
  if (curenv)
    do {
      int is_root = (curenv->root == NULL);
      int i;
      for (i = 0; i < curenv->n_inline; i++) {
        const char *k = curenv->inline_keys[i];
        if (k && strcmp(k, key) == 0) {
          *global = is_root;
          return refexp(curenv->inline_vals[i]);
        }
      }
      if ((curenv->d) && (ret = set_get_keyval_dict(curenv->d, key, NULL))) {
        *global = is_root;
        return refexp(ret->val);
      }
    } while ((curenv = curenv->root));
  *global = 0;
  return NULL;
}
exp_t *updatebang(exp_t *keyv, env_t *env, exp_t *val) {
  keyval_t *ret = NULL;
  exp_t *fret = NULL;
  exp_t *key = NULL;
  if (val == NULL)
    val = NIL_EXP;
  /* (= count 5) where count is a builtin: lookup would never return this
     binding (reserved symbols resolve first), so reject it like let/param. */
  exp_t *_rerr = NULL;
  REJECT_RESERVED_ASSIGN(keyv, _rerr, {
    unrefexp(keyv);
    unrefexp(val);
    return _rerr;
  });
  if (issymbol(keyv) || isstring(keyv)) { // form (= "qweqwe" 10) (= weq 10)
    if (islambda(val) && val->meta == NULL)
      val->meta = (keyval_t *)strdup(exp_text(keyv));
    /* Walk env chain inner→outer looking for an existing binding;
       mutate it in place if found. This is what makes mutable
       closures work — `(= n ...)` inside (fn (...) ...) finds the
       captured n in the closure's env, instead of silently creating
       a fresh local that shadows it. The walk also handles the local
       case (let-bound slots in current env) correctly. */
    {
      env_t *cur = env;
      while (cur) {
        int i;
        for (i = 0; i < cur->n_inline; i++) {
          const char *k = cur->inline_keys[i];
          if (k && strcmp(k, exp_text(keyv)) == 0) {
            unrefexp(cur->inline_vals[i]);
            cur->inline_vals[i] = refexp(val);
            unrefexp(keyv);
            return val;
          }
        }
        if (cur->d) {
          keyval_t *kv = set_get_keyval_dict(cur->d, exp_text(keyv), NULL);
          if (kv) {
            /* Refuse mutating a GLOBAL binding (root env) from a concurrent
               RESP callback. Local/closure bindings (cur->root != NULL) are
               per-invocation env and stay writable. */
            if (g_resp_cb_guard && cur->root == NULL) {
              unrefexp(keyv);
              unrefexp(val);
              return resp_cb_readonly_error(env);
            }
            /* Bump gen BEFORE the unref. The reverse order is a TOCTOU:
               under threading (or even under a JIT callout that re-enters
               and reads bc->gcache while we're between the unref and the
               bump), the cache reader could see a still-matching gen
               pointing at a freed value, then refexp the freed pointer.
               alcove is single-threaded today; this is hardening for the
               documented future-threading goal. */
            GEN_BUMP();
            unrefexp(kv->val);
            kv->val = refexp(val);
            unrefexp(keyv);
            return val;
          }
        }
        cur = cur->root;
      }
    }
    /* No existing binding anywhere — create in current env. When env is the
       global env (no root), this is a global write: refuse under the RESP
       callback guard. */
    if (g_resp_cb_guard && env->root == NULL) {
      unrefexp(keyv);
      unrefexp(val);
      return resp_cb_readonly_error(env);
    }
    if (!(env->d))
      env->d = create_dict();
    ret = set_get_keyval_dict(env->d, exp_text(keyv), val);
    GEN_BUMP();
    unrefexp(keyv);
    return val;
  } else if (ispair(keyv)) { /*evaluate(keyv,env)=val*/
    key = car(keyv);
    if (key && issymbol(key)) {
      if (strcmp(exp_text(key), "car") == 0) // form (= (car x) 'z)
      {
        key = EVAL(cadr(keyv), env);
        if iserror (key) {
          unrefexp(keyv);
          unrefexp(val);
          return key;
        }
        unrefexp(key->content);
        key->content = refexp(val);
        unrefexp(key);
        goto finish;

      } else if (strcmp(exp_text(key), "cdr") == 0) {
        key = EVAL(cadr(keyv), env);
        if iserror (key) {
          unrefexp(keyv);
          unrefexp(val);
          return key;
        }
        unrefexp(key->next);
        key->next = refexp(val);
        unrefexp(key);
        goto finish;
      } else {
        /* (= (str i) char) — write char into string at i. The index
           must be EVAL'd, not read off the AST: the car/cdr branches
           above already do this; the string branch was a long-standing
           gap that silently no-op'd whenever i wasn't a literal. */
        exp_t *idx = NULL;
        key = EVAL(key, env);
        if (isstring(key)) {
          idx = EVAL(cadr(keyv), env);
          if iserror (idx) {
            unrefexp(key);
            unrefexp(keyv);
            unrefexp(val);
            return idx;
          }
          if (idx && isnumber(idx) && ischar(val)) {
            const char *cur = exp_text(key);
            int64_t cpi = FIX_VAL(idx);
            if ((cpi >= 0) && (cpi < utf8_strlen(cur))) {
              /* Replace codepoint cpi with val, rebuilding the byte buffer
                 since the new codepoint may differ in width from the old. */
              size_t a = utf8_byte_offset(cur, cpi);
              size_t aend = utf8_byte_offset(cur, cpi + 1);
              size_t total = strlen(cur);
              char enc[4];
              int k = utf8_encode((uint32_t)CHAR_VAL(val), enc);
              size_t newlen = a + (size_t)k + (total - aend);
              char *nb = memalloc(newlen + 1, 1);
              memcpy(nb, cur, a);
              memcpy(nb + a, enc, (size_t)k);
              memcpy(nb + a + (size_t)k, cur + aend, total - aend);
              nb[newlen] = '\0';
              exp_set_text(key, nb, newlen);
              free(nb);
              if (idx)
                unrefexp(idx);
              unrefexp(key);
              goto finish;
            } else {
              fret = error(ERROR_INDEX_OUT_OF_RANGE, keyv, env,
                           "Error index out of range");
            }
          } else {
            fret = error(ERROR_NUMBER_EXPECTED, keyv, env,
                         "Error number and char expected");
          }
          if (idx)
            unrefexp(idx);
          unrefexp(key);
          unrefexp(keyv);
          unrefexp(val);
          return fret;
        } else {
          unrefexp(key);
          unrefexp(val);
          return NULL; // SHOULD BE ERROR
        }
      }
    }

  } else {
    fret = error(EXP_ERROR_INVALID_KEY_UPDATE, keyv, env,
                 "Error invalid key in =");
    unrefexp(val);
    unrefexp(keyv);
    return fret;
  }
  if (ret)
    return ret->val;
  else
    return NULL; /* ERROR? */
finish:
  unrefexp(keyv);
  return val;
}

/* True if `name` is a reserved builtin / special-form name. Binding a
   variable with such a name is rejected: lookup() resolves reserved
   symbols BEFORE the lexical env, so the binding would be silently
   shadowed by the builtin (the classic make_counter-with-`count` footgun).
   Fail fast at definition/eval time with a clear message instead. */
static int is_reserved_name(const char *name) {
  return name && reserved_symbol &&
         set_get_keyval_dict(reserved_symbol, (char *)name, NULL) != NULL;
}

/* Return the first reserved name used in a param spec, or NULL. Handles a
   flat list (a b c), a bare rest-only symbol (fn xs ...), the dotted rest
   marker (a . rest), and nested destructuring patterns ((x y) z). */
static const char *reserved_param_name(exp_t *params) {
  if (issymbol(params)) {
    const char *nm = (const char *)exp_text(params);
    return (strcmp(nm, ".") != 0 && is_reserved_name(nm)) ? nm : NULL;
  }
  for (exp_t *p = params; p && ispair(p) && istrue(p); p = p->next) {
    exp_t *el = p->content;
    if (issymbol(el)) {
      const char *nm = (const char *)exp_text(el);
      if (strcmp(nm, ".") != 0 && is_reserved_name(nm))
        return nm;
    } else if (ispair(el) && istrue(el)) {
      if (issymbol(car(el)) && cadr(el) && !issymbol(cadr(el))) {
        /* (name default-expr) optional param: only the name binds; the
           default is an expression, so don't scan it for reserved names. */
        const char *nm = (const char *)exp_text(car(el));
        if (strcmp(nm, ".") != 0 && is_reserved_name(nm))
          return nm;
      } else {
        const char *r = reserved_param_name(el); /* nested destructuring */
        if (r)
          return r;
      }
    }
  }
  return NULL;
}

/* Reject binding a reserved name. PARAMS is a param spec (a bare symbol or
   a [possibly destructuring] list). On violation, build the error into the
   lvalue ERRLV, then run FAIL (e.g. `goto finish` or
   `{ unrefexp(e); return ERRLV; }`). CTX is appended to the message.
   Used across def, fn, let, let*, with, and for so the check is in one
   place. */
#define CHECK_RESERVED_BIND(PARAMS, ERRLV, CTX, FAIL)                          \
  do {                                                                         \
    const char *_rsv = reserved_param_name(PARAMS);                            \
    if (_rsv) {                                                                \
      (ERRLV) = error(ERROR_ILLEGAL_VALUE, NULL, env,                          \
                      "cannot bind reserved name '%s' " CTX, _rsv);            \
      FAIL;                                                                    \
    }                                                                          \
  } while (0)

/* ---- optional type annotations (JIT speculation hints) ----
   Surface: a trailing keyword annotates the preceding parameter, and a keyword
   right after the param list (followed by a body) is the return type:
     (def dot (a :vec-f64 b :vec-f64) :f64 ...)
   Hints feed the JIT (Phase 2); they never change semantics — a value that
   doesn't match its hint at runtime just de-opts to the VM. Unknown type
   keywords are a hard error at definition (catches typos). Phase 1 validates
   the hints and strips them so var2env / compile_lambda / arity see plain
   params and bodies; Phase 2 will record them for the JIT instead of
   discarding. The vocabulary is C-like: :int (fixnum), :f64 (double), :vec-f64,
   :vec-i64. The TYPE_HINT_* enum lives in alcove.h (used by bytecode_t /
   compile_lambda). */
/* Name of a TYPE_HINT_* code, for disasm / introspection. */
static const char *type_hint_name(int code) { /* expanded from ALC_TYPE_HINTS */
  switch (code) {
#define X(c, kw)                                                               \
  case c:                                                                      \
    return kw;
    ALC_TYPE_HINTS(X)
#undef X
  default:
    return ":any";
  }
}
/* Map a type-hint keyword's text to its code, or -1 if not a known type. */
static int type_hint_code(const char *kw) { /* expanded from ALC_TYPE_HINTS */
#define X(c, ks)                                                               \
  if (!strcmp(kw, ks))                                                         \
    return c;
  ALC_TYPE_HINTS(X)
#undef X
  return -1;
}
/* A keyword is an EXP_SYMBOL whose text starts with ':'. */
static int is_keyword_exp(exp_t *x) {
  return is_ptr(x) && issymbol(x) && ((char *)exp_text(x))[0] == ':';
}
/* True if a param list carries any trailing-keyword hint (top-level keyword).
   Only a real list (pair chain) can; a bare-symbol rest param (fn xs ...) or
   empty () cannot, and must NOT be walked as a node chain. */
static int params_have_hint(exp_t *params) {
  if (!ispair(params))
    return 0;
  for (exp_t *p = params; p && p->content; p = p->next)
    if (is_keyword_exp(p->content))
      return 1;
  return 0;
}
/* Validate the hints in `params` and return a fresh param list with them
   stripped. No hints → the list is returned unchanged (refexp'd), so the
   common case allocates nothing extra. On a bad hint (unknown type, or a
   keyword not following a parameter) sets *errp and returns NULL. When
   hints_out is non-NULL it is filled (zeroed first) with the per-parameter
   TYPE_HINT_* code, indexed by the cleaned param position, for the JIT. */
static exp_t *build_clean_params(exp_t *params, exp_t *form, env_t *env,
                                 exp_t **errp, uint8_t *hints_out) {
  *errp = NULL;
  if (hints_out)
    memset(hints_out, 0, ENV_INLINE_SLOTS);
  /* Shape check (def/fn/defn all reach here for list-form params): every
     top-level parameter must be a symbol — a name, '.', or a :type-hint
     keyword — or a pair: (name default) / a destructuring pattern. A bare
     non-symbol atom is malformed, e.g. the flat `(x 10)` someone writes
     meaning the default `((x 10))`. Caught here at definition rather than
     silently swallowing an argument at the first call. A bare-symbol param
     list (rest-only, `fn xs`) is handled by params_have_hint's !ispair path
     below and never walked here. */
  if (ispair(params)) {
    for (exp_t *p = params; p && ispair(p) && istrue(p); p = p->next) {
      exp_t *el = p->content;
      if (is_ptr(el) && (issymbol(el) || (ispair(el) && istrue(el))))
        continue;
      *errp = error(ERROR_ILLEGAL_VALUE, form, env,
                    "illegal parameter: a parameter must be a symbol, a "
                    "(name default) pair, or a destructuring pattern (did "
                    "you mean ((x 10)) to give a default value?)");
      return NULL;
    }
  }
  if (!params_have_hint(params))
    return refexp(params);
  exp_t *head = NULL, *tail = NULL;
  int prev_bindable = 0; /* did the previous kept node take a hint slot? */
  int kept = -1;         /* index of the last kept param (for hints_out) */
  for (exp_t *p = params; p && p->content; p = p->next) {
    exp_t *c = p->content;
    if (is_keyword_exp(c)) {
      int code = type_hint_code((char *)exp_text(c));
      if (code < 0)
        *errp = error(ERROR_ILLEGAL_VALUE, form, env,
                      "unknown type hint '%s' (expected :int :f64 :vec-f64 "
                      ":vec-i64)",
                      (char *)exp_text(c));
      else if (!prev_bindable)
        *errp = error(ERROR_ILLEGAL_VALUE, form, env,
                      "type hint '%s' must follow a parameter",
                      (char *)exp_text(c));
      if (*errp) {
        if (head)
          unrefexp(head);
        return NULL;
      }
      if (hints_out && kept >= 0 && kept < ENV_INLINE_SLOTS)
        hints_out[kept] = (uint8_t)code;
      prev_bindable = 0; /* at most one hint per parameter */
      continue;          /* strip the hint */
    }
    exp_t *node = make_node(refexp(c));
    if (tail)
      tail = tail->next = node;
    else
      head = tail = node;
    kept++;
    prev_bindable = 1;
  }
  return head ? head : refexp(NIL_EXP);
}
/* If `body` begins with a return-type keyword followed by more forms, validate
   it and return the body with that keyword skipped (writing its TYPE_HINT_*
   code to *ret_out); else return body unchanged with *ret_out = 0. On an
   unknown return-type keyword sets *errp and returns NULL. */
static exp_t *strip_return_hint(exp_t *body, exp_t *form, env_t *env,
                                exp_t **errp, uint8_t *ret_out) {
  *errp = NULL;
  if (ret_out)
    *ret_out = TYPE_HINT_NONE;
  if (body && is_keyword_exp(car(body)) && cdr(body)) {
    int code = type_hint_code((char *)exp_text(car(body)));
    if (code < 0) {
      *errp = error(ERROR_ILLEGAL_VALUE, form, env,
                    "unknown return-type hint '%s' (expected :int :f64 "
                    ":vec-f64 :vec-i64)",
                    (char *)exp_text(car(body)));
      return NULL;
    }
    if (ret_out)
      *ret_out = (uint8_t)code;
    return cdr(body); /* skip the return-type keyword */
  }
  return body;
}

/* Lambdas here are NOT closures: the returned EXP_LAMBDA stores only
   params + body, with no reference to the defining env. Free variables
   are resolved dynamically against the CALLER's env chain at invoke
   time. If closure semantics are ever added, the env arena and the
   bytecode VM's lifetime assumptions both need revisiting. */
const char doc_fn[] = "(fn (params...) body...) — anonymous function. Body is "
                      "a sequence; the last expression's value is returned.";
exp_t *fncmd(exp_t *e, env_t *env) {
  exp_t *val;
  exp_t *vali;
  exp_t *header;
  exp_t *body;
  exp_t *cur = cdr(e);
  /* Accept list params OR bare symbol for rest-only: (fn xs body) */
  if (cur && (ispair(cur->content) || issymbol(cur->content))) {
    header = car(cur);
    cur = cdr(cur);
    CHECK_RESERVED_BIND(header, val, "as a parameter", {
      unrefexp(e);
      return val;
    });
    if (cur) {
      /* Type annotations: validate + strip param hints + an optional
         return-type keyword, exactly as def does; record them for the JIT. */
      uint8_t phints[ENV_INLINE_SLOTS];
      uint8_t rhint = TYPE_HINT_NONE;
      exp_t *herr = NULL;
      exp_t *clean_params = build_clean_params(header, e, env, &herr, phints);
      if (herr) {
        unrefexp(e);
        return herr;
      }
      cur = strip_return_hint(cur, e, env, &herr, &rhint);
      if (herr) {
        unrefexp(clean_params);
        unrefexp(e);
        return herr;
      }
      /* Body is the remaining list; first form may be nil/literal/symbol
         as well as a pair — all are legal body expressions. */
      body = cur;
      vali = make_node(refexp(body));
      if (issymbol(header)) {
        exp_t *dot = make_node(make_symbol(".", 1));
        dot->next = make_node(clean_params);
        val = make_node(dot);
      } else {
        val = make_node(clean_params);
      }
      val->next = vali;
      val->type = EXP_LAMBDA;
      /* Closure: stash the env at fn-creation time in the wrapper
         node's `meta` field (see comment in unrefexp). invoke() later
         uses this as the new call env's root, so let/with bindings
         from the enclosing scope resolve correctly. For top-level fns
         env is global, so the capture is just an extra ref on global
         (cheap). */
      if (env) {
        val->next->meta = (struct keyval_t *)ref_env(env);
        env->has_closure = 1;
      }
      /* Compile both top-level fns and closures. Closures (a real captured
         scope, env->root != NULL) compile with no_gcache so free-var reads
         always re-resolve against the captured env (a closure that *mutates*
         a free var can't slot-resolve it and safely falls back to AST). */
      compile_lambda(val, env && env->root, phints, rhint);
    } else
      val = error(EXP_ERROR_BODY_NOT_LIST, e, env, "Error body is not a list");
  } else
    val = error(EXP_ERROR_PARAM_NOT_LIST, e, env, "Error params is not a list");
  unrefexp(e);
  return val;
}

const char doc_def[] =
    "(def name (params...) body...) — define a named function. Body that uses "
    "only supported forms compiles to bytecode (sometimes JIT'd).";
exp_t *defcmd(exp_t *e, env_t *env) {
  /* def installs a global binding — refuse from a concurrent RESP callback. */
  if (g_resp_cb_guard) {
    unrefexp(e);
    return resp_cb_readonly_error(env);
  }
  exp_t *val;
  exp_t *vali;
  exp_t *name;
  exp_t *header;
  exp_t *body;
  exp_t *cur = cdr(e);
  if (cur && issymbol(cur->content)) {
    name = car(cur);
    cur = cdr(cur);
    /* Accept (params...) list OR bare symbol for rest-only: (def f xs body) */
    if (cur && (ispair(cur->content) || issymbol(cur->content))) {
      header = car(cur);
      cur = cdr(cur);
      CHECK_RESERVED_BIND(header, val, "as a parameter", {
        unrefexp(e);
        return val;
      });
      if (cur) {
        /* Type annotations: validate + strip the param hints and an optional
           return-type keyword (recording them for the JIT). `clean_params` is
           owned; everything downstream sees plain params/body. */
        uint8_t phints[ENV_INLINE_SLOTS];
        uint8_t rhint = TYPE_HINT_NONE;
        exp_t *herr = NULL;
        exp_t *clean_params = build_clean_params(header, e, env, &herr, phints);
        if (herr) {
          unrefexp(e);
          return herr;
        }
        cur = strip_return_hint(cur, e, env, &herr, &rhint);
        if (herr) {
          unrefexp(clean_params);
          unrefexp(e);
          return herr;
        }
        /* (ns ...)-aware binding name: foo/<name> for a top-level def, else
           plain. Used for the docstring key, the lambda self-name, and the
           global binding so a qualified self-call still gets self-tail TCO. */
        char *qname = ns_qualify(exp_text(name), env);
        /* Docstring: (def f (args) "..." body...) — a leading string that
           is FOLLOWED by more forms is documentation, not the body. A lone
           string body (def f () "hi") stays the return value. Stored by
           name for the `doc` builtin. */
        if (isstring(car(cur)) && cdr(cur)) {
          if (!user_doc)
            user_doc = create_dict();
          set_get_keyval_dict(user_doc, qname, car(cur));
          cur = cdr(cur);
        }
        /* Body is the remaining list; first form may be nil/literal/symbol
           as well as a pair — all are legal body expressions. */
        body = cur;
        vali = make_node(refexp(body));
        /* For bare-symbol params, wrap in a 1-element list so lambda_params
           always returns a list. We mark this as a rest-param lambda by
           storing the symbol directly as the sole param wrapped in a pair
           whose first element is itself a symbol — compile_lambda will see
           FLAG_REST and skip compilation, falling back to AST eval where
           var2env handles the rest collection. */
        if (issymbol(header)) {
          /* Bare-symbol params: represent as (. sym) so var2env collects
             all args into a list bound to sym. */
          exp_t *dot = make_node(make_symbol(".", 1));
          dot->next = make_node(clean_params);
          val = make_node(dot);
        } else {
          val = make_node(clean_params);
        }
        val->next = vali;
        val->type = EXP_LAMBDA;
        val->meta = (keyval_t *)strdup(qname);
        /* Closure: capture defining env (see fncmd for rationale). */
        if (env) {
          val->next->meta = (struct keyval_t *)ref_env(env);
          env->has_closure = 1;
        }
        /* Compile top-level defs and nested (closure) defs alike; closures
           get no_gcache (fresh free-var lookups against the captured env). */
        compile_lambda(val, env && env->root, phints, rhint);
        if (!(env->d))
          env->d = create_dict();
        set_get_keyval_dict(env->d, qname,
                            val);    /* return value (the kv) unused */
        if (qname != exp_text(name)) /* ns_qualify allocated iff qualified */
          free(qname);
        GEN_BUMP(); /* invalidate bytecode global-resolution caches */
      } else
        val =
            error(EXP_ERROR_BODY_NOT_LIST, e, env, "Error body is not a list");
    } else
      val =
          error(EXP_ERROR_PARAM_NOT_LIST, e, env, "Error params is not a list");
  } else
    val = error(EXP_ERROR_MISSING_NAME, e, env,
                "Error missing name or name not a lambda");
  unrefexp(e);
  return val;
}

const char doc_defn[] =
    "(defn name CLAUSE...) — multi-arity function; a call dispatches on the "
    "argument count to the matching clause (a variadic . rest clause catches "
    "any count >= its fixed arity; no match raises). A clause is either "
    "((params) body...) or leading-symbol form (a b body...) where the leading "
    "symbols are the params and the first non-symbol begins the body — e.g. "
    "(defn f ((x) (* x x)) (w h (* w h))).";
/* Multi-arity define. Builds one ordinary closure per clause and stores them
   in a FLAG_MULTI wrapper lambda (content = the clause-lambda list); the call
   paths intercept FLAG_MULTI and dispatch by arity to the right clause, reusing
   invoke / vm_invoke_values for the actual bind+run. */
exp_t *defncmd(exp_t *e, env_t *env) {
  exp_t *val;
  exp_t *cur = cdr(e);
  if (!cur || !issymbol(cur->content)) {
    val = error(EXP_ERROR_MISSING_NAME, e, env, "defn: missing function name");
    unrefexp(e);
    return val;
  }
  exp_t *name = car(cur);
  CHECK_RESERVED_BIND(name, val, "as a function name", {
    unrefexp(e);
    return val;
  });
  cur = cdr(cur);
  if (!cur) {
    val = error(ERROR_MISSING_PARAMETER, e, env,
                "defn: needs at least one (params body...) clause");
    unrefexp(e);
    return val;
  }
  /* Build the clause-lambda list. A clause is accepted in two shapes:
       explicit param list:   ((r) BODY...)   -> params (r),  body BODY
       leading-symbol params:  (r BODY...)     -> params (r),  body BODY
                               (w h BODY...)   -> params (w h),body BODY
     The leading-symbol shape is what the Adder block `r:` / `w h:` produces
     (the `:` block appends the body next to the head symbols). It is read as
     "leading symbols are the params; the first non-symbol element begins the
     body" — so a clause body that *starts* with a bare symbol needs the
     explicit `()` form. Empty params are written `()` (explicit form). */
  exp_t *clauses_head = NIL_EXP, *clauses_tail = NULL;
  for (exp_t *cl = cur; cl; cl = cl->next) {
    exp_t *clause = cl->content;
    exp_t *params_node = NULL,
          *body_node = NULL; /* each becomes an owned ref */
    uint8_t phints[ENV_INLINE_SLOTS];
    memset(phints, 0, sizeof phints);
    if (is_ptr(clause) && ispair(clause)) {
      exp_t *first = car(clause);
      if (is_ptr(first) && ispair(first)) {
        /* explicit param list — validate/strip type hints */
        exp_t *herr = NULL;
        params_node = build_clean_params(first, e, env, &herr, phints);
        if (herr) {
          unrefexp(clauses_head);
          unrefexp(e);
          return herr;
        }
        if (cdr(clause))
          body_node = refexp(cdr(clause));
      } else if (issymbol(first)) {
        /* leading-symbol params: collect leading symbols into a fresh list,
           the remainder is the body. A trailing keyword annotates (and is
           stripped from) the preceding param — same hints as the list form. */
        exp_t *ph = NIL_EXP, *pt = NULL, *p = clause;
        int prev_bindable = 0, kept = -1;
        while (p && p->content && issymbol(p->content)) {
          if (is_keyword_exp(p->content)) {
            exp_t *herr = NULL;
            int code = type_hint_code((char *)exp_text(p->content));
            if (code < 0)
              herr = error(ERROR_ILLEGAL_VALUE, e, env,
                           "unknown type hint '%s' (expected :int :f64 "
                           ":vec-f64 :vec-i64)",
                           (char *)exp_text(p->content));
            else if (!prev_bindable)
              herr = error(ERROR_ILLEGAL_VALUE, e, env,
                           "type hint '%s' must follow a parameter",
                           (char *)exp_text(p->content));
            if (herr) {
              unrefexp(ph);
              unrefexp(clauses_head);
              unrefexp(e);
              return herr;
            }
            if (kept >= 0 && kept < ENV_INLINE_SLOTS)
              phints[kept] = (uint8_t)code;
            prev_bindable = 0;
            p = p->next;
            continue; /* strip the hint */
          }
          exp_t *pn = make_node(refexp(p->content));
          if (pt)
            pt = pt->next = pn;
          else
            ph = pt = pn;
          kept++;
          prev_bindable = 1;
          p = p->next;
        }
        params_node = ph; /* owned */
        if (p && p->content)
          body_node = refexp(p);
      }
    }
    if (!params_node || !body_node) {
      unrefexp(params_node);
      unrefexp(clauses_head);
      val = error(ERROR_ILLEGAL_VALUE, e, env,
                  "defn: each clause must be (PARAMS BODY...) — params as a "
                  "list ((r) ...) or leading symbols (r ...) — with a "
                  "non-empty body");
      unrefexp(e);
      return val;
    }
    exp_t *L = make_node(params_node); /* content = params */
    L->next = make_node(body_node);    /* next->content = body */
    L->type = EXP_LAMBDA;
    if (env)
      L->next->meta = (struct keyval_t *)ref_env(env); /* closure capture */
    compile_lambda(L, env && env->root, phints, TYPE_HINT_NONE);
    exp_t *node = make_node(L); /* owns L */
    if (clauses_tail)
      clauses_tail = clauses_tail->next = node;
    else
      clauses_head = clauses_tail = node;
  }
  /* Wrapper: an EXP_LAMBDA flagged multi; content holds the clause list. */
  char *qname =
      ns_qualify(exp_text(name), env); /* (ns ...)-aware, see defcmd */
  val = make_node(clauses_head);
  val->type = EXP_LAMBDA;
  val->flags |= FLAG_MULTI;
  val->meta = (struct keyval_t *)strdup(qname);
  if (!(env->d))
    env->d = create_dict();
  set_get_keyval_dict(env->d, qname, val);
  if (qname != exp_text(name)) /* ns_qualify allocated iff qualified */
    free(qname);
  GEN_BUMP();
  unrefexp(e);
  return val;
}

const char doc_macroexpand[] =
    "(macroexpand-1 form) — expand the outermost macro call in form once and "
    "return the resulting code.";
exp_t *expandmacrocmd(exp_t *e, env_t *env) {
  exp_t *tmpexp;
  exp_t *tmpexp2;

  exp_t *form = cadr(cadr(e)); /* the (quoted) form passed in */
  tmpexp = car(form);
  if (tmpexp)
    if (issymbol(tmpexp))
      if ((tmpexp2 = lookup(refexp(tmpexp), env)))
        if ismacro (tmpexp2) {
          tmpexp = expandmacro(refexp(form), tmpexp2, env);
          goto finish;
        }

  /* Not a macro call — return the form unchanged (standard Lisp behavior),
     so macroexpand-1 works as an identity in expand-if-macro loops rather
     than erroring on ordinary forms. */
  tmpexp = form ? refexp(form) : NIL_EXP;
finish:
  unrefexp(e);
  return tmpexp;
}

const char doc_defc[] =
    "(defc name (params...) body...) — define a function whose body is wrapped "
    "in (call/cc (fn (return) ...)). `return` is an escape continuation: "
    "(return v) exits the function immediately with v. Sugar for "
    "(def name (params) (call/cc (fn (return) body...))).";
exp_t *defccmd(exp_t *e, env_t *env) {
  /* e = (defc name params body...). Build the equivalent
       (def name params (call/cc (fn (return) body...)))
     and evaluate it, reusing def / fn / call/cc verbatim — so closure capture,
     compilation, and the escape continuation behave exactly as the
     hand-written form. `return` is intentionally captured into the body
     (anaphoric), giving an imperative-style early return. */
  exp_t *name_node = cdr(e);
  exp_t *params_node = name_node ? name_node->next : NULL;
  exp_t *body = params_node ? params_node->next : NULL;
  if (!name_node || !params_node || !body) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, NULL, env,
                 "(defc name (params) body...)");
  }
  /* (fn (return) body...) — params list is the single symbol `return`. */
  exp_t *fn_form = make_node(make_symbol("fn", 2));
  fn_form->next = make_node(make_node(make_symbol("return", 6)));
  exp_t *tail = fn_form->next;
  for (exp_t *b = body; b; b = b->next) {
    tail->next = make_node(refexp(b->content));
    tail = tail->next;
  }
  /* (call/cc <fn_form>) */
  exp_t *cc_form = make_node(make_symbol("call/cc", 7));
  cc_form->next = make_node(fn_form);
  /* (def name params <cc_form>) — name/params reused (refexp'd); params is
     forwarded as-is, so both (defc f (a b) ...) and rest-form (defc f xs ...)
     work exactly like def. */
  exp_t *def_form = make_node(make_symbol("def", 3));
  def_form->next = make_node(refexp(name_node->content));
  def_form->next->next = make_node(refexp(params_node->content));
  def_form->next->next->next = make_node(cc_form);
  exp_t *ret = EVAL(def_form, env); /* consumes def_form */
  unrefexp(e);
  return ret;
}

const char doc_defmacro[] =
    "(defmacro name (params...) body) — define a macro. Body returns a code "
    "form that replaces the call site at expansion time.";
exp_t *defmacrocmd(exp_t *e, env_t *env) {
  /* defmacro installs a global binding — refuse from a concurrent callback. */
  if (g_resp_cb_guard) {
    unrefexp(e);
    return resp_cb_readonly_error(env);
  }
  exp_t *val;
  exp_t *vali;
  exp_t *name;
  exp_t *header;
  exp_t *body;
  exp_t *cur = cdr(e);
  if (cur && issymbol(cur->content)) {
    name = car(cur);
    cur = cdr(cur);
    if (cur && ispair(cur->content)) {
      header = car(cur);
      cur = cdr(cur);
      if (cur && ispair(cur->content)) {
        body = car(cur);
        vali = make_node(refexp(body));
        /* Strip/validate type hints in the macro's param list, just like
           def/fn/defn (so (defmacro f (x :int) ...) doesn't treat :int as a
           parameter). Hints are meaningless for macros, so phints is ignored.
         */
        uint8_t phints[ENV_INLINE_SLOTS];
        exp_t *herr = NULL;
        exp_t *clean_params = build_clean_params(header, e, env, &herr, phints);
        if (herr) {
          unrefexp(vali);
          unrefexp(e);
          return herr;
        }
        val = make_node(clean_params);
        val->next = vali;
        val->type = EXP_MACRO;
        char *qname = ns_qualify(exp_text(name), env); /* (ns ...)-aware */
        val->meta = (keyval_t *)strdup(qname);
        if (env) {
          val->next->meta = (struct keyval_t *)ref_env(env);
          env->has_closure = 1;
        }
        if (!(env->d))
          env->d = create_dict();
        set_get_keyval_dict(env->d, qname,
                            val);    /* return value (the kv) unused */
        if (qname != exp_text(name)) /* ns_qualify allocated iff qualified */
          free(qname);
        GEN_BUMP();
      }

      else
        val =
            error(EXP_ERROR_BODY_NOT_LIST, e, env, "Error body is not a list");
    } else
      val =
          error(EXP_ERROR_PARAM_NOT_LIST, e, env, "Error params is not a list");
  } else
    val = error(EXP_ERROR_MISSING_NAME, e, env,
                "Error missing name or name not a lambda");
  unrefexp(e);
  return val;
}

#pragma GCC diagnostic ignored "-Wunused-parameter"
const char doc_quote[] =
    "(quote x) — return x without evaluating it. Reader shorthand: 'x.";
exp_t *quotecmd(exp_t *e, env_t *env) {
  exp_t *ret = refexp(cadr(e));
  unrefexp(e);
  return ret;
}
#pragma GCC diagnostic warning "-Wunused-parameter"

/* quasiquote expander — walks the template, returns a new AST that when
   evaluated produces the filled-in structure.
   Atoms → (quote atom)
   (unquote x) → x  (evaluated at runtime)
   list with unquote-splicing elements → (append <spliced> (cons ...))
   other lists → (cons (qq car) (qq cdr)) */
static exp_t *qq_expand(exp_t *tmpl, int depth);
static exp_t *qq_make_list2(char *sym, exp_t *a, exp_t *b) {
  exp_t *head = make_node(make_symbol(sym, strlen(sym)));
  head->next = make_node(a);
  head->next->next = make_node(b);
  return head;
}
static exp_t *qq_expand(exp_t *tmpl, int depth) {
  if (!tmpl || !ispair(tmpl) || !istrue(tmpl)) {
    /* atom: (quote tmpl) */
    exp_t *q = make_node(make_symbol("quote", 5));
    q->next = make_node(refexp(tmpl));
    return q;
  }
  /* check for (unquote x) */
  if (issymbol(tmpl->content) &&
      !strcmp((char *)exp_text(tmpl->content), "unquote")) {
    if (depth == 0)
      return refexp(cadr(tmpl)); /* splice evaluation site */
    /* nested quasiquote — decrease depth, still expand */
    exp_t *inner = qq_expand(cadr(tmpl), depth - 1);
    exp_t *uq = make_node(make_symbol("unquote", 7));
    uq->next = make_node(inner);
    exp_t *q = make_node(make_symbol("quote", 5));
    q->next = make_node(uq);
    return q;
  }
  /* check for (quasiquote x) — nested, increase depth */
  if (issymbol(tmpl->content) &&
      !strcmp((char *)exp_text(tmpl->content), "quasiquote")) {
    exp_t *inner = qq_expand(cadr(tmpl), depth + 1);
    exp_t *qq_sym = make_node(make_symbol("quasiquote", 10));
    qq_sym->next = make_node(inner);
    exp_t *q = make_node(make_symbol("quote", 5));
    q->next = make_node(qq_sym);
    return q;
  }
  /* list: expand element by element, building (cons car-exp cdr-exp) chain.
     If an element is (unquote-splicing xs), use (append xs ...) instead. */
  exp_t *car = tmpl->content;
  exp_t *cdr = tmpl->next; /* remaining nodes (raw, not wrapped) */

  if (ispair(car) && istrue(car) && issymbol(car->content) &&
      !strcmp((char *)exp_text(car->content), "unquote-splicing")) {
    /* (append <splice-form> <rest-expansion>) */
    exp_t *splice = refexp(cadr(car));
    exp_t *rest_exp;
    if (!cdr || !istrue(cdr))
      rest_exp = refexp(NIL_EXP);
    else
      rest_exp = qq_expand(cdr, depth);
    return qq_make_list2("append", splice, rest_exp);
  } else {
    exp_t *car_exp = qq_expand(car, depth);
    exp_t *cdr_exp;
    if (!cdr || !istrue(cdr))
      cdr_exp = make_node(make_symbol("quote", 5));
    else {
      /* cdr is the raw next node(s) — treat as a list by wrapping it */
      cdr_exp = qq_expand(cdr, depth);
      goto done;
    }
    /* (quote nil) for empty cdr */
    cdr_exp->next = make_node(refexp(NIL_EXP));
  done:
    return qq_make_list2("cons", car_exp, cdr_exp);
  }
}

const char doc_quasiquote[] =
    "(quasiquote tmpl) — template expansion. `x reader shorthand. "
    "Evaluates (unquote expr) sub-forms and splices (unquote-splicing list) "
    "sub-forms into the surrounding list. Shorthands: ,expr and ,@list.";
exp_t *quasiquotecmd(exp_t *e, env_t *env) {
  exp_t *tmpl = cadr(e);
  exp_t *expanded = qq_expand(tmpl, 0);
  unrefexp(e);
  /* EVAL takes ownership of expanded — do NOT unrefexp it after. */
  return EVAL(expanded, env);
}

const char doc_if[] =
    "(if test then [else]) — branch on test. Falsey is nil/empty; everything "
    "else (including 0 and \"\") is truthy.";
exp_t *ifcmd(exp_t *e, env_t *env) {
  /* Tail-aware: propagates in_tail_position to the selected branch. */
  int outer_tail = in_tail_position;
  exp_t *tmpexp, *tmpexp2;
  in_tail_position = 0;
  tmpexp = EVAL(cadr(e), env);
  if iserror (tmpexp) {
    in_tail_position = outer_tail;
    unrefexp(e);
    return tmpexp;
  }
  if (istrue(tmpexp)) {
    unrefexp(tmpexp);
    tmpexp = refexp(caddr(e));
    unrefexp(e);
    in_tail_position = outer_tail;
    return evaluate(tmpexp, env);
  } else {
    unrefexp(tmpexp);
    if ((tmpexp = cdddr(e)))
      do {
        in_tail_position = 0;
        tmpexp2 = EVAL(tmpexp->content, env);
        if ((!iserror(tmpexp2)) && (tmpexp->next)) {
          if (istrue(tmpexp2)) {
            unrefexp(tmpexp2);
            tmpexp2 = refexp(cadr(tmpexp));
            unrefexp(e);
            in_tail_position = outer_tail;
            return evaluate(tmpexp2, env);
          }
          if (!(tmpexp = cddr(tmpexp))) {
            unrefexp(tmpexp2);
            unrefexp(e);
            in_tail_position = outer_tail;
            return NIL_EXP; /* clauses exhausted, no match — canonical nil, not
                               raw NULL (else iso/is mis-compare vs nil literal)
                             */
          }
        } else {
          unrefexp(e);
          in_tail_position = outer_tail;
          return tmpexp2;
        }
      } while (1);
    else {
      unrefexp(e);
      in_tail_position = outer_tail;
      return NIL_EXP; /* no else branch — canonical nil, not raw NULL */
    }
  }
}

static exp_t *setq_store_symbol(exp_t *sym, env_t *env, exp_t *val) {
  env_t *cur = env, *root = env;
  while (root && root->root)
    root = root->root;

  while (cur) {
    for (int i = 0; i < cur->n_inline; i++) {
      const char *k = cur->inline_keys[i];
      if (k && strcmp(k, exp_text(sym)) == 0) {
        unrefexp(cur->inline_vals[i]);
        cur->inline_vals[i] = refexp(val);
        return val;
      }
    }
    if (cur->d) {
      keyval_t *kv = set_get_keyval_dict(cur->d, exp_text(sym), NULL);
      if (kv) {
        GEN_BUMP();
        unrefexp(kv->val);
        kv->val = refexp(val);
        return val;
      }
    }
    cur = cur->root;
  }

  if (!root)
    root = env;
  if (!(root->d))
    root->d = create_dict();
  set_get_keyval_dict(root->d, exp_text(sym), val);
  GEN_BUMP();
  return val;
}

/* Store VAL into the binding for SYM for a compiled `=`/`setf` whose target
   is NOT a local slot (a captured free var or a global). Mirrors the symbol
   path of updatebang EXACTLY so compiling `=` is a behavior-preserving
   substitute for the AST path: walk the env chain inner→outer and mutate the
   nearest existing binding in place; if none exists anywhere, create it in
   the CURRENT env. (This last point is the only difference from
   setq_store_symbol, which creates in the root env instead.) Borrows SYM and
   VAL — consumes neither; the caller keeps VAL on the VM stack as the result.
   This is what lets mutable closures like make-counter compile to bytecode
   instead of falling back to AST. */
/* Returns 1 if it REFUSED a GLOBAL write under the RESP callback guard
   (binding NOT written — caller must clean up val's ref), 0 otherwise. */
static int assign_store_symbol(exp_t *sym, env_t *env, exp_t *val) {
  env_t *cur = env;
  while (cur) {
    for (int i = 0; i < cur->n_inline; i++) {
      const char *k = cur->inline_keys[i];
      if (k && strcmp(k, exp_text(sym)) == 0) {
        unrefexp(cur->inline_vals[i]);
        cur->inline_vals[i] = refexp(val);
        return 0;
      }
    }
    if (cur->d) {
      keyval_t *kv = set_get_keyval_dict(cur->d, exp_text(sym), NULL);
      if (kv) {
        /* Global binding (root env)? Refuse under the concurrent-callback
           guard before mutating. Local/closure bindings stay writable. */
        if (g_resp_cb_guard && cur->root == NULL)
          return 1;
        GEN_BUMP();
        unrefexp(kv->val);
        kv->val = refexp(val);
        return 0;
      }
    }
    cur = cur->root;
  }
  /* Create in current env. If that is the global env, this is a global
     write: refuse under the guard before allocating/writing. */
  if (g_resp_cb_guard && env->root == NULL)
    return 1;
  if (!(env->d))
    env->d = create_dict();
  set_get_keyval_dict(env->d, exp_text(sym), val);
  GEN_BUMP();
  return 0;
}

const char doc_setq[] =
    "(setq sym val [sym val ...]) — Emacs-style variable assignment: update "
    "the nearest existing binding, or create a top-level session binding if "
    "none exists.";
exp_t *setqcmd(exp_t *e, env_t *env) {
  exp_t *args = cdr(e);
  if (!args) {
    unrefexp(e);
    return NIL_EXP;
  }

  exp_t *ret = NIL_EXP;
  for (exp_t *a = args; a; a = cddr(a)) {
    exp_t *sym = car(a);
    if (!issymbol(sym)) {
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "setq: variable name must be a symbol");
    }
    exp_t *_rerr = NULL;
    REJECT_RESERVED_ASSIGN(sym, _rerr, {
      if (ret != NIL_EXP)
        unrefexp(ret);
      unrefexp(e);
      return _rerr;
    });
    if (!cdr(a)) {
      unrefexp(e);
      return error(ERROR_MISSING_PARAMETER, NULL, env,
                   "setq: missing value for symbol");
    }
    exp_t *val = EVAL(cadr(a), env);
    if (iserror(val)) {
      unrefexp(e);
      return val;
    }
    if (ret != NIL_EXP)
      unrefexp(ret);
    ret = val;
    setq_store_symbol(sym, env, val);
  }
  unrefexp(e);
  return ret;
}

const char doc_eq[] =
    "(= place val) — assign val to place. Place can be a symbol, (car/cdr "
    "...), or (str i) for in-place char update.";
const char doc_setf[] =
    "(setf place val) — exact synonym of (= place val); a more readable head "
    "for assignment, especially in indented Adder.";
exp_t *equalcmd(exp_t *e, env_t *env) {
  /* Strict arity: exactly (= place val). Silent truncation was hiding
     real bugs — (= a 1 2 3 4) used to bind a=1 and discard the rest;
     (= a) silently bound a to nil. */
  exp_t *args = cdr(e);
  if (!args || !cdr(args) || cddr(args)) {
    exp_t *err = error(ERROR_MISSING_PARAMETER, e, env,
                       "(= place val): expected exactly 2 arguments");
    unrefexp(e);
    return err;
  }
  exp_t *tmpexp = EVAL(caddr(e), env);
  exp_t *tmpkey = refexp(cadr(e));
  unrefexp(e);
  if iserror (tmpexp) {
    unrefexp(tmpkey);
    return tmpexp;
  }
  return updatebang(tmpkey, env, tmpexp);
  /* to be unrefed tmpkey in case of evaluate */
}

const char doc_persist[] = "(persist sym) — mark sym's top-level binding so it "
                           "survives savedb / loaddb (db.dump).";
exp_t *persistcmd(exp_t *e, env_t *env) {
  /* persist marks a global binding — refuse from a concurrent callback. */
  if (g_resp_cb_guard) {
    unrefexp(e);
    return resp_cb_readonly_error(env);
  }
  exp_t *tmpkey = refexp(cadr(e));
  exp_t *ret = NULL;
  if (!issymbol(tmpkey)) {
    tmpkey = evaluate(tmpkey, env);
  }
  unrefexp(e);
  /* to be unrefed tmpkey in case of evaluate */
  if iserror (tmpkey) {
    return tmpkey;
  }
  env_t *cur = env;
  while (cur->root)
    cur = cur->root;
  if (!cur->d) { /* no global dict allocated yet → nothing bound to persist */
    unrefexp(tmpkey);
    return NIL_EXP;
  }
  ret = set_keyval_dict_timestamp(cur->d, exp_text(tmpkey), gettimeusec());
  unrefexp(tmpkey);
  return ret;
}

const char doc_ispersistent[] = "(ispersistent sym) — t if sym is currently "
                                "marked persistent, nil otherwise.";
exp_t *ispersistentcmd(exp_t *e, env_t *env) {
  exp_t *tmpkey = refexp(cadr(e));
  int64_t ret = 0;
  if (!issymbol(tmpkey)) {
    tmpkey = evaluate(tmpkey, env);
  }
  unrefexp(e);
  if iserror (tmpkey)
    return tmpkey;
  env_t *cur = env;
  while (cur->root)
    cur = cur->root;
  if (!cur->d) { /* no global dict yet → not persistent */
    unrefexp(tmpkey);
    return NIL_EXP;
  }
  ret = get_keyval_dict_timestamp(cur->d, exp_text(tmpkey));
  unrefexp(tmpkey);
  /* Only positive timestamps are persist marks. Negative values encode
     RESP TTL (absolute-µs expire-at) and must NOT report as persistent. */
  if (ret > 0) {
    return TRUE_EXP;
  } else {
    return NIL_EXP;
  }
}

/* `forget` and `unpersist` split apart for clarity. The historical
   `forget` only zeroed the timestamp (= "don't save next savedb") which
   surprised users who expected the binding to actually vanish. */
const char doc_forget[] =
    "(forget sym) — remove sym's binding entirely. After this, sym is unbound. "
    "Use (unpersist sym) if you only want to stop saving it.";
exp_t *forgetcmd(exp_t *e, env_t *env) {
  /* forget removes a global binding — refuse from a concurrent callback. */
  if (g_resp_cb_guard) {
    unrefexp(e);
    return resp_cb_readonly_error(env);
  }
  exp_t *tmpkey = refexp(cadr(e));
  if (!issymbol(tmpkey)) {
    tmpkey = evaluate(tmpkey, env);
  }
  unrefexp(e);
  if iserror (tmpkey)
    return tmpkey;
  /* Walk to the global env (where defs and persists live). del_keyval_dict
     drops the value's ref; bumping global_gen invalidates any stale
     gcache entries pointing at the now-freed pointer. */
  env_t *cur = env;
  while (cur->root)
    cur = cur->root;
  if (cur->d) {
    del_keyval_dict(cur->d, exp_text(tmpkey));
    GEN_BUMP();
  }
  unrefexp(tmpkey);
  return NIL_EXP;
}

const char doc_unpersist[] =
    "(unpersist sym) — clear sym's persistence mark; the binding stays live "
    "but won't be written by (savedb).";
exp_t *unpersistcmd(exp_t *e, env_t *env) {
  /* unpersist clears a global persist mark — refuse from a concurrent cb. */
  if (g_resp_cb_guard) {
    unrefexp(e);
    return resp_cb_readonly_error(env);
  }
  exp_t *tmpkey = refexp(cadr(e));
  exp_t *ret = NULL;
  if (!issymbol(tmpkey)) {
    tmpkey = evaluate(tmpkey, env);
  }
  unrefexp(e);
  if iserror (tmpkey)
    return tmpkey;
  env_t *cur = env;
  while (cur->root)
    cur = cur->root;
  if (!cur->d) { /* no global dict yet → nothing to unpersist */
    unrefexp(tmpkey);
    return NIL_EXP;
  }
  ret = set_keyval_dict_timestamp(cur->d, exp_text(tmpkey), 0);
  unrefexp(tmpkey);
  return ret;
}

/* Process-wide default db path. Overridden by `alcove --db <path>`;
   used by (savedb) / (loaddb) when called without an explicit filename
   AND by the startup auto-loader. So `alcove --db foo.db` uniformly
   makes foo.db "the" db file for the rest of the session. */
const char *alcove_db_path = "db.dump";
/* Tracks whether alcove_db_path points at heap memory we own (a strdup from
   an explicit (savedb/loaddb "path")) vs. a string literal / argv pointer we
   must not free. Lets the path-adoption sites release the previous owned
   value instead of leaking it on every explicit save/load. */
static int alcove_db_path_owned = 0;
/* Adopt `path` as the session-default db path, freeing any previously-owned
   copy. Idempotent if `p` already points at the current value. */
static void alcove_set_db_path(const char *p) {
  char *dup = strdup(p);
  if (!dup)
    return; /* keep the existing path on OOM rather than dropping it */
  if (alcove_db_path_owned)
    free((void *)alcove_db_path);
  alcove_db_path = dup;
  alcove_db_path_owned = 1;
}

/* Unified-dump format magic + version. Files starting with "ALCV"
   carry both the Lisp env and the RESP keyspace; files without the
   magic fall through to the legacy dump_dict reader (env-only).

   v1: vec records are [u32 len][__DUMP__ per element] — every cell
       round-trips through the generic boxer.
   v2: vec records carry a u8 kind tag + raw payload for typed kinds:
       [u8 kind][u32 len] then for GEN repeats __DUMP__ per element, for
       I64 writes len*int64 raw, for F64 writes len*double raw. v1 dumps
       still load (load_vec_v1); dump_vec always writes v2. */
#define ALCOVE_DUMP_MAGIC "ALCV"
/* v3 adds a custom-type table after the header (count + {id, name, spec}) so
   foreign module types persist: the body keeps the 2-byte runtime id, the table
   maps it to a durable name, and the loader remaps id→name→this-process's-id
   (auto-(require)ing the module unless --safe). v1/v2 dumps still load. */
#define ALCOVE_DUMP_VERSION 3
#define ALCOVE_DUMP_VERSION_MIN 1
static void auto_require_native(const char *spec); /* defined after require */
#define ALCOVE_SEC_LISP 'L'
#define ALCOVE_SEC_RESP 'R'

/* alcove_load_dump_version: declared earlier (forward decl near
   dump_vec / load_vec). Initialised to ALCOVE_DUMP_VERSION so v2 is the
   default for "dump without prior load" paths. */

/* Forward decls — resp.c (#included near the bottom of this file)
   exposes the live RESP keyspace pointer for persistence. resp_kv_get
   returns NULL until a reactor has started serving (lazy init);
   resp_kv_init eager-creates the table for the startup auto-load
   path which runs before any reactor spawns. */
struct lfkv;
#ifdef ALCOVE_WEB
/* Web build excludes resp.c (pthread/epoll/socket). resp_kv_get stubbed
   so savedb/loaddb still link; resp_kv_init isn't needed since the -r
   branch that called it is also gated out. */
static inline struct lfkv *resp_kv_get(void) { return NULL; }
#else
struct lfkv *resp_kv_get(void);
struct lfkv *resp_kv_init(void);
#endif

/* Iterator ctx for the section-R walk: counts written records and
   carries the output stream. The lfkv_iter_fn callback below uses
   the same record framing as section L (0xFE marker per record,
   followed by val + key) plus an int64 expiry prefix. */
typedef struct {
  FILE *stream;
  size_t count;
  int failed;
} resp_dump_ctx_t;

static int resp_dump_iter(const char *k, size_t klen, exp_t *val,
                          int64_t expiry_us, void *ctx) {
  resp_dump_ctx_t *c = (resp_dump_ctx_t *)ctx;
  if (c->failed)
    return 1; /* short-circuit on prior I/O failure */
  if (!is_fully_dumpable(val, 0)) {
    /* Skip with a warning — shallow-dumpable containers (e.g. a vector
       holding a dict) would otherwise fail __DUMP__ mid-record and corrupt
       the file. Callers can tighten by registering more dump fns. */
    fprintf(stderr,
            "savedb: skipping resp key (%zu bytes) — type %d (or a nested "
            "element) has no dump fn registered\n",
            klen, TYPEOF_E(val));
    return 0; /* continue */
  }
  uint8_t marker = 0xFE;
  if (fwrite(&marker, 1, 1, c->stream) != 1) {
    c->failed = 1;
    return 1;
  }
  if (fwrite(&expiry_us, sizeof(int64_t), 1, c->stream) != 1) {
    c->failed = 1;
    return 1;
  }
  if (!__DUMP__(val, c->stream)) {
    c->failed = 1;
    return 1;
  }
  if (!dump_strn(k, klen, c->stream)) {
    c->failed = 1;
    return 1;
  }
  c->count++;
  return 0;
}

/* Walk the global env's dict_t and write every persisted (timestamp>0)
   entry as a section-L record: 0xFE marker + val + key. Mirrors the
   existing dump_dict body but with framing, so the reader can detect
   end-of-section without relying on EOF. */
static int dump_lisp_section(dict_t *d, FILE *stream) {
  for (unsigned int i = 0; i < 2; i++) {
    for (unsigned int j = 0; j < d->ht[i].size; j++) {
      keyval_t *ckv = d->ht[i].table[j];
      while (ckv) {
        keyval_t *pkv = ckv;
        ckv = pkv->next;
        if (pkv->timestamp <= 0)
          continue;
        if (!is_fully_dumpable(pkv->val, 0)) {
          fprintf(stderr,
                  "savedb: skipping %s — type %d (or a nested element) has "
                  "no dump fn\n",
                  (char *)pkv->key, TYPEOF_E(pkv->val));
          continue;
        }
        uint8_t marker = 0xFE;
        if (fwrite(&marker, 1, 1, stream) != 1)
          return 0;
        if (!__DUMP__(pkv->val, stream))
          return 0;
        if (!dump_str(pkv->key, stream))
          return 0;
      }
    }
  }
  return 1;
}

/* Unified dump: writes header + section L (Lisp env, persisted vars
   only) + section R (RESP keyspace, all live entries with their
   expiry timestamps). `kv` may be NULL — section R is then omitted.
   Returns 1 on success, 0 on I/O failure. */
int alcove_dump_unified(env_t *global, struct lfkv *kv, FILE *stream) {
  /* Header: 4-byte magic + 2-byte version. */
  if (fwrite(ALCOVE_DUMP_MAGIC, 1, 4, stream) != 4)
    return 0;
  uint16_t ver = ALCOVE_DUMP_VERSION;
  if (fwrite(&ver, 2, 1, stream) != 1)
    return 0;

  /* v3 custom-type table: count, then {id:2, name, module_spec} for every
     registered custom type, so the loader can remap dump ids by durable name
     and (auto-)require the module. Usually empty (count 0). */
  {
    uint16_t ntypes = 0;
    for (unsigned short i = EXP_MAXSIZE; i < g_next_type_id; i++)
      if (g_custom_types[i].name)
        ntypes++;
    if (fwrite(&ntypes, 2, 1, stream) != 1)
      return 0;
    for (unsigned short i = EXP_MAXSIZE; i < g_next_type_id; i++) {
      if (!g_custom_types[i].name)
        continue;
      uint16_t id = i;
      const char *spec =
          g_custom_types[i].module_spec ? g_custom_types[i].module_spec : "";
      if (fwrite(&id, 2, 1, stream) != 1 ||
          !dump_strn(g_custom_types[i].name, strlen(g_custom_types[i].name),
                     stream) ||
          !dump_strn(spec, strlen(spec), stream))
        return 0;
    }
  }

  /* Walk to the global env (savedb is allowed from a nested env, and
     callers that don't have an env handy may pass NULL to skip the
     Lisp section entirely — RESP SAVE uses this path). */
  env_t *root = global;
  while (root && root->root)
    root = root->root;

  /* Section L. */
  uint8_t tag = ALCOVE_SEC_LISP;
  uint8_t end = 0x00;
  if (fwrite(&tag, 1, 1, stream) != 1)
    return 0;
  if (root && root->d && !dump_lisp_section(root->d, stream))
    return 0;
  if (fwrite(&end, 1, 1, stream) != 1)
    return 0;

  /* Section R. */
  if (kv) {
    tag = ALCOVE_SEC_RESP;
    if (fwrite(&tag, 1, 1, stream) != 1)
      return 0;
    resp_dump_ctx_t ctx = {stream, 0, 0};
    lfkv_foreach(kv, resp_dump_iter, &ctx);
    if (ctx.failed)
      return 0;
    if (fwrite(&end, 1, 1, stream) != 1)
      return 0;
  }

  /* Trailer: section-tag 0 marks end-of-file. Forward-compatible —
     a future reader sees 0 and stops; an older reader handed a future
     file with extra sections sees an unknown tag and bails cleanly. */
  if (fwrite(&end, 1, 1, stream) != 1)
    return 0;
  return 1;
}

/* Read records until the 0x00 end-of-section marker. shape determines
   whether to consume the section-R expiry prefix. Returns the number
   of records loaded, or -1 on parse error. */
static int load_section_records(FILE *stream, env_t *root, struct lfkv *kv,
                                int is_resp) {
  int n = 0;
  for (;;) {
    uint8_t marker;
    if (fread(&marker, 1, 1, stream) != 1)
      return -1;
    if (marker == 0x00)
      return n; /* end of section */
    if (marker != 0xFE)
      return -1; /* corrupt */
    int64_t expiry = 0;
    if (is_resp && fread(&expiry, sizeof(int64_t), 1, stream) != 1)
      return -1;
    exp_t *val = load_exp_t(stream);
    if (!val)
      return -1;
    if (is_resp) {
      char *k = NULL;
      size_t klen = 0;
      if (!load_strn(&k, &klen, stream)) {
        unrefexp(val);
        return -1;
      }
      if (kv) {
        /* lfkv_set transfers the caller's ref; bump first since we
           still hold it via val and need to release on failure. */
        if (lfkv_set(kv, k, klen, refexp(val)) == 0 && expiry > 0)
          lfkv_set_expiry(kv, k, klen, expiry);
      }
      free(k);
      unrefexp(val);
    } else {
      char *key = NULL;
      if (!load_str(&key, stream)) {
        unrefexp(val);
        return -1;
      }
      if (root) {
        if (!root->d)
          root->d = create_dict();
        keyval_t *kvp = set_get_keyval_dict(root->d, key, val);
        if (kvp)
          kvp->timestamp = gettimeusec();
      }
      free(key);
      unrefexp(val);
    }
    n++;
  }
}

/* Unified load: reads header, then sections by tag. Returns:
     1 = our format successfully loaded (n_lisp + n_resp via out-params)
     0 = magic missing → caller should fall back to legacy reader
    -1 = our format detected but corrupt/truncated. */
int alcove_load_unified(env_t *global, struct lfkv *kv, FILE *stream,
                        int *n_lisp, int *n_resp) {
  char magic[4];
  if (fread(magic, 1, 4, stream) != 4)
    return 0;
  if (memcmp(magic, ALCOVE_DUMP_MAGIC, 4) != 0) {
    /* Not our format — rewind so the legacy reader sees the original
       byte stream. */
    fseek(stream, 0, SEEK_SET);
    return 0;
  }
  uint16_t ver;
  if (fread(&ver, 2, 1, stream) != 1)
    return -1;
  if (ver < ALCOVE_DUMP_VERSION_MIN || ver > ALCOVE_DUMP_VERSION) {
    fprintf(stderr, "loaddb: unsupported alcove.dump version %u\n", ver);
    return -1;
  }
  alcove_load_dump_version = (int)ver;

  /* v3 custom-type table → build the dump-id → this-process-id remap, keyed by
     the durable type name. A name not registered here is (auto-)required from
     its module spec (unless --safe); if it still can't be resolved its remap
     stays 0, and any value of that type aborts the load in load_exp_t. */
  memset(g_type_remap, 0, sizeof g_type_remap);
  if (ver >= 3) {
    uint16_t ntypes;
    if (fread(&ntypes, 2, 1, stream) != 1)
      return -1;
    for (uint16_t t = 0; t < ntypes; t++) {
      uint16_t dump_id;
      char *name = NULL, *spec = NULL;
      size_t nlen = 0, slen = 0;
      if (fread(&dump_id, 2, 1, stream) != 1 ||
          !load_strn(&name, &nlen, stream) ||
          !load_strn(&spec, &slen, stream)) {
        free(name);
        free(spec);
        return -1;
      }
      unsigned short cur = custom_type_id_by_name(name);
      if (!cur && !g_safe_mode && spec[0]) {
        auto_require_native(spec); /* dlopen + alcove_module_init */
        cur = custom_type_id_by_name(name);
      }
      if (dump_id < ALCOVE_TYPE_CAP)
        g_type_remap[dump_id] = cur; /* 0 stays unresolved */
      if (!cur)
        fprintf(stderr,
                "loaddb: custom type '%s' unavailable%s — values of it won't "
                "load\n",
                name,
                g_safe_mode ? " (--safe: module auto-load disabled)" : "");
      free(name);
      free(spec);
    }
  }

  env_t *root = global;
  while (root && root->root)
    root = root->root;
  int nl = 0, nr = 0;
  for (;;) {
    uint8_t tag;
    if (fread(&tag, 1, 1, stream) != 1)
      return -1;
    if (tag == 0x00)
      break; /* end-of-file trailer */
    if (tag == ALCOVE_SEC_LISP) {
      int got = load_section_records(stream, root, NULL, 0);
      if (got < 0)
        return -1;
      nl += got;
    } else if (tag == ALCOVE_SEC_RESP) {
      int got = load_section_records(stream, NULL, kv, 1);
      if (got < 0)
        return -1;
      nr += got;
    } else {
      fprintf(stderr, "loaddb: unknown section tag 0x%02x — stopping\n", tag);
      return -1;
    }
  }
  if (n_lisp)
    *n_lisp = nl;
  if (n_resp)
    *n_resp = nr;
  GEN_BUMP();
  return 1;
}

const char doc_savedb[] =
    "(savedb) writes to the active db (default ./db.dump, overridden by --db). "
    "(savedb \"path\") writes to the given file.";
exp_t *savedbcmd(exp_t *e, env_t *env) {
  /* Resolve the target path: optional first arg, else session default.
     The arg is evaluated so callers can build the path: (savedb (str ...)). */
  const char *path = alcove_db_path;
  EVAL_ARG_1(path_arg);
  if (path_arg) {
    if (!isstring(path_arg))
      CLEAN_RETURN_1(
          path_arg,
          error(ERROR_ILLEGAL_VALUE, NULL, env,
                "savedb: optional argument must be a filename string"));
    path = (const char *)exp_text(path_arg);
  }
  /* Snapshot path before releasing path_arg — error() would receive a
     dangling pointer if we unrefexp(path_arg) first and its ptr was freed. */
  char *path_snap = (path == alcove_db_path) ? NULL : strdup(path);
  FILE *stream = fopen(path, "w");
  if (!stream) {
    if (path_arg)
      unrefexp(path_arg);
    unrefexp(e);
    exp_t *err =
        error(ERROR_ILLEGAL_VALUE, NULL, env, "Unable to open '%s' for writing",
              path_snap ? path_snap : path);
    free(path_snap);
    return err;
  }
  /* Unified writer: section L (this env) + section R (resp_kv if a
     reactor is alive). Old-format db.dump files are still readable
     via the magic-detection path in loaddb_from_file_path. */
  int ok = alcove_dump_unified(env, resp_kv_get(), stream);
  fclose(stream);
  if (!ok) {
    if (path_arg)
      unrefexp(path_arg);
    unrefexp(e);
    exp_t *err =
        error(ERROR_ILLEGAL_VALUE, NULL, env, "savedb: I/O error writing '%s'",
              path_snap ? path_snap : path);
    free(path_snap);
    return err;
  }
  free(path_snap);
  /* Successful explicit save → adopt this path as the session default,
     so a follow-up (loaddb) or (savedb) targets the same file. */
  if (path_arg) {
    alcove_set_db_path(path);
    unrefexp(path_arg);
  }
  return e;
}

/* Inverse of savedb: walk the on-disk dump (which is a series of
   `__DUMP__(val)` followed by `dump_str(key)` records — see dump_dict)
   and re-install every (key, val) pair into the global env. Each
   reloaded entry is marked persistent (timestamp != 0) so that a later
   (savedb) writes it back out. Returns the number of entries loaded,
   or -1 if the file can't be opened. Note: only EXP_CHAR and EXP_STRING
   currently have load/dump fns registered (alcove.c:5260-5265), so any
   other types weren't actually written by savedb in the first place —
   loaddb here is symmetric with what savedb actually persists. */
int loaddb_from_file_path(env_t *env, const char *path) {
  FILE *stream = fopen(path, "r");
  if (!stream)
    return -1;
  /* Magic detection: unified format ("ALCV") covers env + RESP and
     gets dispatched through alcove_load_unified. Anything else is
     assumed to be the legacy dump_dict format (env-only) — kept
     readable so existing db.dump files migrate transparently on the
     next savedb. */
  int nl = 0, nr = 0;
  int u = alcove_load_unified(env, resp_kv_get(), stream, &nl, &nr);
  if (u == 1) {
    fclose(stream);
    return nl + nr;
  }
  if (u < 0) {
    fclose(stream);
    return -1;
  }
  /* Legacy path — alcove_load_unified rewound the stream on miss. */
  env_t *cur = env;
  while (cur->root)
    cur = cur->root;
  if (!cur->d)
    cur->d = create_dict();
  int n = 0;
  for (;;) {
    exp_t *val = load_exp_t(stream);
    if (!val)
      break;
    char *key = NULL;
    if (!load_str(&key, stream)) {
      unrefexp(val);
      break;
    }
    keyval_t *kv = set_get_keyval_dict(cur->d, key, val);
    if (kv)
      kv->timestamp = gettimeusec(); /* re-persistent */
    free(key);                       /* set_get_keyval_dict strdup'd it */
    unrefexp(val);                   /* dict took its own ref */
    n++;
  }
  fclose(stream);
  GEN_BUMP(); /* invalidate gcache */
  return n;
}

/* Back-compat shim: the path-less callers (auto-load at startup, the
   no-arg (loaddb)) keep working without re-plumbing every site.
   Reads the session default — same as savedb. */
int loaddb_from_file(env_t *env) {
  return loaddb_from_file_path(env, alcove_db_path);
}

const char doc_loaddb[] =
    "(loaddb) reads the active db (default ./db.dump, overridden by --db). "
    "(loaddb \"path\") reads from the given file. Auto-runs at startup unless "
    "--noload.";
exp_t *loaddbcmd(exp_t *e, env_t *env) {
  const char *path = alcove_db_path;
  EVAL_ARG_1(path_arg);
  if (path_arg) {
    if (!isstring(path_arg))
      CLEAN_RETURN_1(
          path_arg,
          error(ERROR_ILLEGAL_VALUE, NULL, env,
                "loaddb: optional argument must be a filename string"));
    path = (const char *)exp_text(path_arg);
  }
  int n = loaddb_from_file_path(env, path);
  if (n < 0) {
    exp_t *err = error(ERROR_ILLEGAL_VALUE, e, env,
                       "Unable to open '%s' for reading", path);
    if (path_arg)
      unrefexp(path_arg);
    return err;
  }
  printf("loaded %d entries from %s\n", n, path);
  /* Successful explicit load → adopt as session default. Same rule as
     savedb: the last filename you mentioned is the active one. */
  if (path_arg) {
    alcove_set_db_path(path);
    unrefexp(path_arg);
  }
  unrefexp(e);
  return MAKE_FIX(n);
}

/* cmpcmd serves four lisp names; one doc per dispatch keeps the table
   clean. Comparisons are variadic: (< a b c) means a<b AND b<c. */
const char doc_lt[] =
    "(< a b ...) — strictly less than (chained: each pair must hold).";
const char doc_gt[] = "(> a b ...) — strictly greater than (chained).";
const char doc_le[] = "(<= a b ...) — less than or equal (chained).";
const char doc_ge[] = "(>= a b ...) — greater than or equal (chained).";
/* Error texts shared by the AST and VM tiers so the two cannot drift (the same
   operator must report identically in interpreted and compiled code). Visible
   to the later-#included builtins_stdlib.h (modcmd). */
/* Only mod-by-zero and compare-incompatible are unified here (they had drifted
   between the AST and VM tiers); other cross-tier messages — div-by-zero,
   integer overflow — still use per-site literals and could be folded in later.
 */
static const char ERR_MODULO_BY_ZERO[] = "Illegal modulo by 0";
static const char ERR_COMPARE_INCOMPAT[] = "compare: incompatible types";

/* Pairwise compare helper. Returns 1 on success with d set to the sign
   of (a - b); returns 0 on type mismatch (caller raises error). */
static int alc_pair_cmp(exp_t *a, exp_t *b, double *d) {
  /* Decimal: exact when both are decimals; mixed with another numeric compares
     via double (comparison is permissive — only *arithmetic* mixing is strict).
   */
  if (isdecimal(a) || isdecimal(b)) {
    if (isdecimal(a) && isdecimal(b)) {
      *d = (double)dec_cmp((alc_dec_t *)a->ptr, (alc_dec_t *)b->ptr);
      return 1;
    }
    int aok = isdecimal(a) || isnumber(a) || isfloat(a) || isrational(a);
    int bok = isdecimal(b) || isnumber(b) || isfloat(b) || isrational(b);
    if (aok && bok) {
      double da = isdecimal(a)    ? dec_to_double(a)
                  : isrational(a) ? rat_to_double(a)
                                  : TO_DOUBLE(a);
      double db = isdecimal(b)    ? dec_to_double(b)
                  : isrational(b) ? rat_to_double(b)
                                  : TO_DOUBLE(b);
      *d = da - db;
      return 1;
    }
    return 0;
  }
  /* Exact vs exact (fixnum/rational): compare without precision loss. */
  if (is_exact(a) && is_exact(b)) {
    *d = (double)rat_cmp(a, b);
    return 1;
  }
  if ((isnumber(a) || isfloat(a) || isrational(a)) &&
      (isnumber(b) || isfloat(b) || isrational(b))) {
    double da = isrational(a) ? rat_to_double(a) : TO_DOUBLE(a);
    double db = isrational(b) ? rat_to_double(b) : TO_DOUBLE(b);
    *d = da - db;
    return 1;
  }
  if (isstring(a) && isstring(b)) {
    *d = strcmp(exp_text(a), exp_text(b));
    return 1;
  }
  if (ischar(a) && ischar(b)) {
    *d = (double)CHAR_VAL(a) - (double)CHAR_VAL(b);
    return 1;
  }
  return 0;
}

exp_t *cmpcmd(exp_t *e, env_t *env) {
  exp_t *op = car(e);
  if (!op || !issymbol(op)) {
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "compare: missing operator");
  }
  /* Decode the operator once via a table (kept in lockstep with the apply
     switch below). Semantics match bytecode SLOT_<cmp>_FIX, so chained results
     agree with the compiler's per-pair comparisons. */
  enum { CMP_LT, CMP_GT, CMP_LE, CMP_GE };
  static const struct {
    const char *sym;
    int kind;
  } cmp_ops[] = {{"<", CMP_LT}, {">", CMP_GT}, {"<=", CMP_LE}, {">=", CMP_GE}};
  int op_kind = -1;
  const char *opname = exp_text(op);
  for (size_t i = 0; i < sizeof cmp_ops / sizeof cmp_ops[0]; i++)
    if (strcmp(opname, cmp_ops[i].sym) == 0) {
      op_kind = cmp_ops[i].kind;
      break;
    }
  if (op_kind < 0) {
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "compare: unknown operator");
  }

  /* Walk args pairwise: (< a b c d) iff a<b AND b<c AND c<d. 0 or 1
     args is vacuously true (matches Scheme/Clojure/Python semantics).
     Doc claimed chaining; the previous implementation only read the
     first two args and silently dropped the rest. */
  exp_t *prev = NULL;
  exp_t *cur_node = e->next;
  while (cur_node) {
    exp_t *v = EVAL(cur_node->content, env);
    if (iserror(v)) {
      if (prev)
        unrefexp(prev);
      unrefexp(e);
      return v;
    }
    if (prev) {
      double d;
      if (!alc_pair_cmp(prev, v, &d)) {
        unrefexp(prev);
        unrefexp(v);
        unrefexp(e);
        return error(ERROR_ILLEGAL_VALUE, NULL, env, ERR_COMPARE_INCOMPAT);
      }
      int ok;
      switch (op_kind) {
      case CMP_LT:
        ok = d < 0;
        break;
      case CMP_GT:
        ok = d > 0;
        break;
      case CMP_LE:
        ok = d <= 0;
        break;
      default:
        ok = d >= 0;
        break; /* CMP_GE */
      }
      unrefexp(prev);
      if (!ok) {
        unrefexp(v);
        unrefexp(e);
        return NIL_EXP;
      }
    }
    prev = v;
    cur_node = cur_node->next;
  }
  if (prev)
    unrefexp(prev);
  unrefexp(e);
  return TRUE_EXP;
}

/* Continue an arithmetic fold in exact (rational) mode. The fast int/float
   MATH_CMD loop calls this the moment it meets a rational operand: `acc` is the
   running result (owned: fixnum or rational), `c` is the remaining arg list,
   `count` is how many operands were already folded into acc (incl. the one that
   triggered the hand-off). Applies `op` ('+','-','*','/') across the rest with
   Python-style contagion: meeting a float demotes the whole expression to
   float; meeting a non-number errors. Bounded: an exact result that overflows
   int64 raises rather than wrapping. Consumes acc, e, and every arg it
   evaluates; returns an owned result or an error exp_t. */
static exp_t *tower_fold(char op, exp_t *acc, exp_t *c, env_t *env, int is_sub,
                         int is_div, int count, exp_t *e) {
  int i = count;
  int saw_float = 0;
  double facc = 0;
  for (; c; c = c->next) {
    exp_t *w = EVAL(c->content, env);
    if (iserror(w)) {
      if (!saw_float)
        unrefexp(acc);
      unrefexp(e);
      return w;
    }
    i++;
    if (saw_float) {
      if (isnumber(w))
        facc = apply_op_d(op, facc, (double)FIX_VAL(w));
      else if (isfloat(w))
        facc = apply_op_d(op, facc, w->f);
      else if (isrational(w))
        facc = apply_op_d(op, facc, rat_to_double(w));
      else {
        /* error() refs `e` for the caret/backtrace, so build it BEFORE
           dropping our own ref (unrefexp(e) first would free e, then
           error()'s refexp(e) would resurrect a freed node — a UAF that
           corrupts refcounts cumulatively). */
        exp_t *er =
            error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
        unrefexp(w);
        unrefexp(e);
        return er;
      }
      unrefexp(w);
      continue;
    }
    if (isfloat(w)) { /* exact + float -> float (contagion) */
      facc = apply_op_d(op, exact_to_double(acc), w->f);
      unrefexp(acc);
      acc = NULL;
      saw_float = 1;
      unrefexp(w);
      continue;
    }
    if (is_exact(w)) { /* fixnum or rational: stay exact */
      const char *err;
      exp_t *r = rat_binop(op, acc, w, &err);
      unrefexp(acc);
      unrefexp(w);
      if (err) {
        exp_t *er =
            error(err[0] == 'd' ? ERROR_DIV_BY0 : ERROR_ILLEGAL_VALUE, e, env,
                  err[0] == 'd' ? "Illegal division by 0"
                                : "exact arithmetic overflow (no bignum; use "
                                  "float for inexact)");
        unrefexp(e); /* error() holds its own ref to e; drop ours after */
        return er;
      }
      acc = r;
      continue;
    }
    /* rational/decimal mixing and non-numbers: refuse (two exact systems don't
       silently combine; non-numbers are type errors). */
    {
      exp_t *er =
          error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
      unrefexp(w);
      unrefexp(acc);
      unrefexp(e);
      return er;
    }
  }
  if (saw_float) {
    unrefexp(e);
    return make_floatf(facc);
  }
  /* Unary (- a) / (/ a): negate / reciprocate. */
  if (i == 1) {
    const char *err;
    exp_t *r = NULL;
    if (is_sub)
      r = rat_binop('-', MAKE_FIX(0), acc, &err);
    else if (is_div)
      r = rat_binop('/', MAKE_FIX(1), acc, &err);
    if (is_sub || is_div) {
      unrefexp(acc);
      if (err) {
        exp_t *er =
            error(err[0] == 'd' ? ERROR_DIV_BY0 : ERROR_ILLEGAL_VALUE, e, env,
                  err[0] == 'd' ? "Illegal division by 0"
                                : "exact arithmetic overflow");
        unrefexp(e);
        return er;
      }
      acc = r;
    }
  }
  unrefexp(e);
  return acc;
}

/* Decimal sibling of tower_fold. Strict contagion (Python-style): a decimal
   combines with integers (coerced to decimal) and other decimals only — a
   float or a rational operand is a type error (two exact systems, or
   exact-vs-inexact, don't silently mix). acc is an owned decimal. Bounded:
   results beyond 28 significant digits, and 1/0, raise. */
static exp_t *dec_err(exp_t *e, env_t *env, int over) {
  return error(over == 2 ? ERROR_DIV_BY0 : ERROR_ILLEGAL_VALUE, e, env,
               over == 2 ? "Illegal division by 0"
                         : "decimal overflow (exceeds 28 significant digits)");
}
static exp_t *decimal_fold(char op, exp_t *acc, exp_t *c, env_t *env,
                           int is_sub, int is_div, int count, exp_t *e) {
  int i = count, over;
  for (; c; c = c->next) {
    exp_t *w = EVAL(c->content, env);
    if (iserror(w)) {
      unrefexp(acc);
      unrefexp(e);
      return w;
    }
    i++;
    exp_t *wd;
    if (isdecimal(w)) {
      wd = w; /* borrow */
    } else if (isnumber(w)) {
      wd = make_decimal_raw((__int128)FIX_VAL(w), 0, &over); /* fits: int64 */
    } else { /* float or rational: strict refusal */
      /* build the error before unref'ing e (error() refs e; freeing it first
         would make that refexp resurrect a freed node — cumulative UAF). */
      exp_t *er = error(ERROR_ILLEGAL_VALUE, e, env,
                        "decimal does not combine with float or rational "
                        "(convert explicitly)");
      unrefexp(w);
      unrefexp(acc);
      unrefexp(e);
      return er;
    }
    exp_t *r = dec_binop(op, acc, wd, &over);
    unrefexp(acc);
    if (wd != w)
      unrefexp(wd);
    unrefexp(w);
    if (over) {
      exp_t *er = dec_err(e, env, over);
      unrefexp(e);
      return er;
    }
    acc = r;
  }
  if (i == 1) { /* unary (- d) negate, (/ d) reciprocal */
    if (is_sub) {
      alc_dec_t *d = (alc_dec_t *)acc->ptr;
      exp_t *r = make_decimal_raw(-d->coef, d->scale, &over);
      unrefexp(acc);
      if (over) {
        exp_t *er = dec_err(e, env, over);
        unrefexp(e);
        return er;
      }
      acc = r;
    } else if (is_div) {
      exp_t *one = make_decimal_raw(1, 0, &over);
      exp_t *r = dec_binop('/', one, acc, &over);
      unrefexp(one);
      unrefexp(acc);
      if (over) {
        exp_t *er = dec_err(e, env, over);
        unrefexp(e);
        return er;
      }
      acc = r;
    }
  }
  unrefexp(e);
  return acc;
}

#define MATH_CMD(name, init_i, OP, IS_SUB, IS_DIV, OP_CHAR)                      \
  exp_t *name(exp_t *e, env_t *env) {                                            \
    int64_t sum_i = (init_i);                                                    \
    expfloat sum_f = (init_i);                                                   \
    int saw_float = 0;                                                           \
    exp_t *c = cdr(e);                                                           \
    exp_t *v = NULL;                                                             \
    int i = 0;                                                                   \
    exp_t *ret = NULL;                                                           \
    do {                                                                         \
      if (c) {                                                                   \
        i++;                                                                     \
        v = EVAL(c->content, env);                                               \
        if (iserror(v)) {                                                        \
          unrefexp(e);                                                           \
          return v;                                                              \
        }                                                                        \
        if ((IS_DIV) && i > 1) {                                                 \
          if ((isnumber(v) && FIX_VAL(v) == 0) || (isfloat(v) && v->f == 0)) {   \
            ret = error(ERROR_DIV_BY0, e, env, "Illegal division by 0");         \
            unrefexp(v);                                                         \
            goto finish;                                                         \
          }                                                                      \
        }                                                                        \
        if (saw_float) {                                                         \
          if (isnumber(v)) {                                                     \
            sum_f OP FIX_VAL(v);                                                 \
          } else if (isfloat(v)) {                                               \
            sum_f OP v->f;                                                       \
          } else if (isrational(v)) { /* rational in float context -> float */   \
            sum_f OP rat_to_double(v);                                           \
          } else if (isdecimal(v)) { /* float + decimal: strict refusal */       \
            ret = error(ERROR_ILLEGAL_VALUE, e, env,                             \
                        "decimal does not combine with float (convert "          \
                        "explicitly)");                                          \
            unrefexp(v);                                                         \
            goto finish;                                                         \
          } else {                                                               \
            ret = error(ERROR_ILLEGAL_VALUE, e, env,                             \
                        "Illegal value in operation");                           \
            unrefexp(v);                                                         \
            goto finish;                                                         \
          }                                                                      \
        } else {                                                                 \
          if (isnumber(v)) {                                                     \
            if (i > 1 || (init_i) != 0) {                                        \
              if (fix_op_ovf((OP_CHAR), &sum_i, FIX_VAL(v))) {                   \
                ret = error(ERROR_ILLEGAL_VALUE, e, env,                         \
                            "integer overflow (no implicit float; use a "        \
                            "float, rational, or decimal)");                     \
                unrefexp(v);                                                     \
                goto finish;                                                     \
              }                                                                  \
            } else {                                                             \
              sum_i = FIX_VAL(v);                                                \
            }                                                                    \
          } else if (isfloat(v)) {                                               \
            if (i > 1 || (init_i) != 0) {                                        \
              sum_f = sum_i;                                                     \
              sum_f OP v->f;                                                     \
            } else {                                                             \
              sum_f = v->f;                                                      \
            }                                                                    \
            sum_i = 0;                                                           \
            saw_float = 1;                                                       \
          } else if (isrational(v)) {                                            \
            /* Exact non-integer: leave the int/float fast path and finish the   \
               fold in rational mode. acc = v seeds the first operand, else      \
               apply OP to the integer accumulated so far. tower_fold consumes   \
               e and the remaining args. */                                      \
            exp_t *acc;                                                          \
            if (i == 1) {                                                        \
              acc = v; /* transfer ownership */                                  \
            } else {                                                             \
              const char *_err;                                                  \
              acc = rat_binop((OP_CHAR), MAKE_FIX(sum_i), v, &_err);             \
              unrefexp(v);                                                       \
              if (_err) {                                                        \
                ret = error(_err[0] == 'd' ? ERROR_DIV_BY0                       \
                                           : ERROR_ILLEGAL_VALUE,                \
                            e, env,                                              \
                            _err[0] == 'd' ? "Illegal division by 0"             \
                                           : "exact arithmetic overflow");       \
                goto finish;                                                     \
              }                                                                  \
            }                                                                    \
            return tower_fold((OP_CHAR), acc, c->next, env, (IS_SUB),            \
                              (IS_DIV), i, e);                                   \
          } else if (isdecimal(v)) {                                             \
            /* Decimal: finish the fold in decimal mode. Seed with v, else       \
               apply OP to the integer accumulated so far (coerced). */          \
            int _o;                                                              \
            exp_t *acc;                                                          \
            if (i == 1) {                                                        \
              acc = v;                                                           \
            } else {                                                             \
              exp_t *si = make_decimal_raw((__int128)sum_i, 0, &_o);             \
              acc = dec_binop((OP_CHAR), si, v, &_o);                            \
              unrefexp(si);                                                      \
              unrefexp(v);                                                       \
              if (_o) {                                                          \
                ret = dec_err(e, env, _o);                                       \
                goto finish;                                                     \
              }                                                                  \
            }                                                                    \
            return decimal_fold((OP_CHAR), acc, c->next, env, (IS_SUB),          \
                                (IS_DIV), i, e);                                 \
          } else {                                                               \
            ret = error(ERROR_ILLEGAL_VALUE, e, env,                             \
                        "Illegal value in operation");                           \
            unrefexp(v);                                                         \
            goto finish;                                                         \
          }                                                                      \
        }                                                                        \
        unrefexp(v);                                                             \
      }                                                                          \
    } while (c && (c = c->next));                                                \
    /* (-)  and (/) with no args are errors — they have no identity value. */    \
    if (i == 0 && ((IS_SUB) || (IS_DIV))) {                                      \
      ret = error(ERROR_MISSING_PARAMETER, e, env,                               \
                  (IS_SUB) ? "(- a ...): needs at least one argument"            \
                           : "(/ a ...): needs at least one argument");          \
      goto finish;                                                               \
    }                                                                            \
    if (i == 1) {                                                                \
      if (IS_SUB) {                                                              \
        if (saw_float) {                                                         \
          sum_f = -sum_f;                                                        \
        } else {                                                                 \
          int64_t _neg;                                                          \
          /* Negation that leaves the 61-bit fixnum range errors — no implicit \
             float (explicit over implicit). */                                  \
          if (__builtin_sub_overflow((int64_t)0, sum_i, &_neg) ||                \
              !FIX_FITS(_neg)) {                                                 \
            ret = error(ERROR_ILLEGAL_VALUE, e, env,                             \
                        "integer overflow (no implicit float; use a float, "     \
                        "rational, or decimal)");                                \
            goto finish;                                                         \
          }                                                                      \
          sum_i = _neg;                                                          \
        }                                                                        \
      } else if (IS_DIV) {                                                       \
        if (saw_float) {                                                         \
          if (sum_f == 0) {                                                      \
            ret = error(ERROR_DIV_BY0, e, env, "Illegal division by 0");         \
            goto finish;                                                         \
          }                                                                      \
          sum_f = 1 / sum_f;                                                     \
        } else {                                                                 \
          if (sum_i == 0) {                                                      \
            ret = error(ERROR_DIV_BY0, e, env, "Illegal division by 0");         \
            goto finish;                                                         \
          }                                                                      \
          sum_i = 1 / sum_i;                                                     \
        }                                                                        \
      }                                                                          \
    }                                                                            \
    if (saw_float)                                                               \
      ret = make_floatf(sum_f);                                                  \
    else if (!FIX_FITS(sum_i)) /* result left the 61-bit fixnum range */         \
      ret = error(ERROR_ILLEGAL_VALUE, e, env,                                   \
                  "integer overflow (no implicit float; use a float, "           \
                  "rational, or decimal)");                                      \
    else                                                                         \
      ret = make_integeri(sum_i);                                                \
  finish:                                                                        \
    unrefexp(e);                                                                 \
    return ret;                                                                  \
  }

const char doc_plus[] =
    "(+ x ...) — sum of all args. (+) is 0. Integer overflow errors (no "
    "implicit float); mix in a float for inexact math.";
MATH_CMD(pluscmd, 0, +=, 0, 0, '+')

const char doc_mul[] = "(* x ...) — product of all args. (*) is 1.";
MATH_CMD(multiplycmd, 1, *=, 0, 0, '*')

const char doc_minus[] =
    "(- a) negates; (- a b c ...) subtracts the rest from a.";
MATH_CMD(minuscmd, 0, -=, 1, 0, '-')

const char doc_div[] = "(/ a b ...) — divide a by the rest. Integer division "
                       "if all args are ints; otherwise float.";
MATH_CMD(dividecmd, 0, /=, 0, 1, '/')

const char doc_rational[] =
    "(rational n d) — exact fraction n/d (d≠0), sign-normalized and reduced. "
    "(rational n) is just n. Integer-valued results collapse to a plain int. "
    "Components are int64; an operation whose exact result overflows int64 "
    "errors (there is no bignum — use a float for inexact big magnitudes).";
exp_t *rationalcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(n, d);
  if (!n || !isnumber(n))
    CLEAN_RETURN_2(n, d,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "rational: numerator must be an integer"));
  int64_t den = 1;
  if (d) {
    if (!isnumber(d))
      CLEAN_RETURN_2(n, d,
                     error(ERROR_ILLEGAL_VALUE, e, env,
                           "rational: denominator must be an integer"));
    den = FIX_VAL(d);
    if (den == 0)
      CLEAN_RETURN_2(
          n, d, error(ERROR_DIV_BY0, e, env, "rational: denominator is 0"));
  }
  CLEAN_RETURN_2(n, d, make_rational(FIX_VAL(n), den));
}

const char doc_numerator[] =
    "(numerator x) — numerator of a rational; x itself if x is an integer.";
exp_t *numeratorcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(v);
  if (isrational(v))
    CLEAN_RETURN_1(v, make_rational(((alc_rat_t *)v->ptr)->num, 1));
  if (isnumber(v))
    CLEAN_RETURN_1(v, v); /* integer: numerator is itself */
  CLEAN_RETURN_1(v, error(ERROR_ILLEGAL_VALUE, e, env,
                          "numerator: argument must be a rational or integer"));
}

const char doc_denominator[] =
    "(denominator x) — denominator of a rational; 1 if x is an integer.";
exp_t *denominatorcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(v);
  if (isrational(v))
    CLEAN_RETURN_1(v, make_rational(((alc_rat_t *)v->ptr)->den, 1));
  if (isnumber(v))
    CLEAN_RETURN_1(v, MAKE_FIX(1));
  CLEAN_RETURN_1(v,
                 error(ERROR_ILLEGAL_VALUE, e, env,
                       "denominator: argument must be a rational or integer"));
}

const char doc_rationalp[] =
    "(rational? x) — t if x is a non-integer rational (an exact fraction). "
    "Integer-valued rationals collapse to ints, so this is nil for plain ints.";
exp_t *rationalpcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(v);
  CLEAN_RETURN_1(v, isrational(v) ? refexp(TRUE_EXP) : refexp(NIL_EXP));
}

const char doc_decimal[] =
    "(decimal x) — exact base-10 number. From a string \"1.50\" (exact, the "
    "normal path), an integer (scale 0), or another decimal. The literal 1.5m "
    "reads the same. Bounded to 28 significant digits; arithmetic that exceeds "
    "that errors. A float arg is refused (it is binary-inexact — pass a string "
    "to say exactly which decimal you mean).";
exp_t *decimalcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(v);
  if (!v)
    CLEAN_RETURN_1(v, error(ERROR_MISSING_PARAMETER, e, env,
                            "decimal: needs one argument"));
  int over = 0;
  exp_t *d = NULL;
  if (isdecimal(v))
    CLEAN_RETURN_1(v, refexp(v));
  if (isnumber(v))
    d = make_decimal_raw((__int128)FIX_VAL(v), 0, &over);
  else if (isstring(v)) {
    const char *s = exp_text(v);
    d = dec_parse(s, strlen(s), &over);
  } else if (isfloat(v))
    CLEAN_RETURN_1(v, error(ERROR_ILLEGAL_VALUE, e, env,
                            "decimal: refusing a float (binary-inexact) — pass "
                            "a string like \"1.5\""));
  else
    CLEAN_RETURN_1(v, error(ERROR_ILLEGAL_VALUE, e, env,
                            "decimal: argument must be a string or integer"));
  if (!d)
    CLEAN_RETURN_1(v,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         over == 3 ? "decimal: malformed number string"
                                   : "decimal: too many significant digits"));
  CLEAN_RETURN_1(v, d);
}

const char doc_decimalp[] = "(decimal? x) — t if x is a decimal.";
exp_t *decimalpcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(v);
  CLEAN_RETURN_1(v, isdecimal(v) ? refexp(TRUE_EXP) : refexp(NIL_EXP));
}

const char doc_sqrt[] =
    "(sqrt x) — float square root. See sqrt-int for the integer version.";
exp_t *sqrtcmd(exp_t *e, env_t *env) {
  exp_t *v;
  exp_t *ret;
  if ((v = e->next))
    v = EVAL(v->content, env);
  if iserror (v) {
    unrefexp(e);
    return v;
  }
  if (isfloat(v))
    ret = make_floatf(sqrt(v->f));
  else if (isnumber(v))
    ret = make_floatf(sqrt((double)FIX_VAL(v)));
  else
    ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
  unrefexp(v);
  unrefexp(e);
  return ret;
}

/* Vector storage + ops live in a dedicated #included fragment (kept in the
   single TU so the inline tensor helpers stay inlinable). */
#include "vector.h"

/* (sqrt-int n) — floor(sqrt(n)) on a non-negative fixnum. Built-in to
   avoid a sqrt + double→int round-trip in pure-integer code (common
   in trial-division early exit). */
const char doc_sqrtint[] = "(sqrt-int n) — integer square root (floor). Faster "
                           "than (int (sqrt n)) and exact.";
exp_t *sqrtintcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(v);
  if (!isnumber(v))
    CLEAN_RETURN_1(v, error(ERROR_NUMBER_EXPECTED, e, env,
                            "(sqrt-int n): n must be a fixnum"));
  int64_t n = FIX_VAL(v);
  if (n < 0)
    CLEAN_RETURN_1(v, MAKE_FIX(0));
  int64_t r = (int64_t)sqrt((double)n);
  /* Use uint64_t for the multiply to avoid signed overflow UB when r is
     near the fixnum maximum (n close to 2^60). */
  while ((uint64_t)(r + 1) * (uint64_t)(r + 1) <= (uint64_t)n)
    r++;
  while ((uint64_t)r * (uint64_t)r > (uint64_t)n)
    r--;
  CLEAN_RETURN_1(v, MAKE_FIX(r));
}

const char doc_exp[] = "(exp x) — natural exponential e^x.";
exp_t *expcmd(exp_t *e, env_t *env) {
  exp_t *v;
  exp_t *ret;
  if ((v = e->next))
    v = EVAL(v->content, env);
  if iserror (v) {
    unrefexp(e);
    return v;
  }
  if (isfloat(v))
    ret = make_floatf(exp(v->f));
  else if (isnumber(v))
    ret = make_floatf(exp((double)FIX_VAL(v)));
  else
    ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
  unrefexp(v);
  unrefexp(e);
  return ret;
}

const char doc_expt[] = "(expt b e) — b raised to the e power. Stays integer "
                        "if both args are fixnums and the result fits in 61 "
                        "bits; falls back to float otherwise. Alias: **.";
exp_t *exptcmd(exp_t *e, env_t *env) {
  exp_t *v = NULL;
  exp_t *v2 = NULL;
  exp_t *ret = NULL;
  if ((v = e->next))
    if ((v2 = v->next)) {
      v = EVAL(v->content, env);
      if iserror (v) {
        unrefexp(e);
        return v;
      }
      v2 = EVAL(v2->content, env);
      if iserror (v2) {
        unrefexp(e);
        unrefexp(v);
        return v2;
      }
    }
  if ((isfloat(v) || isnumber(v)) && (isfloat(v2) || isnumber(v2))) {
    /* Integer fast path: both args fixnums, exponent non-negative, and
       the running product never overflows int64 nor escapes int61.
       Repeated squaring; falls through to pow() if any step overflows. */
    int int_overflow = 0;
    if (isnumber(v) && isnumber(v2)) {
      int64_t k = FIX_VAL(v2);
      if (k >= 0) {
        int64_t base = FIX_VAL(v);
        int64_t r = 1;
        int overflow = 0;
        int64_t fix_max = ((int64_t)1 << 60) - 1;
        int64_t fix_min = -((int64_t)1 << 60);
        while (k > 0 && !overflow) {
          if (k & 1)
            overflow |= __builtin_mul_overflow(r, base, &r);
          k >>= 1;
          if (k > 0 && !overflow)
            overflow |= __builtin_mul_overflow(base, base, &base);
        }
        if (!overflow && r >= fix_min && r <= fix_max)
          ret = MAKE_FIX(r);
        else
          int_overflow = 1; /* integer result doesn't fit — error, no float */
      }
      /* k < 0: a negative exponent is a fractional result (2^-1 = 0.5), not an
         overflow — that still yields a float below. */
    }
    if (!ret) {
      if (int_overflow)
        ret = error(ERROR_ILLEGAL_VALUE, e, env,
                    "integer overflow in expt (no implicit float; use a float "
                    "base or exponent for an inexact result)");
      else
        ret = make_floatf(pow(isfloat(v) ? v->f : (double)FIX_VAL(v),
                              isfloat(v2) ? v2->f : (double)FIX_VAL(v2)));
    }
  } else {
    ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
  }
  unrefexp(v);
  unrefexp(v2);
  unrefexp(e);
  return ret;
}

#define FLOAT_UNARY_CMD(cname, fname, docstr, cdoc_sym)                        \
  const char cdoc_sym[] = docstr;                                              \
  exp_t *cname(exp_t *e, env_t *env) {                                         \
    exp_t *v = EVAL(cadr(e), env);                                             \
    if (iserror(v)) {                                                          \
      unrefexp(e);                                                             \
      return v;                                                                \
    }                                                                          \
    exp_t *ret;                                                                \
    if (isfloat(v))                                                            \
      ret = make_floatf(fname(v->f));                                          \
    else if (isnumber(v))                                                      \
      ret = make_floatf(fname((double)FIX_VAL(v)));                            \
    else {                                                                     \
      unrefexp(v);                                                             \
      unrefexp(e);                                                             \
      return error(ERROR_ILLEGAL_VALUE, NULL, env,                             \
                   #cname ": arg must be a number");                           \
    }                                                                          \
    unrefexp(v);                                                               \
    unrefexp(e);                                                               \
    return ret;                                                                \
  }

FLOAT_UNARY_CMD(roundcmd, round,
                "(round x) — round to nearest integer, as float.", doc_round)
FLOAT_UNARY_CMD(floorcmd, floor,
                "(floor x) — largest integer not greater than x, as float.",
                doc_floor)
FLOAT_UNARY_CMD(ceilcmd, ceil,
                "(ceil x) — smallest integer not less than x, as float.",
                doc_ceil)
FLOAT_UNARY_CMD(truncatecmd, trunc,
                "(truncate x) — round toward zero, as float.", doc_truncate)
FLOAT_UNARY_CMD(logcmd, log, "(log x) — natural logarithm of x.", doc_log)
FLOAT_UNARY_CMD(sincmd, sin, "(sin x) — sine of x (radians).", doc_sin)
FLOAT_UNARY_CMD(coscmd, cos, "(cos x) — cosine of x (radians).", doc_cos)
FLOAT_UNARY_CMD(tancmd, tan, "(tan x) — tangent of x (radians).", doc_tan)
#undef FLOAT_UNARY_CMD

const char doc_float[] = "(float x) — coerce integer to floating-point.";
exp_t *floatcmd(exp_t *e, env_t *env) {
  exp_t *v = EVAL(cadr(e), env);
  if (iserror(v)) {
    unrefexp(e);
    return v;
  }
  exp_t *ret;
  if (isfloat(v)) {
    ret = v;
    unrefexp(e);
    return ret;
  }
  if (isnumber(v))
    ret = make_floatf((double)FIX_VAL(v));
  else {
    unrefexp(v);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "float: arg must be a number");
  }
  unrefexp(v);
  unrefexp(e);
  return ret;
}

const char doc_int[] = "(int x) — coerce float to integer by truncation.";
exp_t *intcmd(exp_t *e, env_t *env) {
  exp_t *v = EVAL(cadr(e), env);
  if (iserror(v)) {
    unrefexp(e);
    return v;
  }
  exp_t *ret;
  if (isnumber(v)) {
    ret = v;
    unrefexp(e);
    return ret;
  }
  if (isfloat(v))
    ret = make_integeri((int64_t)v->f);
  else {
    unrefexp(v);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "int: arg must be a number");
  }
  unrefexp(v);
  unrefexp(e);
  return ret;
}

/* prcmd backs both `pr` and `print`; prncmd backs `prn` and `println`. */
const char doc_pr[] = "(pr x ...) — print each arg with no separator and no "
                      "trailing newline. Alias: print.";
exp_t *prcmd(exp_t *e, env_t *env) {
  exp_t *v = e;
  exp_t *val;
  while ((v = v->next)) {
    val = EVAL(v->content, env);
    if iserror (val) {
      unrefexp(e);
      return val;
    }
    if (val && isstring(val))
      printf("%s", (char *)exp_text(val));
    else
      print_node(val);
    unrefexp(val);
  }
  unrefexp(e);
  return NIL_EXP; /* NULL caused forcmd to mistake a clean run as "missing
                     param" */
}

const char doc_prn[] = "(prn x ...) — like pr, then a newline. Alias: println.";
exp_t *prncmd(exp_t *e, env_t *env) {
  exp_t *ret;
  ret = prcmd(e, env);
  printf("\n");
  return ret;
}

static const char *inspect_type_name(int t);

static void str_buf_put(char **buf, size_t *len, size_t *cap, const char *s,
                        size_t n) {
  if (*len + n + 1 > *cap) {
    if (*cap == 0)
      *cap =
          64; /* seed: doubling from 0 would loop forever (buf may be NULL) */
    while (*len + n + 1 > *cap)
      *cap *= 2;
    char *p = realloc(*buf, *cap);
    if (!p)
      oom_raise();
    *buf = p;
  }
  if (n)
    memcpy(*buf + *len, s, n);
  *len += n;
  (*buf)[*len] = 0;
}

/* Grow a raw byte buffer so it can hold len+n bytes (doubling from 64). Returns
   the possibly-moved buffer, or NULL on OOM WITHOUT freeing the old one (the
   caller still owns it). *cap is updated only on growth. Shared by the json /
   msgpack codecs — distinct from str_buf_put, which oom_raise()s instead of
   returning failure, because those codecs propagate OOM as a decode error. */
static void *buf_reserve(void *b, size_t len, size_t n, size_t *cap) {
  if (len + n <= *cap)
    return b;
  size_t nc = *cap ? *cap : 64;
  while (nc < len + n)
    nc *= 2;
  void *nb = realloc(b, nc);
  if (!nb)
    return NULL;
  *cap = nc;
  return nb;
}

static void exp_to_string_buf(exp_t *v, char **buf, size_t *len, size_t *cap) {
  char tmp[128];
  /* NULL/nil and the tagged immediates (fixnum, char) have no ->type word, so
     they're handled before the switch; everything past here is is_ptr. */
  if (!v || v == NIL_EXP) {
    str_buf_put(buf, len, cap, "nil", 3);
    return;
  }
  if (isnumber(v)) {
    int n = snprintf(tmp, sizeof tmp, "%lld", (long long)FIX_VAL(v));
    str_buf_put(buf, len, cap, tmp, (size_t)n);
    return;
  }
  if (ischar(v)) {
    char u[4];
    int k = utf8_encode((uint32_t)CHAR_VAL(v), u);
    str_buf_put(buf, len, cap, u, (size_t)k);
    return;
  }
  if (!is_ptr(v)) { /* an unknown immediate — deterministic, no pointer */
    str_buf_put(buf, len, cap, "#<value>", 8);
    return;
  }
  switch (v->type) {
  case EXP_STRING:
  case EXP_SYMBOL: {
    const char *_t = exp_text(v);
    str_buf_put(buf, len, cap, (char *)_t, strlen((char *)_t));
    break;
  }
  case EXP_FLOAT: {
    int n = snprintf(tmp, sizeof tmp, "%g", v->f);
    str_buf_put(buf, len, cap, tmp, (size_t)n);
    break;
  }
  case EXP_RATIONAL: {
    alc_rat_t *r = (alc_rat_t *)v->ptr;
    int n = snprintf(tmp, sizeof tmp, "%lld/%lld", (long long)r->num,
                     (long long)r->den);
    str_buf_put(buf, len, cap, tmp, (size_t)n);
    break;
  }
  case EXP_DECIMAL: {
    char db[48];
    int n = dec_to_str((alc_dec_t *)v->ptr, db); /* value text, no 'm' marker */
    str_buf_put(buf, len, cap, db, (size_t)n);
    break;
  }
  case EXP_BLOB: {
    alc_blob_t *b = (alc_blob_t *)v->ptr;
    if (b && b->len)
      str_buf_put(buf, len, cap, b->bytes, b->len);
    break;
  }
  case EXP_PAIR: {
    str_buf_put(buf, len, cap, "(", 1);
    exp_t *n = v;
    int first = 1;
    while (n && ispair(n) && istrue(n)) {
      if (!first)
        str_buf_put(buf, len, cap, " ", 1);
      first = 0;
      exp_to_string_buf(n->content, buf, len, cap);
      n = n->next;
    }
    /* improper tail: (a . b) */
    if (n && !ispair(n) && istrue(n)) {
      str_buf_put(buf, len, cap, " . ", 3);
      exp_to_string_buf(n, buf, len, cap);
    }
    str_buf_put(buf, len, cap, ")", 1);
    break;
  }
  case EXP_VECTOR: {
    /* Structural, deterministic — mirrors prn's #[...] so (str vec) and
       (prn vec) agree. The old fallback emitted #<vector@PTR>, a
       non-deterministic address. */
    str_buf_put(buf, len, cap, "#[", 2);
    int64_t vn = vec_len(v);
    for (int64_t i = 0; i < vn; i++) {
      if (i)
        str_buf_put(buf, len, cap, " ", 1);
      exp_t *cell = vec_get_boxed(v, i);
      exp_to_string_buf(cell, buf, len, cap);
      unrefexp(cell);
    }
    str_buf_put(buf, len, cap, "]", 1);
    break;
  }
  case EXP_DICT: {
    str_buf_put(buf, len, cap, "{", 1);
    dict_t *d = (dict_t *)v->ptr;
    int first = 1;
    if (d)
      for (unsigned int i = 0; i < d->ht[0].size; i++)
        for (keyval_t *k = d->ht[0].table[i]; k; k = k->next) {
          if (!first)
            str_buf_put(buf, len, cap, ", ", 2);
          first = 0;
          /* keys raw, like every other nested string in str output */
          str_buf_put(buf, len, cap, k->key, strlen(k->key));
          str_buf_put(buf, len, cap, " ", 1);
          exp_to_string_buf(k->val, buf, len, cap);
        }
    str_buf_put(buf, len, cap, "}", 1);
    break;
  }
  case EXP_SET: {
    str_buf_put(buf, len, cap, "#{", 2);
    dict_t *d = (dict_t *)v->ptr;
    int first = 1;
    if (d)
      for (unsigned int i = 0; i < d->ht[0].size; i++)
        for (keyval_t *k = d->ht[0].table[i]; k; k = k->next) {
          if (!first)
            str_buf_put(buf, len, cap, " ", 1);
          first = 0;
          exp_to_string_buf(k->val, buf, len, cap);
        }
    str_buf_put(buf, len, cap, "}", 1);
    break;
  }
  case EXP_LIST: {
    str_buf_put(buf, len, cap, "(", 1);
    alc_list_t *l = (alc_list_t *)v->ptr;
    if (l) {
      int first = 1;
      for (alc_listnode_t *ln = l->head; ln; ln = ln->next) {
        if (!first)
          str_buf_put(buf, len, cap, " ", 1);
        first = 0;
        exp_to_string_buf(ln->val, buf, len, cap);
      }
    }
    str_buf_put(buf, len, cap, ")", 1);
    break;
  }
  case EXP_LAMBDA:
    if (v->meta) {
      str_buf_put(buf, len, cap, "#<procedure:", 12);
      str_buf_put(buf, len, cap, (char *)v->meta, strlen((char *)v->meta));
      str_buf_put(buf, len, cap, ">", 1);
    } else
      str_buf_put(buf, len, cap, "#<procedure>", 12);
    break;
  case EXP_MACRO:
    if (v->meta) {
      str_buf_put(buf, len, cap, "#<macro:", 8);
      str_buf_put(buf, len, cap, (char *)v->meta, strlen((char *)v->meta));
      str_buf_put(buf, len, cap, ">", 1);
    } else
      str_buf_put(buf, len, cap, "#<macro>", 8);
    break;
  default: {
    /* builtins / ffi / anything else — deterministic type name, no
       pointer (str output must be reproducible). */
    int n = snprintf(tmp, sizeof tmp, "#<%s>", inspect_type_name(v->type));
    str_buf_put(buf, len, cap, tmp, (size_t)n);
  }
  }
}

const char doc_str[] = "(str x ...) — concatenate printable string forms.";
exp_t *strcmd(exp_t *e, env_t *env) {
  size_t cap = 64, len = 0;
  char *buf = memalloc(cap, 1);
  for (exp_t *a = cdr(e); a; a = a->next) {
    exp_t *v = EVAL(car(a), env);
    if (iserror(v)) {
      free(buf);
      unrefexp(e);
      return v;
    }
    exp_to_string_buf(v, &buf, &len, &cap);
    unrefexp(v);
  }
  exp_t *ret = make_string_take(buf, (int)len);
  unrefexp(e);
  return ret;
}

#define FMT_SPEC_MAX 32
const char doc_fmt[] =
    "(fmt template arg ...) — format string with {} placeholders. Use {} for "
    "default rendering or {:<spec>} for printf-style: {:.2f} {:%d} {:x} {:s}. "
    "Example: (fmt \"{} + {} = {:.1f}\" 1 2 3.0) → \"1 + 2 = 3.0\".";
exp_t *fmtcmd(exp_t *e, env_t *env) {
  exp_t *fmtarg = EVAL(cadr(e), env);
  if (iserror(fmtarg)) {
    unrefexp(e);
    return fmtarg;
  }
  if (!isstring(fmtarg)) {
    unrefexp(fmtarg);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "fmt: first arg must be a string template");
  }
  const char *tmpl = (const char *)exp_text(fmtarg);
  size_t cap = 128, len = 0;
  char *buf = memalloc(cap, 1);
  exp_t *cur = cddr(e);
  for (size_t i = 0; tmpl[i]; i++) {
    if (tmpl[i] != '{') {
      str_buf_put(&buf, &len, &cap, tmpl + i, 1);
      continue;
    }
    if (tmpl[i + 1] == '}') {
      i++;
      if (!cur)
        continue;
      exp_t *v = EVAL(car(cur), env);
      if (iserror(v)) {
        free(buf);
        unrefexp(fmtarg);
        unrefexp(e);
        return v;
      }
      exp_to_string_buf(v, &buf, &len, &cap);
      unrefexp(v);
      cur = cur->next;
    } else if (tmpl[i + 1] == ':') {
      size_t j = i + 2;
      while (tmpl[j] && tmpl[j] != '}')
        j++;
      size_t spec_len = j - (i + 2);
      if (!tmpl[j] || spec_len == 0 || spec_len >= FMT_SPEC_MAX) {
        str_buf_put(&buf, &len, &cap, "{", 1);
        continue;
      }
      char printf_fmt[FMT_SPEC_MAX + 2]; /* '%' + spec + '\0' */
      printf_fmt[0] = '%';
      memcpy(printf_fmt + 1, tmpl + i + 2, spec_len);
      printf_fmt[spec_len + 1] = '\0';
      char ftype = printf_fmt[spec_len];
      /* Reject anything other than printf flags/width/precision in the
         spec body (every char before the final type char). This blocks
         format-string injection: an embedded '%' or conversion letter
         would feed snprintf a second specifier, and a '*' would consume
         the arg as a width then read a nonexistent second arg — both
         undefined behavior. Invalid specs degrade to literal text. */
      int spec_ok = 1;
      for (size_t k = 0; k + 1 < spec_len; k++) {
        char sc = printf_fmt[1 + k];
        if (!(sc == '-' || sc == '+' || sc == ' ' || sc == '#' || sc == '.' ||
              (sc >= '0' && sc <= '9'))) {
          spec_ok = 0;
          break;
        }
      }
      if (!spec_ok) {
        str_buf_put(&buf, &len, &cap, "{", 1);
        continue;
      }
      i = j;
      if (!cur)
        continue;
      exp_t *v = EVAL(car(cur), env);
      if (iserror(v)) {
        free(buf);
        unrefexp(fmtarg);
        unrefexp(e);
        return v;
      }
      char tmp[256];
      char *out = tmp;
      char *heap = NULL;
      int n = 0;
      if (ftype == 'd' || ftype == 'i' || ftype == 'o' || ftype == 'u' ||
          ftype == 'x' || ftype == 'X') {
        int64_t iv = isnumber(v)  ? FIX_VAL(v)
                     : isfloat(v) ? (int64_t)v->f
                                  : 0LL;
        /* int64_t needs 'll' length modifier before the type letter */
        char safe_fmt[FMT_SPEC_MAX + 4]; /* '%' + spec + 'll' + '\0' */
        memcpy(safe_fmt, printf_fmt, spec_len);
        safe_fmt[spec_len] = 'l';
        safe_fmt[spec_len + 1] = 'l';
        safe_fmt[spec_len + 2] = ftype;
        safe_fmt[spec_len + 3] = '\0';
        n = snprintf(tmp, sizeof(tmp), safe_fmt, iv);
      } else if (ftype == 'f' || ftype == 'e' || ftype == 'E' || ftype == 'g' ||
                 ftype == 'G') {
        double dv = isfloat(v) ? v->f : isnumber(v) ? (double)FIX_VAL(v) : 0.0;
        n = snprintf(tmp, sizeof(tmp), printf_fmt, dv);
      } else if (ftype == 's') {
        const char *sv = isstring(v) ? (char *)exp_text(v) : "";
        n = snprintf(tmp, sizeof(tmp), printf_fmt, sv);
        /* %s with width or long strings can exceed tmp; retry on heap */
        if (n >= (int)sizeof(tmp)) {
          heap = memalloc((size_t)n + 1, 1);
          n = snprintf(heap, (size_t)n + 1, printf_fmt, sv);
          out = heap;
        }
      } else {
        exp_to_string_buf(v, &buf, &len, &cap);
      }
      if (n > 0)
        str_buf_put(&buf, &len, &cap, out,
                    (size_t)(n < (int)sizeof(tmp) || out == heap
                                 ? n
                                 : (int)sizeof(tmp) - 1));
      if (heap)
        free(heap);
      unrefexp(v);
      cur = cur->next;
    } else {
      str_buf_put(&buf, &len, &cap, "{", 1);
    }
  }
  buf[len] = 0;
  exp_t *ret = make_string_take(buf, (int)len);
  unrefexp(fmtarg);
  unrefexp(e);
  return ret;
}

const char doc_stringappend[] =
    "(string-append s ...) / (string-concat s ...) — concatenate strings "
    "(string-concat is an alias). Non-strings are errors.";
exp_t *stringappendcmd(exp_t *e, env_t *env) {
  size_t cap = 64, len = 0;
  char *buf = memalloc(cap, 1);
  for (exp_t *a = cdr(e); a; a = a->next) {
    exp_t *v = EVAL(car(a), env);
    if (iserror(v)) {
      free(buf);
      unrefexp(e);
      return v;
    }
    if (!isstring(v)) {
      unrefexp(v);
      free(buf);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "string-append: args must be strings");
    }
    {
      const char *_t = exp_text(v);
      str_buf_put(&buf, &len, &cap, (char *)_t, strlen((char *)_t));
    }
    unrefexp(v);
  }
  exp_t *ret = make_string_take(buf, (int)len);
  unrefexp(e);
  return ret;
}

/* ---- mutable string buffers (docs/specs/proposals.md Spec 1) ----
   Fresh fixed-size strings plus in-place codepoint mutation, for building
   text / FFI char* payloads. All indices are CODEPOINT-based, consistent
   with `length`, `substr`, and `(= (s i) ch)` (use `count` for byte length).
   For ASCII buffers codepoint == byte, so an FFI char* round-trips directly.
   Each op rebuilds the byte buffer (a replacement codepoint may differ in
   width) and mutates in place via exp_set_text. */

/* Build a fresh EXP_STRING of `n` copies of codepoint `cp`. */
static exp_t *make_filled_string(int64_t n, uint32_t cp) {
  char enc[4];
  int k = utf8_encode(cp, enc);
#if UINTPTR_MAX <= 0xffffffff
  /* Cap total allocation to 64 MiB to prevent overflow/excessive memory usage */
  if (n < 0 || (n > 0 && (size_t)n > (size_t)(64 * 1024 * 1024) / (size_t)k))
    return NULL;
#else
  if (n < 0)
    return NULL;
#endif
  size_t total = (size_t)n * (size_t)k;
  char *buf = memalloc(total + 1, 1);
  for (int64_t j = 0; j < n; j++)
    memcpy(buf + (size_t)j * (size_t)k, enc, (size_t)k);
  buf[total] = '\0';
  exp_t *s = make_string_take(buf, (int)total);
  return s;
}

const char doc_stringbuf[] =
    "(string-buf n [init]) — fresh mutable string of n copies of char init "
    "(default space). For building text / FFI char* buffers; mutate with "
    "string-set!/string-fill!/string-copy! or (= (s i) ch).";
exp_t *stringbufcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(nexp, initexp);
  if (!isnumber(nexp))
    CLEAN_RETURN_2(nexp, initexp,
                   error(ERROR_NUMBER_EXPECTED, e, env,
                         "string-buf: length must be an integer"));
  if (initexp && !ischar(initexp))
    CLEAN_RETURN_2(
        nexp, initexp,
        error(ERROR_ILLEGAL_VALUE, e, env, "string-buf: init must be a char"));
  int64_t n = FIX_VAL(nexp);
  if (n < 0 || n > ((int64_t)1 << 30))
    CLEAN_RETURN_2(nexp, initexp,
                   error(ERROR_INDEX_OUT_OF_RANGE, e, env,
                         "string-buf: length out of range"));
  uint32_t cp = initexp ? (uint32_t)CHAR_VAL(initexp) : (uint32_t)' ';
  exp_t *ret = make_filled_string(n, cp);
  if (!ret)
    ret = error(ERROR_ILLEGAL_VALUE, e, env, "string-buf: result too large");
  CLEAN_RETURN_2(nexp, initexp, ret);
}

const char doc_stringset[] =
    "(string-set! s i ch) — set codepoint i of string s to char ch, in place. "
    "Index is codepoint-based; out of range is an error. Returns s.";
exp_t *stringsetcmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(s, idx, ch);
  if (!isstring(s) || !isnumber(idx) || !ischar(ch))
    CLEAN_RETURN_3(
        s, idx, ch,
        error(ERROR_ILLEGAL_VALUE, e, env,
              "(string-set! s i ch): expected string, integer, char"));
  const char *cur = exp_text(s);
  int64_t cpi = FIX_VAL(idx);
  if (cpi < 0 || cpi >= utf8_strlen(cur))
    CLEAN_RETURN_3(s, idx, ch,
                   error(ERROR_INDEX_OUT_OF_RANGE, e, env,
                         "string-set!: index out of range"));
  size_t a = utf8_byte_offset(cur, cpi);
  size_t aend = utf8_byte_offset(cur, cpi + 1);
  size_t total = strlen(cur);
  char enc[4];
  int k = utf8_encode((uint32_t)CHAR_VAL(ch), enc);
  size_t newlen = a + (size_t)k + (total - aend);
  char *nb = memalloc(newlen + 1, 1);
  memcpy(nb, cur, a);
  memcpy(nb + a, enc, (size_t)k);
  memcpy(nb + a + (size_t)k, cur + aend, total - aend);
  nb[newlen] = '\0';
  exp_set_text(s, nb, newlen);
  free(nb);
  exp_t *ret = refexp(s);
  CLEAN_RETURN_3(s, idx, ch, ret);
}

const char doc_stringfill[] =
    "(string-fill! s ch) — set every codepoint of s to char ch, in place. "
    "Returns s.";
exp_t *stringfillcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(s, ch);
  if (!isstring(s) || !ischar(ch))
    CLEAN_RETURN_2(s, ch,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(string-fill! s ch): expected string and char"));
  int64_t n = utf8_strlen(exp_text(s));
  char enc[4];
  int k = utf8_encode((uint32_t)CHAR_VAL(ch), enc);
  size_t total = (size_t)n * (size_t)k;
  char *nb = memalloc(total + 1, 1);
  for (int64_t j = 0; j < n; j++)
    memcpy(nb + (size_t)j * (size_t)k, enc, (size_t)k);
  nb[total] = '\0';
  exp_set_text(s, nb, total);
  free(nb);
  exp_t *ret = refexp(s);
  CLEAN_RETURN_2(s, ch, ret);
}

const char doc_stringcopy[] =
    "(string-copy! dst i src) — copy src's codepoints into dst starting at "
    "codepoint index i, clamped at dst's end (dst's length never grows). "
    "Returns dst.";
exp_t *stringcopycmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(dst, idx, src);
  if (!isstring(dst) || !isnumber(idx) || !isstring(src))
    CLEAN_RETURN_3(
        dst, idx, src,
        error(ERROR_ILLEGAL_VALUE, e, env,
              "(string-copy! dst i src): expected string, integer, string"));
  int64_t di = FIX_VAL(idx);
  if (di < 0)
    CLEAN_RETURN_3(dst, idx, src,
                   error(ERROR_INDEX_OUT_OF_RANGE, e, env,
                         "string-copy!: negative index"));
  const char *d = exp_text(dst);
  const char *s = exp_text(src);
  int64_t dn = utf8_strlen(d);
  int64_t sn = utf8_strlen(s);
  if (di > dn)
    di = dn;
  int64_t ncopy = sn;
  if (di + ncopy > dn) /* clamp at dst's end — never grow dst */
    ncopy = dn - di;
  if (ncopy <= 0) { /* nothing fits; leave dst unchanged */
    exp_t *ret = refexp(dst);
    CLEAN_RETURN_3(dst, idx, src, ret);
  }
  /* Rebuild dst = d[0..di) + src[0..ncopy) + d[di+ncopy..dn). Reads of d and s
     finish before exp_set_text replaces dst's storage, so dst==src is safe. */
  size_t pre_end = utf8_byte_offset(d, di);
  size_t post_start = utf8_byte_offset(d, di + ncopy);
  size_t dtotal = strlen(d);
  size_t src_bytes = utf8_byte_offset(s, ncopy);
  size_t newlen = pre_end + src_bytes + (dtotal - post_start);
  char *nb = memalloc(newlen + 1, 1);
  memcpy(nb, d, pre_end);
  memcpy(nb + pre_end, s, src_bytes);
  memcpy(nb + pre_end + src_bytes, d + post_start, dtotal - post_start);
  nb[newlen] = '\0';
  exp_set_text(dst, nb, newlen);
  free(nb);
  exp_t *ret = refexp(dst);
  CLEAN_RETURN_3(dst, idx, src, ret);
}

const char doc_substr[] = "(substr s start end) — substring [start,end).";
exp_t *substrcmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(s, start, end);
  if (!isstring(s) || !isnumber(start) || !isnumber(end))
    CLEAN_RETURN_3(s, start, end,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "substr: expected string and numeric bounds"));
  const char *sp = (const char *)exp_text(s);
  int64_t n = utf8_strlen(sp); /* codepoint count, not byte count */
  int64_t a = FIX_VAL(start), b = FIX_VAL(end);
  if (a < 0)
    a = 0;
  if (a > n) /* clamp start to the top too: without this, start > len left
                a large while b dropped to n, so (b - a) went negative and
                flowed into make_string as a huge size → heap OOB. */
    a = n;
  if (b < a)
    b = a;
  if (b > n)
    b = n;
  size_t ba = utf8_byte_offset(sp, a); /* codepoint indices -> byte offsets */
  size_t bb = utf8_byte_offset(sp, b);
  exp_t *ret = make_string((char *)sp + ba, (int)(bb - ba));
  CLEAN_RETURN_3(s, start, end, ret);
}

static exp_t *list_append_owned(exp_t **ret, exp_t **tail, exp_t *v) {
  exp_t *node = make_node(v);
  if (*tail)
    *tail = (*tail)->next = node;
  else
    *ret = *tail = node;
  return node;
}

const char doc_stringsplit[] =
    "(string-split s sep) — split s on literal separator sep.";
exp_t *stringsplitcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(s, sep);
  if (!isstring(s) || !isstring(sep))
    CLEAN_RETURN_2(s, sep,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "string-split: args must be strings"));
  const char *str = (const char *)exp_text(s);
  const char *needle = (const char *)exp_text(sep);
  size_t nlen = strlen(needle);
  exp_t *ret = NIL_EXP, *tail = NULL;
  if (nlen == 0) {
    /* Empty separator: one element per codepoint, not per byte. */
    size_t off = 0;
    while (str[off]) {
      size_t prev = off;
      utf8_decode_at(str, &off);
      list_append_owned(&ret, &tail,
                        make_string((char *)str + prev, (int)(off - prev)));
    }
  } else {
    const char *p = str;
    const char *hit;
    while ((hit = strstr(p, needle))) {
      list_append_owned(&ret, &tail, make_string((char *)p, (int)(hit - p)));
      p = hit + nlen;
    }
    list_append_owned(&ret, &tail, make_string((char *)p, (int)strlen(p)));
  }
  CLEAN_RETURN_2(s, sep, ret);
}

const char doc_stringjoin[] =
    "(string-join xs sep) — join a list of strings with sep.";
exp_t *stringjoincmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(xs, sep);
  if (!isstring(sep))
    CLEAN_RETURN_2(xs, sep,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "string-join: separator must be a string"));
  size_t cap = 64, len = 0;
  char *buf = memalloc(cap, 1);
  int first = 1;
  exp_t *p = xs;
  while (p && istrue(p)) {
    if (!ispair(p) || !isstring(car(p))) {
      free(buf);
      CLEAN_RETURN_2(xs, sep,
                     error(ERROR_ILLEGAL_VALUE, NULL, env,
                           "string-join: xs must be a list of strings"));
    }
    if (!first) {
      const char *_t = exp_text(sep);
      str_buf_put(&buf, &len, &cap, (char *)_t, strlen((char *)_t));
    }
    first = 0;
    str_buf_put(&buf, &len, &cap, exp_text(car(p)), strlen(exp_text(car(p))));
    p = cdr(p);
  }
  exp_t *ret = make_string_take(buf, (int)len);
  CLEAN_RETURN_2(xs, sep, ret);
}

const char doc_stringtrim[] =
    "(string-trim s) — trim leading and trailing ASCII whitespace.";
exp_t *stringtrimcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(s);
  if (!isstring(s))
    CLEAN_RETURN_1(s, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "string-trim: arg must be a string"));
  char *p = (char *)exp_text(s);
  size_t n = strlen(p), a = 0, b = n;
  while (a < n && isspace((unsigned char)p[a]))
    a++;
  while (b > a && isspace((unsigned char)p[b - 1]))
    b--;
  exp_t *ret = make_string(p + a, (int)(b - a));
  CLEAN_RETURN_1(s, ret);
}

#define STRING_CASE_CMD(fname, docname, cname, fn)                             \
  const char docname[] = cname;                                                \
  exp_t *fname(exp_t *e, env_t *env) {                                         \
    EVAL_ARG_1(s);                                                             \
    if (!isstring(s))                                                          \
      CLEAN_RETURN_1(s, error(ERROR_ILLEGAL_VALUE, NULL, env,                  \
                              cname ": arg must be a string"));                \
    size_t n = strlen((char *)exp_text(s));                                    \
    char *buf = memalloc(n + 1, 1);                                            \
    for (size_t i = 0; i < n; i++)                                             \
      buf[i] = (char)fn((unsigned char)((char *)exp_text(s))[i]);              \
    exp_t *ret = make_string_take(buf, (int)n);                                \
    CLEAN_RETURN_1(s, ret);                                                    \
  }

STRING_CASE_CMD(stringupcasecmd, doc_stringupcase,
                "(string-upcase s) — uppercase ASCII letters", toupper)
STRING_CASE_CMD(stringdowncasecmd, doc_stringdowncase,
                "(string-downcase s) — lowercase ASCII letters", tolower)

static exp_t *slurp_file_as_string(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return NIL_EXP;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return NIL_EXP;
  }
  long sz = ftell(fp);
  if (sz < 0 || sz > (long)(1L << 30)) {
    fclose(fp);
    return NIL_EXP;
  }
  rewind(fp);
  char *buf = memalloc((size_t)sz + 1, 1);
  if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
    free(buf);
    fclose(fp);
    return NIL_EXP;
  }
  fclose(fp);
  exp_t *ret = make_string_take(buf, (int)sz);
  return ret;
}

/* Basename of a path (last component after '/'), for the short source label
   in error prefixes — "/tmp/bad.alc" -> "bad.alc". Falls back to the whole
   string when there's no slash. */
static const char *src_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/* Directory portion of path (everything before the last '/'), malloc'd; "."
   when there is no slash. Caller frees. Used so `require` can resolve a module
   relative to the file that required it. */
static char *path_dirname(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash)
    return strdup(".");
  size_t n = (size_t)(slash - path);
  if (n == 0)
    n = 1; /* "/foo" → dir "/" */
  char *d = memalloc(n + 1, 1);
  memcpy(d, path, n);
  d[n] = '\0';
  return d;
}

/* Read an entire stream into a malloc'd, NUL-terminated buffer. Tries a
   seek/size/read fast path; falls back to a chunked read for non-seekable
   streams (pipes). Returns NULL on OOM; *out_len gets the byte length. */
static char *slurp_stream(FILE *fp, size_t *out_len) {
  if (fseek(fp, 0, SEEK_END) == 0) {
    long sz = ftell(fp);
    if (sz >= 0) {
      rewind(fp);
#if UINTPTR_MAX <= 0xffffffff
      if ((size_t)sz > SIZE_MAX - 1)
        return NULL;
#endif
      char *buf = malloc((size_t)sz + 1);
      if (!buf)
        return NULL;
      size_t got = fread(buf, 1, (size_t)sz, fp);
      buf[got] = 0;
      if (out_len)
        *out_len = got;
      return buf;
    }
  }
  /* non-seekable: chunked grow */
  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;
  size_t got;
  char chunk[4096];
  while ((got = fread(chunk, 1, sizeof chunk, fp)) > 0) {
    if (len + got + 1 > cap) {
      size_t new_cap = cap;
      while (len + got + 1 > new_cap) {
#if UINTPTR_MAX <= 0xffffffff
        if (new_cap > SIZE_MAX / 2) {
          free(buf);
          return NULL; /* overflow guard */
        }
#endif
        new_cap *= 2;
      }
      char *nb = realloc(buf, new_cap);
      if (!nb) {
        free(buf);
        return NULL;
      }
      buf = nb;
      cap = new_cap;
    }
    memcpy(buf + len, chunk, got);
    len += got;
  }
  buf[len] = 0;
  if (out_len)
    *out_len = len;
  return buf;
}

static exp_t *eval_file_forms(const char *path, env_t *env) {
  FILE *fp = fopen(path, "r");
  if (!fp)
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "load: cannot open '%s'",
                 path);
  /* Dialect by extension: a ".adr" file is Adder surface syntax — slurp it,
     transpile to s-expressions via als_to_sexpr, and read the result from a
     memstream. ".alc" (and anything else) is read directly as s-expressions.
     This is what lets an .alc program (require) a .adr module and vice versa,
     in either binary. `transpiled` backs the memstream and is freed at the end.
   */
  /* Slurp the whole file: we both (maybe) transpile it and keep the text so an
     error can render a source-line caret. `filetext` is the original source
     (Adder or s-expr); `transpiled` (when set) is the generated s-expr that
     backs the reader for a .adr module. */
  size_t filelen = 0;
  char *filetext = slurp_stream(fp, &filelen);
  fclose(fp);
  if (!filetext)
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "load: cannot read '%s'",
                 path);
  char *transpiled = NULL;
  als_map amap = {0}; /* generated-line → Adder-line, for .adr error carets */
  size_t plen = strlen(path);
  if (plen >= 4 && strcmp(path + plen - 4, ".adr") == 0) {
    transpiled = als_to_sexpr_mapped(filetext, &amap);
    fp = transpiled ? fmemopen(transpiled, strlen(transpiled), "r") : NULL;
    if (!fp) {
      free(transpiled);
      free(filetext);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "require: failed to transpile Adder module '%s'", path);
    }
  } else {
    fp = fmemopen(filetext, filelen, "r");
    if (!fp) {
      free(filetext);
      return error(ERROR_ILLEGAL_VALUE, NULL, env, "load: cannot read '%s'",
                   path);
    }
  }
  /* Save/restore the reader location so a nested (load …) reports the inner
     file's lines and the outer file's count resumes afterwards. For a
     transpiled .adr module the reader counts lines in the GENERATED
     s-expressions, which don't map back to the source — so drop the label
     rather than print misleading "<file>:<line>:" (same call the main loop
     makes for Adder input). */
  const char *prev_src = g_reader_src;
  int prev_line = g_reader_line;
  const char *prev_srctext = g_reader_srctext;
  size_t prev_srctext_len = g_reader_srctext_len;
  als_map *prev_adder_map = g_adder_map;
  int prev_col = g_reader_col;
  long prev_off = g_reader_off;
  /* Label and caret-render against the ORIGINAL file text (Adder source for a
     .adr module); the source map translates the reader's generated lines back
     to Adder lines, so we keep the label instead of dropping it. */
  g_reader_src = src_basename(path);
  g_reader_srctext = filetext;
  g_reader_srctext_len = filelen;
  g_adder_map = transpiled ? &amap : NULL;
  g_reader_line = 1;
  g_reader_col = 1;
  g_reader_off = 0;
  /* Track this file's directory (for require's sibling-first search) and give
     the file a fresh namespace slate — (ns ...) it declares stays local to it
     and the outer file's namespace resumes afterward. */
  char *prev_dir = g_reader_dir;
  char *prev_ns = g_current_ns;
  g_reader_dir = path_dirname(path);
  g_current_ns = NULL;
  exp_t *result = TRUE_EXP;
  for (;;) {
    g_form_line = g_reader_line; /* fallback if no significant byte is read */
    g_form_line_arm = 1;         /* reader() stamps the true form-start line */
    exp_t *form = reader(fp, 0, 0);
    if (!form)
      break;
    if (iserror(form)) {
      if (form->flags == EXP_ERROR_PARSING_EOF) {
        unrefexp(form);
        break;
      }
      /* Reader syntax error: its line is g_reader_line at the failure point. */
      fclose(fp);
      result =
          annotate_error_loc(form, g_reader_src, display_line(g_reader_line));
      goto done;
    }
    exp_t *ret = evaluate(form, env);
    if (iserror(ret)) {
      fclose(fp);
      result = annotate_error_loc(ret, g_reader_src, display_line(g_form_line));
      goto done;
    }
    unrefexp(ret);
  }
  fclose(fp);
done:
  g_reader_src = prev_src;
  g_reader_line = prev_line;
  g_reader_srctext = prev_srctext;
  g_reader_srctext_len = prev_srctext_len;
  g_adder_map = prev_adder_map;
  g_reader_col = prev_col;
  g_reader_off = prev_off;
  free(g_reader_dir);
  g_reader_dir = prev_dir;
  /* Expose the namespace this file declared (if any) for require's :refer,
     then restore the outer file's namespace. */
  free(g_last_module_ns);
  g_last_module_ns = g_current_ns ? strdup(g_current_ns) : NULL;
  free(g_current_ns);
  g_current_ns = prev_ns;
  free(transpiled); /* NULL when the file was read directly (.alc) */
  free(filetext);   /* the slurped original source */
  als_map_free(&amap);
  return result;
}

const char doc_readstring[] =
    "(read-string path) — read the whole file as a string, or nil.";
exp_t *readstringcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(path);
  if (!isstring(path))
    CLEAN_RETURN_1(path, error(ERROR_ILLEGAL_VALUE, NULL, env,
                               "read-string: path must be a string"));
  exp_t *ret = slurp_file_as_string((char *)exp_text(path));
  CLEAN_RETURN_1(path, ret);
}

static exp_t *write_string_mode(exp_t *e, env_t *env, const char *mode,
                                const char *name) {
  EVAL_ARG_2(path, text);
  if (!isstring(path) || !isstring(text))
    CLEAN_RETURN_2(path, text,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "%s: path and text must be strings", (char *)name));
  FILE *fp = fopen((char *)exp_text(path), mode);
  if (!fp)
    CLEAN_RETURN_2(path, text,
                   error(ERROR_ILLEGAL_VALUE, NULL, env, "%s: cannot open '%s'",
                         (char *)name, (char *)exp_text(path)));
  size_t n = strlen((char *)exp_text(text));
  int ok = (n == 0 || fwrite((char *)exp_text(text), 1, n, fp) == n);
  fclose(fp);
  if (!ok)
    CLEAN_RETURN_2(path, text,
                   error(ERROR_ILLEGAL_VALUE, NULL, env, "%s: write failed",
                         (char *)name));
  CLEAN_RETURN_2(path, text, TRUE_EXP);
}

const char doc_writestring[] =
    "(write-string path text) — overwrite path with text. Returns t.";
exp_t *writestringcmd(exp_t *e, env_t *env) {
  return write_string_mode(e, env, "wb", "write-string");
}

const char doc_appendstring[] =
    "(append-string path text) — append text to path. Returns t.";
exp_t *appendstringcmd(exp_t *e, env_t *env) {
  return write_string_mode(e, env, "ab", "append-string");
}

const char doc_readlines[] =
    "(read-lines path) — read file into a list of line strings.";
exp_t *readlinescmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(path);
  if (!isstring(path))
    CLEAN_RETURN_1(path, error(ERROR_ILLEGAL_VALUE, NULL, env,
                               "read-lines: path must be a string"));
  exp_t *s = slurp_file_as_string((char *)exp_text(path));
  if (s == NIL_EXP)
    CLEAN_RETURN_1(path, NIL_EXP);
  exp_t *ret = NIL_EXP, *tail = NULL;
  char *p = (char *)exp_text(s);
  char *start = p;
  for (; *p; p++) {
    if (*p == '\n') {
      size_t n = (size_t)(p - start);
      if (n && start[n - 1] == '\r')
        n--;
      list_append_owned(&ret, &tail, make_string(start, (int)n));
      start = p + 1;
    }
  }
  if (*start)
    list_append_owned(&ret, &tail, make_string(start, (int)strlen(start)));
  unrefexp(s);
  CLEAN_RETURN_1(path, ret);
}

const char doc_fileexistsp[] = "(file-exists? path) — t if path exists.";
exp_t *fileexistspcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(path);
  if (!isstring(path))
    CLEAN_RETURN_1(path, error(ERROR_ILLEGAL_VALUE, NULL, env,
                               "file-exists?: path must be a string"));
  exp_t *ret = (access((char *)exp_text(path), F_OK) == 0) ? TRUE_EXP : NIL_EXP;
  CLEAN_RETURN_1(path, ret);
}

const char doc_writebytes[] =
    "(write-bytes path blob) — overwrite path with blob bytes. Returns t.";
exp_t *writebytescmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(path, blob);
  if (!isstring(path) || !isblob(blob))
    CLEAN_RETURN_2(path, blob,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "write-bytes: expected path string and blob"));
  FILE *fp = fopen((char *)exp_text(path), "wb");
  if (!fp)
    CLEAN_RETURN_2(path, blob,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "write-bytes: cannot open '%s'",
                         (char *)exp_text(path)));
  alc_blob_t *b = (alc_blob_t *)blob->ptr;
  int ok = (!b || b->len == 0 || fwrite(b->bytes, 1, b->len, fp) == b->len);
  fclose(fp);
  if (!ok)
    CLEAN_RETURN_2(
        path, blob,
        error(ERROR_ILLEGAL_VALUE, NULL, env, "write-bytes: write failed"));
  CLEAN_RETURN_2(path, blob, TRUE_EXP);
}

const char doc_load[] = "(load path) — read and evaluate an Alcove file.";
exp_t *loadcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(path);
  if (!isstring(path))
    CLEAN_RETURN_1(path, error(ERROR_ILLEGAL_VALUE, NULL, env,
                               "load: path must be a string"));
  exp_t *ret = eval_file_forms((char *)exp_text(path), env);
  CLEAN_RETURN_1(path, ret);
}

const char doc_ns[] =
    "(ns name) — set the current namespace. While active, top-level "
    "def/defn/defc/defmacro auto-qualify their name to name/<symbol>; "
    "reference them from elsewhere as name/symbol. Stays active until the next "
    "(ns ...) or the end of the file being loaded. (ns) with no arg clears it.";
exp_t *nscmd(exp_t *e, env_t *env) {
  /* Like def, (ns) mutates global load state — refuse from a RESP callback. */
  if (g_resp_cb_guard) {
    unrefexp(e);
    return resp_cb_readonly_error(env);
  }
  /* Name is taken literally (unevaluated), like def's name. (ns) clears. */
  exp_t *arg = cadr(e);
  if (arg && !issymbol(arg) && !isstring(arg)) {
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "ns: name must be a bare symbol (or string), or omitted");
  }
  free(g_current_ns);
  g_current_ns = arg ? strdup(exp_text(arg)) : NULL;
  unrefexp(e);
  return TRUE_EXP;
}

/* require's load-once + cycle bookkeeping, keyed by canonical (realpath) path.
   Plain globals, not TLS: module loading is a startup/main-thread activity and
   is already refused from RESP callbacks (g_resp_cb_guard). g_loaded =
   fully-loaded modules; g_loading = modules whose load is in progress (cycle
   guard — a require that re-enters an in-flight module returns immediately). */
static dict_t *g_loaded_modules = NULL;
static dict_t *g_loading_modules = NULL;

/* Write "<dir>/<name>" (or just "<name>" when dir is NULL) into out and return
   1 if that file exists, else 0. The single snprintf+access check shared by all
   of resolve_module_path's search branches. */
static int module_file_at(char *out, const char *dir, const char *name) {
  if (dir) {
    if (snprintf(out, PATH_MAX, "%s/%s", dir, name) >= PATH_MAX)
      return 0;
  } else {
    snprintf(out, PATH_MAX, "%.*s", (int)(PATH_MAX - 1), name);
  }
  return access(out, F_OK) == 0;
}

/* Try every candidate basename (cand[0..ncand)) under `dir` (NULL = as-is),
   returning 1 and writing the first hit into out. Candidates are ordered by
   preference (.alc before .adr), so a sibling .alc wins over a sibling .adr. */
static int module_try_dir(char *out, const char *dir, char cand[][PATH_MAX],
                          int ncand) {
  for (int c = 0; c < ncand; c++)
    if (module_file_at(out, dir, cand[c]))
      return 1;
  return 0;
}

/* Resolve a require spec to an existing file path. With an explicit ".alc" or
   ".adr" suffix the spec is used verbatim; otherwise BOTH "<spec>.alc" and
   "<spec>.adr" are tried (so a program can require either dialect by bare
   name, .alc preferred). Search order: absolute → used as-is; else the
   requiring file's directory (g_reader_dir), then each ':'-separated entry of
   $ALCOVE_PATH, then cwd. Writes the first hit into out (size PATH_MAX) and
   returns 1; returns 0 if nothing exists. */
static int resolve_module_path(const char *spec, char *out) {
  size_t sl = strlen(spec);
  /* An explicit source (.alc/.adr) or native-module (.so/.dylib) extension is
     used verbatim; a bare name is tried as <spec>.alc then <spec>.adr (source
     modules are primary — a native module must be named with its extension). */
  int explicit_ext = (sl >= 4 && (strcmp(spec + sl - 4, ".alc") == 0 ||
                                  strcmp(spec + sl - 4, ".adr") == 0)) ||
                     (sl >= 3 && strcmp(spec + sl - 3, ".so") == 0) ||
                     (sl >= 6 && strcmp(spec + sl - 6, ".dylib") == 0);
  char cand[2][PATH_MAX];
  int ncand = 0;
  /* Bound the spec with a precision so the ".alc"/".adr" suffix always fits —
     a spec too long to hold a 4-char suffix + NUL simply won't resolve. */
  if (sl >= PATH_MAX - 5)
    return 0;
  if (explicit_ext) {
    snprintf(cand[0], PATH_MAX, "%s", spec);
    ncand = 1;
  } else {
    snprintf(cand[0], PATH_MAX, "%.*s.alc", (int)(PATH_MAX - 5), spec);
    snprintf(cand[1], PATH_MAX, "%.*s.adr", (int)(PATH_MAX - 5), spec);
    ncand = 2;
  }
  if (cand[0][0] == '/') /* absolute → used as-is, no search */
    return module_try_dir(out, NULL, cand, ncand);
  /* 1) relative to the requiring file's directory */
  if (g_reader_dir && module_try_dir(out, g_reader_dir, cand, ncand))
    return 1;
  /* 2) each dir in $ALCOVE_PATH */
  const char *ap = getenv("ALCOVE_PATH");
  if (ap && *ap) {
    const char *p = ap;
    while (*p) {
      const char *colon = strchr(p, ':');
      size_t len = colon ? (size_t)(colon - p) : strlen(p);
      if (len > 0 && len < PATH_MAX) {
        char dir[PATH_MAX];
        memcpy(dir, p, len);
        dir[len] = '\0';
        if (module_try_dir(out, dir, cand, ncand))
          return 1;
      }
      if (!colon)
        break;
      p = colon + 1;
    }
  }
  /* 3) cwd (the spec as given) */
  return module_try_dir(out, NULL, cand, ncand);
}

/* Bind the unqualified <bare> in the global env to whatever <ns>/<bare> is
   currently bound to, so callers can use the short name. Returns 1 if the
   qualified name existed (and was aliased), 0 otherwise. */
static int refer_one(const char *ns, const char *bare) {
  if (!g_global_env || !g_global_env->d)
    return 0;
  char *q = NULL;
  if (asprintf(&q, "%s/%s", ns, bare) < 0 || !q)
    return 0;
  keyval_t *kv = set_get_keyval_dict(g_global_env->d, q, NULL);
  free(q);
  if (!kv)
    return 0;
  /* set_get_keyval_dict refs the value itself + unrefs any prior binding. */
  set_get_keyval_dict(g_global_env->d, (char *)bare, kv->val);
  return 1;
}

/* Alias every <ns>/<name> binding to its unqualified <name>. Collects the hits
   first, then binds — inserting into env->d can rehash it, so we must not
   insert while walking it. Nested-namespace names (suffix still has a '/') are
   left qualified. */
static void refer_all(const char *ns) {
  if (!g_global_env || !g_global_env->d)
    return;
  dict_t *dp = g_global_env->d;
  size_t nslen = strlen(ns), n = 0;
  /* A key is in namespace ns iff it starts with "ns/" and the rest is a single
     unqualified segment (no further '/'). */
#define REFER_MATCH(kk)                                                        \
  (strncmp((kk), ns, nslen) == 0 && (kk)[nslen] == '/' && (kk)[nslen + 1] &&   \
   !strchr((kk) + nslen + 1, '/'))
  for (unsigned int i = 0; i < dp->ht[0].size; i++)
    for (keyval_t *k = dp->ht[0].table[i]; k; k = k->next)
      if (REFER_MATCH((const char *)k->key))
        n++;
  if (!n)
    return;
  char **names = memalloc(n, sizeof(char *));
  exp_t **vals = memalloc(n, sizeof(exp_t *));
  size_t j = 0;
  for (unsigned int i = 0; i < dp->ht[0].size; i++)
    for (keyval_t *k = dp->ht[0].table[i]; k; k = k->next) {
      const char *key = (const char *)k->key;
      if (REFER_MATCH(key)) {
        names[j] = strdup(key + nslen + 1);
        vals[j] = k->val;
        j++;
      }
    }
#undef REFER_MATCH
  for (j = 0; j < n; j++) {
    set_get_keyval_dict(dp, names[j], vals[j]);
    free(names[j]);
  }
  free(names);
  free(vals);
  GEN_BUMP();
}

/* Defined after the ffi.h include (it uses dlopen). A .so/.dylib require routes
   here instead of eval_file_forms. */
static exp_t *load_native_module(const char *path, const char *spec,
                                 env_t *env);

const char doc_require[] =
    "(require \"path\") — load an Alcove module once. \".alc\" is appended if "
    "absent; the file is searched relative to the requiring file, then "
    "$ALCOVE_PATH, then cwd. A \".so\"/\".dylib\" path instead loads a native "
    "module (dlopen + its alcove_module_init). Already-loaded (or mid-load, "
    "for "
    "cycles) modules are not re-run. Returns t when it loaded, nil when "
    "already "
    "loaded. "
    "(require \"path\" :refer) also binds every name the module's namespace "
    "defines unqualified; (require \"path\" :refer a b) binds only a and b — "
    "so "
    "you can call `parse` instead of `json/parse`.";
exp_t *requirecmd(exp_t *e, env_t *env) {
  if (g_resp_cb_guard) {
    unrefexp(e);
    return resp_cb_readonly_error(env);
  }
  EVAL_ARG_1(spec);
  if (!isstring(spec))
    CLEAN_RETURN_1(spec, error(ERROR_ILLEGAL_VALUE, NULL, env,
                               "require: argument must be a string"));
  char found[PATH_MAX];
  if (!resolve_module_path((char *)exp_text(spec), found))
    CLEAN_RETURN_1(spec, error(ERROR_ILLEGAL_VALUE, NULL, env,
                               "require: module not found: '%s' (searched "
                               "requiring dir, $ALCOVE_PATH, cwd)",
                               (char *)exp_text(spec)));
  /* Canonical path is the dedup key (so "a.alc" and "./a.alc" are one module).
   */
  char canon[PATH_MAX];
  if (!realpath(found, canon))
    snprintf(canon, sizeof(canon), "%s",
             found); /* fall back to the found path */
  if (!g_loaded_modules)
    g_loaded_modules = create_dict();
  if (!g_loading_modules)
    g_loading_modules = create_dict();

  /* Load (unless already loaded / mid-load), recording the module's declared
     namespace so a later :refer works even when it was loaded earlier. The
     g_loaded_modules value is the ns string, or TRUE_EXP for a ns-less module.
   */
  const char *module_ns = NULL;
  keyval_t *loaded = set_get_keyval_dict(g_loaded_modules, canon, NULL);
  if (loaded) {
    module_ns = isstring(loaded->val) ? exp_text(loaded->val) : NULL;
  } else if (set_get_keyval_dict(g_loading_modules, canon, NULL)) {
    CLEAN_RETURN_1(spec, NIL_EXP); /* cyclic: mid-load, names not all defined */
  } else {
    set_get_keyval_dict(g_loading_modules, canon, TRUE_EXP);
    size_t fl = strlen(found);
    int is_native = (fl >= 3 && strcmp(found + fl - 3, ".so") == 0) ||
                    (fl >= 6 && strcmp(found + fl - 6, ".dylib") == 0);
    if (is_native) {
      /* Native module: dlopen + alcove_module_init registers its own builtins.
         No source namespace, so :refer below is a no-op (the module names its
         own exports, qualified as it likes). */
      exp_t *nret = load_native_module(canon, (char *)exp_text(spec), env);
      /* canon: the realpath we deduped on (avoids a symlink swap between check
         & load); spec: the user's arg, recorded for db re-(require). */
      del_keyval_dict(g_loading_modules, canon);
      if (iserror(nret))
        CLEAN_RETURN_1(spec, nret);
      set_get_keyval_dict(g_loaded_modules, canon, TRUE_EXP);
      CLEAN_RETURN_1(spec, TRUE_EXP);
    }
    /* require always loads into the GLOBAL env, regardless of caller scope, so
       a module's defs are top-level (and (ns ...)-qualifiable). */
    exp_t *ret = eval_file_forms(canon, g_global_env); /* load the realpath we
                     deduped on (canon), not the unresolved hit (found) */
    del_keyval_dict(g_loading_modules, canon);
    if (iserror(ret))
      CLEAN_RETURN_1(spec, ret); /* propagate; don't mark loaded */
    unrefexp(ret);
    /* Record the module's declared namespace (string), or TRUE_EXP if none, so
       a later :refer on this already-loaded module still knows the prefix. */
    exp_t *nsval = g_last_module_ns ? make_string(g_last_module_ns,
                                                  (int)strlen(g_last_module_ns))
                                    : TRUE_EXP;
    set_get_keyval_dict(g_loaded_modules, canon, nsval); /* dict refs it */
    if (nsval != TRUE_EXP)
      unrefexp(nsval);
    module_ns =
        g_last_module_ns; /* borrowed — stable for the rest of this call */
  }

  /* Optional :refer. `:refer` followed by no names (or only a keyword like
     :all) imports EVERY name the module's namespace defines, unqualified;
     otherwise each symbol following :refer is imported on its own. A ns-less
     module's names are already global, so :refer is a no-op there. */
  exp_t *kw = caddr(e);
  if (module_ns && kw && issymbol(kw) &&
      strcmp((char *)exp_text(kw), ":refer") == 0) {
    int any = 0;
    for (exp_t *n = e->next->next->next; n; n = n->next) {
      if (!issymbol(n->content) || ((char *)exp_text(n->content))[0] == ':')
        continue; /* skip a stray keyword like :all */
      any = 1;
      if (!refer_one(module_ns, (char *)exp_text(n->content)))
        CLEAN_RETURN_1(spec, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                   "require :refer: %s/%s is not defined",
                                   module_ns, (char *)exp_text(n->content)));
    }
    if (any)
      GEN_BUMP();
    else
      refer_all(module_ns); /* :refer with no explicit names → import all */
  }
  CLEAN_RETURN_1(spec, loaded ? NIL_EXP : TRUE_EXP);
}

/* db-load auto-(require): resolve `spec` and, if it's a native module, dlopen +
   alcove_module_init it so its custom types register. Best-effort — failure
   leaves the type unresolved and the value's load aborts. Gated by --safe at
   the call site (alcove_load_unified). */
static void auto_require_native(const char *spec) {
  if (!spec || !*spec)
    return;
  char found[PATH_MAX];
  if (!resolve_module_path(spec, found))
    return;
  size_t fl = strlen(found);
  int native = (fl >= 3 && strcmp(found + fl - 3, ".so") == 0) ||
               (fl >= 6 && strcmp(found + fl - 6, ".dylib") == 0);
  if (!native)
    return; /* only native modules define custom types */
  char canon[PATH_MAX];
  if (!realpath(found, canon))
    snprintf(canon, sizeof(canon), "%s", found);
  exp_t *r = load_native_module(canon, spec, g_global_env);
  if (r && iserror(r))
    unrefexp(r);
}

/* Forward decls needed by HOFs and alc_apply helpers below. */
static exp_t *vm_invoke_values(exp_t *fn, int nargs, exp_t **argv, env_t *env);
static exp_t *alc_apply_n(exp_t *fn, int nargs, exp_t **argv, env_t *env);
static exp_t *make_cont_escape(int64_t id, exp_t *payload, env_t *env);
static exp_t *alc_apply1(exp_t *fn, exp_t *arg, env_t *env);
static exp_t *alc_apply2(exp_t *fn, exp_t *a, exp_t *b, env_t *env);

/* ---------------- FFI (libffi-backed) ----------------
   Lets alcove call into shared libraries (libc, libm, custom .so's).
   Each `(ffi-fn lib name rtype atype1 atype2 ...)` call returns an
   exp_t of type EXP_FFI carrying the resolved fn pointer + ffi_cif.
   Calling an EXP_FFI value marshals alcove args → C ABI, invokes via
   ffi_call, and marshals the result back.

   Conditional on -DALCOVE_FFI=1 (Makefile auto-detects via ffi.h). */
/* (ffi?) — runtime probe for FFI support. Always compiled (outside the
   ALCOVE_FFI block) so scripts/tests can branch on it; lets test.alc guard
   its FFI assertions so a no-libffi build skips them uniformly. */
const char doc_ffip[] =
    "(ffi?) — t if this build links libffi (ffi-fn/-callback/-struct usable), "
    "else nil.";
exp_t *ffipcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
#ifdef ALCOVE_FFI
  return refexp(TRUE_EXP);
#else
  return refexp(NIL_EXP);
#endif
}

const char doc_ffifn[] =
    "(ffi-fn lib name ret arg-types ...) — bind a C function from a shared "
    "library. Types: void int long uint ulong ptr cstr double float. "
    "An empty lib name (\"\") resolves symbols already linked into alcove "
    "(libc/libm). ALCOVE_FFI build only.";
const char doc_ffivfn[] =
    "(ffi-vfn lib name ret fixed-arg-types...) — bind a VARIADIC C function "
    "(e.g. printf). The given arg types are the fixed prefix; extra call args "
    "are passed with types inferred from their value (int->long use %ld, "
    "float->double, char->int, string->%s). ALCOVE_FFI build only.";
const char doc_fficallback[] =
    "(ffi-callback ret (arg-types...) fn) — expose an alcove fn to C as a "
    "function pointer (e.g. a qsort comparator). Pass the result where a ptr "
    "arg is expected. ret/args: void int long double ptr (no string return). "
    "ALCOVE_FFI build only.";
const char doc_ffistruct[] =
    "(ffi-struct field-types...) — define a by-value C struct type. Fields: "
    "int long double ptr. Returns a descriptor usable as an ffi-fn arg/return "
    "type and with ffi-pack / ffi-unpack. ALCOVE_FFI build only.";
const char doc_ffipack[] =
    "(ffi-pack struct-desc vals...) — pack field values into a blob laid out "
    "per the C ABI for struct-desc. Pass the blob where a struct arg is "
    "expected. ALCOVE_FFI build only.";
const char doc_ffiunpack[] =
    "(ffi-unpack struct-desc blob) — read a packed struct blob back into a "
    "list of field values (the inverse of ffi-pack). ALCOVE_FFI build only.";
/* The libffi binding (impl + non-FFI stubs) lives in a dedicated
   #included fragment. */
#include "ffi.h"

/* Load a native (shared-library) module: dlopen `path`, then call its
   `int alcove_module_init(void)` hook (returns 0 on success), which registers
   builtins via alcove_register_cmd. Returns TRUE_EXP on success or an error.
   Needs an FFI-enabled build: the dlopen machinery + -rdynamic (so the module
   resolves the host's alcove_register_cmd / make_* symbols at load). Defined
   here, after ffi.h, where dlopen/dlsym and alc_ffi_dlopen are available;
   forward-declared up by requirecmd. */
/* path = the file to dlopen (canonical); spec = the user's original require
   argument, recorded against any type the module registers so a db.dump can
   re-(require) it on load. */
static exp_t *load_native_module(const char *path, const char *spec,
                                 env_t *env) {
#ifdef ALCOVE_FFI
  void *h = alc_ffi_dlopen(path);
  if (!h)
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "require: cannot load native module '%s': %s", path,
                 dlerror());
  /* void* → function pointer: not strictly portable C, but POSIX dlsym
     guarantees it (memcpy to dodge the -pedantic cast warning). */
  void *sym = dlsym(h, "alcove_module_init");
  if (!sym)
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "require: native module '%s' exports no alcove_module_init",
                 path);
  /* ABI guard: if the module declares the embedding-API version it was built
     against, refuse a mismatch here rather than let an incompatible exp_t/env_t
     layout corrupt silently. A module without the (optional) symbol predates
     the convention and is loaded as before. */
  void *abisym = dlsym(h, "alcove_module_abi");
  if (abisym) {
    int (*abi)(void);
    memcpy(&abi, &abisym, sizeof abi);
    int mod_abi = abi();
    if (mod_abi != ALCOVE_API_VERSION)
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "require: native module '%s' built against alcove API v%d, "
                   "but this host is API v%d — rebuild the module",
                   path, mod_abi, ALCOVE_API_VERSION);
  }
  int (*init)(void);
  memcpy(&init, &sym, sizeof init);
  const char *prev_spec = g_current_module_spec;
  g_current_module_spec = spec; /* alcove_register_type reads this */
  int rc = init();
  g_current_module_spec = prev_spec;
  if (rc != 0)
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "require: native module '%s' alcove_module_init failed", path);
  GEN_BUMP(); /* new builtins registered → invalidate global-resolution caches
               */
  return TRUE_EXP;
#else
  (void)path;
  (void)spec;
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "require: native (.so/.dylib) modules need an FFI-enabled build "
               "(rebuild with libffi installed)");
#endif
}

/* The standard-library builtins live in a dedicated #included fragment. */
#include "builtins_log.h"
#include "builtins_os.h"
#include "builtins_regex.h"
#include "builtins_stdlib.h"

#include "pp.h"
const char doc_cons[] = "(cons x ys) — pair with car=x, cdr=ys. To prepend "
                        "onto a list: (cons elem list).";
exp_t *conscmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(a, b);
  exp_t *ret = make_node(a);
  /* Keep ANY non-nil cdr, including falsey-but-non-nil values like 0, "",
     an empty vec, or a function — only a literal nil terminates the list.
     The old test was istrue(b), which dropped a 0/"" cdr: (cons 1 0) gave
     (1) instead of the improper pair (1 . 0). */
  if (b && b != NIL_EXP)
    ret->next = b;
  else {
    if (b)
      unrefexp(b); /* b == NIL_EXP — immortal, unref is a no-op */
    ret->next = NULL;
  }
  unrefexp(e);
  return ret;
}

const char doc_cdr[] = "(cdr xs) — tail of a pair / rest of a list.";
PAIR_PART_CMD(cdrcmd, cdr)

const char doc_car[] = "(car xs) — head of a pair / first element of a list.";
PAIR_PART_CMD(carcmd, car)

const char doc_list[] =
    "(list x ...) — construct a list of all args. (list) is nil.";
exp_t *listcmd(exp_t *e, env_t *env) {
  exp_t *a = cdr(e);
  exp_t *tmpexp = NULL;
  exp_t *ret = NULL;
  exp_t *cur = NULL;
  if (!a) {
    unrefexp(e);
    return NIL_EXP;
  }
  tmpexp = EVAL(car(a), env);
  if iserror (tmpexp)
    goto error;
  ret = make_node(tmpexp);
  tmpexp = NULL;
  cur = ret;
  while ((a = a->next)) {
    tmpexp = EVAL(car(a), env);
    if iserror (tmpexp) {
      unrefexp(ret);
      goto error;
    }
    cur = cur->next = make_node(tmpexp);
  }
  unrefexp(e);
  return ret;
error:
  unrefexp(e);
  return tmpexp;
}

const char doc_eval[] =
    "(eval expr) — evaluate a quoted expression as code in the current env.";
exp_t *evalcmd(exp_t *e, env_t *env) {
  exp_t *ret = evaluate(EVAL(cadr(e), env), env);
  unrefexp(e);
  return ret;
}

static void var2env_bind(char *name, exp_t *val, env_t *env);

/* Shapes the first argument of a `let` can take. Shared by letcmd (AST) and
   compile_let (bytecode) so both agree. The parens disambiguate:
     (let x 5 body)               LET_SINGLE      — bare symbol
     (let (a b) listval body)     LET_DESTRUCTURE — all-symbol list + a val arg
     (let (x 5 y 6) body)         LET_FLAT        — name/value pairs flattened
     (let ((x 5) (y 6)) body)     LET_CLOJURE     — list of (name val) pairs
   FLAT/CLOJURE are the universally-expected forms; they used to error (mis-read
   as destructuring), so accepting them is backward-compatible. An all-symbol
   list stays destructuring (it needs the separate value arg). */
typedef enum {
  LET_SINGLE,
  LET_DESTRUCTURE,
  LET_FLAT,
  LET_CLOJURE,
  LET_BAD
} let_shape_t;

/* A valid binding NAME is a symbol that isn't a keyword. Keywords (:foo) are
   symbols too, but they self-evaluate as values, so a name list like (k :v)
   must be read as a flat name/value pair, NOT an all-symbol destructure list.
 */
#define IS_LET_NAME(e) (issymbol(e) && ((const char *)exp_text(e))[0] != ':')

static let_shape_t let_classify(exp_t *first) {
  if (first && issymbol(first))
    return first && ((const char *)exp_text(first))[0] == ':' ? LET_BAD
                                                              : LET_SINGLE;
  if (!ispair(first))
    return LET_BAD;
  if (!first->content)
    return LET_FLAT; /* () — zero bindings */
  int all_names = 1, all_pairs = 1, n = 0;
  for (exp_t *p = first; p && p->content; p = p->next) {
    exp_t *el = p->content;
    n++;
    if (!IS_LET_NAME(el))
      all_names = 0;
    if (!(ispair(el) && el->content && IS_LET_NAME(el->content) && el->next &&
          el->next->content))
      all_pairs = 0;
  }
  if (all_pairs)
    return LET_CLOJURE; /* ((x 5) (y 6)) */
  if (all_names)
    return LET_DESTRUCTURE; /* (a b) + listval */
  if (n % 2 == 0) {         /* (x 5 y 6): even count, names in even positions */
    int i = 0;
    for (exp_t *p = first; p && p->content; p = p->next, i++)
      if ((i & 1) == 0 && !IS_LET_NAME(p->content))
        return LET_BAD;
    return LET_FLAT;
  }
  return LET_BAD;
}

const char doc_let[] =
    "(let (x 5 y 6) body...) or (let ((x 5) (y 6)) body...) — parallel "
    "bindings, local to body. (let x 5 body) — single bare binding. "
    "(let (a b) listval body) — destructure listval (all-symbol list) into a, "
    "b "
    "(missing elements get nil).";
exp_t *letcmd(exp_t *e, env_t *env) {
  int outer_tail = in_tail_position;
  env_t *newenv = make_env(env);
  exp_t *ret;
  exp_t *curvar;
  exp_t *curval;

  exp_t *body = NULL;
  if (!(curvar = e->next)) {
    ret = error(ERROR_MISSING_PARAMETER, e, env, "Missing parameter in let");
    goto finish;
  }
  exp_t *first = curvar->content;
  let_shape_t shape = let_classify(first);
  in_tail_position = 0;

  if (shape == LET_SINGLE) {
    if (!(curval = curvar->next)) {
      ret = error(ERROR_MISSING_PARAMETER, e, env, "Missing parameter in let");
      goto finish;
    }
    CHECK_RESERVED_BIND(first, ret, "in let", goto finish);
    if ((ret = EVAL(curval->content, env)) == NULL)
      ret = NIL_EXP;
    if iserror (ret)
      goto finish;
    var2env_bind(exp_text(first), ret, newenv);
    ret = NULL;
    body = curval->next;
  } else if (shape == LET_DESTRUCTURE) {
    /* (let (a b ...) val body): eval val once, bind each name to the matching
       list element; names without a matching element get nil. */
    if (!(curval = curvar->next)) {
      ret = error(ERROR_MISSING_PARAMETER, e, env, "Missing parameter in let");
      goto finish;
    }
    ret = EVAL(curval->content, env);
    if (ret == NULL)
      ret = NIL_EXP;
    if (iserror(ret))
      goto finish;
    exp_t *dnames = first, *dvals = ret;
    while (dnames && ispair(dnames) && istrue(dnames)) {
      exp_t *nm = dnames->content;
      int have_val = dvals && ispair(dvals) && istrue(dvals);
      var2env_bind(exp_text(nm), refexp(have_val ? dvals->content : NIL_EXP),
                   newenv);
      dnames = dnames->next;
      if (have_val)
        dvals = dvals->next;
    }
    unrefexp(ret);
    ret = NULL;
    body = curval->next;
  } else if (shape == LET_FLAT) {
    /* (let (v1 e1 v2 e2 ...) body): parallel — each ei is evaluated in the
       OUTER env, then all are bound. */
    for (exp_t *p = (first && first->content) ? first : NULL;
         p && p->content;) {
      exp_t *nm = p->content, *vp = p->next;
      CHECK_RESERVED_BIND(nm, ret, "in let", goto finish);
      ret = EVAL(vp->content, env);
      if (iserror(ret))
        goto finish;
      var2env_bind(exp_text(nm), ret, newenv);
      ret = NULL;
      p = vp->next;
    }
    body = curvar->next;
  } else if (shape == LET_CLOJURE) {
    /* (let ((v1 e1) (v2 e2) ...) body): same, pairs grouped. */
    for (exp_t *p = first; p && p->content; p = p->next) {
      exp_t *pair = p->content, *nm = pair->content;
      CHECK_RESERVED_BIND(nm, ret, "in let", goto finish);
      ret = EVAL(pair->next->content, env);
      if (iserror(ret))
        goto finish;
      var2env_bind(exp_text(nm), ret, newenv);
      ret = NULL;
    }
    body = curvar->next;
  } else {
    ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in let");
    goto finish;
  }

  if (!body) {
    ret = error(ERROR_MISSING_PARAMETER, e, env, "Missing parameter in let");
    goto finish;
  }
  while (body->next) {
    in_tail_position = 0;
    ret = EVAL(body->content, newenv);
    if (iserror(ret))
      goto finish;
    unrefexp(ret);
    ret = NULL;
    body = body->next;
  }
  in_tail_position = outer_tail;
  ret = EVAL(body->content, newenv);
finish:
  in_tail_position = outer_tail;
  destroy_env(newenv);
  unrefexp(e);
  return ret;
}

const char doc_with[] = "(with (var1 val1 var2 val2 ...) body) — bind all "
                        "pairs in parallel, then evaluate body.";
exp_t *withcmd(exp_t *e, env_t *env) {
  int outer_tail = in_tail_position;
  env_t *newenv = make_env(env);
  exp_t *ret;
  exp_t *curvar;
  exp_t *curval;
  exp_t *curex;

  if ((curvar = e->next)) {
    if (!ispair(curvar->content)) {
      ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in with");
      goto finish;
    }
    if ((curex = curvar->next)) {
      curvar = curvar->content;
      if ((curval = curvar->next)) {
        in_tail_position = 0;
        while (curvar && curval) {
          if (issymbol(curvar->content)) {
            CHECK_RESERVED_BIND(curvar->content, ret, "in with", goto finish);
            ret = EVAL(curval->content, env);
            if iserror (ret)
              goto finish;
            var2env_bind(exp_text(curvar->content), ret, newenv);
            ret = NULL;
          }

          else {
            ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in with");
            goto finish;
          }
          curvar = curval->next;
          if (curvar)
            curval = curvar->next;
          if (!curval) {
            ret = error(ERROR_MISSING_PARAMETER, e, env,
                        "Missing parameter in with");
            goto finish;
          }
        }

        while (curex->next) {
          in_tail_position = 0;
          ret = EVAL(curex->content, newenv);
          if (iserror(ret))
            goto finish;
          unrefexp(ret);
          ret = NULL;
          curex = curex->next;
        }
        in_tail_position = outer_tail;
        ret = EVAL(curex->content, newenv);
        goto finish;
      }
    }
  }

  ret = error(ERROR_MISSING_PARAMETER, e, env, "Missing parameter in with");
finish:
  in_tail_position = outer_tail;
  destroy_env(newenv);
  unrefexp(e);
  return ret;
}

const char doc_letstar[] =
    "(let* (v1 val1 v2 val2 ...) body ...) — sequential bindings: each val "
    "is evaluated in the scope that includes all preceding bindings. The "
    "flat form (let* v1 val1 ... single-body) is also accepted for the "
    "single-body case.";
exp_t *letstar_cmd(exp_t *e, env_t *env) {
  int outer_tail = in_tail_position;
  exp_t *cur = cdr(e);
  if (!cur || !cur->next) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, NULL, env,
                 "let*: need bindings and a body");
  }
  env_t *newenv = make_env(env);
  exp_t *ret = NULL;
  in_tail_position = 0;

  /* New form: bindings collected into a single list — supports multi-body.
     (let* (v1 val1 v2 val2 ...) body ...) */
  if (ispair(cur->content)) {
    exp_t *bcur = cur->content;
    exp_t *body_start = cur->next;
    while (bcur && bcur->content) {
      exp_t *var = bcur->content;
      if (!issymbol(var)) {
        ret = error(ERROR_ILLEGAL_VALUE, NULL, newenv,
                    "let*: binding name must be a symbol");
        goto finish;
      }
      CHECK_RESERVED_BIND(var, ret, "in let*", goto finish);
      exp_t *val_node = bcur->next;
      if (!val_node) {
        ret = error(ERROR_MISSING_PARAMETER, NULL, newenv,
                    "let*: binding without value");
        goto finish;
      }
      ret = EVAL(val_node->content, newenv);
      if (iserror(ret))
        goto finish;
      var2env_bind(exp_text(var), ret, newenv);
      ret = NULL;
      bcur = val_node->next;
    }
    exp_t *body = body_start;
    while (body && body->next) {
      in_tail_position = 0;
      ret = EVAL(body->content, newenv);
      if (iserror(ret))
        goto finish;
      unrefexp(ret);
      ret = NULL;
      body = body->next;
    }
    in_tail_position = outer_tail;
    ret = body ? EVAL(body->content, newenv) : NIL_EXP;
    goto finish;
  }

  /* Old form: flat pairs followed by exactly one body expression.
     (let* v1 val1 v2 val2 ... body) */
  if (!cur->next->next) {
    ret = error(ERROR_MISSING_PARAMETER, NULL, env,
                "let*: need at least one binding and a body");
    goto finish;
  }
  while (cur && cur->next && cur->next->next) {
    exp_t *var = cur->content;
    exp_t *val_node = cur->next;
    if (!issymbol(var)) {
      ret = error(ERROR_ILLEGAL_VALUE, NULL, newenv,
                  "let*: binding name must be a symbol");
      goto finish;
    }
    CHECK_RESERVED_BIND(var, ret, "in let*", goto finish);
    ret = EVAL(val_node->content, newenv);
    if (iserror(ret))
      goto finish;
    var2env_bind(exp_text(var), ret, newenv);
    ret = NULL;
    cur = val_node->next;
  }
  in_tail_position = outer_tail;
  ret = cur ? EVAL(cur->content, newenv) : NIL_EXP;
finish:
  in_tail_position = outer_tail;
  destroy_env(newenv);
  unrefexp(e);
  return ret;
}

/* Control-flow builtins (cond / match / generators) live in a dedicated
   #included fragment. */
#include "builtins_control.h"

#include "compiler.h"

#ifdef ALCOVE_JIT
/* JIT backends live in dedicated #included fragments (one TU — see each
   file's header). The arch guards pick exactly one backend per build. */
#include "jit_common.h"
#if defined(__aarch64__)
#include "jit_arm64.h"
#elif defined(__x86_64__)
#include "jit_amd64.h"
#endif
#endif /* ALCOVE_JIT */

#include "compiler_impl.h"

static exp_t *invoke_body(exp_t *e, exp_t *fn, env_t *env);

#include "debugger.h"
/* Help / discovery — both implemented at file scope so they can scan
   lispProcList[] (defined at the top of this file). doccmd looks up a
   single symbol; helpcmd lists everything (or, given a name, defers to
   doccmd). The arity/level fields of lispProc are reserved (see the
   struct comment); doc is the only field we surface here. */
const char doc_doc[] = "(doc name) — print the documentation for a builtin.";
const char doc_help[] =
    "(help) — list every builtin and its docstring. (help name) is (doc name).";

/* Resolve the arg to a name string. Accepts:
     (doc cons)    — bare symbol (literal head — common interactive form)
     (doc 'cons)   — quoted symbol (evaluation yields the symbol)
     (doc "cons")  — string literal
   On success returns a borrowed const char*; on failure returns NULL after
   *err is set to the error exp_t to propagate. The caller is responsible
   for unrefing eval_owned if it's non-NULL (the buffer holding evaluated
   args we need to keep alive across the printf). */
static const char *doc_resolve_name(exp_t *arg, env_t *env, exp_t **eval_owned,
                                    exp_t **err) {
  *eval_owned = NULL;
  *err = NULL;
  if (issymbol(arg))
    return (const char *)exp_text(arg);
  if (isstring(arg))
    return (const char *)exp_text(arg);
  /* Anything else: evaluate and look again. */
  exp_t *v = EVAL(arg, env);
  if (iserror(v)) {
    *err = v;
    return NULL;
  }
  if (is_ptr(v) && (issymbol(v) || isstring(v))) {
    *eval_owned = v;
    return (const char *)exp_text(v);
  }
  unrefexp(v);
  *err = error(ERROR_ILLEGAL_VALUE, NULL, env,
               "doc/help: argument must be a symbol or string");
  return NULL;
}

exp_t *doccmd(exp_t *e, env_t *env) {
  exp_t *arg = cadr(e);
  if (!arg) {
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "doc: expected a name, e.g. (doc cons) or (doc 'cons)");
  }
  exp_t *owned = NULL, *err = NULL;
  const char *name = doc_resolve_name(arg, env, &owned, &err);
  if (!name) {
    unrefexp(e);
    return err;
  }
  int N = (int)(sizeof(lispProcList) / sizeof(lispProc));
  int found = 0;
  for (int i = 0; i < N; i++) {
    if (strcmp(lispProcList[i].name, name) == 0) {
      if (lispProcList[i].doc)
        printf("%s\n", lispProcList[i].doc);
      else
        printf("(no documentation for %s)\n", name);
      found = 1;
      break;
    }
  }
  if (!found && user_doc) {
    keyval_t *kv = set_get_keyval_dict(user_doc, (char *)name, NULL);
    if (kv && kv->val && isstring(kv->val)) {
      printf("%s\n", exp_text(kv->val));
      found = 1;
    }
  }
  if (!found)
    printf("(no documentation for '%s'; try (help) to list builtins)\n", name);
  if (owned)
    unrefexp(owned);
  unrefexp(e);
  return NIL_EXP;
}

const char doc_docstring[] =
    "(docstring name) — return a user function's docstring as a string (or "
    "nil if none). The value-returning companion to (doc name), which prints.";
exp_t *docstringcmd(exp_t *e, env_t *env) {
  exp_t *arg = cadr(e);
  exp_t *owned = NULL, *err = NULL, *ret = NIL_EXP;
  if (arg) {
    const char *name = doc_resolve_name(arg, env, &owned, &err);
    if (name && user_doc) {
      keyval_t *kv = set_get_keyval_dict(user_doc, (char *)name, NULL);
      if (kv && kv->val && isstring(kv->val))
        ret = refexp(kv->val);
    }
    if (name && ret == NIL_EXP) {
      /* builtins too — the same text (doc) prints; tooling (LSP hover) needs
         it as a VALUE, not a side effect on stdout. */
      int N = (int)(sizeof(lispProcList) / sizeof(lispProc));
      for (int i = 0; i < N; i++)
        if (strcmp(lispProcList[i].name, name) == 0) {
          if (lispProcList[i].doc)
            ret = make_string((char *)lispProcList[i].doc,
                              (int)strlen(lispProcList[i].doc));
          break;
        }
    }
    if (owned)
      unrefexp(owned);
    if (err)
      unrefexp(err);
  }
  unrefexp(e);
  return ret;
}

const char doc_withdb[] =
    "(with-db n body...) — evaluate body with keyspace database n (0..15) "
    "selected for the dynamic extent: redis-set / redis-val / redis-del and "
    "the rest of the keyspace bridge target db n inside the body (and in any "
    "function it calls). The previous db is restored afterward, even on "
    "error. db 0 is the shared keyspace the RESP server uses; 1..15 are "
    "separate. Returns the last body value.";
exp_t *withdbcmd(exp_t *e, env_t *env) {
  exp_t *nx = EVAL(cadr(e), env);
  if (iserror(nx)) {
    unrefexp(e);
    return nx;
  }
  if (!isnumber(nx)) {
    unrefexp(nx);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "with-db: db index must be a number");
  }
  int64_t db = FIX_VAL(nx);
  unrefexp(nx);
  if (db < 0 || db >= ALC_NDB) {
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "with-db: db index out of range (0..%d)", ALC_NDB - 1);
  }
  /* Dynamic-scope the thread-local selector; restore on the way out so
     nested (with-db ...) and the error path both unwind correctly. */
  int saved = alcove_kv_db;
  alcove_kv_db = (int)db;
  exp_t *ret = NIL_EXP;
  for (exp_t *b = cddr(e); b; b = b->next) {
    if (ret != NIL_EXP)
      unrefexp(ret);
    ret = EVAL(b->content, env);
    if (ret && iserror(ret))
      break;
  }
  alcove_kv_db = saved;
  unrefexp(e);
  return ret ? ret : NIL_EXP;
}

exp_t *helpcmd(exp_t *e, env_t *env) {
  exp_t *arg = cadr(e);
  if (arg)
    return doccmd(e, env); /* (help name) — defer */
  int N = (int)(sizeof(lispProcList) / sizeof(lispProc));
  for (int i = 0; i < N; i++) {
    printf("  %-16s %s\n", lispProcList[i].name,
           lispProcList[i].doc ? lispProcList[i].doc : "");
  }
  unrefexp(e);
  return NIL_EXP;
}

const char doc_builtins[] =
    "(builtins) — every builtin's name as a list of strings, in registration "
    "order. The machine-readable sibling of (help); completion sources "
    "(the LSP) are built on it.";
exp_t *builtinscmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  int N = (int)(sizeof(lispProcList) / sizeof(lispProc));
  exp_t *head = NULL, *tail = NULL;
  for (int i = 0; i < N; i++) {
    exp_t *node = make_node(make_string((char *)lispProcList[i].name,
                                        (int)strlen(lispProcList[i].name)));
    if (tail) {
      tail->next = node;
      tail = node;
    } else
      head = tail = node;
  }
  return head ? head : refexp(NIL_EXP);
}

const char doc_globals[] =
    "(globals) — the names bound in the global environment (user defs, "
    "*args*, …) as a list of strings. Builtins live in their own table — "
    "see (builtins).";
exp_t *globalscmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  exp_t *head = NULL, *tail = NULL;
  dict_t *d = g_global_env ? g_global_env->d : NULL;
  if (d)
    for (unsigned int i = 0; i < d->ht[0].size; i++)
      for (keyval_t *k = d->ht[0].table[i]; k; k = k->next) {
        exp_t *node =
            make_node(make_string((char *)k->key, (int)strlen(k->key)));
        if (tail) {
          tail->next = node;
          tail = node;
        } else
          head = tail = node;
      }
  return head ? head : refexp(NIL_EXP);
}

/* ============================================================
   Clojure-style builtins for EXP_DICT, EXP_LIST, EXP_BLOB
   ============================================================
   Naming: dict ops follow Clojure (assoc!/dissoc!/get/contains?/keys/vals).
   Deque ops live under their own names (push-left!/pop-left!/...) since
   Clojure's vector ≠ deque; conj!/peek/pop! act on the right end as in
   Clojure for symmetry. count is polymorphic (dict/list/vec/string/pair).

   Key encoding: keys hash on their printed text. Keywords (`:a`) hash as
   ":a"; strings hash as their bytes; numbers as decimal. So `:a` and "a"
   are distinct keys, matching Clojure semantics. NUL bytes in keys are
   not supported (dict_t uses strlen) — for binary-safe keys use blob
   values, not blob keys. */

/* Convert a Lisp value to the canonical key string used by dict_t.
   Returns a borrowed pointer if the input already has one (symbol,
   string), else writes into `tmpbuf` (caller-owned) for numeric keys
   and returns tmpbuf. tmpbuf must hold at least 32 bytes. Returns NULL
   for unsupported key types. */
static char *alc_key_to_cstr(exp_t *k, char *tmpbuf) {
  if (k == NULL)
    return NULL;
  if (issymbol(k) || isstring(k))
    return (char *)exp_text(k);
  if (isnumber(k)) {
    snprintf(tmpbuf, 32, "%lld", (long long)FIX_VAL(k));
    return tmpbuf;
  }
  return NULL;
}

/* Reconstruct a Lisp value from a stored key string. Symbols starting
   with ':' are keywords; all-digit strings round-trip as fixnums; the
   rest become EXP_STRING. Used by `keys` and dict-printing. */
static exp_t *alc_cstr_to_key(const char *k) {
  if (!k)
    return NIL_EXP;
  if (k[0] == ':')
    return make_symbol((char *)k, strlen(k));
  /* All-digit (with optional leading '-')? Try parsing as fixnum. */
  const char *p = (k[0] == '-') ? k + 1 : k;
  if (*p) {
    int all_digit = 1;
    for (const char *q = p; *q; q++) {
      if (*q < '0' || *q > '9') {
        all_digit = 0;
        break;
      }
    }
    if (all_digit) {
      char buf[32];
      snprintf(buf, sizeof buf, "%s", k);
      return make_integer(buf);
    }
  }
  return make_string((char *)k, strlen(k));
}

/* Lisp-value hash-map builtins live in a dedicated #included fragment. */
#include "builtins_dict.h"

/* hash-set (EXP_SET) ops live in a dedicated #included fragment (on dict.h). */
#include "set.h"

/* HAMT ops + builtins live in a dedicated #included fragment. */
#include "hamt.h"

/* hamt_node_foreach visitor: append a (key value) 2-list to acc[0]/acc[1].
   Matches the dict entry shape so (seq hamt) reads like (seq dict). */
static int hamt_collect_entries(exp_t *key, exp_t *val, void *ctx) {
  exp_t **acc = (exp_t **)ctx; /* acc[0]=head, acc[1]=tail */
  exp_t *entry = make_node(refexp(key));
  entry->next = make_node(refexp(val));
  exp_t *node = make_node(entry);
  if (!acc[0])
    acc[0] = node;
  else
    acc[1]->next = node;
  acc[1] = node;
  return 1;
}

/* Associative-collection arm of the generic seq protocol (forward-declared in
   builtins_stdlib.h's coll_to_list). Defined here because it needs set.h's
   DICT_FOREACH/set_value_clone + hamt's iterator. Set → members; dict and HAMT
   → (key value) entries. Returns a fresh owned list, or NULL if coll isn't an
   associative collection. */
static exp_t *coll_assoc_to_list(exp_t *coll) {
  exp_t *head = NULL, *tail = NULL;
  if (isset(coll)) {
    dict_t *d = (dict_t *)coll->ptr;
    if (d)
      DICT_FOREACH(d, k, 0, 0)
    list_append_owned(&head, &tail, set_value_clone(k->val));
  } else if (isdict(coll)) {
    dict_t *d = (dict_t *)coll->ptr;
    if (d)
      DICT_FOREACH(d, k, 0, 0) {
        exp_t *entry =
            make_node(make_string((char *)k->key, strlen((char *)k->key)));
        entry->next = make_node(refexp(k->val));
        list_append_owned(&head, &tail, entry);
      }
  } else if (ishamt(coll)) {
    exp_t *acc[2] = {NULL, NULL};
    if (coll->ptr)
      hamt_node_foreach(((hamt_t *)coll->ptr)->root, hamt_collect_entries, acc);
    head = acc[0];
  } else {
    return NULL; /* not an associative collection */
  }
  return head ? head : refexp(NIL_EXP);
}

/* MessagePack codec lives in a dedicated #included fragment. */
#include "json.h"
#include "msgpack.h"

/* deque (EXP_LIST) ops live in a dedicated #included fragment. */
#include "deque.h"
/* blob (EXP_BLOB) ops live in a dedicated #included fragment. */
#include "blob.h"
/* (gc-cycles) on-demand cycle collector — needs the container internals
   (dict_t, alc_list_t, vec accessors) included above. */
#include "gc.h"

/* Epoch-based reclamation for the lock-free keyspace (LF-1). Included
   here so it can see ALCOVE_TLS and any other build-time toggles. */
#include "epoch.c"

/* Lock-free keyspace (LF-2). Backs the RESP store with an open-addressed
   table of atomic exp_t * slots. Reclamation via epoch.h. */
#include "lfkv.c"

/* ---- shared REPL eval core --------------------------------------------
   Defined here (before the resp.c include and main) so all three input
   loops — the interactive readline REPL, the file/stdin loop, and the -R
   combined REPL+RESP loop — run the SAME transpile + eval + print path.
   Previously each loop reimplemented it, which is how `adder -R` ended
   up reading raw s-expressions instead of adder. */

#ifdef ALCOVE_READLINE
/* Shared readline configuration for the interactive REPL — used by BOTH the
   standalone REPL (main) and the -R combined REPL's reader thread, so line
   editing, completion, syntax coloring, paren-blink, and history behave
   identically. The colored-redisplay hook is load-bearing: it suppresses
   readline's own trailing newline (als_rl_read_form emits one itself), so
   without it -R doubled every line's newline. History FILE load/save stays
   with each caller (the standalone REPL gates it on --no-history). */
static int g_rl_ready; /* tentative def; real one below */
static void
repl_apply_bindings(void); /* defined with the key-binding helpers */
static void repl_readline_setup(env_t *global) {
  /* Honor the terminal's locale so readline treats UTF-8 input as whole
     characters — cursor movement, deletion, and width math operate per
     codepoint instead of per byte (otherwise typing é/ï/世 desyncs the
     cursor). Only LC_CTYPE, to avoid changing number formatting etc. */
  setlocale(LC_CTYPE, "");
  g_global_env = global; /* completer walks env bindings */
  rl_attempted_completion_function = alcove_rl_completer;
  /* TAB indents at line start (only whitespace precedes), else completes. */
  rl_bind_key('\t', alcove_smart_tab);
  /* List all candidates immediately on an ambiguous TAB. Required because TAB is
     bound to alcove_smart_tab, not rl_complete directly: readline's "list on the
     2nd consecutive TAB" check compares rl_last_func against rl_complete, which
     is never the dispatched key here, so the match list would otherwise never
     show (e.g. `redis-<TAB>` would ring the bell instead of listing redis-*).
     The debugger sets this the same way for its own completion. */
  rl_variable_bind("show-all-if-ambiguous", "on");
  /* Ctrl-C cancels the current input (or aborts a multi-line form) and reprompts;
     on an empty line it exits. Takes over SIGINT from readline — see the handler
     in debugger.h. */
  repl_install_sigint();
  /* Shift-TAB (back-tab, ESC[Z) dedents by up to one indent level. */
  rl_bind_keyseq("\033[Z", alcove_back_tab);
  rl_basic_word_break_characters = " \t\n()'`,;\"";
  rl_variable_bind("blink-matching-paren", "on");
  /* Ask the terminal to WRAP pastes in ESC[200~ / 201~ so a multi-line
     paste arrives as ONE buffer (newlines as literal chars — the custom
     redisplay already renders them with continuation prompts) instead of
     each '\n' acting as Enter and stacking the pasted indentation on top
     of the auto-indent. readline 8.x supports this but was not emitting
     the enable sequence here without an explicit bind. */
  rl_variable_bind("enable-bracketed-paste", "on");
  rl_redisplay_function = alcove_colored_redisplay; /* real-time highlighting */
  using_history();
  stifle_history(1000);
  /* Apply user key bindings AFTER the defaults above, so a (bind-key ...) from
     .init (which runs before this) overrides Tab/Shift-Tab etc. Mark readline
     live so later (bind-key ...) calls bind immediately. */
  g_rl_ready = 1;
  repl_apply_bindings();
}

/* History-file persistence, shared by the standalone REPL and the -R
   combined REPL so both round-trip ~/.alcove(s)_history. The resolved path
   lives in a file-scope buffer; an empty buffer means "no persistence"
   (HOME unset, path too long, or --no-history). */
static char alc_hist_path[1024];

static void repl_history_load(int save_history) {
  alc_hist_path[0] = 0;
  if (!save_history)
    return;
  const char *home = getenv("HOME");
#ifdef ALCOVE_ALS
  const char *hist_name = "/.adder_history";
#else
  const char *hist_name = "/.alcove_history";
#endif
  if (home && (size_t)snprintf(alc_hist_path, sizeof alc_hist_path, "%s%s",
                               home, hist_name) < sizeof alc_hist_path)
    read_history(alc_hist_path); /* missing file on first run is fine */
  else
    alc_hist_path[0] = 0; /* HOME unset or path too long — no persistence */
}

static void repl_history_save(void) {
  if (!alc_hist_path[0])
    return;
  write_history(alc_hist_path);
  history_truncate_file(alc_hist_path, 1000);
  chmod(alc_hist_path, 0600); /* may have pasted secrets */
}

/* ---- programmable key bindings (Emacs-style) ----------------------------- */
/* keyseq (the actual terminal bytes) -> Lisp handler thunk. Persistent across
   the session so (bind-key ...) from .init survives until repl_readline_setup
   applies it. g_rl_ready gates live rl_bind_keyseq calls: .init runs BEFORE
   readline is set up (see main), so a bind from there is stored now and applied
   in bulk at the end of repl_readline_setup. */
static dict_t *g_key_bindings = NULL;
static int g_rl_ready = 0;

/* Resolve a friendly key spec to the raw readline keyseq bytes. Aliases:
   tab S-tab home end C-<a-z> M-<char>; anything else is taken as raw bytes
   (e.g. a literal ESC sequence). Returns 1 on success, filling `out`. */
static int repl_resolve_keyseq(const char *spec, char *out, size_t outsz) {
  if (!spec || !*spec || outsz < 4)
    return 0;
  if (!strcmp(spec, "tab")) {
    out[0] = '\t';
    out[1] = 0;
  } else if (!strcmp(spec, "S-tab") || !strcmp(spec, "shift-tab")) {
    snprintf(out, outsz, "\033[Z");
  } else if (!strcmp(spec, "home")) {
    snprintf(out, outsz, "\033[H");
  } else if (!strcmp(spec, "end")) {
    snprintf(out, outsz, "\033[F");
  } else if ((spec[0] == 'C' || spec[0] == 'c') && spec[1] == '-' && spec[2] &&
             !spec[3]) {
    /* C-<letter> -> the control byte (C-a == 1 ... C-z == 26). */
    int c = tolower((unsigned char)spec[2]);
    if (c < 'a' || c > 'z')
      return 0;
    out[0] = (char)(c - 'a' + 1);
    out[1] = 0;
  } else if ((spec[0] == 'M' || spec[0] == 'm') && spec[1] == '-' && spec[2] &&
             !spec[3]) {
    out[0] = '\033'; /* Meta-<char> -> ESC + char */
    out[1] = spec[2];
    out[2] = 0;
  } else {
    if (strlen(spec) + 1 > outsz)
      return 0;
    strcpy(out, spec); /* raw keyseq bytes */
  }
  return 1;
}

/* The single trampoline bound to every user keyseq. readline reports the exact
   sequence that fired in rl_executing_keyseq; we look up its handler and call
   it as a no-arg thunk, then repaint so any buffer edits show. */
static int alcove_key_dispatch(int count, int key) {
  (void)count;
  (void)key;
  exp_t *fn = NULL;
  const char *seq = rl_executing_keyseq;
  if (seq && g_key_bindings) {
    keyval_t *kv = set_get_keyval_dict(g_key_bindings, (char *)seq, NULL);
    fn = kv ? (exp_t *)kv->val : NULL;
  }
  if (fn && islambda(fn)) {
    exp_t *r = alc_apply_n(fn, 0, NULL, g_global_env);
    if (r)
      unrefexp(r);
    rl_forced_update_display();
  }
  return 0;
}

/* Record (or replace) a binding and, if readline is live, bind it now. A nil
   handler stores nil → the key becomes an inert no-op (dispatch ignores it). */
static void repl_bind_one(const char *seq, exp_t *handler) {
  if (!g_key_bindings)
    g_key_bindings = create_dict();
  keyval_t *old = set_get_keyval_dict(g_key_bindings, (char *)seq, NULL);
  if (old && old->val)
    unrefexp((exp_t *)old->val);
  set_get_keyval_dict(g_key_bindings, (char *)seq, refexp(handler));
  if (g_rl_ready)
    rl_bind_keyseq(seq, alcove_key_dispatch);
}

/* Apply every recorded binding to the current keymap. Called at the end of
   repl_readline_setup (after the default Tab/Shift-Tab binds) so user bindings
   from .init take precedence over the defaults. */
static void repl_apply_bindings(void) {
  if (!g_key_bindings)
    return;
  for (int h = 0; h < 2; h++) {
    if (!g_key_bindings->ht[h].size)
      continue;
    for (size_t i = 0; i < g_key_bindings->ht[h].size; i++)
      for (keyval_t *k = g_key_bindings->ht[h].table[i]; k; k = k->next)
        if (k->key)
          rl_bind_keyseq((const char *)k->key, alcove_key_dispatch);
  }
}
#endif

#include "repl_builtins.h"
#ifndef ALCOVE_NO_MAIN
int main(int argc, char *argv[]) {
  dict_t *dict = create_dict();
  env_t *global = alcove_init();
  FILE *stream;
  int evaluatingfile = 0;
  /* When running a FILE argument, the basename used to prefix file-context
     error messages with "<src>:<line>:". NULL for stdin / -e / interactive. */
  const char *script_src = NULL;
  int idx = 0;

#ifdef ALCOVE_WEB
  /* Web build: init complete, hand control back to JS. The Emscripten
     runtime stays alive (build with -sNO_EXIT_RUNTIME=1) so JS can
     call _alcove_web_eval after main returns. g_global_env was set
     above; alcove_web_eval below uses it. */
  (void)argc;
  (void)argv;
  (void)dict;
  (void)stream;
  (void)evaluatingfile;
  (void)idx;
  return 0;
#endif

  /* CLI flag scan:
       --noload / -n       skip auto-load of the db file
       --db <path>         set the session db path. Used by the startup
                           auto-load AND by future no-arg (savedb) /
                           (loaddb) calls — so the file you load from is
                           the same file you save to.
       -e "<code>"         evaluate the string as a script (skips file read)
       -r [port]           run as a RESP2 (Redis) server on 127.0.0.1.
                           Default port 6379 if next arg is not an int.
     Flags get filtered out of argv in place so the existing positional
     handling (last arg = file, prev = -i) still works whether the user
     passes flags first, last, or in the middle. */
  int auto_load = 1;
  int run_init = 1;
  int save_history = 1;
  int interactive_after = 0; /* -i: drop into the REPL after the script */
  char *eval_string = NULL;
#ifndef ALCOVE_WEB
  int resp_mode = 0;     /* -r: RESP server only, no REPL */
  int resp_combined = 0; /* -R: combined REPL + RESP single-reactor */
  int resp_port = 6379;
  int resp_threads = 1; /* --threads N: number of reactor threads (-r only) */
#endif
  {
    int dst = 1, src;
    for (src = 1; src < argc; src++) {
      if (strcmp(argv[src], "--noload") == 0 || strcmp(argv[src], "-n") == 0) {
        auto_load = 0;
      } else if (strcmp(argv[src], "--version") == 0) {
#ifdef ALCOVE_ALS
        printf("adder %s\n", ALCOVE_VERSION);
#else
        printf("alcove %s\n", ALCOVE_VERSION);
#endif
        return 0;
      } else if (strcmp(argv[src], "-i") == 0) {
        interactive_after = 1;
      } else if (strcmp(argv[src], "--safe") == 0) {
        /* Don't let a db.dump auto-(require) native modules to resolve its
           custom types — loading a dump then can't execute module code. */
        g_safe_mode = 1;
      } else if (strcmp(argv[src], "--interpret") == 0) {
        /* Force the AST tree-walker (no bytecode compile) — differential
           testing vs the default compiled path. No TCO; keep recursion bounded.
         */
        g_no_compile = 1;
      } else if (strcmp(argv[src], "--no-line-info") == 0) {
        /* Disable the reader's per-form line/col stamping. Tracking is parse-
           time only (zero runtime cost — the VM/JIT never reads it), so this is
           a purity/minimalism switch, not a speed one: errors fall back to
           top-level-form granularity and the debugger loses source lines. */
        g_track_lines = 0;
      } else if (strcmp(argv[src], "--debug") == 0 ||
                 strcmp(argv[src], "-d") == 0) {
        /* gdb-style debugger: force the AST walker (every frame a live env_t),
           track source lines, and arm the per-form hook. Starts in continue
           mode — a startup prompt lets you set breakpoints, then run. */
        g_no_compile = 1;
        g_track_lines = 1;
        g_debug = 1;
        g_dbg_mode = 0;
      } else if (strcmp(argv[src], "--no-init") == 0 ||
                 strcmp(argv[src], "--noinit") == 0) {
        run_init = 0;
      } else if (strcmp(argv[src], "--no-history") == 0 ||
                 strcmp(argv[src], "--nohistory") == 0) {
        save_history = 0;
      } else if (strcmp(argv[src], "-e") == 0 && src + 1 < argc) {
        eval_string = argv[++src];
      } else if (strcmp(argv[src], "--db") == 0 && src + 1 < argc) {
        alcove_db_path = argv[++src]; /* session-wide default */
#ifndef ALCOVE_WEB
      } else if ((strcmp(argv[src], "--threads") == 0 ||
                  strcmp(argv[src], "-t") == 0) &&
                 src + 1 < argc) {
        char *end;
        long n = strtol(argv[++src], &end, 10);
        if (*end == '\0' && n >= 1 && n <= EPOCH_MAX_THREADS) {
          resp_threads = (int)n;
        } else {
          fprintf(stderr, "alcove: --threads must be 1..%d\n",
                  EPOCH_MAX_THREADS);
          return 1;
        }
      } else if (strcmp(argv[src], "-r") == 0 || strcmp(argv[src], "-R") == 0) {
        if (strcmp(argv[src], "-R") == 0)
          resp_combined = 1;
        else
          resp_mode = 1;
        /* Optional explicit port follows. Only consume if it parses
           cleanly as a positive int < 65536, otherwise treat next arg
           as a positional (file/-i). */
        if (src + 1 < argc) {
          char *end;
          long p = strtol(argv[src + 1], &end, 10);
          if (*end == '\0' && p > 0 && p < 65536) {
            resp_port = (int)p;
            src++;
          }
        }
#endif /* !ALCOVE_WEB */
      } else {
        argv[dst++] = argv[src];
      }
    }
    argc = dst;
  }

  /* RESP server short-circuits the rest of main(): no REPL, no file/-e
     processing. The init file still runs so users can pre-populate
     resp_db (or any other startup hook). Auto-load of the persistence
     file happens here too — eager-initialize g_resp_kv first since
     reactors lazy-init it on first write. */
#ifndef ALCOVE_WEB
  if (resp_mode) {
    int N = sizeof(lispProcList) / sizeof(lispProc);
    /* lispProcList registration is needed downstream by isstring/etc.
       only indirectly — but resp_db lives separately. Skip it. */
    (void)N;
    if (run_init)
      alcove_try_init_files(global);
    if (auto_load) {
      lfkv_t *kv = resp_kv_init();
      if (kv) {
        int loaded = loaddb_from_file_path(global, alcove_db_path);
        if (loaded > 0)
          printf(ALCOVE_PROGNAME ": auto-loaded %d entries from %s\n", loaded,
                 alcove_db_path);
      }
    }
    int rc = respN_serve(resp_port, resp_threads);
    /* Release the unused top-level dict we created above so leak
       checkers see a clean exit. The non-RESP path frees it later. */
    destroy_dict(dict);
    return rc;
  }

  /* -R: combined REPL + RESP on a single select() reactor. Both run
     on this thread; they're mutually exclusive in time, so resp_db
     and the global env can be shared without locking. The REPL gets
     redis-keys / redis-get / redis-type / redis-count / redis-flush
     / redis-port to inspect the live db. */
  if (resp_combined) {
    if (auto_load) {
      int loaded = loaddb_from_file_path(global, alcove_db_path);
      if (loaded > 0)
        printf(ALCOVE_PROGNAME ": auto-loaded %d entries from %s\n", loaded,
               alcove_db_path);
    }
    if (run_init)
      alcove_try_init_files(global);
    return resp_repl_serve(resp_port, global);
  }
#endif /* !ALCOVE_WEB */

  /* Auto-load persisted bindings from the chosen db path. Silent on
     missing-file (first run, no DB yet); prints a one-line summary on
     success so the user sees what came back. */
  if (auto_load) {
    int loaded = loaddb_from_file_path(global, alcove_db_path);
    if (loaded > 0)
      printf(ALCOVE_PROGNAME
             ": auto-loaded %d entries from %s (use --noload to "
             "skip)\n",
             loaded, alcove_db_path);
  }
  if (run_init)
    alcove_try_init_files(global);

  if (eval_string) {
    /* Wrap the inline code as a read-only memory stream so the existing
       reader/eval loop runs against it unchanged. `evaluatingfile=1`
       makes the loop skip the REPL prompts and quietly exit on EOF. */
    stream = fmemopen(eval_string, strlen(eval_string), "r");
    if (!stream) {
      printf("alcove: fmemopen failed for -e\n");
      exit(1);
    }
    evaluatingfile = 1;
  } else if (argc >= 2) {
    if ((stream = fopen(argv[1], "r"))) {
      evaluatingfile = 1;
      if (interactive_after)
        evaluatingfile |= 2;
      script_src = src_basename(argv[1]);
      /* Let (require ...) at the top level of a script resolve modules
         relative to the script's own directory first. */
      g_reader_dir = path_dirname(argv[1]);
    } else {
      printf("Error opening %s\n", argv[1]);
      exit(1);
    }
  } else
    stream = stdin;

  /* Rebind *args* to the remaining positionals: for a script, everything
     after the script path; for -e, every positional. (Previously extra
     positionals were silently mis-read — the LAST one won as the script —
     so no working invocation changes meaning.) */
  {
    int first = (evaluatingfile && !eval_string) ? 2 : 1;
    exp_t *head = NULL, *tail = NULL;
    for (int ai = first; ai < argc; ai++) {
      exp_t *node = make_node(make_string(argv[ai], (int)strlen(argv[ai])));
      if (tail) {
        tail->next = node;
        tail = node;
      } else
        head = tail = node;
    }
    if (head) {
      set_get_keyval_dict(global->d, "*args*", head);
      unrefexp(head);
    }
  }

#ifdef ALCOVE_ALS
  /* Non-interactive input (file arg, piped stdin, -e) is Adder:
     slurp it, transpile to s-expressions, and hand reader() a memstream
     of the result. Interactive tty input is handled block-wise in the
     readline path below, so skip the slurp there (it would block). */
  if (!(stream == stdin && isatty(fileno(stdin)))) {
    als_buf slurp;
    als_buf_init(&slurp);
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, stream)) > 0)
      als_buf_putn(&slurp, chunk, got);
    if (stream != stdin)
      fclose(stream);
    als_map *m = (als_map *)calloc(1, sizeof *m);
    char *sx = als_to_sexpr_mapped(slurp.p, m);
    stream = fmemopen(sx, strlen(sx), "r");
    /* sx intentionally outlives this scope: it backs `stream` for the
       remainder of the run and is reclaimed by the OS at exit. */
    if (evaluatingfile) {
      /* A .adr file or -e: keep the ORIGINAL Adder source + the generated→Adder
         line map so an error renders an Adder-source caret (and keeps the file
         label, since the map translates the reader's lines back). slurp.p and m
         outlive this scope and are reclaimed at exit. */
      g_reader_srctext = slurp.p;
      g_reader_srctext_len = slurp.len;
      g_adder_map = m;
    } else {
      /* Piped stdin: no retained source (keeps the generated web battery, which
         pipes expressions through stdin, identical — web carets are a separate
         step). */
      free(slurp.p);
      als_map_free(m);
      free(m);
      script_src = NULL;
    }
  }
#endif

#ifndef ALCOVE_ALS
  /* Retain the source text so an error can render a caret on the offending
     line. -e already holds it; a file argument is slurped into a memstream
     (the FILE* alone gives no text). Piped/interactive stdin is left as-is. */
  if (eval_string) {
    g_reader_srctext = eval_string;
    g_reader_srctext_len = strlen(eval_string);
  } else if (evaluatingfile && stream != stdin) {
    size_t slen = 0;
    char *txt = slurp_stream(stream, &slen);
    fclose(stream);
    stream = txt ? fmemopen(txt, slen, "r") : NULL;
    if (!stream) {
      printf("Error reading %s\n", argv[1]);
      exit(1);
    }
    g_reader_srctext = txt; /* outlives the run; reclaimed at exit */
    g_reader_srctext_len = slen;
  }
#endif

#ifdef ALCOVE_READLINE
  /* Enable line editing + tab completion + history when stdin is a tty.
     Non-interactive (pipe / file redirect / scripted) stays on the plain
     reader path. The completer needs g_global_env to walk env bindings. */
  int rl_active = (stream == stdin) && isatty(fileno(stdin));
  if (rl_active) {
    repl_readline_setup(global); /* shared with the -R reader thread */
    repl_history_load(save_history);
  }
#endif

  /* Debugger startup prompt (gdb's `(gdb)` before `run`): set breakpoints, then
     `c` to run (or `s` to step from the start). Applies to both a file and the
     interactive REPL; breakpoints / (break) then stop with fully live frames.
   */
  if (g_debug)
    debug_repl(NIL_EXP, global);

  exp_t *stre = NULL;

#ifdef ALCOVE_READLINE
/* Re-entry point for `-i`: after a script's stream hits EOF we switch to an
   interactive tty and jump here so the post-script session gets line editing,
   tab completion and history — not the raw reader the file path uses. */
interactive_readline:
  if (rl_active) {
    /* Interactive readline-based REPL: per-iteration we read a complete
       top-level form (continuation prompt for unbalanced parens) into
       a string, fmemopen it, and run the same eval+print pipeline as
       the file path against the memstream. */
    while (1) {
      idx++;
#ifdef ALCOVE_ALS
      char *line = als_rl_read_form(idx); /* one adder unit */
#else
      char *line = rl_read_form(idx); /* one balanced s-expr form */
#endif
      if (!line) {
        printf("\n");
        goto endcleanly;
      }
      if (!line[0]) {
        free(line);
        idx--;
        continue;
      }
      /* Shared transpile + eval + print core (als-aware in the adder
         build). Returns 1 on quit/exit. */
      int quit = repl_eval_text(line, strlen(line), global, idx);
      free(line);
      if (quit)
        goto endcleanly;
    }
  }
#endif

  /* File context: label errors with "<basename>:<line>:" and start counting
     lines from 1. NULL src (stdin / -e / interactive / adder-transpiled)
     leaves error messages unprefixed. */
  g_reader_src = (evaluatingfile && script_src) ? script_src : NULL;
  g_reader_line = 1;
  g_reader_col = 1;
  g_reader_off = 0;
  while (1) {
    idx++;
    if (!evaluatingfile) {
      char *iph = repl_prompt_str(global, "*prompt-in*", idx);
      if (iph) {
        fputs(iph, stdout);
        free(iph);
      } else
        printf("\x1B[34mIn [\x1B[94m%d\x1B[34m]:\x1B[39m", idx);
    }
    g_form_line = g_reader_line; /* fallback if no significant byte is read */
    g_form_line_arm = 1;         /* reader() stamps the true form-start line */
    stre = reader(stream, 0, 0);
    if (iserror(stre) && (stre->flags == EXP_ERROR_PARSING_EOF)) {
      if (evaluatingfile) {
        if (evaluatingfile & 2) {
          stream = stdin;
          evaluatingfile = 0;
          g_reader_src = NULL; /* switching to interactive stdin */
          unrefexp(stre);
#ifdef ALCOVE_READLINE
          /* `-i` dropping into the REPL: arm readline now (it was off at
             startup because `stream` was the script file) so Tab completion,
             history and line editing work just like a bare interactive run. */
          if (isatty(fileno(stdin))) {
            rl_active = 1;
            repl_readline_setup(global);
            repl_history_load(save_history);
            idx--; /* the readline loop re-increments for this prompt */
            goto interactive_readline;
          }
#endif
          continue;
        } else {
          unrefexp(stre);
          goto endcleanly;
        }
      }
      /* Interactive EOF (Ctrl-D or piped input exhausted) */
      unrefexp(stre);
      if (!evaluatingfile)
        printf("\n");
      goto endcleanly;
    }
    /* A reader syntax error (not EOF) in a file: its line is g_reader_line at
       the failure point. Surface it on stderr with the source location and
       move on to the next form instead of feeding the error exp to eval. */
    if (iserror(stre) && g_reader_src) {
      annotate_error_loc(stre, g_reader_src, display_line(g_reader_line));
      fprintf(stderr, "%s\n", (const char *)stre->ptr);
      render_form_caret(stderr, g_reader_line, g_reader_col);
      g_script_error = 1; /* syntax error in a script → non-zero exit */
      unrefexp(stre);
      continue;
    }
    /* Shared eval + print core; quiet (no Out[] print) while loading a
       file. Returns 1 on quit/exit. */
    if (repl_eval_print_form(stre, global, idx, evaluatingfile))
      break;
  }
endcleanly:
#ifdef ALCOVE_READLINE
  repl_history_save();
#endif
  free(g_reader_dir); /* top-level script dir (eval_file_forms frees nested) */
  g_reader_dir = NULL;
  destroy_dict(dict);
  destroy_env(global);
  destroy_dict(reserved_symbol);
  /* Free every exp_tfunc slot we allocated in the type-fn registration
     (CHAR/STRING were here originally; the rest were added when
     persistence grew to cover number/float/symbol/pair/lambda/macro). */
  free(exp_tfuncList[EXP_CHAR]);
  free(exp_tfuncList[EXP_STRING]);
  free(exp_tfuncList[EXP_NUMBER]);
  free(exp_tfuncList[EXP_FLOAT]);
  free(exp_tfuncList[EXP_SYMBOL]);
  free(exp_tfuncList[EXP_PAIR]);
  free(exp_tfuncList[EXP_LAMBDA]);
  free(exp_tfuncList[EXP_MACRO]);
  free(exp_tfuncList[EXP_BLOB]);
  free(exp_tfuncList[EXP_VECTOR]);
  free(exp_tfuncList[EXP_SET]);
  free(exp_tfuncList[EXP_DICT]);
  free(exp_tfuncList[EXP_LIST]);
  free(exp_tfuncList[EXP_HAMT]);
  /* Custom module types registered via alcove_register_type. */
  for (unsigned short ti = EXP_MAXSIZE; ti < g_next_type_id; ti++) {
    free(exp_tfuncList[ti]);
    free(g_custom_types[ti].name);
    free(g_custom_types[ti].module_spec);
  }
  /* (t / nil were the immortal singletons — no unref needed; freed below.) */
  /* Immortal singletons. We can't free() the exp_t pointer itself
     anymore (it lives inside the bump-allocator chunk, not a separate
     malloc), but the strdup'd ptr field for the "t" symbol can be
     released. The chunks themselves are reclaimed by the OS at exit. */
  if (true_singleton) {
    if (true_singleton->ptr && !(true_singleton->flags & FLAG_INLINE_TXT))
      free(true_singleton->ptr);
    true_singleton = NULL;
  }
  if (nil_singleton) {
    nil_singleton = NULL;
  }
  /* Exit non-zero if a script (file arg or -e) had a top-level parse/eval
     error, so runners/CI can detect failure. Interactive / piped-stdin REPL
     sessions never set g_script_error, so they still exit 0 on normal quit. */
  return g_script_error;
}
#endif /* ALCOVE_NO_MAIN — a C embedder defines this to supply its own main */

#ifdef ALCOVE_WEB
/* Web entry point: invoked from JS after main() has initialised the
   runtime. Reads forms from `src`, evaluates them against the global
   env, prints each non-nil result to stdout. Emscripten routes stdout
   to Module.print, which our JS shim captures. */
__attribute__((used)) int alcove_web_eval(const char *src) {
  if (!src || !*src || !g_global_env)
    return 0;
#ifdef ALCOVE_ALS
  char *translated = als_to_sexpr(src);
  if (!translated)
    return 0;
  FILE *stream = fmemopen((void *)translated, strlen(translated), "r");
#else
  FILE *stream = fmemopen((void *)src, strlen(src), "r");
#endif
  if (!stream) {
#ifdef ALCOVE_ALS
    free(translated);
#endif
    return 0;
  }
  int forms = 0;
  while (1) {
    exp_t *stre = reader(stream, 0, 0);
    if (iserror(stre) && stre->flags == EXP_ERROR_PARSING_EOF) {
      unrefexp(stre);
      break;
    }
    bt_clear(); /* per-top-level-form capture scope (see repl_eval_print_form)
                 */
    exp_t *strf = evaluate(stre, g_global_env);
    if (strf) {
      /* Match the script-execution convention: don't echo nil results.
         (prn ...) returns nil, and printing "nil" after every print
         call in a REPL session is just noise. */
      if (strf != NIL_EXP) {
        print_node(strf);
        printf("\n");
        /* an uncaught error: show the captured call backtrace (the web path has
           no source-file caret, but the call chain is the useful part). */
        if (iserror(strf))
          render_backtrace(stdout);
      }
      unrefexp(strf);
    }
    forms++;
  }
  fclose(stream);
#ifdef ALCOVE_ALS
  free(translated);
#endif
  fflush(stdout);
  return forms;
}
#endif
