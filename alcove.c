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



#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/time.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <assert.h>
#ifdef ALCOVE_JIT
#include <sys/mman.h>
#include <stddef.h>
#ifdef __APPLE__
#include <pthread.h>
#endif
#endif
//#include <jemalloc/jemalloc.h>
#include "alcove.h"



int toeval=1;
int verbose=00;
dict_t *reserved_symbol=NULL;
exp_tfunc* exp_tfuncList[EXP_MAXSIZE];

/* Canonical singletons — pointer set at main() startup. */
exp_t *nil_singleton = NULL;
exp_t *true_singleton = NULL;

/* Set by invoke's body loop / ifcmd's selected-branch eval. When true,
   evaluate returns a trampoline marker instead of recursing into
   invoke, giving us O(1) C stack for tail-recursive code. */
static int in_tail_position = 0;

lispProc lispProcList[]={
  /* name, arity, flags, level, cmd*/
  {"verbose",2,0,0,verbosecmd},
  {"quote",2,0,0,quotecmd},
  {"if",2,FLAG_TAIL_AWARE,0,ifcmd},
  {"=",2,0,0,equalcmd},
  {"<",2,0,0,cmpcmd},
  {">",2,0,0,cmpcmd},
  {"<=",2,0,0,cmpcmd},
  {">=",2,0,0,cmpcmd},
  {"+",2,0,0,pluscmd},
  {"*",2,0,0,multiplycmd},
  {"-",2,0,0,minuscmd},
  {"/",2,0,0,dividecmd},
  {"cons",2,1,0,conscmd},
  {"eval",2,1,0,evalcmd},
  {"car",2,1,0,carcmd},
  {"cdr",2,1,0,cdrcmd},
  {"list",2,1,0,listcmd},
  {"def",2,1,0,defcmd},
  {"macroexpand-1",2,1,0,expandmacrocmd},
  {"defmacro",2,1,0,defmacrocmd},
  {"fn",2,1,0,fncmd},
  {"let",2,1,0,letcmd},
  {"with",2,1,0,withcmd},
  {"sqrt",2,1,0,sqrtcmd},
  {"exp",2,1,0,expcmd},
  {"expt",2,1,0,exptcmd},
  {"pr",2,1,0,prcmd},
  {"print",2,1,0,prcmd},
  {"prn",2,1,0,prncmd},
  {"println",2,1,0,prncmd},
  {"odd",2,1,0,oddcmd},
  {"do",2,FLAG_TAIL_AWARE,0,docmd},
  {"when",2,1,0,whencmd},
  {"while",2,1,0,whilecmd},
  {"repeat",2,1,0,repeatcmd},
  {"and",2,1,0,andcmd},
  {"or",2,1,0,orcmd},
  {"no",2,1,0,nocmd},
  {"is",2,1,0,iscmd},
  {"iso",2,1,0,isocmd},
  {"in",2,1,0,incmd},
  {"case",2,1,0,casecmd},
  {"for",2,1,0,forcmd},
  {"each",2,1,0,eachcmd},
  {"time",2,1,0,timecmd},
  {"persist",2,1,0,persistcmd},
  {"forget",2,1,0,forgetcmd},
  {"savedb",2,1,0,savedbcmd},
  {"ispersistent",2,1,0,ispersistentcmd},
  {"inspect",2,1,0,inspectcmd},

};




int64_t gettimeusec() 
{   
	struct timeval tv;
	gettimeofday(&tv,NULL);
	return (tv.tv_sec*1000000+tv.tv_usec);
}
	
	

#pragma GCC diagnostic ignored "-Wunused-parameter"
exp_t *error(int errnum,exp_t *id,env_t *env,char *err_message, ...)
{
  exp_t *ret;
  va_list ap;
  va_start(ap,err_message);
  ret=make_nil();
  ret->type=EXP_ERROR;
  ret->flags=errnum;
  if (vasprintf((char**)&ret->ptr, err_message, ap) < 0) {
    ret->ptr = strdup("<error formatting error message>");
  }
  va_end(ap);
  ret->next=refexp(id);
  return ret;
}
#pragma GCC diagnostic warning "-Wunused-parameter"

/* MEMORY MANAGEMENT FUNCTIONS */
/*
There are two function, refexp and unrefexp which handle , which handle the incrementing and decrementing of the reference count of exp_t objects.

By default, functions that return a reference to an object pass on ownership with the reference.

Macros such car,cdr,cadr,cddr,cddr,isatom... of course borrow reference and are not supposed to decrement the reference count of the object.

print_node only borrow ownership and does not modify objects.

When objects are passed as parameter of a *cmd function, ownership is also transfered otherwise, ownership is borrowed.

Concurrency issues are not yet taken care of.

When the reference count of an object reaches zero when decremented by unrefexp, unrefexp frees the object. At some point a the struct exp_tfunc will point to the function to be called to free the object depending on its type. It is not yet implemented.

There are 2 ways to handle an object at the end of the function, transfer the ownership or call unrefexp.

*/


void *memalloc(size_t count, size_t size){
  return calloc(count,size);
  // ERROR MANAGEMENT TO BE ADDDED
}


inline exp_t *refexp(exp_t *e) {
  /* Tagged immediates (fixnum, char) and canonical singletons (nil, t)
     are immortal — skip the refcount traffic. */
  if (!is_ptr(e)) return e;
  if (e == nil_singleton || e == true_singleton) return e;
  REFCOUNT_INC(&e->nref);
  return e;
}

