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
    LISPCMD("defn", defncmd, doc_defn),
    LISPCMD("fn", fncmd, doc_fn),
    LISPCMD("defc", defccmd, doc_defc),
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

/* Per-type dump/load serializers live in a dedicated #included fragment. */
#include "persist.h"

/* The s-expression reader/tokenizer lives in a dedicated #included
   fragment (single TU — keeps the per-byte loop inlinable). */
#include "reader.c"
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
      if (env) {
        val->next->meta = (struct keyval_t *)ref_env(env);
        env->has_closure = 1;
      }
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
        if (env) {
          val->next->meta = (struct keyval_t *)ref_env(env);
          env->has_closure = 1;
        }
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
  CHECK_RESERVED_BIND(name, val, "as a function name",
                      { unrefexp(e); return val; });
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
    exp_t *params_node = NULL, *body_node = NULL; /* each becomes an owned ref */
    if (is_ptr(clause) && ispair(clause)) {
      exp_t *first = car(clause);
      if (is_ptr(first) && ispair(first)) {
        /* explicit param list */
        params_node = refexp(first);
        if (cdr(clause))
          body_node = refexp(cdr(clause));
      } else if (issymbol(first)) {
        /* leading-symbol params: collect leading symbols into a fresh list,
           the remainder is the body. */
        exp_t *ph = NIL_EXP, *pt = NULL, *p = clause;
        while (p && p->content && issymbol(p->content)) {
          exp_t *pn = make_node(refexp(p->content));
          if (pt)
            pt = pt->next = pn;
          else
            ph = pt = pn;
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
    exp_t *L = make_node(params_node);              /* content = params */
    L->next = make_node(body_node);                 /* next->content = body */
    L->type = EXP_LAMBDA;
    if (env)
      L->next->meta = (struct keyval_t *)ref_env(env); /* closure capture */
    compile_lambda(L, env && env->root);
    exp_t *node = make_node(L); /* owns L */
    if (clauses_tail)
      clauses_tail = clauses_tail->next = node;
    else
      clauses_head = clauses_tail = node;
  }
  /* Wrapper: an EXP_LAMBDA flagged multi; content holds the clause list. */
  val = make_node(clauses_head);
  val->type = EXP_LAMBDA;
  val->flags |= FLAG_MULTI;
  val->meta = (struct keyval_t *)strdup(exp_text(name));
  if (!(env->d))
    env->d = create_dict();
  set_get_keyval_dict(env->d, exp_text(name), val);
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
        if (env) {
          val->next->meta = (struct keyval_t *)ref_env(env);
          env->has_closure = 1;
        }
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
/* The libffi binding (impl + non-FFI stubs) lives in a dedicated
   #included fragment. */
#include "ffi.h"

/* The standard-library builtins live in a dedicated #included fragment. */
#include "builtins_stdlib.h"

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

/* Control-flow builtins (cond / match / generators) live in a dedicated
   #included fragment. */
#include "builtins_control.h"

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
  case OP_DUP:
    return "DUP";
  case OP_EVAL_AST:
    return "EVAL_AST";
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
  case OP_BIND_SLOT_NAMED:
    return "BIND_SLOT_NAMED";
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
  case OP_SLOT_IS_FIX:
    return "SLOT_IS_FIX";
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
  case OP_ABS:
    return "ABS";
  case OP_NMAX:
    return "MAX";
  case OP_NMIN:
    return "MIN";
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
  case OP_DUP:
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
  case OP_ABS:
  case OP_NMAX:
  case OP_NMIN:
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
  case OP_EVAL_AST:
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
  case OP_BIND_SLOT_NAMED:
    printf("  %04d  %s slot=%d name_const=%d\n", pc, bc_opname(op),
           (int)code[pc + 1], (int)code[pc + 2]);
    return 3;
  case OP_SLOT_ADD_FIX:
  case OP_SLOT_SUB_FIX:
  case OP_SLOT_LT_FIX:
  case OP_SLOT_LE_FIX:
  case OP_SLOT_GT_FIX:
  case OP_SLOT_GE_FIX:
  case OP_SLOT_IS_FIX: {
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
/* JIT backends live in dedicated #included fragments (one TU — see each
   file's header). The arch guards pick exactly one backend per build. */
#include "jit_common.h"
#if defined(__aarch64__)
#include "jit_arm64.h"
#elif defined(__x86_64__)
#include "jit_amd64.h"
#endif
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
/* Emit a NAMED slot binding for a let/with/for-counter local. Storing the
   name (as a const symbol) lets an OP_EVAL_AST sub-form resolve the local by
   symbol at runtime; plain OP_BIND_SLOT leaves a NULL key and is kept only for
   for's hidden, nameless end-value slot. */
static void emit_bind_named(compiler_t *c, int slot, exp_t *var) {
  int nk = add_const(c, var);
  if (c->failed)
    return;
  emit_u8(c, OP_BIND_SLOT_NAMED);
  emit_u8(c, (uint8_t)slot);
  emit_u8(c, (uint8_t)nk);
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
  case OP_IS:
    return OP_SLOT_IS_FIX;
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
  if (is_binary && (op == OP_ADD || op == OP_MUL || op == OP_IS) &&
      isnumber(arg1) && issymbol(arg2)) {
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
  emit_bind_named(c, slot, var);
  if (c->failed)
    return;
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
    emit_bind_named(c, c->nslots, var);
    if (c->failed)
      return;
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
    emit_bind_named(c, c->nslots, var);
    if (c->failed)
      return;
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

/* Backpatch helper: write the i16 at code[patch..patch+1] as the relative
   offset from end-of-operand (patch+2) to `target`. Mirrors the open-coded
   fixups in compile_if/compile_when_unless. */
static void patch_i16(compiler_t *c, int patch, int target) {
  int16_t off = (int16_t)(target - (patch + 2));
  c->code[patch] = off & 0xff;
  c->code[patch + 1] = (off >> 8) & 0xff;
}

/* Cap on conditional branches we can backpatch per form. Each clause needs
   one fixup slot; 64 is far past any hand-written and/or/cond/case. Above it
   we bail to the AST tree-walker (c->failed) rather than truncate. */
#define COND_MAX_PATCHES 64

/* (and a b ... z) / (or a b ... z) — short-circuit, *value-preserving*.
   `and` returns the first falsy operand (else the last); `or` the first
   truthy (else the last). The deciding value must survive the branch, but
   OP_BR_IF_* pop their test — so DUP first: the branch pops one copy and,
   on short-circuit, jumps with the other still live; on fall-through we POP
   that survivor and evaluate the next operand. The final operand needs no
   guard and inherits the caller's tail flag, so a tail call inside
   `(and ... (f x))` keeps O(1)-stack TCO instead of bailing to AST. */
static void compile_and_or(compiler_t *c, exp_t *form, int tail, int is_or) {
  exp_t *args = cdr(form);
  if (!args) { /* (and) -> t ; (or) -> nil */
    int k = add_const(c, is_or ? NIL_EXP : TRUE_EXP);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
    return;
  }
  uint8_t br = is_or ? OP_BR_IF_TRUE : OP_BR_IF_FALSE;
  int patches[COND_MAX_PATCHES];
  int npatch = 0;
  for (exp_t *a = args; a; a = a->next) {
    int is_last = (a->next == NULL);
    compile_expr(c, a->content, is_last && tail);
    if (c->failed)
      return;
    if (is_last)
      break;
    if (npatch >= COND_MAX_PATCHES) {
      c->failed = 1;
      return;
    }
    emit_u8(c, OP_DUP);
    emit_u8(c, br);
    patches[npatch++] = c->ncode;
    emit_i16(c, 0);
    emit_u8(c, OP_POP); /* fall-through: drop the survivor, try next operand */
  }
  int end_target = c->ncode;
  for (int i = 0; i < npatch; i++)
    patch_i16(c, patches[i], end_target);
}

/* (cond t1 e1 t2 e2 ... [default]) — Arc-style flat cond. A lone trailing
   element is the unconditional default; with no default and every test
   falsy the result is nil. Tests compile in non-tail position; the selected
   expr (and the default) inherit the caller's tail flag. Uses only the
   existing popping branch — tests aren't returned, so no DUP needed. */
static void compile_cond(compiler_t *c, exp_t *form, int tail) {
  exp_t *cur = cdr(form);
  if (!cur) { /* (cond) -> nil */
    int k = add_const(c, NIL_EXP);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
    return;
  }
  int end_patches[COND_MAX_PATCHES];
  int n_end = 0;
  int had_default = 0;
  while (cur) {
    if (!cur->next) { /* lone trailing default */
      compile_expr(c, cur->content, tail);
      if (c->failed)
        return;
      had_default = 1;
      break;
    }
    compile_expr(c, cur->content, 0); /* test */
    if (c->failed)
      return;
    emit_u8(c, OP_BR_IF_FALSE);
    int patch_next = c->ncode;
    emit_i16(c, 0);
    compile_expr(c, cur->next->content, tail); /* paired expr */
    if (c->failed)
      return;
    if (n_end >= COND_MAX_PATCHES) {
      c->failed = 1;
      return;
    }
    emit_u8(c, OP_JUMP);
    end_patches[n_end++] = c->ncode;
    emit_i16(c, 0);
    patch_i16(c, patch_next, c->ncode); /* false -> next clause */
    cur = cur->next->next;
  }
  if (!had_default) { /* no clause matched -> nil */
    int k = add_const(c, NIL_EXP);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
  }
  int end_target = c->ncode;
  for (int i = 0; i < n_end; i++)
    patch_i16(c, end_patches[i], end_target);
}

/* (case key v1 e1 v2 e2 ... [default]) — Arc-style flat pairs. The vN are
   UNEVALUATED literals matched to the evaluated key via isequal (OP_IS,
   exactly as casecmd does). The key is evaluated once, then DUP'd per
   comparison; the selected expr (or trailing default) inherits the tail
   flag. Every path discards the saved key before leaving its value. */
static void compile_case(compiler_t *c, exp_t *form, int tail) {
  exp_t *key = cadr(form);
  if (!key) {
    c->failed = 1;
    return;
  }
  compile_expr(c, key, 0); /* stack: [key] */
  if (c->failed)
    return;
  exp_t *cur = cddr(form); /* first vN */
  int end_patches[COND_MAX_PATCHES];
  int n_end = 0;
  int had_default = 0;
  while (cur) {
    if (!cur->next) {     /* lone trailing default */
      emit_u8(c, OP_POP); /* discard saved key */
      compile_expr(c, cur->content, tail);
      if (c->failed)
        return;
      had_default = 1;
      break;
    }
    emit_u8(c, OP_DUP);                 /* [key, key] */
    int k = add_const(c, cur->content); /* literal vN */
    if (c->failed)
      return;
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k); /* [key, key, vN] */
    emit_u8(c, OP_IS);      /* [key, bool] (isequal) */
    emit_u8(c, OP_BR_IF_FALSE);
    int patch_next = c->ncode; /* false -> next clause, [key] */
    emit_i16(c, 0);
    emit_u8(c, OP_POP); /* matched: drop saved key, eval paired expr */
    compile_expr(c, cur->next->content, tail);
    if (c->failed)
      return;
    if (n_end >= COND_MAX_PATCHES) {
      c->failed = 1;
      return;
    }
    emit_u8(c, OP_JUMP);
    end_patches[n_end++] = c->ncode;
    emit_i16(c, 0);
    patch_i16(c, patch_next, c->ncode);
    cur = cur->next->next;
  }
  if (!had_default) { /* no match -> drop key, result nil */
    emit_u8(c, OP_POP);
    int k = add_const(c, NIL_EXP);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
  }
  int end_target = c->ncode;
  for (int i = 0; i < n_end; i++)
    patch_i16(c, end_patches[i], end_target);
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
  emit_bind_named(c, counter_slot, var);
  if (c->failed)
    return;
  c->slot_names[counter_slot] = (char *)exp_text(var);
  c->nslots++;

  compile_expr(c, end_node->content, 0);
  if (c->failed)
    return;
  emit_u8(c, OP_BIND_SLOT); /* hidden end-value slot: nameless, NULL key */
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
    if (!strcmp(s, "and")) {
      compile_and_or(c, e, tail, 0);
      return;
    }
    if (!strcmp(s, "or")) {
      compile_and_or(c, e, tail, 1);
      return;
    }
    if (!strcmp(s, "cond")) {
      compile_cond(c, e, tail);
      return;
    }
    if (!strcmp(s, "case")) {
      compile_case(c, e, tail);
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
    /* abs (unary) / max,min (binary) get native opcodes instead of the
       OP_EVAL_AST tree-walk — the common arity. Wrong/variadic arity falls
       through to OP_EVAL_AST below (where abscmd/maxcmd/mincmd handle it,
       including variadic max/min over >2 args). */
    if (!strcmp(s, "abs")) {
      exp_t *a = cadr(e);
      if (a && !cddr(e)) {
        compile_expr(c, a, 0);
        if (c->failed)
          return;
        emit_u8(c, OP_ABS);
        return;
      }
    }
    if (!strcmp(s, "max") || !strcmp(s, "min")) {
      exp_t *a = cadr(e), *b = caddr(e);
      if (a && b && !cdddr(e)) {
        compile_expr(c, a, 0);
        if (c->failed)
          return;
        compile_expr(c, b, 0);
        if (c->failed)
          return;
        emit_u8(c, !strcmp(s, "max") ? OP_NMAX : OP_NMIN);
        return;
      }
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
    /* A head that resolves to an EXP_INTERNAL we haven't whitelisted above is a
       builtin with no native opcode. Two sub-cases:
         - TAIL_AWARE special forms (match, for-gen, …) and closure-creating
           forms (fn/lambda/def/defn/defmacro/macro): keep the wholesale-AST
           fallback. TAIL_AWARE forms need the tree-walker's own tail-marker
           trampoline for their tail recursion; closure-creators would capture
           THIS compiled frame's env (whose inline slots die on return) and
           dangle. Both must run with the whole lambda as AST.
         - everything else (max, abs, string-*, reverse, map, …): a value
           sub-expression, never the tail. Emit OP_EVAL_AST so just THIS form
           defers to the tree-walker while the enclosing lambda stays compiled
           (and keeps its tail call). Fixes the deep-recursion segfault any
           non-whitelisted builtin in a hot body used to cause.
       OP_EVAL_AST resolves the sub-form's free vars BY NAME via the
       tree-walker; params (param_keys) plus let/with/for-counter locals (now
       bound with OP_BIND_SLOT_NAMED) plus captures (parent chain) are all
       name-resolvable, so it is safe even inside a let/with/for body. User
       lambdas (not in reserved_symbol) fall through to compile_call. */
    keyval_t *kv = set_get_keyval_dict(reserved_symbol, (char *)s, NULL);
    if (kv && isinternal(kv->val)) {
      if ((kv->val->flags & FLAG_TAIL_AWARE) || !strcmp(s, "fn") ||
          !strcmp(s, "lambda") || !strcmp(s, "def") || !strcmp(s, "defn") ||
          !strcmp(s, "defc") || !strcmp(s, "defmacro") || !strcmp(s, "macro")) {
        c->failed = 1;
        return;
      }
      int k = add_const(c, e);
      if (c->failed)
        return;
      emit_u8(c, OP_EVAL_AST);
      emit_u8(c, (uint8_t)k);
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
      [OP_DUP] = &&l_dup,
      [OP_EVAL_AST] = &&l_eval_ast,
      [OP_BIND_SLOT_NAMED] = &&l_bind_slot_named,
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
      [OP_SLOT_IS_FIX] = &&l_slot_is_fix,
      [OP_SLOT_LE_SLOT] = &&l_slot_le_slot,
      [OP_VEC_REF] = &&l_vec_ref,
      [OP_VEC_SET] = &&l_vec_set,
      [OP_VEC_LEN] = &&l_vec_len,
      [OP_VEC_NEW] = &&l_vec_new,
      [OP_SQRT_INT] = &&l_sqrt_int,
      [OP_ABS] = &&l_abs,
      [OP_NMAX] = &&l_max,
      [OP_NMIN] = &&l_min,
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

l_dup: {
  exp_t *t = stack[sp - 1];
  PUSH(refexp(t));
  NEXT;
}

l_eval_ast: {
  /* Run one stored raw sub-form through the tree-walker — the escape hatch for
     builtins with no native opcode. in_tail_position must be 0: the VM owns TCO
     and this form is a value sub-expression (never the tail), so a stray tail
     marker would be mis-handled. EVAL consumes the ref it's handed, so pass a
     fresh one; the const keeps its own. lookup() walks inline slots + the
     parent chain, so locals and captures resolve as in full-AST mode; nested
     user-lambda calls re-enter the compiled path via invoke(). */
  uint8_t idx = READ_U8;
  int saved_tail = in_tail_position;
  in_tail_position = 0;
  exp_t *r = EVAL(refexp(consts[idx]), env);
  in_tail_position = saved_tail;
  if (r && iserror(r)) {
    while (sp > 0)
      unrefexp(POP());
    if (fn_owned)
      unrefexp(fn);
    return r;
  }
  if (!r)
    r = NIL_EXP;
  PUSH(r);
  NEXT;
}

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
l_bind_slot_named: {
  /* Like OP_BIND_SLOT, but records the slot's NAME so an OP_EVAL_AST sub-form
     can resolve this let/with/for-counter local by name (symbolic lookup skips
     NULL-keyed slots). The name points into a const symbol, which outlives the
     frame. lookup() scans inline slots innermost-first, so a same-named inner
     binding correctly shadows an outer one. */
  uint8_t idx = READ_U8;
  uint8_t name_idx = READ_U8;
  exp_t *v = POP();
  env->inline_vals[idx] = v;
  env->inline_keys[idx] = (char *)exp_text(consts[name_idx]);
  if (idx >= env->n_inline)
    env->n_inline = idx + 1;
  NEXT;
}
l_unbind_slot: {
  /* let/with exit: release the binding. destroy_env would catch any
     leftover refs via n_inline, but we explicitly clear here so the
     slot is reusable for subsequent lets. Clear the key too — a freed slot
     must not stay name-visible to a later OP_EVAL_AST symbolic lookup. */
  uint8_t idx = READ_U8;
  unrefexp(env->inline_vals[idx]);
  env->inline_vals[idx] = NULL;
  env->inline_keys[idx] = NULL;
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
l_slot_is_fix: {
  /* (is slot K) with a fixnum immediate is exactly a tagged-pointer bit
     compare — no numeric coercion, no error path: a non-fixnum slot value
     (float / char / heap ptr) simply isn't bit-equal to MAKE_FIX(imm) and
     yields nil, matching isequal's fixnum-vs-other behavior. */
  uint8_t idx = READ_U8;
  int16_t imm = READ_I16;
  exp_t *a = env->inline_vals[idx];
  PUSH(a == MAKE_FIX((int64_t)imm) ? TRUE_EXP : NIL_EXP);
  NEXT;
}

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
l_abs: {
  /* (abs a) — mirrors abscmd. Fixnum: |v|, promoting FIXMIN to float on
     overflow. Float: fabs. A fixnum operand is an immediate (no ref); a
     float is heap (drop its ref after reading). */
  exp_t *a = POP();
  if (isnumber(a)) {
    int64_t v = FIX_VAL(a);
    int64_t av = v < 0 ? -v : v;
    PUSH((v < 0 && !FIX_FITS(av)) ? make_floatf(-(expfloat)v) : MAKE_FIX(av));
  } else if (isfloat(a)) {
    expfloat f = a->f;
    unrefexp(a);
    PUSH(make_floatf(f < 0 ? -f : f));
  } else {
    unrefexp(a);
    RUNTIME_ERR("abs: not a number");
  }
  NEXT;
}
l_max: {
  /* (max a b) — value-preserving (returns the actual larger operand, keeping
     its int/float type), via alc_numlt (= maxcmd's comparator). Keep the
     winner's ref, drop the loser's. Equal → keep a (matches maxcmd's fold). */
  exp_t *b = POP(), *a = POP();
  int err;
  int a_lt_b = alc_numlt(a, b, &err);
  if (err) {
    unrefexp(a);
    unrefexp(b);
    RUNTIME_ERR("max: non-numeric");
  }
  if (a_lt_b) {
    unrefexp(a);
    PUSH(b);
  } else {
    unrefexp(b);
    PUSH(a);
  }
  NEXT;
}
l_min: {
  /* (min a b) — symmetric to OP_MAX; keep the smaller. Equal → keep a. */
  exp_t *b = POP(), *a = POP();
  int err;
  int b_lt_a = alc_numlt(b, a, &err);
  if (err) {
    unrefexp(a);
    unrefexp(b);
    RUNTIME_ERR("min: non-numeric");
  }
  if (b_lt_a) {
    unrefexp(a);
    PUSH(b);
  } else {
    unrefexp(b);
    PUSH(a);
  }
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
static exp_t *multi_pick(exp_t *clauses, int n); /* defined below, before invoke */

static exp_t *vm_invoke_values(exp_t *fn, int nargs, exp_t **argv, env_t *env) {
  /* Multi-arity (defn) reached from compiled code: dispatch on arg count to
     the matching clause lambda, then run it normally. Before the param-list
     reads below. */
  /* Hot path first: a plain compiled lambda is by far the common callee, so
     test islambda once up front and jump straight to the bind+run code,
     skipping the rare-callee probes (container / continuation / ffi) below.
     Those probes are only worth running when fn is NOT a lambda. */
  if (islambda(fn)) {
    if (fn->flags & FLAG_MULTI) {
      exp_t *chosen = multi_pick(fn->content, nargs);
      if (!chosen) {
        for (int i = 0; i < nargs; i++)
          unrefexp(argv[i]);
        return error(ERROR_MISSING_PARAMETER, fn, env,
                     "no matching clause for %d argument(s)", nargs);
      }
      return vm_invoke_values(chosen, nargs, argv, env);
    }
    goto bind_lambda;
  }
  /* fn is not a lambda — the rare callee shapes, each of which returns. */
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
bind_lambda:; /* a plain (non-MULTI) lambda jumps straight here */
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

/* Pick the clause lambda from a FLAG_MULTI wrapper whose param list matches
   `n` args: an exact fixed-arity match, or a variadic (. rest) clause whose
   fixed count is <= n. First match in definition order wins; NULL if none. */
static exp_t *multi_pick(exp_t *clauses, int n) {
  for (exp_t *c = clauses; c && c->content; c = c->next) {
    exp_t *L = c->content;
    int fixed = 0, variadic = 0;
    for (exp_t *p = lambda_params(L); p && p->content; p = p->next) {
      if (issymbol(p->content) &&
          strcmp((char *)exp_text(p->content), ".") == 0) {
        variadic = 1;
        break;
      }
      fixed++;
    }
    if (variadic ? (n >= fixed) : (n == fixed))
      return L;
  }
  return NULL;
}

exp_t *invoke(exp_t *e, exp_t *fn, env_t *env) {
  /* e->content = fn name, e->next = args list,
     fn->content = params list, fn->next->content = body list.

     We hold a ref on `fn` across the invocation so that its header
     symbols (whose ->ptr we borrow into env->inline_keys) can never be
     freed while the env is live. Tail calls reuse the frame via a
     trampoline loop — O(1) C stack for tail recursion. */

  /* Multi-arity (defn): dispatch on argument count to the matching clause,
     then run that ordinary clause lambda through the normal path. Must come
     before any read of fn->content as a param list. */
  if (fn->flags & FLAG_MULTI) {
    int n = 0;
    for (exp_t *a = e->next; a; a = a->next)
      n++;
    exp_t *chosen = multi_pick(fn->content, n);
    if (!chosen) {
      exp_t *er = error(ERROR_MISSING_PARAMETER, e, env,
                        "no matching clause for %d argument(s)", n);
      unrefexp(e);
      return er;
    }
    return invoke(e, chosen, env); /* consumes e; refexps chosen internally */
  }

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

#endif /* ALCOVE_ALS */

/* TAB at the start of a line — column 0, or only whitespace before the cursor
   — inserts one indent level instead of triggering completion. With a real
   token to the left it completes as usual, so `(fi<TAB>` etc. keep working.
   Shared by both binaries: convenient indentation in plain alcove, and
   required by Adder's whitespace-significant syntax. */
#define ALCOVE_INDENT "  " /* one indent level = 2 spaces */
static int alcove_smart_tab(int count, int key) {
  for (int i = 0; i < rl_point; i++)
    if (rl_line_buffer[i] != ' ' && rl_line_buffer[i] != '\t')
      return rl_complete(count, key); /* a token precedes -> complete */
  rl_insert_text(ALCOVE_INDENT);
  return 0;
}

/* Shift-TAB (back-tab, the ESC[Z sequence) dedents: removes up to one indent
   level (2 spaces) immediately before the cursor, when present. A no-op if the
   cursor isn't preceded by spaces. The inverse of alcove_smart_tab's indent. */
static int alcove_back_tab(int count, int key) {
  (void)count;
  (void)key;
  int k = 0;
  while (k < 2 && rl_point - k - 1 >= 0 &&
         rl_line_buffer[rl_point - k - 1] == ' ')
    k++;
  if (k > 0) {
    rl_delete_text(rl_point - k, rl_point);
    rl_point -= k;
  }
  return 0;
}
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

/* Lisp-value hash-map builtins live in a dedicated #included fragment. */
#include "builtins_dict.h"

/* hash-set (EXP_SET) ops live in a dedicated #included fragment (on dict.h). */
#include "set.h"

/* HAMT ops + builtins live in a dedicated #included fragment. */
#include "hamt.h"

/* MessagePack codec lives in a dedicated #included fragment. */
#include "msgpack.h"

/* deque (EXP_LIST) ops live in a dedicated #included fragment. */
#include "deque.h"
/* blob (EXP_BLOB) ops live in a dedicated #included fragment. */
#include "blob.h"

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
  /* TAB indents at line start (only whitespace precedes), else completes. */
  rl_bind_key('\t', alcove_smart_tab);
  /* Shift-TAB (back-tab, ESC[Z) dedents by up to one indent level. */
  rl_bind_keyseq("\033[Z", alcove_back_tab);
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
