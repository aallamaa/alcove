/* 
   Copyright (c) 2012 Abdelkader ALLAM abdelkader.allam@gmail.com

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 2 of the License, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

   In addition, as a special exception, is given the permission to link
   the code of this release  with the OpenSSL project's "OpenSSL"
   library (or with modified versions of it that use the same license as the
   "OpenSSL" library), and distribute the linked executables.  You must obey
   the GNU General Public License in all respects for all of the code used
   other than "OpenSSL".  If you modify this file, you may extend this
   exception to your version of the file, but you are not obligated to do so.
   If you do not wish to do so, delete this exception statement from your
   version. 
*/



#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/time.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "parser.h"




int verbose=0;
dict_t *reserved_symbol=NULL;
exp_tfunc* exp_tfuncList[EXP_MAXSIZE];

lispProc lispProcList[]={
  /* name, arity, flags, level, cmd*/
  {"verbose",2,0,0,verbosecmd},
  {"quote",2,0,0,quotecmd},
  {"if",2,0,0,ifcmd},
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
  {"defmacro",2,1,0,defmacro},
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
  {"do",2,1,0,docmd},
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
  ret=NIL_EXP;
  ret->type=EXP_ERROR;
  ret->flags=errnum;
  vasprintf((char**)&ret->ptr, err_message, ap);
  va_end(ap);
  ret->next=refexp(id);
  return ret;
}
#pragma GCC diagnostic warning "-Wunused-parameter"

/* MEMORY MANAGEMENT FUNCTIONS */

void *memalloc(size_t count, size_t size){
  return calloc(count,size);
  // ERROR MANAGEMENT TO BE ADDDED
}



exp_t *refexp(exp_t *e) {
  if (e) { __sync_add_and_fetch(&(e->nref),1); return e;}
  else return 0;
}

int unrefexp(exp_t *e){
  if (!e) return 0;
  int ret;
  if ((ret=__sync_sub_and_fetch(&(e->nref),1)) <=0) {
    // FREE META??
    if (e->meta) { free(e->meta); /* only lambda uses it so far */}
    if (e->next) unrefexp(e->next);
    if ((e->type==EXP_SYMBOL)||(e->type==EXP_STRING)||(e->type==EXP_ERROR))
      free(e->ptr);
    else if ((e->type>=EXP_NUMBER)||(e->type<=EXP_BOOLEAN)||(e->type==EXP_INTERNAL)) {}
    else 
      unrefexp(e->content); //check if content type is exp
    free(e);
    return 0;
  };
  return ret;
}

/* env management*/

inline env_t * make_env(env_t *rootenv)
{
  env_t *newenv=memalloc(1,sizeof(env_t));
  newenv->root=rootenv; //env;
  newenv->callingfnc=NULL;
  newenv->nref=1;
  return newenv;
}


inline void *destroy_env(env_t *env)
{
  if (__sync_sub_and_fetch(&(env->nref),1) <=0) {
    if (env->d) 
      destroy_dict(env->d);
    if (env->callingfnc)
      unrefexp(env->callingfnc);
    free(env);
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
  if (verbose) printf("Dumping dict %x\n",d);
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
    return k->val;
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
  nil_exp->nref=0;
  nil_exp->content=NULL;
  nil_exp->next=NULL;
  nil_exp->meta=NULL;
  return nil_exp;
}


inline exp_t *make_char(unsigned char c){
  exp_t *exp_char=NIL_EXP;
  exp_char->type=EXP_CHAR;
  exp_char->s64=c;
  return exp_char;
}


inline exp_t *make_node(exp_t *node){
  exp_t *cur=NIL_EXP;
  if (node) cur->content=refexp(node);
  return cur;
  
}

inline exp_t *make_internal(lispCmd *cmd){
  exp_t *cur=NIL_EXP;
  cur->type=EXP_INTERNAL;
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
  else tree->content=refexp(make_node(node));
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
      cur=cur->next=refexp(make_node(node));
      cur->content=refexp(node);
    }
  }
  else printf("ERROR IMPOSSIBLE TO ADD NODE TO NON PAIR OBJECT\n");

}

exp_t *make_tree(exp_t *root,exp_t *node1){
  exp_t *tree=NIL_EXP;
  tree->type=EXP_TREE;
  tree->next=refexp(root);
  if (node1)
    tree->content=refexp(node1);
  if (root) tree_add_node(root,tree);
  return tree;
}

void print_node(exp_t *node)
{
  if (node==NULL)
    printf("nil");
  else if (node->type==EXP_ERROR)
    {
      printf("Error: %s\n",(char*) node->ptr);
    }
  else if (node->type==EXP_TREE){
    printf("[ ");
    if (node->content)
      print_node(node->content);
    printf("] ");
  }
  else if (node->type==EXP_CHAR){
    if (node->s64>32) printf("#\\%c",(char)node->s64);
    else printf("#\\%lld",(long long int)node->s64);
  }
  else if (node->type==EXP_PAIR){
    if (istrue(node)) {printf("(");
      if (node->content) print_node(node->content);
      while ((node=node->next)) {
        if ispair(node) { printf(" ");print_node(node->content);}
        else {printf(" . "); print_node(node); break; }
      }
      printf(")");} else printf("nil");
  }
  else if (node->type==EXP_LAMBDA){
    if (node->meta) printf("#<procedure:%s@%08lx>",(char*)node->meta,(long) node);
    else printf("#<procedure@%08lx>",(long) node);
    if (verbose) { 
      printf("\theader:"); print_node(node->content);
      printf("\tbody:"); print_node(node->next);
    }
    /*  if (node->content) print_node(node->content);
        printf(")");*/
  }
  else if (node->type==EXP_MACRO){
    if (node->meta) printf("#<macro:%s@%08lx>",(char *) node->meta,(long) node);
    else printf("#<macro@@%08lx>",(long) node);
    /*  if (node->content) print_node(node->content);
        printf(")");*/
  }

  else if (node->type==EXP_SYMBOL) printf("%s%s",verbose?"_sym:":"",(char *) node->ptr);
  else if (node->type==EXP_STRING) printf("%s\"%s\"",verbose?"_str:":"",(char *) node->ptr);
  else if (node->type==EXP_NUMBER) printf("%s%ld",verbose?"_num:":"",(long) node->s64);
  else if (node->type==EXP_FLOAT) printf("%s%lf",verbose?"_flo:":"",node->f);
  else {
    printf("type: %d ptr: %08lx\n",node->type,(unsigned long) node->ptr);
  }
  
}