inline int unrefexp(exp_t *e){
  if (!is_ptr(e)) return 0;
  if (e == nil_singleton || e == true_singleton) return 1;
  int ret;
  if ((ret=REFCOUNT_DEC(&e->nref)) <=0) {
    if (verbose) {
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
    if (e->next) unrefexp(e->next);
    if ((e->type==EXP_SYMBOL)||(e->type==EXP_STRING)||(e->type==EXP_ERROR))
      free(e->ptr);
    else if (((e->type>=EXP_NUMBER)&&(e->type<=EXP_BOOLEAN))||(e->type==EXP_INTERNAL)) {
    }
    else 
        unrefexp(e->content); //check if content type is exp

    if (verbose) printf("\x1B[91mFree e:\x1B[39m %p\n",(void*)e);
    free(e);

    return 0;
  };
    if (verbose) {
      printf("\x1B[91mUnref (%d): ",e->nref);
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

#define ENV_ARENA_SLOTS  8192
static env_t env_arena[ENV_ARENA_SLOTS];
static env_t *env_arena_sp = env_arena;
static env_t * const env_arena_end = env_arena + ENV_ARENA_SLOTS;

#define IS_ARENA_ENV(e) ((e) >= env_arena && (e) < env_arena_end)

inline env_t * ref_env(env_t *env){
	if (env){ REFCOUNT_INC(&env->nref); return env; }
	else return env;
}


inline env_t * make_env(env_t *rootenv)
{
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


inline void *destroy_env(env_t *env)
{
  /* Iterative release — each env holds a ref to its parent via make_env/ref_env.
     Recursing would blow the C stack on deep call chains.
     Also scrubs the fields that would carry stale state into a reused
     arena slot, so make_env can skip the wholesale memset. */
  while (env) {
    env_t *parent = env->root;
    if (REFCOUNT_DEC(&env->nref) > 0) break;
    {
      int i;
      for (i=0; i<env->n_inline; i++) unrefexp(env->inline_vals[i]);
    }
    if (env->d) destroy_dict(env->d);
    if (env->callingfnc) unrefexp(env->callingfnc);
    /* Arena envs: roll the bump pointer back (LIFO) and scrub the
       fields that would carry stale state into the slot's next tenant,
       so make_env can skip the wholesale memset.
       Heap-fallback envs return to free() — no scrub needed. */
    if (IS_ARENA_ENV(env)) {
      env->n_inline = 0;
      env->d = NULL;
      env->callingfnc = NULL;
      if (env + 1 == env_arena_sp) env_arena_sp = env;
    } else {
      free(env);
    }
    env = parent;
  }
  return NULL;
}

/* TOKEN MANAGEMENT FUNCTIONS */

inline token_t * tokenize(int v){
  token_t *token=memalloc(1,sizeof(token_t));
  token->size=0;
  token->maxsize=TOKENMINSIZE;
  token->data=memalloc(TOKENMINSIZE,sizeof(char));
  if (v!=-1)
    token->data[token->size++]=v;
  return token;
}

inline void freetoken(token_t *token){
  free(token->data);
  free(token);
}

inline void tokenadd(token_t *token, int v){
  char *tmp;
  if (token->size+1>=token->maxsize){
    tmp=memalloc(token->maxsize*2,sizeof(char));
    strncpy(tmp,token->data,token->maxsize);
    token->maxsize*=2;
    free(token->data);
    token->data=tmp;
  }
  token->data[token->size++]=v;
}

inline void tokenappend(token_t *token,char *src,int len){
  char *tmp;
  if ((token->size+len+1)>=token->maxsize){
    int d=2;
    while ((token->size+len+1)>=(d*token->maxsize)) d*=2;
    tmp=memalloc(token->maxsize*d,sizeof(char));
    strncpy(tmp,token->data,token->maxsize);
    token->maxsize*=d;
    free(token->data);
  }
  strncpy(token->data+token->size,src,len);
  token->size+=len; 
}

// HASH FUNCTIONS

static unsigned int bernstein_seed=3102;

/* Bernstein Hash Function */
unsigned int bernstein_hash(unsigned char *key, int len)
{
  unsigned int hash = bernstein_seed;
  int i;
  for (i=0; i<len; ++i) hash = (hash + (hash<<5)) ^ key[i];
  return hash;
}

/* Bernstein Hash Function not key sensitive*/
unsigned int bernstein_uhash(unsigned char *key, int len)
{
  unsigned int hash = bernstein_seed;
  int i;
  for (i=0; i<len; ++i) hash = (hash + (hash<<5)) ^ tolower(key[i]);
  return hash;
}

static void init_kvht(kvht_t *kvht){
  kvht->table=NULL;
  kvht->size=0;
  kvht->sizemask=0;
  kvht->used=0;
}

dict_t * create_dict() {
  dict_t *d;
  d=memalloc(1,sizeof(dict_t));
  d->meta=NULL;
  d->pos=-1;
  init_kvht(&d->ht[0]);
  init_kvht(&d->ht[1]);
  return d;
}

int destroy_dict(dict_t *d){
  // check if in use
  keyval_t *ckv;
  keyval_t *pkv;
  unsigned int i,j;
  for (i=0;i<2;i++){
    for (j=0;j<d->ht[i].size;j++)
      {
        ckv=d->ht[i].table[j];
        while(ckv){
          pkv=ckv;
          ckv=pkv->next;
          free(pkv->key);
          unrefexp(pkv->val);
          free(pkv);
        }
      }
    if(d->ht[i].table)
      free(d->ht[i].table);
  }
  // FREE META?
  free(d);
  return 1;
}

int dump_dict(dict_t *d,FILE *stream){
  // check if in use
  keyval_t *ckv;
  keyval_t *pkv;
  unsigned int i,j;
  if (verbose) printf("Dumping dict %p\n",(void*)d);
  for (i=0;i<2;i++){
    for (j=0;j<d->ht[i].size;j++)
      {
        ckv=d->ht[i].table[j];
        while(ckv){
          pkv=ckv;
          ckv=pkv->next;
          if (pkv->timestamp) {
            if (verbose) {
				printf("saving %s : ",pkv->key);
            print_node(pkv->val);
		}
            if (__DUMP__(pkv->val,stream)) {
              dump_str(pkv->key,stream);
            }
          }
        }
      }
  }
  return 1;
}


keyval_t * set_get_keyval_dict(dict_t* d, char *key, exp_t *val){
  unsigned int h=bernstein_hash((unsigned char*)key,strlen(key));
  keyval_t *k=NULL;
  if (d->ht[0].size) {
    if ((k = d->ht[0].table[ h&(d->ht[0].sizemask) ])){
      while ( (k->next) && (strcmp(key,k->key)!=0)) k=k->next;
      if (val){
        if (strcmp(key,k->key)==0) unrefexp(k->val);
        else { k=k->next=memalloc(1,sizeof(keyval_t)); d->ht[0].used++; k->key=strdup(key); }
      }
    }
    else if (val){
      k=d->ht[0].table[ h&(d->ht[0].sizemask) ] = memalloc(1,sizeof(keyval_t)); 
      d->ht[0].used++; k->key=strdup(key);}
  }

  else if (val) {
    d->ht[0].size=DICT_KVHT_INITIAL_SIZE;
    d->ht[0].sizemask=DICT_KVHT_INITIAL_SIZE-1;
    d->ht[0].table=memalloc(d->ht[0].size,sizeof(keyval_t*));
    k=d->ht[0].table[h&d->ht[0].sizemask]=memalloc(1,sizeof(keyval_t));
    d->ht[0].used++;
    k->key=strdup(key);
  };
    
  if (val) {
    k->val=refexp(val);
  } else {
    if (k && (strcmp(key,k->key)!=0)) return NULL;
  }
  return k;
}

exp_t * set_keyval_dict_timestamp(dict_t* d, char *key, int64_t timestamp){
  keyval_t *k=set_get_keyval_dict(d,key,NULL);
  if (k) {
    k->timestamp=timestamp;
    return refexp(k->val);
  }
  return NULL;
}

int64_t get_keyval_dict_timestamp(dict_t* d, char *key){
  keyval_t *k=set_get_keyval_dict(d,key,NULL);
  if (k) {
    return k->timestamp;
  }
  return 0;
}


void * del_keyval_dict(dict_t* d, char *key){
  unsigned int h=bernstein_hash((unsigned char*)key,strlen(key));
  keyval_t *p=NULL;
  keyval_t *k;
  if (d->ht[0].size) {
    if ((k=d->ht[0].table[h&(d->ht[0].sizemask)])){
      while ( (k->next) && (strcmp(key,k->key)!=0)) {p=k; k=k->next;};
      if (strcmp(key,k->key)==0) {
        unrefexp(k->val);
        free(k->key);
        d->ht[0].used--;
        if (p) p->next=k->next;
        else d->ht[0].table[h&(d->ht[0].sizemask)]=k->next;
        free(k);
        return NULL;
      }
      else return NULL;
    }
  }
  return NULL;
}

// see page 25 concept of "liaison immuable" et liaison "muable"

inline exp_t *make_nil(){
  exp_t *nil_exp=memalloc(1,sizeof(exp_t));
  nil_exp->type=EXP_PAIR;
  nil_exp->nref=1;
  nil_exp->content=NULL;
  nil_exp->next=NULL;
  nil_exp->meta=NULL;
  return nil_exp;
}


inline exp_t *make_char(unsigned char c){
  /* Tagged immediate: no heap allocation. */
  return MAKE_CHAR(c);
}


inline exp_t *make_node(exp_t *node){
  exp_t *cur=make_nil();
  if (node) cur->content=node;
  return cur;
}

inline exp_t *make_internal(lispCmd *cmd, int flags){
  exp_t *cur=make_nil();
  cur->type=EXP_INTERNAL;
  cur->flags=flags;
  cur->fnc=cmd;
  return cur;
}




void tree_add_node(exp_t *tree,exp_t *node){
  exp_t *cur=tree;
  if ((cur=cur->content)) {
    if (cur->type == EXP_PAIR)
      pair_add_node(cur,node);
    else if (cur->type==EXP_TREE) 
      tree_add_node(cur,node);
    else printf("ERROR IMPOSSIBLE TO ADD ");
  }
  else tree->content=make_node(node);
}

void pair_add_node(exp_t *pair, exp_t *node){
  exp_t *cur=pair;
  if (cur->type==EXP_PAIR){
    if (cur->next){
      cur=cur->next;
      if (cur->type==EXP_PAIR)
        pair_add_node(cur,node);
      else if (cur->type==EXP_PAIR)
        tree_add_node(cur,node);
      else printf("ERROR UNABLE TO ADD NODE TO EXP");
    }
    else{
      cur=cur->next=make_node(node);
      cur->content=node; /* ??? **/
    }
  }
  else printf("ERROR IMPOSSIBLE TO ADD NODE TO NON PAIR OBJECT\n");

}

exp_t *make_tree(exp_t *root,exp_t *node1){
  exp_t *tree=make_nil();
  tree->type=EXP_TREE;
  tree->next=refexp(root);
  if (node1)
    tree->content=refexp(node1);
  if (root) tree_add_node(root,tree);
  return tree;
}

void print_node(exp_t *node)
{
  if (node==NULL) { printf("nil"); return; }
  /* Tagged immediates — handle before any ->field access. */
  if (isnumber(node)) {
    printf("\x1B[92m%s%lld\x1B[39m",verbose?"_num:":"",(long long)FIX_VAL(node));
    return;
  }
  if (ischar(node)) {
    uint32_t c = CHAR_VAL(node);
    if (c>32) printf("#\\%c",(char)c);
    else printf("#\\%u",c);
    return;
  }
  if (!is_ptr(node)) { printf("<?imm %p>",(void*)node); return; }
  if (node->type==EXP_ERROR)
    {
      printf("\x1B[91mError: \x1B[39m%s\n",(char*) node->ptr);
    }
  else if (node->type==EXP_TREE){
    printf("[ ");
    if (node->content)
      print_node(node->content);
    printf("] ");
  }
  else if (node->type==EXP_PAIR){
    if (istrue(node)) {printf("(");
      if (node->content) print_node(node->content);
      while ((node=node->next)) {
        if ispair(node) { printf(" ");print_node(node->content);}
        else {printf(" . "); print_node(node); break; }
      }
      printf(")");
    } else printf("nil");
  }
  else if (node->type==EXP_LAMBDA){
    if (node->meta) printf("\x1B[92m#<procedure:%s@%08lx>\x1B[39m",(char*)node->meta,(long) node);
    else printf("\x1B[92m#<procedure@%08lx>\x1B[39m",(long) node);
    if (verbose) { 
      printf("\theader:"); print_node(node->content);
      printf("\tbody:"); print_node(node->next);
    }
    /*  if (node->content) print_node(node->content);
        printf(")");*/
  }
  else if (node->type==EXP_MACRO){
    if (node->meta) printf("\x1B[92m#<macro:%s@%08lx>\x1B[39m",(char *) node->meta,(long) node);
    else printf("\x1B[92m#<macro@@%08lx>\x1B[39m",(long) node);
    /*  if (node->content) print_node(node->content);
        printf(")");*/
  }

  else if (node->type==EXP_SYMBOL) printf("\x1B[92m%s%s\x1B[39m",verbose?"_sym:":"",(char *) node->ptr);
  else if (node->type==EXP_STRING) printf("\x1B[92m%s\"%s\"\x1B[39m",verbose?"_str:":"",(char *) node->ptr);
  else if (node->type==EXP_FLOAT) printf("\x1B[92m%s%lf\x1B[39m",verbose?"_flo:":"",node->f);
  else {
    printf("\x1B[92mtype: %d ptr: %08lx\x1B[39m",node->type,(unsigned long) node->ptr);
  }
  return;
}

inline exp_t *make_fromstr(char *str,int length)
{
  exp_t *cur=make_nil();
  cur->ptr=memalloc(length+1,sizeof(char));
  strncpy(cur->ptr,str,length);
  *((char*)cur->ptr+length)='\0';
  return cur;
}

inline exp_t *make_string(char *str,int length)
{
  exp_t *cur=make_fromstr(str,length);
  cur->type=EXP_STRING;
  //  printf("STR %s\n",(char*)cur->ptr);
  return cur;
}

inline exp_t *make_symbol(char *str,int length)
{
  exp_t *cur=make_fromstr(str,length);
  cur->type=EXP_SYMBOL;
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


inline exp_t *make_quote(exp_t *node){
  exp_t *cur=make_symbol("quote",strlen("quote"));
  cur=make_node(cur);
  cur->next=make_node(node);
  return cur;  
}

inline exp_t *make_integer(char *str)
{
  /* Tagged immediate: no heap allocation. atoll for 64-bit range. */
  return MAKE_FIX(atoll(str));
}

inline exp_t *make_integeri(int64_t i)
{
  return MAKE_FIX(i);
}


inline exp_t *make_float(char *str)
{
  exp_t *cur=make_nil();
  cur->type=EXP_FLOAT;
  cur->f=strtod(str,NULL);
  return cur;
}

inline exp_t *make_floatf(expfloat f)
{
  exp_t *cur=make_nil();
  cur->type=EXP_FLOAT;
  cur->f=f;
  return cur;
}

// exp_t dump and load

size_t loadtype(FILE *stream,unsigned short int* type) {
  return fread(type,sizeof(unsigned short int),1,stream);
}

size_t dumptype(FILE *stream,unsigned short int *type) {
  return fwrite(type,sizeof(unsigned short int),1,stream);
}

size_t loadsize_t(FILE *stream,size_t* len) {
  return fread(len,sizeof(size_t),1,stream);
}

size_t dumpsize_t(FILE *stream,size_t *len) {
  return fwrite(len,sizeof(size_t),1,stream);
}

exp_t *load_exp_t(FILE *stream) {
  exp_t *resp=make_nil();
  if (loadtype(stream,&(resp->type)) > 0 ) {
    return __LOAD__(resp,stream);
  }
  else {
    unrefexp(resp);
    return NULL;
  }
}

exp_t *dump_exp_t(exp_t *e,FILE *stream) {
  return __DUMP__(e,stream);
}



exp_t *load_char(exp_t *e,FILE *stream){
  /* Chars are tagged immediates — discard the placeholder exp_t that
     load_exp_t handed us and return a fresh tagged char. */
  int c = getc(stream);
  if (e) unrefexp(e);
  if (c == EOF) return NULL;
  return MAKE_CHAR((unsigned char)c);
}

exp_t *dump_char(exp_t *e,FILE *stream){
  unsigned short int t = EXP_CHAR;
  if (dumptype(stream,&t) <=0) return NULL;
  if (fputc((int)CHAR_VAL(e),stream) == EOF) return NULL;
  return e;
}

char *load_str(char **pptr,FILE *stream) {
  size_t length;
  char *ptr;
  if (loadsize_t(stream,&length)<=0) return NULL;
  ptr=*pptr=memalloc(length+1,sizeof(char));
  if (fread(ptr,1,length,stream) != length) {
    free(ptr);
    *pptr=NULL;
    return NULL;
  }
  *((char*)(ptr+length))='\0';
  return ptr;
}

exp_t *load_string(exp_t *e,FILE *stream){
  if (load_str((char**)&(e->ptr),stream)) 
    return e;
  else
    return NULL;
}

char *dump_str(char *ptr,FILE *stream){
  size_t length=strlen(ptr);
  if (dumpsize_t(stream,&length)<=0) return NULL;
  if (fwrite(ptr,1,length,stream) <=0) return NULL;
  return ptr;

}

exp_t *dump_string(exp_t *e,FILE *stream){
  if (dumptype(stream,&e->type) <=0) return NULL;
  if (dump_str(e->ptr,stream)) return e;
  else 
    return NULL;
}



exp_t *make_atom(char *str,int length)
{
  //Generate an atom from a string during parsing
  // TEST -> 0: + or - in front, 1: digit after first + or -, 2: E mantissa, 3:+ or - sign, 4: digit of mantissa
  int test=0;
  int dot=0;
  int len=length;
  char v;
  char *stro=str;
  if (str[0]=='\"')
    return make_string(str+1,length-2);
  while (length--){
    v=(char)*(str++);
    if ((v=='+')||(v=='-')){
      if ((test==1) || (test==3)) {
        break; // A sign after another sign => not an integer or following format +AB+
      }
      else if (test==7) {
        // OK MANTISSA there 
        test=15;
      }
      else if (test==0) test=1; 
      else break;
    }
    else if (v=='.') {if ((test<=3)||!dot) dot+=1; else break;}
    else if ((v=='E')||(v=='e')){
      //set mentisa on if not seen mantisa yet
      if (test==3) test=7;
      else break;
      
    }
    else if ((v<='9')&&(v>='0')){
      if (test<=3){
        test=3;
      }
      else if ((test==7)||(test==15)|(test==31)) test=31;
      else break;
    }
    else break;
  }
  
  if (length!=-1)
    {
      //not an integer then must be a symbol
      return make_symbol(stro,len);
    }
  else
    {
      if (test==1) return make_symbol(stro,len);
      else if ((test==3) &&!dot) return make_integer(stro);
      else if ((test==31) || (test==3)) return make_float(stro);
      else return make_symbol(stro,len);
    }
}

exp_t *callmacrochar(FILE *stream,unsigned char x){
  exp_t* lnode=NULL; // Initial List Node
  exp_t* vnode=NULL; // Val Node 
  exp_t* cnode=NULL; // Current Node

  if (x=='(') {
    vnode=reader(stream,')',0);
      
    if (vnode){
      if (iserror(vnode)) return vnode;
      lnode=make_node(vnode);
      vnode=NULL;
      cnode=lnode;
      while ((vnode=reader(stream,')',0))) { 
        if (iserror(vnode)) { unrefexp(lnode); return vnode;}
        cnode=cnode->next=make_node(vnode);
      }
    }
  }
  else if (x=='[') {
    vnode=reader(stream,']',0);
    // ?? why ?? lnode=vnode;
    if (vnode){
      if (iserror(vnode)) return vnode;
      cnode=make_node(vnode); //body
      vnode=NULL;
      lnode=make_node(make_node(make_symbol("_",1))); //header
      lnode->next=make_node(make_node(cnode));
      lnode->type=EXP_LAMBDA;
      while ((vnode=reader(stream,']',0))) { 
        if (iserror(vnode)) { unrefexp(lnode); return vnode;} // cleaning to be done gc
        cnode=cnode->next=make_node(vnode);
        vnode=NULL;
      }
    }
  }
  else if (x=='\'') {
    vnode=reader(stream,0,0);
    return make_quote(vnode);
  }
  else if (x=='|') {
    vnode=reader(stream,')',PARSER_PIPEMODE);
    if (vnode){
      if (iserror(vnode)) return vnode;
      lnode=make_node(vnode);
      cnode=lnode;
      while ((vnode=reader(stream,')',PARSER_TERMMACROMODE))) {
        if (iserror(vnode)) { unrefexp(lnode); return vnode; }
        cnode=cnode->next=make_node(vnode);
      }
    }
  }
  
  
  else  
    return error(EXP_ERROR_PARSING_MACROCHAR,NULL,NULL,"call to macro char %c unkown!",x); 

  if (lnode)
    return lnode;
  else
    return NIL_EXP;
}

exp_t *escapereader(FILE *stream,token_t ** ptoken,int lastchar) 
{
  /* Parse \n \b ... */
  /* Parse \xAB as char 0xAB */
  /* Parse \u001000 as unicode char 001000 in hex mode */
  int zchar=lastchar;
  int nchar=0;
  if (schrmap[lastchar]) {
    zchar = schrmap[lastchar];
  }
  else if (lastchar == 'x') 
    {
      if ((nchar=getc(stream))==EOF) 
        return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing");
      if (chr2hex[nchar] <0) goto error;
      zchar = chr2hex[nchar]*16;
      if ((nchar=getc(stream))==EOF) 
        return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing");
      if (chr2hex[nchar] <0) goto error;
      zchar += chr2hex[nchar];
      
    }
  if (*ptoken) {
    tokenadd(*ptoken,zchar);
  }
  else {
    *ptoken=tokenize(zchar); 
  }

  return NULL;
error:
  return error(EXP_ERROR_PARSING_ESCAPE,NULL,NULL,"invalid escape %c unkown!",nchar); 
}


exp_t *reader(FILE *stream,unsigned char clmacro,int keepwspace){
  int x,y,z;
  token_t *token=NULL;
  exp_t *ret=NULL;
  int pushtoken=0;
  int escape=0;

  while ((x=getc(stream))!=EOF){
    pushtoken=0; escape=0;
    if (x>127) { /* UTF-8 SUPPORT */
      token=tokenize(x);
      do {
        if ((y=getc(stream))!=EOF) {
          if ((y<192) && (y>127)) tokenadd(token,y);
        }
        else { freetoken(token); return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing");}
      } while ((y<192) && (y>127));
      ungetc(y,stream);
    }
    else if ((x<0)||(x>255)||!chrmap[x]) 
      return error(EXP_ERROR_PARSING_ILLEGAL_CHAR,NULL,NULL,"Error illegal char %d",x);

    else if (ISWHITESPACE & chrmap[x]) continue; 
    else if ((ISTERMMACRO|ISNTERMMACRO) & chrmap[x]) { 
      if (clmacro==x) { if (keepwspace&PARSER_TERMMACROMODE) ungetc(x,stream); return NULL; /* OK */}
      if (x=='#') {
        // Dispatch macro
        if ((y=getc(stream))!=EOF) { 
          if (y=='\\') { // returning char
            if ((z=getc(stream))!=EOF) { return make_char(z);}
            else return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing");

          }
          else return error(EXP_ERROR_PARSING_MACROCHAR,NULL,NULL,"call to dispatch macro char %c unkown!",y); 
        }
        else return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing"); 
        
      }
      if ((ret=callmacrochar(stream,x))) return ret; else continue;
    }
    else if (ISSINGLEESCAPE & chrmap[x]) {  //step 5
      if ((y=getc(stream))!=EOF) { 
        if (keepwspace&PARSER_PIPEMODE) {token=tokenize(x);tokenadd(token,y);
        } 
        else {
          if ((ret = escapereader(stream,&token,y))) return ret;
        }
      }
      else return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing");
     
    }
    else if (ISMULTIPLEESCAPE & chrmap[x]) { token=tokenize(-1); escape=1;}//step 6
    else if (ISCONSTITUENT&chrmap[x]) token=tokenize(x);      //step 7
    while (!pushtoken){
      if (!escape) {
        //step 8
        if ((y=getc(stream))!=EOF){
          if (y>127) {
            tokenadd(token,y);
            do {
              if ((y=getc(stream))!=EOF) {
                if ((y<192) && (y>127)) tokenadd(token,y);
              }
              else { freetoken(token); return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing");}
            } while ((y<192) && (y>127));
            ungetc(y,stream);
          } 
          else if ((y<0)||(y>255)||!chrmap[y])   {
            freetoken(token);
            return error(EXP_ERROR_PARSING_ILLEGAL_CHAR,NULL,NULL,"Error illegal char %d",x);
          }
          else if ((ISCONSTITUENT|ISNTERMMACRO) & chrmap[y]) { tokenadd(token,y); continue;}
          else if (ISSINGLEESCAPE & chrmap[y]){
            if ((z=getc(stream))!=EOF) { 
              if (keepwspace&PARSER_PIPEMODE) { tokenadd(token,y); tokenadd(token,z);}
              else if ((ret = escapereader(stream,&token,z))) return ret;
              }
            else {
              freetoken(token);
              return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing");
            }
          }
          else if (ISMULTIPLEESCAPE & chrmap[y]) { pushtoken=1; ungetc(y,stream);} //escape=1;
          else if (ISTERMMACRO & chrmap[y]) { ungetc(y,stream); pushtoken=1;}
          else if (ISWHITESPACE & chrmap[y]) {
            pushtoken=1; //ungetc if appropriate
            if (keepwspace&1) tokenadd(token,y);
          }
          else pushtoken=1; 
        } 
        else {
          freetoken(token);
          return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing");
        }
      }
      else
        { // Escape mode
          //step 9
          if ((y=getc(stream))!=EOF){
            if (y>127) {
              tokenadd(token,y);
              do {
                if ((y=getc(stream))!=EOF) {
                  if ((y<192) && (y>127)) tokenadd(token,y);
                }
                else { freetoken(token); return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing");}
              } while ((y<192) && (y>127));
              ungetc(y,stream);
            } 
            else if ((y<0)||(y>255))  {
              freetoken(token);
              return error(EXP_ERROR_PARSING_ILLEGAL_CHAR,NULL,NULL,"Error illegal char %d",x);
            }
            else if ((ISWHITESPACE|ISCONSTITUENT|ISTERMMACRO|ISNTERMMACRO) & chrmap[y]) tokenadd(token,y);
            else if (ISSINGLEESCAPE & chrmap[y]){
              if ((z=getc(stream))!=EOF) {
                if ((ret = escapereader(stream,&token,z))) return ret;
              }
                //{ tokenadd(token,z); continue;} // should we use escapereader here ?? to be checked
              else { 
                freetoken(token);
                return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing");
              }
            }
            else if (ISMULTIPLEESCAPE &chrmap[y]) {      
              ret=make_string(token->data,token->size);
              freetoken(token); 
              return ret;
              /*escape=0;pushtoken=1;*/
            }
            else tokenadd(token,y);
            
          }
          else {
            freetoken(token);
            return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing");
          }
        }
    }
    if (pushtoken) {
      // TOKEN AND STUFF TO BE FREED
      ret=make_atom(token->data,token->size);
      freetoken(token);
      token=NULL;
      return ret;
    }
    else return NULL;
    
  }
  
  if (x==EOF){
    return error(EXP_ERROR_PARSING_EOF,NULL,NULL,"End of file reached while parsing");
    // END OF FILE PROCESSING TO BE DONE STEP 1   
  }
  return NULL;
}
// Syntactic sugar causes cancer of the semicolon. — Alan Perlis
// istrue borrow object reference
inline int istrue(exp_t *e){
  int ret=0;
  if (!e) return 0;
  /* Tagged immediates first — cheap tag check, no deref. */
  if (isnumber(e)) return FIX_VAL(e) != 0;
  if (ischar(e))   return 0; /* preserve historical behavior */
  if (!is_ptr(e))  return 0;
  if isatom(e) {
      if isstring(e) ret = ((e->ptr)?strlen(e->ptr):0);
      else if isfloat(e) ret = (e->f!=0);
      else if issymbol(e) ret = (strcmp(e->ptr,"nil")!=0);
    }
  else if ispair(e) {
      if (e->content||e->next)
        ret = 1;
    }
  return ret;
}


inline exp_t *lookup(exp_t *e,env_t *env)
{
  keyval_t *ret;
  env_t *curenv=env;
  char *key=e->ptr;

  /* Cache fast path: symbols previously resolved into reserved_symbol
     (builtins like +, -, if, <, etc.) skip the hash lookup. On a
     symbol, meta != NULL uniquely means "cached resolution" — the
     lambda/macro meta strings only exist on their own exp_t types. */
  if (e->meta) {
    return refexp((exp_t*)e->meta);
  }

  if ((ret=set_get_keyval_dict(reserved_symbol,key,NULL))) {
    e->meta = (struct keyval_t*)ret->val;
    return refexp(ret->val);
  }
  else {
    if (curenv) do {
        /* Fast path: scan inline function-param slots first. For the
           common case (1-6 params) this beats a full hash lookup. */
        int i;
        for (i=0; i<curenv->n_inline; i++) {
          if (strcmp(curenv->inline_keys[i], key)==0)
            return refexp(curenv->inline_vals[i]);
        }
        if ((curenv->d) &&(ret=set_get_keyval_dict(curenv->d,key,NULL))) return refexp(ret->val);
      } while ((curenv=curenv->root));
  }
  return NULL;
 
}
exp_t *updatebang(exp_t *keyv,env_t *env,exp_t *val){
  keyval_t *ret=NULL;
  exp_t *fret=NULL;
  exp_t *key=NULL;
  exp_t *key2=NULL;
  if (val==NULL) val=NIL_EXP;
  if (issymbol(keyv) || isstring(keyv)) {  // form (= "qweqwe" 10) (= weq 10)
      if (islambda(val) && val->meta==NULL) val->meta=(keyval_t *)strdup(keyv->ptr);
      /* If the name is bound in one of the inline function-param slots,
         update in place so lookup (which scans inline first) sees the
         new value instead of being shadowed by a stale dict entry. */
      {
        int i;
        for (i=0; i<env->n_inline; i++) {
          if (strcmp(env->inline_keys[i], keyv->ptr)==0) {
            unrefexp(env->inline_vals[i]);
            env->inline_vals[i] = refexp(val);
            unrefexp(keyv);
            return refexp(val);
          }
        }
      }
      if (!(env->d)) env->d=create_dict();
      ret=set_get_keyval_dict(env->d,keyv->ptr,val); unrefexp(keyv); return refexp(val);}
  else if (ispair(keyv)) 
    { /*evaluate(keyv,env)=val*/ 
      key=car(keyv);
      if (key && issymbol(key)){
        if (strcmp(key->ptr,"car")==0) // form (= (car x) 'z)
          {
            key=EVAL(cadr(keyv), env);
            if iserror(key) {
                unrefexp(keyv);
                unrefexp(val);
                return key;
              }
            unrefexp(key->content);
            key->content=refexp(val);
            unrefexp(key);
            goto finish;
            
          }
        else if (strcmp(key->ptr,"cdr")==0)
          {
            key=EVAL(cadr(keyv), env);
            if iserror(key) {
                unrefexp(keyv);
                unrefexp(val);
                return key;
              }
            unrefexp(key->next);
            key->next=refexp(val);
            unrefexp(key);
            goto finish;
          }
        else {
          key=EVAL(key, env);
          if (isstring(key)){
            key2=cadr(keyv);
            if (key2 && isnumber(key2) && ischar(val))
              if ((FIX_VAL(key2)>=0) && (FIX_VAL(key2)<(int64_t)strlen(key->ptr)))
                {
                  *((char*) key->ptr +FIX_VAL(key2))=(unsigned char) CHAR_VAL(val);
                  goto finish;
                }
              else fret = error(ERROR_INDEX_OUT_OF_RANGE,keyv,env,"Error index out of range");
            else fret = error(ERROR_NUMBER_EXPECTED,keyv,env,"Error number and char expected");
            unrefexp(keyv);
            unrefexp(val);
            return fret;
          }
          else {
            unrefexp(key);
            unrefexp(val);
            return NULL ; // SHOULD BE ERROR
          }
        }
      }      
      
    }
  else {
    fret =  error(EXP_ERROR_INVALID_KEY_UPDATE,keyv,env,"Error invalid key in =");
    unrefexp(val);
    unrefexp(keyv);
    return fret;
  }
  if (ret) return ret->val;
  else return NULL; /* ERROR? */
 finish:
  unrefexp(keyv);
  return val;
}

/* Lambdas here are NOT closures: the returned EXP_LAMBDA stores only
   params + body, with no reference to the defining env. Free variables
   are resolved dynamically against the CALLER's env chain at invoke
   time. If closure semantics are ever added, the env arena and the
   bytecode VM's lifetime assumptions both need revisiting. */
exp_t *fncmd(exp_t *e, env_t *env)
{
  exp_t *val;
  exp_t *vali;
  exp_t *header;
  exp_t *body;
  exp_t *cur=cdr(e);
  if (cur && ispair(cur->content)){
    header=car(cur); cur=cdr(cur);
    if (cur && ispair(cur->content)) {
      body=cur;
      vali=make_node(refexp(body));
      val=make_node(refexp(header));
      val->next=vali;
      val->type=EXP_LAMBDA;
      compile_lambda(val);
    }
    else val = error(EXP_ERROR_BODY_NOT_LIST,e,env,"Error body is not a list");
  }
  else val = error(EXP_ERROR_PARAM_NOT_LIST,e,env,"Error params is not a list");
  unrefexp(e);
  return val;
}

exp_t *defcmd(exp_t *e, env_t *env)
{
  keyval_t *ret;
  exp_t *val;
  exp_t *vali;
  exp_t *name;
  exp_t *header;
  exp_t *body;
  exp_t *cur=cdr(e);
  if (cur && issymbol(cur->content)){
    name=car(cur); cur=cdr(cur);
    if (cur && ispair(cur->content)) {
      header=car(cur); cur=cdr(cur);
      if (cur && ispair(cur->content)) 
        {
          body=cur;
          vali=make_node(refexp(body));
          val=make_node(refexp(header));
          val->next=vali;
          val->type=EXP_LAMBDA;
          val->meta=(keyval_t *)strdup(name->ptr);
          compile_lambda(val);  /* silent fallback to AST if body unsupported */
          if (!(env->d)) env->d=create_dict();
          ret=set_get_keyval_dict(env->d,name->ptr,val);
        }
      else val = error(EXP_ERROR_BODY_NOT_LIST,e,env,"Error body is not a list");
    }
    else val = error(EXP_ERROR_PARAM_NOT_LIST,e,env,"Error params is not a list");
  }
  else
    val = error(EXP_ERROR_MISSING_NAME,e,env,"Error missing name or name not a lambda");
  unrefexp(e);
  return val;
  
}

exp_t *expandmacrocmd(exp_t *e,env_t *env){
  exp_t *tmpexp;
  exp_t *tmpexp2;

  tmpexp=car(cadr(cadr(e)));
  //if (tmpexp && ispair(tmpexp)) tmpexp=evaluate(tmpexp,env);
  if (tmpexp)
    if (issymbol(tmpexp))
      if ((tmpexp2=lookup(refexp(tmpexp),env)))
        if ismacro(tmpexp2) {
            tmpexp = expandmacro(refexp(cadr(cadr(e))),tmpexp2,env);
            goto finish;
          }
  
  tmpexp = error(ERROR_ILLEGAL_VALUE,e,env,"Error parameter not a macro");
 finish:
  unrefexp(e);
  return tmpexp;
  
}

exp_t *defmacrocmd(exp_t *e, env_t *env)
{
  keyval_t *ret;
  exp_t *val;
  exp_t *vali;
  exp_t *name;
  exp_t *header;
  exp_t *body;
  exp_t *cur=cdr(e);
  if (cur && issymbol(cur->content)){
    name=car(cur); cur=cdr(cur);
    if (cur && ispair(cur->content)) {
      header=car(cur); cur=cdr(cur);
      if (cur && ispair(cur->content)) {
        body=car(cur);
        vali=make_node(refexp(body));
        val=make_node(refexp(header));
        val->next=vali;
        val->type=EXP_MACRO;
        val->meta=(keyval_t *) strdup(name->ptr);
        if (!(env->d)) env->d=create_dict();
        ret=set_get_keyval_dict(env->d,name->ptr,val);
      }
      
      else val = error(EXP_ERROR_BODY_NOT_LIST,e,env,"Error body is not a list");
    }
    else val = error(EXP_ERROR_PARAM_NOT_LIST,e,env,"Error params is not a list"); 
  }
  else
    val = error(EXP_ERROR_MISSING_NAME,e,env,"Error missing name or name not a lambda");
  unrefexp(e);
  return val;
}


#pragma GCC diagnostic ignored "-Wunused-parameter"
exp_t *verbosecmd(exp_t *e, env_t *env)
{
  if (verbose^=1) printf("verbose on\n");
  else printf("verbose off\n");
  unrefexp(e);
  return NULL;
}
exp_t *quotecmd(exp_t *e, env_t *env)
{
  exp_t *ret = refexp(cadr(e));
  unrefexp(e);
  return ret;
}
#pragma GCC diagnostic warning "-Wunused-parameter"

exp_t *ifcmd(exp_t *e, env_t *env)
{
  /* Tail-aware: propagates in_tail_position to the selected branch. */
  int outer_tail = in_tail_position;
  exp_t *tmpexp, *tmpexp2;
  in_tail_position = 0;
  tmpexp = EVAL(cadr(e), env);
  if iserror(tmpexp) {
      in_tail_position = outer_tail;
      unrefexp(e);
      return tmpexp;
    }
  if (istrue(tmpexp)) {
    unrefexp(tmpexp);
    tmpexp=refexp(caddr(e));
    unrefexp(e);
    in_tail_position = outer_tail;
    return evaluate(tmpexp,env);
  }
  else {
    unrefexp(tmpexp);
    if ((tmpexp=cdddr(e)))
      do {
        in_tail_position = 0;
        tmpexp2=EVAL(tmpexp->content, env);
        if ((!iserror(tmpexp2)) && (tmpexp->next)) {
          if (istrue(tmpexp2)) {
            unrefexp(tmpexp2);
            tmpexp2=refexp(cadr(tmpexp));
            unrefexp(e);
            in_tail_position = outer_tail;
            return evaluate(tmpexp2,env);
          }
          if (!(tmpexp=cddr(tmpexp))) {
            unrefexp(tmpexp2);
            unrefexp(e);
            in_tail_position = outer_tail;
            return NULL;
          }
        }
        else {
          unrefexp(e);
          in_tail_position = outer_tail;
          return tmpexp2;
        }
      }
      while (1);
    else {
      unrefexp(e);
      in_tail_position = outer_tail;
      return NULL;
    }
  }
}

exp_t *equalcmd(exp_t *e, env_t *env)
{
  exp_t *tmpexp=EVAL(caddr(e), env);
  exp_t *tmpkey=refexp(cadr(e));
  unrefexp(e);
  if iserror(tmpexp) {
      unrefexp(tmpkey);
      return tmpexp;
    }
  return updatebang(tmpkey,env,tmpexp);
  /* to be unrefed tmpkey in case of evaluate */ 
}

exp_t *persistcmd(exp_t *e, env_t *env)
{
  exp_t *tmpkey=refexp(cadr(e));
  exp_t *ret=NULL;
  if (!issymbol(tmpkey)) {
    tmpkey = evaluate(tmpkey,env); 
  }
  unrefexp(e);
  /* to be unrefed tmpkey in case of evaluate */ 
  if iserror(tmpkey) { return tmpkey; }
  ret = set_keyval_dict_timestamp(env->d,tmpkey->ptr,gettimeusec());
  unrefexp(tmpkey);
  return ret;
}

exp_t *ispersistentcmd(exp_t *e, env_t *env)
{
  exp_t *tmpkey=refexp(cadr(e));
  int64_t ret=0;
  if (!issymbol(tmpkey)) {
    tmpkey = evaluate(tmpkey,env); 
  }
  unrefexp(e);
  if iserror(tmpkey) return tmpkey;
  ret = get_keyval_dict_timestamp(env->d,tmpkey->ptr);
  unrefexp(tmpkey);
  if (ret) {
    return TRUE_EXP;
  }
  else {
    return NIL_EXP;
  }
}

exp_t *forgetcmd(exp_t *e, env_t *env)
{
  exp_t *tmpkey=refexp(cadr(e));
  exp_t *ret=NULL;
  if (!issymbol(tmpkey)) {
    tmpkey = evaluate(tmpkey,env); 
  }
  unrefexp(e);
  /* to be unrefed tmpkey in case of evaluate */ 
  if iserror(tmpkey) return tmpkey;
  ret = set_keyval_dict_timestamp(env->d,tmpkey->ptr,0);
  unrefexp(tmpkey);
  return ret;
}

exp_t *savedbcmd(exp_t *e, env_t *env)
{
  env_t *cur=env;
  FILE *stream=fopen("db.dump","w");
  if (!stream) {
    return error(ERROR_ILLEGAL_VALUE,e,env,"Unable to open db.dump for writing");
  }
  while (cur->root) {
    cur=cur->root;
  }
  if (cur->d) dump_dict(cur->d,stream);
  fclose(stream);
  return e;
}



exp_t *cmpcmd(exp_t *e, env_t *env)
{
  exp_t *op=car(e); /* operator symbol, borrowed */
  exp_t *result=NULL;
  exp_t *v1=EVAL(cadr(e), env);
  if iserror(v1) {
      unrefexp(e);
      return v1;
    }
  exp_t *v2=EVAL(caddr(e), env);
  double d=0;
  int ret=0;

  if iserror(v2) {
      unrefexp(e);
      unrefexp(v1);
      return v2;
    }
  if ((isnumber(v1)||isfloat(v1))&&(isnumber(v2)||isfloat(v2))){
    d=(isnumber(v1)?(double)FIX_VAL(v1):v1->f)-(isnumber(v2)?(double)FIX_VAL(v2):v2->f);
  }
  else if (isstring(v1)&&isstring(v2)) {
    d=strcmp(v1->ptr,v2->ptr);
  }
  else if (ischar(v1)&&ischar(v2)) {
    d=(double)CHAR_VAL(v1)-(double)CHAR_VAL(v2);
  }
  else {
    result = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in compare operation");
    goto finish;
  }
  if (!op || !issymbol(op)) {
    result = error(ERROR_ILLEGAL_VALUE,e,env,"Missing operator in compare operation");
    goto finish;
  }
  if (strcmp(op->ptr,"<")==0) ret=(d<0);
  else if (strcmp(op->ptr,">")==0) ret=(d>0);
  else if (strcmp(op->ptr,"<=")==0) ret=(d<=0);
  else if (strcmp(op->ptr,">=")==0) ret=(d>=0);
  else {
      result = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal operand in compare operation");
      goto finish;
    }
  result = (ret?TRUE_EXP:NIL_EXP);
 finish:
  unrefexp(v1);
  unrefexp(v2);
  unrefexp(e);
  return result;
}



exp_t *pluscmd(exp_t *e, env_t *env)
{
  int64_t sum_i=0;
  expfloat sum_f=0;
  int saw_float=0;
  exp_t *c=cdr(e);
  exp_t *v;
  exp_t *v1=NULL;
  exp_t *ret=NULL;
  do {
    if (c &&(v1=refexp(c->content)))
      {
        if ispair(v1) v=evaluate(v1,env);
        else if issymbol(v1) v=evaluate(v1,env);
        else v=v1;
        if iserror(v) { ret = v; goto finish;}
        if (saw_float){
          if (isnumber(v)) sum_f+=FIX_VAL(v);
          else if (isfloat(v)) sum_f+=v->f;
          else { ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); goto finish; }
        }
        else {
          if (isnumber(v)) sum_i+=FIX_VAL(v);
          else if (isfloat(v)) { sum_f = v->f + sum_i; sum_i=0; saw_float=1; }
          else { ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); goto finish; }
        }
        unrefexp(v);
      }
  } while (c &&(c=c->next));
  ret = saw_float?make_floatf(sum_f):make_integeri(sum_i);
 finish:
  unrefexp(e);
  return ret;
}

exp_t *multiplycmd(exp_t *e, env_t *env)
{
  int64_t sum_i=1;
  expfloat sum_f=1;
  int saw_float=0;
  int saw_int=0;
  exp_t *c=cdr(e);
  exp_t *v;
  exp_t *v1=NULL;
  exp_t *ret=NULL;

  do {
    if (c &&(v1=refexp(c->content)))
      {
        if ispair(v1) v=evaluate(v1,env);
        else if issymbol(v1) v=evaluate(v1,env);
        else v=v1;
        if iserror(v) { ret = v; goto finish;}
        if (saw_float){
          if (isnumber(v)) sum_f*=FIX_VAL(v);
          else if (isfloat(v)) sum_f*=v->f;
          else { ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); goto finish; }
        }
        else {
          if (isnumber(v)) { sum_i = saw_int ? sum_i*FIX_VAL(v) : FIX_VAL(v); saw_int=1; }
          else if (isfloat(v)) { sum_f = saw_int ? v->f*sum_i : v->f; sum_i=1; saw_float=1; }
          else { ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); goto finish; }
        }
        unrefexp(v);
      }
  } while (c &&(c=c->next));
  ret = saw_float?make_floatf(sum_f):make_integeri(sum_i);
 finish:
  unrefexp(e);
  return ret;
}

exp_t *minuscmd(exp_t *e, env_t *env)
{
  int64_t sum_i=0;
  expfloat sum_f=0;
  int saw_float=0;
  exp_t *c=cdr(e);
  exp_t *v;
  exp_t *v1=NULL;
  int i=0;
  exp_t *ret=NULL;

  do {
    if (c &&(v1=refexp(c->content)))
      {
        i++;
        if ispair(v1) v=evaluate(v1,env);
        else if issymbol(v1) v=evaluate(v1,env);
        else v=v1;
        if iserror(v) { unrefexp(e); return v; }
        if (saw_float){
          if (isnumber(v)) sum_f-=FIX_VAL(v);
          else if (isfloat(v)) sum_f-=v->f;
          else { ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); goto finish; }
        }
        else {
          if (isnumber(v)) { if (i>1) sum_i-=FIX_VAL(v); else sum_i=FIX_VAL(v); }
          else if (isfloat(v)) { sum_f = (i>1) ? sum_i - v->f : v->f; sum_i=0; saw_float=1; }
          else { ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); goto finish; }
        }
        unrefexp(v);
      }
  } while (c &&(c=c->next));
  if (i==1) { if (saw_float) sum_f=-sum_f; else sum_i=-sum_i; }
  ret = saw_float?make_floatf(sum_f):make_integeri(sum_i);
 finish:
  unrefexp(e);
  return ret;
}

exp_t *dividecmd(exp_t *e, env_t *env)
{
  int64_t sum_i=0;
  expfloat sum_f=0;
  int saw_float=0;
  exp_t *c=cdr(e);
  exp_t *v;
  exp_t *v1=NULL;
  int i=0;
  exp_t *ret=NULL;

  do {
    if (c &&(v1=refexp(c->content)))
      {
        i++;
        if ispair(v1) v=evaluate(v1,env);
        else if issymbol(v1) v=evaluate(v1,env);
        else v=v1;
        if iserror(v) { unrefexp(e); return v; }
        if (i>1) {
          if ((isnumber(v) && FIX_VAL(v)==0) || (isfloat(v) && v->f==0)) {
            ret = error(ERROR_DIV_BY0,e,env,"Illegal Division by 0");
            goto finish;
          }
        }
        if (saw_float){
          if (isnumber(v)) sum_f/=FIX_VAL(v);
          else if (isfloat(v)) sum_f/=v->f;
          else { ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); goto finish; }
        }
        else {
          if (isnumber(v)) { if (i>1) sum_i/=FIX_VAL(v); else sum_i=FIX_VAL(v); }
          else if (isfloat(v)) { sum_f = (i>1) ? sum_i/v->f : v->f; sum_i=0; saw_float=1; }
          else { ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); goto finish; }
        }
        unrefexp(v);
      }
  } while (c &&(c=c->next));
  if (i==1) {
    if (saw_float) { if (sum_f==0) { ret = error(ERROR_DIV_BY0,e,env,"Illegal Division by 0"); goto finish; } sum_f=1/sum_f; }
    else { if (sum_i==0) { ret = error(ERROR_DIV_BY0,e,env,"Illegal Division by 0"); goto finish; } sum_i=1/sum_i; }
  }
  ret = saw_float?make_floatf(sum_f):make_integeri(sum_i);
 finish:
  unrefexp(e);
  return ret;
}

