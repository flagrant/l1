#include "sys.h"
#include "util.h"
#include "l1.h"
#include "code.h"

typedef
enum {
	Qundef = 0,
	Qnil,
	Qnulllist,
	Qcval,
	Qcl,
	Qbox,
	Qpair,
	Qrange,
	Qstr,
	Qtab,
	Qtype,
	Qvec,
	Qxtn,
	Qnkind
} Qkind;

enum {
	/* Xtypedef flags */ 
	Ru8le,
	Ru16le,
	Ru32le,
	Ru64le,
	Rs8le,
	Rs16le,
	Rs32le,
	Rs64le,
};

enum {
	Tabinitsize=1024,	/* power of 2 */
};

typedef struct Vimm Vimm;
typedef struct Vcval Vcval;
typedef struct Box Box;
typedef struct Pair Pair;
typedef struct Range Range;
typedef struct Str Str;
typedef struct Tab Tab;
typedef struct Vec Vec;
typedef struct Xtypedef Xtypedef;
typedef struct Xtypename Xtypename;

struct Val {
	Qkind qkind;
	union {
		Head *hd;
		Imm imm;
		Cval cval;
		Closure *cl;
		Box *box;
		Pair *pair;
		Range *range;
		Str *str;
		Tab *tab;
		Type *type;
		Vec *vec;
		Xtypedef *xtd;
		Xtypename *xtn;
	} u;
};

struct Env {
	HT *ht;
};

struct Closure {
	Head hd;
	Code *code;
	unsigned long entry;
	unsigned dlen;
	Val *display;
	char *id;
	Imm fp;			/* of continuation */
};

struct Box {
	Head hd;
	Val v;
};

typedef struct Tabidx Tabidx;
struct Tabidx {
	u32 idx;
	Tabidx *link;
};

typedef
struct Tabx {
	u32 nxt, lim;
	u32 sz;
	Val *key;
	Val *val;
	Tabidx **idx;
} Tabx;

struct Tab {
	Head hd;
	u32 cnt;		/* key/val pairs stored */
	Tabx *x;		/* current storage, atomically swappable */
};

struct Vimm {
	Head hd;
	Imm imm;
};

struct Vcval {
	Head hd;
	Cval cval;
};

struct Pair {
	Head hd;
	Val car;
	Val cdr;
};

struct Range {
	Head hd;
	Cval beg;
	Cval len;
};

struct Str {
	Head hd;
	Imm len;
	char *s;
};

struct Vec {
	Head hd;
	Imm len;
	Val *vec;
};

struct Xtypedef {
	Head hd;
	unsigned xtkind;	/* = Tbase, Tstruct, ... */
	unsigned basename;	/* base */
	unsigned rep;		/* base, ptr, enum; = Ru8le ... */
	Str *tid;		/* typedef */
	Str *tag;		/* struct, union, enum */
	Cval *sz;		/* struct, union */
	Cval *cnt;		/* arr */
	Xtypedef *link;		/* typedef, ptr, arr, func (return type) */
	Vec *field;		/* struct, union */
	Vec *param;		/* func */
};

struct Xtypename {
	Head hd;
	unsigned xtkind;	/* = Tbase, Tstruct, ... */
	unsigned basename;	/* base */
	Str *tid;		/* typedef */
	Str *tag;		/* struct, union, enum */
	Cval *cnt;		/* arr */
	Xtypename *link;	/* ptr, arr, func (return type) */
	Vec *param;		/* abstract declarators for func */
};

struct VM {
	Val stack[Maxstk];
	Env *topenv;
	Imm sp, fp, pc;
	Closure *clx;
	Insn *ibuf;
	Val ac, cl;
	unsigned char gcpause, gcrun;
	int cm, cgc;
	pthread_t t;
	jmp_buf esc;
};

static void freeval(Val *val);
static void vmsetcl(VM *vm, Closure *cl);
static void strinit(Str *str, Lits *lits);
static void vmerr(VM *vm, char *fmt, ...) __attribute__((noreturn));
static Cval* valcval(Val *v);
static Pair* valpair(Val *v);
static Range* valrange(Val *v);
static Str* valstr(Val *v);

static Val Xundef;
static Val Xnil;
static Val Xnulllist;
static unsigned long long tick;
static unsigned long gcepoch = 2;
typedef u32 (*Tabhashfn)(Val *v);
typedef int (*Tabeqfn)(Val *a, Val *b);
static Tabhashfn Qhash[Qnkind];
static Tabeqfn Qeq[Qnkind];

static Head eol;		/* end-of-list sentinel */

static char *opstr[Iopmax] = {
	[Iadd] = "+",
	[Iand] = "&",
	[Idiv] = "/",
	[Imod] = "%",
	[Imul] = "*",
	[Ior] = "|",
	[Ishl] = "<<",
	[Ishr] = ">>",
	[Isub] = "-",
	[Icmpeq] = "==",
	[Icmpneq] = "!=",
	[Icmpgt] = ">",
	[Icmpge] = ">=",
	[Icmplt] = "<",
	[Icmple] = "<=",
};

typedef
struct Rootset {
	Head *roots;
	Head *last;
	Head *before_last;
	Head *this;
	Head *(*getlink)(Head *);
	Head *(*getolink)(Head *);
	void (*setlink)(Head *, Head *);
} Rootset;

static Rootset roots;
static Rootset stores;

#define GCCOLOR(i) ((i)%3)
enum {
	GCfree = 3,
	GCrate = 1000,
};

static unsigned long long nextgctick = GCrate;

Heap heapcode, heapcl, heapbox, heapcval, heappair,
	heaprange, heapstr, heaptab, heapvec, heapxtn;
static Code *kcode;

static void*
read_and_clear(void *pp)
{
	void **p = (void**)pp;
	void *q;
	q = 0;
	asm("xchg %0,%1" : "=r"(q), "=m"(*p) : "0"(q), "m"(*p) : "memory");
	return q;
}

static void
writebarrier()
{
	/* force previous writes to be visible to other processors
	   before subsequent ones. */
	/* on x86 (through core 2 duo),
	   processor-ordering memory model precludes
	   need for explict barrier such as sfence.  if
	   the underlying memory were in WC mode (see
	   intel vol 3, chapter 10), things would be different. */
}

Head*
galloc(Heap *heap)
{
	Head *o, *ap, *fp;
	unsigned m;

retry:
	if(heap->free){
		o = heap->free;
		heap->free = o->link;
		if(o->link == &eol)
			fatal("o->link == &eol (1)");
		o->link = 0;
		if(o->state != -1)
			fatal("galloc bad state %d", o->state);
		o->state = 0;
	}else if(heap->swept){
		heap->free = (Head*)read_and_clear(&heap->swept);
		goto retry;
	}else{
		ap = heap->alloc;
		fp = 0;
		for(m = 0; m < AllocBatch; m++){
			o = xmalloc(heap->sz);
			o->heap = heap;
			o->alink = ap;
			o->link = fp;
			if(o->link == &eol)
				fatal("o->link == &eol (2)");
			o->color = GCfree;
			o->state = -1;
			ap = o;
			fp = o;
		}
		heap->alloc = o;
		heap->free = o;
		goto retry;
	}
	
	o->color = GCCOLOR(gcepoch);

	return o;
}

static void
sweepheap(Heap *heap, unsigned color)
{
	Head *p;
	p = heap->alloc;
	while(p){
		if(p->color == color){
			if(heap->free1)
				heap->free1(p);
			if(p->state != 0)
				fatal("sweep heap bad state %d", p->state);
			p->link = heap->sweep;
			p->slink = 0;
			p->state = -1;
			heap->sweep = p;
			p->color = GCfree;
		}
		p = p->alink;
	}
	if(heap->swept == 0){
		heap->swept = heap->sweep;
		heap->sweep = 0;
	}
}

static void
sweep(unsigned color)
{
	sweepheap(&heapcode, color);
	sweepheap(&heapcl, color);
	sweepheap(&heapbox, color);
	sweepheap(&heapcval, color);
	sweepheap(&heappair, color);
	sweepheap(&heaprange, color);
	sweepheap(&heapstr, color);
	sweepheap(&heaptab, color);
	sweepheap(&heapvec, color);
	sweepheap(&heapxtn, color);
}

static void
freeheap(Heap *heap)
{
	Head *p, *q;
	p = heap->alloc;
	while(p){
		q = p->alink;
		free(p);
		p = q;
	}
}

static Head*
valhead(Val *v)
{
	switch(v->qkind){
	case Qundef:
	case Qnil:
	case Qnulllist:
	case Qcval:
	case Qtype:
		return 0;
		break;
	default:
		return v->u.hd;
		break;
	}
}

/* called on roots by marker.  called on stores by mutator. */
static void
addroot(Rootset *rs, Head *h)
{
	int x;

	if(h == 0)
		return;
	if(rs->getlink(h))
		return;		/* already on rootlist */
	x = h->state;
	if(x > 2 || x < 0)
		fatal("addroot bad state %d", x);
	h->state++;
	rs->setlink(h, rs->roots);
	writebarrier();
	rs->roots = h;
}

static Head*
removeroot(Rootset *rs)
{
	Head *h;
	int x;

	if(rs->this == rs->before_last){
		rs->before_last = rs->last;
		rs->this = rs->last = rs->roots;
	}
	if(rs->this == rs->before_last)
		return 0;
	h = rs->this;
	rs->this = rs->getlink(h);
	x = h->state;
	if(x > 2 || x <= 0)
		fatal("remove root bad state %d", x);
	h->state--;
	rs->setlink(h, 0);
	return h;
}

static int
rootsetempty(Rootset *rs)
{
	return rs->before_last == rs->roots;
}

static Head*
getrootslink(Head *h)
{
	return h->link;
}

static Head*
getstoreslink(Head *h)
{
	return h->slink;
}

static void
setrootslink(Head *h, Head *v)
{
	h->link = v;
}

static void
setstoreslink(Head *h, Head *v)
{
	h->slink = v;
}

static void
markhead(Head *hd, unsigned color)
{
	Ictx ictx;
	Head *c;

	if(hd == 0)
		return;

	if(hd->color == color)
		return;

	hd->color = color;
	if(hd->heap->iter == 0)
		return;
	memset(&ictx, 0, sizeof(ictx));
	while(1){
		c = hd->heap->iter(hd, &ictx);
		if(c == 0)
			break;
		if(c->color != color)
			addroot(&roots, c);
	}
}

