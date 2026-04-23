#ifndef ALCOVE_H
#define ALCOVE_H

#include <stdint.h>
#include "char.h"
/* Structure definition */

enum {
  EXP_SYMBOL=1, EXP_NUMBER, EXP_FLOAT, EXP_STRING, EXP_CHAR, EXP_BOOLEAN, EXP_VECTOR, EXP_ERROR,
  /* ALL ATOMS ARE ABOVE THIS COMMENT */
  /*EXP_QUOTE,*/
  EXP_PAIR, EXP_LAMBDA,EXP_INTERNAL,EXP_MACRO,EXO_MACROINTERNAL,
  /* ALL EXP BEYOND THIS COMMENT ARE "CIRCULAR" MEANING THEY POINT TO A PREVIOUSLY MEM ALLOCATED EXP */
  EXP_TREE,
  EXP_PAIR_CIRCULAR,

  /* should always be the last */
  EXP_MAXSIZE
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

#define TAG_MASK   ((uintptr_t)0x7)
#define TAG_PTR    ((uintptr_t)0x0)
#define TAG_FIX    ((uintptr_t)0x1)
#define TAG_CHAR   ((uintptr_t)0x2)

#define TAG(e)     ((uintptr_t)(e) & TAG_MASK)
#define is_ptr(e)  ((e) != NULL && TAG(e) == TAG_PTR)
#define is_imm(e)  ((e) != NULL && TAG(e) != TAG_PTR)

#define MAKE_FIX(v)  ((exp_t*)((((uintptr_t)(int64_t)(v)) << 3) | TAG_FIX))
#define FIX_VAL(e)   ((int64_t)((intptr_t)(e)) >> 3)    /* arithmetic shift */
#define MAKE_CHAR(c) ((exp_t*)((((uintptr_t)(uint32_t)(c)) << 3) | TAG_CHAR))
#define CHAR_VAL(e)  ((uint32_t)((uintptr_t)(e) >> 3))

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
# define REFCOUNT_INC(p) (++(*(p)))
# define REFCOUNT_DEC(p) (--(*(p)))
#else
# define REFCOUNT_INC(p) __sync_add_and_fetch((p), 1)
# define REFCOUNT_DEC(p) __sync_sub_and_fetch((p), 1)
#endif

enum {
  EXP_ERROR_PARSING_MACROCHAR=1,EXP_ERROR_PARSING_ILLEGAL_CHAR,EXP_ERROR_PARSING_EOF,EXP_ERROR_PARSING_ESCAPE,
  EXP_ERROR_INVALID_KEY_UPDATE=256,EXP_ERROR_BODY_NOT_LIST,EXP_ERROR_PARAM_NOT_LIST,
  EXP_ERROR_MISSING_NAME,ERROR_ILLEGAL_VALUE,ERROR_DIV_BY0, ERROR_MISSING_PARAMETER,
  ERROR_UNBOUND_VARIABLE,ERROR_NUMBER_EXPECTED,ERROR_INDEX_OUT_OF_RANGE,
} exp_error_t;

/* Type predicates — all tag-aware. is_ptr() guards every heap deref so a
   tagged immediate passed to any of these returns 0 cleanly. */
#define isnumber(e)   (TAG(e) == TAG_FIX)
#define ischar(e)     (TAG(e) == TAG_CHAR)
#define issymbol(e)   (is_ptr(e) && (e)->type == EXP_SYMBOL)
#define isfloat(e)    (is_ptr(e) && (e)->type == EXP_FLOAT)
#define isstring(e)   (is_ptr(e) && (e)->type == EXP_STRING)
#define ispair(e)     (is_ptr(e) && (e)->type == EXP_PAIR)
#define islambda(e)   (is_ptr(e) && (e)->type == EXP_LAMBDA)
#define isinternal(e) (is_ptr(e) && (e)->type == EXP_INTERNAL)
#define ismacro(e)    (is_ptr(e) && (e)->type == EXP_MACRO)
#define iserror(e)    (is_ptr(e) && (e)->type == EXP_ERROR)
#define isatom(e)     (is_imm(e) || (is_ptr(e) && (e)->type <= EXP_VECTOR))

#define car(e) ((is_ptr(e)&&(e)->type==EXP_PAIR)?(e)->content:NULL)
#define cdr(e) ((is_ptr(e)&&(e)->type==EXP_PAIR)?(e)->next:NULL)
#define cadr(e) car(cdr(e))
#define cddr(e) cdr(cdr(e))
#define cdddr(e) cdr(cdr(cdr(e)))
#define caddr(e) car(cdr(cdr(e)))
#define cadddr(e) car(cdr(cdr(cdr(e))))
#define expfloat double
struct keyval_t;
struct exp_t;
struct env_t;
typedef struct exp_t *lispCmd(struct exp_t *e,struct env_t *env);

#define FLAG_TAILREC  1
/* Symbol AST node carries a cached lookup result in its ->meta field.
   Only set for resolutions into reserved_symbol (builtins) — those are
   immutable at runtime so the cache never needs invalidation. */
#define FLAG_RESOLVED 2

typedef struct exp_t {
  unsigned short int flags; /* 2 bytes --- bit 0 set to 1 for disk persistance */
  unsigned short int type; //  2 bytes --- exp type cf exptype_t enum list
  int nref; // --------------- 4 bytes --- Garbage collector number of reference counter
  union {   // --------------- 8 bytes
    struct exp_t *content;
    void *ptr;
    int64_t s64;
    expfloat f;
    lispCmd *fnc;
  } ;
  struct keyval_t
  /*char*/ *meta;  // --------- 8 bytes
  struct exp_t *next; // ------ 8 bytes
} exp_t; // total              32 bytes


typedef struct exp_tfunc {
  unsigned short int flags; 
  exp_t *(*clone)(exp_t *this); /* clone exp_t */
  exp_t *(*clone_flag)(exp_t *this); /* clone exp_t and flag as persistent*/
  exp_t *(*load)(exp_t *e,FILE *stream); /* load object as serialized data from stream */
  exp_t *(*dump)(exp_t *this,FILE *stream); /* serialized object this to stream */	
} exp_tfunc;



#define __CLONE__(e) (exp_tfuncList[e->type]&&exp_tfuncList[e->type]->clone?exp_tfuncList[e->type]->clone(e):NULL)
#define __CLONE_FLAG__(e) (exp_tfuncList[e->type]&&exp_tfuncList[e->type]->clone_flag?exp_tfuncList[e->type]->clone_flag(e):NULL)
/* Tag-aware type discriminator: returns the logical type for both heap
   exp_t and tagged immediates. Used to dispatch into exp_tfuncList. */
#define TYPEOF_E(e) (is_ptr(e) ? (int)(e)->type : \
                     (TAG(e) == TAG_FIX  ? (int)EXP_NUMBER : \
                     (TAG(e) == TAG_CHAR ? (int)EXP_CHAR   : 0)))

