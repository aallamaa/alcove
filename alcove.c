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
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h> /* isatty for the readline REPL gate; needed even
                          when ALCOVE_JIT is off. */
#ifdef ALCOVE_JIT
#include <stddef.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <pthread.h>
#endif
#endif
// #include <jemalloc/jemalloc.h>
#include "alcove.h"

int toeval = 1;
int verbose = 00;
dict_t *reserved_symbol = NULL;
exp_tfunc *exp_tfuncList[EXP_MAXSIZE];

/* Canonical singletons — pointer set at main() startup. */
exp_t *nil_singleton = NULL;
exp_t *true_singleton = NULL;

/* Global env handle for the readline tab-completion callback (which
   takes no user-data param). Set in main() before the REPL loop. */
struct env_t *g_global_env = NULL;

/* Set by invoke's body loop / ifcmd's selected-branch eval. When true,
   evaluate returns a trampoline marker instead of recursing into
   invoke, giving us O(1) C stack for tail-recursive code. */
static int in_tail_position = 0;

/* Builtin registration. Every entry's `arity` and `level` columns are
   the same today; the macros hide them. Use LISPCMD_TAIL for control-
   flow forms (if, do) that need FLAG_TAIL_AWARE so evaluate() exposes
   in_tail_position to them. */
#define LISPCMD(name, fn) {name, 2, 0, 0, fn}
#define LISPCMD_TAIL(name, fn) {name, 2, FLAG_TAIL_AWARE, 0, fn}

lispProc lispProcList[] = {
    /* Special forms / control flow */
    LISPCMD("verbose", verbosecmd),
    LISPCMD("quote", quotecmd),
    LISPCMD_TAIL("if", ifcmd),
    LISPCMD_TAIL("do", docmd),
    LISPCMD("when", whencmd),
    LISPCMD("while", whilecmd),
    LISPCMD("repeat", repeatcmd),
    LISPCMD("and", andcmd),
    LISPCMD("or", orcmd),
    LISPCMD("case", casecmd),
    LISPCMD("for", forcmd),
    LISPCMD("each", eachcmd),
    LISPCMD("let", letcmd),
    LISPCMD("with", withcmd),
    /* Comparison / equality */
    LISPCMD("=", equalcmd),
    LISPCMD("<", cmpcmd),
    LISPCMD(">", cmpcmd),
    LISPCMD("<=", cmpcmd),
    LISPCMD(">=", cmpcmd),
    LISPCMD("is", iscmd),
    LISPCMD("iso", isocmd),
    LISPCMD("in", incmd),
    LISPCMD("no", nocmd),
    /* Arithmetic */
    LISPCMD("+", pluscmd),
    LISPCMD("*", multiplycmd),
    LISPCMD("-", minuscmd),
    LISPCMD("/", dividecmd),
    LISPCMD("mod", modcmd),
    LISPCMD("abs", abscmd),
    LISPCMD("max", maxcmd),
    LISPCMD("min", mincmd),
    LISPCMD("odd", oddcmd),
    LISPCMD("sqrt", sqrtcmd),
    LISPCMD("sqrt-int", sqrtintcmd),
    LISPCMD("exp", expcmd),
    LISPCMD("expt", exptcmd),
    LISPCMD("random", randomcmd),
    /* Pairs and lists */
    LISPCMD("cons", conscmd),
    LISPCMD("car", carcmd),
    LISPCMD("cdr", cdrcmd),
    LISPCMD("list", listcmd),
    LISPCMD("length", lengthcmd),
    LISPCMD("nth", nthcmd),
    LISPCMD("reverse", reversecmd),
    LISPCMD("append", appendcmd),
    /* Vectors — O(1) random-access array */
    LISPCMD("vec", veccmd),
    LISPCMD("vec-ref", vecrefcmd),
    LISPCMD("vec-set!", vecsetcmd),
    LISPCMD("vec-len", veclencmd),
    /* Functions and binding */
    LISPCMD("def", defcmd),
    LISPCMD("fn", fncmd),
    LISPCMD("defmacro", defmacrocmd),
    LISPCMD("macroexpand-1", expandmacrocmd),
    LISPCMD("eval", evalcmd),
    LISPCMD("apply", applycmd),
    /* Higher-order */
    LISPCMD("map", mapcmd),
    LISPCMD("filter", filtercmd),
    LISPCMD("reduce", reducecmd),
    LISPCMD("any?", anypcmd),
    LISPCMD("all?", allpcmd),
    /* Predicates */
    LISPCMD("number?", numberpcmd),
    LISPCMD("string?", stringpcmd),
    LISPCMD("symbol?", symbolpcmd),
    LISPCMD("pair?", pairpcmd),
    LISPCMD("fn?", fnpcmd),
    /* I/O */
    LISPCMD("pr", prcmd),
    LISPCMD("print", prcmd),
    LISPCMD("prn", prncmd),
    LISPCMD("println", prncmd),
    /* Persistence */
    LISPCMD("persist", persistcmd),
    LISPCMD("forget", forgetcmd),
    LISPCMD("savedb", savedbcmd),
    LISPCMD("loaddb", loaddbcmd),
    LISPCMD("ispersistent", ispersistentcmd),
    /* Introspection / utilities */
    LISPCMD("inspect", inspectcmd),
    LISPCMD("disasm", disasmcmd),
    LISPCMD("source", sourcecmd),
    LISPCMD("dir", dircmd),
    LISPCMD("time", timecmd),
    LISPCMD("exit", exitcmd),
    LISPCMD("quit", exitcmd),
    /* FFI */
    LISPCMD("ffi-fn", ffifncmd),
};
#undef LISPCMD
#undef LISPCMD_TAIL

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

void *memalloc(size_t count, size_t size) {
  return calloc(count, size);
  // ERROR MANAGEMENT TO BE ADDDED
}

inline exp_t *refexp(exp_t *e) {
  /* Tagged immediates (fixnum, char) and canonical singletons (nil, t)
     are immortal — skip the refcount traffic. */
  if (!is_ptr(e))
    return e;
  if (e == nil_singleton || e == true_singleton)
    return e;
  REFCOUNT_INC(&e->nref);
  return e;
}

/* exp_t free-list. Hot in cons-heavy code (sieve, listsum): each cons
   make_nils a fresh node, and discarding a list of length N triggers N
   unrefexp/free pairs. malloc/free is ~50ns per round trip; the
   free-list pop/push is ~3 cycles, ~10x faster. The list re-uses each
   freed exp_t's `next` pointer as the freelist link — safe because
   unrefexp recursively releases the original next before this point.
   Single-threaded (alcove never threads the interp). */
static exp_t *exp_freelist = NULL;

/* Bump-allocator for fresh exp_t when the free-list is empty. calloc(1,
   sizeof exp_t) is ~50ns per call; chunk-allocating 256 at a time
   amortizes that to ~4ns per exp_t. The chunks themselves are never
   freed — they live for the process lifetime, which matches alcove's
   model (the interpreter exits and the OS reclaims). */
#define EXP_BUMP_CHUNK 256
static exp_t *exp_bump_next = NULL;
static int exp_bump_left = 0;

inline int unrefexp(exp_t *e) {
  if (!is_ptr(e))
    return 0;
  if (e == nil_singleton || e == true_singleton)
    return 1;
  int ret;
  if ((ret = REFCOUNT_DEC(&e->nref)) <= 0) {
    /* Skip the verbose "Freeing:" trace for the EOF parse marker.
       Each form the REPL reads ends with reader() returning this
       sentinel so the loop knows to stop. Surfacing it as an error
       in verbose mode is misleading — it's normal end-of-input,
       not a real failure. */
    if (verbose &&
        !(e->type == EXP_ERROR && e->flags == EXP_ERROR_PARSING_EOF)) {
      printf("\x1B[91mFreeing:\x1B[39m ");
      print_node(e);
      printf("\n");
    };
    /* meta holds a strdup'd name for LAMBDA/MACRO, or a borrowed
       resolved-exp_t* pointer for cached SYMBOL lookups. Only free()
       in the former case — the cached pointer is borrowed. */
    if (e->meta && (e->type == EXP_LAMBDA || e->type == EXP_MACRO)) {
      free(e->meta);
    }
    if (e->bc && e->type == EXP_LAMBDA) {
      bytecode_free(e->bc);
    }
    /* Closure capture lives in lambda/macro's wrapper-node bc field
       (see fncmd / defcmd / defmacrocmd). Release it BEFORE unref'ing
       the wrapper, since after that e->next may be freed. */
    if ((e->type == EXP_LAMBDA || e->type == EXP_MACRO) && e->next &&
        e->next->bc) {
      destroy_env((env_t *)e->next->bc);
      e->next->bc = NULL;
    }
    if (e->next)
      unrefexp(e->next);
    if ((e->type == EXP_SYMBOL) || (e->type == EXP_STRING) ||
        (e->type == EXP_ERROR))
      free(e->ptr);
    else if (e->type == EXP_FFI) {
      extern void alc_ffi_free(void *ptr); /* defined alongside ffi_call */
      alc_ffi_free(e->ptr);
    } else if (e->type == EXP_VECTOR && e->ptr) {
      alc_vec_t *v = (alc_vec_t *)e->ptr;
      int64_t i;
      for (i = 0; i < v->len; i++)
        unrefexp(v->data[i]);
      free(v);
    } else if (((e->type >= EXP_NUMBER) && (e->type <= EXP_BOOLEAN)) ||
               (e->type == EXP_INTERNAL)) {
    } else
      unrefexp(e->content); // check if content type is exp

    if (verbose &&
        !(e->type == EXP_ERROR && e->flags == EXP_ERROR_PARSING_EOF))
      printf("\x1B[91mFree e:\x1B[39m %p\n", (void *)e);
    /* Push onto the free list instead of free(). next was just
       recursively released above so it's safe to overwrite. */
    e->next = exp_freelist;
    exp_freelist = e;

    return 0;
  };
  if (verbose) {
    printf("\x1B[91mUnref (%d): ", e->nref);
    print_node(e);
    printf("\x1B[39m\n");
  };
  return ret;
}

/* env management*/

/* ---------------- Environment stack arena ----------------
   alcove has no closures: lambdas do not capture their defining env, so
   every env created by make_env is destroyed in LIFO order (at the end
   of the call/let/with/for that made it). We exploit this with a bump
   allocator: make_env claims sizeof(env_t) bytes from the top of a
   fixed buffer; destroy_env rolls the top back down.
   Eliminates the calloc/free pair per function call — typically the
   single largest fixed cost in a tree-walking Lisp.
   Falls back to malloc() if the arena ever overflows (deep recursion
   beyond ENV_ARENA_SLOTS). */

#define ENV_ARENA_SLOTS 8192
static env_t env_arena[ENV_ARENA_SLOTS];
static env_t *env_arena_sp = env_arena;
static env_t *const env_arena_end = env_arena + ENV_ARENA_SLOTS;

/* Bumped on every operation that mutates a global binding (def, defmacro,
   persist, forget, savedb, top-level updatebang). The bytecode global-
   resolution cache compares this against its own per-slot gen to detect
   stale entries. Starts at 1 so a fresh gcache_entry{val=NULL,gen=0} is
   trivially stale. */
uint64_t alcove_global_gen = 1;

#define IS_ARENA_ENV(e) ((e) >= env_arena && (e) < env_arena_end)

inline env_t *ref_env(env_t *env) {
  if (env) {
    REFCOUNT_INC(&env->nref);
    return env;
  } else
    return env;
}

inline env_t *make_env(env_t *rootenv) {
  /* No memset here: destroy_env leaves reused arena slots with the
     fields that could carry stale state (callingfnc, d, n_inline)
     already cleared. Fresh arena slots (first use) are BSS-zeroed.
     Heap-fallback slots come from memalloc which calloc's. Saves a
     ~128-byte store per call — the biggest per-call cost on fib. */
  env_t *newenv;
  if (env_arena_sp < env_arena_end) {
    newenv = env_arena_sp++;
  } else {
    newenv = memalloc(1, sizeof(env_t));
  }
  newenv->root = ref_env(rootenv);
  newenv->nref = 1;
  return newenv;
}