static void
markrs(Rootset *rs, unsigned color)
{
	Head *h;

	while(1){
		h = removeroot(rs);
		if(h == 0)
			break;
		markhead(h, color);
	}
}

static void
mark(unsigned color)
{
	markrs(&stores, GCCOLOR(gcepoch));
	markrs(&roots, GCCOLOR(gcepoch));
}

static void
bindingroot(void *u, char *k, void *v)
{
	addroot(&roots, valhead((Val*)v));
}

static void
rootset(VM *vm)
{
	unsigned m;

	/* never collect these things */
	addroot(&roots, (Head*)kcode); 

	if(vm == 0)
		return;

	for(m = vm->sp; m < Maxstk; m++)
		addroot(&roots, valhead(&vm->stack[m]));
	hforeach(vm->topenv->ht, bindingroot, 0);
	addroot(&roots, valhead(&vm->ac));
	addroot(&roots, valhead(&vm->cl));
}

static void
rootsetreset(Rootset *rs)
{
	rs->roots = &eol;
	rs->last = &eol;
	rs->before_last = &eol;
	rs->this = &eol;
}

static void
gcreset()
{
	rootsetreset(&roots);
	rootsetreset(&stores);
}

static void
gc(VM *vm)
{
	rootset(vm);
	gcepoch++;
	mark(GCCOLOR(gcepoch));
	sweep(GCCOLOR(gcepoch-2));
	while(!rootsetempty(&stores))
		mark(GCCOLOR(gcepoch));
	gcreset();
}

enum {
	GCdie = 0x1,
	GCdied = 0x2,
	GCpaused = 0x4,
	GCresume = 0x8,
	GCrun = 0x10,
	GCrunning = 0x20,
};

static int
waitmutator(VM *vm)
{
	char b;
	
	vm->gcpause = 1;
	if(0 > read(vm->cgc, &b, 1))
		fatal("gc synchronization failure"); 
	if(b == GCdie)
		return 1;
	if(b != GCpaused)
		fatal("gc protocol botch");
	return 0;
}

static void
resumemutator(VM *vm)
{
	char b;

	vm->gcpause = 0;
	b = GCresume;
	if(0 > write(vm->cgc, &b, 1))
		fatal("gc synchronization failure");
}

static void
waitgcrun(VM *vm)
{
	char b;
	
	if(0 > read(vm->cgc, &b, 1))
		fatal("gc synchronization failure"); 
	if(b == GCdie){
		b = GCdied;
		if(0 > write(vm->cgc, &b, 1))
			fatal("gc sychronization failure");
		pthread_exit(0);
	}
	if(b != GCrun)
		fatal("gc protocol botch");

	gcreset();
	rootset(vm);
	gcepoch++;
	vm->gcrun = 1;
	b = GCrunning;
	if(0 > write(vm->cgc, &b, 1))
		fatal("gc synchronization failure");

}

static void
gcsync(int fd, char t, char r)
{
	char b;
	if(0 > write(fd, &t, 1))
		fatal("gc synchronization failure");
	if(0 > read(fd, &b, 1))
		fatal("gc synchronization failure");
	if(b != r)
		fatal("gc protocol botch");
}

static int
needsgc()
{
	if(tick >= nextgctick){
		nextgctick = tick+GCrate;
		return 1;
	}else
		return 0;
}

static void
gcpoll(VM *vm)
{
	if(vm->gcpause)
		gcsync(vm->cm, GCpaused, GCresume);
	else if(vm->gcrun)
		return;
	else if(needsgc())
		gcsync(vm->cm, GCrun, GCrunning);
}

static void
gckill(VM *vm)
{
	gcsync(vm->cm, GCdie, GCdied);
	close(vm->cm);
	close(vm->cgc);
	pthread_join(vm->t, 0);
}

#if 0
static int
#else
static void*
#endif
gcchild(void *p)
{
	VM *vm = (VM*)p;
	int die = 0;
	char b;

	while(!die){
		waitgcrun(vm);
		mark(GCCOLOR(gcepoch));
		sweep(GCCOLOR(gcepoch-2));
		die = waitmutator(vm);
		while(!rootsetempty(&stores)){
			if(!die)
				resumemutator(vm);
			mark(GCCOLOR(gcepoch));
			if(!die)
				die = waitmutator(vm);
		}
		vm->gcrun = 0;
		if(!die)
			resumemutator(vm);
	}

	b = GCdied;
	if(0 > write(vm->cgc, &b, 1))
		fatal("gc sychronization failure");
	pthread_exit(0);
	return 0;
}

static void
newchan(int *left, int *right)
{
	int fd[2];

	if(0 > socketpair(PF_UNIX, SOCK_STREAM, 0, fd))
		fatal("cannot allocate channel");
	*left = fd[0];
	*right = fd[1];
}

static void
concurrentgc(VM *vm)
{
	newchan(&vm->cm, &vm->cgc);
	vm->gcpause = 0;

	if(0 > pthread_create(&vm->t, 0, gcchild, vm))
		fatal("pthread create failed");
}

static Imm
typesize(VM *vm, Type *t)
{
	switch(t->kind){
	case Tbase:
		return basesize[t->base];
	case Ttypedef:
		return typesize(vm, t->link);
	case Tstruct:
	case Tunion:
		vmerr(vm, "need to evaluate struct/union size");
		break;
	case Tenum:
		vmerr(vm, "need to get enums right");
		break;
	case Tptr:
		return ptrsize;
	case Tarr:
		vmerr(vm, "need to evaluate array size");
		break;
	case Tfun:
		vmerr(vm, "attempt to compute size of function type");
		break;
	default:
		fatal("typesize bug");
	}
}

static Head*
iterbox(Head *hd, Ictx *ictx)
{
	Box *box;
	box = (Box*)hd;
	if(ictx->n > 0)
		return 0;
	ictx->n = 1;
	return valhead(&box->v);
}

static Head*
itercl(Head *hd, Ictx *ictx)
{
	Closure *cl;
	cl = (Closure*)hd;
	if(ictx->n > cl->dlen)
		return 0;
	if(ictx->n == cl->dlen){
		ictx->n++;
		return (Head*)cl->code;
	}
	return valhead(&cl->display[ictx->n++]);
}

static Head*
iterpair(Head *hd, Ictx *ictx)
{
	Pair *pair;
	pair = (Pair*)hd;

	switch(ictx->n++){
	case 0:
		return valhead(&pair->car);
	case 1:
		return valhead(&pair->cdr);
	default:
		return 0;
	}
}

Closure*
mkcl(Code *code, unsigned long entry, unsigned len, char *id)
{
	Closure *cl;
	cl = (Closure*)galloc(&heapcl);
	cl->code = code;
	cl->entry = entry;
	cl->dlen = len;
	cl->display = xmalloc(cl->dlen*sizeof(Val));
	cl->id = xstrdup(id);
	return cl;
}

static void
freecl(Head *hd)
{
	Closure *cl;
	cl = (Closure*)hd;
	free(cl->display);
	free(cl->id);
}


/* http://www.cris.com/~Ttwang/tech/inthash.htm */
static u32
hash6432shift(u64 key)
{
	key = (~key) + (key << 18);
	key = key ^ (key >> 31);
	key = key * 21;
	key = key ^ (key >> 11);
	key = key + (key << 6);
	key = key ^ (key >> 22);
	return (u32)key;
}

static u32
hashptr32shift(void *p)
{
	uintptr_t key;
	key = (uintptr_t)p;
	key = (~key) + (key << 18);
	key = key ^ (key >> 31);
	key = key * 21;
	key = key ^ (key >> 11);
	key = key + (key << 6);
	key = key ^ (key >> 22);
	return (u32)key;
}


/* one-at-a-time by jenkins */
static u32
shash(char *s, Imm len)
{
	unsigned char *p = (unsigned char*)s;
	u32 h;

	h = 0;
	while(len > 0){
		h += *p;
		h += h<<10;
		h ^= h>>6;
		p++;
		len--;
	}
	h += h<<3;
	h ^= h>>11;
	h += h<<15;
	return h;
}

static u32
nohash(Val *val)
{
	fatal("bad type of key (%d) to table operation", val->qkind);
}

static u32
hashptr(Val *val)
{
	return hashptr32shift(valhead(val));
}

static int
eqptr(Val *a, Val *b)
{
	return valhead(a)==valhead(b);
}

static u32
hashptrv(Val *val)
{
	return hashptr32shift(val);
}

static int
eqptrv(Val *a, Val *b)
{
	return a==b;
}

static u32
hashcval(Val *val)
{
	Cval *cv;
	cv = valcval(val);
	return hash6432shift(cv->val);
}

static int
eqcval(Val *a, Val *b)
{
	Cval *cva, *cvb;
	cva = valcval(a);
	cvb = valcval(b);
	return cva->val==cvb->val;
}

static u32
hashrange(Val *val)
{
	Range *r;
	r = valrange(val);
	return hash6432shift(r->beg.val)^hash6432shift(r->len.val);
}

static int
eqrange(Val *a, Val *b)
{
	Range *ra, *rb;
	ra = valrange(a);
	rb = valrange(b);
	return ra->beg.val==rb->beg.val && ra->len.val==rb->len.val;
}

static u32
hashstr(Val *val)
{
	Str *s;
	s = valstr(val);
	return shash(s->s, s->len);
}

static int
eqstr(Val *a, Val *b)
{
	Str *sa, *sb;
	sa = valstr(a);
	sb = valstr(b);
	if(sa->len != sb->len)
		return 0;
	return memcmp(sa->s, sb->s, sa->len) ? 0 : 1;
}

static Str*
mkstr0(char *s)
{
	Str *str;
	str = (Str*)galloc(&heapstr);
	str->len = strlen(s);
	str->s = xmalloc(str->len);
	memcpy(str->s, s, str->len);
	return str;
}

static Str*
mkstr(char *s, unsigned long len)
{
	Str *str;
	str = (Str*)galloc(&heapstr);
	str->len = len;
	str->s = xmalloc(str->len);
	memcpy(str->s, s, str->len);
	return str;
}

