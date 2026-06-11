#ifndef ALCOVE_BUILTINS_H
#define ALCOVE_BUILTINS_H

#include "alcove.h"

/* Forward declarations for doc strings defined alongside each cmd
   function. Each is `static const char doc_<symbolname>[]` so a future
   reader can grep `doc_+` to land directly on the help text + the body. */
extern const char doc_quote[], doc_quasiquote[], doc_if[], doc_do[];
extern const char doc_when[], doc_unless[], doc_while[], doc_repeat[];
exp_t *unlesscmd(exp_t *e, env_t *env);
extern const char doc_and[], doc_or[], doc_case[], doc_cond[], doc_for[], doc_each[];
extern const char doc_match[], doc_forgen[];
exp_t *condcmd(exp_t *e, env_t *env);
exp_t *matchcmd(exp_t *e, env_t *env);
exp_t *forgencmd(exp_t *e, env_t *env);
extern const char doc_let[], doc_letstar[], doc_with[];
exp_t *letstar_cmd(exp_t *e, env_t *env);
extern const char doc_gendone[], doc_gendonep[], doc_genlist[], doc_genrange[];
extern const char doc_gennext[], doc_gencollect[], doc_genmap[], doc_genfilter[];
exp_t *gendone_cmd(exp_t *e, env_t *env);
exp_t *gendonep_cmd(exp_t *e, env_t *env);
exp_t *genlist_cmd(exp_t *e, env_t *env);
exp_t *genrange_cmd(exp_t *e, env_t *env);
exp_t *gennext_cmd(exp_t *e, env_t *env);
exp_t *gencollect_cmd(exp_t *e, env_t *env);
exp_t *genmap_cmd(exp_t *e, env_t *env);
exp_t *genfilter_cmd(exp_t *e, env_t *env);
extern const char doc_eq[], doc_setf[], doc_lt[], doc_gt[], doc_le[], doc_ge[];
extern const char doc_is[], doc_iso[], doc_in[], doc_no[];
extern const char doc_plus[], doc_mul[], doc_minus[], doc_div[];
extern const char doc_mod[], doc_abs[], doc_max[], doc_min[], doc_odd[];
extern const char doc_bitand[], doc_bitor[], doc_bitxor[], doc_bitnot[];
extern const char doc_shl[], doc_shr[];
extern const char doc_sqrt[], doc_sqrtint[], doc_exp[], doc_expt[], doc_random[];
extern const char doc_round[], doc_floor[], doc_ceil[], doc_truncate[];
extern const char doc_log[], doc_sin[], doc_cos[], doc_tan[];
extern const char doc_float[], doc_int[];
exp_t *roundcmd(exp_t *e, env_t *env);
exp_t *floorcmd(exp_t *e, env_t *env);
exp_t *ceilcmd(exp_t *e, env_t *env);
exp_t *truncatecmd(exp_t *e, env_t *env);
exp_t *logcmd(exp_t *e, env_t *env);
exp_t *sincmd(exp_t *e, env_t *env);
exp_t *coscmd(exp_t *e, env_t *env);
exp_t *tancmd(exp_t *e, env_t *env);
exp_t *floatcmd(exp_t *e, env_t *env);
exp_t *intcmd(exp_t *e, env_t *env);
extern const char doc_cons[], doc_car[], doc_cdr[], doc_list[];
extern const char doc_length[], doc_nth[], doc_reverse[], doc_append[];
extern const char doc_seq[], doc_first[], doc_rest[];
extern const char doc_conj[], doc_into[];
extern const char doc_typeof[], doc_defstruct[];
extern const char doc_defmulti[], doc_defmethod[];
extern const char doc_vec[], doc_vecref[], doc_vecset[], doc_veclen[];
extern const char doc_def[], doc_defn[], doc_fn[], doc_defc[], doc_defmacro[],
    doc_macroexpand[];