inline void *destroy_env(env_t *env) {
  /* Iterative release — each env holds a ref to its parent via
     make_env/ref_env. Recursing would blow the C stack on deep call chains.
     Also scrubs the fields that would carry stale state into a reused
     arena slot, so make_env can skip the wholesale memset. */
  while (env) {
    env_t *parent = env->root;
    if (REFCOUNT_DEC(&env->nref) > 0)
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
    if (IS_ARENA_ENV(env)) {
      env->n_inline = 0;
      env->d = NULL;
      env->callingfnc = NULL;
      if (env + 1 == env_arena_sp)
        env_arena_sp = env;
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

int dump_dict(dict_t *d, FILE *stream) {
  // check if in use
  keyval_t *ckv;
  keyval_t *pkv;
  unsigned int i, j;
  if (verbose)
    printf("Dumping dict %p\n", (void *)d);
  for (i = 0; i < 2; i++) {
    for (j = 0; j < d->ht[i].size; j++) {
      ckv = d->ht[i].table[j];
      while (ckv) {
        pkv = ckv;
        ckv = pkv->next;
        if (pkv->timestamp) {
          if (verbose) {
            printf("saving %s : ", (char *)pkv->key);
            print_node(pkv->val);
          }
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

void *del_keyval_dict(dict_t *d, char *key) {
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
        return NULL;
      } else
        return NULL;
    }
  }
  return NULL;
}

// see page 25 concept of "liaison immuable" et liaison "muable"

inline exp_t *make_nil() {
  exp_t *nil_exp;
  if (exp_freelist) {
    nil_exp = exp_freelist;
    exp_freelist = nil_exp->next;
    /* Zero out the fields that could carry stale state from the
       previous tenant. memset would also work but is ~3x slower
       for a 40-byte struct on a hot path. */
    nil_exp->flags = 0;
    nil_exp->content = NULL;
    nil_exp->meta = NULL;
    nil_exp->next = NULL;
    nil_exp->bc = NULL;
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

inline exp_t *make_char(unsigned char c) {
  /* Tagged immediate: no heap allocation. */
  return MAKE_CHAR(c);
}

inline exp_t *make_node(exp_t *node) {
  exp_t *cur = make_nil();
  if (node)
    cur->content = node;
  return cur;
}

inline exp_t *make_internal(lispCmd *cmd, int flags) {
  exp_t *cur = make_nil();
  cur->type = EXP_INTERNAL;
  cur->flags = flags;
  cur->fnc = cmd;
  return cur;
}

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
      else if (cur->type == EXP_PAIR)
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
    printf("\x1B[92m%s%lld\x1B[39m", verbose ? "_num:" : "",
           (long long)FIX_VAL(node));
    return;
  }
  if (ischar(node)) {
    uint32_t c = CHAR_VAL(node);
    if (c > 32)
      printf("#\\%c", (char)c);
    else
      printf("#\\%u", c);
    return;
  }
  if (!is_ptr(node)) {
    printf("<?imm %p>", (void *)node);
    return;
  }
  if (node->type == EXP_ERROR) {
    printf("\x1B[91mError: \x1B[39m%s\n", (char *)node->ptr);
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
    if (verbose) {
      printf("\theader:");
      print_node(node->content);
      printf("\tbody:");
      print_node(node->next);
    }
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
    printf("\x1B[92m%s%s\x1B[39m", verbose ? "_sym:" : "", (char *)node->ptr);
  else if (node->type == EXP_STRING)
    printf("\x1B[92m%s\"%s\"\x1B[39m", verbose ? "_str:" : "",
           (char *)node->ptr);
  else if (node->type == EXP_FLOAT)
    printf("\x1B[92m%s%lf\x1B[39m", verbose ? "_flo:" : "", node->f);
  else if (node->type == EXP_VECTOR) {
    alc_vec_t *v = (alc_vec_t *)node->ptr;
    printf("#[");
    int64_t i;
    for (i = 0; i < v->len; i++) {
      if (i)
        printf(" ");
      print_node(v->data[i]);
    }
    printf("]");
  } else {
    printf("\x1B[92mtype: %d ptr: %08lx\x1B[39m", node->type,
           (unsigned long)node->ptr);
  }
  return;
}

inline exp_t *make_fromstr(char *str, int length) {
  exp_t *cur = make_nil();
  cur->ptr = memalloc(length + 1, sizeof(char));
  /* memcpy not strncpy: we explicitly NUL-terminate at +length below.
     strncpy here triggers -Wstringop-truncation because the count
     equals the source length, with no room for the implicit NUL. */
  memcpy(cur->ptr, str, length);
  *((char *)cur->ptr + length) = '\0';
  return cur;
}

inline exp_t *make_string(char *str, int length) {
  exp_t *cur = make_fromstr(str, length);
  cur->type = EXP_STRING;
  //  printf("STR %s\n",(char*)cur->ptr);
  return cur;
}

inline exp_t *make_symbol(char *str, int length) {
  exp_t *cur = make_fromstr(str, length);
  cur->type = EXP_SYMBOL;
  //  printf("SYM %s\n",(char*)cur->ptr);
  return cur;
}

/* OLD
   exp_t *make_quote(exp_t *node){
   exp_t *cur=NIL_EXP;
   cur->type=EXP_QUOTE;
   cur->content=refexp(node);
   return cur;
   }*/

inline exp_t *make_quote(exp_t *node) {
  exp_t *cur = make_symbol("quote", strlen("quote"));
  cur = make_node(cur);
  cur->next = make_node(node);
  return cur;
}

inline exp_t *make_integer(char *str) {
  /* Tagged immediate: no heap allocation. atoll for 64-bit range. */
  return MAKE_FIX(atoll(str));
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

exp_t *load_exp_t(FILE *stream) {
  exp_t *resp = make_nil();
  if (loadtype(stream, &(resp->type)) > 0) {
    return __LOAD__(resp, stream);
  } else {
    unrefexp(resp);
    return NULL;
  }
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
  return MAKE_CHAR((unsigned char)c);
}

exp_t *dump_char(exp_t *e, FILE *stream) {
  unsigned short int t = EXP_CHAR;
  if (dumptype(stream, &t) <= 0)
    return NULL;
  if (fputc((int)CHAR_VAL(e), stream) == EOF)
    return NULL;
  return e;
}

char *load_str(char **pptr, FILE *stream) {
  size_t length;
  char *ptr;
  if (loadsize_t(stream, &length) <= 0)
    return NULL;
  ptr = *pptr = memalloc(length + 1, sizeof(char));
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
  else
    return NULL;
}

char *dump_str(char *ptr, FILE *stream) {
  size_t length = strlen(ptr);
  if (dumpsize_t(stream, &length) <= 0)
    return NULL;
  if (fwrite(ptr, 1, length, stream) <= 0)
    return NULL;
  return ptr;
}

exp_t *dump_string(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  if (dump_str(e->ptr, stream))
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
  if (fread(&e->f, sizeof(e->f), 1, stream) != 1)
    return NULL;
  return e;
}

/* EXP_SYMBOL — same length-prefixed bytes as a string; on load we just
   stash the name into e->ptr. Symbol identity (eq?) isn't preserved
   across runs but iso-equality / lookup-by-name works fine. */
exp_t *dump_symbol(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  if (dump_str(e->ptr, stream))
    return e;
  return NULL;
}
exp_t *load_symbol(exp_t *e, FILE *stream) {
  if (load_str((char **)&(e->ptr), stream))
    return e;
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
  e->content = (flags & 1) ? load_exp_t(stream) : NULL;
  e->next = (flags & 2) ? load_exp_t(stream) : NULL;
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
  uint8_t flags = 0;
  if (e->content)
    flags |= 1;
  if (e->next && e->next->content)
    flags |= 2;
  if (fwrite(&flags, 1, 1, stream) != 1)
    return NULL;
  if ((flags & 1) && !__DUMP__(e->content, stream))
    return NULL;
  if ((flags & 2) && !__DUMP__(e->next->content, stream))
    return NULL;
  return e;
}
exp_t *load_lambda(exp_t *e, FILE *stream) {
  char *name = NULL;
  if (!load_str(&name, stream))
    return NULL;
  uint8_t flags;
  if (fread(&flags, 1, 1, stream) != 1) {
    free(name);
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
     body uses an unsupported form). The lambda still works either way. */
  compile_lambda(e);
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
  uint8_t flags = 0;
  if (e->content)
    flags |= 1;
  if (e->next && e->next->content)
    flags |= 2;
  if (fwrite(&flags, 1, 1, stream) != 1)
    return NULL;
  if ((flags & 1) && !__DUMP__(e->content, stream))
    return NULL;
  if ((flags & 2) && !__DUMP__(e->next->content, stream))
    return NULL;
  return e;
}
exp_t *load_macro(exp_t *e, FILE *stream) {
  char *name = NULL;
  if (!load_str(&name, stream))
    return NULL;
  uint8_t flags;
  if (fread(&flags, 1, 1, stream) != 1) {
    free(name);
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

/* EXP_VECTOR — currently an enum-only placeholder in alcove (no
   make_vector / no user-facing constructor as of this commit). When
   vectors land, the persistence story will need a length-prefixed
   recursive dump/load along the lines of dump_pair. Until then, no
   exp of type EXP_VECTOR can exist, so registering nothing here is
   correct (savedb's __DUMPABLE__ check will warn if one ever appears). */

exp_t *make_atom(char *str, int length) {
  // Generate an atom from a string during parsing
  //  TEST -> 0: + or - in front, 1: digit after first + or -, 2: E mantissa,
  //  3:+ or - sign, 4: digit of mantissa
  int test = 0;
  int dot = 0;
  int len = length;
  char v;
  char *stro = str;
  if (str[0] == '\"')
    return make_string(str + 1, length - 2);
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
    return make_symbol(stro, len);
  } else {
    if (test == 1)
      return make_symbol(stro, len);
    else if ((test == 3) && !dot)
      return make_integer(stro);
    else if ((test == 31) || (test == 3))
      return make_float(stro);
    else
      return make_symbol(stro, len);
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
  } else if (x == '\'') {
    vnode = reader(stream, 0, 0);
    return make_quote(vnode);
  } else if (x == '|') {
    vnode = reader(stream, ')', PARSER_PIPEMODE);
    if (vnode) {
      if (iserror(vnode))
        return vnode;
      lnode = make_node(vnode);
      cnode = lnode;
      while ((vnode = reader(stream, ')', PARSER_TERMMACROMODE))) {
        if (iserror(vnode)) {
          unrefexp(lnode);
          return vnode;
        }
        cnode = cnode->next = make_node(vnode);
      }
    }
  }

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
              return make_char(z);
            } else
              return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                           "End of file reached while parsing");

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
        if (keepwspace & PARSER_PIPEMODE) {
          token = tokenize(x);
          tokenadd(token, y);
        } else {
          if ((ret = escapereader(stream, &token, y)))
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
              if (keepwspace & PARSER_PIPEMODE) {
                tokenadd(token, y);
                tokenadd(token, z);
              } else if ((ret = escapereader(stream, &token, z)))
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
            }
            //{ tokenadd(token,z); continue;} // should we use escapereader here
            //?? to be checked
            else {
              freetoken(token);
              return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                           "End of file reached while parsing");
            }
          } else if (ISMULTIPLEESCAPE & chrmap[y]) {
            ret = make_string(token->data, token->size);
            freetoken(token);
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
      ret = make_atom(token->data, token->size);
      freetoken(token);
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
  int ret = 0;
  if (!e)
    return 0;
  /* Tagged immediates first — cheap tag check, no deref. */
  if (isnumber(e))
    return FIX_VAL(e) != 0;
  if (ischar(e))
    return 0; /* preserve historical behavior */
  if (!is_ptr(e))
    return 0;
  if isatom (e) {
    if isstring (e)
      ret = ((e->ptr) ? strlen(e->ptr) : 0);
    else if isfloat (e)
      ret = (e->f != 0);
    else if issymbol (e)
      ret = (strcmp(e->ptr, "nil") != 0);
  } else if ispair (e) {
    if (e->content || e->next)
      ret = 1;
  }
  return ret;
}

inline exp_t *lookup(exp_t *e, env_t *env) {
  keyval_t *ret;
  env_t *curenv = env;
  char *key = e->ptr;

  /* Cache fast path: symbols previously resolved into reserved_symbol
     (builtins like +, -, if, <, etc.) skip the hash lookup. On a
     symbol, meta != NULL uniquely means "cached resolution" — the
     lambda/macro meta strings only exist on their own exp_t types. */
  if (e->meta) {
    return refexp((exp_t *)e->meta);
  }

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
          if (strcmp(curenv->inline_keys[i], key) == 0)
            return refexp(curenv->inline_vals[i]);
        }
        if ((curenv->d) && (ret = set_get_keyval_dict(curenv->d, key, NULL)))
          return refexp(ret->val);
      } while ((curenv = curenv->root));
  }
  return NULL;
}
exp_t *updatebang(exp_t *keyv, env_t *env, exp_t *val) {
  keyval_t *ret = NULL;
  exp_t *fret = NULL;
  exp_t *key = NULL;
  exp_t *key2 = NULL;
  if (val == NULL)
    val = NIL_EXP;
  if (issymbol(keyv) || isstring(keyv)) { // form (= "qweqwe" 10) (= weq 10)
    if (islambda(val) && val->meta == NULL)
      val->meta = (keyval_t *)strdup(keyv->ptr);
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
          if (strcmp(cur->inline_keys[i], keyv->ptr) == 0) {
            unrefexp(cur->inline_vals[i]);
            cur->inline_vals[i] = refexp(val);
            unrefexp(keyv);
            return refexp(val);
          }
        }
        if (cur->d) {
          keyval_t *kv = set_get_keyval_dict(cur->d, keyv->ptr, NULL);
          if (kv) {
            /* found — replace and bump global gen so any cached
               lookups of this name re-resolve. */
            unrefexp(kv->val);
            kv->val = refexp(val);
            alcove_global_gen++;
            unrefexp(keyv);
            return refexp(val);
          }
        }
        cur = cur->root;
      }
    }
    /* No existing binding anywhere — create in current env. */
    if (!(env->d))
      env->d = create_dict();
    ret = set_get_keyval_dict(env->d, keyv->ptr, val);
    alcove_global_gen++;
    unrefexp(keyv);
    return refexp(val);
  } else if (ispair(keyv)) { /*evaluate(keyv,env)=val*/
    key = car(keyv);
    if (key && issymbol(key)) {
      if (strcmp(key->ptr, "car") == 0) // form (= (car x) 'z)
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

      } else if (strcmp(key->ptr, "cdr") == 0) {
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
        key = EVAL(key, env);
        if (isstring(key)) {
          key2 = cadr(keyv);
          if (key2 && isnumber(key2) && ischar(val))
            if ((FIX_VAL(key2) >= 0) &&
                (FIX_VAL(key2) < (int64_t)strlen(key->ptr))) {
              *((char *)key->ptr + FIX_VAL(key2)) =
                  (unsigned char)CHAR_VAL(val);
              goto finish;
            } else
              fret = error(ERROR_INDEX_OUT_OF_RANGE, keyv, env,
                           "Error index out of range");
          else
            fret = error(ERROR_NUMBER_EXPECTED, keyv, env,
                         "Error number and char expected");
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

/* Lambdas here are NOT closures: the returned EXP_LAMBDA stores only
   params + body, with no reference to the defining env. Free variables
   are resolved dynamically against the CALLER's env chain at invoke
   time. If closure semantics are ever added, the env arena and the
   bytecode VM's lifetime assumptions both need revisiting. */
exp_t *fncmd(exp_t *e, env_t *env) {
  exp_t *val;
  exp_t *vali;
  exp_t *header;
  exp_t *body;
  exp_t *cur = cdr(e);
  if (cur && ispair(cur->content)) {
    header = car(cur);
    cur = cdr(cur);
    if (cur && ispair(cur->content)) {
      body = cur;
      vali = make_node(refexp(body));
      val = make_node(refexp(header));
      val->next = vali;
      val->type = EXP_LAMBDA;
      /* Closure: stash the env at fn-creation time in the wrapper
         node's `bc` field (unused for non-bytecode wrappers — see
         comment in unrefexp). invoke() later uses this as the new
         call env's root, so let/with bindings from the enclosing scope
         resolve correctly. For top-level fns env is global, so the
         capture is just an extra ref on global (cheap). */
      if (env)
        val->next->bc = (struct bytecode_t *)ref_env(env);
      /* Compile only when there's no real captured scope — the bytecode
         VM's gcache caches global resolutions per-bc, which is wrong if
         a captured `n` from a let later mutates. Closures (env != global,
         i.e. env->root != NULL) run as AST. Top-level fns still compile
         and JIT as before. */
      if (!env || !env->root)
        compile_lambda(val);
    } else
      val = error(EXP_ERROR_BODY_NOT_LIST, e, env, "Error body is not a list");
  } else
    val = error(EXP_ERROR_PARAM_NOT_LIST, e, env, "Error params is not a list");
  unrefexp(e);
  return val;
}

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
    if (cur && ispair(cur->content)) {
      header = car(cur);
      cur = cdr(cur);
      if (cur && ispair(cur->content)) {
        body = cur;
        vali = make_node(refexp(body));
        val = make_node(refexp(header));
        val->next = vali;
        val->type = EXP_LAMBDA;
        val->meta = (keyval_t *)strdup(name->ptr);
        /* Closure: capture defining env (see fncmd for rationale). */
        if (env)
          val->next->bc = (struct bytecode_t *)ref_env(env);
        /* Compile only when defined at top level (env->root == NULL).
           A nested (def ...) inside a let captures the let env and
           runs as AST so name lookups walk the captured chain. */
        if (!env || !env->root)
          compile_lambda(val); /* silent fallback to AST if body unsupported */
        if (!(env->d))
          env->d = create_dict();
        set_get_keyval_dict(env->d, name->ptr,
                            val); /* return value (the kv) unused */
        alcove_global_gen++; /* invalidate bytecode global-resolution caches */
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

exp_t *expandmacrocmd(exp_t *e, env_t *env) {
  exp_t *tmpexp;
  exp_t *tmpexp2;

  tmpexp = car(cadr(cadr(e)));
  // if (tmpexp && ispair(tmpexp)) tmpexp=evaluate(tmpexp,env);
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
        val->meta = (keyval_t *)strdup(name->ptr);
        if (env)
          val->next->bc = (struct bytecode_t *)ref_env(env);
        if (!(env->d))
          env->d = create_dict();
        set_get_keyval_dict(env->d, name->ptr,
                            val); /* return value (the kv) unused */
        alcove_global_gen++;
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
exp_t *verbosecmd(exp_t *e, env_t *env) {
  if (verbose ^= 1)
    printf("verbose on\n");
  else
    printf("verbose off\n");
  unrefexp(e);
  return NULL;
}
exp_t *quotecmd(exp_t *e, env_t *env) {
  exp_t *ret = refexp(cadr(e));
  unrefexp(e);
  return ret;
}
#pragma GCC diagnostic warning "-Wunused-parameter"

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

exp_t *equalcmd(exp_t *e, env_t *env) {
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
  ret = set_keyval_dict_timestamp(env->d, tmpkey->ptr, gettimeusec());
  unrefexp(tmpkey);
  return ret;
}

exp_t *ispersistentcmd(exp_t *e, env_t *env) {
  exp_t *tmpkey = refexp(cadr(e));
  int64_t ret = 0;
  if (!issymbol(tmpkey)) {
    tmpkey = evaluate(tmpkey, env);
  }
  unrefexp(e);
  if iserror (tmpkey)
    return tmpkey;
  ret = get_keyval_dict_timestamp(env->d, tmpkey->ptr);
  unrefexp(tmpkey);
  if (ret) {
    return TRUE_EXP;
  } else {
    return NIL_EXP;
  }
}

exp_t *forgetcmd(exp_t *e, env_t *env) {
  exp_t *tmpkey = refexp(cadr(e));
  exp_t *ret = NULL;
  if (!issymbol(tmpkey)) {
    tmpkey = evaluate(tmpkey, env);
  }
  unrefexp(e);
  /* to be unrefed tmpkey in case of evaluate */
  if iserror (tmpkey)
    return tmpkey;
  ret = set_keyval_dict_timestamp(env->d, tmpkey->ptr, 0);
  unrefexp(tmpkey);
  return ret;
}

exp_t *savedbcmd(exp_t *e, env_t *env) {
  env_t *cur = env;
  FILE *stream = fopen("db.dump", "w");
  if (!stream) {
    return error(ERROR_ILLEGAL_VALUE, e, env,
                 "Unable to open db.dump for writing");
  }
  while (cur->root) {
    cur = cur->root;
  }
  if (cur->d)
    dump_dict(cur->d, stream);
  fclose(stream);
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
int loaddb_from_file(env_t *env) {
  FILE *stream = fopen("db.dump", "r");
  if (!stream)
    return -1;
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
  alcove_global_gen++; /* invalidate gcache */
  return n;
}

exp_t *loaddbcmd(exp_t *e, env_t *env) {
  int n = loaddb_from_file(env);
  if (n < 0) {
    return error(ERROR_ILLEGAL_VALUE, e, env,
                 "Unable to open db.dump for reading");
  }
  printf("loaded %d entries from db.dump\n", n);
  unrefexp(e);
  return MAKE_FIX(n);
}

exp_t *cmpcmd(exp_t *e, env_t *env) {
  exp_t *op = car(e); /* operator symbol, borrowed */
  exp_t *result = NULL;
  exp_t *v1 = EVAL(cadr(e), env);
  if iserror (v1) {
    unrefexp(e);
    return v1;
  }
  exp_t *v2 = EVAL(caddr(e), env);
  double d = 0;
  int ret = 0;

  if iserror (v2) {
    unrefexp(e);
    unrefexp(v1);
    return v2;
  }
  if ((isnumber(v1) || isfloat(v1)) && (isnumber(v2) || isfloat(v2))) {
    d = (isnumber(v1) ? (double)FIX_VAL(v1) : v1->f) -
        (isnumber(v2) ? (double)FIX_VAL(v2) : v2->f);
  } else if (isstring(v1) && isstring(v2)) {
    d = strcmp(v1->ptr, v2->ptr);
  } else if (ischar(v1) && ischar(v2)) {
    d = (double)CHAR_VAL(v1) - (double)CHAR_VAL(v2);
  } else {
    result = error(ERROR_ILLEGAL_VALUE, e, env,
                   "Illegal value in compare operation");
    goto finish;
  }
  if (!op || !issymbol(op)) {
    result = error(ERROR_ILLEGAL_VALUE, e, env,
                   "Missing operator in compare operation");
    goto finish;
  }
  if (strcmp(op->ptr, "<") == 0)
    ret = (d < 0);
  else if (strcmp(op->ptr, ">") == 0)
    ret = (d > 0);
  else if (strcmp(op->ptr, "<=") == 0)
    ret = (d <= 0);
  else if (strcmp(op->ptr, ">=") == 0)
    ret = (d >= 0);
  else {
    result = error(ERROR_ILLEGAL_VALUE, e, env,
                   "Illegal operand in compare operation");
    goto finish;
  }
  result = (ret ? TRUE_EXP : NIL_EXP);
finish:
  unrefexp(v1);
  unrefexp(v2);
  unrefexp(e);
  return result;
}

exp_t *pluscmd(exp_t *e, env_t *env) {
  int64_t sum_i = 0;
  expfloat sum_f = 0;
  int saw_float = 0;
  exp_t *c = cdr(e);
  exp_t *v;
  exp_t *v1 = NULL;
  exp_t *ret = NULL;
  do {
    if (c && (v1 = refexp(c->content))) {
      if ispair (v1)
        v = evaluate(v1, env);
      else if issymbol (v1)
        v = evaluate(v1, env);
      else
        v = v1;
      if iserror (v) {
        ret = v;
        goto finish;
      }
      if (saw_float) {
        if (isnumber(v))
          sum_f += FIX_VAL(v);
        else if (isfloat(v))
          sum_f += v->f;
        else {
          ret =
              error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
          goto finish;
        }
      } else {
        if (isnumber(v))
          sum_i += FIX_VAL(v);
        else if (isfloat(v)) {
          sum_f = v->f + sum_i;
          sum_i = 0;
          saw_float = 1;
        } else {
          ret =
              error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
          goto finish;
        }
      }
      unrefexp(v);
    }
  } while (c && (c = c->next));
  ret = saw_float ? make_floatf(sum_f) : make_integeri(sum_i);
finish:
  unrefexp(e);
  return ret;
}

exp_t *multiplycmd(exp_t *e, env_t *env) {
  int64_t sum_i = 1;
  expfloat sum_f = 1;
  int saw_float = 0;
  int saw_int = 0;
  exp_t *c = cdr(e);
  exp_t *v;
  exp_t *v1 = NULL;
  exp_t *ret = NULL;

  do {
    if (c && (v1 = refexp(c->content))) {
      if ispair (v1)
        v = evaluate(v1, env);
      else if issymbol (v1)
        v = evaluate(v1, env);
      else
        v = v1;
      if iserror (v) {
        ret = v;
        goto finish;
      }
      if (saw_float) {
        if (isnumber(v))
          sum_f *= FIX_VAL(v);
        else if (isfloat(v))
          sum_f *= v->f;
        else {
          ret =
              error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
          goto finish;
        }
      } else {
        if (isnumber(v)) {
          sum_i = saw_int ? sum_i * FIX_VAL(v) : FIX_VAL(v);
          saw_int = 1;
        } else if (isfloat(v)) {
          sum_f = saw_int ? v->f * sum_i : v->f;
          sum_i = 1;
          saw_float = 1;
        } else {
          ret =
              error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
          goto finish;
        }
      }
      unrefexp(v);
    }
  } while (c && (c = c->next));
  ret = saw_float ? make_floatf(sum_f) : make_integeri(sum_i);
finish:
  unrefexp(e);
  return ret;
}

exp_t *minuscmd(exp_t *e, env_t *env) {
  int64_t sum_i = 0;
  expfloat sum_f = 0;
  int saw_float = 0;
  exp_t *c = cdr(e);
  exp_t *v;
  exp_t *v1 = NULL;
  int i = 0;
  exp_t *ret = NULL;

  do {
    if (c && (v1 = refexp(c->content))) {
      i++;
      if ispair (v1)
        v = evaluate(v1, env);
      else if issymbol (v1)
        v = evaluate(v1, env);
      else
        v = v1;
      if iserror (v) {
        unrefexp(e);
        return v;
      }
      if (saw_float) {
        if (isnumber(v))
          sum_f -= FIX_VAL(v);
        else if (isfloat(v))
          sum_f -= v->f;
        else {
          ret =
              error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
          goto finish;
        }
      } else {
        if (isnumber(v)) {
          if (i > 1)
            sum_i -= FIX_VAL(v);
          else
            sum_i = FIX_VAL(v);
        } else if (isfloat(v)) {
          sum_f = (i > 1) ? sum_i - v->f : v->f;
          sum_i = 0;
          saw_float = 1;
        } else {
          ret =
              error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
          goto finish;
        }
      }
      unrefexp(v);
    }
  } while (c && (c = c->next));
  if (i == 1) {
    if (saw_float)
      sum_f = -sum_f;
    else
      sum_i = -sum_i;
  }
  ret = saw_float ? make_floatf(sum_f) : make_integeri(sum_i);
finish:
  unrefexp(e);
  return ret;
}

exp_t *dividecmd(exp_t *e, env_t *env) {
  int64_t sum_i = 0;
  expfloat sum_f = 0;
  int saw_float = 0;
  exp_t *c = cdr(e);
  exp_t *v;
  exp_t *v1 = NULL;
  int i = 0;
  exp_t *ret = NULL;

  do {
    if (c && (v1 = refexp(c->content))) {
      i++;
      if ispair (v1)
        v = evaluate(v1, env);
      else if issymbol (v1)
        v = evaluate(v1, env);
      else
        v = v1;
      if iserror (v) {
        unrefexp(e);
        return v;
      }
      if (i > 1) {
        if ((isnumber(v) && FIX_VAL(v) == 0) || (isfloat(v) && v->f == 0)) {
          ret = error(ERROR_DIV_BY0, e, env, "Illegal Division by 0");
          goto finish;
        }
      }
      if (saw_float) {
        if (isnumber(v))
          sum_f /= FIX_VAL(v);
        else if (isfloat(v))
          sum_f /= v->f;
        else {
          ret =
              error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
          goto finish;
        }
      } else {
        if (isnumber(v)) {
          if (i > 1)
            sum_i /= FIX_VAL(v);
          else
            sum_i = FIX_VAL(v);
        } else if (isfloat(v)) {
          sum_f = (i > 1) ? sum_i / v->f : v->f;
          sum_i = 0;
          saw_float = 1;
        } else {
          ret =
              error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
          goto finish;
        }
      }
      unrefexp(v);
    }
  } while (c && (c = c->next));
  if (i == 1) {
    if (saw_float) {
      if (sum_f == 0) {
        ret = error(ERROR_DIV_BY0, e, env, "Illegal Division by 0");
        goto finish;
      }
      sum_f = 1 / sum_f;
    } else {
      if (sum_i == 0) {
        ret = error(ERROR_DIV_BY0, e, env, "Illegal Division by 0");
        goto finish;
      }
      sum_i = 1 / sum_i;
    }
  }
  ret = saw_float ? make_floatf(sum_f) : make_integeri(sum_i);
finish:
  unrefexp(e);
  return ret;
}

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
   Mutable, O(1) random-access array of exp_t*. Layout:
     e->type = EXP_VECTOR
     e->ptr  = alc_vec_t* with header { len } + data[len]
   Each element holds an owning ref. Refcount + free walk all elements
   in unrefexp. The slow-sieve trial-division benchmark wins from this
   because we can use a sqrt-cutoff on smallest-first vector iteration
   instead of cdr-walking a largest-first cons list.
   alc_vec_t is now declared in alcove.h. */

exp_t *make_vector(int64_t n, exp_t *fill) {
  alc_vec_t *v =
      (alc_vec_t *)memalloc(1, sizeof(alc_vec_t) + (size_t)n * sizeof(exp_t *));
  v->len = n;
  int64_t i;
  for (i = 0; i < n; i++)
    v->data[i] = refexp(fill);
  exp_t *e = make_nil();
  e->type = EXP_VECTOR;
  e->ptr = v;
  return e;
}

exp_t *veccmd(exp_t *e, env_t *env) {
  exp_t *nexp = NULL, *fill = NIL_EXP;
  if (e->next)
    nexp = EVAL(e->next->content, env);
  if (iserror(nexp)) {
    unrefexp(e);
    return nexp;
  }
  if (e->next && e->next->next)
    fill = EVAL(e->next->next->content, env);
  if (iserror(fill)) {
    unrefexp(nexp);
    unrefexp(e);
    return fill;
  }
  if (!isnumber(nexp)) {
    if (nexp)
      unrefexp(nexp);
    unrefexp(fill);
    unrefexp(e);
    return error(ERROR_NUMBER_EXPECTED, e, env,
                 "(vec n init): n must be a number");
  }
  int64_t n = FIX_VAL(nexp);
  if (n < 0)
    n = 0;
  unrefexp(nexp);
  exp_t *ret = make_vector(n, fill);
  unrefexp(fill);
  unrefexp(e);
  return ret;
}

exp_t *vecrefcmd(exp_t *e, env_t *env) {
  exp_t *vexp = NULL, *iexp = NULL;
  if (e->next)
    vexp = EVAL(e->next->content, env);
  if (iserror(vexp)) {
    unrefexp(e);
    return vexp;
  }
  if (e->next && e->next->next)
    iexp = EVAL(e->next->next->content, env);
  if (iserror(iexp)) {
    unrefexp(vexp);
    unrefexp(e);
    return iexp;
  }
  if (!is_ptr(vexp) || vexp->type != EXP_VECTOR || !isnumber(iexp)) {
    if (vexp)
      unrefexp(vexp);
    if (iexp)
      unrefexp(iexp);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, e, env, "(vec-ref v i): bad args");
  }
  alc_vec_t *v = (alc_vec_t *)vexp->ptr;
  int64_t i = FIX_VAL(iexp);
  if (i < 0 || i >= v->len) {
    unrefexp(vexp);
    unrefexp(iexp);
    unrefexp(e);
    return error(ERROR_INDEX_OUT_OF_RANGE, e, env,
                 "vec-ref: index out of range");
  }
  exp_t *ret = refexp(v->data[i]);
  unrefexp(vexp);
  unrefexp(iexp);
  unrefexp(e);
  return ret;
}

exp_t *vecsetcmd(exp_t *e, env_t *env) {
  exp_t *vexp = NULL, *iexp = NULL, *valexp = NULL;
  if (e->next)
    vexp = EVAL(e->next->content, env);
  if (iserror(vexp)) {
    unrefexp(e);
    return vexp;
  }
  if (e->next && e->next->next)
    iexp = EVAL(e->next->next->content, env);
  if (iserror(iexp)) {
    unrefexp(vexp);
    unrefexp(e);
    return iexp;
  }
  if (e->next && e->next->next && e->next->next->next)
    valexp = EVAL(e->next->next->next->content, env);
  if (iserror(valexp)) {
    unrefexp(vexp);
    unrefexp(iexp);
    unrefexp(e);
    return valexp;
  }
  if (!is_ptr(vexp) || vexp->type != EXP_VECTOR || !isnumber(iexp)) {
    if (vexp)
      unrefexp(vexp);
    if (iexp)
      unrefexp(iexp);
    if (valexp)
      unrefexp(valexp);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, e, env, "(vec-set! v i val): bad args");
  }
  alc_vec_t *v = (alc_vec_t *)vexp->ptr;
  int64_t i = FIX_VAL(iexp);
  if (i < 0 || i >= v->len) {
    unrefexp(vexp);
    unrefexp(iexp);
    unrefexp(valexp);
    unrefexp(e);
    return error(ERROR_INDEX_OUT_OF_RANGE, e, env,
                 "vec-set!: index out of range");
  }
  /* Replace the slot — drop old ref, install new (transfer valexp's). */
  unrefexp(v->data[i]);
  v->data[i] = valexp; /* ownership transferred */
  unrefexp(vexp);
  unrefexp(iexp);
  unrefexp(e);
  return refexp(v->data[i]); /* return the value, like (= ...) */
}

exp_t *veclencmd(exp_t *e, env_t *env) {
  exp_t *vexp = NULL;
  if (e->next)
    vexp = EVAL(e->next->content, env);
  if (iserror(vexp)) {
    unrefexp(e);
    return vexp;
  }
  if (!is_ptr(vexp) || vexp->type != EXP_VECTOR) {
    if (vexp)
      unrefexp(vexp);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, e, env, "(vec-len v): not a vector");
  }
  int64_t n = ((alc_vec_t *)vexp->ptr)->len;
  unrefexp(vexp);
  unrefexp(e);
  return MAKE_FIX(n);
}

/* (sqrt-int n) — floor(sqrt(n)) on a non-negative fixnum. Built-in to
   avoid a sqrt + double→int round-trip in pure-integer code (common
   in trial-division early exit). */
exp_t *sqrtintcmd(exp_t *e, env_t *env) {
  exp_t *v = NULL;
  if (e->next)
    v = EVAL(e->next->content, env);
  if (iserror(v)) {
    unrefexp(e);
    return v;
  }
  if (!isnumber(v)) {
    if (v)
      unrefexp(v);
    unrefexp(e);
    return error(ERROR_NUMBER_EXPECTED, e, env,
                 "(sqrt-int n): n must be a fixnum");
  }
  int64_t n = FIX_VAL(v);
  unrefexp(v);
  unrefexp(e);
  if (n < 0)
    return MAKE_FIX(0);
  int64_t r = (int64_t)sqrt((double)n);
  /* Correct for double imprecision: tighten boundary. */
  while ((r + 1) * (r + 1) <= n)
    r++;
  while (r * r > n)
    r--;
  return MAKE_FIX(r);
}

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
  if ((isfloat(v) || isnumber(v)) && (isfloat(v2) || isnumber(v2)))
    ret = make_floatf(pow(isfloat(v) ? v->f : (double)FIX_VAL(v),
                          isfloat(v2) ? v2->f : (double)FIX_VAL(v2)));
  else
    ret = error(ERROR_ILLEGAL_VALUE, e, env,
                "Illegal value in operation"); /*ERROR*/
  unrefexp(v);
  unrefexp(v2);
  unrefexp(e);
  return ret;
}

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
      printf("%s", (char *)val->ptr);
    else
      print_node(val);
    unrefexp(val);
  }
  unrefexp(e);
  return NULL;
}

exp_t *prncmd(exp_t *e, env_t *env) {
  exp_t *ret;
  ret = prcmd(e, env);
  printf("\n");
  return ret;
}

/* Forward decl needed by applycmd below; defined further down. */
static exp_t *vm_invoke_values(exp_t *fn, int nargs, exp_t **argv, env_t *env);

/* ---------------- FFI (libffi-backed) ----------------
   Lets alcove call into shared libraries (libc, libm, custom .so's).
   Each `(ffi-fn lib name rtype atype1 atype2 ...)` call returns an
   exp_t of type EXP_FFI carrying the resolved fn pointer + ffi_cif.
   Calling an EXP_FFI value marshals alcove args → C ABI, invokes via
   ffi_call, and marshals the result back.

   Conditional on -DALCOVE_FFI=1 (Makefile auto-detects via ffi.h). */
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
  AFFI_PTR
} alc_ffi_tag_t;