inline exp_t *make_fromstr(char *str,int length)
{
  exp_t *cur=NIL_EXP;
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
  cur->next=refexp(make_node(node));
  return cur;  
}

inline exp_t *make_integer(char *str)
{
  exp_t *cur=NIL_EXP;
  cur->type=EXP_NUMBER;
  cur->s64=atoi(str);
  //  printf("INT %ld\n",cur->d64);
  return cur;
}

inline exp_t *make_integeri(int64_t i)
{
  exp_t *cur=NIL_EXP;
  cur->type=EXP_NUMBER;
  cur->s64=i;
  //  printf("INT %ld\n",cur->d64);
  return cur;
}


inline exp_t *make_float(char *str)
{
  exp_t *cur=NIL_EXP;
  cur->type=EXP_FLOAT;
  cur->f=strtod(str,NULL);
  //  printf("INT %ld\n",cur->d64);
  return cur;
}

inline exp_t *make_floatf(expfloat f)
{
  exp_t *cur=NIL_EXP;
  cur->type=EXP_FLOAT;
  cur->f=f;
  //  printf("INT %ld\n",cur->d64);
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
  exp_t *resp=NIL_EXP;
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
  if ((e->s64=getc(stream))!= EOF) return e;
  else {
    unrefexp(e);
    return NULL;
  }
}

exp_t *dump_char(exp_t *e,FILE *stream){
  if (dumptype(stream,&e->type) <=0) return NULL;
  if (fputc(e->s64,stream) <=0) return NULL;
  return e;
}

char *load_str(char **pptr,FILE *stream) {
  size_t length;
  char *ptr;
  if ((length = loadsize_t(stream,&length))<=0) return NULL;
  ptr=*pptr=memalloc(length+1,sizeof(char));
  if (fread(ptr,1,length,stream) != length) {
    free(ptr);
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
  exp_t* lnode; // Initial List Node
  exp_t* vnode; // Val Node 
  exp_t* cnode; // Current Node

  if (x=='(') {
    vnode=reader(stream,')',0);
    lnode=make_node(vnode);
      
    if (vnode){
      if (iserror(vnode)) return vnode;
      cnode=lnode;
      while ((vnode=reader(stream,')',0))) { 
        if (iserror(vnode)) { unrefexp(lnode); return vnode;}
        cnode=cnode->next=refexp(make_node(vnode));
      }
    }
  }
  else if (x=='[') {
    vnode=reader(stream,']',0);
    lnode=vnode;
    if (vnode){
      if (iserror(vnode)) return vnode;
      cnode=make_node(vnode); //body
      lnode=make_node(make_node(make_symbol("_",1))); //header
      lnode->next=refexp(make_node(make_node(cnode)));
      lnode->type=EXP_LAMBDA;
      while ((vnode=reader(stream,']',0))) { 
        if (iserror(vnode)) { unrefexp(lnode); return vnode;} // cleaning to be done gc
        cnode=cnode->next=refexp(make_node(vnode));
      }
    }
  }
  else if (x=='\'') {
    vnode=reader(stream,0,0);
    return make_quote(vnode);
  }
  else if (x=='|') {
    vnode=reader(stream,')',2);
    lnode=make_node(vnode);
      
    if (vnode){
      if (iserror(vnode)) return vnode;
      cnode=lnode;
      while ((vnode=reader(stream,')',4))) { 
        if (iserror(vnode)) return vnode;
        cnode=cnode->next=refexp(make_node(vnode));
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


exp_t *reader(FILE *stream,unsigned char clmacro,int keepwspace){
  int x,y,z;
  token_t *token;
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
      if (clmacro==x) { if (keepwspace&4) ungetc(x,stream); return NULL; /* OK */}
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
      if ((y=getc(stream))!=EOF) { if (keepwspace&2) {token=tokenize(x);tokenadd(token,y);} else token=tokenize(y);}
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
            if ((z=getc(stream))!=EOF) { if (keepwspace&2) tokenadd(token,y); tokenadd(token,z); }
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
              if ((z=getc(stream))!=EOF) { tokenadd(token,z); continue;}
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

inline int istrue(exp_t *e){
  if (e) {
    if isatom(e) {
        if isstring(e) return ((e->ptr)?strlen(e->ptr):0);
        else if isnumber(e) return (e->s64);
        else if isfloat(e) return (e->f!=0);
        else if issymbol(e) return (strcmp(e->ptr,"nil")!=0);
      }
    else if ispair(e) {
        if (e->content||e->next)
          return 1;
      }
  }
  return 0;
}


inline exp_t *lookup(exp_t *e,env_t *env)
{
  keyval_t *ret;
  env_t *curenv=env;

  if ((ret=set_get_keyval_dict(reserved_symbol,e->ptr,NULL))) return ret->val;
  else {
    if (curenv) do {
        if ((curenv->d) &&(ret=set_get_keyval_dict(curenv->d,e->ptr,NULL))) return ret->val;
      } while ((curenv=curenv->root));
  }
  return NULL;
 
}
exp_t *updatebang(exp_t *keyv,env_t *env,exp_t *val){
  keyval_t *ret=NULL;
  exp_t *key;
  exp_t *key2;
  if (!(env->d)) env->d=create_dict();
  if (val==NULL) val=NIL_EXP;
  if issymbol(keyv) { 
      if (islambda(val) && val->meta==NULL) val->meta=(keyval_t *)strdup(keyv->ptr);
      ret=set_get_keyval_dict(env->d,keyv->ptr,val); return val;}
  else if (ispair(keyv)) { /*evaluate(keyv,env)=val*/ 
    key=car(keyv);
    if (key && issymbol(key)){
      if (strcmp(key->ptr,"car")==0)
        {
          key=evaluate(cadr(keyv),env);
          unrefexp(key->content);
          return (key->content=refexp(val));
        }
      else if (strcmp(key->ptr,"car")==0)
        {
          key=evaluate(cadr(keyv),env);
          unrefexp(key->next);
          return (key->next=refexp(val));
        }
      else {
        key=evaluate(key,env);
        if (isstring(key)){
          key2=cadr(keyv);
          if (key2 && isnumber(key2) && ischar(val))
            if ((key2->s64>=0) && (key2->s64<(int64_t)strlen(key->ptr)))
              {
                *((char*) key->ptr +key2->s64)=(unsigned char) val->s64;
                return val;
              }
            else return error(ERROR_INDEX_OUT_OF_RANGE,keyv,env,"Error index out of range");
          else return error(ERROR_NUMBER_EXPECTED,keyv,env,"Error number and char expected");
        }
      }
    }      
    else return NULL;
 
  }
  else return error(EXP_ERROR_INVALID_KEY_UPDATE,keyv,env,"Error invalid key in =");
  if (ret) return ret->val;
  else return NULL; /* ERROR? */
}

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
      vali=make_node(body);
      val=make_node(header);
      val->next=refexp(vali);
      val->type=EXP_LAMBDA;
      return val;
    }
    else return error(EXP_ERROR_BODY_NOT_LIST,e,env,"Error body is not a list");
  }
  else return error(EXP_ERROR_PARAM_NOT_LIST,e,env,"Error params is not a list");
  
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
          vali=make_node(body);
          val=make_node(header);
          val->next=refexp(vali);
          val->type=EXP_LAMBDA;
          val->meta=(keyval_t *)strdup(name->ptr);
          if (!(env->d)) env->d=create_dict();
          ret=set_get_keyval_dict(env->d,name->ptr,val);
          return val;
        }
      else return error(EXP_ERROR_BODY_NOT_LIST,e,env,"Error body is not a list");
    }
    else return error(EXP_ERROR_PARAM_NOT_LIST,e,env,"Error params is not a list");
  }
  else
    return error(EXP_ERROR_MISSING_NAME,e,env,"Error missing name or name not a lambda");
  
}