extern const char doc_eval[], doc_apply[], doc_setq[];
extern const char doc_map[], doc_filter[], doc_reduce[], doc_any[], doc_all[];
extern const char doc_numberp[], doc_stringp[], doc_symbolp[], doc_pairp[], doc_fnp[];
extern const char doc_listp[], doc_nullp[], doc_gensym[], doc_withgensyms[];
exp_t *withgensymscmd(exp_t *e, env_t *env);
extern const char doc_take[], doc_drop[], doc_range[], doc_zip[], doc_flatten[];
extern const char doc_sort[], doc_sortby[];
extern const char doc_stringcontainsp[], doc_stringindex[], doc_stringreplace[];
extern const char doc_errorp[], doc_errormessage[], doc_try[];
extern const char doc_callcc[];
exp_t *errorpcmd(exp_t *e, env_t *env);
exp_t *errormessagecmd(exp_t *e, env_t *env);
exp_t *trycmd(exp_t *e, env_t *env);
exp_t *listpcmd(exp_t *e, env_t *env);
exp_t *nullpcmd(exp_t *e, env_t *env);
exp_t *gensymcmd(exp_t *e, env_t *env);
exp_t *takecmd(exp_t *e, env_t *env);
exp_t *dropcmd(exp_t *e, env_t *env);
exp_t *rangecmd(exp_t *e, env_t *env);
exp_t *zipcmd(exp_t *e, env_t *env);
exp_t *flattencmd(exp_t *e, env_t *env);
exp_t *sortcmd(exp_t *e, env_t *env);
exp_t *sortbycmd(exp_t *e, env_t *env);
exp_t *stringcontainspcmd(exp_t *e, env_t *env);
exp_t *stringindexcmd(exp_t *e, env_t *env);
exp_t *stringreplacecmd(exp_t *e, env_t *env);
extern const char doc_vecp[], doc_blobp[], doc_dictp[], doc_dequep[], doc_setp[];
exp_t *vecpcmd(exp_t *e, env_t *env);
exp_t *blobpcmd(exp_t *e, env_t *env);
exp_t *dictpcmd(exp_t *e, env_t *env);
exp_t *dequepcmd(exp_t *e, env_t *env);
exp_t *setpcmd(exp_t *e, env_t *env);
/* Introspection predicates/accessors — expose internal exp_t state as real
   return values (not just printed) so regression tests can assert on the
   bytecode-compilation, JIT, and inline-text optimizations. */
exp_t *compiledpcmd(exp_t *e, env_t *env);
exp_t *jitpcmd(exp_t *e, env_t *env);
exp_t *inlinepcmd(exp_t *e, env_t *env);
exp_t *expflagscmd(exp_t *e, env_t *env);
extern const char doc_compiledp[], doc_jitp[], doc_inlinep[], doc_expflags[];
extern const char doc_backtrace[];
exp_t *backtracecmd(exp_t *e, env_t *env);

/* builtins_os.h — OS/scripting floor */
extern const char doc_getenv[], doc_setenv[], doc_epr[], doc_eprn[],
                  doc_readline[], doc_deletefile[], doc_renamefile[],
                  doc_makedir[], doc_listdir[], doc_fileinfo[], doc_shell[];
exp_t *getenvcmd(exp_t *e, env_t *env);
exp_t *setenvcmd(exp_t *e, env_t *env);
exp_t *eprcmd(exp_t *e, env_t *env);
exp_t *eprncmd(exp_t *e, env_t *env);
exp_t *readlinecmd(exp_t *e, env_t *env);
exp_t *deletefilecmd(exp_t *e, env_t *env);
exp_t *renamefilecmd(exp_t *e, env_t *env);
exp_t *makedircmd(exp_t *e, env_t *env);
exp_t *listdircmd(exp_t *e, env_t *env);
exp_t *fileinfocmd(exp_t *e, env_t *env);
exp_t *shellcmd(exp_t *e, env_t *env);

/* json.h */
extern const char doc_jsonencode[], doc_jsondecode[];
exp_t *jsonencodecmd(exp_t *e, env_t *env);
exp_t *jsondecodecmd(exp_t *e, env_t *env);

/* blob.h byte codecs */
extern const char doc_base64encode[], doc_base64decode[], doc_hexencode[],
                  doc_hexdecode[];
exp_t *base64encodecmd(exp_t *e, env_t *env);
exp_t *base64decodecmd(exp_t *e, env_t *env);
exp_t *hexencodecmd(exp_t *e, env_t *env);
exp_t *hexdecodecmd(exp_t *e, env_t *env);

