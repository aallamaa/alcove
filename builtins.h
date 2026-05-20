#ifndef ALCOVE_BUILTINS_H
#define ALCOVE_BUILTINS_H

#include "alcove.h"

/* Forward declarations for doc strings defined alongside each cmd
   function. Each is `static const char doc_<symbolname>[]` so a future
   reader can grep `doc_+` to land directly on the help text + the body. */
extern const char doc_quote[], doc_quasiquote[], doc_if[], doc_do[];
extern const char doc_when[], doc_while[], doc_repeat[];
extern const char doc_and[], doc_or[], doc_case[], doc_for[], doc_each[];
extern const char doc_let[], doc_letstar[], doc_with[];
exp_t *letstar_cmd(exp_t *e, env_t *env);
extern const char doc_eq[], doc_lt[], doc_gt[], doc_le[], doc_ge[];
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
extern const char doc_vec[], doc_vecref[], doc_vecset[], doc_veclen[];
extern const char doc_def[], doc_fn[], doc_defmacro[], doc_macroexpand[];
extern const char doc_eval[], doc_apply[], doc_setq[];
extern const char doc_map[], doc_filter[], doc_reduce[], doc_any[], doc_all[];
extern const char doc_numberp[], doc_stringp[], doc_symbolp[], doc_pairp[], doc_fnp[];
extern const char doc_listp[], doc_nullp[], doc_gensym[];
extern const char doc_take[], doc_drop[], doc_range[], doc_zip[], doc_flatten[];
extern const char doc_sort[], doc_sortby[];
extern const char doc_stringcontainsp[], doc_stringindex[], doc_stringreplace[];
extern const char doc_errorp[], doc_errormessage[], doc_try[];
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
extern const char doc_pr[], doc_prn[];
extern const char doc_str[], doc_fmt[], doc_substr[], doc_stringappend[],
                  doc_stringsplit[], doc_stringjoin[], doc_stringtrim[],
                  doc_stringupcase[], doc_stringdowncase[];
extern const char doc_readstring[], doc_writestring[], doc_appendstring[],
                  doc_readlines[], doc_fileexistsp[], doc_writebytes[],
                  doc_load[];
extern const char doc_persist[], doc_forget[], doc_unpersist[];
extern const char doc_savedb[], doc_loaddb[];
extern const char doc_ispersistent[];
extern const char doc_inspect[], doc_disasm[], doc_source[], doc_dir[];
extern const char doc_time[], doc_exit[], doc_webp[], doc_sleepms[];
exp_t *webpcmd(exp_t *e, env_t *env);
exp_t *sleepmscmd(exp_t *e, env_t *env);
extern const char doc_ffifn[];
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
extern const char doc_makeblob[], doc_bloblen[], doc_blobref[],
                  doc_blob2string[], doc_string2blob[], doc_readbytes[];
extern const char doc_vecdot[], doc_vecaxpy[], doc_vecscale[], doc_vecadd[],
                  doc_vecfill[], doc_vecrelu[], doc_vecargmax[], doc_vecmax[],
                  doc_veccopy[];
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
exp_t *vecdotcmd(exp_t *e, env_t *env);
exp_t *vecaxpycmd(exp_t *e, env_t *env);
exp_t *vecscalecmd(exp_t *e, env_t *env);
exp_t *vecaddcmd(exp_t *e, env_t *env);
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
  PTR_TYPE *val_ptr = (PTR_TYPE *)obj->ptr; \
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