exp_t *expandmacrocmd(exp_t *e,env_t *env){
  exp_t *tmpexp;
  exp_t *tmpexp2;

  tmpexp=car(cadr(cadr(e)));
  //if (tmpexp && ispair(tmpexp)) tmpexp=evaluate(tmpexp,env);
  if (tmpexp)
    if (issymbol(tmpexp))
      if ((tmpexp2=lookup(tmpexp,env)))
        if ismacro(tmpexp2) return expandmacro(cadr(cadr(e)),tmpexp2,env);
  
  return error(ERROR_ILLEGAL_VALUE,e,env,"Error parameter not a macro");
  
}

exp_t *defmacro(exp_t *e, env_t *env)
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
        vali=make_node(body);
        val=make_node(header);
        val->next=refexp(vali);
        val->type=EXP_MACRO;
        val->meta=(keyval_t *) strdup(name->ptr);
        if (!(env->d)) env->d=create_dict();
        ret=set_get_keyval_dict(env->d,name->ptr,val);
        return val;
      }
      
      else return error(EXP_ERROR_BODY_NOT_LIST,e,env,"Error body is not a list");
    }
    else return error(EXP_ERROR_PARAM_NOT_LIST,e,env,"Error params is not a list"); 
  }
  else
    return error(EXP_ERROR_MISSING_NAME,e,env,"Error missing name or name not a lambda");
    
}


#pragma GCC diagnostic ignored "-Wunused-parameter"
exp_t *verbosecmd(exp_t *e, env_t *env)
{
  if (verbose^=1) printf("verbose on\n");
  else printf("verbose off\n");
  return NULL;
}
exp_t *quotecmd(exp_t *e, env_t *env)
{
  return cadr(e);
}
#pragma GCC diagnostic warning "-Wunused-parameter"

exp_t *ifcmd(exp_t *e, env_t *env)
{
  exp_t *tmpexp=evaluate(cadr(e),env);
  exp_t *tmpexp2;
  if iserror(tmpexp)
              return tmpexp;
  if (istrue(tmpexp)) { return evaluate(caddr(e),env); }
  else {
    if ((tmpexp=cdddr(e)))
      do {
        tmpexp2=evaluate(tmpexp->content,env);
        if ((!iserror(tmpexp2)) && (tmpexp->next)) {
          if (istrue(tmpexp2)) return evaluate(cadr(tmpexp),env);
          if (!(tmpexp=cddr(tmpexp))) return NULL;
        }
        else return tmpexp2;
      }
      while (1);
    else return NULL;
  }
}

exp_t *equalcmd(exp_t *e, env_t *env)
{
  exp_t *tmpexp=evaluate(caddr(e),env);
  exp_t *tmpkey=cadr(e);
  if (!issymbol(tmpkey)) {tmpkey = evaluate(cadr(e),env); }
  if iserror(tmpkey)
              return tmpkey;
  if iserror(tmpexp)
              return tmpexp;
  return updatebang(tmpkey,env,tmpexp);
  /* to be unrefed tmpkey in case of evaluate */ 
}

exp_t *persistcmd(exp_t *e, env_t *env)
{
  exp_t *tmpkey=cadr(e);
  if (!issymbol(tmpkey)) {tmpkey = evaluate(cadr(e),env); }
  /* to be unrefed tmpkey in case of evaluate */ 
  if iserror(tmpkey) return tmpkey;
  return set_keyval_dict_timestamp(env->d,tmpkey->ptr,gettimeusec());
}

exp_t *ispersistentcmd(exp_t *e, env_t *env)
{
  exp_t *tmpkey=cadr(e);
  if (!issymbol(tmpkey)) {tmpkey = evaluate(cadr(e),env); }
  /* to be unrefed tmpkey in case of evaluate */ 
  if iserror(tmpkey) return tmpkey;
  if (get_keyval_dict_timestamp(env->d,tmpkey->ptr)) {
      return TRUE_EXP;}
  else 
    return NIL_EXP;
}