/* builtins_stdlib.h — v0.2 stdlib batch */
extern const char doc_groupby[], doc_frequencies[], doc_partition[],
                  doc_interleave[], doc_maxby[], doc_minby[],
                  doc_startswith[], doc_endswith[], doc_stringrepeat[],
                  doc_stringpadleft[], doc_stringpadright[];
exp_t *groupbycmd(exp_t *e, env_t *env);
exp_t *frequenciescmd(exp_t *e, env_t *env);
exp_t *partitioncmd(exp_t *e, env_t *env);
exp_t *interleavecmd(exp_t *e, env_t *env);
exp_t *maxbycmd(exp_t *e, env_t *env);
exp_t *minbycmd(exp_t *e, env_t *env);
exp_t *startswithcmd(exp_t *e, env_t *env);
exp_t *endswithcmd(exp_t *e, env_t *env);
exp_t *stringrepeatcmd(exp_t *e, env_t *env);
exp_t *stringpadleftcmd(exp_t *e, env_t *env);
exp_t *stringpadrightcmd(exp_t *e, env_t *env);

/* builtins_regex.h — POSIX ERE */
extern const char doc_rematch[], doc_refind[], doc_refindall[],
                  doc_rereplace[], doc_resplit[];
exp_t *rematchcmd(exp_t *e, env_t *env);
exp_t *refindcmd(exp_t *e, env_t *env);
exp_t *refindallcmd(exp_t *e, env_t *env);
exp_t *rereplacecmd(exp_t *e, env_t *env);
exp_t *resplitcmd(exp_t *e, env_t *env);
exp_t *docstringcmd(exp_t *e, env_t *env);
extern const char doc_docstring[];
exp_t *withdbcmd(exp_t *e, env_t *env);
extern const char doc_withdb[];
extern const char doc_pr[], doc_prn[];
extern const char doc_str[], doc_fmt[], doc_substr[], doc_stringappend[],
                  doc_stringsplit[], doc_stringjoin[], doc_stringtrim[],
                  doc_stringupcase[], doc_stringdowncase[];
extern const char doc_readstring[], doc_writestring[], doc_appendstring[],
                  doc_readlines[], doc_fileexistsp[], doc_writebytes[],
                  doc_load[], doc_ns[], doc_require[];
extern const char doc_persist[], doc_forget[], doc_unpersist[];
extern const char doc_savedb[], doc_loaddb[];
extern const char doc_ispersistent[];
extern const char doc_inspect[], doc_disasm[], doc_source[], doc_dir[];
extern const char doc_time[], doc_exit[], doc_webp[], doc_sleepms[];
extern const char doc_platform[], doc_arch[], doc_dylibsuffix[];
extern const char doc_nowms[];
extern const char doc_stringbuf[], doc_stringset[], doc_stringfill[],
    doc_stringcopy[];
exp_t *webpcmd(exp_t *e, env_t *env);
exp_t *sleepmscmd(exp_t *e, env_t *env);
exp_t *nowmscmd(exp_t *e, env_t *env);
exp_t *platformcmd(exp_t *e, env_t *env);
exp_t *archcmd(exp_t *e, env_t *env);
exp_t *dylibsuffixcmd(exp_t *e, env_t *env);
exp_t *stringbufcmd(exp_t *e, env_t *env);
exp_t *stringsetcmd(exp_t *e, env_t *env);
exp_t *stringfillcmd(exp_t *e, env_t *env);
exp_t *stringcopycmd(exp_t *e, env_t *env);
extern const char doc_ffip[];
extern const char doc_ffifn[];
extern const char doc_ffivfn[];
extern const char doc_fficallback[];
extern const char doc_ffistruct[];
extern const char doc_ffipack[];
extern const char doc_ffiunpack[];
extern const char doc_doc[], doc_help[];
/* Clojure-style containers (EXP_DICT / EXP_LIST / EXP_BLOB). */
extern const char doc_hashmap[], doc_assocbang[], doc_dissocbang[], doc_get[],
                  doc_containsp[], doc_keys[], doc_vals[], doc_count[];
extern const char doc_deque[], doc_pushrightbang[], doc_pushleftbang[],
                  doc_poprightbang[], doc_popleftbang[],
                  doc_peekleft[], doc_peekright[];