exp_t *sqrtcmd(exp_t *e, env_t *env){
  exp_t *v;
  exp_t *ret;
  if ((v=e->next))
    v=EVAL(v->content, env);
  if iserror(v) {
      unrefexp(e);
      return v;
    }
  if (isfloat(v))
    ret = make_floatf(sqrt(v->f));
  else if (isnumber(v))
    ret = make_floatf(sqrt((double)FIX_VAL(v)));
  else
    ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation");
  unrefexp(v);
  unrefexp(e);
  return ret;
}

exp_t *expcmd(exp_t *e, env_t *env){
  exp_t *v;
  exp_t *ret;
  if ((v=e->next))
    v=EVAL(v->content, env);
  if iserror(v) { unrefexp(e); return v; }
  if (isfloat(v))
    ret = make_floatf(exp(v->f));
  else if (isnumber(v))
    ret = make_floatf(exp((double)FIX_VAL(v)));
  else
    ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation");
  unrefexp(v);
  unrefexp(e);
  return ret;
}

exp_t *exptcmd(exp_t *e, env_t *env){
  exp_t *v=NULL;
  exp_t *v2=NULL;
  exp_t *ret=NULL;
  if ((v=e->next))
    if ((v2=v->next))
      {
        v=EVAL(v->content, env);
        if iserror(v) { unrefexp(e) ; return v;}
        v2=EVAL(v2->content, env);
        if iserror(v2) { unrefexp(e); unrefexp(v) ; return v2;}
      }
  if ( (isfloat(v)||isnumber(v)) && (isfloat(v2)||isnumber(v2)))
    ret = make_floatf(pow(isfloat(v)?v->f:(double)FIX_VAL(v),isfloat(v2)?v2->f:(double)FIX_VAL(v2)));
  else
    ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); /*ERROR*/
  unrefexp(v);
  unrefexp(v2);
  unrefexp(e);
  return ret;
}