static Str*
mkstrn(unsigned long len)
{
	Str *str;
	str = (Str*)galloc(&heapstr);
	str->len = len;
	str->s = xmalloc(str->len);
	return str;
}

static void
strinit(Str *str, Lits *lits)
{
	str->len = lits->len;
	str->s = xmalloc(str->len);
	memcpy(str->s, lits->s, str->len);
}

static Str*
strslice(Str *str, Imm beg, Imm end)
{
	return mkstr(str->s+beg, end-beg);
}

static void
freestr(Head *hd)
{
	Str *str;
	str = (Str*)hd;
	free(str->s);
}

static int
listlen(Val *v, Imm *rv)
{
	Imm m;
	Pair *p;

	m = 0;
	while(v->qkind == Qpair){
		m++;
		p = valpair(v);
		v = &p->cdr;
	}
	if(v->qkind != Qnulllist)
		return 0;
	*rv = m;
	return 1;
}

static Vec*
mkvec(Imm len)
{
	Vec *vec;

	vec = (Vec*)galloc(&heapvec);
	vec->len = len;
	vec->vec = xmalloc(len*sizeof(*vec->vec));
	return vec;
}

static Vec*
mkvecnil(Imm len)
{
	Vec *vec;
	Imm i;

	vec = mkvec(len);
	for(i = 0; i < len; i++)
		vec->vec[i] = Xnil;
	return vec;
}

static Val*
vecref(Vec *vec, Imm idx)
{
	return &vec->vec[idx];
}

static void
_vecset(Vec *vec, Imm idx, Val *v)
{
	vec->vec[idx] = *v;
}

static void
vecset(VM *vm, Vec *vec, Imm idx, Val *v)
{
	if(vm->gcrun)
		addroot(&stores, valhead(&vec->vec[idx]));
	_vecset(vec, idx, v);
}

static Head*
itervec(Head *hd, Ictx *ictx)
{
	Vec *vec;
	vec = (Vec*)hd;
	if(ictx->n >= vec->len)
		return 0;
	return valhead(&vec->vec[ictx->n++]);
}

static void
freevec(Head *hd)
{
	Vec *vec;
	vec = (Vec*)hd;
	free(vec->vec);
}

static Tabx*
mktabx(u32 sz)
{
	Tabx *x;
	x = xmalloc(sizeof(Tabx));
	x->sz = sz;
	x->lim = 2*sz/3;
	x->val = xmalloc(x->lim*sizeof(Val)); /* matches Xundef */
	x->key = xmalloc(x->lim*sizeof(Val)); /* matches Xundef */
	x->idx = xmalloc(x->sz*sizeof(Tabidx*));
	return x;
}

static void
freetabx(Tabx *x)
{
	free(x->val);
	free(x->key);
	free(x->idx);
	free(x);
}

static Tab*
_mktab(Tabx *x)
{
	Tab *tab;
	tab = (Tab*)galloc(&heaptab);
	tab->x = x;
	return tab;
}

static Tab*
mktab()
{
	return _mktab(mktabx(Tabinitsize));
}

static Head*
itertab(Head *hd, Ictx *ictx)
{
	Tab *tab;
	u32 idx;
	Tabx *x;

	tab = (Tab*)hd;

	if(ictx->n == 0){
		ictx->x = x = tab->x;
	}else
		x = ictx->x;
		
	if(ictx->n >= 2*x->nxt)
		return 0;
	if(ictx->n >= x->nxt){
		idx = ictx->n-x->nxt;
		ictx->n++;
		return valhead(&x->val[idx]);
	}
	idx = ictx->n++;
	return valhead(&x->key[idx]);
}

static void
freetab(Head *hd)
{
	u32 i;
	Tab *tab;
	Tabx *x;
	Tabidx *tk, *pk;

	tab = (Tab*)hd;
	x = tab->x;
	for(i = 0; i < x->sz; i++){
		tk = x->idx[i];
		while(tk){
			pk = tk;
			tk = tk->link;
			free(pk);
		}
	}
	freetabx(x);
}

static Tabidx*
_tabget(Tab *tab, Val *keyv, Tabidx ***prev)
{
	Tabidx **p, *tk;
	Val *kv;
	u32 idx;
	Tabx *x;
	unsigned kind;

	x = tab->x;
	kind = keyv->qkind;
	idx = Qhash[kind](keyv)&(x->sz-1);
	p = &x->idx[idx];
	tk = *p;
	while(tk){
		kv = &x->key[tk->idx];
		if(kv->qkind == kind && Qeq[kind](keyv, kv)){
			if(prev)
				*prev = p;
			return tk;
		}
		p = &tk->link;
		tk = *p;
	}
	return 0;
}

static Val*
tabget(Tab *tab, Val *keyv)
{
	Tabidx *tk;
	tk = _tabget(tab, keyv, 0);
	if(tk)
		return &tab->x->val[tk->idx];
	return 0;
}

/* double hash table size and re-hash entries */
static void
tabexpand(VM *vm, Tab *tab)
{
	Tabidx *tk, *nxt;
	u32 i, m, idx;
	Tabx *x, *nx;
	Val *kv;

	x = tab->x;
	nx = mktabx(x->sz*2);
	m = 0;
	for(i = 0; i < x->sz && m < tab->cnt; i++){
		tk = x->idx[i];
		while(tk){
			nxt = tk->link;
			kv = &x->key[tk->idx];
			idx = Qhash[kv->qkind](kv)&(nx->sz-1);
			tk->link = nx->idx[idx];
			nx->idx[idx] = tk;
			nx->key[m] = x->key[tk->idx];
			nx->val[m] = x->val[tk->idx];
			tk->idx = m;
			m++;
			tk = nxt;
		}
	}
	nx->nxt = tab->cnt;

	/* fresh garbage reference to pre-expanded state of table.
	   this preserves a reference to the pre-expand storage
	   (tab->x) in case gc (via itertab) is concurrently (i.e., in
	   this epoch) marking it.  we exploit the property that
	   objects always survive the epoch of their creation. */
	x->sz = 0;		/* so freetab does not free nx's Tabkeys */
	_mktab(x);

	/* FIXME: it seems snapshot-at-beginning property, plus above
	   referenced property of objects, together ensure that any
	   object added to expanded table is safe from collection. */
	tab->x = nx;
}

static void
tabput(VM *vm, Tab *tab, Val *keyv, Val *val)
{
	Tabidx *tk;
	u32 idx;
	Tabx *x;

	x = tab->x;
	tk = _tabget(tab, keyv, 0);
	if(tk){
		if(vm->gcrun)
			addroot(&stores, valhead(&x->val[tk->idx]));
		x->val[tk->idx] = *val;
		return;
	}

	if(x->nxt >= x->lim){
		tabexpand(vm, tab);
		x = tab->x;
	}

	tk = xmalloc(sizeof(Tabidx));

	/* FIXME: snapshot-at-beginning seems to imply that it does
	   not matter whether gc can see these new values in this
	   epoch. right? */
	tk->idx = x->nxt;
	x->key[tk->idx] = *keyv;
	x->val[tk->idx] = *val;
	idx = Qhash[keyv->qkind](keyv)&(x->sz-1);
	tk->link = x->idx[idx];
	x->idx[idx] = tk;
	x->nxt++;
	tab->cnt++;
}

static void
tabdel(VM *vm, Tab *tab, Val *keyv)
{
	Tabidx *tk, **ptk;
	Tabx *x;
	
	x = tab->x;
	tk = _tabget(tab, keyv, &ptk);
	if(tk == 0)
		return;
	if(vm->gcrun){
		addroot(&stores, valhead(&x->key[tk->idx]));
		addroot(&stores, valhead(&x->val[tk->idx]));
	}
	x->key[tk->idx] = Xundef;
	x->val[tk->idx] = Xundef;
	*ptk = tk->link;
	free(tk);
	tab->cnt--;
}

/* create a new vector of len 2N, where N is number of elements in TAB.
   elements 0..N-1 are the keys of TAB; N..2N-1 are the associated vals. */
static Vec*
tabenum(Tab *tab)
{
	Vec *vec;
	Tabidx *tk;
	u32 i;;
	Imm m;
	Tabx *x;

	x = tab->x;
	vec = mkvec(2*tab->cnt);
	m = 0;
	for(i = 0; i < x->sz && m < tab->cnt; i++){
		tk = x->idx[i];
		while(tk){
			_vecset(vec, m, &x->key[tk->idx]);
			_vecset(vec, m+tab->cnt, &x->val[tk->idx]);
			m++;
			tk = tk->link;
		}
	}
	return vec;
}

static Xtypename*
mkxtn()
{
	Xtypename *xtn;
	xtn = galloc(&heapxtn);
	return xtn;
}

static Head*
iterxtn(Head *hd, Ictx *ictx)
{
	return 0;
}

Env*
mkenv()
{
	Env *env;
	env = xmalloc(sizeof(Env));
	env->ht = mkht();
	return env;
}

Val*
envgetbind(Env *env, char *id)
{
	Val *v;

	v = hget(env->ht, id);
	if(!v){
		v = xmalloc(sizeof(Val));
		hput(env->ht, id, v);
	}
	return v;
}

static void
envbind(Env *env, char *id, Val *val)
{
	Val *v;

	v = envgetbind(env, id);
	*v = *val;
}

static int
envlookup(Env *env, char *id, Val *val)
{
	Val *v;

	v = envgetbind(env, id);
	if(v->qkind == Qundef)
		return 0;
	*val = *v;
	return 1;
}

static void
freebinding(void *u, char *id, void *v)
{
	Val *val;
	val = (Val*)v;
	freeval(val);
}

void
freeenv(Env *env)
{
	hforeach(env->ht, freebinding, 0);
	freeht(env->ht);
	free(env);
}

static void
mkvalimm(Imm imm, Val *vp)
{
	vp->qkind = Qcval;
	initcval(&vp->u.cval, 0, imm);
}

static void
mkvalcval(Type *t, Imm imm, Val *vp)
{
	vp->qkind = Qcval;
	initcval(&vp->u.cval, t, imm);
}

static void
mkvalcval2(Cval *cv, Val *vp)
{
	vp->qkind = Qcval;
	vp->u.cval = *cv;
}

static void
mkvalcl(Closure *cl, Val *vp)
{
	vp->qkind = Qcl;
	vp->u.cl = cl;
}