exp_t *forgetcmd(exp_t *e, env_t *env)
{
  exp_t *tmpkey=cadr(e);
  if (!issymbol(tmpkey)) {tmpkey = evaluate(cadr(e),env); }
  /* to be unrefed tmpkey in case of evaluate */ 
  if iserror(tmpkey) return tmpkey;
  return set_keyval_dict_timestamp(env->d,tmpkey->ptr,0);
}

exp_t *savedbcmd(exp_t *e, env_t *env)
{
  env_t *cur=env;
  FILE *stream=fopen("db.dump","w");
  //if (!cur->root) dump_dict(cur->d,stream);
  while (cur->root) {
    //dump_dict(cur->d,stream);
    cur=cur->root;}
  dump_dict(cur->d,stream);
  fclose(stream);
  return e;
}



exp_t *cmpcmd(exp_t *e, env_t *env)
{
  exp_t *op=car(e);
  exp_t *v1=evaluate(cadr(e),env);
  exp_t *v2=evaluate(caddr(e),env);
  double d;
  int ret;
  if iserror(v1) return v1;
  if iserror(v2) return v2;
  if ((isnumber(v1)||isfloat(v1))&&(isnumber(v2)||isfloat(v2))){
    d=(isnumber(v1)?v1->s64:v1->f)-(isnumber(v2)?v2->s64:v2->f);
  } 
  else if (isstring(v1)&&isstring(v2)) {
    d=strcmp(v1->ptr,v2->ptr);
  }
  else if (ischar(v1)&&ischar(v2)) {
    d=v1->s64-v2->s64;
  }
  else return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in compare operation");
  if (strcmp(op->ptr,"<")==0) ret=(d<0);
  else if (strcmp(op->ptr,">")==0) ret=(d>0);
  else if (strcmp(op->ptr,"<=")==0) ret=(d<=0);
  else if (strcmp(op->ptr,">=")==0) ret=(d>=0);
  else return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal operand in compare operation");
  return (ret?TRUE_EXP:NIL_EXP);
}



exp_t *pluscmd(exp_t *e, env_t *env)
{
  int sum_i=0;
  expfloat sum_f=0;
  exp_t *c=cdr(e);
  exp_t *v;
  exp_t *v1=NULL;
   
  do {
    if (c &&(v1=c->content))
      {
        if ispair(v1) v=evaluate(v1,env);
        else if issymbol(v1) v=evaluate(v1,env);
        else v=v1; 
        if iserror(v) return v;
        if (sum_f){
          if isnumber(v) sum_f+=v->s64;
          else if isfloat(v) sum_f+=v->f;
          else return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation");
        }
        else {
          if isnumber(v) sum_i+=v->s64;
          else if isfloat(v) { sum_f = v->f + sum_i; sum_i=0;}
          else return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation");

        }
      }
  } while (c &&(c=c->next));
  return (sum_f?make_floatf(sum_f):make_integeri(sum_i));
}

exp_t *multiplycmd(exp_t *e, env_t *env)
{
  int sum_i=1;
  expfloat sum_f=1;
  exp_t *c=cdr(e);
  exp_t *v;
  exp_t *v1=NULL;
   
  do {
    if (c &&(v1=c->content))
      {
        if ispair(v1) v=evaluate(v1,env);
        else if issymbol(v1) v=evaluate(v1,env);
        else v=v1;
        if iserror(v) return v;
        if (sum_f!=1){
          if isnumber(v) sum_f*=v->s64;
          else if isfloat(v) sum_f*=v->f;
          else return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation");

        }
        else {
          if isnumber(v) sum_i=((sum_i!=1)?sum_i*=v->s64:v->s64);
          else if isfloat(v) { sum_f = v->f; if (sum_i!=1) {sum_f=sum_f*sum_i; sum_i=1;}}
          else return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); 

        }
      }
  } while (c &&(c=c->next));
  return ((sum_f!=1)?make_floatf(sum_f):make_integeri(sum_i));
}

exp_t *minuscmd(exp_t *e, env_t *env)
{
  int sum_i=0;
  expfloat sum_f=0;
  exp_t *c=cdr(e);
  exp_t *v;
  exp_t *v1=NULL;
  int i=0;
   
  do {
    if (c &&(v1=c->content))
      {
        i++;
        if ispair(v1) v=evaluate(v1,env);
        else if issymbol(v1) v=evaluate(v1,env);
        else v=v1; 
        if iserror(v) return v;
        if (sum_f){
          if isnumber(v) sum_f-=v->s64;
          else if isfloat(v) sum_f-=v->f;
          else return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); /*ERROR*/
        }
        else {
          if isnumber(v) {if (sum_i) sum_i-=v->s64; else sum_i=v->s64;}
          else if isfloat(v) {if (sum_i) { sum_f = sum_i - (v->f);} else sum_f=v->f; sum_i=0;}
          else return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); /*ERROR*/
        }
      }
  } while (c &&(c=c->next));
  if (i==1) { if (sum_f) sum_f=-sum_f; else sum_i=-sum_i; }
  return (sum_f?make_floatf(sum_f):make_integeri(sum_i));
}

exp_t *dividecmd(exp_t *e, env_t *env)
{
  int sum_i=0;
  expfloat sum_f=0;
  exp_t *c=cdr(e);
  exp_t *v;
  exp_t *v1=NULL;
  int i=0;
   
  do {
    if (c &&(v1=c->content))
      {
        i++;
        if ispair(v1) v=evaluate(v1,env);
        else if issymbol(v1) v=evaluate(v1,env);
        else v=v1; 
        if iserror(v) return v;
        if (i>1) { 
          if (isnumber(v) && (v->s64==0)) return error(ERROR_DIV_BY0,e,env,"Illegal Division by 0");
          else if (isfloat(v) && (v->f==0)) return error(ERROR_DIV_BY0,e,env,"Illegal Division by 0");
        }
        if (sum_f){
          if isnumber(v) sum_f/=v->s64;
          else if isfloat(v) sum_f/=v->f;
          else return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation");
        }
        else {
          if isnumber(v) {if (sum_i) sum_i/=v->s64; else sum_i=v->s64;}
          else if isfloat(v) {if (sum_i) { sum_f = sum_i /(v->f);} else sum_f=(v->f); sum_i=0;}
          else return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation");
        }
      }
  } while (c &&(c=c->next));
  if (i==1) { if (sum_f) sum_f=1/sum_f; else if (sum_i) sum_i=1/sum_i; else return error(ERROR_DIV_BY0,e,env,"Illegal Division by 0");}
  return (sum_f?make_floatf(sum_f):make_integeri(sum_i));
}