extern const char doc_set[], doc_hashset[], doc_setaddbang[], doc_setdelbang[],
                  doc_sethasp[], doc_setunion[], doc_setintersection[],
                  doc_setdifference[], doc_setlist[];
extern const char doc_hamt[], doc_hamtassoc[], doc_hamtget[], doc_hamtdissoc[],
                  doc_hamtcount[], doc_hamtcontainsp[], doc_hamtkeys[],
                  doc_hamtvals[], doc_hamtlist[], doc_hamtmerge[], doc_hamtp[];
extern const char doc_msgpackencode[], doc_msgpackdecode[];
extern const char doc_makeblob[], doc_bloblen[], doc_blobref[],
                  doc_blob2string[], doc_string2blob[], doc_readbytes[];
extern const char doc_vecdot[], doc_vecaxpy[], doc_vecscale[], doc_vecadd[],
                  doc_vecfill[], doc_vecrelu[], doc_vecargmax[], doc_vecmax[],
                  doc_veccopy[], doc_veccountle[];
extern const char doc_vecmul[], doc_vecsub[], doc_vecsum[], doc_vecmin[],
                  doc_vecargmin[], doc_vecexp[], doc_vecsigmoid[],
                  doc_vectanh[], doc_vecsoftmax[];
exp_t *vecmulcmd(exp_t *e, env_t *env);
exp_t *vecsubcmd(exp_t *e, env_t *env);
exp_t *vecsumcmd(exp_t *e, env_t *env);
exp_t *vecmincmd(exp_t *e, env_t *env);
exp_t *vecargmincmd(exp_t *e, env_t *env);
exp_t *vecexpcmd(exp_t *e, env_t *env);
exp_t *vecsigmoidcmd(exp_t *e, env_t *env);
exp_t *vectanhcmd(exp_t *e, env_t *env);
exp_t *vecsoftmaxcmd(exp_t *e, env_t *env);
extern const char doc_matvec[], doc_matmul[], doc_matvecbang[],
    doc_matvectbang[], doc_vecger[], doc_vecfromblob[];
exp_t *matveccmd(exp_t *e, env_t *env);
exp_t *matvecbangcmd(exp_t *e, env_t *env);
exp_t *matvectbangcmd(exp_t *e, env_t *env);
exp_t *vecgercmd(exp_t *e, env_t *env);
exp_t *vecfromblobcmd(exp_t *e, env_t *env);
exp_t *matmulcmd(exp_t *e, env_t *env);
extern const char doc_vector[];
extern const char doc_vecpush[], doc_vecpop[], doc_vecunshift[], doc_vecshift[];
/* Redis keyspace bridge builtins. Defined below the `#include "resp.c"`
   line so they can use the RESP exp_t-backed keyspace directly. */
extern const char doc_redis_count[], doc_redis_keys[], doc_redis_type[],
                  doc_redis_get[], doc_redis_val[], doc_redis_set[],
                  doc_redis_del[], doc_redis_flush[], doc_redis_port[],
                  doc_redis_defcmd[], doc_redis_undefcmd[], doc_redis_cmds[];

/* Forward decls for cmds defined below the table — every callee must be
   visible at table-init time. The original cmds had a top-level
   #pragma-style sweep (functions all sit in alcove.c above this point);
   newer additions land below and need explicit decls. */
