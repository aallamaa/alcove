
/* Structure definition */

enum {
  EXP_SYMBOL=1, EXP_NUMBER, EXP_FLOAT, EXP_STRING, EXP_CHAR, EXP_BOOLEAN, EXP_VECTOR, EXP_ERROR,
  /* ALL ATOMS ARE ABOVE THIS COMMENT */
  /*EXP_QUOTE,*/
  EXP_PAIR, EXP_LAMBDA,EXP_INTERNAL,EXP_MACRO,EXO_MACROINTERNAL,
  /* ALL EXP BEYOND THIS COMMENT ARE "CIRCULAR" MEANING THEY POINT TO A PREVIOUSLY MEM ALLOCATED EXP */
  EXP_TREE,
  EXP_PAIR_CIRCULAR,
} exptype_t;

enum {
  EXP_ERROR_PARSING_MACROCHAR=1,EXP_ERROR_PARSING_ILLEGAL_CHAR,EXP_ERROR_PARSING_EOF,
  EXP_ERROR_INVALID_KEY_UPDATE=256,EXP_ERROR_BODY_NOT_LIST,EXP_ERROR_PARAM_NOT_LIST,EXP_ERROR_MISSING_NAME,ERROR_ILLEGAL_VALUE,ERROR_DIV_BY0, ERROR_MISSING_PARAMETER,ERROR_UNBOUND_VARIABLE,ERROR_NUMBER_EXPECTED,ERROR_INDEX_OUT_OF_RANGE,
} exp_error_t;

#define isatom(e) (e->type<=EXP_VECTOR)
#define issymbol(e) (e->type == EXP_SYMBOL)
#define isnumber(e) (e->type == EXP_NUMBER)
#define isfloat(e) (e->type == EXP_FLOAT)
#define isstring(e) (e->type == EXP_STRING)
#define ischar(e) (e->type == EXP_CHAR)
#define ispair(e) (e->type == EXP_PAIR)
#define islambda(e) (e->type == EXP_LAMBDA)
#define isinternal(e) (e->type == EXP_INTERNAL)
#define ismacro(e) (e->type == EXP_MACRO)
#define isinternal(e) (e->type == EXP_INTERNAL)
#define iserror(e) (e->type == EXP_ERROR)
#define car(e) ((e&&(e->type==EXP_PAIR))?(e->content):NULL)
#define cdr(e) ((e&&(e->type==EXP_PAIR))?(e->next):NULL)
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

typedef struct exp_t {
  unsigned short int flags; /* bit 0 for disk persistance */
  unsigned short int type; // exp type cf exptype_t enum list
  int nref; // Garbage collector number of reference counter
  union {
    struct exp_t *content;
    void *ptr;
    int64_t s64;
    expfloat f;
    lispCmd *fnc;
  } ;
  struct keyval_t
  /*char*/ *meta;
  struct exp_t *next;
} exp_t;


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

#define DICT_KVHT_INITIAL_SIZE 8

typedef struct dict_t {
	void *meta;
	int pos;
	kvht_t ht[2];
} dict_t;


typedef struct env_t {
  struct env_t *root;
  jmp_buf jmp_env;
  dict_t *d;
} env_t;



typedef struct lispProc {
  char *name;
  int arity;
  int flags; /*1 is macro*/
  int level; /* security level */
  lispCmd *cmd;
} lispProc;

/* Functions declaration */

/* memory management */
exp_t *refexp(exp_t *e);
void *memalloc(size_t count, size_t size);
int unrefexp(exp_t *e);

/* env management*/
env_t * make_env(env_t *rootenv);
void *destroy_env(env_t *env);

/* dict management */
void * del_keyval_dict(dict_t* d, char *key);
dict_t * create_dict();
int destroy_dict(dict_t *d);
static void init_kvht(kvht_t *kvht);
unsigned int bernstein_hash(unsigned char *key, int len);
unsigned int bernstein_uhash(unsigned char *key, int len);
keyval_t * set_get_keyval_dict(dict_t* d, char *key, exp_t *val);