static void
mkvalbox(Val *boxed, Val *vp)
{
	Box *box;
	box = (Box*)galloc(&heapbox);
	box->v = *boxed;
	vp->qkind = Qbox;
	vp->u.box = box;
}

static void
mkvalpair(Val *car, Val *cdr, Val *vp)
{
	Pair *pair;
	pair = (Pair*)galloc(&heappair);
	pair->car = *car;
	pair->cdr = *cdr;
	vp->qkind = Qpair;
	vp->u.pair = pair;
}

static void
mkvalstr(Str *str, Val *vp)
{
	vp->qkind = Qstr;
	vp->u.str = str;
}

static void
mkvaltab(Tab *tab, Val *vp)
{
	vp->qkind = Qtab;
	vp->u.tab = tab;
}

static void
mkvalvec(Vec *vec, Val *vp)
{
	vp->qkind = Qvec;
	vp->u.vec = vec;
}

static void
mkvaltype(Type *type, Val *vp)
{
	vp->qkind = Qtype;
	vp->u.type = type;
}

static void
mkvalrange(Cval *beg, Cval *len, Val *vp)
{
	Range *r;

	r = (Range*)galloc(&heaprange);
	r->beg = *beg;
	r->len = *len;
	vp->qkind = Qrange;
	vp->u.range = r;
}

static void
mkvalxtn(Xtypename *xtn, Val *vp)
{
	vp->qkind = Qxtn;
	vp->u.xtn = xtn;
}

static Imm
valimm(Val *v)
{
	if(v->qkind != Qcval)
		fatal("valimm on non-cval");
	return v->u.cval.val;
}

static Cval*
valcval(Val *v)
{
	if(v->qkind != Qcval)
		fatal("valcval on non-cval");
	return &v->u.cval;
}

static Closure*
valcl(Val *v)
{
	if(v->qkind != Qcl)
		fatal("valcl on non-closure");
	return v->u.cl;
}

static Pair*
valpair(Val *v)
{
	if(v->qkind != Qpair)
		fatal("valpair on non-pair");
	return v->u.pair;
}

static Range*
valrange(Val *v)
{
	if(v->qkind != Qrange)
		fatal("valrange on non-range");
	return v->u.range;
}

static Str*
valstr(Val *v)
{
	if(v->qkind != Qstr)
		fatal("valstr on non-string");
	return v->u.str;
}

static Tab*
valtab(Val *v)
{
	if(v->qkind != Qtab)
		fatal("valtab on non-table");
	return v->u.tab;
}

static Vec*
valvec(Val *v)
{
	if(v->qkind != Qvec)
		fatal("valvec on non-vector");
	return v->u.vec;
}

static Xtypename*
valxtn(Val *v)
{
	if(v->qkind != Qxtn)
		fatal("valxtn on non-typename");
	return v->u.xtn;
}

static void
valboxed(Val *v, Val *dst)
{
	if(v->qkind != Qbox)
		fatal("valboxed on non-box");
	*dst = v->u.box->v;
}

static Cval*
valboxedcval(Val *v)
{
	return &v->u.box->v.u.cval;
}

static Type*
valtype(Val *v)
{
	if(v->qkind != Qtype)
		fatal("valtype on non-type");
	return v->u.type;
}

static int
zeroval(Val *v)
{
	Cval *cv;

	switch(v->qkind){
	case Qcval:
		cv = valcval(v);
		return cv->val == 0;
	default:
		fatal("branch on non-integer value");
		return 0;
	}
}

static void
freeval(Val *v)
{
	free(v);
}

static void
putbox(VM *vm, Val *box, Val *boxed)
{
	if(box->qkind != Qbox)
		fatal("putbox on non-box");
	if(boxed->qkind == Qbox)
		fatal("boxing boxes is insane");
	if(vm->gcrun)
		addroot(&stores, valhead(&box->u.box->v));
	box->u.box->v = *boxed;
}

static void
putval(VM *vm, Val *v, Location *loc)
{
	Val *dst;

	switch(loc->kind){
	case Lreg:
		switch(loc->idx){
		case Rac:
			vm->ac = *v;
			break;
		case Rsp:
			vm->sp = valimm(v);
			break;
		case Rfp:
			vm->fp = valimm(v);
			break;
		case Rpc:
			vm->pc = valimm(v);
			break;
		case Rcl:
			vm->cl = *v;
			vmsetcl(vm, valcl(&vm->cl));
			break;
		default:
			fatal("bug");
		}
		break;
	case Lparam:
		dst = &vm->stack[(vm->fp+1)+loc->idx];
		if(loc->indirect)
			putbox(vm, dst, v);
		else
			*dst = *v;
		break;
	case Llocal:
		dst = &vm->stack[(vm->fp-1)-loc->idx];
		if(loc->indirect)
			putbox(vm, dst, v);
		else
			*dst = *v;
		break;
	case Ldisp:
		dst = &vm->clx->display[loc->idx];
		if(loc->indirect)
			putbox(vm, dst, v);
		else
			*dst = *v;
		break;
	case Ltopl:
		dst = loc->val;
		*dst = *v;
		break;
	default:
		fatal("bug");
	}
}

static void
printsrc(Closure *cl, Imm pc)
{
	Code *code;
	
	code = cl->code;
	while(1){
		if(code->labels[pc] && code->labels[pc]->src){
			printf("%s:%u\n",
			       code->labels[pc]->src->filename,
			       code->labels[pc]->src->line);
			return;
		}
		if(pc == 0)
			break;
		pc--;
	}
	printf("(no source information)\n");
}

static void
vmerr(VM *vm, char *fmt, ...)
{
	va_list args;
	Imm pc, fp, narg;
	Closure *cl;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
	fflush(stderr);
	
	/* dump stack trace */
	pc = vm->pc-1;		/* vm loop increments pc after fetch */
	if(vm->pc == 0)
		printf("vmerr: pc is 0!\n");
	fp = vm->fp;
	cl = vm->clx;
	while(fp != 0){
		printsrc(cl, pc);
		narg = valimm(&vm->stack[fp]);
		pc = valimm(&vm->stack[fp+narg+1]);
		pc--;		/* return address is insn following call */
		cl = valcl(&vm->stack[fp+narg+2]);
		fp = valimm(&vm->stack[fp+narg+3]);
	}

	longjmp(vm->esc, 1);
}

static void
getval(VM *vm, Location *loc, Val *vp)
{
	Val *p;

	switch(loc->kind){
	case Lreg:
		switch(loc->idx){
		case Rac:
			*vp = vm->ac;
			return;
		case Rsp:
			mkvalimm(vm->sp, vp);
			return;
		case Rfp:
			mkvalimm(vm->fp, vp);
			return;
		case Rpc:
			mkvalimm(vm->pc, vp);
			return;
		case Rcl:
			*vp = vm->cl;
			return;
		default:
			fatal("bug");
		}
		break;
	case Lparam:
		p = &vm->stack[(vm->fp+1)+loc->idx];
		if(loc->indirect)
			valboxed(p, vp);
		else
			*vp = *p;
		return;
	case Llocal:
		p = &vm->stack[(vm->fp-1)-loc->idx];
		if(loc->indirect)
			valboxed(p, vp);
		else
			*vp = *p;
		return;
	case Ldisp:
		p = &vm->clx->display[loc->idx];
		if(loc->indirect)
			valboxed(p, vp);
		else
			*vp = *p;
		return;
	case Ltopl:
		p = loc->val;
		if(p->qkind == Qundef)
			vmerr(vm, "reference to undefined variable %s",
			      topvecid(loc->idx, vm->clx->code->topvec));
		*vp = *p;
		return;
	default:
		fatal("bug");
	}
}

static Cval*
getcval(VM *vm, Location *loc, Cval *tmp)
{
	Val *p;

	switch(loc->kind){
	case Lreg:
		switch(loc->idx){
		case Rac:
			return valcval(&vm->ac);
		case Rsp:
			initcval(tmp, 0, vm->sp);
			return tmp;
		case Rfp:
			initcval(tmp, 0, vm->fp);
			return tmp;
		case Rpc:
			initcval(tmp, 0, vm->pc);
			return tmp;
		case Rcl:
		default:
			fatal("bug");
		}
		break;
	case Lparam:
		p = &vm->stack[(vm->fp+1)+loc->idx];
		if(loc->indirect)
			return valboxedcval(p);
		return valcval(p);
	case Llocal:
		p = &vm->stack[(vm->fp-1)-loc->idx];
		if(loc->indirect)
			return valboxedcval(p);
		return valcval(p);
	case Ldisp:
		p = &vm->clx->display[loc->idx];
		if(loc->indirect)
			return valboxedcval(p);
		return valcval(p);
	case Ltopl:
		p = loc->val;
		if(p->qkind == Qundef)
			vmerr(vm, "reference to undefined variable %s",
			      topvecid(loc->idx, vm->clx->code->topvec));
		return valcval(p);
	default:
		fatal("bug");
	}
}

static void
getvalrand(VM *vm, Operand *r, Val *vp)
{
	switch(r->okind){
	case Oloc:
		getval(vm, &r->u.loc, vp);
		break;
	case Ocval:
		mkvalcval2(&r->u.cval, vp);
		break;
	case Olits:
		vp->qkind = Qstr;
		vp->u.str = (Str*)galloc(&heapstr);
		strinit(vp->u.str, r->u.lits);
		break;
	case Onil:
		vp->qkind = Qnil;
		break;
	default:
		fatal("bug");
	}
}

static Cval*
getcvalrand(VM *vm, Operand *r, Cval *tmp)
{
	switch(r->okind){
	case Oloc:
		return getcval(vm, &r->u.loc, tmp);
	case Ocval:
		return &r->u.cval;
	default:
		fatal("bug");
	}
}

static void
putvalrand(VM *vm, Val *v, Operand *r)
{
	if(r->okind != Oloc)
		fatal("bad destination");
	putval(vm, v, &r->u.loc);
}

static void
putcvalrand(VM *vm, Cval *cv, Operand *r)
{
	Val v;

	if(r->okind != Oloc)
		fatal("bad destination");
	mkvalcval2(cv, &v);
	putval(vm, &v, &r->u.loc);
}