exp_t *prcmd(exp_t *e, env_t *env){
  exp_t *v=e;
  exp_t *val;
  while ((v=v->next)){
    val=EVAL(v->content, env);
    if iserror(val) { unrefexp(e); return val;}
    if (val && isstring(val)) printf("%s",(char*)val->ptr);
    else print_node(val);
    unrefexp(val);
  }
  unrefexp(e);
  return NULL;
    
}

exp_t *prncmd(exp_t *e, env_t *env){
  exp_t *ret;
  ret=prcmd(e,env);
  printf("\n");
  return ret;
}

exp_t *oddcmd(exp_t *e, env_t *env){
  exp_t *ret;
  if (e->next && isnumber(e->next->content)) ret = ((FIX_VAL(e->next->content)&1)?TRUE_EXP:NIL_EXP);
  else ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation");
  unrefexp(e);
  return ret;
}

exp_t *docmd(exp_t *e, env_t *env){
  /* Tail-aware: propagates in_tail_position to the final expression so
     a tail call inside (do ... (f x)) actually gets TCO. Returns the
     last expression's value (not nil — that was a pre-existing bug). */
  int outer_tail = in_tail_position;
  exp_t *cur=cdr(e);
  exp_t *ret=NULL;
  while (cur) {
    if (ret) unrefexp(ret);
    in_tail_position = (cur->next == NULL) ? outer_tail : 0;
    ret = EVAL(car(cur), env);
    if (ret && iserror(ret)) { in_tail_position = outer_tail; unrefexp(e); return ret; }
    cur = cdr(cur);
  }
  in_tail_position = outer_tail;
  unrefexp(e);
  return ret ? ret : NIL_EXP;
}

exp_t *whencmd(exp_t *e, env_t *env){
  exp_t *val=cadr(e);
  exp_t *cur=cddr(e);
  exp_t *ret=EVAL(val, env);
  if iserror(ret) { unrefexp(e); return ret; }
  if (istrue(ret))
    do { unrefexp(ret); ret=EVAL(car(cur), env);} while ((cur=cdr(cur)) && !(ret && iserror(ret)));
  if (ret && iserror(ret)) { unrefexp(e); return ret; }
  else { unrefexp(ret); unrefexp(e); return NIL_EXP;}
}

exp_t *whilecmd(exp_t *e, env_t *env){
  exp_t *val=cadr(e);
  exp_t *cur=cddr(e);
  exp_t *curi=cur;
  exp_t *ret=NULL;
  while (istrue(ret=EVAL(val, env))&&(!iserror(ret)))
    {
      cur=curi;
      do { unrefexp(ret); ret=EVAL(car(cur), env);} while ((cur=cdr(cur)) && !(ret && iserror(ret)));
    }
  if (ret && iserror(ret)) { unrefexp(e); return ret; }
  else { unrefexp(ret); unrefexp(e); return NIL_EXP;}
}

exp_t *repeatcmd(exp_t *e, env_t *env){
  exp_t *val=EVAL(cadr(e), env);
  exp_t *cur=cddr(e);
  exp_t *curi=cur;
  exp_t *ret=NULL;
  int64_t counter=0;
  if iserror(val) { unrefexp(e) ; return val;}
  if (isnumber(val)) counter=FIX_VAL(val);
  else {
    ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value for repeat counter");
    unrefexp(val);
    unrefexp(e);
    return ret;
  }
  unrefexp(val);
  while (counter-- >0)
    {
      cur=curi;
      do { if (ret) unrefexp(ret); ret=EVAL(car(cur), env);} while ((cur=cdr(cur)) && !(ret && iserror(ret)));
    }
  if (ret && iserror(ret)) { unrefexp(e); return ret; }
  else { if (ret) unrefexp(ret); unrefexp(e); return NIL_EXP;}
}

exp_t *andcmd(exp_t *e, env_t *env){
  exp_t *cur=cdr(e);
  exp_t *ret=NULL;
  do
    {
      if (ret) unrefexp(ret);
      ret=EVAL(car(cur), env);
      if iserror(ret) goto finish;
    } while (istrue(ret) && (cur=cdr(cur)));
 finish:
  unrefexp(e);
  return ret;
}

exp_t *orcmd(exp_t *e, env_t *env){
  exp_t *cur=cdr(e);
  exp_t *ret=NULL;
  do {
    if (ret) unrefexp(ret);
    ret=EVAL(car(cur), env);
    if iserror(ret) goto finish;
    if (istrue(ret)) goto finish;
  } while ((cur=cdr(cur)));
 finish:
  unrefexp(e);
  return ret;
}

exp_t *nocmd(exp_t *e, env_t *env){
  exp_t *cur=cdr(e);
  exp_t *tmpexp=EVAL(car(cur), env);
  if iserror(tmpexp) goto finish;
  if (istrue(cur=tmpexp)) tmpexp = NIL_EXP;
  else tmpexp = TRUE_EXP;
  unrefexp(cur);
 finish:
  unrefexp(e);
  return tmpexp;
}


int isequal(exp_t *cur1, exp_t *cur2)
{
  /* borrow ref to cur1 and cur2 */
  int ret=0;
  /* Fast path: any two tagged immediates compare by bit-pattern equality.
     Fixnum 5 == Fixnum 5, char 'a' == char 'a', and cross-type never equal. */
  if (cur1 == cur2) return 1;
  if (!is_ptr(cur1) || !is_ptr(cur2)) return 0;
  if (cur1->type == cur2->type){
    if (isfloat(cur1)) ret=(cur1->f==cur2->f);
    else if (issymbol(cur1) || isstring(cur1)) ret=(strcmp(cur1->ptr,cur2->ptr)==0);
    else if (iserror(cur1)) ret=(cur1->s64==cur2->s64);
    else ret = (cur1 == cur2);
  }
  return ret;
}

int isoequal(exp_t *cur1,exp_t *cur2){
  /* borrow ref to cur1 and cur2 */
  int ret=0;
  exp_t *cur1n;
  exp_t *cur2n;

  if (cur1 == cur2) return 1;
  if (!is_ptr(cur1) || !is_ptr(cur2)) return 0;
  if (cur1->type == cur2->type){
    if (ispair(cur1)) {
      cur1n=cur1; cur2n=cur2;
      ret=1;
      do {
        ret*=isoequal(car(cur1n),car(cur2n));
        cur1n=cur1n->next;
        cur2n=cur2n->next;
      }
      while (ret && cur1n && cur2n);
      ret*=(cur1n==cur2n);
    }
    else ret = isequal(cur1,cur2);
  }
  return ret;
}

exp_t *iscmd(exp_t *e, env_t *env){
  exp_t *ret=NULL;
  exp_t *cur1=EVAL(cadr(e), env);
  if iserror(cur1) {unrefexp(e);return cur1;}
  exp_t *cur2=EVAL(caddr(e), env);
  if iserror(cur2) {unrefexp(cur1);unrefexp(e);return cur2;}
  unrefexp(e);
  ret = (isequal(cur1,cur2)?TRUE_EXP:NIL_EXP);
  unrefexp(cur1);
  unrefexp(cur2);
  return ret;
}

exp_t *isocmd(exp_t *e, env_t *env){
  exp_t *ret=NULL;
  exp_t *cur1=EVAL(cadr(e), env);
  if iserror(cur1) {unrefexp(e);return cur1;}
  exp_t *cur2=EVAL(caddr(e), env);
  if iserror(cur2) {unrefexp(cur1);unrefexp(e);return cur2;}
  unrefexp(e);
  ret = (isoequal(cur1,cur2)?TRUE_EXP:NIL_EXP);
  unrefexp(cur1);
  unrefexp(cur2);
  return ret;
}


exp_t *incmd(exp_t *e, env_t *env){
  exp_t *cur=cdr(e);
  exp_t *val=EVAL(cadr(e), env);
  exp_t *val2=NULL;

  if iserror(val) { unrefexp(e); return val;}
  int ret=0;
  while ((cur=cdr(cur)))
    {
      if (val2) unrefexp(val2);
      val2=EVAL(car(cur), env);
      if iserror(val2) {unrefexp(e); unrefexp(val); return val2;}
      if ((ret=isoequal(val,val2))) break;
    }

  cur = (ret?TRUE_EXP:NIL_EXP);
  unrefexp(val);
  if (val2) unrefexp(val2);
  unrefexp(e);
  return cur;
}

exp_t *casecmd(exp_t *e, env_t *env){
  exp_t *cur=cdr(e);
  exp_t *val=EVAL(cadr(e), env);
  if iserror(val) { unrefexp(e); return val;}
  exp_t *ret=NULL;
  while ((cur=cdr(cur)))
    if (cur->next) {
      if (isequal(val,car(cur))) { ret=cadr(cur); break;}
      else cur=cdr(cur);
    }
    else ret= car(cur);
  unrefexp(val);
  cur = EVAL(ret, env);
  unrefexp(e);
  return cur;
}



exp_t *forcmd(exp_t *e,env_t *env){
  env_t *newenv=make_env(env);
  exp_t *ret=NULL;
  exp_t *curvar;
  exp_t *curval;
  exp_t *curin;
  exp_t *lastidx=NULL;
  exp_t *retval=NULL;
  keyval_t *keyv;

  if ((curvar=e->next)) {
    if ((curval=curvar->next)){
      if (!(newenv->d)) newenv->d=create_dict();

      if (issymbol(curvar->content)) {
        if ((retval=EVAL(curval->content, env))==NULL) retval=NIL_EXP;
        if (isnumber(retval)) {
          if (!curval->next) lastidx=NIL_EXP;
          if (curval->next && (lastidx=EVAL(curval->next->content, env))==NULL) lastidx=NIL_EXP;
          if (isnumber(lastidx)) {
            curin=curval->next->next;
          }
          else
            {
              if iserror(lastidx) ret=refexp(lastidx);
              else ret=error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value (not integer) in for counter");
              goto error;
            }
        }
        else {
          if iserror(retval) ret=refexp(retval);
          else ret=error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value (not integer) in for counter");
          goto error;
        }

      }
      else {
        ret=error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in for");
        goto error;
      }
      {
      int64_t counter = FIX_VAL(retval);
      int64_t idx     = FIX_VAL(lastidx) + 1;

      while (counter < idx)
        {
          /* Rebind the loop variable to a fresh tagged fixnum. Tagged
             immediates don't allocate, so this is free. */
          keyv = set_get_keyval_dict(newenv->d, curvar->content->ptr, MAKE_FIX(counter));
          curval=curin;
          while (curval) {
            if (ret) unrefexp(ret);
            ret=EVAL(curval->content, newenv);
            if iserror(ret) goto error;
            curval=curval->next;
          }
          counter++;
        }
      }
    }
  }
 error:
  destroy_env(newenv);
  if (!ret) ret = error(ERROR_MISSING_PARAMETER,e,env,"Missing parameter in for");
  if (lastidx) unrefexp(lastidx);
  if (retval) unrefexp(retval);
  unrefexp(e);
  return ret;
}


exp_t *eachcmd(exp_t *e,env_t *env){
  env_t *newenv=make_env(env);
  exp_t *curvar;
  exp_t *curval;
  exp_t *curin;
  exp_t *retval=NULL;
  exp_t *tmpexp=NULL;
  exp_t *ret=NULL;
  keyval_t *keyv;

  if ((curvar=e->next)) {
    if ((curval=curvar->next)){
      if (!(newenv->d)) newenv->d=create_dict();
      
      if (issymbol(curvar->content)) {
        if ((retval=EVAL(curval->content, env))==NULL) retval=NIL_EXP;
        if (ispair(retval)) {
          curin=curval->next;
          tmpexp=retval;
          while (retval) {
            keyv=set_get_keyval_dict(newenv->d,curvar->content->ptr,car(retval));
            curval=curin;
            while (curval) {
              ret=EVAL(curval->content, newenv);
              if iserror(ret) goto finish;
              unrefexp(ret); curval=curval->next;}
            retval=retval->next;
          }
          ret = NULL;
          goto finish;
        } 
        else {
          if iserror(retval) ret = refexp(retval);
          else ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value (not list) in each"); 
          goto finish;
        }
        
      }
      else { 
        ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in each"); 
        goto finish;
      }
    }
  }

  ret = error(ERROR_MISSING_PARAMETER,e,env,"Missing parameter in each");
 finish:
  destroy_env(newenv);
  /* only the head (tmpexp) owns a ref; retval is a borrowed walker */
  if (tmpexp) unrefexp(tmpexp);
  else if (retval) unrefexp(retval);
  unrefexp(e);
  return ret;
}

