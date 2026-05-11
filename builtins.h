#ifndef ALCOVE_BUILTINS_H
#define ALCOVE_BUILTINS_H

#include "alcove.h"

/* Forward declarations for doc strings defined alongside each cmd
   function. Each is `static const char doc_<symbolname>[]` so a future
   reader can grep `doc_+` to land directly on the help text + the body. */
extern const char doc_quote[], doc_if[], doc_do[];
extern const char doc_when[], doc_while[], doc_repeat[];
extern const char doc_and[], doc_or[], doc_case[], doc_for[], doc_each[];
extern const char doc_let[], doc_with[];
extern const char doc_eq[], doc_lt[], doc_gt[], doc_le[], doc_ge[];
extern const char doc_is[], doc_iso[], doc_in[], doc_no[];
extern const char doc_plus[], doc_mul[], doc_minus[], doc_div[];
extern const char doc_mod[], doc_abs[], doc_max[], doc_min[], doc_odd[];
extern const char doc_bitand[], doc_bitor[], doc_bitxor[], doc_bitnot[];
extern const char doc_shl[], doc_shr[];
extern const char doc_sqrt[], doc_sqrtint[], doc_exp[], doc_expt[], doc_random[];
extern const char doc_cons[], doc_car[], doc_cdr[], doc_list[];
extern const char doc_length[], doc_nth[], doc_reverse[], doc_append[];
extern const char doc_vec[], doc_vecref[], doc_vecset[], doc_veclen[];
extern const char doc_def[], doc_fn[], doc_defmacro[], doc_macroexpand[];
extern const char doc_eval[], doc_apply[];
extern const char doc_map[], doc_filter[], doc_reduce[], doc_any[], doc_all[];
extern const char doc_numberp[], doc_stringp[], doc_symbolp[], doc_pairp[], doc_fnp[];
extern const char doc_vecp[], doc_blobp[], doc_dictp[], doc_dequep[];
exp_t *vecpcmd(exp_t *e, env_t *env);
exp_t *blobpcmd(exp_t *e, env_t *env);
exp_t *dictpcmd(exp_t *e, env_t *env);
exp_t *dequepcmd(exp_t *e, env_t *env);
extern const char doc_pr[], doc_prn[];
extern const char doc_persist[], doc_forget[], doc_unpersist[];
extern const char doc_savedb[], doc_loaddb[];
extern const char doc_ispersistent[];
extern const char doc_inspect[], doc_disasm[], doc_source[], doc_dir[];
extern const char doc_time[], doc_exit[];
extern const char doc_ffifn[];
extern const char doc_doc[], doc_help[];
/* Clojure-style containers (EXP_DICT / EXP_LIST / EXP_BLOB). */
extern const char doc_hashmap[], doc_assocbang[], doc_dissocbang[], doc_get[],
                  doc_containsp[], doc_keys[], doc_vals[], doc_count[];
extern const char doc_deque[], doc_pushrightbang[], doc_pushleftbang[],
                  doc_poprightbang[], doc_popleftbang[],
                  doc_peekleft[], doc_peekright[];
extern const char doc_makeblob[], doc_bloblen[], doc_blobref[],
                  doc_blob2string[], doc_string2blob[], doc_readbytes[];
extern const char doc_vecdot[], doc_vecaxpy[], doc_vecscale[], doc_vecadd[],
                  doc_vecfill[], doc_vecrelu[], doc_vecargmax[], doc_vecmax[],
                  doc_veccopy[];
extern const char doc_vector[];
/* Redis inspector builtins (only registered, only callable, when the
   process started under -R; otherwise resp_db is empty and they all
   return zero/nil/none). Defined below the `#include "resp.c"` line so
   they can read the resp_db static directly. */
extern const char doc_redis_count[], doc_redis_keys[], doc_redis_type[],
                  doc_redis_get[], doc_redis_flush[], doc_redis_port[],
                  doc_redis_defcmd[], doc_redis_undefcmd[], doc_redis_cmds[];

/* Forward decls for cmds defined below the table — every callee must be
   visible at table-init time. The original cmds had a top-level
   #pragma-style sweep (functions all sit in alcove.c above this point);
   newer additions land below and need explicit decls. */
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
exp_t *dequecmd(exp_t *e, env_t *env);
exp_t *pushrightbangcmd(exp_t *e, env_t *env);
exp_t *pushleftbangcmd(exp_t *e, env_t *env);
exp_t *poprightbangcmd(exp_t *e, env_t *env);
exp_t *popleftbangcmd(exp_t *e, env_t *env);
exp_t *peekleftcmd(exp_t *e, env_t *env);
exp_t *peekrightcmd(exp_t *e, env_t *env);
exp_t *makeblobcmd(exp_t *e, env_t *env);
exp_t *bloblencmd(exp_t *e, env_t *env);
exp_t *blobrefcmd(exp_t *e, env_t *env);
exp_t *blob2stringcmd(exp_t *e, env_t *env);
exp_t *string2blobcmd(exp_t *e, env_t *env);
exp_t *readbytescmd(exp_t *e, env_t *env);
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
exp_t *rediscountcmd(exp_t *e, env_t *env);
exp_t *rediskeyscmd(exp_t *e, env_t *env);
exp_t *redistypecmd(exp_t *e, env_t *env);
exp_t *redisgetcmd(exp_t *e, env_t *env);
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