static Imm
str2imm(Type *t, Str *s)
{
	if(t->kind != Tbase && t->kind != Tptr)
		fatal("str2imm on non-scalar type");
	if(t->kind == Tptr){
		return *(u32*)s;
	}

	switch(t->base){
	case Vchar:
		return *(s8*)s;
	case Vshort:
		return *(s16*)s;
	case Vint:
		return *(s32*)s;
	case Vlong:
		return *(s32*)s;
	case Vvlong:
		return *(s64*)s;
	case Vuchar:
		return *(u8*)s;
	case Vushort:
		return *(u16*)s;
	case Vuint:
		return *(u32*)s;
	case Vulong:
		return *(u32*)s;
	case Vuvlong:
		return *(u64*)s;
	default:
		fatal("missing case in str2imm");
	}	
}

static Str*
imm2str(Type *t, Imm imm)
{
	Str *str;
	char *s;

	if(t->kind != Tbase && t->kind != Tptr)
		fatal("str2imm on non-scalar type");

	if(t->kind == Tptr){
		str = mkstrn(sizeof(u32));
		s = str->s;
		*(u32*)s = (u32)imm;
		return str;
	}


	switch(t->base){
	case Vchar:
		str = mkstrn(sizeof(s8));
		s = str->s;
		*(s8*)s = (s8)imm;
		return str;
	case Vshort:
		str = mkstrn(sizeof(s16));
		s = str->s;
		*(s16*)s = (s16)imm;
		return str;
	case Vint:
		str = mkstrn(sizeof(s32));
		s = str->s;
		*(s32*)s = (s32)imm;
		return str;
	case Vlong:
		str = mkstrn(sizeof(s32));
		s = str->s;
		*(s32*)s = (s32)imm;
		return str;
	case Vvlong:
		str = mkstrn(sizeof(s64));
		s = str->s;
		*(s64*)s = (s64)imm;
		return str;
	case Vuchar:
		str = mkstrn(sizeof(u8));
		s = str->s;
		*(u8*)s = (u8)imm;
		return str;
	case Vushort:
		str = mkstrn(sizeof(u16));
		s = str->s;
		*(u16*)s = (u16)imm;
		return str;
	case Vuint:
		str = mkstrn(sizeof(u32));
		s = str->s;
		*(u32*)s = (u32)imm;
		return str;
	case Vulong:
		str = mkstrn(sizeof(u32));
		s = str->s;
		*(u32*)s = (u32)imm;
		return str;
	case Vuvlong:
		str = mkstrn(sizeof(u64));
		s = str->s;
		*(u64*)s = (u64)imm;
		return str;
	default:
		fatal("missing case in imm2str");
	}	
}

static void
printval(Val *val)
{
	Cval *cv;
	Closure *cl;
	Pair *pair;
	Range *r;
	Str *str;
	char *o;
	Val bv;
	Tab *tab;
	Type *t;
	Vec *vec;

	if(val == 0){
		printf("(no value)");
		return;
	}

	switch(val->qkind){
	case Qcval:
		cv = valcval(val);
		printf("<cval %llu>", cv->val);
		break;
	case Qcl:
		cl = valcl(val);
		if(cl->id)
			printf("<closure %s>", cl->id);
		else
			printf("<continuation %p>", cl);
		break;
	case Qundef:
		printf("<undefined>");
		break;
	case Qnil:
		printf("<nil>");
		break;
	case Qnulllist:
		printf("<null>");
		break;
	case Qbox:
		printf("<box ");
		valboxed(val, &bv);
		printval(&bv);
		printf(">");
		break;
	case Qpair:
		pair = valpair(val);
		printf("<pair %p>", pair);
		break;
	case Qrange:
		r = valrange(val);
		printf("<range %llu %llu>", r->beg.val, r->len.val);
		break;
	case Qstr:
		str = valstr(val);
		printf("%.*s", (int)str->len, str->s);
		break;
	case Qtab:
		tab = valtab(val);
		printf("<table %p>", vec);
		break;
	case Qtype:
		t = valtype(val);
		o = fmttype(t, xstrdup(""));
		printf("<type %s>", o);
		free(o);
		break;
	case Qvec:
		vec = valvec(val);
		printf("<vector %p>", vec);
		break;
	default:
		printf("<unprintable type %d>", val->qkind);
		break;
	}
}

static Imm
binopimm(ikind op, Imm i1, Imm i2)
{
	switch(op){
	case Iadd:
		return i1+i2;
	case Iand:
		return i1&i2;
	case Idiv:
		return i1/i2;	/* FIXME: trap div by zero */
	case Imod:
		return i1%i2;	/* FIXME: trap div by zero */
	case Imul:
		return i1*i2;
	case Ior:
		return i1|i2;
	case Ishl:
		return i1<<i2;
	case Ishr:
		return i1>>i2;
	case Isub:
		return i1-i2;
	case Ixor:
		return i1^i2;
	case Icmpeq:
		return i1==i2 ? 1 : 0;
	case Icmpneq:
		return i1!=i2 ? 1 : 0;
	case Icmpgt:
		return i1>i2 ? 1 : 0;
	case Icmpge:
		return i1>=i2 ? 1 : 0;
	case Icmplt:
		return i1<i2 ? 1 : 0;
	case Icmple:
		return i1<=i2 ? 1 : 0;
	default:
		fatal("unsupport binary operator %d on immediates", op);
		return 0;
	}
}

static Imm
binopstr(ikind op, Str *s1, Str *s2)
{
	unsigned long len;
	int x;

	switch(op){
	case Icmpeq:
		if(s1->len != s2->len)
			return 0;
		else
			return memcmp(s1->s, s2->s, s1->len) ? 0 : 1;
	case Icmpneq:
		if(s1->len != s2->len)
			return 1;
		else
			return memcmp(s1->s, s2->s, s1->len) ? 1 : 0;
	case Icmpgt:
		len = s1->len > s2->len ? s2->len : s1->len;
		x = memcmp(s1, s2, len);
		return x > 0 ? 1 : 0;
	case Icmpge:
		len = s1->len > s2->len ? s2->len : s1->len;
		x = memcmp(s1, s2, len);
		return x >= 0 ? 1 : 0;
	case Icmplt:
		len = s1->len > s2->len ? s2->len : s1->len;
		x = memcmp(s1, s2, len);
		return x < 0 ? 1 : 0;
	case Icmple:
		len = s1->len > s2->len ? s2->len : s1->len;
		x = memcmp(s1, s2, len);
		return x <= 0 ? 1 : 0;
	default:
		fatal("unsupported binary operator %d on strings", op);
		return 0;
	}
}