/* lisp */
exp_t *error(int errnum,exp_t *id,env_t *env,char *err_message, ...);
exp_t *make_nil();
exp_t *make_char(unsigned char c);
exp_t *make_node(exp_t *node);
exp_t *make_internal(lispCmd *cmd);
exp_t *make_tree(exp_t *root,exp_t *node1);
exp_t *make_fromstr(char *str,int length);
exp_t *make_string(char *str,int length);
exp_t *make_symbol(char *str,int length);
exp_t *make_quote(exp_t *node);
exp_t *make_integer(char *str,int length);
exp_t *make_integeri(int64_t *i);
exp_t *make_float(char *str,int length);
exp_t *make_floatf(expfloat f);
exp_t *make_atom(char *str,int length);
exp_t *callmacrochar(FILE *stream,unsigned char x);
exp_t *lookup(exp_t *e,env_t *env);
exp_t *updatebang(exp_t *key,env_t *env,exp_t *val);
exp_t *reader(FILE *stream,unsigned char clmacro,int keepwspace);
exp_t *optimize(exp_t *e,env_t *env);
exp_t *evaluate(exp_t *e,env_t *env);
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
exp_t *quotecmd(exp_t *e, env_t *env);
exp_t *ifcmd(exp_t *e, env_t *env);
exp_t *equalcmd(exp_t *e, env_t *env);
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

/* lisp macro */
exp_t *defcmd(exp_t *e, env_t *env);
exp_t *expandmacrocmd(exp_t *e,env_t *env);
exp_t *defmacro(exp_t *e, env_t *env);
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