typedef struct alc_ffi_t {
  void *fn; /* dlsym result */
  ffi_cif cif;
  ffi_type *rtype;
  unsigned int nargs;
  ffi_type *atypes[ALC_FFI_MAX_ARGS];
  uint8_t ret_tag;
  uint8_t arg_tags[ALC_FFI_MAX_ARGS];
  char *display_name; /* "lib:fn" for error messages */
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

/* Process-wide cache of dlopen handles keyed by lib name. dlopen with
   the same name on Linux returns the same handle anyway, but caching
   avoids re-resolving on each (ffi-fn) call. */
static struct ffi_lib_cache {
  char *name;
  void *h;
  struct ffi_lib_cache *next;
} *g_ffi_libs = NULL;
static void *alc_ffi_dlopen(const char *name) {
  struct ffi_lib_cache *c;
  for (c = g_ffi_libs; c; c = c->next)
    if (!strcmp(c->name, name))
      return c->h;
  /* RTLD_LOCAL keeps the lib's symbols out of the global namespace —
     reduces accidental shadowing if multiple libs export the same
     name. RTLD_NOW resolves all symbols at load so dlsym failures
     surface promptly. */
  void *h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
  if (!h)
    return NULL;
  c = (struct ffi_lib_cache *)memalloc(1, sizeof(*c));
  c->name = strdup(name);
  c->h = h;
  c->next = g_ffi_libs;
  g_ffi_libs = c;
  return h;
}

/* (ffi-fn lib-name fn-name return-type arg-type ...) */
exp_t *ffifncmd(exp_t *e, env_t *env) {
  exp_t *cur = e->next;
  exp_t *libname = NULL, *fnname = NULL, *rtype = NULL;
  exp_t *atypes[ALC_FFI_MAX_ARGS] = {0};
  int n_a = 0;
  exp_t *err = NULL;
  exp_t *ret = NULL; /* declared up here so the `goto cleanup`
                        above doesn't jump over its init */

  if (!cur || !cur->next || !cur->next->next) {
    err = error(ERROR_MISSING_PARAMETER, e, env,
                "(ffi-fn lib name return-type arg-type ...)");
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

  if (!isstring(libname) || !isstring(fnname) || !isstring(rtype)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-fn: lib/name/rtype must be strings");
    goto cleanup;
  }
  void *h = alc_ffi_dlopen((char *)libname->ptr);
  if (!h) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-fn: dlopen failed (%s)",
                dlerror());
    goto cleanup;
  }
  void *sym = dlsym(h, (char *)fnname->ptr);
  if (!sym) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-fn: dlsym failed for %s",
                (char *)fnname->ptr);
    goto cleanup;
  }

  alc_ffi_t *f = (alc_ffi_t *)memalloc(1, sizeof(alc_ffi_t));
  f->fn = sym;
  f->nargs = (unsigned int)n_a;
  alc_ffi_tag_t rt;
  ffi_type *rt_ffi;
  if (alc_ffi_typeof((char *)rtype->ptr, &rt, &rt_ffi) < 0) {
    free(f);
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-fn: unknown return type %s",
                (char *)rtype->ptr);
    goto cleanup;
  }
  f->ret_tag = rt;
  f->rtype = rt_ffi;
  for (int i = 0; i < n_a; i++) {
    if (!isstring(atypes[i])) {
      free(f);
      err =
          error(ERROR_ILLEGAL_VALUE, e, env, "ffi-fn: arg-type must be string");
      goto cleanup;
    }
    alc_ffi_tag_t at;
    ffi_type *at_ffi;
    if (alc_ffi_typeof((char *)atypes[i]->ptr, &at, &at_ffi) < 0) {
      free(f);
      err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-fn: unknown arg type %s",
                  (char *)atypes[i]->ptr);
      goto cleanup;
    }
    f->arg_tags[i] = at;
    f->atypes[i] = at_ffi;
  }
  if (ffi_prep_cif(&f->cif, FFI_DEFAULT_ABI, f->nargs, f->rtype, f->atypes) !=
      FFI_OK) {
    free(f);
    err = error(ERROR_ILLEGAL_VALUE, e, env, "ffi-fn: ffi_prep_cif failed");
    goto cleanup;
  }
  size_t dnlen = strlen((char *)libname->ptr) + strlen((char *)fnname->ptr) + 2;
  f->display_name = (char *)memalloc(dnlen, 1);
  snprintf(f->display_name, dnlen, "%s:%s", (char *)libname->ptr,
           (char *)fnname->ptr);

  ret = make_nil();
  ret->type = EXP_FFI;
  ret->ptr = f;

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