static void
xunop(VM *vm, ikind op, Operand *op1, Operand *dst)
{
	Val rv;
	Cval *cv, tmp;
	Imm imm, nv;
	
	cv = getcvalrand(vm, op1, &tmp);
	imm = cv->val;

	switch(op){
	case Ineg:
		nv = -imm;
		break;
	case Iinv:
		nv = ~imm;
		break;
	case Inot:
		if(imm)
			nv = 0;
		else
			nv = 1;
		break;
	default:
		fatal("unknown unary operator %d", op);
	}

	mkvalcval(cv->type, nv, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xbinopcval(VM *vm, ikind op, Operand *op1, Operand *op2, Operand *dst)
{
	Val rv;
	Cval *cv, tmp1, tmp2;
	Imm i1, i2, nv;

	cv = getcvalrand(vm, op1, &tmp1);
	i1 = cv->val;
	cv = getcvalrand(vm, op2, &tmp2);
	i2 = cv->val;

	nv = binopimm(op, i1, i2);
	mkvalcval(cv->type, nv, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xbinop(VM *vm, ikind op, Operand *op1, Operand *op2, Operand *dst)
{
	Val v1, v2, rv;
	Cval *cv;
	Str *s1, *s2;
	Imm i1, i2, nv;

	getvalrand(vm, op1, &v1);
	getvalrand(vm, op2, &v2);
	if(v1.qkind != v2.qkind)
		vmerr(vm, "incompatible operands to binary %s", opstr[op]);

	if(v1.qkind == Qcval){
		cv = valcval(&v1);
		i1 = cv->val;
		cv = valcval(&v2);
		i2 = cv->val;
		nv = binopimm(op, i1, i2);
		mkvalcval(cv->type, nv, &rv);
		putvalrand(vm, &rv, dst);
		return;
	}

	if(v1.qkind == Qstr){
		s1 = valstr(&v1);
		s2 = valstr(&v2);
		nv = binopstr(op, s1, s2); /* assume comparison */
		mkvalcval(0, nv, &rv);
		putvalrand(vm, &rv, dst);
		return;
	}

	fatal("binop on unsupported operands");
}



static void
xclo(VM *vm, Operand *dl, Ctl *label, Operand *dst)
{
	Closure *cl;
	Val rv;
	Cval *cv, tmp;
	Imm m, len;

	/* dl is number of values to copy from stack into display */
	/* label points to instruction in current closure's code */
	/* captured variables are in display order on stack,
	   from low to high stack address */

	cv = getcvalrand(vm, dl, &tmp);
	len = cv->val;

	cl = mkcl(vm->clx->code, label->insn, len, label->label);
	for(m = 0; m < len; m++)
		cl->display[m] = vm->stack[vm->sp+m];
	vm->sp += m;

	mkvalcl(cl, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xkg(VM *vm, Operand *dst)
{
	Closure *k;
	Val rv;
	Imm len;

	len = Maxstk-vm->sp;
	k = mkcl(kcode, 0, len, 0);
	memcpy(k->display, &vm->stack[vm->sp], len*sizeof(Val));
	k->fp = vm->fp;

	mkvalcl(k, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xkp(VM *vm)
{
	Closure *k;
	k = vm->clx;
	vm->fp = k->fp;
	vm->sp = Maxstk-k->dlen;
	memcpy(&vm->stack[vm->sp], k->display, k->dlen*sizeof(Val));
}

static void
xmov(VM *vm, Operand *src, Operand *dst)
{
	Val v;
	getvalrand(vm, src, &v);
	putvalrand(vm, &v, dst);
}

static void
xjnz(VM *vm, Operand *src, Ctl *label)
{
	Cval *cv, tmp;
	cv = getcvalrand(vm, src, &tmp);
	if(cv->val != 0)
		vm->pc = label->insn;
}

static void
xjz(VM *vm, Operand *src, Ctl *label)
{
	Val v;
	getvalrand(vm, src, &v);
	if(zeroval(&v))
		vm->pc = label->insn;
}

static void
checkoverflow(VM *vm, unsigned dec)
{
	if(dec > vm->sp)
		fatal("stack overflow");
}

static void
vmpush(VM *vm, Val *v)
{
	checkoverflow(vm, 1);
	vm->stack[--vm->sp] = *v;
}

static void
vmpop(VM *vm, unsigned n)
{
	vm->sp += n;
}

static void
xpush(VM *vm, Operand *op)
{
	Val v;
	getvalrand(vm, op, &v);
	vmpush(vm, &v);
}

static void
xbox(VM *vm, Operand *op)
{
	Val v, bv;
	getvalrand(vm, op, &v);
	mkvalbox(&v, &bv);
	putvalrand(vm, &bv, op);
}

static void
xbox0(VM *vm, Operand *op)
{
	Val bv;
	mkvalbox(&Xundef, &bv);
	putvalrand(vm, &bv, op);
}

static void
xprint(VM *vm, Operand *op)
{
	Val v;
	getvalrand(vm, op, &v);
	printval(&v);
	printf("\n");
}

static void
xcar(VM *vm, Operand *op, Operand *dst)
{
	Val v;
	Pair *p;
	getvalrand(vm, op, &v);
	if(v.qkind != Qpair)
		vmerr(vm, "car on non-pair");
	p = valpair(&v);
	putvalrand(vm, &p->car, dst);
}

static void
xcdr(VM *vm, Operand *op, Operand *dst)
{
	Val v;
	Pair *p;
	getvalrand(vm, op, &v);
	if(v.qkind != Qpair)
		vmerr(vm, "cdr on non-pair");
	p = valpair(&v);
	putvalrand(vm, &p->cdr, dst);
}

static void
xcons(VM *vm, Operand *car, Operand *cdr, Operand *dst)
{
	Val carv, cdrv, rv;

	getvalrand(vm, car, &carv);
	getvalrand(vm, cdr, &cdrv);
	mkvalpair(&carv, &cdrv, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xrbeg(VM *vm, Operand *op, Operand *dst)
{
	Val v;
	Range *r;
	getvalrand(vm, op, &v);
	if(v.qkind != Qrange)
		vmerr(vm, "rbeg on non-range");
	r = valrange(&v);
	putcvalrand(vm, &r->beg, dst);
}

static void
xrlen(VM *vm, Operand *op, Operand *dst)
{
	Val v;
	Range *r;
	getvalrand(vm, op, &v);
	if(v.qkind != Qrange)
		vmerr(vm, "rlen on non-range");
	r = valrange(&v);
	putcvalrand(vm, &r->len, dst);
}

static void
xrange(VM *vm, Operand *beg, Operand *len, Operand *dst)
{
	Val begv, lenv, rv;

	getvalrand(vm, beg, &begv);
	getvalrand(vm, len, &lenv);
	if(begv.qkind != Qcval || lenv.qkind != Qcval)
		vmerr(vm, "range on non-cval");
	mkvalrange(&begv.u.cval, &lenv.u.cval, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xcval(VM *vm, Operand *type, Operand *str, Operand *dst)
{
	Val typev, strv, rv;
	Imm imm;
	Type *t;

	getvalrand(vm, type, &typev);
	getvalrand(vm, str, &strv);
	t = valtype(&typev);
	imm = str2imm(t, valstr(&strv));
	mkvalcval(t, imm, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xslices(VM *vm, Operand *str, Operand *beg, Operand *end, Operand *dst)
{
	Val strv, begv, endv, rv;
	Cval *b, *e;
	Str *r, *s;

	getvalrand(vm, str, &strv);
	getvalrand(vm, beg, &begv);
	getvalrand(vm, end, &endv);
	s = valstr(&strv);
	b = valcval(&begv);
	e = valcval(&endv);
	if(b->val > s->len)
		vmerr(vm, "string slice out of bounds");
	if(e->val > s->len)
		vmerr(vm, "string slice out of bounds");
	if(b->val > e->val)
		vmerr(vm, "string slice out of bounds");
	r = strslice(s, b->val, e->val);
	mkvalstr(r, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xlens(VM *vm, Operand *str, Operand *dst)
{
	Val strv, rv;
	Str *s;
	getvalrand(vm, str, &strv);
	s = valstr(&strv);
	mkvalcval(0, s->len, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xstr(VM *vm, Operand *cval, Operand *dst)
{
	Val cvalv, rv;
	Str *str;
	Cval *cv;
	
	getvalrand(vm, cval, &cvalv);
	cv = valcval(&cvalv);
	str = mkstrn(cv->val);
	mkvalstr(str, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xlenl(VM *vm, Operand *l, Operand *dst)
{
	Val lv, rv;
	Imm len;
	getvalrand(vm, l, &lv);
	if(listlen(&lv, &len) == 0)
		vmerr(vm, "length on non-list");
	mkvalcval(0, len, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xvecnil(VM *vm, Operand *cval, Operand *dst)
{
	Val cvalv, rv;
	Vec *vec;
	Cval *cv;

	getvalrand(vm, cval, &cvalv);
	cv = valcval(&cvalv);
	vec = mkvecnil(cv->val);
	mkvalvec(vec, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xlenv(VM *vm, Operand *vec, Operand *dst)
{
	Val vecv, rv;
	Vec *v;
	getvalrand(vm, vec, &vecv);
	v = valvec(&vecv);
	mkvalcval(0, v->len, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xvecref(VM *vm, Operand *vec, Operand *idx, Operand *dst)
{
	Val vecv, idxv;
	Vec *v;
	Cval *cv;

	getvalrand(vm, vec, &vecv);
	getvalrand(vm, idx, &idxv);
	v = valvec(&vecv);
	cv = valcval(&idxv);
	/* FIXME: check sign of cv */
	if(cv->val >= v->len)
		vmerr(vm, "vector reference out of bounds");
	putvalrand(vm, vecref(v, cv->val), dst);
}

static void
xvecset(VM *vm, Operand *vec, Operand *idx, Operand *val)
{
	Val vecv, idxv, valv;
	Vec *v;
	Cval *cv;

	getvalrand(vm, vec, &vecv);
	getvalrand(vm, idx, &idxv);
	getvalrand(vm, val, &valv);
	v = valvec(&vecv);
	cv = valcval(&idxv);
	/* FIXME: check sign of cv */
	if(cv->val >= v->len)
		vmerr(vm, "vector set out of bounds");
	vecset(vm, v, cv->val, &valv);
}

static void
xtab(VM *vm, Operand *dst)
{
	Val rv;
	Tab *tab;

	tab = mktab();
	mkvaltab(tab, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xtabdel(VM *vm, Operand *tab, Operand *key)
{
	Val tabv, keyv;
	Tab *t;

	getvalrand(vm, tab, &tabv);
	getvalrand(vm, key, &keyv);
	t = valtab(&tabv);
	tabdel(vm, t, &keyv);
}

static void
xtabenum(VM *vm, Operand *tab, Operand *dst)
{
	Val tabv, rv;
	Tab *t;
	Vec *v;

	getvalrand(vm, tab, &tabv);
	t = valtab(&tabv);
	v = tabenum(t);
	mkvalvec(v, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xtabget(VM *vm, Operand *tab, Operand *key, Operand *dst)
{
	Val tabv, keyv, *rv;
	Tab *t;

	getvalrand(vm, tab, &tabv);
	getvalrand(vm, key, &keyv);
	t = valtab(&tabv);
	rv = tabget(t, &keyv);
	if(rv)
		putvalrand(vm, rv, dst);
	else
		putvalrand(vm, &Xnil, dst);
}

static void
xtabput(VM *vm, Operand *tab, Operand *key, Operand *val)
{
	Val tabv, keyv, valv;
	Tab *t;

	getvalrand(vm, tab, &tabv);
	getvalrand(vm, key, &keyv);
	getvalrand(vm, val, &valv);
	t = valtab(&tabv);
	tabput(vm, t, &keyv, &valv);
}

static void
xxcast(VM *vm, Operand *type, Operand *cval, Operand *dst)
{
	Val typev, cvalv, rv;
	Cval *cv;
	Type *t;

	getvalrand(vm, type, &typev);
	getvalrand(vm, cval, &cvalv);
	t = valtype(&typev);
	cv = valcval(&cvalv);
	mkvalcval(t, cv->val, &rv); /* FIXME: sanity, representation change */
	putvalrand(vm, &rv, dst);
}

static void
xnull(VM *vm, Operand *dst)
{
	putvalrand(vm, &Xnulllist, dst);
}

static void
xiscl(VM *vm, Operand *op, Operand *dst)
{
	Val v, rv;
	getvalrand(vm, op, &v);
	if(v.qkind == Qcl)
		mkvalimm(1, &rv);
	else
		mkvalimm(0, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xiscval(VM *vm, Operand *op, Operand *dst)
{
	Val v, rv;
	getvalrand(vm, op, &v);
	if(v.qkind == Qcval)
		mkvalimm(1, &rv);
	else
		mkvalimm(0, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xisnull(VM *vm, Operand *op, Operand *dst)
{
	Val v, rv;
	getvalrand(vm, op, &v);
	if(v.qkind == Qnulllist)
		mkvalimm(1, &rv);
	else
		mkvalimm(0, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xispair(VM *vm, Operand *op, Operand *dst)
{
	Val v, rv;
	getvalrand(vm, op, &v);
	if(v.qkind == Qpair)
		mkvalimm(1, &rv);
	else
		mkvalimm(0, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xisrange(VM *vm, Operand *op, Operand *dst)
{
	Val v, rv;
	getvalrand(vm, op, &v);
	if(v.qkind == Qrange)
		mkvalimm(1, &rv);
	else

		mkvalimm(0, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xisstr(VM *vm, Operand *op, Operand *dst)
{
	Val v, rv;
	getvalrand(vm, op, &v);
	if(v.qkind == Qstr)
		mkvalimm(1, &rv);
	else
		mkvalimm(0, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xistab(VM *vm, Operand *op, Operand *dst)
{
	Val v, rv;
	getvalrand(vm, op, &v);
	if(v.qkind == Qtab)
		mkvalimm(1, &rv);
	else
		mkvalimm(0, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xistype(VM *vm, Operand *op, Operand *dst)
{
	Val v, rv;
	getvalrand(vm, op, &v);
	if(v.qkind == Qtype)
		mkvalimm(1, &rv);
	else
		mkvalimm(0, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xisvec(VM *vm, Operand *op, Operand *dst)
{
	Val v, rv;
	getvalrand(vm, op, &v);
	if(v.qkind == Qvec)
		mkvalimm(1, &rv);
	else
		mkvalimm(0, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xvlist(VM *vm, Operand *op, Operand *dst)
{
	Val v, *vp;
	Imm sp, n, i;
	Val rv;

	getvalrand(vm, op, &v);
	sp = valimm(&v);
	vp = &vm->stack[sp];
	n = valimm(vp);
	rv = Xnulllist;
	for(i = n; i > 0; i--)
		mkvalpair(&vm->stack[sp+i], &rv, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xvvec(VM *vm, Operand *op, Operand *dst)
{
	Val v, *vp;
	Imm sp, n, i;
	Val rv;
	Vec *vec;

	getvalrand(vm, op, &v);
	sp = valimm(&v);
	vp = &vm->stack[sp];
	n = valimm(vp);
	vec = mkvec(n);
	for(i = 0; i < n; i++)
		_vecset(vec, i, &vm->stack[sp+i+1]);
	mkvalvec(vec, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xencode(VM *vm, Operand *op, Operand *dst)
{
	Val v, rv;
	Str *str;
	Cval *cv;

	getvalrand(vm, op, &v);
	if(v.qkind != Qcval)
		vmerr(vm, "bad operand to encode");
	cv = valcval(&v);
	str = imm2str(cv->type, cv->val);
	mkvalstr(str, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xsizeof(VM *vm, Operand *op, Operand *dst)
{
	Val v, rv;
	Type *t;
	Imm imm;

	getvalrand(vm, op, &v);
	if(v.qkind != Qtype && v.qkind != Qcval)
		vmerr(vm, "bad operand to sizeof");
	if(v.qkind == Qcval)
		vmerr(vm, "sizeof cvalues not implemented");
	t = valtype(&v);
	imm = typesize(vm, t);
	mkvalcval(0, imm, &rv);
	putvalrand(vm, &rv, dst);
}

static void
xtn(VM *vm, u8 bits, Operand *op1, Operand *op2, Operand *dst)
{
	Xtypename *xtn;
	Val rv;

	xtn = mkxtn();
	xtn->xtkind = TBITSTYPE(bits);
	switch(xtn->xtkind){
	case Tbase:
		xtn->basename = TBITSBASE(bits);
		break;
	case Tstruct:
	case Tunion:
	case Tenum:
	case Tptr:
	case Tarr:
	case Tfun:
	case Ttypedef:
		fatal("xtn incomplete");
	default:
		fatal("bug");
	}

	mkvalxtn(xtn, &rv);
	putvalrand(vm, &rv, dst);
}

static void* gotab[Iopmax];

static void
vmsetcl(VM *vm, Closure *cl)
{
	unsigned k;
	Insn *i;
	vm->clx = cl;
	vm->ibuf = vm->clx->code->insn;
	if(vm->ibuf->go == 0){
		i = vm->ibuf;
		for(k = 0; k < vm->clx->code->ninsn; k++){
			i->go = gotab[i->kind];
			i++;
		}
	}
}

void
dovm(VM *vm, Closure *cl)
{
	Insn *i;
	Val val, haltv, zero;
	Closure *halt;
	Imm narg, onarg;

	gotab[Iadd]	= &&Iadd;
	gotab[Iand]	= &&Iand;
	gotab[Ibox]	= &&Ibox;
	gotab[Ibox0]	= &&Ibox0;
	gotab[Icall]	= &&Icall;
	gotab[Icallt]	= &&Icallt;
	gotab[Icar]	= &&Icar;
	gotab[Icdr]	= &&Icdr;
	gotab[Iclo]	= &&Iclo;
	gotab[Icmpeq] 	= &&Icmpeq;
	gotab[Icmpgt] 	= &&Icmpgt;
	gotab[Icmpge] 	= &&Icmpge;
	gotab[Icmplt] 	= &&Icmplt;
	gotab[Icmple] 	= &&Icmple;
	gotab[Icmpneq] 	= &&Icmpneq;
	gotab[Icons] 	= &&Icons;
	gotab[Icval] 	= &&Icval;
	gotab[Iding] 	= &&Iding;
	gotab[Idiv] 	= &&Idiv;
	gotab[Iencode]	= &&Iencode;
	gotab[Iframe] 	= &&Iframe;
	gotab[Igc] 	= &&Igc;
	gotab[Ihalt] 	= &&Ihalt;
	gotab[Iinv] 	= &&Iinv;
	gotab[Iiscl] 	= &&Iiscl;
	gotab[Iiscval] 	= &&Iiscval;
	gotab[Iisnull] 	= &&Iisnull;
	gotab[Iispair] 	= &&Iispair;
	gotab[Iisrange]	= &&Iisrange;
	gotab[Iisstr] 	= &&Iisstr;
	gotab[Iistab] 	= &&Iistab;
	gotab[Iistype] 	= &&Iistype;
	gotab[Iisvec] 	= &&Iisvec;
	gotab[Ijmp] 	= &&Ijmp;
	gotab[Ijnz] 	= &&Ijnz;
	gotab[Ijz] 	= &&Ijz;
	gotab[Ikg] 	= &&Ikg;
	gotab[Ikp] 	= &&Ikp;
	gotab[Ilenl]	= &&Ilenl;
	gotab[Ilens]	= &&Ilens;
	gotab[Ilenv]	= &&Ilenv;
	gotab[Imod] 	= &&Imod;
	gotab[Imov] 	= &&Imov;
	gotab[Imul] 	= &&Imul;
	gotab[Ineg] 	= &&Ineg;
	gotab[Inot] 	= &&Inot;
	gotab[Inull] 	= &&Inull;
	gotab[Ior] 	= &&Ior;
	gotab[Inop] 	= &&Inop;
	gotab[Iprint] 	= &&Iprint;
	gotab[Ipush] 	= &&Ipush;
	gotab[Irange] 	= &&Irange;
	gotab[Irbeg]	= &&Irbeg;
	gotab[Iret] 	= &&Iret;
	gotab[Irlen]	= &&Irlen;
	gotab[Ishl] 	= &&Ishl;
	gotab[Ishr] 	= &&Ishr;
	gotab[Isizeof]	= &&Isizeof;
	gotab[Islices]	= &&Islices;
	gotab[Istr]	= &&Istr;
	gotab[Isub] 	= &&Isub;
	gotab[Itab]	= &&Itab;
	gotab[Itabdel]	= &&Itabdel;
	gotab[Itabenum]	= &&Itabenum;
	gotab[Itabget]	= &&Itabget;
	gotab[Itabput]	= &&Itabput;
	gotab[Itn]	= &&Itn;
	gotab[Ivec] 	= &&Ivec;
	gotab[Ivecref] 	= &&Ivecref;
	gotab[Ivecset] 	= &&Ivecset;
	gotab[Ivlist] 	= &&Ivlist;
	gotab[Ivvec] 	= &&Ivvec;
	gotab[Ixcast] 	= &&Ixcast;
	gotab[Ixor] 	= &&Ixor;

	if(!envlookup(vm->topenv, "halt", &haltv))
		fatal("broken vm");
	halt = valcl(&haltv);

	checkoverflow(vm, 4);
	mkvalimm(0, &zero);
	vmpush(vm, &zero);	/* arbitrary fp */
	vmpush(vm, &haltv);
	mkvalimm(halt->entry, &val);
	vmpush(vm, &val);
	vmpush(vm, &zero);	/* narg */
	vm->fp = vm->sp;

	mkvalcl(cl, &vm->cl);
	vmsetcl(vm, cl);
	vm->pc = vm->clx->entry;

	if(setjmp(vm->esc) != 0)
		return;		/* error throw */

	while(1){
		i = &vm->ibuf[vm->pc++];
		tick++;
		gcpoll(vm);
		goto *(i->go);
		fatal("bug");
	Inop:
		continue;
	Iding:
		printf("ding\n");
		continue;
	Iinv:
	Ineg:
	Inot:
		xunop(vm, i->kind, &i->op1, &i->dst);
		continue;
	Iadd:
	Iand:
	Idiv:
	Imod:
	Imul:
	Ior:
	Ishl:
	Ishr:
	Isub:
	Ixor:
		xbinopcval(vm, i->kind, &i->op1, &i->op2, &i->dst);
		continue;
	Icmplt:
	Icmple:
	Icmpgt:
	Icmpge:
	Icmpeq:
	Icmpneq:
		xbinop(vm, i->kind, &i->op1, &i->op2, &i->dst);
		continue;
	Imov:
		xmov(vm, &i->op1, &i->dst);
		continue;
	Ipush:
		xpush(vm, &i->op1);
		continue;
	Icall:
		getvalrand(vm, &i->op1, &vm->cl);
		vmsetcl(vm, valcl(&vm->cl));
		vm->pc = vm->clx->entry;
		vm->fp = vm->sp;
		continue;
	Icallt:
		getvalrand(vm, &i->op1, &vm->cl);
		vmsetcl(vm, valcl(&vm->cl));
		/* shift current arguments over previous arguments */
		narg = valimm(&vm->stack[vm->sp]);
		onarg = valimm(&vm->stack[vm->fp]);
		vm->fp = vm->fp+onarg-narg;
		memmove(&vm->stack[vm->fp], &vm->stack[vm->sp],
			(narg+1)*sizeof(Val));
		vm->sp = vm->fp;
		vm->pc = vm->clx->entry;
		continue;
	Iframe:
		mkvalimm(vm->fp, &val);
		vmpush(vm, &val);
		vmpush(vm, &vm->cl);
		mkvalimm(i->dstlabel->insn, &val);
		vmpush(vm, &val);
		continue;
	Igc:
		// gc(vm);
		continue;
	Iret:
		vm->sp = vm->fp+valimm(&vm->stack[vm->fp])+1;/*narg+1*/
		vm->fp = valimm(&vm->stack[vm->sp+2]);
		vm->cl = vm->stack[vm->sp+1];
		vmsetcl(vm, valcl(&vm->cl));
		vm->pc = valimm(&vm->stack[vm->sp]);
		vmpop(vm, 3);
		continue;
	Ijmp:
		vm->pc = i->dstlabel->insn;
		continue;
	Ijnz:
		xjnz(vm, &i->op1, i->dstlabel);
		continue;
	Ijz:
		xjz(vm, &i->op1, i->dstlabel);
		continue;
	Iclo:
		xclo(vm, &i->op1, i->dstlabel, &i->dst); 
		/* vm->sp has been updated */
		continue;
	Ikg:
		xkg(vm, &i->dst);
		continue;
	Ikp:
		xkp(vm);
		/* vm->sp, vm->fp have been updated */
		continue;
	Ibox:
		xbox(vm, &i->op1);
		continue;
	Ibox0:
		xbox0(vm, &i->op1);
		continue;
	Iprint:
		xprint(vm, &i->op1);
		continue;
	Ihalt:
		printf("halted (ac = ");
		printval(&vm->ac);
		printf(")\n");
		return;
	Icar:
		xcar(vm, &i->op1, &i->dst);
		continue;
	Icdr:
		xcdr(vm, &i->op1, &i->dst);
		continue;
	Icons:
		xcons(vm, &i->op1, &i->op2, &i->dst);
		continue;
	Irbeg:
		xrbeg(vm, &i->op1, &i->dst);
		continue;
	Irlen:
		xrlen(vm, &i->op1, &i->dst);
		continue;
	Irange:
		xrange(vm, &i->op1, &i->op2, &i->dst);
		continue;
	Icval:
		xcval(vm, &i->op1, &i->op2, &i->dst);
		continue;
	Istr:
		xstr(vm, &i->op1, &i->dst);
		continue;
	Islices:
		xslices(vm, &i->op1, &i->op2, &i->op3, &i->dst);
		continue;
	Ilens:
		xlens(vm, &i->op1, &i->dst);
		continue;
	Ilenl:
		xlenl(vm, &i->op1, &i->dst);
		continue;
	Ivec:
		xvecnil(vm, &i->op1, &i->dst);
		continue;
	Ivecref:
		xvecref(vm, &i->op1, &i->op2, &i->dst);
		continue;
	Ivecset:
		xvecset(vm, &i->op1, &i->op2, &i->op3);
		continue;
	Itab:
		xtab(vm, &i->dst);
		continue;
	Itabdel:
		xtabdel(vm, &i->op1, &i->op2);
		continue;
	Itabenum:
		xtabenum(vm, &i->op1, &i->dst);
		continue;
	Itabget:
		xtabget(vm, &i->op1, &i->op2, &i->dst);
		continue;
	Itabput:
		xtabput(vm, &i->op1, &i->op2, &i->op3);
		continue;
	Ilenv:
		xlenv(vm, &i->op1, &i->dst);
		continue;
	Ixcast:
		xxcast(vm, &i->op1, &i->op2, &i->dst);
		continue;
	Inull:
		xnull(vm, &i->dst);
		continue;
	Iiscval:
		xiscval(vm, &i->op1, &i->dst);
		continue;
	Iiscl:
		xiscl(vm, &i->op1, &i->dst);
		continue;
	Iisnull:
		xisnull(vm, &i->op1, &i->dst);
		continue;
	Iispair:
		xispair(vm, &i->op1, &i->dst);
		continue;
	Iisrange:
		xisrange(vm, &i->op1, &i->dst);
		continue;
	Iisstr:
		xisstr(vm, &i->op1, &i->dst);
		continue;
	Iistab:
		xistab(vm, &i->op1, &i->dst);
		continue;
	Iistype:
		xistype(vm, &i->op1, &i->dst);
		continue;
	Iisvec:
		xisvec(vm, &i->op1, &i->dst);
		continue;
	Ivlist:
		xvlist(vm, &i->op1, &i->dst);
		continue;
	Ivvec:
		xvvec(vm, &i->op1, &i->dst);
		continue;
	Iencode:
		xencode(vm, &i->op1, &i->dst);
		continue;
	Isizeof:
		xsizeof(vm, &i->op1, &i->dst);
		continue;
	Itn:
		xtn(vm, i->bits, &i->op1, &i->op2, &i->dst);
		continue;
	}
}

static void
builtinfn(Env *env, char *name, Closure *cl)
{
	Val val;
	mkvalcl(cl, &val);
	envbind(env, name, &val);
}

static void
builtinstr(Env *env, char *name, char *s)
{
	Val val;
	mkvalstr(mkstr0(s), &val);
	envbind(env, name, &val);
}

VM*
mkvm(Env *env)
{
	VM *vm;
	
	vm = xmalloc(sizeof(VM));
	vm->sp = Maxstk;
	vm->ac = Xundef;
	vm->topenv = env;
	
	builtinfn(env, "gc", gcthunk());
	builtinfn(env, "ding", dingthunk());
	builtinfn(env, "print", printthunk());
	builtinfn(env, "halt", haltthunk());
	builtinfn(env, "callcc", callcc());
	builtinfn(env, "car", carthunk());
	builtinfn(env, "cdr", cdrthunk());
	builtinfn(env, "cons", consthunk());
	builtinfn(env, "rangebeg", rangebegthunk());
	builtinfn(env, "rangelen", rangelenthunk());
	builtinfn(env, "range", rangethunk());
	builtinfn(env, "null", nullthunk());
	builtinfn(env, "iscvalue", iscvaluethunk());
	builtinfn(env, "isprocedure", isprocedurethunk());
	builtinfn(env, "isnull", isnullthunk());
	builtinfn(env, "ispair", ispairthunk());
	builtinfn(env, "isrange", israngethunk());
	builtinfn(env, "isstring", isstringthunk());
	builtinfn(env, "istable", istablethunk());
	builtinfn(env, "istype", istypethunk());
	builtinfn(env, "isvector", isvectorthunk());
	builtinfn(env, "string", stringthunk());
	builtinfn(env, "strlen", strlenthunk());
	builtinfn(env, "substr", substrthunk());
	builtinfn(env, "table", tablethunk());
	builtinfn(env, "tabinsert", tabinsertthunk());
	builtinfn(env, "tabdelete", tabdeletethunk());
	builtinfn(env, "tabenum", tabenumthunk());
	builtinfn(env, "tablook", tablookthunk());
	builtinfn(env, "mkvec", mkvecthunk());
	builtinfn(env, "vector", vectorthunk());
	builtinfn(env, "veclen", veclenthunk());
	builtinfn(env, "vecref", vecrefthunk());
	builtinfn(env, "vecset", vecsetthunk());

	builtinstr(env, "$get", "get");
	builtinstr(env, "$put", "put");
	builtinstr(env, "$looksym", "looksym");
	builtinstr(env, "$looktype", "looktype");

	concurrentgc(vm);

	return vm;
}

void
freevm(VM *vm)
{
	gckill(vm);
	free(vm);
}

void
initvm()
{
	Xundef.qkind = Qundef;
	Xnil.qkind = Qnil;
	Xnulllist.qkind = Qnulllist;

	heapbox.id = "box";
	heapcl.id = "closure";
	heapcode.id = "code";
	heapcval.id = "cval";
	heappair.id = "pair";
	heaprange.id = "range";
	heapstr.id = "string";
	heaptab.id = "table";
	heapvec.id = "vector";
	heapxtn.id = "typename";

	heapbox.sz = sizeof(Box);
	heapcl.sz = sizeof(Closure);
	heapcode.sz = sizeof(Code);
	heapcval.sz = sizeof(Vcval);
	heappair.sz = sizeof(Pair);
	heaprange.sz = sizeof(Range);
	heapstr.sz = sizeof(Str);
	heaptab.sz = sizeof(Tab);
	heapvec.sz = sizeof(Vec);
	heapxtn.sz = sizeof(Xtypename);

	heapcode.free1 = freecode;
	heapcl.free1 = freecl;
	heapstr.free1 = freestr;
	heaptab.free1 = freetab;
	heapvec.free1 = freevec;

	heapbox.iter = iterbox;
	heapcl.iter = itercl;
	heappair.iter = iterpair;
	heaptab.iter = itertab;
	heapvec.iter = itervec;
	heapxtn.iter = iterxtn;
	/* FIXME: itercval? to walk Xtype */

	kcode = contcode();

	roots.getlink = getrootslink;
	roots.setlink = setrootslink;
	roots.getolink = getstoreslink;

	stores.getlink = getstoreslink;
	stores.setlink = setstoreslink;
	stores.getolink = getrootslink;

	Qhash[Qundef] = nohash;
	Qhash[Qnil] = hashptrv;
	Qhash[Qnulllist] = hashptrv;
	Qhash[Qcval] = hashcval;
	Qhash[Qcl] = hashptr;
	Qhash[Qbox] = nohash;
	Qhash[Qpair] = hashptr;
	Qhash[Qrange] = hashrange;
	Qhash[Qstr] = hashstr;
	Qhash[Qtab] = hashptr;
	Qhash[Qtype] = hashptr;
	Qhash[Qvec] = hashptr;

	Qeq[Qundef] = 0;
	Qeq[Qnil] = eqptrv;
	Qeq[Qnulllist] = eqptrv;
	Qeq[Qcval] = eqcval;
	Qeq[Qcl] = eqptr;
	Qeq[Qbox] = 0;
	Qeq[Qpair] = eqptr;
	Qeq[Qrange] = eqrange;
	Qeq[Qstr] = eqstr;
	Qeq[Qtab] = eqptr;
	Qeq[Qtype] = eqptr;
	Qeq[Qvec] = eqptr;

	gcreset();
}

void
finivm()
{
	gcreset();		/* clear store set (FIXME: still needed?) */
	gc(0);
	gc(0);	/* must run two epochs without mutator to collect everything */

	freecode((Head*)kcode);

	freeheap(&heapcode);
	freeheap(&heapcl); 
	freeheap(&heapbox);
	freeheap(&heapcval);
	freeheap(&heappair);
	freeheap(&heaprange);
	freeheap(&heapstr);
	freeheap(&heaptab);
	freeheap(&heapvec);
}