exp_t *sqrtcmd(exp_t *e, env_t *env){
  exp_t *v;
  if ((v=e->next))
    v=evaluate(v->content,env);
  if iserror(v) return v;
  if (isfloat(v)) 
    return make_floatf(sqrt(v->f));
  else if (isnumber(v))
    return make_floatf(sqrt(v->s64));
  return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation");
}

exp_t *expcmd(exp_t *e, env_t *env){
  exp_t *v;
  if ((v=e->next))
    v=evaluate(v->content,env);
  if iserror(v) return v;
  if (isfloat(v)) 
    return make_floatf(exp(v->f));
  else if (isnumber(v))
    return make_floatf(exp(v->s64));
  return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation");
}

exp_t *exptcmd(exp_t *e, env_t *env){
  exp_t *v;
  exp_t *v2;
  if ((v=e->next))
    if ((v2=v->next))
      {
        v=evaluate(v->content,env);
        if iserror(v) return v;
        v2=evaluate(v2->content,env);
        if iserror(v2) return v2;
      }
  if ( (isfloat(v)||isnumber(v)) && (isfloat(v2)||isnumber(v2)))
    return make_floatf(pow(isfloat(v)?v->f:v->s64,isfloat(v2)?v2->f:v2->s64));
  return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); /*ERROR*/
}

exp_t *prcmd(exp_t *e, env_t *env){
  exp_t *v=e;
  exp_t *val;
  while ((v=v->next)){
    val=evaluate(v->content,env);
    if iserror(val) return val;
    if (val && isstring(val)) printf("%s",(char*)val->ptr);
    else print_node(val);
  }
  return NULL;
    
}

exp_t *prncmd(exp_t *e, env_t *env){
  exp_t *ret;
  ret=prcmd(e,env);
  printf("\n");
  return ret;
}

exp_t *oddcmd(exp_t *e, env_t *env){
  if (e->next && isnumber(e->next->content)) return (e->next->content->s64&1?TRUE_EXP:NIL_EXP);
  return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in operation"); 
}

exp_t *docmd(exp_t *e, env_t *env){
  exp_t *cur=cdr(e);
  exp_t *ret;
  do
    {
      ret=evaluate(car(cur),env);
    } while ((cur=cdr(cur)) && !(ret && iserror(ret)));
  if (ret && iserror(ret))
    return ret;
  else return NIL_EXP;
}

exp_t *whencmd(exp_t *e, env_t *env){
  exp_t *val=cadr(e);
  exp_t *cur=cddr(e);
  exp_t *ret=evaluate(val,env);
  if iserror(ret) return ret;
  if (istrue(ret))
    do ret=evaluate(car(cur),env); while ((cur=cdr(cur)) && !(ret && iserror(ret)));
  if (ret && iserror(ret))
    return ret;
  else return NIL_EXP;
}

exp_t *whilecmd(exp_t *e, env_t *env){
  exp_t *val=cadr(e);
  exp_t *cur=cddr(e);
  exp_t *curi=cur;
  exp_t *ret=NULL;
  while (istrue(ret=evaluate(val,env))&&(!iserror(ret)))
    {
      cur=curi;
      do ret=evaluate(car(cur),env); while ((cur=cdr(cur)) && !(ret && iserror(ret)));
    }
  if (ret && iserror(ret))
    return ret;
  else return NIL_EXP;
}

exp_t *repeatcmd(exp_t *e, env_t *env){
  exp_t *val=evaluate(cadr(e),env);
  exp_t *cur=cddr(e);
  exp_t *curi=cur;
  exp_t *ret=NULL;
  int64_t counter;
  if iserror(val) return val;
  if (isnumber(val)) counter=val->s64;
  else return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value for repeat counter"); 
  while (counter-- >0)
    {
      cur=curi;
      do ret=evaluate(car(cur),env); while ((cur=cdr(cur)) && !(ret && iserror(ret)));
    }
  if (ret && iserror(ret))
    return ret;
  else return NIL_EXP;
}

exp_t *andcmd(exp_t *e, env_t *env){
  exp_t *cur=cdr(e);
  exp_t *ret;
  do
    {
      ret=evaluate(car(cur),env);
      if iserror(ret) return ret;
    } while (istrue(ret) && (cur=cdr(cur)));
  return ret;
}

exp_t *orcmd(exp_t *e, env_t *env){
  exp_t *cur=cdr(e);
  exp_t *ret;
  do {
    ret=evaluate(car(cur),env);
    if iserror(ret) return ret;
    if (istrue(ret)) break;
  } while ((cur=cdr(cur)));
  return ret;
}

exp_t *nocmd(exp_t *e, env_t *env){
  exp_t *cur=cdr(e);
  exp_t *tmpexp=evaluate(car(cur),env);
  if iserror(tmpexp) return tmpexp;
  if (istrue(tmpexp)) return NIL_EXP;
  else return TRUE_EXP;
}


int isequal(exp_t *cur1, exp_t *cur2)
{
  int ret=0;
  if (cur1 && cur2) {
    if (cur1->type == cur2->type){
      if (isnumber(cur1)) ret=(cur1->s64==cur2->s64);
      else if (isfloat(cur1)) ret=(cur1->f==cur2->f);
      else if (issymbol(cur1) || isstring(cur1)) ret=(strcmp(cur1->ptr,cur2->ptr)==0);
      else if (ischar(cur1)) ret=(cur1->s64==cur2->s64);
      else if (iserror(cur1)) ret=(cur1->s64==cur2->s64);
      else ret = (cur1 == cur2);
    }
  }
  else ret = (cur1 == cur2);
  return ret;
}