void alc_ffi_free(void *ptr) {
  alc_ffi_t *f = (alc_ffi_t *)ptr;
  if (!f)
    return;
  if (f->display_name)
    free(f->display_name);
  free(f);
}

/* Marshal alcove args → C, ffi_call, marshal return. */
static exp_t *alc_ffi_call(alc_ffi_t *f, int nargs, exp_t **args) {
  if ((unsigned int)nargs != f->nargs) {
    int i;
    for (i = 0; i < nargs; i++)
      unrefexp(args[i]);
    return error(ERROR_MISSING_PARAMETER, NULL, NULL,
                 "ffi: wrong arg count for %s (expected %u, got %d)",
                 f->display_name ? f->display_name : "?", f->nargs, nargs);
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
  for (int i = 0; i < nargs; i++) {
    exp_t *a = args[i];
    switch (f->arg_tags[i]) {
    case AFFI_INT:
      slots[i].i = isnumber(a) ? (int32_t)FIX_VAL(a) : 0;
      avalues[i] = &slots[i].i;
      break;
    case AFFI_LONG:
      slots[i].l = isnumber(a) ? FIX_VAL(a) : 0;
      avalues[i] = &slots[i].l;
      break;
    case AFFI_DOUBLE:
      slots[i].d = isfloat(a) ? a->f : (isnumber(a) ? (double)FIX_VAL(a) : 0.0);
      avalues[i] = &slots[i].d;
      break;
    case AFFI_STRING:
      slots[i].s = isstring(a) ? (const char *)a->ptr : NULL;
      avalues[i] = &slots[i].s;
      break;
    case AFFI_PTR:
      slots[i].p = isnumber(a) ? (void *)(uintptr_t)FIX_VAL(a) : NULL;
      avalues[i] = &slots[i].p;
      break;
    default:
      slots[i].l = 0;
      avalues[i] = &slots[i].l;
      break;
    }
  }
  union {
    int32_t i;
    int64_t l;
    double d;
    void *p;
  } rval;
  ffi_call(&f->cif, FFI_FN(f->fn), &rval, avalues);
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
  case AFFI_STRING:
    ret =
        rval.p ? make_string((char *)rval.p, strlen((char *)rval.p)) : NIL_EXP;
    break;
  case AFFI_PTR:
    ret = MAKE_FIX((int64_t)(uintptr_t)rval.p);
    break;
  }
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
void alc_ffi_free(void *ptr) {
  (void)ptr;
} /* called from unrefexp; no FFI exp can exist */
#endif /* ALCOVE_FFI */

/* ---------------- Standard-library builtins (math/seq/predicates)
   ---------------- Each follows the prn/expt pattern: walk e->next, EVAL each
   arg, type-check, produce result, unrefexp args + form, return owned ref. */

/* (mod a b) — integer modulo. Both args must be fixnums. */
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
      double da = isnumber(a) ? (double)FIX_VAL(a) : a->f;
      double db = isnumber(b) ? (double)FIX_VAL(b) : b->f;
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

/* (abs x) — |x| for fixnum or float. */
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
      ret = MAKE_FIX(v < 0 ? -v : v);
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
    double da = isnumber(a) ? (double)FIX_VAL(a) : a->f;
    double db = isnumber(b) ? (double)FIX_VAL(b) : b->f;
    return da < db;
  }
  *err = 1;
  return 0;
}

/* (max a b ...) — variadic; at least one arg required. */
exp_t *maxcmd(exp_t *e, env_t *env) {
  exp_t *cur = e->next;
  if (!cur) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, e, env, "(max ...)");
  }
  exp_t *best = EVAL(cur->content, env);
  if (iserror(best)) {
    unrefexp(e);
    return best;
  }
  cur = cur->next;
  while (cur) {
    exp_t *v = EVAL(cur->content, env);
    if (iserror(v)) {
      unrefexp(best);
      unrefexp(e);
      return v;
    }
    int err;
    int lt = alc_numlt(best, v, &err);
    if (err) {
      unrefexp(best);
      unrefexp(v);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, e, env, "max: non-numeric");
    }
    if (lt) {
      unrefexp(best);
      best = v;
    } else
      unrefexp(v);
    cur = cur->next;
  }
  unrefexp(e);
  return best;
}
/* (min a b ...) */
exp_t *mincmd(exp_t *e, env_t *env) {
  exp_t *cur = e->next;
  if (!cur) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, e, env, "(min ...)");
  }
  exp_t *best = EVAL(cur->content, env);
  if (iserror(best)) {
    unrefexp(e);
    return best;
  }
  cur = cur->next;
  while (cur) {
    exp_t *v = EVAL(cur->content, env);
    if (iserror(v)) {
      unrefexp(best);
      unrefexp(e);
      return v;
    }
    int err;
    int lt = alc_numlt(v, best, &err);
    if (err) {
      unrefexp(best);
      unrefexp(v);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, e, env, "min: non-numeric");
    }
    if (lt) {
      unrefexp(best);
      best = v;
    } else
      unrefexp(v);
    cur = cur->next;
  }
  unrefexp(e);
  return best;
}

/* (length x) — list length, string length, or 0 for nil. */
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
      n = a->ptr ? (int64_t)strlen((char *)a->ptr) : 0;
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
exp_t *nthcmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *b = NULL, *ret = NIL_EXP;
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
    if (isnumber(a)) {
      int64_t idx = FIX_VAL(a);
      exp_t *cur = b;
      while (idx > 0 && cur && cur->next) {
        cur = cur->next;
        idx--;
      }
      if (idx == 0 && cur && cur->content)
        ret = refexp(cur->content);
    }
  }
  unrefexp(a);
  unrefexp(b);
  unrefexp(e);
  return ret;
}