#pragma GCC diagnostic ignored "-Wunused-parameter"
exp_t *timecmd(exp_t *e,env_t *env){
  unrefexp(e);
  return make_integeri(gettimeusec());
}
#pragma GCC diagnostic warning "-Wunused-parameter"

#pragma GCC diagnostic ignored "-Wunused-parameter"
exp_t *inspectcmd(exp_t *e,env_t *env){
  if (is_ptr(e))
    printf("\x1B[96mtype:\t%d\nflag:\t%d\nref:\t%d\x1B[39m\n",e->type,e->flags,e->nref);
  else if (isnumber(e))
    printf("\x1B[96m<imm fixnum %lld>\x1B[39m\n",(long long)FIX_VAL(e));
  else if (ischar(e))
    printf("\x1B[96m<imm char %u>\x1B[39m\n",CHAR_VAL(e));
  unrefexp(e);
  return NULL;
}
#pragma GCC diagnostic warning "-Wunused-parameter"



exp_t *conscmd(exp_t *e, env_t *env){
  exp_t *a=EVAL(cadr(e), env);
  if iserror(a) { unrefexp(e); return a;}
  exp_t *b=EVAL(caddr(e), env);
  if iserror(b) { unrefexp(a); unrefexp(e); return b;}
  exp_t *ret=make_node(a);
  if (istrue(b)) ret->next=b;
  else { unrefexp(b); ret->next=NULL;}
  unrefexp(e);
  return ret;
}

exp_t *cdrcmd(exp_t *e, env_t *env){
  exp_t *tmpexp=EVAL(cadr(e), env);
  exp_t *tmpexp2=tmpexp;
  unrefexp(e);
  if (!iserror(tmpexp)) {
    tmpexp=refexp(cdr(tmpexp2));
    unrefexp(tmpexp2);
  }
  return tmpexp;
}

exp_t *carcmd(exp_t *e, env_t *env){
  exp_t *tmpexp=EVAL(cadr(e), env);
  exp_t *tmpexp2=tmpexp;
  if (!iserror(tmpexp)) {
    tmpexp = refexp(car(tmpexp2));
    unrefexp(tmpexp2);
  }
  return tmpexp;
}

exp_t *listcmd(exp_t *e, env_t *env){
  exp_t *a=cdr(e);
  exp_t *tmpexp=NULL;
  exp_t *ret=NULL;
  exp_t *cur=NULL;
  if (!a) { unrefexp(e); return NIL_EXP; }
  tmpexp=EVAL(car(a), env);
  if iserror(tmpexp) goto error;
  ret=make_node(tmpexp);
  tmpexp=NULL;
  cur=ret;
  while ((a=a->next))
    {
      tmpexp=EVAL(car(a), env);
      if iserror(tmpexp) { unrefexp(ret); goto error; }
      cur=cur->next=make_node(tmpexp);
    }
  unrefexp(e);
  return ret;
 error:
  unrefexp(e);
  return tmpexp;
}

exp_t *evalcmd(exp_t *e, env_t *env){
  exp_t *ret=evaluate(EVAL(cadr(e), env),env);
  unrefexp(e);
  return ret;
}

exp_t *letcmd(exp_t *e,env_t *env){
  env_t *newenv=make_env(env);
  exp_t *ret;
  exp_t *curvar;
  exp_t *curval;
  keyval_t *keyv;

  if ((curvar=e->next)) {
    if ((curval=curvar->next)){
      if (!(newenv->d)) newenv->d=create_dict();
      
      if (issymbol(curvar->content)) {
        if ((ret=EVAL(curval->content, env))==NULL) ret=NIL_EXP;
        if iserror(ret) goto finish;
        keyv=set_get_keyval_dict(newenv->d,curvar->content->ptr,ret);
        unrefexp(ret);
        ret=NULL;
      }
      else { 
        ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in let"); 
        goto finish;
      }
      if (curval->next)
        {
          ret=EVAL(curval->next->content, newenv);
          goto finish;
        }
      
    }
  }
  ret = error(ERROR_MISSING_PARAMETER,e,env,"Missing parameter in let"); /* MISSING PARAMETER*/
 finish:
  destroy_env(newenv);
  unrefexp(e);
  return ret;
}

exp_t *withcmd(exp_t *e,env_t *env){
  env_t *newenv=make_env(env);
  exp_t *ret;
  exp_t *curvar;
  exp_t *curval;
  exp_t *curex;
  keyval_t *keyv;
  
  if ((curvar=e->next)) {
    if (!ispair(curvar->content)) {
      ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in with"); 
      goto finish;
    }
    if ((curex=curvar->next)){
      curvar=curvar->content;
      if ((curval=curvar->next)){
        if (!(newenv->d)) newenv->d=create_dict();
        
        while (curvar && curval) {
          if (issymbol(curvar->content)) { 
            ret=EVAL(curval->content, env);
            if iserror(ret) goto finish;
            keyv=set_get_keyval_dict(newenv->d,curvar->content->ptr,ret);
            unrefexp(ret);
            ret = NULL;
          }
          
          else { 
            ret = error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in with"); 
            goto finish;
          }
          curvar=curval->next;
          if (curvar) curval=curvar->next;
          if (!curval) {
            ret = error(ERROR_MISSING_PARAMETER,e,env,"Missing parameter in with");
            goto finish;
          }
        }

        ret=EVAL(curex->content, newenv);
        goto finish;
      }
    }
  }

  ret = error(ERROR_MISSING_PARAMETER,e,env,"Missing parameter in with");
 finish:
  destroy_env(newenv);
  unrefexp(e);
  return ret;
}



exp_t *var2env(exp_t *e,exp_t *var, exp_t *val,env_t *env,int evalexp)
{
  /* borrow references */
  exp_t *curvar=var;
  exp_t *retvar;
  exp_t *curval=val;

  while (curvar){
    if ((curval)) {
      if ((retvar = (evalexp?EVAL(curval->content, env->root):refexp(curval->content)))) {
        if (evalexp && iserror(retvar)) {
          return retvar;
        }
      }
      else retvar= NIL_EXP;
      if (issymbol(curvar->content)) {
        /* Fast path: use inline slots for function-param bindings.
           Keys BORROW from the symbol's ptr; caller guarantees lifetime
           by holding a ref to the lambda header. Falls back to the
           dict once inline slots overflow. */
        if (env->n_inline < ENV_INLINE_SLOTS) {
          env->inline_keys[env->n_inline] = curvar->content->ptr;
          env->inline_vals[env->n_inline] = retvar;   /* ownership transferred */
          env->n_inline++;
        } else {
          if (!env->d) env->d=create_dict();
          set_get_keyval_dict(env->d,curvar->content->ptr,retvar);
          unrefexp(retvar);
        }
      }
      else { unrefexp(retvar); return NULL; }
    }
    else return error(ERROR_MISSING_PARAMETER,e,env,"Missing parameter in macro or function invoke");
    curval=curval->next;
    curvar=curvar->next;
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
static exp_t *make_tail_marker(exp_t *fn, exp_t *call_form, env_t *env)
{
  /* Args are themselves not in tail position. */
  int saved_tail = in_tail_position;
  in_tail_position = 0;
  exp_t *marker = make_nil();
  marker->flags |= FLAG_TAILREC;
  marker->content = refexp(fn);
  exp_t *tail = marker;
  exp_t *src  = call_form->next;
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

void bytecode_free(bytecode_t *bc) {
  if (!bc) return;
  int i;
  for (i = 0; i < bc->nconsts; i++) unrefexp(bc->consts[i]);
  free(bc->consts);
  free(bc->code);
#ifdef ALCOVE_JIT
  if (bc->jit_mem) munmap(bc->jit_mem, bc->jit_size);
#endif
  free(bc);
}

#ifdef ALCOVE_JIT
/* ---------------- JIT (arm64-only proof of concept) ----------------
   Recognizes a narrow set of lambda body shapes and emits arm64
   machine code that bypasses the bytecode dispatch loop entirely.
   Currently handled (one-param lambdas only):
     - LOAD_SLOT 0; RET                     →  identity      (n)
     - LOAD_FIX K; RET                      →  constant      K
     - LOAD_SLOT 0; LOAD_FIX K; ADD; RET    →  (+ n K) for K in int16
     - LOAD_SLOT 0; LOAD_FIX K; SUB; RET    →  (- n K) for K in int16
   Anything else: jit_compile returns 0; the bytecode interpreter
   handles the call.
   This is foundation work — copy-and-patch templates, broader op
   coverage, and call-out support to alcove's runtime are next-session
   work. The win here is the wiring: mmap, write-protect toggle on
   Apple Silicon, instruction emission, and the dispatch hook in
   vm_invoke_values. */

#if !defined(__aarch64__)
#error "ALCOVE_JIT currently requires arm64. Disable with -UALCOVE_JIT."
#endif

/* arm64 instruction encoders. All return uint32_t little-endian; arm64
   is fixed-width 4-byte instructions. */

/* LDR Xt, [Xn, #imm]   — imm is 8-byte aligned offset, 0..32760. */
static uint32_t arm64_ldr_imm(int rt, int rn, int byte_offset) {
  uint32_t imm12 = (uint32_t)(byte_offset / 8) & 0xfff;
  return 0xF9400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
/* ADD Xd, Xn, #imm12 (no shift). */
static uint32_t arm64_add_imm(int rd, int rn, int imm) {
  return 0x91000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
/* SUB Xd, Xn, #imm12 (no shift). */
static uint32_t arm64_sub_imm(int rd, int rn, int imm) {
  return 0xD1000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
/* MOVZ Xd, #imm16, LSL #(hw*16) */
static uint32_t arm64_movz(int rd, uint16_t imm, int hw) {
  return 0xD2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd;
}
/* MOVK Xd, #imm16, LSL #(hw*16) — keep other bits */
static uint32_t arm64_movk(int rd, uint16_t imm, int hw) {
  return 0xF2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd;
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
  return 0xF1000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) | 31u;
}
/* B (unconditional, PC-relative). off is in INSTRUCTIONS (×4 for bytes), signed. */
static uint32_t arm64_b(int off_insns) {
  return 0x14000000u | ((uint32_t)off_insns & 0x3FFFFFFu);
}
/* B.cond — off in instructions, signed 19-bit. cond is the 4-bit code:
   GE=10, LT=11, GT=12, LE=13. */
static uint32_t arm64_b_cond(int cond, int off_insns) {
  return 0x54000000u | (((uint32_t)off_insns & 0x7FFFFu) << 5) | ((uint32_t)cond & 0xfu);
}
/* TBZ Xt, #bit, label — branch if bit is zero. off in instructions, signed 14-bit. */
static uint32_t arm64_tbz(int rt, int bit, int off_insns) {
  uint32_t b40 = (uint32_t)(bit & 0x1f);
  uint32_t b5  = (bit & 0x20) ? 1u : 0u;
  return 0x36000000u | (b5 << 31) | (b40 << 19)
       | (((uint32_t)off_insns & 0x3FFFu) << 5) | (uint32_t)(rt & 0x1f);
}
/* MOV Xd, Xm  — alias for ORR Xd, XZR, Xm. */
static uint32_t arm64_mov_reg(int rd, int rm) {
  return 0xAA0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}

/* Materialize an arbitrary 64-bit immediate into Xd via MOVZ + up-to-3 MOVKs. */
static int emit_mov64(uint32_t *out, int rd, uint64_t v) {
  int n = 0;
  out[n++] = arm64_movz(rd, (uint16_t)(v & 0xffff), 0);
  if ((v >> 16) & 0xffff) out[n++] = arm64_movk(rd, (uint16_t)((v >> 16) & 0xffff), 1);
  if ((v >> 32) & 0xffff) out[n++] = arm64_movk(rd, (uint16_t)((v >> 32) & 0xffff), 2);
  if ((v >> 48) & 0xffff) out[n++] = arm64_movk(rd, (uint16_t)((v >> 48) & 0xffff), 3);
  return n;
}

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
  __builtin___clear_cache((char*)p, (char*)p + sz);
}

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
  if (bc->ncode != 19) return 0;
  uint8_t *c = bc->code;

  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX) return 0;
  uint8_t cmp_slot = c[1];
  int16_t cmp_imm  = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE) return 0;
  /* Don't validate the inner offsets — we know the layout from the
     compiler. The pattern check on op kinds + RET at the tail is
     sufficient. */

  uint8_t arith_op = c[7];
  if (arith_op != OP_SLOT_SUB_FIX && arith_op != OP_SLOT_ADD_FIX) return 0;
  uint8_t arith_slot = c[8];
  int16_t arith_imm  = (int16_t)((uint16_t)c[9] | ((uint16_t)c[10] << 8));

  if (c[11] != OP_TAIL_SELF || c[12] != 1) return 0;
  if (c[13] != OP_JUMP) return 0;
  if (c[16] != OP_LOAD_SLOT) return 0;
  uint8_t load_slot = c[17];
  if (c[18] != OP_RET) return 0;

  if (cmp_slot != arith_slot || cmp_slot != load_slot) return 0;
  if (cmp_slot >= ENV_INLINE_SLOTS) return 0;

  int slot_off = (int)offsetof(env_t, inline_vals[0]) + (int)cmp_slot * 8;
  if (slot_off < 0 || slot_off > 32760) return 0;

  /* Tagged immediate for cmp: FIX(K1) = (K1<<3)|1. Must fit u12. */
  int64_t cmp_tagged_64 = ((int64_t)cmp_imm << 3) | 1;
  if (cmp_tagged_64 < 0 || cmp_tagged_64 > 4095) return 0;

  /* Arithmetic delta is K2<<3 (preserves tag bit). Must fit u12 add/sub. */
  int arith_delta = ((int)arith_imm) << 3;
  if (arith_delta < 0 || arith_delta > 4095) return 0;

  /* Branch condition for "BR_IF_FALSE on cmp's result" — invert cmp.
     ARM64 cond codes: GE=10, LT=11, GT=12, LE=13. */
  int cond;
  switch (cmp_op) {
    case OP_SLOT_GT_FIX: cond = 13; break;  /* !GT → LE */
    case OP_SLOT_LT_FIX: cond = 10; break;  /* !LT → GE */
    case OP_SLOT_GE_FIX: cond = 11; break;  /* !GE → LT */
    case OP_SLOT_LE_FIX: cond = 12; break;  /* !LE → GT */
    default: return 0;
  }

  int n = 0;
  /* Load slot value once; verify it's a tagged fixnum (bit 0 set).
     If not, branch to deopt → return NULL → caller falls back to vm_run. */
  out[n++] = arm64_ldr_imm(1, 0, slot_off);            /* ldr x1, [x0,#off] */
  int patch_tbz = n;
  out[n++] = 0;                                         /* placeholder tbz x1,#0,deopt */
  int loop_top = n;
  out[n++] = arm64_cmp_imm(1, (int)cmp_tagged_64);     /* cmp x1, #FIX(K1) */
  int patch_bcond = n;
  out[n++] = 0;                                         /* placeholder b.cond end */
  if (arith_op == OP_SLOT_SUB_FIX)
    out[n++] = arm64_sub_imm(1, 1, arith_delta);
  else
    out[n++] = arm64_add_imm(1, 1, arith_delta);
  /* tag bit preserved across ±(K2<<3); subsequent loads stay tagged. */
  out[n++] = arm64_str_imm(1, 0, slot_off);            /* str x1, [x0,#off] */
  out[n++] = arm64_b(loop_top - n);                    /* b loop_top */
  /* end: */
  int end_pc = n;
  out[patch_bcond] = arm64_b_cond(cond, end_pc - patch_bcond);
  out[n++] = arm64_mov_reg(0, 1);                      /* x0 = x1 (last value) */
  out[n++] = arm64_ret();
  /* deopt: */
  int deopt_pc = n;
  out[patch_tbz] = arm64_tbz(1, 0, deopt_pc - patch_tbz);
  out[n++] = arm64_movz(0, 0, 0);                      /* mov x0, #0 (NULL) */
  out[n++] = arm64_ret();

  *outn = n;
  return 1;
}

int jit_compile(bytecode_t *bc) {
  if (!bc || bc->jit) return bc && bc->jit ? 1 : 0;
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
  }
  else if (bc->ncode == 3 && c[0] == OP_LOAD_SLOT && c[1] == 0 && c[2] == OP_RET) {
    /* (fn (n) n) — return env->inline_vals[0]. */
    insns[n++] = arm64_ldr_imm(0, 0, offsetof(env_t, inline_vals[0]));
    insns[n++] = arm64_ret();
  }
  else if (bc->ncode == 7 && c[0] == OP_LOAD_SLOT && c[1] == 0 &&
           c[2] == OP_LOAD_FIX &&
           (c[5] == OP_ADD || c[5] == OP_SUB) && c[6] == OP_RET) {
    /* (fn (n) (op n K)) where K fits int16. ADD/SUB on a tagged
       fixnum is just ±(K<<3): the low tag bit is preserved. */
    int16_t k = (int16_t)((uint16_t)c[3] | ((uint16_t)c[4] << 8));
    int delta = ((int)k) << 3;        /* fits because |K| <= 32767 */
    if (delta < 0 || delta > 4095) return 0;  /* out of immediate range */
    insns[n++] = arm64_ldr_imm(0, 0, offsetof(env_t, inline_vals[0]));
    int patch_tbz = n;
    insns[n++] = 0;                            /* tbz x0,#0,deopt */
    insns[n++] = (c[5] == OP_ADD) ? arm64_add_imm(0, 0, delta)
                                  : arm64_sub_imm(0, 0, delta);
    insns[n++] = arm64_ret();
    int deopt_pc = n;
    insns[patch_tbz] = arm64_tbz(0, 0, deopt_pc - patch_tbz);
    insns[n++] = arm64_movz(0, 0, 0);          /* mov x0, #0 (NULL) */
    insns[n++] = arm64_ret();
  }
  /* Also handle the slot-fix superinstructions that the compiler emits
     today instead of the 3-op sequence. */
  else if (bc->ncode == 5 &&
           c[0] == OP_LOAD_SLOT && c[1] == 0 &&
           (c[2] == OP_SLOT_ADD_FIX || c[2] == OP_SLOT_SUB_FIX) &&
           0 /* would need to re-read slot/imm; defer */ ) {
    return 0;
  }
  else if (bc->ncode == 5 &&
           (c[0] == OP_SLOT_ADD_FIX || c[0] == OP_SLOT_SUB_FIX) &&
           c[1] == 0 && c[4] == OP_RET) {
    int16_t k = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));
    int delta = ((int)k) << 3;
    if (delta < 0 || delta > 4095) return 0;
    insns[n++] = arm64_ldr_imm(0, 0, offsetof(env_t, inline_vals[0]));
    int patch_tbz = n;
    insns[n++] = 0;                            /* tbz x0,#0,deopt */
    insns[n++] = (c[0] == OP_SLOT_ADD_FIX) ? arm64_add_imm(0, 0, delta)
                                           : arm64_sub_imm(0, 0, delta);
    insns[n++] = arm64_ret();
    int deopt_pc = n;
    insns[patch_tbz] = arm64_tbz(0, 0, deopt_pc - patch_tbz);
    insns[n++] = arm64_movz(0, 0, 0);
    insns[n++] = arm64_ret();
  }
  else {
    return 0;  /* shape not recognized */
  }

  size_t sz = (size_t)n * 4;
  size_t pagesz = 4096;
  size_t mapsz = (sz + pagesz - 1) & ~(pagesz - 1);
  void *page = jit_alloc(mapsz);
  if (!page) return 0;
  jit_write_begin();
  memcpy(page, insns, sz);
  jit_write_end(page, sz);
  bc->jit = (exp_t *(*)(env_t *))page;
  bc->jit_mem = page;
  bc->jit_size = mapsz;
  return 1;
}
#endif /* ALCOVE_JIT */