int isoequal(exp_t *cur1,exp_t *cur2){
  int ret=0;
  exp_t *cur1n;
  exp_t *cur2n;

  if (cur1 && cur2) {
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
  }
  else ret = (cur1 == cur2);
  return ret;
}

exp_t *iscmd(exp_t *e, env_t *env){
  exp_t *cur1=evaluate(cadr(e),env);
  if iserror(cur1) return cur1;
  exp_t *cur2=evaluate(caddr(e),env);
  if iserror(cur2) return cur2;
  return (isequal(cur1,cur2)?TRUE_EXP:NIL_EXP);
}

exp_t *isocmd(exp_t *e, env_t *env){
  exp_t *cur1=evaluate(cadr(e),env);
  if iserror(cur1) return cur1;
  exp_t *cur2=evaluate(caddr(e),env);
  if iserror(cur2) return cur2;
  return (isoequal(cur1,cur2)?TRUE_EXP:NIL_EXP);
}


exp_t *incmd(exp_t *e, env_t *env){
  exp_t *cur=cdr(e);
  exp_t *val=evaluate(cadr(e),env);
  exp_t *val2;
  
  if iserror(val) return val;
  int ret=0;
  while ((cur=cdr(cur)))
    {
      val2=evaluate(car(cur),env);  
      if iserror(val2) return val2;
      if ((ret=isoequal(val,val2))) break;
    }
  
  return (ret?TRUE_EXP:NIL_EXP);
}

exp_t *casecmd(exp_t *e, env_t *env){
  exp_t *cur=cdr(e);
  exp_t *val=evaluate(cadr(e),env);
  if iserror(val) return val;
  exp_t *ret=NULL;
  while ((cur=cdr(cur)))
    if (cur->next) 
      if (isequal(val,car(cur))) { ret=cadr(cur); break;}
      else cur=cdr(cur);
    else ret= car(cur);
  return evaluate(ret,env);
}



exp_t *forcmd(exp_t *e,env_t *env){
  env_t *newenv=make_env(env);
  exp_t *ret=NULL;
  exp_t *curvar;
  exp_t *curval;
  exp_t *curin;
  exp_t *lastidx;
  exp_t *retval;
  keyval_t *keyv;

  if ((curvar=e->next)) {
    if ((curval=curvar->next)){
      if (!(newenv->d)) newenv->d=create_dict();
      
      if (issymbol(curvar->content)) {
        if ((retval=evaluate(curval->content,env))==NULL) retval=NIL_EXP;
        if (isnumber(retval)) {
          if (!curval->next) lastidx=NIL_EXP;
          if (curval->next && (lastidx=evaluate(curval->next->content,env))==NULL) lastidx=NIL_EXP;
          if (isnumber(lastidx)) {
            keyv=set_get_keyval_dict(newenv->d,curvar->content->ptr,retval);
            curin=curval->next->next;
          } 
          else 
            { 
              if iserror(lastidx) ret=lastidx;
              else ret=error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value (not integer) in for counter");
              goto error;
            }
        }
        else { 
          if iserror(retval) ret=retval;
          else ret=error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value (not integer) in for counter"); 
          goto error;
        }
        
      }
      else { 
        ret=error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in for"); 
        goto error;
      }
      int idx=lastidx->s64+1;
      /*if (idx>100){
        curval=curin;
        curin=NIL_EXP;
        curvar=curin;
        while (curval) {
        curvar->content=refexp(optimize(curval->content,env)); curvar=curvar->next=NIL_EXP; curval=curval->next;
        }
        }*/
      while (retval->s64<idx)
        {
          curval=curin;
          while (curval) { 
            ret=evaluate(curval->content,newenv);
            if iserror(ret) return ret;
            unrefexp(ret); curval=curval->next;}
          retval->s64++;
        }
      destroy_env(newenv);
      return ret;
      
      
    }
  }
 error:
  //cleaning to be made unref exp ..
  destroy_env(newenv);
  if (ret) return ret;
  else return error(ERROR_MISSING_PARAMETER,e,env,"Missing parameter in for");
}


exp_t *eachcmd(exp_t *e,env_t *env){
  env_t *newenv=make_env(env);
  exp_t *curvar;
  exp_t *curval;
  exp_t *curin;
  exp_t *retval;
  exp_t *tmpexp;
  keyval_t *keyv;

  if ((curvar=e->next)) {
    if ((curval=curvar->next)){
      if (!(newenv->d)) newenv->d=create_dict();
      
      if (issymbol(curvar->content)) {
        if ((retval=evaluate(curval->content,env))==NULL) retval=NIL_EXP;
        if (ispair(retval)) {
          curin=curval->next;
          while (retval) {
            keyv=set_get_keyval_dict(newenv->d,curvar->content->ptr,car(retval));
            curval=curin;
            while (curval) { tmpexp=evaluate(curval->content,newenv);
              if iserror(tmpexp) return tmpexp;
              unrefexp(tmpexp); curval=curval->next;}
            retval=retval->next;
          }
          destroy_env(newenv);
          return NULL;
        } 
        else {
          destroy_env(newenv);
          if iserror(retval) return retval;
          return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value (not list) in each"); 
        }
        
      }
      else { 
        destroy_env(newenv);
        return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in each"); 
      }
    }
  }
  destroy_env(newenv);
  return error(ERROR_MISSING_PARAMETER,e,env,"Missing parameter in each");
}

#pragma GCC diagnostic ignored "-Wunused-parameter"
exp_t *timecmd(exp_t *e,env_t *env){
  return make_integeri(gettimeusec());
}
#pragma GCC diagnostic warning "-Wunused-parameter"


exp_t *conscmd(exp_t *e, env_t *env){
  exp_t *a=evaluate(cadr(e),env);
  exp_t *b=evaluate(caddr(e),env);
  if iserror(a) return a;
  if iserror(b) return b;
  exp_t *ret=make_node(a);
  if (istrue(b)) ret->next=refexp(b);
  else ret->next=NULL;
  return ret;

}

exp_t *cdrcmd(exp_t *e, env_t *env){
  exp_t *tmpexp=evaluate(cadr(e),env);
  if iserror(tmpexp) return tmpexp;
  return cdr(tmpexp);
}