exp_t *quasiquotecmd(exp_t *e, env_t *env);
exp_t *doccmd(exp_t *e, env_t *env);
exp_t *helpcmd(exp_t *e, env_t *env);
exp_t *bitandcmd(exp_t *e, env_t *env);
exp_t *bitorcmd(exp_t *e, env_t *env);
exp_t *bitxorcmd(exp_t *e, env_t *env);
exp_t *bitnotcmd(exp_t *e, env_t *env);
exp_t *shlcmd(exp_t *e, env_t *env);
exp_t *shrcmd(exp_t *e, env_t *env);
exp_t *unpersistcmd(exp_t *e, env_t *env);
/* Clojure-style container cmds — defined below the table. */
exp_t *hashmapcmd(exp_t *e, env_t *env);
exp_t *assocbangcmd(exp_t *e, env_t *env);
exp_t *dissocbangcmd(exp_t *e, env_t *env);
exp_t *getcmd(exp_t *e, env_t *env);
exp_t *containspcmd(exp_t *e, env_t *env);
exp_t *keyscmd(exp_t *e, env_t *env);
exp_t *valscmd(exp_t *e, env_t *env);
exp_t *countcmd(exp_t *e, env_t *env);
exp_t *setqcmd(exp_t *e, env_t *env);
exp_t *dequecmd(exp_t *e, env_t *env);
exp_t *pushrightbangcmd(exp_t *e, env_t *env);
exp_t *pushleftbangcmd(exp_t *e, env_t *env);
exp_t *poprightbangcmd(exp_t *e, env_t *env);
exp_t *popleftbangcmd(exp_t *e, env_t *env);
exp_t *peekleftcmd(exp_t *e, env_t *env);
exp_t *peekrightcmd(exp_t *e, env_t *env);
exp_t *setcmd(exp_t *e, env_t *env);
exp_t *hashsetcmd(exp_t *e, env_t *env);
exp_t *setaddbangcmd(exp_t *e, env_t *env);
exp_t *setdelbangcmd(exp_t *e, env_t *env);
exp_t *sethaspcmd(exp_t *e, env_t *env);
exp_t *setunioncmd(exp_t *e, env_t *env);
exp_t *setintersectioncmd(exp_t *e, env_t *env);
exp_t *setdifferencecmd(exp_t *e, env_t *env);
exp_t *setlistcmd(exp_t *e, env_t *env);
exp_t *makeblobcmd(exp_t *e, env_t *env);
exp_t *bloblencmd(exp_t *e, env_t *env);
exp_t *blobrefcmd(exp_t *e, env_t *env);
exp_t *blob2stringcmd(exp_t *e, env_t *env);
exp_t *string2blobcmd(exp_t *e, env_t *env);
exp_t *readbytescmd(exp_t *e, env_t *env);
exp_t *strcmd(exp_t *e, env_t *env);
exp_t *fmtcmd(exp_t *e, env_t *env);
exp_t *substrcmd(exp_t *e, env_t *env);
exp_t *stringappendcmd(exp_t *e, env_t *env);
exp_t *stringsplitcmd(exp_t *e, env_t *env);
exp_t *stringjoincmd(exp_t *e, env_t *env);
exp_t *stringtrimcmd(exp_t *e, env_t *env);
exp_t *stringupcasecmd(exp_t *e, env_t *env);
exp_t *stringdowncasecmd(exp_t *e, env_t *env);
exp_t *readstringcmd(exp_t *e, env_t *env);
exp_t *writestringcmd(exp_t *e, env_t *env);
exp_t *appendstringcmd(exp_t *e, env_t *env);
exp_t *readlinescmd(exp_t *e, env_t *env);
exp_t *fileexistspcmd(exp_t *e, env_t *env);
exp_t *writebytescmd(exp_t *e, env_t *env);
exp_t *loadcmd(exp_t *e, env_t *env);
exp_t *nscmd(exp_t *e, env_t *env);
exp_t *requirecmd(exp_t *e, env_t *env);
exp_t *vecdotcmd(exp_t *e, env_t *env);
exp_t *vecaxpycmd(exp_t *e, env_t *env);
exp_t *vecscalecmd(exp_t *e, env_t *env);
exp_t *vecaddcmd(exp_t *e, env_t *env);
exp_t *veccountlecmd(exp_t *e, env_t *env);
exp_t *vecfillcmd(exp_t *e, env_t *env);
exp_t *vecrelucmd(exp_t *e, env_t *env);
exp_t *vecargmaxcmd(exp_t *e, env_t *env);
exp_t *vecmaxcmd(exp_t *e, env_t *env);
exp_t *veccopycmd(exp_t *e, env_t *env);
exp_t *vectorcmd(exp_t *e, env_t *env);
exp_t *vecpushcmd(exp_t *e, env_t *env);
exp_t *vecpopcmd(exp_t *e, env_t *env);
exp_t *vecunshiftcmd(exp_t *e, env_t *env);
exp_t *vecshiftcmd(exp_t *e, env_t *env);
exp_t *rediscountcmd(exp_t *e, env_t *env);
exp_t *rediskeyscmd(exp_t *e, env_t *env);
exp_t *redistypecmd(exp_t *e, env_t *env);
exp_t *redisgetcmd(exp_t *e, env_t *env);
exp_t *redisvalcmd(exp_t *e, env_t *env);
exp_t *redissetcmd(exp_t *e, env_t *env);
exp_t *redisdelcmd(exp_t *e, env_t *env);
exp_t *redisflushcmd(exp_t *e, env_t *env);
exp_t *redisportcmd(exp_t *e, env_t *env);
exp_t *rediscmddefcmd(exp_t *e, env_t *env);
exp_t *rediscmdundefcmd(exp_t *e, env_t *env);
exp_t *rediscmdscmd(exp_t *e, env_t *env);