#define __LOAD__(e,s)   (exp_tfuncList[TYPEOF_E(e)]&&exp_tfuncList[TYPEOF_E(e)]->load?exp_tfuncList[TYPEOF_E(e)]->load(e,s):NULL)
#define __DUMP__(e,s)   (exp_tfuncList[TYPEOF_E(e)]&&exp_tfuncList[TYPEOF_E(e)]->dump?exp_tfuncList[TYPEOF_E(e)]->dump(e,s):NULL)
#define __DUMPABLE__(e) (exp_tfuncList[TYPEOF_E(e)]&&exp_tfuncList[TYPEOF_E(e)]->dump?1:0)


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
	} ;
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
	int pos;
	kvht_t ht[2];
} dict_t;


/* How many function-parameter bindings an env_t holds inline before
   spilling to the dict. Sized for the common recursive-function case
   where all params fit inline, so invoke() skips the dict/table/keyval
   allocs entirely. Tune up if deeper arities become common. */
#define ENV_INLINE_SLOTS 6

typedef struct env_t {
  struct env_t *root;
  exp_t *callingfnc;
  dict_t *d;                /* lazy — used for let/with and inline-slot overflow */
  int nref;                 /* refcount */
  int n_inline;             /* number of filled inline slots */
  /* Inline bindings for function-invocation params. Keys BORROW from
     the lambda header symbol's ptr; caller (invoke) must hold a ref to
     the lambda while the env is live. Vals own a ref. */
  char  *inline_keys[ENV_INLINE_SLOTS];
  exp_t *inline_vals[ENV_INLINE_SLOTS];
} env_t;



typedef struct lispProc {
  char *name;
  int arity;
  int flags; /*1 is macro*/
  int level; /* security level */
  lispCmd *cmd;
} lispProc;

/* Functions declaration */

int64_t gettimeusec();

/* memory management */
exp_t *refexp(exp_t *e);
void *memalloc(size_t count, size_t size);
int unrefexp(exp_t *e);

/* env management and exception handling inside env*/
env_t * ref_env(env_t *env);
env_t * make_env(env_t *rootenv);
void *destroy_env(env_t *env);
exp_t *set_return_point(env_t *env);



/* dict management */
dict_t * create_dict();
int destroy_dict(dict_t *d);
int dump_dict(dict_t *d,FILE *stream);
static void init_kvht(kvht_t *kvht);
unsigned int bernstein_hash(unsigned char *key, int len);
unsigned int bernstein_uhash(unsigned char *key, int len);
keyval_t * set_get_keyval_dict(dict_t* d, char *key, exp_t *val);
exp_t * set_keyval_dict_timestamp(dict_t* d, char *key, int64_t timestamp);
int64_t get_keyval_dict_timestamp(dict_t* d, char *key);
void * del_keyval_dict(dict_t* d, char *key);

/* lisp */
exp_t *error(int errnum,exp_t *id,env_t *env,char *err_message, ...);
exp_t *make_nil();   /* fresh heap pair (content=next=NULL) — for builders */
exp_t *make_char(unsigned char c);
exp_t *make_node(exp_t *node);
exp_t *make_internal(lispCmd *cmd);
exp_t *make_tree(exp_t *root,exp_t *node1);
exp_t *make_fromstr(char *str,int length);
exp_t *make_string(char *str,int length);
exp_t *make_symbol(char *str,int length);