typedef struct compiler_t {
  uint8_t *code;  int ncode;  int code_cap;
  exp_t  **consts; int nconsts; int consts_cap;
  /* slot_names[0..nparams) = lambda params; slot_names[nparams..nslots)
     = let/with-bound names. Scope-managed as a stack. */
  char    *slot_names[ENV_INLINE_SLOTS];
  int      nparams;
  int      nslots;      /* current total: nparams + active let/with bindings */
  int      nlet_depth;  /* >0 disables OP_TAIL_SELF (keys/slots mismatch) */
  const char *self_name; /* for self-tail-call detection; NULL in anon fn */
  int      failed;
} compiler_t;

static void emit_u8(compiler_t *c, uint8_t b) {
  if (c->failed) return;
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
  for (i = 0; i < c->nconsts; i++) if (c->consts[i] == v) return i;
  /* OP_LOAD_CONST encodes the index as u8, so at most 256 distinct
     constants per lambda. Above that we bail rather than silently
     wrap — the tree-walker will still handle the body. */
  if (c->nconsts >= 256) { c->failed = 1; return -1; }
  if (c->nconsts + 1 > c->consts_cap) {
    c->consts_cap = c->consts_cap ? c->consts_cap * 2 : 8;
    c->consts = realloc(c->consts, c->consts_cap * sizeof(exp_t*));
  }
  c->consts[c->nconsts] = refexp(v);
  return c->nconsts++;
}
static int find_slot(compiler_t *c, const char *name) {
  /* Innermost (highest idx) binding wins so inner let shadows outer.
     NULL slot_names are "hidden" (e.g. for's end-value slot) — skipped. */
  int i;
  for (i = c->nslots - 1; i >= 0; i--) {
    if (!c->slot_names[i]) continue;
    if (strcmp(c->slot_names[i], name) == 0) return i;
  }
  return -1;
}

static int  op_for_head(const char *s);
static void compile_expr(compiler_t *c, exp_t *e, int tail);

/* Returns OP_ADD..OP_NOT for pure-arithmetic/cmp symbols, -1 otherwise. */
static int op_for_head(const char *s) {
  if (!strcmp(s, "+"))   return OP_ADD;
  if (!strcmp(s, "-"))   return OP_SUB;
  if (!strcmp(s, "*"))   return OP_MUL;
  if (!strcmp(s, "/"))   return OP_DIV;
  if (!strcmp(s, "<"))   return OP_LT;
  if (!strcmp(s, ">"))   return OP_GT;
  if (!strcmp(s, "<="))  return OP_LE;
  if (!strcmp(s, ">="))  return OP_GE;
  if (!strcmp(s, "is"))  return OP_IS;
  if (!strcmp(s, "iso")) return OP_ISO;
  if (!strcmp(s, "no"))  return OP_NOT;
  return -1;
}

static void compile_if(compiler_t *c, exp_t *form, int tail) {
  /* (if cond then else)  — only 2-way for phase 1 */
  exp_t *cond = cadr(form);
  exp_t *thn  = caddr(form);
  exp_t *els  = cadddr(form);
  compile_expr(c, cond, 0);
  if (c->failed) return;
  emit_u8(c, OP_BR_IF_FALSE);
  int patch_false = c->ncode; emit_i16(c, 0);
  compile_expr(c, thn, tail);
  if (c->failed) return;
  emit_u8(c, OP_JUMP);
  int patch_end = c->ncode; emit_i16(c, 0);
  int false_target = c->ncode;
  if (els) compile_expr(c, els, tail);
  else {
    /* (if cond then) with no else: result is nil. */
    int k = add_const(c, NIL_EXP);
    emit_u8(c, OP_LOAD_CONST); emit_u8(c, (uint8_t)k);
  }
  if (c->failed) return;
  int end_target = c->ncode;
  int16_t off_false = (int16_t)(false_target - (patch_false + 2));
  int16_t off_end   = (int16_t)(end_target   - (patch_end   + 2));
  c->code[patch_false]   = off_false & 0xff;
  c->code[patch_false+1] = (off_false >> 8) & 0xff;
  c->code[patch_end]     = off_end   & 0xff;
  c->code[patch_end+1]   = (off_end   >> 8) & 0xff;
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
  if (!is_self_tail) {
    compile_expr(c, head, 0);
    if (c->failed) return;
  }
  for (a = form->next; a; a = a->next) {
    compile_expr(c, a->content, 0);
    if (c->failed) return;
    nargs++;
  }
  if (nargs > 255) { c->failed = 1; return; }
  if      (is_self_tail)  { emit_u8(c, OP_TAIL_SELF); emit_u8(c, (uint8_t)nargs); }
  else if (is_cross_tail) { emit_u8(c, OP_TAIL_CALL); emit_u8(c, (uint8_t)nargs); }
  else                    { emit_u8(c, OP_CALL);      emit_u8(c, (uint8_t)nargs); }
}

/* Superinstruction fuse table — maps a plain binary op to its fused
   slot-op-fix variant. Returns 0 when no fuse exists for this op. */
static int fuse_slot_fix(int op) {
  switch (op) {
    case OP_ADD: return OP_SLOT_ADD_FIX;
    case OP_SUB: return OP_SLOT_SUB_FIX;
    case OP_LT:  return OP_SLOT_LT_FIX;
    case OP_LE:  return OP_SLOT_LE_FIX;
    case OP_GT:  return OP_SLOT_GT_FIX;
    case OP_GE:  return OP_SLOT_GE_FIX;
    default:     return 0;
  }
}

static void compile_arith(compiler_t *c, exp_t *form, int op) {
  /* Binary left-fold: (+ a b c d) → a b + c + d + */
  exp_t *a = form->next;
  if (!a || !a->next) { c->failed = 1; return; }
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

  compile_expr(c, arg1, 0); if (c->failed) return;
  compile_expr(c, arg2, 0); if (c->failed) return;
  emit_u8(c, (uint8_t)op);
  for (a = a->next->next; a; a = a->next) {
    compile_expr(c, a->content, 0); if (c->failed) return;
    emit_u8(c, (uint8_t)op);
  }
}

/* (= sym val) — only when sym resolves to a local slot. Global / car /
   cdr / string-index assignment stays in the tree-walker. */
static void compile_assign(compiler_t *c, exp_t *form, int tail) {
  (void)tail;
  exp_t *key = cadr(form);
  exp_t *val = caddr(form);
  if (!is_ptr(key) || !issymbol(key)) { c->failed = 1; return; }
  int slot = find_slot(c, key->ptr);
  if (slot < 0) { c->failed = 1; return; }
  compile_expr(c, val, 0); if (c->failed) return;
  emit_u8(c, OP_STORE_SLOT); emit_u8(c, (uint8_t)slot);
  /* STORE_SLOT re-pushes the stored value so (= x v) returns v. */
}

/* (let var val body) — single binding, evaluates body in extended scope.
   Falls back to AST if slot count would exceed ENV_INLINE_SLOTS. */
static void compile_let(compiler_t *c, exp_t *form, int tail) {
  exp_t *var  = cadr(form);
  exp_t *val  = caddr(form);
  exp_t *body = cadddr(form);
  if (!is_ptr(var) || !issymbol(var) || !body) { c->failed = 1; return; }
  if (c->nslots >= ENV_INLINE_SLOTS) { c->failed = 1; return; }
  int slot = c->nslots;
  compile_expr(c, val, 0); if (c->failed) return;
  emit_u8(c, OP_BIND_SLOT); emit_u8(c, (uint8_t)slot);
  c->slot_names[slot] = (char*)var->ptr;
  c->nslots++;
  c->nlet_depth++;
  compile_expr(c, body, tail); if (c->failed) return;
  c->nlet_depth--;
  c->nslots--;
  /* Body's value is on the stack. Peek-unbind: swap top of stack with
     the slot, but we only have POP/PUSH — cheapest is stash via a temp
     dedicated op. Simpler: store body result into slot (STORE discards
     old binding), emit LOAD_SLOT, emit UNBIND_SLOT — round-trip but
     clean. Actually simpler yet: the binding's owning ref is still in
     the slot; we can just emit UNBIND_SLOT which unrefs and NULLs it,
     leaving the body result untouched on the stack. */
  emit_u8(c, OP_UNBIND_SLOT); emit_u8(c, (uint8_t)slot);
}

/* (with (v1 e1 v2 e2 ...) body) — N parallel-like bindings then body.
   In alcove's semantics, bindings evaluate left-to-right against the
   enclosing env (each val doesn't see earlier v's in the same with,
   matching the tree-walker's withcmd). */