/* ---------------- Macros for Builtin Commands ---------------- */











/* Segfault guard shared by map/filter/reduce/any?/all?/nth/reverse/append:
   a tagged immediate (fixnum, char) passes a bare `cur != NULL` walk and then
   dereferences its tag bits via cur->content. nil/the empty list is fine.
   This is ONLY the predicate — callers keep their own unref cascade + error
   id (NULL vs e), so the macro changes nothing about behavior. */
#define NOT_A_LIST(x) ((x) && (x) != NIL_EXP && !ispair(x))

#define DICT_KV_SETUP(err_name) \
  exp_t *d = EVAL(cadr(e), env); \
  if (iserror(d)) { unrefexp(e); return d; } \
  if (!isdict(d)) { unrefexp(d); unrefexp(e); \
    return error(ERROR_ILLEGAL_VALUE, NULL, env, err_name ": first arg must be a hash-map"); } \
  exp_t *k = EVAL(caddr(e), env); \
  if (iserror(k)) { unrefexp(d); unrefexp(e); return k; } \
  char tmp[32]; \
  char *ks = alc_key_to_cstr(k, tmp);


#define DICT_ITER_CMD(name, err_name, node_val) \
exp_t *name(exp_t *e, env_t *env) { \
  exp_t *d = EVAL(cadr(e), env); \
  if (iserror(d)) { unrefexp(e); return d; } \
  if (!isdict(d)) { unrefexp(d); unrefexp(e); \
    return error(ERROR_ILLEGAL_VALUE, NULL, env, err_name ": arg must be a hash-map"); } \
  dict_t *dp = (dict_t *)d->ptr; \
  exp_t *ret = NIL_EXP; \
  exp_t *cur = NULL; \
  unsigned int i; \
  for (i = 0; i < dp->ht[0].size; i++) { \
    keyval_t *k = dp->ht[0].table[i]; \
    while (k) { \
      exp_t *node = make_node(node_val); \
      if (cur) cur = cur->next = node; \
      else { ret = cur = node; } \
      k = k->next; \
    } \
  } \
  unrefexp(d); unrefexp(e); \
  return ret ? ret : NIL_EXP; \
}


#define UNARY_TYPE_CMD(name, err_str, TYPE_CHECK, PTR_TYPE, RET_EXPR) \
exp_t *name(exp_t *e, env_t *env) { \
  exp_t *obj = EVAL(cadr(e), env); \
  if (iserror(obj)) { unrefexp(e); return obj; } \
  if (!(TYPE_CHECK(obj))) { unrefexp(obj); unrefexp(e); \
    return error(ERROR_ILLEGAL_VALUE, NULL, env, err_str); } \
  PTR_TYPE *val_ptr = (PTR_TYPE *)exp_text(obj); \
  exp_t *ret = (RET_EXPR); \
  unrefexp(obj); unrefexp(e); \
  return ret; \
}


#define PAIR_PART_CMD(name, part_macro) \
exp_t *name(exp_t *e, env_t *env) { \
  EVAL_ARG_1(a); \
  exp_t *ret = refexp(part_macro(a)); \
  CLEAN_RETURN_1(a, ret); \
}

#define EQUALITY_CMD(name, eq_func) \
exp_t *name(exp_t *e, env_t *env) { \
  EVAL_ARG_2(a, b); \
  exp_t *ret = (eq_func(a, b) ? TRUE_EXP : NIL_EXP); \
  CLEAN_RETURN_2(a, b, ret); \
}

#endif /* ALCOVE_BUILTINS_H */