exp_t *carcmd(exp_t *e, env_t *env){
  exp_t *tmpexp=evaluate(cadr(e),env);
  if iserror(tmpexp) return tmpexp;
  return car(tmpexp);
}

exp_t *listcmd(exp_t *e, env_t *env){
  exp_t *a=cdr(e);
  exp_t *tmpexp=evaluate(car(a),env);
  if iserror(tmpexp) return tmpexp;
  exp_t *ret=make_node(tmpexp);
  exp_t *cur=ret;
  while ((a=a->next))
    {
      tmpexp=evaluate(car(a),env);
      if iserror(tmpexp) return tmpexp;
      cur=cur->next=make_node(tmpexp);
    }
  return ret;
}

exp_t *evalcmd(exp_t *e, env_t *env){
  return evaluate(evaluate(cadr(e),env),env);
}

exp_t *letcmd(exp_t *e,env_t *env){
  env_t *newenv=make_env(env);
  exp_t *ret;
  exp_t *curvar;
  exp_t *curval;
  exp_t *retval;
  keyval_t *keyv;

  if ((curvar=e->next)) {
    if ((curval=curvar->next)){
      if (!(newenv->d)) newenv->d=create_dict();
      
      if (issymbol(curvar->content)) {
        if ((retval=evaluate(curval->content,env))==NULL) retval=NIL_EXP;
        if iserror(retval) return retval;
        keyv=set_get_keyval_dict(newenv->d,curvar->content->ptr,retval);
      }
      else { 
        destroy_env(newenv);
        return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in let"); 
      }
      if (curval->next)
        {
          ret=evaluate(curval->next->content,newenv);
          destroy_env(newenv);
          return ret;
        }
      
    }
  }
  destroy_env(newenv);
  return error(ERROR_MISSING_PARAMETER,e,env,"Missing parameter in let"); /* MISSING PARAMETER*/
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
      destroy_env(newenv);
      return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in with"); 
    }
    if ((curex=curvar->next)){
      curvar=curvar->content;
      if ((curval=curvar->next)){
        if (!(newenv->d)) newenv->d=create_dict();
        
        while (curvar && curval) {
          if (issymbol(curvar->content)) { 
            ret=evaluate(curval->content,env);
            if iserror(ret) return ret;
            keyv=set_get_keyval_dict(newenv->d,curvar->content->ptr,ret); 
          }
          
          else return error(ERROR_ILLEGAL_VALUE,e,env,"Illegal value in with"); 
          curvar=curval->next;
          if (curvar) curval=curvar->next;
          if (!curval) {
            destroy_env(newenv);
            return error(ERROR_MISSING_PARAMETER,e,env,"Missing parameter in with");
  
          }
        }

        ret=evaluate(curex->content,newenv);
        destroy_env(newenv);
        return ret;
      }
    }
  }
  destroy_env(newenv);
  return error(ERROR_MISSING_PARAMETER,e,env,"Missing parameter in with");
}



exp_t *var2env(exp_t *e,exp_t *var, exp_t *val,env_t *env,int evalexp)
{
  exp_t *curvar=var;
  exp_t *retvar;
  exp_t *curval=val;
  keyval_t *keyv;

  while (curvar){
    if ((curval)) {
      if (!(env->d)) env->d=create_dict();
      if ((retvar = (evalexp?evaluate(curval->content,env->root):curval->content))) {
        if (evalexp && iserror(retvar)) return retvar;
      }
      else retvar= NIL_EXP;
      if (issymbol(curvar->content)) keyv=set_get_keyval_dict(env->d,curvar->content->ptr,retvar);
      else return NULL;
      
    }
    else return error(ERROR_MISSING_PARAMETER,e,env,"Missing parameter in macro or function invoke");
    curval=curval->next;
    curvar=curvar->next;
  }
  return NULL;

}
exp_t *invoke(exp_t *e, exp_t *fn, env_t *env) 
{
  /* e->content = fn name,
     e->next->content-> 
     fn->content = var names list
     fn-> next-> content =body*/
  //  int ncall=0;
  env_t *newenv=env;
  exp_t *ret;
  exp_t *body=fn->next->content;

  if (!(e->content->flags&FLAG_TAILREC)) {
    while ((newenv->root) && (newenv->callingfnc==NULL)) newenv=newenv->root;
    if ((newenv->callingfnc) && (strcmp(e->content->ptr,env->callingfnc->ptr)==0))
      {
        exp_t *cur=ret=make_node(make_symbol(e->content->ptr,strlen(e->content->ptr)));
        ret->content->flags=FLAG_TAILREC;
        exp_t *curv=e;
        while ((curv=curv->next))
          {
            cur=cur->next=make_node(evaluate(curv->content,env));
          }
        return ret;
        
      }
  }
  
 tailrec:
  if (verbose) { printf("invoke:"); print_node(e); printf("\n");}
  
  
  newenv=make_env(env);
  newenv->callingfnc=refexp(e);
  if ((ret=var2env(e,fn->content,e->next,newenv,true))) 
    {
      return ret;
    }
  
  do
    ret=evaluate(body->content,newenv);
  while ((body=body->next));
  destroy_env(newenv);
  /* if ret is lazy... loopback*/
  if (ret && issymbol(ret) && (ret->flags&FLAG_TAILREC)) 
    {
      e=ret;
      if (verbose) printf("Tail recursive invoke\n");
      goto tailrec;
    }
  return ret;
}

#pragma GCC diagnostic ignored "-Wunused-parameter"
exp_t *expandmacro(exp_t *e, exp_t *fn, env_t *env){
  env_t *newenv=make_env(NULL); // NULL instead of env
  exp_t *ret;

  if ((ret=var2env(e,fn->content,e->next,newenv,false)))
    return ret;
  //  printf("\nto be evaluated: ");
  //print_node(fn->next->content);
  ret = evaluate(fn->next->content,newenv);
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
  ret=evaluate(ret,env);
 
  return ret;
}