static void compile_with(compiler_t *c, exp_t *form, int tail) {
  exp_t *pairs = cadr(form);
  exp_t *body  = caddr(form);
  if (!is_ptr(pairs) || !ispair(pairs) || !body) { c->failed = 1; return; }
  /* Collect (var, val) pairs. */
  int start_slot = c->nslots;
  int nbindings = 0;
  exp_t *p = pairs;
  while (p) {
    exp_t *var = p->content;
    exp_t *nxt = p->next;
    if (!nxt) { c->failed = 1; return; }
    exp_t *val = nxt->content;
    if (!is_ptr(var) || !issymbol(var)) { c->failed = 1; return; }
    if (c->nslots >= ENV_INLINE_SLOTS) { c->failed = 1; return; }
    compile_expr(c, val, 0); if (c->failed) return;
    emit_u8(c, OP_BIND_SLOT); emit_u8(c, (uint8_t)c->nslots);
    c->slot_names[c->nslots] = (char*)var->ptr;
    c->nslots++;
    nbindings++;
    p = nxt->next;
  }
  c->nlet_depth++;
  compile_expr(c, body, tail); if (c->failed) return;
  c->nlet_depth--;
  /* Unbind in reverse order. */
  int i;
  for (i = nbindings - 1; i >= 0; i--) {
    emit_u8(c, OP_UNBIND_SLOT); emit_u8(c, (uint8_t)(start_slot + i));
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
  exp_t *var_node   = form->next;       if (!var_node)   { c->failed=1; return; }
  exp_t *var        = var_node->content;
  exp_t *start_node = var_node->next;   if (!start_node) { c->failed=1; return; }
  exp_t *end_node   = start_node->next; if (!end_node)   { c->failed=1; return; }
  exp_t *body_node  = end_node->next;

  if (!is_ptr(var) || !issymbol(var)) { c->failed=1; return; }
  if (c->nslots + 2 > ENV_INLINE_SLOTS) { c->failed=1; return; }

  int counter_slot = c->nslots;
  int end_slot     = c->nslots + 1;

  compile_expr(c, start_node->content, 0); if (c->failed) return;
  emit_u8(c, OP_BIND_SLOT); emit_u8(c, (uint8_t)counter_slot);
  c->slot_names[counter_slot] = (char*)var->ptr;
  c->nslots++;

  compile_expr(c, end_node->content, 0); if (c->failed) return;
  emit_u8(c, OP_BIND_SLOT); emit_u8(c, (uint8_t)end_slot);
  c->slot_names[end_slot] = NULL;
  c->nslots++;

  /* Seed the loop's "current result" with nil so an un-entered or
     empty-body for still returns something at exit. */
  int k_nil = add_const(c, nil_singleton);
  emit_u8(c, OP_LOAD_CONST); emit_u8(c, (uint8_t)k_nil);

  c->nlet_depth++;
  int loop_top = c->ncode;

  emit_u8(c, OP_LOAD_SLOT); emit_u8(c, (uint8_t)counter_slot);
  emit_u8(c, OP_LOAD_SLOT); emit_u8(c, (uint8_t)end_slot);
  emit_u8(c, OP_LE);
  emit_u8(c, OP_BR_IF_FALSE);
  int patch_exit = c->ncode; emit_i16(c, 0);

  if (body_node) {
    /* Replace previous iteration's result with this one's. */
    emit_u8(c, OP_POP);
    exp_t *b;
    for (b = body_node; b; b = b->next) {
      compile_expr(c, b->content, 0); if (c->failed) return;
      if (b->next) emit_u8(c, OP_POP);  /* discard non-last body exprs */
    }
    /* Last body expr's value remains on stack as the new "current result". */
  }

  emit_u8(c, OP_LOAD_SLOT); emit_u8(c, (uint8_t)counter_slot);
  emit_u8(c, OP_LOAD_FIX);  emit_i16(c, 1);
  emit_u8(c, OP_ADD);
  emit_u8(c, OP_STORE_SLOT); emit_u8(c, (uint8_t)counter_slot);
  emit_u8(c, OP_POP);

  emit_u8(c, OP_JUMP);
  int patch_jump = c->ncode; emit_i16(c, 0);

  int loop_end = c->ncode;

  int16_t off_exit = (int16_t)(loop_end - (patch_exit + 2));
  c->code[patch_exit]   = off_exit & 0xff;
  c->code[patch_exit+1] = (off_exit >> 8) & 0xff;
  int16_t off_jump = (int16_t)(loop_top - (patch_jump + 2));
  c->code[patch_jump]   = off_jump & 0xff;
  c->code[patch_jump+1] = (off_jump >> 8) & 0xff;

  c->nlet_depth--;

  emit_u8(c, OP_UNBIND_SLOT); emit_u8(c, (uint8_t)end_slot);
  emit_u8(c, OP_UNBIND_SLOT); emit_u8(c, (uint8_t)counter_slot);
  c->nslots -= 2;
  /* Result left on top of stack by the last iteration (or the seed nil). */
}

static void compile_expr(compiler_t *c, exp_t *e, int tail) {
  if (c->failed) return;
  /* Tagged fixnum literal: if it fits in int16, inline; else const pool. */
  if (isnumber(e)) {
    int64_t v = FIX_VAL(e);
    if (v >= INT16_MIN && v <= INT16_MAX) {
      emit_u8(c, OP_LOAD_FIX);
      emit_i16(c, (int16_t)v);
    } else {
      int k = add_const(c, e);
      emit_u8(c, OP_LOAD_CONST); emit_u8(c, (uint8_t)k);
    }
    return;
  }
  if (!is_ptr(e)) {
    /* tagged char or other immediate */
    int k = add_const(c, e);
    emit_u8(c, OP_LOAD_CONST); emit_u8(c, (uint8_t)k);
    return;
  }
  if (isstring(e) || isfloat(e)) {
    int k = add_const(c, e);
    emit_u8(c, OP_LOAD_CONST); emit_u8(c, (uint8_t)k);
    return;
  }
  if (e == nil_singleton || e == true_singleton) {
    int k = add_const(c, e);
    emit_u8(c, OP_LOAD_CONST); emit_u8(c, (uint8_t)k);
    return;
  }
  if (issymbol(e)) {
    int slot = find_slot(c, e->ptr);
    if (slot >= 0) {
      emit_u8(c, OP_LOAD_SLOT); emit_u8(c, (uint8_t)slot);
      return;
    }
    /* Global / builtin. Runtime lookup via the constant (the symbol
       itself is the key — lookup will cache on it via meta). */
    int k = add_const(c, e);
    emit_u8(c, OP_LOAD_GLOBAL); emit_u8(c, (uint8_t)k);
    return;
  }
  if (!ispair(e)) { c->failed = 1; return; }

  /* Call form. Dispatch on head. */
  exp_t *head = car(e);
  if (is_ptr(head) && issymbol(head)) {
    const char *s = (const char*)head->ptr;
    if (!strcmp(s, "if"))    { compile_if(c, e, tail); return; }
    if (!strcmp(s, "let"))   { compile_let(c, e, tail); return; }
    if (!strcmp(s, "with"))  { compile_with(c, e, tail); return; }
    if (!strcmp(s, "for"))   { compile_for(c, e, tail); return; }
    if (!strcmp(s, "="))     { compile_assign(c, e, tail); return; }
    if (!strcmp(s, "cons")) {
      exp_t *a = cadr(e), *b = caddr(e);
      if (!a || !b) { c->failed = 1; return; }
      compile_expr(c, a, 0); if (c->failed) return;
      compile_expr(c, b, 0); if (c->failed) return;
      emit_u8(c, OP_CONS);
      return;
    }
    if (!strcmp(s, "car")) {
      exp_t *a = cadr(e);
      if (!a) { c->failed = 1; return; }
      compile_expr(c, a, 0); if (c->failed) return;
      emit_u8(c, OP_CAR);
      return;
    }
    if (!strcmp(s, "cdr")) {
      exp_t *a = cadr(e);
      if (!a) { c->failed = 1; return; }
      compile_expr(c, a, 0); if (c->failed) return;
      emit_u8(c, OP_CDR);
      return;
    }
    if (!strcmp(s, "list")) {
      int n = 0;
      exp_t *a;
      for (a = e->next; a; a = a->next) {
        compile_expr(c, a->content, 0); if (c->failed) return;
        n++;
        if (n > 255) { c->failed = 1; return; }
      }
      emit_u8(c, OP_LIST); emit_u8(c, (uint8_t)n);
      return;
    }
    if (!strcmp(s, "quote")) {
      exp_t *q = cadr(e);
      int k = add_const(c, q ? q : nil_singleton);
      emit_u8(c, OP_LOAD_CONST); emit_u8(c, (uint8_t)k);
      return;
    }
    int op = op_for_head(s);
    if (op >= 0) {
      if (op == OP_NOT) {
        /* Unary: (no x) */
        if (!e->next) { c->failed = 1; return; }
        compile_expr(c, e->next->content, 0); if (c->failed) return;
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
    keyval_t *kv = set_get_keyval_dict(reserved_symbol, (char*)s, NULL);
    if (kv && is_ptr(kv->val) && isinternal(kv->val)) {
      c->failed = 1; return;
    }
    compile_call(c, e, tail);
    return;
  }
  /* Complex head — fall back. */
  c->failed = 1;
}

int compile_lambda(exp_t *fn) {
  if (!fn || !islambda(fn)) return 0;
  if (fn->flags & FLAG_COMPILED) return 1; /* idempotent */
  exp_t *params = fn->content;
  exp_t *body   = fn->next->content;
  compiler_t c = {0};
  c.self_name = (const char*)fn->meta;  /* may be NULL for anon fn */

  /* Register params into slots 0..N-1 matching env->inline_slots. */
  exp_t *p;
  for (p = params; p; p = p->next) {
    if (c.nparams >= ENV_INLINE_SLOTS) { c.failed = 1; break; }
    if (!is_ptr(p->content) || !issymbol(p->content)) { c.failed = 1; break; }
    c.slot_names[c.nparams++] = (char*)p->content->ptr;
  }
  c.nslots = c.nparams;

  /* Walk body list: each expression, pop between, except the last. */
  exp_t *b;
  int saw_any = 0;
  for (b = body; b && !c.failed; b = b->next) {
    if (saw_any) emit_u8(&c, OP_POP);
    int is_last = (b->next == NULL);
    compile_expr(&c, b->content, is_last);
    saw_any = 1;
  }
  if (!saw_any) { c.failed = 1; }
  if (!c.failed) emit_u8(&c, OP_RET);

  if (c.failed) {
    int i;
    for (i = 0; i < c.nconsts; i++) unrefexp(c.consts[i]);
    free(c.consts); free(c.code);
    return 0;
  }
  bytecode_t *bc = calloc(1, sizeof(bytecode_t));
  bc->code = c.code; bc->ncode = c.ncode;
  bc->consts = c.consts; bc->nconsts = c.nconsts;
  fn->bc = bc;
  fn->flags |= FLAG_COMPILED;
#ifdef ALCOVE_JIT
  jit_compile(bc);  /* opportunistic; no-op for shapes we don't recognize */
#endif
  return 1;
}

static exp_t *vm_invoke_values(exp_t *fn, int nargs, exp_t **argv, env_t *env);

/* Bytecode dispatch loop. Entered with `env` already populated (params
   in inline slots). Returns an owned exp_t* (or NULL).
   OP_TAIL_CALL re-enters via goto tail_reentry with a fresh fn —
   `fn_owned` tracks whether we took ownership of the post-tail fn (so
   we can unref it on final return or error). */
exp_t *vm_run(exp_t *fn, env_t *env)
{
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

#define RUNTIME_ERR(msg) do {                                        \
    exp_t *_err = error(ERROR_ILLEGAL_VALUE, fn, env, msg);          \
    int _i;                                                          \
    for (_i = 0; _i < sp; _i++) unrefexp(stack[_i]);                 \
    if (fn_owned) unrefexp(fn);                                      \
    return _err;                                                     \
  } while (0)
#define PUSH(v)   do {                                               \
    if (sp >= VM_STACK_MAX) RUNTIME_ERR("VM stack overflow");        \
    stack[sp++] = (v);                                               \
  } while (0)
#define POP()     (stack[--sp])
#define READ_U8   (code[pc++])
#define READ_I16  (pc+=2, (int16_t)(code[pc-2] | (code[pc-1] << 8)))

  /* Threaded dispatch via GCC/Clang computed goto: each op ends with a
     direct indirect branch to the next op's label. Lets the CPU's
     branch predictor learn per-op successor patterns — measurably
     faster than a single switch-based jump-table on hot loops. */
  static const void *const dispatch[OP_MAX] = {
    [OP_HALT]         = &&l_halt,
    [OP_RET]          = &&l_ret,
    [OP_POP]          = &&l_pop,
    [OP_LOAD_FIX]     = &&l_load_fix,
    [OP_LOAD_CONST]   = &&l_load_const,
    [OP_LOAD_SLOT]    = &&l_load_slot,
    [OP_LOAD_GLOBAL]  = &&l_load_global,
    [OP_STORE_SLOT]   = &&l_store_slot,
    [OP_BIND_SLOT]    = &&l_bind_slot,
    [OP_UNBIND_SLOT]  = &&l_unbind_slot,
    [OP_ADD]          = &&l_add,
    [OP_SUB]          = &&l_sub,
    [OP_MUL]          = &&l_mul,
    [OP_DIV]          = &&l_div,
    [OP_LT]           = &&l_lt,
    [OP_GT]           = &&l_gt,
    [OP_LE]           = &&l_le,
    [OP_GE]           = &&l_ge,
    [OP_IS]           = &&l_is,
    [OP_ISO]          = &&l_iso,
    [OP_NOT]          = &&l_not,
    [OP_JUMP]         = &&l_jump,
    [OP_BR_IF_FALSE]  = &&l_br_if_false,
    [OP_BR_IF_TRUE]   = &&l_br_if_true,
    [OP_CALL]         = &&l_call,
    [OP_TAIL_SELF]    = &&l_tail_self,
    [OP_TAIL_CALL]    = &&l_tail_call,
    [OP_CONS]         = &&l_cons,
    [OP_CAR]          = &&l_car,
    [OP_CDR]          = &&l_cdr,
    [OP_LIST]         = &&l_list,
    [OP_SLOT_ADD_FIX] = &&l_slot_add_fix,
    [OP_SLOT_SUB_FIX] = &&l_slot_sub_fix,
    [OP_SLOT_LT_FIX]  = &&l_slot_lt_fix,
    [OP_SLOT_LE_FIX]  = &&l_slot_le_fix,
    [OP_SLOT_GT_FIX]  = &&l_slot_gt_fix,
    [OP_SLOT_GE_FIX]  = &&l_slot_ge_fix,
  };
#ifndef NDEBUG
  /* Catches "added an opcode but forgot to initialize dispatch[]" —
     a designated-init gap silently leaves a slot NULL and would jump
     to 0 on that op. One-time cost at vm_run entry; NDEBUG strips it. */
  { int _i; for (_i = 0; _i < OP_MAX; _i++) assert(dispatch[_i] != NULL); }
#endif
#define NEXT goto *dispatch[code[pc++]]

  NEXT;

l_halt:
  RUNTIME_ERR("Bytecode: OP_HALT reached (compiler bug)");

l_ret: {
    exp_t *r = POP();
    while (sp > 0) unrefexp(POP());
    if (fn_owned) unrefexp(fn);
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
    exp_t *v = lookup(consts[idx], env);
    if (!v) RUNTIME_ERR("Unbound variable");
    PUSH(v);
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
    if (idx >= env->n_inline) env->n_inline = idx + 1;
    NEXT;
  }
l_unbind_slot: {
    /* let/with exit: release the binding. destroy_env would catch any
       leftover refs via n_inline, but we explicitly clear here so the
       slot is reusable for subsequent lets. */
    uint8_t idx = READ_U8;
    unrefexp(env->inline_vals[idx]);
    env->inline_vals[idx] = NULL;
    if (idx + 1 == env->n_inline) env->n_inline = idx;
    NEXT;
  }

  /* Numeric binary op helpers. COERCE_TO_DOUBLE implicitly references
     `a` and `b` in its error path so both operands get unref'd before
     we jump to the error return — not reusable outside BIN_ARITH /
     CMP_OP sites that name their operands `a` and `b`. */
#define COERCE_TO_DOUBLE(v, out, opname) do {                       \
    if      (isnumber(v)) (out) = (double)FIX_VAL(v);               \
    else if (isfloat(v))  { (out) = (v)->f; unrefexp(v); }          \
    else { unrefexp(a); unrefexp(b); RUNTIME_ERR(opname); }         \
  } while (0)
#define BIN_ARITH(op, opname) do {                                  \
    exp_t *b = POP(), *a = POP();                                   \
    if (isnumber(a) && isnumber(b)) {                               \
      PUSH(MAKE_FIX(FIX_VAL(a) op FIX_VAL(b)));                     \
    } else {                                                        \
      double da, db;                                                \
      COERCE_TO_DOUBLE(a, da, "Illegal value in " opname);          \
      COERCE_TO_DOUBLE(b, db, "Illegal value in " opname);          \
      PUSH(make_floatf(da op db));                                  \
    }                                                               \
  } while (0)

l_add: BIN_ARITH(+, "+"); NEXT;
l_sub: BIN_ARITH(-, "-"); NEXT;
l_mul: BIN_ARITH(*, "*"); NEXT;
l_div: {
    exp_t *b = POP(), *a = POP();
    if (isnumber(a) && isnumber(b)) {
      int64_t bb = FIX_VAL(b);
      if (bb == 0) RUNTIME_ERR("Illegal division by 0");
      PUSH(MAKE_FIX(FIX_VAL(a) / bb));
    } else {
      double da, db;
      COERCE_TO_DOUBLE(a, da, "Illegal value in /");
      COERCE_TO_DOUBLE(b, db, "Illegal value in /");
      if (db == 0) RUNTIME_ERR("Illegal division by 0");
      PUSH(make_floatf(da / db));
    }
    NEXT;
  }

  /* Integer compares on two fixnums skip the cast-to-double step
     (which would overflow at 61-bit boundaries). Mixed-type paths
     have a/b already unref'd by COERCE_TO_DOUBLE — fixnums are
     immediates and never need unref either, so no trailing cleanup. */
#define CMP_OP(intcmp, flcmp) do {                                  \
    exp_t *b = POP(), *a = POP();                                   \
    int r;                                                          \
    if (isnumber(a) && isnumber(b)) {                               \
      r = FIX_VAL(a) intcmp FIX_VAL(b);                             \
    } else {                                                        \
      double da, db;                                                \
      COERCE_TO_DOUBLE(a, da, "Illegal value in compare");          \
      COERCE_TO_DOUBLE(b, db, "Illegal value in compare");          \
      r = da flcmp db;                                              \
    }                                                               \
    PUSH(r ? TRUE_EXP : NIL_EXP);                                   \
  } while (0)

l_lt: CMP_OP(<,  <);  NEXT;
l_gt: CMP_OP(>,  >);  NEXT;
l_le: CMP_OP(<=, <=); NEXT;
l_ge: CMP_OP(>=, >=); NEXT;

l_is: {
    exp_t *b = POP(), *a = POP();
    int r = isequal(a, b);
    unrefexp(a); unrefexp(b);
    PUSH(r ? TRUE_EXP : NIL_EXP);
    NEXT;
  }
l_iso: {
    exp_t *b = POP(), *a = POP();
    int r = isoequal(a, b);
    unrefexp(a); unrefexp(b);
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
    if (!istrue(a)) pc += off;
    unrefexp(a);
    NEXT;
  }
l_br_if_true: {
    int16_t off = READ_I16;
    exp_t *a = POP();
    if (istrue(a)) pc += off;
    unrefexp(a);
    NEXT;
  }

l_tail_self: {
    /* Self-tail: rebind inline slots from the top of the operand
       stack, keep keys as-is (same fn → same params), jump to PC 0. */
    uint8_t n = READ_U8;
    int base = sp - n;
    int i;
    for (i = 0; i < env->n_inline; i++) unrefexp(env->inline_vals[i]);
    env->n_inline = n <= ENV_INLINE_SLOTS ? n : ENV_INLINE_SLOTS;
    for (i = 0; i < env->n_inline; i++) env->inline_vals[i] = stack[base + i];
    for (; i < n; i++) unrefexp(stack[base + i]);
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
      while (sp > 0) unrefexp(POP());
      if (fn_owned) unrefexp(fn);
      return ret;
    }
    if (!ret) ret = NIL_EXP;
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
      while (sp > 0) unrefexp(POP());
      if (fn_owned) unrefexp(fn);
      return ret;
    }

    /* Compiled target: stash args, unwind, rebind, jump. */
    if (n > ENV_INLINE_SLOTS) {
      int i; for (i = 0; i < n; i++) unrefexp(stack[base + i]);
      sp = base - 1;
      unrefexp(new_fn);
      RUNTIME_ERR("OP_TAIL_CALL: too many args");
    }
    exp_t *args_buf[ENV_INLINE_SLOTS];
    { int i; for (i = 0; i < n; i++) args_buf[i] = stack[base + i]; }
    sp = base - 1;   /* drop args */
    /* stack[base-1] (new_fn slot) is above sp; we've taken ownership */

    /* Release current env's inline slots + any dict. */
    { int i; for (i = 0; i < env->n_inline; i++) unrefexp(env->inline_vals[i]); }
    env->n_inline = 0;
    if (env->d) { destroy_dict(env->d); env->d = NULL; }

    /* Bind new args to new_fn's params. */
    exp_t *p = new_fn->content;
    int i = 0;
    while (p && i < n) {
      if (!is_ptr(p->content) || !issymbol(p->content)) {
        int j; for (j = i; j < n; j++) unrefexp(args_buf[j]);
        unrefexp(new_fn);
        if (fn_owned) unrefexp(fn);
        return error(ERROR_ILLEGAL_VALUE, fn, env, "OP_TAIL_CALL: bad param");
      }
      if (env->n_inline < ENV_INLINE_SLOTS) {
        env->inline_keys[env->n_inline] = (char*)p->content->ptr;
        env->inline_vals[env->n_inline] = args_buf[i];
        env->n_inline++;
      } else {
        if (!env->d) env->d = create_dict();
        set_get_keyval_dict(env->d, p->content->ptr, args_buf[i]);
        unrefexp(args_buf[i]);
      }
      p = p->next; i++;
    }
    while (i < n) unrefexp(args_buf[i++]);

    /* Swap in new_fn and re-enter. */
    if (fn_owned) unrefexp(fn);
    fn = new_fn;
    fn_owned = 1;
    goto tail_reentry;
  }