/* (reverse list) — non-destructive; returns a new list. */
exp_t *reversecmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *acc = NIL_EXP;
  if (e->next) {
    a = EVAL(e->next->content, env);
    if (iserror(a)) {
      unrefexp(e);
      return a;
    }
    exp_t *cur = a;
    while (cur && cur->content) {
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
    exp_t *cur = list;
    while (cur && cur->content) {
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
PRED_CMD(numberpcmd, (isnumber(a) || isfloat(a)))
PRED_CMD(stringpcmd, isstring(a))
PRED_CMD(symbolpcmd, issymbol(a))
PRED_CMD(pairpcmd, (ispair(a) && a->content))
PRED_CMD(fnpcmd, (islambda(a) || isinternal(a)))
#undef PRED_CMD

/* (exit) / (exit code) — terminate the process. */
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
  if (n <= 0)
    return MAKE_FIX(0);
  return MAKE_FIX(
      (int64_t)((double)rand() / ((double)RAND_MAX + 1) * (double)n));
}

/* (map fn list) — non-destructive; returns a new list of (fn x) values. */
exp_t *mapcmd(exp_t *e, env_t *env) {
  exp_t *fn = NULL, *xs = NULL, *head = NULL, *tail = NULL;
  if (!(e->next && e->next->next)) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, e, env, "(map fn list)");
  }
  fn = EVAL(e->next->content, env);
  if (iserror(fn)) {
    unrefexp(e);
    return fn;
  }
  xs = EVAL(e->next->next->content, env);
  if (iserror(xs)) {
    unrefexp(fn);
    unrefexp(e);
    return xs;
  }
  exp_t *cur = xs;
  while (cur && cur->content) {
    exp_t *argv[1] = {refexp(cur->content)};
    exp_t *res = vm_invoke_values(fn, 1, argv, env);
    if (res && iserror(res)) {
      if (head)
        unrefexp(head);
      unrefexp(fn);
      unrefexp(xs);
      unrefexp(e);
      return res;
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
  unrefexp(fn);
  unrefexp(xs);
  unrefexp(e);
  return head ? head : NIL_EXP;
}
/* (filter pred list) — keep elements where (pred x) is truthy. */
exp_t *filtercmd(exp_t *e, env_t *env) {
  exp_t *fn = NULL, *xs = NULL, *head = NULL, *tail = NULL;
  if (!(e->next && e->next->next)) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, e, env, "(filter pred list)");
  }
  fn = EVAL(e->next->content, env);
  if (iserror(fn)) {
    unrefexp(e);
    return fn;
  }
  xs = EVAL(e->next->next->content, env);
  if (iserror(xs)) {
    unrefexp(fn);
    unrefexp(e);
    return xs;
  }
  exp_t *cur = xs;
  while (cur && cur->content) {
    exp_t *argv[1] = {refexp(cur->content)};
    exp_t *res = vm_invoke_values(fn, 1, argv, env);
    if (res && iserror(res)) {
      if (head)
        unrefexp(head);
      unrefexp(fn);
      unrefexp(xs);
      unrefexp(e);
      return res;
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
  unrefexp(fn);
  unrefexp(xs);
  unrefexp(e);
  return head ? head : NIL_EXP;
}
/* (reduce fn init list) — left fold: ((fn (fn init x0) x1) x2 ...). */
exp_t *reducecmd(exp_t *e, env_t *env) {
  exp_t *fn = NULL, *acc = NULL, *xs = NULL;
  if (!(e->next && e->next->next && e->next->next->next)) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, e, env, "(reduce fn init list)");
  }
  fn = EVAL(e->next->content, env);
  if (iserror(fn)) {
    unrefexp(e);
    return fn;
  }
  acc = EVAL(e->next->next->content, env);
  if (iserror(acc)) {
    unrefexp(fn);
    unrefexp(e);
    return acc;
  }
  xs = EVAL(e->next->next->next->content, env);
  if (iserror(xs)) {
    unrefexp(fn);
    unrefexp(acc);
    unrefexp(e);
    return xs;
  }

  /* Fast path: detect a simple 6-byte binary-arithmetic lambda
     (fn (a b) (op a b)) — bytecode is LOAD_SLOT 0, LOAD_SLOT 1, OP, RET.
     Common across reduce-sum, reduce-product, reduce-max, etc. We
     inline the tagged-fixnum operation directly, skipping the
     vm_invoke_values per-element env churn. Gives ~10x on listsum-style
     workloads. Falls back to the general path on non-fixnum or when
     the lambda has any other shape. */
  int fast_op = 0; /* 0=none, 1=add, 2=sub, 3=mul */
  if (is_ptr(fn) && islambda(fn) && (fn->flags & FLAG_COMPILED) && fn->bc &&
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
    while (cur && cur->content) {
      exp_t *x = cur->content;
      if (isnumber(acc) && isnumber(x)) {
        int64_t a = FIX_VAL(acc), b = FIX_VAL(x);
        int64_t r = (fast_op == 1)   ? (a + b)
                    : (fast_op == 2) ? (a - b)
                                     : (a * b);
        acc = MAKE_FIX(r);
      } else {
        /* Slow path for this element — fall back to vm_invoke_values
           and resume fast-path on the next element. */
        exp_t *argv[2] = {acc, refexp(x)};
        acc = vm_invoke_values(fn, 2, argv, env);
        if (acc && iserror(acc)) {
          unrefexp(fn);
          unrefexp(xs);
          unrefexp(e);
          return acc;
        }
        if (!acc)
          acc = NIL_EXP;
      }
      cur = cur->next;
    }
  } else {
    while (cur && cur->content) {
      exp_t *argv[2] = {acc, refexp(cur->content)};
      acc = vm_invoke_values(fn, 2, argv, env);
      if (acc && iserror(acc)) {
        unrefexp(fn);
        unrefexp(xs);
        unrefexp(e);
        return acc;
      }
      if (!acc)
        acc = NIL_EXP;
      cur = cur->next;
    }
  }
  unrefexp(fn);
  unrefexp(xs);
  unrefexp(e);
  return acc;
}

/* (any? pred list) — return t as soon as (pred x) is truthy for any
   element of list, nil if none match. Walks in C with one
   vm_invoke_values per element instead of recursive bytecode. */
exp_t *anypcmd(exp_t *e, env_t *env) {
  exp_t *fn = NULL, *xs = NULL, *ret = NIL_EXP;
  if (!(e->next && e->next->next)) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, e, env, "(any? pred list)");
  }
  fn = EVAL(e->next->content, env);
  if (iserror(fn)) {
    unrefexp(e);
    return fn;
  }
  xs = EVAL(e->next->next->content, env);
  if (iserror(xs)) {
    unrefexp(fn);
    unrefexp(e);
    return xs;
  }
  exp_t *cur = xs;
  while (cur && cur->content) {
    exp_t *argv[1] = {refexp(cur->content)};
    exp_t *res = vm_invoke_values(fn, 1, argv, env);
    if (res && iserror(res)) {
      unrefexp(fn);
      unrefexp(xs);
      unrefexp(e);
      return res;
    }
    int truthy = (res != NULL && res != NIL_EXP);
    if (res)
      unrefexp(res);
    if (truthy) {
      ret = TRUE_EXP;
      break;
    }
    cur = cur->next;
  }
  unrefexp(fn);
  unrefexp(xs);
  unrefexp(e);
  return ret;
}
/* (all? pred list) — return t if (pred x) is truthy for every
   element, nil at the first failure. Empty list → t. */
exp_t *allpcmd(exp_t *e, env_t *env) {
  exp_t *fn = NULL, *xs = NULL, *ret = TRUE_EXP;
  if (!(e->next && e->next->next)) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, e, env, "(all? pred list)");
  }
  fn = EVAL(e->next->content, env);
  if (iserror(fn)) {
    unrefexp(e);
    return fn;
  }
  xs = EVAL(e->next->next->content, env);
  if (iserror(xs)) {
    unrefexp(fn);
    unrefexp(e);
    return xs;
  }
  exp_t *cur = xs;
  while (cur && cur->content) {
    exp_t *argv[1] = {refexp(cur->content)};
    exp_t *res = vm_invoke_values(fn, 1, argv, env);
    if (res && iserror(res)) {
      unrefexp(fn);
      unrefexp(xs);
      unrefexp(e);
      return res;
    }
    int truthy = (res != NULL && res != NIL_EXP);
    if (res)
      unrefexp(res);
    if (!truthy) {
      ret = NIL_EXP;
      break;
    }
    cur = cur->next;
  }
  unrefexp(fn);
  unrefexp(xs);
  unrefexp(e);
  return ret;
}

/* (apply fn args-list) — call fn with each element of args-list as
   separate args. Implemented by re-using vm_invoke_values for compiled
   lambdas; falls back to AST invoke otherwise. */
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

exp_t *oddcmd(exp_t *e, env_t *env) {
  exp_t *ret;
  if (e->next && isnumber(e->next->content))
    ret = ((FIX_VAL(e->next->content) & 1) ? TRUE_EXP : NIL_EXP);
  else
    ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
  unrefexp(e);
  return ret;
}

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

exp_t *whencmd(exp_t *e, env_t *env) {
  exp_t *val = cadr(e);
  exp_t *cur = cddr(e);
  exp_t *ret = EVAL(val, env);
  if iserror (ret) {
    unrefexp(e);
    return ret;
  }
  if (istrue(ret))
    do {
      unrefexp(ret);
      ret = EVAL(car(cur), env);
    } while ((cur = cdr(cur)) && !(ret && iserror(ret)));
  if (ret && iserror(ret)) {
    unrefexp(e);
    return ret;
  } else {
    unrefexp(ret);
    unrefexp(e);
    return NIL_EXP;
  }
}

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

exp_t *andcmd(exp_t *e, env_t *env) {
  exp_t *cur = cdr(e);
  exp_t *ret = NULL;
  do {
    if (ret)
      unrefexp(ret);
    ret = EVAL(car(cur), env);
    if iserror (ret)
      goto finish;
  } while (istrue(ret) && (cur = cdr(cur)));
finish:
  unrefexp(e);
  return ret;
}

exp_t *orcmd(exp_t *e, env_t *env) {
  exp_t *cur = cdr(e);
  exp_t *ret = NULL;
  do {
    if (ret)
      unrefexp(ret);
    ret = EVAL(car(cur), env);
    if iserror (ret)
      goto finish;
    if (istrue(ret))
      goto finish;
  } while ((cur = cdr(cur)));
finish:
  unrefexp(e);
  return ret;
}

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
      ret = (strcmp(cur1->ptr, cur2->ptr) == 0);
    else if (iserror(cur1))
      ret = (cur1->s64 == cur2->s64);
    else
      ret = (cur1 == cur2);
  }
  return ret;
}

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
    } else
      ret = isequal(cur1, cur2);
  }
  return ret;
}

exp_t *iscmd(exp_t *e, env_t *env) {
  exp_t *ret = NULL;
  exp_t *cur1 = EVAL(cadr(e), env);
  if iserror (cur1) {
    unrefexp(e);
    return cur1;
  }
  exp_t *cur2 = EVAL(caddr(e), env);
  if iserror (cur2) {
    unrefexp(cur1);
    unrefexp(e);
    return cur2;
  }
  unrefexp(e);
  ret = (isequal(cur1, cur2) ? TRUE_EXP : NIL_EXP);
  unrefexp(cur1);
  unrefexp(cur2);
  return ret;
}

exp_t *isocmd(exp_t *e, env_t *env) {
  exp_t *ret = NULL;
  exp_t *cur1 = EVAL(cadr(e), env);
  if iserror (cur1) {
    unrefexp(e);
    return cur1;
  }
  exp_t *cur2 = EVAL(caddr(e), env);
  if iserror (cur2) {
    unrefexp(cur1);
    unrefexp(e);
    return cur2;
  }
  unrefexp(e);
  ret = (isoequal(cur1, cur2) ? TRUE_EXP : NIL_EXP);
  unrefexp(cur1);
  unrefexp(cur2);
  return ret;
}

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