#define ISWHITESPACE 1
#define ISSINGLEESCAPE 2
#define ISMULTIPLEESCAPE 4
#define ISTERMMACRO 8
#define ISNTERMMACRO 16
#define ISCONSTITUENT 32
#define ISDIGIT 64
unsigned char chrmap[]={ 
 0 /* NUL Null character */,
 0 /* SOH Start of Header */,
 0 /* STX Start of Text */,
 0 /* ETX End of Text */,
 0 /* EOT End of Transmission */,
 0 /* ENQ Enquiry */,
 0 /* ACK Acknowledgment */,
 0 /* BEL Bell */,
 0 /* BS Backspace[d][e] */,
 1 /* HT Horizontal Tab[f] */,
 1 /* LF Line feed */,
 1 /* VT Vertical Tab */,
 1 /* FF Form feed */,
 1 /* CR Carriage return[g] */,
 0 /* SO Shift Out */,
 0 /* SI Shift In */,
 0 /* DLE Data Link Escape */,
 0 /* DC1 Device Control 1 (oft. XON) */,
 0 /* DC2 Device Control 2 */,
 0 /* DC3 Device Control 3 (oft. XOFF) */,
 0 /* DC4 Device Control 4 */,
 0 /* NAK Negative Acknowledgement */,
 0 /* SYN Synchronous idle */,
 0 /* ETB End of Transmission Block */,
 0 /* CAN Cancel */,
 0 /* EM End of Medium */,
 0 /* SUB Substitute */,
 0 /* ESC Escape[i] */,
 0 /* FS File Separator */,
 0 /* GS Group Separator */,
 0 /* RS Record Separator */,
 0 /* US Unit Separator */,
 1 /* SPACE Space */,
32 /* ! */,	 4 /* " */,	16 /* # */,	32 /* $ */,	32 /* % */,	32 /* & */,	 8 /* ' */,
 8 /* ( */,	 8 /* ) */,	32 /* * */,	32 /* + */,	 8 /* , */,	32 /* - */,	32 /* . */,
32 /* / */,	96 /* 0 */,	96 /* 1 */,	96 /* 2 */,	96 /* 3 */,	96 /* 4 */,	96 /* 5 */,
96 /* 6 */,	96 /* 7 */,	96 /* 8 */,	96 /* 9 */,	32 /* : */,	 8 /* ; */,	32 /* < */,
32 /* = */,	32 /* > */,	32 /* ? */,	32 /* @ */,	32 /* A */,	32 /* B */,	32 /* C */,
32 /* D */,	32 /* E */,	32 /* F */,	32 /* G */,	32 /* H */,	32 /* I */,	32 /* J */,
32 /* K */,	32 /* L */,	32 /* M */,	32 /* N */,	32 /* O */,	32 /* P */,	32 /* Q */,
32 /* R */,	32 /* S */,	32 /* T */,	32 /* U */,	32 /* V */,	32 /* W */,	32 /* X */,
32 /* Y */,	32 /* Z */,	 8 /* [ */,	 2 /* \ */,	 8 /* ] */,	32 /* ^ */,	32 /* _ */,
 8 /* ` */,	32 /* a */,	32 /* b */,	32 /* c */,	32 /* d */,	32 /* e */,	32 /* f */,
32 /* g */,	32 /* h */,	32 /* i */,	32 /* j */,	32 /* k */,	32 /* l */,	32 /* m */,
32 /* n */,	32 /* o */,	32 /* p */,	32 /* q */,	32 /* r */,	32 /* s */,	32 /* t */,
32 /* u */,	32 /* v */,	32 /* w */,	32 /* x */,	32 /* y */,	32 /* z */,	 8 /* { */,
 8 /* | */,	 8 /* } */,	32 /* ~ */,	 0 /*  */,	 0 /* 128 */,	 0 /* 129 */,
 0 /* 130 */,	 0 /* 131 */,	 0 /* 132 */,	 0 /* 133 */,	 0 /* 134 */,	 0 /* 135 */,
 0 /* 136 */,	 0 /* 137 */,	 0 /* 138 */,	 0 /* 139 */,	 0 /* 140 */,	 0 /* 141 */,
 0 /* 142 */,	 0 /* 143 */,	 0 /* 144 */,	 0 /* 145 */,	 0 /* 146 */,	 0 /* 147 */,
 0 /* 148 */,	 0 /* 149 */,	 0 /* 150 */,	 0 /* 151 */,	 0 /* 152 */,	 0 /* 153 */,
 0 /* 154 */,	 0 /* 155 */,	 0 /* 156 */,	 0 /* 157 */,	 0 /* 158 */,	 0 /* 159 */,
 0 /* 160 */,	 0 /* 161 */,	 0 /* 162 */,	 0 /* 163 */,	 0 /* 164 */,	 0 /* 165 */,
 0 /* 166 */,	 0 /* 167 */,	 0 /* 168 */,	 0 /* 169 */,	 0 /* 170 */,	 0 /* 171 */,
 0 /* 172 */,	 0 /* 173 */,	 0 /* 174 */,	 0 /* 175 */,	 0 /* 176 */,	 0 /* 177 */,
 0 /* 178 */,	 0 /* 179 */,	 0 /* 180 */,	 0 /* 181 */,	 0 /* 182 */,	 0 /* 183 */,
 0 /* 184 */,	 0 /* 185 */,	 0 /* 186 */,	 0 /* 187 */,	 0 /* 188 */,	 0 /* 189 */,
 0 /* 190 */,	 0 /* 191 */,	 0 /* 192 */,	 0 /* 193 */,	 0 /* 194 */,	 0 /* 195 */,
 0 /* 196 */,	 0 /* 197 */,	 0 /* 198 */,	 0 /* 199 */,	 0 /* 200 */,	 0 /* 201 */,
 0 /* 202 */,	 0 /* 203 */,	 0 /* 204 */,	 0 /* 205 */,	 0 /* 206 */,	 0 /* 207 */,
 0 /* 208 */,	 0 /* 209 */,	 0 /* 210 */,	 0 /* 211 */,	 0 /* 212 */,	 0 /* 213 */,
 0 /* 214 */,	 0 /* 215 */,	 0 /* 216 */,	 0 /* 217 */,	 0 /* 218 */,	 0 /* 219 */,
 0 /* 220 */,	 0 /* 221 */,	 0 /* 222 */,	 0 /* 223 */,	 0 /* 224 */,	 0 /* 225 */,
 0 /* 226 */,	 0 /* 227 */,	 0 /* 228 */,	 0 /* 229 */,	 0 /* 230 */,	 0 /* 231 */,
 0 /* 232 */,	 0 /* 233 */,	 0 /* 234 */,	 0 /* 235 */,	 0 /* 236 */,	 0 /* 237 */,
 0 /* 238 */,	 0 /* 239 */,	 0 /* 240 */,	 0 /* 241 */,	 0 /* 242 */,	 0 /* 243 */,
 0 /* 244 */,	 0 /* 245 */,	 0 /* 246 */,	 0 /* 247 */,	 0 /* 248 */,	 0 /* 249 */,
 0 /* 250 */,	 0 /* 251 */,	 0 /* 252 */,	 0 /* 253 */,	 0 /* 254 */,	 0 /* 255 */
};
