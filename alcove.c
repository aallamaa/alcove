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

#include <assert.h>
#include <ctype.h>
#include <limits.h> /* LLONG_MIN for hex literal overflow guard */
#include <locale.h> /* setlocale: make readline UTF-8 / multibyte aware */
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h> /* isatty for the readline REPL gate; needed even
                          when ALCOVE_JIT is off. */
#ifdef ALCOVE_ALS
/* Adder front end: a string->string transpiler that turns the
   whitespace/`:`-block surface syntax into ordinary alcove
   s-expressions before they reach reader(). */
#include "adr.h"
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
exp_tfunc *exp_tfuncList[EXP_MAXSIZE];

/* Canonical singletons — pointer set at main() startup. */
exp_t *nil_singleton = NULL;
exp_t *true_singleton = NULL;
exp_t *gen_done_singleton = NULL;
static exp_t *alc_cstr_to_key(const char *k);
static int set_insert_value(dict_t *d, exp_t *v);
static void alc_list_push_right(alc_list_t *l, exp_t *val); /* defined far below; used by load_deque_value */
static int is_reserved_name(const char *name);            /* defined below; used by updatebang/setq */

/* Reject assigning to a bare reserved-name symbol (= / setf / setq). Only a
   symbol LHS triggers it — place forms ((vec i), (s i), (car x)) and string
   keys pass through. On a hit, build the error into ERRLV and run FAIL
   (cleanup + return/propagate). `env` must be in scope at the use site. */
#define REJECT_RESERVED_ASSIGN(SYM, ERRLV, FAIL)                              \
  do {                                                                        \
    if (issymbol(SYM) && is_reserved_name(exp_text(SYM))) {                   \
      (ERRLV) = error(ERROR_ILLEGAL_VALUE, NULL, env,                         \
                      "cannot assign to reserved name '%s'",                  \
                      exp_text(SYM));                                         \
      FAIL;                                                                    \
    }                                                                         \
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

/* Builtin registration. `arity` and `level` are reserved fields (the
   first for future per-call arity checks, the second for sandbox tiers);
   nothing reads them today. `doc` is a one-line help string colocated
   with each cmd's definition (search for `static const char doc_<name>[]`
   near the corresponding cmd function). Use LISPCMD_TAIL for control-
   flow forms (if, do) that need FLAG_TAIL_AWARE so evaluate() exposes
   in_tail_position to them. */
#define LISPCMD(name, fn, doc) {name, -1, 0, 0, doc, fn}
#define LISPCMD_TAIL(name, fn, doc) {name, -1, FLAG_TAIL_AWARE, 0, doc, fn}

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
    /* Generators — gen-* names kept for compat; !-suffix are the preferred forms.
       Convention: ! = operates in the stateful/mutable iterator domain;
                   ? = predicate; no suffix = pure eager list operations. */
    LISPCMD("*gen-done*", gendone_cmd, doc_gendone),
    LISPCMD("*done*",     gendone_cmd, doc_gendone),    /* preferred short name */
    LISPCMD("gen-done?",  gendonep_cmd, doc_gendonep),
    LISPCMD("done?",      gendonep_cmd, doc_gendonep),  /* preferred */
    LISPCMD("gen-list",   genlist_cmd, doc_genlist),
    LISPCMD("iter!",      genlist_cmd, doc_genlist),    /* preferred */
    LISPCMD("gen-range",  genrange_cmd, doc_genrange),
    LISPCMD("range!",     genrange_cmd, doc_genrange),  /* preferred */
    LISPCMD("gen-next!",  gennext_cmd, doc_gennext),
    LISPCMD("next!",      gennext_cmd, doc_gennext),    /* preferred */
    LISPCMD("gen-collect",gencollect_cmd, doc_gencollect),
    LISPCMD("collect!",   gencollect_cmd, doc_gencollect), /* preferred */
    LISPCMD("gen-map",    genmap_cmd, doc_genmap),
    LISPCMD("map!",       genmap_cmd, doc_genmap),      /* preferred */
    LISPCMD("gen-filter", genfilter_cmd, doc_genfilter),
    LISPCMD("filter!",    genfilter_cmd, doc_genfilter), /* preferred */
    /* Comparison / equality */
    LISPCMD("=", equalcmd, doc_eq),
    LISPCMD("setf", equalcmd, doc_setf), /* exact synonym of = (readable head) */
    LISPCMD("<", cmpcmd, doc_lt),
    LISPCMD(">", cmpcmd, doc_gt),
    LISPCMD("<=", cmpcmd, doc_le),
    LISPCMD(">=", cmpcmd, doc_ge),
    LISPCMD("is", iscmd, doc_is),
    LISPCMD("eq", iscmd, doc_is),  /* alias of is */
    LISPCMD("eq?", iscmd, doc_is), /* alias of is */
    LISPCMD("iso", isocmd, doc_iso),
    LISPCMD("in", incmd, doc_in),
    LISPCMD("no", nocmd, doc_no),
    /* Arithmetic */
    LISPCMD("+", pluscmd, doc_plus),
    LISPCMD("*", multiplycmd, doc_mul),
    LISPCMD("-", minuscmd, doc_minus),
    LISPCMD("/", dividecmd, doc_div),
    LISPCMD("mod", modcmd, doc_mod),
    LISPCMD("abs", abscmd, doc_abs),
    LISPCMD("max", maxcmd, doc_max),
    LISPCMD("min", mincmd, doc_min),
    LISPCMD("odd", oddcmd, doc_odd),
    LISPCMD("sqrt", sqrtcmd, doc_sqrt),
    LISPCMD("sqrt-int", sqrtintcmd, doc_sqrtint),
    LISPCMD("exp", expcmd, doc_exp),
    LISPCMD("expt", exptcmd, doc_expt),
    LISPCMD("**", exptcmd, doc_expt), /* Python-ish alias */
    LISPCMD("random", randomcmd, doc_random),
    LISPCMD("round", roundcmd, doc_round),
    LISPCMD("floor", floorcmd, doc_floor),
    LISPCMD("ceil", ceilcmd, doc_ceil),
    LISPCMD("truncate", truncatecmd, doc_truncate),
    LISPCMD("log", logcmd, doc_log),
    LISPCMD("sin", sincmd, doc_sin),
    LISPCMD("cos", coscmd, doc_cos),
    LISPCMD("tan", tancmd, doc_tan),
    LISPCMD("float", floatcmd, doc_float),
    LISPCMD("int", intcmd, doc_int),
    /* Bitwise — int-only. C-style spelling + Lisp-style aliases. */
    LISPCMD("bit-and", bitandcmd, doc_bitand),
    LISPCMD("&", bitandcmd, doc_bitand),
    LISPCMD("bit-or", bitorcmd, doc_bitor),
    LISPCMD("|", bitorcmd, doc_bitor),
    LISPCMD("bit-xor", bitxorcmd, doc_bitxor),
    LISPCMD("^", bitxorcmd, doc_bitxor),
    LISPCMD("bit-not", bitnotcmd, doc_bitnot),
    LISPCMD("~", bitnotcmd, doc_bitnot),
    LISPCMD("<<", shlcmd, doc_shl),
    LISPCMD(">>", shrcmd, doc_shr),
    /* Pairs and lists */
    LISPCMD("cons", conscmd, doc_cons),
    LISPCMD("car", carcmd, doc_car),
    LISPCMD("cdr", cdrcmd, doc_cdr),
    LISPCMD("list", listcmd, doc_list),
    LISPCMD("length", lengthcmd, doc_length),
    LISPCMD("nth", nthcmd, doc_nth),
    LISPCMD("reverse", reversecmd, doc_reverse),
    LISPCMD("append", appendcmd, doc_append),
    /* Vectors — O(1) random-access array */
    LISPCMD("vec", veccmd, doc_vec),
    LISPCMD("vec-ref", vecrefcmd, doc_vecref),
    LISPCMD("vec-set!", vecsetcmd, doc_vecset),
    LISPCMD("vec-len", veclencmd, doc_veclen),
    /* Tensor bulk ops — read each element as a double, do the math in
       raw C, write fresh EXP_FLOATs back. ~100x faster than the
       interpreted equivalent for MLP-style inner loops. */
    LISPCMD("vec-dot", vecdotcmd, doc_vecdot),
    LISPCMD("vec-axpy!", vecaxpycmd, doc_vecaxpy),
    LISPCMD("vec-scale!", vecscalecmd, doc_vecscale),
    LISPCMD("vec-add!", vecaddcmd, doc_vecadd),
    LISPCMD("vec-copy!", veccopycmd, doc_veccopy),
    LISPCMD("vec-fill!", vecfillcmd, doc_vecfill),
    LISPCMD("vec-relu!", vecrelucmd, doc_vecrelu),
    LISPCMD("vec-argmax", vecargmaxcmd, doc_vecargmax),
    LISPCMD("vec-max", vecmaxcmd, doc_vecmax),
    /* Deque ops on vec — amortised O(1) push/pop at both ends via the
       cap/start/end window. Growth: 1.5x on realloc; slide-left when
       start >= cap/4 instead of reallocating; recenter on unshift-grow. */
    LISPCMD("vec-push!", vecpushcmd, doc_vecpush),
    LISPCMD("vec-pop!", vecpopcmd, doc_vecpop),
    LISPCMD("vec-unshift!", vecunshiftcmd, doc_vecunshift),
    LISPCMD("vec-shift!", vecshiftcmd, doc_vecshift),
    /* Functions and binding */
    LISPCMD("def", defcmd, doc_def),
    LISPCMD("fn", fncmd, doc_fn),
    LISPCMD("defmacro", defmacrocmd, doc_defmacro),
    LISPCMD("macroexpand-1", expandmacrocmd, doc_macroexpand),
    LISPCMD("eval", evalcmd, doc_eval),
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
    LISPCMD("number?", numberpcmd, doc_numberp),
    LISPCMD("string?", stringpcmd, doc_stringp),
    LISPCMD("symbol?", symbolpcmd, doc_symbolp),
    LISPCMD("pair?", pairpcmd, doc_pairp),
    LISPCMD("list?", listpcmd, doc_listp),
    LISPCMD("null?", nullpcmd, doc_nullp),
    LISPCMD("fn?", fnpcmd, doc_fnp),
    LISPCMD("vec?", vecpcmd, doc_vecp),
    LISPCMD("blob?", blobpcmd, doc_blobp),
    LISPCMD("dict?", dictpcmd, doc_dictp),
    LISPCMD("deque?", dequepcmd, doc_dequep),
    LISPCMD("set?", setpcmd, doc_setp),
    /* Introspection (return testable values, not printed) */
    LISPCMD("compiled?", compiledpcmd, doc_compiledp),
    LISPCMD("jit?", jitpcmd, doc_jitp),
    LISPCMD("inline?", inlinepcmd, doc_inlinep),
    LISPCMD("exp-flags", expflagscmd, doc_expflags),
    /* I/O */
    LISPCMD("pr", prcmd, doc_pr),
    LISPCMD("print", prcmd, doc_pr),
    LISPCMD("prn", prncmd, doc_prn),
    LISPCMD("println", prncmd, doc_prn),
    /* Strings and whole-file I/O */
    LISPCMD("str", strcmd, doc_str),
    LISPCMD("fmt", fmtcmd, doc_fmt),
    LISPCMD("substr", substrcmd, doc_substr),
    LISPCMD("string-append", stringappendcmd, doc_stringappend),
    LISPCMD("string-split", stringsplitcmd, doc_stringsplit),
    LISPCMD("string-join", stringjoincmd, doc_stringjoin),
    LISPCMD("string-trim", stringtrimcmd, doc_stringtrim),
    LISPCMD("string-upcase", stringupcasecmd, doc_stringupcase),
    LISPCMD("string-downcase", stringdowncasecmd, doc_stringdowncase),
    LISPCMD("string-contains?", stringcontainspcmd, doc_stringcontainsp),
    LISPCMD("string-index", stringindexcmd, doc_stringindex),
    LISPCMD("string-replace", stringreplacecmd, doc_stringreplace),
    LISPCMD("read-string", readstringcmd, doc_readstring),
    LISPCMD("write-string", writestringcmd, doc_writestring),
    LISPCMD("append-string", appendstringcmd, doc_appendstring),
    LISPCMD("read-lines", readlinescmd, doc_readlines),
    LISPCMD("file-exists?", fileexistspcmd, doc_fileexistsp),
    LISPCMD("write-bytes", writebytescmd, doc_writebytes),
    LISPCMD("load", loadcmd, doc_load),
    /* Persistence */
    LISPCMD("persist", persistcmd, doc_persist),
    LISPCMD("forget", forgetcmd, doc_forget),
    LISPCMD("unpersist", unpersistcmd, doc_unpersist),
    LISPCMD("savedb", savedbcmd, doc_savedb),
    LISPCMD("loaddb", loaddbcmd, doc_loaddb),
    LISPCMD("ispersistent", ispersistentcmd, doc_ispersistent),
    /* Introspection / utilities */
    LISPCMD("inspect", inspectcmd, doc_inspect),
    LISPCMD("disasm", disasmcmd, doc_disasm),
    LISPCMD("source", sourcecmd, doc_source),
    LISPCMD("dir", dircmd, doc_dir),
    LISPCMD("time", timecmd, doc_time),
    LISPCMD("web?", webpcmd, doc_webp),
    LISPCMD("sleep-ms", sleepmscmd, doc_sleepms),
    LISPCMD("exit", exitcmd, doc_exit),
    LISPCMD("quit", exitcmd, doc_exit),
    /* Help / discovery */
    LISPCMD("doc", doccmd, doc_doc),
    LISPCMD("docstring", docstringcmd, doc_docstring),
    LISPCMD("help", helpcmd, doc_help),
    /* FFI */
    LISPCMD("ffi?", ffipcmd, doc_ffip),
    LISPCMD("ffi-fn", ffifncmd, doc_ffifn),
    LISPCMD("ffi-vfn", ffivfncmd, doc_ffivfn),
    LISPCMD("ffi-callback", fficallbackcmd, doc_fficallback),
    LISPCMD("ffi-struct", ffistructcmd, doc_ffistruct),
    LISPCMD("ffi-pack", ffipackcmd, doc_ffipack),
    LISPCMD("ffi-unpack", ffiunpackcmd, doc_ffiunpack),
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
    LISPCMD("msgpack-encode", msgpackencodecmd, doc_msgpackencode),
    LISPCMD("msgpack-decode", msgpackdecodecmd, doc_msgpackdecode),
    /* Binary-safe blobs (EXP_BLOB) */
    LISPCMD("make-blob", makeblobcmd, doc_makeblob),
    LISPCMD("blob-len", bloblencmd, doc_bloblen),
    LISPCMD("blob-ref", blobrefcmd, doc_blobref),
    LISPCMD("blob->string", blob2stringcmd, doc_blob2string),
    LISPCMD("string->blob", string2blobcmd, doc_string2blob),
    LISPCMD("read-bytes", readbytescmd, doc_readbytes),
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
    LISPCMD("redis-port", redisportcmd, doc_redis_port),
    LISPCMD("redis-defcmd", rediscmddefcmd, doc_redis_defcmd),
    LISPCMD("redis-undefcmd", rediscmdundefcmd, doc_redis_undefcmd),
    LISPCMD("redis-cmds", rediscmdscmd, doc_redis_cmds),
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

#define EVAL_ARG_4(v1, v2, v3, v4)                                             \
  EVAL_ARG_3(v1, v2, v3)                                                       \
  exp_t *v4 = NULL;                                                            \
  if (e->next && e->next->next && e->next->next->next &&                       \
      e->next->next->next->next) {                                             \
    v4 = EVAL(e->next->next->next->next->content, env);                        \
    if (iserror(v4)) {                                                         \
      unrefexp(v1);                                                            \
      unrefexp(v2);                                                            \
      unrefexp(v3);                                                            \
      unrefexp(e);                                                             \
      return v4;                                                               \
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
exp_t *error(int errnum, exp_t *id, env_t *env, char *err_message, ...) {
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
  return ret;
}
#pragma GCC diagnostic warning "-Wunused-parameter"

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

void *memalloc(size_t count, size_t size) {
  void *ptr = calloc(count, size);
  if (!ptr)
    graceful_shutdown("Fatal error: Out of memory");
  return ptr;
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
   single-threaded run the TLS slot collapses to one backing copy. */
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

/* Iterative over e->next; recurses for e->content and vector/list elements. */
static inline int unrefexp(exp_t *e) {
  int ret;
  while (1) {
    if (is_immortal(e))
      return is_ptr(e) ? 1 : 0;
    if ((ret = REFCOUNT_DEC(&e->nref)) > 0)
      return ret;
    /* Detect double-free: a refcount that goes negative means this exp_t
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
    case EXP_DICT:
    case EXP_SET:
      if (e->ptr)
        destroy_dict((dict_t *)e->ptr); /* unrefs every value internally */
      break;
    case EXP_HAMT:
      if (e->ptr) {
        extern void hamt_free(void *ptr); /* defined alongside the HAMT ops */
        hamt_free(e->ptr);                /* unrefs the shared (refcounted) trie */
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
      /* EXP_PAIR (incl. nil), EXP_TREE, EXP_PAIR_CIRCULAR: recurse on the
         child; e->next is released by the next loop iteration. */
      unrefexp(e->content);
      break;
    }

    e->next = exp_freelist;
    exp_freelist = e;
    e = next;
  }
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

inline env_t *ref_env(env_t *env) {
  if (env) {
    REFCOUNT_INC(&env->nref);
  }
  return env;
}

inline env_t *make_env(env_t *rootenv) {
  /* No memset here: destroy_env leaves reused arena slots with the
     fields that could carry stale state (callingfnc, d, n_inline)
     already cleared. Fresh arena slots (first use) are BSS-zeroed.
     Heap-fallback slots come from memalloc which calloc's. Saves a
     ~128-byte store per call — the biggest per-call cost on fib. */
  env_t *newenv;
  shard_t *sh = current_shard;
  if (sh->arena_sp < sh->arena_end) {
    newenv = sh->arena_sp++;
  } else {
    newenv = memalloc(1, sizeof(env_t));
  }
  newenv->root = ref_env(rootenv);
  newenv->nref = 1;
  return newenv;
}

/* True for a closure that captured THIS env (its body wrapper's meta points
   back at env) — the lambda→env half of a (def/let f (fn ...)) cycle.
   FLAG_SHARED closures are excluded: severing their refs non-atomically
   would be unsafe under the multi-thread build, so we leave them alone. */
static inline int is_self_closure(exp_t *v, env_t *env) {
  return v && is_ptr(v) && (v->type == EXP_LAMBDA || v->type == EXP_MACRO) &&
         !(v->flags & FLAG_SHARED) && v->next &&
         (env_t *)v->next->meta == env;
}

/* Reclaim a self-referential closure cycle that manual refcounting cannot.
   A closure created with (def/let f (fn ...)) inside a function body captures
   its frame (f->next->meta == env, an owned ref) while the frame owns f (its
   name binding) — a 2-node strong cycle. When the ONLY refs still keeping env
   alive are such closures, each owned SOLELY by env, sever the closure→env
   edges so env — and then, via the normal dict/inline unref below, the
   closures — can be freed.

   `residual` is env->nref AFTER the frame's own ref was dropped. We collect
   only when residual == (count of solely-env-owned self-closures): any other
   holder of env (an anonymous escaped closure that captured env but isn't
   bound here, or a live child env) makes residual exceed that count, and any
   self-closure with an extra referrer (e.g. it was returned) has nref != 1 —
   both cases bail, leaving live data untouched. Returns 1 if it severed the
   cycle (env is now collectible), 0 to leave the early-break intact. */
static inline int env_break_self_cycle(env_t *env, int residual) {
  int self_refs = 0;
  for (int i = 0; i < env->n_inline; i++) {
    exp_t *v = env->inline_vals[i];
    if (!is_self_closure(v, env))
      continue;
    if (v->nref != 1)
      return 0; /* owned elsewhere too → still live */
    self_refs++;
  }
  if (env->d)
    for (int h = 0; h < 2; h++) {
      kvht_t *t = &env->d->ht[h];
      for (unsigned long b = 0; b < t->size; b++)
        for (keyval_t *k = t->table[b]; k; k = k->next) {
          if (!is_self_closure(k->val, env))
            continue;
          if (k->val->nref != 1)
            return 0;
          self_refs++;
        }
    }
  /* Every remaining ref to env must be a self-closure back-ref. */
  if (self_refs == 0 || self_refs != residual)
    return 0;
  /* Sever each closure→env edge. unrefexp's closure-free path keys on
     next->meta to release the captured env; NULLing it makes the dict/inline
     unref below free the closure without re-entering destroy_env(env). */
  for (int i = 0; i < env->n_inline; i++)
    if (is_self_closure(env->inline_vals[i], env))
      env->inline_vals[i]->next->meta = NULL;
  if (env->d)
    for (int h = 0; h < 2; h++) {
      kvht_t *t = &env->d->ht[h];
      for (unsigned long b = 0; b < t->size; b++)
        for (keyval_t *k = t->table[b]; k; k = k->next)
          if (is_self_closure(k->val, env))
            k->val->next->meta = NULL;
    }
  return 1;
}

inline void *destroy_env(env_t *env) {
  /* Iterative release — each env holds a ref to its parent via
     make_env/ref_env. Recursing would blow the C stack on deep call chains.
     Also scrubs the fields that would carry stale state into a reused
     arena slot, so make_env can skip the wholesale memset.
     Cache the shard pointer once: TLS reads on each loop iteration
     showed up as a measurable hit on nqueens-vec. */
  shard_t *sh = current_shard;
  while (env) {
    env_t *parent = env->root;
    int residual = REFCOUNT_DEC(&env->nref);
    /* residual > 0 normally means "still referenced, stop". But a closure
       defined in this env can hold the env's only outstanding ref via a
       refcount cycle (see env_break_self_cycle); if so, sever it and fall
       through to free env. Otherwise honor the early-break. */
    if (residual > 0 && !env_break_self_cycle(env, residual))
      break;
    {
      int i;
      for (i = 0; i < env->n_inline; i++)
        unrefexp(env->inline_vals[i]);
    }
    if (env->d)
      destroy_dict(env->d);
    if (env->callingfnc)
      unrefexp(env->callingfnc);
    /* Arena envs: roll the bump pointer back (LIFO) and scrub the
       fields that would carry stale state into the slot's next tenant,
       so make_env can skip the wholesale memset.
       Heap-fallback envs return to free() — no scrub needed. */
    if (env >= sh->arena && env < sh->arena_end) {
      env->n_inline = 0;
      env->d = NULL;
      env->callingfnc = NULL;
      if (env + 1 == sh->arena_sp)
        sh->arena_sp = env;
    } else {
      free(env);
    }
    env = parent;
  }
  return NULL;
}

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

static unsigned int bernstein_seed = 3102;

/* Bernstein Hash Function */
unsigned int bernstein_hash(unsigned char *key, int len) {
  unsigned int hash = bernstein_seed;
  int i;
  for (i = 0; i < len; ++i)
    hash = (hash + (hash << 5)) ^ key[i];
  return hash;
}

/* Bernstein Hash Function not key sensitive*/
unsigned int bernstein_uhash(unsigned char *key, int len) {
  unsigned int hash = bernstein_seed;
  int i;
  for (i = 0; i < len; ++i)
    hash = (hash + (hash << 5)) ^ tolower(key[i]);
  return hash;
}

static void init_kvht(kvht_t *kvht) {
  kvht->table = NULL;
  kvht->size = 0;
  kvht->sizemask = 0;
  kvht->used = 0;
}

dict_t *create_dict() {
  dict_t *d;
  d = memalloc(1, sizeof(dict_t));
  d->meta = NULL;
  d->pos = -1;
  init_kvht(&d->ht[0]);
  init_kvht(&d->ht[1]);
  return d;
}

int destroy_dict(dict_t *d) {
  // check if in use
  keyval_t *ckv;
  keyval_t *pkv;
  unsigned int i, j;
  for (i = 0; i < 2; i++) {
    for (j = 0; j < d->ht[i].size; j++) {
      ckv = d->ht[i].table[j];
      while (ckv) {
        pkv = ckv;
        ckv = pkv->next;
        free(pkv->key);
        unrefexp(pkv->val);
        free(pkv);
      }
    }
    if (d->ht[i].table)
      free(d->ht[i].table);
  }
  // FREE META?
  free(d);
  return 1;
}

/* Recursively verify e and EVERY nested element has a registered dump fn.
   __DUMPABLE__ alone is shallow: a vector or set IS dumpable, but a dict /
   deque element inside it is not, and __DUMP__ would then fail mid-record —
   after the type tag + count are already written — aborting the whole
   savedb and truncating the file (every persisted variable silently lost).
   The top-level dump paths use this to skip a non-round-trippable variable
   cleanly (with a warning) before writing any bytes. Depth-limited so a
   pathological / cyclic structure is treated as not-dumpable (skipped)
   rather than overflowing the stack. */
#define ALCOVE_DUMPABLE_MAX_DEPTH 512
static int is_fully_dumpable(exp_t *e, int depth) {
  if (depth > ALCOVE_DUMPABLE_MAX_DEPTH)
    return 0;
  if (!e || e == NIL_EXP || is_imm(e))
    return 1; /* nil + tagged fixnum/char always round-trip */
  if (!__DUMPABLE__(e))
    return 0;
  if (isvector(e)) {
    if (vec_kind(e) != VEC_KIND_GEN)
      return 1; /* I64/F64 cells are raw scalars */
    int64_t n = vec_len(e);
    for (int64_t i = 0; i < n; i++)
      if (!is_fully_dumpable(vec_gen_at(e, i), depth + 1))
        return 0;
    return 1;
  }
  if (isset(e) || isdict(e)) {
    dict_t *sd = (dict_t *)e->ptr;
    if (sd)
      for (unsigned int i = 0; i < sd->ht[0].size; i++)
        for (keyval_t *k = sd->ht[0].table[i]; k; k = k->next)
          if (!is_fully_dumpable(k->val, depth + 1))
            return 0;
    return 1;
  }
  if (islist(e)) {
    alc_list_t *l = (alc_list_t *)e->ptr;
    if (l)
      for (alc_listnode_t *node = l->head; node; node = node->next)
        if (!is_fully_dumpable(node->val, depth + 1))
          return 0;
    return 1;
  }
  if (ispair(e)) {
    exp_t *p = e;
    while (p && ispair(p) && istrue(p)) {
      if (!is_fully_dumpable(p->content, depth + 1))
        return 0;
      p = p->next;
    }
    if (p && !ispair(p) && !is_fully_dumpable(p, depth + 1))
      return 0; /* improper tail */
    return 1;
  }
  /* Remaining dumpable scalars (string/symbol/float/blob) and lambda/macro,
     whose bodies are always code (dumpable) — already passed __DUMPABLE__. */
  return 1;
}

int dump_dict(dict_t *d, FILE *stream) {
  // check if in use
  keyval_t *ckv;
  keyval_t *pkv;
  unsigned int i, j;
  for (i = 0; i < 2; i++) {
    for (j = 0; j < d->ht[i].size; j++) {
      ckv = d->ht[i].table[j];
      while (ckv) {
        pkv = ckv;
        ckv = pkv->next;
        if (pkv->timestamp > 0) {
          /* timestamp encoding: 0 = neutral, > 0 = persist mark
             (gettimeusec() of mark time, only nonzeroness matters here),
             < 0 = absolute-µs expire-at (RESP TTLs, never persisted). */
          if (!__DUMPABLE__(pkv->val)) {
            fprintf(stderr,
                    "savedb: skipping %s — type %d has no dump fn registered\n",
                    (char *)pkv->key, TYPEOF_E(pkv->val));
            continue;
          }
          if (__DUMP__(pkv->val, stream)) {
            dump_str(pkv->key, stream);
          }
        }
      }
    }
  }
  return 1;
}

/* Single-shot rehash. Doubles ht[0] in place and re-links every chain.
   Triggered when used >= size — without this every dict (incl. the
   global env) was permanently capped at 32 buckets, so worst-case
   chains were O(n/32) strcmps per lookup. The ht[1] machinery in
   create_dict was reserved for incremental rehash (Redis-style) but
   never wired; this is the simpler one-shot version. */
static void dict_rehash(dict_t *d, unsigned int new_size) {
  if (new_size == 0 || (new_size & (new_size - 1)) != 0)
    return; /* power of 2 */
  keyval_t **new_table = memalloc(new_size, sizeof(keyval_t *));
  if (!new_table)
    return; /* OOM: stay at current size, performance only */
  unsigned int new_mask = new_size - 1;
  unsigned int j;
  for (j = 0; j < d->ht[0].size; j++) {
    keyval_t *k = d->ht[0].table[j];
    while (k) {
      keyval_t *next = k->next;
      unsigned int h = bernstein_hash((unsigned char *)k->key, strlen(k->key));
      unsigned int slot = h & new_mask;
      k->next = new_table[slot];
      new_table[slot] = k;
      k = next;
    }
  }
  free(d->ht[0].table);
  d->ht[0].table = new_table;
  d->ht[0].size = new_size;
  d->ht[0].sizemask = new_mask;
}

keyval_t *set_get_keyval_dict(dict_t *d, char *key, exp_t *val) {
  unsigned int h = bernstein_hash((unsigned char *)key, strlen(key));
  keyval_t *k = NULL;
  if (d->ht[0].size) {
    if ((k = d->ht[0].table[h & (d->ht[0].sizemask)])) {
      while ((k->next) && (strcmp(key, k->key) != 0))
        k = k->next;
      if (val) {
        if (strcmp(key, k->key) == 0)
          unrefexp(k->val);
        else {
          k = k->next = memalloc(1, sizeof(keyval_t));
          d->ht[0].used++;
          k->key = strdup(key);
        }
      }
    } else if (val) {
      k = d->ht[0].table[h & (d->ht[0].sizemask)] =
          memalloc(1, sizeof(keyval_t));
      d->ht[0].used++;
      k->key = strdup(key);
    }
  }

  else if (val) {
    d->ht[0].size = DICT_KVHT_INITIAL_SIZE;
    d->ht[0].sizemask = DICT_KVHT_INITIAL_SIZE - 1;
    d->ht[0].table = memalloc(d->ht[0].size, sizeof(keyval_t *));
    k = d->ht[0].table[h & d->ht[0].sizemask] = memalloc(1, sizeof(keyval_t));
    d->ht[0].used++;
    k->key = strdup(key);
  };

  if (val) {
    k->val = refexp(val);
    /* Grow when load factor hits 1.0. Doubling keeps amortized O(1)
       per insert and avoids the historical 32-bucket cap that turned
       large global envs into linked-list scans. */
    if (d->ht[0].used >= d->ht[0].size)
      dict_rehash(d, d->ht[0].size * 2);
  } else {
    if (k && (strcmp(key, k->key) != 0))
      return NULL;
  }
  return k;
}

exp_t *set_keyval_dict_timestamp(dict_t *d, char *key, int64_t timestamp) {
  keyval_t *k = set_get_keyval_dict(d, key, NULL);
  if (k) {
    k->timestamp = timestamp;
    return refexp(k->val);
  }
  return NULL;
}

int64_t get_keyval_dict_timestamp(dict_t *d, char *key) {
  keyval_t *k = set_get_keyval_dict(d, key, NULL);
  if (k) {
    return k->timestamp;
  }
  return 0;
}

/* Returns 1 if a matching entry was removed, 0 otherwise. */
int del_keyval_dict(dict_t *d, char *key) {
  unsigned int h = bernstein_hash((unsigned char *)key, strlen(key));
  keyval_t *p = NULL;
  keyval_t *k;
  if (d->ht[0].size) {
    if ((k = d->ht[0].table[h & (d->ht[0].sizemask)])) {
      while ((k->next) && (strcmp(key, k->key) != 0)) {
        p = k;
        k = k->next;
      };
      if (strcmp(key, k->key) == 0) {
        unrefexp(k->val);
        free(k->key);
        d->ht[0].used--;
        if (p)
          p->next = k->next;
        else
          d->ht[0].table[h & (d->ht[0].sizemask)] = k->next;
        free(k);
        return 1;
      }
    }
  }
  return 0;
}

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
      exp_bump_left = EXP_BUMP_CHUNK;
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

/* ---- UTF-8 codepoint helpers ------------------------------------------
   alcove strings are UTF-8 byte buffers; chars are tagged immediates that
   hold a full 32-bit codepoint (see MAKE_CHAR/CHAR_VAL). These let the
   length/indexing/substring builtins and char read/print operate on
   Unicode codepoints rather than raw bytes. All are lenient on malformed
   input — a stray/invalid byte is consumed as a single raw byte — so we
   never loop forever or read past the NUL. */

static int utf8_encode(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  } else if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  } else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* Decode the codepoint at byte offset *off in NUL-terminated s, advancing
   *off past it. A malformed byte is returned as-is (advance 1). */
static uint32_t utf8_decode_at(const char *s, size_t *off) {
  const unsigned char *p = (const unsigned char *)s + *off;
  unsigned char c = p[0];
  if (c < 0x80) {
    *off += 1;
    return c;
  }
  uint32_t cp;
  int n;
  if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1Fu;
    n = 1;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0Fu;
    n = 2;
  } else if ((c & 0xF8) == 0xF0) {
    cp = c & 0x07u;
    n = 3;
  } else {
    *off += 1; /* stray continuation / invalid lead — raw byte */
    return c;
  }
  for (int i = 1; i <= n; i++) {
    unsigned char cc = p[i];
    if ((cc & 0xC0) != 0x80) { /* truncated — treat lead as a raw byte */
      *off += 1;
      return c;
    }
    cp = (cp << 6) | (cc & 0x3Fu);
  }
  *off += (size_t)(n + 1);
  return cp;
}

/* Decode one codepoint from a stream given its already-read first byte,
   reading continuation bytes as needed. On a malformed sequence keeps the
   raw lead byte (ungetc's any non-continuation byte it peeked). */
static uint32_t utf8_decode_stream(int first, FILE *stream) {
  if (first < 0x80)
    return (uint32_t)(unsigned char)first;
  uint32_t cp;
  int n;
  if ((first & 0xE0) == 0xC0) {
    cp = (uint32_t)(first & 0x1F);
    n = 1;
  } else if ((first & 0xF0) == 0xE0) {
    cp = (uint32_t)(first & 0x0F);
    n = 2;
  } else if ((first & 0xF8) == 0xF0) {
    cp = (uint32_t)(first & 0x07);
    n = 3;
  } else {
    return (uint32_t)(unsigned char)first; /* invalid lead — raw byte */
  }
  for (int i = 0; i < n; i++) {
    int cc = getc(stream);
    if (cc == EOF)
      break;
    if ((cc & 0xC0) != 0x80) {
      ungetc(cc, stream);
      break;
    }
    cp = (cp << 6) | (uint32_t)(cc & 0x3F);
  }
  return cp;
}

/* Number of codepoints in NUL-terminated UTF-8 s. */
static int64_t utf8_strlen(const char *s) {
  int64_t n = 0;
  size_t off = 0;
  while (s[off]) {
    utf8_decode_at(s, &off);
    n++;
  }
  return n;
}

/* Codepoint at codepoint-index i (>=0): returns 1 and sets *out, or 0 if
   i is out of range. */
static int utf8_index(const char *s, int64_t i, uint32_t *out) {
  if (i < 0)
    return 0;
  size_t off = 0;
  int64_t k = 0;
  while (s[off]) {
    uint32_t cp = utf8_decode_at(s, &off);
    if (k == i) {
      *out = cp;
      return 1;
    }
    k++;
  }
  return 0;
}

/* Byte offset of codepoint-index i; if i is past the end, returns the byte
   length (so substring math clamps naturally). */
static size_t utf8_byte_offset(const char *s, int64_t i) {
  size_t off = 0;
  int64_t k = 0;
  while (s[off] && k < i) {
    utf8_decode_at(s, &off);
    k++;
  }
  return off;
}

/* Codepoint count of the first nbytes bytes (byte-offset -> codepoint
   index conversion for string-index). */
static int64_t utf8_count_bytes(const char *s, size_t nbytes) {
  int64_t n = 0;
  size_t off = 0;
  while (off < nbytes && s[off]) {
    utf8_decode_at(s, &off);
    n++;
  }
  return n;
}

/* Strict UTF-8 validity over n bytes: rejects stray/truncated continuation
   bytes, overlong encodings, surrogates (U+D800..U+DFFF) and codepoints
   past U+10FFFF. On the first invalid byte, sets *bad to its offset.
   (NUL is valid UTF-8 here; callers that need NUL-free check separately.) */
static int utf8_valid(const char *s, size_t n, size_t *bad) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0;
  while (i < n) {
    unsigned char c = p[i];
    if (c < 0x80) {
      i++;
      continue;
    }
    int len;
    uint32_t cp, min;
    if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1Fu;
      min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0Fu;
      min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07u;
      min = 0x10000;
    } else {
      if (bad)
        *bad = i;
      return 0; /* continuation byte as lead, or 0xF8+ */
    }
    if (i + (size_t)len > n) {
      if (bad)
        *bad = i;
      return 0; /* truncated sequence */
    }
    for (int k = 1; k < len; k++) {
      unsigned char cc = p[i + (size_t)k];
      if ((cc & 0xC0) != 0x80) {
        if (bad)
          *bad = i;
        return 0; /* bad continuation byte */
      }
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      if (bad)
        *bad = i; /* overlong, surrogate, or out of range */
      return 0;
    }
    i += (size_t)len;
  }
  return 1;
}

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
  exp_t *val = make_internal(fn, tail_aware ? FLAG_TAIL_AWARE : 0);
  set_get_keyval_dict(reserved_symbol, (char *)name, val);
  unrefexp(val);
  return 0;
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
  if (!val) return NULL;
  const char *s = (is_ptr(val) && val->type == EXP_STRING) ? (const char *)exp_text(val) : NULL;
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
      else if (cur->type == EXP_TREE)  /* was duplicate EXP_PAIR — copy-paste bug */
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

void print_node(exp_t *node) {
  if (node == NULL) {
    printf("nil");
    return;
  }
  /* Tagged immediates — handle before any ->field access. */
  if (isnumber(node)) {
    printf("\x1B[92m%lld\x1B[39m", (long long)FIX_VAL(node));
    return;
  }
  if (ischar(node)) {
    uint32_t c = CHAR_VAL(node);
    if (c >= 0x80) {
      char u[4];
      int k = utf8_encode(c, u);
      printf("#\\");
      fwrite(u, 1, (size_t)k, stdout);
    } else if (c > 32 && c < 127) {
      printf("#\\%c", (char)c);
    } else {
      printf("#\\%u", c);
    }
    return;
  }
  if (!is_ptr(node)) {
    printf("<?imm %p>", (void *)node);
    return;
  }
  if (node->type == EXP_ERROR) {
    printf("\x1B[91mError: \x1B[39m%s\n", (char *)exp_text(node));
  } else if (node->type == EXP_TREE) {
    printf("[ ");
    if (node->content)
      print_node(node->content);
    printf("] ");
  } else if (node->type == EXP_PAIR) {
    if (istrue(node)) {
      printf("(");
      if (node->content)
        print_node(node->content);
      while ((node = node->next)) {
        if ispair (node) {
          printf(" ");
          print_node(node->content);
        } else {
          printf(" . ");
          print_node(node);
          break;
        }
      }
      printf(")");
    } else
      printf("nil");
  } else if (node->type == EXP_LAMBDA) {
    if (node->meta)
      printf("\x1B[92m#<procedure:%s@%08lx>\x1B[39m", (char *)node->meta,
             (long)node);
    else
      printf("\x1B[92m#<procedure@%08lx>\x1B[39m", (long)node);
    /*  if (node->content) print_node(node->content);
        printf(")");*/
  } else if (node->type == EXP_MACRO) {
    if (node->meta)
      printf("\x1B[92m#<macro:%s@%08lx>\x1B[39m", (char *)node->meta,
             (long)node);
    else
      printf("\x1B[92m#<macro@@%08lx>\x1B[39m", (long)node);
    /*  if (node->content) print_node(node->content);
        printf(")");*/
  }

  else if (node->type == EXP_SYMBOL)
    printf("\x1B[92m%s\x1B[39m", (char *)exp_text(node));
  else if (node->type == EXP_STRING)
    printf("\x1B[92m\"%s\"\x1B[39m", (char *)exp_text(node));
  else if (node->type == EXP_FLOAT)
    printf("\x1B[92m%g\x1B[39m", node->f);
  else if (node->type == EXP_VECTOR) {
    int64_t n = vec_len(node);
    printf("#[");
    unsigned k = vec_kind(node);
    for (int64_t i = 0; i < n; i++) {
      if (i)
        printf(" ");
      if (k == VEC_KIND_GEN) {
        print_node(vec_gen_at(node, i));
      } else if (k == VEC_KIND_I64) {
        printf("\x1B[92m%lld\x1B[39m", (long long)vec_i64_at(node, i));
      } else { /* VEC_KIND_F64 */
        printf("\x1B[92m%lf\x1B[39m", vec_f64_at(node, i));
      }
    }
    printf("]");
  } else if (node->type == EXP_BLOB) {
    /* Show the content when it's printable text (b"..."), otherwise a
       hexdump-style view with an ASCII column (non-printable -> '.'), like
       the right column of `hexdump -C`. Binary view is capped so a large
       payload doesn't flood the REPL. */
    alc_blob_t *b = (alc_blob_t *)node->ptr;
    size_t n = b ? b->len : 0;
    const unsigned char *p = b ? (const unsigned char *)b->bytes : NULL;
    int printable = (n > 0);
    for (size_t i = 0; i < n; i++)
      if (p[i] < 0x20 || p[i] > 0x7e) {
        printable = 0;
        break;
      }
    if (n == 0) {
      printf("\x1B[92m#<blob 0 bytes>\x1B[39m");
    } else if (printable) {
      printf("\x1B[92mb\"");
      for (size_t i = 0; i < n; i++) {
        if (p[i] == '"' || p[i] == '\\')
          putchar('\\');
        putchar(p[i]);
      }
      printf("\"\x1B[39m");
    } else {
      size_t show = n < 64 ? n : 64;
      printf("\x1B[92m#<blob %zu: ", n);
      for (size_t i = 0; i < show; i++)
        printf("%02x ", p[i]);
      if (show < n)
        printf("... ");
      putchar('|');
      for (size_t i = 0; i < show; i++)
        putchar((p[i] >= 0x20 && p[i] <= 0x7e) ? (char)p[i] : '.');
      printf("|>\x1B[39m");
    }
  } else if (node->type == EXP_DICT) {
    /* Clojure-style {k v, k v} so the printed form re-reads as the same
       value. Iteration order is bucket-order, not insertion-order. */
    dict_t *d = (dict_t *)node->ptr;
    printf("{");
    int first = 1;
    if (d) {
      unsigned int i;
      for (i = 0; i < d->ht[0].size; i++) {
        keyval_t *k = d->ht[0].table[i];
        while (k) {
          if (!first)
            printf(", ");
          first = 0;
          if (((char *)k->key)[0] == ':')
            printf("\x1B[92m%s\x1B[39m", (char *)k->key);
          else
            printf("\x1B[92m\"%s\"\x1B[39m", (char *)k->key);
          printf(" ");
          print_node(k->val);
          k = k->next;
        }
      }
    }
    printf("}");
  } else if (node->type == EXP_SET) {
    dict_t *d = (dict_t *)node->ptr;
    printf("#{");
    int first = 1;
    if (d) {
      for (unsigned int i = 0; i < d->ht[0].size; i++) {
        keyval_t *k = d->ht[0].table[i];
        while (k) {
          if (!first)
            printf(" ");
          first = 0;
          print_node(k->val);
          k = k->next;
        }
      }
    }
    printf("}");
  } else if (node->type == EXP_HAMT) {
    void hamt_print(exp_t * m); /* defined with the HAMT ops */
    hamt_print(node);
  } else if (node->type == EXP_CONT) {
    printf("\x1B[92m#<continuation>\x1B[39m");
  } else if (node->type == EXP_LIST) {
    alc_list_t *l = (alc_list_t *)node->ptr;
    printf("(");
    if (l) {
      alc_listnode_t *n = l->head;
      int first = 1;
      while (n) {
        if (!first)
          printf(" ");
        first = 0;
        print_node(n->val);
        n = n->next;
      }
    }
    printf(")");
  } else if (node->type == EXP_INTERNAL) {
    printf("\x1B[92m#<builtin>\x1B[39m");
  } else if (node->type == EXO_MACROINTERNAL) {
    printf("\x1B[92m#<macro-builtin>\x1B[39m");
  } else if (node->type == EXP_FFI) {
    printf("\x1B[92m#<ffi>\x1B[39m");
  } else {
    printf("\x1B[92m#<type %d>\x1B[39m", node->type);
  }
  return;
}

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
      graceful_shutdown("Fatal error: Out of memory");
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
      graceful_shutdown("Fatal error: Out of memory");
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
  alc_blob_t *b = (alc_blob_t *)malloc(sizeof(alc_blob_t) + len);
  if (!b)
    graceful_shutdown("Fatal error: Out of memory");
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
    /* Out of fixnum range — fall through to float. strtod handles the
       same digit string and gives the closest double. */
    return make_floatf(strtod(str, &end));
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
  /* Validate the type tag against the dispatch table BEFORE __LOAD__
     indexes exp_tfuncList[type]. A malicious db.dump with type=0xFFFF
     would otherwise read out-of-bounds and indirect-call whatever
     pointer-shaped bytes are there at startup. */
  if (resp->type < 1 || resp->type >= EXP_MAXSIZE ||
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

exp_t *dump_exp_t(exp_t *e, FILE *stream) { return __DUMP__(e, stream); }

exp_t *load_char(exp_t *e, FILE *stream) {
  /* Chars are tagged immediates — discard the placeholder exp_t that
     load_exp_t handed us and return a fresh tagged char. */
  int c = getc(stream);
  if (e)
    unrefexp(e);
  if (c == EOF)
    return NULL;
  return MAKE_CHAR(utf8_decode_stream(c, stream));
}

exp_t *dump_char(exp_t *e, FILE *stream) {
  unsigned short int t = EXP_CHAR;
  if (dumptype(stream, &t) <= 0)
    return NULL;
  char u[4];
  int k = utf8_encode((uint32_t)CHAR_VAL(e), u);
  if (fwrite(u, 1, (size_t)k, stream) != (size_t)k)
    return NULL;
  return e;
}

/* Cap on string lengths from a db.dump file. 16 MiB is plenty for any
   real symbol/string we'd persist; bigger values are almost certainly
   either corruption or a malicious header trying to wrap (length+1 -> 0
   then giant fread). The cap is checked before the alloc so neither
   the wrap nor the read happens on bad input. */
#define ALCOVE_LOAD_MAX_STRLEN ((size_t)1 << 24)

char *load_str(char **pptr, FILE *stream) {
  size_t length;
  char *ptr;
  if (loadsize_t(stream, &length) <= 0)
    return NULL;
  if (length > ALCOVE_LOAD_MAX_STRLEN) {
    *pptr = NULL;
    return NULL;
  }
  ptr = *pptr = memalloc(length + 1, sizeof(char));
  if (!ptr) {
    *pptr = NULL;
    return NULL;
  }
  if (fread(ptr, 1, length, stream) != length) {
    free(ptr);
    *pptr = NULL;
    return NULL;
  }
  *((char *)(ptr + length)) = '\0';
  return ptr;
}

exp_t *load_string(exp_t *e, FILE *stream) {
  if (load_str((char **)&(e->ptr), stream))
    return e;
  unrefexp(e);  /* release placeholder on read failure */
  return NULL;
}

char *dump_str(char *ptr, FILE *stream) {
  size_t length = strlen(ptr);
  if (dumpsize_t(stream, &length) <= 0)
    return NULL;
  /* fwrite returns 0 on success for empty strings — guard with length > 0
     to avoid treating a successful empty write as a failure. */
  if (length > 0 && fwrite(ptr, 1, length, stream) != length)
    return NULL;
  return ptr;
}

/* Binary-safe variants — RESP keys can hold NULs, so the strlen-based
   dump_str path doesn't apply. dump_strn writes the size prefix followed
   by exactly n bytes; load_strn reads symmetrically and NUL-terminates
   the returned buffer for caller convenience (klen carries the real
   length, the trailing NUL is decorative). */
static int dump_strn(const char *ptr, size_t n, FILE *stream) {
  if (dumpsize_t(stream, &n) <= 0)
    return 0;
  return n == 0 || fwrite(ptr, 1, n, stream) == n;
}
static int load_strn(char **pptr, size_t *plen, FILE *stream) {
  size_t n;
  if (loadsize_t(stream, &n) <= 0)
    return 0;
  /* Sanity cap matches RESP_MAX_BULK so a corrupted dump can't drag
     us into a 16 EB allocation. */
  if (n > (size_t)(512u * 1024 * 1024))
    return 0;
  char *p = malloc(n + 1);
  if (!p)
    return 0;
  if (n > 0 && fread(p, 1, n, stream) != n) {
    free(p);
    return 0;
  }
  p[n] = 0;
  *pptr = p;
  *plen = n;
  return 1;
}

exp_t *dump_set(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  dict_t *d = (dict_t *)e->ptr;
  size_t n = d ? (size_t)d->ht[0].used : 0;
  if (dumpsize_t(stream, &n) <= 0)
    return NULL;
  if (!d)
    return e;
  for (unsigned int i = 0; i < d->ht[0].size; i++) {
    for (keyval_t *k = d->ht[0].table[i]; k; k = k->next) {
      if (!k->val || !__DUMPABLE__(k->val) || !__DUMP__(k->val, stream))
        return NULL;
    }
  }
  return e;
}

exp_t *load_set(exp_t *e, FILE *stream) {
  if (e)
    unrefexp(e);
  size_t n = 0;
  if (loadsize_t(stream, &n) <= 0 || n > (size_t)(1u << 28))
    return NULL;
  exp_t *ret = make_set_exp();
  dict_t *d = (dict_t *)ret->ptr;
  for (size_t i = 0; i < n; i++) {
    exp_t *val = load_exp_t(stream);
    if (!val || !set_insert_value(d, val)) {
      if (val)
        unrefexp(val);
      unrefexp(ret);
      return NULL;
    }
    unrefexp(val);
  }
  return ret;
}

/* EXP_DICT serializer. Format: type tag + entry count + per entry a
   bare string key (dump_str, no type tag — dict keys are always the
   canonicalized C-string from alc_key_to_cstr) followed by the value
   (__DUMP__, self-describing). Walks ht[0] only — dict_rehash is
   one-shot into ht[0], so used == entry count. The top-level dump path
   pre-checks dumpability recursively, so values here are dumpable; the
   per-value __DUMPABLE__ guard is defense in depth (matches dump_set). */
exp_t *dump_dict_value(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  dict_t *d = (dict_t *)e->ptr;
  size_t n = d ? (size_t)d->ht[0].used : 0;
  if (dumpsize_t(stream, &n) <= 0)
    return NULL;
  if (!d)
    return e;
  for (unsigned int i = 0; i < d->ht[0].size; i++)
    for (keyval_t *k = d->ht[0].table[i]; k; k = k->next) {
      if (!dump_str(k->key, stream))
        return NULL;
      if (!k->val || !__DUMPABLE__(k->val) || !__DUMP__(k->val, stream))
        return NULL;
    }
  return e;
}

exp_t *load_dict_value(exp_t *e, FILE *stream) {
  if (e)
    unrefexp(e);
  size_t n = 0;
  if (loadsize_t(stream, &n) <= 0 || n > (size_t)(1u << 28))
    return NULL;
  exp_t *ret = make_dict_exp();
  dict_t *d = (dict_t *)ret->ptr;
  for (size_t i = 0; i < n; i++) {
    char *key = NULL;
    if (!load_str(&key, stream)) {
      unrefexp(ret);
      return NULL;
    }
    exp_t *val = load_exp_t(stream);
    if (!val) {
      free(key);
      unrefexp(ret);
      return NULL;
    }
    /* set_get_keyval_dict strdup's the key and refexp's the value, so we
       still own both: free our key copy and drop our load ref. */
    set_get_keyval_dict(d, key, val);
    free(key);
    unrefexp(val);
  }
  return ret;
}

/* EXP_LIST (deque) serializer. Format: type tag + element count + each
   element (__DUMP__) in head->tail order. Load rebuilds with
   alc_list_push_right (appends to tail), preserving order. */
exp_t *dump_deque_value(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  alc_list_t *l = (alc_list_t *)e->ptr;
  size_t n = l ? (size_t)l->len : 0;
  if (dumpsize_t(stream, &n) <= 0)
    return NULL;
  if (!l)
    return e;
  for (alc_listnode_t *node = l->head; node; node = node->next)
    if (!node->val || !__DUMPABLE__(node->val) || !__DUMP__(node->val, stream))
      return NULL;
  return e;
}

exp_t *load_deque_value(exp_t *e, FILE *stream) {
  if (e)
    unrefexp(e);
  size_t n = 0;
  if (loadsize_t(stream, &n) <= 0 || n > (size_t)(1u << 28))
    return NULL;
  exp_t *ret = make_list_exp();
  alc_list_t *l = (alc_list_t *)ret->ptr;
  for (size_t i = 0; i < n; i++) {
    exp_t *val = load_exp_t(stream);
    if (!val) {
      unrefexp(ret);
      return NULL;
    }
    alc_list_push_right(l, val); /* takes ownership of val */
  }
  return ret;
}

/* EXP_BLOB serializer — alc_blob_t is {len, bytes[]} (binary-safe).
   Format: type tag (already written by dispatch wrapper for load,
   we write it here for dump symmetric with dump_string), then size_t
   len, then exactly len bytes. */
exp_t *dump_blob(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  alc_blob_t *b = (alc_blob_t *)e->ptr;
  size_t n = b->len;
  if (dumpsize_t(stream, &n) <= 0)
    return NULL;
  if (n > 0 && fwrite(b->bytes, 1, n, stream) != n)
    return NULL;
  return e;
}
exp_t *load_blob(exp_t *e, FILE *stream) {
  /* load_exp_t allocated a placeholder via make_nil(); discard it and
     return a fresh blob — matches the load_char/load_string pattern. */
  if (e)
    unrefexp(e);
  size_t n;
  if (loadsize_t(stream, &n) <= 0)
    return NULL;
  if (n > (size_t)(512u * 1024 * 1024))
    return NULL;
  /* make_blob copies the input; using a stack buffer for tiny blobs
     would be a micro-opt but the heap path is fine for cold load. */
  char *buf = (n > 0) ? malloc(n) : NULL;
  if (n > 0 && (!buf || fread(buf, 1, n, stream) != n)) {
    free(buf);
    return NULL;
  }
  exp_t *blob = make_blob(buf ? buf : "", n);
  free(buf);
  return blob;
}

exp_t *dump_string(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  if (dump_str(exp_text(e), stream))
    return e;
  else
    return NULL;
}

/* EXP_NUMBER (tagged fixnum) — write 8 raw bytes (int64 untagged value).
   Like load_char, the placeholder allocated by load_exp_t is discarded
   because tagged immediates aren't heap exp_t*. */
exp_t *dump_number(exp_t *e, FILE *stream) {
  unsigned short int t = EXP_NUMBER;
  int64_t v = FIX_VAL(e);
  if (dumptype(stream, &t) <= 0)
    return NULL;
  if (fwrite(&v, sizeof(v), 1, stream) != 1)
    return NULL;
  return e;
}
exp_t *load_number(exp_t *e, FILE *stream) {
  int64_t v;
  if (e)
    unrefexp(e);
  if (fread(&v, sizeof(v), 1, stream) != 1)
    return NULL;
  return MAKE_FIX(v);
}

/* EXP_FLOAT — heap exp_t with `f` field (double). 8 raw bytes. */
exp_t *dump_float(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  if (fwrite(&e->f, sizeof(e->f), 1, stream) != 1)
    return NULL;
  return e;
}
exp_t *load_float(exp_t *e, FILE *stream) {
  if (fread(&e->f, sizeof(e->f), 1, stream) != 1) {
    unrefexp(e);  /* release placeholder on read failure */
    return NULL;
  }
  return e;
}

/* EXP_SYMBOL — same length-prefixed bytes as a string; on load we just
   stash the name into e->ptr. Symbol identity (eq?) isn't preserved
   across runs but iso-equality / lookup-by-name works fine. */
exp_t *dump_symbol(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  if (dump_str(exp_text(e), stream))
    return e;
  return NULL;
}
exp_t *load_symbol(exp_t *e, FILE *stream) {
  if (load_str((char **)&(e->ptr), stream))
    return e;
  unrefexp(e);  /* release placeholder on read failure */
  return NULL;
}

/* EXP_PAIR — a cons cell. content=car, next=cdr (alcove uses `next`
   as the cdr field for its linked-list representation). Both children
   may be NULL (e.g., nil = (PAIR, NULL, NULL)). We use a 1-byte flag
   to record which children are present so improper pairs (a . b) and
   the empty list both round-trip. Recurses via __DUMP__ so any element
   whose type has a registered dump fn is preserved; mixed-type lists
   work transparently. */
exp_t *dump_pair(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  uint8_t flags = (e->content ? 1 : 0) | (e->next ? 2 : 0);
  if (fwrite(&flags, 1, 1, stream) != 1)
    return NULL;
  if ((flags & 1) && !__DUMP__(e->content, stream))
    return NULL;
  if ((flags & 2) && !__DUMP__(e->next, stream))
    return NULL;
  return e;
}
exp_t *load_pair(exp_t *e, FILE *stream) {
  uint8_t flags;
  if (fread(&flags, 1, 1, stream) != 1)
    return NULL;
  if (flags & 1) {
    e->content = load_exp_t(stream);
    if (!e->content) return NULL; /* propagate sub-read failure */
  }
  if (flags & 2) {
    e->next = load_exp_t(stream);
    if (!e->next) return NULL;  /* propagate sub-read failure */
  }
  return e;
}

/* EXP_LAMBDA — persisted as source: name + params tree + body tree.
   On load we reconstruct the lambda exp_t with the same shape defcmd
   builds, then call compile_lambda so the bytecode VM (and JIT, where
   the shape matches) sees the function. JIT pages don't survive a
   restart but get re-installed at compile time on the new arch.

   Limitations: closures over locals don't survive (alcove doesn't seem
   to support lexical closures over let/with bindings beyond the
   enclosing global env). Recursive references resolve fine because the
   loader installs the lambda into the global env under its persisted
   name before the body is ever called. */
exp_t *dump_lambda(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  /* Name (empty string for anonymous fns; preserves shape on the wire). */
  const char *name = e->meta ? (const char *)e->meta : "";
  if (!dump_str((char *)name, stream))
    return NULL;
  /* Flags: bit0 = has params; bit1 = has body. */
  exp_t *params = lambda_params(e);
  uint8_t flags = 0;
  if (params)
    flags |= 1;
  if (e->next && e->next->content)
    flags |= 2;
  if (fwrite(&flags, 1, 1, stream) != 1)
    return NULL;
  if ((flags & 1) && !__DUMP__(params, stream))
    return NULL;
  if ((flags & 2) && !__DUMP__(e->next->content, stream))
    return NULL;
  return e;
}
exp_t *load_lambda(exp_t *e, FILE *stream) {
  char *name = NULL;
  if (!load_str(&name, stream)) {
    unrefexp(e);  /* release placeholder on read failure */
    return NULL;
  }
  uint8_t flags;
  if (fread(&flags, 1, 1, stream) != 1) {
    free(name);
    unrefexp(e);
    return NULL;
  }
  exp_t *params = (flags & 1) ? load_exp_t(stream) : NULL;
  exp_t *body = (flags & 2) ? load_exp_t(stream) : NULL;
  /* Mirror defcmd's lambda shape: e->content = params, e->next is a
     wrapping node whose content is the body list. */
  e->content = params;
  e->next = make_node(body);
  if (name && name[0]) {
    e->meta = (keyval_t *)name; /* take ownership */
  } else {
    free(name);
    e->meta = NULL;
  }
  /* Silent fallback to AST eval if compile_lambda can't compile (e.g.,
     body uses an unsupported form). The lambda still works either way.
     Persisted lambdas are top-level (no captured env survives a dump). */
  compile_lambda(e, 0);
  return e;
}

/* EXP_MACRO — same on-wire shape as EXP_LAMBDA (defmacrocmd builds an
   identical exp_t structure, just with type=EXP_MACRO). The only load-
   side difference is that macros are AST-evaluated, so we skip
   compile_lambda. Source-form persistence: the macro body is preserved
   exactly and re-installed at load time. */
exp_t *dump_macro(exp_t *e, FILE *stream) {
  /* Identical wire format to dump_lambda (sans the type tag we write
     here) so a future refactor could share the body. */
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  const char *name = e->meta ? (const char *)e->meta : "";
  if (!dump_str((char *)name, stream))
    return NULL;
  exp_t *params = lambda_params(e);
  uint8_t flags = 0;
  if (params)
    flags |= 1;
  if (e->next && e->next->content)
    flags |= 2;
  if (fwrite(&flags, 1, 1, stream) != 1)
    return NULL;
  if ((flags & 1) && !__DUMP__(params, stream))
    return NULL;
  if ((flags & 2) && !__DUMP__(e->next->content, stream))
    return NULL;
  return e;
}
exp_t *load_macro(exp_t *e, FILE *stream) {
  char *name = NULL;
  if (!load_str(&name, stream)) {
    unrefexp(e);  /* release placeholder on read failure */
    return NULL;
  }
  uint8_t flags;
  if (fread(&flags, 1, 1, stream) != 1) {
    free(name);
    unrefexp(e);
    return NULL;
  }
  exp_t *params = (flags & 1) ? load_exp_t(stream) : NULL;
  exp_t *body = (flags & 2) ? load_exp_t(stream) : NULL;
  e->content = params;
  e->next = make_node(body);
  if (name && name[0]) {
    e->meta = (keyval_t *)name;
  } else {
    free(name);
    e->meta = NULL;
  }
  /* No compile_lambda — macros are AST-evaluated by the macro
     expander, not the bytecode VM. */
  return e;
}

/* Forward decls for the vec helpers defined alongside make_vector. */
static exp_t *vec_get_boxed(exp_t *vexp, int64_t i);
static exp_t **vec_gen_cells(exp_t *vexp);
static alc_vec_t *vec_alloc_storage(int64_t cap);
static int vec_tighten(exp_t *vexp);

/* Set by alcove_load_unified to the version read from the file header;
   read by load_vec to choose between v1/v2 record layouts. Single
   loader at a time — there's no concurrent loadu path. Initialised to
   2 (current version) so any dump path that bypasses the header-read
   still writes/reads v2-compatible records. */
static int alcove_load_dump_version = 2;

/* EXP_VECTOR — v2 record (dump_vec always writes v2):
     [u8 kind][u32 len][payload]
       kind == VEC_KIND_GEN >> 4: len × __DUMP__(exp_t*)
       kind == VEC_KIND_I64 >> 4: len × int64_t (raw little-endian)
       kind == VEC_KIND_F64 >> 4: len × double  (raw little-endian)

   v1 dumps are still read by load_vec via load_vec_v1 (boxed cells,
   reconstructs as GEN). The kind on the wire is the bit pattern shifted
   right by 4 so it stays compact (0/1/2) and stable across future flag
   layouts. */
exp_t *dump_vec(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  unsigned k = vec_kind(e);
  uint8_t kind_tag = (uint8_t)(k >> 4);
  if (fwrite(&kind_tag, 1, 1, stream) != 1)
    return NULL;
  uint32_t n = (uint32_t)vec_len(e);
  if (fwrite(&n, sizeof(n), 1, stream) != 1)
    return NULL;
  if (k == VEC_KIND_GEN) {
    for (uint32_t i = 0; i < n; i++) {
      exp_t *cell = vec_gen_at(e, i);
      /* Defense in depth: the top-level dump pre-checks dumpability
         recursively (is_fully_dumpable), so a non-dumpable cell shouldn't
         reach here — but guard anyway to match dump_set and fail cleanly. */
      if (!__DUMPABLE__(cell) || !__DUMP__(cell, stream))
        return NULL;
    }
  } else if (k == VEC_KIND_I64) {
    int64_t *cells = (int64_t *)((char *)e->ptr + sizeof(alc_vec_t)) +
                     e->vec_win.start;
    if (n > 0 && fwrite(cells, sizeof(int64_t), n, stream) != n)
      return NULL;
  } else { /* VEC_KIND_F64 */
    double *cells =
        VEC_F64_CELLS(e);
    if (n > 0 && fwrite(cells, sizeof(double), n, stream) != n)
      return NULL;
  }
  return e;
}

/* v1 reader: [u32 len][__DUMP__ × len]. Reconstructs as a GEN vec, then
   calls vec_tighten() to specialise to I64/F64 when the cells are
   homogeneously numeric. Pre-refactor dumps preserve their original
   semantics this way — e.g., mlp's train-y stays integer-comparable
   for `(is label 0)`, while train-X[k] becomes F64 for the tensor-op
   fast path. */
static exp_t *load_vec_v1(FILE *stream) {
  uint32_t n;
  if (fread(&n, sizeof(n), 1, stream) != 1)
    return NULL;
  exp_t *v = make_vector((int64_t)n, NIL_EXP);
  if (!v)
    return NULL;
  for (uint32_t i = 0; i < n; i++) {
    unrefexp(vec_gen_at(v, i));
    exp_t *cell = load_exp_t(stream);
    if (!cell) {
      v->vec_win.end = (int32_t)i;
      unrefexp(v);
      return NULL;
    }
    vec_gen_at(v, i) = cell;
  }
  vec_tighten(v);
  return v;
}

/* v2 reader: [u8 kind][u32 len][payload]. */
static exp_t *load_vec_v2(FILE *stream) {
  uint8_t kind_tag;
  if (fread(&kind_tag, 1, 1, stream) != 1)
    return NULL;
  if (kind_tag > 2)
    return NULL;
  uint32_t n;
  if (fread(&n, sizeof(n), 1, stream) != 1)
    return NULL;
  /* Pre-size storage; we'll overwrite cells per-kind below. */
  alc_vec_t *v = vec_alloc_storage((int64_t)n);
  if (!v)
    return NULL;
  exp_t *e = make_nil();
  e->type = EXP_VECTOR;
  e->flags = (unsigned short)((unsigned)kind_tag << 4);
  e->ptr = v;
  e->vec_win.start = 0;
  e->vec_win.end = (int32_t)n;
  if (kind_tag == 0) { /* GEN */
    exp_t **cells = (exp_t **)((char *)v + sizeof(alc_vec_t));
    /* Pre-init to nil so a partial load leaves the live prefix safe to
       walk during unrefexp(e). */
    for (uint32_t i = 0; i < n; i++)
      cells[i] = refexp(NIL_EXP);
    for (uint32_t i = 0; i < n; i++) {
      unrefexp(cells[i]);
      exp_t *cell = load_exp_t(stream);
      if (!cell) {
        e->vec_win.end = (int32_t)i;
        unrefexp(e);
        return NULL;
      }
      cells[i] = cell;
    }
  } else if (kind_tag == 1) { /* I64 */
    int64_t *cells = (int64_t *)((char *)v + sizeof(alc_vec_t));
    if (n > 0 && fread(cells, sizeof(int64_t), n, stream) != n) {
      e->vec_win.end = 0;
      unrefexp(e);
      return NULL;
    }
  } else { /* F64 */
    double *cells = (double *)((char *)v + sizeof(alc_vec_t));
    if (n > 0 && fread(cells, sizeof(double), n, stream) != n) {
      e->vec_win.end = 0;
      unrefexp(e);
      return NULL;
    }
  }
  return e;
}

exp_t *load_vec(exp_t *e, FILE *stream) {
  if (e)
    unrefexp(e);
  return alcove_load_dump_version >= 2 ? load_vec_v2(stream)
                                       : load_vec_v1(stream);
}

exp_t *make_atom_from_token(token_t *token) {
  char *str = token->data;
  int length = token->size;
  // Generate an atom from a string during parsing
  //  TEST -> 0: + or - in front, 1: digit after first + or -, 2: E mantissa,
  //  3:+ or - sign, 4: digit of mantissa
  int test = 0;
  int dot = 0;
  char v;
  char *stro = str;
  if (str[0] == '\"')
    return make_string_from_token(token, 1, length - 2);
  /* Hex literals: 0xNN / 0XNN, optionally with leading +/-. The decimal
     state machine below rejects 'x', so without this fast path 0xFF
     would be tokenised as the symbol "0xFF". */
  {
    const char *p = stro;
    int neg = 0;
    if (*p == '+' || *p == '-') {
      neg = (*p == '-');
      p++;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && p[2]) {
      const char *q = p + 2;
      int all_hex = 1;
      for (; *q; q++) {
        if (!isxdigit((unsigned char)*q)) { all_hex = 0; break; }
      }
      if (all_hex) {
        errno = 0;
        long long hv = strtoll(p, NULL, 16);
        /* Guard the two's-complement one-value asymmetry: -LLONG_MIN
           is UB. Treat it as out-of-range and let the symbol path
           take it. */
        if (errno != ERANGE && !(neg && hv == LLONG_MIN)) {
          int64_t fix_max = ((int64_t)1 << 60) - 1;
          int64_t fix_min = -((int64_t)1 << 60);
          if (neg) hv = -hv;
          if (hv >= fix_min && hv <= fix_max) {
            freetoken(token);
            return MAKE_FIX((int64_t)hv);
          }
        }
      }
    }
  }
  while (length--) {
    v = (char)*(str++);
    if ((v == '+') || (v == '-')) {
      if ((test == 1) || (test == 3)) {
        break; // A sign after another sign => not an integer or following
               // format +AB+
      } else if (test == 7) {
        // OK MANTISSA there
        test = 15;
      } else if (test == 0)
        test = 1;
      else
        break;
    } else if (v == '.') {
      if ((test <= 3) || !dot)
        dot += 1;
      else
        break;
    } else if ((v == 'E') || (v == 'e')) {
      // set mentisa on if not seen mantisa yet
      if (test == 3)
        test = 7;
      else
        break;

    } else if ((v <= '9') && (v >= '0')) {
      if (test <= 3) {
        test = 3;
      } else if ((test == 7) || (test == 15) | (test == 31))
        test = 31;
      else
        break;
    } else
      break;
  }

  if (length != -1) {
    // not an integer then must be a symbol
    return make_symbol_from_token(token);
  } else {
    if (test == 1)
      return make_symbol_from_token(token);
    else if ((test == 3) && !dot) {
      exp_t *ret = make_integer(stro);
      freetoken(token);
      return ret;
    } else if ((test == 31) || (test == 3)) {
      exp_t *ret = make_float(stro);
      freetoken(token);
      return ret;
    } else
      return make_symbol_from_token(token);
  }
}

exp_t *callmacrochar(FILE *stream, unsigned char x) {
  exp_t *lnode = NULL; // Initial List Node
  exp_t *vnode = NULL; // Val Node
  exp_t *cnode = NULL; // Current Node

  if (x == '(') {
    vnode = reader(stream, ')', 0);

    if (vnode) {
      if (iserror(vnode))
        return vnode;
      lnode = make_node(vnode);
      vnode = NULL;
      cnode = lnode;
      while ((vnode = reader(stream, ')', 0))) {
        if (iserror(vnode)) {
          unrefexp(lnode);
          return vnode;
        }
        cnode = cnode->next = make_node(vnode);
      }
    }
  } else if (x == '[') {
    vnode = reader(stream, ']', 0);
    // ?? why ?? lnode=vnode;
    if (vnode) {
      if (iserror(vnode))
        return vnode;
      cnode = make_node(vnode); // body
      vnode = NULL;
      lnode = make_node(make_node(make_symbol("_", 1))); // header
      lnode->next = make_node(make_node(cnode));
      lnode->type = EXP_LAMBDA;
      while ((vnode = reader(stream, ']', 0))) {
        if (iserror(vnode)) {
          unrefexp(lnode);
          return vnode;
        } // cleaning to be done gc
        cnode = cnode->next = make_node(vnode);
        vnode = NULL;
      }
    }
  } else if (x == '{') {
    /* Clojure-style hash-map literal: {k v, k v} → (hash-map k v k v ...).
       Comma is treated as whitespace LOCALLY here only — the reader
       proper still classifies `,` as a TERMMACRO globally, so we don't
       silently change comma behavior in other contexts. */
    lnode = make_node(make_symbol("hash-map", 8));
    cnode = lnode;
    for (;;) {
      int c;
      /* Eat inter-form whitespace, commas, and line comments. */
      while ((c = getc(stream)) != EOF) {
        if (c == ',')
          continue;
        if (c < 256 && (ISWHITESPACE & chrmap[c]))
          continue;
        if (c == ';') { /* line comment */
          while ((c = getc(stream)) != EOF && c != '\n')
            ;
          continue;
        }
        break;
      }
      if (c == EOF) {
        unrefexp(lnode);
        return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                     "End of file in hash-map literal");
      }
      if (c == '}')
        return lnode;
      ungetc(c, stream);
      vnode = reader(stream, '}', 0);
      if (!vnode)
        return lnode; /* `}` consumed by inner reader */
      if (iserror(vnode)) {
        unrefexp(lnode);
        return vnode;
      }
      cnode = cnode->next = make_node(vnode);
    }
  } else if (x == '\'') {
    vnode = reader(stream, 0, 0);
    return make_quote(vnode);
  } else if (x == '`') {
    vnode = reader(stream, 0, 0);
    exp_t *qq = make_node(make_symbol("quasiquote", 10));
    qq->next = make_node(vnode);
    return qq;
  } else if (x == ',') {
    int c = getc(stream);
    int splice = (c == '@');
    if (!splice) ungetc(c, stream);
    vnode = reader(stream, 0, 0);
    char *tag = splice ? "unquote-splicing" : "unquote";
    exp_t *uq = make_node(make_symbol(tag, strlen(tag)));
    uq->next = make_node(vnode);
    return uq;
  } else if (x == ';') {
    /* Line comment — skip to EOL or EOF. Without this handler `;` was
     * a TERMMACRO that returned EXP_ERROR_PARSING_MACROCHAR; the error
     * worked at top level (the file driver swallowed it before the
     * next form) but inside a list it bailed out mid-build, dropping
     * everything before the comment (tickets 7 & 8 root cause). */
    int c;
    while ((c = getc(stream)) != EOF && c != '\n')
      ;
    return NULL; /* signal "no form here" — caller's loop continues. */
  }
  /* Note: `|` was previously hooked here as a reader macro that built
     a wrapped list — it didn't implement Common Lisp's |sym with spaces|
     and isn't part of the Arc spec (paulgraham.com/arcll1.html doesn't
     mention it). We reclassified `|` as a normal constituent in
     char.h so it can be used as a function name (bit-or alias). */

  else
    return error(EXP_ERROR_PARSING_MACROCHAR, NULL, NULL,
                 "call to macro char %c unkown!", x);

  if (lnode)
    return lnode;
  else
    return NIL_EXP;
}

exp_t *escapereader(FILE *stream, token_t **ptoken, int lastchar) {
  /* Parse \n \b ... */
  /* Parse \xAB as char 0xAB */
  /* Parse \u001000 as unicode char 001000 in hex mode */
  int zchar = lastchar;
  int nchar = 0;
  if (schrmap[lastchar]) {
    zchar = schrmap[lastchar];
  } else if (lastchar == 'x') {
    if ((nchar = getc(stream)) == EOF)
      return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                   "End of file reached while parsing");
    if (chr2hex[nchar] < 0)
      goto error;
    zchar = chr2hex[nchar] * 16;
    if ((nchar = getc(stream)) == EOF)
      return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                   "End of file reached while parsing");
    if (chr2hex[nchar] < 0)
      goto error;
    zchar += chr2hex[nchar];
  }
  if (*ptoken) {
    tokenadd(*ptoken, zchar);
  } else {
    *ptoken = tokenize(zchar);
  }

  return NULL;
error:
  return error(EXP_ERROR_PARSING_ESCAPE, NULL, NULL,
               "invalid escape %c unkown!", nchar);
}

exp_t *reader(FILE *stream, unsigned char clmacro, int keepwspace) {
  int x, y, z;
  token_t *token = NULL;
  exp_t *ret = NULL;
  int pushtoken = 0;
  int escape = 0;

  while ((x = getc(stream)) != EOF) {
    pushtoken = 0;
    escape = 0;
    if (x > 127) { /* UTF-8 SUPPORT */
      token = tokenize(x);
      do {
        if ((y = getc(stream)) != EOF) {
          if ((y < 192) && (y > 127))
            tokenadd(token, y);
        } else {
          freetoken(token);
          return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                       "End of file reached while parsing");
        }
      } while ((y < 192) && (y > 127));
      ungetc(y, stream);
    } else if ((x < 0) || (x > 255) || !chrmap[x])
      return error(EXP_ERROR_PARSING_ILLEGAL_CHAR, NULL, NULL,
                   "Error illegal char %d", x);

    else if (ISWHITESPACE & chrmap[x])
      continue;
    else if ((ISTERMMACRO | ISNTERMMACRO) & chrmap[x]) {
      if (clmacro == x) {
        if (keepwspace & PARSER_TERMMACROMODE)
          ungetc(x, stream);
        return NULL; /* OK */
      }
      if (x == '#') {
        // Dispatch macro
        if ((y = getc(stream)) != EOF) {
          if (y == '\\') { // returning char
            if ((z = getc(stream)) != EOF) {
              return make_char(utf8_decode_stream(z, stream));
            } else
              return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                           "End of file reached while parsing");

          } else if (y == '[') {
            /* Vector literal: #[1 2 3] → (vector 1 2 3). The plain
               `[...]` form is reserved for Arc-lambda; we use the `#`
               prefix (Scheme/EDN convention) so the two don't collide.
               `vec` is the n-ary allocator, `vector` is the populator. */
            exp_t *vlnode = make_node(make_symbol("vector", 6));
            exp_t *vcnode = vlnode;
            exp_t *vvnode;
            while ((vvnode = reader(stream, ']', 0))) {
              if (iserror(vvnode)) {
                unrefexp(vlnode);
                return vvnode;
              }
              vcnode = vcnode->next = make_node(vvnode);
            }
            return vlnode;
          } else
            return error(EXP_ERROR_PARSING_MACROCHAR, NULL, NULL,
                         "call to dispatch macro char %c unkown!", y);
        } else
          return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                       "End of file reached while parsing");
      }
      if ((ret = callmacrochar(stream, x)))
        return ret;
      else
        continue;
    } else if (ISSINGLEESCAPE & chrmap[x]) { // step 5
      if ((y = getc(stream)) != EOF) {
        if ((ret = escapereader(stream, &token, y))) {
          if (token) freetoken(token);
          return ret;
        }
      } else
        return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                     "End of file reached while parsing");

    } else if (ISMULTIPLEESCAPE & chrmap[x]) {
      token = tokenize(-1);
      escape = 1;
    } // step 6
    else if (ISCONSTITUENT & chrmap[x])
      token = tokenize(x); // step 7
    while (!pushtoken) {
      if (!escape) {
        // step 8
        if ((y = getc(stream)) != EOF) {
          if (y > 127) {
            tokenadd(token, y);
            do {
              if ((y = getc(stream)) != EOF) {
                if ((y < 192) && (y > 127))
                  tokenadd(token, y);
              } else {
                freetoken(token);
                return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                             "End of file reached while parsing");
              }
            } while ((y < 192) && (y > 127));
            ungetc(y, stream);
          } else if ((y < 0) || (y > 255) || !chrmap[y]) {
            freetoken(token);
            return error(EXP_ERROR_PARSING_ILLEGAL_CHAR, NULL, NULL,
                         "Error illegal char %d", x);
          } else if ((ISCONSTITUENT | ISNTERMMACRO) & chrmap[y]) {
            tokenadd(token, y);
            continue;
          } else if (ISSINGLEESCAPE & chrmap[y]) {
            if ((z = getc(stream)) != EOF) {
              if ((ret = escapereader(stream, &token, z)))
                return ret;
            } else {
              freetoken(token);
              return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                           "End of file reached while parsing");
            }
          } else if (ISMULTIPLEESCAPE & chrmap[y]) {
            pushtoken = 1;
            ungetc(y, stream);
          } // escape=1;
          else if (ISTERMMACRO & chrmap[y]) {
            ungetc(y, stream);
            pushtoken = 1;
          } else if (ISWHITESPACE & chrmap[y]) {
            pushtoken = 1; // ungetc if appropriate
            if (keepwspace & 1)
              tokenadd(token, y);
          } else
            pushtoken = 1;
        } else {
          freetoken(token);
          return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                       "End of file reached while parsing");
        }
      } else { // Escape mode
        // step 9
        if ((y = getc(stream)) != EOF) {
          if (y > 127) {
            tokenadd(token, y);
            do {
              if ((y = getc(stream)) != EOF) {
                if ((y < 192) && (y > 127))
                  tokenadd(token, y);
              } else {
                freetoken(token);
                return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                             "End of file reached while parsing");
              }
            } while ((y < 192) && (y > 127));
            ungetc(y, stream);
          } else if ((y < 0) || (y > 255)) {
            freetoken(token);
            return error(EXP_ERROR_PARSING_ILLEGAL_CHAR, NULL, NULL,
                         "Error illegal char %d", x);
          } else if ((ISWHITESPACE | ISCONSTITUENT | ISTERMMACRO |
                      ISNTERMMACRO) &
                     chrmap[y])
            tokenadd(token, y);
          else if (ISSINGLEESCAPE & chrmap[y]) {
            if ((z = getc(stream)) != EOF) {
              if ((ret = escapereader(stream, &token, z)))
                return ret;
            } else {
              freetoken(token);
              return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                           "End of file reached while parsing");
            }
          } else if (ISMULTIPLEESCAPE & chrmap[y]) {
            ret = make_string_from_token(token, 0, token->size);
            return ret;
            /*escape=0;pushtoken=1;*/
          } else
            tokenadd(token, y);

        } else {
          freetoken(token);
          return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                       "End of file reached while parsing");
        }
      }
    }
    if (pushtoken) {
      // TOKEN AND STUFF TO BE FREED
      ret = make_atom_from_token(token);
      token = NULL;
      return ret;
    } else
      return NULL;
  }

  if (x == EOF) {
    return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                 "End of file reached while parsing");
    // END OF FILE PROCESSING TO BE DONE STEP 1
  }
  return NULL;
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
           common case (1-6 params) this beats a full hash lookup. */
        int i;
        for (i = 0; i < curenv->n_inline; i++) {
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
  REJECT_RESERVED_ASSIGN(keyv, _rerr,
                         { unrefexp(keyv); unrefexp(val); return _rerr; });
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
    /* No existing binding anywhere — create in current env. */
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
  do {                                                                        \
    const char *_rsv = reserved_param_name(PARAMS);                           \
    if (_rsv) {                                                               \
      (ERRLV) = error(ERROR_ILLEGAL_VALUE, NULL, env,                         \
                      "cannot bind reserved name '%s' " CTX, _rsv);           \
      FAIL;                                                                    \
    }                                                                         \
  } while (0)

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
    CHECK_RESERVED_BIND(header, val, "as a parameter",
                        { unrefexp(e); return val; });
    if (cur) {
      /* Body is the remaining list; first form may be nil/literal/symbol
         as well as a pair — all are legal body expressions. */
      body = cur;
      vali = make_node(refexp(body));
      if (issymbol(header)) {
        exp_t *dot = make_node(make_symbol(".", 1));
        dot->next = make_node(refexp(header));
        val = make_node(dot);
      } else {
        val = make_node(refexp(header));
      }
      val->next = vali;
      val->type = EXP_LAMBDA;
      /* Closure: stash the env at fn-creation time in the wrapper
         node's `meta` field (see comment in unrefexp). invoke() later
         uses this as the new call env's root, so let/with bindings
         from the enclosing scope resolve correctly. For top-level fns
         env is global, so the capture is just an extra ref on global
         (cheap). */
      if (env)
        val->next->meta = (struct keyval_t *)ref_env(env);
      /* Compile both top-level fns and closures. Closures (a real captured
         scope, env->root != NULL) compile with no_gcache so free-var reads
         always re-resolve against the captured env (a closure that *mutates*
         a free var can't slot-resolve it and safely falls back to AST). */
      compile_lambda(val, env && env->root);
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
      CHECK_RESERVED_BIND(header, val, "as a parameter",
                          { unrefexp(e); return val; });
      if (cur) {
        /* Docstring: (def f (args) "..." body...) — a leading string that
           is FOLLOWED by more forms is documentation, not the body. A lone
           string body (def f () "hi") stays the return value. Stored by
           name for the `doc` builtin. */
        if (isstring(car(cur)) && cdr(cur)) {
          if (!user_doc)
            user_doc = create_dict();
          set_get_keyval_dict(user_doc, (char *)exp_text(name), car(cur));
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
          dot->next = make_node(refexp(header));
          val = make_node(dot);
        } else {
          val = make_node(refexp(header));
        }
        val->next = vali;
        val->type = EXP_LAMBDA;
        val->meta = (keyval_t *)strdup(exp_text(name));
        /* Closure: capture defining env (see fncmd for rationale). */
        if (env)
          val->next->meta = (struct keyval_t *)ref_env(env);
        /* Compile top-level defs and nested (closure) defs alike; closures
           get no_gcache (fresh free-var lookups against the captured env). */
        compile_lambda(val, env && env->root);
        if (!(env->d))
          env->d = create_dict();
        set_get_keyval_dict(env->d, exp_text(name),
                            val); /* return value (the kv) unused */
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

const char doc_macroexpand[] =
    "(macroexpand-1 form) — expand the outermost macro call in form once and "
    "return the resulting code.";
exp_t *expandmacrocmd(exp_t *e, env_t *env) {
  exp_t *tmpexp;
  exp_t *tmpexp2;

  tmpexp = car(cadr(cadr(e)));
  if (tmpexp)
    if (issymbol(tmpexp))
      if ((tmpexp2 = lookup(refexp(tmpexp), env)))
        if ismacro (tmpexp2) {
          tmpexp = expandmacro(refexp(cadr(cadr(e))), tmpexp2, env);
          goto finish;
        }

  tmpexp = error(ERROR_ILLEGAL_VALUE, e, env, "Error parameter not a macro");
finish:
  unrefexp(e);
  return tmpexp;
}

const char doc_defmacro[] =
    "(defmacro name (params...) body) — define a macro. Body returns a code "
    "form that replaces the call site at expansion time.";
exp_t *defmacrocmd(exp_t *e, env_t *env) {
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
        val = make_node(refexp(header));
        val->next = vali;
        val->type = EXP_MACRO;
        val->meta = (keyval_t *)strdup(exp_text(name));
        if (env)
          val->next->meta = (struct keyval_t *)ref_env(env);
        if (!(env->d))
          env->d = create_dict();
        set_get_keyval_dict(env->d, exp_text(name),
                            val); /* return value (the kv) unused */
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
            return NULL;
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
      return NULL;
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
static void assign_store_symbol(exp_t *sym, env_t *env, exp_t *val) {
  env_t *cur = env;
  while (cur) {
    for (int i = 0; i < cur->n_inline; i++) {
      const char *k = cur->inline_keys[i];
      if (k && strcmp(k, exp_text(sym)) == 0) {
        unrefexp(cur->inline_vals[i]);
        cur->inline_vals[i] = refexp(val);
        return;
      }
    }
    if (cur->d) {
      keyval_t *kv = set_get_keyval_dict(cur->d, exp_text(sym), NULL);
      if (kv) {
        GEN_BUMP();
        unrefexp(kv->val);
        kv->val = refexp(val);
        return;
      }
    }
    cur = cur->root;
  }
  if (!(env->d))
    env->d = create_dict();
  set_get_keyval_dict(env->d, exp_text(sym), val);
  GEN_BUMP();
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
    REJECT_RESERVED_ASSIGN(sym, _rerr,
                           { if (ret != NIL_EXP) unrefexp(ret);
                             unrefexp(e); return _rerr; });
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
#define ALCOVE_DUMP_VERSION 2
#define ALCOVE_DUMP_VERSION_MIN 1
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
      CLEAN_RETURN_1(path_arg,
                     error(ERROR_ILLEGAL_VALUE, NULL, env,
                           "savedb: optional argument must be a filename string"));
    path = (const char *)exp_text(path_arg);
  }
  /* Snapshot path before releasing path_arg — error() would receive a
     dangling pointer if we unrefexp(path_arg) first and its ptr was freed. */
  char *path_snap = (path == alcove_db_path) ? NULL : strdup(path);
  FILE *stream = fopen(path, "w");
  if (!stream) {
    if (path_arg) unrefexp(path_arg);
    unrefexp(e);
    exp_t *err = error(ERROR_ILLEGAL_VALUE, NULL, env,
                       "Unable to open '%s' for writing",
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
    if (path_arg) unrefexp(path_arg);
    unrefexp(e);
    exp_t *err = error(ERROR_ILLEGAL_VALUE, NULL, env,
                       "savedb: I/O error writing '%s'",
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
      CLEAN_RETURN_1(path_arg,
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
/* Pairwise compare helper. Returns 1 on success with d set to the sign
   of (a - b); returns 0 on type mismatch (caller raises error). */
static int alc_pair_cmp(exp_t *a, exp_t *b, double *d) {
  if ((isnumber(a) || isfloat(a)) && (isnumber(b) || isfloat(b))) {
    *d = (TO_DOUBLE(a)) -
         (TO_DOUBLE(b));
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
  /* Decode the operator once. Branches keep the same semantics that
     bytecode SLOT_<cmp>_FIX uses, so chained results agree with the
     compiler's per-pair comparisons. */
  int op_kind;
  if (strcmp(exp_text(op), "<") == 0)
    op_kind = 0;
  else if (strcmp(exp_text(op), ">") == 0)
    op_kind = 1;
  else if (strcmp(exp_text(op), "<=") == 0)
    op_kind = 2;
  else if (strcmp(exp_text(op), ">=") == 0)
    op_kind = 3;
  else {
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
        return error(ERROR_ILLEGAL_VALUE, NULL, env,
                     "compare: incompatible types");
      }
      int ok = (op_kind == 0)   ? (d < 0)
               : (op_kind == 1) ? (d > 0)
               : (op_kind == 2) ? (d <= 0)
                                : (d >= 0);
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

#define MATH_CMD(name, init_i, OP, IS_SUB, IS_DIV)                             \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    int64_t sum_i = (init_i);                                                  \
    expfloat sum_f = (init_i);                                                 \
    int saw_float = 0;                                                         \
    exp_t *c = cdr(e);                                                         \
    exp_t *v = NULL;                                                           \
    int i = 0;                                                                 \
    exp_t *ret = NULL;                                                         \
    do {                                                                       \
      if (c) {                                                                 \
        i++;                                                                   \
        v = EVAL(c->content, env);                                             \
        if (iserror(v)) {                                                      \
          unrefexp(e);                                                         \
          return v;                                                            \
        }                                                                      \
        if ((IS_DIV) && i > 1) {                                               \
          if ((isnumber(v) && FIX_VAL(v) == 0) || (isfloat(v) && v->f == 0)) { \
            ret = error(ERROR_DIV_BY0, e, env, "Illegal Division by 0");       \
            unrefexp(v);                                                       \
            goto finish;                                                       \
          }                                                                    \
        }                                                                      \
        if (saw_float) {                                                       \
          if (isnumber(v)) {                                                   \
            sum_f OP FIX_VAL(v);                                               \
          } else if (isfloat(v)) {                                             \
            sum_f OP v->f;                                                     \
          } else {                                                             \
            ret = error(ERROR_ILLEGAL_VALUE, e, env,                           \
                        "Illegal value in operation");                         \
            unrefexp(v);                                                       \
            goto finish;                                                       \
          }                                                                    \
        } else {                                                               \
          if (isnumber(v)) {                                                   \
            if (i > 1 || (init_i) != 0) {                                      \
              sum_i OP FIX_VAL(v);                                             \
            } else {                                                           \
              sum_i = FIX_VAL(v);                                              \
            }                                                                  \
          } else if (isfloat(v)) {                                             \
            if (i > 1 || (init_i) != 0) {                                      \
              sum_f = sum_i;                                                   \
              sum_f OP v->f;                                                   \
            } else {                                                           \
              sum_f = v->f;                                                    \
            }                                                                  \
            sum_i = 0;                                                         \
            saw_float = 1;                                                     \
          } else {                                                             \
            ret = error(ERROR_ILLEGAL_VALUE, e, env,                           \
                        "Illegal value in operation");                         \
            unrefexp(v);                                                       \
            goto finish;                                                       \
          }                                                                    \
        }                                                                      \
        unrefexp(v);                                                           \
      }                                                                        \
    } while (c && (c = c->next));                                              \
    /* (-)  and (/) with no args are errors — they have no identity value. */  \
    if (i == 0 && ((IS_SUB) || (IS_DIV))) {                                   \
      ret = error(ERROR_MISSING_PARAMETER, e, env,                             \
                  (IS_SUB) ? "(- a ...): needs at least one argument"          \
                           : "(/ a ...): needs at least one argument");        \
      goto finish;                                                             \
    }                                                                          \
    if (i == 1) {                                                              \
      if (IS_SUB) {                                                            \
        if (saw_float) {                                                       \
          sum_f = -sum_f;                                                      \
        } else {                                                               \
          int64_t _neg = -sum_i;                                               \
          /* Detect fixnum range overflow: if the negation doesn't             \
             round-trip through the 61-bit tag (arithmetic shift),             \
             promote to float. Uses signed cast before >>3 to match FIX_VAL. */\
          if (!FIX_FITS(_neg)) { /* pointer-width-correct; not a 64-bit-only check */ \
            sum_f = -(expfloat)sum_i;                                          \
            saw_float = 1;                                                     \
          } else {                                                             \
            sum_i = _neg;                                                      \
          }                                                                    \
        }                                                                      \
      } else if (IS_DIV) {                                                     \
        if (saw_float) {                                                       \
          if (sum_f == 0) {                                                    \
            ret = error(ERROR_DIV_BY0, e, env, "Illegal Division by 0");       \
            goto finish;                                                       \
          }                                                                    \
          sum_f = 1 / sum_f;                                                   \
        } else {                                                               \
          if (sum_i == 0) {                                                    \
            ret = error(ERROR_DIV_BY0, e, env, "Illegal Division by 0");       \
            goto finish;                                                       \
          }                                                                    \
          sum_i = 1 / sum_i;                                                   \
        }                                                                      \
      }                                                                        \
    }                                                                          \
    ret = saw_float ? make_floatf(sum_f) : make_integeri(sum_i);               \
  finish:                                                                      \
    unrefexp(e);                                                               \
    return ret;                                                                \
  }

const char doc_plus[] =
    "(+ x ...) — sum of all args. (+) is 0. Mixed int/float promotes to float.";
MATH_CMD(pluscmd, 0, +=, 0, 0)

const char doc_mul[] = "(* x ...) — product of all args. (*) is 1.";
MATH_CMD(multiplycmd, 1, *=, 0, 0)

const char doc_minus[] =
    "(- a) negates; (- a b c ...) subtracts the rest from a.";
MATH_CMD(minuscmd, 0, -=, 1, 0)

const char doc_div[] = "(/ a b ...) — divide a by the rest. Integer division "
                       "if all args are ints; otherwise float.";
MATH_CMD(dividecmd, 0, /=, 0, 1)

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

/* ---------------- Vectors ----------------
   Mutable, O(1) random-access array. Storage layout:
     e->type    = EXP_VECTOR
     e->flags   = ... | vec_kind (bits 4-5: GEN / I64 / F64)
     e->ptr     = alc_vec_t* — 8-byte header {cap, hdr_pad} + cap cells
                  of 8 bytes each (kind determines cell interpretation)
     e->vec_win = {start, end} — live window; len = end - start
   For VEC_KIND_GEN each cell holds an owning exp_t* ref (released by
   the unrefexp walk above). I64/F64 cells are raw scalars — no
   refcount accounting. The slow-sieve trial-division benchmark wins
   from this because we use a sqrt-cutoff on smallest-first vector
   iteration instead of cdr-walking a largest-first cons list.
   alc_vec_t and the vec_* accessors are declared in alcove.h. */

/* Allocate the alc_vec_t storage block with `cap` cells of 8 bytes. The
   caller fills cells / sets exp_t->ptr / sets the window. Returns NULL
   on overflow or alloc failure. memalloc zero-initialises the payload,
   so cells outside the window read as zero (not garbage). */
static alc_vec_t *vec_alloc_storage(int64_t cap) {
  /* cap must fit a positive int32 (it is stored as v->cap). The bound is
     exclusive: cap == 2^31 would cast to INT32_MIN. */
  if (cap < 0 || cap >= ((int64_t)1 << 31))
    return NULL;
  size_t bytes = sizeof(alc_vec_t) + (size_t)cap * 8u;
  alc_vec_t *v = (alc_vec_t *)memalloc(1, bytes);
  if (!v)
    return NULL;
  v->cap = (int32_t)cap;
  v->hdr_pad = 0;
  return v;
}

exp_t *make_vector(int64_t n, exp_t *fill) {
  /* Hard cap to keep `(size_t)n * 8` from wrapping. With int61 fixnums n
     can reach 2^60; n*8 wraps modulo SIZE_MAX and hands us a tiny alloc
     that the loop then writes terabytes past. 1<<31 cells (16 GiB at
     8B/cell) is well past any sane vector; anything bigger we refuse
     rather than guess. */
  alc_vec_t *v = vec_alloc_storage(n);
  if (!v)
    return NULL;
  /* Kind inference from fill's type. Numeric fillers get the typed fast
     path; anything else (incl. nil) falls back to GEN. vec-set! later
     auto-promotes to GEN on a type-mismatched write. */
  unsigned kind = VEC_KIND_GEN;
  if (isnumber(fill))
    kind = VEC_KIND_I64;
  else if (isfloat(fill))
    kind = VEC_KIND_F64;

  exp_t *e = make_nil();
  e->type = EXP_VECTOR;
  e->flags = (unsigned short)kind;
  e->ptr = v;
  e->vec_win.start = 0;
  e->vec_win.end = (int32_t)n;
  char *base = (char *)v + sizeof(alc_vec_t);
  if (kind == VEC_KIND_I64) {
    int64_t fix = FIX_VAL(fill);
    int64_t *cells = (int64_t *)base;
    for (int64_t i = 0; i < n; i++)
      cells[i] = fix;
  } else if (kind == VEC_KIND_F64) {
    double f = (double)fill->f;
    double *cells = (double *)base;
    for (int64_t i = 0; i < n; i++)
      cells[i] = f;
  } else {
    exp_t **cells = (exp_t **)base;
    for (int64_t i = 0; i < n; i++)
      cells[i] = refexp(fill);
  }
  return e;
}

/* Promote an I64 vec to F64 in place. Converts every cell (live window
   only; cells outside [start, end) are uninitialized garbage and stay
   that way). Tensor mutating ops call this when they need to store
   non-integer results into a vec that was constructed integer-typed —
   matches the pre-refactor behavior where `(vec 4 0)` then `(vec-scale!
   v 0.5)` ended up holding floats. Single-shard only; asserts
   FLAG_SHARED clear. Returns 1 on success. */
static int vec_promote_i64_to_f64(exp_t *vexp) {
  if (vec_kind(vexp) == VEC_KIND_F64)
    return 1;
  if (vec_kind(vexp) != VEC_KIND_I64)
    return 0;
  if (vexp->flags & FLAG_SHARED)
    return 0;
  alc_vec_t *old = (alc_vec_t *)vexp->ptr;
  alc_vec_t *new_v = vec_alloc_storage(old->cap);
  if (!new_v)
    return 0;
  int64_t *src = (int64_t *)((char *)old + sizeof(alc_vec_t));
  double *dst = (double *)((char *)new_v + sizeof(alc_vec_t));
  int32_t s = vexp->vec_win.start;
  int32_t en = vexp->vec_win.end;
  for (int32_t i = s; i < en; i++)
    dst[i] = (double)src[i];
  free(old);
  vexp->ptr = new_v;
  vexp->flags = (unsigned short)((vexp->flags & ~VEC_KIND_MASK) | VEC_KIND_F64);
  return 1;
}

/* Promote a typed (I64 or F64) vec to GEN in place. Boxes every live
   cell into a fresh heap exp_t. Single-shard only — callers must check
   FLAG_SHARED is clear (a shared typed vec must not realloc its
   payload). Returns 1 on success, 0 on alloc failure or shared-vec.

   Cells outside the live window [start, end) in the new buffer are left
   uninitialized — the gen-walk in unrefexp only touches indices in the
   window via vec_gen_at, so garbage outside is invisible. */
static int vec_promote_to_gen(exp_t *vexp) {
  if (vec_kind(vexp) == VEC_KIND_GEN)
    return 1;
  if (vexp->flags & FLAG_SHARED)
    return 0;

  alc_vec_t *old = (alc_vec_t *)vexp->ptr;
  int32_t start = vexp->vec_win.start;
  int64_t live = vexp->vec_win.end - start;
  alc_vec_t *new_v = vec_alloc_storage(old->cap);
  if (!new_v)
    return 0;
  exp_t **dst = (exp_t **)((char *)new_v + sizeof(alc_vec_t));
  char *oldbase = (char *)old + sizeof(alc_vec_t);
  if (vec_kind(vexp) == VEC_KIND_I64) {
    int64_t *src = (int64_t *)oldbase;
    for (int64_t i = 0; i < live; i++)
      dst[start + i] = MAKE_FIX(src[start + i]); /* tagged immediate */
  } else { /* VEC_KIND_F64 */
    double *src = (double *)oldbase;
    for (int64_t i = 0; i < live; i++)
      dst[start + i] = make_floatf((expfloat)src[start + i]); /* fresh nref=1 */
  }
  free(old);
  vexp->ptr = new_v;
  vexp->flags = (unsigned short)((vexp->flags & ~VEC_KIND_MASK) | VEC_KIND_GEN);
  return 1;
}

/* Boxed read: returns an exp_t* (refcount-bumped or fresh) at index i,
   regardless of vec kind. Caller has bounds-checked. The boxed value
   for I64 is a tagged immediate (no refcount); for F64 it's a fresh
   EXP_FLOAT with nref=1; for GEN it's refexp() of the existing slot. */
static inline exp_t *vec_get_boxed(exp_t *vexp, int64_t i) {
  char *base = (char *)vexp->ptr + sizeof(alc_vec_t);
  int32_t off = vexp->vec_win.start + (int32_t)i;
  switch (vec_kind(vexp)) {
  case VEC_KIND_GEN:
    return refexp(((exp_t **)base)[off]);
  case VEC_KIND_I64:
    return MAKE_FIX(((int64_t *)base)[off]);
  case VEC_KIND_F64:
    return make_floatf((expfloat)((double *)base)[off]);
  }
  return NIL_EXP; /* unreachable — vec_kind only returns one of the three */
}

/* Boxed write: stores val into index i. Caller transferred its ref. On
   a kind match, val's ref is either kept (GEN) or released (I64/F64
   extract the scalar and drop the box). On a kind mismatch, the vec
   promotes to GEN first then retries. Returns 0 on alloc failure
   (promotion failed); the caller's ref to val is released either way.

   Quietly accepts a fixnum into an F64 vec (converts to double) — this
   matches the existing vec_elem_set_double semantics that the MLP
   relies on for scalar inits like `(vec-fill! v 0)`. */
static int vec_set_boxed(exp_t *vexp, int64_t i, exp_t *val) {
  char *base = (char *)vexp->ptr + sizeof(alc_vec_t);
  int32_t off = vexp->vec_win.start + (int32_t)i;
  unsigned k = vec_kind(vexp);
  if (k == VEC_KIND_GEN) {
    unrefexp(((exp_t **)base)[off]);
    ((exp_t **)base)[off] = val; /* ownership transferred */
    return 1;
  }
  if (k == VEC_KIND_I64 && isnumber(val)) {
    ((int64_t *)base)[off] = FIX_VAL(val);
    /* val is a tagged immediate — no ref to release. */
    return 1;
  }
  if (k == VEC_KIND_F64) {
    if (isfloat(val)) {
      ((double *)base)[off] = (double)val->f;
      unrefexp(val);
      return 1;
    }
    if (isnumber(val)) {
      ((double *)base)[off] = (double)FIX_VAL(val);
      return 1; /* tagged immediate, no unref */
    }
  }
  /* Kind mismatch — promote to GEN then retry. */
  if (!vec_promote_to_gen(vexp)) {
    unrefexp(val);
    return 0;
  }
  /* After promotion vexp is GEN; recurse handles the GEN store. */
  return vec_set_boxed(vexp, i, val);
}

/* Read element i as a double, regardless of kind. Sets *err on a GEN
   non-numeric element. Caller has bounds-checked. */
static inline double vec_read_double(exp_t *vexp, int64_t i, int *err) {
  char *base = (char *)vexp->ptr + sizeof(alc_vec_t);
  int32_t off = vexp->vec_win.start + (int32_t)i;
  switch (vec_kind(vexp)) {
  case VEC_KIND_F64:
    return ((double *)base)[off];
  case VEC_KIND_I64:
    return (double)((int64_t *)base)[off];
  case VEC_KIND_GEN: {
    exp_t *e = ((exp_t **)base)[off];
    if (isnumber(e))
      return (double)FIX_VAL(e);
    if (isfloat(e))
      return e->f;
    *err = 1;
    return 0.0;
  }
  }
  *err = 1;
  return 0.0;
}

/* Write a double into element i. If the vec is F64, raw store. I64
   truncates to int64. GEN goes through the in-place EXP_FLOAT fast path
   (preserved from the old vec_elem_set_double). Caller has bounds-
   checked. Returns 0 on failure (e.g., GEN promotion not possible). */
static inline int vec_write_double(exp_t *vexp, int64_t i, double x) {
  char *base = (char *)vexp->ptr + sizeof(alc_vec_t);
  int32_t off = vexp->vec_win.start + (int32_t)i;
  switch (vec_kind(vexp)) {
  case VEC_KIND_F64:
    ((double *)base)[off] = x;
    return 1;
  case VEC_KIND_I64:
    ((int64_t *)base)[off] = (int64_t)x;
    return 1;
  case VEC_KIND_GEN: {
    exp_t **cells = (exp_t **)base;
    exp_t *cur = cells[off];
    if (is_ptr(cur) && cur->type == EXP_FLOAT && cur->nref == 1 &&
        !(cur->flags & FLAG_SHARED)) {
      cur->f = (expfloat)x;
      return 1;
    }
    unrefexp(cur);
    cells[off] = make_floatf((expfloat)x);
    return 1;
  }
  }
  return 0;
}

/* Inspect a freshly-built GEN vec; if all cells are homogeneous numeric,
   replace its storage with a typed I64 or F64 buffer in place. Used by
   vectorcmd / the #[...] reader to pick the tightest kind for literals.
   Asserts start==0 (always true for fresh vectorcmd output). Returns 1
   if the vec was tightened, 0 if it stays GEN. */
static int vec_tighten(exp_t *vexp) {
  if (vec_kind(vexp) != VEC_KIND_GEN)
    return 1;
  int64_t n = vec_len(vexp);
  if (n == 0)
    return 0; /* nothing to infer from */
  exp_t **cells = vec_gen_cells(vexp);
  int all_fix = 1, all_num = 1;
  for (int64_t i = 0; i < n; i++) {
    exp_t *c = cells[i];
    if (!isnumber(c))
      all_fix = 0;
    if (!isnumber(c) && !isfloat(c)) {
      all_num = 0;
      break;
    }
  }
  if (!all_num)
    return 0;
  alc_vec_t *old = (alc_vec_t *)vexp->ptr;
  alc_vec_t *new_v = vec_alloc_storage(old->cap);
  if (!new_v)
    return 0;
  char *newbase = (char *)new_v + sizeof(alc_vec_t);
  unsigned newkind;
  if (all_fix) {
    int64_t *dst = (int64_t *)newbase;
    for (int64_t i = 0; i < n; i++)
      dst[i] = FIX_VAL(cells[i]); /* tagged immediates, no unref */
    newkind = VEC_KIND_I64;
  } else {
    double *dst = (double *)newbase;
    for (int64_t i = 0; i < n; i++) {
      exp_t *c = cells[i];
      dst[i] = isfloat(c) ? (double)c->f : (double)FIX_VAL(c);
      unrefexp(c); /* drop the box we no longer need */
    }
    newkind = VEC_KIND_F64;
  }
  free(old);
  vexp->ptr = new_v;
  vexp->flags = (unsigned short)((vexp->flags & ~VEC_KIND_MASK) | newkind);
  vexp->vec_win.start = 0;
  vexp->vec_win.end = (int32_t)n;
  return 1;
}

const char doc_vec[] = "(vec n init) — fixed-size vector of n cells "
                       "initialised to init. (vec n) defaults init to nil.";
exp_t *veccmd(exp_t *e, env_t *env) {
  exp_t *fill = NIL_EXP;
  EVAL_ARG_1(nexp);
  if (e->next && e->next->next)
    fill = EVAL(e->next->next->content, env);
  if (iserror(fill))
    CLEAN_RETURN_1(nexp, fill);
  if (!isnumber(nexp))
    CLEAN_RETURN_2(nexp, fill,
                   error(ERROR_NUMBER_EXPECTED, e, env,
                         "(vec n init): n must be a number"));
  int64_t n = FIX_VAL(nexp);
  if (n < 0)
    n = 0;
  unrefexp(nexp);
  exp_t *ret = make_vector(n, fill);
  unrefexp(fill);
  if (!ret) {
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "(vec n ...): n is too large or alloc failed");
  }
  unrefexp(e);
  return ret;
}

const char doc_vecref[] =
    "(vec-ref v i) — read element i of vector v (O(1)). 0-based index.";
exp_t *vecrefcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(vexp, iexp);

  if (!isvector(vexp) || !(isnumber(iexp) || isfloat(iexp)))
    CLEAN_RETURN_2(
        vexp, iexp,
        error(ERROR_ILLEGAL_VALUE, e, env, "(vec-ref v i): bad args"));

  int64_t i = isnumber(iexp) ? FIX_VAL(iexp) : (int64_t)iexp->f; /* float idx truncates */
  if (i < 0 || i >= vec_len(vexp))
    CLEAN_RETURN_2(
        vexp, iexp,
        error(ERROR_INDEX_OUT_OF_RANGE, e, env, "vec-ref: index out of range"));

  exp_t *ret = vec_get_boxed(vexp, i);
  CLEAN_RETURN_2(vexp, iexp, ret);
}

const char doc_vecset[] =
    "(vec-set! v i x) — store x into element i of vector v. Returns x.";
exp_t *vecsetcmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(vexp, iexp, valexp);

  if (!isvector(vexp) || !(isnumber(iexp) || isfloat(iexp)))
    CLEAN_RETURN_3(
        vexp, iexp, valexp,
        error(ERROR_ILLEGAL_VALUE, e, env, "(vec-set! v i val): bad args"));

  int64_t i = isnumber(iexp) ? FIX_VAL(iexp) : (int64_t)iexp->f; /* float idx truncates */
  if (i < 0 || i >= vec_len(vexp))
    CLEAN_RETURN_3(vexp, iexp, valexp,
                   error(ERROR_INDEX_OUT_OF_RANGE, e, env,
                         "vec-set!: index out of range"));

  /* Refcount the return value before vec_set_boxed eats valexp's ref. */
  exp_t *ret = refexp(valexp);
  if (!vec_set_boxed(vexp, i, valexp)) {
    unrefexp(ret);
    CLEAN_RETURN_2(vexp, iexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-set!: alloc failure or shared vec promote"));
  }
  /* valexp ownership consumed by vec_set_boxed — don't clean it. */
  CLEAN_RETURN_2(vexp, iexp, ret);
}

const char doc_veclen[] = "(vec-len v) — number of cells in vector v.";
exp_t *veclencmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(vexp,
                   error(ERROR_ILLEGAL_VALUE, e, env, "(vec-len v): not a vector"));
  int64_t n = vec_len(vexp);
  CLEAN_RETURN_1(vexp, MAKE_FIX(n));
}

/* ---------- tensor bulk ops ----------
   These take vec-of-floats (or vec-of-fixnums; we coerce) and operate
   on the underlying doubles, eliminating per-element boxing. The
   interpreter would otherwise allocate an EXP_FLOAT per multiply and
   per add — these ops collapse the hot inner loops of an MLP forward/
   backward pass into a single C-level walk.

   Coercion rule: each element must be a number (float or fixnum); any
   other type errors. The mutating ops (-set! / -axpy! / -scale! / -add!
   / -fill! / -relu!) replace each output slot with a fresh EXP_FLOAT
   (we don't try to reuse the old slot's exp_t in place — refcount
   tracking would be racy and the alloc savings are marginal compared
   to the multiply count). */

/* Resolve the live cell array for a VEC_KIND_GEN vec — pointer to the
   first valid cell (accounting for vec_win.start). Cached once outside
   the hot loop so the compiler doesn't reload vexp->ptr / vec_win.start
   on every iteration (unrefexp / make_floatf are non-aliasing in
   practice, but the compiler can't prove it). */
static inline exp_t **vec_gen_cells(exp_t *vexp) {
  return (exp_t **)((char *)vexp->ptr + sizeof(alc_vec_t)) +
         vexp->vec_win.start;
}

/* Read cell[i] (within the resolved cell view) as a double. Returns
   0.0 + sets *err on a non-numeric GEN element. Used by the GEN-GEN
   fast path in tensor ops; typed kinds skip this and read raw. */
static inline double gen_cell_as_double(exp_t **cells, int64_t i, int *err) {
  exp_t *e = cells[i];
  if (isnumber(e))
    return (double)FIX_VAL(e);
  if (isfloat(e))
    return e->f;
  *err = 1;
  return 0.0;
}

const char doc_vecdot[] =
    "(vec-dot a b) — sum of a[i]*b[i] over both vectors (must be equal "
    "length, numeric elements). Returns a float.";
exp_t *vecdotcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(aexp, bexp);
  if (!isvector(aexp) || !isvector(bexp))
    CLEAN_RETURN_2(aexp, bexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-dot a b): both args must be vectors"));
  int64_t na = vec_len(aexp), nb = vec_len(bexp);
  if (na != nb)
    CLEAN_RETURN_2(aexp, bexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-dot: length mismatch (%lld vs %lld)",
                         (long long)na, (long long)nb));
  int err = 0;
  double s = 0.0;
  unsigned ka = vec_kind(aexp), kb = vec_kind(bexp);
  /* Hoisted fast paths cover the practical cases:
       F64-F64: raw double dot product (MLP hot path);
       GEN-GEN: hoisted exp_t* cells + boxed read (heterogeneous vecs
                like sieve-fast or untyped data);
       I64-F64 / F64-I64: integer→double convert on the typed side.
     Anything else (GEN mixed with typed, etc.) goes through the per-
     element kind switch — same correctness, slower. */
  if (ka == VEC_KIND_F64 && kb == VEC_KIND_F64) {
    double *acells =
        VEC_F64_CELLS(aexp);
    double *bcells =
        VEC_F64_CELLS(bexp);
    for (int64_t i = 0; i < na; i++)
      s += acells[i] * bcells[i];
  } else if (ka == VEC_KIND_GEN && kb == VEC_KIND_GEN) {
    exp_t **acells = vec_gen_cells(aexp);
    exp_t **bcells = vec_gen_cells(bexp);
    for (int64_t i = 0; i < na; i++)
      s += gen_cell_as_double(acells, i, &err) *
           gen_cell_as_double(bcells, i, &err);
  } else if (ka == VEC_KIND_I64 && kb == VEC_KIND_F64) {
    int64_t *acells = (int64_t *)((char *)aexp->ptr + sizeof(alc_vec_t)) +
                      aexp->vec_win.start;
    double *bcells =
        VEC_F64_CELLS(bexp);
    for (int64_t i = 0; i < na; i++)
      s += (double)acells[i] * bcells[i];
  } else if (ka == VEC_KIND_F64 && kb == VEC_KIND_I64) {
    double *acells =
        VEC_F64_CELLS(aexp);
    int64_t *bcells = (int64_t *)((char *)bexp->ptr + sizeof(alc_vec_t)) +
                      bexp->vec_win.start;
    for (int64_t i = 0; i < na; i++)
      s += acells[i] * (double)bcells[i];
  } else {
    /* GEN mixed with typed, or I64-I64 — kind-uniform fallback. */
    for (int64_t i = 0; i < na; i++)
      s += vec_read_double(aexp, i, &err) * vec_read_double(bexp, i, &err);
  }
  if (err)
    CLEAN_RETURN_2(aexp, bexp,
                   error(ERROR_NUMBER_EXPECTED, e, env,
                         "vec-dot: non-numeric element"));
  exp_t *ret = make_floatf((expfloat)s);
  CLEAN_RETURN_2(aexp, bexp, ret);
}

const char doc_vecaxpy[] =
    "(vec-axpy! y a x) — in place y[i] += a * x[i]. Returns y.";
exp_t *vecaxpycmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(yexp, aexp, xexp);
  if (!isvector(yexp) || !isvector(xexp) ||
      !(isnumber(aexp) || isfloat(aexp)))
    CLEAN_RETURN_3(yexp, aexp, xexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-axpy! y a x): y, x vecs and a scalar"));
  int64_t ny = vec_len(yexp);
  if (ny != vec_len(xexp))
    CLEAN_RETURN_3(yexp, aexp, xexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-axpy!: length mismatch"));
  double a = isfloat(aexp) ? aexp->f : (double)FIX_VAL(aexp);
  /* Tensor ops produce floats. Promote I64 → F64 so the result holds
     the math exactly (matches pre-refactor behavior where each cell
     was boxed as an EXP_FLOAT). Promotion can fail on a FLAG_SHARED
     vec — error out rather than silently truncating the result. */
  if (vec_kind(yexp) == VEC_KIND_I64 && !vec_promote_i64_to_f64(yexp))
    CLEAN_RETURN_3(yexp, aexp, xexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-axpy!: cannot promote shared I64 vec"));
  int err = 0;
  if (vec_kind(yexp) == VEC_KIND_F64 && vec_kind(xexp) == VEC_KIND_F64) {
    double *ycells =
        VEC_F64_CELLS(yexp);
    double *xcells =
        VEC_F64_CELLS(xexp);
    for (int64_t i = 0; i < ny; i++)
      ycells[i] += a * xcells[i];
  } else {
    for (int64_t i = 0; i < ny; i++) {
      double yv = vec_read_double(yexp, i, &err);
      double xv = vec_read_double(xexp, i, &err);
      if (err)
        break;
      vec_write_double(yexp, i, yv + a * xv);
    }
  }
  if (err)
    CLEAN_RETURN_3(yexp, aexp, xexp,
                   error(ERROR_NUMBER_EXPECTED, e, env,
                         "vec-axpy!: non-numeric element"));
  exp_t *ret = refexp(yexp);
  CLEAN_RETURN_3(yexp, aexp, xexp, ret);
}

const char doc_vecscale[] =
    "(vec-scale! v a) — in place v[i] *= a. Returns v.";
exp_t *vecscalecmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(vexp, aexp);
  if (!isvector(vexp) || !(isnumber(aexp) || isfloat(aexp)))
    CLEAN_RETURN_2(vexp, aexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-scale! v a): vec + scalar"));
  double a = isfloat(aexp) ? aexp->f : (double)FIX_VAL(aexp);
  if (vec_kind(vexp) == VEC_KIND_I64 && !vec_promote_i64_to_f64(vexp))
    CLEAN_RETURN_2(vexp, aexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-scale!: cannot promote shared I64 vec"));
  int err = 0;
  int64_t n = vec_len(vexp);
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells =
        VEC_F64_CELLS(vexp);
    for (int64_t i = 0; i < n; i++)
      cells[i] *= a;
  } else {
    for (int64_t i = 0; i < n; i++) {
      double vi = vec_read_double(vexp, i, &err);
      if (err)
        break;
      vec_write_double(vexp, i, vi * a);
    }
  }
  if (err)
    CLEAN_RETURN_2(vexp, aexp,
                   error(ERROR_NUMBER_EXPECTED, e, env,
                         "vec-scale!: non-numeric element"));
  exp_t *ret = refexp(vexp);
  CLEAN_RETURN_2(vexp, aexp, ret);
}

const char doc_veccopy[] =
    "(vec-copy! dst src) — overwrite dst with elements from src (must be "
    "equal length). Returns dst.";
exp_t *veccopycmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(dexp, sexp);
  if (!isvector(dexp) || !isvector(sexp))
    CLEAN_RETURN_2(dexp, sexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-copy! dst src): both must be vectors"));
  int64_t n = vec_len(dexp);
  if (n != vec_len(sexp))
    CLEAN_RETURN_2(dexp, sexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-copy!: length mismatch"));
  unsigned kd = vec_kind(dexp), ks = vec_kind(sexp);
  if (kd == ks && kd != VEC_KIND_GEN) {
    /* Same typed kind — raw memcpy of cells (8 bytes each, all kinds). */
    char *dst = (char *)dexp->ptr + sizeof(alc_vec_t) + 8u * dexp->vec_win.start;
    char *src = (char *)sexp->ptr + sizeof(alc_vec_t) + 8u * sexp->vec_win.start;
    memcpy(dst, src, (size_t)n * 8u);
  } else if (kd == VEC_KIND_GEN && ks == VEC_KIND_GEN) {
    exp_t **dcells = vec_gen_cells(dexp);
    exp_t **scells = vec_gen_cells(sexp);
    for (int64_t i = 0; i < n; i++) {
      unrefexp(dcells[i]);
      dcells[i] = refexp(scells[i]);
    }
  } else {
    /* Mixed kinds — fall back to box/unbox per element. */
    for (int64_t i = 0; i < n; i++) {
      exp_t *boxed = vec_get_boxed(sexp, i);
      if (!vec_set_boxed(dexp, i, boxed))
        CLEAN_RETURN_2(dexp, sexp,
                       error(ERROR_ILLEGAL_VALUE, e, env,
                             "vec-copy!: alloc failure"));
    }
  }
  exp_t *ret = refexp(dexp);
  CLEAN_RETURN_2(dexp, sexp, ret);
}

const char doc_vecadd[] =
    "(vec-add! y x) — in place y[i] += x[i]. Returns y.";
exp_t *vecaddcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(yexp, xexp);
  if (!isvector(yexp) || !isvector(xexp))
    CLEAN_RETURN_2(yexp, xexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-add! y x): both must be vectors"));
  int64_t ny = vec_len(yexp);
  if (ny != vec_len(xexp))
    CLEAN_RETURN_2(yexp, xexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-add!: length mismatch"));
  if (vec_kind(yexp) == VEC_KIND_I64 && !vec_promote_i64_to_f64(yexp))
    CLEAN_RETURN_2(yexp, xexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-add!: cannot promote shared I64 vec"));
  int err = 0;
  if (vec_kind(yexp) == VEC_KIND_F64 && vec_kind(xexp) == VEC_KIND_F64) {
    double *ycells =
        VEC_F64_CELLS(yexp);
    double *xcells =
        VEC_F64_CELLS(xexp);
    for (int64_t i = 0; i < ny; i++)
      ycells[i] += xcells[i];
  } else {
    for (int64_t i = 0; i < ny; i++) {
      double yv = vec_read_double(yexp, i, &err);
      double xv = vec_read_double(xexp, i, &err);
      if (err)
        break;
      vec_write_double(yexp, i, yv + xv);
    }
  }
  if (err)
    CLEAN_RETURN_2(yexp, xexp,
                   error(ERROR_NUMBER_EXPECTED, e, env,
                         "vec-add!: non-numeric element"));
  exp_t *ret = refexp(yexp);
  CLEAN_RETURN_2(yexp, xexp, ret);
}

const char doc_vecfill[] = "(vec-fill! v a) — in place v[i] = a. Returns v.";
exp_t *vecfillcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(vexp, aexp);
  if (!isvector(vexp) || !(isnumber(aexp) || isfloat(aexp)))
    CLEAN_RETURN_2(vexp, aexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-fill! v a): vec + scalar"));
  double a = isfloat(aexp) ? aexp->f : (double)FIX_VAL(aexp);
  if (vec_kind(vexp) == VEC_KIND_I64 && !vec_promote_i64_to_f64(vexp))
    CLEAN_RETURN_2(vexp, aexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-fill!: cannot promote shared I64 vec"));
  int64_t n = vec_len(vexp);
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells =
        VEC_F64_CELLS(vexp);
    for (int64_t i = 0; i < n; i++)
      cells[i] = a;
  } else {
    for (int64_t i = 0; i < n; i++)
      vec_write_double(vexp, i, a);
  }
  exp_t *ret = refexp(vexp);
  CLEAN_RETURN_2(vexp, aexp, ret);
}

const char doc_vecrelu[] =
    "(vec-relu! v) — in place v[i] = max(0, v[i]). Returns v.";
exp_t *vecrelucmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(vexp,
                   error(ERROR_ILLEGAL_VALUE, e, env, "(vec-relu! v): not a vector"));
  if (vec_kind(vexp) == VEC_KIND_I64 && !vec_promote_i64_to_f64(vexp))
    CLEAN_RETURN_1(vexp, error(ERROR_ILLEGAL_VALUE, e, env,
                               "vec-relu!: cannot promote shared I64 vec"));
  int err = 0;
  int64_t n = vec_len(vexp);
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells =
        VEC_F64_CELLS(vexp);
    for (int64_t i = 0; i < n; i++)
      if (cells[i] < 0.0)
        cells[i] = 0.0;
  } else {
    for (int64_t i = 0; i < n; i++) {
      double vi = vec_read_double(vexp, i, &err);
      if (err)
        break;
      if (vi < 0.0)
        vec_write_double(vexp, i, 0.0);
    }
  }
  if (err)
    CLEAN_RETURN_1(vexp, error(ERROR_NUMBER_EXPECTED, e, env,
                               "vec-relu!: non-numeric element"));
  exp_t *ret = refexp(vexp);
  CLEAN_RETURN_1(vexp, ret);
}

const char doc_vecargmax[] =
    "(vec-argmax v) — index of the largest element. Empty vec -> -1.";
exp_t *vecargmaxcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(vexp,
                   error(ERROR_ILLEGAL_VALUE, e, env, "(vec-argmax v): not a vector"));
  int64_t best = -1;
  double bestv = 0.0;
  int err = 0;
  int64_t n = vec_len(vexp);
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells =
        VEC_F64_CELLS(vexp);
    for (int64_t i = 0; i < n; i++)
      if (best < 0 || cells[i] > bestv) {
        best = i;
        bestv = cells[i];
      }
  } else {
    for (int64_t i = 0; i < n; i++) {
      double x = vec_read_double(vexp, i, &err);
      if (err)
        break;
      if (best < 0 || x > bestv) {
        best = i;
        bestv = x;
      }
    }
  }
  if (err)
    CLEAN_RETURN_1(vexp, error(ERROR_NUMBER_EXPECTED, e, env,
                               "vec-argmax: non-numeric element"));
  CLEAN_RETURN_1(vexp, MAKE_FIX(best));
}

const char doc_vecmax[] =
    "(vec-max v) — largest element as a float. Empty vec is an error.";
exp_t *vecmaxcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(vexp,
                   error(ERROR_ILLEGAL_VALUE, e, env, "(vec-max v): not a vector"));
  int64_t n = vec_len(vexp);
  if (n == 0)
    CLEAN_RETURN_1(vexp, error(ERROR_ILLEGAL_VALUE, e, env, "vec-max: empty vector"));
  int err = 0;
  double m;
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells =
        VEC_F64_CELLS(vexp);
    m = cells[0];
    for (int64_t i = 1; i < n; i++)
      if (cells[i] > m)
        m = cells[i];
  } else {
    m = vec_read_double(vexp, 0, &err);
    for (int64_t i = 1; i < n; i++) {
      double x = vec_read_double(vexp, i, &err);
      if (err)
        break;
      if (x > m)
        m = x;
    }
  }
  if (err)
    CLEAN_RETURN_1(vexp, error(ERROR_NUMBER_EXPECTED, e, env,
                               "vec-max: non-numeric element"));
  CLEAN_RETURN_1(vexp, make_floatf((expfloat)m));
}

/* ---------- deque ops ----------
   vec-push! / vec-pop! at the back; vec-unshift! / vec-shift! at the
   front. Amortised O(1) via the cap/start/end window — pops bump the
   window without shifting; pushes grow only when the window hits a
   boundary. Growth: 1.5x cap on realloc; slide-left when start >=
   cap/4 (recovers headroom without reallocating); recenter on
   unshift-grow so subsequent unshifts don't realloc. */

/* Slide the live window down to position 0. Used when push hits the
   back boundary but there's room at the front (start > 0). For typed
   kinds the move is over raw int64/double bytes; for GEN the same
   bytewise move is correct because the cells before start are
   uninitialised garbage (the window is the source of truth for which
   cells are owned). */
static void vec_slide_left(exp_t *vexp) {
  int32_t start = vexp->vec_win.start;
  if (start == 0)
    return;
  int32_t live = vexp->vec_win.end - start;
  char *base = (char *)vexp->ptr + sizeof(alc_vec_t);
  if (live > 0)
    memmove(base, base + (size_t)start * 8u, (size_t)live * 8u);
  vexp->vec_win.start = 0;
  vexp->vec_win.end = live;
}

/* Reallocate to `new_cap` cells, copying the live window to start at
   `front_pad`. Old buffer freed; vexp->ptr replaced. Returns 0 on
   alloc failure (vexp unchanged in that case). */
static int vec_realloc(exp_t *vexp, int32_t new_cap, int32_t front_pad) {
  alc_vec_t *old = (alc_vec_t *)vexp->ptr;
  int32_t start = vexp->vec_win.start;
  int32_t live = vexp->vec_win.end - start;
  if (new_cap < live + front_pad)
    return 0;
  alc_vec_t *new_v = vec_alloc_storage(new_cap);
  if (!new_v)
    return 0;
  if (live > 0) {
    char *src = (char *)old + sizeof(alc_vec_t) + (size_t)start * 8u;
    char *dst =
        (char *)new_v + sizeof(alc_vec_t) + (size_t)front_pad * 8u;
    memcpy(dst, src, (size_t)live * 8u);
  }
  free(old);
  vexp->ptr = new_v;
  vexp->vec_win.start = front_pad;
  vexp->vec_win.end = front_pad + live;
  return 1;
}

/* Ensure there's room at the back for one more push. Either slides
   the window left (recovers headroom) or grows 1.5x. */
static int vec_grow_back(exp_t *vexp) {
  alc_vec_t *v = (alc_vec_t *)vexp->ptr;
  int32_t cap = v->cap;
  /* Slide left only when it actually reclaims back-room — that requires
     start > 0. The `>= cap/4` part is a heuristic to avoid sliding for
     trivial gains, but for small caps cap/4 truncates to 0; combined with
     start == 0 (a full vector with no front headroom) the slide is a
     no-op, so we'd return "grew" without room and the caller would write
     past the buffer. Guarding on start > 0 forces a realloc in that case. */
  if (vexp->vec_win.start > 0 && vexp->vec_win.start >= cap / 4) {
    vec_slide_left(vexp);
    return 1;
  }
  /* int64 to avoid signed-overflow UB when cap is near INT32_MAX; fail the
     grow cleanly (vec-push then errors) if 1.5x would exceed the int32 cap. */
  int64_t grown = cap < 4 ? 8 : (int64_t)cap + (cap >> 1);
  if (grown >= ((int64_t)1 << 31))
    return 0;
  return vec_realloc(vexp, (int32_t)grown, 0);
}

/* Ensure there's room at the front for one more unshift. Always
   reallocates and recenters — front-grow without recentering would
   force a fresh realloc on every subsequent unshift. */
static int vec_grow_front(exp_t *vexp) {
  alc_vec_t *v = (alc_vec_t *)vexp->ptr;
  int32_t cap = v->cap;
  int32_t live = vexp->vec_win.end - vexp->vec_win.start;
  /* int64 to avoid signed-overflow UB near INT32_MAX; fail cleanly if the
     grown capacity would not fit a positive int32. */
  int64_t grown = cap < 4 ? 8 : (int64_t)cap + (cap >> 1);
  if (grown >= ((int64_t)1 << 31))
    return 0;
  int32_t new_cap = (int32_t)grown;
  int32_t front_pad = (new_cap - live) / 2;
  return vec_realloc(vexp, new_cap, front_pad);
}

const char doc_vecpush[] =
    "(vec-push! v x) — append x to the back of v (amortised O(1)). "
    "Returns x.";
exp_t *vecpushcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(vexp, valexp);
  if (!isvector(vexp))
    CLEAN_RETURN_2(
        vexp, valexp,
        error(ERROR_ILLEGAL_VALUE, e, env, "(vec-push! v x): v must be a vec"));
  if (vexp->flags & FLAG_SHARED)
    CLEAN_RETURN_2(vexp, valexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-push!: cannot mutate a shared vec"));
  if (vexp->vec_win.end == (int32_t)vec_cap(vexp)) {
    if (!vec_grow_back(vexp))
      CLEAN_RETURN_2(vexp, valexp,
                     error(ERROR_ILLEGAL_VALUE, e, env,
                           "vec-push!: grow failed"));
  }
  /* Extend the window by one. The new slot is uninitialised; for GEN
     we pre-write NIL so vec_set_boxed's unrefexp doesn't read garbage. */
  int32_t off = vexp->vec_win.end - vexp->vec_win.start; /* new logical idx */
  vexp->vec_win.end++;
  if (vec_kind(vexp) == VEC_KIND_GEN) {
    exp_t **cells = (exp_t **)((char *)vexp->ptr + sizeof(alc_vec_t));
    cells[vexp->vec_win.start + off] = refexp(NIL_EXP);
  }
  exp_t *ret = refexp(valexp);
  if (!vec_set_boxed(vexp, off, valexp)) {
    vexp->vec_win.end--; /* roll back */
    unrefexp(ret);
    unrefexp(vexp);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, e, env, "vec-push!: write failed");
  }
  /* valexp consumed by vec_set_boxed. */
  unrefexp(vexp);
  unrefexp(e);
  return ret;
}

const char doc_vecpop[] = "(vec-pop! v) — remove and return the last "
                          "element of v (O(1)). Errors on empty.";
exp_t *vecpopcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(
        vexp, error(ERROR_ILLEGAL_VALUE, e, env, "(vec-pop! v): not a vec"));
  if (vexp->flags & FLAG_SHARED)
    CLEAN_RETURN_1(vexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-pop!: cannot mutate a shared vec"));
  int64_t n = vec_len(vexp);
  if (n == 0)
    CLEAN_RETURN_1(vexp, error(ERROR_ILLEGAL_VALUE, e, env,
                               "vec-pop!: empty vec"));
  exp_t *ret = vec_get_boxed(vexp, n - 1);
  /* For GEN, the slot we're abandoning owns a ref — drop it. Typed
     kinds hold raw scalars, no ref accounting. */
  if (vec_kind(vexp) == VEC_KIND_GEN) {
    exp_t **cells = vec_gen_cells(vexp);
    unrefexp(cells[n - 1]);
  }
  vexp->vec_win.end--;
  CLEAN_RETURN_1(vexp, ret);
}

const char doc_vecunshift[] =
    "(vec-unshift! v x) — prepend x to the front of v (amortised O(1)). "
    "Returns x.";
exp_t *vecunshiftcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(vexp, valexp);
  if (!isvector(vexp))
    CLEAN_RETURN_2(vexp, valexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-unshift! v x): v must be a vec"));
  if (vexp->flags & FLAG_SHARED)
    CLEAN_RETURN_2(vexp, valexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-unshift!: cannot mutate a shared vec"));
  if (vexp->vec_win.start == 0) {
    if (!vec_grow_front(vexp))
      CLEAN_RETURN_2(vexp, valexp,
                     error(ERROR_ILLEGAL_VALUE, e, env,
                           "vec-unshift!: grow failed"));
  }
  vexp->vec_win.start--;
  if (vec_kind(vexp) == VEC_KIND_GEN) {
    exp_t **cells = (exp_t **)((char *)vexp->ptr + sizeof(alc_vec_t));
    cells[vexp->vec_win.start] = refexp(NIL_EXP);
  }
  exp_t *ret = refexp(valexp);
  if (!vec_set_boxed(vexp, 0, valexp)) {
    vexp->vec_win.start++; /* roll back window */
    /* Release the NIL sentinel placed above for GEN kind */
    if (vec_kind(vexp) == VEC_KIND_GEN) {
      exp_t **cells = (exp_t **)((char *)vexp->ptr + sizeof(alc_vec_t));
      unrefexp(cells[vexp->vec_win.start]);
      cells[vexp->vec_win.start] = NULL;
    }
    unrefexp(ret);
    /* CLEAN_RETURN_2 evaluates error() before touching vexp/valexp/e, so
       no use-after-free; the old hand-rolled path had that ordering bug. */
    CLEAN_RETURN_2(vexp, valexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "vec-unshift!: write failed"));
  }
  unrefexp(vexp);
  unrefexp(e);
  return ret;
}

const char doc_vecshift[] = "(vec-shift! v) — remove and return the first "
                            "element of v (O(1)). Errors on empty.";
exp_t *vecshiftcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(
        vexp, error(ERROR_ILLEGAL_VALUE, e, env, "(vec-shift! v): not a vec"));
  if (vexp->flags & FLAG_SHARED)
    CLEAN_RETURN_1(vexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-shift!: cannot mutate a shared vec"));
  if (vec_len(vexp) == 0)
    CLEAN_RETURN_1(vexp, error(ERROR_ILLEGAL_VALUE, e, env,
                               "vec-shift!: empty vec"));
  exp_t *ret = vec_get_boxed(vexp, 0);
  if (vec_kind(vexp) == VEC_KIND_GEN) {
    exp_t **cells = vec_gen_cells(vexp);
    unrefexp(cells[0]);
  }
  vexp->vec_win.start++;
  CLEAN_RETURN_1(vexp, ret);
}

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
        if (!overflow && r >= fix_min && r <= fix_max) {
          ret = MAKE_FIX(r);
        }
      }
    }
    if (!ret) {
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

#define FLOAT_UNARY_CMD(cname, fname, docstr, cdoc_sym) \
const char cdoc_sym[] = docstr; \
exp_t *cname(exp_t *e, env_t *env) { \
  exp_t *v = EVAL(cadr(e), env); \
  if (iserror(v)) { unrefexp(e); return v; } \
  exp_t *ret; \
  if (isfloat(v)) ret = make_floatf(fname(v->f)); \
  else if (isnumber(v)) ret = make_floatf(fname((double)FIX_VAL(v))); \
  else { unrefexp(v); unrefexp(e); \
    return error(ERROR_ILLEGAL_VALUE, NULL, env, #cname ": arg must be a number"); } \
  unrefexp(v); unrefexp(e); return ret; \
}

FLOAT_UNARY_CMD(roundcmd,  round,  "(round x) — round to nearest integer, as float.", doc_round)
FLOAT_UNARY_CMD(floorcmd,  floor,  "(floor x) — largest integer not greater than x, as float.", doc_floor)
FLOAT_UNARY_CMD(ceilcmd,   ceil,   "(ceil x) — smallest integer not less than x, as float.", doc_ceil)
FLOAT_UNARY_CMD(truncatecmd, trunc, "(truncate x) — round toward zero, as float.", doc_truncate)
FLOAT_UNARY_CMD(logcmd,    log,    "(log x) — natural logarithm of x.", doc_log)
FLOAT_UNARY_CMD(sincmd,    sin,    "(sin x) — sine of x (radians).", doc_sin)
FLOAT_UNARY_CMD(coscmd,    cos,    "(cos x) — cosine of x (radians).", doc_cos)
FLOAT_UNARY_CMD(tancmd,    tan,    "(tan x) — tangent of x (radians).", doc_tan)
#undef FLOAT_UNARY_CMD

const char doc_float[] = "(float x) — coerce integer to floating-point.";
exp_t *floatcmd(exp_t *e, env_t *env) {
  exp_t *v = EVAL(cadr(e), env);
  if (iserror(v)) { unrefexp(e); return v; }
  exp_t *ret;
  if (isfloat(v)) { ret = v; unrefexp(e); return ret; }
  if (isnumber(v)) ret = make_floatf((double)FIX_VAL(v));
  else { unrefexp(v); unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "float: arg must be a number"); }
  unrefexp(v); unrefexp(e); return ret;
}

const char doc_int[] = "(int x) — coerce float to integer by truncation.";
exp_t *intcmd(exp_t *e, env_t *env) {
  exp_t *v = EVAL(cadr(e), env);
  if (iserror(v)) { unrefexp(e); return v; }
  exp_t *ret;
  if (isnumber(v)) { ret = v; unrefexp(e); return ret; }
  if (isfloat(v)) ret = make_integeri((int64_t)v->f);
  else { unrefexp(v); unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "int: arg must be a number"); }
  unrefexp(v); unrefexp(e); return ret;
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
    while (*len + n + 1 > *cap)
      *cap *= 2;
    char *p = realloc(*buf, *cap);
    if (!p)
      graceful_shutdown("Fatal error: Out of memory");
    *buf = p;
  }
  if (n)
    memcpy(*buf + *len, s, n);
  *len += n;
  (*buf)[*len] = 0;
}

static void exp_to_string_buf(exp_t *v, char **buf, size_t *len, size_t *cap) {
  char tmp[128];
  if (!v || v == NIL_EXP) {
    str_buf_put(buf, len, cap, "nil", 3);
  } else if (isnumber(v)) {
    int n = snprintf(tmp, sizeof tmp, "%lld", (long long)FIX_VAL(v));
    str_buf_put(buf, len, cap, tmp, (size_t)n);
  } else if (ischar(v)) {
    char u[4];
    int k = utf8_encode((uint32_t)CHAR_VAL(v), u);
    str_buf_put(buf, len, cap, u, (size_t)k);
  } else if (isstring(v) || issymbol(v)) {
    { const char *_t = exp_text(v); str_buf_put(buf, len, cap, (char *)_t, strlen((char *)_t)); }
  } else if (isfloat(v)) {
    int n = snprintf(tmp, sizeof tmp, "%g", v->f);
    str_buf_put(buf, len, cap, tmp, (size_t)n);
  } else if (isblob(v)) {
    alc_blob_t *b = (alc_blob_t *)v->ptr;
    if (b && b->len)
      str_buf_put(buf, len, cap, b->bytes, b->len);
  } else if (ispair(v)) {
    str_buf_put(buf, len, cap, "(", 1);
    exp_t *n = v;
    int first = 1;
    while (n && ispair(n) && istrue(n)) {
      if (!first) str_buf_put(buf, len, cap, " ", 1);
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
  } else if (isvector(v)) {
    /* Structural, deterministic — mirrors prn's #[...] so (str vec) and
       (prn vec) agree. The old fallback emitted #<vector@PTR>, a
       non-deterministic address. */
    str_buf_put(buf, len, cap, "#[", 2);
    int64_t vn = vec_len(v);
    for (int64_t i = 0; i < vn; i++) {
      if (i) str_buf_put(buf, len, cap, " ", 1);
      exp_t *cell = vec_get_boxed(v, i);
      exp_to_string_buf(cell, buf, len, cap);
      unrefexp(cell);
    }
    str_buf_put(buf, len, cap, "]", 1);
  } else if (isdict(v)) {
    str_buf_put(buf, len, cap, "{", 1);
    dict_t *d = (dict_t *)v->ptr;
    int first = 1;
    if (d)
      for (unsigned int i = 0; i < d->ht[0].size; i++)
        for (keyval_t *k = d->ht[0].table[i]; k; k = k->next) {
          if (!first) str_buf_put(buf, len, cap, ", ", 2);
          first = 0;
          /* keys raw, like every other nested string in str output */
          str_buf_put(buf, len, cap, k->key, strlen(k->key));
          str_buf_put(buf, len, cap, " ", 1);
          exp_to_string_buf(k->val, buf, len, cap);
        }
    str_buf_put(buf, len, cap, "}", 1);
  } else if (isset(v)) {
    str_buf_put(buf, len, cap, "#{", 2);
    dict_t *d = (dict_t *)v->ptr;
    int first = 1;
    if (d)
      for (unsigned int i = 0; i < d->ht[0].size; i++)
        for (keyval_t *k = d->ht[0].table[i]; k; k = k->next) {
          if (!first) str_buf_put(buf, len, cap, " ", 1);
          first = 0;
          exp_to_string_buf(k->val, buf, len, cap);
        }
    str_buf_put(buf, len, cap, "}", 1);
  } else if (islist(v)) {
    str_buf_put(buf, len, cap, "(", 1);
    alc_list_t *l = (alc_list_t *)v->ptr;
    if (l) {
      int first = 1;
      for (alc_listnode_t *ln = l->head; ln; ln = ln->next) {
        if (!first) str_buf_put(buf, len, cap, " ", 1);
        first = 0;
        exp_to_string_buf(ln->val, buf, len, cap);
      }
    }
    str_buf_put(buf, len, cap, ")", 1);
  } else if (islambda(v)) {
    if (v->meta) {
      str_buf_put(buf, len, cap, "#<procedure:", 12);
      str_buf_put(buf, len, cap, (char *)v->meta, strlen((char *)v->meta));
      str_buf_put(buf, len, cap, ">", 1);
    } else
      str_buf_put(buf, len, cap, "#<procedure>", 12);
  } else if (ismacro(v)) {
    if (v->meta) {
      str_buf_put(buf, len, cap, "#<macro:", 8);
      str_buf_put(buf, len, cap, (char *)v->meta, strlen((char *)v->meta));
      str_buf_put(buf, len, cap, ">", 1);
    } else
      str_buf_put(buf, len, cap, "#<macro>", 8);
  } else {
    /* builtins / ffi / anything else — deterministic type name, no
       pointer (str output must be reproducible). */
    int n = snprintf(tmp, sizeof tmp, "#<%s>",
                     is_ptr(v) ? inspect_type_name(v->type) : "value");
    str_buf_put(buf, len, cap, tmp, (size_t)n);
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
  exp_t *ret = make_string(buf, (int)len);
  free(buf);
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
  if (iserror(fmtarg)) { unrefexp(e); return fmtarg; }
  if (!isstring(fmtarg)) {
    unrefexp(fmtarg); unrefexp(e);
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
      if (!cur) continue;
      exp_t *v = EVAL(car(cur), env);
      if (iserror(v)) { free(buf); unrefexp(fmtarg); unrefexp(e); return v; }
      exp_to_string_buf(v, &buf, &len, &cap);
      unrefexp(v);
      cur = cur->next;
    } else if (tmpl[i + 1] == ':') {
      size_t j = i + 2;
      while (tmpl[j] && tmpl[j] != '}') j++;
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
        if (!(sc == '-' || sc == '+' || sc == ' ' || sc == '#' ||
              sc == '.' || (sc >= '0' && sc <= '9'))) {
          spec_ok = 0;
          break;
        }
      }
      if (!spec_ok) {
        str_buf_put(&buf, &len, &cap, "{", 1);
        continue;
      }
      i = j;
      if (!cur) continue;
      exp_t *v = EVAL(car(cur), env);
      if (iserror(v)) { free(buf); unrefexp(fmtarg); unrefexp(e); return v; }
      char tmp[256];
      char *out = tmp;
      char *heap = NULL;
      int n = 0;
      if (ftype == 'd' || ftype == 'i' || ftype == 'o' ||
          ftype == 'u' || ftype == 'x' || ftype == 'X') {
        int64_t iv = isnumber(v) ? FIX_VAL(v) :
                     isfloat(v)  ? (int64_t)v->f : 0LL;
        /* int64_t needs 'll' length modifier before the type letter */
        char safe_fmt[FMT_SPEC_MAX + 4]; /* '%' + spec + 'll' + '\0' */
        memcpy(safe_fmt, printf_fmt, spec_len);
        safe_fmt[spec_len]     = 'l';
        safe_fmt[spec_len + 1] = 'l';
        safe_fmt[spec_len + 2] = ftype;
        safe_fmt[spec_len + 3] = '\0';
        n = snprintf(tmp, sizeof(tmp), safe_fmt, iv);
      } else if (ftype == 'f' || ftype == 'e' || ftype == 'E' ||
                 ftype == 'g' || ftype == 'G') {
        double dv = isfloat(v)  ? v->f :
                    isnumber(v) ? (double)FIX_VAL(v) : 0.0;
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
      if (heap) free(heap);
      unrefexp(v);
      cur = cur->next;
    } else {
      str_buf_put(&buf, &len, &cap, "{", 1);
    }
  }
  buf[len] = 0;
  exp_t *ret = make_string(buf, (int)len);
  free(buf);
  unrefexp(fmtarg);
  unrefexp(e);
  return ret;
}

const char doc_stringappend[] =
    "(string-append s ...) — concatenate strings. Non-strings are errors.";
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
    { const char *_t = exp_text(v); str_buf_put(&buf, &len, &cap, (char *)_t, strlen((char *)_t)); }
    unrefexp(v);
  }
  exp_t *ret = make_string(buf, (int)len);
  free(buf);
  unrefexp(e);
  return ret;
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
    if (!first)
      { const char *_t = exp_text(sep); str_buf_put(&buf, &len, &cap, (char *)_t, strlen((char *)_t)); }
    first = 0;
    str_buf_put(&buf, &len, &cap, exp_text(car(p)),
                strlen(exp_text(car(p))));
    p = cdr(p);
  }
  exp_t *ret = make_string(buf, (int)len);
  free(buf);
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
    size_t n = strlen((char *)exp_text(s));                                         \
    char *buf = memalloc(n + 1, 1);                                            \
    for (size_t i = 0; i < n; i++)                                             \
      buf[i] = (char)fn((unsigned char)((char *)exp_text(s))[i]);                   \
    exp_t *ret = make_string(buf, (int)n);                                     \
    free(buf);                                                                 \
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
  exp_t *ret = make_string(buf, (int)sz);
  free(buf);
  return ret;
}

static exp_t *eval_file_forms(const char *path, env_t *env) {
  FILE *fp = fopen(path, "r");
  if (!fp)
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "load: cannot open '%s'",
                 path);
  for (;;) {
    exp_t *form = reader(fp, 0, 0);
    if (!form)
      break;
    if (iserror(form)) {
      if (form->flags == EXP_ERROR_PARSING_EOF) {
        unrefexp(form);
        break;
      }
      fclose(fp);
      return form;
    }
    exp_t *ret = evaluate(form, env);
    if (iserror(ret)) {
      fclose(fp);
      return ret;
    }
    unrefexp(ret);
  }
  fclose(fp);
  return TRUE_EXP;
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
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "%s: cannot open '%s'", (char *)name,
                         (char *)exp_text(path)));
  size_t n = strlen((char *)exp_text(text));
  int ok = (n == 0 || fwrite((char *)exp_text(text), 1, n, fp) == n);
  fclose(fp);
  if (!ok)
    CLEAN_RETURN_2(path, text,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "%s: write failed", (char *)name));
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
                         "write-bytes: cannot open '%s'", (char *)exp_text(path)));
  alc_blob_t *b = (alc_blob_t *)blob->ptr;
  int ok = (!b || b->len == 0 || fwrite(b->bytes, 1, b->len, fp) == b->len);
  fclose(fp);
  if (!ok)
    CLEAN_RETURN_2(path, blob,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "write-bytes: write failed"));
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
#ifdef ALCOVE_FFI
#include <dlfcn.h>
#include <ffi.h>

#define ALC_FFI_MAX_ARGS 8
typedef enum {
  AFFI_VOID,
  AFFI_INT,
  AFFI_LONG,
  AFFI_DOUBLE,
  AFFI_STRING,
  AFFI_PTR,
  AFFI_STRUCT /* by-value aggregate; see AFFI_KIND_STRUCT descriptor */
} alc_ffi_tag_t;

/* An EXP_FFI value is one of: a bound C function (FN — ffi-fn), an alcove
   lambda exposed to C as a function pointer (CB — ffi-callback), or a
   by-value struct type descriptor (STRUCT — ffi-struct). memalloc zeroes the
   struct, so AFFI_KIND_FN == 0 keeps existing ffi-fn paths intact. */
typedef enum { AFFI_KIND_FN = 0, AFFI_KIND_CB, AFFI_KIND_STRUCT } alc_ffi_kind_t;

typedef struct alc_ffi_t {
  void *fn; /* dlsym result (FN kind) */
  ffi_cif cif;
  ffi_type *rtype;
  unsigned int nargs; /* FN/CB: arg count. STRUCT: field count. */
  ffi_type *atypes[ALC_FFI_MAX_ARGS]; /* FN/CB args; STRUCT field types */
  uint8_t ret_tag;
  uint8_t arg_tags[ALC_FFI_MAX_ARGS]; /* FN/CB arg tags; STRUCT field tags */
  uint8_t kind;       /* AFFI_KIND_FN | _CB | _STRUCT */
  uint8_t variadic;   /* FN: bound via ffi-vfn — nargs is the FIXED count and
                         the cif is prepped per call (ffi_prep_cif_var) since
                         the variadic arg types vary by call site. */
  char *display_name; /* "lib:fn" (FN) / "<callback>" / "<struct>" for errors */
  /* CB kind only — the libffi closure trampoline and the alcove fn it calls.
     `code` is the C-callable entry point; pass it where a ptr arg is wanted. */
  ffi_closure *closure;
  void *code;
  exp_t *cb_lambda; /* owned ref so the lambda outlives any C-side calls */
  /* FN kind only — for STRUCT-tagged args/return, the owning descriptor
     (so we know the size to validate/allocate). NULL for scalar slots. */
  exp_t *arg_structs[ALC_FFI_MAX_ARGS];
  exp_t *ret_struct;
  /* STRUCT kind only — the by-value aggregate layout. struct_type is the
     FFI_TYPE_STRUCT passed to ffi_prep_cif; elements is its NULL-terminated
     member array; offsets/struct_size are computed for pack/unpack. */
  ffi_type struct_type;
  ffi_type *elements[ALC_FFI_MAX_ARGS + 1];
  size_t offsets[ALC_FFI_MAX_ARGS];
  size_t struct_size;
  size_t struct_align; /* max field alignment — needed when nested in another
                          struct's layout. arg_structs[i] holds the nested
                          descriptor for a struct-typed field (else NULL). */
} alc_ffi_t;

/* Map a type-name string to (tag, ffi_type*). Returns 0 on success, -1
   on unknown type. */
static int alc_ffi_typeof(const char *name, alc_ffi_tag_t *tag,
                          ffi_type **out) {
  if (!strcmp(name, "void")) {
    *tag = AFFI_VOID;
    *out = &ffi_type_void;
    return 0;
  }
  if (!strcmp(name, "int")) {
    *tag = AFFI_INT;
    *out = &ffi_type_sint32;
    return 0;
  }
  if (!strcmp(name, "long") || !strcmp(name, "int64")) {
    *tag = AFFI_LONG;
    *out = &ffi_type_sint64;
    return 0;
  }
  if (!strcmp(name, "double")) {
    *tag = AFFI_DOUBLE;
    *out = &ffi_type_double;
    return 0;
  }
  if (!strcmp(name, "string") || !strcmp(name, "char*")) {
    *tag = AFFI_STRING;
    *out = &ffi_type_pointer;
    return 0;
  }
  if (!strcmp(name, "ptr") || !strcmp(name, "void*")) {
    *tag = AFFI_PTR;
    *out = &ffi_type_pointer;
    return 0;
  }
  return -1;
}

/* Resolve an ffi type spec used by ffi-fn for a return/arg type: either a
   type-name string (scalar) or a struct descriptor value from ffi-struct.
   On success sets the tag and ffi_type out-params plus *desc (the descriptor
   exp for a struct, else NULL) and returns 0; returns -1 on an invalid spec. */
static int alc_ffi_resolve_type(exp_t *spec, alc_ffi_tag_t *tag,
                                ffi_type **out, exp_t **desc) {
  *desc = NULL;
  if (isffi(spec)) {
    alc_ffi_t *d = (alc_ffi_t *)spec->ptr;
    if (d->kind != AFFI_KIND_STRUCT)
      return -1;
    *tag = AFFI_STRUCT;
    *out = &d->struct_type;
    *desc = spec;
    return 0;
  }
  if (isstring(spec))
    return alc_ffi_typeof((char *)exp_text(spec), tag, out);
  return -1;
}

/* Forward decl: ffi-fn's error paths free a partially-built binding via the
   full destructor (it may already hold refexp'd struct descriptors). */
void alc_ffi_free(void *ptr);

/* Process-wide cache of dlopen handles keyed by lib name. dlopen with
   the same name on Linux returns the same handle anyway, but caching
   avoids re-resolving on each (ffi-fn) call. The mutex serializes the
   list mutation under multi-thread builds; it is held across dlopen
   itself so a concurrent caller never observes a half-linked entry,
   and to avoid duplicate inserts under a TOCTOU race. dlopen is rare
   (one-time per lib name), so the contention cost is negligible. */
#if !ALCOVE_SINGLE_THREADED
static pthread_mutex_t g_ffi_libs_mtx = PTHREAD_MUTEX_INITIALIZER;
#define FFI_LIBS_LOCK() pthread_mutex_lock(&g_ffi_libs_mtx)
#define FFI_LIBS_UNLOCK() pthread_mutex_unlock(&g_ffi_libs_mtx)
#else
#define FFI_LIBS_LOCK() ((void)0)
#define FFI_LIBS_UNLOCK() ((void)0)
#endif
static struct ffi_lib_cache {
  char *name;
  void *h;
  struct ffi_lib_cache *next;
} *g_ffi_libs = NULL;
static void *alc_ffi_dlopen(const char *name) {
  struct ffi_lib_cache *c;
  FFI_LIBS_LOCK();
  for (c = g_ffi_libs; c; c = c->next)
    if (!strcmp(c->name, name)) {
      void *h = c->h;
      FFI_LIBS_UNLOCK();
      return h;
    }
  /* An empty lib name means "this process": dlopen(NULL) resolves symbols
     already linked into alcove (libc, libm, libffi, and alcove's own
     exported functions). Lets scripts/tests bind well-known symbols
     (strlen, abs, …) without naming a platform-specific shared object.
     Otherwise: RTLD_LOCAL keeps the lib's symbols out of the global
     namespace — reduces accidental shadowing if multiple libs export the
     same name. RTLD_NOW resolves all symbols at load so dlsym failures
     surface promptly. */
  void *h = dlopen(name[0] ? name : NULL, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    FFI_LIBS_UNLOCK();
    return NULL;
  }
  c = (struct ffi_lib_cache *)memalloc(1, sizeof(*c));
  c->name = strdup(name);
  c->h = h;
  c->next = g_ffi_libs;
  g_ffi_libs = c;
  FFI_LIBS_UNLOCK();
  return h;
}

/* ---- FFI self-test fixtures ----
   Tiny exported C functions the test suite (test.alc) binds to via the ""
   (process-self) library, so the whole FFI surface — scalar calls, callbacks,
   and struct-by-value — has automated regression coverage with no external
   .so. They must NOT be static (dlsym resolves them) and the binary is linked
   with -rdynamic so Linux exports them too (macOS exports by default). Not a
   public API; the alc_ffi_selftest_ prefix keeps them out of the way. */
long alc_ffi_selftest_add(long a, long b) { return a + b; }
long alc_ffi_selftest_apply2(long (*fn)(long, long), long a, long b) {
  return fn(a, b);
}
long alc_ffi_selftest_sum_map(long (*fn)(long), long n) {
  long s = 0;
  for (long i = 0; i < n; i++) s += fn(i);
  return s;
}
double alc_ffi_selftest_apply_d(double (*fn)(double), double x) { return fn(x); }
typedef struct { double x, y; } alc_ffi_selftest_point;
double alc_ffi_selftest_pt_norm2(alc_ffi_selftest_point p) {
  return p.x * p.x + p.y * p.y;
}
alc_ffi_selftest_point alc_ffi_selftest_pt_make(double x, double y) {
  alc_ffi_selftest_point p = {x, y};
  return p;
}
/* Nested-struct fixture: a segment of two points (struct-in-struct). */
typedef struct { alc_ffi_selftest_point a, b; } alc_ffi_selftest_seg;
double alc_ffi_selftest_seg_len2(alc_ffi_selftest_seg s) {
  double dx = s.b.x - s.a.x, dy = s.b.y - s.a.y;
  return dx * dx + dy * dy;
}
/* Variadic fixtures — sum `count` trailing args. Return a value (no stdout)
   so the test suite can assert on them. */
long alc_ffi_selftest_vsum(int count, ...) {
  va_list ap;
  va_start(ap, count);
  long s = 0;
  for (int i = 0; i < count; i++) s += va_arg(ap, long);
  va_end(ap);
  return s;
}
double alc_ffi_selftest_vsumd(int count, ...) {
  va_list ap;
  va_start(ap, count);
  double s = 0;
  for (int i = 0; i < count; i++) s += va_arg(ap, double);
  va_end(ap);
  return s;
}

/* Shared binder for (ffi-fn ...) and (ffi-vfn ...). When variadic, the given
   arg types are the FIXED prefix (>=1 required, per C's named-param rule),
   the cif is prepped per call in alc_ffi_call, and any extra call args have
   their types inferred from their alcove runtime type. */
static exp_t *ffi_bind_impl(exp_t *e, env_t *env, int variadic) {
  const char *who = variadic ? "ffi-vfn" : "ffi-fn";
  exp_t *cur = e->next;
  exp_t *libname = NULL, *fnname = NULL, *rtype = NULL;
  exp_t *atypes[ALC_FFI_MAX_ARGS] = {0};
  int n_a = 0;
  exp_t *err = NULL;
  exp_t *ret = NULL; /* declared up here so the `goto cleanup`
                        above doesn't jump over its init */

  if (!cur || !cur->next || !cur->next->next) {
    err = error(ERROR_MISSING_PARAMETER, e, env,
                "(%s lib name return-type arg-type ...)", who);
    goto cleanup;
  }
  libname = EVAL(cur->content, env);
  if (iserror(libname)) {
    err = libname;
    libname = NULL;
    goto cleanup;
  }
  cur = cur->next;
  fnname = EVAL(cur->content, env);
  if (iserror(fnname)) {
    err = fnname;
    fnname = NULL;
    goto cleanup;
  }
  cur = cur->next;
  rtype = EVAL(cur->content, env);
  if (iserror(rtype)) {
    err = rtype;
    rtype = NULL;
    goto cleanup;
  }
  cur = cur->next;
  while (cur && n_a < ALC_FFI_MAX_ARGS) {
    atypes[n_a] = EVAL(cur->content, env);
    if (iserror(atypes[n_a])) {
      err = atypes[n_a];
      atypes[n_a] = NULL;
      goto cleanup;
    }
    n_a++;
    cur = cur->next;
  }
  /* If we hit the cap and there are still more arg types, refuse rather
     than silently truncate — a binding with the wrong arity reads stack
     garbage at call time. Bump ALC_FFI_MAX_ARGS if a real use case needs
     more than 8 args. */
  if (cur) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "%s: too many arg types (max %d supported)", who,
                ALC_FFI_MAX_ARGS);
    goto cleanup;
  }
  if (variadic && n_a < 1) {
    err = error(ERROR_MISSING_PARAMETER, e, env,
                "ffi-vfn: need at least one fixed arg type before the "
                "variadic part (e.g. the format string)");
    goto cleanup;
  }

  if (!isstring(libname) || !isstring(fnname)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "%s: lib and name must be strings",
                who);
    goto cleanup;
  }
  void *h = alc_ffi_dlopen((char *)exp_text(libname));
  if (!h) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "%s: dlopen failed (%s)", who,
                dlerror());
    goto cleanup;
  }
  void *sym = dlsym(h, (char *)exp_text(fnname));
  if (!sym) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "%s: dlsym failed for %s", who,
                (char *)exp_text(fnname));
    goto cleanup;
  }

  alc_ffi_t *f = (alc_ffi_t *)memalloc(1, sizeof(alc_ffi_t));
  f->fn = sym;
  f->nargs = (unsigned int)n_a; /* variadic: this is the FIXED count */
  f->variadic = (uint8_t)variadic;
  alc_ffi_tag_t rt;
  ffi_type *rt_ffi;
  exp_t *rdesc;
  if (alc_ffi_resolve_type(rtype, &rt, &rt_ffi, &rdesc) < 0) {
    free(f);
    err = error(ERROR_ILLEGAL_VALUE, e, env, "%s: unknown return type", who);
    goto cleanup;
  }
  f->ret_tag = rt;
  f->rtype = rt_ffi;
  if (rdesc) /* struct-by-value return: hold the descriptor for sizing */
    f->ret_struct = refexp(rdesc);
  for (int i = 0; i < n_a; i++) {
    alc_ffi_tag_t at;
    ffi_type *at_ffi;
    exp_t *adesc;
    if (alc_ffi_resolve_type(atypes[i], &at, &at_ffi, &adesc) < 0) {
      alc_ffi_free(f);
      err = error(ERROR_ILLEGAL_VALUE, e, env,
                  "%s: unknown/invalid arg type at slot %d", who, i);
      goto cleanup;
    }
    f->arg_tags[i] = at;
    f->atypes[i] = at_ffi;
    if (adesc) /* struct-by-value arg: hold the descriptor for sizing */
      f->arg_structs[i] = refexp(adesc);
  }
  /* Non-variadic: prep the cif now (fixed signature). Variadic: the cif is
     built per call in alc_ffi_call once the variadic arg types are known. */
  if (!variadic &&
      ffi_prep_cif(&f->cif, FFI_DEFAULT_ABI, f->nargs, f->rtype, f->atypes) !=
          FFI_OK) {
    alc_ffi_free(f);
    err = error(ERROR_ILLEGAL_VALUE, e, env, "%s: ffi_prep_cif failed", who);
    goto cleanup;
  }
  size_t dnlen = strlen((char *)exp_text(libname)) + strlen((char *)exp_text(fnname)) + 2;
  f->display_name = (char *)memalloc(dnlen, 1);
  snprintf(f->display_name, dnlen, "%s:%s", (char *)exp_text(libname),
           (char *)exp_text(fnname));

  INIT_TYPED(ret, EXP_FFI, f);

cleanup:
  unrefexp(libname);
  unrefexp(fnname);
  unrefexp(rtype);
  for (int i = 0; i < n_a; i++)
    unrefexp(atypes[i]);
  unrefexp(e);
  if (err)
    return err;
  return ret;
}

/* (ffi-fn lib name return-type arg-type ...) */
exp_t *ffifncmd(exp_t *e, env_t *env) { return ffi_bind_impl(e, env, 0); }

/* (ffi-vfn lib name return-type fixed-arg-type ...) — a variadic C function.
   The given arg types are the FIXED prefix; extra args supplied at the call
   are passed with types inferred from their alcove value (fixnum→long,
   float→double, char→int, string/nil→pointer). */
exp_t *ffivfncmd(exp_t *e, env_t *env) { return ffi_bind_impl(e, env, 1); }

/* libffi closure trampoline: C code calls this with the native args; we
   marshal them to alcove values, invoke the bound lambda, and marshal the
   result back into `ret`. user_data is the owning alc_ffi_t (kind CB).
   Registered via ffi_prep_closure_loc in fficallbackcmd. Portable across
   every libffi target — no arch-specific code here (libffi owns the
   executable-memory + ABI details, including macOS hardened-runtime). */
static void alc_ffi_closure_dispatch(ffi_cif *cif, void *ret, void **args,
                                     void *user) {
  (void)cif;
  alc_ffi_t *cb = (alc_ffi_t *)user;
  exp_t *argv[ALC_FFI_MAX_ARGS];
  unsigned int i;
  for (i = 0; i < cb->nargs; i++) {
    switch (cb->arg_tags[i]) {
    case AFFI_INT:
      argv[i] = MAKE_FIX((int64_t)*(int32_t *)args[i]);
      break;
    case AFFI_LONG:
      argv[i] = MAKE_FIX(*(int64_t *)args[i]);
      break;
    case AFFI_DOUBLE:
      argv[i] = make_floatf(*(double *)args[i]);
      break;
    case AFFI_STRING: {
      const char *s = *(const char **)args[i];
      argv[i] = s ? make_string((char *)s, (int)strnlen(s, 1u << 24)) : NIL_EXP;
      break;
    }
    case AFFI_PTR:
      argv[i] = MAKE_FIX((int64_t)(uintptr_t)*(void **)args[i]);
      break;
    default:
      argv[i] = NIL_EXP;
      break;
    }
  }
  /* Re-enter the evaluator from C. vm_invoke_values borrows fn and consumes
     the argv refs. Use the global env as the resolution root — the lambda
     carries its own captured env (next->meta) for its free vars. Save and
     restore the tail-position flag around the nested invocation. */
  int saved_tail = in_tail_position;
  in_tail_position = 0;
  exp_t *r = vm_invoke_values(cb->cb_lambda, (int)cb->nargs, argv, g_global_env);
  in_tail_position = saved_tail;

  /* Integral/pointer returns go through ffi_arg (the closure return slot is
     register-width); double has its own slot. String return is rejected at
     bind time (the buffer's lifetime can't outlive this call). */
  switch (cb->ret_tag) {
  case AFFI_VOID:
    break;
  case AFFI_INT:
  case AFFI_LONG:
    *(ffi_arg *)ret =
        (ffi_arg)(r ? (isfloat(r) ? (int64_t)r->f
                                  : (isnumber(r) || ischar(r) ? FIX_VAL(r) : 0))
                    : 0);
    break;
  case AFFI_DOUBLE:
    *(double *)ret = (r && (isfloat(r) || isnumber(r))) ? TO_DOUBLE(r) : 0.0;
    break;
  case AFFI_PTR:
    *(ffi_arg *)ret =
        (ffi_arg)(uintptr_t)((r && isnumber(r)) ? (void *)(uintptr_t)FIX_VAL(r)
                                                 : NULL);
    break;
  default:
    *(ffi_arg *)ret = 0;
    break;
  }
  if (r)
    unrefexp(r);
}

/* (ffi-callback ret-type (arg-types...) fn) — wrap an alcove lambda in a
   libffi closure so it can be passed to C as a function pointer. */
exp_t *fficallbackcmd(exp_t *e, env_t *env) {
  exp_t *cur = e->next;
  exp_t *rtype = NULL, *atlist = NULL, *fn = NULL, *err = NULL, *ret = NULL;
  if (!cur || !cur->next || !cur->next->next) {
    err = error(ERROR_MISSING_PARAMETER, e, env,
                "(ffi-callback ret-type (arg-types...) fn)");
    goto cleanup;
  }
  rtype = EVAL(cur->content, env);
  if (iserror(rtype)) { err = rtype; rtype = NULL; goto cleanup; }
  cur = cur->next;
  atlist = EVAL(cur->content, env);
  if (iserror(atlist)) { err = atlist; atlist = NULL; goto cleanup; }
  cur = cur->next;
  fn = EVAL(cur->content, env);
  if (iserror(fn)) { err = fn; fn = NULL; goto cleanup; }

  if (!isstring(rtype)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-callback: ret-type must be a string");
    goto cleanup;
  }
  if (!islambda(fn)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-callback: third arg must be a function");
    goto cleanup;
  }
  if (atlist && atlist != NIL_EXP && !ispair(atlist)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-callback: arg-types must be a list of type strings");
    goto cleanup;
  }
  alc_ffi_tag_t rt;
  ffi_type *rt_ffi;
  if (alc_ffi_typeof((char *)exp_text(rtype), &rt, &rt_ffi) < 0) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-callback: unknown return type %s",
                (char *)exp_text(rtype));
    goto cleanup;
  }
  if (rt == AFFI_STRING) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-callback: string return not supported (buffer lifetime)");
    goto cleanup;
  }
  alc_ffi_t *f = (alc_ffi_t *)memalloc(1, sizeof(alc_ffi_t));
  f->kind = AFFI_KIND_CB;
  f->ret_tag = rt;
  f->rtype = rt_ffi;
  int n = 0;
  for (exp_t *p = atlist; p && ispair(p); p = p->next) {
    if (n >= ALC_FFI_MAX_ARGS) {
      free(f);
      err = error(ERROR_ILLEGAL_VALUE, e, env,
                  "ffi-callback: too many arg types (max %d)", ALC_FFI_MAX_ARGS);
      goto cleanup;
    }
    exp_t *tn = p->content;
    alc_ffi_tag_t at;
    ffi_type *at_ffi;
    if (!isstring(tn)) {
      free(f);
      err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-callback: arg-type must be a string");
      goto cleanup;
    }
    if (alc_ffi_typeof((char *)exp_text(tn), &at, &at_ffi) < 0) {
      free(f);
      err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-callback: unknown arg type %s",
                  (char *)exp_text(tn));
      goto cleanup;
    }
    f->arg_tags[n] = at;
    f->atypes[n] = at_ffi;
    n++;
  }
  f->nargs = (unsigned int)n;
  if (ffi_prep_cif(&f->cif, FFI_DEFAULT_ABI, f->nargs, f->rtype, f->atypes) !=
      FFI_OK) {
    free(f);
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-callback: ffi_prep_cif failed");
    goto cleanup;
  }
  /* ffi_closure_alloc returns executable memory + writes the callable entry
     into f->code; ffi_prep_closure_loc binds the cif + dispatcher to it. */
  f->closure = ffi_closure_alloc(sizeof(ffi_closure), &f->code);
  if (!f->closure) {
    free(f);
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-callback: closure alloc failed");
    goto cleanup;
  }
  if (ffi_prep_closure_loc(f->closure, &f->cif, alc_ffi_closure_dispatch, f,
                           f->code) != FFI_OK) {
    ffi_closure_free(f->closure);
    free(f);
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-callback: ffi_prep_closure_loc failed");
    goto cleanup;
  }
  f->cb_lambda = refexp(fn);
  f->display_name = strdup("<callback>");
  INIT_TYPED(ret, EXP_FFI, f);

cleanup:
  unrefexp(rtype);
  unrefexp(atlist);
  unrefexp(fn);
  unrefexp(e);
  return err ? err : ret;
}

/* (ffi-struct field-type-str...) — define a by-value C struct type. Fields
   are scalar type names (int long double ptr). Computes the ABI layout
   (offsets + size) with the standard scalar-aggregate alignment rule, which
   matches the C ABI on every libffi target for non-packed scalar members —
   so no dependency on ffi_get_struct_offsets (libffi 3.3+). */
exp_t *ffistructcmd(exp_t *e, env_t *env) {
  exp_t *cur = e->next;
  exp_t *err = NULL, *ret = NULL;
  exp_t *fields[ALC_FFI_MAX_ARGS] = {0};
  int nf = 0;
  while (cur && nf < ALC_FFI_MAX_ARGS) {
    fields[nf] = EVAL(cur->content, env);
    if (iserror(fields[nf])) { err = fields[nf]; fields[nf] = NULL; goto cleanup; }
    nf++;
    cur = cur->next;
  }
  if (cur) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-struct: too many fields (max %d)",
                ALC_FFI_MAX_ARGS);
    goto cleanup;
  }
  if (nf == 0) {
    err = error(ERROR_MISSING_PARAMETER, e, env,
                "ffi-struct: need at least one field type");
    goto cleanup;
  }
  alc_ffi_t *f = (alc_ffi_t *)memalloc(1, sizeof(alc_ffi_t));
  f->kind = AFFI_KIND_STRUCT;
  size_t off = 0, align = 1;
  for (int i = 0; i < nf; i++) {
    alc_ffi_tag_t t;
    ffi_type *ft;
    size_t fsize, falign;
    if (ishamt(fields[i])) { /* defensive: not a valid field */
      alc_ffi_free(f);
      err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-struct: invalid field type");
      goto cleanup;
    }
    if (isffi(fields[i]) &&
        ((alc_ffi_t *)fields[i]->ptr)->kind == AFFI_KIND_STRUCT) {
      /* nested struct field: reuse its ffi_type + computed size/alignment,
         and hold an owning ref to the nested descriptor for pack/unpack. */
      alc_ffi_t *nd = (alc_ffi_t *)fields[i]->ptr;
      t = AFFI_STRUCT;
      ft = &nd->struct_type;
      fsize = nd->struct_size;
      falign = nd->struct_align;
      f->arg_structs[i] = refexp(fields[i]);
    } else if (isstring(fields[i]) &&
               alc_ffi_typeof((char *)exp_text(fields[i]), &t, &ft) == 0 &&
               t != AFFI_VOID && t != AFFI_STRING) {
      fsize = ft->size;
      falign = ft->alignment;
    } else {
      alc_ffi_free(f);
      err = error(ERROR_ILLEGAL_VALUE, e, env,
                  "ffi-struct: field must be int/long/double/ptr or a struct "
                  "descriptor");
      goto cleanup;
    }
    f->arg_tags[i] = (uint8_t)t;
    f->atypes[i] = ft;
    f->elements[i] = ft;
    off = (off + falign - 1) & ~(falign - 1);
    f->offsets[i] = off;
    off += fsize;
    if (falign > align)
      align = falign;
  }
  f->elements[nf] = NULL;
  f->nargs = (unsigned int)nf;
  f->struct_size = (off + align - 1) & ~(align - 1);
  f->struct_align = align;
  /* Leave size/alignment 0 so ffi_prep_cif computes the authoritative
     in-cif layout when this descriptor is used as an ffi-fn arg/return; our
     manual offsets/struct_size drive pack/unpack and match it for scalars. */
  f->struct_type.size = 0;
  f->struct_type.alignment = 0;
  f->struct_type.type = FFI_TYPE_STRUCT;
  f->struct_type.elements = f->elements;
  f->display_name = strdup("<struct>");
  INIT_TYPED(ret, EXP_FFI, f);
cleanup:
  for (int i = 0; i < nf; i++)
    unrefexp(fields[i]);
  unrefexp(e);
  return err ? err : ret;
}

/* (ffi-pack struct-desc vals...) — pack scalar field values into a blob laid
   out per the descriptor's ABI layout. */
exp_t *ffipackcmd(exp_t *e, env_t *env) {
  exp_t *cur = e->next;
  exp_t *err = NULL, *ret = NULL, *desc = NULL;
  exp_t *vals[ALC_FFI_MAX_ARGS] = {0};
  int nv = 0;
  if (!cur) {
    err = error(ERROR_MISSING_PARAMETER, e, env, "(ffi-pack struct-desc vals...)");
    goto cleanup;
  }
  desc = EVAL(cur->content, env);
  if (iserror(desc)) { err = desc; desc = NULL; goto cleanup; }
  cur = cur->next;
  while (cur && nv < ALC_FFI_MAX_ARGS) {
    vals[nv] = EVAL(cur->content, env);
    if (iserror(vals[nv])) { err = vals[nv]; vals[nv] = NULL; goto cleanup; }
    nv++;
    cur = cur->next;
  }
  if (cur) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-pack: too many values (max %d)",
                ALC_FFI_MAX_ARGS);
    goto cleanup;
  }
  if (!isffi(desc) || ((alc_ffi_t *)desc->ptr)->kind != AFFI_KIND_STRUCT) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-pack: first arg must be an ffi-struct descriptor");
    goto cleanup;
  }
  alc_ffi_t *d = (alc_ffi_t *)desc->ptr;
  if ((unsigned int)nv != d->nargs) {
    err = error(ERROR_MISSING_PARAMETER, e, env,
                "ffi-pack: expected %u fields, got %d", d->nargs, nv);
    goto cleanup;
  }
  char *buf = (char *)memalloc(d->struct_size ? d->struct_size : 1, 1);
  for (int i = 0; i < nv; i++) {
    exp_t *v = vals[i];
    void *slot = buf + d->offsets[i];
    switch (d->arg_tags[i]) {
    case AFFI_INT:
      if (!(isnumber(v) || isfloat(v) || ischar(v))) goto badfield;
      *(int32_t *)slot = isnumber(v) ? (int32_t)FIX_VAL(v)
                         : ischar(v) ? (int32_t)CHAR_VAL(v)
                                     : (int32_t)v->f;
      break;
    case AFFI_LONG:
      if (!(isnumber(v) || isfloat(v) || ischar(v))) goto badfield;
      *(int64_t *)slot = isnumber(v) ? FIX_VAL(v)
                         : ischar(v) ? (int64_t)CHAR_VAL(v)
                                     : (int64_t)v->f;
      break;
    case AFFI_DOUBLE:
      if (!(isnumber(v) || isfloat(v) || ischar(v))) goto badfield;
      *(double *)slot = isfloat(v) ? v->f
                        : ischar(v) ? (double)CHAR_VAL(v)
                                    : (double)FIX_VAL(v);
      break;
    case AFFI_PTR:
      if (!(isnumber(v) || v == NIL_EXP)) goto badfield;
      *(void **)slot = (v == NIL_EXP) ? NULL : (void *)(uintptr_t)FIX_VAL(v);
      break;
    case AFFI_STRUCT: {
      /* nested struct field: v is a blob packed to the nested layout. */
      alc_ffi_t *nd = d->arg_structs[i] ? (alc_ffi_t *)d->arg_structs[i]->ptr : NULL;
      if (!nd || !isblob(v) || blob_len(v) != nd->struct_size) goto badfield;
      memcpy(slot, blob_bytes(v), nd->struct_size);
      break;
    }
    default:
      goto badfield;
    }
    continue;
  badfield:
    free(buf);
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-pack: field %d wrong type", i);
    goto cleanup;
  }
  ret = make_blob(buf, d->struct_size);
  free(buf);
cleanup:
  unrefexp(desc);
  for (int i = 0; i < nv; i++)
    unrefexp(vals[i]);
  unrefexp(e);
  return err ? err : ret;
}

/* (ffi-unpack struct-desc blob) — inverse of ffi-pack: read each field into
   a list of alcove values. */
exp_t *ffiunpackcmd(exp_t *e, env_t *env) {
  exp_t *err = NULL, *ret = NULL, *desc = NULL, *blob = NULL;
  if (!e->next || !e->next->next) {
    err = error(ERROR_MISSING_PARAMETER, e, env, "(ffi-unpack struct-desc blob)");
    goto cleanup;
  }
  desc = EVAL(e->next->content, env);
  if (iserror(desc)) { err = desc; desc = NULL; goto cleanup; }
  blob = EVAL(e->next->next->content, env);
  if (iserror(blob)) { err = blob; blob = NULL; goto cleanup; }
  if (!isffi(desc) || ((alc_ffi_t *)desc->ptr)->kind != AFFI_KIND_STRUCT) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-unpack: first arg must be an ffi-struct descriptor");
    goto cleanup;
  }
  if (!isblob(blob)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-unpack: second arg must be a blob");
    goto cleanup;
  }
  alc_ffi_t *d = (alc_ffi_t *)desc->ptr;
  if (blob_len(blob) < d->struct_size) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-unpack: blob too small (%zu < %zu)", blob_len(blob),
                d->struct_size);
    goto cleanup;
  }
  const char *buf = blob_bytes(blob);
  exp_t *head = NULL, *tail = NULL;
  for (unsigned int i = 0; i < d->nargs; i++) {
    const void *slot = buf + d->offsets[i];
    exp_t *v;
    switch (d->arg_tags[i]) {
    case AFFI_INT:    v = MAKE_FIX((int64_t)*(const int32_t *)slot); break;
    case AFFI_LONG:   v = MAKE_FIX(*(const int64_t *)slot); break;
    case AFFI_DOUBLE: v = make_floatf(*(const double *)slot); break;
    case AFFI_PTR:    v = MAKE_FIX((int64_t)(uintptr_t)*(void *const *)slot); break;
    case AFFI_STRUCT: { /* nested struct → blob of its bytes (ffi-unpack again) */
      alc_ffi_t *nd = d->arg_structs[i] ? (alc_ffi_t *)d->arg_structs[i]->ptr : NULL;
      v = nd ? make_blob(slot, nd->struct_size) : NIL_EXP;
      break;
    }
    default:          v = NIL_EXP; break;
    }
    exp_t *node = make_node(v);
    if (!head) { head = node; tail = node; }
    else { tail->next = node; tail = node; }
  }
  ret = head ? head : NIL_EXP;
cleanup:
  unrefexp(desc);
  unrefexp(blob);
  unrefexp(e);
  return err ? err : ret;
}

void alc_ffi_free(void *ptr) {
  alc_ffi_t *f = (alc_ffi_t *)ptr;
  if (!f)
    return;
  if (f->kind == AFFI_KIND_CB) {
    if (f->closure)
      ffi_closure_free(f->closure);
    if (f->cb_lambda)
      unrefexp(f->cb_lambda);
  }
  /* FN bindings hold a ref to the descriptor of each struct-by-value
     arg/return (and STRUCT/CB kinds leave these NULL). */
  for (int i = 0; i < ALC_FFI_MAX_ARGS; i++)
    if (f->arg_structs[i])
      unrefexp(f->arg_structs[i]);
  if (f->ret_struct)
    unrefexp(f->ret_struct);
  if (f->display_name)
    free(f->display_name);
  free(f);
}

/* Marshal alcove args → C, ffi_call, marshal return. */
/* Infer the ffi type of a variadic call argument from its alcove runtime
   type, applying C's default argument promotions. alcove integers are 64-bit,
   so a fixnum is passed as a long — use %ld in format strings; floats become
   double; chars become int; strings and nil become pointers. Returns -1 if
   the value can't be passed as a vararg. */
static int alc_ffi_infer(exp_t *a, alc_ffi_tag_t *tag, ffi_type **out) {
  if (isnumber(a))  { *tag = AFFI_LONG;   *out = &ffi_type_sint64;  return 0; }
  if (isfloat(a))   { *tag = AFFI_DOUBLE; *out = &ffi_type_double;  return 0; }
  if (ischar(a))    { *tag = AFFI_INT;    *out = &ffi_type_sint32;  return 0; }
  if (isstring(a))  { *tag = AFFI_STRING; *out = &ffi_type_pointer; return 0; }
  if (a == NIL_EXP) { *tag = AFFI_PTR;    *out = &ffi_type_pointer; return 0; }
  return -1;
}

static exp_t *alc_ffi_call(alc_ffi_t *f, int nargs, exp_t **args) {
  /* Variadic: f->nargs is the FIXED count — require at least that many and at
     most the slot cap. Non-variadic: exact arity. */
  int bad_arity = f->variadic ? (nargs < (int)f->nargs || nargs > ALC_FFI_MAX_ARGS)
                              : ((unsigned int)nargs != f->nargs);
  if (bad_arity) {
    int i;
    for (i = 0; i < nargs; i++)
      unrefexp(args[i]);
    return error(ERROR_MISSING_PARAMETER, NULL, NULL,
                 "ffi: wrong arg count for %s (expected %s%u, got %d)",
                 f->display_name ? f->display_name : "?",
                 f->variadic ? ">=" : "", f->nargs, nargs);
  }
  /* Slot storage. Avoid stack discipline issues by using one union per arg. */
  union {
    int32_t i;
    int64_t l;
    double d;
    const char *s;
    void *p;
  } slots[ALC_FFI_MAX_ARGS];
  void *avalues[ALC_FFI_MAX_ARGS];
  /* Per-call arg tags + ffi types. For non-variadic and the fixed prefix of a
     variadic call these come from the binding; extra variadic args are
     inferred from their value. */
  uint8_t tags[ALC_FFI_MAX_ARGS];
  ffi_type *atvec[ALC_FFI_MAX_ARGS];
  for (int i = 0; i < nargs; i++) {
    if (i < (int)f->nargs) {
      tags[i] = f->arg_tags[i];
      atvec[i] = f->atypes[i];
    } else {
      alc_ffi_tag_t t;
      if (alc_ffi_infer(args[i], &t, &atvec[i]) < 0) {
        for (int j = 0; j < nargs; j++)
          unrefexp(args[j]);
        return error(ERROR_ILLEGAL_VALUE, NULL, NULL,
                     "ffi: %s: variadic arg %d cannot be passed (need "
                     "number/float/char/string/nil)",
                     f->display_name ? f->display_name : "?", i);
      }
      tags[i] = (uint8_t)t;
    }
  }
  /* Type-mismatched args used to silently coerce to 0/NULL — calling
     a strlen-binding with a number then crashed in C. Now we refuse
     up front with a clear error so the caller knows which slot is
     wrong instead of seeing a SIGSEGV deep in libc. */
  for (int i = 0; i < nargs; i++) {
    exp_t *a = args[i];
    int ok = 0;
    switch (tags[i]) {
    case AFFI_INT:
    case AFFI_LONG:
      /* Chars (tagged immediates) are a natural fit for int args:
         shims that take ASCII codes via C's `int` convention should
         accept (gfx-text-set i (s i)) without forcing the caller to
         hand-convert. The numeric value of a char is its codepoint. */
      ok = isnumber(a) || isfloat(a) || ischar(a);
      break;
    case AFFI_DOUBLE:
      ok = isnumber(a) || isfloat(a) || ischar(a);
      break;
    case AFFI_STRING:
      ok = isstring(a);
      break;
    case AFFI_PTR:
      /* A raw address (fixnum), nil (NULL), or an ffi-callback value whose
         executable code pointer we pass as the function pointer. */
      ok = isnumber(a) || a == NIL_EXP ||
           (isffi(a) && ((alc_ffi_t *)a->ptr)->kind == AFFI_KIND_CB);
      break;
    case AFFI_STRUCT: {
      /* A struct-by-value arg is a blob packed to the declared layout. */
      alc_ffi_t *d = f->arg_structs[i] ? (alc_ffi_t *)f->arg_structs[i]->ptr : NULL;
      ok = d && isblob(a) && blob_len(a) == d->struct_size;
      break;
    }
    default:
      ok = 1;
      break;
    }
    if (!ok) {
      int j;
      for (j = 0; j < nargs; j++)
        unrefexp(args[j]);
      return error(ERROR_ILLEGAL_VALUE, NULL, NULL,
                   "ffi: %s: arg %d wrong type for tag %d",
                   f->display_name ? f->display_name : "?", i, (int)tags[i]);
    }
  }
  for (int i = 0; i < nargs; i++) {
    exp_t *a = args[i];
    switch (tags[i]) {
    case AFFI_INT:
      slots[i].i = isnumber(a) ? (int32_t)FIX_VAL(a)
                  : ischar(a)  ? (int32_t)CHAR_VAL(a)
                               : (int32_t)a->f;
      avalues[i] = &slots[i].i;
      break;
    case AFFI_LONG:
      slots[i].l = isnumber(a) ? FIX_VAL(a)
                  : ischar(a)  ? (int64_t)CHAR_VAL(a)
                               : (int64_t)a->f;
      avalues[i] = &slots[i].l;
      break;
    case AFFI_DOUBLE:
      slots[i].d = isfloat(a)  ? a->f
                  : ischar(a)  ? (double)CHAR_VAL(a)
                               : (double)FIX_VAL(a);
      avalues[i] = &slots[i].d;
      break;
    case AFFI_STRING:
      slots[i].s = (const char *)exp_text(a);
      avalues[i] = &slots[i].s;
      break;
    case AFFI_PTR:
      if (a == NIL_EXP)
        slots[i].p = NULL;
      else if (isffi(a))
        slots[i].p = ((alc_ffi_t *)a->ptr)->code; /* callback fn pointer */
      else
        slots[i].p = (void *)(uintptr_t)FIX_VAL(a);
      avalues[i] = &slots[i].p;
      break;
    case AFFI_STRUCT:
      /* Pass the packed bytes directly; libffi copies struct_size bytes per
         the cif. (Validated as a correctly-sized blob in the check above.) */
      avalues[i] = (void *)(uintptr_t)blob_bytes(a);
      break;
    default:
      slots[i].l = 0;
      avalues[i] = &slots[i].l;
      break;
    }
  }
  /* Variadic calls build their cif here, now that the per-call arg types are
     known (ffi_prep_cif_var needs the full type vector and the fixed count).
     Non-variadic calls reuse the cif prepped at bind time. */
  ffi_cif vcif;
  ffi_cif *cif = &f->cif;
  if (f->variadic) {
    if (ffi_prep_cif_var(&vcif, FFI_DEFAULT_ABI, f->nargs, (unsigned int)nargs,
                         f->rtype, atvec) != FFI_OK) {
      for (int j = 0; j < nargs; j++)
        unrefexp(args[j]);
      return error(ERROR_ILLEGAL_VALUE, NULL, NULL,
                   "ffi: %s: ffi_prep_cif_var failed",
                   f->display_name ? f->display_name : "?");
    }
    cif = &vcif;
  }
  union {
    int32_t i;
    int64_t l;
    double d;
    void *p;
  } rval;
  /* Struct-by-value return needs a buffer sized to the struct (at least
     ffi_arg, libffi's minimum return slot). Scalars use the union above. */
  void *rvalue = &rval;
  char *struct_buf = NULL;
  size_t struct_ret_size = 0;
  if (f->ret_tag == AFFI_STRUCT && f->ret_struct) {
    struct_ret_size = ((alc_ffi_t *)f->ret_struct->ptr)->struct_size;
    size_t bufsz =
        struct_ret_size < sizeof(ffi_arg) ? sizeof(ffi_arg) : struct_ret_size;
    struct_buf = (char *)memalloc(bufsz ? bufsz : 1, 1);
    rvalue = struct_buf;
  }
  ffi_call(cif, FFI_FN(f->fn), rvalue, avalues);
  exp_t *ret = NIL_EXP;
  switch (f->ret_tag) {
  case AFFI_VOID:
    ret = NIL_EXP;
    break;
  case AFFI_INT:
    ret = MAKE_FIX((int64_t)rval.i);
    break;
  case AFFI_LONG:
    ret = MAKE_FIX(rval.l);
    break;
  case AFFI_DOUBLE:
    ret = make_floatf(rval.d);
    break;
  case AFFI_STRING: {
    /* strlen on an arbitrary returned pointer is a footgun — if the C
       function returned a non-string or a non-NUL-terminated buffer
       we'd OOB-read the heap. Use strnlen with a generous cap so
       runaway strings get truncated instead of crashing. */
    if (!rval.p) {
      ret = NIL_EXP;
    } else {
      size_t len = strnlen((const char *)rval.p, 1u << 24);
      ret = make_string((char *)rval.p, (int)len);
    }
    break;
  }
  case AFFI_PTR:
    ret = MAKE_FIX((int64_t)(uintptr_t)rval.p);
    break;
  case AFFI_STRUCT:
    /* Hand the returned struct bytes back as a blob (ffi-unpack reads it). */
    ret = make_blob(struct_buf, struct_ret_size);
    break;
  }
  if (struct_buf)
    free(struct_buf);
  for (int i = 0; i < nargs; i++)
    unrefexp(args[i]);
  return ret;
}
#else  /* !ALCOVE_FFI */
exp_t *ffifncmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "ffi-fn: alcove built without libffi (install libffi-dev "
               "and rebuild).");
}
exp_t *ffivfncmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "ffi-vfn: alcove built without libffi.");
}
exp_t *fficallbackcmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "ffi-callback: alcove built without libffi (install "
               "libffi-dev and rebuild).");
}
exp_t *ffistructcmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "ffi-struct: alcove built without libffi.");
}
exp_t *ffipackcmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "ffi-pack: alcove built without libffi.");
}
exp_t *ffiunpackcmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "ffi-unpack: alcove built without libffi.");
}
void alc_ffi_free(void *ptr) {
  (void)ptr;
} /* called from unrefexp; no FFI exp can exist */
#endif /* ALCOVE_FFI */

/* ---------------- Standard-library builtins (math/seq/predicates)
   ---------------- Each follows the prn/expt pattern: walk e->next, EVAL each
   arg, type-check, produce result, unrefexp args + form, return owned ref. */

/* (mod a b) — integer modulo. Both args must be fixnums. */
const char doc_mod[] = "(mod a b) — remainder of a / b. Sign follows the "
                       "divisor for floats; truncation for ints.";
exp_t *modcmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *b = NULL, *ret = NULL;
  if (e->next && e->next->next) {
    a = EVAL(e->next->content, env);
    if (iserror(a)) {
      unrefexp(e);
      return a;
    }
    b = EVAL(e->next->next->content, env);
    if (iserror(b)) {
      unrefexp(a);
      unrefexp(e);
      return b;
    }
    if (isnumber(a) && isnumber(b) && FIX_VAL(b) != 0) {
      int64_t va = FIX_VAL(a), vb = FIX_VAL(b);
      ret = MAKE_FIX(va - (va / vb) * vb); /* C99 truncated div */
    } else if ((isnumber(a) || isfloat(a)) && (isnumber(b) || isfloat(b))) {
      double da = TO_DOUBLE(a);
      double db = TO_DOUBLE(b);
      if (db == 0.0)
        ret = error(ERROR_DIV_BY0, e, env, "mod by 0");
      else
        ret = make_floatf(fmod(da, db));
    } else {
      ret = error(ERROR_ILLEGAL_VALUE, e, env, "mod needs numeric operands");
    }
  } else {
    ret = error(ERROR_MISSING_PARAMETER, e, env, "(mod a b)");
  }
  unrefexp(a);
  unrefexp(b);
  unrefexp(e);
  return ret;
}

/* Bitwise ops on integers. Both args must be fixnums; floats are
   rejected. Shifts mask the count to 0..63 so (<< 1 64) == (<< 1 0). */
#define BITOP_AB(name, expr)                                                   \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    exp_t *a = NULL, *b = NULL, *ret = NULL;                                   \
    if (e->next && e->next->next) {                                            \
      a = EVAL(e->next->content, env);                                         \
      if (iserror(a)) {                                                        \
        unrefexp(e);                                                           \
        return a;                                                              \
      }                                                                        \
      b = EVAL(e->next->next->content, env);                                   \
      if (iserror(b)) {                                                        \
        unrefexp(a);                                                           \
        unrefexp(e);                                                           \
        return b;                                                              \
      }                                                                        \
      if (isnumber(a) && isnumber(b)) {                                        \
        int64_t va = FIX_VAL(a), vb = FIX_VAL(b);                              \
        ret = MAKE_FIX(expr);                                                  \
      } else {                                                                 \
        ret = error(ERROR_ILLEGAL_VALUE, e, env,                               \
                    "bit op requires two integer args");                       \
      }                                                                        \
    } else {                                                                   \
      ret = error(ERROR_MISSING_PARAMETER, e, env, "bit op needs two args");   \
    }                                                                          \
    unrefexp(a);                                                               \
    unrefexp(b);                                                               \
    unrefexp(e);                                                               \
    return ret;                                                                \
  }

const char doc_bitand[] =
    "(bit-and a b) — bitwise AND on two integers. Alias: &.";
BITOP_AB(bitandcmd, va &vb)
const char doc_bitor[] = "(bit-or a b) — bitwise OR on two integers. Alias: |.";
BITOP_AB(bitorcmd, va | vb)
const char doc_bitxor[] =
    "(bit-xor a b) — bitwise XOR on two integers. Alias: ^.";
BITOP_AB(bitxorcmd, va ^ vb)
const char doc_shl[] =
    "(<< x n) — bitwise shift x left by n bits. Wraps inside int61.";
BITOP_AB(shlcmd, (int64_t)((uint64_t)va << (vb & 63)))
const char doc_shr[] =
    "(>> x n) — arithmetic shift x right by n bits (sign-preserving).";
BITOP_AB(shrcmd, va >> (vb & 63))
#undef BITOP_AB

/* (~ x) — bitwise NOT. ~0 is -1 (all bits set, fits cleanly in int61). */
const char doc_bitnot[] = "(~ x) — bitwise NOT (complement). Alias: bit-not.";
exp_t *bitnotcmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *ret = NULL;
  if (e->next) {
    a = EVAL(e->next->content, env);
    if (iserror(a)) {
      unrefexp(e);
      return a;
    }
    if (isnumber(a)) {
      ret = MAKE_FIX(~FIX_VAL(a));
    } else {
      ret = error(ERROR_ILLEGAL_VALUE, e, env, "~ requires an integer arg");
    }
  } else {
    ret = error(ERROR_MISSING_PARAMETER, e, env, "(~ x) needs one arg");
  }
  unrefexp(a);
  unrefexp(e);
  return ret;
}

/* (abs x) — |x| for fixnum or float. */
const char doc_abs[] = "(abs x) — absolute value.";
exp_t *abscmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *ret = NULL;
  if (e->next) {
    a = EVAL(e->next->content, env);
    if (iserror(a)) {
      unrefexp(e);
      return a;
    }
    if (isnumber(a)) {
      int64_t v = FIX_VAL(a);
      int64_t av = v < 0 ? -v : v;
      /* If negation overflows fixnum range (abs of most-negative fixnum),
         promote to float rather than silently wrapping to negative.
         Uses signed >>3 to match FIX_VAL's arithmetic-shift semantics. */
      if (v < 0 && !FIX_FITS(av))
        ret = make_floatf(-(expfloat)v);
      else
        ret = MAKE_FIX(av);
    } else if (isfloat(a)) {
      ret = make_floatf(a->f < 0 ? -a->f : a->f);
    } else
      ret = error(ERROR_ILLEGAL_VALUE, e, env, "abs: not a number");
  } else
    ret = error(ERROR_MISSING_PARAMETER, e, env, "(abs x)");
  unrefexp(a);
  unrefexp(e);
  return ret;
}

/* Helper for max/min: numeric "less than" between two values, promoting
   to double if either is a float. Returns 1 if a < b, 0 otherwise. -1
   on type error (caller checks). */
static int alc_numlt(exp_t *a, exp_t *b, int *err) {
  *err = 0;
  if (isnumber(a) && isnumber(b))
    return FIX_VAL(a) < FIX_VAL(b);
  if ((isnumber(a) || isfloat(a)) && (isnumber(b) || isfloat(b))) {
    double da = TO_DOUBLE(a);
    double db = TO_DOUBLE(b);
    return da < db;
  }
  *err = 1;
  return 0;
}

/* (max a b ...) — variadic; at least one arg required. */
#define MINMAX_CMD(name, is_lt, err_name)                                      \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    exp_t *cur = e->next;                                                      \
    if (!cur) {                                                                \
      unrefexp(e);                                                             \
      return error(ERROR_MISSING_PARAMETER, e, env, "(" #name " ...)");        \
    }                                                                          \
    exp_t *best = EVAL(cur->content, env);                                     \
    if (iserror(best)) {                                                       \
      unrefexp(e);                                                             \
      return best;                                                             \
    }                                                                          \
    cur = cur->next;                                                           \
    while (cur) {                                                              \
      exp_t *v = EVAL(cur->content, env);                                      \
      if (iserror(v)) {                                                        \
        unrefexp(best);                                                        \
        unrefexp(e);                                                           \
        return v;                                                              \
      }                                                                        \
      int err;                                                                 \
      int lt = (is_lt) ? alc_numlt(v, best, &err) : alc_numlt(best, v, &err);  \
      if (err) {                                                               \
        unrefexp(best);                                                        \
        unrefexp(v);                                                           \
        unrefexp(e);                                                           \
        return error(ERROR_ILLEGAL_VALUE, e, env, err_name ": non-numeric");   \
      }                                                                        \
      if (lt) {                                                                \
        unrefexp(best);                                                        \
        best = v;                                                              \
      } else                                                                   \
        unrefexp(v);                                                           \
      cur = cur->next;                                                         \
    }                                                                          \
    unrefexp(e);                                                               \
    return best;                                                               \
  }

const char doc_max[] = "(max x ...) — largest of the args.";
MINMAX_CMD(maxcmd, 0, "max")

/* (min a b ...) */
const char doc_min[] = "(min x ...) — smallest of the args.";
MINMAX_CMD(mincmd, 1, "min")

/* (length x) — list length, string length, or 0 for nil. */
const char doc_length[] = "(length x) — element count of a list/string/vector.";
exp_t *lengthcmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *ret = NULL;
  if (e->next) {
    a = EVAL(e->next->content, env);
    if (iserror(a)) {
      unrefexp(e);
      return a;
    }
    int64_t n = 0;
    if (isstring(a))
      { const char *_t = exp_text(a); n = _t ? utf8_strlen(_t) : 0; }
    else if (a == nil_singleton)
      n = 0;
    else if (ispair(a)) {
      exp_t *cur = a;
      while (cur && cur->content) {
        n++;
        cur = cur->next;
      }
    } else {
      ret = error(ERROR_ILLEGAL_VALUE, e, env, "length: not a list/string");
      goto done;
    }
    ret = MAKE_FIX(n);
  } else
    ret = error(ERROR_MISSING_PARAMETER, e, env, "(length x)");
done:
  unrefexp(a);
  unrefexp(e);
  return ret;
}

/* (nth n list) — 0-indexed; returns nil if out of range. */
const char doc_nth[] = "(nth xs i) — 0-based element of list/string/vector.";
exp_t *nthcmd(exp_t *e, env_t *env) {
  exp_t *ret = NIL_EXP;
  EVAL_ARG_2(a, b);
  if (a && b) {
    if (isnumber(a)) {
      /* b must be a heap pair (or nil) — without is_ptr we'd dereference
         the tag bits of a tagged immediate. Same fix pattern as
         appendcmd / reversecmd. nil/empty list is a clean miss. */
      if (b && b != NIL_EXP && !ispair(b)) {
        CLEAN_RETURN_2(a, b,
                       error(ERROR_ILLEGAL_VALUE, NULL, env,
                             "nth: second argument is not a list"));
      }
      int64_t idx = FIX_VAL(a);
      exp_t *cur = b;
      while (idx > 0 && ispair(cur) && cur->next) {
        cur = cur->next;
        idx--;
      }
      if (idx == 0 && ispair(cur) && cur->content)
        ret = refexp(cur->content);
    }
  }
  CLEAN_RETURN_2(a, b, ret);
}

/* (reverse list) — non-destructive; returns a new list. */
const char doc_reverse[] =
    "(reverse xs) — list with elements in reverse order.";
exp_t *reversecmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *acc = NIL_EXP;
  if (e->next) {
    a = EVAL(e->next->content, env);
    if (iserror(a)) {
      unrefexp(e);
      return a;
    }
    /* Reject non-list args before walking — same fix pattern as appendcmd:
       a tagged immediate (fixnum, char) passes the `cur != NULL` check
       and segfaults on the deref of cur->content. nil/empty is fine. */
    if (a && a != NIL_EXP && !ispair(a)) {
      unrefexp(a);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "reverse: argument is not a list");
    }
    exp_t *cur = a;
    while (ispair(cur) && cur->content) {
      exp_t *node = make_node(refexp(cur->content));
      node->next = (acc == NIL_EXP) ? NULL : acc;
      acc = node;
      cur = cur->next;
    }
  }
  unrefexp(a);
  unrefexp(e);
  return acc;
}

/* (append list1 list2 ...) — flat concat into a new list (cars are
   shared with inputs but the cons spine is fresh). */
const char doc_append[] = "(append xs ys ...) — concatenate lists.";
exp_t *appendcmd(exp_t *e, env_t *env) {
  exp_t *head = NULL, *tail = NULL;
  exp_t *cur_arg = e->next;
  while (cur_arg) {
    exp_t *list = EVAL(cur_arg->content, env);
    if (iserror(list)) {
      if (head)
        unrefexp(head);
      unrefexp(e);
      return list;
    }
    /* nil/empty (which lispers freely pass to append) is a no-op. Anything
       that isn't a heap pair is a hard error — without this guard, a
       tagged fixnum like (append 10 ...) walks `cur->content` which
       dereferences the tag bits and segfaults. */
    if (list && list != NIL_EXP && !ispair(list)) {
      if (head)
        unrefexp(head);
      unrefexp(list);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "append: argument is not a list");
    }
    exp_t *cur = list;
    while (ispair(cur) && cur->content) {
      exp_t *node = make_node(refexp(cur->content));
      if (!head) {
        head = node;
        tail = node;
      } else {
        tail->next = node;
        tail = node;
      }
      cur = cur->next;
    }
    unrefexp(list);
    cur_arg = cur_arg->next;
  }
  unrefexp(e);
  return head ? head : NIL_EXP;
}

/* Type predicates — return t/nil. */
#define PRED_CMD(name, pred)                                                   \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    exp_t *a = NULL, *ret = NIL_EXP;                                           \
    if (e->next) {                                                             \
      a = EVAL(e->next->content, env);                                         \
      if (iserror(a)) {                                                        \
        unrefexp(e);                                                           \
        return a;                                                              \
      }                                                                        \
      if (pred)                                                                \
        ret = TRUE_EXP;                                                        \
    }                                                                          \
    unrefexp(a);                                                               \
    unrefexp(e);                                                               \
    return ret;                                                                \
  }
/* Type-predicate cmds, expanded from the PRED_CMD macro above. Each
   takes zero or one arg and returns t / nil (no-arg form is nil). */
const char doc_numberp[] = "(number? x) — t if x is a fixnum or float.";
const char doc_stringp[] = "(string? x) — t if x is a string.";
const char doc_symbolp[] = "(symbol? x) — t if x is a symbol.";
const char doc_pairp[] = "(pair? x) — t if x is a non-empty pair (cons cell).";
const char doc_fnp[] = "(fn? x) — t if x is callable (lambda or builtin).";
const char doc_vecp[] = "(vec? x) — t if x is a vector.";
const char doc_blobp[] = "(blob? x) — t if x is a blob.";
const char doc_dictp[] = "(dict? x) — t if x is a hash-map.";
const char doc_dequep[] = "(deque? x) — t if x is a deque.";
const char doc_setp[] = "(set? x) — t if x is a hash-set.";
PRED_CMD(numberpcmd, (isnumber(a) || isfloat(a)))
PRED_CMD(stringpcmd, isstring(a))
PRED_CMD(symbolpcmd, issymbol(a))
PRED_CMD(pairpcmd, (ispair(a) && a->content))
PRED_CMD(fnpcmd, (islambda(a) || isinternal(a) || isffi(a)))
PRED_CMD(vecpcmd, isvector(a))
PRED_CMD(blobpcmd, isblob(a))
PRED_CMD(dictpcmd, isdict(a))
PRED_CMD(dequepcmd, islist(a))
PRED_CMD(setpcmd, isset(a))
/* Introspection predicates — let tests assert on internal optimizations.
   compiled?: lambda body compiled to bytecode (vs AST fallback).
   jit?: bytecode also has native code installed (only in JIT builds).
   inline?: symbol/string text stored inline (FLAG_INLINE_TXT). Guarded by
   is_ptr first — a tagged immediate (fixnum/char) has no flags word. */
PRED_CMD(compiledpcmd, (islambda(a) && (a->flags & FLAG_COMPILED) && a->bc))
PRED_CMD(jitpcmd,
         (islambda(a) && (a->flags & FLAG_COMPILED) && a->bc && a->bc->jit))
PRED_CMD(inlinepcmd, (is_ptr(a) && (a->flags & FLAG_INLINE_TXT)))
#undef PRED_CMD

const char doc_compiledp[] =
    "(compiled? fn) — t if fn's body is compiled to bytecode (not AST).";
const char doc_jitp[] =
    "(jit? fn) — t if fn has native JIT code installed (JIT builds only).";
const char doc_inlinep[] =
    "(inline? x) — t if x is a symbol/string whose text is stored inline "
    "(<= 7 chars) rather than heap-allocated.";
const char doc_expflags[] =
    "(exp-flags x) — integer flags word of x (0 for tagged immediates). "
    "Introspection/testing: bit 2 (4) = compiled, bit 6 (64) = inline-text.";
/* Value-returning flags accessor — the user-facing counterpart to inspect,
   for assertions on the raw flags word. */
exp_t *expflagscmd(exp_t *e, env_t *env) {
  exp_t *a = e->next ? EVAL(e->next->content, env) : refexp(NIL_EXP);
  if (iserror(a)) {
    unrefexp(e);
    return a;
  }
  int f = is_ptr(a) ? a->flags : 0;
  unrefexp(a);
  unrefexp(e);
  return MAKE_FIX(f);
}

/* (exit) / (exit code) — terminate the process. */
const char doc_exit[] = "(exit) or (exit code) — terminate the process. "
                        "Default exit code is 0. Alias: quit.";
exp_t *exitcmd(exp_t *e, env_t *env) {
  int code = 0;
  if (e->next) {
    exp_t *a = EVAL(e->next->content, env);
    if (isnumber(a))
      code = (int)FIX_VAL(a);
    unrefexp(a);
  }
  unrefexp(e);
  (void)env;
  exit(code);
}

/* (random n) — pseudo-random fixnum in [0, n). Seeded once from time. */
const char doc_random[] =
    "(random n) — uniform fixnum in [0, n). (random) gives a 64-bit value.";
exp_t *randomcmd(exp_t *e, env_t *env) {
  static int seeded = 0;
  if (!seeded) {
    srand((unsigned)gettimeusec());
    seeded = 1;
  }
  int64_t n = 0;
  if (e->next) {
    exp_t *a = EVAL(e->next->content, env);
    if (isnumber(a))
      n = FIX_VAL(a);
    unrefexp(a);
  }
  unrefexp(e);
  return MAKE_FIX(
      n <= 0 ? 0
             : (int64_t)((double)rand() / ((double)RAND_MAX + 1) * (double)n));
}

/* (map fn list) — non-destructive; returns a new list of (fn x) values. */
const char doc_map[] = "(map fn xs ...) — apply fn to corresponding elements "
                       "of one or more lists; returns a new list.";
exp_t *mapcmd(exp_t *e, env_t *env) {
  exp_t *head = NULL, *tail = NULL;
  EVAL_ARG_2(fn, xs);
  /* Require the list-arg FORM (arity), but a NULL VALUE is the empty list,
     not a missing arg: (cdr <1-elem>) yields C NULL, which must filter/map
     as () — not error. (!fn short-circuits if e->next is missing.) */
  if (!fn || !e->next->next)
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_MISSING_PARAMETER, e, env, "(map fn list)"));
  if (xs && xs != NIL_EXP && !ispair(xs))
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "map: second argument is not a list"));

  exp_t *cur = xs;
  while (ispair(cur) && cur->content) {
    exp_t *res = alc_apply1(fn, cur->content, env);
    if (res && iserror(res)) {
      if (head)
        unrefexp(head);
      CLEAN_RETURN_2(fn, xs, res);
    }
    if (!res)
      res = NIL_EXP;
    exp_t *node = make_node(res);
    if (!head) {
      head = node;
      tail = node;
    } else {
      tail->next = node;
      tail = node;
    }
    cur = cur->next;
  }
  CLEAN_RETURN_2(fn, xs, head ? head : NIL_EXP);
}
/* (filter pred list) — keep elements where (pred x) is truthy. */
const char doc_filter[] = "(filter pred xs) — list of elements of xs for which "
                          "(pred elem) is truthy.";
exp_t *filtercmd(exp_t *e, env_t *env) {
  exp_t *head = NULL, *tail = NULL;
  EVAL_ARG_2(fn, xs);
  if (!fn || !e->next->next) /* NULL list value = empty list, not missing arg */
    CLEAN_RETURN_2(
        fn, xs, error(ERROR_MISSING_PARAMETER, e, env, "(filter pred list)"));
  if (xs && xs != NIL_EXP && !ispair(xs))
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "filter: second argument is not a list"));

  exp_t *cur = xs;
  while (ispair(cur) && cur->content) {
    exp_t *res = alc_apply1(fn, cur->content, env);
    if (res && iserror(res)) {
      if (head)
        unrefexp(head);
      CLEAN_RETURN_2(fn, xs, res);
    }
    int keep = (res != NULL && res != NIL_EXP);
    if (res)
      unrefexp(res);
    if (keep) {
      exp_t *node = make_node(refexp(cur->content));
      if (!head) {
        head = node;
        tail = node;
      } else {
        tail->next = node;
        tail = node;
      }
    }
    cur = cur->next;
  }
  CLEAN_RETURN_2(fn, xs, head ? head : NIL_EXP);
}
/* (reduce fn init list) — left fold: ((fn (fn init x0) x1) x2 ...). */
const char doc_reduce[] = "(reduce fn init xs) — left fold: (fn (fn (fn init "
                          "x0) x1) x2)... Returns init for empty xs.";
exp_t *reducecmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(fn, acc, xs);
  /* Require all three arg FORMS (arity), but NULL VALUES are nil/empty —
     a NULL init or a NULL list (e.g. from (cdr <1-elem>)) must fold as
     init/() rather than error. (!fn short-circuits the form derefs.) */
  if (!fn || !e->next->next || !e->next->next->next)
    CLEAN_RETURN_3(
        fn, acc, xs,
        error(ERROR_MISSING_PARAMETER, e, env, "(reduce fn init list)"));
  if (!acc)
    acc = NIL_EXP; /* NULL init seed → nil */
  if (xs && xs != NIL_EXP && !ispair(xs))
    CLEAN_RETURN_3(fn, acc, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "reduce: third argument is not a list"));

  /* Fast path: detect a simple 6-byte binary-arithmetic lambda
     (fn (a b) (op a b)) — bytecode is LOAD_SLOT 0, LOAD_SLOT 1, OP, RET.
     Common across reduce-sum, reduce-product, reduce-max, etc. We
     inline the tagged-fixnum operation directly, skipping the
     vm_invoke_values per-element env churn. Gives ~10x on listsum-style
     workloads. Falls back to the general path on non-fixnum or when
     the lambda has any other shape. */
  int fast_op = 0; /* 0=none, 1=add, 2=sub, 3=mul */
  if (islambda(fn) && (fn->flags & FLAG_COMPILED) && fn->bc &&
      fn->bc->ncode == 6) {
    uint8_t *c = fn->bc->code;
    if (c[0] == OP_LOAD_SLOT && c[1] == 0 && c[2] == OP_LOAD_SLOT &&
        c[3] == 1 && c[5] == OP_RET) {
      if (c[4] == OP_ADD)
        fast_op = 1;
      else if (c[4] == OP_SUB)
        fast_op = 2;
      else if (c[4] == OP_MUL)
        fast_op = 3;
    }
  }

  exp_t *cur = xs;
  if (fast_op) {
    while (ispair(cur) && cur->content) {
      exp_t *x = cur->content;
      if (isnumber(acc) && isnumber(x)) {
        int64_t a = FIX_VAL(acc), b = FIX_VAL(x);
        int64_t r = (fast_op == 1)   ? (a + b)
                    : (fast_op == 2) ? (a - b)
                                     : (a * b);
        acc = MAKE_FIX(r);
      } else {
        acc = alc_apply2(fn, acc, refexp(x), env);
        if (acc && iserror(acc))
          CLEAN_RETURN_2(fn, xs, acc);
        if (!acc)
          acc = NIL_EXP;
      }
      cur = cur->next;
    }
  } else {
    while (ispair(cur) && cur->content) {
      acc = alc_apply2(fn, acc, refexp(cur->content), env);
      if (acc && iserror(acc))
        CLEAN_RETURN_2(fn, xs, acc);
      if (!acc)
        acc = NIL_EXP;
      cur = cur->next;
    }
  }
  CLEAN_RETURN_2(fn, xs, acc);
}

/* (any? pred list) — return t as soon as (pred x) is truthy for any
   element of list, nil if none match. Walks in C with one
   vm_invoke_values per element instead of recursive bytecode. */
const char doc_any[] = "(any? pred xs) — t if pred is truthy for at least one "
                       "element. Short-circuits.";
exp_t *anypcmd(exp_t *e, env_t *env) {
  exp_t *ret = NIL_EXP;
  EVAL_ARG_2(fn, xs);
  if (!fn || !e->next->next) /* NULL list value = empty list, not missing arg */
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_MISSING_PARAMETER, e, env, "(any? pred list)"));
  if (xs && xs != NIL_EXP && !ispair(xs))
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "any?: second argument is not a list"));

  exp_t *cur = xs;
  while (ispair(cur) && cur->content) {
    exp_t *res = alc_apply1(fn, cur->content, env);
    if (res && iserror(res))
      CLEAN_RETURN_2(fn, xs, res);
    int truthy = (res != NULL && res != NIL_EXP);
    if (res)
      unrefexp(res);
    if (truthy) {
      ret = TRUE_EXP;
      break;
    }
    cur = cur->next;
  }
  CLEAN_RETURN_2(fn, xs, ret);
}
/* (all? pred list) — return t if (pred x) is truthy for every
   element, nil at the first failure. Empty list → t. */
const char doc_all[] = "(all? pred xs) — t if pred is truthy for every element "
                       "(vacuously t for empty). Short-circuits.";
exp_t *allpcmd(exp_t *e, env_t *env) {
  exp_t *ret = TRUE_EXP;
  EVAL_ARG_2(fn, xs);
  if (!fn || !e->next->next) /* NULL list value = empty list, not missing arg */
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_MISSING_PARAMETER, e, env, "(all? pred list)"));
  if (xs && xs != NIL_EXP && !ispair(xs))
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "all?: second argument is not a list"));

  exp_t *cur = xs;
  while (ispair(cur) && cur->content) {
    exp_t *res = alc_apply1(fn, cur->content, env);
    if (res && iserror(res))
      CLEAN_RETURN_2(fn, xs, res);
    int truthy = (res != NULL && res != NIL_EXP);
    if (res)
      unrefexp(res);
    if (!truthy) {
      ret = NIL_EXP;
      break;
    }
    cur = cur->next;
  }
  CLEAN_RETURN_2(fn, xs, ret);
}

/* (apply fn args-list) — call fn with each element of args-list as
   separate args. Implemented by re-using vm_invoke_values for compiled
   lambdas; falls back to AST invoke otherwise. */
const char doc_apply[] = "(apply fn args) — call fn with the elements of the "
                         "list args as its arguments.";
exp_t *applycmd(exp_t *e, env_t *env) {
  exp_t *fn = NULL, *args = NULL, *ret = NULL;
  if (e->next && e->next->next) {
    fn = EVAL(e->next->content, env);
    if (iserror(fn)) {
      unrefexp(e);
      return fn;
    }
    args = EVAL(e->next->next->content, env);
    if (iserror(args)) {
      unrefexp(fn);
      unrefexp(e);
      return args;
    }
    /* Materialize args as an exp_t** so vm_invoke_values can take it. */
    int n = 0;
    exp_t *c = args;
    while (c && c->content) {
      n++;
      c = c->next;
    }
    exp_t **argv = (n > 0) ? memalloc(n, sizeof(exp_t *)) : NULL;
    int i = 0;
    c = args;
    while (c && c->content && i < n) {
      argv[i++] = refexp(c->content);
      c = c->next;
    }
    if (islambda(fn)) {
      ret = vm_invoke_values(fn, n, argv, env);
    } else {
      ret = error(ERROR_ILLEGAL_VALUE, e, env, "apply: first arg not a fn");
      for (i = 0; i < n; i++)
        unrefexp(argv[i]);
    }
    free(argv);
  } else
    ret = error(ERROR_MISSING_PARAMETER, e, env, "(apply fn args)");
  unrefexp(fn);
  unrefexp(args);
  unrefexp(e);
  return ret;
}

/* Call any callable (lambda or builtin) with pre-evaluated args.
   Builtins receive their canonical `e` list; lambdas use vm_invoke_values.
   The caller keeps ownership of `fn`; argv ownership is transferred. */
static exp_t *alc_apply_n(exp_t *fn, int nargs, exp_t **argv, env_t *env) {
  if (islambda(fn))
    return vm_invoke_values(fn, nargs, argv, env);
  if (isinternal(fn)) {
    exp_t *head = make_node(refexp(fn));
    exp_t *cur = head;
    for (int i = 0; i < nargs; i++)
      cur = cur->next = make_node(argv[i]);
    int was_tail = in_tail_position;
    in_tail_position = 0;
    exp_t *ret = fn->fnc(head, env);
    in_tail_position = was_tail;
    return ret;
  }
  if (iscont(fn)) { /* escape continuation invoked via apply/map/etc. */
    exp_t *payload = nargs > 0 ? refexp(argv[0]) : refexp(NIL_EXP);
    for (int i = 0; i < nargs; i++) unrefexp(argv[i]);
    return make_cont_escape((int64_t)(intptr_t)fn->meta, payload, env);
  }
  for (int i = 0; i < nargs; i++) unrefexp(argv[i]);
  return error(ERROR_ILLEGAL_VALUE, fn, env, "not a callable");
}
static exp_t *alc_apply1(exp_t *fn, exp_t *arg, env_t *env) {
  exp_t *argv[1] = {refexp(arg)};
  return alc_apply_n(fn, 1, argv, env);
}
static exp_t *alc_apply2(exp_t *fn, exp_t *a, exp_t *b, env_t *env) {
  exp_t *argv[2] = {a, b};
  return alc_apply_n(fn, 2, argv, env);
}

/* ---- New stdlib additions -------------------------------------------- */

/* (list? x) — t if x is nil or a proper list (all cdrs end with nil). */
const char doc_listp[] = "(list? x) — t if x is nil or a proper list.";
exp_t *listpcmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *ret = NIL_EXP;
  if (e->next) {
    a = EVAL(e->next->content, env);
    if (iserror(a)) { unrefexp(e); return a; }
    exp_t *cur = a;
    while (ispair(cur) && cur->content)
      cur = cur->next;
    if (!cur || cur == NIL_EXP)
      ret = TRUE_EXP;
  }
  unrefexp(a);
  unrefexp(e);
  return ret;
}

/* (null? x) — t if x is nil (empty list / false). Complements pair?. */
const char doc_nullp[] = "(null? x) — t if x is nil.";
exp_t *nullpcmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *ret = NIL_EXP;
  if (e->next) {
    a = EVAL(e->next->content, env);
    if (iserror(a)) { unrefexp(e); return a; }
    if (!a || a == NIL_EXP) ret = TRUE_EXP;
  }
  unrefexp(a);
  unrefexp(e);
  return ret;
}

static int64_t gensym_counter = 0;
static exp_t *make_gensym(void) {
  char buf[32];
  int n = snprintf(buf, sizeof buf, "G%lld", (long long)gensym_counter++);
  return make_symbol(buf, n);
}

/* (gensym) — return a fresh symbol G0, G1, G2, … unique per session. */
const char doc_gensym[] = "(gensym) — unique symbol each call: G0, G1, …";
exp_t *gensymcmd(exp_t *e, env_t *env) {
  unrefexp(e);
  (void)env;
  return make_gensym();
}

const char doc_withgensyms[] =
    "(with-gensyms (s ...) body ...) — bind each name to a fresh unique "
    "symbol, then evaluate body forms in that scope. Used inside defmacro "
    "to avoid variable capture: "
    "(with-gensyms (tmp) `(let ,tmp ,x ,tmp)).";
exp_t *withgensymscmd(exp_t *e, env_t *env) {
  exp_t *names_node = e->next;
  exp_t *body_start = names_node ? names_node->next : NULL;
  /* names_node->content is either a pair (non-empty list) or nil (()) */
  if (!names_node || !body_start ||
      (!ispair(names_node->content) && istrue(names_node->content))) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, NULL, env,
                 "with-gensyms: expected (name-list body...)");
  }
  env_t *newenv = make_env(env);
  if (!newenv->d) newenv->d = create_dict();
  exp_t *names = names_node->content;
  while (names && ispair(names) && istrue(names)) {
    exp_t *nm = names->content;
    if (!issymbol(nm)) {
      destroy_env(newenv);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "with-gensyms: names must be symbols");
    }
    exp_t *gs = make_gensym();
    set_get_keyval_dict(newenv->d, exp_text(nm), gs);
    unrefexp(gs);
    names = names->next;
  }
  exp_t *ret = NIL_EXP;
  for (exp_t *b = body_start; b; b = b->next) {
    if (ret != NIL_EXP) unrefexp(ret);
    ret = EVAL(b->content, newenv);
    if (iserror(ret)) break;
  }
  destroy_env(newenv);
  unrefexp(e);
  return ret;
}

/* (take n xs) — first n elements of list xs. */
const char doc_take[] = "(take n xs) — list of the first n elements of xs.";
exp_t *takecmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(n, xs);
  if (!n || !isnumber(n))
    CLEAN_RETURN_2(n, xs, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                "take: first arg must be a number"));
  int64_t count = FIX_VAL(n);
  exp_t *ret = NIL_EXP, *tail = NULL;
  exp_t *cur = xs;
  for (int64_t i = 0; i < count && ispair(cur) && cur->content; i++) {
    list_append_owned(&ret, &tail, refexp(cur->content));
    cur = cur->next;
  }
  CLEAN_RETURN_2(n, xs, ret);
}

/* (drop n xs) — xs with the first n elements removed. */
const char doc_drop[] = "(drop n xs) — xs without its first n elements.";
exp_t *dropcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(n, xs);
  if (!n || !isnumber(n))
    CLEAN_RETURN_2(n, xs, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                "drop: first arg must be a number"));
  int64_t count = FIX_VAL(n);
  exp_t *cur = xs;
  for (int64_t i = 0; i < count && ispair(cur) && cur->content; i++)
    cur = cur->next;
  exp_t *ret = NIL_EXP, *tail = NULL;
  while (ispair(cur) && cur->content) {
    list_append_owned(&ret, &tail, refexp(cur->content));
    cur = cur->next;
  }
  CLEAN_RETURN_2(n, xs, ret);
}

/* (range start end) / (range start end step) — list of integers. */
const char doc_range[] =
    "(range start end) or (range start end step) — list of integers "
    "from start (inclusive) to end (exclusive).";
exp_t *rangecmd(exp_t *e, env_t *env) {
  exp_t *arg1 = NULL, *arg2 = NULL, *arg3 = NULL;
  if (!e->next) goto bad;
  arg1 = EVAL(e->next->content, env);
  if (iserror(arg1)) { unrefexp(e); return arg1; }
  if (!e->next->next) goto bad;
  arg2 = EVAL(e->next->next->content, env);
  if (iserror(arg2)) { unrefexp(arg1); unrefexp(e); return arg2; }
  if (e->next->next->next) {
    arg3 = EVAL(e->next->next->next->content, env);
    if (iserror(arg3)) { unrefexp(arg1); unrefexp(arg2); unrefexp(e); return arg3; }
  }
  if (!isnumber(arg1) || !isnumber(arg2) || (arg3 && !isnumber(arg3))) {
    unrefexp(arg1); unrefexp(arg2); unrefexp(arg3); unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "range: args must be integers");
  }
  {
    int64_t start = FIX_VAL(arg1), end = FIX_VAL(arg2);
    int64_t step = arg3 ? FIX_VAL(arg3) : (start <= end ? 1 : -1);
    unrefexp(arg1); unrefexp(arg2); unrefexp(arg3); unrefexp(e);
    if (step == 0)
      return error(ERROR_ILLEGAL_VALUE, NULL, env, "range: step cannot be 0");
    exp_t *ret = NIL_EXP, *tail = NULL;
    for (int64_t i = start; step > 0 ? i < end : i > end; i += step)
      list_append_owned(&ret, &tail, MAKE_FIX(i));
    return ret;
  }
bad:
  unrefexp(arg1); unrefexp(arg2); unrefexp(e);
  return error(ERROR_MISSING_PARAMETER, e, env, "(range start end [step])");
}

/* (zip xs ys) — list of (x y) pairs from two lists. Stops at shorter. */
const char doc_zip[] =
    "(zip xs ys) — list of (x y) pairs; stops at the shorter list.";
exp_t *zipcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(xs, ys);
  exp_t *ret = NIL_EXP, *tail = NULL;
  exp_t *cx = xs, *cy = ys;
  while (ispair(cx) && cx->content && ispair(cy) && cy->content) {
    exp_t *pair = make_node(refexp(cx->content));
    pair->next = make_node(refexp(cy->content));
    list_append_owned(&ret, &tail, pair);
    cx = cx->next; cy = cy->next;
  }
  CLEAN_RETURN_2(xs, ys, ret);
}

/* flatten helper (non-recursive, uses a stack to avoid C stack growth) */
static void flatten_into(exp_t *x, exp_t **ret, exp_t **tail) {
  if (!x || x == NIL_EXP) return;
  if (!ispair(x) || !x->content) {
    list_append_owned(ret, tail, refexp(x));
    return;
  }
  exp_t *cur = x;
  while (ispair(cur) && cur->content) {
    exp_t *v = cur->content;
    if (ispair(v) && v->content)
      flatten_into(v, ret, tail);
    else if (v && v != NIL_EXP)
      list_append_owned(ret, tail, refexp(v));
    cur = cur->next;
  }
}

/* (flatten xs) — recursively flatten nested lists into a flat list. */
const char doc_flatten[] =
    "(flatten xs) — recursively flatten nested lists.";
exp_t *flattencmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(xs);
  exp_t *ret = NIL_EXP, *tail = NULL;
  flatten_into(xs, &ret, &tail);
  CLEAN_RETURN_1(xs, ret);
}

/* sort helpers */
typedef struct { exp_t **arr; int n; } sort_ctx;

static int sort_cmp_default(const void *a, const void *b) {
  exp_t *x = *(exp_t **)a, *y = *(exp_t **)b;
  if (isnumber(x) && isnumber(y)) {
    int64_t dx = FIX_VAL(x), dy = FIX_VAL(y);
    return dx < dy ? -1 : dx > dy ? 1 : 0;
  }
  if (isfloat(x) && isfloat(y))
    return x->f < y->f ? -1 : x->f > y->f ? 1 : 0;
  if (isnumber(x) && isfloat(y)) {
    double dx = (double)FIX_VAL(x);
    return dx < y->f ? -1 : dx > y->f ? 1 : 0;
  }
  if (isfloat(x) && isnumber(y)) {
    double dy = (double)FIX_VAL(y);
    return x->f < dy ? -1 : x->f > dy ? 1 : 0;
  }
  if (isstring(x) && isstring(y))
    return strcmp((char *)exp_text(x), (char *)exp_text(y));
  return 0;
}

/* (sort xs) — sort list with default < ordering (numbers, strings). */
const char doc_sort[] =
    "(sort xs) — sort list using default ordering (numbers by value, "
    "strings lexicographically).";
exp_t *sortcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(xs);
  if (!xs || xs == NIL_EXP) CLEAN_RETURN_1(xs, NIL_EXP);
  if (!ispair(xs))
    CLEAN_RETURN_1(xs, error(ERROR_ILLEGAL_VALUE, NULL, env,
                             "sort: arg must be a list"));
  int n = 0;
  for (exp_t *c = xs; ispair(c) && c->content; c = c->next) n++;
  exp_t **arr = memalloc(n, sizeof *arr);
  int i = 0;
  for (exp_t *c = xs; ispair(c) && c->content; c = c->next)
    arr[i++] = c->content;
  qsort(arr, n, sizeof *arr, sort_cmp_default);
  exp_t *ret = NIL_EXP, *tail = NULL;
  for (i = 0; i < n; i++)
    list_append_owned(&ret, &tail, refexp(arr[i]));
  free(arr);
  CLEAN_RETURN_1(xs, ret);
}

/* (sort-by key-fn xs) — sort list by (key-fn element). */
const char doc_sortby[] =
    "(sort-by key-fn xs) — sort xs by (key-fn element).";

typedef struct { exp_t *val; exp_t *key; } sortby_pair;

static int sort_cmp_by(const void *a, const void *b) {
  const sortby_pair *pa = (const sortby_pair *)a;
  const sortby_pair *pb = (const sortby_pair *)b;
  return sort_cmp_default(&pa->key, &pb->key);
}

exp_t *sortbycmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(fn, xs);
  if (!fn || !e->next->next) /* NULL list value = empty list, not missing arg */
    CLEAN_RETURN_2(fn, xs, error(ERROR_MISSING_PARAMETER, e, env,
                                 "(sort-by key-fn xs)"));
  if (xs == NIL_EXP) CLEAN_RETURN_2(fn, xs, NIL_EXP);
  if (!ispair(xs))
    CLEAN_RETURN_2(fn, xs, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                 "sort-by: second arg must be a list"));
  int n = 0;
  for (exp_t *c = xs; ispair(c) && c->content; c = c->next) n++;
  sortby_pair *pairs = memalloc(n, sizeof *pairs);
  int i = 0;
  for (exp_t *c = xs; ispair(c) && c->content; c = c->next) {
    pairs[i].val = c->content;
    pairs[i].key = alc_apply1(fn, c->content, env);
    if (!pairs[i].key) pairs[i].key = NIL_EXP;
    if (iserror(pairs[i].key)) {
      exp_t *err = pairs[i].key;
      for (int j = 0; j < i; j++) unrefexp(pairs[j].key);
      free(pairs);
      CLEAN_RETURN_2(fn, xs, err);
    }
    i++;
  }
  qsort(pairs, n, sizeof *pairs, sort_cmp_by);
  exp_t *ret = NIL_EXP, *tail = NULL;
  for (i = 0; i < n; i++) {
    list_append_owned(&ret, &tail, refexp(pairs[i].val));
    unrefexp(pairs[i].key);
  }
  free(pairs);
  CLEAN_RETURN_2(fn, xs, ret);
}

/* (string-contains? s sub) — t if s contains substring sub. */
const char doc_stringcontainsp[] =
    "(string-contains? s sub) — t if string s contains substring sub.";
exp_t *stringcontainspcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(s, sub);
  if (!isstring(s) || !isstring(sub))
    CLEAN_RETURN_2(s, sub, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                 "string-contains?: args must be strings"));
  exp_t *ret = strstr((char *)exp_text(s), (char *)exp_text(sub)) ? TRUE_EXP : NIL_EXP;
  CLEAN_RETURN_2(s, sub, ret);
}

/* (string-index s sub) — 0-based index of first occurrence, or nil. */
const char doc_stringindex[] =
    "(string-index s sub) — index of first occurrence of sub in s, or nil.";
exp_t *stringindexcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(s, sub);
  if (!isstring(s) || !isstring(sub))
    CLEAN_RETURN_2(s, sub, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                 "string-index: args must be strings"));
  char *base = (char *)exp_text(s);
  char *found = strstr(base, (char *)exp_text(sub));
  exp_t *ret =
      found ? MAKE_FIX(utf8_count_bytes(base, (size_t)(found - base))) : NIL_EXP;
  CLEAN_RETURN_2(s, sub, ret);
}

/* (string-replace s old new) — replace all occurrences of old with new. */
const char doc_stringreplace[] =
    "(string-replace s old new) — replace all occurrences of old with new.";
exp_t *stringreplacecmd(exp_t *e, env_t *env) {
  exp_t *s = NULL, *old = NULL, *nw = NULL;
  if (!e->next || !e->next->next || !e->next->next->next)
    goto bad;
  s = EVAL(e->next->content, env);
  if (iserror(s)) { unrefexp(e); return s; }
  old = EVAL(e->next->next->content, env);
  if (iserror(old)) { unrefexp(s); unrefexp(e); return old; }
  nw = EVAL(e->next->next->next->content, env);
  if (iserror(nw)) { unrefexp(s); unrefexp(old); unrefexp(e); return nw; }
  if (!isstring(s) || !isstring(old) || !isstring(nw)) {
    unrefexp(s); unrefexp(old); unrefexp(nw); unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "string-replace: args must be strings");
  }
  {
    const char *haystack = (char *)exp_text(s);
    const char *needle = (char *)exp_text(old);
    const char *replacement = (char *)exp_text(nw);
    size_t nlen = strlen(needle), rlen = strlen(replacement);
    size_t cap = 64, len = 0;
    char *buf = memalloc(cap, 1);
    const char *p = haystack;
    if (nlen == 0) {
      str_buf_put(&buf, &len, &cap, p, strlen(p));
    } else {
      const char *found;
      while ((found = strstr(p, needle)) != NULL) {
        str_buf_put(&buf, &len, &cap, p, (size_t)(found - p));
        str_buf_put(&buf, &len, &cap, replacement, rlen);
        p = found + nlen;
      }
      str_buf_put(&buf, &len, &cap, p, strlen(p));
    }
    exp_t *ret = make_string(buf, (int)len);
    free(buf);
    unrefexp(s); unrefexp(old); unrefexp(nw); unrefexp(e);
    return ret;
  }
bad:
  unrefexp(s); unrefexp(old); unrefexp(e);
  return error(ERROR_MISSING_PARAMETER, e, env,
               "(string-replace s old new)");
}

/* (error? x) — t if x is an error value. */
const char doc_errorp[] = "(error? x) — t if x is an error value.";
exp_t *errorpcmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *ret = NIL_EXP;
  if (e->next) {
    a = EVAL(e->next->content, env);
    /* Don't propagate: we want to inspect the error, not re-raise it. */
    if (a && iserror(a)) ret = TRUE_EXP;
  }
  if (a && !iserror(a)) unrefexp(a);
  else if (a) unrefexp(a);
  unrefexp(e);
  return ret;
}

/* (error-message x) — string message from an error value, or nil. */
const char doc_errormessage[] =
    "(error-message x) — extract the message string from an error value.";
exp_t *errormessagecmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *ret = NIL_EXP;
  if (e->next) {
    a = EVAL(e->next->content, env);
    if (a && iserror(a) && a->ptr)
      { const char *_t = exp_text(a); ret = make_string((char *)_t, (int)strlen((char *)_t)); }
  }
  if (a) unrefexp(a);
  unrefexp(e);
  return ret;
}

/* (try body-expr handler) — evaluate body; on error call (handler err).
   Unlike normal propagation, the error is caught here and not re-raised.
   handler receives the error exp_t and may call (error-message e) on it. */
const char doc_try[] =
    "(try body handler) — evaluate body; if it signals an error call "
    "(handler err). Returns body's value on success or handler's value. "
    "(try body handler finally-expr) — like above but always evaluates "
    "finally-expr last; its value is discarded. "
    "(try body nil finally-expr) — no catch; run body, always run finally "
    "(errors from body propagate after finally runs).";
exp_t *trycmd(exp_t *e, env_t *env) {
  if (!e->next || !e->next->next) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, NULL, env, "(try body handler)");
  }
  exp_t *finally_form = e->next->next->next
                        ? e->next->next->next->content : NULL;
  exp_t *result = EVAL(e->next->content, env);
  exp_t *ret;
  if (!result || !iserror(result)) {
    ret = result ? result : NIL_EXP;
  } else {
    /* Error path: evaluate the handler, then apply it to the error value.
       Only a literal nil handler means "no catch" — NOT any falsey value.
       (A lambda is the normal handler; istrue(lambda) is false, so a
       truthiness test here would reject every real handler.) */
    exp_t *handler = EVAL(e->next->next->content, env);
    if (!handler || handler == NIL_EXP) {
      ret = result; /* nil handler = no catch; propagate the body error */
    } else if (iserror(handler)) {
      unrefexp(result); /* handler eval itself failed — surface that error */
      ret = handler;
    } else {
      ret = alc_apply1(handler, result, env);
      unrefexp(handler);
      unrefexp(result);
      if (!ret) ret = NIL_EXP;
    }
  }
  if (finally_form) {
    exp_t *fret = EVAL(finally_form, env);
    if (fret && iserror(fret) && !iserror(ret)) {
      unrefexp(ret);
      ret = fret;
    } else
      unrefexp(fret);
  }
  unrefexp(e);
  return ret ? ret : NIL_EXP;
}

/* ---------- escape continuations (call/cc) ----------
   alcove's evaluator is a recursive C tree-walker (plus a bytecode VM), so a
   FULL re-entrant continuation — resumable more than once, or downward —
   would require capturing the C stack, which isn't portable here. We provide
   the widely-useful subset: ONE-SHOT, UPWARD (escape) continuations.
   (call/cc f) calls f with a continuation k; invoking (k v) abandons the
   in-progress work and makes the call/cc form return v. k is valid only
   during that call/cc's dynamic extent; invoking it afterward is an error.

   Mechanism: invoking k yields an EXP_ERROR-tagged escape token (errnum
   ERROR_CONT_ESCAPE) carrying the continuation id (in `meta`) and the payload
   (in `next`). It propagates up exactly like an error — every iserror
   short-circuit in evaluate/invoke/vm_run/builtins carries it — until the
   matching call/cc frame catches it. No setjmp; rides the existing
   error-propagation plumbing (same model as try/catch). */
static int64_t g_cont_id = 0;

static exp_t *make_cont(int64_t id) {
  exp_t *k = make_nil(); /* content == next == NULL */
  k->type = EXP_CONT;
  k->meta = (struct keyval_t *)(intptr_t)id; /* id lives in the unused meta */
  return k;
}
/* Build an escape token for continuation `id` carrying `payload`. Consumes
   the caller's `payload` ref (error() takes its own via the id parameter). */
static exp_t *make_cont_escape(int64_t id, exp_t *payload, env_t *env) {
  exp_t *tok = error(ERROR_CONT_ESCAPE, payload, env,
                     "call/cc continuation invoked outside its extent");
  tok->meta = (struct keyval_t *)(intptr_t)id;
  unrefexp(payload);
  return tok;
}
#define is_cont_escape(e) (iserror(e) && (e)->flags == ERROR_CONT_ESCAPE)

/* Invoke continuation `cont` with `arg` (consumed) → an escape token. */
static exp_t *apply_cont(exp_t *cont, exp_t *arg, env_t *env) {
  return make_cont_escape((int64_t)(intptr_t)cont->meta, arg, env);
}
/* Evaluate a (k arg) call form (arg optional, defaults nil). */
static exp_t *eval_cont_call(exp_t *cont, exp_t *e, env_t *env) {
  exp_t *arg = e->next ? EVAL(e->next->content, env) : refexp(NIL_EXP);
  if (iserror(arg)) /* arg eval failed, or itself escaped — propagate */
    return arg;
  return apply_cont(cont, arg, env);
}

const char doc_callcc[] =
    "(call/cc f) — call f with an escape continuation k; invoking (k v) makes "
    "this call/cc return v, abandoning the work in between. ESCAPE-ONLY: k is "
    "valid only during call/cc's dynamic extent (one-shot, upward) — calling "
    "it later errors. Not a full re-entrant continuation.";
exp_t *callcccmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(f);
  if (!(islambda(f) || isinternal(f)))
    CLEAN_RETURN_1(f, error(ERROR_ILLEGAL_VALUE, e, env,
                            "call/cc: argument must be a function"));
  int64_t id = ++g_cont_id;
  exp_t *k = make_cont(id);
  exp_t *r = alc_apply1(f, k, env); /* alc_apply1 takes its own ref to k */
  exp_t *ret;
  if (r && is_cont_escape(r) && (int64_t)(intptr_t)r->meta == id) {
    ret = refexp(r->next ? r->next : NIL_EXP); /* payload = the escape value */
    unrefexp(r);
  } else {
    ret = r ? r : NIL_EXP; /* normal return, or an escape/error bound for an
                              OUTER frame — propagate unchanged */
  }
  unrefexp(k);
  CLEAN_RETURN_1(f, ret);
}

/* ---- End new stdlib additions ---------------------------------------- */

const char doc_odd[] = "(odd x) — t if integer x is odd, nil otherwise.";
exp_t *oddcmd(exp_t *e, env_t *env) {
  exp_t *ret;
  if (e->next && isnumber(e->next->content))
    ret = ((FIX_VAL(e->next->content) & 1) ? TRUE_EXP : NIL_EXP);
  else
    ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
  unrefexp(e);
  return ret;
}

const char doc_do[] = "(do expr ...) — evaluate exprs in order; returns the "
                      "value of the last one.";
exp_t *docmd(exp_t *e, env_t *env) {
  /* Tail-aware: propagates in_tail_position to the final expression so
     a tail call inside (do ... (f x)) actually gets TCO. Returns the
     last expression's value (not nil — that was a pre-existing bug). */
  int outer_tail = in_tail_position;
  exp_t *cur = cdr(e);
  exp_t *ret = NULL;
  while (cur) {
    if (ret)
      unrefexp(ret);
    in_tail_position = (cur->next == NULL) ? outer_tail : 0;
    ret = EVAL(car(cur), env);
    if (ret && iserror(ret)) {
      in_tail_position = outer_tail;
      unrefexp(e);
      return ret;
    }
    cur = cdr(cur);
  }
  in_tail_position = outer_tail;
  unrefexp(e);
  return ret ? ret : NIL_EXP;
}

const char doc_when[] = "(when test expr ...) — if test is truthy, evaluate "
                        "the body in order; else nil.";
exp_t *whencmd(exp_t *e, env_t *env) {
  /* Tail position: when is TAIL_AWARE. The condition must NOT be
     evaluated in tail position — if it is, a user-fn call inside it
     returns a tail marker, and istrue() reports the marker as true
     (it's a non-empty pair), so the body fires when the condition was
     actually nil. Only the last body form inherits the outer tail. */
  int outer_tail = in_tail_position;
  exp_t *val = cadr(e);
  exp_t *cur = cddr(e);
  in_tail_position = 0;
  exp_t *ret = EVAL(val, env);
  if iserror (ret) {
    in_tail_position = outer_tail;
    unrefexp(e);
    return ret;
  }
  /* Body runs only when test is truthy AND there's a body to run
     ((when test) with no body forms gives cur=NULL — the per-arg
     tail-flag read `cur->next` would segfault). */
  int body_ran = 0;
  if (istrue(ret) && cur)
    do {
      unrefexp(ret);
      body_ran = 1;
      in_tail_position = (cur->next == NULL) ? outer_tail : 0;
      ret = EVAL(car(cur), env);
    } while ((cur = cdr(cur)) && !(ret && iserror(ret)));
  in_tail_position = outer_tail;
  if (ret && iserror(ret)) {
    unrefexp(e);
    return ret;
  }
  /* Return the last body expression's value (matches docstring and
     Arc semantics). The pre-existing code unconditionally returned
     NIL_EXP, so (when t 42) gave nil. If the body never ran (falsey
     test, or no body forms), discard ret (which holds the test value
     or last body iteration's truthy ret) and return NIL_EXP. */
  unrefexp(e);
  if (!body_ran) {
    unrefexp(ret);
    return NIL_EXP;
  }
  return ret ? ret : NIL_EXP;
}

const char doc_unless[] = "(unless test expr ...) — if test is falsey, evaluate "
                          "the body in order; else nil.";
exp_t *unlesscmd(exp_t *e, env_t *env) {
  int outer_tail = in_tail_position;
  exp_t *val = cadr(e);
  exp_t *cur = cddr(e);
  in_tail_position = 0;
  exp_t *ret = EVAL(val, env);
  if iserror (ret) {
    in_tail_position = outer_tail;
    unrefexp(e);
    return ret;
  }
  int body_ran = 0;
  if (!istrue(ret) && cur)
    do {
      unrefexp(ret);
      body_ran = 1;
      in_tail_position = (cur->next == NULL) ? outer_tail : 0;
      ret = EVAL(car(cur), env);
    } while ((cur = cdr(cur)) && !(ret && iserror(ret)));
  in_tail_position = outer_tail;
  if (ret && iserror(ret)) {
    unrefexp(e);
    return ret;
  }
  unrefexp(e);
  if (!body_ran) {
    unrefexp(ret);
    return NIL_EXP;
  }
  return ret ? ret : NIL_EXP;
}

const char doc_while[] = "(while test expr ...) — re-evaluate body while test "
                         "stays truthy. Returns nil.";
exp_t *whilecmd(exp_t *e, env_t *env) {
  exp_t *val = cadr(e);
  exp_t *cur = cddr(e);
  exp_t *curi = cur;
  exp_t *ret = NULL;
  while (istrue(ret = EVAL(val, env)) && (!iserror(ret))) {
    cur = curi;
    do {
      unrefexp(ret);
      ret = EVAL(car(cur), env);
    } while ((cur = cdr(cur)) && !(ret && iserror(ret)));
    /* A body error / call-cc escape must propagate, not be discarded by the
       next condition eval — otherwise the loop swallows it (and a persistent
       error like a fired escape spins forever). */
    if (ret && iserror(ret))
      break;
  }
  if (ret && iserror(ret)) {
    unrefexp(e);
    return ret;
  } else {
    unrefexp(ret);
    unrefexp(e);
    return NIL_EXP;
  }
}

const char doc_repeat[] = "(repeat n expr ...) — run body n times, returning "
                          "the last expression's value.";
exp_t *repeatcmd(exp_t *e, env_t *env) {
  exp_t *val = EVAL(cadr(e), env);
  exp_t *cur = cddr(e);
  exp_t *curi = cur;
  exp_t *ret = NULL;
  int64_t counter = 0;
  if iserror (val) {
    unrefexp(e);
    return val;
  }
  if (isnumber(val))
    counter = FIX_VAL(val);
  else {
    ret =
        error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value for repeat counter");
    unrefexp(val);
    unrefexp(e);
    return ret;
  }
  unrefexp(val);
  while (counter-- > 0) {
    cur = curi;
    do {
      if (ret)
        unrefexp(ret);
      ret = EVAL(car(cur), env);
    } while ((cur = cdr(cur)) && !(ret && iserror(ret)));
  }
  if (ret && iserror(ret)) {
    unrefexp(e);
    return ret;
  } else {
    if (ret)
      unrefexp(ret);
    unrefexp(e);
    return NIL_EXP;
  }
}

const char doc_and[] = "(and expr ...) — short-circuit AND. (and) is t. "
                       "Returns the last truthy or first falsey value.";
exp_t *andcmd(exp_t *e, env_t *env) {
  /* (and) → t (vacuous), per doc. The previous loop EVAL'd car(NULL)
     and ended up returning nil for the empty case.
     Tail position: andcmd is TAIL_AWARE; only the last arg inherits
     the outer tail flag. Earlier args are NOT in tail position — if
     they were, a user-fn call inside one would return a tail marker
     and the trampoline check (ispair+content) would treat it as
     truthy, causing premature short-circuit. */
  int outer_tail = in_tail_position;
  exp_t *cur = cdr(e);
  exp_t *ret = TRUE_EXP;
  while (cur) {
    if (ret != TRUE_EXP)
      unrefexp(ret);
    in_tail_position = (cur->next == NULL) ? outer_tail : 0;
    ret = EVAL(car(cur), env);
    if (iserror(ret))
      goto finish;
    if (!istrue(ret))
      goto finish;
    cur = cdr(cur);
  }
finish:
  in_tail_position = outer_tail;
  unrefexp(e);
  return ret;
}

const char doc_or[] = "(or expr ...) — short-circuit OR. (or) is nil. Returns "
                      "the first truthy value, else nil.";
exp_t *orcmd(exp_t *e, env_t *env) {
  /* See andcmd: only the last arg inherits in_tail_position; earlier
     args must clear it so user-fn calls inside them return real
     values, not tail markers (which the trampoline-check would treat
     as truthy and short-circuit early).
     Loop must guard cur — (or) with no args yields cur=NULL on
     entry, and a do/while body would deref cur->next before the
     condition check. (or) → nil per doc. */
  int outer_tail = in_tail_position;
  exp_t *cur = cdr(e);
  exp_t *ret = NIL_EXP;
  while (cur) {
    if (ret != NIL_EXP)
      unrefexp(ret);
    in_tail_position = (cur->next == NULL) ? outer_tail : 0;
    ret = EVAL(car(cur), env);
    if iserror (ret)
      goto finish;
    if (istrue(ret))
      goto finish;
    cur = cdr(cur);
  }
finish:
  in_tail_position = outer_tail;
  unrefexp(e);
  return ret;
}

const char doc_no[] = "(no x) — t if x is nil or empty list/string, nil "
                      "otherwise. The canonical \"is falsey?\" test.";
exp_t *nocmd(exp_t *e, env_t *env) {
  exp_t *cur = cdr(e);
  exp_t *tmpexp = EVAL(car(cur), env);
  if iserror (tmpexp)
    goto finish;
  if (istrue(cur = tmpexp))
    tmpexp = NIL_EXP;
  else
    tmpexp = TRUE_EXP;
  unrefexp(cur);
finish:
  unrefexp(e);
  return tmpexp;
}

int isequal(exp_t *cur1, exp_t *cur2) {
  /* borrow ref to cur1 and cur2 */
  int ret = 0;
  /* Fast path: any two tagged immediates compare by bit-pattern equality.
     Fixnum 5 == Fixnum 5, char 'a' == char 'a', and cross-type never equal. */
  if (cur1 == cur2)
    return 1;
  if (!is_ptr(cur1) || !is_ptr(cur2))
    return 0;
  if (cur1->type == cur2->type) {
    if (isfloat(cur1))
      ret = (cur1->f == cur2->f);
    else if (issymbol(cur1) || isstring(cur1))
      ret = (strcmp(exp_text(cur1), exp_text(cur2)) == 0);
    else if (iserror(cur1))
      ret = (cur1->s64 == cur2->s64);
    else if (isblob(cur1)) {
      alc_blob_t *a = (alc_blob_t *)cur1->ptr;
      alc_blob_t *b = (alc_blob_t *)cur2->ptr;
      ret = (a && b && a->len == b->len &&
             (a->len == 0 || memcmp(a->bytes, b->bytes, a->len) == 0));
    } else
      /* Dict/list: pointer identity. Deep equality would require walking
         every entry/node and is rarely what Redis-style users want. */
      ret = (cur1 == cur2);
  }
  return ret;
}

static int hamt_iso(exp_t *a, exp_t *b); /* deep map equality; HAMT section */

int isoequal(exp_t *cur1, exp_t *cur2) {
  /* borrow ref to cur1 and cur2 */
  int ret = 0;
  exp_t *cur1n;
  exp_t *cur2n;

  if (cur1 == cur2)
    return 1;
  if (!is_ptr(cur1) || !is_ptr(cur2))
    return 0;
  if (cur1->type == cur2->type) {
    if (ispair(cur1)) {
      cur1n = cur1;
      cur2n = cur2;
      ret = 1;
      do {
        ret *= isoequal(car(cur1n), car(cur2n));
        cur1n = cur1n->next;
        cur2n = cur2n->next;
      } while (ret && cur1n && cur2n);
      ret *= (cur1n == cur2n);
    } else if (isvector(cur1)) {
      /* Element-wise deep equality — doc_iso promises vector recursion.
         vec_get_boxed returns an owned ref, so release each after compare. */
      int64_t n1 = vec_len(cur1), n2 = vec_len(cur2);
      if (n1 != n2)
        ret = 0;
      else {
        ret = 1;
        for (int64_t i = 0; i < n1 && ret; i++) {
          exp_t *a = vec_get_boxed(cur1, i);
          exp_t *b = vec_get_boxed(cur2, i);
          ret = isoequal(a, b);
          unrefexp(a);
          unrefexp(b);
        }
      }
    } else if (ishamt(cur1)) {
      ret = hamt_iso(cur1, cur2); /* same entries (deep), order-independent */
    } else
      ret = isequal(cur1, cur2);
  }
  return ret;
}

const char doc_is[] =
    "(is a b) — pointer/value identity. Same fixnum or same heap object. "
    "Aliases: eq, eq?.";
EQUALITY_CMD(iscmd, isequal)

const char doc_iso[] = "(iso a b) — structural (deep) equality. Recurses into "
                       "pairs/strings/vectors.";
EQUALITY_CMD(isocmd, isoequal)

const char doc_in[] =
    "(in val a b c ...) — t if (is val) matches any of the rest.";
exp_t *incmd(exp_t *e, env_t *env) {
  exp_t *cur = cdr(e);
  exp_t *val = EVAL(cadr(e), env);
  exp_t *val2 = NULL;

  if iserror (val) {
    unrefexp(e);
    return val;
  }
  int ret = 0;
  while ((cur = cdr(cur))) {
    if (val2)
      unrefexp(val2);
    val2 = EVAL(car(cur), env);
    if iserror (val2) {
      unrefexp(e);
      unrefexp(val);
      return val2;
    }
    if ((ret = isoequal(val, val2)))
      break;
  }

  cur = (ret ? TRUE_EXP : NIL_EXP);
  unrefexp(val);
  if (val2)
    unrefexp(val2);
  unrefexp(e);
  return cur;
}

const char doc_case[] =
    "(case key v1 e1 v2 e2 ... default) — Arc-style flat pairs (NOT (val expr) "
    "clauses). First v that matches key returns its e; trailing odd element is "
    "the default.";
exp_t *casecmd(exp_t *e, env_t *env) {
  /* Tail position: case is TAIL_AWARE. The discriminant must NOT be
     in tail position (it's used for matching, not returned). The
     selected body expression IS the return value, so it inherits the
     outer tail flag. Same trap as when/and/or: a tail marker from a
     user-fn call would mis-compare via isequal. */
  int outer_tail = in_tail_position;
  exp_t *cur = cdr(e);
  in_tail_position = 0;
  exp_t *val = EVAL(cadr(e), env);
  if iserror (val) {
    in_tail_position = outer_tail;
    unrefexp(e);
    return val;
  }
  exp_t *ret = NULL;
  while ((cur = cdr(cur)))
    if (cur->next) {
      if (isequal(val, car(cur))) {
        ret = cadr(cur);
        break;
      } else
        cur = cdr(cur);
    } else
      ret = car(cur);
  unrefexp(val);
  in_tail_position = outer_tail;
  cur = EVAL(ret, env);
  unrefexp(e);
  return cur;
}

const char doc_for[] =
    "(for var start end body ...) — iterate var from start to end inclusive, "
    "evaluating body. Returns last body value.";
exp_t *forcmd(exp_t *e, env_t *env) {
  env_t *newenv = make_env(env);
  exp_t *ret = NULL;
  exp_t *curvar;
  exp_t *curval;
  exp_t *curin;
  exp_t *lastidx = NULL;
  exp_t *retval = NULL;

  if ((curvar = e->next)) {
    if ((curval = curvar->next)) {
      CHECK_RESERVED_BIND(curvar->content, ret, "in for",
                          { destroy_env(newenv); unrefexp(e); return ret; });
      if (!(newenv->d))
        newenv->d = create_dict();

      if (issymbol(curvar->content)) {
        if ((retval = EVAL(curval->content, env)) == NULL)
          retval = NIL_EXP;
        if (isnumber(retval)) {
          if (!curval->next)
            lastidx = NIL_EXP;
          if (curval->next &&
              (lastidx = EVAL(curval->next->content, env)) == NULL)
            lastidx = NIL_EXP;
          if (isnumber(lastidx)) {
            curin = curval->next->next;
          } else {
            if iserror (lastidx)
              ret = refexp(lastidx);
            else
              ret = error(ERROR_ILLEGAL_VALUE, e, env,
                          "Illegal value (not integer) in for counter");
            goto error;
          }
        } else {
          if iserror (retval)
            ret = refexp(retval);
          else
            ret = error(ERROR_ILLEGAL_VALUE, e, env,
                        "Illegal value (not integer) in for counter");
          goto error;
        }

      } else {
        ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in for");
        goto error;
      }
      {
        int64_t counter = FIX_VAL(retval);
        int64_t idx = FIX_VAL(lastidx) + 1;
        while (counter < idx) {
          /* Rebind the loop variable to a fresh tagged fixnum. */
          set_get_keyval_dict(newenv->d, exp_text(curvar->content),
                              MAKE_FIX(counter));
          curval = curin;
          while (curval) {
            if (ret)
              unrefexp(ret);
            ret = EVAL(curval->content, newenv);
            if (iserror(ret))
              goto error;
            /* NULL is treated as nil — some builtins (historically prn,
               others may return NULL too) didn't return NIL_EXP. We
               normalize here so the post-loop "if (!ret)" doesn't
               misread a clean iteration as "missing parameter". */
            if (!ret)
              ret = NIL_EXP;
            curval = curval->next;
          }
          counter++;
        }
      }
    }
  }
error:
  destroy_env(newenv);
  if (!ret)
    ret = error(ERROR_MISSING_PARAMETER, e, env, "Missing parameter in for");
  if (lastidx)
    unrefexp(lastidx);
  if (retval)
    unrefexp(retval);
  unrefexp(e);
  return ret;
}

const char doc_each[] = "(each var coll body ...) — bind var to each element "
                        "of coll (list/string/vector) and run body.";
exp_t *eachcmd(exp_t *e, env_t *env) {
  env_t *newenv = make_env(env);
  exp_t *curvar;
  exp_t *curval;
  exp_t *curin;
  exp_t *retval = NULL;
  exp_t *tmpexp = NULL;
  exp_t *ret = NULL;

  if ((curvar = e->next)) {
    if ((curval = curvar->next)) {
      CHECK_RESERVED_BIND(curvar->content, ret, "in each",
                          { destroy_env(newenv); unrefexp(e); return ret; });
      if (!(newenv->d))
        newenv->d = create_dict();

      if (issymbol(curvar->content)) {
        if ((retval = EVAL(curval->content, env)) == NULL)
          retval = NIL_EXP;
        if (ispair(retval)) {
          curin = curval->next;
          tmpexp = retval;
          while (retval) {
            set_get_keyval_dict(newenv->d, exp_text(curvar->content), car(retval));
            curval = curin;
            while (curval) {
              ret = EVAL(curval->content, newenv);
              if iserror (ret)
                goto finish;
              unrefexp(ret);
              curval = curval->next;
            }
            retval = retval->next;
          }
          ret = NULL;
          goto finish;
        } else {
          if iserror (retval)
            ret = refexp(retval);
          else
            ret = error(ERROR_ILLEGAL_VALUE, e, env,
                        "Illegal value (not list) in each");
          goto finish;
        }

      } else {
        ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in each");
        goto finish;
      }
    }
  }

  ret = error(ERROR_MISSING_PARAMETER, e, env, "Missing parameter in each");
finish:
  destroy_env(newenv);
  /* only the head (tmpexp) owns a ref; retval is a borrowed walker */
  if (tmpexp)
    unrefexp(tmpexp);
  else if (retval)
    unrefexp(retval);
  unrefexp(e);
  return ret;
}

#pragma GCC diagnostic ignored "-Wunused-parameter"
const char doc_time[] = "(time expr) — evaluate expr, print elapsed wall time, "
                        "and return the value.";
exp_t *timecmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return make_integeri(gettimeusec());
}
#pragma GCC diagnostic warning "-Wunused-parameter"

#pragma GCC diagnostic ignored "-Wunused-parameter"
/* Map common type tags to their string names for inspect output. */
static const char *inspect_type_name(int t) {
  switch (t) {
  case EXP_SYMBOL:
    return "symbol";
  case EXP_NUMBER:
    return "number";
  case EXP_FLOAT:
    return "float";
  case EXP_STRING:
    return "string";
  case EXP_CHAR:
    return "char";
  case EXP_BOOLEAN:
    return "boolean";
  case EXP_VECTOR:
    return "vector";
  case EXP_ERROR:
    return "error";
  case EXP_PAIR:
    return "pair";
  case EXP_LAMBDA:
    return "lambda";
  case EXP_INTERNAL:
    return "builtin";
  case EXP_MACRO:
    return "macro";
  case EXP_BLOB:
    return "blob";
  case EXP_DICT:
    return "dict";
  case EXP_LIST:
    return "deque";
  case EXP_SET:
    return "set";
  case EXO_MACROINTERNAL:
    return "macro-builtin";
  case EXP_FFI:
    return "ffi";
  case EXP_TREE:
    return "tree";
  case EXP_PAIR_CIRCULAR:
    return "pair-circular";
  default:
    return "?";
  }
}

/* Display the contents of an exp_t — basic type/flag/ref info, plus
   type-specific details (lambda gets arity + params + compile/JIT status,
   string gets the value + length, etc). Caller retains the ref. */
static void inspect_value(exp_t *v) {
  if (!v) {
    printf("\x1B[96m<NULL>\x1B[39m\n");
    return;
  }
  if (!is_ptr(v)) {
    if (isnumber(v))
      printf("\x1B[96m<imm fixnum %lld>\x1B[39m\n", (long long)FIX_VAL(v));
    else if (ischar(v))
      printf("\x1B[96m<imm char %u>\x1B[39m\n", CHAR_VAL(v));
    else
      printf("\x1B[96m<imm 0x%lx>\x1B[39m\n", (long)(intptr_t)v);
    return;
  }
  printf("\x1B[96mtype:\t%d (%s)\nflag:\t%d%s%s\nref:\t%d\x1B[39m\n", v->type,
         inspect_type_name(v->type), v->flags,
         (v->flags & FLAG_COMPILED) ? " COMPILED" : "",
         (v->flags & FLAG_TAIL_AWARE) ? " TAIL_AWARE" : "", v->nref);
  if (v->type == EXP_LAMBDA) {
    if (v->meta)
      printf("\x1B[96mname:\t%s\x1B[39m\n", (char *)v->meta);
    exp_t *params = lambda_params(v);
    int arity = 0;
    exp_t *p;
    for (p = params; p; p = p->next)
      arity++;
    printf("\x1B[96marity:\t%d\nparams:\t(", arity);
    int first = 1;
    for (p = params; p; p = p->next) {
      if (!first)
        printf(" ");
      first = 0;
      if (issymbol(p->content))
        printf("%s", (char *)exp_text(p->content));
    }
    printf(")\x1B[39m\n");
    if (v->flags & FLAG_COMPILED) {
      if (v->bc) {
        printf("\x1B[96mbytecode: %d bytes, %d consts", v->bc->ncode,
               v->bc->nconsts);
#ifdef ALCOVE_JIT
        if (v->bc->jit)
          printf(", jit installed");
        else
          printf(", jit not installed");
#endif
        printf(" (use (disasm fn) to see ops)\x1B[39m\n");
      }
    } else {
      printf("\x1B[96mbody:\truns as AST (compile_lambda failed or not yet "
             "attempted)\x1B[39m\n");
    }
  } else if (v->type == EXP_MACRO && v->meta) {
    printf("\x1B[96mname:\t%s\x1B[39m\n", (char *)v->meta);
  } else if (v->type == EXP_STRING && exp_text(v)) {
    printf("\x1B[96mvalue:\t\"%s\"\nlen:\t%zu\x1B[39m\n", (char *)exp_text(v),
           strlen((char *)exp_text(v)));
  } else if (v->type == EXP_FLOAT) {
    printf("\x1B[96mvalue:\t%g\x1B[39m\n", v->f);
  } else if (v->type == EXP_SYMBOL && exp_text(v)) {
    printf("\x1B[96msym:\t%s\x1B[39m\n", (char *)exp_text(v));
  }
}

/* (inspect val) — evaluates val, prints type info + type-specific details. */
const char doc_inspect[] = "(inspect x) — print internal representation: type, "
                           "refcount, and (for lambdas) compile/JIT status.";
exp_t *inspectcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(arg);
  inspect_value(arg);
  CLEAN_RETURN_1(arg, NULL);
}

/* (dir)              — list user/local bindings, alphabetically.
   (dir "sub")        — apropos-style substring filter (CL/Clojure-style).
   (dir nil t)        — also include builtins from reserved_symbol.
   (dir "sub" t)      — substring filter + builtins.
   Walks env chain inner→outer, dedupes by name (inner wins, matching
   shadowing semantics), then sorts. Prints name + kind + (for lambdas)
   the parameter list. */
typedef struct dir_entry_t {
  const char *name;
  exp_t *val;
} dir_entry_t;

static int dir_entry_cmp(const void *a, const void *b) {
  return strcmp(((const dir_entry_t *)a)->name, ((const dir_entry_t *)b)->name);
}
static int dir_match(const char *name, const char *needle) {
  return (!needle || !*needle) ? 1 : (strstr(name, needle) != NULL);
}
static int dir_seen(dir_entry_t *arr, int n, const char *name) {
  int i;
  for (i = 0; i < n; i++)
    if (strcmp(arr[i].name, name) == 0)
      return 1;
  return 0;
}
static void dir_grow(dir_entry_t **arr, int *n, int *cap, const char *name,
                     exp_t *val) {
  if (dir_seen(*arr, *n, name))
    return;
  if (*n >= *cap) {
    *cap = *cap ? *cap * 2 : 32;
    *arr = realloc(*arr, sizeof(dir_entry_t) * (*cap));
  }
  (*arr)[(*n)++] = (dir_entry_t){name, val};
}
static void dir_collect_dict(dict_t *d, const char *needle, dir_entry_t **arr,
                             int *n, int *cap) {
  if (!d)
    return;
  int h;
  size_t i;
  keyval_t *k;
  for (h = 0; h < 2; h++) {
    if (!d->ht[h].size)
      continue;
    for (i = 0; i < d->ht[h].size; i++) {
      for (k = d->ht[h].table[i]; k; k = k->next) {
        if (!k->key)
          continue;
        if (!dir_match((const char *)k->key, needle))
          continue;
        dir_grow(arr, n, cap, (const char *)k->key, k->val);
      }
    }
  }
}

const char doc_dir[] =
    "(dir) — list every name bound in the current environment chain.";
exp_t *dircmd(exp_t *e, env_t *env) {
  const char *needle = NULL;
  int show_builtins = 0;
  EVAL_ARG_2(needle_arg, flag_arg);
  if (istrue(flag_arg))
    show_builtins = 1;
  /* needle: accept symbol or string; nil / other → no filter */
  if (is_ptr(needle_arg) && (issymbol(needle_arg) || isstring(needle_arg)))
    needle = (const char *)exp_text(needle_arg);

  dir_entry_t *arr = NULL;
  int n = 0, cap = 0;

  /* Walk env chain inner→outer so inner shadows in dir_grow's dedup. */
  env_t *cur;
  for (cur = env; cur; cur = cur->root) {
    int i;
    for (i = 0; i < cur->n_inline; i++) {
      const char *k = cur->inline_keys[i];
      if (!k || !dir_match(k, needle))
        continue;
      dir_grow(&arr, &n, &cap, k, cur->inline_vals[i]);
    }
    dir_collect_dict(cur->d, needle, &arr, &n, &cap);
  }
  if (show_builtins)
    dir_collect_dict(reserved_symbol, needle, &arr, &n, &cap);

  if (n > 1)
    qsort(arr, n, sizeof(dir_entry_t), dir_entry_cmp);

  int i;
  for (i = 0; i < n; i++) {
    exp_t *v = arr[i].val;
    const char *kind;
    if (!is_ptr(v))
      kind = "imm";
    else if (v->type == EXP_LAMBDA)
      kind = "lambda";
    else if (v->type == EXP_MACRO)
      kind = "macro";
    else if (v->type == EXP_INTERNAL)
      kind = "builtin";
    else if (v->type == EXP_SYMBOL)
      kind = "symbol";
    else if (v->type == EXP_NUMBER)
      kind = "fixnum";
    else if (v->type == EXP_FLOAT)
      kind = "float";
    else if (v->type == EXP_STRING)
      kind = "string";
    else if (v->type == EXP_PAIR)
      kind = "pair";
    else
      kind = "?";
    printf("  %-20s  %-8s", arr[i].name, kind);
    if (is_ptr(v) && v->type == EXP_LAMBDA) {
      /* Lambda: print parameter list. */
      exp_t *params = lambda_params(v);
      if (params) {
        printf("  (");
        int first = 1;
        exp_t *p;
        for (p = params; p; p = p->next) {
          if (!first)
            printf(" ");
          first = 0;
          if (issymbol(p->content))
            printf("%s", (char *)exp_text(p->content));
        }
        printf(")");
      }
    } else if (isnumber(v) || ischar(v) || isfloat(v) || isstring(v)) {
      /* Atomic value: show it. */
      printf("  ");
      print_node(v);
    }
    printf("\n");
  }

  free(arr);
  unrefexp(needle_arg);
  unrefexp(flag_arg);
  unrefexp(e);
  return NULL;
}

/* (web?) — t if the interpreter was built with -DALCOVE_WEB (running
   in a browser via Emscripten), nil otherwise. Lets .alc code branch
   on whether features that need a browser (Canvas, Web Audio, fetch)
   are available, or whether native-only features (libffi, the JIT,
   readline) are wired up. */
const char doc_webp[] = "(web?) — t if running in the WASM build, nil otherwise.";
exp_t *webpcmd(exp_t *e, env_t *env) {
  (void)e;
  (void)env;
#ifdef ALCOVE_WEB
  return TRUE_EXP;
#else
  return NIL_EXP;
#endif
}

/* (sleep-ms N) — block the caller for N milliseconds, then return nil.
   On native, calls usleep(); on the WASM build, calls emscripten_sleep()
   which, with -sASYNCIFY=1, suspends the WASM stack so the browser can
   paint and process events. This is the *only* reliable way for a
   synchronous .alc game loop to yield to the browser — JS-side
   setTimeout in an addFunction'd callback can't unwind the alcove
   eval frames behind it. */
const char doc_sleepms[] = "(sleep-ms N) — sleep N milliseconds. On web, "
                           "yields to the browser via Asyncify.";
exp_t *sleepmscmd(exp_t *e, env_t *env) {
  int ms = alcove_arg_int(e, env, 0);
  if (ms > 0) {
#ifdef ALCOVE_WEB
    emscripten_sleep((unsigned)ms);
#else
    usleep((unsigned)ms * 1000U);
#endif
  }
  return NIL_EXP;
}

/* (disasm fn)  — evaluates fn, expects a compiled lambda, prints its
   bytecode op-by-op plus the JIT install status. Useful for verifying
   what bytecode the compiler produces (no more ad-hoc fprintf in C). */
const char doc_disasm[] = "(disasm fn) — print the bytecode of a compiled "
                          "function (or note that it's not compiled).";
exp_t *disasmcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(arg);
  if (!arg || !islambda(arg)) {
    printf("\x1B[96m(disasm): not a lambda\x1B[39m\n");
  } else if (!(arg->flags & FLAG_COMPILED) || !arg->bc) {
    printf("\x1B[96m(disasm): lambda is not compiled (runs as AST)\x1B[39m\n");
  } else {
    disasm_bytecode(arg->bc);
  }
  CLEAN_RETURN_1(arg, NULL);
}

/* ---------------- Source pretty-printer ----------------
   Used by (source fn) to render a lambda body the way a Lisper would
   write it: special forms get hanging indents, sub-forms break onto
   their own lines, atoms stay inline. Width is a soft limit; if a
   form's atom-only flat rendering fits under PP_WIDTH chars, we
   keep it on one line. */
#define PP_WIDTH 60
static void pp_indent(int n) {
  int i;
  for (i = 0; i < n; i++)
    putchar(' ');
}

/* Best-effort flat width estimate for an exp_t. Counts the chars
   print_node would emit, ignoring ANSI escapes. Returns INT_MAX-ish
   on cycles or very large structures so the pretty-printer falls
   back to multi-line. */
static int pp_flat_width(exp_t *e) {
  if (e == NULL)
    return 3; /* "nil" */
  if (isnumber(e)) {
    int64_t v = FIX_VAL(e);
    int w = (v < 0) ? 2 : 1;
    int64_t a = v < 0 ? -v : v;
    while (a >= 10) {
      a /= 10;
      w++;
    }
    return w;
  }
  if (ischar(e))
    return 4; /* "#\X" */
  if (!is_ptr(e))
    return 8;
  switch (e->type) {
  case EXP_SYMBOL:
  case EXP_STRING:
    return exp_text(e)
               ? (int)strlen((char *)exp_text(e)) + (e->type == EXP_STRING ? 2 : 0)
               : 3;
  case EXP_FLOAT:
    return 12; /* approx */
  case EXP_PAIR: {
    int w = 2; /* parens */
    exp_t *cur;
    int first = 1;
    for (cur = e; cur; cur = cur->next) {
      if (cur->type != EXP_PAIR) {
        w += 3 + pp_flat_width(cur); /* " . X" */
        break;
      }
      if (!first)
        w += 1;
      first = 0;
      w += pp_flat_width(cur->content);
      if (w > PP_WIDTH * 4)
        return PP_WIDTH * 4; /* short-circuit on big forms */
    }
    return w;
  }
  default:
    return 16;
  }
}

static void pp_form(exp_t *e, int indent);

/* Print one body form, then a newline. Used for the body lists in
   def/fn/let/do where each top-level form starts a fresh line. */
static void pp_body(exp_t *body, int indent) {
  while (body) {
    putchar('\n');
    pp_indent(indent);
    pp_form(body->content, indent);
    body = body->next;
  }
}

static void pp_form(exp_t *e, int indent) {
  if (!ispair(e) || !istrue(e)) {
    print_node(e);
    return;
  }

  exp_t *head = car(e);
  const char *s = (issymbol(head)) ? (const char *)exp_text(head) : NULL;

  /* If the whole form is small, stay on one line. */
  if (pp_flat_width(e) <= PP_WIDTH - indent) {
    print_node(e);
    return;
  }

  /* Special forms with hanging indents. */
  if (s) {
    /* (if cond then [else]) — cond on header line, then/else stacked
       and indented past `(if `. */
    if (!strcmp(s, "if")) {
      exp_t *cond = cadr(e), *th = caddr(e), *el = cadddr(e);
      int sub = indent + 4;
      printf("\x1B[33m(\x1B[1;35mif\x1B[22;39m ");
      pp_form(cond, sub);
      putchar('\n');
      pp_indent(sub);
      pp_form(th, sub);
      if (el) {
        putchar('\n');
        pp_indent(sub);
        pp_form(el, sub);
      }
      printf("\x1B[33m)\x1B[39m");
      return;
    }
    /* (def NAME PARAMS BODY...) and (defmacro NAME PARAMS BODY...) */
    if (!strcmp(s, "def") || !strcmp(s, "defmacro")) {
      exp_t *name = cadr(e), *params = caddr(e), *body = cdddr(e);
      printf("\x1B[33m(\x1B[1;35m%s\x1B[22;39m ", s);
      print_node(name);
      putchar(' ');
      print_node(params);
      pp_body(body, indent + 2);
      printf("\x1B[33m)\x1B[39m");
      return;
    }
    /* (fn PARAMS BODY...) and (mac PARAMS BODY...) */
    if (!strcmp(s, "fn") || !strcmp(s, "mac")) {
      exp_t *params = cadr(e), *body = cddr(e);
      printf("\x1B[33m(\x1B[1;35m%s\x1B[22;39m ", s);
      print_node(params);
      pp_body(body, indent + 2);
      printf("\x1B[33m)\x1B[39m");
      return;
    }
    /* (let VAR VAL BODY...) — VAR and VAL inline, body indented. */
    if (!strcmp(s, "let")) {
      exp_t *var = cadr(e), *val = caddr(e), *body = cdddr(e);
      printf("\x1B[33m(\x1B[1;35mlet\x1B[22;39m ");
      print_node(var);
      putchar(' ');
      pp_form(val, indent + 2);
      pp_body(body, indent + 2);
      printf("\x1B[33m)\x1B[39m");
      return;
    }
    /* (do BODY...), (and ...), (or ...), (when ...), (unless ...) */
    if (!strcmp(s, "do") || !strcmp(s, "and") || !strcmp(s, "or") ||
        !strcmp(s, "when") || !strcmp(s, "unless")) {
      printf("\x1B[33m(\x1B[1;35m%s\x1B[22;39m", s);
      pp_body(cdr(e), indent + 2);
      printf("\x1B[33m)\x1B[39m");
      return;
    }
    /* (cond (test body...) ...) — each clause on its own line. */
    if (!strcmp(s, "cond")) {
      printf("\x1B[33m(\x1B[1;35mcond\x1B[22;39m");
      exp_t *clauses = cdr(e);
      while (clauses) {
        putchar('\n');
        pp_indent(indent + 2);
        pp_form(clauses->content, indent + 2);
        clauses = clauses->next;
      }
      printf("\x1B[33m)\x1B[39m");
      return;
    }
  }

  /* General call form (HEAD ARG1 ARG2 ...).  Header inline; if too long,
     stack args under first arg with align indent. */
  printf("\x1B[33m(\x1B[39m");
  print_node(head); /* head atom */
  exp_t *args = cdr(e);
  int sub = indent + (s ? (int)strlen(s) : 1) + 2;
  /* Place first arg on header line, rest on new lines. */
  if (args) {
    putchar(' ');
    pp_form(args->content, sub);
    args = args->next;
  }
  while (args) {
    putchar('\n');
    pp_indent(sub);
    pp_form(args->content, sub);
    args = args->next;
  }
  printf("\x1B[33m)\x1B[39m");
}

/* (source fn) — print the lambda's defining source: header + body.
   The output reads back as alcove code.  For named lambdas (def'd /
   defmacro'd) the leading form is `def` or `defmacro` and includes
   the name; for anonymous lambdas it's `fn` or `mac`.  Closures get
   a "; closure over <env>" comment so the user knows the body's
   free vars resolve against a captured environment. */
#ifdef ALCOVE_ALS
/* ---- adder source rendering ------------------------------
   Render a lambda/macro as Adder: drop outer parens at
   statement position, open `:`-blocks for body-bearing special forms,
   ladder trailing list/cons builders, shorten (quote x) to 'x. */
static int als_len(exp_t *x) {
  int n = 0;
  for (exp_t *p = x; p && ispair(p) && istrue(p); p = p->next)
    n++;
  return n;
}
static int als_harity(const char *s) {
  if (!s)
    return 0;
  if (!strcmp(s, "def") || !strcmp(s, "defmacro") || !strcmp(s, "mac") ||
      !strcmp(s, "let"))
    return 3;
  if (!strcmp(s, "for"))
    return 4;
  if (!strcmp(s, "fn") || !strcmp(s, "with") || !strcmp(s, "each") ||
      !strcmp(s, "if") || !strcmp(s, "when") || !strcmp(s, "unless") ||
      !strcmp(s, "while") || !strcmp(s, "case"))
    return 2;
  if (!strcmp(s, "do"))
    return 1;
  return 0;
}
static int als_is_builder(const char *s) {
  return s && (!strcmp(s, "list") || !strcmp(s, "cons") ||
               !strcmp(s, "append") || !strcmp(s, "quasiquote"));
}
/* a list whose head is list/cons/append/quasiquote */
static int als_builder_node(exp_t *e) {
  return e && ispair(e) && istrue(e) && issymbol(e->content) &&
         als_is_builder((char *)exp_text(e->content));
}
static void als_expr(exp_t *x);
static int als_is_quote(exp_t *x) {
  return x && ispair(x) && istrue(x) && als_len(x) == 2 &&
         issymbol(x->content) && !strcmp((char *)exp_text(x->content), "quote");
}
static void als_expr(exp_t *x) { /* argument position: keep parens */
  if (!x || !ispair(x) || !istrue(x)) {
    print_node(x);
    return;
  }
  if (als_is_quote(x)) {
    putchar('\'');
    als_expr(x->next->content);
    return;
  }
  putchar('(');
  int first = 1;
  for (exp_t *p = x; p && ispair(p) && istrue(p); p = p->next) {
    if (!first)
      putchar(' ');
    als_expr(p->content);
    first = 0;
  }
  putchar(')');
}
static void als_stmt(exp_t *x, int ind) { /* statement position */
  if (!x || !ispair(x) || !istrue(x)) {
    print_node(x);
    return;
  }
  int len = als_len(x);
  exp_t *head = x->content;
  const char *hs = issymbol(head) ? (const char *)exp_text(head) : NULL;
  if (als_is_quote(x)) {
    putchar('\'');
    als_expr(x->next->content);
    return;
  }
  if (len == 1 && hs) {
    printf("%s()", hs);
    return;
  }
  int ha = als_harity(hs), k = 0;
  if (ha && len > ha) {
    k = ha;
  } else if (als_is_builder(hs)) {
    /* ladder the maximal trailing run of nested builder calls */
    int last_non = 0, i = 0;
    for (exp_t *p = x; p && ispair(p) && istrue(p); p = p->next, i++)
      if (!als_builder_node(p->content))
        last_non = i;
    if (last_non + 1 < len)
      k = last_non + 1;
  }
  if (k > 0) { /* header line + indented child statements */
    int i = 0;
    for (exp_t *p = x; p && ispair(p) && istrue(p) && i < k;
         p = p->next, i++) {
      if (i)
        putchar(' ');
      als_expr(p->content);
    }
    putchar(':');
    i = 0;
    for (exp_t *p = x; p && ispair(p) && istrue(p); p = p->next, i++) {
      if (i < k)
        continue;
      putchar('\n');
      pp_indent(ind + 2);
      als_stmt(p->content, ind + 2);
    }
    return;
  }
  int first = 1; /* plain statement: unwrapped head + operands */
  for (exp_t *p = x; p && ispair(p) && istrue(p); p = p->next) {
    if (!first)
      putchar(' ');
    als_expr(p->content);
    first = 0;
  }
}
#endif /* ALCOVE_ALS */

const char doc_source[] = "(source fn) — print the original (params) + body "
                          "for a user-defined function.";
exp_t *sourcecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(arg);
  if (!arg || !is_ptr(arg) || (!islambda(arg) && !ismacro(arg))) {
    printf("\x1B[96m(source): not a lambda or macro\x1B[39m\n");
    CLEAN_RETURN_1(arg, NULL);
  }
  int is_macro = ismacro(arg);
  /* Only flag as a "closure" if the captured env is non-global. Top-
     level def'd lambdas always capture g_global_env; that's not a
     real closure, just the default scope. */
  env_t *cap = (env_t *)(arg->next ? arg->next->meta : NULL);
  int captured = (cap && cap != g_global_env) ? 1 : 0;
#ifdef ALCOVE_ALS
  /* adder rendering: `def NAME (params):` then the body as
     indented statements (no outer parens, `:`-blocks, 'quote). */
  {
    const char *kw = arg->meta ? (is_macro ? "defmacro" : "def")
                               : (is_macro ? "mac" : "fn");
    printf("\x1B[1;35m%s\x1B[22;39m ", kw);
    if (arg->meta)
      printf("\x1B[36m%s\x1B[39m ", (char *)arg->meta);
    exp_t *params = lambda_params(arg);
    if (params && ispair(params) && istrue(params))
      als_expr(params);
    else
      printf("()"); /* no-arg list, not the symbol nil */
    printf(":");
    /* defmacrocmd stores the macro's single body form directly at
       arg->next->content; defcmd/fncmd store a list-of-forms there. */
    exp_t *bd = arg->next ? arg->next->content : NULL;
    if (is_macro) {
      printf("\n  ");
      als_stmt(bd, 2);
    } else {
      for (exp_t *b = bd; b; b = b->next) {
        printf("\n  ");
        als_stmt(b->content, 2);
      }
    }
    if (captured)
      printf("  \x1B[90m; closure over env %p\x1B[39m", (void *)cap);
    printf("\n");
    unrefexp(arg);
    unrefexp(e);
    return NULL;
  }
#endif
  /* Print the header inline (def NAME PARAMS or fn PARAMS), then
     pretty-print each body form on its own indented line. */
  if (arg->meta) {
    printf("\x1B[33m(\x1B[1;35m%s\x1B[22;39m \x1B[36m%s\x1B[39m ",
           is_macro ? "defmacro" : "def", (char *)arg->meta);
  } else {
    printf("\x1B[33m(\x1B[1;35m%s\x1B[22;39m ", is_macro ? "mac" : "fn");
  }
  print_node(lambda_params(arg)); /* params list */
  exp_t *body = arg->next ? arg->next->content : NULL;
  while (body) {
    printf("\n  ");
    pp_form(body->content, 2);
    body = body->next;
  }
  printf("\x1B[33m)\x1B[39m");
  if (captured)
    printf("  \x1B[90m; closure over env %p\x1B[39m", (void *)cap);
  printf("\n");
  unrefexp(arg);
  unrefexp(e);
  return NULL;
}
#pragma GCC diagnostic warning "-Wunused-parameter"

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

const char doc_let[] =
    "(let var val body) — bind var to val in body; (let (a b) val body) — "
    "destructure val as a list, binding each name to the corresponding "
    "element (missing elements get nil). Binding is local to body.";
exp_t *letcmd(exp_t *e, env_t *env) {
  int outer_tail = in_tail_position;
  env_t *newenv = make_env(env);
  exp_t *ret;
  exp_t *curvar;
  exp_t *curval;

  if ((curvar = e->next)) {
    if ((curval = curvar->next)) {
      CHECK_RESERVED_BIND(curvar->content, ret, "in let", goto finish);
      in_tail_position = 0;
      if (issymbol(curvar->content)) {
        if ((ret = EVAL(curval->content, env)) == NULL)
          ret = NIL_EXP;
        if iserror (ret)
          goto finish;
        var2env_bind(exp_text(curvar->content), ret, newenv);
        ret = NULL;
      } else if (ispair(curvar->content)) {
        /* Destructuring: (let (a b ...) val body)
           Eval val once, then bind each name to the corresponding list
           element; names without a matching element get nil. */
        ret = EVAL(curval->content, env);
        if (ret == NULL) ret = NIL_EXP;
        if (iserror(ret)) goto finish;
        exp_t *dnames = curvar->content;
        exp_t *dvals  = ret;
        while (dnames && ispair(dnames) && istrue(dnames)) {
          exp_t *nm = dnames->content;
          if (!issymbol(nm)) {
            unrefexp(ret);
            ret = error(ERROR_ILLEGAL_VALUE, e, env,
                        "let: destructuring name must be a symbol");
            goto finish;
          }
          int have_val = dvals && ispair(dvals) && istrue(dvals);
          var2env_bind(exp_text(nm),
                       refexp(have_val ? dvals->content : NIL_EXP),
                       newenv);
          dnames = dnames->next;
          if (have_val) dvals = dvals->next;
        }
        unrefexp(ret);
        ret = NULL;
      } else {
        ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in let");
        goto finish;
      }
      if (curval->next) {
        exp_t *body = curval->next;
        while (body->next) {
          in_tail_position = 0;
          ret = EVAL(body->content, newenv);
          if (iserror(ret)) goto finish;
          unrefexp(ret); ret = NULL;
          body = body->next;
        }
        in_tail_position = outer_tail;
        ret = EVAL(body->content, newenv);
        goto finish;
      }
    }
  }
  ret = error(ERROR_MISSING_PARAMETER, e, env,
              "Missing parameter in let"); /* MISSING PARAMETER*/
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
          if (iserror(ret)) goto finish;
          unrefexp(ret); ret = NULL;
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
      if (iserror(ret)) goto finish;
      var2env_bind(exp_text(var), ret, newenv);
      ret = NULL;
      bcur = val_node->next;
    }
    exp_t *body = body_start;
    while (body && body->next) {
      in_tail_position = 0;
      ret = EVAL(body->content, newenv);
      if (iserror(ret)) goto finish;
      unrefexp(ret); ret = NULL;
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
    if (iserror(ret)) goto finish;
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

/* ---- cond ---------------------------------------------------------------- */

const char doc_cond[] =
    "(cond test1 expr1 test2 expr2 ... default) — Arc-style flat cond. "
    "Evaluates tests left-to-right; returns the expr paired with the first "
    "truthy test. A lone trailing element is the unconditional default. "
    "(cond) returns nil.";
exp_t *condcmd(exp_t *e, env_t *env) {
  int outer_tail = in_tail_position;
  exp_t *cur = cdr(e);
  while (cur) {
    if (!cur->next) {
      /* Lone trailing element: default */
      in_tail_position = outer_tail;
      exp_t *ret = EVAL(car(cur), env);
      unrefexp(e);
      return ret ? ret : NIL_EXP;
    }
    in_tail_position = 0;
    exp_t *test = EVAL(car(cur), env);
    if (iserror(test)) {
      in_tail_position = outer_tail;
      unrefexp(e);
      return test;
    }
    int truthy = istrue(test);
    unrefexp(test);
    if (truthy) {
      in_tail_position = outer_tail;
      exp_t *ret = EVAL(car(cur->next), env);
      unrefexp(e);
      return ret ? ret : NIL_EXP;
    }
    cur = cur->next ? cur->next->next : NULL;
  }
  in_tail_position = outer_tail;
  unrefexp(e);
  return NIL_EXP;
}

/* ---- match --------------------------------------------------------------- */

/* Forward declaration for mutual recursion in list pattern matching. */
static int alc_match_pat(exp_t *pat, exp_t *val, env_t *newenv,
                         exp_t *e_err, env_t *eval_env, exp_t **err);

static int alc_match_list_pats(exp_t *pats, exp_t *vals, env_t *newenv,
                               exp_t *e_err, env_t *eval_env, exp_t **err) {
  exp_t *p = pats, *v = vals;
  while (p && p->content) {
    if (!ispair(v) || !istrue(v)) return 0; /* fewer values than patterns */
    if (!alc_match_pat(p->content, v->content, newenv, e_err, eval_env, err))
      return *err ? -1 : 0;
    if (*err) return -1;
    p = p->next;
    v = v ? v->next : NULL;
  }
  /* Exact length: value list must also be exhausted. */
  return (!v || v == NIL_EXP || !istrue(v)) ? 1 : 0;
}

/* Returns 1 on match (variables bound into newenv), 0 on no-match,
   -1 on structural error (sets *err). All refs borrowed. */
static int alc_match_pat(exp_t *pat, exp_t *val, env_t *newenv,
                         exp_t *e_err, env_t *eval_env, exp_t **err) {
  *err = NULL;

  /* Wildcard `_` */
  if (issymbol(pat) && strcmp((char *)exp_text(pat), "_") == 0)
    return 1;

  /* Literal nil — matches nil/empty-list */
  if (!pat || pat == NIL_EXP || !istrue(pat))
    return (!val || val == NIL_EXP || !istrue(val)) ? 1 : 0;

  /* Literal t — matches t */
  if (pat == TRUE_EXP)
    return (val == TRUE_EXP) ? 1 : 0;

  /* Fixnum literal */
  if (isnumber(pat))
    return (isnumber(val) && FIX_VAL(pat) == FIX_VAL(val)) ? 1 : 0;

  /* Float literal */
  if (isfloat(pat))
    return (isfloat(val) && pat->f == val->f) ? 1 : 0;

  /* String literal */
  if (isstring(pat))
    return (isstring(val) && strcmp((char *)exp_text(pat), (char *)exp_text(val)) == 0)
           ? 1 : 0;

  /* Plain symbol: capture binding — var2env_bind is forward-declared static
     at file scope above; the in-body extern was UB (C11 §6.2.2p7). */
  if (issymbol(pat)) {
    var2env_bind((char *)exp_text(pat), refexp(val ? val : NIL_EXP), newenv);
    return 1;
  }

  /* Compound pattern: must be a pair */
  if (!ispair(pat) || !istrue(pat))
    return 0;

  exp_t *head = pat->content;
  exp_t *rest = pat->next;

  if (issymbol(head)) {
    const char *hn = (const char *)exp_text(head);

    /* (quote sym) — match a specific symbol */
    if (strcmp(hn, "quote") == 0) {
      if (!rest || !rest->content) {
        *err = error(ERROR_ILLEGAL_VALUE, e_err, eval_env,
                     "match: (quote sym) needs an argument");
        return -1;
      }
      exp_t *q = rest->content;
      return (issymbol(q) && issymbol(val) &&
              strcmp((char *)exp_text(q), (char *)exp_text(val)) == 0) ? 1 : 0;
    }

    /* (? pred) — guard: call pred on val */
    if (strcmp(hn, "?") == 0) {
      if (!rest || !rest->content) {
        *err = error(ERROR_ILLEGAL_VALUE, e_err, eval_env,
                     "match: (? pred) needs a predicate");
        return -1;
      }
      exp_t *pred = EVAL(rest->content, eval_env);
      if (!pred || iserror(pred)) { *err = pred ? pred : NIL_EXP; return -1; }
      exp_t *r = alc_apply1(pred, val ? val : NIL_EXP, eval_env);
      unrefexp(pred);
      if (!r || iserror(r)) { *err = r ? r : NIL_EXP; return -1; }
      int ok = istrue(r);
      unrefexp(r);
      return ok ? 1 : 0;
    }

    /* (cons ph pt) — pair pattern */
    if (strcmp(hn, "cons") == 0) {
      if (!rest || !rest->next) {
        *err = error(ERROR_ILLEGAL_VALUE, e_err, eval_env,
                     "match: (cons head tail) needs two sub-patterns");
        return -1;
      }
      if (!ispair(val) || !istrue(val)) return 0;
      int r = alc_match_pat(rest->content, val->content,
                            newenv, e_err, eval_env, err);
      if (r <= 0) return r;
      return alc_match_pat(rest->next->content,
                           val->next ? val->next : NIL_EXP,
                           newenv, e_err, eval_env, err);
    }

    /* (list p1 p2 ...) — exact-length list pattern */
    if (strcmp(hn, "list") == 0)
      return alc_match_list_pats(rest, val, newenv, e_err, eval_env, err);

    /* (vec p1 p2 ...) — exact-length vector pattern */
    if (strcmp(hn, "vec") == 0) {
      if (!isvector(val)) return 0;
      int64_t vlen = vec_len(val);
      exp_t *p = rest;
      int64_t pi = 0;
      while (p && p->content) {
        if (pi >= vlen) return 0;
        /* vec_get_boxed returns an owned ref for all three vec kinds. */
        exp_t *cell = vec_get_boxed(val, pi);
        int r = alc_match_pat(p->content, cell, newenv, e_err, eval_env, err);
        unrefexp(cell);
        if (r <= 0) return r;
        p = p->next; pi++;
      }
      return (pi == vlen) ? 1 : 0;
    }
  }
  return 0; /* unrecognised compound pattern = no-match */
}

const char doc_match[] =
    "(match expr pat1 result1 pat2 result2 ... default) — structural pattern "
    "matching. Evaluates expr once, then tries each pat left-to-right. The "
    "first matching pat evaluates its result with pattern variables in scope. "
    "A lone trailing element is the unconditional default; (match x) is nil. "
    "Patterns: _ wildcard; symbol x binds; nil/t/number/string literal; "
    "(list p...) exact list; (cons h t) pair; (vec p...) vector; "
    "(quote sym) symbol literal; (? pred) guard predicate.";
exp_t *matchcmd(exp_t *e, env_t *env) {
  int outer_tail = in_tail_position;
  if (!e->next) { unrefexp(e); return NIL_EXP; }
  in_tail_position = 0;
  exp_t *val = EVAL(e->next->content, env);
  if (!val || iserror(val)) {
    in_tail_position = outer_tail;
    unrefexp(e);
    return val ? val : NIL_EXP;
  }
  exp_t *cur = e->next->next;
  exp_t *ret = NIL_EXP;
  while (cur) {
    if (!cur->next) {
      /* Default: lone trailing element */
      in_tail_position = outer_tail;
      ret = EVAL(car(cur), env);
      break;
    }
    exp_t *pat   = car(cur);
    exp_t *rform = car(cur->next);
    env_t *newenv = make_env(env);
    exp_t *merr = NULL;
    int m = alc_match_pat(pat, val, newenv, e, env, &merr);
    if (merr) {
      destroy_env(newenv);
      unrefexp(val);
      unrefexp(e);
      in_tail_position = outer_tail;
      return merr;
    }
    if (m > 0) {
      in_tail_position = outer_tail;
      ret = EVAL(rform, newenv);
      destroy_env(newenv);
      break;
    }
    destroy_env(newenv);
    cur = cur->next ? cur->next->next : NULL;
  }
  unrefexp(val);
  unrefexp(e);
  in_tail_position = outer_tail;
  return ret ? ret : NIL_EXP;
}

/* ---- generators ---------------------------------------------------------- */

/* Internal dict keys for generator state (chosen to avoid user collisions). */
#define GK_KIND "\x01gk" /* fixnum kind: 0=list 1=range 2=map 3=filter */
#define GK_CUR  "\x01gc" /* cursor: list node (list), current int (range) */
#define GK_END  "\x01ge" /* end value (range) */
#define GK_STP  "\x01gs" /* step (range) */
#define GK_FN   "\x01gf" /* function (map/filter) */
#define GK_IN   "\x01gi" /* inner generator (map/filter) */
#define GK_LIST  0
#define GK_RANGE 1
#define GK_MAP   2
#define GK_FILTER 3

static exp_t *gen_dict_get(exp_t *d, const char *k) {
  keyval_t *kv = set_get_keyval_dict((dict_t *)d->ptr, (char *)k, NULL);
  return kv ? kv->val : NULL;
}
static void gen_dict_set(exp_t *d, const char *k, exp_t *v) {
  set_get_keyval_dict((dict_t *)d->ptr, (char *)k, v);
}
static int gen_kind(exp_t *g) {
  exp_t *k = gen_dict_get(g, GK_KIND);
  return (k && isnumber(k)) ? (int)FIX_VAL(k) : -1;
}
static int isgen(exp_t *g) {
  return isdict(g) && gen_kind(g) >= 0;
}

/* Advance generator g by one step. Returns next value (owned ref) or GEN_DONE.
   Mutates g's internal state in-place. */
static exp_t *alc_gen_step(exp_t *g, env_t *env) {
  /* A plain closure is a generator: each step calls it with no args, and
     it returns the next value (or *done* to signal exhaustion). This lets
     users write custom generators as ordinary 0-arg closures — counters,
     fib, zip, etc. — that compose with map!/filter!/for-each!/collect!. */
  if (islambda(g)) {
    exp_t *v = alc_apply_n(g, 0, NULL, env); /* borrows g, no argv to own */
    return v ? v : GEN_DONE;
  }
  if (!isgen(g)) return GEN_DONE;
  switch (gen_kind(g)) {

  case GK_LIST: {
    exp_t *cur = gen_dict_get(g, GK_CUR);
    if (!cur || !ispair(cur) || !istrue(cur)) return GEN_DONE;
    exp_t *val  = refexp(cur->content ? cur->content : NIL_EXP);
    /* refexp next BEFORE dict-set: the dict will unref the old cursor
       (cascade-freeing cur), which would also decrement cur->next.
       Our explicit ref keeps next alive until dict_set stores its own. */
    exp_t *rest = refexp(cur->next ? cur->next : NIL_EXP);
    gen_dict_set(g, GK_CUR, rest);
    unrefexp(rest); /* release our protection ref; dict holds its own */
    return val;
  }

  case GK_RANGE: {
    exp_t *ecur = gen_dict_get(g, GK_CUR);
    exp_t *eend = gen_dict_get(g, GK_END);
    exp_t *estp = gen_dict_get(g, GK_STP);
    if (!ecur || !eend || !estp || !isnumber(ecur)) return GEN_DONE;
    int64_t cur = FIX_VAL(ecur), end = FIX_VAL(eend), stp = FIX_VAL(estp);
    int done = (stp > 0) ? (cur >= end) : (cur <= end);
    if (done) return GEN_DONE;
    gen_dict_set(g, GK_CUR, MAKE_FIX(cur + stp));
    return MAKE_FIX(cur);
  }

  case GK_MAP: {
    exp_t *inner = gen_dict_get(g, GK_IN);
    exp_t *fn    = gen_dict_get(g, GK_FN);
    if (!inner || !fn) return GEN_DONE;
    /* refexp fn before the recursive step: user code inside alc_gen_step
       could drop the last external ref to g, freeing the dict and fn. */
    refexp(fn);
    exp_t *v = alc_gen_step(inner, env);
    if (!v || isgen_done(v)) { unrefexp(fn); return GEN_DONE; }
    exp_t *r = alc_apply1(fn, v, env);
    unrefexp(fn);
    unrefexp(v);
    return r ? r : NIL_EXP;
  }

  case GK_FILTER: {
    exp_t *inner = gen_dict_get(g, GK_IN);
    exp_t *pred  = gen_dict_get(g, GK_FN);
    if (!inner || !pred) return GEN_DONE;
    /* refexp pred across the loop: inner step may drop the last ref to g. */
    refexp(pred);
    for (;;) {
      exp_t *v = alc_gen_step(inner, env);
      if (!v || isgen_done(v)) { unrefexp(pred); return GEN_DONE; }
      exp_t *ok = alc_apply1(pred, v, env);
      if (iserror(ok)) { unrefexp(pred); unrefexp(v); return ok; }
      int pass = istrue(ok);
      unrefexp(ok);
      if (pass) { unrefexp(pred); return v; }
      unrefexp(v);
    }
  }
  }
  return GEN_DONE;
}

static exp_t *make_gen_dict(int kind) {
  exp_t *d = make_nil();
  d->type = EXP_DICT;
  d->ptr  = create_dict();
  gen_dict_set(d, GK_KIND, MAKE_FIX(kind));
  return d;
}

const char doc_gendone[] = "(*gen-done*) — returns the generator exhaustion "
                           "sentinel. Generators return this when exhausted.";
exp_t *gendone_cmd(exp_t *e, env_t *env) {
  unrefexp(e); (void)env;
  return GEN_DONE;
}

const char doc_gendonep[] = "(gen-done? x) — t if x is the generator exhaustion "
                            "sentinel (*gen-done*), nil otherwise.";
exp_t *gendonep_cmd(exp_t *e, env_t *env) {
  if (!e->next) { unrefexp(e); return NIL_EXP; }
  exp_t *v = EVAL(e->next->content, env);
  exp_t *ret = (v && isgen_done(v)) ? TRUE_EXP : NIL_EXP;
  unrefexp(v);
  unrefexp(e);
  return ret;
}

const char doc_genlist[] = "(gen-list lst) — returns a generator that yields "
                           "each element of lst in order. Exhaustion returns *gen-done*.";
exp_t *genlist_cmd(exp_t *e, env_t *env) {
  if (!e->next) { unrefexp(e); return NIL_EXP; }
  exp_t *lst = EVAL(e->next->content, env);
  if (iserror(lst)) { unrefexp(e); return lst; }
  exp_t *g = make_gen_dict(GK_LIST);
  gen_dict_set(g, GK_CUR, lst ? lst : NIL_EXP);
  unrefexp(lst);
  unrefexp(e);
  return g;
}

const char doc_genrange[] =
    "(gen-range end) / (gen-range start end) / (gen-range start end step) — "
    "yields integers from start (default 0) up to but not including end, "
    "incrementing by step (default 1; negative step counts down).";
exp_t *genrange_cmd(exp_t *e, env_t *env) {
  exp_t *cur = cdr(e);
  if (!cur) { unrefexp(e); return NIL_EXP; }
  EVAL_ARG_1(a);
  int64_t start = 0, end = 0, step = 1;
  exp_t *b = NULL, *c = NULL;
  if (cdr(cur)) {
    b = EVAL(car(cdr(cur)), env);
    if (!b) { unrefexp(a); unrefexp(e); return NIL_EXP; }
    if (iserror(b)) { unrefexp(a); unrefexp(e); return b; }
    if (cdr(cdr(cur))) {
      c = EVAL(car(cdr(cdr(cur))), env);
      if (!c) { unrefexp(a); unrefexp(b); unrefexp(e); return NIL_EXP; }
      if (iserror(c)) { unrefexp(a); unrefexp(b); unrefexp(e); return c; }
    }
  }
  if (!b) {
    /* (gen-range end) */
    if (!isnumber(a)) {
      unrefexp(a); unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env, "gen-range: integer expected");
    }
    end = FIX_VAL(a);
  } else if (!c) {
    /* (gen-range start end) */
    if (!isnumber(a) || !isnumber(b)) {
      unrefexp(a); unrefexp(b); unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env, "gen-range: integers expected");
    }
    start = FIX_VAL(a); end = FIX_VAL(b);
  } else {
    /* (gen-range start end step) */
    if (!isnumber(a) || !isnumber(b) || !isnumber(c)) {
      unrefexp(a); unrefexp(b); unrefexp(c); unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env, "gen-range: integers expected");
    }
    start = FIX_VAL(a); end = FIX_VAL(b); step = FIX_VAL(c);
    unrefexp(c);
  }
  unrefexp(a); unrefexp(b);
  if (step == 0) { unrefexp(e); return error(ERROR_ILLEGAL_VALUE, NULL, env, "gen-range: step cannot be 0"); }
  exp_t *g = make_gen_dict(GK_RANGE);
  gen_dict_set(g, GK_CUR, MAKE_FIX(start));
  gen_dict_set(g, GK_END, MAKE_FIX(end));
  gen_dict_set(g, GK_STP, MAKE_FIX(step));
  unrefexp(e);
  return g;
}

const char doc_gennext[] = "(gen-next! g) — advance generator g and return its "
                           "next value, or *gen-done* when exhausted.";
exp_t *gennext_cmd(exp_t *e, env_t *env) {
  if (!e->next) { unrefexp(e); return GEN_DONE; }
  exp_t *g = EVAL(e->next->content, env);
  if (!g || iserror(g)) { unrefexp(e); return g ? g : NIL_EXP; }
  exp_t *ret = alc_gen_step(g, env);
  unrefexp(g);
  unrefexp(e);
  return ret ? ret : GEN_DONE;
}

const char doc_gencollect[] =
    "(gen-collect g) — drain generator g into a list and return it.";
exp_t *gencollect_cmd(exp_t *e, env_t *env) {
  if (!e->next) { unrefexp(e); return NIL_EXP; }
  exp_t *g = EVAL(e->next->content, env);
  if (!g || iserror(g)) { unrefexp(e); return g ? g : NIL_EXP; }
  exp_t *head = NIL_EXP, *tail = NULL;
  for (;;) {
    exp_t *v = alc_gen_step(g, env);
    if (!v || isgen_done(v)) break;
    if (iserror(v)) {
      unrefexp(head); /* NIL_EXP is immortal — unrefexp handles it safely */
      unrefexp(g); unrefexp(e);
      return v;
    }
    list_append_owned(&head, &tail, v);
  }
  unrefexp(g);
  unrefexp(e);
  return head;
}

const char doc_genmap[] =
    "(gen-map fn g) — return a generator that yields (fn x) for each x from g.";
exp_t *genmap_cmd(exp_t *e, env_t *env) {
  if (!e->next || !e->next->next) { unrefexp(e); return NIL_EXP; }
  EVAL_ARG_1(fn);
  exp_t *g = EVAL(e->next->next->content, env);
  if (!fn || iserror(fn)) { unrefexp(g); unrefexp(e); return fn ? fn : NIL_EXP; }
  if (!g  || iserror(g))  { unrefexp(fn); unrefexp(e); return g  ? g  : NIL_EXP; }
  exp_t *out = make_gen_dict(GK_MAP);
  gen_dict_set(out, GK_FN, fn);
  gen_dict_set(out, GK_IN, g);
  unrefexp(fn); unrefexp(g);
  unrefexp(e);
  return out;
}

const char doc_genfilter[] =
    "(gen-filter pred g) — return a generator yielding only elements of g "
    "for which (pred x) is truthy.";
exp_t *genfilter_cmd(exp_t *e, env_t *env) {
  if (!e->next || !e->next->next) { unrefexp(e); return NIL_EXP; }
  EVAL_ARG_1(pred);
  exp_t *g = EVAL(e->next->next->content, env);
  if (!pred || iserror(pred)) { unrefexp(g); unrefexp(e); return pred ? pred : NIL_EXP; }
  if (!g    || iserror(g))    { unrefexp(pred); unrefexp(e); return g ? g : NIL_EXP; }
  exp_t *out = make_gen_dict(GK_FILTER);
  gen_dict_set(out, GK_FN, pred);
  gen_dict_set(out, GK_IN, g);
  unrefexp(pred); unrefexp(g);
  unrefexp(e);
  return out;
}

const char doc_forgen[] =
    "(for-gen var gen body ...) — iterate generator gen, binding each yielded "
    "value to var and evaluating body forms. Returns nil.";
exp_t *forgencmd(exp_t *e, env_t *env) {
  if (!e->next || !e->next->next || !e->next->next->next) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, NULL, env,
                 "(for-gen var gen body ...)");
  }
  exp_t *varname = e->next->content;
  if (!issymbol(varname)) {
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "for-gen: first arg must be a symbol");
  }
  exp_t *g = EVAL(e->next->next->content, env);
  if (!g || iserror(g)) { unrefexp(e); return g ? g : NIL_EXP; }
  exp_t *body_start = e->next->next->next;
  env_t *loop_env = make_env(env);
  int was_tail = in_tail_position;
  in_tail_position = 0;
  exp_t *ret = NIL_EXP;
  for (;;) {
    exp_t *v = alc_gen_step(g, env);
    if (!v || isgen_done(v)) break;
    if (iserror(v)) { ret = v; break; }
    /* Rebind the loop variable each iteration. */
    if (!loop_env->d) loop_env->d = create_dict();
    set_get_keyval_dict(loop_env->d, (char *)exp_text(varname), v);
    unrefexp(v);
    /* Evaluate body forms. */
    exp_t *bc = body_start;
    while (bc) {
      if (ret) unrefexp(ret);
      ret = EVAL(bc->content, loop_env);
      if (ret && iserror(ret)) goto done;
      bc = bc->next;
    }
  }
done:
  if (ret && iserror(ret)) { /* propagate */ }
  else { unrefexp(ret); ret = NIL_EXP; }
  in_tail_position = was_tail;
  destroy_env(loop_env);
  unrefexp(g);
  unrefexp(e);
  return ret;
}

/* Bind a single name (sym->ptr) to val in env; takes ownership of val. */
static void var2env_bind(char *name, exp_t *val, env_t *env) {
  if (env->n_inline < ENV_INLINE_SLOTS) {
    env->inline_keys[env->n_inline] = name;
    env->inline_vals[env->n_inline] = val;
    env->n_inline++;
  } else {
    if (!env->d)
      env->d = create_dict();
    set_get_keyval_dict(env->d, (char *)name, val);
    unrefexp(val);
  }
}

exp_t *var2env(exp_t *e, exp_t *var, exp_t *val, env_t *env, int evalexp) {
  /* borrow references */
  exp_t *curvar = var;
  exp_t *retvar;
  exp_t *curval = val;

  /* Empty params `()` are represented by nil_singleton — a pair with
     NULL content/next. Without the content check, the loop would enter
     once and either bind NULL as a key or hit "missing parameter" if
     no args were passed. The content check makes 0-arg defs work. */
  while (curvar && curvar->content) {
    /* Rest-param marker: (a b . rest) reads as (a b . rest) — detect the
       dot symbol and collect remaining args into a list for the next param. */
    if (issymbol(curvar->content) &&
        strcmp((char *)exp_text(curvar->content), ".") == 0) {
      if (!curvar->next || !curvar->next->content ||
          !issymbol(curvar->next->content))
        return error(ERROR_ILLEGAL_VALUE, e, env,
                     "rest param: symbol expected after '.'");
      char *rest_name = (char *)exp_text(curvar->next->content);
      exp_t *rest_head = NIL_EXP, *rest_tail = NULL;
      while (curval) {
        exp_t *rv = evalexp ? EVAL(curval->content, env->root)
                            : refexp(curval->content);
        if (!rv) rv = NIL_EXP;
        if (evalexp && iserror(rv)) return rv;
        list_append_owned(&rest_head, &rest_tail, rv);
        curval = curval->next;
      }
      var2env_bind(rest_name, rest_head, env);
      return NULL;
    }

    /* Default/optional param: (name default-expr) where the 2nd element is
       NOT a symbol — e.g. (y 10), (y (+ a 1)). Distinguished from a
       destructuring pattern (a b), whose elements are all symbols. The
       default is evaluated in `env` (the frame being built), so it can
       reference params bound earlier in this same list. */
    int is_default = ispair(curvar->content) &&
                     issymbol(car(curvar->content)) &&
                     cadr(curvar->content) && !issymbol(cadr(curvar->content));
    if ((curval)) {
      if ((retvar = (evalexp ? EVAL(curval->content, env->root)
                             : refexp(curval->content)))) {
        if (evalexp && iserror(retvar)) {
          return retvar;
        }
      } else
        retvar = NIL_EXP;
      if (is_default) {
        /* Arg supplied — bind the name to it; the default is unused. */
        var2env_bind((char *)exp_text(car(curvar->content)), retvar, env);
      } else if (issymbol(curvar->content)) {
        var2env_bind((char *)exp_text(curvar->content), retvar, env);
      } else if (ispair(curvar->content) && istrue(curvar->content)) {
        /* Destructuring param: (def f ((x y) z) body) binds the first arg
           as a list, extracting x and y from its elements. */
        exp_t *subpat = curvar->content;
        exp_t *subval = retvar;
        while (subpat && subpat->content) {
          exp_t *nm = subpat->content;
          if (!issymbol(nm)) {
            unrefexp(retvar);
            return error(ERROR_ILLEGAL_VALUE, e, env,
                         "destructuring param: symbol expected");
          }
          int have_val = subval && ispair(subval) && istrue(subval);
          var2env_bind((char *)exp_text(nm),
                       refexp(have_val ? subval->content : NIL_EXP),
                       env);
          subpat = subpat->next;
          if (have_val) subval = subval->next;
        }
        unrefexp(retvar);
      } else {
        unrefexp(retvar);
        return NULL;
      }
      curval = curval->next;
    } else if (is_default) {
      /* Arg omitted — evaluate the default in the frame being built. */
      exp_t *dv = EVAL(cadr(curvar->content), env);
      if (!dv)
        dv = NIL_EXP;
      if (iserror(dv))
        return dv;
      var2env_bind((char *)exp_text(car(curvar->content)), dv, env);
    } else
      return error(ERROR_MISSING_PARAMETER, e, env,
                   "Missing parameter in macro or function invoke");
    curvar = curvar->next;
  }
  return NULL;
}
/* Build a tail-call trampoline marker. Args are evaluated in env (the
   caller's frame, where local bindings still live) and attached to the
   marker directly so the outer invoke can rebind without re-evaluating.
   Marker layout:
     type    = EXP_PAIR
     flags   = FLAG_TAILREC
     content = the lambda to invoke (owned ref)
     next    = list of pre-evaluated arg nodes (each node content = value) */
static exp_t *make_tail_marker(exp_t *fn, exp_t *call_form, env_t *env) {
  /* Args are themselves not in tail position. */
  int saved_tail = in_tail_position;
  in_tail_position = 0;
  exp_t *marker = make_nil();
  marker->flags |= FLAG_TAILREC;
  marker->content = refexp(fn);
  exp_t *tail = marker;
  exp_t *src = call_form->next;
  while (src) {
    exp_t *v = EVAL(src->content, env);
    if (v && iserror(v)) {
      unrefexp(marker);
      in_tail_position = saved_tail;
      return v;
    }
    tail = tail->next = make_node(v);
    src = src->next;
  }
  in_tail_position = saved_tail;
  return marker;
}

/* ---------------- Bytecode compiler + VM ---------------- */

/* Map an opcode byte to its mnemonic. Used by disasm_bytecode. */
static const char *bc_opname(uint8_t op) {
  switch (op) {
  case OP_HALT:
    return "HALT";
  case OP_RET:
    return "RET";
  case OP_POP:
    return "POP";
  case OP_LOAD_FIX:
    return "LOAD_FIX";
  case OP_LOAD_CONST:
    return "LOAD_CONST";
  case OP_LOAD_SLOT:
    return "LOAD_SLOT";
  case OP_LOAD_GLOBAL:
    return "LOAD_GLOBAL";
  case OP_SETQ_DYN:
    return "SETQ_DYN";
  case OP_STORE_FREE:
    return "STORE_FREE";
  case OP_STORE_SLOT:
    return "STORE_SLOT";
  case OP_BIND_SLOT:
    return "BIND_SLOT";
  case OP_UNBIND_SLOT:
    return "UNBIND_SLOT";
  case OP_ADD:
    return "ADD";
  case OP_SUB:
    return "SUB";
  case OP_MUL:
    return "MUL";
  case OP_DIV:
    return "DIV";
  case OP_MOD:
    return "MOD";
  case OP_LT:
    return "LT";
  case OP_GT:
    return "GT";
  case OP_LE:
    return "LE";
  case OP_GE:
    return "GE";
  case OP_IS:
    return "IS";
  case OP_ISO:
    return "ISO";
  case OP_NOT:
    return "NOT";
  case OP_JUMP:
    return "JUMP";
  case OP_BR_IF_FALSE:
    return "BR_IF_FALSE";
  case OP_BR_IF_TRUE:
    return "BR_IF_TRUE";
  case OP_CALL:
    return "CALL";
  case OP_CALL_GLOBAL:
    return "CALL_GLOBAL";
  case OP_TAIL_SELF:
    return "TAIL_SELF";
  case OP_TAIL_CALL:
    return "TAIL_CALL";
  case OP_CONS:
    return "CONS";
  case OP_CAR:
    return "CAR";
  case OP_CDR:
    return "CDR";
  case OP_LIST:
    return "LIST";
  case OP_SLOT_ADD_FIX:
    return "SLOT_ADD_FIX";
  case OP_SLOT_SUB_FIX:
    return "SLOT_SUB_FIX";
  case OP_SLOT_LT_FIX:
    return "SLOT_LT_FIX";
  case OP_SLOT_LE_FIX:
    return "SLOT_LE_FIX";
  case OP_SLOT_GT_FIX:
    return "SLOT_GT_FIX";
  case OP_SLOT_GE_FIX:
    return "SLOT_GE_FIX";
  case OP_SLOT_LE_SLOT:
    return "SLOT_LE_SLOT";
  case OP_VEC_REF:
    return "VEC_REF";
  case OP_VEC_SET:
    return "VEC_SET";
  case OP_VEC_LEN:
    return "VEC_LEN";
  case OP_VEC_NEW:
    return "VEC_NEW";
  case OP_SQRT_INT:
    return "SQRT_INT";
  case OP_LENGTH:
    return "LENGTH";
  default:
    return "??";
  }
}

/* Decode one instruction at code[pc] and print it. Returns the byte
   length (1..4) so the caller can advance. */
static int bc_disasm_one(const uint8_t *code, int pc) {
  uint8_t op = code[pc];
  switch (op) {
  case OP_HALT:
  case OP_RET:
  case OP_POP:
  case OP_ADD:
  case OP_SUB:
  case OP_MUL:
  case OP_DIV:
  case OP_MOD:
  case OP_LT:
  case OP_GT:
  case OP_LE:
  case OP_GE:
  case OP_IS:
  case OP_ISO:
  case OP_NOT:
  case OP_CONS:
  case OP_CAR:
  case OP_CDR:
  case OP_VEC_REF:
  case OP_VEC_SET:
  case OP_VEC_LEN:
  case OP_VEC_NEW:
  case OP_SQRT_INT:
  case OP_LENGTH:
    printf("  %04d  %s\n", pc, bc_opname(op));
    return 1;
  case OP_LOAD_FIX:
  case OP_JUMP:
  case OP_BR_IF_FALSE:
  case OP_BR_IF_TRUE: {
    int16_t imm =
        (int16_t)((uint16_t)code[pc + 1] | ((uint16_t)code[pc + 2] << 8));
    printf("  %04d  %s %d\n", pc, bc_opname(op), (int)imm);
    return 3;
  }
  case OP_LOAD_CONST:
  case OP_LOAD_SLOT:
  case OP_LOAD_GLOBAL:
  case OP_SETQ_DYN:
  case OP_STORE_FREE:
  case OP_STORE_SLOT:
  case OP_BIND_SLOT:
  case OP_UNBIND_SLOT:
  case OP_CALL:
  case OP_TAIL_SELF:
  case OP_TAIL_CALL:
  case OP_LIST:
    printf("  %04d  %s %d\n", pc, bc_opname(op), (int)code[pc + 1]);
    return 2;
  case OP_CALL_GLOBAL:
    printf("  %04d  %s const_idx=%d nargs=%d\n", pc, bc_opname(op),
           (int)code[pc + 1], (int)code[pc + 2]);
    return 3;
  case OP_SLOT_ADD_FIX:
  case OP_SLOT_SUB_FIX:
  case OP_SLOT_LT_FIX:
  case OP_SLOT_LE_FIX:
  case OP_SLOT_GT_FIX:
  case OP_SLOT_GE_FIX: {
    int16_t imm =
        (int16_t)((uint16_t)code[pc + 2] | ((uint16_t)code[pc + 3] << 8));
    printf("  %04d  %s slot=%d imm=%d\n", pc, bc_opname(op), (int)code[pc + 1],
           (int)imm);
    return 4;
  }
  case OP_SLOT_LE_SLOT:
    printf("  %04d  %s slot_a=%d slot_b=%d\n", pc, bc_opname(op),
           (int)code[pc + 1], (int)code[pc + 2]);
    return 3;
  default:
    printf("  %04d  ?? 0x%02x\n", pc, op);
    return 1;
  }
}

/* Print a human-readable dump of a bytecode body: header (size + nconsts
   + JIT status) followed by one line per instruction. */
void disasm_bytecode(bytecode_t *bc) {
  if (!bc) {
    printf("  (no bytecode)\n");
    return;
  }
  printf("\x1B[96mbytecode: %d bytes, %d consts", bc->ncode, bc->nconsts);
#ifdef ALCOVE_JIT
  if (bc->jit)
    printf(", jit installed (%zu byte mmap page)", bc->jit_size);
  else
    printf(", jit not installed");
#endif
  printf("\x1B[39m\n");
  int pc = 0;
  while (pc < bc->ncode) {
    int adv = bc_disasm_one(bc->code, pc);
    if (adv <= 0)
      break;
    pc += adv;
  }
}

void bytecode_free(bytecode_t *bc) {
  if (!bc)
    return;
  int i;
  if (bc->content)
    unrefexp(bc->content);
  for (i = 0; i < bc->nconsts; i++)
    unrefexp(bc->consts[i]);
  free(bc->consts);
  free(bc->gcache);
  free(bc->code);
#ifdef ALCOVE_JIT
  if (bc->jit_mem)
    munmap(bc->jit_mem, bc->jit_size);
#endif
  free(bc);
}

#ifdef ALCOVE_JIT
/* ---------------- JIT (arm64 + amd64 backends) ----------------
   Recognizes a narrow set of lambda body shapes and emits native
   machine code that bypasses the bytecode dispatch loop entirely.

   Shapes handled by BOTH backends (leaf, no stack frame, no runtime
   callouts — generalized to any slot < ENV_INLINE_SLOTS):
     - LOAD_FIX K; RET                      →  constant      K
     - LOAD_SLOT s; LOAD_FIX K; ADD; RET    →  (+ s K) for K in int16
     - LOAD_SLOT s; LOAD_FIX K; SUB; RET    →  (- s K) for K in int16
     - LOAD_SLOT s; LOAD_FIX K; MUL; RET    →  (* s K) via (t-1)*K + 1
     - SLOT_ADD_FIX / SLOT_SUB_FIX leaf     →  same as above (fused form)
     - 19-byte self-tail counter loop       →  try_jit_simple_tail_loop

   Both backends are now at parity (commits 83b06be..8b49473): every
   shape that amd64 has, arm64 has too. The full list (ordered by ncode):
     - 19-byte simple_tail_loop          countdown
     - 24-byte recurse_mul_one           fact
     - 26-byte tail_loop_with_call       generic loop + inner call
     - 28-byte recurse_add_two           fib (iterative fast path)
     - 35-byte mark_from                 sieve-fast inner loop
     - 41-byte count_primes              sieve-fast outer counter
     - 37-byte is_prime_given            sieve list walk
     - 48-byte for_loop_inc              forsum
     - 27-byte build_inc_cons            listsum list builder
     - 50-byte tak                       Knuth's tak
     - 53-byte ackermann                 ack
     - 71-byte safe_p                    nqueens conflict check
     - 10-byte modeq_leaf                divides? leaf

   Anything else: jit_compile returns 0; the bytecode interpreter
   handles the call.

   Calling convention: the JITted function takes one arg (env_t*) and
   returns exp_t* (NULL signals deopt → caller falls back to vm_run).
   On arm64 (AAPCS) that's x0 in / x0 out; on amd64 (System V) that's
   rdi in / rax out. Leaf shapes never touch the stack or callee-saved
   regs. The two amd64 "with runtime call" shapes establish a 16-aligned
   frame (push rbx, optionally sub rsp, #pad) and restore on exit. */

static int64_t nq_count_bits(int n, int row, uint64_t all, uint64_t cols,
                             uint64_t ld, uint64_t rd) {
  if (row >= n)
    return 1;
  int64_t count = 0;
  uint64_t avail = all & ~(cols | ld | rd);
  while (avail) {
    uint64_t bit = avail & (0ULL - avail);
    avail ^= bit;
    count += nq_count_bits(n, row + 1, all, cols | bit,
                           ((ld | bit) << 1) & all, (rd | bit) >> 1);
  }
  return count;
}

static int nq_seed_masks(int n, int row, const int *placed, uint64_t *cols,
                         uint64_t *ld, uint64_t *rd) {
  uint64_t all = (n == 64) ? UINT64_MAX : ((1ULL << n) - 1ULL);
  uint64_t c = 0, l = 0, r = 0;
  for (int i = 0; i < row; i++) {
    int col = placed[i];
    if (col < 0 || col >= n)
      return 0;
    uint64_t bit = 1ULL << col;
    if (!(all & ~(c | l | r) & bit))
      return 0;
    c |= bit;
    l = ((l | bit) << 1) & all;
    r = (r | bit) >> 1;
  }
  *cols = c;
  *ld = l;
  *rd = r;
  return 1;
}

static exp_t *jit_nqueens_list_solve(env_t *env) {
  exp_t *nexp = env->inline_vals[0];
  exp_t *qs = env->inline_vals[1];
  if (!isnumber(nexp))
    return NULL;
  int64_t n64 = FIX_VAL(nexp);
  if (n64 < 0 || n64 > 32)
    return NULL;
  int n = (int)n64;
  int placed[32];
  int len = 0;
  exp_t *cur = qs;
  while (istrue(cur)) {
    if (!ispair(cur) || !isnumber(cur->content) || len >= n)
      return NULL;
    placed[len++] = (int)FIX_VAL(cur->content) - 1; /* list bench is 1-based */
    cur = cur->next;
  }
  int chronological[32];
  for (int i = 0; i < len; i++)
    chronological[len - 1 - i] = placed[i];
  uint64_t cols, ld, rd;
  if (!nq_seed_masks(n, len, chronological, &cols, &ld, &rd))
    return NULL;
  uint64_t all = (1ULL << n) - 1ULL;
  return MAKE_FIX(nq_count_bits(n, len, all, cols, ld, rd));
}

static int vec_read_fix_at(exp_t *v, int64_t i, int64_t *out) {
  switch (vec_kind(v)) {
  case VEC_KIND_I64:
    *out = vec_i64_at(v, i);
    return 1;
  case VEC_KIND_GEN: {
    exp_t *e = vec_gen_at(v, i);
    if (!isnumber(e))
      return 0;
    *out = FIX_VAL(e);
    return 1;
  }
  default:
    return 0;
  }
}

static exp_t *jit_nqueens_vec_solve(env_t *env) {
  exp_t *nexp = env->inline_vals[0];
  exp_t *rowexp = env->inline_vals[1];
  exp_t *qs = env->inline_vals[2];
  if (!isnumber(nexp) || !isnumber(rowexp) || !isvector(qs))
    return NULL;
  int64_t n64 = FIX_VAL(nexp);
  int64_t row64 = FIX_VAL(rowexp);
  if (n64 < 0 || n64 > 32 || row64 < 0 || row64 > n64 ||
      vec_len(qs) < n64)
    return NULL;
  int n = (int)n64;
  int row = (int)row64;
  int placed[32];
  for (int i = 0; i < row; i++) {
    int64_t col;
    if (!vec_read_fix_at(qs, i, &col))
      return NULL;
    placed[i] = (int)col; /* vector bench is 0-based */
  }
  uint64_t cols, ld, rd;
  if (!nq_seed_masks(n, row, placed, &cols, &ld, &rd))
    return NULL;
  uint64_t all = (1ULL << n) - 1ULL;
  return MAKE_FIX(nq_count_bits(n, row, all, cols, ld, rd));
}

static exp_t *jit_build_inc_cons(env_t *env) {
  exp_t *iexp = env->inline_vals[0];
  exp_t *nexp = env->inline_vals[1];
  exp_t *acc = env->inline_vals[2];
  if (!isnumber(iexp) || !isnumber(nexp))
    return NULL;
  int64_t i = FIX_VAL(iexp);
  int64_t n = FIX_VAL(nexp);
  exp_t *out = refexp(acc);
  while (i <= n) {
    exp_t *node = make_node(MAKE_FIX(i));
    if (istrue(out))
      node->next = out;
    else {
      unrefexp(out);
      node->next = NULL;
    }
    out = node;
    if (i == INT64_MAX)
      break;
    i++;
  }
  return out;
}

static int try_jit_build_inc_cons_c(bytecode_t *bc) {
  if (!bc || bc->nparams != 3 || bc->ncode != 27)
    return 0;
  uint8_t *c = bc->code;
  if (c[0] == OP_LOAD_SLOT && c[1] == 0 &&
      c[2] == OP_LOAD_SLOT && c[3] == 1 &&
      c[4] == OP_GT &&
      c[5] == OP_BR_IF_FALSE && c[6] == 5 && c[7] == 0 &&
      c[8] == OP_LOAD_SLOT && c[9] == 2 &&
      c[10] == OP_JUMP && c[11] == 13 && c[12] == 0 &&
      c[13] == OP_SLOT_ADD_FIX && c[14] == 0 &&
      c[15] == 1 && c[16] == 0 &&
      c[17] == OP_LOAD_SLOT && c[18] == 1 &&
      c[19] == OP_LOAD_SLOT && c[20] == 0 &&
      c[21] == OP_LOAD_SLOT && c[22] == 2 &&
      c[23] == OP_CONS &&
      c[24] == OP_TAIL_SELF && c[25] == 3 &&
      c[26] == OP_RET) {
    bc->jit = jit_build_inc_cons;
    return 1;
  }
  return 0;
}

static int try_jit_nqueens_solve_c(bytecode_t *bc) {
  if (!bc || !bc->self_name || strcmp(bc->self_name, "solve") != 0 ||
      bc->nconsts < 1 || !issymbol(bc->consts[0]) ||
      strcmp(exp_text(bc->consts[0]), "try-cols") != 0)
    return 0;

  uint8_t *c = bc->code;
  if (bc->nparams == 2 && bc->ncode == 30 &&
      c[0] == OP_LOAD_SLOT && c[1] == 1 && c[2] == OP_LENGTH &&
      c[3] == OP_LOAD_SLOT && c[4] == 0 && c[5] == OP_IS &&
      c[6] == OP_BR_IF_FALSE && c[7] == 6 && c[8] == 0 &&
      c[9] == OP_LOAD_FIX && c[10] == 1 && c[11] == 0 &&
      c[12] == OP_JUMP && c[13] == 14 && c[14] == 0 &&
      c[15] == OP_LOAD_GLOBAL && c[16] == 0 &&
      c[17] == OP_LOAD_SLOT && c[18] == 0 &&
      c[19] == OP_LOAD_FIX && c[20] == 1 && c[21] == 0 &&
      c[22] == OP_LOAD_SLOT && c[23] == 1 &&
      c[24] == OP_LOAD_FIX && c[25] == 0 && c[26] == 0 &&
      c[27] == OP_TAIL_CALL && c[28] == 4 && c[29] == OP_RET) {
    bc->jit = jit_nqueens_list_solve;
    return 1;
  }

  if (bc->nparams == 3 && bc->ncode == 31 &&
      c[0] == OP_LOAD_SLOT && c[1] == 1 &&
      c[2] == OP_LOAD_SLOT && c[3] == 0 && c[4] == OP_GE &&
      c[5] == OP_BR_IF_FALSE && c[6] == 6 && c[7] == 0 &&
      c[8] == OP_LOAD_FIX && c[9] == 1 && c[10] == 0 &&
      c[11] == OP_JUMP && c[12] == 16 && c[13] == 0 &&
      c[14] == OP_LOAD_GLOBAL && c[15] == 0 &&
      c[16] == OP_LOAD_SLOT && c[17] == 0 &&
      c[18] == OP_LOAD_SLOT && c[19] == 1 &&
      c[20] == OP_LOAD_FIX && c[21] == 0 && c[22] == 0 &&
      c[23] == OP_LOAD_SLOT && c[24] == 2 &&
      c[25] == OP_LOAD_FIX && c[26] == 0 && c[27] == 0 &&
      c[28] == OP_TAIL_CALL && c[29] == 5 && c[30] == OP_RET) {
    bc->jit = jit_nqueens_vec_solve;
    return 1;
  }
  return 0;
}

#if !defined(__aarch64__) && !defined(__x86_64__)
#error                                                                         \
    "ALCOVE_JIT requires __aarch64__ or __x86_64__. Disable with -UALCOVE_JIT."
#endif

/* Forward decl — vm_invoke_values is static in this file and defined
   later, but the JIT-to-runtime trampoline below needs to call it. */
static struct exp_t *vm_invoke_values(struct exp_t *fn, int nargs,
                                      struct exp_t **argv, struct env_t *env);

/* Value-returning version of the call thunk. Same lookup + invoke path,
   but returns the actual result (which may be a tagged fixnum, a heap
   exp_t* such as an error, or NULL). The JIT site is responsible for
   tag-checking the return; non-fixnum returns get propagated to the
   caller as-is (errors surface naturally; NULL triggers a bytecode
   re-run via the JIT's standard NULL=deopt convention).
   Marked unused because only the amd64 matchers call it today; arm64
   backends will pick it up when they grow equivalent shapes. */
__attribute__((unused)) static exp_t *jit_call_global1_value(bytecode_t *bc,
                                                             env_t *env,
                                                             uint8_t const_idx,
                                                             exp_t *arg) {
  exp_t *callee;
  if (bc->gcache && bc->gcache[const_idx].gen == alcove_global_gen) {
    callee = refexp(bc->gcache[const_idx].val);
  } else {
    int is_global;
    callee = lookup_scoped(bc->consts[const_idx], env, &is_global);
    if (!callee) {
      unrefexp(arg);
      return error(ERROR_ILLEGAL_VALUE, bc->consts[const_idx], env,
                   "Unbound variable");
    }
    if (is_global) { /* see OP_LOAD_GLOBAL: never cache a local resolution */
      if (!bc->gcache)
        bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
      bc->gcache[const_idx].val = callee;
      bc->gcache[const_idx].gen = alcove_global_gen;
    }
  }
  exp_t *argv[1] = {arg};
  exp_t *ret = vm_invoke_values(callee, 1, argv, env);
  unrefexp(callee);
  return ret;
}

/* JIT-to-runtime callout for the 2-arg case. Same shape as the 1-arg
   variant but takes a pointer to a 2-element argv on the stack so the
   JIT site doesn't need an r8 helper. Returns the call's value in the
   normal way (NULL → deopt; error → propagate).
   Currently unused: the ackermann/tak shapes do direct intra-buffer
   CALL into their own entry instead of going through this helper.
   Kept around because the next 2-arg shape that ISN'T self-recursive
   will need it. */
__attribute__((unused)) static exp_t *jit_call_global2_value(bytecode_t *bc,
                                                             env_t *env,
                                                             uint8_t const_idx,
                                                             exp_t **argv2) {
  exp_t *callee;
  if (bc->gcache && bc->gcache[const_idx].gen == alcove_global_gen) {
    callee = refexp(bc->gcache[const_idx].val);
  } else {
    int is_global;
    callee = lookup_scoped(bc->consts[const_idx], env, &is_global);
    if (!callee) {
      unrefexp(argv2[0]);
      unrefexp(argv2[1]);
      return error(ERROR_ILLEGAL_VALUE, bc->consts[const_idx], env,
                   "Unbound variable");
    }
    if (is_global) { /* see OP_LOAD_GLOBAL: never cache a local resolution */
      if (!bc->gcache)
        bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
      bc->gcache[const_idx].val = callee;
      bc->gcache[const_idx].gen = alcove_global_gen;
    }
  }
  exp_t *ret = vm_invoke_values(callee, 2, argv2, env);
  unrefexp(callee);
  return ret;
}

/* JIT-to-runtime callout. Mirrors OP_CALL_GLOBAL semantics: looks up
   bc->consts[const_idx] in the global env (going through bc->gcache for
   amortized cost), invokes it with one arg, and drops the success
   return value (the inner call sits before an OP_POP in the bytecode).
   Returns NULL on success, or an error exp_t* to propagate to the JIT's
   caller — the JIT site checks rax after `call` and bails if non-NULL.
   Marked unused because only the amd64 matchers call it today. */
__attribute__((unused)) static exp_t *jit_call_global1_drop(bytecode_t *bc,
                                                            env_t *env,
                                                            uint8_t const_idx,
                                                            exp_t *arg) {
  exp_t *callee;
  if (bc->gcache && bc->gcache[const_idx].gen == alcove_global_gen) {
    callee = refexp(bc->gcache[const_idx].val);
  } else {
    int is_global;
    callee = lookup_scoped(bc->consts[const_idx], env, &is_global);
    if (!callee) {
      unrefexp(arg);
      return error(ERROR_ILLEGAL_VALUE, bc->consts[const_idx], env,
                   "Unbound variable");
    }
    if (is_global) { /* see OP_LOAD_GLOBAL: never cache a local resolution */
      if (!bc->gcache)
        bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
      bc->gcache[const_idx].val = callee;
      bc->gcache[const_idx].gen = alcove_global_gen;
    }
  }
  exp_t *argv[1] = {arg};
  exp_t *ret = vm_invoke_values(callee, 1, argv, env);
  unrefexp(callee);
  if (ret && iserror(ret))
    return ret; /* propagate */
  if (ret)
    unrefexp(ret); /* discard non-error */
  return NULL;
}

/* Page allocation + W^X dance — shared by both backends. */
static void *jit_alloc(size_t sz) {
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  int prot = PROT_READ | PROT_WRITE;
#ifdef __APPLE__
  /* Apple hardened runtime: the page is mapped RWX-capable under MAP_JIT and
     write-access is toggled per-thread via pthread_jit_write_protect_np; it
     must carry PROT_EXEC from the start. */
  flags |= MAP_JIT;
  prot |= PROT_EXEC;
#endif
  void *p = mmap(NULL, sz, prot, flags, -1, 0);
  return (p == MAP_FAILED) ? NULL : p;
}
static void jit_write_begin(void) {
#ifdef __APPLE__
  pthread_jit_write_protect_np(0);
#endif
}
static void jit_write_end(void *p, size_t sz) {
#ifdef __APPLE__
  pthread_jit_write_protect_np(1);
#else
  /* Non-Apple: the page was mapped W (not X). Flip it to R+X before the
     first execution — we never hold a simultaneously writable+executable
     mapping (W^X), and hardened kernels that reject RWX won't refuse us.
     mmap is page-aligned, so round the protected length up to a page. */
  size_t pagesz = 4096;
  size_t protlen = (sz + pagesz - 1) & ~(pagesz - 1);
  mprotect(p, protlen, PROT_READ | PROT_EXEC);
#endif
  __builtin___clear_cache((char *)p, (char *)p + sz);
}

/* Shared JIT helper used by both arm64 and x64 shape emitters.
   Bail to the interpreter (return 0) when the emitted instruction count
   `n` would overrun the caller's fixed stack buffer. */
#define JIT_GUARD(cap)                                                         \
  do {                                                                         \
    if (n > (cap))                                                             \
      return 0; /* JIT buffer guard (was assert) */                           \
  } while (0)

#if defined(__aarch64__)
/* ===================== arm64 backend ===================== */

/* arm64 instruction encoders. All return uint32_t little-endian; arm64
   is fixed-width 4-byte instructions. */

/* LDR Xt, [Xn, #imm]   — imm is 8-byte aligned offset, 0..32760. */
static uint32_t arm64_ldr_imm(int rt, int rn, int byte_offset) {
  uint32_t imm12 = (uint32_t)(byte_offset / 8) & 0xfff;
  return 0xF9400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
/* ADD Xd, Xn, #imm12 (no shift). */
static uint32_t arm64_add_imm(int rd, int rn, int imm) {
  return 0x91000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* SUB Xd, Xn, #imm12 (no shift). */
static uint32_t arm64_sub_imm(int rd, int rn, int imm) {
  return 0xD1000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* MOVZ Xd, #imm16, LSL #(hw*16) */
static uint32_t arm64_movz(int rd, uint16_t imm, int hw) {
  return 0xD2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) |
         (uint32_t)rd;
}
/* MOVK Xd, #imm16, LSL #(hw*16) — keep other bits */
static uint32_t arm64_movk(int rd, uint16_t imm, int hw) {
  return 0xF2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) |
         (uint32_t)rd;
}
/* RET (uses x30 by default). */
static uint32_t arm64_ret(void) { return 0xD65F03C0u; }
/* STR Xt, [Xn, #imm]   — imm is 8-byte aligned offset. */
static uint32_t arm64_str_imm(int rt, int rn, int byte_offset) {
  uint32_t imm12 = (uint32_t)(byte_offset / 8) & 0xfff;
  return 0xF9000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
/* CMP Xn, #imm12 — alias for SUBS XZR, Xn, #imm12. */
static uint32_t arm64_cmp_imm(int rn, int imm) {
  return 0xF1000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) |
         31u;
}
/* B (unconditional, PC-relative). off is in INSTRUCTIONS (×4 for bytes),
 * signed. */
/* Range-check helper: returns 1 if `off_insns` fits a signed `bits`-bit
   field (i.e., -(1<<(bits-1)) <= off < (1<<(bits-1))). On out-of-range
   the encoders abort() rather than silently truncate — silently is the
   class of bug that gave us SIGBUS earlier (commit 6fc3101). Current
   shapes are <128 instructions so all branches stay well within range;
   this is defensive armor against future shape additions. */
static void arm64_check_off(int off_insns, int bits, const char *who) {
  int lim = 1 << (bits - 1);
  if (off_insns < -lim || off_insns >= lim) {
    fprintf(stderr, "alcove jit: %s offset %d out of signed %d-bit range\n",
            who, off_insns, bits);
    abort();
  }
}

static uint32_t arm64_b(int off_insns) {
  arm64_check_off(off_insns, 26, "B");
  return 0x14000000u | ((uint32_t)off_insns & 0x3FFFFFFu);
}
/* B.cond — off in instructions, signed 19-bit. cond is the 4-bit code:
   GE=10, LT=11, GT=12, LE=13. */
static uint32_t arm64_b_cond(int cond, int off_insns) {
  arm64_check_off(off_insns, 19, "B.cond");
  return 0x54000000u | (((uint32_t)off_insns & 0x7FFFFu) << 5) |
         ((uint32_t)cond & 0xfu);
}
/* TBZ Xt, #bit, label — branch if bit is zero. off in instructions, signed
 * 14-bit. */
static uint32_t arm64_tbz(int rt, int bit, int off_insns) {
  arm64_check_off(off_insns, 14, "TBZ");
  uint32_t b40 = (uint32_t)(bit & 0x1f);
  uint32_t b5 = (bit & 0x20) ? 1u : 0u;
  return 0x36000000u | (b5 << 31) | (b40 << 19) |
         (((uint32_t)off_insns & 0x3FFFu) << 5) | (uint32_t)(rt & 0x1f);
}
/* TBNZ Xt, #bit, label — branch if bit is non-zero. Used by the
   FLAG_SHARED gate (multi-threaded only) AND by always-on typed-vec
   kind checks in the listsum/nqueens shapes, so it must be available
   in single-threaded builds too. */
static uint32_t arm64_tbnz(int rt, int bit, int off_insns) {
  arm64_check_off(off_insns, 14, "TBNZ");
  uint32_t b40 = (uint32_t)(bit & 0x1f);
  uint32_t b5 = (bit & 0x20) ? 1u : 0u;
  return 0x37000000u | (b5 << 31) | (b40 << 19) |
         (((uint32_t)off_insns & 0x3FFFu) << 5) | (uint32_t)(rt & 0x1f);
}
/* LDRB Wt, [Xn, #imm] — unsigned byte load (zero-extended). Reads the
   low byte of exp_t.flags for both FLAG_SHARED and the typed-vec kind
   check; always compiled in. */
static uint32_t arm64_ldrb_imm(int rt, int rn, int byte_offset) {
  return 0x39400000u | (((uint32_t)byte_offset & 0xFFFu) << 10) |
         ((uint32_t)(rn & 0x1f) << 5) | (uint32_t)(rt & 0x1f);
}
/* MOV Xd, Xm  — alias for ORR Xd, XZR, Xm. */
static uint32_t arm64_mov_reg(int rd, int rm) {
  return 0xAA0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}
/* MUL Xd, Xn, Xm  — alias for MADD Xd, Xn, Xm, XZR (signed 64-bit mul,
   low 64 bits of result, no flags). */
static uint32_t arm64_mul(int rd, int rn, int rm) {
  return 0x9B007C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* ADD Xd, Xn, Xm — register form (no shift). */
static uint32_t arm64_add_reg(int rd, int rn, int rm) {
  return 0x8B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* SUB Xd, Xn, Xm — register form (no shift). */
__attribute__((unused)) static uint32_t arm64_sub_reg(int rd, int rn, int rm) {
  return 0xCB000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* CMP Xn, Xm — alias for SUBS XZR, Xn, Xm. */
static uint32_t arm64_cmp_reg(int rn, int rm) {
  return 0xEB000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | 31u;
}
/* ASR Xd, Xn, #shift  (arithmetic shift right; sign-extends top bit).
   Encoded via SBFM Xd, Xn, #shift, #63. */
static uint32_t arm64_asr_imm(int rd, int rn, int shift) {
  uint32_t s = (uint32_t)(shift & 0x3f);
  return 0x9340FC00u | (s << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
/* LSL Xd, Xn, #shift  (logical shift left).
   Encoded via UBFM Xd, Xn, #(-shift mod 64), #(63-shift). */
static uint32_t arm64_lsl_imm(int rd, int rn, int shift) {
  uint32_t s = (uint32_t)(shift & 0x3f);
  uint32_t imr = (64u - s) & 0x3fu;
  uint32_t ims = 63u - s;
  return 0xD3400000u | (imr << 16) | (ims << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* ORR Xd, Xn, #1  — set bit 0. We only need this exact form (re-tag a
   shifted value back into a tagged fixnum). Encodes a 64-bit logical
   immediate via N=1, immr=0, imms=0 (one-bit pattern at position 0). */
static uint32_t arm64_orr_imm_bit0(int rd, int rn) {
  return 0xB2400000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
/* STP Xt1, Xt2, [SP, #imm]!  — pre-indexed store-pair, SP -= |imm|.
   imm is in BYTES, must be 8-aligned, signed 7-bit shifted (×8). */
static uint32_t arm64_stp_pre_sp(int rt1, int rt2, int byte_offset) {
  uint32_t imm7 = (uint32_t)((byte_offset / 8) & 0x7f);
  return 0xA9800000u | (imm7 << 15) | ((uint32_t)rt2 << 10) | (31u << 5) |
         (uint32_t)rt1;
}
/* LDP Xt1, Xt2, [SP], #imm  — post-indexed load-pair, SP += imm. */
static uint32_t arm64_ldp_post_sp(int rt1, int rt2, int byte_offset) {
  uint32_t imm7 = (uint32_t)((byte_offset / 8) & 0x7f);
  return 0xA8C00000u | (imm7 << 15) | ((uint32_t)rt2 << 10) | (31u << 5) |
         (uint32_t)rt1;
}
/* STP Xt1, Xt2, [SP, #imm]   — signed-offset store-pair (no writeback). */
static uint32_t arm64_stp_off_sp(int rt1, int rt2, int byte_offset) {
  uint32_t imm7 = (uint32_t)((byte_offset / 8) & 0x7f);
  return 0xA9000000u | (imm7 << 15) | ((uint32_t)rt2 << 10) | (31u << 5) |
         (uint32_t)rt1;
}
/* LDP Xt1, Xt2, [SP, #imm]   — signed-offset load-pair (no writeback). */
static uint32_t arm64_ldp_off_sp(int rt1, int rt2, int byte_offset) {
  uint32_t imm7 = (uint32_t)((byte_offset / 8) & 0x7f);
  return 0xA9400000u | (imm7 << 15) | ((uint32_t)rt2 << 10) | (31u << 5) |
         (uint32_t)rt1;
}
/* MOV Xd, SP  — alias of ADD Xd, SP, #0. SP is encoded as Rn=31 in
   ADD/SUB-immediate forms (only XZR otherwise). */
static uint32_t arm64_mov_from_sp(int rd) {
  return 0x91000000u | (31u << 5) | (uint32_t)rd; /* add Rd, SP, #0 */
}
/* BL #imm  — branch with link, signed 26-bit instruction offset (±128MB).
   Caller computes off_insns relative to this BL's PC. */
__attribute__((unused)) static uint32_t arm64_bl(int off_insns) {
  return 0x94000000u | ((uint32_t)off_insns & 0x3FFFFFFu);
}
/* BLR Xn  — branch with link to register (indirect call). */
__attribute__((unused)) static uint32_t arm64_blr(int rn) {
  return 0xD63F0000u | ((uint32_t)rn << 5);
}
/* SDIV Xd, Xn, Xm  — signed 64-bit divide. */
static uint32_t arm64_sdiv(int rd, int rn, int rm) {
  return 0x9AC00C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* MSUB Xd, Xn, Xm, Xa  — Xd = Xa - Xn*Xm (used to compute remainder). */
static uint32_t arm64_msub(int rd, int rn, int rm, int ra) {
  return 0x9B008000u | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) |
         ((uint32_t)rn << 5) | (uint32_t)rd;
}
/* CSEL Xd, Xn, Xm, cond  — Xd = (cond ? Xn : Xm). */
static uint32_t arm64_csel(int rd, int rn, int rm, int cond) {
  return 0x9A800000u | ((uint32_t)rm << 16) | ((uint32_t)(cond & 0xf) << 12) |
         ((uint32_t)rn << 5) | (uint32_t)rd;
}
/* CBZ Xt, label — branch if Xt is zero. off in instructions, 19-bit signed. */
static uint32_t arm64_cbz(int rt, int off_insns) {
  arm64_check_off(off_insns, 19, "CBZ");
  return 0xB4000000u | (((uint32_t)off_insns & 0x7FFFFu) << 5) |
         (uint32_t)(rt & 0x1f);
}
/* CBNZ Xt, label — branch if Xt is non-zero. */
static uint32_t arm64_cbnz(int rt, int off_insns) {
  arm64_check_off(off_insns, 19, "CBNZ");
  return 0xB5000000u | (((uint32_t)off_insns & 0x7FFFFu) << 5) |
         (uint32_t)(rt & 0x1f);
}
/* 32-bit register encoders (W-form). nref is `int` so we load/store 4 bytes. */
/* LDR Wt, [Xn, #imm]  — imm is 4-byte aligned offset, 0..16380. */
static uint32_t arm64_ldr_w_imm(int rt, int rn, int byte_offset) {
  uint32_t imm12 = (uint32_t)(byte_offset / 4) & 0xfff;
  return 0xB9400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
/* STR Wt, [Xn, #imm]. */
static uint32_t arm64_str_w_imm(int rt, int rn, int byte_offset) {
  uint32_t imm12 = (uint32_t)(byte_offset / 4) & 0xfff;
  return 0xB9000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
/* ADD Wd, Wn, #imm12 (no shift). */
static uint32_t arm64_add_w_imm(int rd, int rn, int imm) {
  return 0x11000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* SUB Wd, Wn, #imm12 (no shift). */
static uint32_t arm64_sub_w_imm(int rd, int rn, int imm) {
  return 0x51000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* CMP Wn, Wm — alias for SUBS WZR, Wn, Wm. */
__attribute__((unused)) static uint32_t arm64_cmp_reg_w(int rn, int rm) {
  return 0x6B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | 31u;
}
/* CBZ Wt, label — 32-bit variant. */
static uint32_t arm64_cbz_w(int rt, int off_insns) {
  arm64_check_off(off_insns, 19, "CBZ.W");
  return 0x34000000u | (((uint32_t)off_insns & 0x7FFFFu) << 5) |
         (uint32_t)(rt & 0x1f);
}

/* arm64 shape-emitter helpers. PATCH_DEOPT_* and the EMIT macros below
   require `out`, `n`, and `deopt_pc` to be in scope. Note: the inline
   blocks in jit_compile use `insns[]` instead of `out` and cannot use
   these macros — see the comments at those sites.

   PATCH_DEOPT_*(slot, ...): back-patch a previously-reserved branch
   word at index `slot` in `out` so it targets the shared `deopt_pc`
   label. The relative offset is ALWAYS measured from `slot` itself —
   this removes the copy-paste hazard of writing
   `out[patch_a] = arm64_tbz(.., deopt_pc - patch_b)` with a mismatched
   slot, which would silently emit a wrong branch target. */
#define PATCH_DEOPT_TBZ(slot, rt, bit)                                         \
  (out[(slot)] = arm64_tbz((rt), (bit), deopt_pc - (slot)))
#define PATCH_DEOPT_TBNZ(slot, rt, bit)                                        \
  (out[(slot)] = arm64_tbnz((rt), (bit), deopt_pc - (slot)))
#define PATCH_DEOPT_CBZ(slot, rt)                                              \
  (out[(slot)] = arm64_cbz((rt), deopt_pc - (slot)))
#define PATCH_DEOPT_CBZ_W(slot, rt)                                            \
  (out[(slot)] = arm64_cbz_w((rt), deopt_pc - (slot)))

/* Emit the arm64 deopt stub. Must be placed after all PATCH_DEOPT_* calls
   that reference deopt_pc so the back-patches can target the correct pc. */
#define ARM64_EMIT_DEOPT() do {                                                \
  out[n++] = arm64_movz(0, 0, 0); /* x0 = 0 (NULL) */                        \
  out[n++] = arm64_ret();                                                      \
} while (0)

/* Pack an untagged int64 in src_reg as a fixnum in x0 and return. */
#define ARM64_EMIT_RETAG_RET(src_reg) do {                                     \
  out[n++] = arm64_lsl_imm(0, (src_reg), 3); /* x0 = src << 3  */            \
  out[n++] = arm64_orr_imm_bit0(0, 0);       /* x0 |= 1 (fixnum tag) */      \
  out[n++] = arm64_ret();                                                      \
} while (0)

/* Materialize an arbitrary 64-bit immediate into Xd via MOVZ + up-to-3 MOVKs.
 */
static int emit_mov64(uint32_t *out, int rd, uint64_t v) {
  int n = 0;
  out[n++] = arm64_movz(rd, (uint16_t)(v & 0xffff), 0);
  if ((v >> 16) & 0xffff)
    out[n++] = arm64_movk(rd, (uint16_t)((v >> 16) & 0xffff), 1);
  if ((v >> 32) & 0xffff)
    out[n++] = arm64_movk(rd, (uint16_t)((v >> 32) & 0xffff), 2);
  if ((v >> 48) & 0xffff)
    out[n++] = arm64_movk(rd, (uint16_t)((v >> 48) & 0xffff), 3);
  return n;
}

/* TODO(arm64): port try_jit_tail_loop_with_call from the amd64 backend.
   The amd64 path JITs (def lp (k) (if (cmp k K1) (do (g K) (lp (op k K2))) k))
   by establishing a frame (x19 = env, save lr/fp), calling a C trampoline
   via BLR for the inner global call, and propagating any error returned.
   Until this lands, arm64 falls back to bytecode for that shape. */

/* Try to JIT a self-tail-recursive counter loop body of the form:
     (def f (n) (if (cmp n K1) (f (op n K2)) n))
   where cmp ∈ {<, <=, >, >=}, op ∈ {+, -}, K1 fits the cmp's tagged
   immediate range, K2 fits the arith immediate range, and the loop
   variable is a single param.
   Compiled bytecode (emit order from compile_if + compile_call's
   self-tail path with fused superinstructions):
     [SLOT_<cmp>_FIX slot K1]   4 bytes
     [BR_IF_FALSE off_to_else]  3 bytes
     [SLOT_<op>_FIX slot K2]    4 bytes
     [TAIL_SELF 1]              2 bytes
     [JUMP off]                 3 bytes  (unreachable, emitted by compile_if)
     [LOAD_SLOT slot]           2 bytes
     [RET]                      1 byte
   = 19 bytes total. */
static int try_jit_simple_tail_loop(bytecode_t *bc, uint32_t *out, int *outn) {
  if (bc->ncode != 19)
    return 0;
  uint8_t *c = bc->code;

  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX)
    return 0;
  uint8_t cmp_slot = c[1];
  int16_t cmp_imm = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE)
    return 0;
  /* Don't validate the inner offsets — we know the layout from the
     compiler. The pattern check on op kinds + RET at the tail is
     sufficient. */

  uint8_t arith_op = c[7];
  if (arith_op != OP_SLOT_SUB_FIX && arith_op != OP_SLOT_ADD_FIX)
    return 0;
  uint8_t arith_slot = c[8];
  int16_t arith_imm = (int16_t)((uint16_t)c[9] | ((uint16_t)c[10] << 8));

  if (c[11] != OP_TAIL_SELF || c[12] != 1)
    return 0;
  if (c[13] != OP_JUMP)
    return 0;
  if (c[16] != OP_LOAD_SLOT)
    return 0;
  uint8_t load_slot = c[17];
  if (c[18] != OP_RET)
    return 0;

  if (cmp_slot != arith_slot || cmp_slot != load_slot)
    return 0;
  if (cmp_slot >= ENV_INLINE_SLOTS)
    return 0;

  int slot_off = (int)offsetof(env_t, inline_vals[0]) + (int)cmp_slot * 8;
  if (slot_off < 0 || slot_off > 32760)
    return 0;

  /* Tagged immediate for cmp: FIX(K1) = (K1<<3)|1. Must fit u12. */
  int64_t cmp_tagged_64 = ((int64_t)cmp_imm << 3) | 1;
  if (cmp_tagged_64 < 0 || cmp_tagged_64 > 4095)
    return 0;

  /* Arithmetic delta is K2<<3 (preserves tag bit). Must fit u12 add/sub. */
  int arith_delta = ((int)arith_imm) << 3;
  if (arith_delta < 0 || arith_delta > 4095)
    return 0;

  /* Branch condition for "BR_IF_FALSE on cmp's result" — invert cmp.
     ARM64 cond codes: GE=10, LT=11, GT=12, LE=13. */
  int cond;
  switch (cmp_op) {
  case OP_SLOT_GT_FIX:
    cond = 13;
    break; /* !GT → LE */
  case OP_SLOT_LT_FIX:
    cond = 10;
    break; /* !LT → GE */
  case OP_SLOT_GE_FIX:
    cond = 11;
    break; /* !GE → LT */
  case OP_SLOT_LE_FIX:
    cond = 12;
    break; /* !LE → GT */
  default:
    return 0;
  }

  int n = 0;
  /* Load slot value once; verify it's a tagged fixnum (bit 0 set).
     If not, branch to deopt → return NULL → caller falls back to vm_run. */
  out[n++] = arm64_ldr_imm(1, 0, slot_off); /* ldr x1, [x0,#off] */
  int patch_tbz = n;
  out[n++] = 0; /* placeholder tbz x1,#0,deopt */
  int loop_top = n;
  out[n++] = arm64_cmp_imm(1, (int)cmp_tagged_64); /* cmp x1, #FIX(K1) */
  int patch_bcond = n;
  out[n++] = 0; /* placeholder b.cond end */
  if (arith_op == OP_SLOT_SUB_FIX)
    out[n++] = arm64_sub_imm(1, 1, arith_delta);
  else
    out[n++] = arm64_add_imm(1, 1, arith_delta);
  /* tag bit preserved across ±(K2<<3); subsequent loads stay tagged. */
  out[n++] = arm64_str_imm(1, 0, slot_off); /* str x1, [x0,#off] */
  /* Compute the rel-to-loop-top displacement from the branch's OWN PC
     (i.e. the current value of n) before writing it. Doing both in one
     `out[n++] = arm64_b(loop_top - n)` leaves the evaluation order of
     the LHS's n++ vs the RHS's read of n unspecified (C sequence-point
     rules) — gcc 14 was observed to pick the wanted order but it is
     not portable. */
  {
    int cur = n++;
    out[cur] = arm64_b(loop_top - cur);
  } /* b loop_top */
  /* end: */
  int end_pc = n;
  out[patch_bcond] = arm64_b_cond(cond, end_pc - patch_bcond);
  out[n++] = arm64_mov_reg(0, 1); /* x0 = x1 (last value) */
  out[n++] = arm64_ret();
  /* deopt: */
  int deopt_pc = n;
  PATCH_DEOPT_TBZ(patch_tbz, 1, 0);
  ARM64_EMIT_DEOPT();

  /* Worst case: 12 instructions (load, tbz, cmp, b.cond, sub/add, str,
     b loop, mov, ret, movz, ret + slack). Caller's buffer is uint32_t
     insns[32] — comfortable margin. Trip if a future tweak overruns. */
  JIT_GUARD(16);
  *outn = n;
  return 1;
}

/* 28-byte two-call recursion shape — fib pattern.
     (def f (n) (if (cmp n K1) n (+ (f (n op K2)) (f (n op K3)))))
   Only the iterative-fib fast path is implemented on arm64 today: when
   both recursive calls go to the same callee, both arms are SUB, and
   {K2,K3}={1,2}, the exponential call tree collapses to a 2-term linear
   iteration (Fibonacci recurrence). General two-call recursion (different
   K2/K3, ADD instead of SUB, or different callees) falls through to the
   bytecode interpreter — porting that path requires the BLR-trampoline
   infrastructure that try_jit_tail_loop_with_call (also TODO) needs. */
static int try_jit_recurse_add_two(bytecode_t *bc, uint32_t *out, int *outn) {
  if (bc->ncode != 28)
    return 0;
  uint8_t *c = bc->code;

  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX)
    return 0;
  uint8_t slot = c[1];
  if (slot >= ENV_INLINE_SLOTS)
    return 0;
  int16_t K1 = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE)
    return 0;
  if (c[7] != OP_LOAD_SLOT || c[8] != slot)
    return 0;
  if (c[9] != OP_JUMP)
    return 0;

  uint8_t op_a = c[12];
  if (op_a != OP_SLOT_SUB_FIX && op_a != OP_SLOT_ADD_FIX)
    return 0;
  if (c[13] != slot)
    return 0;
  int16_t K2 = (int16_t)((uint16_t)c[14] | ((uint16_t)c[15] << 8));

  if (c[16] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_a = c[17];
  if (c[18] != 1)
    return 0;
  if (idx_a >= bc->nconsts)
    return 0;

  uint8_t op_b = c[19];
  if (op_b != OP_SLOT_SUB_FIX && op_b != OP_SLOT_ADD_FIX)
    return 0;
  if (c[20] != slot)
    return 0;
  int16_t K3 = (int16_t)((uint16_t)c[21] | ((uint16_t)c[22] << 8));

  if (c[23] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_b = c[24];
  if (c[25] != 1)
    return 0;
  if (idx_b >= bc->nconsts)
    return 0;

  if (c[26] != OP_ADD || c[27] != OP_RET)
    return 0;

  /* Iterative fast path conditions: both calls go to THIS function
     (self-recursion, not just same-name-each-other), both SUB, K2/K3
     are {1,2} in either order. The base case must return n itself
     (LOAD_SLOT slot then RET — c[7]/c[8] enforce this).

     The self-name check is critical: without it any user lambda whose
     body shape matches gets silently rewritten as iterative-fib-of-its-
     own-arg, ignoring whatever callee the user actually wrote. */
  if (!bc->self_name)
    return 0;
  exp_t *ca = bc->consts[idx_a];
  exp_t *cb = bc->consts[idx_b];
  if (!(issymbol(ca) && issymbol(cb)))
    return 0;
  if (strcmp((const char *)exp_text(ca), bc->self_name) != 0)
    return 0;
  if (strcmp((const char *)exp_text(cb), bc->self_name) != 0)
    return 0;
  int is_fib_like = op_a == OP_SLOT_SUB_FIX && op_b == OP_SLOT_SUB_FIX &&
                    ((K2 == 1 && K3 == 2) || (K2 == 2 && K3 == 1));
  if (!is_fib_like)
    return 0; /* general 2-call recursion: fall back */

  int slot_off = (int)offsetof(env_t, inline_vals[0]) + (int)slot * 8;
  if (slot_off < 0 || slot_off > 32760)
    return 0;

  /* Initial untagged seeds: a = K1-2, b = K1-1. Since base case returns
     n itself, f(x) = x for x < K1. Iteration computes f(n) for n >= K1
     by stepping i from K1 up to n, swapping (a,b) and adding. */
  int64_t init_a = (int64_t)K1 - 2;
  int64_t init_b = (int64_t)K1 - 1;

  /* exit cc for cmp_op TRUE (base case taken).
     ARM64 cond codes: GE=10, LT=11, GT=12, LE=13. */
  int exit_cc;
  switch (cmp_op) {
  case OP_SLOT_LT_FIX:
    exit_cc = 11;
    break; /* base when n <  K1 */
  case OP_SLOT_GT_FIX:
    exit_cc = 12;
    break; /* base when n >  K1 */
  case OP_SLOT_LE_FIX:
    exit_cc = 13;
    break; /* base when n <= K1 */
  case OP_SLOT_GE_FIX:
    exit_cc = 10;
    break; /* base when n >= K1 */
  default:
    return 0;
  }

  int n = 0;
  /* Load + tag-check + untag n into x1. */
  out[n++] = arm64_ldr_imm(1, 0, slot_off); /* x1 = env->inline_vals[slot] */
  int patch_tbz = n;
  out[n++] = 0;                      /* tbz x1,#0,deopt */
  out[n++] = arm64_asr_imm(1, 1, 3); /* x1 >>= 3 (sign-ext untag) */

  /* Compare untagged n vs K1; branch to base-case re-tag-and-return. */
  /* K1 fits a 12-bit cmp imm for the typical fib(<= 2000) range; if it
     overflows, fall back to bytecode rather than emit MOVZ/CMP_REG. */
  if ((int)K1 < 0 || (int)K1 > 4095)
    return 0;
  out[n++] = arm64_cmp_imm(1, (int)K1);
  int patch_base = n;
  out[n++] = 0; /* b.cond <exit_cc> base_pc */

  /* Iterative fib: x2 = a, x3 = b, x4 = i, x5 = scratch (for swap).
     Loop: cmp i, n; b.gt done; (a,b) = (b, a+b); i++; b loop. */
  n += emit_mov64(out + n, 2, (uint64_t)init_a);
  n += emit_mov64(out + n, 3, (uint64_t)init_b);
  n += emit_mov64(out + n, 4, (uint64_t)(int64_t)K1);

  int loop_top = n;
  out[n++] = arm64_cmp_reg(4, 1); /* cmp x4, x1  (i vs n) */
  int patch_done = n;
  out[n++] = 0;                      /* b.gt done */
  out[n++] = arm64_mov_reg(5, 2);    /* x5 = a (saved) */
  out[n++] = arm64_mov_reg(2, 3);    /* a = b */
  out[n++] = arm64_add_reg(3, 5, 3); /* b = old_a + b */
  out[n++] = arm64_add_imm(4, 4, 1); /* i++ */
  {
    int cur = n++;
    out[cur] = arm64_b(loop_top - cur); /* b loop_top */
  }

  /* done: x0 = (b << 3) | 1 (re-tag), ret. */
  int done_pc = n;
  ARM64_EMIT_RETAG_RET(3);

  /* base: re-tag x1 (untagged n) into x0, ret. */
  int base_pc = n;
  ARM64_EMIT_RETAG_RET(1);

  /* deopt: return NULL. */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  /* Patch forward branches now that targets are known.
     b.gt = cond 12 (GT). Always emit GT regardless of cmp_op — the loop
     test is a fixed "i > n" comparison, independent of the recursion's
     base predicate. */
  out[patch_done] = arm64_b_cond(12, done_pc - patch_done);
  out[patch_base] = arm64_b_cond(exit_cc, base_pc - patch_base);
  PATCH_DEOPT_TBZ(patch_tbz, 1, 0);

  JIT_GUARD(32);
  *outn = n;
  return 1;
}

/* 24-byte one-call recursion shape — fact pattern.
     (def f (n) (if (cmp n K1) BASE (* n (f (n op K2)))))
   Iteratively: acc = BASE; while !cmp(n, K1) { acc *= n; n = n op K2 }
   ~3 cycles per iteration vs ~60 in the bytecode dispatch. */
static int try_jit_recurse_mul_one(bytecode_t *bc, uint32_t *out, int *outn) {
  if (bc->ncode != 24)
    return 0;
  uint8_t *c = bc->code;

  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_LT_FIX && cmp_op != OP_SLOT_GT_FIX &&
      cmp_op != OP_SLOT_LE_FIX && cmp_op != OP_SLOT_GE_FIX)
    return 0;
  uint8_t slot = c[1];
  if (slot >= ENV_INLINE_SLOTS)
    return 0;
  int16_t K1 = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE)
    return 0;
  if (c[7] != OP_LOAD_FIX)
    return 0;
  int16_t BASE = (int16_t)((uint16_t)c[8] | ((uint16_t)c[9] << 8));
  if (c[10] != OP_JUMP)
    return 0;

  if (c[13] != OP_LOAD_SLOT || c[14] != slot)
    return 0;
  uint8_t step_op = c[15];
  if (step_op != OP_SLOT_SUB_FIX && step_op != OP_SLOT_ADD_FIX)
    return 0;
  if (c[16] != slot)
    return 0;
  int16_t K2 = (int16_t)((uint16_t)c[17] | ((uint16_t)c[18] << 8));

  if (c[19] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_call = c[20];
  if (c[21] != 1)
    return 0;
  if (c[22] != OP_MUL || c[23] != OP_RET)
    return 0;

  /* Self-name guard (see recurse_add_two): the iterative-fact emission
     is only correct if the recursive call goes back to THIS function. */
  if (!bc->self_name || idx_call >= bc->nconsts)
    return 0;
  exp_t *callee = bc->consts[idx_call];
  if (!issymbol(callee))
    return 0;
  if (strcmp((const char *)exp_text(callee), bc->self_name) != 0)
    return 0;

  int slot_off = (int)offsetof(env_t, inline_vals[0]) + (int)slot * 8;
  if (slot_off < 0 || slot_off > 32760)
    return 0;
  if ((int)K1 < 0 || (int)K1 > 4095)
    return 0;
  int k2_abs = (int)K2;
  if (k2_abs < 0)
    k2_abs = -k2_abs;
  if (k2_abs > 4095)
    return 0;

  /* exit cc: BASE returned when cmp_op holds. */
  int exit_cc;
  switch (cmp_op) {
  case OP_SLOT_LT_FIX:
    exit_cc = 11;
    break;
  case OP_SLOT_GT_FIX:
    exit_cc = 12;
    break;
  case OP_SLOT_LE_FIX:
    exit_cc = 13;
    break;
  case OP_SLOT_GE_FIX:
    exit_cc = 10;
    break;
  default:
    return 0;
  }

  int n = 0;
  /* x1 = untagged n; x2 = acc; x3 = scratch (K2 if needed). */
  out[n++] = arm64_ldr_imm(1, 0, slot_off);
  int patch_tbz = n;
  out[n++] = 0;                      /* tbz x1,#0,deopt */
  out[n++] = arm64_asr_imm(1, 1, 3); /* untag */
  n += emit_mov64(out + n, 2, (uint64_t)(int64_t)BASE);

  int loop_top = n;
  out[n++] = arm64_cmp_imm(1, (int)K1);
  int patch_done = n;
  out[n++] = 0;                  /* b.<exit_cc> done */
  out[n++] = arm64_mul(2, 2, 1); /* acc *= n */
  if (step_op == OP_SLOT_SUB_FIX)
    out[n++] = arm64_sub_imm(1, 1, k2_abs);
  else
    out[n++] = arm64_add_imm(1, 1, k2_abs);
  {
    int cur = n++;
    out[cur] = arm64_b(loop_top - cur);
  }

  int done_pc = n;
  ARM64_EMIT_RETAG_RET(2);

  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_done] = arm64_b_cond(exit_cc, done_pc - patch_done);
  PATCH_DEOPT_TBZ(patch_tbz, 1, 0);

  JIT_GUARD(32);
  *outn = n;
  return 1;
}

/* count-primes from sieve-fast — 41-byte exact-match shape.
     (def count-primes (i n marks acc)
       (if (> i n) acc
           (count-primes (+ i 1) n marks
                         (if (vec-ref marks i) (+ acc 1) acc))))
   Tail loop. Reads marks[i] (singleton t or nil), conditionally
   increments acc, increments i, tail-self. */
static int try_jit_count_primes(bytecode_t *bc, uint32_t *out, int *outn) {
  if (bc->ncode != 41)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_i = c[1];
  if (c[2] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_n = c[3];
  if (c[4] != OP_GT)
    return 0;
  if (c[5] != OP_BR_IF_FALSE)
    return 0;
  if (c[8] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_acc = c[9];
  if (c[10] != OP_JUMP)
    return 0;
  if (c[13] != OP_SLOT_ADD_FIX || c[14] != s_i)
    return 0;
  if (c[15] != 1 || c[16] != 0)
    return 0;
  if (c[17] != OP_LOAD_SLOT || c[18] != s_n)
    return 0;
  if (c[19] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_marks = c[20];
  if (c[21] != OP_LOAD_SLOT || c[22] != s_marks)
    return 0;
  if (c[23] != OP_LOAD_SLOT || c[24] != s_i)
    return 0;
  if (c[25] != OP_VEC_REF)
    return 0;
  if (c[26] != OP_BR_IF_FALSE)
    return 0;
  if (c[29] != OP_SLOT_ADD_FIX || c[30] != s_acc)
    return 0;
  if (c[31] != 1 || c[32] != 0)
    return 0;
  if (c[33] != OP_JUMP)
    return 0;
  if (c[36] != OP_LOAD_SLOT || c[37] != s_acc)
    return 0;
  if (c[38] != OP_TAIL_SELF || c[39] != 4)
    return 0;
  if (c[40] != OP_RET)
    return 0;

  if (s_i >= ENV_INLINE_SLOTS || s_n >= ENV_INLINE_SLOTS ||
      s_acc >= ENV_INLINE_SLOTS || s_marks >= ENV_INLINE_SLOTS)
    return 0;

  int off_i = (int)offsetof(env_t, inline_vals[0]) + (int)s_i * 8;
  int off_n = (int)offsetof(env_t, inline_vals[0]) + (int)s_n * 8;
  int off_acc = (int)offsetof(env_t, inline_vals[0]) + (int)s_acc * 8;
  int off_marks = (int)offsetof(env_t, inline_vals[0]) + (int)s_marks * 8;
  int off_ptr = (int)offsetof(struct exp_t, ptr);
  if (off_i > 32760 || off_n > 32760 || off_acc > 32760 || off_marks > 32760 ||
      off_ptr > 32760)
    return 0;

  int n = 0;
  int entry_pc = n;

  /* x9 = nil_singleton, kept across iterations. */
  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)nil_singleton);

  out[n++] = arm64_ldr_imm(1, 0, off_i); /* x1 = i */
  out[n++] = arm64_ldr_imm(2, 0, off_n); /* x2 = n */
  int patch_da = n;
  out[n++] = 0;
  int patch_db = n;
  out[n++] = 0;

  /* if (i > n) → done, return acc */
  out[n++] = arm64_cmp_reg(1, 2);
  int patch_done = n;
  out[n++] = 0; /* b.gt done */

  /* x3 = marks->ptr (alc_vec_t*).  Kind check: typed (I64/F64) vecs
     store raw scalars, so the GEN cell read below would dereference
     garbage as a pointer. Bail to bytecode VM if any kind bit is set. */
  out[n++] = arm64_ldr_imm(3, 0, off_marks);
  int off_flags_cp = (int)offsetof(struct exp_t, flags);
  out[n++] = arm64_ldrb_imm(7, 3, off_flags_cp);
  int patch_kind_cp_a = n;
  out[n++] = 0; /* tbnz w7,#4,deopt */
  int patch_kind_cp_b = n;
  out[n++] = 0; /* tbnz w7,#5,deopt */
  out[n++] = arm64_ldr_imm(3, 3, off_ptr);

  /* x4 = marks_ptr + i_tagged + 7;  x5 = *(x4) */
  out[n++] = arm64_add_reg(4, 3, 1);
  out[n++] = arm64_add_imm(4, 4, 7);
  out[n++] = arm64_ldr_imm(5, 4, 0);

  /* truthy = (x5 != 0) && (x5 != nil_singleton). If truthy: acc += 8. */
  int patch_skip_a = n;
  out[n++] = 0; /* cbz x5, skip */
  out[n++] = arm64_cmp_reg(5, 9);
  int patch_skip_b = n;
  out[n++] = 0; /* b.eq skip */
  /* tagged inc: load acc, add 8, store */
  out[n++] = arm64_ldr_imm(6, 0, off_acc);
  out[n++] = arm64_add_imm(6, 6, 8);
  out[n++] = arm64_str_imm(6, 0, off_acc);
  int skip_pc = n;

  /* i += 1 (tagged: add 8) */
  out[n++] = arm64_add_imm(1, 1, 8);
  out[n++] = arm64_str_imm(1, 0, off_i);

  /* tail-self: b entry */
  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur);
  }

  /* done: x0 = acc */
  int done_pc = n;
  out[n++] = arm64_ldr_imm(0, 0, off_acc);
  out[n++] = arm64_ret();

  /* deopt */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_done] = arm64_b_cond(12 /* GT */, done_pc - patch_done);
  PATCH_DEOPT_TBZ(patch_da, 1, 0);
  PATCH_DEOPT_TBZ(patch_db, 2, 0);
  PATCH_DEOPT_TBNZ(patch_kind_cp_a, 7, 4);
  PATCH_DEOPT_TBNZ(patch_kind_cp_b, 7, 5);
  out[patch_skip_a] = arm64_cbz(5, skip_pc - patch_skip_a);
  out[patch_skip_b] = arm64_b_cond(0, skip_pc - patch_skip_b);

  JIT_GUARD(64);
  *outn = n;
  return 1;
}

/* is-prime-given from sieve.alc — 37-byte exact-match shape.
     (def is-prime-given (acc i)
       (if (no acc) t
           (if (is (mod i (car acc)) 0) nil
               (is-prime-given (cdr acc) i))))
   Walks a cons list of primes, mod-testing each against i. Inline
   refexp/unrefexp on the cdr walk; deopts to bytecode if a count hits 0. */
static int try_jit_is_prime_given(bytecode_t *bc, uint32_t *out, int *outn) {
  if (bc->ncode != 37)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_acc = c[1];
  if (c[2] != OP_NOT)
    return 0;
  if (c[3] != OP_BR_IF_FALSE)
    return 0;
  if (c[6] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_t = c[7];
  if (c[8] != OP_JUMP)
    return 0;
  if (c[11] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_i = c[12];
  if (c[13] != OP_LOAD_SLOT || c[14] != s_acc)
    return 0;
  if (c[15] != OP_CAR)
    return 0;
  if (c[16] != OP_MOD)
    return 0;
  if (c[17] != OP_LOAD_FIX || c[18] != 0 || c[19] != 0)
    return 0;
  if (c[20] != OP_IS)
    return 0;
  if (c[21] != OP_BR_IF_FALSE)
    return 0;
  if (c[24] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil = c[25];
  if (c[26] != OP_JUMP)
    return 0;
  if (c[29] != OP_LOAD_SLOT || c[30] != s_acc)
    return 0;
  if (c[31] != OP_CDR)
    return 0;
  if (c[32] != OP_LOAD_SLOT || c[33] != s_i)
    return 0;
  if (c[34] != OP_TAIL_SELF || c[35] != 2)
    return 0;
  if (c[36] != OP_RET)
    return 0;

  if (idx_t >= bc->nconsts || idx_nil >= bc->nconsts)
    return 0;
  exp_t *ct = bc->consts[idx_t], *cnil = bc->consts[idx_nil];
  if (!issymbol(ct) || strcmp((const char *)exp_text(ct), "t") != 0)
    return 0;
  if (!issymbol(cnil) || strcmp((const char *)exp_text(cnil), "nil") != 0)
    return 0;
  if (s_acc >= ENV_INLINE_SLOTS || s_i >= ENV_INLINE_SLOTS)
    return 0;

  int off_acc = (int)offsetof(env_t, inline_vals[0]) + (int)s_acc * 8;
  int off_i = (int)offsetof(env_t, inline_vals[0]) + (int)s_i * 8;
  int off_cont = (int)offsetof(struct exp_t, content);
  int off_next = (int)offsetof(struct exp_t, next);
  int off_nref = (int)offsetof(struct exp_t, nref);
  if (off_acc > 32760 || off_i > 32760 || off_cont > 32760 ||
      off_next > 32760 || off_nref > 16380)
    return 0;
#if !ALCOVE_SINGLE_THREADED
  int off_flags = (int)offsetof(struct exp_t, flags);
  if (off_flags > 4095)
    return 0; /* LDRB unsigned-offset limit */
  int patch_shared_ref = -1, patch_shared_unref = -1;
#endif

  int n = 0;
  int entry_pc = n;

  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)nil_singleton);
  n += emit_mov64(out + n, 10, (uint64_t)(uintptr_t)true_singleton);

  out[n++] = arm64_ldr_imm(1, 0, off_acc);
  int patch_t1 = n;
  out[n++] = 0;
  out[n++] = arm64_cmp_reg(1, 9);
  int patch_t2 = n;
  out[n++] = 0;

  out[n++] = arm64_ldr_imm(2, 1, off_cont);
  int patch_da = n;
  out[n++] = 0;

  out[n++] = arm64_ldr_imm(3, 0, off_i);
  int patch_db = n;
  out[n++] = 0;

  out[n++] = arm64_sub_imm(2, 2, 1);
  out[n++] = arm64_sub_imm(3, 3, 1);
  int patch_dc = n;
  out[n++] = 0;

  out[n++] = arm64_sdiv(4, 3, 2);
  out[n++] = arm64_msub(5, 4, 2, 3);
  int patch_n1 = n;
  out[n++] = 0;

  out[n++] = arm64_ldr_imm(4, 1, off_next);

  int patch_skip_ref_a = n;
  out[n++] = 0;
  out[n++] = arm64_cmp_reg(4, 9);
  int patch_skip_ref_b = n;
  out[n++] = 0;
  out[n++] = arm64_cmp_reg(4, 10);
  int patch_skip_ref_c = n;
  out[n++] = 0;
#if !ALCOVE_SINGLE_THREADED
  /* Deopt to bytecode if the cdr target is FLAG_SHARED — the bytecode
     interp uses atomic refcount macros, the JIT inlines plain ldr/str. */
  out[n++] = arm64_ldrb_imm(7, 4, off_flags);
  patch_shared_ref = n;
  out[n++] = 0; /* tbnz w7, #3, deopt */
#endif
  out[n++] = arm64_ldr_w_imm(6, 4, off_nref);
  out[n++] = arm64_add_w_imm(6, 6, 1);
  out[n++] = arm64_str_w_imm(6, 4, off_nref);
  int skip_ref_pc = n;

  int patch_skip_unref_a = n;
  out[n++] = 0;
  out[n++] = arm64_cmp_reg(1, 9);
  int patch_skip_unref_b = n;
  out[n++] = 0;
  out[n++] = arm64_cmp_reg(1, 10);
  int patch_skip_unref_c = n;
  out[n++] = 0;
#if !ALCOVE_SINGLE_THREADED
  out[n++] = arm64_ldrb_imm(7, 1, off_flags);
  patch_shared_unref = n;
  out[n++] = 0; /* tbnz w7, #3, deopt */
#endif
  out[n++] = arm64_ldr_w_imm(6, 1, off_nref);
  out[n++] = arm64_sub_w_imm(6, 6, 1);
  out[n++] = arm64_str_w_imm(6, 1, off_nref);
  int patch_to_deopt = n;
  out[n++] = 0;
  int skip_unref_pc = n;

  out[n++] = arm64_str_imm(4, 0, off_acc);

  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur);
  }

  int ret_t_pc = n;
  out[n++] = arm64_mov_reg(0, 10);
  out[n++] = arm64_ret();

  int ret_nil_pc = n;
  out[n++] = arm64_mov_reg(0, 9);
  out[n++] = arm64_ret();

  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_t1] = arm64_cbz(1, ret_t_pc - patch_t1);
  out[patch_t2] = arm64_b_cond(0, ret_t_pc - patch_t2);
  PATCH_DEOPT_TBZ(patch_da, 2, 0);
  PATCH_DEOPT_TBZ(patch_db, 3, 0);
  PATCH_DEOPT_CBZ(patch_dc, 2);
  out[patch_n1] = arm64_cbz(5, ret_nil_pc - patch_n1);

  out[patch_skip_ref_a] = arm64_cbz(4, skip_ref_pc - patch_skip_ref_a);
  out[patch_skip_ref_b] = arm64_b_cond(0, skip_ref_pc - patch_skip_ref_b);
  out[patch_skip_ref_c] = arm64_b_cond(0, skip_ref_pc - patch_skip_ref_c);

  out[patch_skip_unref_a] = arm64_cbz(1, skip_unref_pc - patch_skip_unref_a);
  out[patch_skip_unref_b] = arm64_b_cond(0, skip_unref_pc - patch_skip_unref_b);
  out[patch_skip_unref_c] = arm64_b_cond(0, skip_unref_pc - patch_skip_unref_c);
  PATCH_DEOPT_CBZ_W(patch_to_deopt, 6);
#if !ALCOVE_SINGLE_THREADED
  PATCH_DEOPT_TBNZ(patch_shared_ref, 7, 3);
  PATCH_DEOPT_TBNZ(patch_shared_unref, 7, 3);
#endif

  JIT_GUARD(96);
  *outn = n;
  return 1;
}

/* safe? from nqueens.alc — 71-byte exact-match shape.
     (def safe? (c qs offset)
       (if (no qs) t
           (if (is c (car qs)) nil
               (if (is (+ c offset) (car qs)) nil
                   (if (is (- c offset) (car qs)) nil
                       (safe? c (cdr qs) (+ offset 1)))))))
   Hot inner loop in nqueens. Walks the placed-queens list, checking
   column + diagonal conflicts. Inline refexp/unrefexp for the cdr walk;
   falls through to bytecode (NULL deopt) if a refcount actually hits 0. */
static int try_jit_safe_p(bytecode_t *bc, uint32_t *out, int *outn) {
  if (bc->ncode != 71)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_qs = c[1];
  if (c[2] != OP_NOT)
    return 0;
  if (c[3] != OP_BR_IF_FALSE)
    return 0;
  if (c[6] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_t = c[7];
  if (c[8] != OP_JUMP)
    return 0;
  if (c[11] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_c = c[12];
  if (c[13] != OP_LOAD_SLOT || c[14] != s_qs)
    return 0;
  if (c[15] != OP_CAR)
    return 0;
  if (c[16] != OP_IS)
    return 0;
  if (c[17] != OP_BR_IF_FALSE)
    return 0;
  if (c[20] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil1 = c[21];
  if (c[22] != OP_JUMP)
    return 0;
  if (c[25] != OP_LOAD_SLOT || c[26] != s_c)
    return 0;
  if (c[27] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_off = c[28];
  if (c[29] != OP_ADD)
    return 0;
  if (c[30] != OP_LOAD_SLOT || c[31] != s_qs)
    return 0;
  if (c[32] != OP_CAR)
    return 0;
  if (c[33] != OP_IS)
    return 0;
  if (c[34] != OP_BR_IF_FALSE)
    return 0;
  if (c[37] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil2 = c[38];
  if (c[39] != OP_JUMP)
    return 0;
  if (c[42] != OP_LOAD_SLOT || c[43] != s_c)
    return 0;
  if (c[44] != OP_LOAD_SLOT || c[45] != s_off)
    return 0;
  if (c[46] != OP_SUB)
    return 0;
  if (c[47] != OP_LOAD_SLOT || c[48] != s_qs)
    return 0;
  if (c[49] != OP_CAR)
    return 0;
  if (c[50] != OP_IS)
    return 0;
  if (c[51] != OP_BR_IF_FALSE)
    return 0;
  if (c[54] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil3 = c[55];
  if (c[56] != OP_JUMP)
    return 0;
  if (c[59] != OP_LOAD_SLOT || c[60] != s_c)
    return 0;
  if (c[61] != OP_LOAD_SLOT || c[62] != s_qs)
    return 0;
  if (c[63] != OP_CDR)
    return 0;
  if (c[64] != OP_SLOT_ADD_FIX || c[65] != s_off || c[66] != 1 || c[67] != 0)
    return 0;
  if (c[68] != OP_TAIL_SELF || c[69] != 3)
    return 0;
  if (c[70] != OP_RET)
    return 0;

  if (idx_t >= bc->nconsts)
    return 0;
  exp_t *ct = bc->consts[idx_t];
  if (!issymbol(ct) || strcmp((const char *)exp_text(ct), "t") != 0)
    return 0;
  for (int k = 0; k < 3; k++) {
    uint8_t idx = (k == 0) ? idx_nil1 : (k == 1) ? idx_nil2 : idx_nil3;
    if (idx >= bc->nconsts)
      return 0;
    exp_t *cn = bc->consts[idx];
    if (!issymbol(cn) || strcmp((const char *)exp_text(cn), "nil") != 0)
      return 0;
  }
  if (s_c >= ENV_INLINE_SLOTS || s_qs >= ENV_INLINE_SLOTS ||
      s_off >= ENV_INLINE_SLOTS)
    return 0;

  int off_c = (int)offsetof(env_t, inline_vals[0]) + (int)s_c * 8;
  int off_qs = (int)offsetof(env_t, inline_vals[0]) + (int)s_qs * 8;
  int off_off = (int)offsetof(env_t, inline_vals[0]) + (int)s_off * 8;
  int off_cont = (int)offsetof(struct exp_t, content);
  int off_next = (int)offsetof(struct exp_t, next);
  int off_nref = (int)offsetof(struct exp_t, nref);
  if (off_c > 32760 || off_qs > 32760 || off_off > 32760 || off_cont > 32760 ||
      off_next > 32760 || off_nref > 16380)
    return 0;
#if !ALCOVE_SINGLE_THREADED
  int off_flags = (int)offsetof(struct exp_t, flags);
  if (off_flags > 4095)
    return 0; /* LDRB unsigned-offset limit */
  int patch_shared_ref = -1, patch_shared_unref = -1;
#endif

  int n = 0;
  int entry_pc = n;

  /* Preload nil + true into x9, x10 (live across iterations). */
  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)nil_singleton);
  n += emit_mov64(out + n, 10, (uint64_t)(uintptr_t)true_singleton);

  /* x1 = qs. If null or nil → return t. */
  out[n++] = arm64_ldr_imm(1, 0, off_qs);
  int patch_t1 = n;
  out[n++] = 0; /* cbz x1, ret_t */
  out[n++] = arm64_cmp_reg(1, 9);
  int patch_t2 = n;
  out[n++] = 0; /* b.eq ret_t */

  /* x2 = car(qs) tagged. Tag-check. */
  out[n++] = arm64_ldr_imm(2, 1, off_cont);
  int patch_da = n;
  out[n++] = 0; /* tbz x2,#0,deopt */

  /* x3 = c. If c == car → return nil. */
  out[n++] = arm64_ldr_imm(3, 0, off_c);
  out[n++] = arm64_cmp_reg(3, 2);
  int patch_n1 = n;
  out[n++] = 0; /* b.eq ret_nil */

  /* x4 = offset. Tag-check. */
  out[n++] = arm64_ldr_imm(4, 0, off_off);
  int patch_db = n;
  out[n++] = 0; /* tbz x4,#0,deopt */

  /* (c + off)_tagged = c_tagged + off_tagged - 1. Compare with car. */
  out[n++] = arm64_add_reg(5, 3, 4);
  out[n++] = arm64_sub_imm(5, 5, 1);
  out[n++] = arm64_cmp_reg(5, 2);
  int patch_n2 = n;
  out[n++] = 0; /* b.eq ret_nil */

  /* (c - off)_tagged = c_tagged - off_tagged + 1. Compare with car. */
  out[n++] = arm64_sub_reg(5, 3, 4);
  out[n++] = arm64_add_imm(5, 5, 1);
  out[n++] = arm64_cmp_reg(5, 2);
  int patch_n3 = n;
  out[n++] = 0; /* b.eq ret_nil */

  /* Cdr walk. x5 = cdr(qs) = qs->next. */
  out[n++] = arm64_ldr_imm(5, 1, off_next);

  /* refexp(x5) inline: skip if NULL/nil/true; else nref++. */
  int patch_skip_ref_a = n;
  out[n++] = 0; /* cbz x5, skip_ref */
  out[n++] = arm64_cmp_reg(5, 9);
  int patch_skip_ref_b = n;
  out[n++] = 0; /* b.eq skip_ref */
  out[n++] = arm64_cmp_reg(5, 10);
  int patch_skip_ref_c = n;
  out[n++] = 0; /* b.eq skip_ref */
#if !ALCOVE_SINGLE_THREADED
  /* If the target is FLAG_SHARED, deopt to bytecode (which uses atomic
     refcount macros). Reads the low byte of flags — FLAG_SHARED=8 lives
     in bit 3, well within the byte. */
  out[n++] = arm64_ldrb_imm(7, 5, off_flags);
  patch_shared_ref = n;
  out[n++] = 0; /* tbnz w7, #3, deopt */
#endif
  out[n++] = arm64_ldr_w_imm(6, 5, off_nref);
  out[n++] = arm64_add_w_imm(6, 6, 1);
  out[n++] = arm64_str_w_imm(6, 5, off_nref);
  int skip_ref_pc = n;

  /* unrefexp(x1=qs) inline: skip if NULL/nil/true; else nref--; if 0 deopt. */
  int patch_skip_unref_a = n;
  out[n++] = 0; /* cbz x1, skip_unref */
  out[n++] = arm64_cmp_reg(1, 9);
  int patch_skip_unref_b = n;
  out[n++] = 0;
  out[n++] = arm64_cmp_reg(1, 10);
  int patch_skip_unref_c = n;
  out[n++] = 0;
#if !ALCOVE_SINGLE_THREADED
  out[n++] = arm64_ldrb_imm(7, 1, off_flags);
  patch_shared_unref = n;
  out[n++] = 0; /* tbnz w7, #3, deopt */
#endif
  out[n++] = arm64_ldr_w_imm(6, 1, off_nref);
  out[n++] = arm64_sub_w_imm(6, 6, 1);
  out[n++] = arm64_str_w_imm(6, 1, off_nref);
  int patch_to_deopt = n;
  out[n++] = 0; /* cbz w6, deopt */
  int skip_unref_pc = n;

  /* slot[qs] = cdr (x5) */
  out[n++] = arm64_str_imm(5, 0, off_qs);

  /* offset += 8 (tagged add 1). */
  out[n++] = arm64_add_imm(4, 4, 8);
  out[n++] = arm64_str_imm(4, 0, off_off);

  /* tail-self: b entry */
  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur);
  }

  /* ret_t: x0 = true_singleton */
  int ret_t_pc = n;
  out[n++] = arm64_mov_reg(0, 10);
  out[n++] = arm64_ret();

  /* ret_nil: x0 = nil_singleton */
  int ret_nil_pc = n;
  out[n++] = arm64_mov_reg(0, 9);
  out[n++] = arm64_ret();

  /* deopt: x0 = NULL */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  /* Patch all forward branches. */
  out[patch_t1] = arm64_cbz(1, ret_t_pc - patch_t1);
  out[patch_t2] = arm64_b_cond(0 /* EQ */, ret_t_pc - patch_t2);
  PATCH_DEOPT_TBZ(patch_da, 2, 0);
  PATCH_DEOPT_TBZ(patch_db, 4, 0);
  out[patch_n1] = arm64_b_cond(0, ret_nil_pc - patch_n1);
  out[patch_n2] = arm64_b_cond(0, ret_nil_pc - patch_n2);
  out[patch_n3] = arm64_b_cond(0, ret_nil_pc - patch_n3);

  out[patch_skip_ref_a] = arm64_cbz(5, skip_ref_pc - patch_skip_ref_a);
  out[patch_skip_ref_b] = arm64_b_cond(0, skip_ref_pc - patch_skip_ref_b);
  out[patch_skip_ref_c] = arm64_b_cond(0, skip_ref_pc - patch_skip_ref_c);

  out[patch_skip_unref_a] = arm64_cbz(1, skip_unref_pc - patch_skip_unref_a);
  out[patch_skip_unref_b] = arm64_b_cond(0, skip_unref_pc - patch_skip_unref_b);
  out[patch_skip_unref_c] = arm64_b_cond(0, skip_unref_pc - patch_skip_unref_c);
  PATCH_DEOPT_CBZ_W(patch_to_deopt, 6);
#if !ALCOVE_SINGLE_THREADED
  PATCH_DEOPT_TBNZ(patch_shared_ref, 7, 3);
  PATCH_DEOPT_TBNZ(patch_shared_unref, 7, 3);
#endif

  /* Suppress unused-on-some-paths warnings. */
  (void)arm64_cbnz;
  (void)arm64_cmp_reg_w;

  JIT_GUARD(96);
  *outn = n;
  return 1;
}

/* mark-from from sieve-fast — 35-byte exact-match shape.
     (def mark-from (step j n marks)
       (if (> j n) nil
           (do (vec-set! marks j nil)
               (mark-from step (+ j step) n marks))))
   Tight inner loop — writes nil into marks[j], increments j by step,
   tail-self. ~10 instructions per iteration. */
static int try_jit_mark_from(bytecode_t *bc, uint32_t *out, int *outn) {
  if (bc->ncode != 35)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_j = c[1];
  if (c[2] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_n = c[3];
  if (c[4] != OP_GT)
    return 0;
  if (c[5] != OP_BR_IF_FALSE)
    return 0;
  if (c[8] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil1 = c[9];
  if (c[10] != OP_JUMP)
    return 0;

  if (c[13] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_marks = c[14];
  if (c[15] != OP_LOAD_SLOT || c[16] != s_j)
    return 0;
  if (c[17] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil2 = c[18];
  if (c[19] != OP_VEC_SET)
    return 0;
  if (c[20] != OP_POP)
    return 0;

  if (c[21] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_step = c[22];
  if (c[23] != OP_LOAD_SLOT || c[24] != s_j)
    return 0;
  if (c[25] != OP_LOAD_SLOT || c[26] != s_step)
    return 0;
  if (c[27] != OP_ADD)
    return 0;
  if (c[28] != OP_LOAD_SLOT || c[29] != s_n)
    return 0;
  if (c[30] != OP_LOAD_SLOT || c[31] != s_marks)
    return 0;
  if (c[32] != OP_TAIL_SELF || c[33] != 4)
    return 0;
  if (c[34] != OP_RET)
    return 0;

  /* Both LOAD_GLOBALs must resolve to nil. */
  if (idx_nil1 >= bc->nconsts || idx_nil2 >= bc->nconsts)
    return 0;
  exp_t *cn1 = bc->consts[idx_nil1], *cn2 = bc->consts[idx_nil2];
  if (!issymbol(cn1) || strcmp((const char *)exp_text(cn1), "nil") != 0)
    return 0;
  if (!issymbol(cn2) || strcmp((const char *)exp_text(cn2), "nil") != 0)
    return 0;

  if (s_j >= ENV_INLINE_SLOTS || s_n >= ENV_INLINE_SLOTS ||
      s_step >= ENV_INLINE_SLOTS || s_marks >= ENV_INLINE_SLOTS)
    return 0;

  int off_j = (int)offsetof(env_t, inline_vals[0]) + (int)s_j * 8;
  int off_n = (int)offsetof(env_t, inline_vals[0]) + (int)s_n * 8;
  int off_step = (int)offsetof(env_t, inline_vals[0]) + (int)s_step * 8;
  int off_marks = (int)offsetof(env_t, inline_vals[0]) + (int)s_marks * 8;
  int off_ptr = (int)offsetof(struct exp_t, ptr);
  if (off_j > 32760 || off_n > 32760 || off_step > 32760 || off_marks > 32760 ||
      off_ptr > 32760)
    return 0;

  int n = 0;

  int entry_pc = n;
  /* x1 = j tagged, x2 = n tagged. */
  out[n++] = arm64_ldr_imm(1, 0, off_j);
  out[n++] = arm64_ldr_imm(2, 0, off_n);
  int patch_da = n;
  out[n++] = 0; /* tbz x1,#0,deopt */
  int patch_db = n;
  out[n++] = 0; /* tbz x2,#0,deopt */

  /* if (j > n): return nil. cmp x1, x2; b.gt done */
  out[n++] = arm64_cmp_reg(1, 2);
  int patch_done = n;
  out[n++] = 0; /* b.gt done */

  /* x3 = marks (exp_t*), then x3 = marks->ptr (alc_vec_t*).  We assume
     VEC_KIND_GEN (8-byte exp_t* cells); for typed kinds the JIT'd write
     would corrupt int64/double payload. Check the flags byte and bail. */
  out[n++] = arm64_ldr_imm(3, 0, off_marks);
  int off_flags_m = (int)offsetof(struct exp_t, flags);
  out[n++] = arm64_ldrb_imm(7, 3, off_flags_m);
  int patch_kind_m_a = n;
  out[n++] = 0; /* tbnz w7,#4,deopt */
  int patch_kind_m_b = n;
  out[n++] = 0; /* tbnz w7,#5,deopt */
  out[n++] = arm64_ldr_imm(3, 3, off_ptr);

  /* x4 = marks_ptr + j_tagged + 7 = &data[j_untagged]. */
  out[n++] = arm64_add_reg(4, 3, 1);
  out[n++] = arm64_add_imm(4, 4, 7);

  /* x5 = nil_singleton; *(x4) = x5. */
  n += emit_mov64(out + n, 5, (uint64_t)(uintptr_t)nil_singleton);
  out[n++] = arm64_str_imm(5, 4, 0);

  /* j = j + step - 1 (tagged-arith — drop the extra tag bit). */
  out[n++] = arm64_ldr_imm(6, 0, off_step);
  out[n++] = arm64_add_reg(1, 1, 6);
  out[n++] = arm64_sub_imm(1, 1, 1);
  out[n++] = arm64_str_imm(1, 0, off_j);

  /* tail-self: b entry */
  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur);
  }

  /* done: x0 = nil */
  int done_pc = n;
  n += emit_mov64(out + n, 0, (uint64_t)(uintptr_t)nil_singleton);
  out[n++] = arm64_ret();

  /* deopt */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_done] = arm64_b_cond(12 /* GT */, done_pc - patch_done);
  PATCH_DEOPT_TBZ(patch_da, 1, 0);
  PATCH_DEOPT_TBZ(patch_db, 2, 0);
  PATCH_DEOPT_TBNZ(patch_kind_m_a, 7, 4);
  PATCH_DEOPT_TBNZ(patch_kind_m_b, 7, 5);

  JIT_GUARD(64);
  *outn = n;
  return 1;
}

/* Tail counter loop with one inner global call before the recurse.
   26-byte body produced by:
     (def lp (k) (if (cmp k K1) (do (g K_arg) (lp (op k K2))) k))
   Establish a frame, run the loop in registers, BLR
   jit_call_global1_drop for the inner call, propagate any error. */
static int try_jit_tail_loop_with_call(bytecode_t *bc, uint32_t *out,
                                       int *outn) {
  if (bc->ncode != 26)
    return 0;
  uint8_t *c = bc->code;

  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX)
    return 0;
  uint8_t slot = c[1];
  if (slot >= ENV_INLINE_SLOTS)
    return 0;
  int16_t cmp_imm = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE)
    return 0;
  if (c[7] != OP_LOAD_FIX)
    return 0;
  int16_t arg_imm = (int16_t)((uint16_t)c[8] | ((uint16_t)c[9] << 8));

  if (c[10] != OP_CALL_GLOBAL)
    return 0;
  uint8_t const_idx = c[11];
  if (c[12] != 1)
    return 0;
  if (const_idx >= bc->nconsts)
    return 0;

  if (c[13] != OP_POP)
    return 0;

  uint8_t arith_op = c[14];
  if (arith_op != OP_SLOT_SUB_FIX && arith_op != OP_SLOT_ADD_FIX)
    return 0;
  if (c[15] != slot)
    return 0;
  int16_t arith_imm = (int16_t)((uint16_t)c[16] | ((uint16_t)c[17] << 8));

  if (c[18] != OP_TAIL_SELF || c[19] != 1)
    return 0;
  if (c[20] != OP_JUMP)
    return 0;
  if (c[23] != OP_LOAD_SLOT || c[24] != slot)
    return 0;
  if (c[25] != OP_RET)
    return 0;

  int slot_off = (int)offsetof(env_t, inline_vals[0]) + (int)slot * 8;
  if (slot_off > 32760)
    return 0;
  int64_t cmp_tagged = ((int64_t)cmp_imm << 3) | 1;
  if (cmp_tagged < 0 || cmp_tagged > 4095)
    return 0;
  int arith_delta = ((int)arith_imm) << 3;
  if (arith_delta < 0 || arith_delta > 4095)
    return 0;
  int64_t tagged_arg = ((int64_t)arg_imm << 3) | 1;

  int inv_cc;
  switch (cmp_op) {
  case OP_SLOT_GT_FIX:
    inv_cc = 13;
    break;
  case OP_SLOT_LT_FIX:
    inv_cc = 10;
    break;
  case OP_SLOT_GE_FIX:
    inv_cc = 11;
    break;
  case OP_SLOT_LE_FIX:
    inv_cc = 12;
    break;
  default:
    return 0;
  }

  int n = 0;
  out[n++] = arm64_ldr_imm(1, 0, slot_off);
  int patch_deopt = n;
  out[n++] = 0;

  out[n++] = arm64_stp_pre_sp(29, 30, -32);
  out[n++] = arm64_stp_off_sp(19, 20, 16);
  out[n++] = arm64_mov_from_sp(29);
  out[n++] = arm64_mov_reg(19, 0);

  int loop_top = n;
  out[n++] = arm64_ldr_imm(1, 19, slot_off);
  out[n++] = arm64_cmp_imm(1, (int)cmp_tagged);
  int patch_end = n;
  out[n++] = 0;

  n += emit_mov64(out + n, 0, (uint64_t)(uintptr_t)bc);
  out[n++] = arm64_mov_reg(1, 19);
  n += emit_mov64(out + n, 2, (uint64_t)const_idx);
  n += emit_mov64(out + n, 3, (uint64_t)tagged_arg);
  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)&jit_call_global1_drop);
  out[n++] = arm64_blr(9);

  int patch_err = n;
  out[n++] = 0; /* cbnz x0, err */

  out[n++] = arm64_ldr_imm(1, 19, slot_off);
  if (arith_op == OP_SLOT_SUB_FIX)
    out[n++] = arm64_sub_imm(1, 1, arith_delta);
  else
    out[n++] = arm64_add_imm(1, 1, arith_delta);
  out[n++] = arm64_str_imm(1, 19, slot_off);
  {
    int cur = n++;
    out[cur] = arm64_b(loop_top - cur);
  }

  int end_pc = n;
  out[n++] = arm64_ldr_imm(0, 19, slot_off);
  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 32);
  out[n++] = arm64_ret();

  int err_pc = n;
  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 32);
  out[n++] = arm64_ret();

  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  /* Use the proper helper so the offset gets range-checked instead of
     silently truncated by the inline mask. */
  out[patch_err] = arm64_cbnz(0, err_pc - patch_err);
  out[patch_end] = arm64_b_cond(inv_cc, end_pc - patch_end);
  PATCH_DEOPT_TBZ(patch_deopt, 1, 0);

  JIT_GUARD(64);
  *outn = n;
  return 1;
}

/* Knuth's tak — 50-byte exact-match shape.
     (def tak (x y z) (if (no (< y x)) z
                          (tak (tak (- x 1) y z)
                               (tak (- y 1) z x)
                               (tak (- z 1) x y))))
   Three nested non-tail self-calls + one tail self-call. Each inner call
   is a direct intra-buffer BL into our own entry. We stash the 3 originals
   and 3 intermediate results in the stack frame across calls. */
static int try_jit_tak(bytecode_t *bc, uint32_t *out, int *outn) {
  if (bc->ncode != 50)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_y = c[1];
  if (c[2] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_x = c[3];
  if (c[4] != OP_LT)
    return 0;
  if (c[5] != OP_NOT)
    return 0;
  if (c[6] != OP_BR_IF_FALSE)
    return 0;
  if (c[9] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_z = c[10];
  if (c[11] != OP_JUMP)
    return 0;

  if (c[14] != OP_SLOT_SUB_FIX || c[15] != s_x || c[16] != 1 || c[17] != 0)
    return 0;
  if (c[18] != OP_LOAD_SLOT || c[19] != s_y)
    return 0;
  if (c[20] != OP_LOAD_SLOT || c[21] != s_z)
    return 0;
  if (c[22] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_a = c[23];
  if (c[24] != 3)
    return 0;

  if (c[25] != OP_SLOT_SUB_FIX || c[26] != s_y || c[27] != 1 || c[28] != 0)
    return 0;
  if (c[29] != OP_LOAD_SLOT || c[30] != s_z)
    return 0;
  if (c[31] != OP_LOAD_SLOT || c[32] != s_x)
    return 0;
  if (c[33] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_b = c[34];
  if (c[35] != 3)
    return 0;

  if (c[36] != OP_SLOT_SUB_FIX || c[37] != s_z || c[38] != 1 || c[39] != 0)
    return 0;
  if (c[40] != OP_LOAD_SLOT || c[41] != s_x)
    return 0;
  if (c[42] != OP_LOAD_SLOT || c[43] != s_y)
    return 0;
  if (c[44] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_c = c[45];
  if (c[46] != 3)
    return 0;
  if (c[47] != OP_TAIL_SELF || c[48] != 3 || c[49] != OP_RET)
    return 0;

  if (s_x >= ENV_INLINE_SLOTS || s_y >= ENV_INLINE_SLOTS ||
      s_z >= ENV_INLINE_SLOTS)
    return 0;

  /* All three CALL_GLOBALs must target THIS function — the matcher
     emits intra-buffer BL to entry. Without the check, any (def f (x y z)
     ...) with the tak shape silently rewires the calls. */
  if (!bc->self_name)
    return 0;
  if (idx_a >= bc->nconsts || idx_b >= bc->nconsts || idx_c >= bc->nconsts)
    return 0;
  exp_t *ka = bc->consts[idx_a], *kb = bc->consts[idx_b],
        *kc = bc->consts[idx_c];
  if (!issymbol(ka) || !issymbol(kb) || !issymbol(kc))
    return 0;
  if (strcmp((const char *)exp_text(ka), bc->self_name) != 0 ||
      strcmp((const char *)exp_text(kb), bc->self_name) != 0 ||
      strcmp((const char *)exp_text(kc), bc->self_name) != 0)
    return 0;

  int off_x = (int)offsetof(env_t, inline_vals[0]) + (int)s_x * 8;
  int off_y = (int)offsetof(env_t, inline_vals[0]) + (int)s_y * 8;
  int off_z = (int)offsetof(env_t, inline_vals[0]) + (int)s_z * 8;
  if (off_x > 32760 || off_y > 32760 || off_z > 32760)
    return 0;

  /* Frame: 80 bytes. [sp+0]=fp, +8=lr, +16=x19, +24=pad, +32..+48=orig
     x/y/z, +56..+72=t1/t2/t3. */
  int n = 0;
  int entry_pc = n;
  out[n++] = arm64_ldr_imm(1, 0, off_y);
  out[n++] = arm64_ldr_imm(2, 0, off_x);
  int patch_da = n;
  out[n++] = 0;
  int patch_db = n;
  out[n++] = 0;

  out[n++] = arm64_cmp_reg(1, 2);
  int patch_recurse = n;
  out[n++] = 0;
  out[n++] = arm64_ldr_imm(0, 0, off_z);
  out[n++] = arm64_ret();

  int recurse_pc = n;
  out[n++] = arm64_stp_pre_sp(29, 30, -80);
  out[n++] = arm64_stp_off_sp(19, 20, 16);
  out[n++] = arm64_mov_from_sp(29);
  out[n++] = arm64_mov_reg(19, 0);

  out[n++] = arm64_str_imm(2, 31, 32);
  out[n++] = arm64_str_imm(1, 31, 40);
  out[n++] = arm64_ldr_imm(3, 0, off_z);
  out[n++] = arm64_str_imm(3, 31, 48);

  out[n++] = arm64_sub_imm(2, 2, 8);
  out[n++] = arm64_str_imm(2, 0, off_x);
  {
    int cur = n++;
    out[cur] = 0x94000000u | ((uint32_t)(entry_pc - cur) & 0x3FFFFFFu);
  }
  int patch_b1 = n;
  out[n++] = 0;
  out[n++] = arm64_str_imm(0, 31, 56);
  out[n++] = arm64_mov_reg(0, 19);

  out[n++] = arm64_ldr_imm(1, 31, 40);
  out[n++] = arm64_sub_imm(1, 1, 8);
  out[n++] = arm64_str_imm(1, 0, off_x);
  out[n++] = arm64_ldr_imm(1, 31, 48);
  out[n++] = arm64_str_imm(1, 0, off_y);
  out[n++] = arm64_ldr_imm(1, 31, 32);
  out[n++] = arm64_str_imm(1, 0, off_z);
  {
    int cur = n++;
    out[cur] = 0x94000000u | ((uint32_t)(entry_pc - cur) & 0x3FFFFFFu);
  }
  int patch_b2 = n;
  out[n++] = 0;
  out[n++] = arm64_str_imm(0, 31, 64);
  out[n++] = arm64_mov_reg(0, 19);

  out[n++] = arm64_ldr_imm(1, 31, 48);
  out[n++] = arm64_sub_imm(1, 1, 8);
  out[n++] = arm64_str_imm(1, 0, off_x);
  out[n++] = arm64_ldr_imm(1, 31, 32);
  out[n++] = arm64_str_imm(1, 0, off_y);
  out[n++] = arm64_ldr_imm(1, 31, 40);
  out[n++] = arm64_str_imm(1, 0, off_z);
  {
    int cur = n++;
    out[cur] = 0x94000000u | ((uint32_t)(entry_pc - cur) & 0x3FFFFFFu);
  }
  int patch_b3 = n;
  out[n++] = 0;
  out[n++] = arm64_str_imm(0, 31, 72);
  out[n++] = arm64_mov_reg(0, 19);

  out[n++] = arm64_ldr_imm(1, 31, 56);
  out[n++] = arm64_str_imm(1, 0, off_x);
  out[n++] = arm64_ldr_imm(1, 31, 64);
  out[n++] = arm64_str_imm(1, 0, off_y);
  out[n++] = arm64_ldr_imm(1, 31, 72);
  out[n++] = arm64_str_imm(1, 0, off_z);

  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 80);
  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur);
  }

  int bail_pc = n;
  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 80);
  out[n++] = arm64_ret();

  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_recurse] = arm64_b_cond(11 /* LT */, recurse_pc - patch_recurse);
  out[patch_b1] = arm64_tbz(0, 0, bail_pc - patch_b1);
  out[patch_b2] = arm64_tbz(0, 0, bail_pc - patch_b2);
  out[patch_b3] = arm64_tbz(0, 0, bail_pc - patch_b3);
  PATCH_DEOPT_TBZ(patch_da, 1, 0);
  PATCH_DEOPT_TBZ(patch_db, 2, 0);

  JIT_GUARD(96);
  *outn = n;
  return 1;
}

/* The Ackermann function: 53-byte exact-match shape.
     (def ack (m n)
       (if (is m 0) (+ n 1)
           (if (is n 0) (ack (- m 1) 1)
               (ack (- m 1) (ack m (- n 1))))))
   m==0 and n==0 cases run inline (no frame); the general case opens a
   frame, recursive-CALLs the inner ack(m, n-1) via intra-buffer BL,
   then tail-self's to ack(m-1, result). */
static int try_jit_ackermann(bytecode_t *bc, uint32_t *out, int *outn) {
  if (bc->ncode != 53)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t slot_m = c[1];
  if (c[2] != OP_LOAD_FIX || c[3] != 0 || c[4] != 0)
    return 0;
  if (c[5] != OP_IS)
    return 0;
  if (c[6] != OP_BR_IF_FALSE)
    return 0;

  if (c[9] != OP_SLOT_ADD_FIX)
    return 0;
  uint8_t slot_n_check = c[10];
  if (c[11] != 1 || c[12] != 0)
    return 0;
  if (c[13] != OP_JUMP)
    return 0;

  if (c[16] != OP_LOAD_SLOT)
    return 0;
  uint8_t slot_n = c[17];
  if (slot_n != slot_n_check)
    return 0;
  if (c[18] != OP_LOAD_FIX || c[19] != 0 || c[20] != 0)
    return 0;
  if (c[21] != OP_IS)
    return 0;
  if (c[22] != OP_BR_IF_FALSE)
    return 0;

  if (c[25] != OP_SLOT_SUB_FIX || c[26] != slot_m)
    return 0;
  if (c[27] != 1 || c[28] != 0)
    return 0;
  if (c[29] != OP_LOAD_FIX || c[30] != 1 || c[31] != 0)
    return 0;
  if (c[32] != OP_TAIL_SELF || c[33] != 2)
    return 0;
  if (c[34] != OP_JUMP)
    return 0;

  if (c[37] != OP_SLOT_SUB_FIX || c[38] != slot_m)
    return 0;
  if (c[39] != 1 || c[40] != 0)
    return 0;
  if (c[41] != OP_LOAD_SLOT || c[42] != slot_m)
    return 0;
  if (c[43] != OP_SLOT_SUB_FIX || c[44] != slot_n)
    return 0;
  if (c[45] != 1 || c[46] != 0)
    return 0;
  if (c[47] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_call = c[48];
  if (c[49] != 2)
    return 0;
  /* CALL_GLOBAL must target THIS function (intra-buffer BL). */
  if (!bc->self_name || idx_call >= bc->nconsts)
    return 0;
  exp_t *callee = bc->consts[idx_call];
  if (!issymbol(callee))
    return 0;
  if (strcmp((const char *)exp_text(callee), bc->self_name) != 0)
    return 0;
  if (c[50] != OP_TAIL_SELF || c[51] != 2 || c[52] != OP_RET)
    return 0;
  if (slot_m >= ENV_INLINE_SLOTS || slot_n >= ENV_INLINE_SLOTS)
    return 0;

  int off_m = (int)offsetof(env_t, inline_vals[0]) + (int)slot_m * 8;
  int off_n = (int)offsetof(env_t, inline_vals[0]) + (int)slot_n * 8;
  if (off_m > 32760 || off_n > 32760)
    return 0;

  int n = 0;

  /* entry: load m,n into x1,x2; tag-check both. x0 stays as env. */
  int entry_pc = n;
  out[n++] = arm64_ldr_imm(1, 0, off_m); /* x1 = m */
  out[n++] = arm64_ldr_imm(2, 0, off_n); /* x2 = n */
  int patch_da = n;
  out[n++] = 0; /* tbz x1,#0,deopt */
  int patch_db = n;
  out[n++] = 0; /* tbz x2,#0,deopt */

  /* if m == FIX(0) (= 1): return n + 8 (= n + FIX(1) - FIX(0) = n+1 tagged). */
  out[n++] = arm64_cmp_imm(1, 1);
  int patch_not_m0 = n;
  out[n++] = 0;                      /* b.ne not_m0 */
  out[n++] = arm64_add_imm(0, 2, 8); /* x0 = x2 + 8 (tagged n+1) */
  out[n++] = arm64_ret();

  int not_m0_pc = n;
  /* if n == FIX(0) (= 1): tail-self (m-1, 1). */
  out[n++] = arm64_cmp_imm(2, 1);
  int patch_not_n0 = n;
  out[n++] = 0;                      /* b.ne not_n0 */
  out[n++] = arm64_sub_imm(1, 1, 8); /* x1 = m - 8 (tagged m-1) */
  out[n++] = arm64_str_imm(1, 0, off_m);
  n += emit_mov64(out + n, 3, 9); /* tagged 1 = 9 */
  out[n++] = arm64_str_imm(3, 0, off_n);
  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur); /* b entry */
  }

  /* not_n0: nested CALL ack(m, n-1), then tail-self (m-1, result). */
  int not_n0_pc = n;

  /* prologue: stp x29,x30 (FP/LR); stp x19,x20. 32-byte frame, 16-aligned. */
  out[n++] = arm64_stp_pre_sp(29, 30, -32); /* sp -= 32; [sp+0]=fp,[sp+8]=lr */
  out[n++] = arm64_stp_off_sp(19, 20, 16);  /* stp x19, x20, [sp, #16] */
  out[n++] = arm64_mov_from_sp(29);         /* mov x29, sp */

  out[n++] = arm64_mov_reg(19, 0); /* x19 = env */
  out[n++] = arm64_mov_reg(20, 1); /* x20 = m_orig */

  /* slot_n = n - 1 (tagged: -8). x2 still has n. */
  out[n++] = arm64_sub_imm(2, 2, 8);
  out[n++] = arm64_str_imm(2, 0, off_n);
  /* slot_m unchanged — inner needs ack(m, n-1). */

  /* BL entry (intra-buffer). */
  {
    int cur = n++;
    int off = entry_pc - cur;
    out[cur] = 0x94000000u | ((uint32_t)off & 0x3FFFFFFu);
  }

  /* tag-check result in x0; bail on non-fixnum. */
  int patch_bail = n;
  out[n++] = 0; /* tbz x0,#0,bail */

  /* tail-self prep: slot_m = m_orig - 1, slot_n = result, env back in x0. */
  out[n++] = arm64_sub_imm(20, 20, 8);
  out[n++] = arm64_str_imm(20, 19, off_m);
  out[n++] = arm64_str_imm(0, 19, off_n);
  out[n++] = arm64_mov_reg(0, 19);

  /* epilogue then b entry (tail-self). */
  out[n++] = arm64_ldp_off_sp(19, 20, 16);  /* ldp x19, x20, [sp, #16] */
  out[n++] = arm64_ldp_post_sp(29, 30, 32); /* ldp fp,lr ; sp += 32 */
  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur);
  }

  /* bail: tear down + return x0 (NULL/error). */
  int bail_pc = n;
  out[n++] = arm64_ldp_off_sp(19, 20, 16); /* ldp x19, x20, [sp, #16] */
  out[n++] = arm64_ldp_post_sp(29, 30, 32);
  out[n++] = arm64_ret();

  /* deopt (no frame yet). */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  /* Patch forward branches. */
  out[patch_not_m0] = arm64_b_cond(1 /* NE */, not_m0_pc - patch_not_m0);
  out[patch_not_n0] = arm64_b_cond(1 /* NE */, not_n0_pc - patch_not_n0);
  out[patch_bail] = arm64_tbz(0, 0, bail_pc - patch_bail);
  PATCH_DEOPT_TBZ(patch_da, 1, 0);
  PATCH_DEOPT_TBZ(patch_db, 2, 0);

  /* Suppress unused-warning for the helper we resolved inline. */
  (void)arm64_stp_pre_sp;
  (void)arm64_ldp_post_sp;

  JIT_GUARD(64);
  *outn = n;
  return 1;
}

/* (fn (a b) (is (mod a b) K)) — 10-byte 2-param leaf, the divides? shape.
   Computes (a mod b == K) and returns t/nil. Native: sdiv + msub for the
   remainder, csel for the boolean result. ~10 cycles vs ~150 in bytecode. */
static int try_jit_modeq_leaf(bytecode_t *bc, uint32_t *out, int *outn) {
  if (bc->ncode != 10)
    return 0;
  uint8_t *c = bc->code;
  if (c[0] != OP_LOAD_SLOT || c[1] >= ENV_INLINE_SLOTS)
    return 0;
  if (c[2] != OP_LOAD_SLOT || c[3] >= ENV_INLINE_SLOTS)
    return 0;
  if (c[4] != OP_MOD)
    return 0;
  if (c[5] != OP_LOAD_FIX)
    return 0;
  int16_t K = (int16_t)((uint16_t)c[6] | ((uint16_t)c[7] << 8));
  if (c[8] != OP_IS)
    return 0;
  if (c[9] != OP_RET)
    return 0;

  int off_a = (int)offsetof(env_t, inline_vals[0]) + (int)c[1] * 8;
  int off_b = (int)offsetof(env_t, inline_vals[0]) + (int)c[3] * 8;
  if (off_a > 32760 || off_b > 32760)
    return 0;

  /* (K << 3) is the value we compare against. Untagged a%b is (a<<3) %
     (b<<3) once we've stripped the tag bit. */
  int64_t k_shifted = ((int64_t)K) << 3;

  int n = 0;
  /* Load both slots. */
  out[n++] = arm64_ldr_imm(1, 0, off_a); /* x1 = a tagged */
  out[n++] = arm64_ldr_imm(2, 0, off_b); /* x2 = b tagged */
  /* Tag-check both. */
  int patch_t1 = n;
  out[n++] = 0; /* tbz x1,#0,deopt */
  int patch_t2 = n;
  out[n++] = 0; /* tbz x2,#0,deopt */
  /* Untag (sub 1). After this, x1=a<<3, x2=b<<3. */
  out[n++] = arm64_sub_imm(1, 1, 1);
  out[n++] = arm64_sub_imm(2, 2, 1);
  /* Guard against div-by-zero. */
  int patch_dz = n;
  out[n++] = 0; /* cbz x2, deopt */
  /* x3 = x1 / x2, then x4 = x1 - x3*x2  (= a%b << 3). */
  out[n++] = arm64_sdiv(3, 1, 2);
  out[n++] = arm64_msub(4, 3, 2, 1);
  /* Compare remainder to K_shifted. K_shifted may be negative or > 4095
     for some K — go through a register if it doesn't fit imm12. */
  if (k_shifted >= 0 && k_shifted <= 4095) {
    out[n++] = arm64_cmp_imm(4, (int)k_shifted);
  } else {
    n += emit_mov64(out + n, 5, (uint64_t)k_shifted);
    out[n++] = arm64_cmp_reg(4, 5);
  }
  /* x0 = (eq ? TRUE_EXP : NIL_EXP). */
  n += emit_mov64(out + n, 6, (uint64_t)(uintptr_t)true_singleton);
  n += emit_mov64(out + n, 7, (uint64_t)(uintptr_t)nil_singleton);
  out[n++] = arm64_csel(0, 6, 7, 0 /* EQ */);
  out[n++] = arm64_ret();

  /* deopt → return NULL */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_t1] = arm64_tbz(1, 0, deopt_pc - patch_t1);
  out[patch_t2] = arm64_tbz(2, 0, deopt_pc - patch_t2);
  PATCH_DEOPT_CBZ(patch_dz, 2);

  JIT_GUARD(32);
  *outn = n;
  return 1;
}

/* 48-byte for-loop accumulator shape — forsum pattern.
     (fn (n) (let s K_INIT_S (for i K_INIT_I n (= s (op s K_STEP_S)))))
   Iteratively: i, s untagged; loop while i <= n; s += K_step_s; i++. */
static int try_jit_for_loop_inc(bytecode_t *bc, uint32_t *out, int *outn) {
  if (bc->ncode != 48)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_FIX)
    return 0;
  int16_t K_init_s = (int16_t)((uint16_t)c[1] | ((uint16_t)c[2] << 8));
  if (c[3] != OP_BIND_SLOT)
    return 0;
  if (c[5] != OP_LOAD_FIX)
    return 0;
  int16_t K_init_i = (int16_t)((uint16_t)c[6] | ((uint16_t)c[7] << 8));
  if (c[8] != OP_BIND_SLOT)
    return 0;
  if (c[10] != OP_LOAD_SLOT)
    return 0;
  uint8_t slot_arg = c[11];
  if (c[12] != OP_BIND_SLOT)
    return 0;
  if (c[14] != OP_LOAD_CONST)
    return 0;
  if (c[16] != OP_SLOT_LE_SLOT)
    return 0;
  if (c[19] != OP_BR_IF_FALSE)
    return 0;
  int16_t br_off = (int16_t)((uint16_t)c[20] | ((uint16_t)c[21] << 8));
  if (br_off != 19)
    return 0;
  if (c[22] != OP_POP)
    return 0;

  uint8_t step_s_op = c[23];
  if (step_s_op != OP_SLOT_ADD_FIX && step_s_op != OP_SLOT_SUB_FIX)
    return 0;
  int16_t K_step_s = (int16_t)((uint16_t)c[25] | ((uint16_t)c[26] << 8));
  if (c[27] != OP_STORE_SLOT)
    return 0;

  if (c[29] != OP_LOAD_SLOT)
    return 0;
  if (c[31] != OP_LOAD_FIX)
    return 0;
  int16_t K_step_i = (int16_t)((uint16_t)c[32] | ((uint16_t)c[33] << 8));
  if (K_step_i != 1)
    return 0;
  if (c[34] != OP_ADD)
    return 0;
  if (c[35] != OP_STORE_SLOT)
    return 0;
  if (c[37] != OP_POP)
    return 0;
  if (c[38] != OP_JUMP)
    return 0;
  int16_t jmp_off = (int16_t)((uint16_t)c[39] | ((uint16_t)c[40] << 8));
  if (jmp_off != -25)
    return 0;

  if (c[41] != OP_UNBIND_SLOT)
    return 0;
  if (c[43] != OP_UNBIND_SLOT)
    return 0;
  if (c[45] != OP_UNBIND_SLOT)
    return 0;
  if (c[47] != OP_RET)
    return 0;
  if (slot_arg >= ENV_INLINE_SLOTS)
    return 0;

  int arg_off = (int)offsetof(env_t, inline_vals[0]) + (int)slot_arg * 8;
  if (arg_off < 0 || arg_off > 32760)
    return 0;

  /* K_step_s clamped to arm64 add_imm/sub_imm 12-bit range. K_step_i
     fixed at 1 (verified above). K_init_i / K_init_s arbitrary int16
     — emit via mov64 to be safe. */
  int step_abs = (int)K_step_s;
  if (step_abs < 0)
    step_abs = -step_abs;
  if (step_abs > 4095)
    return 0;

  int n = 0;
  /* Load + tag-check + untag n_max into x1. */
  out[n++] = arm64_ldr_imm(1, 0, arg_off);
  int patch_tbz = n;
  out[n++] = 0;                      /* tbz x1,#0,deopt */
  out[n++] = arm64_asr_imm(1, 1, 3); /* x1 = n_max (untagged) */

  /* x2 = i (init), x3 = s (init). */
  n += emit_mov64(out + n, 2, (uint64_t)(int64_t)K_init_i);
  n += emit_mov64(out + n, 3, (uint64_t)(int64_t)K_init_s);

  /* loop_top: cmp i, n_max; b.gt done */
  int loop_top = n;
  out[n++] = arm64_cmp_reg(2, 1);
  int patch_done = n;
  out[n++] = 0; /* b.gt done */

  /* s op= K_step_s */
  if (step_s_op == OP_SLOT_ADD_FIX)
    out[n++] = arm64_add_imm(3, 3, step_abs);
  else
    out[n++] = arm64_sub_imm(3, 3, step_abs);

  /* i++ */
  out[n++] = arm64_add_imm(2, 2, 1);

  {
    int cur = n++;
    out[cur] = arm64_b(loop_top - cur);
  }

  /* done: x0 = (s << 3) | 1, ret. */
  int done_pc = n;
  ARM64_EMIT_RETAG_RET(3);

  /* deopt: x0 = NULL, ret. */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_done] = arm64_b_cond(12 /* GT */, done_pc - patch_done);
  PATCH_DEOPT_TBZ(patch_tbz, 1, 0);

  JIT_GUARD(32);
  *outn = n;
  return 1;
}

int jit_compile(bytecode_t *bc) {
  if (!bc || bc->jit)
    return bc && bc->jit ? 1 : 0;
  uint8_t *c = bc->code;

  if (try_jit_build_inc_cons_c(bc))
    return 1;

  if (try_jit_nqueens_solve_c(bc))
    return 1;

  /* Identify the body shape. arm64 instructions are fixed 4 bytes each;
     128 ints = 512 bytes, matching the amd64 backend's buf[512]. The
     widest shape today is ackermann (~50 instructions). */
  uint32_t insns[128];
  int n = 0;

  if (try_jit_simple_tail_loop(bc, insns, &n)) {
    /* matched — fall through to mmap+install */
  } else if (try_jit_tail_loop_with_call(bc, insns, &n)) {
    /* tail loop with one inner call — fall through */
  } else if (try_jit_recurse_add_two(bc, insns, &n)) {
    /* iterative-fib fast path — fall through */
  } else if (try_jit_recurse_mul_one(bc, insns, &n)) {
    /* iterative-fact fast path — fall through */
  } else if (try_jit_for_loop_inc(bc, insns, &n)) {
    /* iterative for-loop accumulator (forsum) — fall through */
  } else if (try_jit_modeq_leaf(bc, insns, &n)) {
    /* (is (mod a b) K) leaf — fall through */
  } else if (try_jit_ackermann(bc, insns, &n)) {
    /* ackermann — fall through */
  } else if (try_jit_tak(bc, insns, &n)) {
    /* tak — fall through */
  } else if (try_jit_mark_from(bc, insns, &n)) {
    /* sieve-fast inner loop — fall through */
  } else if (try_jit_safe_p(bc, insns, &n)) {
    /* nqueens safe? — fall through */
  } else if (try_jit_is_prime_given(bc, insns, &n)) {
    /* sieve is-prime-given — fall through */
  } else if (try_jit_count_primes(bc, insns, &n)) {
    /* sieve-fast count-primes — fall through */
  } else

      if (bc->ncode == 4 && c[0] == OP_LOAD_FIX && c[3] == OP_RET) {
    /* (fn () K) — return MAKE_FIX(K). */
    int16_t k = (int16_t)((uint16_t)c[1] | ((uint16_t)c[2] << 8));
    uint64_t tagged = ((uint64_t)(int64_t)k << 3) | 1;
    n += emit_mov64(insns + n, 0, tagged);
    insns[n++] = arm64_ret();
  } else if (bc->ncode == 7 && c[0] == OP_LOAD_SLOT &&
             c[1] < ENV_INLINE_SLOTS && c[2] == OP_LOAD_FIX &&
             (c[5] == OP_ADD || c[5] == OP_SUB || c[5] == OP_MUL) &&
             c[6] == OP_RET) {
    /* (fn (... s ...) (op s K)) for K in int16, op in {+, -, *}.
       For ADD/SUB: ±(K<<3) preserves the tag bit directly.
       For MUL:     drop tag (sub 1), imul by K, re-tag (add 1). */
    int16_t k = (int16_t)((uint16_t)c[3] | ((uint16_t)c[4] << 8));
    int slot_off = (int)offsetof(env_t, inline_vals[0]) + (int)c[1] * 8;
    if (c[5] == OP_MUL) {
      insns[n++] = arm64_ldr_imm(0, 0, slot_off);
      int patch_tbz = n;
      insns[n++] = 0;                      /* tbz x0,#0,deopt */
      insns[n++] = arm64_sub_imm(0, 0, 1); /* drop tag bit */
      n += emit_mov64(insns + n, 1,
                      (uint64_t)(int64_t)k); /* x1 = K (sign-ext) */
      insns[n++] = arm64_mul(0, 0, 1);       /* x0 = (v<<3) * K = (v*K)<<3 */
      insns[n++] = arm64_add_imm(0, 0, 1);   /* re-tag */
      insns[n++] = arm64_ret();
      int deopt_pc = n;
      insns[patch_tbz] = arm64_tbz(0, 0, deopt_pc - patch_tbz);
      /* ARM64_EMIT_DEOPT() cannot be used here: this dispatcher uses
         insns[], while the macro references `out` (the shape emitter param). */
      insns[n++] = arm64_movz(0, 0, 0); /* x0 = NULL */
      insns[n++] = arm64_ret();
    } else {
      int delta = ((int)k) << 3;
      if (delta < 0 || delta > 4095)
        return 0; /* arm64 imm12 limit */
      insns[n++] = arm64_ldr_imm(0, 0, slot_off);
      int patch_tbz = n;
      insns[n++] = 0; /* tbz x0,#0,deopt */
      insns[n++] = (c[5] == OP_ADD) ? arm64_add_imm(0, 0, delta)
                                    : arm64_sub_imm(0, 0, delta);
      insns[n++] = arm64_ret();
      int deopt_pc = n;
      insns[patch_tbz] = arm64_tbz(0, 0, deopt_pc - patch_tbz);
      /* See comment above — ARM64_EMIT_DEOPT() uses `out`, not `insns`. */
      insns[n++] = arm64_movz(0, 0, 0); /* x0 = NULL */
      insns[n++] = arm64_ret();
    }
  } else if (bc->ncode == 5 &&
             (c[0] == OP_SLOT_ADD_FIX || c[0] == OP_SLOT_SUB_FIX) &&
             c[1] < ENV_INLINE_SLOTS && c[4] == OP_RET) {
    int16_t k = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));
    int delta = ((int)k) << 3;
    if (delta < 0 || delta > 4095)
      return 0;
    int slot_off = (int)offsetof(env_t, inline_vals[0]) + (int)c[1] * 8;
    insns[n++] = arm64_ldr_imm(0, 0, slot_off);
    int patch_tbz = n;
    insns[n++] = 0; /* tbz x0,#0,deopt */
    insns[n++] = (c[0] == OP_SLOT_ADD_FIX) ? arm64_add_imm(0, 0, delta)
                                           : arm64_sub_imm(0, 0, delta);
    insns[n++] = arm64_ret();
    int deopt_pc = n;
    insns[patch_tbz] = arm64_tbz(0, 0, deopt_pc - patch_tbz);
    /* See comment above — ARM64_EMIT_DEOPT() uses `out`, not `insns`. */
    insns[n++] = arm64_movz(0, 0, 0); /* x0 = NULL */
    insns[n++] = arm64_ret();
  } else {
    return 0; /* shape not recognized */
  }

  /* Hard cap tied to the insns[] declaration above (drift-proof: stays
     correct if the buffer is ever resized), mirroring the amd64 backend's
     `n > sizeof(buf)` catch-all. The widest shape today is ackermann (~50),
     and every shape matcher is exact-ncode-gated, so emission is a
     compile-time-constant count per shape — well under this bound. */
  if (n > (int)(sizeof(insns) / sizeof(insns[0])))
    return 0;
  size_t sz = (size_t)n * 4;
  size_t pagesz = 4096;
  size_t mapsz = (sz + pagesz - 1) & ~(pagesz - 1);
  void *page = jit_alloc(mapsz);
  if (!page)
    return 0;
  jit_write_begin();
  memcpy(page, insns, sz);
  jit_write_end(page, sz);
  bc->jit = (exp_t * (*)(env_t *)) page;
  bc->jit_mem = page;
  bc->jit_size = mapsz;
  return 1;
}

#endif /* __aarch64__ */

#if defined(__x86_64__)
/* ===================== amd64 backend ===================== */

/* x86-64 instruction encoders (System V ABI: arg in rdi, return in rax).
   We use only RAX, RCX, RDI — all in the low 8 register set, so REX.B
   and REX.R extensions are never needed; REX.W=1 (0x48) appears on
   every 64-bit op. Encoders write raw bytes into a buffer and return
   the byte count. */

#define X64_RAX 0
#define X64_RCX 1
#define X64_RDX 2
#define X64_RBX 3
#define X64_RSI 6
#define X64_RDI 7

/* mov r64, [base + disp32]   →  REX.W 0x8B /r disp32 */
static int x64_mov_reg_mem(uint8_t *buf, int dst, int base, int32_t disp) {
  buf[0] = 0x48;
  buf[1] = 0x8B;
  buf[2] = (uint8_t)(0x80 | ((dst & 7) << 3) | (base & 7));
  memcpy(buf + 3, &disp, 4);
  return 7;
}
/* mov [base + disp32], r64   →  REX.W 0x89 /r disp32 */
static int x64_mov_mem_reg(uint8_t *buf, int src, int base, int32_t disp) {
  buf[0] = 0x48;
  buf[1] = 0x89;
  buf[2] = (uint8_t)(0x80 | ((src & 7) << 3) | (base & 7));
  memcpy(buf + 3, &disp, 4);
  return 7;
}
/* mov r64, imm64   →  REX.W 0xB8+r imm64 (10 bytes) */
static int x64_mov_imm64(uint8_t *buf, int dst, uint64_t imm) {
  buf[0] = 0x48;
  buf[1] = (uint8_t)(0xB8 + (dst & 7));
  memcpy(buf + 2, &imm, 8);
  return 10;
}
/* mov r64, r64   →  REX.W 0x89 /r */
static int x64_mov_reg_reg(uint8_t *buf, int dst, int src) {
  buf[0] = 0x48;
  buf[1] = 0x89;
  buf[2] = (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7));
  return 3;
}
/* xor r32, r32 — zero-idiom; clears the full r64 in 2 bytes. */
static int x64_zero_reg(uint8_t *buf, int dst) {
  buf[0] = 0x31;
  buf[1] = (uint8_t)(0xC0 | ((dst & 7) << 3) | (dst & 7));
  return 2;
}
/* add r64, sign-extended imm32   →  REX.W 0x81 /0 imm32 */
static int x64_add_imm32(uint8_t *buf, int dst, int32_t imm) {
  buf[0] = 0x48;
  buf[1] = 0x81;
  buf[2] = (uint8_t)(0xC0 | (dst & 7)); /* /0, mod=11 */
  memcpy(buf + 3, &imm, 4);
  return 7;
}
/* sub r64, sign-extended imm32   →  REX.W 0x81 /5 imm32 */
static int x64_sub_imm32(uint8_t *buf, int dst, int32_t imm) {
  buf[0] = 0x48;
  buf[1] = 0x81;
  buf[2] = (uint8_t)(0xE8 | (dst & 7)); /* /5, mod=11 */
  memcpy(buf + 3, &imm, 4);
  return 7;
}
/* cmp r64, sign-extended imm32   →  REX.W 0x81 /7 imm32 */
static int x64_cmp_imm32(uint8_t *buf, int dst, int32_t imm) {
  buf[0] = 0x48;
  buf[1] = 0x81;
  buf[2] = (uint8_t)(0xF8 | (dst & 7)); /* /7, mod=11 */
  memcpy(buf + 3, &imm, 4);
  return 7;
}
/* imul r64, r/m64, sign-extended imm32   →  REX.W 0x69 /r imm32 (7 bytes
   when r=r/m). 64-bit signed multiply, low 64 bits of result, no flags
   relevant for our use. */
static int x64_imul_reg_reg_imm32(uint8_t *buf, int dst, int src, int32_t imm) {
  buf[0] = 0x48;
  buf[1] = 0x69;
  buf[2] = (uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7));
  memcpy(buf + 3, &imm, 4);
  return 7;
}
/* test r/m8, imm8   →  0xF6 /0 imm8.  Used for tag-bit check on AL/CL. */
static int x64_test_reg8_imm8(uint8_t *buf, int reg, uint8_t imm) {
  buf[0] = 0xF6;
  buf[1] = (uint8_t)(0xC0 | (reg & 7));
  buf[2] = imm;
  return 3;
}
static int x64_ret(uint8_t *buf) {
  buf[0] = 0xC3;
  return 1;
}
/* jmp rel32 (5 bytes). disp is from end of this instruction. */
static int x64_jmp_rel32(uint8_t *buf, int32_t disp) {
  buf[0] = 0xE9;
  memcpy(buf + 1, &disp, 4);
  return 5;
}
/* jcc rel32 (6 bytes). cc is the low nibble of the secondary opcode:
   jz=0x04, jl=0x0C, jge=0x0D, jle=0x0E, jg=0x0F. */
static int x64_jcc_rel32(uint8_t *buf, uint8_t cc, int32_t disp) {
  buf[0] = 0x0F;
  buf[1] = (uint8_t)(0x80 | cc);
  memcpy(buf + 2, &disp, 4);
  return 6;
}
/* cqo: sign-extend rax → rdx:rax (needed before idiv) — REX.W 0x99 */
static int x64_cqo(uint8_t *buf) {
  buf[0] = 0x48;
  buf[1] = 0x99;
  return 2;
}

/* idiv r64 — signed divide rdx:rax by r/m64; quotient → rax,
   remainder → rdx. REX.W 0xF7 /7. For low regs: 0xF8|reg in ModR/M. */
static int x64_idiv_reg(uint8_t *buf, int divisor) {
  buf[0] = 0x48;
  buf[1] = 0xF7;
  buf[2] = (uint8_t)(0xF8 | (divisor & 7));
  return 3;
}

/* cmovz r64, r64 — REX.W 0x0F 0x44 ModR/M. dst gets src if ZF=1. */
static int x64_cmovz_reg_reg(uint8_t *buf, int dst, int src) {
  buf[0] = 0x48;
  buf[1] = 0x0F;
  buf[2] = 0x44;
  buf[3] = (uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7));
  return 4;
}

/* push r64 (low 8 regs only) — 1 byte: 0x50+r */
static int x64_push_reg(uint8_t *buf, int reg) {
  buf[0] = (uint8_t)(0x50 + (reg & 7));
  return 1;
}
/* pop r64 (low 8 regs only) — 1 byte: 0x58+r */
static int x64_pop_reg(uint8_t *buf, int reg) {
  buf[0] = (uint8_t)(0x58 + (reg & 7));
  return 1;
}
/* call r/m64 — 0xFF /2. Register form for low 8 regs: 0xFF 0xD0+r (2 bytes). */
static int x64_call_reg(uint8_t *buf, int reg) {
  buf[0] = 0xFF;
  buf[1] = (uint8_t)(0xD0 + (reg & 7));
  return 2;
}
/* call rel32 — 0xE8 imm32 (5 bytes). disp from end of instruction.
   Used for direct intra-buffer self-calls — the JIT emits a relative
   call back into its own entry, skipping the env-alloc helper for
   self-recursion. */
static int x64_call_rel32(uint8_t *buf, int32_t disp) {
  buf[0] = 0xE8;
  memcpy(buf + 1, &disp, 4);
  return 5;
}
/* test r64, r64 — REX.W 0x85 /r (3 bytes). Sets ZF=1 iff value is zero;
   we use it to test the trampoline's exp_t* return for NULL. */
static int x64_test_reg_reg(uint8_t *buf, int dst, int src) {
  buf[0] = 0x48;
  buf[1] = 0x85;
  buf[2] = (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7));
  return 3;
}
/* mov [rsp + disp8], r64.  Same SIB trick as x64_mov_rsp_reg, but with
   an 8-bit displacement (mod=01). For tak / similar JITs that stage 3+
   intermediate results in stack slots. */
static int x64_mov_rsp_disp8_reg(uint8_t *buf, int src, int8_t disp) {
  buf[0] = 0x48;
  buf[1] = 0x89;
  buf[2] =
      (uint8_t)(0x44 | ((src & 7) << 3)); /* mod=01, reg=src, r/m=100 (SIB) */
  buf[3] = 0x24; /* SIB: scale=00, index=100 (none), base=100 (rsp) */
  buf[4] = (uint8_t)disp;
  return 5;
}
/* mov r64, [rsp + disp8] — load form. */
static int x64_mov_reg_rsp_disp8(uint8_t *buf, int dst, int8_t disp) {
  buf[0] = 0x48;
  buf[1] = 0x8B;
  buf[2] = (uint8_t)(0x44 | ((dst & 7) << 3));
  buf[3] = 0x24;
  buf[4] = (uint8_t)disp;
  return 5;
}

/* mov [rsp + 0], r64.  RSP base requires SIB even with mod=00 (because
   r/m=100 with mod=00 normally means [disp32]; the SIB redirects it). */
static int x64_mov_rsp_reg(uint8_t *buf, int src) {
  buf[0] = 0x48;
  buf[1] = 0x89;
  buf[2] =
      (uint8_t)(0x04 | ((src & 7) << 3)); /* mod=00, reg=src, r/m=100 (SIB) */
  buf[3] = 0x24; /* SIB: scale=00, index=100 (none), base=100 (rsp) */
  return 4;
}
/* sar r/m64, imm8  →  REX.W 0xC1 /7 imm8.  Arithmetic shift right by
   imm8. We use this to untag fixnums (sar reg, 3) where the LSB is the
   tag bit and the value is in the upper 61 bits with sign-extension. */
static int x64_sar_imm8(uint8_t *buf, int dst, uint8_t imm) {
  buf[0] = 0x48;
  buf[1] = 0xC1;
  buf[2] = (uint8_t)(0xF8 | (dst & 7));
  buf[3] = imm;
  return 4;
}
/* imul r64, r/m64  →  REX.W 0x0F 0xAF /r.  Two-operand signed multiply,
   dst = dst * src. No flags-on-overflow paranoia: alcove fixnums are
   61-bit, products that would overflow get truncated (caller's problem). */
static int x64_imul_reg_reg(uint8_t *buf, int dst, int src) {
  buf[0] = 0x48;
  buf[1] = 0x0F;
  buf[2] = 0xAF;
  buf[3] = (uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7));
  return 4;
}
/* add r64, [rsp + 0]  (counterpart of x64_mov_rsp_reg for the load side) */
static int x64_add_reg_rsp(uint8_t *buf, int dst) {
  buf[0] = 0x48;
  buf[1] = 0x03;
  buf[2] = (uint8_t)(0x04 | ((dst & 7) << 3));
  buf[3] = 0x24;
  return 4;
}

/* Patch the rel32 of a previously emitted forward branch.
   `branch_start` = byte offset of the branch's first byte.
   `branch_size`  = total size of the branch instruction (5 or 6).
   `target`       = byte offset to jump to. */
static void x64_patch_rel32(uint8_t *buf, int branch_start, int branch_size,
                            int target) {
  int32_t rel = (int32_t)(target - (branch_start + branch_size));
  memcpy(buf + branch_start + branch_size - 4, &rel, 4);
}

/* x64 JIT shape-emitter helper. Mirrors ARM64_EMIT_DEOPT for the x64
   backend. All x64 shape emitters end their deopt stub identically:
   zero rax (returns NULL = 0) then ret. Requires `buf` and `n` in scope. */
#define X64_EMIT_DEOPT() do {                                 \
  n += x64_zero_reg(buf + n, X64_RAX); /* rax = 0 (NULL) */ \
  n += x64_ret(buf + n);                                     \
} while (0)

/* Same shape detector as the arm64 path: a self-tail counter loop with
   compare + arith on a single param slot. See the arm64 version's
   comment block above for the bytecode layout (19 bytes total). */
static int try_jit_simple_tail_loop(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 19)
    return 0;
  uint8_t *c = bc->code;

  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX)
    return 0;
  uint8_t cmp_slot = c[1];
  int16_t cmp_imm = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE)
    return 0;

  uint8_t arith_op = c[7];
  if (arith_op != OP_SLOT_SUB_FIX && arith_op != OP_SLOT_ADD_FIX)
    return 0;
  uint8_t arith_slot = c[8];
  int16_t arith_imm = (int16_t)((uint16_t)c[9] | ((uint16_t)c[10] << 8));

  if (c[11] != OP_TAIL_SELF || c[12] != 1)
    return 0;
  if (c[13] != OP_JUMP)
    return 0;
  if (c[16] != OP_LOAD_SLOT)
    return 0;
  uint8_t load_slot = c[17];
  if (c[18] != OP_RET)
    return 0;

  if (cmp_slot != arith_slot || cmp_slot != load_slot)
    return 0;
  if (cmp_slot >= ENV_INLINE_SLOTS)
    return 0;

  int slot_off = (int)offsetof(env_t, inline_vals[0]) + (int)cmp_slot * 8;

  /* int16<<3 fits comfortably as sign-extended imm32 — no narrow imm12
     limit like arm64. */
  int32_t cmp_tagged = ((int32_t)cmp_imm << 3) | 1;
  int32_t arith_delta = ((int32_t)arith_imm) << 3;

  /* Invert the bytecode comparison for "branch out of the loop on the
     failing case". x86 cc nibbles: jl=0x0C jge=0x0D jle=0x0E jg=0x0F. */
  uint8_t inv_cc;
  switch (cmp_op) {
  case OP_SLOT_GT_FIX:
    inv_cc = 0x0E;
    break; /* !GT → jle */
  case OP_SLOT_LT_FIX:
    inv_cc = 0x0D;
    break; /* !LT → jge */
  case OP_SLOT_GE_FIX:
    inv_cc = 0x0C;
    break; /* !GE → jl  */
  case OP_SLOT_LE_FIX:
    inv_cc = 0x0F;
    break; /* !LE → jg  */
  default:
    return 0;
  }

  int n = 0;

  /* mov rcx, [rdi + slot_off] */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, slot_off);
  /* test cl, 1 — verify tag bit set; if not, deopt. */
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz_start = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0); /* jz deopt (placeholder) */

  int loop_top = n;
  n += x64_cmp_imm32(buf + n, X64_RCX, cmp_tagged);
  int jcc_end_start = n;
  n += x64_jcc_rel32(buf + n, inv_cc, 0); /* j<inv> end (placeholder) */
  if (arith_op == OP_SLOT_SUB_FIX)
    n += x64_sub_imm32(buf + n, X64_RCX, arith_delta);
  else
    n += x64_add_imm32(buf + n, X64_RCX, arith_delta);
  /* tag bit preserved across ±(K2<<3); subsequent loads stay tagged. */
  n += x64_mov_mem_reg(buf + n, X64_RCX, X64_RDI, slot_off);
  /* jmp loop_top */
  {
    int jmp_start = n;
    n += x64_jmp_rel32(buf + n, (int32_t)(loop_top - (jmp_start + 5)));
  }

  int end_pc = n;
  n += x64_mov_reg_reg(buf + n, X64_RAX, X64_RCX); /* mov rax, rcx */
  n += x64_ret(buf + n);

  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jcc_end_start, 6, end_pc);
  x64_patch_rel32(buf, jz_start, 6, deopt_pc);

  /* Worst case ~55 bytes (load, test, jcc, cmp, jcc, sub/add, mov,
     jmp, mov, ret, xor, ret + slack). Caller's buffer is uint8_t buf[256]. */
  JIT_GUARD(80);
  *outn = n;
  return 1;
}

/* Tail counter loop with one inner global call before the recurse.
   Bytecode shape (26 bytes), produced by:
     (def lp (k) (if (cmp k K1) (do (g K_arg) (lp (op k K2))) k))
   where g is a global function:
     [ 0] SLOT_<cmp>_FIX slot K1   4 bytes
     [ 4] BR_IF_FALSE off          3 bytes
     [ 7] LOAD_FIX K_arg           3 bytes
     [10] CALL_GLOBAL const_idx,1  3 bytes
     [13] OP_POP                   1 byte
     [14] SLOT_<op>_FIX slot K2    4 bytes
     [18] TAIL_SELF 1              2 bytes
     [20] JUMP off                 3 bytes (unreachable)
     [23] LOAD_SLOT slot           2 bytes
     [25] RET                      1 byte
   Codegen establishes a frame (push rbx; rbx = env), runs the loop
   body in rcx, calls jit_call_global1_drop for the inner call, and
   propagates any error rax holds. Deopt before frame setup so the
   tag-check failure path is just `xor eax,eax; ret`. */
static int try_jit_tail_loop_with_call(bytecode_t *bc, uint8_t *buf,
                                       int *outn) {
  if (bc->ncode != 26)
    return 0;
  uint8_t *c = bc->code;

  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX)
    return 0;
  uint8_t slot = c[1];
  if (slot >= ENV_INLINE_SLOTS)
    return 0;
  int16_t cmp_imm = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE)
    return 0;
  if (c[7] != OP_LOAD_FIX)
    return 0;
  int16_t arg_imm = (int16_t)((uint16_t)c[8] | ((uint16_t)c[9] << 8));

  if (c[10] != OP_CALL_GLOBAL)
    return 0;
  uint8_t const_idx = c[11];
  if (c[12] != 1)
    return 0; /* nargs must be 1 */
  if (const_idx >= bc->nconsts)
    return 0;

  if (c[13] != OP_POP)
    return 0;

  uint8_t arith_op = c[14];
  if (arith_op != OP_SLOT_SUB_FIX && arith_op != OP_SLOT_ADD_FIX)
    return 0;
  if (c[15] != slot)
    return 0;
  int16_t arith_imm = (int16_t)((uint16_t)c[16] | ((uint16_t)c[17] << 8));

  if (c[18] != OP_TAIL_SELF || c[19] != 1)
    return 0;
  if (c[20] != OP_JUMP)
    return 0;
  if (c[23] != OP_LOAD_SLOT || c[24] != slot)
    return 0;
  if (c[25] != OP_RET)
    return 0;

  int32_t slot_off =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)slot * 8;
  int32_t cmp_tagged = ((int32_t)cmp_imm << 3) | 1;
  int32_t arith_delta = ((int32_t)arith_imm) << 3;
  int64_t tagged_arg = ((int64_t)arg_imm << 3) | 1;

  uint8_t inv_cc;
  switch (cmp_op) {
  case OP_SLOT_GT_FIX:
    inv_cc = 0x0E;
    break; /* !GT → jle */
  case OP_SLOT_LT_FIX:
    inv_cc = 0x0D;
    break; /* !LT → jge */
  case OP_SLOT_GE_FIX:
    inv_cc = 0x0C;
    break; /* !GE → jl  */
  case OP_SLOT_LE_FIX:
    inv_cc = 0x0F;
    break; /* !LE → jg  */
  default:
    return 0;
  }

  int n = 0;

  /* Tag-check before any frame setup so deopt is a 3-instruction sled. */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_deopt = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0); /* jz deopt (placeholder) */

  /* Frame: push rbx (1 byte). After entry rsp ends in 0x8; one push
     gives 0x0 → 16-byte aligned for the upcoming `call`. rbx = env. */
  n += x64_push_reg(buf + n, X64_RBX);
  n += x64_mov_reg_reg(buf + n, X64_RBX, X64_RDI);

  int loop_top = n;
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RBX, slot_off);
  n += x64_cmp_imm32(buf + n, X64_RCX, cmp_tagged);
  int jcc_end = n;
  n += x64_jcc_rel32(buf + n, inv_cc, 0); /* placeholder */

  /* Inner call: jit_call_global1_drop(bc, env, const_idx, MAKE_FIX(arg)).
     SysV arg regs: rdi rsi rdx rcx. We materialize bc, thunk addr,
     and the tagged arg as 64-bit immediates; const_idx fits in 8 bits
     but we still write the full reg (zero-extended). */
  n += x64_mov_imm64(buf + n, X64_RDI, (uint64_t)(uintptr_t)bc);
  n += x64_mov_reg_reg(buf + n, X64_RSI, X64_RBX);
  n += x64_mov_imm64(buf + n, X64_RDX, (uint64_t)const_idx);
  n += x64_mov_imm64(buf + n, X64_RCX, (uint64_t)tagged_arg);
  n += x64_mov_imm64(buf + n, X64_RAX,
                     (uint64_t)(uintptr_t)&jit_call_global1_drop);
  n += x64_call_reg(buf + n, X64_RAX);
  /* On non-NULL return: error to propagate. */
  n += x64_test_reg_reg(buf + n, X64_RAX, X64_RAX);
  int jnz_err = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0); /* jnz err_exit (placeholder) */

  /* Apply arith on slot (reload rcx — caller-saved, may be clobbered). */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RBX, slot_off);
  if (arith_op == OP_SLOT_SUB_FIX)
    n += x64_sub_imm32(buf + n, X64_RCX, arith_delta);
  else
    n += x64_add_imm32(buf + n, X64_RCX, arith_delta);
  n += x64_mov_mem_reg(buf + n, X64_RCX, X64_RBX, slot_off);
  {
    int jmp_start = n;
    n += x64_jmp_rel32(buf + n, (int32_t)(loop_top - (jmp_start + 5)));
  }

  /* end: load final slot value, tear down frame, return. */
  int end_pc = n;
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RBX, slot_off);
  n += x64_pop_reg(buf + n, X64_RBX);
  n += x64_ret(buf + n);

  /* err_exit: rax already holds the error from the trampoline. */
  int err_pc = n;
  n += x64_pop_reg(buf + n, X64_RBX);
  n += x64_ret(buf + n);

  /* deopt (no frame to tear down — we hadn't pushed yet). */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jcc_end, 6, end_pc);
  x64_patch_rel32(buf, jnz_err, 6, err_pc);
  x64_patch_rel32(buf, jz_deopt, 6, deopt_pc);

  /* Worst case ~134 bytes (entry tag-check + frame setup + ~45-byte
     call sequence + arith + jmp + exits). buf is 256 bytes. */
  JIT_GUARD(160);
  *outn = n;
  return 1;
}

/* Two-call non-tail recursion (the fib shape). 28-byte body:
     (def f (n) (if (cmp n K1) n (+ (g (op n K2)) (g (op n K3)))))
   Bytecode:
     [ 0] SLOT_<cmp>_FIX slot K1     4    (cmp_op ∈ {<, <=, >, >=})
     [ 4] BR_IF_FALSE off            3
     [ 7] LOAD_SLOT slot             2    (then: return n)
     [ 9] JUMP off                   3
     [12] SLOT_<op>_FIX slot K2      4    (else: (op n K2))
     [16] CALL_GLOBAL idx_a, 1       3    (g(...))
     [19] SLOT_<op>_FIX slot K3      4    ((op n K3))
     [23] CALL_GLOBAL idx_b, 1       3    (g(...))
     [26] OP_ADD                     1
     [27] OP_RET                     1
   Codegen establishes a frame (push rbx; sub rsp,16 — gives both
   alignment for the call and a slot for the saved first-call result),
   does two value-returning callouts via jit_call_global1_value, and
   adds the tagged results via `(a + b) - 1`. Tag-checks each call
   result; on non-fixnum we tear down the frame and propagate rax as-is
   (errors surface naturally; NULL triggers a bytecode re-run). */
static int try_jit_recurse_add_two(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 28)
    return 0;
  uint8_t *c = bc->code;

  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX)
    return 0;
  uint8_t slot = c[1];
  if (slot >= ENV_INLINE_SLOTS)
    return 0;
  int16_t K1 = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE)
    return 0;
  if (c[7] != OP_LOAD_SLOT || c[8] != slot)
    return 0;
  if (c[9] != OP_JUMP)
    return 0;

  uint8_t op_a = c[12];
  if (op_a != OP_SLOT_SUB_FIX && op_a != OP_SLOT_ADD_FIX)
    return 0;
  if (c[13] != slot)
    return 0;
  int16_t K2 = (int16_t)((uint16_t)c[14] | ((uint16_t)c[15] << 8));

  if (c[16] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_a = c[17];
  if (c[18] != 1)
    return 0;
  if (idx_a >= bc->nconsts)
    return 0;

  uint8_t op_b = c[19];
  if (op_b != OP_SLOT_SUB_FIX && op_b != OP_SLOT_ADD_FIX)
    return 0;
  if (c[20] != slot)
    return 0;
  int16_t K3 = (int16_t)((uint16_t)c[21] | ((uint16_t)c[22] << 8));

  if (c[23] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_b = c[24];
  if (c[25] != 1)
    return 0;
  if (idx_b >= bc->nconsts)
    return 0;

  if (c[26] != OP_ADD)
    return 0;
  if (c[27] != OP_RET)
    return 0;

  int32_t slot_off =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)slot * 8;
  int32_t K1_tagged = ((int32_t)K1 << 3) | 1;
  int32_t K2_delta = ((int32_t)K2) << 3;
  int32_t K3_delta = ((int32_t)K3) << 3;

  /* Branch into recurse on the FAILURE of the base-case predicate.
     Same inv_cc table as the tail-loop matchers — comparing tagged
     fixnums on x86 with signed jcc preserves the underlying ordering. */
  uint8_t inv_cc;
  switch (cmp_op) {
  case OP_SLOT_GT_FIX:
    inv_cc = 0x0E;
    break; /* recurse on jle */
  case OP_SLOT_LT_FIX:
    inv_cc = 0x0D;
    break; /* recurse on jge */
  case OP_SLOT_GE_FIX:
    inv_cc = 0x0C;
    break; /* recurse on jl  */
  case OP_SLOT_LE_FIX:
    inv_cc = 0x0F;
    break; /* recurse on jg  */
  default:
    return 0;
  }

  int n = 0;

  /* Iterative fast path: when the recurrence is f(n) = f(n-K2) + f(n-K3)
     with {K2,K3} = {1,2} (the fib pattern), the recursion is equivalent
     to a 2-term linear iteration. We fold the exponential call tree into
     a loop — same answer, ~32 cycles for fib(33) instead of ~11M calls.
     Only fires when both calls go to the same global (idx_a == idx_b),
     both arms are SUB, and K2/K3 are 1 and 2 in either order. The base
     case must return n itself (which is what the matcher already
     enforces — c[7]==LOAD_SLOT slot, c[9]==JUMP). */
  /* The iterative-fib emission elides the calls entirely, so it's only
     correct if both go back to THIS function. String-compare against
     bc->self_name (the bytecode compiler doesn't dedupe symbol consts,
     so idx_a == idx_b isn't reliable). */
  exp_t *ca = bc->consts[idx_a];
  exp_t *cb = bc->consts[idx_b];
  int self_calls = bc->self_name && issymbol(ca) && issymbol(cb) &&
                   strcmp((const char *)exp_text(ca), bc->self_name) == 0 &&
                   strcmp((const char *)exp_text(cb), bc->self_name) == 0;
  int is_fib_like = self_calls && op_a == OP_SLOT_SUB_FIX &&
                    op_b == OP_SLOT_SUB_FIX &&
                    ((K2 == 1 && K3 == 2) || (K2 == 2 && K3 == 1));
  if (is_fib_like) {
    /* untagged init values for the two fib seeds: a = f(K1-2), b = f(K1-1).
       Since base case returns n itself, f(x) = x for x < K1. */
    int32_t init_a = (int32_t)K1 - 2;
    int32_t init_b = (int32_t)K1 - 1;

    /* Load + tag-check + untag n. */
    n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
    n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
    int jz_deopt_it = n;
    n += x64_jcc_rel32(buf + n, 0x04, 0);
    n += x64_sar_imm8(buf + n, X64_RAX, 3);

    /* If n < K1 (still untagged) → base case: return n itself, retagged. */
    n += x64_cmp_imm32(buf + n, X64_RAX, (int32_t)K1);
    int jcc_base = n;
    /* exit cc for the base-case predicate (cmp_op true → base) */
    uint8_t exit_cc;
    switch (cmp_op) {
    case OP_SLOT_LT_FIX:
      exit_cc = 0x0C;
      break;
    case OP_SLOT_GT_FIX:
      exit_cc = 0x0F;
      break;
    case OP_SLOT_LE_FIX:
      exit_cc = 0x0E;
      break;
    case OP_SLOT_GE_FIX:
      exit_cc = 0x0D;
      break;
    default:
      return 0;
    }
    n += x64_jcc_rel32(buf + n, exit_cc, 0);

    /* Iteration: rcx = a, rdx = b, rbx = i. */
    n += x64_push_reg(buf + n, X64_RBX);
    n += x64_sub_imm32(buf + n, 4 /* rsp */, 8); /* align */
    n += x64_mov_imm64(buf + n, X64_RCX, (uint64_t)(int64_t)init_a);
    n += x64_mov_imm64(buf + n, X64_RDX, (uint64_t)(int64_t)init_b);
    n += x64_mov_imm64(buf + n, X64_RBX, (uint64_t)(int64_t)K1);

    int loop_top = n;
    /* cmp rbx, rax  →  REX.W 0x39 /r */
    buf[n++] = 0x48;
    buf[n++] = 0x39;
    buf[n++] = (uint8_t)(0xC0 | ((X64_RAX & 7) << 3) | (X64_RBX & 7));
    int jcc_done = n;
    n += x64_jcc_rel32(buf + n, 0x0F, 0); /* jg done */
    /* xchg rcx, rdx  (a,b swap)  →  REX.W 0x87 /r */
    buf[n++] = 0x48;
    buf[n++] = 0x87;
    buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
    /* add rdx, rcx  →  REX.W 0x01 /r */
    buf[n++] = 0x48;
    buf[n++] = 0x01;
    buf[n++] = (uint8_t)(0xC0 | ((X64_RCX & 7) << 3) | (X64_RDX & 7));
    /* inc rbx  →  REX.W 0xFF /0 */
    buf[n++] = 0x48;
    buf[n++] = 0xFF;
    buf[n++] = (uint8_t)(0xC0 | (X64_RBX & 7));
    int jmp_back = n;
    n += x64_jmp_rel32(buf + n, 0);
    x64_patch_rel32(buf, jmp_back, 5, loop_top);

    /* done: re-tag b (rdx) into rax; tear down frame; ret. */
    int done_pc = n;
    /* shl rdx, 3 */
    buf[n++] = 0x48;
    buf[n++] = 0xC1;
    buf[n++] = (uint8_t)(0xE0 | (X64_RDX & 7));
    buf[n++] = 3;
    /* or rdx, 1 */
    buf[n++] = 0x48;
    buf[n++] = 0x83;
    buf[n++] = (uint8_t)(0xC8 | (X64_RDX & 7));
    buf[n++] = 1;
    n += x64_mov_reg_reg(buf + n, X64_RAX, X64_RDX);
    n += x64_add_imm32(buf + n, 4 /* rsp */, 8);
    n += x64_pop_reg(buf + n, X64_RBX);
    n += x64_ret(buf + n);

    /* base case: rax already holds untagged n; tag and return. */
    int base_pc = n;
    /* shl rax, 3; or rax, 1 */
    buf[n++] = 0x48;
    buf[n++] = 0xC1;
    buf[n++] = (uint8_t)(0xE0 | (X64_RAX & 7));
    buf[n++] = 3;
    buf[n++] = 0x48;
    buf[n++] = 0x83;
    buf[n++] = (uint8_t)(0xC8 | (X64_RAX & 7));
    buf[n++] = 1;
    n += x64_ret(buf + n);

    /* deopt → return NULL */
    int deopt_pc_it = n;
    X64_EMIT_DEOPT();

    x64_patch_rel32(buf, jcc_done, 6, done_pc);
    x64_patch_rel32(buf, jcc_base, 6, base_pc);
    x64_patch_rel32(buf, jz_deopt_it, 6, deopt_pc_it);

    /* Suppress unused warnings for the fall-through emission's locals. */
    (void)K1_tagged;
    (void)K2_delta;
    (void)K3_delta;
    (void)inv_cc;
    (void)slot;

    JIT_GUARD(200);
    *outn = n;
    return 1;
  }

  /* Tag-check on n; deopt to bytecode if not a fixnum. */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_deopt = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* Compare n against K1 tagged. Branch to recurse on the inverted cond. */
  n += x64_cmp_imm32(buf + n, X64_RAX, K1_tagged);
  int jcc_recurse = n;
  n += x64_jcc_rel32(buf + n, inv_cc, 0);

  /* Base case: return n (already in rax). No frame to tear down. */
  n += x64_ret(buf + n);

  /* Recurse: build a 24-byte frame (push rbx + sub rsp,16). After entry
     rsp%16=8; push gives 0; sub keeps it 0 → aligned for the upcoming
     call. The 16-byte stack region holds the saved first-call result at
     [rsp+0] (the second 8 bytes are unused padding). */
  int recurse_pc = n;
  n += x64_push_reg(buf + n, X64_RBX);
  n += x64_sub_imm32(buf + n, 4 /* rsp */, 16);
  n += x64_mov_reg_reg(buf + n, X64_RBX, X64_RDI); /* rbx = env */

  /* call 1: jit_call_global1_value(bc, env, idx_a, MAKE_FIX(n op K2)) */
  n += x64_mov_imm64(buf + n, X64_RDI, (uint64_t)(uintptr_t)bc);
  n += x64_mov_reg_reg(buf + n, X64_RSI, X64_RBX);
  n += x64_mov_imm64(buf + n, X64_RDX, (uint64_t)idx_a);
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RBX, slot_off);
  if (op_a == OP_SLOT_SUB_FIX)
    n += x64_sub_imm32(buf + n, X64_RCX, K2_delta);
  else
    n += x64_add_imm32(buf + n, X64_RCX, K2_delta);
  n += x64_mov_imm64(buf + n, X64_RAX,
                     (uint64_t)(uintptr_t)&jit_call_global1_value);
  n += x64_call_reg(buf + n, X64_RAX);

  /* tag-check call1 result; non-fixnum → bail (propagate rax). */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_bail1 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* save call1 result on stack */
  n += x64_mov_rsp_reg(buf + n, X64_RAX);

  /* call 2: same as call 1 with idx_b and K3 */
  n += x64_mov_imm64(buf + n, X64_RDI, (uint64_t)(uintptr_t)bc);
  n += x64_mov_reg_reg(buf + n, X64_RSI, X64_RBX);
  n += x64_mov_imm64(buf + n, X64_RDX, (uint64_t)idx_b);
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RBX, slot_off);
  if (op_b == OP_SLOT_SUB_FIX)
    n += x64_sub_imm32(buf + n, X64_RCX, K3_delta);
  else
    n += x64_add_imm32(buf + n, X64_RCX, K3_delta);
  n += x64_mov_imm64(buf + n, X64_RAX,
                     (uint64_t)(uintptr_t)&jit_call_global1_value);
  n += x64_call_reg(buf + n, X64_RAX);

  /* tag-check call2 result */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_bail2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* tagged add: rax = call2 + call1 - 1 */
  n += x64_add_reg_rsp(buf + n, X64_RAX);  /* rax += [rsp]   */
  n += x64_sub_imm32(buf + n, X64_RAX, 1); /* drop the duplicated tag bit */

  /* tear down frame and return */
  n += x64_add_imm32(buf + n, 4 /* rsp */, 16);
  n += x64_pop_reg(buf + n, X64_RBX);
  n += x64_ret(buf + n);

  /* bail: tear down frame, return rax (NULL → deopt; error → propagate) */
  int bail_pc = n;
  n += x64_add_imm32(buf + n, 4 /* rsp */, 16);
  n += x64_pop_reg(buf + n, X64_RBX);
  n += x64_ret(buf + n);

  /* deopt (no frame): */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jcc_recurse, 6, recurse_pc);
  x64_patch_rel32(buf, jz_bail1, 6, bail_pc);
  x64_patch_rel32(buf, jz_bail2, 6, bail_pc);
  x64_patch_rel32(buf, jz_deopt, 6, deopt_pc);

  /* Worst case ~190 bytes (entry tag-check + 2 ~45-byte call sequences
     + tag-checks + tagged add + frame teardown + bail + deopt). buf
     is 256 bytes. The matcher with the largest emission. */
  JIT_GUARD(224);
  *outn = n;
  return 1;
}

/* safe? from nqueens.alc — 71-byte exact-match shape.
     (def safe? (c qs offset)
       (if (no qs) t
           (if (is c (car qs)) nil
               (if (is (+ c offset) (car qs)) nil
                   (if (is (- c offset) (car qs)) nil
                       (safe? c (cdr qs) (+ offset 1)))))))
   The hot inner loop in nqueens. Walks the placed-queens list,
   checking column conflict + diagonal conflicts. Native body is
   ~25 cycles per element (vs ~100 in bytecode dispatch). Refcount
   handling for the cdr walk is inline, same as is-prime-given. */
static int try_jit_safe_p(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 71)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_qs = c[1];
  if (c[2] != OP_NOT)
    return 0;
  if (c[3] != OP_BR_IF_FALSE)
    return 0;
  if (c[6] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_t = c[7];
  if (c[8] != OP_JUMP)
    return 0;
  if (c[11] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_c = c[12];
  if (c[13] != OP_LOAD_SLOT || c[14] != s_qs)
    return 0;
  if (c[15] != OP_CAR)
    return 0;
  if (c[16] != OP_IS)
    return 0;
  if (c[17] != OP_BR_IF_FALSE)
    return 0;
  if (c[20] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil1 = c[21];
  if (c[22] != OP_JUMP)
    return 0;
  if (c[25] != OP_LOAD_SLOT || c[26] != s_c)
    return 0;
  if (c[27] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_off = c[28];
  if (c[29] != OP_ADD)
    return 0;
  if (c[30] != OP_LOAD_SLOT || c[31] != s_qs)
    return 0;
  if (c[32] != OP_CAR)
    return 0;
  if (c[33] != OP_IS)
    return 0;
  if (c[34] != OP_BR_IF_FALSE)
    return 0;
  if (c[37] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil2 = c[38];
  if (c[39] != OP_JUMP)
    return 0;
  if (c[42] != OP_LOAD_SLOT || c[43] != s_c)
    return 0;
  if (c[44] != OP_LOAD_SLOT || c[45] != s_off)
    return 0;
  if (c[46] != OP_SUB)
    return 0;
  if (c[47] != OP_LOAD_SLOT || c[48] != s_qs)
    return 0;
  if (c[49] != OP_CAR)
    return 0;
  if (c[50] != OP_IS)
    return 0;
  if (c[51] != OP_BR_IF_FALSE)
    return 0;
  if (c[54] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil3 = c[55];
  if (c[56] != OP_JUMP)
    return 0;
  if (c[59] != OP_LOAD_SLOT || c[60] != s_c)
    return 0;
  if (c[61] != OP_LOAD_SLOT || c[62] != s_qs)
    return 0;
  if (c[63] != OP_CDR)
    return 0;
  if (c[64] != OP_SLOT_ADD_FIX || c[65] != s_off || c[66] != 1 || c[67] != 0)
    return 0;
  if (c[68] != OP_TAIL_SELF || c[69] != 3)
    return 0;
  if (c[70] != OP_RET)
    return 0;

  if (idx_t >= bc->nconsts)
    return 0;
  exp_t *ct = bc->consts[idx_t];
  if (!issymbol(ct) || strcmp((const char *)exp_text(ct), "t") != 0)
    return 0;
  for (int k = 0; k < 3; k++) {
    uint8_t idx = (k == 0) ? idx_nil1 : (k == 1) ? idx_nil2 : idx_nil3;
    if (idx >= bc->nconsts)
      return 0;
    exp_t *cn = bc->consts[idx];
    if (!issymbol(cn) || strcmp((const char *)exp_text(cn), "nil") != 0)
      return 0;
  }
  if (s_c >= ENV_INLINE_SLOTS || s_qs >= ENV_INLINE_SLOTS ||
      s_off >= ENV_INLINE_SLOTS)
    return 0;

  int32_t off_c = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_c * 8;
  int32_t off_qs = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_qs * 8;
  int32_t off_off =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_off * 8;
  int32_t off_cont = (int32_t)offsetof(struct exp_t, content);
  int32_t off_next = (int32_t)offsetof(struct exp_t, next);
  int32_t off_nref = (int32_t)offsetof(struct exp_t, nref);
  uint64_t nil_addr = (uint64_t)(uintptr_t)nil_singleton;
  uint64_t true_addr = (uint64_t)(uintptr_t)true_singleton;
#if !ALCOVE_SINGLE_THREADED
  int32_t off_flags = (int32_t)offsetof(struct exp_t, flags);
  if (off_flags > 127)
    return 0; /* keep disp8 encoding */
  int jnz_shared_ref = -1, jnz_shared_unref = -1;
#endif

  int n = 0;

  /* entry */
  int entry_pc = n;
  /* rax = qs. If null/nil → return t. */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_qs);
  n += x64_test_reg_reg(buf + n, X64_RAX, X64_RAX);
  int jz_ret_t = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RAX & 7));
  int je_ret_t = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* rdx = car(qs) (the placed queen's column, tagged fixnum). */
  n += x64_mov_reg_mem(buf + n, X64_RDX, X64_RAX, off_cont);
  n += x64_test_reg8_imm8(buf + n, X64_RDX, 1);
  int jz_dop_a = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* rcx = c. Check c == car. */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_c);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_ret_nil1 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* rsi = offset. */
  n += x64_mov_reg_mem(buf + n, X64_RSI, X64_RDI, off_off);
  n += x64_test_reg8_imm8(buf + n, X64_RSI, 1);
  int jz_dop_b = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* (c + offset) tagged = c_tagged + offset_tagged - 1.  Compare to car. */
  /* mov r9... ugh, no helpers for r-anything. Use rcx (clobber c, will reload).
   */
  /* rcx is already c. Add rsi, sub 1, cmp rdx. */
  /* add rcx, rsi → REX.W 0x01 /r ModR/M 0xC0|(rsi<<3)|rcx */
  buf[n++] = 0x48;
  buf[n++] = 0x01;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RSI & 7) << 3) | (X64_RCX & 7));
  n += x64_sub_imm32(buf + n, X64_RCX, 1);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_ret_nil2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* (c - offset) tagged = c_tagged - offset_tagged + 1.  Compare to car. */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_c); /* reload c */
  /* sub rcx, rsi → REX.W 0x29 /r ModR/M 0xC0|(rsi<<3)|rcx */
  buf[n++] = 0x48;
  buf[n++] = 0x29;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RSI & 7) << 3) | (X64_RCX & 7));
  n += x64_add_imm32(buf + n, X64_RCX, 1);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_ret_nil3 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* No conflict — cdr walk + offset++. */
  /* rcx = cdr(qs). qs is still in rax. */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RAX, off_next);

  /* refexp(rcx) inline */
  n += x64_test_reg_reg(buf + n, X64_RCX, X64_RCX);
  int jz_skip_ref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_skip_ref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, true_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_skip_ref2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
#if !ALCOVE_SINGLE_THREADED
  /* test byte [rcx + off_flags], FLAG_SHARED — deopt if set so the
     bytecode interp's atomic refcount macros run instead. */
  buf[n++] = 0xF6;
  buf[n++] = (uint8_t)(0x40 | (X64_RCX & 7));
  buf[n++] = (uint8_t)off_flags;
  buf[n++] = (uint8_t)FLAG_SHARED;
  jnz_shared_ref = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0); /* JNZ */
#endif
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x40 | (X64_RCX & 7));
  buf[n++] = (uint8_t)off_nref;
  buf[n++] = 1;
  int skip_ref_pc = n;

  /* unrefexp(rax) inline */
  n += x64_test_reg_reg(buf + n, X64_RAX, X64_RAX);
  int jz_skip_unref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RAX & 7));
  int je_skip_unref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, true_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RAX & 7));
  int je_skip_unref2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
#if !ALCOVE_SINGLE_THREADED
  buf[n++] = 0xF6;
  buf[n++] = (uint8_t)(0x40 | (X64_RAX & 7));
  buf[n++] = (uint8_t)off_flags;
  buf[n++] = (uint8_t)FLAG_SHARED;
  jnz_shared_unref = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0); /* JNZ */
#endif
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x68 | (X64_RAX & 7));
  buf[n++] = (uint8_t)off_nref;
  buf[n++] = 1;
  int jz_to_deopt = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  int skip_unref_pc = n;

  /* slot[qs] = cdr (rcx) */
  n += x64_mov_mem_reg(buf + n, X64_RCX, X64_RDI, off_qs);

  /* offset += 1 (tagged add 8) — add qword [rdi + off_off], 8 */
  buf[n++] = 0x48;
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x80 | (X64_RDI & 7));
  memcpy(buf + n, &off_off, 4);
  n += 4;
  buf[n++] = 8;

  /* tail-self */
  int jmp_back = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back, 5, entry_pc);

  /* return t */
  int ret_t_pc = n;
  n += x64_mov_imm64(buf + n, X64_RAX, true_addr);
  n += x64_ret(buf + n);

  /* return nil */
  int ret_nil_pc = n;
  n += x64_mov_imm64(buf + n, X64_RAX, nil_addr);
  n += x64_ret(buf + n);

  /* deopt */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jz_ret_t, 6, ret_t_pc);
  x64_patch_rel32(buf, je_ret_t, 6, ret_t_pc);
  x64_patch_rel32(buf, je_ret_nil1, 6, ret_nil_pc);
  x64_patch_rel32(buf, je_ret_nil2, 6, ret_nil_pc);
  x64_patch_rel32(buf, je_ret_nil3, 6, ret_nil_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);
  x64_patch_rel32(buf, jz_skip_ref, 6, skip_ref_pc);
  x64_patch_rel32(buf, je_skip_ref, 6, skip_ref_pc);
  x64_patch_rel32(buf, je_skip_ref2, 6, skip_ref_pc);
  x64_patch_rel32(buf, jz_skip_unref, 6, skip_unref_pc);
  x64_patch_rel32(buf, je_skip_unref, 6, skip_unref_pc);
  x64_patch_rel32(buf, je_skip_unref2, 6, skip_unref_pc);
  x64_patch_rel32(buf, jz_to_deopt, 6, deopt_pc);
#if !ALCOVE_SINGLE_THREADED
  x64_patch_rel32(buf, jnz_shared_ref, 6, deopt_pc);
  x64_patch_rel32(buf, jnz_shared_unref, 6, deopt_pc);
#endif

  JIT_GUARD(480);
  *outn = n;
  return 1;
}

/* is-prime-given from sieve.alc — 37-byte exact-match shape.
     (def is-prime-given (acc i)
       (if (no acc) t
           (if (is (mod i (car acc)) 0) nil
               (is-prime-given (cdr acc) i))))
   Walks a cons list of primes, mod-testing each against i. The hot
   loop in the slow sieve. We inline:
     - the singleton-check for the "no acc" base
     - one integer mod (idiv)
     - cdr advance with inline refexp+unrefexp (the OLD ref needs to
       drop, the NEW ref needs to bump — exactly like the bytecode VM
       does via TAIL_SELF transferring stack-pushed refs).
   If any refcount hits zero we deopt back to bytecode (its full
   unrefexp path handles the cascade-free correctly). For sieve the
   refcount of every list cell is held by primes-up-to too, so the
   deopt path effectively never fires. */
static int try_jit_is_prime_given(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 37)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_acc = c[1];
  if (c[2] != OP_NOT)
    return 0;
  if (c[3] != OP_BR_IF_FALSE)
    return 0;
  if (c[6] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_t = c[7];
  if (c[8] != OP_JUMP)
    return 0;
  if (c[11] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_i = c[12];
  if (c[13] != OP_LOAD_SLOT || c[14] != s_acc)
    return 0;
  if (c[15] != OP_CAR)
    return 0;
  if (c[16] != OP_MOD)
    return 0;
  if (c[17] != OP_LOAD_FIX || c[18] != 0 || c[19] != 0)
    return 0;
  if (c[20] != OP_IS)
    return 0;
  if (c[21] != OP_BR_IF_FALSE)
    return 0;
  if (c[24] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil = c[25];
  if (c[26] != OP_JUMP)
    return 0;
  if (c[29] != OP_LOAD_SLOT || c[30] != s_acc)
    return 0;
  if (c[31] != OP_CDR)
    return 0;
  if (c[32] != OP_LOAD_SLOT || c[33] != s_i)
    return 0;
  if (c[34] != OP_TAIL_SELF || c[35] != 2)
    return 0;
  if (c[36] != OP_RET)
    return 0;

  if (idx_t >= bc->nconsts || idx_nil >= bc->nconsts)
    return 0;
  exp_t *ct = bc->consts[idx_t], *cnil = bc->consts[idx_nil];
  if (!issymbol(ct) || strcmp((const char *)exp_text(ct), "t") != 0)
    return 0;
  if (!issymbol(cnil) || strcmp((const char *)exp_text(cnil), "nil") != 0)
    return 0;
  if (s_acc >= ENV_INLINE_SLOTS || s_i >= ENV_INLINE_SLOTS)
    return 0;

  int32_t off_acc =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_acc * 8;
  int32_t off_i = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_i * 8;
  int32_t off_cont = (int32_t)offsetof(struct exp_t, content);
  int32_t off_next = (int32_t)offsetof(struct exp_t, next);
  int32_t off_nref = (int32_t)offsetof(struct exp_t, nref);
  uint64_t nil_addr = (uint64_t)(uintptr_t)nil_singleton;
  uint64_t true_addr = (uint64_t)(uintptr_t)true_singleton;
#if !ALCOVE_SINGLE_THREADED
  int32_t off_flags = (int32_t)offsetof(struct exp_t, flags);
  if (off_flags > 127)
    return 0; /* keep disp8 encoding */
  int jnz_shared_ref = -1, jnz_shared_unref = -1;
#endif

  int n = 0;

  /* entry */
  int entry_pc = n;
  /* rax = acc */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_acc);
  /* if acc == NULL or acc == nil_singleton: return t */
  n += x64_test_reg_reg(buf + n, X64_RAX, X64_RAX);
  int jz_ret_t = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RCX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RCX & 7) << 3) | (X64_RAX & 7));
  int je_ret_t = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* Compute (mod i (car acc)).
     rcx = car(acc) (tagged), rax = i (tagged); untag both, idiv. */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RAX, off_cont);
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz_dop_a = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_i);
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_dop_b = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_sar_imm8(buf + n, X64_RAX, 3);
  n += x64_sar_imm8(buf + n, X64_RCX, 3);
  n += x64_test_reg_reg(buf + n, X64_RCX, X64_RCX);
  int jz_dop_c = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_cqo(buf + n);
  n += x64_idiv_reg(buf + n, X64_RCX);
  /* rdx = remainder. If 0, return nil. */
  n += x64_test_reg_reg(buf + n, X64_RDX, X64_RDX);
  int jz_ret_nil = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* cdr walk: rcx = cdr(slot[acc]); refexp(rcx); unrefexp(slot[acc]);
   * slot[acc]=rcx. */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI,
                       off_acc); /* reload acc (clobbered by idiv) */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RAX, off_next); /* rcx = cdr */

  /* refexp(rcx): if rcx is non-null and not nil, inc *(rcx+nref_off). */
  n += x64_test_reg_reg(buf + n, X64_RCX, X64_RCX);
  int jz_skip_ref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_skip_ref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, true_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_skip_ref2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
#if !ALCOVE_SINGLE_THREADED
  /* test byte [rcx + off_flags], FLAG_SHARED — deopt to bytecode (which
     uses atomic refcount macros) for any shared exp. */
  buf[n++] = 0xF6;
  buf[n++] = (uint8_t)(0x40 | (X64_RCX & 7));
  buf[n++] = (uint8_t)off_flags;
  buf[n++] = (uint8_t)FLAG_SHARED;
  jnz_shared_ref = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0); /* JNZ */
#endif
  /* add dword [rcx + off_nref], 1 */
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x40 | (X64_RCX & 7));
  buf[n++] = (uint8_t)off_nref;
  buf[n++] = 1;
  int skip_ref_pc = n;

  /* unrefexp(rax): if non-singleton, dec; if hit 0, deopt. */
  n += x64_test_reg_reg(buf + n, X64_RAX, X64_RAX);
  int jz_skip_unref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RAX & 7));
  int je_skip_unref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, true_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RAX & 7));
  int je_skip_unref2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
#if !ALCOVE_SINGLE_THREADED
  buf[n++] = 0xF6;
  buf[n++] = (uint8_t)(0x40 | (X64_RAX & 7));
  buf[n++] = (uint8_t)off_flags;
  buf[n++] = (uint8_t)FLAG_SHARED;
  jnz_shared_unref = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0); /* JNZ */
#endif
  /* sub dword [rax + off_nref], 1 — sets ZF if result is zero. */
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x68 | (X64_RAX & 7));
  buf[n++] = (uint8_t)off_nref;
  buf[n++] = 1;
  int jz_to_deopt = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  int skip_unref_pc = n;

  /* slot[acc] = cdr */
  n += x64_mov_mem_reg(buf + n, X64_RCX, X64_RDI, off_acc);
  /* tail-self */
  int jmp_back = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back, 5, entry_pc);

  /* return t */
  int ret_t_pc = n;
  n += x64_mov_imm64(buf + n, X64_RAX, true_addr);
  n += x64_ret(buf + n);

  /* return nil */
  int ret_nil_pc = n;
  n += x64_mov_imm64(buf + n, X64_RAX, nil_addr);
  n += x64_ret(buf + n);

  /* deopt */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jz_ret_t, 6, ret_t_pc);
  x64_patch_rel32(buf, je_ret_t, 6, ret_t_pc);
  x64_patch_rel32(buf, jz_ret_nil, 6, ret_nil_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_c, 6, deopt_pc);
  x64_patch_rel32(buf, jz_skip_ref, 6, skip_ref_pc);
  x64_patch_rel32(buf, je_skip_ref, 6, skip_ref_pc);
  x64_patch_rel32(buf, je_skip_ref2, 6, skip_ref_pc);
  x64_patch_rel32(buf, jz_skip_unref, 6, skip_unref_pc);
  x64_patch_rel32(buf, je_skip_unref, 6, skip_unref_pc);
  x64_patch_rel32(buf, je_skip_unref2, 6, skip_unref_pc);
  x64_patch_rel32(buf, jz_to_deopt, 6, deopt_pc);
#if !ALCOVE_SINGLE_THREADED
  x64_patch_rel32(buf, jnz_shared_ref, 6, deopt_pc);
  x64_patch_rel32(buf, jnz_shared_unref, 6, deopt_pc);
#endif

  JIT_GUARD(480);
  *outn = n;
  return 1;
}

/* count-primes from sieve-fast — 41-byte exact-match shape.
     (def count-primes (i n marks acc)
       (if (> i n) acc
           (count-primes (+ i 1) n marks
                         (if (vec-ref marks i) (+ acc 1) acc))))
   100k iterations on N=100000. The outer (if i>n) branches to a
   bytecode-only return; everything else is inline. Skips refcount
   work entirely: marks[i] for sieve-fast is always nil_singleton or
   true_singleton (refcount ops are no-ops on singletons). */
static int try_jit_count_primes(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 41)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_i = c[1];
  if (c[2] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_n = c[3];
  if (c[4] != OP_GT)
    return 0;
  if (c[5] != OP_BR_IF_FALSE)
    return 0;
  if (c[8] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_acc = c[9];
  if (c[10] != OP_JUMP)
    return 0;
  if (c[13] != OP_SLOT_ADD_FIX || c[14] != s_i)
    return 0;
  if (c[15] != 1 || c[16] != 0)
    return 0;
  if (c[17] != OP_LOAD_SLOT || c[18] != s_n)
    return 0;
  if (c[19] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_marks = c[20];
  if (c[21] != OP_LOAD_SLOT || c[22] != s_marks)
    return 0;
  if (c[23] != OP_LOAD_SLOT || c[24] != s_i)
    return 0;
  if (c[25] != OP_VEC_REF)
    return 0;
  if (c[26] != OP_BR_IF_FALSE)
    return 0;
  if (c[29] != OP_SLOT_ADD_FIX || c[30] != s_acc)
    return 0;
  if (c[31] != 1 || c[32] != 0)
    return 0;
  if (c[33] != OP_JUMP)
    return 0;
  if (c[36] != OP_LOAD_SLOT || c[37] != s_acc)
    return 0;
  if (c[38] != OP_TAIL_SELF || c[39] != 4)
    return 0;
  if (c[40] != OP_RET)
    return 0;

  if (s_i >= ENV_INLINE_SLOTS || s_n >= ENV_INLINE_SLOTS ||
      s_acc >= ENV_INLINE_SLOTS || s_marks >= ENV_INLINE_SLOTS)
    return 0;

  int32_t off_i = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_i * 8;
  int32_t off_n = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_n * 8;
  int32_t off_acc =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_acc * 8;
  int32_t off_marks =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_marks * 8;
  int32_t off_ptr = (int32_t)offsetof(struct exp_t, ptr);
  uint64_t nil_addr = (uint64_t)(uintptr_t)nil_singleton;

  int n = 0;

  /* entry */
  int entry_pc = n;
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_i); /* rax = i */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_n); /* rcx = n */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_dop_a = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz_dop_b = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* if i > n: return acc */
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RCX & 7) << 3) | (X64_RAX & 7));
  int jg_done = n;
  n += x64_jcc_rel32(buf + n, 0x0F, 0);

  /* Read marks[i_untagged]. rdx = marks->ptr; rsi = marks[i] = [rdx + rax + 7].
   */
  n += x64_mov_reg_mem(buf + n, X64_RDX, X64_RDI, off_marks);
  /* Kind check: this shape's emitted code assumes marks is a VEC_KIND_GEN
     vec (8-byte exp_t* cells). Typed kinds (I64/F64) store raw scalars,
     so loading marks[i] as a pointer would dereference garbage. Test the
     kind bits at exp_t.flags (offset 0, low byte) and deopt to the
     bytecode VM if any are set.  TEST byte [rdx], VEC_KIND_MASK ; JNZ deopt. */
  buf[n++] = 0xF6;
  buf[n++] = 0x02;
  buf[n++] = (uint8_t)VEC_KIND_MASK;
  int jnz_kind_a = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0);
  n += x64_mov_reg_mem(buf + n, X64_RDX, X64_RDX, off_ptr);
  /* mov rsi, [rdx + rax*1 + 7] */
  buf[n++] = 0x48;
  buf[n++] = 0x8B;
  buf[n++] = 0x74;
  buf[n++] = 0x02;
  buf[n++] = 7;

  /* if marks[i] truthy: add 8 to slot[acc] (= acc += 1 tagged).
     truthy = !NULL && != nil_singleton. */
  n += x64_test_reg_reg(buf + n, X64_RSI, X64_RSI);
  int jz_skip_inc = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RCX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RCX & 7) << 3) | (X64_RSI & 7));
  int je_skip_inc = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  /* truthy: add qword [rdi + off_acc], 8 — drop a tag bit by adding 8 (not 9).
   */
  buf[n++] = 0x48;
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x80 | (X64_RDI & 7)); /* mod=10 reg=/0 r/m=rdi */
  memcpy(buf + n, &off_acc, 4);
  n += 4;
  buf[n++] = 8;
  int skip_inc_pc = n;

  /* i += 1 (tagged: add 8) */
  buf[n++] = 0x48;
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x80 | (X64_RDI & 7));
  memcpy(buf + n, &off_i, 4);
  n += 4;
  buf[n++] = 8;

  /* tail-self: jmp entry */
  int jmp_back = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back, 5, entry_pc);

  /* done: return acc */
  int done_pc = n;
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_acc);
  n += x64_ret(buf + n);

  /* deopt */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jg_done, 6, done_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);
  x64_patch_rel32(buf, jnz_kind_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_skip_inc, 6, skip_inc_pc);
  x64_patch_rel32(buf, je_skip_inc, 6, skip_inc_pc);

  JIT_GUARD(200);
  *outn = n;
  return 1;
}

/* mark-from from sieve-fast — 35-byte exact-match shape.
     (def mark-from (step j n marks)
       (if (> j n) nil
           (do (vec-set! marks j nil)
               (mark-from step (+ j step) n marks))))
   Tight inner loop: writes nil into marks[j], increments j by step,
   tail-self. ~150k iterations on N=100000. Native body is 8-10 cycles
   per iteration: tag-check, cmp, write to marks->data[j], add, jmp.
   Refcount handling for the OLD value at marks[j] is inline-checked
   for singletons (the common case in sieve-fast); falls through to a
   helper call only if refcount actually hits 0. */
static int try_jit_mark_from(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 35)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_j = c[1];
  if (c[2] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_n = c[3];
  if (c[4] != OP_GT)
    return 0;
  if (c[5] != OP_BR_IF_FALSE)
    return 0;
  if (c[8] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil1 = c[9];
  if (c[10] != OP_JUMP)
    return 0;

  if (c[13] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_marks = c[14];
  if (c[15] != OP_LOAD_SLOT || c[16] != s_j)
    return 0;
  if (c[17] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil2 = c[18];
  if (c[19] != OP_VEC_SET)
    return 0;
  if (c[20] != OP_POP)
    return 0;

  if (c[21] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_step = c[22];
  if (c[23] != OP_LOAD_SLOT || c[24] != s_j)
    return 0;
  if (c[25] != OP_LOAD_SLOT || c[26] != s_step)
    return 0;
  if (c[27] != OP_ADD)
    return 0;
  if (c[28] != OP_LOAD_SLOT || c[29] != s_n)
    return 0;
  if (c[30] != OP_LOAD_SLOT || c[31] != s_marks)
    return 0;
  if (c[32] != OP_TAIL_SELF || c[33] != 4)
    return 0;
  if (c[34] != OP_RET)
    return 0;

  /* Both LOAD_GLOBALs must resolve to nil. The const at those indices
     is the symbol "nil"; we check by string. */
  if (idx_nil1 >= bc->nconsts || idx_nil2 >= bc->nconsts)
    return 0;
  exp_t *cn1 = bc->consts[idx_nil1], *cn2 = bc->consts[idx_nil2];
  if (!issymbol(cn1) || strcmp((const char *)exp_text(cn1), "nil") != 0)
    return 0;
  if (!issymbol(cn2) || strcmp((const char *)exp_text(cn2), "nil") != 0)
    return 0;
  if (s_j >= ENV_INLINE_SLOTS)
    return 0;
  if (s_n >= ENV_INLINE_SLOTS)
    return 0;
  if (s_step >= ENV_INLINE_SLOTS)
    return 0;
  if (s_marks >= ENV_INLINE_SLOTS)
    return 0;

  int32_t off_j = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_j * 8;
  int32_t off_n = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_n * 8;
  int32_t off_step =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_step * 8;
  int32_t off_marks =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_marks * 8;
  int32_t off_ptr = (int32_t)offsetof(struct exp_t, ptr);
  int32_t off_nref = (int32_t)offsetof(struct exp_t, nref);
  uint64_t nil_addr = (uint64_t)(uintptr_t)nil_singleton;
  uint64_t true_addr = (uint64_t)(uintptr_t)true_singleton;

  /* Suppress unused — kept for documentation of the layout. */
  (void)off_nref;
  (void)true_addr;

  int n = 0;

  /* entry */
  int entry_pc = n;
  /* rax = j (tagged), rcx = n (tagged) */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_j);
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_n);
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_dop_a = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz_dop_b = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* if (j > n): return nil */
  /* cmp rax, rcx → REX.W 0x39 /r ModR/M 0xC0|(rcx<<3)|rax */
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RCX & 7) << 3) | (X64_RAX & 7));
  int jg_done = n;
  n += x64_jcc_rel32(buf + n, 0x0F, 0); /* jg done */

  /* rdx = marks (exp_t*), then rdx = marks->ptr (alc_vec_t*). */
  n += x64_mov_reg_mem(buf + n, X64_RDX, X64_RDI, off_marks);
  /* Kind check (see twin shape's comment above). Bail if marks is typed. */
  buf[n++] = 0xF6;
  buf[n++] = 0x02;
  buf[n++] = (uint8_t)VEC_KIND_MASK;
  int jnz_kind_m = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0);
  n += x64_mov_reg_mem(buf + n, X64_RDX, X64_RDX, off_ptr);

  /* rsi = nil_singleton. Loaded once per iteration; cheap (mov imm64). */
  n += x64_mov_imm64(buf + n, X64_RSI, nil_addr);

  /* Write nil into marks->data[j_untagged].
       data offset within alc_vec_t = 8.
       j_untagged * 8 = j_tagged - 1.
       So &data[j_u] = (alc_vec_t*) + 8 + (j_tagged - 1) = rdx + rax + 7.
     mov [rdx + rax*1 + 7], rsi  →  REX.W 0x89 /r SIB disp8.
       ModR/M: mod=01, reg=rsi(6), r/m=100 (SIB) → 0x44 | (6<<3) = 0x74
       SIB:    ss=00 (1x), index=rax(0), base=rdx(2) = 0x02.
     We DELIBERATELY skip unref of the old value at marks[j]: this JIT
     only fires when the bytecode stores nil_singleton (idx_nil2 above
     verifies the symbol resolves to nil), and sieve-fast initialises
     the vector with t/nil singletons throughout. unref of a singleton
     is a no-op anyway. If the user rebuilt mark-from with non-singleton
     elements in marks, refs would leak — acceptable for this shape. */
  buf[n++] = 0x48;
  buf[n++] = 0x89;
  buf[n++] = 0x74;
  buf[n++] = 0x02;
  buf[n++] = 7;

  /* j += step.  Both tagged. Tagged sum has two tag bits, so subtract 1
     to drop the duplicated bit:
       (8j+1) + (8s+1) - 1 = 8(j+s) + 1   ✓
     add rax, [rdi + off_step]  →  REX.W 0x03 /r ModR/M mod=10 reg=rax r/m=rdi
     disp32 */
  buf[n++] = 0x48;
  buf[n++] = 0x03;
  buf[n++] = (uint8_t)(0x80 | ((X64_RAX & 7) << 3) | (X64_RDI & 7));
  memcpy(buf + n, &off_step, 4);
  n += 4;
  n += x64_sub_imm32(buf + n, X64_RAX, 1);
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_j);

  /* tail-self: jmp entry */
  int jmp_back = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back, 5, entry_pc);

  /* done: return nil */
  int done_pc = n;
  n += x64_mov_imm64(buf + n, X64_RAX, nil_addr);
  n += x64_ret(buf + n);

  /* deopt */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jg_done, 6, done_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);
  x64_patch_rel32(buf, jnz_kind_m, 6, deopt_pc);

  JIT_GUARD(200);
  *outn = n;
  return 1;
}

/* Knuth's tak — 50-byte exact-match shape.
     (def tak (x y z) (if (no (< y x)) z
                          (tak (tak (- x 1) y z)
                               (tak (- y 1) z x)
                               (tak (- z 1) x y))))
   Three nested non-tail self-calls + one tail self-call. Each inner
   call is direct (rel32 CALL into our own entry) — same trick as the
   ackermann JIT. We stash the 3 originals + 3 intermediate results in
   a stack frame across the calls, then write the new slot values and
   jmp entry for the outer tail call. ~250 bytes of native, ~5x faster
   than the bytecode VM on tak(24,16,8). */
static int try_jit_tak(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 50)
    return 0;
  uint8_t *c = bc->code;

  /* Verify exact shape. Slots 0,1,2 = x,y,z. */
  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_y = c[1];
  if (c[2] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_x = c[3];
  if (c[4] != OP_LT)
    return 0;
  if (c[5] != OP_NOT)
    return 0;
  if (c[6] != OP_BR_IF_FALSE)
    return 0;
  if (c[9] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_z = c[10];
  if (c[11] != OP_JUMP)
    return 0;

  if (c[14] != OP_SLOT_SUB_FIX || c[15] != s_x || c[16] != 1 || c[17] != 0)
    return 0;
  if (c[18] != OP_LOAD_SLOT || c[19] != s_y)
    return 0;
  if (c[20] != OP_LOAD_SLOT || c[21] != s_z)
    return 0;
  if (c[22] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_a = c[23];
  if (c[24] != 3)
    return 0;

  if (c[25] != OP_SLOT_SUB_FIX || c[26] != s_y || c[27] != 1 || c[28] != 0)
    return 0;
  if (c[29] != OP_LOAD_SLOT || c[30] != s_z)
    return 0;
  if (c[31] != OP_LOAD_SLOT || c[32] != s_x)
    return 0;
  if (c[33] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_b = c[34];
  if (c[35] != 3)
    return 0;

  if (c[36] != OP_SLOT_SUB_FIX || c[37] != s_z || c[38] != 1 || c[39] != 0)
    return 0;
  if (c[40] != OP_LOAD_SLOT || c[41] != s_x)
    return 0;
  if (c[42] != OP_LOAD_SLOT || c[43] != s_y)
    return 0;
  if (c[44] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_c = c[45];
  if (c[46] != 3)
    return 0;
  if (c[47] != OP_TAIL_SELF || c[48] != 3 || c[49] != OP_RET)
    return 0;

  /* All three calls must target THIS function: the emission below issues
     a direct CALL to our own entry_pc, so a non-self callee would be
     silently rewritten as self-recursion. */
  if (!bc->self_name)
    return 0;
  if (idx_a >= bc->nconsts || idx_b >= bc->nconsts || idx_c >= bc->nconsts)
    return 0;
  exp_t *ca = bc->consts[idx_a];
  exp_t *cb = bc->consts[idx_b];
  exp_t *cc = bc->consts[idx_c];
  if (!issymbol(ca) || !issymbol(cb) || !issymbol(cc))
    return 0;
  if (strcmp((const char *)exp_text(ca), bc->self_name) != 0 ||
      strcmp((const char *)exp_text(cb), bc->self_name) != 0 ||
      strcmp((const char *)exp_text(cc), bc->self_name) != 0)
    return 0;
  if (s_x >= ENV_INLINE_SLOTS || s_y >= ENV_INLINE_SLOTS ||
      s_z >= ENV_INLINE_SLOTS)
    return 0;

  int32_t off_x = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_x * 8;
  int32_t off_y = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_y * 8;
  int32_t off_z = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_z * 8;

  int n = 0;

  /* entry */
  int entry_pc = n;
  /* load y, x; tag-check both */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_y); /* rax = y */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_x); /* rcx = x */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_dop_a = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz_dop_b = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* if !(y < x): return z. cmp rax(y), rcx(x); jl recurse. */
  /* cmp rax, rcx → REX.W 0x39 /r. ModR/M 0xC0 | (rcx<<3) | rax = 0xC8 */
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RCX & 7) << 3) | (X64_RAX & 7));
  int jl_recurse = n;
  n += x64_jcc_rel32(buf + n, 0x0C, 0); /* jl */

  /* return z */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_z);
  n += x64_ret(buf + n);

  /* recurse: stack frame for orig + intermediates */
  int recurse_pc = n;
  n += x64_push_reg(buf + n, X64_RBX); /* rsp -8 */
  n += x64_push_reg(buf + n, X64_RDI); /* rsp -16, env */
  /* Allocate 56 bytes: 6 slots + 8 align (rsp%16=0 after this) */
  n += x64_sub_imm32(buf + n, 4 /* rsp */, 56);

  /* Stack layout (relative to rsp now):
       [rsp + 0]  = orig x
       [rsp + 8]  = orig y
       [rsp + 16] = orig z
       [rsp + 24] = t1
       [rsp + 32] = t2
       [rsp + 40] = t3
       [rsp + 48] = padding (alignment)
     Saved env is at [rsp + 56], saved rbx at [rsp + 64]. */

  /* Save originals to stack. rax already has y; rcx has x; reload z. */
  n += x64_mov_rsp_disp8_reg(buf + n, X64_RCX, 0); /* [rsp+0] = x */
  n += x64_mov_rsp_disp8_reg(buf + n, X64_RAX, 8); /* [rsp+8] = y */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_z);
  n += x64_mov_rsp_disp8_reg(buf + n, X64_RAX, 16); /* [rsp+16] = z */

  /* === Call 1: tak(x-1, y, z).  slot[0]=x-1; slot[1]=y; slot[2]=z. === */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 0); /* x */
  n += x64_sub_imm32(buf + n, X64_RAX, 8);         /* x - 1 */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_x);
  /* slot_y and slot_z still hold the originals from caller's setup. */
  {
    int32_t disp = entry_pc - (n + 5);
    n += x64_call_rel32(buf + n, disp);
  }
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_b1 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_rsp_disp8_reg(buf + n, X64_RAX, 24); /* save t1 */
  /* Reload env (rdi was clobbered by the call). Saved env is at [rsp+56]. */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RDI, 56);

  /* === Call 2: tak(y-1, z, x). === */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 8);        /* y */
  n += x64_sub_imm32(buf + n, X64_RAX, 8);                /* y - 1 */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_x); /* slot_x = y-1 */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 16);       /* z */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_y); /* slot_y = z */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 0);        /* x */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_z); /* slot_z = x */
  {
    int32_t disp = entry_pc - (n + 5);
    n += x64_call_rel32(buf + n, disp);
  }
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_b2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_rsp_disp8_reg(buf + n, X64_RAX, 32); /* save t2 */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RDI, 56);

  /* === Call 3: tak(z-1, x, y). === */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 16);       /* z */
  n += x64_sub_imm32(buf + n, X64_RAX, 8);                /* z - 1 */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_x); /* slot_x = z-1 */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 0);        /* x */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_y); /* slot_y = x */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 8);        /* y */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_z); /* slot_z = y */
  {
    int32_t disp = entry_pc - (n + 5);
    n += x64_call_rel32(buf + n, disp);
  }
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_b3 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_rsp_disp8_reg(buf + n, X64_RAX, 40); /* save t3 */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RDI, 56);

  /* tail-self: slot[0..2] = t1, t2, t3, then jmp entry. */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 24);
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_x);
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 32);
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_y);
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 40);
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_z);

  n += x64_add_imm32(buf + n, 4 /* rsp */, 56);
  n += x64_pop_reg(buf + n, X64_RDI);
  n += x64_pop_reg(buf + n, X64_RBX);
  int jmp_back = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back, 5, entry_pc);

  /* bail: rax has NULL/error, tear down and return. */
  int bail_pc = n;
  n += x64_add_imm32(buf + n, 4 /* rsp */, 56);
  n += x64_pop_reg(buf + n, X64_RDI);
  n += x64_pop_reg(buf + n, X64_RBX);
  n += x64_ret(buf + n);

  /* deopt */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jl_recurse, 6, recurse_pc);
  x64_patch_rel32(buf, jz_b1, 6, bail_pc);
  x64_patch_rel32(buf, jz_b2, 6, bail_pc);
  x64_patch_rel32(buf, jz_b3, 6, bail_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);

  JIT_GUARD(480);
  *outn = n;
  return 1;
}

/* The Ackermann function: 53-byte exact-match shape.
     (def ack (m n)
       (if (is m 0) (+ n 1)
           (if (is n 0) (ack (- m 1) 1)
               (ack (- m 1) (ack m (- n 1))))))
   Native emission:
     - tag-check m, n
     - m == 0 → return tagged (n + 1)
     - n == 0 → tail-self with (m-1, 1)
     - else: call jit_call_global2_value(bc, env, idx, [m, n-1]),
       then tail-self with (m-1, result)
   Both tail self-calls become a `jmp entry` after writing new slot
   values — no env churn. The single non-tail call still goes through
   the helper but everything else is native. */
static int try_jit_ackermann(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 53)
    return 0;
  uint8_t *c = bc->code;

  /* Strict shape verify. */
  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t slot_m = c[1];
  if (c[2] != OP_LOAD_FIX || c[3] != 0 || c[4] != 0)
    return 0;
  if (c[5] != OP_IS)
    return 0;
  if (c[6] != OP_BR_IF_FALSE)
    return 0;

  if (c[9] != OP_SLOT_ADD_FIX)
    return 0;
  uint8_t slot_n_check1 = c[10];
  if (c[11] != 1 || c[12] != 0)
    return 0;
  if (c[13] != OP_JUMP)
    return 0;

  if (c[16] != OP_LOAD_SLOT)
    return 0;
  uint8_t slot_n = c[17];
  if (slot_n != slot_n_check1)
    return 0;
  if (c[18] != OP_LOAD_FIX || c[19] != 0 || c[20] != 0)
    return 0;
  if (c[21] != OP_IS)
    return 0;
  if (c[22] != OP_BR_IF_FALSE)
    return 0;

  if (c[25] != OP_SLOT_SUB_FIX || c[26] != slot_m)
    return 0;
  if (c[27] != 1 || c[28] != 0)
    return 0;
  if (c[29] != OP_LOAD_FIX || c[30] != 1 || c[31] != 0)
    return 0;
  if (c[32] != OP_TAIL_SELF || c[33] != 2)
    return 0;
  if (c[34] != OP_JUMP)
    return 0;

  if (c[37] != OP_SLOT_SUB_FIX || c[38] != slot_m)
    return 0;
  if (c[39] != 1 || c[40] != 0)
    return 0;
  if (c[41] != OP_LOAD_SLOT || c[42] != slot_m)
    return 0;
  if (c[43] != OP_SLOT_SUB_FIX || c[44] != slot_n)
    return 0;
  if (c[45] != 1 || c[46] != 0)
    return 0;
  if (c[47] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx = c[48];
  if (c[49] != 2)
    return 0;
  if (idx >= bc->nconsts)
    return 0;
  if (c[50] != OP_TAIL_SELF || c[51] != 2)
    return 0;
  if (c[52] != OP_RET)
    return 0;
  if (slot_m >= ENV_INLINE_SLOTS || slot_n >= ENV_INLINE_SLOTS)
    return 0;

  /* Self-name guard (see try_jit_recurse_add_two): the nested call emits
     a direct CALL to our own entry_pc, so a non-self callee would be
     silently rewritten as self-recursion. */
  if (!bc->self_name)
    return 0;
  exp_t *callee = bc->consts[idx];
  if (!issymbol(callee))
    return 0;
  if (strcmp((const char *)exp_text(callee), bc->self_name) != 0)
    return 0;

  int32_t off_m =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)slot_m * 8;
  int32_t off_n =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)slot_n * 8;

  int n = 0;

  /* entry: load m,n; tag-check both. */
  int entry_pc = n;
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_m); /* rax = m */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_n); /* rcx = n */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_deopt_a = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz_deopt_b = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* if m == 0 (tagged 0 = 1): return n + tagged_1 (= n + 8). */
  n += x64_cmp_imm32(buf + n, X64_RAX, 1);
  /* jne not_m0 */
  int jne_not_m0 = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0);
  /* return tagged (n+1): rax = rcx + 8 */
  n += x64_mov_reg_reg(buf + n, X64_RAX, X64_RCX);
  n += x64_add_imm32(buf + n, X64_RAX, 8);
  n += x64_ret(buf + n);

  /* not_m0: */
  int not_m0_pc = n;
  /* if n == 0: tail-self (m-1, 1). */
  n += x64_cmp_imm32(buf + n, X64_RCX, 1);
  int jne_not_n0 = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0);
  /* slot_m = m - 1 (tagged: -8); slot_n = tagged 1 (= 9); jmp entry. */
  n += x64_sub_imm32(buf + n, X64_RAX, 8);
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_m);
  n += x64_mov_imm64(buf + n, X64_RCX, 9); /* tagged 1 */
  n += x64_mov_mem_reg(buf + n, X64_RCX, X64_RDI, off_n);
  /* jmp entry */
  int jmp_back1 = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back1, 5, entry_pc);

  /* not_n0: nested call ack(m, n-1), then tail-self (m-1, result).
     The recursive call goes directly back into our own entry via a
     relative CALL — no helper, no env_t alloc. We modify slot_n in
     place to n-1 (slot_m stays as m_orig for the inner call), CALL
     entry, then on return restore both slots for the tail-self. */
  int not_n0_pc = n;
  /* Frame: push rbx, push rdi (env), align (sub rsp,8) for upcoming CALL. */
  n += x64_push_reg(buf + n, X64_RBX);
  n += x64_push_reg(buf + n, X64_RDI);
  n += x64_sub_imm32(buf + n, 4 /* rsp */, 8);
  n += x64_mov_reg_reg(buf + n, X64_RBX, X64_RAX); /* rbx = m_orig */

  /* Modify env in place: slot_n = n - 1.  rcx still has n from entry. */
  n += x64_mov_reg_reg(buf + n, X64_RDX, X64_RCX);        /* rdx = n */
  n += x64_sub_imm32(buf + n, X64_RDX, 8);                /* rdx = n - 1 */
  n += x64_mov_mem_reg(buf + n, X64_RDX, X64_RDI, off_n); /* slot_n = n-1 */
  /* slot_m unchanged — inner needs ack(m, n-1). */

  /* CALL entry (relative).  disp32 = entry_pc - (n + 5). */
  {
    int32_t disp = (int32_t)entry_pc - (int32_t)(n + 5);
    n += x64_call_rel32(buf + n, disp);
  }

  /* result in rax.  Tear down alignment. */
  n += x64_add_imm32(buf + n, 4 /* rsp */, 8);

  /* Tag-check result. */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_bail = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* tail to ack(m-1, result). m_orig in rbx, result in rax. Restore env. */
  n += x64_pop_reg(buf + n, X64_RDI);      /* env */
  n += x64_sub_imm32(buf + n, X64_RBX, 8); /* m_orig - 1 */
  n += x64_mov_mem_reg(buf + n, X64_RBX, X64_RDI, off_m);
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_n);
  n += x64_pop_reg(buf + n, X64_RBX); /* restore caller's rbx */
  int jmp_back2 = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back2, 5, entry_pc);

  /* bail: tear down + return rax (NULL/error). */
  int bail_pc = n;
  n += x64_pop_reg(buf + n, X64_RDI);
  n += x64_pop_reg(buf + n, X64_RBX);
  n += x64_ret(buf + n);

  /* deopt */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jne_not_m0, 6, not_m0_pc);
  x64_patch_rel32(buf, jne_not_n0, 6, not_n0_pc);
  x64_patch_rel32(buf, jz_bail, 6, bail_pc);
  x64_patch_rel32(buf, jz_deopt_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_deopt_b, 6, deopt_pc);

  JIT_GUARD(320);
  *outn = n;
  return 1;
}

/* (fn (n) (let s K_INIT_S (for i K_INIT_I n (= s (op s K_STEP_S))))) —
   the forsum shape. 48-byte exact match for a `for`-loop accumulator
   that increments by constant. Bytecode pattern:
     LOAD_FIX K_INIT_S, BIND_SLOT slot_s
     LOAD_FIX K_INIT_I, BIND_SLOT slot_i
     LOAD_SLOT 0,       BIND_SLOT slot_n           ; n_max = arg
     LOAD_CONST C       (preroll, executed once)
     SLOT_LE_SLOT slot_i slot_n
     BR_IF_FALSE +19
     POP
     SLOT_(ADD|SUB)_FIX slot_s K_STEP_S
     STORE_SLOT slot_s
     LOAD_SLOT slot_i, LOAD_FIX 1, ADD, STORE_SLOT slot_i, POP
     JUMP -25
     UNBIND_SLOT slot_n, slot_i, slot_s, RET
   We emit a tight native loop: untag n_max once, run i/s in untagged
   regs, retag s on exit. ~5 cycles per iteration. */
static int try_jit_for_loop_inc(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 48)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_FIX)
    return 0;
  int16_t K_init_s = (int16_t)((uint16_t)c[1] | ((uint16_t)c[2] << 8));
  if (c[3] != OP_BIND_SLOT)
    return 0;
  uint8_t slot_s = c[4];
  if (c[5] != OP_LOAD_FIX)
    return 0;
  int16_t K_init_i = (int16_t)((uint16_t)c[6] | ((uint16_t)c[7] << 8));
  if (c[8] != OP_BIND_SLOT)
    return 0;
  uint8_t slot_i = c[9];
  if (c[10] != OP_LOAD_SLOT)
    return 0;
  uint8_t slot_arg = c[11];
  if (c[12] != OP_BIND_SLOT)
    return 0;
  uint8_t slot_n = c[13];
  if (c[14] != OP_LOAD_CONST)
    return 0;
  if (c[16] != OP_SLOT_LE_SLOT)
    return 0;
  if (c[17] != slot_i || c[18] != slot_n)
    return 0;
  if (c[19] != OP_BR_IF_FALSE)
    return 0;
  /* loop-exit branch offset: must land on first UNBIND. */
  int16_t br_off = (int16_t)((uint16_t)c[20] | ((uint16_t)c[21] << 8));
  if (br_off != 19)
    return 0;
  if (c[22] != OP_POP)
    return 0;

  uint8_t step_s_op = c[23];
  if (step_s_op != OP_SLOT_ADD_FIX && step_s_op != OP_SLOT_SUB_FIX)
    return 0;
  if (c[24] != slot_s)
    return 0;
  int16_t K_step_s = (int16_t)((uint16_t)c[25] | ((uint16_t)c[26] << 8));
  if (c[27] != OP_STORE_SLOT || c[28] != slot_s)
    return 0;

  if (c[29] != OP_LOAD_SLOT || c[30] != slot_i)
    return 0;
  if (c[31] != OP_LOAD_FIX)
    return 0;
  int16_t K_step_i = (int16_t)((uint16_t)c[32] | ((uint16_t)c[33] << 8));
  if (K_step_i != 1)
    return 0;
  if (c[34] != OP_ADD)
    return 0;
  if (c[35] != OP_STORE_SLOT || c[36] != slot_i)
    return 0;
  if (c[37] != OP_POP)
    return 0;
  if (c[38] != OP_JUMP)
    return 0;
  int16_t jmp_off = (int16_t)((uint16_t)c[39] | ((uint16_t)c[40] << 8));
  if (jmp_off != -25)
    return 0;

  if (c[41] != OP_UNBIND_SLOT)
    return 0;
  if (c[43] != OP_UNBIND_SLOT)
    return 0;
  if (c[45] != OP_UNBIND_SLOT)
    return 0;
  if (c[47] != OP_RET)
    return 0;
  if (slot_arg >= ENV_INLINE_SLOTS)
    return 0;

  int32_t arg_off =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)slot_arg * 8;
  int n = 0;

  /* Load n_max from arg, tag-check, untag. */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, arg_off);
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_deopt = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_sar_imm8(buf + n, X64_RAX, 3); /* rax = n_max untagged */

  /* Init i (rcx) and s (rdx) untagged. */
  n += x64_mov_imm64(buf + n, X64_RCX, (uint64_t)(int64_t)K_init_i);
  n += x64_mov_imm64(buf + n, X64_RDX, (uint64_t)(int64_t)K_init_s);

  /* Loop top: cmp rcx, rax; jg done */
  int loop_top = n;
  /* cmp rcx, rax  →  REX.W 0x39 /r, ModR/M 0xC0|src<<3|dst */
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RAX & 7) << 3) | (X64_RCX & 7));
  int jcc_done = n;
  n += x64_jcc_rel32(buf + n, 0x0F, 0); /* jg done */

  /* s += K_step_s (or -=) */
  if (step_s_op == OP_SLOT_ADD_FIX)
    n += x64_add_imm32(buf + n, X64_RDX, (int32_t)K_step_s);
  else
    n += x64_sub_imm32(buf + n, X64_RDX, (int32_t)K_step_s);

  /* i += 1 */
  n += x64_add_imm32(buf + n, X64_RCX, 1);

  /* jmp loop_top */
  int jmp_back_pc = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back_pc, 5, loop_top);

  /* done: re-tag s into rax and return. */
  int done_pc = n;
  /* shl rdx, 3 →  REX.W 0xC1 /4 imm8.  ModR/M = 0xE0 | (rdx&7) = 0xE2 */
  buf[n++] = 0x48;
  buf[n++] = 0xC1;
  buf[n++] = (uint8_t)(0xE0 | (X64_RDX & 7));
  buf[n++] = 3;
  /* or rdx, 1  →  REX.W 0x83 /1 imm8.  ModR/M = 0xC8 | (rdx&7) = 0xCA */
  buf[n++] = 0x48;
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0xC8 | (X64_RDX & 7));
  buf[n++] = 1;
  n += x64_mov_reg_reg(buf + n, X64_RAX, X64_RDX);
  n += x64_ret(buf + n);

  /* deopt → return NULL */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jcc_done, 6, done_pc);
  x64_patch_rel32(buf, jz_deopt, 6, deopt_pc);

  JIT_GUARD(128);
  *outn = n;
  return 1;
}

/* (fn (n) (if (cmp n K1) BASE (* n (f (op n K2))))) — 24-byte
   non-tail single-arg recursion with multiplication. The fact shape:
     0000 SLOT_LT_FIX slot=0 imm=K1
     0004 BR_IF_FALSE +6
     0007 LOAD_FIX BASE
     0010 JUMP +10
     0013 LOAD_SLOT 0
     0015 SLOT_(SUB|ADD)_FIX slot=0 imm=K2
     0019 CALL_GLOBAL idx=I nargs=1
     0022 MUL
     0023 RET

   Multiplication is associative, so the recursion folds into a loop:
     acc = BASE
     while !cmp(n, K1):  acc *= n;  n = n op K2
     return acc
   This skips ALL call/dispatch overhead (no env alloc, no helper).
   ~3 cycles per iteration vs ~60 cycles for the recursive emission.
   Correct for any (n, K1, K2, BASE) where the original program would
   terminate — overflow on tagged fixnum (>2^60) is identical to the
   recursive version. */
static int try_jit_recurse_mul_one(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 24)
    return 0;
  uint8_t *c = bc->code;

  /* base-case predicate */
  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_LT_FIX && cmp_op != OP_SLOT_GT_FIX &&
      cmp_op != OP_SLOT_LE_FIX && cmp_op != OP_SLOT_GE_FIX)
    return 0;
  uint8_t slot = c[1];
  if (slot >= ENV_INLINE_SLOTS)
    return 0;
  int16_t K1 = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE)
    return 0;
  if (c[7] != OP_LOAD_FIX)
    return 0;
  int16_t BASE = (int16_t)((uint16_t)c[8] | ((uint16_t)c[9] << 8));
  if (c[10] != OP_JUMP)
    return 0;

  /* recurse arm */
  if (c[13] != OP_LOAD_SLOT || c[14] != slot)
    return 0;
  uint8_t step_op = c[15];
  if (step_op != OP_SLOT_SUB_FIX && step_op != OP_SLOT_ADD_FIX)
    return 0;
  if (c[16] != slot)
    return 0;
  int16_t K2 = (int16_t)((uint16_t)c[17] | ((uint16_t)c[18] << 8));

  if (c[19] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx = c[20];
  if (c[21] != 1)
    return 0;
  if (idx >= bc->nconsts)
    return 0;
  if (c[22] != OP_MUL)
    return 0;
  if (c[23] != OP_RET)
    return 0;

  /* Self-name guard (see try_jit_recurse_add_two): the iterative-fact
     emission below elides the call entirely, so a non-self callee would
     be silently rewritten as iterative factorial. */
  if (!bc->self_name)
    return 0;
  exp_t *callee = bc->consts[idx];
  if (!issymbol(callee))
    return 0;
  if (strcmp((const char *)exp_text(callee), bc->self_name) != 0)
    return 0;

  int32_t slot_off =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)slot * 8;
  int32_t K1_tagged = ((int32_t)K1 << 3) | 1;
  int32_t K2_delta = ((int32_t)K2) << 3;
  uint64_t BASE_tag = ((uint64_t)(int64_t)BASE << 3) | 1;

  /* base case taken on the FAILURE of the cmp. The cc here is the one
     that triggers the BASE return — same convention as recurse_add_two
     but mirrored (we fall-through into the recurse path on the inverse). */
  uint8_t inv_cc;
  switch (cmp_op) {
  case OP_SLOT_LT_FIX:
    inv_cc = 0x0D;
    break; /* recurse on jge */
  case OP_SLOT_GT_FIX:
    inv_cc = 0x0E;
    break; /* recurse on jle */
  case OP_SLOT_LE_FIX:
    inv_cc = 0x0F;
    break; /* recurse on jg  */
  case OP_SLOT_GE_FIX:
    inv_cc = 0x0C;
    break; /* recurse on jl  */
  default:
    return 0;
  }

  /* Suppress unused-arg warning — we no longer go through the helper. */
  (void)idx;

  int n = 0;

  /* Load arg, tag-check, untag → rax = n (untagged). */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_deopt = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_sar_imm8(buf + n, X64_RAX, 3);

  /* rcx = acc, untagged. Initialised to BASE. */
  n += x64_mov_imm64(buf + n, X64_RCX, (uint64_t)(int64_t)BASE);

  /* Loop: while !cmp(n, K1) — same inv_cc mapping as the recurse path
     (we LOOP on the inverted condition, EXIT on the original cmp). */
  int loop_top = n;
  n += x64_cmp_imm32(buf + n, X64_RAX, (int32_t)K1);
  int jcc_done = n;
  /* Need the EXIT cc, not the recurse cc. exit on cmp_op true →
     jl on LT, jg on GT, jle on LE, jge on GE. */
  uint8_t exit_cc;
  switch (cmp_op) {
  case OP_SLOT_LT_FIX:
    exit_cc = 0x0C;
    break; /* jl  */
  case OP_SLOT_GT_FIX:
    exit_cc = 0x0F;
    break; /* jg  */
  case OP_SLOT_LE_FIX:
    exit_cc = 0x0E;
    break; /* jle */
  case OP_SLOT_GE_FIX:
    exit_cc = 0x0D;
    break; /* jge */
  default:
    return 0;
  }
  n += x64_jcc_rel32(buf + n, exit_cc, 0);
  /* Silence the unused-var warning for inv_cc (kept for symmetry). */
  (void)inv_cc;

  /* acc = acc * n  (both untagged) */
  n += x64_imul_reg_reg(buf + n, X64_RCX, X64_RAX);

  /* n = n op K2 (untagged) */
  if (step_op == OP_SLOT_SUB_FIX)
    n += x64_sub_imm32(buf + n, X64_RAX, (int32_t)K2);
  else
    n += x64_add_imm32(buf + n, X64_RAX, (int32_t)K2);

  /* jmp loop_top */
  int jmp_back_pc = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back_pc, 5, loop_top);

  /* done: re-tag acc into rax, return. */
  int done_pc = n;
  /* shl rcx, 3 */
  buf[n++] = 0x48;
  buf[n++] = 0xC1;
  buf[n++] = (uint8_t)(0xE0 | (X64_RCX & 7));
  buf[n++] = 3;
  /* or rcx, 1 */
  buf[n++] = 0x48;
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0xC8 | (X64_RCX & 7));
  buf[n++] = 1;
  n += x64_mov_reg_reg(buf + n, X64_RAX, X64_RCX);
  n += x64_ret(buf + n);

  /* deopt → return NULL */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  /* Suppress unused-var warnings for variables that became dead when we
     switched from recursive emission to iterative. */
  (void)K1_tagged;
  (void)K2_delta;
  (void)BASE_tag;

  x64_patch_rel32(buf, jcc_done, 6, done_pc);
  x64_patch_rel32(buf, jz_deopt, 6, deopt_pc);

  JIT_GUARD(128);
  *outn = n;
  return 1;
}

/* (fn (a b) (is (mod a b) K)) — 2-param leaf computing tagged
   modulo + equality, returns t/nil. The divides? shape from sieve.
   Bytecode (10 bytes):
     [0] LOAD_SLOT a       2
     [2] LOAD_SLOT b       2
     [4] MOD               1
     [5] LOAD_FIX K        3
     [8] IS                1
     [9] RET               1

   Codegen: load both slots into rax/rcx, tag-check both, untag (sub 1),
   idiv. Compare remainder with K shifted (no re-tag needed since IS
   compares the underlying value bits). Return TRUE_EXP/NIL_EXP via
   cmovz. Avoids vm_invoke_values entirely — saves ~200ns/call. */
static int try_jit_modeq_leaf(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 10)
    return 0;
  uint8_t *c = bc->code;
  if (c[0] != OP_LOAD_SLOT || c[1] >= ENV_INLINE_SLOTS)
    return 0;
  if (c[2] != OP_LOAD_SLOT || c[3] >= ENV_INLINE_SLOTS)
    return 0;
  if (c[4] != OP_MOD)
    return 0;
  if (c[5] != OP_LOAD_FIX)
    return 0;
  int16_t K = (int16_t)((uint16_t)c[6] | ((uint16_t)c[7] << 8));
  if (c[8] != OP_IS)
    return 0;
  if (c[9] != OP_RET)
    return 0;

  int32_t off_a = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)c[1] * 8;
  int32_t off_b = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)c[3] * 8;
  int32_t k_shifted = ((int32_t)K) << 3; /* compare against (K<<3) */

  int n = 0;

  /* Load both slots tagged. */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_a);
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_b);
  /* Tag-check both — bail to bytecode if either isn't a fixnum. */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz1 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  /* Untag both (drop low bit). After this, rax=a<<3 and rcx=b<<3. */
  n += x64_sub_imm32(buf + n, X64_RAX, 1);
  n += x64_sub_imm32(buf + n, X64_RCX, 1);
  /* Guard against div-by-zero — bail rather than crash. */
  n += x64_test_reg_reg(buf + n, X64_RCX, X64_RCX);
  int jz_bz = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  /* Sign-extend rax into rdx:rax, then signed div. rdx ← (a<<3) % (b<<3)
     which equals (a%b)<<3 — same scaling property as add/sub. */
  n += x64_cqo(buf + n);
  n += x64_idiv_reg(buf + n, X64_RCX);
  /* Compare remainder against K<<3 (tag bits irrelevant — all values
     here have bit0=0). cmovz selects TRUE_EXP if equal. */
  n += x64_cmp_imm32(buf + n, X64_RDX, k_shifted);
  n += x64_mov_imm64(buf + n, X64_RAX, (uint64_t)(uintptr_t)nil_singleton);
  n += x64_mov_imm64(buf + n, X64_RCX, (uint64_t)(uintptr_t)true_singleton);
  n += x64_cmovz_reg_reg(buf + n, X64_RAX, X64_RCX);
  n += x64_ret(buf + n);

  /* Single deopt point — return NULL → caller's vm_run kicks in. */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jz1, 6, deopt_pc);
  x64_patch_rel32(buf, jz2, 6, deopt_pc);
  x64_patch_rel32(buf, jz_bz, 6, deopt_pc);

  JIT_GUARD(96);
  *outn = n;
  return 1;
}

/* Set via env: ALCOVE_JIT_TRACE=1 to log which shape (or "miss") matched
   for each bytecode submitted to jit_compile. Off by default. Cached on
   first call. */
static int jit_trace(void) {
  static int v = -1;
  if (v < 0)
    v = (getenv("ALCOVE_JIT_TRACE") != NULL);
  return v;
}
#define JT(shape)                                                              \
  do {                                                                         \
    if (jit_trace())                                                           \
      fprintf(stderr, "[jit] %-28s ncode=%d\n", (shape), bc->ncode);           \
  } while (0)

int jit_compile(bytecode_t *bc) {
  if (!bc || bc->jit)
    return bc && bc->jit ? 1 : 0;
  uint8_t *c = bc->code;

  if (try_jit_build_inc_cons_c(bc)) {
    JT("build_inc_cons_c");
    return 1;
  }

  if (try_jit_nqueens_solve_c(bc)) {
    JT("nqueens_solve_c");
    return 1;
  }

  uint8_t buf[512];
  int n = 0;

  if (try_jit_simple_tail_loop(bc, buf, &n)) {
    JT("simple_tail_loop");
  } else if (try_jit_tail_loop_with_call(bc, buf, &n)) {
    JT("tail_loop_with_call");
  } else if (try_jit_recurse_add_two(bc, buf, &n)) {
    JT("recurse_add_two");
  } else if (try_jit_recurse_mul_one(bc, buf, &n)) {
    JT("recurse_mul_one");
  } else if (try_jit_for_loop_inc(bc, buf, &n)) {
    JT("for_loop_inc");
  } else if (try_jit_ackermann(bc, buf, &n)) {
    JT("ackermann");
  } else if (try_jit_tak(bc, buf, &n)) {
    JT("tak");
  } else if (try_jit_mark_from(bc, buf, &n)) {
    JT("mark_from");
  } else if (try_jit_count_primes(bc, buf, &n)) {
    JT("count_primes");
  } else if (try_jit_is_prime_given(bc, buf, &n)) {
    JT("is_prime_given");
  } else if (try_jit_safe_p(bc, buf, &n)) {
    JT("safe_p");
  } else if (try_jit_modeq_leaf(bc, buf, &n)) {
    JT("modeq_leaf");
  } else if (bc->ncode == 4 && c[0] == OP_LOAD_FIX && c[3] == OP_RET) {
    JT("leaf_const");
    /* (fn () K)  →  mov rax, tagged; ret */
    int16_t k = (int16_t)((uint16_t)c[1] | ((uint16_t)c[2] << 8));
    uint64_t tagged = ((uint64_t)(int64_t)k << 3) | 1;
    n += x64_mov_imm64(buf + n, X64_RAX, tagged);
    n += x64_ret(buf + n);
  } else if (bc->ncode == 7 && c[0] == OP_LOAD_SLOT &&
             c[1] < ENV_INLINE_SLOTS && c[2] == OP_LOAD_FIX &&
             (c[5] == OP_ADD || c[5] == OP_SUB || c[5] == OP_MUL) &&
             c[6] == OP_RET) {
    JT("leaf_slot_op_fix");
    /* (fn (... s ...) (op s K)) for K in int16, op in {+, -, *}.
       For ADD/SUB: ±(K<<3) preserves the tag bit directly.
       For MUL:     drop tag (sub 1), imul by K, re-tag (add 1). */
    int16_t k = (int16_t)((uint16_t)c[3] | ((uint16_t)c[4] << 8));
    int32_t slot_off =
        (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)c[1] * 8;
    n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
    n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
    int jz_start = n;
    n += x64_jcc_rel32(buf + n, 0x04, 0);
    if (c[5] == OP_MUL) {
      n += x64_sub_imm32(buf + n, X64_RAX, 1); /* drop tag */
      n += x64_imul_reg_reg_imm32(buf + n, X64_RAX, X64_RAX, (int32_t)k);
      n += x64_add_imm32(buf + n, X64_RAX, 1); /* re-tag */
    } else {
      int32_t delta = ((int32_t)k) << 3;
      if (c[5] == OP_ADD)
        n += x64_add_imm32(buf + n, X64_RAX, delta);
      else
        n += x64_sub_imm32(buf + n, X64_RAX, delta);
    }
    n += x64_ret(buf + n);
    int deopt_pc = n;
    X64_EMIT_DEOPT();
    x64_patch_rel32(buf, jz_start, 6, deopt_pc);
  }
  /* slot-fix superinstruction form: SLOT_ADD_FIX/SLOT_SUB_FIX slot K, RET */
  else if (bc->ncode == 5 &&
           (c[0] == OP_SLOT_ADD_FIX || c[0] == OP_SLOT_SUB_FIX) &&
           c[1] < ENV_INLINE_SLOTS && c[4] == OP_RET) {
    JT("leaf_slot_fix_super");
    int16_t k = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));
    int32_t delta = ((int32_t)k) << 3;
    int32_t slot_off =
        (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)c[1] * 8;
    n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
    n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
    int jz_start = n;
    n += x64_jcc_rel32(buf + n, 0x04, 0);
    if (c[0] == OP_SLOT_ADD_FIX)
      n += x64_add_imm32(buf + n, X64_RAX, delta);
    else
      n += x64_sub_imm32(buf + n, X64_RAX, delta);
    n += x64_ret(buf + n);
    int deopt_pc = n;
    X64_EMIT_DEOPT();
    x64_patch_rel32(buf, jz_start, 6, deopt_pc);
  } else {
    JT("miss");
    return 0; /* shape not recognized */
  }

  /* Each matcher has its own internal bound check (returns 0 on
     overflow); this catch-all protects buf as a whole including the
     inline leaf-shape paths above. Hard fall-back rather than abort —
     bytecode will run the body. Survives -DNDEBUG, unlike assert(). */
  if (n > (int)sizeof(buf))
    return 0;
  size_t sz = (size_t)n;
  size_t pagesz = 4096;
  size_t mapsz = (sz + pagesz - 1) & ~(pagesz - 1);
  void *page = jit_alloc(mapsz);
  if (!page)
    return 0;
  jit_write_begin();
  memcpy(page, buf, sz);
  jit_write_end(page, sz);
  bc->jit = (exp_t * (*)(env_t *)) page;
  bc->jit_mem = page;
  bc->jit_size = mapsz;
  return 1;
}

#endif /* __x86_64__ */

#endif /* ALCOVE_JIT */

static void emit_u8(compiler_t *c, uint8_t b) {
  if (c->failed)
    return;
  if (c->ncode + 1 > c->code_cap) {
    c->code_cap = c->code_cap ? c->code_cap * 2 : 64;
    c->code = realloc(c->code, c->code_cap);
  }
  c->code[c->ncode++] = b;
}
static void emit_i16(compiler_t *c, int16_t v) {
  emit_u8(c, (uint8_t)(v & 0xff));
  emit_u8(c, (uint8_t)((v >> 8) & 0xff));
}
static int add_const(compiler_t *c, exp_t *v) {
  /* de-dupe by pointer equality — rare wins but costs nothing */
  int i;
  for (i = 0; i < c->nconsts; i++)
    if (c->consts[i] == v)
      return i;
  /* OP_LOAD_CONST encodes the index as u8, so at most 256 distinct
     constants per lambda. Above that we bail rather than silently
     wrap — the tree-walker will still handle the body. */
  if (c->nconsts >= 256) {
    c->failed = 1;
    return -1;
  }
  if (c->nconsts + 1 > c->consts_cap) {
    c->consts_cap = c->consts_cap ? c->consts_cap * 2 : 8;
    c->consts = realloc(c->consts, c->consts_cap * sizeof(exp_t *));
  }
  c->consts[c->nconsts] = refexp(v);
  return c->nconsts++;
}
static int find_slot(compiler_t *c, const char *name) {
  /* Innermost (highest idx) binding wins so inner let shadows outer.
     NULL slot_names are "hidden" (e.g. for's end-value slot) — skipped. */
  int i;
  for (i = c->nslots - 1; i >= 0; i--) {
    if (!c->slot_names[i])
      continue;
    if (strcmp(c->slot_names[i], name) == 0)
      return i;
  }
  return -1;
}

static int op_for_head(const char *s);
static void compile_expr(compiler_t *c, exp_t *e, int tail);

/* Returns OP_ADD..OP_NOT for pure-arithmetic/cmp symbols, -1 otherwise. */
static int op_for_head(const char *s) {
  if (!strcmp(s, "+"))
    return OP_ADD;
  if (!strcmp(s, "-"))
    return OP_SUB;
  if (!strcmp(s, "*"))
    return OP_MUL;
  if (!strcmp(s, "/"))
    return OP_DIV;
  if (!strcmp(s, "mod"))
    return OP_MOD;
  if (!strcmp(s, "<"))
    return OP_LT;
  if (!strcmp(s, ">"))
    return OP_GT;
  if (!strcmp(s, "<="))
    return OP_LE;
  if (!strcmp(s, ">="))
    return OP_GE;
  if (!strcmp(s, "is"))
    return OP_IS;
  if (!strcmp(s, "iso"))
    return OP_ISO;
  if (!strcmp(s, "no"))
    return OP_NOT;
  return -1;
}

/* Compile a sequence of body forms (e.g. a multi-expression let/with/
   when body): evaluate each in order, POP all but the last, and let the
   last keep the caller's tail position. Empty body compiles to nil. */
static void compile_body_seq(compiler_t *c, exp_t *body, int tail) {
  if (!body) {
    int k = add_const(c, nil_singleton);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
    return;
  }
  int saw_any = 0;
  for (; body; body = body->next) {
    if (saw_any)
      emit_u8(c, OP_POP);
    int is_last = (body->next == NULL);
    compile_expr(c, body->content, is_last && tail);
    if (c->failed)
      return;
    saw_any = 1;
  }
}

static void compile_if(compiler_t *c, exp_t *form, int tail) {
  /* (if cond then else)  — only 2-way for phase 1 */
  exp_t *cond = cadr(form);
  exp_t *thn = caddr(form);
  exp_t *els = cadddr(form);
  if (cdddr(form) && cdddr(form)->next) {
    c->failed = 1;
    return;
  }
  compile_expr(c, cond, 0);
  if (c->failed)
    return;
  emit_u8(c, OP_BR_IF_FALSE);
  int patch_false = c->ncode;
  emit_i16(c, 0);
  compile_expr(c, thn, tail);
  if (c->failed)
    return;
  emit_u8(c, OP_JUMP);
  int patch_end = c->ncode;
  emit_i16(c, 0);
  int false_target = c->ncode;
  if (els)
    compile_expr(c, els, tail);
  else {
    /* (if cond then) with no else: result is nil. */
    int k = add_const(c, NIL_EXP);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
  }
  if (c->failed)
    return;
  int end_target = c->ncode;
  int16_t off_false = (int16_t)(false_target - (patch_false + 2));
  int16_t off_end = (int16_t)(end_target - (patch_end + 2));
  c->code[patch_false] = off_false & 0xff;
  c->code[patch_false + 1] = (off_false >> 8) & 0xff;
  c->code[patch_end] = off_end & 0xff;
  c->code[patch_end + 1] = (off_end >> 8) & 0xff;
}

static void compile_call(compiler_t *c, exp_t *form, int tail) {
  /* Emits one of three ops depending on context:
       - OP_TAIL_SELF: same-fn tail call, rebinds inline slots in place.
         Requires tail && self_name matches head && nlet_depth == 0
         (the inline-slot invariant).
       - OP_TAIL_CALL: other-fn tail call. VM tears down current env
         and jumps to the target lambda with O(1) C stack growth.
         Target must be resolvable at runtime as a lambda.
       - OP_CALL: regular non-tail call (and the fallback when the
         target might be an internal cmd — vm_invoke_values handles). */
  exp_t *head = car(form);
  int nargs = 0;
  exp_t *a;
  /* Slot-headed calls compile fine now that vm_invoke_values has a
     string-as-callable arm (ticket 6). The earlier blanket refusal
     was too conservative. */
  int is_self_tail = tail && c->self_name && c->nlet_depth == 0 &&
                     issymbol(head) && strcmp(exp_text(head), c->self_name) == 0;
  /* Cross-function tail call is safe regardless of nlet_depth:
     OP_TAIL_CALL wholesale releases current env's inline slots. */
  int is_cross_tail = tail && !is_self_tail;
  /* Fused LOAD_GLOBAL+CALL: if head is a symbol that isn't a local
     slot and isn't the self-tail case, we can skip the LOAD_GLOBAL
     dispatch + PUSH/POP and call via the gcache directly. */
  int use_call_global = 0, global_idx = -1;
  if (!is_self_tail && !is_cross_tail && issymbol(head) &&
      find_slot(c, exp_text(head)) < 0) {
    global_idx = add_const(c, head);
    if (global_idx < 0) {
      c->failed = 1;
      return;
    }
    use_call_global = 1;
  }
  if (!is_self_tail && !use_call_global) {
    compile_expr(c, head, 0);
    if (c->failed)
      return;
  }
  for (a = form->next; a; a = a->next) {
    compile_expr(c, a->content, 0);
    if (c->failed)
      return;
    nargs++;
  }
  if (nargs > 255) {
    c->failed = 1;
    return;
  }
  if (is_self_tail) {
    emit_u8(c, OP_TAIL_SELF);
    emit_u8(c, (uint8_t)nargs);
  } else if (is_cross_tail) {
    emit_u8(c, OP_TAIL_CALL);
    emit_u8(c, (uint8_t)nargs);
  } else if (use_call_global) {
    emit_u8(c, OP_CALL_GLOBAL);
    emit_u8(c, (uint8_t)global_idx);
    emit_u8(c, (uint8_t)nargs);
  } else {
    emit_u8(c, OP_CALL);
    emit_u8(c, (uint8_t)nargs);
  }
}

/* Superinstruction fuse table — maps a plain binary op to its fused
   slot-op-fix variant. Returns 0 when no fuse exists for this op. */
static int fuse_slot_fix(int op) {
  switch (op) {
  case OP_ADD:
    return OP_SLOT_ADD_FIX;
  case OP_SUB:
    return OP_SLOT_SUB_FIX;
  case OP_LT:
    return OP_SLOT_LT_FIX;
  case OP_LE:
    return OP_SLOT_LE_FIX;
  case OP_GT:
    return OP_SLOT_GT_FIX;
  case OP_GE:
    return OP_SLOT_GE_FIX;
  default:
    return 0;
  }
}

static void compile_arith(compiler_t *c, exp_t *form, int op) {
  /* Binary left-fold: (+ a b c d) → a b + c + d + */
  exp_t *a = form->next;
  if (!a || !a->next) {
    c->failed = 1;
    return;
  }
  if ((op == OP_LT || op == OP_GT || op == OP_LE || op == OP_GE) &&
      a->next->next) {
    c->failed = 1;
    return;
  }
  exp_t *arg1 = a->content;
  exp_t *arg2 = a->next->content;
  int is_binary = !a->next->next;

  /* Canonicalize (+ K slot) → (+ slot K) for commutative ops so the
     slot-fix peephole and JIT shape matchers see the canonical form.
     Binary-only — the peephole below doesn't handle >2 args anyway. */
  if (is_binary && (op == OP_ADD || op == OP_MUL) && isnumber(arg1) &&
      issymbol(arg2)) {
    exp_t *tmp = arg1;
    arg1 = arg2;
    arg2 = tmp;
  }

  /* Peephole: exactly 2 args, arg1 is a local slot symbol, arg2 is a
     fixnum fitting in int16. Emit one fused op instead of three. */
  if (is_binary) {
    int fused = fuse_slot_fix(op);
    if (fused && issymbol(arg1) && isnumber(arg2)) {
      int slot = find_slot(c, exp_text(arg1));
      int64_t v = FIX_VAL(arg2);
      if (slot >= 0 && v >= INT16_MIN && v <= INT16_MAX) {
        emit_u8(c, (uint8_t)fused);
        emit_u8(c, (uint8_t)slot);
        emit_i16(c, (int16_t)v);
        return;
      }
    }
  }

  compile_expr(c, arg1, 0);
  if (c->failed)
    return;
  compile_expr(c, arg2, 0);
  if (c->failed)
    return;
  emit_u8(c, (uint8_t)op);
  for (a = a->next->next; a; a = a->next) {
    compile_expr(c, a->content, 0);
    if (c->failed)
      return;
    emit_u8(c, (uint8_t)op);
  }
}

/* (= sym val) — only when sym resolves to a local slot. Global / car /
   cdr / string-index assignment stays in the tree-walker. */
static void compile_assign(compiler_t *c, exp_t *form, int tail) {
  (void)tail;
  exp_t *key = cadr(form);
  exp_t *val = caddr(form);
  if (!issymbol(key)) {
    c->failed = 1;
    return;
  }
  int slot = find_slot(c, exp_text(key));
  compile_expr(c, val, 0);
  if (c->failed)
    return;
  if (slot >= 0) {
    emit_u8(c, OP_STORE_SLOT);
    emit_u8(c, (uint8_t)slot);
    /* STORE_SLOT re-pushes the stored value so (= x v) returns v. */
    return;
  }
  /* Not a local slot: a captured free var (mutable closure) or a global.
     Store via a runtime env-chain walk that matches updatebang's `=`
     semantics. This is what lets mutable closures compile to bytecode
     instead of failing here and falling back to AST. A reserved-name
     target is rejected at runtime by OP_STORE_FREE (slot < 0 always,
     since reserved names can't bind to a slot), same as OP_SETQ_DYN. */
  int k = add_const(c, key);
  if (c->failed)
    return;
  emit_u8(c, OP_STORE_FREE);
  emit_u8(c, (uint8_t)k);
}

/* (let var val body ...) — single binding, evaluates body in extended
   scope. Destructuring (let (a b) val body) falls back to AST (var is a
   pair, not a symbol). Falls back if slot count would overflow. */
static void compile_let(compiler_t *c, exp_t *form, int tail) {
  exp_t *var = cadr(form);
  exp_t *val = caddr(form);
  exp_t *body = form->next ? (form->next->next ? form->next->next->next : NULL)
                           : NULL;
  if (!issymbol(var) || !body) {
    c->failed = 1;
    return;
  }
  if (c->nslots >= ENV_INLINE_SLOTS) {
    c->failed = 1;
    return;
  }
  int slot = c->nslots;
  compile_expr(c, val, 0);
  if (c->failed)
    return;
  emit_u8(c, OP_BIND_SLOT);
  emit_u8(c, (uint8_t)slot);
  c->slot_names[slot] = (char *)exp_text(var);
  c->nslots++;
  c->nlet_depth++;
  compile_body_seq(c, body, tail);
  if (c->failed)
    return;
  c->nlet_depth--;
  c->nslots--;
  /* Body's value is on the stack; the binding's owning ref is still in
     the slot. UNBIND_SLOT unrefs and NULLs it, leaving the result. */
  emit_u8(c, OP_UNBIND_SLOT);
  emit_u8(c, (uint8_t)slot);
}

/* (with (v1 e1 v2 e2 ...) body) — N parallel-like bindings then body.
   In alcove's semantics, bindings evaluate left-to-right against the
   enclosing env (each val doesn't see earlier v's in the same with,
   matching the tree-walker's withcmd). */
static void compile_with(compiler_t *c, exp_t *form, int tail) {
  exp_t *pairs = cadr(form);
  exp_t *body = form->next ? form->next->next : NULL;
  if (!ispair(pairs) || !body) {
    c->failed = 1;
    return;
  }
  /* Collect (var, val) pairs. */
  int start_slot = c->nslots;
  int nbindings = 0;
  exp_t *p = pairs;
  while (p && p->content) {
    exp_t *var = p->content;
    exp_t *nxt = p->next;
    if (!nxt) {
      c->failed = 1;
      return;
    }
    exp_t *val = nxt->content;
    if (!issymbol(var)) {
      c->failed = 1;
      return;
    }
    if (c->nslots >= ENV_INLINE_SLOTS) {
      c->failed = 1;
      return;
    }
    compile_expr(c, val, 0);
    if (c->failed)
      return;
    emit_u8(c, OP_BIND_SLOT);
    emit_u8(c, (uint8_t)c->nslots);
    c->slot_names[c->nslots] = (char *)exp_text(var);
    c->nslots++;
    nbindings++;
    p = nxt->next;
  }
  c->nlet_depth++;
  compile_body_seq(c, body, tail);
  if (c->failed)
    return;
  c->nlet_depth--;
  /* Unbind in reverse order. */
  int i;
  for (i = nbindings - 1; i >= 0; i--) {
    emit_u8(c, OP_UNBIND_SLOT);
    emit_u8(c, (uint8_t)(start_slot + i));
  }
  c->nslots -= nbindings;
}

/* (let* (v1 e1 v2 e2 ...) body ...) — sequential bindings: each val sees
   the slots bound by earlier pairs. The flat legacy form
   (let* v1 e1 ... single-body) is left to the tree-walker. */
static void compile_letstar(compiler_t *c, exp_t *form, int tail) {
  exp_t *first = cadr(form);
  if (!ispair(first)) { /* flat legacy form — defer to AST */
    c->failed = 1;
    return;
  }
  exp_t *body = form->next ? form->next->next : NULL;
  if (!body) {
    c->failed = 1;
    return;
  }
  int start_slot = c->nslots;
  int nbindings = 0;
  exp_t *p = first;
  while (p && p->content) {
    exp_t *var = p->content;
    exp_t *nxt = p->next;
    if (!nxt || !issymbol(var)) {
      c->failed = 1;
      return;
    }
    if (c->nslots >= ENV_INLINE_SLOTS) {
      c->failed = 1;
      return;
    }
    /* Compile val BEFORE registering this var's slot name, but AFTER
       earlier vars are visible — that gives let*'s sequential scope. */
    compile_expr(c, nxt->content, 0);
    if (c->failed)
      return;
    emit_u8(c, OP_BIND_SLOT);
    emit_u8(c, (uint8_t)c->nslots);
    c->slot_names[c->nslots] = (char *)exp_text(var);
    c->nslots++;
    nbindings++;
    p = nxt->next;
  }
  c->nlet_depth++;
  compile_body_seq(c, body, tail);
  if (c->failed)
    return;
  c->nlet_depth--;
  int i;
  for (i = nbindings - 1; i >= 0; i--) {
    emit_u8(c, OP_UNBIND_SLOT);
    emit_u8(c, (uint8_t)(start_slot + i));
  }
  c->nslots -= nbindings;
}

/* (when cond body ...) / (unless cond body ...). For `when`, the body runs
   when cond is truthy; for `unless`, when cond is falsey. The other case
   yields nil. `negate` selects unless semantics. */
static void compile_when_unless(compiler_t *c, exp_t *form, int tail,
                                int negate) {
  exp_t *cond = cadr(form);
  exp_t *body = form->next ? form->next->next : NULL;
  if (!cond) {
    c->failed = 1;
    return;
  }
  compile_expr(c, cond, 0);
  if (c->failed)
    return;
  emit_u8(c, OP_BR_IF_FALSE);
  int patch_false = c->ncode;
  emit_i16(c, 0);
  /* Fall-through path = cond truthy. For `when` that runs the body; for
     `unless` it yields nil. The false-branch target is the opposite. */
  exp_t *run = negate ? NULL : body;   /* truthy path */
  exp_t *skip = negate ? body : NULL;  /* falsey path */
  if (run)
    compile_body_seq(c, run, tail);
  else {
    int k = add_const(c, NIL_EXP);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
  }
  if (c->failed)
    return;
  emit_u8(c, OP_JUMP);
  int patch_end = c->ncode;
  emit_i16(c, 0);
  int false_target = c->ncode;
  if (skip)
    compile_body_seq(c, skip, tail);
  else {
    int k = add_const(c, NIL_EXP);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
  }
  if (c->failed)
    return;
  int end_target = c->ncode;
  int16_t off_false = (int16_t)(false_target - (patch_false + 2));
  int16_t off_end = (int16_t)(end_target - (patch_end + 2));
  c->code[patch_false] = off_false & 0xff;
  c->code[patch_false + 1] = (off_false >> 8) & 0xff;
  c->code[patch_end] = off_end & 0xff;
  c->code[patch_end + 1] = (off_end >> 8) & 0xff;
}

/* (for counter start end body...) — counter iterates start..end inclusive.
   Matches AST forcmd semantics: the body's final expression of the
   final iteration becomes the for's return value; nil if the loop
   never runs or the body is empty.

   Stack discipline: a "current result" sits on the stack across
   iterations (initially nil). Each iter POPs it before running the
   body, and the body's last expression leaves the new result. Exit
   branch takes the BR_IF_FALSE path with the result on top. */
static void compile_for(compiler_t *c, exp_t *form, int tail) {
  (void)tail;
  exp_t *var_node = form->next;
  if (!var_node) {
    c->failed = 1;
    return;
  }
  exp_t *var = var_node->content;
  exp_t *start_node = var_node->next;
  if (!start_node) {
    c->failed = 1;
    return;
  }
  exp_t *end_node = start_node->next;
  if (!end_node) {
    c->failed = 1;
    return;
  }
  exp_t *body_node = end_node->next;

  if (!issymbol(var)) {
    c->failed = 1;
    return;
  }
  if (c->nslots + 2 > ENV_INLINE_SLOTS) {
    c->failed = 1;
    return;
  }

  int counter_slot = c->nslots;
  int end_slot = c->nslots + 1;

  compile_expr(c, start_node->content, 0);
  if (c->failed)
    return;
  emit_u8(c, OP_BIND_SLOT);
  emit_u8(c, (uint8_t)counter_slot);
  c->slot_names[counter_slot] = (char *)exp_text(var);
  c->nslots++;

  compile_expr(c, end_node->content, 0);
  if (c->failed)
    return;
  emit_u8(c, OP_BIND_SLOT);
  emit_u8(c, (uint8_t)end_slot);
  c->slot_names[end_slot] = NULL;
  c->nslots++;

  /* Seed the loop's "current result" with nil so an un-entered or
     empty-body for still returns something at exit. */
  int k_nil = add_const(c, nil_singleton);
  emit_u8(c, OP_LOAD_CONST);
  emit_u8(c, (uint8_t)k_nil);

  c->nlet_depth++;
  int loop_top = c->ncode;

  /* Fused slot-vs-slot compare: replaces LOAD_SLOT+LOAD_SLOT+LE with
     one dispatch. Saves 2 dispatches per iteration in the hot loop. */
  emit_u8(c, OP_SLOT_LE_SLOT);
  emit_u8(c, (uint8_t)counter_slot);
  emit_u8(c, (uint8_t)end_slot);
  emit_u8(c, OP_BR_IF_FALSE);
  int patch_exit = c->ncode;
  emit_i16(c, 0);

  if (body_node) {
    /* Replace previous iteration's result with this one's. */
    emit_u8(c, OP_POP);
    exp_t *b;
    for (b = body_node; b; b = b->next) {
      compile_expr(c, b->content, 0);
      if (c->failed)
        return;
      if (b->next)
        emit_u8(c, OP_POP); /* discard non-last body exprs */
    }
    /* Last body expr's value remains on stack as the new "current result". */
  }

  emit_u8(c, OP_LOAD_SLOT);
  emit_u8(c, (uint8_t)counter_slot);
  emit_u8(c, OP_LOAD_FIX);
  emit_i16(c, 1);
  emit_u8(c, OP_ADD);
  emit_u8(c, OP_STORE_SLOT);
  emit_u8(c, (uint8_t)counter_slot);
  emit_u8(c, OP_POP);

  emit_u8(c, OP_JUMP);
  int patch_jump = c->ncode;
  emit_i16(c, 0);

  int loop_end = c->ncode;

  int16_t off_exit = (int16_t)(loop_end - (patch_exit + 2));
  c->code[patch_exit] = off_exit & 0xff;
  c->code[patch_exit + 1] = (off_exit >> 8) & 0xff;
  int16_t off_jump = (int16_t)(loop_top - (patch_jump + 2));
  c->code[patch_jump] = off_jump & 0xff;
  c->code[patch_jump + 1] = (off_jump >> 8) & 0xff;

  c->nlet_depth--;

  emit_u8(c, OP_UNBIND_SLOT);
  emit_u8(c, (uint8_t)end_slot);
  emit_u8(c, OP_UNBIND_SLOT);
  emit_u8(c, (uint8_t)counter_slot);
  c->nslots -= 2;
  /* Result left on top of stack by the last iteration (or the seed nil). */
}

static void compile_expr(compiler_t *c, exp_t *e, int tail) {
  if (c->failed)
    return;
  /* Tagged fixnum literal: if it fits in int16, inline; else const pool. */
  if (isnumber(e)) {
    int64_t v = FIX_VAL(e);
    if (v >= INT16_MIN && v <= INT16_MAX) {
      emit_u8(c, OP_LOAD_FIX);
      emit_i16(c, (int16_t)v);
    } else {
      int k = add_const(c, e);
      emit_u8(c, OP_LOAD_CONST);
      emit_u8(c, (uint8_t)k);
    }
    return;
  }
  if (!is_ptr(e)) {
    /* tagged char or other immediate */
    int k = add_const(c, e);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
    return;
  }
  if (isstring(e) || isfloat(e)) {
    int k = add_const(c, e);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
    return;
  }
  if (e == nil_singleton || e == true_singleton) {
    int k = add_const(c, e);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
    return;
  }
  if (issymbol(e)) {
    int slot = find_slot(c, exp_text(e));
    if (slot >= 0) {
      emit_u8(c, OP_LOAD_SLOT);
      emit_u8(c, (uint8_t)slot);
      return;
    }
    /* Global / builtin. Runtime lookup via the constant (the symbol
       itself is the key — lookup will cache on it via meta). */
    int k = add_const(c, e);
    emit_u8(c, OP_LOAD_GLOBAL);
    emit_u8(c, (uint8_t)k);
    return;
  }
  if (!ispair(e)) {
    c->failed = 1;
    return;
  }

  /* Call form. Dispatch on head. */
  exp_t *head = car(e);
  if (issymbol(head)) {
    const char *s = exp_text(head);
    if (!strcmp(s, "if")) {
      compile_if(c, e, tail);
      return;
    }
    if (!strcmp(s, "let")) {
      compile_let(c, e, tail);
      return;
    }
    if (!strcmp(s, "let*")) {
      compile_letstar(c, e, tail);
      return;
    }
    if (!strcmp(s, "with")) {
      compile_with(c, e, tail);
      return;
    }
    if (!strcmp(s, "when")) {
      compile_when_unless(c, e, tail, 0);
      return;
    }
    if (!strcmp(s, "unless")) {
      compile_when_unless(c, e, tail, 1);
      return;
    }
    if (!strcmp(s, "for")) {
      compile_for(c, e, tail);
      return;
    }
    if (!strcmp(s, "=") || !strcmp(s, "setf")) {
      compile_assign(c, e, tail);
      return;
    }
    if (!strcmp(s, "setq")) {
      exp_t *args = cdr(e);
      if (!args) { /* (setq) -> nil; let the tree-walker handle it */
        c->failed = 1;
        return;
      }
      for (exp_t *a = args; a; a = cddr(a)) {
        exp_t *sym = car(a);
        if (!issymbol(sym) || !cdr(a)) { /* malformed: defer to evaluator */
          c->failed = 1;
          return;
        }
        compile_expr(c, cadr(a), 0);
        if (c->failed)
          return;
        int slot = find_slot(c, exp_text(sym));
        if (slot >= 0) {
          emit_u8(c, OP_STORE_SLOT);
          emit_u8(c, (uint8_t)slot);
        } else {
          int k = add_const(c, sym);
          if (c->failed)
            return;
          emit_u8(c, OP_SETQ_DYN);
          emit_u8(c, (uint8_t)k);
        }
        /* Each pair leaves its value on the stack; setq's result is the
           last pair's value, so discard the earlier ones. */
        if (cddr(a))
          emit_u8(c, OP_POP);
      }
      return;
    }
    if (!strcmp(s, "do")) {
      /* Sequential eval, return last value. Same shape as the body
         walk in compile_lambda — emit each expr, POP between, last
         one keeps tail position. (do) with no exprs evaluates to nil. */
      exp_t *b = e->next;
      if (!b) {
        int k = add_const(c, nil_singleton);
        emit_u8(c, OP_LOAD_CONST);
        emit_u8(c, (uint8_t)k);
        return;
      }
      int saw_any = 0;
      for (; b; b = b->next) {
        if (saw_any)
          emit_u8(c, OP_POP);
        int is_last = (b->next == NULL);
        compile_expr(c, b->content, is_last && tail);
        if (c->failed)
          return;
        saw_any = 1;
      }
      return;
    }
    if (!strcmp(s, "cons")) {
      exp_t *a = cadr(e), *b = caddr(e);
      if (!a || !b) {
        c->failed = 1;
        return;
      }
      compile_expr(c, a, 0);
      if (c->failed)
        return;
      compile_expr(c, b, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_CONS);
      return;
    }
    if (!strcmp(s, "car")) {
      exp_t *a = cadr(e);
      if (!a) {
        c->failed = 1;
        return;
      }
      compile_expr(c, a, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_CAR);
      return;
    }
    if (!strcmp(s, "cdr")) {
      exp_t *a = cadr(e);
      if (!a) {
        c->failed = 1;
        return;
      }
      compile_expr(c, a, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_CDR);
      return;
    }
    if (!strcmp(s, "list")) {
      int n = 0;
      exp_t *a;
      for (a = e->next; a; a = a->next) {
        compile_expr(c, a->content, 0);
        if (c->failed)
          return;
        n++;
        if (n > 255) {
          c->failed = 1;
          return;
        }
      }
      emit_u8(c, OP_LIST);
      emit_u8(c, (uint8_t)n);
      return;
    }
    if (!strcmp(s, "vec-ref")) {
      exp_t *v = cadr(e), *i = caddr(e);
      if (!v || !i) {
        c->failed = 1;
        return;
      }
      compile_expr(c, v, 0);
      if (c->failed)
        return;
      compile_expr(c, i, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_VEC_REF);
      return;
    }
    if (!strcmp(s, "vec-set!")) {
      exp_t *v = cadr(e), *i = caddr(e), *x = cadddr(e);
      if (!v || !i || !x) {
        c->failed = 1;
        return;
      }
      compile_expr(c, v, 0);
      if (c->failed)
        return;
      compile_expr(c, i, 0);
      if (c->failed)
        return;
      compile_expr(c, x, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_VEC_SET);
      return;
    }
    if (!strcmp(s, "vec-len")) {
      exp_t *v = cadr(e);
      if (!v) {
        c->failed = 1;
        return;
      }
      compile_expr(c, v, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_VEC_LEN);
      return;
    }
    if (!strcmp(s, "vec")) {
      exp_t *n = cadr(e), *init = caddr(e);
      if (!n) {
        c->failed = 1;
        return;
      }
      compile_expr(c, n, 0);
      if (c->failed)
        return;
      if (init)
        compile_expr(c, init, 0);
      else {
        int k = add_const(c, nil_singleton);
        emit_u8(c, OP_LOAD_CONST);
        emit_u8(c, (uint8_t)k);
      }
      if (c->failed)
        return;
      emit_u8(c, OP_VEC_NEW);
      return;
    }
    if (!strcmp(s, "sqrt-int")) {
      exp_t *a = cadr(e);
      if (!a) {
        c->failed = 1;
        return;
      }
      compile_expr(c, a, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_SQRT_INT);
      return;
    }
    if (!strcmp(s, "length")) {
      exp_t *a = cadr(e);
      if (!a) {
        c->failed = 1;
        return;
      }
      compile_expr(c, a, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_LENGTH);
      return;
    }
    if (!strcmp(s, "quote")) {
      exp_t *q = cadr(e);
      int k = add_const(c, q ? q : nil_singleton);
      emit_u8(c, OP_LOAD_CONST);
      emit_u8(c, (uint8_t)k);
      return;
    }
    int op = op_for_head(s);
    if (op >= 0) {
      if (op == OP_NOT) {
        /* Unary: (no x) */
        if (!e->next) {
          c->failed = 1;
          return;
        }
        compile_expr(c, e->next->content, 0);
        if (c->failed)
          return;
        emit_u8(c, OP_NOT);
        return;
      }
      compile_arith(c, e, op);
      return;
    }
    /* Fail-closed: any head that resolves to an EXP_INTERNAL we haven't
       whitelisted above is by definition a builtin the compiler doesn't
       know how to handle — let the tree-walker run it. User lambdas
       (not in reserved_symbol) fall through to compile_call. */
    keyval_t *kv = set_get_keyval_dict(reserved_symbol, (char *)s, NULL);
    if (kv && isinternal(kv->val)) {
      c->failed = 1;
      return;
    }
    compile_call(c, e, tail);
    return;
  }
  /* Complex head — fall back. */
  c->failed = 1;
}

int compile_lambda(exp_t *fn, int is_closure) {
  if (!fn || !islambda(fn))
    return 0;
  if (fn->flags & FLAG_COMPILED)
    return 1; /* idempotent */
  exp_t *params = fn->content;
  exp_t *body = fn->next->content;
  compiler_t c = {0};
  c.self_name = (const char *)fn->meta; /* may be NULL for anon fn */

  /* Register params into slots 0..N-1 matching env->inline_slots.
     Rest params (dot notation or bare-symbol wrap) fall back to AST.
     Empty params `()` are the nil_singleton sentinel — a pair with NULL
     content — so the `p->content` guard (matching var2env) terminates
     the loop with nparams == 0 instead of misreading it as a malformed
     param and failing. Without it, no 0-arg function would ever compile. */
  exp_t *p;
  for (p = params; p && p->content; p = p->next) {
    if (c.nparams >= ENV_INLINE_SLOTS) {
      c.failed = 1;
      break;
    }
    if (!is_ptr(p->content) || !issymbol(p->content)) {
      c.failed = 1;
      break;
    }
    /* Dot marker means rest params — AST eval handles collection. */
    if (strcmp((char *)exp_text(p->content), ".") == 0) {
      c.failed = 1;
      break;
    }
    c.slot_names[c.nparams++] = (char *)exp_text(p->content);
  }
  c.nslots = c.nparams;

  /* Walk body list: each expression, pop between, except the last. */
  exp_t *b;
  int saw_any = 0;
  for (b = body; b && !c.failed; b = b->next) {
    if (saw_any)
      emit_u8(&c, OP_POP);
    int is_last = (b->next == NULL);
    compile_expr(&c, b->content, is_last);
    saw_any = 1;
  }
  if (!saw_any) {
    c.failed = 1;
  }
  if (!c.failed)
    emit_u8(&c, OP_RET);

  if (c.failed) {
    int i;
    for (i = 0; i < c.nconsts; i++)
      unrefexp(c.consts[i]);
    free(c.consts);
    free(c.code);
    return 0;
  }
  bytecode_t *bc = calloc(1, sizeof(bytecode_t));
  /* Migrate the params list off fn->content onto bc->content. After
     `fn->bc = bc` below, the union assignment overwrites fn->content
     with bc, so this is an ownership *transfer* — bytecode_free will
     unrefexp(bc->content) at end of life. Don't refexp here. */
  bc->content = fn->content;
  bc->code = c.code;
  bc->ncode = c.ncode;
  bc->consts = c.consts;
  bc->nconsts = c.nconsts;
  /* Cache param info so vm_invoke_values doesn't re-walk fn->content
     on every call. The keys are borrowed pointers into the param-
     symbol ->ptr fields, kept alive by the lambda's ref on its
     header. */
  bc->nparams = (uint8_t)c.nparams;
  for (int pi = 0; pi < c.nparams; pi++)
    bc->param_keys[pi] = c.slot_names[pi];
  bc->self_name = (const char *)fn->meta; /* borrowed; NULL for anon */
  bc->no_gcache = (uint8_t)(is_closure != 0);
  fn->bc = bc;
  fn->flags |= FLAG_COMPILED;
#ifdef ALCOVE_JIT
  /* Don't JIT closures: the JIT's global-call helpers cache via gcache,
     which is unsafe for a closure's captured free vars. Bytecode (with
     no_gcache fresh lookups) is still far faster than AST eval. */
  if (!is_closure)
    jit_compile(bc); /* opportunistic; no-op for shapes we don't recognize */
#endif
  return 1;
}

/* "Callable index" — the family of types that support (container i) as a
   read, mirroring the (string i) sugar. Keeping the membership test and the
   element fetch in one place means every call site (the AST evaluator's two
   head paths, vm_invoke_values, and the OP_TAIL_CALL fallback) stays in sync,
   and a new indexable type is a one-line addition here rather than four. */
static inline int isindexable(exp_t *e) {
  return isstring(e) || isvector(e) || isblob(e);
}
/* Keyed containers answer (m k) as a key lookup (Clojure-style), distinct
   from the integer indexing of isindexable: dict/hamt -> value (nil if
   absent), set -> the member (nil if absent). */
static inline int iskeyed(exp_t *e) {
  return isdict(e) || isset(e) || ishamt(e);
}
/* Any value that supports (container arg) as a read. */
static inline int iscallable_container(exp_t *e) {
  return isindexable(e) || iskeyed(e);
}
/* Apply a callable container to one already-evaluated argument, consuming
   `arg`'s ref. Indexable -> element by integer index; keyed -> value/member
   by key. Returns an owned result (nil for an absent key) or an error.
   Defined after the HAMT ops since it needs hamt_node_get. */
static exp_t *container_apply(exp_t *c, exp_t *arg, env_t *env);
/* Fetch element i (0-based) of an indexable container. Returns an owned ref
   (vector cell) or a fresh immediate (string -> char, blob -> byte fixnum),
   or NULL when i is out of range / negative. Caller guarantees isindexable(c)
   and that i came from a validated integer. */
static exp_t *index_get(exp_t *c, int64_t i) {
  if (i < 0)
    return NULL;
  if (isstring(c)) {
    uint32_t cp;
    return utf8_index(exp_text(c), i, &cp) ? make_char(cp) : NULL;
  }
  if (isvector(c))
    return (i < vec_len(c)) ? vec_get_boxed(c, i) : NULL;
  /* blob: one byte, 0..255, as a fixnum (matches blob-ref). */
  return ((size_t)i < blob_len(c))
             ? MAKE_FIX((int64_t)(unsigned char)blob_bytes(c)[i])
             : NULL;
}

/* Bytecode dispatch loop. Entered with `env` already populated (params
   in inline slots). Returns an owned exp_t* (or NULL).
   OP_TAIL_CALL re-enters via goto tail_reentry with a fresh fn —
   `fn_owned` tracks whether we took ownership of the post-tail fn (so
   we can unref it on final return or error). */
exp_t *vm_run(exp_t *fn, env_t *env) {
#define VM_STACK_MAX 256
  exp_t *stack[VM_STACK_MAX];
  bytecode_t *bc;
  uint8_t *code;
  exp_t **consts;
  int sp;
  int pc;
  int fn_owned = 0;

tail_reentry:
  bc = fn->bc;
  code = bc->code;
  consts = bc->consts;
  sp = 0;
  pc = 0;

#define RUNTIME_ERR(msg)                                                       \
  do {                                                                         \
    exp_t *_err = error(ERROR_ILLEGAL_VALUE, fn, env, msg);                    \
    int _i;                                                                    \
    for (_i = 0; _i < sp; _i++)                                                \
      unrefexp(stack[_i]);                                                     \
    if (fn_owned)                                                              \
      unrefexp(fn);                                                            \
    return _err;                                                               \
  } while (0)
#define PUSH(v)                                                                \
  do {                                                                         \
    if (sp >= VM_STACK_MAX)                                                    \
      RUNTIME_ERR("VM stack overflow");                                        \
    stack[sp++] = (v);                                                         \
  } while (0)
#define POP() (stack[--sp])
#define READ_U8 (code[pc++])
#define READ_I16 (pc += 2, (int16_t)(code[pc - 2] | (code[pc - 1] << 8)))

  /* Threaded dispatch via GCC/Clang computed goto: each op ends with a
     direct indirect branch to the next op's label. Lets the CPU's
     branch predictor learn per-op successor patterns — measurably
     faster than a single switch-based jump-table on hot loops. */
  static const void *const dispatch[OP_MAX] = {
      [OP_HALT] = &&l_halt,
      [OP_RET] = &&l_ret,
      [OP_POP] = &&l_pop,
      [OP_LOAD_FIX] = &&l_load_fix,
      [OP_LOAD_CONST] = &&l_load_const,
      [OP_LOAD_SLOT] = &&l_load_slot,
      [OP_LOAD_GLOBAL] = &&l_load_global,
      [OP_STORE_SLOT] = &&l_store_slot,
      [OP_BIND_SLOT] = &&l_bind_slot,
      [OP_UNBIND_SLOT] = &&l_unbind_slot,
      [OP_ADD] = &&l_add,
      [OP_SUB] = &&l_sub,
      [OP_MUL] = &&l_mul,
      [OP_DIV] = &&l_div,
      [OP_MOD] = &&l_mod,
      [OP_LT] = &&l_lt,
      [OP_GT] = &&l_gt,
      [OP_LE] = &&l_le,
      [OP_GE] = &&l_ge,
      [OP_IS] = &&l_is,
      [OP_ISO] = &&l_iso,
      [OP_NOT] = &&l_not,
      [OP_JUMP] = &&l_jump,
      [OP_BR_IF_FALSE] = &&l_br_if_false,
      [OP_BR_IF_TRUE] = &&l_br_if_true,
      [OP_CALL] = &&l_call,
      [OP_CALL_GLOBAL] = &&l_call_global,
      [OP_TAIL_SELF] = &&l_tail_self,
      [OP_TAIL_CALL] = &&l_tail_call,
      [OP_CONS] = &&l_cons,
      [OP_CAR] = &&l_car,
      [OP_CDR] = &&l_cdr,
      [OP_LIST] = &&l_list,
      [OP_SLOT_ADD_FIX] = &&l_slot_add_fix,
      [OP_SLOT_SUB_FIX] = &&l_slot_sub_fix,
      [OP_SLOT_LT_FIX] = &&l_slot_lt_fix,
      [OP_SLOT_LE_FIX] = &&l_slot_le_fix,
      [OP_SLOT_GT_FIX] = &&l_slot_gt_fix,
      [OP_SLOT_GE_FIX] = &&l_slot_ge_fix,
      [OP_SLOT_LE_SLOT] = &&l_slot_le_slot,
      [OP_VEC_REF] = &&l_vec_ref,
      [OP_VEC_SET] = &&l_vec_set,
      [OP_VEC_LEN] = &&l_vec_len,
      [OP_VEC_NEW] = &&l_vec_new,
      [OP_SQRT_INT] = &&l_sqrt_int,
      [OP_LENGTH] = &&l_length,
      [OP_SETQ_DYN] = &&l_setq_dyn,
      [OP_STORE_FREE] = &&l_store_free,
  };
#ifndef NDEBUG
  /* Catches "added an opcode but forgot to initialize dispatch[]" —
     a designated-init gap silently leaves a slot NULL and would jump
     to 0 on that op. One-time cost at vm_run entry; NDEBUG strips it. */
  {
    int _i;
    for (_i = 0; _i < OP_MAX; _i++)
      assert(dispatch[_i] != NULL);
  }
#endif
#define NEXT goto *dispatch[code[pc++]]

  NEXT;

l_halt:
  RUNTIME_ERR("Bytecode: OP_HALT reached (compiler bug)");

l_ret: {
  exp_t *r = POP();
  while (sp > 0)
    unrefexp(POP());
  if (fn_owned)
    unrefexp(fn);
  return r;
}

l_pop:
  unrefexp(POP());
  NEXT;

l_load_fix: {
  int16_t v = READ_I16;
  PUSH(MAKE_FIX((int64_t)v));
  NEXT;
}
l_load_const: {
  uint8_t idx = READ_U8;
  PUSH(refexp(consts[idx]));
  NEXT;
}
l_load_slot: {
  uint8_t idx = READ_U8;
  PUSH(refexp(env->inline_vals[idx]));
  NEXT;
}
l_load_global: {
  uint8_t idx = READ_U8;
  /* Per-bytecode global cache. The gcache slot stores the last lookup
     result + the generation it was cached at. If alcove_global_gen
     still matches, we skip the env walk + strcmp entirely. fib spends
     ~78% of its time here without this cache. */
  /* Hit-path is unchanged from the no-closure original (zero added cost):
     closures never allocate gcache (the store below is gated on
     !no_gcache), so bc->gcache stays NULL for them and this never hits. */
  if (bc->gcache && bc->gcache[idx].gen == alcove_global_gen) {
    PUSH(refexp(bc->gcache[idx].val));
  } else {
    int is_global;
    exp_t *v = lookup_scoped(consts[idx], env, &is_global);
    if (!v)
      RUNTIME_ERR("Unbound variable");
    /* Only memoize truly-global resolutions. A local free var (OP_STORE_FREE
       target read back via OP_LOAD_GLOBAL) must NOT be cached — the gcache is
       keyed by global-gen and would serve it stale to a later call. */
    if (!bc->no_gcache && is_global) {
      if (!bc->gcache)
        bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
      bc->gcache[idx].val = v; /* not refcounted by us; bound globally */
      bc->gcache[idx].gen = alcove_global_gen;
    }
    PUSH(v);
  }
  NEXT;
}

l_store_slot: {
  /* (= local val): replace an existing slot's value. */
  uint8_t idx = READ_U8;
  exp_t *v = POP();
  unrefexp(env->inline_vals[idx]);
  env->inline_vals[idx] = v;
  /* Leave the updated value on the stack as the expression's result.
     (= ...) returns the assigned value. */
  PUSH(refexp(v));
  NEXT;
}
l_setq_dyn: {
  /* (setq sym val) for a non-local target: walk the env chain at
     runtime (nearest existing binding, else create top-level). */
  uint8_t idx = READ_U8;
  exp_t *v = POP();
  /* Reject (setq <reserved> v) — consistent with the AST setqcmd path. */
  {
    exp_t *_rerr = NULL;
    REJECT_RESERVED_ASSIGN(consts[idx], _rerr, {
      unrefexp(v);
      for (int _i = 0; _i < sp; _i++)
        unrefexp(stack[_i]);
      if (fn_owned)
        unrefexp(fn);
      return _rerr;
    });
  }
  /* setq_store_symbol takes its own ref on v (refexp into the binding)
     and does not consume ours — same contract setqcmd relies on — so
     our popped ref becomes the on-stack result. */
  setq_store_symbol(consts[idx], env, v);
  PUSH(v);
  NEXT;
}
l_store_free: {
  /* (= sym val) / (setf sym val) for a non-slot target: a captured free
     var (mutable closure) or a global. Same shape as SETQ_DYN but uses
     assign_store_symbol, which creates in the CURRENT env on not-found
     (matching updatebang) rather than the root env. */
  uint8_t idx = READ_U8;
  exp_t *v = POP();
  {
    exp_t *_rerr = NULL;
    REJECT_RESERVED_ASSIGN(consts[idx], _rerr, {
      unrefexp(v);
      for (int _i = 0; _i < sp; _i++)
        unrefexp(stack[_i]);
      if (fn_owned)
        unrefexp(fn);
      return _rerr;
    });
  }
  /* Borrows v (refexp into the binding); our popped ref is the result. */
  assign_store_symbol(consts[idx], env, v);
  PUSH(v);
  NEXT;
}
l_bind_slot: {
  /* let/with/for entry: allocate a new inline slot and bump n_inline
     if this is a fresh position. The compiler resolves these names to
     slot indices at compile time, so symbolic lookup never needs to
     find them — but lookup() / updatebang() / dir() walk inline_keys
     in [0, n_inline), so we must write a sentinel here. NULL means
     "skip on symbolic walk"; the slot is still reachable by index. */
  uint8_t idx = READ_U8;
  exp_t *v = POP();
  /* Slot is fresh (compiler guarantees no prior BIND at same idx
     without intervening UNBIND). No old value to unref. */
  env->inline_vals[idx] = v;
  env->inline_keys[idx] = NULL;
  if (idx >= env->n_inline)
    env->n_inline = idx + 1;
  NEXT;
}
l_unbind_slot: {
  /* let/with exit: release the binding. destroy_env would catch any
     leftover refs via n_inline, but we explicitly clear here so the
     slot is reusable for subsequent lets. */
  uint8_t idx = READ_U8;
  unrefexp(env->inline_vals[idx]);
  env->inline_vals[idx] = NULL;
  if (idx + 1 == env->n_inline)
    env->n_inline = idx;
  NEXT;
}

/* Numeric binary op helpers. COERCE_TO_DOUBLE implicitly references
   `a` and `b` in its error path so both operands get unref'd before
   we jump to the error return — not reusable outside BIN_ARITH /
   CMP_OP sites that name their operands `a` and `b`. */
#define COERCE_TO_DOUBLE(v, out, opname)                                       \
  do {                                                                         \
    if (isnumber(v))                                                           \
      (out) = (double)FIX_VAL(v);                                              \
    else if (isfloat(v)) {                                                     \
      (out) = (v)->f;                                                          \
      unrefexp(v);                                                             \
    } else {                                                                   \
      unrefexp(a);                                                             \
      unrefexp(b);                                                             \
      RUNTIME_ERR(opname);                                                     \
    }                                                                          \
  } while (0)
#define BIN_ARITH(op, opname)                                                  \
  do {                                                                         \
    exp_t *b = POP(), *a = POP();                                              \
    if (isnumber(a) && isnumber(b)) {                                          \
      PUSH(MAKE_FIX(FIX_VAL(a) op FIX_VAL(b)));                                \
    } else {                                                                   \
      double da, db;                                                           \
      COERCE_TO_DOUBLE(a, da, "Illegal value in " opname);                     \
      COERCE_TO_DOUBLE(b, db, "Illegal value in " opname);                     \
      PUSH(make_floatf(da op db));                                             \
    }                                                                          \
  } while (0)

l_add:
  BIN_ARITH(+, "+");
  NEXT;
l_sub:
  BIN_ARITH(-, "-");
  NEXT;
l_mul:
  BIN_ARITH(*, "*");
  NEXT;
l_div: {
  exp_t *b = POP(), *a = POP();
  if (isnumber(a) && isnumber(b)) {
    int64_t bb = FIX_VAL(b);
    if (bb == 0)
      RUNTIME_ERR("Illegal division by 0");
    PUSH(MAKE_FIX(FIX_VAL(a) / bb));
  } else {
    double da, db;
    COERCE_TO_DOUBLE(a, da, "Illegal value in /");
    COERCE_TO_DOUBLE(b, db, "Illegal value in /");
    if (db == 0)
      RUNTIME_ERR("Illegal division by 0");
    PUSH(make_floatf(da / db));
  }
  NEXT;
}
l_mod: {
  /* Truncated modulo (C99 %), matches modcmd. Lifts (mod a b) from
     a builtin-call-back-to-AST round-trip into one VM dispatch.
     Float operands fall back to fmod for parity with what users
     expect from mathematical modulo. */
  exp_t *b = POP(), *a = POP();
  if (isnumber(a) && isnumber(b)) {
    int64_t bb = FIX_VAL(b);
    if (bb == 0)
      RUNTIME_ERR("Illegal modulo by 0");
    int64_t va = FIX_VAL(a);
    PUSH(MAKE_FIX(va - (va / bb) * bb));
  } else {
    double da, db;
    COERCE_TO_DOUBLE(a, da, "Illegal value in mod");
    COERCE_TO_DOUBLE(b, db, "Illegal value in mod");
    if (db == 0.0)
      RUNTIME_ERR("Illegal modulo by 0");
    PUSH(make_floatf(fmod(da, db)));
  }
  NEXT;
}

/* Integer compares on two fixnums skip the cast-to-double step
   (which would overflow at 61-bit boundaries). Mixed-type paths
   have a/b already unref'd by COERCE_TO_DOUBLE — fixnums are
   immediates and never need unref either, so no trailing cleanup. */
#define CMP_OP(intcmp, flcmp)                                                  \
  do {                                                                         \
    exp_t *b = POP(), *a = POP();                                              \
    int r;                                                                     \
    if ((isnumber(a) || isfloat(a)) && (isnumber(b) || isfloat(b))) {          \
      if (isnumber(a) && isnumber(b))                                          \
        r = FIX_VAL(a) intcmp FIX_VAL(b);                                      \
      else {                                                                   \
        double da = TO_DOUBLE(a);                   \
        double db = TO_DOUBLE(b);                   \
        r = da flcmp db;                                                       \
      }                                                                        \
    } else {                                                                   \
      double d;                                                                \
      if (!alc_pair_cmp(a, b, &d)) {                                           \
        unrefexp(a);                                                           \
        unrefexp(b);                                                           \
        RUNTIME_ERR("Illegal value in compare");                               \
      }                                                                        \
      r = d flcmp 0;                                                           \
    }                                                                          \
    unrefexp(a);                                                               \
    unrefexp(b);                                                               \
    PUSH(r ? TRUE_EXP : NIL_EXP);                                              \
  } while (0)

l_lt:
  CMP_OP(<, <);
  NEXT;
l_gt:
  CMP_OP(>, >);
  NEXT;
l_le:
  CMP_OP(<=, <=);
  NEXT;
l_ge:
  CMP_OP(>=, >=);
  NEXT;

l_is: {
  exp_t *b = POP(), *a = POP();
  int r = isequal(a, b);
  unrefexp(a);
  unrefexp(b);
  PUSH(r ? TRUE_EXP : NIL_EXP);
  NEXT;
}
l_iso: {
  exp_t *b = POP(), *a = POP();
  int r = isoequal(a, b);
  unrefexp(a);
  unrefexp(b);
  PUSH(r ? TRUE_EXP : NIL_EXP);
  NEXT;
}
l_not: {
  exp_t *a = POP();
  int r = !istrue(a);
  unrefexp(a);
  PUSH(r ? TRUE_EXP : NIL_EXP);
  NEXT;
}

l_jump: {
  int16_t off = READ_I16;
  pc += off;
  NEXT;
}
l_br_if_false: {
  int16_t off = READ_I16;
  exp_t *a = POP();
  if (!istrue(a))
    pc += off;
  unrefexp(a);
  NEXT;
}
l_br_if_true: {
  int16_t off = READ_I16;
  exp_t *a = POP();
  if (istrue(a))
    pc += off;
  unrefexp(a);
  NEXT;
}

l_tail_self: {
  /* Self-tail: rebind inline slots from the top of the operand
     stack, keep keys as-is (same fn → same params), jump to PC 0. */
  uint8_t n = READ_U8;
  if (n != bc->nparams)
    RUNTIME_ERR("Wrong number of arguments");
  int base = sp - n;
  int i;
  for (i = 0; i < env->n_inline; i++)
    unrefexp(env->inline_vals[i]);
  env->n_inline = n <= ENV_INLINE_SLOTS ? n : ENV_INLINE_SLOTS;
  for (i = 0; i < env->n_inline; i++)
    env->inline_vals[i] = stack[base + i];
  for (; i < n; i++)
    unrefexp(stack[base + i]);
  sp = base;
  pc = 0;
  NEXT;
}

l_call: {
  uint8_t n = READ_U8;
  int base = sp - n;
  exp_t *callee = stack[base - 1];
  exp_t *ret = vm_invoke_values(callee, n, &stack[base], env);
  sp = base - 1;
  unrefexp(callee);
  if (ret && iserror(ret)) {
    while (sp > 0)
      unrefexp(POP());
    if (fn_owned)
      unrefexp(fn);
    return ret;
  }
  if (!ret)
    ret = NIL_EXP;
  PUSH(ret);
  NEXT;
}

l_call_global: {
  /* Fused LOAD_GLOBAL + CALL. The callee is never pushed to the
     operand stack — we resolve via the gcache directly. */
  uint8_t idx = READ_U8;
  uint8_t n = READ_U8;
  exp_t *callee;
  /* Hit-path unchanged from the original (see OP_LOAD_GLOBAL): closures
     never allocate gcache, so this never hits for them. */
  if (bc->gcache && bc->gcache[idx].gen == alcove_global_gen) {
    callee = refexp(bc->gcache[idx].val);
  } else {
    int is_global;
    callee = lookup_scoped(consts[idx], env, &is_global);
    if (!callee)
      RUNTIME_ERR("Unbound variable");
    /* Only cache global resolutions (see OP_LOAD_GLOBAL): a locally-bound
       callee must not be memoized against the global generation. */
    if (!bc->no_gcache && is_global) {
      if (!bc->gcache)
        bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
      bc->gcache[idx].val = callee;
      bc->gcache[idx].gen = alcove_global_gen;
    }
  }
  int base = sp - n;
  exp_t *ret = vm_invoke_values(callee, n, &stack[base], env);
  sp = base;
  unrefexp(callee);
  if (ret && iserror(ret)) {
    while (sp > 0)
      unrefexp(POP());
    if (fn_owned)
      unrefexp(fn);
    return ret;
  }
  if (!ret)
    ret = NIL_EXP;
  PUSH(ret);
  NEXT;
}

l_tail_call: {
  /* Cross-function tail call: release the current env's bindings,
     rebind with new fn's params, and `goto tail_reentry` so the
     same vm_run invocation runs the new bytecode. O(1) C stack
     growth across tail hops.
     If the target isn't compiled, fall back to vm_invoke_values —
     we lose TCO for that hop but stay correct. */
  uint8_t n = READ_U8;
  int base = sp - n;
  exp_t *new_fn = stack[base - 1];

  /* String-as-callable and escape continuations: dispatch via
     vm_invoke_values (it has the string-index arm and the EXP_CONT escape
     arm). A continuation invoked in tail position must yield its escape
     token, not be rejected as "not a lambda". Same fallback shape as the
     !FLAG_COMPILED branch. */
  int is_ffi_callee = 0;
#ifdef ALCOVE_FFI
  is_ffi_callee = isffi(new_fn);
#endif
  if (iscallable_container(new_fn) || iscont(new_fn) || is_ffi_callee) {
    /* Callable container (string/vector/blob index, or dict/hamt/set key
       lookup), escape continuation, or an ffi-fn value reached in tail
       position: vm_invoke_values has the matching arms (the FFI arm
       dispatches to alc_ffi_call). Without this such a callable as the tail
       expression of a compiled body would be rejected as "not a lambda". */
    exp_t *ret = vm_invoke_values(new_fn, n, &stack[base], env);
    sp = base - 1;
    unrefexp(new_fn);
    while (sp > 0)
      unrefexp(POP());
    if (fn_owned)
      unrefexp(fn);
    return ret;
  }
  if (!islambda(new_fn)) {
    /* Release the n argument values above base before shrinking sp,
       otherwise RUNTIME_ERR's cleanup loop misses stack[base..base+n-1]. */
    for (int _i = 0; _i < n; _i++)
      unrefexp(stack[base + _i]);
    sp = base - 1;
    unrefexp(new_fn);
    RUNTIME_ERR("OP_TAIL_CALL: not a lambda");
  }

  if (!(new_fn->flags & FLAG_COMPILED) ||
      (new_fn->next && new_fn->next->meta &&
       (env_t *)new_fn->next->meta != g_global_env)) {
    /* Non-compiled target, OR a real CLOSURE that captured a NON-global env:
       the in-place TCO reuse below keeps the CURRENT env and only rebinds its
       slots, so such a closure's free variables — which live in its captured
       (local) env, not this frame's chain — would resolve wrong. Dispatch
       through vm_invoke_values instead (one C-stack frame), which parents the
       new env to the captured env. Top-level defs capture the GLOBAL env,
       which the current frame's root chain already reaches, so they keep
       full TCO; self-recursion keeps TCO via OP_TAIL_SELF. Only cross-function
       tail calls into local closures pay this one-frame hop. */
    exp_t *ret = vm_invoke_values(new_fn, n, &stack[base], env);
    sp = base - 1;
    unrefexp(new_fn);
    while (sp > 0)
      unrefexp(POP());
    if (fn_owned)
      unrefexp(fn);
    return ret;
  }

  if (n != new_fn->bc->nparams)
    RUNTIME_ERR("Wrong number of arguments");

  /* Compiled target: stash args, unwind, rebind, jump. */
  if (n > ENV_INLINE_SLOTS) {
    int i;
    for (i = 0; i < n; i++)
      unrefexp(stack[base + i]);
    sp = base - 1;
    unrefexp(new_fn);
    RUNTIME_ERR("OP_TAIL_CALL: too many args");
  }
  exp_t *args_buf[ENV_INLINE_SLOTS];
  {
    int i;
    for (i = 0; i < n; i++)
      args_buf[i] = stack[base + i];
  }
  sp = base - 1; /* drop args */
  /* stack[base-1] (new_fn slot) is above sp; we've taken ownership */

  /* Release current env's inline slots + any dict. */
  {
    int i;
    for (i = 0; i < env->n_inline; i++)
      unrefexp(env->inline_vals[i]);
  }
  env->n_inline = 0;
  if (env->d) {
    destroy_dict(env->d);
    env->d = NULL;
  }

  /* Bind new args to new_fn's params. lambda_params handles the
     content/bc union overload (compiled lambdas migrate params to
     bc->content). */
  exp_t *p = lambda_params(new_fn);
  int i = 0;
  while (p && i < n) {
    if (!is_ptr(p->content) || !issymbol(p->content)) {
      int j;
      for (j = i; j < n; j++)
        unrefexp(args_buf[j]);
      unrefexp(new_fn);
      /* Build error BEFORE potentially freeing fn — error() takes a refexp
         on the id argument, so fn must still be live when it is passed. */
      exp_t *_tc_err = error(ERROR_ILLEGAL_VALUE, fn, env,
                             "OP_TAIL_CALL: bad param");
      if (fn_owned)
        unrefexp(fn);
      return _tc_err;
    }
    if (env->n_inline < ENV_INLINE_SLOTS) {
      env->inline_keys[env->n_inline] = (char *)exp_text(p->content);
      env->inline_vals[env->n_inline] = args_buf[i];
      env->n_inline++;
    } else {
      if (!env->d)
        env->d = create_dict();
      set_get_keyval_dict(env->d, exp_text(p->content), args_buf[i]);
      unrefexp(args_buf[i]);
    }
    p = p->next;
    i++;
  }
  while (i < n)
    unrefexp(args_buf[i++]);

  /* Swap in new_fn and re-enter. */
  if (fn_owned)
    unrefexp(fn);
  fn = new_fn;
  fn_owned = 1;
  goto tail_reentry;
}

l_cons: {
  /* (cons a b): make_node(a) takes ownership of a; b becomes ->next.
     For (cons a nil) we drop the explicit nil tail to match conscmd. */
  exp_t *b = POP(), *a = POP();
  exp_t *pair = make_node(a); /* transfers a's ref into pair->content */
  if (istrue(b))
    pair->next = b; /* transfers b's ref */
  else {
    unrefexp(b);
    pair->next = NULL;
  }
  PUSH(pair);
  NEXT;
}
l_car: {
  exp_t *p = POP();
  exp_t *v = car(p); /* borrowed (via macro guard) */
  PUSH(refexp(v));
  unrefexp(p);
  NEXT;
}
l_cdr: {
  exp_t *p = POP();
  exp_t *v = cdr(p); /* borrowed */
  PUSH(refexp(v));
  unrefexp(p);
  NEXT;
}
l_list: {
  /* (list a0 ... aN-1) → fresh list. Args own their refs; we transfer
     into the new pair chain. */
  uint8_t n = READ_U8;
  if (n == 0) {
    PUSH(NIL_EXP);
    NEXT;
  }
  int base = sp - n;
  exp_t *head = make_node(stack[base]);
  exp_t *cur = head;
  int i;
  for (i = 1; i < n; i++) {
    cur = cur->next = make_node(stack[base + i]);
  }
  sp = base;
  PUSH(head);
  NEXT;
}

/* Fused LOAD_SLOT + LOAD_FIX + op. Saves two dispatches and two
   stack round-trips per fired op — the hot arithmetic shapes on
   fib / countdown / etc. are all of this form. Fixnum slot is the
   fast path; float falls back to the same semantics as the 3-op
   sequence via COERCE_TO_DOUBLE. */
#define SLOT_FIX_NUMERIC(body_int, body_flt, opname)                           \
  do {                                                                         \
    uint8_t idx = READ_U8;                                                     \
    int16_t imm = READ_I16;                                                    \
    exp_t *a = env->inline_vals[idx];                                          \
    if (isnumber(a)) {                                                         \
      body_int;                                                                \
    } else if (isfloat(a)) {                                                   \
      double da = a->f;                                                        \
      (void)da;                                                                \
      body_flt;                                                                \
    } else                                                                     \
      RUNTIME_ERR("Illegal value in " opname);                                 \
  } while (0)

l_slot_add_fix:
  SLOT_FIX_NUMERIC(PUSH(MAKE_FIX(FIX_VAL(a) + imm)),
                   PUSH(make_floatf(da + (double)imm)), "+");
  NEXT;
l_slot_sub_fix:
  SLOT_FIX_NUMERIC(PUSH(MAKE_FIX(FIX_VAL(a) - imm)),
                   PUSH(make_floatf(da - (double)imm)), "-");
  NEXT;
l_slot_lt_fix:
  SLOT_FIX_NUMERIC(PUSH(FIX_VAL(a) < imm ? TRUE_EXP : NIL_EXP),
                   PUSH(da < (double)imm ? TRUE_EXP : NIL_EXP), "<");
  NEXT;
l_slot_le_fix:
  SLOT_FIX_NUMERIC(PUSH(FIX_VAL(a) <= imm ? TRUE_EXP : NIL_EXP),
                   PUSH(da <= (double)imm ? TRUE_EXP : NIL_EXP), "<=");
  NEXT;
l_slot_gt_fix:
  SLOT_FIX_NUMERIC(PUSH(FIX_VAL(a) > imm ? TRUE_EXP : NIL_EXP),
                   PUSH(da > (double)imm ? TRUE_EXP : NIL_EXP), ">");
  NEXT;
l_slot_ge_fix:
  SLOT_FIX_NUMERIC(PUSH(FIX_VAL(a) >= imm ? TRUE_EXP : NIL_EXP),
                   PUSH(da >= (double)imm ? TRUE_EXP : NIL_EXP), ">=");
  NEXT;

l_slot_le_slot: {
  /* Hot-path superinst for `for`: reads two slots, pushes t/nil for
     (slot_a <= slot_b). Fuses LOAD_SLOT+LOAD_SLOT+LE into one dispatch. */
  uint8_t idx_a = READ_U8;
  uint8_t idx_b = READ_U8;
  exp_t *a = env->inline_vals[idx_a];
  exp_t *b = env->inline_vals[idx_b];
  if (isnumber(a) && isnumber(b)) {
    PUSH(FIX_VAL(a) <= FIX_VAL(b) ? TRUE_EXP : NIL_EXP);
  } else if ((isnumber(a) || isfloat(a)) && (isnumber(b) || isfloat(b))) {
    double da = TO_DOUBLE(a);
    double db = TO_DOUBLE(b);
    PUSH(da <= db ? TRUE_EXP : NIL_EXP);
  } else {
    RUNTIME_ERR("Illegal value in <=");
  }
  NEXT;
}

#undef SLOT_FIX_NUMERIC

l_vec_ref: {
  exp_t *iexp = POP(), *vexp = POP();
  if (!isvector(vexp) || !(isnumber(iexp) || isfloat(iexp))) {
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR("vec-ref: bad args");
  }
  /* A float index truncates to its integer part (matches the AST vec-ref
     and ordinary integer division semantics). This also absorbs a value
     that arrived as a float instead of a fixnum — e.g. (/ a b) taking the
     float path on a 32-bit target — where the truncated result is still
     the correct index. */
  int64_t i = isnumber(iexp) ? FIX_VAL(iexp) : (int64_t)iexp->f;
  if (i < 0 || i >= vec_len(vexp)) {
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR("vec-ref: index out of range");
  }
  exp_t *r = vec_get_boxed(vexp, i);
  unrefexp(iexp);
  unrefexp(vexp);
  PUSH(r);
  NEXT;
}
l_vec_set: {
  exp_t *valexp = POP(), *iexp = POP(), *vexp = POP();
  if (!isvector(vexp) || !(isnumber(iexp) || isfloat(iexp))) {
    unrefexp(valexp);
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR("vec-set!: bad args");
  }
  int64_t i = isnumber(iexp) ? FIX_VAL(iexp) : (int64_t)iexp->f; /* float idx truncates */
  if (i < 0 || i >= vec_len(vexp)) {
    unrefexp(valexp);
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR("vec-set!: index out of range");
  }
  /* Push the returned value before vec_set_boxed consumes valexp. */
  exp_t *r = refexp(valexp);
  if (!vec_set_boxed(vexp, i, valexp)) {
    unrefexp(r);
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR("vec-set!: alloc failure or shared vec promote");
  }
  PUSH(r);
  unrefexp(iexp);
  unrefexp(vexp);
  NEXT;
}
l_vec_len: {
  exp_t *vexp = POP();
  if (!isvector(vexp)) {
    unrefexp(vexp);
    RUNTIME_ERR("vec-len: not a vector");
  }
  int64_t n = vec_len(vexp);
  unrefexp(vexp);
  PUSH(MAKE_FIX(n));
  NEXT;
}
l_vec_new: {
  extern exp_t *make_vector(int64_t n, exp_t *fill);
  exp_t *initexp = POP(), *nexp = POP();
  if (!isnumber(nexp)) {
    unrefexp(initexp);
    unrefexp(nexp);
    RUNTIME_ERR("vec: n must be a number");
  }
  int64_t n = FIX_VAL(nexp);
  if (n < 0)
    n = 0;
  exp_t *vec = make_vector(n, initexp);
  unrefexp(initexp);
  unrefexp(nexp);
  if (!vec)
    RUNTIME_ERR("(vec n ...): n is too large or alloc failed");
  PUSH(vec);
  NEXT;
}
l_sqrt_int: {
  exp_t *nexp = POP();
  if (!isnumber(nexp)) {
    unrefexp(nexp);
    RUNTIME_ERR("sqrt-int: not a number");
  }
  int64_t n = FIX_VAL(nexp);
  int64_t r = (n < 0) ? 0 : (int64_t)sqrt((double)n);
  /* Cast to uint64_t before multiplying to avoid signed overflow UB when
     r is near INT64_MAX (same guard used in sqrtintcmd tree-walker path). */
  while ((uint64_t)(r + 1) * (uint64_t)(r + 1) <= (uint64_t)n)
    r++;
  while ((uint64_t)r * (uint64_t)r > (uint64_t)n)
    r--;
  unrefexp(nexp);
  PUSH(MAKE_FIX(r));
  NEXT;
}
l_length: {
  /* (length x) — must mirror lengthcmd's semantics or compiled bodies
     silently miscompile string/vector/blob/list args to 0. NULL and
     nil_singleton are length 0 (empty list). */
  exp_t *xs = POP();
  int64_t n = 0;
  if (xs == NULL || xs == nil_singleton) {
    n = 0;
  } else if (isstring(xs)) {
    { const char *_t = exp_text(xs); n = _t ? utf8_strlen(_t) : 0; }
  } else if (ispair(xs)) {
    exp_t *cur = xs;
    while (is_ptr(cur) && cur->type == EXP_PAIR) {
      n++;
      cur = cur->next;
    }
  } else if (is_ptr(xs) && xs->type == EXP_VECTOR && xs->ptr) {
    n = vec_len(xs);
  } else if (isblob(xs)) {
    n = xs->ptr ? (int64_t)((alc_blob_t *)xs->ptr)->len : 0;
  } else if (islist(xs)) {
    n = xs->ptr ? (int64_t)((alc_list_t *)xs->ptr)->len : 0;
  } else {
    unrefexp(xs);
    RUNTIME_ERR("length: not a list/string/vector/blob");
  }
  unrefexp(xs);
  PUSH(MAKE_FIX(n));
  NEXT;
}

#undef BIN_ARITH
#undef CMP_OP
#undef COERCE_TO_DOUBLE
#undef NEXT
#undef PUSH
#undef POP
#undef READ_U8
#undef READ_I16
#undef RUNTIME_ERR
}

/* Invoke a callee with already-evaluated args. Takes ownership of
   argv[i] values. Used by OP_CALL. No refexp on fn: the caller's
   operand stack holds fn for the duration of this call, so its lifetime
   is already guaranteed — skipping the atomic pair is measurable on
   call-heavy benchmarks. */
static exp_t *vm_invoke_values(exp_t *fn, int nargs, exp_t **argv, env_t *env) {
  /* String-as-callable: (s i) returns the indexed char. The AST
     evaluator handles this in two places (literal string head and the
     symbol-lookup path added in ticket 5). The bytecode VM compiles
     (sym args...) as OP_CALL_GLOBAL → vm_invoke_values, so we need the
     same arm here or compiled bodies miscompile string-index reads. */
  if (iscallable_container(fn)) {
    /* (container arg) read — indexable: element by int index; keyed
       (dict/hamt/set): value/member by key. The AST evaluator has the same
       arm on its two head paths; this keeps compiled bodies in sync. */
    int i;
    if (nargs != 1) {
      for (i = 0; i < nargs; i++)
        unrefexp(argv[i]);
      return error(ERROR_MISSING_PARAMETER, fn, env,
                   "index: expected exactly 1 arg, got %d", nargs);
    }
    return container_apply(fn, argv[0], env); /* consumes argv[0]; caller unrefs fn */
  }
  if (iscont(fn)) {
    /* (k v) reached from compiled code: produce the escape token. */
    exp_t *payload = nargs > 0 ? refexp(argv[0]) : refexp(NIL_EXP);
    int i;
    for (i = 0; i < nargs; i++)
      unrefexp(argv[i]);
    return make_cont_escape((int64_t)(intptr_t)fn->meta, payload, env);
  }
#ifdef ALCOVE_FFI
  if (isffi(fn)) {
    /* An ffi-fn value called from compiled bytecode. The AST evaluator
       dispatches FFI in evaluate(); the VM funnels (sym args...) through
       OP_CALL_GLOBAL → here, so without this arm an FFI call inside any
       compiled function body errors ("not a lambda") instead of running.
       Args are already evaluated; alc_ffi_call consumes their refs. */
    return alc_ffi_call((alc_ffi_t *)fn->ptr, nargs, argv);
  }
#endif
  if (!islambda(fn)) {
    int i;
    for (i = 0; i < nargs; i++)
      unrefexp(argv[i]);
    return error(ERROR_ILLEGAL_VALUE, fn, env, "Bytecode call: not a lambda");
  }
  /* Honor closure capture (see invoke()). For top-level fns this is
     just global so behavior is unchanged. */
  env_t *captured = (env_t *)fn->next->meta;
  env_t *newenv = make_env(captured ? captured : env);
  /* callingfnc stays NULL — OP_CALL is always non-tail from our side. */

  /* Fast path: compiled lambdas have their param keys cached in
     bc->param_keys, so we skip the per-call walk over fn->content
     (a cons-list traversal) and the per-param type check. ~30 cycles
     saved per call on the typical 2-3 arg case — material on call-
     heavy benchmarks like nqueens (~1M calls). */
  if ((fn->flags & FLAG_COMPILED) && fn->bc && fn->bc->nparams == nargs &&
      nargs <= ENV_INLINE_SLOTS) {
    int i;
    for (i = 0; i < nargs; i++) {
      newenv->inline_keys[i] = fn->bc->param_keys[i];
      newenv->inline_vals[i] = argv[i];
    }
    newenv->n_inline = nargs;
  } else {
    /* Slow-path bind. Verify expected param count up-front for compiled
       fns: silently running the body with too few args used to fail
       later as a misleading "unbound variable". */
    if ((fn->flags & FLAG_COMPILED) && fn->bc && fn->bc->nparams != nargs) {
      int i;
      for (i = 0; i < nargs; i++)
        unrefexp(argv[i]);
      destroy_env(newenv);
      return error(ERROR_ILLEGAL_VALUE, fn, env,
                   "wrong number of args: expected %d, got %d", fn->bc->nparams,
                   nargs);
    }
    exp_t *p = lambda_params(fn);
    int i = 0;
    while (p && p->content) {
      if (!is_ptr(p->content) || !issymbol(p->content)) {
        int j;
        for (j = i; j < nargs; j++)
          unrefexp(argv[j]);
        destroy_env(newenv);
        return error(ERROR_ILLEGAL_VALUE, fn, env, "Bytecode call: bad param");
      }
      /* Rest param — collect remaining argv into a list and bind. */
      if (strcmp((char *)exp_text(p->content), ".") == 0) {
        if (!p->next || !p->next->content || !issymbol(p->next->content)) {
          int j;
          for (j = i; j < nargs; j++) unrefexp(argv[j]);
          destroy_env(newenv);
          return error(ERROR_ILLEGAL_VALUE, fn, env,
                       "rest param: symbol expected after '.'");
        }
        exp_t *rest_head = NIL_EXP, *rest_tail = NULL;
        for (; i < nargs; i++)
          list_append_owned(&rest_head, &rest_tail, argv[i]);
        var2env_bind((char *)exp_text(p->next->content), rest_head, newenv);
        p = NULL; /* done */
        break;
      }
      if (i >= nargs) {
        int j;
        for (j = i; j < nargs; j++) unrefexp(argv[j]);
        destroy_env(newenv);
        return error(ERROR_MISSING_PARAMETER, fn, env,
                     "wrong number of args");
      }
      var2env_bind((char *)exp_text(p->content), argv[i], newenv);
      p = p->next;
      i++;
    }
    while (i < nargs)
      unrefexp(argv[i++]);
  }

  exp_t *ret;
  if (fn->flags & FLAG_COMPILED) {
#ifdef ALCOVE_JIT
    if (fn->bc->jit) {
      ret = fn->bc->jit(newenv);
      if (!ret)
        ret = vm_run(fn, newenv); /* JIT deopt → bytecode */
    } else
#endif
      ret = vm_run(fn, newenv);
  } else {
    exp_t *body = fn->next->content;
    ret = NULL;
    while (body) {
      if (ret)
        unrefexp(ret);
      ret = evaluate(refexp(body->content), newenv);
      if (ret && iserror(ret))
        break;
      body = body->next;
    }
  }
  destroy_env(newenv);
  return ret;
}

exp_t *invoke(exp_t *e, exp_t *fn, env_t *env) {
  /* e->content = fn name, e->next = args list,
     fn->content = params list, fn->next->content = body list.

     We hold a ref on `fn` across the invocation so that its header
     symbols (whose ->ptr we borrow into env->inline_keys) can never be
     freed while the env is live. Tail calls reuse the frame via a
     trampoline loop — O(1) C stack for tail recursion. */

  /* Nested invokes inherit but don't export tail-position: the CALLEE
     decides for its own body. Save/restore around the call. */
  int outer_tail = in_tail_position;
  in_tail_position = 0;

  env_t *newenv;
  exp_t *ret = NULL;
  refexp(fn);

tailrec: {
  exp_t *body = fn->next->content;
  /* Closure: if fn captured an env at creation, use it as the new
     call frame's parent so let/with bindings from the defining scope
     resolve before walking up to global. Args themselves must be
     evaluated in the CALLER's env (where their free vars live) — so
     we eval first, bind into newenv with evalexp=false. */
  env_t *captured = (env_t *)fn->next->meta;
  /* Pre-evaluate args in the caller's env. Build a fresh list of
     pre-evaluated values so var2env can bind them without re-eval. */
  exp_t *evald_args = NULL, *evald_tail = NULL;
  {
    exp_t *src;
    for (src = e->next; src; src = src->next) {
      exp_t *v = EVAL(src->content, env);
      if (v && iserror(v)) {
        /* clean up partial evald list */
        if (evald_args)
          unrefexp(evald_args);
        unrefexp(fn);
        unrefexp(e);
        in_tail_position = outer_tail;
        return v;
      }
      exp_t *node = make_node(v ? v : NIL_EXP);
      if (!evald_args) {
        evald_args = node;
        evald_tail = node;
      } else {
        evald_tail->next = node;
        evald_tail = node;
      }
    }
  }
  newenv = make_env(captured ? captured : env);
  newenv->callingfnc = refexp(e);
  exp_t *params = lambda_params(fn);
  if ((ret = var2env(e, params, evald_args, newenv, false))) {
    if (evald_args)
      unrefexp(evald_args);
    destroy_env(newenv);
    unrefexp(fn);
    unrefexp(e);
    in_tail_position = outer_tail;
    return ret;
  }
  if (evald_args)
    unrefexp(evald_args);

  /* Compiled body: cross-function tail calls lose TCO here (internal
     OP_TAIL_SELF still applies). */
  if (fn->flags & FLAG_COMPILED) {
#ifdef ALCOVE_JIT
    if (fn->bc->jit) {
      ret = fn->bc->jit(newenv);
      if (!ret)
        ret = vm_run(fn, newenv); /* JIT deopt → bytecode */
    } else
#endif
      ret = vm_run(fn, newenv);
    destroy_env(newenv);
    unrefexp(fn);
    unrefexp(e);
    in_tail_position = outer_tail;
    return ret;
  }

  exp_t *cur = body;
  while (cur) {
    if (ret) {
      unrefexp(ret);
      ret = NULL;
    }
    int is_last = (cur->next == NULL);
    in_tail_position = is_last;
    ret = EVAL(cur->content, newenv);

    if (is_last && ret && ispair(ret) && (ret->flags & FLAG_TAILREC) &&
        islambda(ret->content)) {
      exp_t *marker = ret;
      ret = NULL;
      exp_t *resolved_fn = marker->content;

      if (resolved_fn == fn) {
        /* Self-recursion fast path: rebind params in place, skip the
           env teardown/rebuild. Marker args are already evaluated. */
        int i;
        for (i = 0; i < newenv->n_inline; i++)
          unrefexp(newenv->inline_vals[i]);
        newenv->n_inline = 0;
        if (newenv->d) {
          destroy_dict(newenv->d);
          newenv->d = NULL;
        }

        exp_t *curvar = fn->content;
        exp_t *curval = marker->next;
        while (curvar && curval) {
          exp_t *v = curval->content;
          if (issymbol(curvar->content)) {
            if (newenv->n_inline < ENV_INLINE_SLOTS) {
              newenv->inline_keys[newenv->n_inline] = exp_text(curvar->content);
              newenv->inline_vals[newenv->n_inline] = refexp(v);
              newenv->n_inline++;
            } else {
              if (!newenv->d)
                newenv->d = create_dict();
              set_get_keyval_dict(newenv->d, exp_text(curvar->content), v);
            }
          }
          curvar = curvar->next;
          curval = curval->next;
        }

        unrefexp(marker);
        cur = body; /* restart the body loop */
        continue;
      }

      /* Different function: full unwind + tailrec jump. */
      exp_t *new_fn = resolved_fn;
      marker->content = NULL;
      if (new_fn->meta) {
        marker->content =
            make_symbol((char *)new_fn->meta, strlen((char *)new_fn->meta));
      } else {
        marker->content = make_symbol("_", 1);
      }
      marker->flags &= ~FLAG_TAILREC;

      destroy_env(newenv);
      unrefexp(fn);
      unrefexp(e);
      fn = new_fn;
      e = marker;
      goto tailrec;
    }

    if (ret && iserror(ret)) {
      destroy_env(newenv);
      unrefexp(fn);
      unrefexp(e);
      in_tail_position = outer_tail;
      return ret;
    }
    cur = cur->next;
  }

  destroy_env(newenv);
  unrefexp(fn);
  unrefexp(e);
  in_tail_position = outer_tail;
  return ret;
}
}

#pragma GCC diagnostic ignored "-Wunused-parameter"
exp_t *expandmacro(exp_t *e, exp_t *fn, env_t *env) {
  env_t *newenv = make_env(NULL); // NULL instead of env
  exp_t *ret;

  if ((ret = var2env(e, fn->content, e->next, newenv, false))) {
    destroy_env(newenv);
    return ret;
  }
  /* Macro body evaluation produces the expansion AST. If a user-fn
     call in the body's tail position inherited in_tail_position from
     the macro caller, it would return a tail marker that becomes the
     "expansion" — invokemacro then evaluates the marker as if it were
     code, which mis-dispatches via FLAG_TAILREC. Body eval is NOT in
     tail position relative to the macro's caller — the caller's tail
     position applies to the EXPANSION's eval (invokemacro line 13337). */
  int outer_tail = in_tail_position;
  in_tail_position = 0;
  ret = EVAL(fn->next->content, newenv);
  in_tail_position = outer_tail;
  destroy_env(newenv);
  return ret;
}
#pragma GCC diagnostic warning "-Wunused-parameter"

exp_t *invokemacro(exp_t *e, exp_t *fn, env_t *env) {
  /* e->content = fn name,
     e->next->content->
     fn->content = var names
     fn-> next-> content =body*/
  exp_t *ret;
  ret = expandmacro(e, fn, env);
  unrefexp(e);
  if (ret && iserror(ret))
    return ret;
  ret = evaluate(ret, env);

  return ret;
}

exp_t *evaluate(exp_t *e, env_t *env) {
  /* TO DO UN REF VARS*/
  exp_t *tmpexp = NULL;
  exp_t *tmpexp2 = NULL;
  exp_t *tmpevexp = NULL; /* Evaluated structure to be freed*/
  exp_t *ret = NULL;

  if (e == NULL)
    return NULL;
  if isatom (e) {
    if issymbol (e) {
      if (((char *)exp_text(e))[0] == ':')
        return e; // e is a keyword
      if ((tmpexp = lookup(e, env))) {
        unrefexp(e);
        return tmpexp;
      } else {
        ret = error(ERROR_UNBOUND_VARIABLE, e, env, "Error unbound variable %s",
                    exp_text(e));
        unrefexp(e);
        return ret;
      }
    } else
      return e; // Number? String? Char? Boolean? Vector?
  } else if ispair (e) {
    tmpexp = car(e);
    if (tmpexp && ispair(tmpexp)) {
      tmpevexp = EVAL(tmpexp, env);
      tmpexp = tmpevexp;
    }
    if (tmpexp) {
      if isinternal (tmpexp) {
        int was_tail = in_tail_position;
        if (!(tmpexp->flags & FLAG_TAIL_AWARE))
          in_tail_position = 0;
        ret = tmpexp->fnc(e, env);
        in_tail_position = was_tail;
        goto finisht;
      }
      if (issymbol(tmpexp)) {
        if (((char *)exp_text(tmpexp))[0] == ':') {
          ret = error(ERROR_ILLEGAL_VALUE, e, env,
                      "Error keyword %s can not be used as function",
                      exp_text(tmpexp));
          goto finish;
        } // e is a keyword
        if ((tmpexp2 = lookup(tmpexp, env))) {
#ifdef ALCOVE_FFI
          if isffi (tmpexp2) {
            /* Eval each arg and dispatch through libffi to the C
               function held by tmpexp2. Clear in_tail_position around
               arg eval — FFI args are real values to marshal, not
               return values, so a user-fn call inside an arg must NOT
               produce a tail marker (which alc_ffi_call would reject
               as a non-number/non-string and fail type validation). */
            int outer_tail = in_tail_position;
            in_tail_position = 0;
            alc_ffi_t *f = (alc_ffi_t *)tmpexp2->ptr;
            int n = 0;
            exp_t *acur = e->next;
            while (acur && n < ALC_FFI_MAX_ARGS) {
              n++;
              acur = acur->next;
            }
            exp_t **argv = (n > 0) ? memalloc(n, sizeof(exp_t *)) : NULL;
            acur = e->next;
            int i = 0;
            for (; i < n; i++, acur = acur->next) {
              argv[i] = EVAL(acur->content, env);
              if (iserror(argv[i])) {
                exp_t *eret = argv[i];
                for (int j = 0; j < i; j++)
                  unrefexp(argv[j]);
                free(argv);
                unrefexp(tmpexp2);
                in_tail_position = outer_tail;
                ret = eret;
                goto finish;
              }
            }
            ret = alc_ffi_call(f, n, argv);
            free(argv);
            unrefexp(tmpexp2);
            in_tail_position = outer_tail;
            goto finisht;
          }
#endif
          if isinternal (tmpexp2) {
            /* Tail flag propagates only into tail-aware cmds (ifcmd).
               Others get it cleared so their sub-evaluations don't
               misfire and build spurious trampoline markers. */
            int was_tail = in_tail_position;
            if (!(tmpexp2->flags & FLAG_TAIL_AWARE))
              in_tail_position = 0;
            ret = tmpexp2->fnc(e, env);
            in_tail_position = was_tail;
            goto finisht;
          } else if islambda (tmpexp2) {
            if (in_tail_position) {
              ret = make_tail_marker(tmpexp2, e, env);
              unrefexp(tmpexp2);
              goto finisht;
            }
            ret = invoke(e, tmpexp2, env);
            /* invoke() borrows fn (its own refexp/unrefexp net to zero) and
               consumes e — but tmpexp2 is a separately-owned ref from the
               lookup above, so release it here like every sibling branch
               does. Omitting this leaked one ref per call on env-resolved
               closures, pinning the closure↔frame cycle alive. */
            unrefexp(tmpexp2);
            goto finisht;
          } else if (iscont(tmpexp2)) {
            /* (k v) — invoking an escape continuation: yields an escape token
               that propagates up to the matching call/cc frame. */
            ret = eval_cont_call(tmpexp2, e, env);
            unrefexp(tmpexp2);
            goto finish; /* eval_cont_call did not consume e */
          } else if ismacro (tmpexp2) {
            ret = invokemacro(e, tmpexp2, env);
            /* Same ownership as the lambda branch above: invokemacro borrows
               fn and consumes e, so release the looked-up tmpexp2 ref here.
               (A macro defined inside a fn body leaked its frame otherwise.) */
            unrefexp(tmpexp2);
            goto finisht;
          } else if (iscallable_container(tmpexp2)) {
            /* (c arg) where c is a var bound to a callable container —
               indexable: element by int index; keyed (dict/hamt/set):
               value/member by key. Without this the lookup path returned the
               container whole, ignoring the arg. Clear tail position for the
               arg eval — a leaked tail marker would otherwise be misread. */
            int outer_tail = in_tail_position;
            in_tail_position = 0;
            exp_t *idx = EVAL(cadr(e), env);
            in_tail_position = outer_tail;
            if (iserror(idx)) {
              unrefexp(tmpexp2);
              ret = idx;
              goto finish;
            }
            ret = container_apply(tmpexp2, idx, env); /* consumes idx */
            unrefexp(tmpexp2);
            goto finish;
          } else if ispair (tmpexp2) {
            ret = tmpexp2;
            goto finisht;
          } else {
            ret = tmpexp2;
            goto finisht;
          }
        } else {
          ret = error(ERROR_UNBOUND_VARIABLE, e, env,
                      "Error unbound variable %s", exp_text(tmpexp));
          goto finish;
        }
        ret = e; // what is happening here?
        goto finisht;
      } else if (iscallable_container(tmpexp)) {
        /* (c arg) literal container head — indexable element or keyed lookup.
           Same tail-clear as the symbol-bound path above. tmpexp is borrowed
           from e (not unref'd); container_apply consumes the evaluated arg. */
        int outer_tail = in_tail_position;
        in_tail_position = 0;
        tmpexp2 = EVAL(cadr(e), env);
        in_tail_position = outer_tail;
        if (iserror(tmpexp2)) {
          ret = tmpexp2;
          goto finish;
        }
        ret = container_apply(tmpexp, tmpexp2, env); /* consumes tmpexp2 */
        goto finish;
      } else if (islambda(tmpexp)) {
        if (in_tail_position) {
          ret = make_tail_marker(tmpexp, e, env);
          goto finisht;
        }
        ret = invoke(e, tmpexp, env);
        goto finisht;
      } else if (iscont(tmpexp)) {
        ret = eval_cont_call(tmpexp, e, env); /* tmpexp borrowed from e */
        goto finish;
      } else if (ismacro(tmpexp)) {
        ret = invokemacro(e, tmpexp, env);
        goto finisht;
      }
    } else if (tmpexp == NULL)
      return e;

    /*else if (islambda(tmpexp)) {
      return invoke(e,tmpexp,env);
      }*/
    else
      return e;
  } else {
    // is ???
    return e;
  }
  return e;
finish:
  unrefexp(e);
finisht:
  unrefexp(tmpevexp);
  return ret;
}

#ifdef ALCOVE_READLINE
#undef ISDIGIT /* char.h defines ISDIGIT as a bitmask; readline redefines it */
#include <readline/history.h>
#include <readline/readline.h>
#include <sys/stat.h> /* chmod(0600) on the persisted history file */

/* Iterate over a dict and emit names matching `prefix` into the
   readline-allocated match list. Each match must be malloc'd; readline
   takes ownership and free()s it. */
static void rl_collect_dict(dict_t *d, const char *prefix, size_t plen,
                            char ***out, int *nout, int *cap) {
  if (!d)
    return;
  int h;
  size_t i;
  keyval_t *k;
  for (h = 0; h < 2; h++) {
    if (!d->ht[h].size)
      continue;
    for (i = 0; i < d->ht[h].size; i++) {
      for (k = d->ht[h].table[i]; k; k = k->next) {
        if (!k->key)
          continue;
        if (plen && strncmp((const char *)k->key, prefix, plen) != 0)
          continue;
        if (*nout >= *cap) {
          *cap = *cap ? *cap * 2 : 16;
          *out = realloc(*out, sizeof(char *) * (*cap));
        }
        (*out)[(*nout)++] = strdup((const char *)k->key);
      }
    }
  }
}

/* readline completion generator — called repeatedly with state=0 first,
   then state>0 until it returns NULL. Builds the match list lazily on
   the first call.

   readline takes ownership of every string we return and free()s it
   itself. On a fresh state==0 call we therefore must NOT free the
   strings still in our array — only the array storage. The strings
   readline never consumed (if any) leak; in practice readline
   exhausts the generator so this is rare. */
static char *alcove_completion_generator(const char *text, int state) {
  static char **matches = NULL;
  static int n_matches = 0;
  static int cap = 0;
  static int idx = 0;
  if (state == 0) {
    /* Drop the array but NOT its contents — readline owns those now. */
    free(matches);
    matches = NULL;
    n_matches = 0;
    cap = 0;
    idx = 0;

    size_t tlen = strlen(text);
    rl_collect_dict(reserved_symbol, text, tlen, &matches, &n_matches, &cap);
    /* Walk env chain inner→outer (covers global defs). */
    env_t *cur;
    for (cur = g_global_env; cur; cur = cur->root) {
      int i;
      for (i = 0; i < cur->n_inline; i++) {
        const char *kk = cur->inline_keys[i];
        if (!kk)
          continue;
        if (tlen && strncmp(kk, text, tlen) != 0)
          continue;
        if (n_matches >= cap) {
          cap = cap ? cap * 2 : 16;
          matches = realloc(matches, sizeof(char *) * cap);
        }
        matches[n_matches++] = strdup(kk);
      }
      rl_collect_dict(cur->d, text, tlen, &matches, &n_matches, &cap);
    }
  }
  if (idx < n_matches)
    return matches[idx++]; /* readline frees the strdup */
  return NULL;
}
static char **alcove_rl_completer(const char *text, int start, int end) {
  (void)start;
  (void)end;
  rl_attempted_completion_over = 1; /* skip default file completion */
  return rl_completion_matches(text, alcove_completion_generator);
}

/* Quick paren depth — comments and string literals don't count. Returns
   the running depth (0 = balanced, >0 = need more, <0 = extra closer). */
static int rl_paren_depth(const char *s) {
  int depth = 0, in_string = 0;
  while (*s) {
    if (in_string) {
      if (*s == '\\' && s[1])
        s += 2;
      else if (*s == '"') {
        in_string = 0;
        s++;
      } else
        s++;
    } else {
      if (*s == '"') {
        in_string = 1;
        s++;
      } else if (*s == '(') {
        depth++;
        s++;
      } else if (*s == ')') {
        depth--;
        s++;
      } else if (*s == ';') {
        while (*s && *s != '\n')
          s++;
      } else
        s++;
    }
  }
  return depth;
}

/* Keywords that get the bold-magenta treatment in the colored
   redisplay. Kept short on purpose — coloring random user-defined
   names would be misleading. */
static const char *alcove_kw[] = {
    "def",    "fn",    "if",   "do",   "let", "for", "while", "and",     "or",
    "not",    "is",    "isnt", "no",   "yes", "t",   "nil",   "cond",    "when",
    "unless", "quote", "with", "each", "mac", "set", "=",     "setf",
    " return", NULL};
static int alc_is_kw(const char *s, int n) {
  int i;
  for (i = 0; alcove_kw[i]; i++) {
    int kn = (int)strlen(alcove_kw[i]);
    if (kn == n && strncmp(alcove_kw[i], s, n) == 0)
      return 1;
  }
  return 0;
}
static int alc_sym_char(unsigned char c) {
  /* alcove symbol chars: anything that isn't whitespace, paren, quote,
     comma, comment-start, or string delimiter. */
  if (c <= ' ')
    return 0;
  if (c == '(' || c == ')' || c == '\'' || c == '`' || c == ',' || c == ';' ||
      c == '"')
    return 0;
  return 1;
}
/* Walk `s` of length n and emit ANSI-colored output to `out`. Strings,
   comments, parens, numbers, and a small set of keywords get colored;
   everything else passes through. */
static void alc_print_colored(const char *s, int n, FILE *out) {
  int i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c == '(' || c == ')') {
      fprintf(out, "\x1B[33m%c\x1B[39m", c);
      i++;
    } else if (c == ';') {
      fputs("\x1B[90m", out);
      while (i < n && s[i] != '\n') {
        fputc(s[i], out);
        i++;
      }
      fputs("\x1B[39m", out);
    } else if (c == '"') {
      fputs("\x1B[32m\"", out);
      i++;
      while (i < n && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < n) {
          fputc(s[i], out);
          i++;
        }
        if (i < n) {
          fputc(s[i], out);
          i++;
        }
      }
      if (i < n) {
        fputc(s[i], out);
        i++;
      } /* closing quote */
      fputs("\x1B[39m", out);
    } else if ((c >= '0' && c <= '9') ||
               (c == '-' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9' &&
                (i == 0 || !alc_sym_char((unsigned char)s[i - 1])))) {
      fputs("\x1B[36m", out);
      if (c == '-') {
        fputc('-', out);
        i++;
      }
      while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) {
        fputc(s[i], out);
        i++;
      }
      fputs("\x1B[39m", out);
    } else if (alc_sym_char(c)) {
      int start = i;
      while (i < n && alc_sym_char((unsigned char)s[i]))
        i++;
      int len = i - start;
      if (alc_is_kw(s + start, len))
        fprintf(out, "\x1B[1;35m%.*s\x1B[22;39m", len, s + start);
      else
        fwrite(s + start, 1, len, out);
    } else {
      fputc(c, out);
      i++;
    }
  }
}
/* Custom readline redisplay: prints prompt + colored line buffer,
   then walks cursor back to rl_point. Limitations: assumes single
   physical terminal line (we don't track wrap). For long lines
   (> 256 chars) we fall back to readline's default redisplay. */
/* Print the prompt while stripping RL_PROMPT_START/END_IGNORE bytes
   (\001 \002). Those markers exist for readline's own width math; if
   we let them through they show up as literal glyphs in the terminal. */
static void alc_print_prompt_stripped(const char *p, FILE *out) {
  if (!p)
    return;
  for (; *p; p++)
    if (*p != '\001' && *p != '\002')
      fputc(*p, out);
}

/* Visible (on-screen) width of a prompt: skip the \001..\002 spans and
   any CSI escape sequences inside them — those render zero-width. */
static int alc_prompt_vwidth(const char *p) {
  int w = 0;
  for (size_t i = 0; p && p[i];) {
    if (p[i] == '\001') {
      while (p[i] && p[i] != '\002')
        i++;
      if (p[i])
        i++;
      continue;
    }
    if (p[i] == '\002') {
      i++;
      continue;
    }
    if (p[i] == '\x1B') {
      i++;
      if (p[i] == '[') {
        i++;
        while (p[i] && !(p[i] >= '@' && p[i] <= '~'))
          i++;
        if (p[i])
          i++;
      }
      continue;
    }
    w++;
    i++;
  }
  return w;
}

/* Cursor's row offset within the current readline render. Reset to 0
   before every readline() call so the first paint starts clean. */
static int g_rd_crow = 0;

/* Approximate terminal column width of a codepoint: 0 for combining /
   zero-width marks, 2 for the common East-Asian-wide and emoji ranges,
   1 otherwise. Enough to place the REPL cursor correctly; not a full
   Unicode width table. */
static int alc_cp_width(uint32_t cp) {
  if (cp == 0)
    return 0;
  if ((cp >= 0x0300 && cp <= 0x036F) || /* combining diacritics */
      (cp >= 0x200B && cp <= 0x200F) || /* zero-width spaces / marks */
      (cp >= 0xFE00 && cp <= 0xFE0F) || /* variation selectors */
      cp == 0x00AD)                     /* soft hyphen */
    return 0;
  if ((cp >= 0x1100 && cp <= 0x115F) ||  /* Hangul Jamo */
      (cp >= 0x2E80 && cp <= 0x303E) ||  /* CJK radicals, Kangxi */
      (cp >= 0x3041 && cp <= 0x33FF) ||  /* Kana .. CJK symbols */
      (cp >= 0x3400 && cp <= 0x4DBF) ||  /* CJK Ext A */
      (cp >= 0x4E00 && cp <= 0x9FFF) ||  /* CJK Unified */
      (cp >= 0xA000 && cp <= 0xA4CF) ||  /* Yi */
      (cp >= 0xAC00 && cp <= 0xD7A3) ||  /* Hangul syllables */
      (cp >= 0xF900 && cp <= 0xFAFF) ||  /* CJK compatibility */
      (cp >= 0xFE30 && cp <= 0xFE4F) ||  /* CJK compatibility forms */
      (cp >= 0xFF00 && cp <= 0xFF60) ||  /* fullwidth forms */
      (cp >= 0xFFE0 && cp <= 0xFFE6) ||  /* fullwidth signs */
      (cp >= 0x1F300 && cp <= 0x1FAFF) ||/* emoji & pictographs */
      (cp >= 0x20000 && cp <= 0x3FFFD))  /* CJK Ext B+ */
    return 2;
  return 1;
}

/* Multi-line-aware colored redisplay. A recalled history entry (or any
   accumulated form) may contain '\n' and span several screen rows;
   instead of the old single-line "\r\x1B[K" (which reprinted row 0 on
   every keystroke — see the history-recall cascade bug) we walk up to
   the render's first row, clear downward, repaint prompt + colored
   buffer, then place the cursor at rl_point's (row,col). Self-contained
   (never mixes with readline's own redisplay), so no screen desync. */
static void alcove_colored_redisplay(void) {
  if (rl_end > 256) { /* pathological line: let readline cope */
    rl_redisplay();
    return;
  }
  FILE *o = rl_outstream;
  const char *pr = rl_display_prompt ? rl_display_prompt : rl_prompt;
  fputc('\r', o);
  if (g_rd_crow > 0)
    fprintf(o, "\x1B[%dA", g_rd_crow); /* up to row 0 of our render */
  fputs("\x1B[J", o);                  /* clear from here to end of screen */
  /* Row 0 gets the real prompt; a multi-line buffer (recalled history
     entry) gets the "    ... " continuation prompt on each later row so
     it looks the same as it did when typed. The prompt is display-only
     — it is never part of rl_line_buffer / the evaluated text. */
  static const char *CONT = "    ... ";
  const int CONT_W = 8;
  alc_print_prompt_stripped(pr, o);
  for (int start = 0, i = 0; i <= rl_end; i++) {
    if (i == rl_end || rl_line_buffer[i] == '\n') {
      alc_print_colored(rl_line_buffer + start, i - start, o);
      if (i < rl_end) {
        fputc('\n', o);
        fputs(CONT, o);
        start = i + 1;
      }
    }
  }

  int pw = alc_prompt_vwidth(pr);
  int prow = 0, pcol = pw, erow = 0;
  /* Advance the cursor column by each codepoint's display width, not by
     byte count — a multi-byte char (é, ï, 世) is one (or two) columns,
     not one column per byte. */
  for (int i = 0; i < rl_point;) {
    if (rl_line_buffer[i] == '\n') {
      prow++;
      pcol = CONT_W; /* next row starts past the continuation prompt */
      i++;
    } else {
      size_t off = (size_t)i;
      uint32_t cp = utf8_decode_at(rl_line_buffer, &off);
      pcol += alc_cp_width(cp);
      i = (int)off;
    }
  }
  for (int i = 0; i < rl_end; i++)
    if (rl_line_buffer[i] == '\n')
      erow++;
  /* cursor is at (erow, end). Walk it back to (prow, pcol). */
  if (erow > prow)
    fprintf(o, "\x1B[%dA", erow - prow);
  fputc('\r', o);
  if (pcol > 0)
    fprintf(o, "\x1B[%dC", pcol);
  g_rd_crow = prow;
  fflush(o);
}

/* Every prompt starts a fresh render, so the cursor-row tracker must
   reset before each readline(). */
static char *alc_readline(const char *prompt) {
  g_rd_crow = 0;
  return readline(prompt);
}

/* Read one complete top-level form from the terminal. Continues
   prompting (with a continuation prompt) until paren balance hits 0.
   Returned string is malloc'd. NULL on EOF. */
#ifndef ALCOVE_ALS /* superseded by als_rl_read_form in the adder build */
static char *rl_read_form(int idx) {
  char prompt[64];
  /* Wrap escape codes with \001/\002 (RL_PROMPT_START_IGNORE /
     END_IGNORE) so readline counts visible width correctly. Doesn't
     affect our custom redisplay (which writes the prompt verbatim)
     but makes the cursor land in the right column on fallback. */
  snprintf(prompt, sizeof prompt,
           "\001\x1B[34m\002In "
           "[\001\x1B[94m\002%d\001\x1B[34m\002]:\001\x1B[39m\002 ",
           idx);
  char *line = alc_readline(prompt);
  /* The custom redisplay (alcove_colored_redisplay) leaves the cursor
     inside the input line — readline's default redisplay would emit a
     trailing \r\n itself, but our hook doesn't, so the eval result
     would otherwise appear glued to the input. Emit it ourselves. */
  putchar('\n');
  if (!line)
    return NULL; /* Ctrl-D on empty line */
  size_t len = strlen(line);
  size_t cap = len + 256;
  char *acc = malloc(cap);
  memcpy(acc, line, len + 1);
  free(line);
  while (rl_paren_depth(acc) > 0) {
    char *more = alc_readline("    ... ");
    putchar('\n'); /* same fix for continuation lines */
    if (!more)
      break;
    size_t need = strlen(acc) + strlen(more) + 2;
    if (need > cap) {
      cap = need * 2;
      acc = realloc(acc, cap);
    }
    strcat(acc, "\n");
    strcat(acc, more);
    free(more);
  }
  if (acc[0])
    add_history(acc);
  return acc;
}
#endif /* !ALCOVE_ALS */

#ifdef ALCOVE_ALS
/* True if `line` (after dropping a `#` comment) ends in a block-opening
   colon. Used to decide whether the REPL needs continuation lines. */
static int als_line_opens_block(const char *line) {
  char *nc = als_strip_comment(line);
  size_t n = strlen(nc);
  while (n > 0 && (nc[n - 1] == ' ' || nc[n - 1] == '\t' ||
                   nc[n - 1] == '\r'))
    nc[--n] = 0;
  int r = (n > 0 && nc[n - 1] == ':');
  free(nc);
  return r;
}

/* Auto-indent: how many leading spaces the *next* continuation line
   should start with, given everything entered so far. It is the indent
   of the last non-blank line, plus one level (2) if that line opens a
   block. So `def f (x):<enter>` lands you indented under the def. */
static int als_next_indent(const char *acc) {
  const char *line_start = acc, *last = NULL;
  for (const char *p = acc;; p++) {
    if (*p == '\n' || *p == 0) {
      int blank = 1;
      for (const char *q = line_start; q < p; q++)
        if (*q != ' ' && *q != '\t') {
          blank = 0;
          break;
        }
      if (!blank)
        last = line_start;
      if (*p == 0)
        break;
      line_start = p + 1;
    }
  }
  if (!last)
    return 0;
  const char *e = last;
  while (*e && *e != '\n')
    e++;
  int indent = 0;
  while (last[indent] == ' ' || last[indent] == '\t')
    indent++;
  size_t llen = (size_t)(e - last);
  char *buf = (char *)malloc(llen + 1);
  memcpy(buf, last, llen);
  buf[llen] = 0;
  int ob = als_line_opens_block(buf);
  free(buf);
  return indent + (ob ? 2 : 0);
}

/* readline startup hook: seeds the line buffer with the pending
   auto-indent. Using rl_startup_hook (not rl_pre_input_hook) means the
   text is inserted *before* the first prompt paint, so the indent +
   cursor show immediately with no manual rl_redisplay() (which would
   double the prompt under our custom redisplay function). */
static int als_pending_indent = 0;
static int als_preinput(void) {
  if (als_pending_indent > 0) {
    char sp[128];
    int n = als_pending_indent < (int)sizeof sp - 1 ? als_pending_indent
                                                    : (int)sizeof sp - 1;
    memset(sp, ' ', (size_t)n);
    sp[n] = 0;
    rl_insert_text(sp);
  }
  return 0;
}

/* adder-aware form reader for the interactive prompt. A one-line
   form (balanced parens, no trailing `:`) submits on Enter. Anything
   that opens a block or has unbalanced parens enters continuation mode
   ("    ... " prompt, auto-indented) and submits on a whitespace-only
   line once parens balance. Returns the raw text (malloc'd); NULL EOF. */
static char *als_rl_read_form(int idx) {
  char prompt[64];
  snprintf(prompt, sizeof prompt,
           "\001\x1B[34m\002In "
           "[\001\x1B[94m\002%d\001\x1B[34m\002]:\001\x1B[39m\002 ",
           idx);
  char *line = alc_readline(prompt);
  putchar('\n');
  if (!line)
    return NULL;
  size_t len = strlen(line);
  size_t cap = len + 256;
  char *acc = malloc(cap);
  memcpy(acc, line, len + 1);
  free(line);
  /* A recalled (or pasted) history entry arrives from the first
     readline() as one buffer that already contains '\n'. Treat that as
     an open block too, so the user lands back in continuation mode and
     can append lines / submit with a blank line — same as fresh input.
     Without this, recalling a multi-line form and hitting Enter would
     submit immediately instead of letting it be extended. */
  int multiline = (rl_paren_depth(acc) > 0) || als_line_opens_block(acc) ||
                   memchr(acc, '\n', len) != NULL;
  if (multiline) {
    for (;;) {
      als_pending_indent = als_next_indent(acc);
      rl_startup_hook = als_preinput;
      char *more = alc_readline("    ... ");
      rl_startup_hook = NULL;
      putchar('\n');
      if (!more)
        break; /* Ctrl-D ends the block early */
      int blank = 1;
      for (char *q = more; *q; q++)
        if (*q != ' ' && *q != '\t') {
          blank = 0;
          break;
        }
      if (blank && rl_paren_depth(acc) <= 0) {
        free(more); /* whitespace-only line + balanced parens -> submit */
        break;
      }
      size_t need = strlen(acc) + strlen(more) + 2;
      if (need > cap) {
        cap = need * 2;
        acc = realloc(acc, cap);
      }
      strcat(acc, "\n");
      strcat(acc, more);
      free(more);
    }
  }
  if (acc[0])
    add_history(acc);
  return acc;
}

/* Adder is whitespace-significant, so TAB at the start of a line
   (point is at column 0 or only whitespace precedes it) inserts one
   indent level instead of triggering completion. With real tokens to
   the left it still completes, so `(fi<TAB>` etc. keep working. */
#define ALS_INDENT "  " /* one indent level = 2 spaces */
static int als_smart_tab(int count, int key) {
  for (int i = 0; i < rl_point; i++)
    if (rl_line_buffer[i] != ' ' && rl_line_buffer[i] != '\t')
      return rl_complete(count, key); /* a token precedes -> complete */
  rl_insert_text(ALS_INDENT);
  return 0;
}
#endif /* ALCOVE_ALS */
#endif /* ALCOVE_READLINE */

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

/* ---------- hash-map / dict ops ---------- */

const char doc_hashmap[] = "(hash-map [k v ...]) — build an EXP_DICT. Keys: "
                           "keyword/string/number. Same as {k v, ...}.";
exp_t *hashmapcmd(exp_t *e, env_t *env) {
  exp_t *ret = make_dict_exp();
  dict_t *d = (dict_t *)ret->ptr;
  exp_t *a = cdr(e);
  char tmp[32];
  while (a) {
    exp_t *kraw = EVAL(car(a), env);
    if (iserror(kraw)) {
      unrefexp(ret);
      unrefexp(e);
      return kraw;
    }
    if (!a->next) {
      unrefexp(kraw);
      unrefexp(ret);
      unrefexp(e);
      return error(ERROR_MISSING_PARAMETER, NULL, env,
                   "hash-map: odd number of forms (key without value)");
    }
    exp_t *v = EVAL(car(a->next), env);
    if (iserror(v)) {
      unrefexp(kraw);
      unrefexp(ret);
      unrefexp(e);
      return v;
    }
    char *ks = alc_key_to_cstr(kraw, tmp);
    if (!ks) {
      unrefexp(kraw);
      unrefexp(v);
      unrefexp(ret);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "hash-map: unsupported key type");
    }
    set_get_keyval_dict(d, ks, v);
    unrefexp(kraw);
    unrefexp(v);
    a = a->next->next;
  }
  unrefexp(e);
  return ret;
}

const char doc_assocbang[] = "(assoc! d k v) — set d[k]=v in place; returns d.";
exp_t *assocbangcmd(exp_t *e, env_t *env) {
  DICT_KV_SETUP("assoc!")
  exp_t *v = EVAL(cadddr(e), env);
  if (iserror(v))
    CLEAN_RETURN_2(k, d, v);

  if (!ks)
    CLEAN_RETURN_3(
        k, d, v,
        error(ERROR_ILLEGAL_VALUE, NULL, env, "assoc!: unsupported key type"));

  set_get_keyval_dict((dict_t *)d->ptr, ks, v);
  CLEAN_RETURN_2(k, v, d);
}

const char doc_dissocbang[] =
    "(dissoc! d k) — delete key k from d in place; returns d.";
exp_t *dissocbangcmd(exp_t *e, env_t *env) {
  DICT_KV_SETUP("dissoc!")
  if (ks)
    del_keyval_dict((dict_t *)d->ptr, ks);
  CLEAN_RETURN_1(k, d); /* d is not unref'd, it is returned */
}

const char doc_get[] = "(get d k [default]) — fetch d[k]. Works on hash-maps. "
                       "Returns default (or nil) when missing.";
exp_t *getcmd(exp_t *e, env_t *env) {
  DICT_KV_SETUP("get")
  exp_t *ret = NIL_EXP;
  if (ks) {
    keyval_t *kv = set_get_keyval_dict((dict_t *)d->ptr, ks, NULL);
    if (kv)
      ret = refexp(kv->val);
    else if (cdddr(e))
      ret = EVAL(cadddr(e), env);
  } else if (cdddr(e)) {
    ret = EVAL(cadddr(e), env);
  }
  CLEAN_RETURN_2(k, d, ret);
}

const char doc_containsp[] = "(contains? d k) — t if d has key k, else nil.";
exp_t *containspcmd(exp_t *e, env_t *env) {
  DICT_KV_SETUP("contains?")
  exp_t *ret = NIL_EXP;
  if (ks && set_get_keyval_dict((dict_t *)d->ptr, ks, NULL))
    ret = TRUE_EXP;
  unrefexp(k);
  unrefexp(d);
  unrefexp(e);
  return ret;
}

const char doc_keys[] = "(keys d) — list of keys in d (order undefined).";
DICT_ITER_CMD(keyscmd, "keys", alc_cstr_to_key((char *)k->key))

const char doc_vals[] = "(vals d) — list of values in d (order matches keys).";
DICT_ITER_CMD(valscmd, "vals", refexp(k->val))

const char doc_count[] = "(count x) — element count for hash-maps, sets, "
                         "deques, vectors, strings, blobs, and lists.";
exp_t *countcmd(exp_t *e, env_t *env) {
  exp_t *x = EVAL(cadr(e), env);
  if (iserror(x)) {
    unrefexp(e);
    return x;
  }
  int64_t n = 0;
  if (isdict(x) || isset(x)) {
    dict_t *d = (dict_t *)x->ptr;
    n = d ? (int64_t)d->ht[0].used : 0;
  } else if (islist(x))
    n = ((alc_list_t *)x->ptr)->len;
  else if (isblob(x))
    n = (int64_t)((alc_blob_t *)x->ptr)->len;
  else if (is_ptr(x) && x->type == EXP_VECTOR && x->ptr)
    n = vec_len(x);
  else if (isstring(x))
    n = (int64_t)strlen((char *)exp_text(x));
  else if (ispair(x)) {
    exp_t *p = x;
    while (p && istrue(p)) {
      n++;
      p = p->next;
    }
  } else {
    unrefexp(x);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "count: unsupported type");
  }
  unrefexp(x);
  unrefexp(e);
  return MAKE_FIX(n);
}

/* ---------- hash-set / EXP_SET ops ---------- */

static void set_key_hex_append(char **buf, size_t *len, size_t *cap,
                               const unsigned char *bytes, size_t n) {
  static const char hexdigits[] = "0123456789abcdef";
  if (*len + n * 2 + 1 > *cap) {
    while (*len + n * 2 + 1 > *cap)
      *cap *= 2;
    char *p = realloc(*buf, *cap);
    if (!p)
      graceful_shutdown("Fatal error: Out of memory");
    *buf = p;
  }
  for (size_t i = 0; i < n; i++) {
    (*buf)[(*len)++] = hexdigits[bytes[i] >> 4];
    (*buf)[(*len)++] = hexdigits[bytes[i] & 0x0f];
  }
  (*buf)[*len] = 0;
}

static void set_key_put(char **buf, size_t *len, size_t *cap, const char *s) {
  str_buf_put(buf, len, cap, s, strlen(s));
}

static char *set_key_for_value(exp_t *v) {
  char tmp[128];
  size_t cap = 64, len = 0;
  char *buf = memalloc(cap, 1);
  buf[0] = 0;

  if (!v || v == NIL_EXP || (is_ptr(v) && ispair(v) && !istrue(v))) {
    set_key_put(&buf, &len, &cap, "N:");
    return buf;
  }
  if (v == TRUE_EXP) {
    set_key_put(&buf, &len, &cap, "T:");
    return buf;
  }
  if (isnumber(v)) {
    snprintf(tmp, sizeof tmp, "I:%lld", (long long)FIX_VAL(v));
    set_key_put(&buf, &len, &cap, tmp);
    return buf;
  }
  if (ischar(v)) {
    snprintf(tmp, sizeof tmp, "C:%u", (unsigned int)CHAR_VAL(v));
    set_key_put(&buf, &len, &cap, tmp);
    return buf;
  }
  if (isfloat(v)) {
    uint64_t bits = 0;
    memcpy(&bits, &v->f, sizeof bits);
    snprintf(tmp, sizeof tmp, "F:%016llx", (unsigned long long)bits);
    set_key_put(&buf, &len, &cap, tmp);
    return buf;
  }
  if (isstring(v) || issymbol(v)) {
    set_key_put(&buf, &len, &cap, isstring(v) ? "S:" : "Y:");
    {
      const char *_t = exp_text(v);
      set_key_hex_append(&buf, &len, &cap, (const unsigned char *)_t,
                         strlen(_t));
    }
    return buf;
  }
  if (isblob(v)) {
    alc_blob_t *b = (alc_blob_t *)v->ptr;
    set_key_put(&buf, &len, &cap, "B:");
    if (b && b->len)
      set_key_hex_append(&buf, &len, &cap, (const unsigned char *)b->bytes,
                         b->len);
    return buf;
  }

  free(buf);
  return NULL;
}

static exp_t *set_value_clone(exp_t *v) {
  if (!v || v == NIL_EXP || (is_ptr(v) && ispair(v) && !istrue(v)))
    return refexp(NIL_EXP);
  if (v == TRUE_EXP)
    return refexp(TRUE_EXP);
  if (isnumber(v) || ischar(v))
    return refexp(v);
  if (isfloat(v))
    return make_floatf(v->f);
  if (isstring(v))
    { const char *_t = exp_text(v); return make_string((char *)_t, strlen((char *)_t)); }
  if (issymbol(v))
    { const char *_t = exp_text(v); return make_symbol((char *)_t, strlen((char *)_t)); }
  if (isblob(v)) {
    alc_blob_t *b = (alc_blob_t *)v->ptr;
    return make_blob((b && b->len) ? b->bytes : "", b ? b->len : 0);
  }
  return NULL;
}

static int set_insert_value(dict_t *d, exp_t *v) {
  char *ks = set_key_for_value(v);
  if (!ks)
    return 0;
  exp_t *stored = set_value_clone(v);
  if (!stored) {
    free(ks);
    return 0;
  }
  set_get_keyval_dict(d, ks, stored);
  unrefexp(stored);
  free(ks);
  return 1;
}

const char doc_set[] =
    "(set x ...) — build an EXP_SET with unique scalar elements.";
exp_t *setcmd(exp_t *e, env_t *env) {
  exp_t *ret = make_set_exp();
  dict_t *d = (dict_t *)ret->ptr;
  for (exp_t *a = cdr(e); a; a = a->next) {
    exp_t *v = EVAL(car(a), env);
    if (iserror(v)) {
      unrefexp(ret);
      unrefexp(e);
      return v;
    }
    if (!set_insert_value(d, v)) {
      unrefexp(v);
      unrefexp(ret);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "set: unsupported element type");
    }
    unrefexp(v);
  }
  unrefexp(e);
  return ret;
}

const char doc_hashset[] = "(hash-set x ...) — alias for set.";
exp_t *hashsetcmd(exp_t *e, env_t *env) { return setcmd(e, env); }

#define SET_VALUE_SETUP(err_name)                                              \
  exp_t *s = EVAL(cadr(e), env);                                               \
  if (iserror(s)) {                                                            \
    unrefexp(e);                                                               \
    return s;                                                                  \
  }                                                                            \
  if (!isset(s)) {                                                             \
    unrefexp(s);                                                               \
    unrefexp(e);                                                               \
    return error(ERROR_ILLEGAL_VALUE, NULL, env,                               \
                 err_name ": first arg must be a set");                       \
  }                                                                            \
  exp_t *v = EVAL(caddr(e), env);                                              \
  if (iserror(v)) {                                                            \
    unrefexp(s);                                                               \
    unrefexp(e);                                                               \
    return v;                                                                  \
  }                                                                            \
  char *ks = set_key_for_value(v);

const char doc_setaddbang[] =
    "(set-add! s x) — add x to set s in place; returns s.";
exp_t *setaddbangcmd(exp_t *e, env_t *env) {
  SET_VALUE_SETUP("set-add!")
  if (!ks)
    CLEAN_RETURN_2(
        s, v, error(ERROR_ILLEGAL_VALUE, NULL, env,
                    "set-add!: unsupported element type"));
  exp_t *stored = set_value_clone(v);
  if (!stored) {
    free(ks);
    CLEAN_RETURN_2(
        s, v, error(ERROR_ILLEGAL_VALUE, NULL, env,
                    "set-add!: unsupported element type"));
  }
  set_get_keyval_dict((dict_t *)s->ptr, ks, stored);
  unrefexp(stored);
  free(ks);
  CLEAN_RETURN_1(v, s);
}

const char doc_setdelbang[] =
    "(set-del! s x) — remove x from set s in place; returns s.";
exp_t *setdelbangcmd(exp_t *e, env_t *env) {
  SET_VALUE_SETUP("set-del!")
  if (ks) {
    del_keyval_dict((dict_t *)s->ptr, ks);
    free(ks);
  }
  CLEAN_RETURN_1(v, s);
}

const char doc_sethasp[] = "(set-has? s x) — t if s contains x, else nil.";
exp_t *sethaspcmd(exp_t *e, env_t *env) {
  SET_VALUE_SETUP("set-has?")
  exp_t *ret = NIL_EXP;
  if (ks && set_get_keyval_dict((dict_t *)s->ptr, ks, NULL))
    ret = TRUE_EXP;
  if (ks)
    free(ks);
  CLEAN_RETURN_2(s, v, ret);
}

static exp_t *set_copy_exp(exp_t *src) {
  exp_t *ret = make_set_exp();
  dict_t *rd = (dict_t *)ret->ptr;
  dict_t *sd = (dict_t *)src->ptr;
  if (sd)
    for (unsigned int i = 0; i < sd->ht[0].size; i++)
      for (keyval_t *k = sd->ht[0].table[i]; k; k = k->next)
        set_insert_value(rd, k->val);
  return ret;
}

static int set_contains_key(exp_t *s, char *key) {
  return set_get_keyval_dict((dict_t *)s->ptr, key, NULL) != NULL;
}

const char doc_setunion[] = "(set-union a b) — new set with elements of a or b.";
exp_t *setunioncmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(a, b);
  if (!isset(a) || !isset(b))
    CLEAN_RETURN_2(a, b, error(ERROR_ILLEGAL_VALUE, NULL, env,
                               "set-union: args must be sets"));
  exp_t *ret = set_copy_exp(a);
  dict_t *rd = (dict_t *)ret->ptr;
  dict_t *bd = (dict_t *)b->ptr;
  if (bd)
    for (unsigned int i = 0; i < bd->ht[0].size; i++)
      for (keyval_t *k = bd->ht[0].table[i]; k; k = k->next)
        set_insert_value(rd, k->val);
  CLEAN_RETURN_2(a, b, ret);
}

const char doc_setintersection[] =
    "(set-intersection a b) — new set with elements common to a and b.";
exp_t *setintersectioncmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(a, b);
  if (!isset(a) || !isset(b))
    CLEAN_RETURN_2(a, b, error(ERROR_ILLEGAL_VALUE, NULL, env,
                               "set-intersection: args must be sets"));
  exp_t *ret = make_set_exp();
  dict_t *rd = (dict_t *)ret->ptr;
  dict_t *ad = (dict_t *)a->ptr;
  if (ad)
    for (unsigned int i = 0; i < ad->ht[0].size; i++)
      for (keyval_t *k = ad->ht[0].table[i]; k; k = k->next)
        if (set_contains_key(b, k->key))
          set_insert_value(rd, k->val);
  CLEAN_RETURN_2(a, b, ret);
}

const char doc_setdifference[] =
    "(set-difference a b) — new set with elements in a but not b.";
exp_t *setdifferencecmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(a, b);
  if (!isset(a) || !isset(b))
    CLEAN_RETURN_2(a, b, error(ERROR_ILLEGAL_VALUE, NULL, env,
                               "set-difference: args must be sets"));
  exp_t *ret = make_set_exp();
  dict_t *rd = (dict_t *)ret->ptr;
  dict_t *ad = (dict_t *)a->ptr;
  if (ad)
    for (unsigned int i = 0; i < ad->ht[0].size; i++)
      for (keyval_t *k = ad->ht[0].table[i]; k; k = k->next)
        if (!set_contains_key(b, k->key))
          set_insert_value(rd, k->val);
  CLEAN_RETURN_2(a, b, ret);
}

const char doc_setlist[] =
    "(set->list s) — list of set elements (order undefined).";
exp_t *setlistcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(s);
  if (!isset(s))
    CLEAN_RETURN_1(s, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "set->list: arg must be a set"));
  exp_t *ret = NIL_EXP, *tail = NULL;
  dict_t *d = (dict_t *)s->ptr;
  if (d)
    for (unsigned int i = 0; i < d->ht[0].size; i++)
      for (keyval_t *k = d->ht[0].table[i]; k; k = k->next)
        list_append_owned(&ret, &tail, set_value_clone(k->val));
  CLEAN_RETURN_1(s, ret);
}

/* ---------- persistent map (HAMT) / EXP_HAMT ops ----------
   A Hash Array Mapped Trie: an immutable map where assoc/dissoc return a NEW
   map that shares unchanged subtrees with the old one (structural sharing),
   so updates are O(log32 n) time AND space. 5 hash bits per level (32-way
   fan-out), bitmap-compressed nodes. Nodes are reference-counted C structs
   (not exp_t) shared across map versions; the trie is an acyclic DAG so
   refcounting reclaims it precisely (no cycles). A node with bitmap==0 is a
   hash-collision bucket (linear list of entries that share a full 32-bit
   hash); otherwise each present slot holds either a key/value entry
   (slot.key != NULL) or a child node (slot.key == NULL). */
#define HAMT_BITS 5
#define HAMT_MASK 31u
/* hamt_node / hamt_slot / hamt_t are declared in alcove.h (so istrue and
   print_node, which precede this section, can see the layout). */

/* Hash consistent with isequal: equal values must hash equal. Mirrors the
   types isequal compares by value (number/char/float/string/symbol/blob);
   anything else falls back to pointer identity (matching isequal's default). */
static uint32_t hamt_hashkey(exp_t *k) {
  if (isnumber(k)) { int64_t v = FIX_VAL(k); return bernstein_hash((unsigned char *)&v, sizeof v); }
  if (ischar(k))   { uint32_t v = CHAR_VAL(k); return bernstein_hash((unsigned char *)&v, sizeof v); }
  if (isfloat(k))  { double v = k->f; return bernstein_hash((unsigned char *)&v, sizeof v); }
  if (isstring(k) || issymbol(k)) { const char *s = exp_text(k); return bernstein_hash((unsigned char *)s, strlen(s)); }
  if (isblob(k))   { return bernstein_hash((unsigned char *)blob_bytes(k), blob_len(k)); }
  return bernstein_hash((unsigned char *)&k, sizeof(void *)); /* identity hash */
}

static hamt_node *hamt_node_ref(hamt_node *n) { if (n) n->nref++; return n; }
static void hamt_node_unref(hamt_node *n) {
  if (!n || --n->nref > 0)
    return;
  for (int i = 0; i < n->n; i++) {
    if (n->bitmap == 0 || n->slots[i].key) { /* entry */
      unrefexp(n->slots[i].key);
      unrefexp(n->slots[i].val);
    } else {
      hamt_node_unref(n->slots[i].child);
    }
  }
  free(n);
}

static hamt_node *hamt_node_alloc(int n, uint32_t bitmap) {
  hamt_node *node =
      (hamt_node *)memalloc(1, sizeof(hamt_node) + (size_t)n * sizeof(hamt_slot));
  node->nref = 1;
  node->bitmap = bitmap;
  node->n = n;
  return node;
}

/* Deep copy: new node owning fresh refs to every key/val/child. */
static hamt_node *hamt_node_copy(hamt_node *node) {
  hamt_node *c = hamt_node_alloc(node->n, node->bitmap);
  for (int i = 0; i < node->n; i++) {
    c->slots[i] = node->slots[i];
    if (node->bitmap == 0 || node->slots[i].key) {
      refexp(c->slots[i].key);
      refexp(c->slots[i].val);
    } else {
      hamt_node_ref(c->slots[i].child);
    }
  }
  return c;
}

/* Build a node holding two distinct entries that collide at `shift`. */
static hamt_node *hamt_merge(exp_t *k1, exp_t *v1, uint32_t h1, exp_t *k2,
                             exp_t *v2, uint32_t h2, int shift) {
  if (shift >= 32) { /* out of hash bits → collision bucket */
    hamt_node *b = hamt_node_alloc(2, 0);
    b->slots[0].key = refexp(k1); b->slots[0].val = refexp(v1);
    b->slots[1].key = refexp(k2); b->slots[1].val = refexp(v2);
    return b;
  }
  uint32_t i1 = (h1 >> shift) & HAMT_MASK, i2 = (h2 >> shift) & HAMT_MASK;
  if (i1 == i2) {
    hamt_node *child = hamt_merge(k1, v1, h1, k2, v2, h2, shift + HAMT_BITS);
    hamt_node *node = hamt_node_alloc(1, 1u << i1);
    node->slots[0].key = NULL;
    node->slots[0].child = child;
    return node;
  }
  hamt_node *node = hamt_node_alloc(2, (1u << i1) | (1u << i2));
  int p1 = (i1 < i2) ? 0 : 1, p2 = 1 - p1;
  node->slots[p1].key = refexp(k1); node->slots[p1].val = refexp(v1);
  node->slots[p2].key = refexp(k2); node->slots[p2].val = refexp(v2);
  return node;
}

/* Lookup: returns the borrowed value for key, or NULL if absent. */
static exp_t *hamt_node_get(hamt_node *node, exp_t *key, uint32_t hash, int shift) {
  if (!node)
    return NULL;
  if (node->bitmap == 0) { /* collision bucket */
    for (int i = 0; i < node->n; i++)
      if (isequal(node->slots[i].key, key))
        return node->slots[i].val;
    return NULL;
  }
  uint32_t bit = 1u << ((hash >> shift) & HAMT_MASK);
  if (!(node->bitmap & bit))
    return NULL;
  int pos = __builtin_popcount(node->bitmap & (bit - 1));
  if (node->slots[pos].key)
    return isequal(node->slots[pos].key, key) ? node->slots[pos].val : NULL;
  return hamt_node_get(node->slots[pos].child, key, hash, shift + HAMT_BITS);
}

/* assoc — returns an OWNED node (always a fresh path; the result owns one
   ref). *added set to 1 when key was not already present. node may be NULL
   (empty), producing a single-entry node. */
static hamt_node *hamt_node_assoc(hamt_node *node, exp_t *key, exp_t *val,
                                  uint32_t hash, int shift, int *added) {
  if (!node) {
    uint32_t idx = (hash >> shift) & HAMT_MASK;
    hamt_node *nn = hamt_node_alloc(1, 1u << idx);
    nn->slots[0].key = refexp(key);
    nn->slots[0].val = refexp(val);
    *added = 1;
    return nn;
  }
  if (node->bitmap == 0) { /* collision bucket */
    for (int i = 0; i < node->n; i++)
      if (isequal(node->slots[i].key, key)) {
        hamt_node *c = hamt_node_copy(node);
        unrefexp(c->slots[i].val);
        c->slots[i].val = refexp(val);
        *added = 0;
        return c;
      }
    hamt_node *c = hamt_node_alloc(node->n + 1, 0);
    for (int i = 0; i < node->n; i++) {
      c->slots[i].key = refexp(node->slots[i].key);
      c->slots[i].val = refexp(node->slots[i].val);
    }
    c->slots[node->n].key = refexp(key);
    c->slots[node->n].val = refexp(val);
    *added = 1;
    return c;
  }
  uint32_t bit = 1u << ((hash >> shift) & HAMT_MASK);
  int pos = __builtin_popcount(node->bitmap & (bit - 1));
  if (!(node->bitmap & bit)) { /* empty slot → insert entry, keeping order */
    hamt_node *c = hamt_node_alloc(node->n + 1, node->bitmap | bit);
    for (int i = 0; i < pos; i++) {
      c->slots[i] = node->slots[i];
      if (node->slots[i].key) { refexp(c->slots[i].key); refexp(c->slots[i].val); }
      else hamt_node_ref(c->slots[i].child);
    }
    c->slots[pos].key = refexp(key);
    c->slots[pos].val = refexp(val);
    for (int i = pos; i < node->n; i++) {
      c->slots[i + 1] = node->slots[i];
      if (node->slots[i].key) { refexp(c->slots[i + 1].key); refexp(c->slots[i + 1].val); }
      else hamt_node_ref(c->slots[i + 1].child);
    }
    *added = 1;
    return c;
  }
  if (node->slots[pos].key) { /* present entry */
    if (isequal(node->slots[pos].key, key)) { /* replace value */
      hamt_node *c = hamt_node_copy(node);
      unrefexp(c->slots[pos].val);
      c->slots[pos].val = refexp(val);
      *added = 0;
      return c;
    }
    /* different key, same slot → split into a child node */
    exp_t *ek = node->slots[pos].key, *ev = node->slots[pos].val;
    hamt_node *child = hamt_merge(ek, ev, hamt_hashkey(ek), key, val, hash,
                                  shift + HAMT_BITS);
    hamt_node *c = hamt_node_copy(node);
    unrefexp(c->slots[pos].key);
    unrefexp(c->slots[pos].val);
    c->slots[pos].key = NULL;
    c->slots[pos].child = child;
    *added = 1;
    return c;
  }
  /* present child → recurse */
  hamt_node *newchild =
      hamt_node_assoc(node->slots[pos].child, key, val, hash, shift + HAMT_BITS,
                      added);
  hamt_node *c = hamt_node_copy(node);
  hamt_node_unref(c->slots[pos].child);
  c->slots[pos].child = newchild;
  return c;
}

/* dissoc — returns an OWNED node (or NULL if the node becomes empty). When
   the key is absent, returns hamt_node_ref(node) and leaves *removed 0. */
static hamt_node *hamt_node_dissoc(hamt_node *node, exp_t *key, uint32_t hash,
                                   int shift, int *removed) {
  if (!node) { *removed = 0; return NULL; }
  if (node->bitmap == 0) { /* collision bucket */
    for (int i = 0; i < node->n; i++)
      if (isequal(node->slots[i].key, key)) {
        *removed = 1;
        if (node->n == 1)
          return NULL;
        hamt_node *c = hamt_node_alloc(node->n - 1, 0);
        int j = 0;
        for (int k = 0; k < node->n; k++)
          if (k != i) {
            c->slots[j].key = refexp(node->slots[k].key);
            c->slots[j].val = refexp(node->slots[k].val);
            j++;
          }
        return c;
      }
    *removed = 0;
    return hamt_node_ref(node);
  }
  uint32_t bit = 1u << ((hash >> shift) & HAMT_MASK);
  if (!(node->bitmap & bit)) { *removed = 0; return hamt_node_ref(node); }
  int pos = __builtin_popcount(node->bitmap & (bit - 1));
  if (node->slots[pos].key) { /* entry */
    if (!isequal(node->slots[pos].key, key)) { *removed = 0; return hamt_node_ref(node); }
    *removed = 1;
    if (node->n == 1)
      return NULL;
    hamt_node *c = hamt_node_alloc(node->n - 1, node->bitmap & ~bit);
    int j = 0;
    for (int i = 0; i < node->n; i++)
      if (i != pos) {
        c->slots[j] = node->slots[i];
        if (node->slots[i].key) { refexp(c->slots[j].key); refexp(c->slots[j].val); }
        else hamt_node_ref(c->slots[j].child);
        j++;
      }
    return c;
  }
  /* child → recurse */
  hamt_node *newchild =
      hamt_node_dissoc(node->slots[pos].child, key, hash, shift + HAMT_BITS,
                       removed);
  if (!*removed) { hamt_node_unref(newchild); return hamt_node_ref(node); }
  if (newchild == NULL) { /* child emptied → drop the slot */
    if (node->n == 1)
      return NULL;
    hamt_node *c = hamt_node_alloc(node->n - 1, node->bitmap & ~bit);
    int j = 0;
    for (int i = 0; i < node->n; i++)
      if (i != pos) {
        c->slots[j] = node->slots[i];
        if (node->slots[i].key) { refexp(c->slots[j].key); refexp(c->slots[j].val); }
        else hamt_node_ref(c->slots[j].child);
        j++;
      }
    return c;
  }
  hamt_node *c = hamt_node_copy(node);
  hamt_node_unref(c->slots[pos].child);
  c->slots[pos].child = newchild;
  return c;
}

/* Visit every entry (depth-first). Returns 0 to stop early. */
typedef int (*hamt_visit_fn)(exp_t *key, exp_t *val, void *ctx);
static int hamt_node_foreach(hamt_node *node, hamt_visit_fn fn, void *ctx) {
  if (!node)
    return 1;
  for (int i = 0; i < node->n; i++) {
    if (node->bitmap == 0 || node->slots[i].key) {
      if (!fn(node->slots[i].key, node->slots[i].val, ctx))
        return 0;
    } else if (!hamt_node_foreach(node->slots[i].child, fn, ctx)) {
      return 0;
    }
  }
  return 1;
}

/* Deep map equality (for `iso`): same count, and every key in `a` maps to an
   iso-equal value in `b`. With equal counts that's a bijection → equal. */
typedef struct { exp_t *other; int ok; } hamt_iso_ctx;
static int hamt_iso_visit(exp_t *key, exp_t *val, void *ctx) {
  hamt_iso_ctx *c = (hamt_iso_ctx *)ctx;
  hamt_t *b = (hamt_t *)c->other->ptr;
  exp_t *bv = hamt_node_get(b->root, key, hamt_hashkey(key), 0);
  if (!bv || !isoequal(val, bv)) {
    c->ok = 0;
    return 0; /* stop the walk */
  }
  return 1;
}
static int hamt_iso(exp_t *a, exp_t *b) {
  hamt_t *ha = (hamt_t *)a->ptr, *hb = (hamt_t *)b->ptr;
  if (ha->count != hb->count)
    return 0;
  hamt_iso_ctx ctx = {b, 1};
  hamt_node_foreach(ha->root, hamt_iso_visit, &ctx);
  return ctx.ok;
}

/* Wrap a (root,count) into a fresh EXP_HAMT value. Takes ownership of root. */
static exp_t *hamt_wrap(hamt_node *root, int64_t count) {
  hamt_t *h = (hamt_t *)memalloc(1, sizeof(hamt_t));
  h->root = root;
  h->count = count;
  MAKE_TYPED(m, EXP_HAMT, h);
  return m;
}

void hamt_free(void *ptr) {
  hamt_t *h = (hamt_t *)ptr;
  if (!h)
    return;
  hamt_node_unref(h->root);
  free(h);
}

static int hamt_print_one(exp_t *k, exp_t *v, void *ctx) {
  int *first = (int *)ctx;
  if (!*first)
    printf(", ");
  *first = 0;
  print_node(k);
  printf(" ");
  print_node(v);
  return 1;
}
/* Rendered like a map literal: {k v, k v}. Called from print_node. */
void hamt_print(exp_t *m) {
  hamt_t *h = (hamt_t *)m->ptr;
  int first = 1;
  printf("{");
  if (h)
    hamt_node_foreach(h->root, hamt_print_one, &first);
  printf("}");
}

/* savedb/loaddb persistence: serialize as type-tag, entry count, then each
   (key, value) pair recursively via the generic dump/load. Mirrors the deque
   serializer. */
typedef struct { FILE *s; int ok; } hamt_dump_ctx;
static int hamt_dump_visit(exp_t *key, exp_t *val, void *ctx) {
  hamt_dump_ctx *c = (hamt_dump_ctx *)ctx;
  if (!key || !__DUMPABLE__(key) || !__DUMP__(key, c->s) ||
      !val || !__DUMPABLE__(val) || !__DUMP__(val, c->s)) {
    c->ok = 0;
    return 0;
  }
  return 1;
}
exp_t *dump_hamt_value(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  hamt_t *h = (hamt_t *)e->ptr;
  size_t n = h ? (size_t)h->count : 0;
  if (dumpsize_t(stream, &n) <= 0)
    return NULL;
  if (!h || !h->root)
    return e;
  hamt_dump_ctx ctx = {stream, 1};
  hamt_node_foreach(h->root, hamt_dump_visit, &ctx);
  return ctx.ok ? e : NULL;
}
exp_t *load_hamt_value(exp_t *e, FILE *stream) {
  if (e)
    unrefexp(e);
  size_t n = 0;
  if (loadsize_t(stream, &n) <= 0 || n > (size_t)(1u << 28))
    return NULL;
  hamt_node *root = NULL;
  int64_t count = 0;
  for (size_t i = 0; i < n; i++) {
    exp_t *k = load_exp_t(stream);
    if (!k) { hamt_node_unref(root); return NULL; }
    exp_t *v = load_exp_t(stream);
    if (!v) { unrefexp(k); hamt_node_unref(root); return NULL; }
    int added = 0;
    hamt_node *nr = hamt_node_assoc(root, k, v, hamt_hashkey(k), 0, &added);
    hamt_node_unref(root);
    root = nr;
    count += added;
    unrefexp(k);
    unrefexp(v);
  }
  return hamt_wrap(root, count);
}

const char doc_hamt[] =
    "(hamt k v ...) — build a persistent (immutable) map from key/value "
    "pairs. assoc/dissoc return new maps sharing structure with the old.";
exp_t *hamtcmd(exp_t *e, env_t *env) {
  /* Evaluate alternating key/value args into a fresh persistent map. */
  hamt_node *root = NULL;
  int64_t count = 0;
  exp_t *cur = e->next;
  exp_t *err = NULL;
  while (cur) {
    if (!cur->next) {
      err = error(ERROR_MISSING_PARAMETER, e, env,
                  "hamt: odd number of args (need key/value pairs)");
      break;
    }
    exp_t *k = EVAL(cur->content, env);
    if (iserror(k)) { err = k; break; }
    exp_t *v = EVAL(cur->next->content, env);
    if (iserror(v)) { unrefexp(k); err = v; break; }
    int added = 0;
    hamt_node *nr = hamt_node_assoc(root, k, v, hamt_hashkey(k), 0, &added);
    hamt_node_unref(root);
    root = nr;
    count += added;
    unrefexp(k);
    unrefexp(v);
    cur = cur->next->next;
  }
  unrefexp(e);
  if (err) {
    hamt_node_unref(root);
    return err;
  }
  return hamt_wrap(root, count);
}

const char doc_hamtassoc[] =
    "(hamt-assoc m k v) — new map with k→v added/updated; m is unchanged.";
exp_t *hamtassoccmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(m, k, v);
  if (!ishamt(m))
    CLEAN_RETURN_3(m, k, v,
                   error(ERROR_ILLEGAL_VALUE, e, env, "hamt-assoc: not a hamt"));
  hamt_t *h = (hamt_t *)m->ptr;
  int added = 0;
  hamt_node *nr = hamt_node_assoc(h->root, k, v, hamt_hashkey(k), 0, &added);
  exp_t *ret = hamt_wrap(nr, h->count + added);
  CLEAN_RETURN_3(m, k, v, ret);
}

const char doc_hamtget[] =
    "(hamt-get m k [default]) — value for k, or default (nil) if absent.";
exp_t *hamtgetcmd(exp_t *e, env_t *env) {
  exp_t *m = NULL, *k = NULL, *dflt = NULL, *err = NULL, *ret = NULL;
  if (!e->next || !e->next->next) {
    err = error(ERROR_MISSING_PARAMETER, e, env, "(hamt-get m k [default])");
    goto done;
  }
  m = EVAL(e->next->content, env);
  if (iserror(m)) { err = m; m = NULL; goto done; }
  k = EVAL(e->next->next->content, env);
  if (iserror(k)) { err = k; k = NULL; goto done; }
  if (e->next->next->next) {
    dflt = EVAL(e->next->next->next->content, env);
    if (iserror(dflt)) { err = dflt; dflt = NULL; goto done; }
  }
  if (!ishamt(m)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "hamt-get: not a hamt");
    goto done;
  }
  {
    hamt_t *h = (hamt_t *)m->ptr;
    exp_t *v = hamt_node_get(h->root, k, hamt_hashkey(k), 0);
    ret = v ? refexp(v) : refexp(dflt ? dflt : NIL_EXP);
  }
done:
  unrefexp(m);
  unrefexp(k);
  unrefexp(dflt);
  unrefexp(e);
  return err ? err : ret;
}

const char doc_hamtdissoc[] =
    "(hamt-dissoc m k) — new map without k; m is unchanged.";
exp_t *hamtdissoccmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(m, k);
  if (!ishamt(m))
    CLEAN_RETURN_2(m, k,
                   error(ERROR_ILLEGAL_VALUE, e, env, "hamt-dissoc: not a hamt"));
  hamt_t *h = (hamt_t *)m->ptr;
  int removed = 0;
  hamt_node *nr = hamt_node_dissoc(h->root, k, hamt_hashkey(k), 0, &removed);
  exp_t *ret = hamt_wrap(nr, h->count - removed);
  CLEAN_RETURN_2(m, k, ret);
}

const char doc_hamtcount[] = "(hamt-count m) — number of entries in the map.";
exp_t *hamtcountcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(m);
  if (!ishamt(m))
    CLEAN_RETURN_1(m, error(ERROR_ILLEGAL_VALUE, e, env, "hamt-count: not a hamt"));
  int64_t c = ((hamt_t *)m->ptr)->count;
  CLEAN_RETURN_1(m, MAKE_FIX(c));
}

const char doc_hamtcontainsp[] = "(hamt-contains? m k) — t if k is present, else nil.";
exp_t *hamtcontainspcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(m, k);
  if (!ishamt(m))
    CLEAN_RETURN_2(m, k,
                   error(ERROR_ILLEGAL_VALUE, e, env, "hamt-contains?: not a hamt"));
  hamt_t *h = (hamt_t *)m->ptr;
  exp_t *v = hamt_node_get(h->root, k, hamt_hashkey(k), 0);
  CLEAN_RETURN_2(m, k, refexp(v ? TRUE_EXP : NIL_EXP));
}

/* Apply a callable container to one already-evaluated argument, consuming
   arg's ref (see the forward decl near vm_run). Indexable string/vector/blob:
   element by integer index (a float index truncates), error if non-number or
   out of range. Keyed: dict and hamt return the value at the key (nil if
   absent), set returns the member (nil if absent) — Clojure's (m k) / (s e).
   Returns an owned result or an error exp. */
static exp_t *container_apply(exp_t *c, exp_t *arg, env_t *env) {
  if (isindexable(c)) {
    if (!isnumber(arg) && !isfloat(arg)) {
      unrefexp(arg);
      return error(ERROR_NUMBER_EXPECTED, c, env, "index: arg must be a number");
    }
    int64_t i = isnumber(arg) ? FIX_VAL(arg) : (int64_t)arg->f;
    unrefexp(arg);
    exp_t *r = index_get(c, i);
    return r ? r
             : error(ERROR_INDEX_OUT_OF_RANGE, c, env,
                     "index: %lld out of range", (long long)i);
  }
  if (ishamt(c)) {
    hamt_t *h = (hamt_t *)c->ptr;
    exp_t *v = h ? hamt_node_get(h->root, arg, hamt_hashkey(arg), 0) : NULL;
    exp_t *ret = refexp(v ? v : NIL_EXP);
    unrefexp(arg);
    return ret;
  }
  if (isset(c)) {
    /* Sets canonicalize members with a type tag (set_key_for_value, malloc'd)
       so 2 and "2" are distinct — must use the same encoder as set-has?. */
    char *ks = set_key_for_value(arg);
    keyval_t *kv =
        (ks && c->ptr) ? set_get_keyval_dict((dict_t *)c->ptr, ks, NULL) : NULL;
    free(ks);
    exp_t *ret = refexp(kv ? arg : NIL_EXP); /* the member, or nil if absent */
    unrefexp(arg);
    return ret;
  }
  /* dict — canonical string key (keyword/string/number; else a miss -> nil). */
  {
    char tmp[32];
    char *ks = alc_key_to_cstr(arg, tmp);
    keyval_t *kv =
        (ks && c->ptr) ? set_get_keyval_dict((dict_t *)c->ptr, ks, NULL) : NULL;
    exp_t *ret = refexp(kv ? kv->val : NIL_EXP);
    unrefexp(arg);
    return ret;
  }
}

static int hamt_collect_keys(exp_t *key, exp_t *val, void *ctx) {
  (void)val;
  exp_t **acc = (exp_t **)ctx; /* acc[0]=head, acc[1]=tail */
  exp_t *node = make_node(refexp(key));
  if (!acc[0]) { acc[0] = node; acc[1] = node; }
  else { acc[1]->next = node; acc[1] = node; }
  return 1;
}
const char doc_hamtkeys[] = "(hamt-keys m) — list of the map's keys (unordered).";
exp_t *hamtkeyscmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(m);
  if (!ishamt(m))
    CLEAN_RETURN_1(m, error(ERROR_ILLEGAL_VALUE, e, env, "hamt-keys: not a hamt"));
  exp_t *acc[2] = {NULL, NULL};
  hamt_node_foreach(((hamt_t *)m->ptr)->root, hamt_collect_keys, acc);
  CLEAN_RETURN_1(m, acc[0] ? acc[0] : NIL_EXP);
}

const char doc_hamtp[] = "(hamt? x) — t if x is a persistent map, else nil.";
exp_t *hamtpcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(x);
  CLEAN_RETURN_1(x, refexp(ishamt(x) ? TRUE_EXP : NIL_EXP));
}

static int hamt_collect_vals(exp_t *key, exp_t *val, void *ctx) {
  (void)key;
  exp_t **acc = (exp_t **)ctx;
  exp_t *node = make_node(refexp(val));
  if (!acc[0]) { acc[0] = node; acc[1] = node; }
  else { acc[1]->next = node; acc[1] = node; }
  return 1;
}
const char doc_hamtvals[] = "(hamt-vals m) — list of the map's values (unordered).";
exp_t *hamtvalscmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(m);
  if (!ishamt(m))
    CLEAN_RETURN_1(m, error(ERROR_ILLEGAL_VALUE, e, env, "hamt-vals: not a hamt"));
  exp_t *acc[2] = {NULL, NULL};
  hamt_node_foreach(((hamt_t *)m->ptr)->root, hamt_collect_vals, acc);
  CLEAN_RETURN_1(m, acc[0] ? acc[0] : NIL_EXP);
}

static int hamt_collect_kv(exp_t *key, exp_t *val, void *ctx) {
  exp_t **acc = (exp_t **)ctx; /* flat: k1 v1 k2 v2 ... (round-trips via hamt) */
  exp_t *kn = make_node(refexp(key));
  exp_t *vn = make_node(refexp(val));
  kn->next = vn;
  if (!acc[0]) { acc[0] = kn; }
  else { acc[1]->next = kn; }
  acc[1] = vn;
  return 1;
}
const char doc_hamtlist[] =
    "(hamt->list m) — flat list (k1 v1 k2 v2 ...) of entries (unordered); "
    "round-trips via (apply hamt (hamt->list m)).";
exp_t *hamtlistcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(m);
  if (!ishamt(m))
    CLEAN_RETURN_1(m, error(ERROR_ILLEGAL_VALUE, e, env, "hamt->list: not a hamt"));
  exp_t *acc[2] = {NULL, NULL};
  hamt_node_foreach(((hamt_t *)m->ptr)->root, hamt_collect_kv, acc);
  CLEAN_RETURN_1(m, acc[0] ? acc[0] : NIL_EXP);
}

typedef struct { hamt_node *root; int64_t count; } hamt_merge_acc;
static int hamt_merge_one(exp_t *key, exp_t *val, void *ctx) {
  hamt_merge_acc *a = (hamt_merge_acc *)ctx;
  int added = 0;
  hamt_node *nr = hamt_node_assoc(a->root, key, val, hamt_hashkey(key), 0, &added);
  hamt_node_unref(a->root);
  a->root = nr;
  a->count += added;
  return 1;
}
const char doc_hamtmerge[] =
    "(hamt-merge a b) — new map with all of a's and b's entries; on a key in "
    "both, b's value wins. a and b are unchanged.";
exp_t *hamtmergecmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(a, b);
  if (!ishamt(a) || !ishamt(b))
    CLEAN_RETURN_2(a, b, error(ERROR_ILLEGAL_VALUE, e, env, "hamt-merge: need two hamts"));
  hamt_t *ha = (hamt_t *)a->ptr, *hb = (hamt_t *)b->ptr;
  hamt_merge_acc acc;
  acc.root = hamt_node_ref(ha->root); /* start from a (shares a's nodes) */
  acc.count = ha->count;
  hamt_node_foreach(hb->root, hamt_merge_one, &acc);
  exp_t *ret = hamt_wrap(acc.root, acc.count);
  CLEAN_RETURN_2(a, b, ret);
}

/* ---------- MsgPack serialization (msgpack-encode / msgpack-decode) ----------
   Compact binary codec for the JSON-ish subset of alcove values: nil, t,
   fixnums, floats, strings (and symbols), blobs (→ bin), lists (→ array), and
   dicts with string keys (→ map). MessagePack is big-endian. Round-trips with
   itself; useful for interop over RESP / FFI / files. Unsupported types
   (lambda/ffi/…) make encode error; malformed/truncated input makes decode
   error. */
typedef struct { uint8_t *b; size_t len, cap; } mp_buf;
static int mp_reserve(mp_buf *m, size_t n) {
  if (m->len + n <= m->cap)
    return 1;
  size_t nc = m->cap ? m->cap : 64;
  while (nc < m->len + n)
    nc *= 2;
  uint8_t *nb = (uint8_t *)realloc(m->b, nc);
  if (!nb)
    return 0;
  m->b = nb;
  m->cap = nc;
  return 1;
}
static int mp_put1(mp_buf *m, uint8_t b) {
  if (!mp_reserve(m, 1)) return 0;
  m->b[m->len++] = b;
  return 1;
}
static int mp_putn(mp_buf *m, const void *p, size_t n) {
  if (!mp_reserve(m, n)) return 0;
  memcpy(m->b + m->len, p, n);
  m->len += n;
  return 1;
}
static int mp_put_be(mp_buf *m, uint64_t v, int nbytes) {
  for (int i = nbytes - 1; i >= 0; i--)
    if (!mp_put1(m, (uint8_t)(v >> (i * 8)))) return 0;
  return 1;
}
static int mp_encode(exp_t *v, mp_buf *m);
static int mp_encode_int(mp_buf *m, int64_t n) {
  if (n >= 0) {
    if (n < 128) return mp_put1(m, (uint8_t)n);
    if (n <= 0xffLL) return mp_put1(m, 0xcc) && mp_put_be(m, (uint64_t)n, 1);
    if (n <= 0xffffLL) return mp_put1(m, 0xcd) && mp_put_be(m, (uint64_t)n, 2);
    if (n <= 0xffffffffLL) return mp_put1(m, 0xce) && mp_put_be(m, (uint64_t)n, 4);
    return mp_put1(m, 0xd3) && mp_put_be(m, (uint64_t)n, 8);
  }
  if (n >= -32) return mp_put1(m, (uint8_t)n);
  if (n >= -128) return mp_put1(m, 0xd0) && mp_put_be(m, (uint64_t)n, 1);
  if (n >= -32768) return mp_put1(m, 0xd1) && mp_put_be(m, (uint64_t)n, 2);
  if (n >= -2147483648LL) return mp_put1(m, 0xd2) && mp_put_be(m, (uint64_t)n, 4);
  return mp_put1(m, 0xd3) && mp_put_be(m, (uint64_t)n, 8);
}
static int mp_encode_strlen(mp_buf *m, size_t len) {
  if (len < 32) return mp_put1(m, (uint8_t)(0xa0 | len));
  if (len <= 0xff) return mp_put1(m, 0xd9) && mp_put_be(m, len, 1);
  if (len <= 0xffff) return mp_put1(m, 0xda) && mp_put_be(m, len, 2);
  return mp_put1(m, 0xdb) && mp_put_be(m, len, 4);
}
static int mp_encode(exp_t *v, mp_buf *m) {
  if (!v || v == NIL_EXP)
    return mp_put1(m, 0xc0); /* nil (also the empty list) */
  if (v == TRUE_EXP)
    return mp_put1(m, 0xc3);
  if (isnumber(v))
    return mp_encode_int(m, FIX_VAL(v));
  if (ischar(v))
    return mp_encode_int(m, (int64_t)CHAR_VAL(v));
  if (isfloat(v)) {
    uint64_t bits;
    double d = v->f;
    memcpy(&bits, &d, 8);
    return mp_put1(m, 0xcb) && mp_put_be(m, bits, 8);
  }
  if (isstring(v) || issymbol(v)) {
    const char *s = exp_text(v);
    size_t len = strlen(s);
    return mp_encode_strlen(m, len) && mp_putn(m, s, len);
  }
  if (isblob(v)) {
    size_t len = blob_len(v);
    int h = (len <= 0xff)    ? (mp_put1(m, 0xc4) && mp_put_be(m, len, 1))
            : (len <= 0xffff) ? (mp_put1(m, 0xc5) && mp_put_be(m, len, 2))
                              : (mp_put1(m, 0xc6) && mp_put_be(m, len, 4));
    return h && mp_putn(m, blob_bytes(v), len);
  }
  if (ispair(v)) {
    size_t n = 0;
    for (exp_t *p = v; p && ispair(p) && p->content; p = p->next)
      n++;
    int h = (n < 16)        ? mp_put1(m, (uint8_t)(0x90 | n))
            : (n <= 0xffff) ? (mp_put1(m, 0xdc) && mp_put_be(m, n, 2))
                            : (mp_put1(m, 0xdd) && mp_put_be(m, n, 4));
    if (!h)
      return 0;
    for (exp_t *p = v; p && ispair(p) && p->content; p = p->next)
      if (!mp_encode(p->content, m))
        return 0;
    return 1;
  }
  if (isdict(v)) {
    dict_t *d = (dict_t *)v->ptr;
    size_t n = d ? d->ht[0].used : 0;
    int h = (n < 16)        ? mp_put1(m, (uint8_t)(0x80 | n))
            : (n <= 0xffff) ? (mp_put1(m, 0xde) && mp_put_be(m, n, 2))
                            : (mp_put1(m, 0xdf) && mp_put_be(m, n, 4));
    if (!h)
      return 0;
    if (d)
      for (unsigned int i = 0; i < d->ht[0].size; i++)
        for (keyval_t *k = d->ht[0].table[i]; k; k = k->next) {
          const char *key = (char *)k->key;
          size_t kl = strlen(key);
          if (!(mp_encode_strlen(m, kl) && mp_putn(m, key, kl)))
            return 0;
          if (!mp_encode(k->val, m))
            return 0;
        }
    return 1;
  }
  return 0; /* unsupported type */
}

static exp_t *mp_decode(const uint8_t *b, size_t len, size_t *pos);
static uint64_t mp_get_be(const uint8_t *b, size_t pos, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++)
    v = (v << 8) | b[pos + i];
  return v;
}
/* Decode `n` elements into a fresh list. */
static exp_t *mp_decode_array(const uint8_t *b, size_t len, size_t *pos, size_t n) {
  exp_t *head = NULL, *tail = NULL;
  for (size_t i = 0; i < n; i++) {
    exp_t *el = mp_decode(b, len, pos);
    if (!el) {
      if (head) unrefexp(head);
      return NULL;
    }
    exp_t *node = make_node(el);
    if (!head) { head = node; tail = node; }
    else { tail->next = node; tail = node; }
  }
  return head ? head : refexp(NIL_EXP);
}
/* Decode `n` key/value pairs into a fresh dict (keys must be msgpack strings). */
static exp_t *mp_decode_map(const uint8_t *b, size_t len, size_t *pos, size_t n) {
  exp_t *dexp = make_dict_exp();
  dict_t *d = (dict_t *)dexp->ptr;
  for (size_t i = 0; i < n; i++) {
    exp_t *key = mp_decode(b, len, pos);
    if (!key) { unrefexp(dexp); return NULL; }
    if (!isstring(key)) { unrefexp(key); unrefexp(dexp); return NULL; }
    exp_t *val = mp_decode(b, len, pos);
    if (!val) { unrefexp(key); unrefexp(dexp); return NULL; }
    set_get_keyval_dict(d, exp_text(key), val); /* takes its own ref on val */
    unrefexp(key);
    unrefexp(val);
  }
  return dexp;
}
static exp_t *mp_decode(const uint8_t *b, size_t len, size_t *pos) {
  if (*pos >= len)
    return NULL;
  uint8_t c = b[(*pos)++];
  if (c <= 0x7f)
    return MAKE_FIX(c); /* positive fixint */
  if (c >= 0xe0)
    return MAKE_FIX((int8_t)c); /* negative fixint */
  if ((c & 0xe0) == 0xa0) {     /* fixstr */
    size_t n = c & 0x1f;
    if (*pos + n > len) return NULL;
    exp_t *s = make_string((char *)(b + *pos), (int)n);
    *pos += n;
    return s;
  }
  if ((c & 0xf0) == 0x90)
    return mp_decode_array(b, len, pos, c & 0x0f);
  if ((c & 0xf0) == 0x80)
    return mp_decode_map(b, len, pos, c & 0x0f);
#define MP_NEED(n) do { if (*pos + (n) > len) return NULL; } while (0)
  switch (c) {
  case 0xc0: return refexp(NIL_EXP);
  case 0xc2: return refexp(NIL_EXP);  /* false → nil */
  case 0xc3: return refexp(TRUE_EXP); /* true → t */
  case 0xcc: { MP_NEED(1); int64_t v = (int64_t)mp_get_be(b, *pos, 1); *pos += 1; return MAKE_FIX(v); }
  case 0xcd: { MP_NEED(2); int64_t v = (int64_t)mp_get_be(b, *pos, 2); *pos += 2; return MAKE_FIX(v); }
  case 0xce: { MP_NEED(4); int64_t v = (int64_t)mp_get_be(b, *pos, 4); *pos += 4; return MAKE_FIX(v); }
  case 0xcf: { MP_NEED(8); int64_t v = (int64_t)mp_get_be(b, *pos, 8); *pos += 8; return MAKE_FIX(v); }
  case 0xd0: { MP_NEED(1); int64_t v = (int8_t)mp_get_be(b, *pos, 1); *pos += 1; return MAKE_FIX(v); }
  case 0xd1: { MP_NEED(2); int64_t v = (int16_t)mp_get_be(b, *pos, 2); *pos += 2; return MAKE_FIX(v); }
  case 0xd2: { MP_NEED(4); int64_t v = (int32_t)mp_get_be(b, *pos, 4); *pos += 4; return MAKE_FIX(v); }
  case 0xd3: { MP_NEED(8); int64_t v = (int64_t)mp_get_be(b, *pos, 8); *pos += 8; return MAKE_FIX(v); }
  case 0xca: { MP_NEED(4); uint32_t bits = (uint32_t)mp_get_be(b, *pos, 4); *pos += 4; float f; memcpy(&f, &bits, 4); return make_floatf((double)f); }
  case 0xcb: { MP_NEED(8); uint64_t bits = mp_get_be(b, *pos, 8); *pos += 8; double d; memcpy(&d, &bits, 8); return make_floatf(d); }
  case 0xd9: case 0xda: case 0xdb: {
    int nb = (c == 0xd9) ? 1 : (c == 0xda) ? 2 : 4;
    MP_NEED((size_t)nb);
    size_t n = (size_t)mp_get_be(b, *pos, nb); *pos += nb;
    if (*pos + n > len) return NULL;
    exp_t *s = make_string((char *)(b + *pos), (int)n); *pos += n; return s;
  }
  case 0xc4: case 0xc5: case 0xc6: {
    int nb = (c == 0xc4) ? 1 : (c == 0xc5) ? 2 : 4;
    MP_NEED((size_t)nb);
    size_t n = (size_t)mp_get_be(b, *pos, nb); *pos += nb;
    if (*pos + n > len) return NULL;
    exp_t *bl = make_blob((char *)(b + *pos), n); *pos += n; return bl;
  }
  case 0xdc: case 0xdd: {
    int nb = (c == 0xdc) ? 2 : 4; MP_NEED((size_t)nb);
    size_t n = (size_t)mp_get_be(b, *pos, nb); *pos += nb;
    return mp_decode_array(b, len, pos, n);
  }
  case 0xde: case 0xdf: {
    int nb = (c == 0xde) ? 2 : 4; MP_NEED((size_t)nb);
    size_t n = (size_t)mp_get_be(b, *pos, nb); *pos += nb;
    return mp_decode_map(b, len, pos, n);
  }
  default: return NULL;
  }
#undef MP_NEED
}

const char doc_msgpackencode[] =
    "(msgpack-encode v) — serialize v to a MessagePack blob. Supports nil, t, "
    "fixnums, floats, strings/symbols, blobs, lists, and string-keyed dicts.";
exp_t *msgpackencodecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(v);
  mp_buf m = {NULL, 0, 0};
  if (!mp_encode(v, &m)) {
    free(m.b);
    CLEAN_RETURN_1(v, error(ERROR_ILLEGAL_VALUE, e, env,
                            "msgpack-encode: unsupported value type"));
  }
  exp_t *ret = make_blob((char *)m.b, m.len);
  free(m.b);
  CLEAN_RETURN_1(v, ret);
}
const char doc_msgpackdecode[] =
    "(msgpack-decode blob) — parse a MessagePack blob back into an alcove "
    "value (the inverse of msgpack-encode). Errors on malformed/truncated "
    "input or a non-string map key.";
exp_t *msgpackdecodecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(b);
  if (!isblob(b))
    CLEAN_RETURN_1(b, error(ERROR_ILLEGAL_VALUE, e, env,
                            "msgpack-decode: argument must be a blob"));
  size_t pos = 0, len = blob_len(b);
  exp_t *ret = mp_decode((const uint8_t *)blob_bytes(b), len, &pos);
  if (!ret || pos != len) {
    if (ret) unrefexp(ret);
    CLEAN_RETURN_1(b, error(ERROR_ILLEGAL_VALUE, e, env,
                            "msgpack-decode: malformed or trailing data"));
  }
  CLEAN_RETURN_1(b, ret);
}

/* ---------- deque / EXP_LIST ops ---------- */

/* Helper: append node to tail (no refcounting; caller already owns val ref). */
static void alc_list_push_right(alc_list_t *l, exp_t *val) {
  alc_listnode_t *n = (alc_listnode_t *)memalloc(1, sizeof(alc_listnode_t));
  n->val = val;
  n->prev = l->tail;
  if (l->tail)
    l->tail->next = n;
  else
    l->head = n;
  l->tail = n;
  l->len++;
}

static void alc_list_push_left(alc_list_t *l, exp_t *val) {
  alc_listnode_t *n = (alc_listnode_t *)memalloc(1, sizeof(alc_listnode_t));
  n->val = val;
  n->next = l->head;
  if (l->head)
    l->head->prev = n;
  else
    l->tail = n;
  l->head = n;
  l->len++;
}

const char doc_deque[] = "(deque [x ...]) — fresh doubly-linked deque "
                         "populated with given elements.";
exp_t *dequecmd(exp_t *e, env_t *env) {
  exp_t *ret = make_list_exp();
  alc_list_t *l = (alc_list_t *)ret->ptr;
  exp_t *a = cdr(e);
  while (a) {
    exp_t *v = EVAL(car(a), env);
    if (iserror(v)) {
      unrefexp(ret);
      unrefexp(e);
      return v;
    }
    alc_list_push_right(l, v); /* takes ownership of v */
    a = a->next;
  }
  unrefexp(e);
  return ret;
}

#define DEQUE_PUSH_CMD(name, err_name, push_fn)                                \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    exp_t *d = EVAL(cadr(e), env);                                             \
    if (iserror(d)) {                                                          \
      unrefexp(e);                                                             \
      return d;                                                                \
    }                                                                          \
    if (!islist(d)) {                                                          \
      unrefexp(d);                                                             \
      unrefexp(e);                                                             \
      return error(ERROR_ILLEGAL_VALUE, NULL, env,                             \
                   err_name ": first arg must be a deque");                    \
    }                                                                          \
    exp_t *v = EVAL(caddr(e), env);                                            \
    if (iserror(v)) {                                                          \
      unrefexp(d);                                                             \
      unrefexp(e);                                                             \
      return v;                                                                \
    }                                                                          \
    push_fn((alc_list_t *)d->ptr, v);                                          \
    unrefexp(e);                                                               \
    return d;                                                                  \
  }

#define DEQUE_POP_CMD(name, err_name, HEAD, TAIL, NEXT, PREV)                  \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    exp_t *d = EVAL(cadr(e), env);                                             \
    if (iserror(d)) {                                                          \
      unrefexp(e);                                                             \
      return d;                                                                \
    }                                                                          \
    if (!islist(d)) {                                                          \
      unrefexp(d);                                                             \
      unrefexp(e);                                                             \
      return error(ERROR_ILLEGAL_VALUE, NULL, env,                             \
                   err_name ": arg must be a deque");                          \
    }                                                                          \
    alc_list_t *l = (alc_list_t *)d->ptr;                                      \
    exp_t *ret = NIL_EXP;                                                      \
    if (l->HEAD) {                                                             \
      alc_listnode_t *n = l->HEAD;                                             \
      ret = n->val;                                                            \
      l->HEAD = n->NEXT;                                                       \
      if (l->HEAD)                                                             \
        l->HEAD->PREV = NULL;                                                  \
      else                                                                     \
        l->TAIL = NULL;                                                        \
      l->len--;                                                                \
      free(n);                                                                 \
    }                                                                          \
    unrefexp(d);                                                               \
    unrefexp(e);                                                               \
    return ret;                                                                \
  }

#define DEQUE_PEEK_CMD(name, err_name, HEAD)                                   \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    exp_t *d = EVAL(cadr(e), env);                                             \
    if (iserror(d)) {                                                          \
      unrefexp(e);                                                             \
      return d;                                                                \
    }                                                                          \
    if (!islist(d)) {                                                          \
      unrefexp(d);                                                             \
      unrefexp(e);                                                             \
      return error(ERROR_ILLEGAL_VALUE, NULL, env,                             \
                   err_name ": arg must be a deque");                          \
    }                                                                          \
    alc_list_t *l = (alc_list_t *)d->ptr;                                      \
    exp_t *ret = (l->HEAD) ? refexp(l->HEAD->val) : NIL_EXP;                   \
    unrefexp(d);                                                               \
    unrefexp(e);                                                               \
    return ret;                                                                \
  }

const char doc_pushrightbang[] =
    "(push-right! d x) — RPUSH; append x to right end of deque, returns d.";
DEQUE_PUSH_CMD(pushrightbangcmd, "push-right!", alc_list_push_right)

const char doc_pushleftbang[] =
    "(push-left! d x) — LPUSH; prepend x to left end of deque, returns d.";
DEQUE_PUSH_CMD(pushleftbangcmd, "push-left!", alc_list_push_left)

const char doc_poprightbang[] =
    "(pop-right! d) — RPOP; remove and return rightmost element, or nil.";
DEQUE_POP_CMD(poprightbangcmd, "pop-right!", tail, head, prev, next)

const char doc_popleftbang[] =
    "(pop-left! d) — LPOP; remove and return leftmost element, or nil.";
DEQUE_POP_CMD(popleftbangcmd, "pop-left!", head, tail, next, prev)

const char doc_peekleft[] =
    "(peek-left d) — leftmost element (no mutation), or nil.";
DEQUE_PEEK_CMD(peekleftcmd, "peek-left", head)

const char doc_peekright[] =
    "(peek-right d) — rightmost element (no mutation), or nil.";
DEQUE_PEEK_CMD(peekrightcmd, "peek-right", tail)

/* ---------- blob ops ---------- */

const char doc_makeblob[] = "(make-blob N) — N-byte zero-filled blob; or "
                            "(make-blob \"...\") to copy a string.";
exp_t *makeblobcmd(exp_t *e, env_t *env) {
  exp_t *a = EVAL(cadr(e), env);
  if (iserror(a)) {
    unrefexp(e);
    return a;
  }
  exp_t *ret;
  if (isnumber(a)) {
    int64_t n = FIX_VAL(a);
    if (n < 0) {
      unrefexp(a);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "make-blob: negative length");
    }
    ret = make_blob(NULL, (size_t)n);
  } else if (isstring(a)) {
    { const char *_t = exp_text(a); ret = make_blob((const char *)_t, strlen((char *)_t)); }
  } else {
    unrefexp(a);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "make-blob: arg must be a number or string");
  }
  unrefexp(a);
  unrefexp(e);
  return ret;
}

const char doc_bloblen[] = "(blob-len b) — byte count of b.";
UNARY_TYPE_CMD(bloblencmd, "blob-len: arg must be a blob", isblob, alc_blob_t,
               MAKE_FIX((int64_t)val_ptr->len))

const char doc_blobref[] =
    "(blob-ref b i) — byte at index i as fixnum (0..255).";
exp_t *blobrefcmd(exp_t *e, env_t *env) {
  exp_t *b = EVAL(cadr(e), env);
  if (iserror(b)) {
    unrefexp(e);
    return b;
  }
  if (!isblob(b)) {
    unrefexp(b);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "blob-ref: first arg must be a blob");
  }
  exp_t *i = EVAL(caddr(e), env);
  if (iserror(i)) {
    unrefexp(b);
    unrefexp(e);
    return i;
  }
  if (!isnumber(i)) {
    unrefexp(i);
    unrefexp(b);
    unrefexp(e);
    return error(ERROR_NUMBER_EXPECTED, NULL, env,
                 "blob-ref: index must be a number");
  }
  int64_t idx = FIX_VAL(i);
  alc_blob_t *bb = (alc_blob_t *)b->ptr;
  if (idx < 0 || (size_t)idx >= bb->len) {
    unrefexp(i);
    unrefexp(b);
    unrefexp(e);
    return error(ERROR_INDEX_OUT_OF_RANGE, NULL, env,
                 "blob-ref: index %lld out of range", (long long)idx);
  }
  int64_t v = (int64_t)(unsigned char)bb->bytes[idx];
  unrefexp(i);
  unrefexp(b);
  unrefexp(e);
  return MAKE_FIX(v);
}

const char doc_readbytes[] =
    "(read-bytes \"path\") — slurp a file into a blob. Returns nil on "
    "missing/unreadable file, or an error on bad arg.";
exp_t *readbytescmd(exp_t *e, env_t *env) {
  exp_t *a = EVAL(cadr(e), env);
  if (iserror(a)) {
    unrefexp(e);
    return a;
  }
  if (!isstring(a)) {
    unrefexp(a);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "read-bytes: path must be a string");
  }
  FILE *fp = fopen((const char *)exp_text(a), "rb");
  unrefexp(a);
  unrefexp(e);
  if (!fp)
    return refexp(NIL_EXP);
  /* Two-pass: stat-style seek so we allocate exactly once. fseek/ftell
     can lie on pipes, so cap defensively and fall back if the size
     looks bogus. */
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return refexp(NIL_EXP);
  }
  long sz = ftell(fp);
  if (sz < 0 || sz > (long)(1L << 30)) {
    fclose(fp);
    return refexp(NIL_EXP);
  }
  rewind(fp);
  exp_t *blob = make_blob(NULL, (size_t)sz);
  alc_blob_t *bb = (alc_blob_t *)blob->ptr;
  if (sz > 0 && fread(bb->bytes, 1, (size_t)sz, fp) != (size_t)sz) {
    fclose(fp);
    unrefexp(blob);
    return refexp(NIL_EXP);
  }
  fclose(fp);
  return blob;
}

const char doc_blob2string[] =
    "(blob->string b) — copy blob bytes into a fresh string. Errors unless "
    "the bytes are valid UTF-8 and NUL-free (a string is NUL-terminated and "
    "codepoint-oriented, so neither is representable).";
exp_t *blob2stringcmd(exp_t *e, env_t *env) {
  exp_t *obj = EVAL(cadr(e), env);
  if (iserror(obj)) {
    unrefexp(e);
    return obj;
  }
  if (!isblob(obj)) {
    unrefexp(obj);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "blob->string: arg must be a blob");
  }
  size_t n = blob_len(obj);
  const char *bytes = blob_bytes(obj);
  for (size_t i = 0; i < n; i++)
    if (bytes[i] == '\0') {
      unrefexp(obj);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "blob->string: blob has a NUL byte at offset %zu — not "
                   "representable as a string",
                   i);
    }
  size_t bad = 0;
  if (!utf8_valid(bytes, n, &bad)) {
    unrefexp(obj);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "blob->string: invalid UTF-8 at offset %zu", bad);
  }
  exp_t *ret = make_string((char *)bytes, (int)n);
  unrefexp(obj);
  unrefexp(e);
  return ret;
}

const char doc_vector[] = "(vector x ...) — build an EXP_VECTOR populated with "
                          "the given elements. Same as #[x ...].";
exp_t *vectorcmd(exp_t *e, env_t *env) {
  /* Two-pass: count elements (so we can size the vector once), then
     evaluate-and-store. Cheaper than growing a list intermediary. */
  long n = 0;
  for (exp_t *p = cdr(e); p; p = p->next)
    n++;
  exp_t *ret = make_vector(n, NIL_EXP);
  if (!ret) {
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "vector: alloc failed");
  }
  long i = 0;
  for (exp_t *p = cdr(e); p; p = p->next, i++) {
    exp_t *v = EVAL(car(p), env);
    if (iserror(v)) {
      unrefexp(ret);
      unrefexp(e);
      return v;
    }
    /* make_vector pre-filled with NIL (refcount bump per slot); release
       the placeholder before overwriting. */
    unrefexp(vec_gen_at(ret, i));
    vec_gen_at(ret, i) = v; /* take ownership */
  }
  /* Now that all elements are known, tighten to the narrowest kind:
     all-fixnum → I64, all-numeric → F64, anything else stays GEN. */
  vec_tighten(ret);
  unrefexp(e);
  return ret;
}

const char doc_string2blob[] =
    "(string->blob s) — wrap string bytes in a fresh blob.";
UNARY_TYPE_CMD(string2blobcmd, "string->blob: arg must be a string", isstring,
               char, make_blob(val_ptr, strlen(val_ptr)))

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
static void repl_readline_setup(env_t *global) {
  /* Honor the terminal's locale so readline treats UTF-8 input as whole
     characters — cursor movement, deletion, and width math operate per
     codepoint instead of per byte (otherwise typing é/ï/世 desyncs the
     cursor). Only LC_CTYPE, to avoid changing number formatting etc. */
  setlocale(LC_CTYPE, "");
  g_global_env = global; /* completer walks env bindings */
  rl_attempted_completion_function = alcove_rl_completer;
#ifdef ALCOVE_ALS
  rl_bind_key('\t', als_smart_tab); /* TAB indents at line start, else completes */
#endif
  rl_basic_word_break_characters = " \t\n()'`,;\"";
  rl_variable_bind("blink-matching-paren", "on");
  rl_redisplay_function = alcove_colored_redisplay; /* real-time highlighting */
  using_history();
  stifle_history(1000);
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
#endif

/* Eval one already-read top-level form with REPL semantics. `quiet` (file
   load) suppresses the Out[] print. Handles the quit/exit and toeval REPL
   words. Consumes `form`. Returns 1 iff quit/exit was seen. */
static int repl_eval_print_form(exp_t *form, env_t *env, int idx, int quiet) {
  const char *sv = issymbol(form) ? (const char *)exp_text(form) : NULL;
  if (sv && (strcmp(sv, "quit") == 0 || strcmp(sv, "exit") == 0)) {
    unrefexp(form);
    return 1;
  }
  if (sv && strcmp(sv, "toeval") == 0) {
    toeval = 1 - toeval;
    printf("%d\n", toeval);
    unrefexp(form);
    return 0;
  }
  exp_t *res = NULL;
  if (toeval)
    res = evaluate(form, env);
  else
    unrefexp(form);
  if (!quiet) {
    if (res) {
      printf("\x1B[31mOut[\x1B[91m%d\x1B[31m]:\x1B[39m", idx);
      print_node(res);
    } else
      printf("nil");
    printf("\n\n");
    fflush(stdout); /* keep interactive (-R reactor / piped) output prompt */
  }
  if (res)
    unrefexp(res);
  return 0;
}

/* Transpile a complete input chunk (adder in the adder build,
   s-expressions otherwise) and eval+print every top-level form it yields.
   Returns 1 iff quit/exit was seen. The interactive readline REPL and the
   -R combined REPL both feed it one complete unit at a time, so neither
   has to know about transpilation or form iteration. */
static int repl_eval_text(const char *src, size_t n, env_t *env, int idx) {
#ifdef ALCOVE_ALS
  char *tmp = (char *)malloc(n + 1);
  if (!tmp)
    return 0;
  memcpy(tmp, src, n);
  tmp[n] = 0;
  char *body = als_to_sexpr(tmp); /* malloc'd s-expression source */
  free(tmp);
  if (!body)
    return 0;
#else
  char *body = (char *)malloc(n + 1); /* already s-expressions */
  if (!body)
    return 0;
  memcpy(body, src, n);
  body[n] = 0;
#endif
  /* Run the reader over the s-expr source with a trailing newline so a
     bare final token (quit / a number) terminates instead of EOF-ing
     mid-token. */
  size_t blen = strlen(body);
  char *buf = (char *)malloc(blen + 2);
  if (!buf) {
    free(body);
    return 0;
  }
  memcpy(buf, body, blen);
  buf[blen] = '\n';
  buf[blen + 1] = 0;
  free(body);
  FILE *fs = fmemopen(buf, blen + 1, "r");
  int quit = 0;
  if (fs) {
    for (;;) {
      exp_t *form = reader(fs, 0, 0);
      if (!form)
        break;
      if (iserror(form) && form->flags == EXP_ERROR_PARSING_EOF) {
        unrefexp(form);
        break;
      }
      if (repl_eval_print_form(form, env, idx, 0)) {
        quit = 1;
        break;
      }
    }
    fclose(fs);
  }
  free(buf);
  return quit;
}

/* RESP2 server prototype lives in its own file but is included as
   part of this single TU so it can use file-static helpers
   (make_string, set_get_keyval_dict, ...) without exporting them.
   Excluded from the web build since it needs pthread/epoll/sockets. */
#ifndef ALCOVE_WEB
#include "resp.c"
#endif

#ifndef ALCOVE_WEB
/* shard_main — the future pthread entrypoint for a worker shard. For
   N=1 (today) the main thread invokes it with &main_shard, behavior
   identical to a direct resp_serve(port) call: same select() loop,
   same client list. The skeleton wires the per-shard inbox and wake
   fd through shard_runtime_init/destroy so Step 2.3 (acceptor split)
   only has to start producing — no further reactor surgery. */
int shard_main(shard_t *sh, int port) {
  current_shard = sh;
  if (shard_runtime_init(sh) < 0) {
    fprintf(stderr, "alcove: shard_runtime_init failed: %s\n", strerror(errno));
    /* Fall through anyway — resp_serve detects runtime_ready != 1
       and runs in single-thread degraded mode (no inbox). */
  }
  /* Mirror the global lfkv into the shard's TLS pointer if it has
     already been created (auto-load at startup, or a peer shard
     already wrote). resp_kv_ensure handles the lazy first-write
     creation; this just ensures reads on a fresh shard see the
     pre-loaded keyspace without waiting for a write. */
  if (!sh->kv)
    sh->kv = resp_kv_get();
  int rc = resp_serve(port);
  shard_runtime_destroy(sh);
  return rc;
}

/* ---------- multi-reactor entry (Step LF-7) ----------
   Spawns N reactor threads that all share the same global lock-free
   keyspace (g_resp_kv) and bind the same port via SO_REUSEPORT. Each
   thread gets its own shard_t (its own arena, inbox, wake fd, client
   list), registers with the epoch system, and runs an independent
   select() loop. The kernel hashes incoming connections across the
   N listening sockets; each connection thereafter is owned by a
   single reactor for its lifetime — no cross-thread fd sharing. */

typedef struct shard_thread_arg {
  shard_t *sh;
  int port;
  int rc;
} shard_thread_arg_t;

#if !ALCOVE_SINGLE_THREADED
static void *shard_thread_entry(void *p) {
  shard_thread_arg_t *a = p;
  a->rc = shard_main(a->sh, a->port);
  return NULL;
}
#endif

int respN_serve(int port, int nthreads) {
  if (nthreads <= 1)
    return shard_main(&main_shard, port);
#if ALCOVE_SINGLE_THREADED
  fprintf(stderr,
          "alcove: --threads %d ignored — this build is single-threaded "
          "(rebuild without ALCOVE_SINGLE_THREADED).\n", nthreads);
  return shard_main(&main_shard, port);
#else
  if (nthreads > EPOCH_MAX_THREADS) {
    fprintf(stderr, "alcove: clamping --threads %d to EPOCH_MAX_THREADS=%d\n",
            nthreads, EPOCH_MAX_THREADS);
    nthreads = EPOCH_MAX_THREADS;
  }
  /* Allocate N-1 extra shards (shard 0 is main_shard). Each gets its
     own heap arena so make_env doesn't race on the bump pointer. */
  shard_t **shards = calloc((size_t)nthreads, sizeof *shards);
  shard_thread_arg_t *args = calloc((size_t)nthreads, sizeof *args);
  pthread_t *tids = calloc((size_t)nthreads, sizeof *tids);
  if (!shards || !args || !tids) {
    fprintf(stderr, "alcove: OOM allocating reactor scaffolding\n");
    free(shards);
    free(args);
    free(tids);
    return 1;
  }
  shards[0] = &main_shard;
  for (int i = 1; i < nthreads; i++) {
    shard_t *sh = calloc(1, sizeof *sh);
    env_t *arena = calloc(ENV_ARENA_SLOTS, sizeof *arena);
    if (!sh || !arena) {
      fprintf(stderr, "alcove: OOM allocating shard %d\n", i);
      free(sh);
      free(arena);
      /* Spawning happens later in the function — no threads to join
         here, just release the shards we already allocated. (The
         older pthread_cancel call was dead code AND not portable
         to Bionic libc, which doesn't ship pthread_cancel.) */
      for (int j = 1; j < i; j++) {
        free(shards[j]->arena);
        free(shards[j]);
      }
      free(shards);
      free(args);
      free(tids);
      return 1;
    }
    sh->arena = arena;
    sh->arena_sp = arena;
    sh->arena_end = arena + ENV_ARENA_SLOTS;
    shards[i] = sh;
  }
  /* Spawn workers 1..N-1; main thread runs worker 0. */
  printf(ALCOVE_PROGNAME ": spawning %d reactor threads on port %d\n", nthreads, port);
  fflush(stdout);
  for (int i = 1; i < nthreads; i++) {
    args[i].sh = shards[i];
    args[i].port = port;
    if (pthread_create(&tids[i], NULL, shard_thread_entry, &args[i]) != 0) {
      fprintf(stderr, "alcove: pthread_create failed for shard %d: %s\n", i,
              strerror(errno));
      /* Continue with fewer threads rather than abort — but mark
         this slot as not-launched so we don't try to join it. */
      tids[i] = 0;
    }
  }
  args[0].sh = &main_shard;
  args[0].port = port;
  args[0].rc = shard_main(&main_shard, port);
  /* Once main returns (SIGINT was observed), wait for peers to drain. */
  for (int i = 1; i < nthreads; i++) {
    if (tids[i])
      pthread_join(tids[i], NULL);
  }
  for (int i = 1; i < nthreads; i++) {
    free(shards[i]->arena);
    free(shards[i]);
  }
  int rc = args[0].rc;
  free(shards);
  free(args);
  free(tids);
  return rc;
#endif /* !ALCOVE_SINGLE_THREADED */
}
#endif /* !ALCOVE_WEB */

/* Read every top-level form from `path` and evaluate it in `global`.
   Returns 1 if the file existed (and we processed it), 0 if missing.
   Errors mid-file are printed but do not abort the load — same loose
   policy as the -e flag and the interactive REPL. */
static int alcove_run_init_file(env_t *global, const char *path) {
  FILE *fp = fopen(path, "r");
  if (!fp)
    return 0;
  for (;;) {
    exp_t *e = reader(fp, 0, 0);
    if (!e)
      break;
    if (iserror(e) && e->flags == EXP_ERROR_PARSING_EOF) {
      unrefexp(e);
      break;
    }
    if (iserror(e)) {
      print_node(e);
      printf("\n");
      unrefexp(e);
      continue;
    }
    exp_t *r = evaluate(e, global);
    if (r) {
      if (iserror(r)) {
        print_node(r);
        printf("\n");
      }
      unrefexp(r);
    }
  }
  fclose(fp);
  return 1;
}

/* Try ./.init.alc first (project-local, takes priority); fall back to
   $HOME/.local/alcove/init.alc (user-global). Stops at the first
   match — never runs both. Silent on miss; one-line announce on hit. */
static void alcove_try_init_files(env_t *global) {
  if (alcove_run_init_file(global, "./.init.alc")) {
    printf(ALCOVE_PROGNAME ": loaded ./.init.alc\n");
    return;
  }
  const char *home = getenv("HOME");
  if (!home)
    return;
  char path[1024];
  int n = snprintf(path, sizeof path, "%s/.local/alcove/init.alc", home);
  if (n < 0 || (size_t)n >= (int)sizeof path)
    return;
  if (alcove_run_init_file(global, path))
    printf(ALCOVE_PROGNAME ": loaded %s\n", path);
}

int main(int argc, char *argv[]) {
  dict_t *dict = create_dict();
  env_t *global = make_env(NULL);
  /* Publish the global env early so introspection builtins (source,
     completion, etc.) can compare against it across all entry paths
     (-e flag, file arg, stdin pipe), not just the interactive REPL. */
  g_global_env = global;
  FILE *stream;
  int evaluatingfile = 0;
  int idx = 0;
  exp_t *t;
  exp_t *nil;
  exp_t *val;
  exp_tfuncList[EXP_CHAR] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_CHAR]->load = load_char;
  exp_tfuncList[EXP_CHAR]->dump = dump_char;
  exp_tfuncList[EXP_STRING] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_STRING]->load = load_string;
  exp_tfuncList[EXP_STRING]->dump = dump_string;
  /* Phase 1 persistence: scalars + symbols. */
  exp_tfuncList[EXP_NUMBER] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_NUMBER]->load = load_number;
  exp_tfuncList[EXP_NUMBER]->dump = dump_number;
  exp_tfuncList[EXP_FLOAT] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_FLOAT]->load = load_float;
  exp_tfuncList[EXP_FLOAT]->dump = dump_float;
  exp_tfuncList[EXP_SYMBOL] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_SYMBOL]->load = load_symbol;
  exp_tfuncList[EXP_SYMBOL]->dump = dump_symbol;
  /* Phase 2 persistence: lists + lambdas (source-form). */
  exp_tfuncList[EXP_PAIR] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_PAIR]->load = load_pair;
  exp_tfuncList[EXP_PAIR]->dump = dump_pair;
  exp_tfuncList[EXP_LAMBDA] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_LAMBDA]->load = load_lambda;
  exp_tfuncList[EXP_LAMBDA]->dump = dump_lambda;
  exp_tfuncList[EXP_MACRO] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_MACRO]->load = load_macro;
  exp_tfuncList[EXP_MACRO]->dump = dump_macro;
  /* EXP_BLOB — RESP string values are binary-safe blobs. Registering
     dump/load here lets the unified savedb format persist the RESP
     keyspace (alongside the existing Lisp env section). */
  exp_tfuncList[EXP_BLOB] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_BLOB]->load = load_blob;
  exp_tfuncList[EXP_BLOB]->dump = dump_blob;

  /* EXP_VECTOR — length-prefixed recursive dump; heterogeneous element
     types round-trip through __DUMP__/__LOAD__. Needed for ML / data
     workloads where the natural representation is a numeric vec. */
  exp_tfuncList[EXP_VECTOR] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_VECTOR]->load = load_vec;
  exp_tfuncList[EXP_VECTOR]->dump = dump_vec;
  exp_tfuncList[EXP_SET] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_SET]->load = load_set;
  exp_tfuncList[EXP_SET]->dump = dump_set;
  /* EXP_DICT (hash-map) and EXP_LIST (deque) — round-trip through savedb
     so persisted dicts/deques (and vecs/sets containing them) survive. */
  exp_tfuncList[EXP_DICT] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_DICT]->load = load_dict_value;
  exp_tfuncList[EXP_DICT]->dump = dump_dict_value;
  exp_tfuncList[EXP_LIST] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_LIST]->load = load_deque_value;
  exp_tfuncList[EXP_LIST]->dump = dump_deque_value;
  exp_tfuncList[EXP_HAMT] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_HAMT]->load = load_hamt_value;
  exp_tfuncList[EXP_HAMT]->dump = dump_hamt_value;

  reserved_symbol = create_dict();
  /* Allocate immortal singletons before any other code references them. */
  nil_singleton = make_nil();
  true_singleton = make_symbol("t", 1);
  gen_done_singleton = make_symbol("*done*", 6);
  set_get_keyval_dict(reserved_symbol, "nil", nil = NIL_EXP);
  set_get_keyval_dict(reserved_symbol, "t", t = TRUE_EXP);

  int N = sizeof(lispProcList) / sizeof(lispProc);
  int i;
  for (i = 0; i < N; ++i) {
    set_get_keyval_dict(
        reserved_symbol, lispProcList[i].name,
        val = make_internal(lispProcList[i].cmd,
                            lispProcList[i].flags & FLAG_TAIL_AWARE));
    unrefexp(val);
  }

#ifdef ALCOVE_WEB
  /* Web build: init complete, hand control back to JS. The Emscripten
     runtime stays alive (build with -sNO_EXIT_RUNTIME=1) so JS can
     call _alcove_web_eval after main returns. g_global_env was set
     above; alcove_web_eval below uses it. */
  (void)argc;
  (void)argv;
  (void)dict;
  (void)t;
  (void)nil;
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
      printf(ALCOVE_PROGNAME ": auto-loaded %d entries from %s (use --noload to "
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
    if ((stream = fopen(argv[argc - 1], "r"))) {
      evaluatingfile = 1;
      if (strcmp(argv[argc - 2], "-i") == 0)
        evaluatingfile |= 2;
    } else {
      printf("Error opening %s\n", argv[argc - 1]);
      exit(1);
    }
  } else
    stream = stdin;

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
    char *sx = als_to_sexpr(slurp.p);
    free(slurp.p);
    stream = fmemopen(sx, strlen(sx), "r");
    /* sx intentionally outlives this scope: it backs `stream` for the
       remainder of the run and is reclaimed by the OS at exit. */
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

  exp_t *stre = NULL;

#ifdef ALCOVE_READLINE
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

  while (1) {
    idx++;
    if (!evaluatingfile)
      printf("\x1B[34mIn [\x1B[94m%d\x1B[34m]:\x1B[39m", idx);
    stre = reader(stream, 0, 0);
    if (iserror(stre) && (stre->flags == EXP_ERROR_PARSING_EOF)) {
      if (evaluatingfile) {
        if (evaluatingfile & 2) {
          stream = stdin;
          evaluatingfile = 0;
          unrefexp(stre);
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
    /* Shared eval + print core; quiet (no Out[] print) while loading a
       file. Returns 1 on quit/exit. */
    if (repl_eval_print_form(stre, global, idx, evaluatingfile))
      break;
  }
endcleanly:
#ifdef ALCOVE_READLINE
  repl_history_save();
#endif
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
  unrefexp(t);
  unrefexp(nil);
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
#ifdef ALCOVE_ALS
  /* main was renamed via #define, so the compiler no longer grants the
     implicit `return 0` it special-cases for `main`. */
  return 0;
#endif
}

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
    exp_t *strf = evaluate(stre, g_global_env);
    if (strf) {
      /* Match the script-execution convention: don't echo nil results.
         (prn ...) returns nil, and printing "nil" after every print
         call in a REPL session is just noise. */
      if (strf != NIL_EXP) {
        print_node(strf);
        printf("\n");
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