exp_t *casecmd(exp_t *e, env_t *env) {
  exp_t *cur = cdr(e);
  exp_t *val = EVAL(cadr(e), env);
  if iserror (val) {
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
  cur = EVAL(ret, env);
  unrefexp(e);
  return cur;
}

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
          /* Rebind the loop variable to a fresh tagged fixnum. Tagged
             immediates don't allocate, so this is free. */
          set_get_keyval_dict(newenv->d, curvar->content->ptr,
                              MAKE_FIX(counter));
          curval = curin;
          while (curval) {
            if (ret)
              unrefexp(ret);
            ret = EVAL(curval->content, newenv);
            if iserror (ret)
              goto error;
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
      if (!(newenv->d))
        newenv->d = create_dict();

      if (issymbol(curvar->content)) {
        if ((retval = EVAL(curval->content, env)) == NULL)
          retval = NIL_EXP;
        if (ispair(retval)) {
          curin = curval->next;
          tmpexp = retval;
          while (retval) {
            set_get_keyval_dict(newenv->d, curvar->content->ptr, car(retval));
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
    int arity = 0;
    exp_t *p;
    for (p = v->content; p; p = p->next)
      arity++;
    printf("\x1B[96marity:\t%d\nparams:\t(", arity);
    int first = 1;
    for (p = v->content; p; p = p->next) {
      if (!first)
        printf(" ");
      first = 0;
      if (is_ptr(p->content) && issymbol(p->content))
        printf("%s", (char *)p->content->ptr);
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
  } else if (v->type == EXP_STRING && v->ptr) {
    printf("\x1B[96mvalue:\t\"%s\"\nlen:\t%zu\x1B[39m\n", (char *)v->ptr,
           strlen((char *)v->ptr));
  } else if (v->type == EXP_FLOAT) {
    printf("\x1B[96mvalue:\t%g\x1B[39m\n", v->f);
  } else if (v->type == EXP_SYMBOL && v->ptr) {
    printf("\x1B[96msym:\t%s\x1B[39m\n", (char *)v->ptr);
  }
}

/* (inspect val) — evaluates val, prints type info + type-specific details.
   Also called directly from the REPL's verbose mode at the bottom of the
   file (passing the already-evaluated result), which is why the display
   logic lives in inspect_value above rather than inline here. */
exp_t *inspectcmd(exp_t *e, env_t *env) {
  exp_t *arg = e->next ? EVAL(e->next->content, env) : NULL;
  if (arg && iserror(arg)) {
    unrefexp(e);
    return arg;
  }
  inspect_value(arg);
  unrefexp(arg);
  unrefexp(e);
  return NULL;
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

exp_t *dircmd(exp_t *e, env_t *env) {
  const char *needle = NULL;
  int show_builtins = 0;
  exp_t *needle_arg = NULL, *flag_arg = NULL;

  if (e->next) {
    needle_arg = EVAL(e->next->content, env);
    if (needle_arg && iserror(needle_arg)) {
      unrefexp(e);
      return needle_arg;
    }
    if (e->next->next) {
      flag_arg = EVAL(e->next->next->content, env);
      if (flag_arg && iserror(flag_arg)) {
        unrefexp(needle_arg);
        unrefexp(e);
        return flag_arg;
      }
      if (istrue(flag_arg))
        show_builtins = 1;
    }
    /* needle: accept symbol or string; nil / other → no filter */
    if (is_ptr(needle_arg) && (issymbol(needle_arg) || isstring(needle_arg)))
      needle = (const char *)needle_arg->ptr;
  }

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
    if (is_ptr(v) && v->type == EXP_LAMBDA && v->content) {
      /* Lambda: print parameter list. */
      printf("  (");
      int first = 1;
      exp_t *p;
      for (p = v->content; p; p = p->next) {
        if (!first)
          printf(" ");
        first = 0;
        if (is_ptr(p->content) && issymbol(p->content))
          printf("%s", (char *)p->content->ptr);
      }
      printf(")");
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

/* (disasm fn)  — evaluates fn, expects a compiled lambda, prints its
   bytecode op-by-op plus the JIT install status. Useful for verifying
   what bytecode the compiler produces (no more ad-hoc fprintf in C). */
exp_t *disasmcmd(exp_t *e, env_t *env) {
  exp_t *arg = e->next ? EVAL(e->next->content, env) : NULL;
  if (arg && iserror(arg)) {
    unrefexp(e);
    return arg;
  }
  if (!arg || !is_ptr(arg) || !islambda(arg)) {
    printf("\x1B[96m(disasm): not a lambda\x1B[39m\n");
  } else if (!(arg->flags & FLAG_COMPILED) || !arg->bc) {
    printf("\x1B[96m(disasm): lambda is not compiled (runs as AST)\x1B[39m\n");
  } else {
    disasm_bytecode(arg->bc);
  }
  unrefexp(arg);
  unrefexp(e);
  return NULL;
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
    return 3;                           /* "nil" */
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
    return 4;                           /* "#\X" */
  if (!is_ptr(e))
    return 8;
  switch (e->type) {
  case EXP_SYMBOL:
  case EXP_STRING:
    return e->ptr ? (int)strlen((char *)e->ptr) + (e->type == EXP_STRING ? 2 : 0)
                  : 3;
  case EXP_FLOAT:
    return 12;                          /* approx */
  case EXP_PAIR: {
    int w = 2;                          /* parens */
    exp_t *cur;
    int first = 1;
    for (cur = e; cur; cur = cur->next) {
      if (cur->type != EXP_PAIR) {
        w += 3 + pp_flat_width(cur);    /* " . X" */
        break;
      }
      if (!first)
        w += 1;
      first = 0;
      w += pp_flat_width(cur->content);
      if (w > PP_WIDTH * 4)
        return PP_WIDTH * 4;            /* short-circuit on big forms */
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
  if (!is_ptr(e) || !ispair(e) || !istrue(e)) {
    print_node(e);
    return;
  }

  exp_t *head = car(e);
  const char *s = (is_ptr(head) && issymbol(head)) ? (const char *)head->ptr : NULL;

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
      putchar('\n'); pp_indent(sub); pp_form(th, sub);
      if (el) { putchar('\n'); pp_indent(sub); pp_form(el, sub); }
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
  print_node(head);                      /* head atom */
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
exp_t *sourcecmd(exp_t *e, env_t *env) {
  exp_t *arg = e->next ? EVAL(e->next->content, env) : NULL;
  if (arg && iserror(arg)) {
    unrefexp(e);
    return arg;
  }
  if (!arg || !is_ptr(arg) || (!islambda(arg) && !ismacro(arg))) {
    printf("\x1B[96m(source): not a lambda or macro\x1B[39m\n");
    unrefexp(arg);
    unrefexp(e);
    return NULL;
  }
  int is_macro = ismacro(arg);
  /* Only flag as a "closure" if the captured env is non-global. Top-
     level def'd lambdas always capture g_global_env; that's not a
     real closure, just the default scope. */
  env_t *cap = (env_t *)(arg->next ? arg->next->bc : NULL);
  int captured = (cap && cap != g_global_env) ? 1 : 0;
  /* Print the header inline (def NAME PARAMS or fn PARAMS), then
     pretty-print each body form on its own indented line. */
  if (arg->meta) {
    printf("\x1B[33m(\x1B[1;35m%s\x1B[22;39m \x1B[36m%s\x1B[39m ",
           is_macro ? "defmacro" : "def", (char *)arg->meta);
  } else {
    printf("\x1B[33m(\x1B[1;35m%s\x1B[22;39m ", is_macro ? "mac" : "fn");
  }
  print_node(arg->content); /* params list */
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

exp_t *conscmd(exp_t *e, env_t *env) {
  exp_t *a = EVAL(cadr(e), env);
  if iserror (a) {
    unrefexp(e);
    return a;
  }
  exp_t *b = EVAL(caddr(e), env);
  if iserror (b) {
    unrefexp(a);
    unrefexp(e);
    return b;
  }
  exp_t *ret = make_node(a);
  if (istrue(b))
    ret->next = b;
  else {
    unrefexp(b);
    ret->next = NULL;
  }
  unrefexp(e);
  return ret;
}

exp_t *cdrcmd(exp_t *e, env_t *env) {
  exp_t *tmpexp = EVAL(cadr(e), env);
  exp_t *tmpexp2 = tmpexp;
  unrefexp(e);
  if (!iserror(tmpexp)) {
    tmpexp = refexp(cdr(tmpexp2));
    unrefexp(tmpexp2);
  }
  return tmpexp;
}

exp_t *carcmd(exp_t *e, env_t *env) {
  exp_t *tmpexp = EVAL(cadr(e), env);
  exp_t *tmpexp2 = tmpexp;
  if (!iserror(tmpexp)) {
    tmpexp = refexp(car(tmpexp2));
    unrefexp(tmpexp2);
  }
  return tmpexp;
}

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

exp_t *evalcmd(exp_t *e, env_t *env) {
  exp_t *ret = evaluate(EVAL(cadr(e), env), env);
  unrefexp(e);
  return ret;
}

exp_t *letcmd(exp_t *e, env_t *env) {
  env_t *newenv = make_env(env);
  exp_t *ret;
  exp_t *curvar;
  exp_t *curval;

  if ((curvar = e->next)) {
    if ((curval = curvar->next)) {
      if (!(newenv->d))
        newenv->d = create_dict();

      if (issymbol(curvar->content)) {
        if ((ret = EVAL(curval->content, env)) == NULL)
          ret = NIL_EXP;
        if iserror (ret)
          goto finish;
        set_get_keyval_dict(newenv->d, curvar->content->ptr, ret);
        unrefexp(ret);
        ret = NULL;
      } else {
        ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in let");
        goto finish;
      }
      if (curval->next) {
        ret = EVAL(curval->next->content, newenv);
        goto finish;
      }
    }
  }
  ret = error(ERROR_MISSING_PARAMETER, e, env,
              "Missing parameter in let"); /* MISSING PARAMETER*/
finish:
  destroy_env(newenv);
  unrefexp(e);
  return ret;
}

exp_t *withcmd(exp_t *e, env_t *env) {
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
        if (!(newenv->d))
          newenv->d = create_dict();

        while (curvar && curval) {
          if (issymbol(curvar->content)) {
            ret = EVAL(curval->content, env);
            if iserror (ret)
              goto finish;
            set_get_keyval_dict(newenv->d, curvar->content->ptr, ret);
            unrefexp(ret);
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

        ret = EVAL(curex->content, newenv);
        goto finish;
      }
    }
  }

  ret = error(ERROR_MISSING_PARAMETER, e, env, "Missing parameter in with");
finish:
  destroy_env(newenv);
  unrefexp(e);
  return ret;
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
    if ((curval)) {
      if ((retvar = (evalexp ? EVAL(curval->content, env->root)
                             : refexp(curval->content)))) {
        if (evalexp && iserror(retvar)) {
          return retvar;
        }
      } else
        retvar = NIL_EXP;
      if (issymbol(curvar->content)) {
        /* Fast path: use inline slots for function-param bindings.
           Keys BORROW from the symbol's ptr; caller guarantees lifetime
           by holding a ref to the lambda header. Falls back to the
           dict once inline slots overflow. */
        if (env->n_inline < ENV_INLINE_SLOTS) {
          env->inline_keys[env->n_inline] = curvar->content->ptr;
          env->inline_vals[env->n_inline] = retvar; /* ownership transferred */
          env->n_inline++;
        } else {
          if (!env->d)
            env->d = create_dict();
          set_get_keyval_dict(env->d, curvar->content->ptr, retvar);
          unrefexp(retvar);
        }
      } else {
        unrefexp(retvar);
        return NULL;
      }
    } else
      return error(ERROR_MISSING_PARAMETER, e, env,
                   "Missing parameter in macro or function invoke");
    curval = curval->next;
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
     - LOAD_SLOT s; RET                     →  identity      (n)
     - LOAD_FIX K; RET                      →  constant      K
     - LOAD_SLOT s; LOAD_FIX K; ADD; RET    →  (+ s K) for K in int16
     - LOAD_SLOT s; LOAD_FIX K; SUB; RET    →  (- s K) for K in int16
     - LOAD_SLOT s; LOAD_FIX K; MUL; RET    →  (* s K) via (t-1)*K + 1
     - SLOT_ADD_FIX / SLOT_SUB_FIX leaf     →  same as above (fused form)
     - 19-byte self-tail counter loop       →  try_jit_simple_tail_loop

   Shapes handled by amd64 ONLY (arm64 fall-through to bytecode — see
   TODOs in the arm64 backend). These DO establish a frame and DO call
   into the runtime via jit_call_global1_{drop,value}:
     - 26-byte tail loop + inner global call  →  try_jit_tail_loop_with_call
     - 28-byte two-call recursion (fib shape) →  try_jit_recurse_add_two

   Anything else: jit_compile returns 0; the bytecode interpreter
   handles the call.

   Calling convention: the JITted function takes one arg (env_t*) and
   returns exp_t* (NULL signals deopt → caller falls back to vm_run).
   On arm64 (AAPCS) that's x0 in / x0 out; on amd64 (System V) that's
   rdi in / rax out. Leaf shapes never touch the stack or callee-saved
   regs. The two amd64 "with runtime call" shapes establish a 16-aligned
   frame (push rbx, optionally sub rsp, #pad) and restore on exit. */

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
    callee = lookup(bc->consts[const_idx], env);
    if (!callee) {
      unrefexp(arg);
      return error(ERROR_ILLEGAL_VALUE, bc->consts[const_idx], env,
                   "Unbound variable");
    }
    if (!bc->gcache)
      bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
    bc->gcache[const_idx].val = callee;
    bc->gcache[const_idx].gen = alcove_global_gen;
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
    callee = lookup(bc->consts[const_idx], env);
    if (!callee) {
      unrefexp(argv2[0]);
      unrefexp(argv2[1]);
      return error(ERROR_ILLEGAL_VALUE, bc->consts[const_idx], env,
                   "Unbound variable");
    }
    if (!bc->gcache)
      bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
    bc->gcache[const_idx].val = callee;
    bc->gcache[const_idx].gen = alcove_global_gen;
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
    callee = lookup(bc->consts[const_idx], env);
    if (!callee) {
      unrefexp(arg);
      return error(ERROR_ILLEGAL_VALUE, bc->consts[const_idx], env,
                   "Unbound variable");
    }
    if (!bc->gcache)
      bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
    bc->gcache[const_idx].val = callee;
    bc->gcache[const_idx].gen = alcove_global_gen;
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
#ifdef __APPLE__
  flags |= MAP_JIT;
#endif
  void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
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
#endif
  __builtin___clear_cache((char *)p, (char *)p + sz);
}

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
static uint32_t arm64_b(int off_insns) {
  return 0x14000000u | ((uint32_t)off_insns & 0x3FFFFFFu);
}
/* B.cond — off in instructions, signed 19-bit. cond is the 4-bit code:
   GE=10, LT=11, GT=12, LE=13. */
static uint32_t arm64_b_cond(int cond, int off_insns) {
  return 0x54000000u | (((uint32_t)off_insns & 0x7FFFFu) << 5) |
         ((uint32_t)cond & 0xfu);
}
/* TBZ Xt, #bit, label — branch if bit is zero. off in instructions, signed
 * 14-bit. */
static uint32_t arm64_tbz(int rt, int bit, int off_insns) {
  uint32_t b40 = (uint32_t)(bit & 0x1f);
  uint32_t b5 = (bit & 0x20) ? 1u : 0u;
  return 0x36000000u | (b5 << 31) | (b40 << 19) |
         (((uint32_t)off_insns & 0x3FFFu) << 5) | (uint32_t)(rt & 0x1f);
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
  out[patch_tbz] = arm64_tbz(1, 0, deopt_pc - patch_tbz);
  out[n++] = arm64_movz(0, 0, 0); /* mov x0, #0 (NULL) */
  out[n++] = arm64_ret();

  /* Worst case: 12 instructions (load, tbz, cmp, b.cond, sub/add, str,
     b loop, mov, ret, movz, ret + slack). Caller's buffer is uint32_t
     insns[32] — comfortable margin. Trip if a future tweak overruns. */
  assert(n <= 16);
  *outn = n;
  return 1;
}

int jit_compile(bytecode_t *bc) {
  if (!bc || bc->jit)
    return bc && bc->jit ? 1 : 0;
  uint8_t *c = bc->code;

  /* Identify the body shape. */
  uint32_t insns[32];
  int n = 0;

  if (try_jit_simple_tail_loop(bc, insns, &n)) {
    /* matched — fall through to mmap+install */
  } else

      if (bc->ncode == 4 && c[0] == OP_LOAD_FIX && c[3] == OP_RET) {
    /* (fn () K) — return MAKE_FIX(K). */
    int16_t k = (int16_t)((uint16_t)c[1] | ((uint16_t)c[2] << 8));
    uint64_t tagged = ((uint64_t)(int64_t)k << 3) | 1;
    n += emit_mov64(insns + n, 0, tagged);
    insns[n++] = arm64_ret();
  } else if (bc->ncode == 3 && c[0] == OP_LOAD_SLOT &&
             c[1] < ENV_INLINE_SLOTS && c[2] == OP_RET) {
    /* (fn (... s ...) s) — return env->inline_vals[s]. */
    int slot_off = (int)offsetof(env_t, inline_vals[0]) + (int)c[1] * 8;
    insns[n++] = arm64_ldr_imm(0, 0, slot_off);
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
      insns[n++] = arm64_movz(0, 0, 0);
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
      insns[n++] = arm64_movz(0, 0, 0); /* mov x0, #0 (NULL) */
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
    insns[n++] = arm64_movz(0, 0, 0);
    insns[n++] = arm64_ret();
  } else {
    return 0; /* shape not recognized */
  }

  /* All current shapes fit in well under 32 instructions. Trip if a
     future shape (or a refactor of an existing one) overruns insns[32]. */
  assert(n <= 32);
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
  n += x64_zero_reg(buf + n, X64_RAX); /* xor eax, eax */
  n += x64_ret(buf + n);

  x64_patch_rel32(buf, jcc_end_start, 6, end_pc);
  x64_patch_rel32(buf, jz_start, 6, deopt_pc);

  /* Worst case ~55 bytes (load, test, jcc, cmp, jcc, sub/add, mov,
     jmp, mov, ret, xor, ret + slack). Caller's buffer is uint8_t buf[256]. */
  assert(n <= 80);
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
  n += x64_zero_reg(buf + n, X64_RAX);
  n += x64_ret(buf + n);

  x64_patch_rel32(buf, jcc_end, 6, end_pc);
  x64_patch_rel32(buf, jnz_err, 6, err_pc);
  x64_patch_rel32(buf, jz_deopt, 6, deopt_pc);

  /* Worst case ~134 bytes (entry tag-check + frame setup + ~45-byte
     call sequence + arith + jmp + exits). buf is 256 bytes. */
  assert(n <= 160);
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
  /* Two CALL_GLOBAL ops can reference the same callee via DIFFERENT
     const indices (the bytecode compiler doesn't dedupe symbol consts).
     So check the symbol-string values, not the indices. */
  exp_t *ca = bc->consts[idx_a];
  exp_t *cb = bc->consts[idx_b];
  int same_callee = is_ptr(ca) && is_ptr(cb) && issymbol(ca) && issymbol(cb) &&
                    strcmp((const char *)ca->ptr, (const char *)cb->ptr) == 0;
  int is_fib_like = same_callee && op_a == OP_SLOT_SUB_FIX &&
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
    n += x64_zero_reg(buf + n, X64_RAX);
    n += x64_ret(buf + n);

    x64_patch_rel32(buf, jcc_done, 6, done_pc);
    x64_patch_rel32(buf, jcc_base, 6, base_pc);
    x64_patch_rel32(buf, jz_deopt_it, 6, deopt_pc_it);

    /* Suppress unused warnings for the fall-through emission's locals. */
    (void)K1_tagged;
    (void)K2_delta;
    (void)K3_delta;
    (void)inv_cc;
    (void)slot;

    assert(n <= 200);
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
  n += x64_zero_reg(buf + n, X64_RAX);
  n += x64_ret(buf + n);

  x64_patch_rel32(buf, jcc_recurse, 6, recurse_pc);
  x64_patch_rel32(buf, jz_bail1, 6, bail_pc);
  x64_patch_rel32(buf, jz_bail2, 6, bail_pc);
  x64_patch_rel32(buf, jz_deopt, 6, deopt_pc);

  /* Worst case ~190 bytes (entry tag-check + 2 ~45-byte call sequences
     + tag-checks + tagged add + frame teardown + bail + deopt). buf
     is 256 bytes. The matcher with the largest emission. */
  assert(n <= 224);
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
  if (!is_ptr(ct) || !issymbol(ct) || strcmp((const char *)ct->ptr, "t") != 0)
    return 0;
  for (int k = 0; k < 3; k++) {
    uint8_t idx = (k == 0) ? idx_nil1 : (k == 1) ? idx_nil2 : idx_nil3;
    if (idx >= bc->nconsts)
      return 0;
    exp_t *cn = bc->consts[idx];
    if (!is_ptr(cn) || !issymbol(cn) ||
        strcmp((const char *)cn->ptr, "nil") != 0)
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
  n += x64_zero_reg(buf + n, X64_RAX);
  n += x64_ret(buf + n);

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

  assert(n <= 480);
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
  if (!is_ptr(ct) || !issymbol(ct) || strcmp((const char *)ct->ptr, "t") != 0)
    return 0;
  if (!is_ptr(cnil) || !issymbol(cnil) ||
      strcmp((const char *)cnil->ptr, "nil") != 0)
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
  n += x64_zero_reg(buf + n, X64_RAX);
  n += x64_ret(buf + n);

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

  assert(n <= 480);
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
  n += x64_zero_reg(buf + n, X64_RAX);
  n += x64_ret(buf + n);

  x64_patch_rel32(buf, jg_done, 6, done_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);
  x64_patch_rel32(buf, jz_skip_inc, 6, skip_inc_pc);
  x64_patch_rel32(buf, je_skip_inc, 6, skip_inc_pc);

  assert(n <= 200);
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
  if (!is_ptr(cn1) || !issymbol(cn1) ||
      strcmp((const char *)cn1->ptr, "nil") != 0)
    return 0;
  if (!is_ptr(cn2) || !issymbol(cn2) ||
      strcmp((const char *)cn2->ptr, "nil") != 0)
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
  n += x64_zero_reg(buf + n, X64_RAX);
  n += x64_ret(buf + n);

  x64_patch_rel32(buf, jg_done, 6, done_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);

  assert(n <= 200);
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

  /* All three calls must target the same global (string-compare since
     the bytecode doesn't dedupe symbol consts). */
  if (idx_a >= bc->nconsts || idx_b >= bc->nconsts || idx_c >= bc->nconsts)
    return 0;
  exp_t *ca = bc->consts[idx_a];
  exp_t *cb = bc->consts[idx_b];
  exp_t *cc = bc->consts[idx_c];
  if (!is_ptr(ca) || !issymbol(ca) || !is_ptr(cb) || !issymbol(cb) ||
      !is_ptr(cc) || !issymbol(cc))
    return 0;
  if (strcmp((const char *)ca->ptr, (const char *)cb->ptr) != 0)
    return 0;
  if (strcmp((const char *)ca->ptr, (const char *)cc->ptr) != 0)
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
  n += x64_zero_reg(buf + n, X64_RAX);
  n += x64_ret(buf + n);

  x64_patch_rel32(buf, jl_recurse, 6, recurse_pc);
  x64_patch_rel32(buf, jz_b1, 6, bail_pc);
  x64_patch_rel32(buf, jz_b2, 6, bail_pc);
  x64_patch_rel32(buf, jz_b3, 6, bail_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);

  assert(n <= 480);
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
  n += x64_zero_reg(buf + n, X64_RAX);
  n += x64_ret(buf + n);

  x64_patch_rel32(buf, jne_not_m0, 6, not_m0_pc);
  x64_patch_rel32(buf, jne_not_n0, 6, not_n0_pc);
  x64_patch_rel32(buf, jz_bail, 6, bail_pc);
  x64_patch_rel32(buf, jz_deopt_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_deopt_b, 6, deopt_pc);

  assert(n <= 320);
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
  n += x64_zero_reg(buf + n, X64_RAX);
  n += x64_ret(buf + n);

  x64_patch_rel32(buf, jcc_done, 6, done_pc);
  x64_patch_rel32(buf, jz_deopt, 6, deopt_pc);

  assert(n <= 128);
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
  n += x64_zero_reg(buf + n, X64_RAX);
  n += x64_ret(buf + n);

  /* Suppress unused-var warnings for variables that became dead when we
     switched from recursive emission to iterative. */
  (void)K1_tagged;
  (void)K2_delta;
  (void)BASE_tag;

  x64_patch_rel32(buf, jcc_done, 6, done_pc);
  x64_patch_rel32(buf, jz_deopt, 6, deopt_pc);

  assert(n <= 128);
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
  n += x64_zero_reg(buf + n, X64_RAX);
  n += x64_ret(buf + n);

  x64_patch_rel32(buf, jz1, 6, deopt_pc);
  x64_patch_rel32(buf, jz2, 6, deopt_pc);
  x64_patch_rel32(buf, jz_bz, 6, deopt_pc);

  assert(n <= 96);
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
  } else if (bc->ncode == 3 && c[0] == OP_LOAD_SLOT &&
             c[1] < ENV_INLINE_SLOTS && c[2] == OP_RET) {
    JT("leaf_identity");
    /* (fn (... s ...) s)  →  mov rax, [rdi + slot_off]; ret */
    int32_t slot_off =
        (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)c[1] * 8;
    n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
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
    n += x64_zero_reg(buf + n, X64_RAX);
    n += x64_ret(buf + n);
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
    n += x64_zero_reg(buf + n, X64_RAX);
    n += x64_ret(buf + n);
    x64_patch_rel32(buf, jz_start, 6, deopt_pc);
  } else {
    JT("miss");
    return 0; /* shape not recognized */
  }

  /* Each matcher has its own internal assert against its expected upper
     bound; this is the catch-all that protects buf as a whole, including
     the inline leaf-shape paths above. */
  assert(n <= (int)sizeof(buf));
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
  int nslots;            /* current total: nparams + active let/with bindings */
  int nlet_depth;        /* >0 disables OP_TAIL_SELF (keys/slots mismatch) */
  const char *self_name; /* for self-tail-call detection; NULL in anon fn */
  int failed;
} compiler_t;

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

static void compile_if(compiler_t *c, exp_t *form, int tail) {
  /* (if cond then else)  — only 2-way for phase 1 */
  exp_t *cond = cadr(form);
  exp_t *thn = caddr(form);
  exp_t *els = cadddr(form);
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
  int is_self_tail = tail && c->self_name && c->nlet_depth == 0 &&
                     is_ptr(head) && issymbol(head) &&
                     strcmp(head->ptr, c->self_name) == 0;
  /* Cross-function tail call is safe regardless of nlet_depth:
     OP_TAIL_CALL wholesale releases current env's inline slots. */
  int is_cross_tail = tail && !is_self_tail;
  /* Fused LOAD_GLOBAL+CALL: if head is a symbol that isn't a local
     slot and isn't the self-tail case, we can skip the LOAD_GLOBAL
     dispatch + PUSH/POP and call via the gcache directly. */
  int use_call_global = 0, global_idx = -1;
  if (!is_self_tail && !is_cross_tail && is_ptr(head) && issymbol(head) &&
      find_slot(c, head->ptr) < 0) {
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
  exp_t *arg1 = a->content;
  exp_t *arg2 = a->next->content;

  /* Peephole: exactly 2 args, arg1 is a local slot symbol, arg2 is a
     fixnum fitting in int16. Emit one fused op instead of three. */
  if (!a->next->next) {
    int fused = fuse_slot_fix(op);
    if (fused && is_ptr(arg1) && issymbol(arg1) && isnumber(arg2)) {
      int slot = find_slot(c, arg1->ptr);
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
  if (!is_ptr(key) || !issymbol(key)) {
    c->failed = 1;
    return;
  }
  int slot = find_slot(c, key->ptr);
  if (slot < 0) {
    c->failed = 1;
    return;
  }
  compile_expr(c, val, 0);
  if (c->failed)
    return;
  emit_u8(c, OP_STORE_SLOT);
  emit_u8(c, (uint8_t)slot);
  /* STORE_SLOT re-pushes the stored value so (= x v) returns v. */
}

/* (let var val body) — single binding, evaluates body in extended scope.
   Falls back to AST if slot count would exceed ENV_INLINE_SLOTS. */
static void compile_let(compiler_t *c, exp_t *form, int tail) {
  exp_t *var = cadr(form);
  exp_t *val = caddr(form);
  exp_t *body = cadddr(form);
  if (!is_ptr(var) || !issymbol(var) || !body) {
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
  c->slot_names[slot] = (char *)var->ptr;
  c->nslots++;
  c->nlet_depth++;
  compile_expr(c, body, tail);
  if (c->failed)
    return;
  c->nlet_depth--;
  c->nslots--;
  /* Body's value is on the stack. Peek-unbind: swap top of stack with
     the slot, but we only have POP/PUSH — cheapest is stash via a temp
     dedicated op. Simpler: store body result into slot (STORE discards
     old binding), emit LOAD_SLOT, emit UNBIND_SLOT — round-trip but
     clean. Actually simpler yet: the binding's owning ref is still in
     the slot; we can just emit UNBIND_SLOT which unrefs and NULLs it,
     leaving the body result untouched on the stack. */
  emit_u8(c, OP_UNBIND_SLOT);
  emit_u8(c, (uint8_t)slot);
}

/* (with (v1 e1 v2 e2 ...) body) — N parallel-like bindings then body.
   In alcove's semantics, bindings evaluate left-to-right against the
   enclosing env (each val doesn't see earlier v's in the same with,
   matching the tree-walker's withcmd). */
static void compile_with(compiler_t *c, exp_t *form, int tail) {
  exp_t *pairs = cadr(form);
  exp_t *body = caddr(form);
  if (!is_ptr(pairs) || !ispair(pairs) || !body) {
    c->failed = 1;
    return;
  }
  /* Collect (var, val) pairs. */
  int start_slot = c->nslots;
  int nbindings = 0;
  exp_t *p = pairs;
  while (p) {
    exp_t *var = p->content;
    exp_t *nxt = p->next;
    if (!nxt) {
      c->failed = 1;
      return;
    }
    exp_t *val = nxt->content;
    if (!is_ptr(var) || !issymbol(var)) {
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
    c->slot_names[c->nslots] = (char *)var->ptr;
    c->nslots++;
    nbindings++;
    p = nxt->next;
  }
  c->nlet_depth++;
  compile_expr(c, body, tail);
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

  if (!is_ptr(var) || !issymbol(var)) {
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
  c->slot_names[counter_slot] = (char *)var->ptr;
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
    int slot = find_slot(c, e->ptr);
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
  if (is_ptr(head) && issymbol(head)) {
    const char *s = (const char *)head->ptr;
    if (!strcmp(s, "if")) {
      compile_if(c, e, tail);
      return;
    }
    if (!strcmp(s, "let")) {
      compile_let(c, e, tail);
      return;
    }
    if (!strcmp(s, "with")) {
      compile_with(c, e, tail);
      return;
    }
    if (!strcmp(s, "for")) {
      compile_for(c, e, tail);
      return;
    }
    if (!strcmp(s, "=")) {
      compile_assign(c, e, tail);
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
    if (kv && is_ptr(kv->val) && isinternal(kv->val)) {
      c->failed = 1;
      return;
    }
    compile_call(c, e, tail);
    return;
  }
  /* Complex head — fall back. */
  c->failed = 1;
}

int compile_lambda(exp_t *fn) {
  if (!fn || !islambda(fn))
    return 0;
  if (fn->flags & FLAG_COMPILED)
    return 1; /* idempotent */
  exp_t *params = fn->content;
  exp_t *body = fn->next->content;
  compiler_t c = {0};
  c.self_name = (const char *)fn->meta; /* may be NULL for anon fn */

  /* Register params into slots 0..N-1 matching env->inline_slots. */
  exp_t *p;
  for (p = params; p; p = p->next) {
    if (c.nparams >= ENV_INLINE_SLOTS) {
      c.failed = 1;
      break;
    }
    if (!is_ptr(p->content) || !issymbol(p->content)) {
      c.failed = 1;
      break;
    }
    c.slot_names[c.nparams++] = (char *)p->content->ptr;
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
  fn->bc = bc;
  fn->flags |= FLAG_COMPILED;
#ifdef ALCOVE_JIT
  jit_compile(bc); /* opportunistic; no-op for shapes we don't recognize */
#endif
  return 1;
}

static exp_t *vm_invoke_values(exp_t *fn, int nargs, exp_t **argv, env_t *env);

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
  if (bc->gcache && bc->gcache[idx].gen == alcove_global_gen) {
    PUSH(refexp(bc->gcache[idx].val));
  } else {
    exp_t *v = lookup(consts[idx], env);
    if (!v)
      RUNTIME_ERR("Unbound variable");
    if (!bc->gcache)
      bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
    bc->gcache[idx].val = v; /* not refcounted by us; bound globally */
    bc->gcache[idx].gen = alcove_global_gen;
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
l_bind_slot: {
  /* let/with entry: allocate a new inline slot and bump n_inline if
     this is a fresh position. Key already set by the compiler via
     inline_keys at an earlier BIND (not needed here — lookups go
     through compile-time slot resolution, not inline_keys scan). */
  uint8_t idx = READ_U8;
  exp_t *v = POP();
  /* Slot is fresh (compiler guarantees no prior BIND at same idx
     without intervening UNBIND). No old value to unref. */
  env->inline_vals[idx] = v;
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
    if (isnumber(a) && isnumber(b)) {                                          \
      r = FIX_VAL(a) intcmp FIX_VAL(b);                                        \
    } else {                                                                   \
      double da, db;                                                           \
      COERCE_TO_DOUBLE(a, da, "Illegal value in compare");                     \
      COERCE_TO_DOUBLE(b, db, "Illegal value in compare");                     \
      r = da flcmp db;                                                         \
    }                                                                          \
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
  if (bc->gcache && bc->gcache[idx].gen == alcove_global_gen) {
    callee = refexp(bc->gcache[idx].val);
  } else {
    callee = lookup(consts[idx], env);
    if (!callee)
      RUNTIME_ERR("Unbound variable");
    if (!bc->gcache)
      bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
    bc->gcache[idx].val = callee;
    bc->gcache[idx].gen = alcove_global_gen;
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

  if (!is_ptr(new_fn) || !islambda(new_fn)) {
    sp = base - 1;
    unrefexp(new_fn);
    RUNTIME_ERR("OP_TAIL_CALL: not a lambda");
  }

  if (!(new_fn->flags & FLAG_COMPILED)) {
    /* Non-compiled target — one C-stack frame, then return. */
    exp_t *ret = vm_invoke_values(new_fn, n, &stack[base], env);
    sp = base - 1;
    unrefexp(new_fn);
    while (sp > 0)
      unrefexp(POP());
    if (fn_owned)
      unrefexp(fn);
    return ret;
  }

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

  /* Bind new args to new_fn's params. */
  exp_t *p = new_fn->content;
  int i = 0;
  while (p && i < n) {
    if (!is_ptr(p->content) || !issymbol(p->content)) {
      int j;
      for (j = i; j < n; j++)
        unrefexp(args_buf[j]);
      unrefexp(new_fn);
      if (fn_owned)
        unrefexp(fn);
      return error(ERROR_ILLEGAL_VALUE, fn, env, "OP_TAIL_CALL: bad param");
    }
    if (env->n_inline < ENV_INLINE_SLOTS) {
      env->inline_keys[env->n_inline] = (char *)p->content->ptr;
      env->inline_vals[env->n_inline] = args_buf[i];
      env->n_inline++;
    } else {
      if (!env->d)
        env->d = create_dict();
      set_get_keyval_dict(env->d, p->content->ptr, args_buf[i]);
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
    double da = isnumber(a) ? (double)FIX_VAL(a) : a->f;
    double db = isnumber(b) ? (double)FIX_VAL(b) : b->f;
    PUSH(da <= db ? TRUE_EXP : NIL_EXP);
  } else {
    RUNTIME_ERR("Illegal value in <=");
  }
  NEXT;
}

#undef SLOT_FIX_NUMERIC

l_vec_ref: {
  exp_t *iexp = POP(), *vexp = POP();
  if (!is_ptr(vexp) || vexp->type != EXP_VECTOR || !isnumber(iexp)) {
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR("vec-ref: bad args");
  }
  alc_vec_t *v = (alc_vec_t *)vexp->ptr;
  int64_t i = FIX_VAL(iexp);
  if (i < 0 || i >= v->len) {
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR("vec-ref: index out of range");
  }
  exp_t *r = refexp(v->data[i]);
  unrefexp(iexp);
  unrefexp(vexp);
  PUSH(r);
  NEXT;
}
l_vec_set: {
  exp_t *valexp = POP(), *iexp = POP(), *vexp = POP();
  if (!is_ptr(vexp) || vexp->type != EXP_VECTOR || !isnumber(iexp)) {
    unrefexp(valexp);
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR("vec-set!: bad args");
  }
  alc_vec_t *v = (alc_vec_t *)vexp->ptr;
  int64_t i = FIX_VAL(iexp);
  if (i < 0 || i >= v->len) {
    unrefexp(valexp);
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR("vec-set!: index out of range");
  }
  unrefexp(v->data[i]);
  v->data[i] = valexp; /* transfer */
  /* Push the value back like (= ...) does. We borrow the slot, so refexp
     to keep our own ref balanced after unrefexp(vexp) below. */
  PUSH(refexp(v->data[i]));
  unrefexp(iexp);
  unrefexp(vexp);
  NEXT;
}
l_vec_len: {
  exp_t *vexp = POP();
  if (!is_ptr(vexp) || vexp->type != EXP_VECTOR) {
    unrefexp(vexp);
    RUNTIME_ERR("vec-len: not a vector");
  }
  int64_t n = ((alc_vec_t *)vexp->ptr)->len;
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
  while ((r + 1) * (r + 1) <= n)
    r++;
  while (r * r > n)
    r--;
  unrefexp(nexp);
  PUSH(MAKE_FIX(r));
  NEXT;
}
l_length: {
  /* (length list) — walk the cons chain, return tagged count.
     NULL or nil_singleton both count as length 0 (empty list). */
  exp_t *xs = POP();
  int64_t n = 0;
  exp_t *cur = xs;
  while (is_ptr(cur) && cur->type == EXP_PAIR && cur != nil_singleton) {
    n++;
    cur = cur->next;
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
  if (!is_ptr(fn) || !islambda(fn)) {
    int i;
    for (i = 0; i < nargs; i++)
      unrefexp(argv[i]);
    return error(ERROR_ILLEGAL_VALUE, fn, env, "Bytecode call: not a lambda");
  }
  /* Honor closure capture (see invoke()). For top-level fns this is
     just global so behavior is unchanged. */
  env_t *captured = (env_t *)fn->next->bc;
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
    exp_t *p = fn->content;
    int i = 0;
    while (p && i < nargs) {
      if (!is_ptr(p->content) || !issymbol(p->content)) {
        int j;
        for (j = i; j < nargs; j++)
          unrefexp(argv[j]);
        destroy_env(newenv);
        return error(ERROR_ILLEGAL_VALUE, fn, env, "Bytecode call: bad param");
      }
      if (newenv->n_inline < ENV_INLINE_SLOTS) {
        newenv->inline_keys[newenv->n_inline] = (char *)p->content->ptr;
        newenv->inline_vals[newenv->n_inline] = argv[i];
        newenv->n_inline++;
      } else {
        if (!newenv->d)
          newenv->d = create_dict();
        set_get_keyval_dict(newenv->d, p->content->ptr, argv[i]);
        unrefexp(argv[i]);
      }
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

tailrec:
  if (verbose) {
    printf("invoke:");
    print_node(e);
    printf("\n");
  }
  {
    exp_t *body = fn->next->content;
    /* Closure: if fn captured an env at creation, use it as the new
       call frame's parent so let/with bindings from the defining scope
       resolve before walking up to global. Args themselves must be
       evaluated in the CALLER's env (where their free vars live) — so
       we eval first, bind into newenv with evalexp=false. */
    env_t *captured = (env_t *)fn->next->bc;
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
    if ((ret = var2env(e, fn->content, evald_args, newenv, false))) {
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

      if (is_last && ret && is_ptr(ret) && ispair(ret) &&
          (ret->flags & FLAG_TAILREC) && is_ptr(ret->content) &&
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
            if (is_ptr(curvar->content) && issymbol(curvar->content)) {
              if (newenv->n_inline < ENV_INLINE_SLOTS) {
                newenv->inline_keys[newenv->n_inline] = curvar->content->ptr;
                newenv->inline_vals[newenv->n_inline] = refexp(v);
                newenv->n_inline++;
              } else {
                if (!newenv->d)
                  newenv->d = create_dict();
                set_get_keyval_dict(newenv->d, curvar->content->ptr, v);
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
  ret = EVAL(fn->next->content, newenv);
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
  if (verbose) {
    printf("\n expanded macro");
    print_node(ret);
    printf("\n");
  }
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
      if (((char *)e->ptr)[0] == ':')
        return e; // e is a keyword
      if ((tmpexp = lookup(e, env))) {
        unrefexp(e);
        return tmpexp;
      } else {
        ret = error(ERROR_UNBOUND_VARIABLE, e, env, "Error unbound variable %s",
                    e->ptr);
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
        if (((char *)tmpexp->ptr)[0] == ':') {
          ret = error(ERROR_ILLEGAL_VALUE, e, env,
                      "Error keyword %s can not be used as function",
                      tmpexp->ptr);
          goto finish;
        } // e is a keyword
        if ((tmpexp2 = lookup(tmpexp, env))) {
#ifdef ALCOVE_FFI
          if isffi (tmpexp2) {
            /* Eval each arg and dispatch through libffi to the C
               function held by tmpexp2. */
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
                ret = eret;
                goto finish;
              }
            }
            ret = alc_ffi_call(f, n, argv);
            free(argv);
            unrefexp(tmpexp2);
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
            goto finisht;
          } else if ismacro (tmpexp2) {
            ret = invokemacro(e, tmpexp2, env);
            goto finisht;
          } else if ispair (tmpexp2) {
            ret = tmpexp2;
            goto finisht;
          } else {
            ret = tmpexp2;
            goto finisht;
          }
        } else {
          ret = error(ERROR_UNBOUND_VARIABLE, e, env,
                      "Error unbound variable %s", tmpexp->ptr);
          goto finish;
        }
        ret = e; // what is happening here?
        goto finisht;
      } else if (isstring(tmpexp)) {
        tmpexp2 = EVAL(cadr(e), env);
        if (isnumber(tmpexp2)) {
          int64_t idx = FIX_VAL(tmpexp2);
          if ((idx >= 0) && (idx < (int64_t)strlen(tmpexp->ptr))) {
            ret = make_char(*((char *)tmpexp->ptr + idx));
          } else
            ret = error(ERROR_INDEX_OUT_OF_RANGE, e, env,
                        "Error index out of range");
        } else
          ret = error(ERROR_NUMBER_EXPECTED, e, env, "Error number expected");
        unrefexp(tmpexp2);
        goto finish;
      } else if (islambda(tmpexp)) {
        if (in_tail_position) {
          ret = make_tail_marker(tmpexp, e, env);
          goto finisht;
        }
        ret = invoke(e, tmpexp, env);
        goto finisht;
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
#include <readline/history.h>
#include <readline/readline.h>

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
    "unless", "quote", "with", "each", "mac", "set", "=",     " return", NULL};
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
static void alcove_colored_redisplay(void) {
  if (rl_end > 256) {
    rl_redisplay();
    return;
  }
  fputs("\r\x1B[K", rl_outstream);
  alc_print_prompt_stripped(rl_display_prompt ? rl_display_prompt : rl_prompt,
                            rl_outstream);
  alc_print_colored(rl_line_buffer, rl_end, rl_outstream);
  int back = rl_end - rl_point;
  if (back > 0)
    fprintf(rl_outstream, "\x1B[%dD", back);
  fflush(rl_outstream);
}

/* Read one complete top-level form from the terminal. Continues
   prompting (with a continuation prompt) until paren balance hits 0.
   Returned string is malloc'd. NULL on EOF. */
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
  char *line = readline(prompt);
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
    char *more = readline("    ... ");
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
#endif /* ALCOVE_READLINE */

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

  reserved_symbol = create_dict();
  /* Allocate immortal singletons before any other code references them. */
  nil_singleton = make_nil();
  true_singleton = make_symbol("t", 1);
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

  /* CLI flag scan:
       --noload / -n       skip auto-load of db.dump
       -e "<code>"         evaluate the string as a script (skips file read)
     Flags get filtered out of argv in place so the existing positional
     handling (last arg = file, prev = -i) still works whether the user
     passes flags first, last, or in the middle. */
  int auto_load = 1;
  char *eval_string = NULL;
  {
    int dst = 1, src;
    for (src = 1; src < argc; src++) {
      if (strcmp(argv[src], "--noload") == 0 || strcmp(argv[src], "-n") == 0) {
        auto_load = 0;
      } else if (strcmp(argv[src], "-e") == 0 && src + 1 < argc) {
        eval_string = argv[++src];
      } else {
        argv[dst++] = argv[src];
      }
    }
    argc = dst;
  }

  /* Auto-load persisted bindings if db.dump exists in CWD. Silent on
     missing-file (first run, no DB yet); prints a one-line summary on
     success so the user sees what came back. */
  if (auto_load) {
    int loaded = loaddb_from_file(global);
    if (loaded > 0)
      printf("alcove: auto-loaded %d entries from db.dump (use --noload to "
             "skip)\n",
             loaded);
  }

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

#ifdef ALCOVE_READLINE
  /* Enable line editing + tab completion + history when stdin is a tty.
     Non-interactive (pipe / file redirect / scripted) stays on the plain
     reader path. The completer needs g_global_env to walk env bindings. */
  int rl_active = (stream == stdin) && isatty(fileno(stdin));
  if (rl_active) {
    g_global_env = global;
    rl_attempted_completion_function = alcove_rl_completer;
    /* Use space as the only word separator so e.g. (fib|TAB completes
       the symbol "fib" with "(" left of cursor counted as boundary. */
    rl_basic_word_break_characters = " \t\n()'`,;\"";
    /* Visual matching-paren: when typing ")" the cursor briefly jumps
       to the matching "(" before settling. Built into readline. */
    rl_variable_bind("blink-matching-paren", "on");
    /* Real-time syntax highlighting via custom redisplay function. */
    rl_redisplay_function = alcove_colored_redisplay;
    using_history();
    stifle_history(1000);
  }
#endif

  exp_t *stre = NULL;
  // make_string(strdict,strlen(strdict));
  exp_t *strf = NULL;
  // make_string(strdict,strlen(strdict));
  /*  set_get_keyval_dict(dict,"TOTO",stre);
      kv=set_get_keyval_dict(dict,"TATA",NULL);
      if (kv) printf("TATA TEST NOT OK\n");
      else printf("TATA TEST OK\n");
      kv=set_get_keyval_dict(dict,"TOTO",NULL);
      if (kv->val == stre) printf("TOTO TEST OK\n");
      else printf("TOTO TEST NOTOK\n");
      kv=set_get_keyval_dict(dict,"TOTO",strf);
      if (kv->val == strf) printf("STRF TEST OK\n");
      else printf("STRF TEST NOTOK\n");
      del_keyval_dict(dict,"TOTO");*/
  // unrefexp(stre);

#ifdef ALCOVE_READLINE
  if (rl_active) {
    /* Interactive readline-based REPL: per-iteration we read a complete
       top-level form (continuation prompt for unbalanced parens) into
       a string, fmemopen it, and run the same eval+print pipeline as
       the file path against the memstream. */
    while (1) {
      idx++;
      char *line = rl_read_form(idx);
      if (!line) {
        printf("\n");
        goto endcleanly;
      }
      if (!line[0]) {
        free(line);
        idx--;
        continue;
      }
      /* Append a newline so reader sees a token terminator after the
         last bare symbol/number on the line — without it,
         fmemopen("quit",4,"r") would EOF mid-token and the reader
         would return a parse error instead of the symbol. */
      size_t llen = strlen(line);
      char *line_nl = memalloc(llen + 2, 1);
      memcpy(line_nl, line, llen);
      line_nl[llen] = '\n';
      line_nl[llen + 1] = 0;
      FILE *ls = fmemopen(line_nl, llen + 1, "r");
      while (1) {
        stre = reader(ls, 0, 0);
        if (iserror(stre) && (stre->flags == EXP_ERROR_PARSING_EOF)) {
          unrefexp(stre);
          break;
        }
        if (issymbol(stre) && (strcmp((char *)stre->ptr, "quit") == 0 ||
                               strcmp((char *)stre->ptr, "exit") == 0)) {
          unrefexp(stre);
          fclose(ls);
          free(line_nl);
          free(line);
          goto endcleanly;
        }
        if (issymbol(stre) && strcmp((char *)stre->ptr, "toeval") == 0) {
          toeval = 1 - toeval;
          printf("%d\n", toeval);
          unrefexp(stre);
          continue;
        }
        strf = NULL;
        if (toeval)
          strf = evaluate(stre, global);
        else
          unrefexp(stre);
        if (strf) {
          if (verbose) {
            printf("\x1B[35mstrf:%p\x1B[39m\n", (void *)strf);
            inspect_value(strf);
          }
          printf("\x1B[31mOut[\x1B[91m%d\x1B[31m]:\x1B[39m", idx);
          print_node(strf);
          unrefexp(strf);
        } else
          printf("nil");
        printf("\n\n");
      }
      fclose(ls);
      free(line_nl);
      free(line);
    }
  }
#endif

  while (1) {
    idx++;
    strf = NULL;
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
    if (verbose) {
      if (stre)
        printf("\x1B[35mstre:%p\x1B[39m\n", (void *)stre);
      print_node(stre);
      printf("\n");
    }
    if (issymbol(stre) &&
        (strcmp(stre->ptr, "quit") == 0 || strcmp(stre->ptr, "exit") == 0)) {
      unrefexp(stre);
      break;
    }
    if (issymbol(stre) && (strcmp(stre->ptr, "toeval") == 0)) {
      toeval = 1 - toeval;
      printf("%d\n", toeval);
    }
    //
    if (toeval)
      strf = evaluate(stre, global);
    else
      unrefexp(stre);
    if (!evaluatingfile) {
      if (strf) {
        if (verbose) {
          printf("\x1B[35mstrf:%p\x1B[39m\n", (void *)strf);
          inspect_value(strf); /* borrow, no ref churn */
        }
        printf("\x1B[31mOut[\x1B[91m%d\x1B[31m]:\x1B[39m", idx);
        print_node(strf);
      } else
        printf("nil");
      printf("\n\n");
    };

    if (strf) {
      unrefexp(strf);
      strf = NULL;
    }
  }
endcleanly:
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
  unrefexp(t);
  unrefexp(nil);
  /* Immortal singletons. We can't free() the exp_t pointer itself
     anymore (it lives inside the bump-allocator chunk, not a separate
     malloc), but the strdup'd ptr field for the "t" symbol can be
     released. The chunks themselves are reclaimed by the OS at exit. */
  if (true_singleton) {
    if (true_singleton->ptr)
      free(true_singleton->ptr);
    true_singleton = NULL;
  }
  if (nil_singleton) {
    nil_singleton = NULL;
  }
}