l_cons: {
    /* (cons a b): make_node(a) takes ownership of a; b becomes ->next.
       For (cons a nil) we drop the explicit nil tail to match conscmd. */
    exp_t *b = POP(), *a = POP();
    exp_t *pair = make_node(a);  /* transfers a's ref into pair->content */
    if (istrue(b)) pair->next = b;            /* transfers b's ref */
    else           { unrefexp(b); pair->next = NULL; }
    PUSH(pair);
    NEXT;
  }
l_car: {
    exp_t *p = POP();
    exp_t *v = car(p);                  /* borrowed (via macro guard) */
    PUSH(refexp(v));
    unrefexp(p);
    NEXT;
  }
l_cdr: {
    exp_t *p = POP();
    exp_t *v = cdr(p);                  /* borrowed */
    PUSH(refexp(v));
    unrefexp(p);
    NEXT;
  }
l_list: {
    /* (list a0 ... aN-1) → fresh list. Args own their refs; we transfer
       into the new pair chain. */
    uint8_t n = READ_U8;
    if (n == 0) { PUSH(NIL_EXP); NEXT; }
    int base = sp - n;
    exp_t *head = make_node(stack[base]);
    exp_t *cur  = head;
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
#define SLOT_FIX_NUMERIC(body_int, body_flt, opname) do {           \
    uint8_t idx = READ_U8;                                          \
    int16_t imm = READ_I16;                                         \
    exp_t *a = env->inline_vals[idx];                               \
    if (isnumber(a)) { body_int; }                                  \
    else if (isfloat(a)) { double da = a->f; (void)da; body_flt; }  \
    else RUNTIME_ERR("Illegal value in " opname);                   \
  } while (0)

l_slot_add_fix:
  SLOT_FIX_NUMERIC(
    PUSH(MAKE_FIX(FIX_VAL(a) + imm)),
    PUSH(make_floatf(da + (double)imm)),
    "+");
  NEXT;
l_slot_sub_fix:
  SLOT_FIX_NUMERIC(
    PUSH(MAKE_FIX(FIX_VAL(a) - imm)),
    PUSH(make_floatf(da - (double)imm)),
    "-");
  NEXT;
l_slot_lt_fix:
  SLOT_FIX_NUMERIC(
    PUSH(FIX_VAL(a) <  imm ? TRUE_EXP : NIL_EXP),
    PUSH(da <  (double)imm ? TRUE_EXP : NIL_EXP),
    "<");
  NEXT;
l_slot_le_fix:
  SLOT_FIX_NUMERIC(
    PUSH(FIX_VAL(a) <= imm ? TRUE_EXP : NIL_EXP),
    PUSH(da <= (double)imm ? TRUE_EXP : NIL_EXP),
    "<=");
  NEXT;
l_slot_gt_fix:
  SLOT_FIX_NUMERIC(
    PUSH(FIX_VAL(a) >  imm ? TRUE_EXP : NIL_EXP),
    PUSH(da >  (double)imm ? TRUE_EXP : NIL_EXP),
    ">");
  NEXT;
l_slot_ge_fix:
  SLOT_FIX_NUMERIC(
    PUSH(FIX_VAL(a) >= imm ? TRUE_EXP : NIL_EXP),
    PUSH(da >= (double)imm ? TRUE_EXP : NIL_EXP),
    ">=");
  NEXT;

#undef SLOT_FIX_NUMERIC

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
static exp_t *vm_invoke_values(exp_t *fn, int nargs, exp_t **argv, env_t *env)
{
  if (!is_ptr(fn) || !islambda(fn)) {
    int i; for (i = 0; i < nargs; i++) unrefexp(argv[i]);
    return error(ERROR_ILLEGAL_VALUE, fn, env, "Bytecode call: not a lambda");
  }
  env_t *newenv = make_env(env);
  /* callingfnc stays NULL — OP_CALL is always non-tail from our side. */

  exp_t *p = fn->content;
  int i = 0;
  while (p && i < nargs) {
    if (!is_ptr(p->content) || !issymbol(p->content)) {
      int j; for (j = i; j < nargs; j++) unrefexp(argv[j]);
      destroy_env(newenv);
      return error(ERROR_ILLEGAL_VALUE, fn, env, "Bytecode call: bad param");
    }
    if (newenv->n_inline < ENV_INLINE_SLOTS) {
      newenv->inline_keys[newenv->n_inline] = (char*)p->content->ptr;
      newenv->inline_vals[newenv->n_inline] = argv[i];
      newenv->n_inline++;
    } else {
      if (!newenv->d) newenv->d = create_dict();
      set_get_keyval_dict(newenv->d, p->content->ptr, argv[i]);
      unrefexp(argv[i]);
    }
    p = p->next; i++;
  }
  while (i < nargs) unrefexp(argv[i++]);

  exp_t *ret;
  if (fn->flags & FLAG_COMPILED) {
#ifdef ALCOVE_JIT
    if (fn->bc->jit) {
      ret = fn->bc->jit(newenv);
      if (!ret) ret = vm_run(fn, newenv);  /* JIT deopt → bytecode */
    } else
#endif
    ret = vm_run(fn, newenv);
  } else {
    exp_t *body = fn->next->content;
    ret = NULL;
    while (body) {
      if (ret) unrefexp(ret);
      ret = evaluate(refexp(body->content), newenv);
      if (ret && iserror(ret)) break;
      body = body->next;
    }
  }
  destroy_env(newenv);
  return ret;
}


exp_t *invoke(exp_t *e, exp_t *fn, env_t *env)
{
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
  if (verbose) { printf("invoke:"); print_node(e); printf("\n"); }
  {
    exp_t *body = fn->next->content;
    newenv = make_env(env);
    newenv->callingfnc = refexp(e);
    if ((ret = var2env(e, fn->content, e->next, newenv, true))) {
      destroy_env(newenv);
      unrefexp(fn);
      unrefexp(e);
      in_tail_position = outer_tail;
      return ret;
    }

    /* Compiled body: cross-function tail calls lose TCO here (internal
       OP_TAIL_SELF still applies). */
    if (fn->flags & FLAG_COMPILED) {
#ifdef ALCOVE_JIT
      if (fn->bc->jit) {
        ret = fn->bc->jit(newenv);
        if (!ret) ret = vm_run(fn, newenv);  /* JIT deopt → bytecode */
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
      if (ret) { unrefexp(ret); ret = NULL; }
      int is_last = (cur->next == NULL);
      in_tail_position = is_last;
      ret = EVAL(cur->content, newenv);

      if (is_last && ret && is_ptr(ret) && ispair(ret) &&
          (ret->flags & FLAG_TAILREC) && is_ptr(ret->content) && islambda(ret->content))
      {
        exp_t *marker = ret;
        ret = NULL;
        exp_t *resolved_fn = marker->content;

        if (resolved_fn == fn) {
          /* Self-recursion fast path: rebind params in place, skip the
             env teardown/rebuild. Marker args are already evaluated. */
          int i;
          for (i = 0; i < newenv->n_inline; i++) unrefexp(newenv->inline_vals[i]);
          newenv->n_inline = 0;
          if (newenv->d) { destroy_dict(newenv->d); newenv->d = NULL; }

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
                if (!newenv->d) newenv->d = create_dict();
                set_get_keyval_dict(newenv->d, curvar->content->ptr, v);
              }
            }
            curvar = curvar->next;
            curval = curval->next;
          }

          unrefexp(marker);
          cur = body;                /* restart the body loop */
          continue;
        }

        /* Different function: full unwind + tailrec jump. */
        exp_t *new_fn = resolved_fn;
        marker->content = NULL;
        if (new_fn->meta) {
          marker->content = make_symbol((char*)new_fn->meta, strlen((char*)new_fn->meta));
        } else {
          marker->content = make_symbol("_", 1);
        }
        marker->flags &= ~FLAG_TAILREC;

        destroy_env(newenv);
        unrefexp(fn);
        unrefexp(e);
        fn = new_fn;
        e  = marker;
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
exp_t *expandmacro(exp_t *e, exp_t *fn, env_t *env){
  env_t *newenv=make_env(NULL); // NULL instead of env
  exp_t *ret;

  if ((ret=var2env(e,fn->content,e->next,newenv,false))) {
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
  ret=expandmacro(e,fn,env);
  if (verbose){
    printf("\n expanded macro");
    print_node(ret);
    printf("\n");
  }
  unrefexp(e);
  if (ret && iserror(ret)) return ret;
  ret=evaluate(ret,env);

  return ret;
}


exp_t *evaluate(exp_t *e,env_t *env)
{
  /* TO DO UN REF VARS*/
  exp_t *tmpexp=NULL;
  exp_t *tmpexp2=NULL;
  exp_t *tmpevexp=NULL; /* Evaluated structure to be freed*/
  exp_t *ret=NULL;

   if (e==NULL) return NULL;
  if isatom(e)  {
      if issymbol(e) {
          if (((char*)e->ptr)[0] == ':') return e; // e is a keyword
          if ((tmpexp=lookup(e,env))) { unrefexp(e); return tmpexp;}
          else 
            { ret = error(ERROR_UNBOUND_VARIABLE,e,env,"Error unbound variable %s",e->ptr);
              unrefexp(e);
              return ret;
            }
        }
      else return e; // Number? String? Char? Boolean? Vector?
    }
  else if ispair(e) {
      tmpexp=car(e);
      if (tmpexp && ispair(tmpexp)) {
        tmpevexp=EVAL(tmpexp, env);
        tmpexp=tmpevexp;
      }
      if (tmpexp) {
        if isinternal(tmpexp)  {
            int was_tail = in_tail_position;
            if (!(tmpexp->flags & FLAG_TAIL_AWARE)) in_tail_position = 0;
            ret = tmpexp->fnc(e,env);
            in_tail_position = was_tail;
            goto finisht;
          }
        if (issymbol(tmpexp)) {
          if (((char*)tmpexp->ptr)[0] == ':') { ret = error(ERROR_ILLEGAL_VALUE,e,env,"Error keyword %s can not be used as function",tmpexp->ptr); goto finish;}// e is a keyword
          if ((tmpexp2=lookup(tmpexp,env))) {
            if isinternal(tmpexp2) {
                /* Tail flag propagates only into tail-aware cmds (ifcmd).
                   Others get it cleared so their sub-evaluations don't
                   misfire and build spurious trampoline markers. */
                int was_tail = in_tail_position;
                if (!(tmpexp2->flags & FLAG_TAIL_AWARE)) in_tail_position = 0;
                ret= tmpexp2->fnc(e,env);
                in_tail_position = was_tail;
                goto finisht;
              }
            else if islambda(tmpexp2) {
                if (in_tail_position) {
                  ret = make_tail_marker(tmpexp2, e, env);
                  unrefexp(tmpexp2);
                  goto finisht;
                }
                ret = invoke(e,tmpexp2,env);
                goto finisht;
              }
            else if ismacro(tmpexp2) { ret = invokemacro(e,tmpexp2,env); goto finisht;}
            else if ispair(tmpexp2) { ret = tmpexp2 ; goto finisht;}
            else { ret =  tmpexp2; goto finisht;}
          }
          else 
            { ret = error(ERROR_UNBOUND_VARIABLE,e,env,"Error unbound variable %s",tmpexp->ptr); goto finish; }
          ret = e; // what is happening here?
          goto finisht;
        }
        else if (isstring(tmpexp)) {
          tmpexp2=EVAL(cadr(e), env);
          if (isnumber(tmpexp2)){
            int64_t idx = FIX_VAL(tmpexp2);
            if ((idx>=0)&&(idx<(int64_t)strlen(tmpexp->ptr))){
              ret =  make_char(*((char *) tmpexp->ptr+idx));
            }
            else ret = error(ERROR_INDEX_OUT_OF_RANGE,e,env,"Error index out of range");
          }
          else ret = error(ERROR_NUMBER_EXPECTED,e,env,"Error number expected");
          unrefexp(tmpexp2);
          goto finish;
        }
        else if (islambda(tmpexp)) {
          if (in_tail_position) {
            ret = make_tail_marker(tmpexp, e, env);
            goto finisht;
          }
          ret = invoke(e,tmpexp,env); goto finisht;
        }
        else if (ismacro(tmpexp)) { ret = invokemacro(e,tmpexp,env); goto finisht;}
      }
      else if (tmpexp==NULL) return e;
      
      /*else if (islambda(tmpexp)) {
        return invoke(e,tmpexp,env);
        }*/
      else return e;
    } 
  else {
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

int main(int argc, char *argv[])
{
  //  char *cmd_res;
  char *strdict="test dict";
  //keyval_t* kv;
  dict_t* dict=create_dict();
  env_t *global=make_env(NULL);
  FILE *stream;
  int evaluatingfile=0;
  int idx=0;
  exp_t *t;
  exp_t *nil;
  exp_t *val;
  exp_tfuncList[EXP_CHAR]=(exp_tfunc*)memalloc(1,sizeof(exp_tfunc));
  exp_tfuncList[EXP_CHAR]->load=load_char;
  exp_tfuncList[EXP_CHAR]->dump=dump_char;
  exp_tfuncList[EXP_STRING]=(exp_tfunc*)memalloc(1,sizeof(exp_tfunc));
  exp_tfuncList[EXP_STRING]->load=load_string;
  exp_tfuncList[EXP_STRING]->dump=dump_string;


  reserved_symbol=create_dict();
  /* Allocate immortal singletons before any other code references them. */
  nil_singleton = make_nil();
  true_singleton = make_symbol("t", 1);
  set_get_keyval_dict(reserved_symbol,"nil",nil=NIL_EXP);
  set_get_keyval_dict(reserved_symbol,"t",t=TRUE_EXP);
  
  int N=sizeof(lispProcList)/sizeof(lispProc);
  int i;
  for (i = 0; i < N; ++i) {
    set_get_keyval_dict(reserved_symbol,lispProcList[i].name,val=make_internal(lispProcList[i].cmd,lispProcList[i].flags & FLAG_TAIL_AWARE));
    unrefexp(val);
  }

  if (argc>=2){
    if ((stream=fopen(argv[argc-1],"r"))){
      evaluatingfile=1;
      if (strcmp(argv[argc-2],"-i") == 0) 
        evaluatingfile|=2;
    }
    else
      { printf("Error opening %s\n",argv[argc-1]); exit(1);}
  }
  else stream=stdin;
  exp_t* stre=NULL;
  //make_string(strdict,strlen(strdict));
  exp_t* strf=NULL;
  //make_string(strdict,strlen(strdict));
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
  //unrefexp(stre);
  
  while (1){
    idx++;
    strf=NULL;
    if (!evaluatingfile) printf("\x1B[34mIn [\x1B[94m%d\x1B[34m]:\x1B[39m",idx);
    stre=reader(stream,0,0);
    if (iserror(stre) && (stre->flags == EXP_ERROR_PARSING_EOF)) {
      if (evaluatingfile) {
        if (evaluatingfile&2) { stream=stdin; evaluatingfile=0; unrefexp(stre); continue; }
        else { unrefexp(stre); goto endcleanly; }
      }
      /* Interactive EOF (Ctrl-D or piped input exhausted) */
      unrefexp(stre);
      if (!evaluatingfile) printf("\n");
      goto endcleanly;
    }
    if (verbose) {
      if (stre) printf("\x1B[35mstre:%p\x1B[39m\n",(void*)stre);
      print_node(stre);printf("\n");
    }
    if (issymbol(stre) && (strcmp(stre->ptr,"quit")==0)) { unrefexp(stre); break;}
    if (issymbol(stre) && (strcmp(stre->ptr,"toeval")==0)) { toeval=1-toeval;printf("%d\n",toeval);}
    //
    if (toeval)
      strf=evaluate(stre,global);
    else
      unrefexp(stre);
    if (!evaluatingfile) {
      if (strf) {
        if (verbose) {
          printf("\x1B[35mstrf:%p\x1B[39m\n",(void*)strf);
          inspectcmd(refexp(strf),global);
        }
        printf("\x1B[31mOut[\x1B[91m%d\x1B[31m]:\x1B[39m",idx);
        print_node(strf);
      } else printf("nil");
      printf("\n\n");
    };
    
    if (strf) { 
      unrefexp(strf);
      strf=NULL;
    }
  }
 endcleanly:
  destroy_dict(dict);
  destroy_env(global);
  destroy_dict(reserved_symbol);
  free(exp_tfuncList[EXP_CHAR]);
  free(exp_tfuncList[EXP_STRING]);
  unrefexp(t);
  unrefexp(nil);

}