/* ---------------- Canonical singletons ----------------
   nil_singleton and true_singleton are allocated once at main startup
   and treated as immortal — refexp/unrefexp short-circuit on them, so
   no atomic traffic, no ever-freeing. Every (if …), (iso a b), etc.
   that used to allocate a fresh NIL/TRUE now just returns the pointer.
   Huge alloc reduction on control-flow-heavy code. */
extern exp_t *nil_singleton;
extern exp_t *true_singleton;
#define NIL_EXP   (nil_singleton)
#define TRUE_EXP  (true_singleton)
exp_t *make_quote(exp_t *node);
exp_t *make_integer(char *str);
exp_t *make_integeri(int64_t i);
exp_t *make_float(char *str);
exp_t *make_floatf(expfloat f);
size_t loadtype(FILE *stream,unsigned short int* type);
size_t dumptype(FILE *stream,unsigned short int *type);
size_t loadsize_t(FILE *stream,size_t* len);
size_t dumpsize_t(FILE *stream,size_t *len);
exp_t *load_exp_t(FILE *stream);
exp_t *dump_exp_t(exp_t *e,FILE *stream);
exp_t *load_char(exp_t *e,FILE *stream);
exp_t *dump_char(exp_t *e,FILE *stream);
char *load_str(char **pptr,FILE *stream);
exp_t *load_string(exp_t *e,FILE *stream);
char *dump_str(char *ptr,FILE *stream);
exp_t *dump_string(exp_t *e,FILE *stream);
exp_t *make_atom(char *str,int length);
exp_t *callmacrochar(FILE *stream,unsigned char x);
exp_t *lookup(exp_t *e,env_t *env);
exp_t *updatebang(exp_t *key,env_t *env,exp_t *val);
exp_t *escapereader(FILE *stream,token_t ** ptoken,int lastchar);
exp_t *reader(FILE *stream,unsigned char clmacro,int keepwspace);
exp_t *optimize(exp_t *e,env_t *env);
exp_t *evaluate(exp_t *e,env_t *env);
/* EVAL() — canonical borrowed→owned wrapper. evaluate() consumes its input ref,
   so calling it on a borrowed car/cadr/caddr pointer causes a premature free.
   Prefer EVAL(e, env) over evaluate(refexp(e), env) at call sites. */
#define EVAL(e, env) evaluate(refexp(e), env)
void tree_add_node(exp_t *tree,exp_t *node);
void pair_add_node(exp_t *pair, exp_t *node);
void print_node(exp_t *node);
int istrue(exp_t *e);
int isequal(exp_t *cur1, exp_t *cur2);
int isoequal(exp_t *cur1, exp_t *cur2);
exp_t *letcmd(exp_t *e,env_t *env);
exp_t *withcmd(exp_t *e,env_t *env);
exp_t *var2env(exp_t *e,exp_t *var, exp_t *val,env_t *env,int evalexp);
exp_t *invoke(exp_t *e, exp_t *fn, env_t *env);
exp_t *expandmacro(exp_t *e, exp_t *fn, env_t *env);
exp_t *invokemacro(exp_t *e, exp_t *fn, env_t *env);

/* lisp command */
exp_t *verbosecmd(exp_t *e, env_t *env);
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
exp_t *andcmd(exp_t *e, env_t *env);
exp_t *orcmd(exp_t *e, env_t *env);
exp_t *nocmd(exp_t *e, env_t *env);
exp_t *iscmd(exp_t *e, env_t *env);
exp_t *isocmd(exp_t *e, env_t *env);
exp_t *incmd(exp_t *e, env_t *env);
exp_t *casecmd(exp_t *e, env_t *env);
exp_t *forcmd(exp_t *e, env_t *env);
exp_t *eachcmd(exp_t *e,env_t *env);
exp_t *timecmd(exp_t *e,env_t *env);
exp_t *inspectcmd(exp_t *e,env_t *env);
/* lisp macro */
exp_t *defcmd(exp_t *e, env_t *env);
exp_t *expandmacrocmd(exp_t *e,env_t *env);
exp_t *defmacrocmd(exp_t *e, env_t *env);
exp_t *fncmd(exp_t *e, env_t *env);
exp_t *conscmd(exp_t *e, env_t *env);
exp_t *evalcmd(exp_t *e, env_t *env);
exp_t *carcmd(exp_t *e, env_t *env);
exp_t *cdrcmd(exp_t *e, env_t *env);
exp_t *listcmd(exp_t *e, env_t *env);


/* Token */
token_t * tokenize(int v);
void freetoken(token_t *token);
void tokenadd(token_t *token, int v);
void tokenappend(token_t *token,char *src,int len);

#define true 1
#define false 0

/* Parser */

#define PARSER_KEEPWHITESPACE 1
#define PARSER_PIPEMODE 2
#define PARSER_TERMMACROMODE 4


#endif /* ALCOVE_H */