#pragma GCC diagnostic ignored "-Wunused-parameter"
exp_t *optimize(exp_t *e,env_t *env)
{
  /* TO DO UN REF VARS*/
  exp_t *tmpexp;
  exp_t *tmpexp2;

  if (e==NULL) return NULL;
  if isatom(e)  {
      return e;
    }
  else if ispair(e) {
      tmpexp=car(e);
      if (tmpexp) {
        if (issymbol(tmpexp)) {
          if ((tmpexp2=lookup(tmpexp,NULL))) { 
            if isinternal(tmpexp2) {
                tmpexp2->next=refexp(e->next);
                return tmpexp2;
              }
            else if islambda(tmpexp2) {
                return e;
              }
            else if ismacro(tmpexp2) return e;
            else return e;
          }
          return e; // what is happening here?
        }
      }
    }
  return e;

}
#pragma GCC diagnostic warning "-Wunused-parameter"

exp_t *evaluate2(exp_t *e, env_t *env)
{
  exp_t *tmpexp;
  if (e!=NULL) {
    switch (e->type){
    case EXP_SYMBOL:
      if (((char*)e->ptr)[0] == ':') return e;
      if ((tmpexp=lookup(e,env))) return tmpexp;
      else return error(ERROR_UNBOUND_VARIABLE,e,env,"Error unbound variable %s",e->ptr);
    case EXP_NUMBER:
    case EXP_FLOAT:
    case EXP_STRING:
    case EXP_CHAR:
    case EXP_BOOLEAN:
    case EXP_VECTOR:
    case EXP_ERROR:
      return e;
    case EXP_PAIR:
	
    default:
      return e;
    }
  }
  else return NULL;
  return NULL;
}

exp_t *evaluate(exp_t *e,env_t *env)
{
  /* TO DO UN REF VARS*/
  exp_t *tmpexp;
  exp_t *tmpexp2;

  if (e==NULL) return NULL;
  if isatom(e)  {
      if issymbol(e) {
          if (((char*)e->ptr)[0] == ':') return e; // e is a keyword
          if ((tmpexp=lookup(e,env))) return tmpexp;
          else 
            return error(ERROR_UNBOUND_VARIABLE,e,env,"Error unbound variable %s",e->ptr);
        }
      else return e; // Number? String? Char? Boolean? Vector?
    }
  else if ispair(e) {
      tmpexp=car(e);
      if (tmpexp && ispair(tmpexp)) tmpexp=evaluate(tmpexp,env);
      if (tmpexp) {
        if isinternal(tmpexp)  {
            return tmpexp->fnc(e,env);
          }
        if (issymbol(tmpexp)) {
          if (((char*)tmpexp->ptr)[0] == ':') return error(ERROR_ILLEGAL_VALUE,e,env,"Error keyword %s can not be used as function",tmpexp->ptr); // e is a keyword
          if ((tmpexp2=lookup(tmpexp,env))) { 
            if isinternal(tmpexp2) {
                return tmpexp2->fnc(e,env);
              }
            else if islambda(tmpexp2) {
                return invoke(e,tmpexp2,env);
              }
            else if ismacro(tmpexp2) return invokemacro(e,tmpexp2,env);
            else if ispair(tmpexp2) { return tmpexp2 ; }
            else return tmpexp2;
          }
          else 
            return error(ERROR_UNBOUND_VARIABLE,e,env,"Error unbound variable %s",tmpexp->ptr);
          return e; // what is happening here?
        }
        else if (isstring(tmpexp)) {
          tmpexp2=evaluate(cadr(e),env);
          if (isnumber(tmpexp2)){
            if ((tmpexp2->s64>=0)&&(tmpexp2->s64<(int64_t)strlen(tmpexp->ptr))){
              return make_char(*((char *) tmpexp->ptr+tmpexp2->s64));
            }
            else return error(ERROR_INDEX_OUT_OF_RANGE,e,env,"Error index out of range");
          }
          else return error(ERROR_NUMBER_EXPECTED,e,env,"Error number expected");
        }
        else if (islambda(tmpexp)) return invoke(e,tmpexp,env);
        else if (ismacro(tmpexp)) return invokemacro(e,tmpexp,env);  
      }
      else if (tmpexp==NULL) return NULL;
      
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

  exp_tfuncList[EXP_CHAR]=(exp_tfunc*)memalloc(1,sizeof(exp_tfunc));
  exp_tfuncList[EXP_CHAR]->load=load_char;
  exp_tfuncList[EXP_CHAR]->dump=dump_char;
  exp_tfuncList[EXP_STRING]=(exp_tfunc*)memalloc(1,sizeof(exp_tfunc));
  exp_tfuncList[EXP_STRING]->load=load_string;
  exp_tfuncList[EXP_STRING]->dump=dump_string;


  reserved_symbol=create_dict();
  set_get_keyval_dict(reserved_symbol,"nil",NIL_EXP);
  set_get_keyval_dict(reserved_symbol,"t",TRUE_EXP);
  
  int N=sizeof(lispProcList)/sizeof(lispProc);
  int i;
  for (i = 0; i < N; ++i) {
    set_get_keyval_dict(reserved_symbol,lispProcList[i].name,make_internal(lispProcList[i].cmd));
  }

  if (argc==2){
    if ((stream=fopen(argv[1],"r"))){
      evaluatingfile=1;
    }
    else
      { printf("Error opening %s\n",argv[1]); exit(0);}
  }
  else stream=stdin;
  exp_t* stre=refexp(make_string(strdict,strlen(strdict)));
  exp_t* strf=refexp(make_string(strdict,strlen(strdict)));
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
  unrefexp(stre);
  unrefexp(strf);
  
  while (1){
    if (!evaluatingfile) printf("ALCOVE>");
    stre=refexp(reader(stream,0,0));
    if (iserror(stre) && (stre->flags == EXP_ERROR_PARSING_EOF) && evaluatingfile) {exit(0);}
    if (verbose) {print_node(stre);printf("\n");}
    if (stre && (stre->type==EXP_SYMBOL) && (strcmp(stre->ptr,"quit")==0)) break;
    strf=refexp(evaluate(stre,global));
    if (strf) {print_node(strf);} else printf("nil");
    unrefexp(stre);
    if (strf) unrefexp(strf);
    printf("\n");
  }
  unrefexp(stre);
  destroy_dict(dict);
  
}
