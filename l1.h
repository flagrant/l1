#ifndef _BISONFLAW_L1_H_
#define _BISONFLAW_L1_H_

enum {
	Fcompile	= 0x01,
	Fexec		= 0x02,
	Fprintir	= 0x04,
	Fprintobj	= 0x08,
	Ftime		= 0x10,
};

enum{
	Vintmax		= 2147483647ULL,
	Vuintmax	= 4294967295ULL,
	Vlongmax	= 2147483647ULL,
	Vulongmax	= 4294967295ULL,
	Vvlongmax	= 9223372036854775807ULL,
	Vuvlongmax	= 18446744073709551615ULL,
};

typedef u64 Imm;

typedef
enum{
	Echar=0,
	Edouble,
	Efloat,
	Eint,	
	Elong,
	Eshort,
	Esigned,
	Eunsigned,
	Evoid,
	Enbase,

	Eadd,			/* 10 */
	Eambig,
	Earef,
	Earr,
	Earrow,
	Eband,
	Ebase,
	Ebinop,
	Ebits,
	Eblock,
	Ebor,
	Ebreak,
	Ebxor,
	Ecall,
	Ecast,
	Ecomma,
	Econd,
	Econst,
	Econsts,
	Econtinue,
	Edecl,			/* type specifier + 1 declarator */
	Edecls,			/* type specifier + list of declarators */
	Edefine,
	Ediv,
	Edo,
	Edot,
	Edotdot,
	Eelist,
	Eenum,
	Eenumel,
	Eeq,
	Efieldoff,
	Efields,
	Efor,
	Efun,
	Eg,
	Egop,
	Ege,
	Egt,
	Eid,
	Eif,
	Elambda,
	Eland,
	Ele,
	Elor,
	Elt,
	Emod,
	Emul,
	Ens,
	Eneq,
	Enop,
	Enil,
	Enull,
	Epostdec,
	Epostinc,
	Epredec,
	Epreinc,
	Eptr,
	Eret,
	Eshl,
	Eshr,
	Esizeofe,
	Esizeoft,
	Estruct,
	Esub,
	Etick,
	Etid,
	Etypedef,
	Etypeofe,
	Etypeoft,
	Euand,
	Euminus,
	Eumul,
	Eunion,
	Eunot,
	Euplus,
	Eutwiddle,
	Ewhile,

	/* open-coded primitives */
	E_car,
	E_cdr,
	E_cval,
	E_encode,
	E_lenl,
	E_lens,
	E_lenv,
	E_range,
	E_sizeof,
	E_slices,
	E_str,
	E_tab,
	E_tabdel,
	E_tabenum,
	E_tabget,
	E_tabput,
	E_tn,
	E_vec,
	E_vecl,
	E_vecref,
	E_vecset,
	E_xcast,
} Kind;

typedef
enum Cbase {
	Verr=0,
	Vchar,
	Vshort,
	Vint,
	Vlong,
	Vvlong,
	Vuchar,
	Vushort,
	Vuint,
	Vulong,
	Vuvlong,
	Vfloat,
	Vdouble,
	Vlongdouble,
	Vvoid,
	Vptr,
	Vnil,        
} Cbase;

enum{
	Tbase=0,
	Tstruct,
	Tunion,
	Tenum,
	Tptr,
	Tarr,
	Tfun,
	Ttypedef,

	/* Type flags */
	Ffwd=(1<<0),
};

typedef struct Expr Expr;
typedef struct Decl Decl;
typedef struct Enum Enum;
typedef struct Type Type;

typedef
struct Src {
	char *filename;
	unsigned line;
} Src;

typedef
struct Lits {
	char *s;
	unsigned len;
} Lits;

typedef
struct Liti {
	Imm val;
	Cbase base;
} Liti;

struct Expr {
	Kind kind;

	char *id;		/* Eid, Etick */
	Lits *lits;		/* Econsts */
	Liti liti;		/* Econst */
	Kind op;		/* Ebinop, Egop */

	Expr *e1;
	Expr *e2;
	Expr *e3;
	Expr *e4;

	/* source */
	Src src;

	/* compiler-managed fields */
	void *xp;
	unsigned xn;
};

struct Enum {
	char *id;
	Expr *val;
	Type *type;
	Enum *link;
};

struct Decl {
	Type *type;
	char *id;
	Expr *offs;		/* offset */
	Expr *bits;		/* bit field size */
	Decl *link;
};

struct Type {
	unsigned kind;
	unsigned base;		/* base, enum? */
	char *tid;		/* typedef */
	char *tag;		/* struct, union, enum */
	char *dom;		/* optional domain qualifier for any Type */
	Decl *field;		/* struct, union */
	Enum *en;		/* enum */
	Expr *sz;		/* struct, union */
	Decl *param;		/* func */
	Expr *cnt;		/* arr */
	Type *link;		/* typedef, ptr, arr, func(return type) */
	unsigned char flags;	/* Ffwd, ... */
};

enum {
	MaxIn	= 128,
};

typedef struct YYstate YYstate;

typedef
struct In {
	char *filename;
	FILE *fp;
	unsigned col, line;
	YYstate *yy;
} In;

typedef
struct U {
	jmp_buf jmp;
	In in[MaxIn];
	In *inp;
	Expr *el;
} U;

extern U ctx;
extern char* S[];
extern char* basename[];
extern char *stdinname;
extern Imm basesize[];
extern Imm ptrsize;

YYstate* mkyystate(FILE *fp);
void freeyystate(YYstate *yy);
void setyystate(YYstate *yy);

int yyparse();
Expr* newexpr(unsigned, Expr*, Expr*, Expr*, Expr*);
Expr* copyexpr(Expr *e);
Expr* newbinop(unsigned, Expr*, Expr*);
Expr* newgop(unsigned, Expr*, Expr*);
Expr* mkconst(Cbase base, Imm val); /* rename newconst? */
void freeexpr(Expr*);
void freeexprx(Expr *e);
Expr* invert(Expr*);
Expr* nullelist();
Expr* ptrto(Expr*, Expr*);
Expr* doid(char*);
Expr* doidn(char *s, unsigned long len);
Expr* doconst(char*, unsigned long len);
Expr* doconsts(char*, unsigned long len);
Expr* dotick(Expr*, Expr*);
Lits* mklits(char*, unsigned len);
Lits* copylits(Lits *lits);
void freelits(Lits *lits);
char* fmttype(Type *t, char *o);
Type* basetype(unsigned base);

Expr* dotypes(Expr*);
void dotop(Expr*);
void initparse();
void finiparse();

void printexpr(Expr*);

void pushyy(char *filename);
int popyy();
void tryinclude(char *raw);
void parseerror(char *fmt, ...);
int doparse(char *filename);

#endif /* _BISONFLAW_L1_H_ */

