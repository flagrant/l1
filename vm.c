#include "sys.h"
#include "util.h"
#include "syscqct.h"

#define HEAPPROF 0
#define HEAPDEBUG 0

char *qname[Qnkind] = {
	[Qundef]=	"undefined",
	[Qnil]=		"nil",
	[Qnull]=	"null",
	[Qas]=		"address space",
	[Qbox]=		"box",
	[Qcl]=		"closure",
	[Qcode]=	"code",
	[Qcval]=	"cvalue",
	[Qdom]=		"domain",
	[Qfd]=		"file descriptor",
	[Qlist]=	"list",
	[Qns]=		"name space",
	[Qpair]=	"pair",
	[Qrange]=	"range",
	[Qrec]=		"record",
	[Qrd]=		"record descriptor",
	[Qstr]=		"string",
	[Qtab]=		"table",
	[Qvec]=		"vector",
	[Qxtn]=		"ctype",
};

static Imm repsize[Rnrep] = {
	[Ru08le]=	1,
	[Ru16le]=	2,
	[Ru32le]=	4,
	[Ru64le]=	8,
	[Rs08le]=	1,
	[Rs16le]=	2,
	[Rs32le]=	4,
	[Rs64le]=	8,
	[Ru08be]=	1,
	[Ru16be]=	2,
	[Ru32be]=	4,
	[Ru64be]=	8,
	[Rs08be]=	1,
	[Rs16be]=	2,
	[Rs32be]=	4,
	[Rs64be]=	8,
};

unsigned isunsigned[Vnbase] = {
	[Vuchar] = 1,
	[Vushort] = 1,
	[Vuint] = 1,
	[Vulong] = 1,
	[Vuvlong] = 1,
};

static unsigned isbigendian[Rnrep] = {
	[Ru08be]=	1,
	[Ru16be]=	1,
	[Ru32be]=	1,
	[Ru64be]=	1,
	[Rs08be]=	1,
	[Rs16be]=	1,
	[Rs32be]=	1,
	[Rs64be]=	1,
};

struct Heap {
	char *id;
	Qkind qkind;
	unsigned sz;
	unsigned clearit;
	Freeheadfn free1;
	Head* (*iter)(Head *hd, Ictx *ictx);
	Head *alloc, *swept, *sweep, *free;
	unsigned long nalloc, nfree, nha;    /* statistics */
};

static void *GCiterdone;

#define GCCOLOR(i) ((i)%3)
enum {
	GCfree = 3,
	GCfinal = 4,
	GCrate = 100000,
	GCinitprot = 128,	/* initial # of GC-protected lists in VM */
	/* gc->state */
};

typedef
enum GCstate
{
	GCenabled = 1,
	GCshutdown = 2,
} GCstate;

static unsigned long long nextgctick = GCrate;

typedef struct GC GC;
struct GC {
	GCstate state;
	void (*gcinit)(GC*);
	void (*gcpoll)(GC*, VM*);
	void (*gckill)(GC*);
	unsigned char gcpause;	/* gc is waiting for mutator */
	unsigned char gcrun;	/* gc is running */
	Thread t;
	int cm;			/* mutator->gc channel */
	int cgc;		/* gc->mutator channel */
	Rootset roots;
	Rootset stores;
	Rootset finals;
	u64 heapmax, heaphi;
};

static void dogc(GC*);
static void vmsetcl(VM *vm, Val val);
static void gcprotpush(VM *vm);
static void gcprotpop(VM *vm);
static Xtypename* dolooktype(VM *vm, Xtypename *xtn, Ns *ns);
static Xtypename* mkvoidxtn();
static Xtypename* mkbasextn(Cbase name, Rkind rep);
static Xtypename* mkptrxtn(Xtypename *t, Rkind rep);
static Xtypename* mkconstxtn(Xtypename *t);
static Xtypename* mktypedefxtn(Str *tid, Xtypename *t);
static Xtypename* mkundefxtn(Xtypename *t);
static Env* mktopenv();
static void l1_sort(VM *vm, Imm argc, Val *argv, Val *rv);
static List* mklistn(u32 sz);
static Val listref(VM *vm, List *lst, Imm idx);
static List* listset(VM *vm, List *lst, Imm idx, Val v);
static void addroot(Rootset *rs, Head *h);
static Head* removeroot(Rootset *rs);
static int issym(Vec *sym);
static void setgo(Insn *i, Imm lim);

static struct GC *thegc;
static Val Xundef;
static Val Xnil;
static Val Xnulllist;
static Dom *litdom;
static Ns *litns;
static Xtypename **litbase;
static Tab *finals;

Cval *cvalnull, *cval0, *cval1, *cvalminus1;
VM *vms[Maxvms];
static unsigned nvms;
static unsigned long long tick;
static unsigned long gcepoch = 2;

static char *opstr[Iopmax] = {
	[Iadd] = "+",
	[Iand] = "&",
	[Idiv] = "/",
	[Iinv] = "~",
	[Imod] = "%",
	[Imul] = "*",
	[Ineg] = "-",
	[Inot] = "!",
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

static int freecl(Head*);
static int freefd(Head*);
static int freelist(Head*);
static int freerec(Head*);
static int freestr(Head*);
static int freetab(Head*);
static int freevec(Head*);

static Head *iteras(Head*, Ictx*);
static Head *iterbox(Head*, Ictx*);
static Head *itercl(Head*, Ictx*);
static Head *itercode(Head*, Ictx*);
static Head *itercval(Head*, Ictx*);
static Head *iterdom(Head*, Ictx*);
static Head *iterfd(Head*, Ictx*);
static Head *iterlist(Head*, Ictx*);
static Head *iterrd(Head*, Ictx*);
static Head *iterrec(Head*, Ictx*);
static Head *iterns(Head*, Ictx*);
static Head *iterpair(Head*, Ictx*);
static Head *iterrange(Head*, Ictx*);
static Head *itertab(Head*, Ictx*);
static Head *itervec(Head*, Ictx*);
static Head *iterxtn(Head*, Ictx*);

static Heap heap[Qnkind] = {
	[Qas]	 = { "as", Qas, sizeof(As), 1, 0, iteras },
	[Qbox]	 = { "box", Qbox, sizeof(Box),	0, 0, iterbox },
	[Qcval]  = { "cval", Qcval, sizeof(Cval), 0, 0, itercval },
	[Qcl]	 = { "closure", Qcl, sizeof(Closure), 1, freecl, itercl },
	[Qcode]	 = { "code", Qcode, sizeof(Code), 1, freecode, itercode },
	[Qdom]	 = { "domain", Qdom, sizeof(Dom), 0, 0, iterdom },
	[Qfd]	 = { "fd", Qfd,sizeof(Fd), 0, freefd, iterfd },
	[Qlist]	 = { "list", Qlist, sizeof(List), 0, freelist, iterlist },
	[Qns]	 = { "ns", Qns, sizeof(Ns), 1, 0, iterns },
	[Qpair]	 = { "pair", Qpair, sizeof(Pair), 0, 0, iterpair },
	[Qrange] = { "range", Qrange, sizeof(Range), 0, 0, iterrange },
	[Qrd]    = { "rd", Qrd, sizeof(Rd), 0, 0, iterrd },
	[Qrec]	 = { "record", Qrec, sizeof(Rec), 0, freerec, iterrec },
	[Qstr]	 = { "string", Qstr, sizeof(Str), 1, freestr, 0 },
	[Qtab]	 = { "table", Qtab, sizeof(Tab), 1, freetab, itertab },
	[Qvec]	 = { "vector", Qvec, sizeof(Vec), 0, freevec, itervec },
	[Qxtn]	 = { "typename", Qxtn, sizeof(Xtypename), 1, 0, iterxtn },
};

static u32 nohash(Val);
static u32 hashcval(Val);
static u32 hashptr(Val);
static u32 hashconst(Val);
static u32 hashlist(Val);
static u32 hashrange(Val);
static u32 hashstr(Val);
static u32 hashvec(Val);
static u32 hashxtn(Val);

static int eqcval(Val, Val);
static int eqptr(Val, Val);
static int eqtrue(Val, Val);

static int equalcval(Val, Val);
static int equallistv(Val a, Val b);
static int equalrange(Val, Val);
static int equalstrv(Val, Val);
static int equalvecv(Val a, Val b);
static int equalxtn(Xtypename*, Xtypename*);
static int equalxtnv(Val, Val);

typedef struct Hashop {
	u32 (*hash)(Val);
	int (*eq)(Val, Val);
} Hashop;

static Hashop hashop[Qnkind] = {
	[Qundef] = { nohash, 0 },
	[Qnil]	 = { hashconst, eqtrue },
	[Qnull]  = { hashconst, eqtrue },
	[Qas]	 = { hashptr, eqptr },
	[Qbox]	 = { nohash, 0 },
	[Qcl]	 = { hashptr, eqptr },
	[Qcode]	 = { nohash, 0 },
	[Qcval]	 = { hashcval, eqcval },
	[Qdom]	 = { hashptr, eqptr },
	[Qfd]	 = { hashptr, eqptr },
	[Qlist]	 = { hashlist, equallistv },
	[Qns]	 = { hashptr, eqptr },
	[Qpair]	 = { hashptr, eqptr },
	[Qrange] = { hashrange, equalrange },
	[Qrd]    = { hashptr, eqptr },
	[Qrec]   = { hashptr, eqptr },
	[Qstr]	 = { hashstr, equalstrv },
	[Qtab]	 = { hashptr, eqptr },
	[Qvec]	 = { hashvec, equalvecv },
	[Qxtn]	 = { hashxtn, equalxtnv },
};

static Code *kcode, *cccode;

static Imm
stkimm(Val v)
{
	Imm imm;
	imm = (Imm)(uintptr_t)v;
	if((imm&1) != 1)
		fatal("stkimm on non-imm");
	imm >>= 1;
	return imm;
}

Imm
valimm(Val v)
{
	Cval *cv;
	if(Vkind(v) != Qcval)
		fatal("valimm on non-cval");
	cv = (Cval*)v;
	return cv->val;
}

Val
valboxed(Val v)
{
	Box *b;
	if(Vkind(v) != Qbox)
		fatal("valboxed on non-box");
	b = (Box*)v;
	return b->v;
}

static u64
rdtsc()
{
	u32 hi, lo;
	asm("rdtsc" : "=a"(lo), "=d"(hi));
	return (u64)lo|((u64)hi<<32);
}

static void*
read_and_clear(void *pp)
{
	void **p = (void**)pp;
	void *q;
	q = 0;
	asm("xchg %0,%1" : "=r"(q), "=m"(*p) : "0"(q), "m"(*p) : "memory");
	return q;
}

static inline
void atomic_inc(int *p)
{
	asm("lock incl %0" : "+m" (*p));
}

static inline
void atomic_dec(int *p)
{
	asm("lock decl %0" : "+m" (*p));
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

static unsigned long
hlen(Head *h)
{
	unsigned n;
	n = 0;
	while(h){
		n++;
		h = h->link;
	}
	return n;
}

static u64
szlivehead(Head *h)
{
	Closure *cl;
	Fd *fd;
	List *l;
	Rec *r;
	Str *s;
	Tab *t;
	Vec *v;
	u64 m;

	m = 0;
	switch(Vkind(h)){
	case Qcode:
		m += szcode((Code*)h);
		break;
	case Qcl:
		cl = (Closure*)h;
		m += esize(cl->display);
		m += esize(cl->id);
		break;
	case Qfd:
		fd = (Fd*)h;
		break;
	case Qlist:
		l = (List*)h;
		m += esize(l->x->val);
		m += esize(l->x->oval);
		m += esize(l->x);
		break;
	case Qrec:
		r = (Rec*)h;
		m += esize(r->field);
		break;
	case Qstr:
		s = (Str*)h;
		switch(s->skind){
		case Smmap:
			m += s->mlen;
			break;
		case Smalloc:
			m += esize(s->s);
			break;
		case Sperm:
			/* no bookkeeping on permanent strings */
			break;
		}
		break;
	case Qtab:
		t = (Tab*)h;
		m += esize(t->x->val);
		m += esize(t->x->key);
		m += esize(t->x->idx);
		m += esize(t->x);
		break;
	case Qvec:
		v = (Vec*)h;
		m += esize(v->vec);
		break;
	default:
		break;
	}

	return m;
}


static void
tabstat1(void *u, char *k, void *v)
{
	unsigned m;
	m = (unsigned)(uintptr_t)v;
	printf("%s\t%u\n", k, m);
	efree(k);
}

static void
tabstat()
{
	Heap *hp;
	Head *p;
	HT *ht;
	Tab *t;
	char buf[32];
	void *v;
	unsigned m;

	ht = mkht();
	hp = &heap[Qtab];
	p = hp->alloc;
	while(p){
		if(Vcolor(p) == GCfree)
			goto next;
		t = (Tab*)p;
		snprint(buf, sizeof(buf), "%u", t->x->sz);
		v = hget(ht, buf, strlen(buf));
		if(v == 0)
			hput(ht, xstrdup(buf), strlen(buf),
			     (void*)(uintptr_t)1);
		else{
			m = (unsigned)(uintptr_t)v;
			hput(ht, buf, strlen(buf), (void*)(uintptr_t)(m+1));
		}
		if(t->x->sz == 1024)
			printf("%d\n", t->cnt);
	next:
		p = p->alink;
		continue;
	}
	hforeach(ht, tabstat1, 0);
	freeht(ht);
}

static void
heapstatmem(Heap *hp, u64 *mlp, u64 *mfp)
{
	u64 ml, mf;
	Head *p;

	ml = 0;
	mf = 0;
	p = hp->alloc;
	while(p){
		if(Vcolor(p) == GCfree)
			mf += esize(p);
		else
			ml += esize(p)+szlivehead(p);
		p = p->alink;
	}
	*mlp = ml;
	*mfp = mf;
	if(0)
		tabstat();
}

static void
heapstat(char *s)
{
	unsigned i;
	Heap *hp;
	unsigned long sz, nf, na, ns0, ns1;
	u64 ml, mf;
	
	if(s)
		xprintf("%s ", s);
	xprintf("gc epoch: %lu\n", gcepoch);
	xprintf("%-10s %10s %10s %10s %10s %10s %10s %10s %10s\n",
		"heap", "base size",
		"inuse", "free",
		"nalloc", "nfree",
		"nswept", "nsweep", "total");
	xprintf("-------------------------------------------"
		"-------------------------------------------------------\n");
	for(i = 0; i < Qnkind; i++){
		hp = &heap[i];
		if(hp->id){
			sz = hp->sz;
			na = hp->nalloc;
			nf = hp->nfree;
			ns0 = hlen(hp->swept);
			ns1 = hlen(hp->sweep);
			heapstatmem(hp, &ml, &mf);
			xprintf("%-10s %10lu %10lu %10lu %10lu "
				"%10lu %10lu %10lu %10lu",
				hp->id,
				sz,
				(unsigned long)ml, (unsigned long)mf,
				na, nf, ns0, ns1, hp->nha);
			if(na > nf+ns0+ns1)
				xprintf(" (%lu not collected)\n",
					na-(nf+ns0+ns1));
			else if(na < nf+ns0+ns1)
				xprintf(" (inconsistent alloc count!)\n");
			else
				xprintf("\n");
		}
	}
}

// FIXME: remove sanity checks
static Head*
halloc(Heap *heap)
{
	Head *o, *ap, *fp;
	unsigned m;

retry:
	if(heap->free){
		o = heap->free;
		heap->free = o->link;
		o->link = 0;
//		if(o->state != -1)
//			fatal("halloc bad state %d", o->state);
//		o->state = 0;
		if(HEAPPROF) heap->nfree--;
	}else if(heap->swept){
		heap->free = (Head*)read_and_clear(&heap->swept);
		if(HEAPPROF) heap->nfree += hlen(heap->free);
		goto retry;
	}else{
		if(thegc->heapmax && cqctmeminuse >= thegc->heapmax)
			fatal("heap size limit reached");
		ap = heap->alloc;
		fp = 0;
		for(m = 0; m < AllocBatch; m++){
			o = emalloc(heap->sz);
//			VALGRIND_MAKE_MEM_NOACCESS(o+1, heap->sz-sizeof(Head));
//			o->heap = heap;
			Vsetkind(o, heap->qkind);
			o->alink = ap;
			o->link = fp;
			Vsetcolor(o, GCfree);
//			o->state = -1;
			ap = o;
			fp = o;
		}
		heap->alloc = o;
		heap->free = o;
		if(HEAPPROF) heap->nalloc += AllocBatch;
		if(HEAPPROF) heap->nfree += AllocBatch;
		goto retry;
	}
	
	Vsetcolor(o, GCCOLOR(gcepoch));
	if(HEAPPROF) heap->nha++;
	if(HEAPDEBUG)
		xprintf("alloc %p (%s)\n", o, heap->id);

	// FIXME: only object types that do not initialize all fields
	// (e.g., Xtypename) need to be cleared.  Perhaps add a bit
	// to heap to select clearing.
//	VALGRIND_MAKE_MEM_UNDEFINED(o+1, heap->sz-sizeof(Head));
	if(heap->clearit)
		memset(o+1, 0, heap->sz-sizeof(Head));
	return o;
}

void
heapfree(Head *p)
{
	Head **sp;
	if(HEAPDEBUG)
		xprintf("collect %s %p\n", heap[Vkind(p)].id, p); 
//	if(p->state != 0 || Vinrs(p))
//		fatal("sweep heap (%s) %p bad state %d",
//		      heap[Vkind(p)].id, p, p->state);
	sp = &heap[Vkind(p)].sweep;
	p->link = *sp;
	*sp = p;
//	p->state = -1;
	Vsetcolor(p, GCfree);
	Vsetfinal(p, 0);
//	p->final = 0;
//	VALGRIND_MAKE_MEM_NOACCESS(p+1, p->heap->sz-sizeof(Head));
}

static void
sweepheap(GC *gc, Heap *heap, unsigned color)
{
	Head *p;
	p = heap->alloc;
	while(p){
		if(Vcolor(p) == color){
//			xprintf("collecting %p\n", p);
			if(Vfinal(p) && !(gc->state&GCshutdown)){
				Vsetcolor(p, GCfinal);
//xprintf("adding final %p\n", p);
				addroot(&thegc->finals, p);
				goto next;
			}
			Vsetcolor(p, GCfree);
			if(heap->free1 && heap->free1(p) == 0)
				goto next; /* deferred free */
			heapfree(p);
		}
next:
		p = p->alink;
	}
	if(heap->swept == 0){
		heap->swept = heap->sweep;
		heap->sweep = 0;
	}
}

static void
sweep(GC *gc, unsigned color)
{
	unsigned i;
	Heap *hp;

	for(i = 0; i < Qnkind; i++){
		hp = &heap[i];
		if(hp->id)
			sweepheap(gc, hp, color);
	}
}

static void
freeheap(Heap *heap)
{
	Head *p, *q;
again:
	if(heap->free){
		p = heap->free;
		while(p){
			q = p->link;
			efree(p);
			p = q;
		}
	}
	if(heap->swept){
		heap->free = (Head*)read_and_clear(&heap->swept);
		goto again;
	}
	if(heap->sweep){
		heap->free = (Head*)read_and_clear(&heap->sweep);
		goto again;
	}
}

Freeheadfn
getfreeheadfn(Qkind qkind)
{
	return heap[qkind].free1;
}

void
setfreeheadfn(Qkind qkind, Freeheadfn free1)
{
	heap[qkind].free1 = free1;
}

Head*
valhead(Val v)
{
	Imm imm;

	if(v == 0)
		return 0;

	imm = (Imm)(uintptr_t)v;
	if((imm&1) != 0)
		/* stack immediate */
		return 0;

	switch(Vkind(v)){
	case Qundef:
	case Qnil:
	case Qnull:
		return 0;
		break;
	default:
		return (Head*)v;
		break;
	}
}

static Root*
newroot(Rootset *rs)
{
	Root *r;
	if(rs->free){
		r = rs->free;
		rs->free = r->link;
		return r;
	}
	return emalloc(sizeof(Root));
}

static void
freeroot(Rootset *rs, Root *r)
{
	r->link = rs->free;
	rs->free = r;
}

static void
freerootlist(Rootset *rs, Root *r)
{
	Root *nxt;
	while(r){
		nxt = r->link;
		freeroot(rs, r);
		r = nxt;
	}
}

static void
freefreeroots(Rootset *rs)
{
	Root *r, *nxt;
	r = rs->free;
	while(r){
		nxt = r->link;
		efree(r);
		r = nxt;
	}
}

static void
doaddroot(Rootset *rs, Head *h)
{
	Root *r;

	/* test if already on a rootlist */
//	x = h->state;
//	if(x > 2 || x < 0)
//		fatal("addroot %p (%s) bad state %d", h, heap[Vkind(h)].id, x);
//	atomic_inc(&h->state);

	r = newroot(rs);
	Vsetinrs(h, 1);
	r->hd = h;
	r->link = rs->roots;
	writebarrier();
	rs->roots = r;
}

/* called on roots by marker.  called on stores by mutator. */
static void
addroot(Rootset *rs, Head *h)
{
	if(h == 0)
		return;

	if(Vcolor(h) == GCfree)
		/* stale value on stack already collected */
		return;
	if(Vinrs(h))
		return;

	doaddroot(rs, h);
}

static Root*
firstroot(Rootset *rs)
{
	if(rs->this == rs->before_last){
		rs->before_last = rs->last;
		rs->this = rs->last = rs->roots;
	}
	if(rs->this == rs->before_last)
		return 0;
	return rs->this;
}

static Head*
removeroot(Rootset *rs)
{
	Head *h;
	Root *r;

	if(rs->this == rs->before_last){
		rs->before_last = rs->last;
		rs->this = rs->last = rs->roots;
	}
	if(rs->this == rs->before_last)
		return 0;
	r = rs->this;
	rs->this = r->link;
	h = r->hd;
//	if(HEAPDEBUG)
//		xprintf("rmroot %p %d %d\n", h, Vinrs(h), h->state);
	Vsetinrs(h, 0);
//	x = h->state;
//	if(x > 2 || x <= 0)
//		fatal("remove root %p (%s) bad state %d", h, heap[Vkind(h)].id,
//		      x);
//	atomic_dec(&h->state);
	return h;
}

static int
rootsetempty(Rootset *rs)
{
	return rs->before_last == rs->roots;
}

static void
markhead(GC *gc, Head *hd, unsigned color)
{
	Ictx ictx;
	Head *c;

	if(hd == 0)
		return;

	if(Vcolor(hd) == color)
		return;

	Vsetcolor(hd, color);
//	addroot(&gc->roots, (Head*)hd->final);
	if(heap[Vkind(hd)].iter == 0)
		return;
	memset(&ictx, 0, sizeof(ictx));
	while(1){
		c = heap[Vkind(hd)].iter(hd, &ictx);
		if(c == GCiterdone)
			break;
		if(c && Vcolor(c) != color)
			addroot(&gc->roots, c);
	}
}

static void
markrs(GC *gc, Rootset *rs, unsigned color)
{
	Head *h;

	while(1){
		h = removeroot(rs);
		if(h == 0)
			break;
		markhead(gc, h, color);
	}
}

static void
mark(GC *gc, unsigned color)
{
	markrs(gc, &gc->stores, GCCOLOR(gcepoch));
	markrs(gc, &gc->roots, GCCOLOR(gcepoch));
}

static void
bindingroot(void *u, char *k, void *v)
{
	GC *gc;
	gc = u;
	addroot(&gc->roots, valhead(*(Val*)v));
}

static void
rdroot(void *u, char *k, void *v)
{
	GC *gc;
	gc = u;
	addroot(&gc->roots, (Head*)v);
}

static void
rootset(GC *gc)
{
	unsigned m;
	Root *r;
	VM **vmp, *vm;

	/* never collect these things */
	addroot(&gc->roots, (Head*)kcode); 
	addroot(&gc->roots, (Head*)cccode); 
	addroot(&gc->roots, (Head*)finals);

	vmp = vms;
	while(vmp < vms+Maxvms){
		vm = *vmp++;
		if(vm == 0)
			continue;

		for(m = vm->sp; m < Maxstk; m++)
			addroot(&gc->roots, valhead(vm->stack[m]));
		hforeach(vm->top->env->var, bindingroot, gc);
		hforeach(vm->top->env->rd, rdroot, gc);
		addroot(&gc->roots, valhead(vm->ac));
		addroot(&gc->roots, valhead(vm->cl));
	
		addroot(&gc->roots, (Head*)vm->litdom);
		addroot(&gc->roots, (Head*)vm->prof);

		/* current GC-protected objects */
		for(m = 0; m < vm->pdepth; m++){
			r = vm->prot[m];
			while(r){
				addroot(&gc->roots, r->hd);
				r = r->link;
			}
		}
	}

	/* pending finalizers */
	r = firstroot(&gc->finals);
	while(r){
//		addroot(&gc->roots, (Head*)r->hd->final);
		r = r->link;
	}
}

static void
rootsetreset(Rootset *rs)
{
	freerootlist(rs, rs->roots);
	rs->roots = 0;
	rs->last = 0;
	rs->before_last = 0;
	rs->this = 0;
}

static void
gcreset(GC *gc)
{
	rootsetreset(&gc->roots);
	rootsetreset(&gc->stores);
	if(rootsetempty(&gc->finals))
		// safe: mutator is blocked from finals; nothing there anyway
		rootsetreset(&gc->finals);
}

enum {
	GCdie = 0x1,
	GCdied = 0x2,
	GCpaused = 0x4,
	Gcenable = 0x8,
	GCrun = 0x10,
	GCrunning = 0x20,
};

static int
waitmutator(GC *gc)
{
	char b;
	
	gc->gcpause = 1;
	if(0 > chanreadb(gc->cgc, &b))
		fatal("gc synchronization failure"); 
	if(b == GCdie)
		return 1;
	if(b != GCpaused)
		fatal("gc protocol botch");
	return 0;
}

static void
resumemutator(GC *gc)
{
	char b;

	gc->gcpause = 0;
	b = Gcenable;
	if(0 > chanwriteb(gc->cgc, &b))
		fatal("gc synchronization failure");
}

static void
waitgcrun(GC *gc)
{
	char b;
	
	if(0 > chanreadb(gc->cgc, &b))
		fatal("gc synchronization failure"); 
	if(b == GCdie){
		b = GCdied;
		if(0 > chanwriteb(gc->cgc, &b))
			fatal("gc sychronization failure");
		threadexit(0);
	}
	if(b != GCrun)
		fatal("gc protocol botch");

	gcreset(gc);
	rootset(gc);
	gcepoch++;
	gc->gcrun = 1;
	b = GCrunning;
//	if(HEAPPROF) heapstat("start");
	if(0 > chanwriteb(gc->cgc, &b))
		fatal("gc synchronization failure");
}

static void
gcwb(GC *gc, Val v)
{
	if(gc->gcrun)
		addroot(&gc->stores, valhead(v));
}

static void
gcsync(int fd, char t, char r)
{
	char b;
	if(0 > chanwriteb(fd, &t))
		fatal("gc synchronization failure");
	if(0 > chanreadb(fd, &b))
		fatal("gc synchronization failure");
	if(b != r)
		fatal("gc protocol botch");
}

static int
needsgc(GC *gc)
{
	if(gc->heaphi && cqctmeminuse >= gc->heaphi)
		return 1;
	if(tick >= nextgctick){
		nextgctick = tick+GCrate;
		return 1;
	}else
		return 0;
}

static void
gcclearfinal(GC *gc)
{
	Head *hd;

	while(1){
		hd = removeroot(&gc->finals);
		if(hd == 0)
			break;
		Vsetcolor(hd, GCCOLOR(gcepoch)); /* reintroduce object */
//		hd->final = 0;
	}
}

static void
gcfinal(GC *gc, VM *vm)
{
	Head *hd;
	Closure *cl;
	Val ac, arg;

	ac = vm->ac;
	gcprotect(vm, valhead(ac)); /* valhead filters Xnil, ...  */
	while(1){
		hd = removeroot(&gc->finals);
		if(hd == 0)
			break;
		cl = valcl(tabget(finals, hd));
		tabdel(vm, finals, hd);
//		hd->final = 0;
		gcunpersist(vm, cl);
//xprintf("clearing final on %p\n", hd);
		Vsetfinal(hd, 0);
		Vsetcolor(hd, GCCOLOR(gcepoch)); /* reintroduce object */
		arg = hd;
		dovm(vm, cl, 1, &arg);
	}
	gcunprotect(vm, valhead(ac));
	vm->ac = ac;
}

static void
concurrentgcpoll(GC *gc, VM *vm)
{
	if((gc->state&GCenabled) == 0)
		return;
	if(gc->gcpause)
		gcsync(gc->cm, GCpaused, Gcenable);
	else if(gc->gcrun)
		return;
	else if(needsgc(gc)){
		gcsync(gc->cm, GCrun, GCrunning);
		gcfinal(gc, vm);
	}
}

static void
concurrentgckill(GC *gc)
{
	gcsync(gc->cm, GCdie, GCdied);
	chanclose(gc->cm);
	chanclose(gc->cgc);
	threadwait(gc->t);
	gc->state = 0;
}

static void*
gcchild(void *p)
{
	GC *gc = (GC*)p;
	int die = 0;
	char b;

	threadinit();
	while(!die){
		waitgcrun(gc);
		mark(gc, GCCOLOR(gcepoch));
		sweep(gc, GCCOLOR(gcepoch-2));
		die = waitmutator(gc);
		while(!rootsetempty(&gc->stores)){
			if(!die)
				resumemutator(gc);
			mark(gc, GCCOLOR(gcepoch));
			if(!die)
				die = waitmutator(gc);
		}
		gc->gcrun = 0;
//		if(HEAPPROF) heapstat("end");
		if(!die)
			resumemutator(gc);
	}

	b = GCdied;
	if(0 > chanwriteb(gc->cgc, &b))
		fatal("gc sychronization failure");
	threadexit(0);
	return 0;
}

static void
concurrentgcinit(GC *gc)
{
	newchan(&gc->cm, &gc->cgc);
	gc->gcpause = 0;
	gc->t = newthread(gcchild, gc);
	if(gc->t == 0)
		fatal("newthread failed");
	gc->state = GCenabled;
}

static void
dogc(GC *gc)
{
	gcreset(gc);
	rootset(gc);
	gcepoch++;
	mark(gc, GCCOLOR(gcepoch));
	sweep(gc, GCCOLOR(gcepoch-2));
	while(!rootsetempty(&gc->stores))
		mark(gc, GCCOLOR(gcepoch));
}

static void
serialgcinit(GC *gc)
{
	gc->state = GCenabled;
}

static void
serialgcpoll(GC *gc, VM *vm)
{
	if((gc->state&GCenabled) == 0)
		return;
	if(needsgc(gc)){
		dogc(gc);
		dogc(gc);
		gcfinal(gc, vm);
	}
}

static void
serialgckill(GC *gc)
{
	gc->state = 0;
}

void
gcenable(VM *vm)
{
	if(thegc->state&GCenabled)
		vmerr(vm, "GC is already enabled");
	thegc->gcinit(thegc);
}

void
gcdisable(VM *vm)
{
	if((thegc->state&GCenabled) == 0)
		vmerr(vm, "GC is already disabled");
	thegc->gckill(thegc);
}

static Imm
typesize(VM *vm, Xtypename *xtn)
{
	Cval *cv;
	Str *es;
	if(xtn == 0)
		vmerr(vm, "attempt to compute size of undefined type");
	switch(xtn->tkind){
	case Tvoid:
		vmerr(vm, "attempt to compute size of void type");
	case Tbase:
		return repsize[xtn->rep];
	case Ttypedef:
		return typesize(vm, xtn->link);
	case Tstruct:
	case Tunion:
		cv = valcval(xtn->sz);
		return cv->val;
	case Tenum:
		return typesize(vm, xtn->link);
	case Tptr:
		return repsize[xtn->rep];
	case Tarr:
		if(Vkind(xtn->cnt) != Qcval)
			vmerr(vm,
			      "attempt to compute size of unspecified array");
		cv = valcval(xtn->cnt);
		return cv->val*typesize(vm, xtn->link);
	case Tfun:
		vmerr(vm, "attempt to compute size of function type");
	case Tbitfield:
		vmerr(vm, "attempt to compute size of bitfield"); 
	case Tundef:
		es = fmtxtn(xtn->link);
		vmerr(vm, "attempt to compute size of undefined type: %.*s",
		      (int)es->len, es->s);
	case Tconst:
		vmerr(vm, "shouldn't this be impossible?");
	case Txaccess:
		return typesize(vm, xtn->link);
	}
	fatal("bug");
}

static Head*
iterbox(Head *hd, Ictx *ictx)
{
	Box *box;
	box = (Box*)hd;
	if(ictx->n > 0)
		return GCiterdone;
	ictx->n = 1;
	return valhead(box->v);
}

static Head*
itercl(Head *hd, Ictx *ictx)
{
	Closure *cl;
	cl = (Closure*)hd;
	if(ictx->n > cl->dlen)
		return GCiterdone;
	if(ictx->n == cl->dlen){
		ictx->n++;
		return (Head*)cl->code;
	}
	return valhead(cl->display[ictx->n++]);
}

static Head*
itercode(Head *hd, Ictx *ictx)
{
	Code *code;
	code = (Code*)hd;
	if(ictx->n > 0)
		return GCiterdone;
	ictx->n++;
	return (Head*)(code->konst);
}

static Head*
itercval(Head *hd, Ictx *ictx)
{
	Cval *cval;
	cval = (Cval*)hd;

	switch(ictx->n++){
	case 0:
		return (Head*)cval->dom;
	case 1:
		return (Head*)cval->type;
	default:
		return GCiterdone;
	}
}

static Head*
iterpair(Head *hd, Ictx *ictx)
{
	Pair *pair;
	pair = (Pair*)hd;

	switch(ictx->n++){
	case 0:
		return valhead(pair->car);
	case 1:
		return valhead(pair->cdr);
	default:
		return GCiterdone;
	}
}

static Head*
iterrange(Head *hd, Ictx *ictx)
{
	Range *range;
	range = (Range*)hd;

	switch(ictx->n++){
	case 0:
		return (Head*)range->beg;
	case 1:
		return (Head*)range->len;
	default:
		return GCiterdone;
	}
}

static Head*
iterrd(Head *hd, Ictx *ictx)
{
	Rd *rd;
	rd = (Rd*)hd;
	switch(ictx->n++){
	case 0:
		return (Head*)rd->name;
	case 1:
		return (Head*)rd->fname;
	case 2:
		return (Head*)rd->is;
	case 3:
		return (Head*)rd->mk;
	case 4:
		return (Head*)rd->fmt;
	case 5:
		return (Head*)rd->get;
	case 6:
		return (Head*)rd->set;
	default:
		return GCiterdone;
	}
}

Code*
newcode()
{
	return (Code*)halloc(&heap[Qcode]);
}

Closure*
mkcl(Code *code, unsigned long entry, unsigned len, char *id)
{
	Closure *cl;
	cl = (Closure*)halloc(&heap[Qcl]);
	cl->code = code;
	cl->entry = entry;
	cl->dlen = len;
	cl->display = emalloc(cl->dlen*sizeof(Val));
	cl->id = xstrdup(id);
	return cl;
}

Closure*
mkcfn(char *id, Cfn *cfn)
{
	Closure *cl;
	cl = mkcl(cccode, 0, 0, id);
	cl->cfn = cfn;
	return cl;
}

Closure*
mkccl(char *id, Ccl *ccl, unsigned dlen, ...)
{
	Closure *cl;
	va_list args;
	Val vp;
	unsigned m;

	va_start(args, dlen);
	cl = mkcl(cccode, 0, dlen, id);
	cl->ccl = ccl;
	for(m = 0; m < dlen; m++){
		vp = va_arg(args, Val);
		cl->display[m] = vp;
	}
	va_end(args);
	return cl;
}

static int
freecl(Head *hd)
{
	Closure *cl;
	cl = (Closure*)hd;
	efree(cl->display);
	efree(cl->id);
	return 1;
}

static int
freefd(Head *hd)
{
	Fd *fd;
	fd = (Fd*)hd;
	/* FIXME: should dovm close closure if there is one (as finalizer?) */
	if((fd->flags&Fclosed) == 0
	   && fd->flags&Ffn
	   && fd->u.fn.close)
		fd->u.fn.close(&fd->u.fn);
	return 1;
}

Fd*
mkfdfn(Str *name, int flags, Xfd *xfd)
{
	Fd *fd;
//	if(read == 0)
//		flags &= ~Fread;
//	if(write == 0)
//		flags &= ~Fwrite;
	fd = (Fd*)halloc(&heap[Qfd]);
	fd->name = name;
	fd->u.fn = *xfd;
	fd->flags = flags|Ffn;
	return fd;
}

Fd*
mkfdcl(Str *name, int flags,
       Closure *read,
       Closure *write,
       Closure *close)
{
	Fd *fd;
	if(read == 0)
		flags &= ~Fread;
	if(write == 0)
		flags &= ~Fwrite;
	fd = (Fd*)halloc(&heap[Qfd]);
	fd->name = name;
	fd->u.cl.read = read;
	fd->u.cl.write = write;
	fd->u.cl.close = close;
	fd->flags = flags&~Ffn;
	return fd;
}

static Head*
iterfd(Head *hd, Ictx *ictx)
{
	Fd *fd;
	fd = (Fd*)hd;
	switch(ictx->n++){
	case 0:
		return (Head*)fd->name;
	case 1:
		if(fd->flags&Ffn)
			return GCiterdone;
		return (Head*)fd->u.cl.close;
	case 2:
		return (Head*)fd->u.cl.read;
	case 3:
		return (Head*)fd->u.cl.write;
	default:
		return GCiterdone;
	}
}

int
eqval(Val v1, Val v2)
{
	if(Vkind(v1) != Vkind(v2))
		return 0;
	return hashop[Vkind(v1)].eq(v1, v2);
}

static u32
hashval(Val v)
{
	return hashop[Vkind(v)].hash(v);
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
nohash(Val val)
{
	fatal("bad type of key (%d) to table operation", Vkind(val));
}

static u32
hashptr(Val val)
{
	return hashptr32shift(valhead(val));
}

static int
eqptr(Val a, Val b)
{
	return valhead(a)==valhead(b);
}

static u32
hashconst(Val val)
{
	switch(Vkind(val)){
	case Qnil:
		return hashptr32shift(Xnil);
	case Qnull:
		return hashptr32shift(Xnulllist);
	default:
		fatal("bug");
	}
}

static int
eqtrue(Val a, Val b)
{
	return 1;
}

static u32
hashcval(Val val)
{
	Cval *cv;
	cv = valcval(val);
	return hash6432shift(cv->val)^hashxtn(mkvalxtn(cv->type));
}

static int
eqcval(Val a, Val b)
{
	Cval *cva, *cvb;
	cva = valcval(a);
	cvb = valcval(b);
	if(cva->val!=cvb->val)
		return 0;
	return equalxtn(cva->type, cvb->type);
}

static int
equalcval(Val a, Val b)
{
	Cval *cva, *cvb;
	cva = valcval(a);
	cvb = valcval(b);
	return cva->val==cvb->val;
}

static u32
hashrange(Val val)
{
	Range *r;
	r = valrange(val);
	return hash6432shift(r->beg->val)^hash6432shift(r->len->val);
}

static int
equalrange(Val a, Val b)
{
	Range *ra, *rb;
	ra = valrange(a);
	rb = valrange(b);
	return ra->beg->val==rb->beg->val && ra->len->val==rb->len->val;
}

static u32
hashstr(Val val)
{
	Str *s;
	s = valstr(val);
	return shash(s->s, s->len);
}

static int
equalstrc(Str *a, char *b)
{
	if(a->len != strlen(b))
		return 0;
	return memcmp(a->s, b, a->len) ? 0 : 1;
}

static int
equalstr(Str *a, Str *b)
{
	if(a->len != b->len)
		return 0;
	return memcmp(a->s, b->s, a->len) ? 0 : 1;
}

static int
equalstrv(Val a, Val b)
{
	return equalstr(valstr(a), valstr(b));
}

static u32
hashxtn(Val val)
{
	u32 x;
	Xtypename *xtn;
	Vec *vec;
	Imm m;
	
	xtn = valxtn(val);
	switch(xtn->tkind){
	case Tvoid:
		return hash6432shift(xtn->tkind);
	case Tbase:
		return hash6432shift(xtn->basename^xtn->rep);
	case Ttypedef:
		return hashstr((Val)xtn->tid)>>xtn->tkind;
	case Tstruct:
	case Tunion:
	case Tenum:
		return hashstr((Val)xtn->tag)>>xtn->tkind;
	case Tundef:
	case Tptr:
		return xtn->rep^hashxtn((Val)xtn->link)>>xtn->tkind;
	case Tarr:
		x = hashxtn((Val)xtn->link)>>xtn->tkind;
		if(Vkind(xtn->cnt) == Qcval)
			x ^= hashcval(xtn->cnt);
		return x;
	case Tfun:
		x = hashxtn((Val)xtn->link)>>xtn->tkind;
		for(m = 0; m < xtn->param->len; m++){
			x <<= 1;
			vec = valvec(vecref(xtn->param, m));
			x ^= hashxtn(vecref(vec, Typepos));
		}
		return x;
	case Tbitfield:
		x = hashxtn((Val)xtn->link)>>xtn->tkind;
		x ^= hashcval(xtn->sz);
		return x;
	case Tconst:
		return hashxtn((Val)xtn->link)>>xtn->tkind;
	case Txaccess:
		x = hashxtn((Val)xtn->link)>>xtn->tkind;
		x ^= hashptr((Val)xtn->get);
		x ^= hashptr((Val)xtn->put);
		return x;
	}
	fatal("bug");
}

static int
equalxtn(Xtypename *a, Xtypename *b)
{
	Imm m;

	if(a->tkind != b->tkind)
		return 0;

	switch(a->tkind){
	case Tvoid:
		return 1;
	case Tbase:
		return (a->basename == b->basename) && (a->rep == b->rep);
	case Ttypedef:
		return equalstr(a->tid, b->tid);
	case Tstruct:
	case Tunion:
	case Tenum:
		return equalstr(a->tag, b->tag);
	case Tundef:
	case Tptr:
		return (a->rep == b->rep) && equalxtn(a->link, b->link);
	case Tarr:
		if(Vkind(a->cnt) != Vkind(b->cnt))
			return 0;
		if(Vkind(a->cnt) == Qcval){
			if(!equalcval(a->cnt, b->cnt))
				return 0;
		}
		return equalxtn(a->link, b->link);
	case Tfun:
		if(a->param->len != b->param->len)
			return 0;
		if(!equalxtn(a->link, b->link))
			return 0;
		for(m = 0; m < a->param->len; m++)
			if(!equalxtn(valxtn(vecref(valvec(vecref(a->param, m)),
						   Typepos)),
				     valxtn(vecref(valvec(vecref(b->param, m)),
						   Typepos))))
				return 0;
		return 1;
	case Tbitfield:
		if(!equalcval(a->sz, b->sz))
			return 0;
		return equalxtn(a->link, b->link);
	case Tconst:
		return equalxtn(a->link, b->link);
	case Txaccess:
		return (eqptr((Val)a->get, (Val)b->get)
			&& eqptr((Val)a->put, (Val)b->put)
			&& equalxtn(a->link, b->link));
	}
	fatal("bug");
}

static int
equalxtnv(Val a, Val b)
{
	return equalxtn(valxtn(a), valxtn(b));
}

Str*
mkstr0(char *s)
{
	Str *str;
	str = (Str*)halloc(&heap[Qstr]);
	str->len = strlen(s);
	str->s = emalloc(str->len);
	memcpy(str->s, s, str->len);
	str->skind = Smalloc;
	return str;
}

Str*
mkstr(char *s, Imm len)
{
	Str *str;
	str = (Str*)halloc(&heap[Qstr]);
	str->len = len;
	str->s = emalloc(str->len);
	memcpy(str->s, s, str->len);
	str->skind = Smalloc;
	return str;
}

Str*
mkstrk(char *s, Imm len, Skind skind)
{
	Str *str;
	str = (Str*)halloc(&heap[Qstr]);
	str->s = s;
	str->len = len;
	str->skind = skind;
	if(skind == Smmap)
		str->mlen = len;
	return str;
}

Str*
mkstrn(VM *vm, Imm len)
{
	Str *str;
	str = (Str*)halloc(&heap[Qstr]);
	if(len >= PAGESZ){
		str->len = len;
		str->s = mmap(0, PAGEUP(len), PROT_READ|PROT_WRITE,
			      MAP_NORESERVE|MAP_PRIVATE|MAP_ANON, -1, 0);
		if(str->s == (void*)(-1))
			vmerr(vm, "out of memory");
		str->mlen = PAGEUP(len);
		str->skind = Smmap;
	}else{
		str->len = len;
		str->s = emalloc(str->len);
		str->skind = Smalloc;
	}
	return str;
}

Str*
mkstrlits(Lits *lits)
{
	return mkstr(lits->s, lits->len);
}

static Str*
strcopy(Str *s)
{
	return mkstr(s->s, s->len);
}

char*
str2cstr(Str *str)
{
	char *s;
	s = emalloc(str->len+1);
	memcpy(s, str->s, str->len);
	return s;
}

Str*
strslice(Str *str, Imm beg, Imm end)
{
	return mkstr(str->s+beg, end-beg);
}

static Str*
strconcat(VM *vm, Str *s1, Str *s2)
{
	Str *s;
	s = mkstrn(vm, s1->len+s2->len);
	memcpy(s->s, s1->s, s1->len);
	memcpy(s->s+s1->len, s2->s, s2->len);
	return s;
}

static int
freestr(Head *hd)
{
	Str *str;
	str = (Str*)hd;
	switch(str->skind){
	case Smmap:
		munmap(str->s, str->mlen);
		break;
	case Smalloc:
		efree(str->s);
		break;
	case Sperm:
		break;
	}
	return 1;
}

static int
listlenpair(Val v, Imm *rv)
{
	Imm m;
	Pair *p;

	m = 0;
	while(Vkind(v) == Qpair){
		m++;
		p = valpair(v);
		v = p->cdr;
	}
	if(Vkind(v) != Qnull)
		return 0;
	*rv = m;
	return 1;
}

static int
listlen(Val v, Imm *rv)
{
	List *lst;
	if(Vkind(v) == Qpair || Vkind(v) == Qnull)
		return listlenpair(v, rv);
	if(Vkind(v) == Qlist){
		lst = vallist(v);
		*rv = listxlen(lst->x);
		return 1;
	}
	return 0;
}

Vec*
mkvec(Imm len)
{
	Vec *vec;

	vec = (Vec*)halloc(&heap[Qvec]);
	vec->len = len;
	vec->vec = emalloc(len*sizeof(Val));
	return vec;
}

static Vec*
mkvecinit(Imm len, Val v)
{
	Vec *vec;
	Imm i;

	vec = mkvec(len);
	for(i = 0; i < len; i++)
		vec->vec[i] = v;
	return vec;
}

Val
vecref(Vec *vec, Imm idx)
{
	return vec->vec[idx];
}

void
_vecset(Vec *vec, Imm idx, Val v)
{
	vec->vec[idx] = v;
}

void
vecset(VM *vm, Vec *vec, Imm idx, Val v)
{
	gcwb(thegc, vec->vec[idx]);
	_vecset(vec, idx, v);
}

static u32
hashvec(Val v)
{
	Vec *a;
	u32 i, len, m;
	a = valvec(v);
	m = Vkind(v);
	len = a->len;
	for(i = 0; i < len; i++)
		m ^= hashval(a->vec[i]);
	return m;
}

static int
equalvec(Vec *a, Vec *b)
{
	u32 len, m;
	len = a->len;
	if(len != b->len)
		return 0;
	for(m = 0; m < len; m++)
		if(!eqval(a->vec[m], b->vec[m]))
			return 0;
	return 1;
}

static int
equalvecv(Val a, Val b)
{
	return equalvec(valvec(a), valvec(b));
}

static Vec*
veccopy(Vec *old)
{
	Vec *new;
	new = mkvec(old->len);
	memcpy(new->vec, old->vec, old->len*sizeof(Val));
	return new;
}

static Head*
itervec(Head *hd, Ictx *ictx)
{
	Vec *vec;
	vec = (Vec*)hd;
	if(ictx->n >= vec->len)
		return GCiterdone;
	return valhead(vec->vec[ictx->n++]);
}

static int
freevec(Head *hd)
{
	Vec *vec;
	vec = (Vec*)hd;
	efree(vec->vec);
	return 1;
}

static Tabx*
mktabx(u32 sz)
{
	Tabx *x;
	x = emalloc(sizeof(Tabx));
	x->sz = sz;
	x->lim = 2*sz/3;
	x->val = emalloc(x->lim*sizeof(Val)); /* must be 0 or Xundef */
	x->key = emalloc(x->lim*sizeof(Val)); /* must be 0 or Xundef */
	x->idx = emalloc(x->sz*sizeof(Tabidx*));
	return x;
}

static void
freetabx(Tabx *x)
{
	efree(x->val);
	efree(x->key);
	efree(x->idx);
	efree(x);
}

static Tab*
_mktab(Tabx *x)
{
	Tab *tab;
	tab = (Tab*)halloc(&heap[Qtab]);
	tab->x = x;
	tab->weak = 0;
	return tab;
}

Tab*
mktab()
{
	return _mktab(mktabx(Tabinitsize));
}

static Head*
itertab(Head *hd, Ictx *ictx)
{
	Tab *tab;
	u32 idx, nxt;
	Tabx *x;

	tab = (Tab*)hd;

	if(ictx->n == 0)
		ictx->x = x = tab->x;
	else
		x = ictx->x;
		
	nxt = x->nxt;		/* mutator may update nxt */
	if(ictx->n >= 2*nxt)
		return GCiterdone;
	if(ictx->n >= nxt){
		idx = ictx->n-nxt;
		ictx->n++;
		return valhead(x->val[idx]);
	}
	if(tab->weak){
		/* skip ahead */
		ictx->n = nxt;
		return 0;
	}
	idx = ictx->n++;
	return valhead(x->key[idx]);
}

static int
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
			efree(pk);
		}
	}
	freetabx(x);
	return 1;
}

static Tabidx*
_tabget(Tab *tab, Val keyv, Tabidx ***prev)
{
	Tabidx **p, *tk;
	Val kv;
	u32 idx;
	Tabx *x;
	unsigned kind;
	Hashop *op;

	x = tab->x;
	kind = Vkind(keyv);
	op = &hashop[kind];
	idx = op->hash(keyv)&(x->sz-1);
	p = &x->idx[idx];
	tk = *p;
	while(tk){
		kv = x->key[tk->idx];
		if(Vkind(kv) == kind && op->eq(keyv, kv)){
			if(prev)
				*prev = p;
			return tk;
		}
		p = &tk->link;
		tk = *p;
	}
	return 0;
}

Val
tabget(Tab *tab, Val keyv)
{
	Tabidx *tk;
	tk = _tabget(tab, keyv, 0);
	if(tk)
		return tab->x->val[tk->idx];
	return 0;
}

/* double hash table size and re-hash entries */
static void
tabexpand(Tab *tab)
{
	Tabidx *tk, *nxt;
	u32 i, m, idx;
	Tabx *x, *nx;
	Val kv;

	x = tab->x;
	nx = mktabx(x->sz*2);
	m = 0;
	for(i = 0; i < x->sz && m < tab->cnt; i++){
		tk = x->idx[i];
		while(tk){
			nxt = tk->link;
			kv = x->key[tk->idx];
			idx = hashop[Vkind(kv)].hash(kv)&(nx->sz-1);
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
dotabput(VM *vm, Tab *tab, Val keyv, Val val)
{
	Tabidx *tk;
	u32 idx;
	Tabx *x;

	x = tab->x;
	tk = _tabget(tab, keyv, 0);
	if(tk){
		gcwb(thegc, x->val[tk->idx]);
		x->val[tk->idx] = val;
		return;
	}

	if(x->nxt >= x->lim){
		tabexpand(tab);
		x = tab->x;
	}

	tk = emalloc(sizeof(Tabidx));

	/* FIXME: snapshot-at-beginning seems to imply that it does
	   not matter whether gc can see these new values in this
	   epoch. right? */
	tk->idx = x->nxt;
	x->key[tk->idx] = keyv;
	x->val[tk->idx] = val;
	idx = hashop[Vkind(keyv)].hash(keyv)&(x->sz-1);
	tk->link = x->idx[idx];
	x->idx[idx] = tk;
	x->nxt++;
	tab->cnt++;
}

void
_tabput(Tab *tab, Val keyv, Val val)
{
	dotabput(0, tab, keyv, val);
}

void
tabput(VM *vm, Tab *tab, Val keyv, Val val)
{
	dotabput(vm, tab, keyv, val);
}

void
tabdel(VM *vm, Tab *tab, Val keyv)
{
	Tabidx *tk, **ptk;
	Tabx *x;
	
	x = tab->x;
	tk = _tabget(tab, keyv, &ptk);
	if(tk == 0)
		return;
	gcwb(thegc, x->key[tk->idx]);
	gcwb(thegc, x->val[tk->idx]);
	x->key[tk->idx] = Xundef;
	x->val[tk->idx] = Xundef;
	*ptk = tk->link;
	efree(tk);
	tab->cnt--;
}

/* create a new vector of len 2N, where N is number of elements in TAB.
   elements 0..N-1 are the keys of TAB; N..2N-1 are the associated vals. */
static Vec*
tabenum(Tab *tab)
{
	Vec *vec;
	Tabidx *tk;
	u32 i, m;
	Tabx *x;

	x = tab->x;
	vec = mkvec(2*tab->cnt);
	m = 0;
	for(i = 0; i < x->sz && m < tab->cnt; i++){
		tk = x->idx[i];
		while(tk){
			_vecset(vec, m, x->key[tk->idx]);
			_vecset(vec, m+tab->cnt, x->val[tk->idx]);
			m++;
			tk = tk->link;
		}
	}
	return vec;
}

static Vec*
tabenumkeys(Tab *tab)
{
	Vec *vec;
	Tabidx *tk;
	u32 i, m;
	Tabx *x;

	x = tab->x;
	vec = mkvec(tab->cnt);
	m = 0;
	for(i = 0; i < x->sz && m < tab->cnt; i++){
		tk = x->idx[i];
		while(tk){
			_vecset(vec, m, x->key[tk->idx]);
			m++;
			tk = tk->link;
		}
	}
	return vec;
}

static Vec*
tabenumvals(Tab *tab)
{
	Vec *vec;
	Tabidx *tk;
	u32 i, m;
	Tabx *x;

	x = tab->x;
	vec = mkvec(tab->cnt);
	m = 0;
	for(i = 0; i < x->sz && m < tab->cnt; i++){
		tk = x->idx[i];
		while(tk){
			_vecset(vec, m, x->val[tk->idx]);
			m++;
			tk = tk->link;
		}
	}
	return vec;
}

static void
tabpop(VM *vm, Tab *tab, Val *rv)
{
	Tabidx *tk;
	Tabx *x;
	u32 i;
	Vec *vec;

	if(tab->cnt == 0)
		return; 	/* nil */

	x = tab->x;
	for(i = 0; i < x->sz; i++){
		tk = x->idx[i];
		if(!tk)
			continue;
		x->idx[i] = tk->link;
		tab->cnt--;
		break;
	}
	
	vec = mkvec(2);
	_vecset(vec, 0, x->key[tk->idx]);
	_vecset(vec, 1, x->val[tk->idx]);
	gcwb(thegc, x->key[tk->idx]);
	gcwb(thegc, x->val[tk->idx]);
	x->key[tk->idx] = Xundef;
	x->val[tk->idx] = Xundef;
	efree(tk);
	*rv = mkvalvec(vec);
}

static Tab*
tabcopy(Tab *tab)
{
	Tabidx *tk;
	Tab *rv;
	Tabx *x;
	u32 i, m;

	rv = mktab();
	m = 0;
	x = tab->x;
	for(i = 0; i < x->sz && m < tab->cnt; i++){
		tk = x->idx[i];
		while(tk){
			_tabput(rv, x->key[tk->idx], x->val[tk->idx]);
			m++;
			tk = tk->link;
		}
	}
	return rv;
}

u32
listxlen(Listx *x)
{
	return x->tl-x->hd;
}

static Listx*
mklistx(u32 sz)
{
	Listx *x;
	x = emalloc(sizeof(Listx));
	x->hd = x->tl = sz/2;
	x->sz = sz;
	x->val = emalloc(x->sz*sizeof(Val)); /* must be 0 or Xundef */
	return x;
}

static List*
_mklist(Listx *x)
{
	List *lst;
	lst = (List*)halloc(&heap[Qlist]);
	lst->x = x;
	return lst;
}

static List*
mklistn(u32 sz)
{
	return _mklist(mklistx(sz));
}

List*
mklist()
{
	return mklistn(Listinitsize);
}

static List*
mklistinit(Imm len, Val v)
{
	List *l;
	Imm m;

	l = mklistn(len);
	for(m = 0; m < len; m++)
		_listappend(l, v); 
	return l;
}

static void
freelistx(Listx *x)
{
	efree(x->val);
	efree(x->oval);
	efree(x);
}

static int
freelist(Head *hd)
{
	List *lst;
	lst = (List*)hd;
	freelistx(lst->x);
	return 1;
}

static void
listxaddroots(Listx *x, u32 idx, u32 n)
{
	while(n-- > 0)
		gcwb(thegc, x->val[x->hd+idx+n]);
}

static Val
listref(VM *vm, List *lst, Imm idx)
{
	Listx *x;
	x = lst->x;
	if(idx >= listxlen(x))
		vmerr(vm, "listref out of bounds");
	return x->val[x->hd+idx];
}

static List*
listset(VM *vm, List *lst, Imm idx, Val v)
{
	Listx *x;
	x = lst->x;
	if(idx >= listxlen(x))
		vmerr(vm, "listset out of bounds");
	listxaddroots(x, idx, 1);
	x->val[x->hd+idx] = v;
	return lst;
}

static List*
listcopy(List *lst)
{
	List *new;
	u32 hd, tl, len;
	new = mklistn(lst->x->sz);
	new->x->hd = hd = lst->x->hd;
	new->x->tl = tl = lst->x->tl;
	len = tl-hd;
	memcpy(&new->x->val[hd], &lst->x->val[hd], len*sizeof(Val));
	return new;
}

static void
listcopyv(List *lst, Imm ndx, Imm n, Val *v)
{
	Listx *x;
	x = lst->x;
	memcpy(v, x->val+x->hd+ndx, n*sizeof(Val));
}

static List*
listreverse(List *lst)
{
	List *new;
	u32 hd, tl, len, m;
	new = mklistn(lst->x->sz);
	new->x->hd = hd = lst->x->hd;
	new->x->tl = tl = lst->x->tl;
	len = tl-hd;
	for(m = 0; m < len; m++)
		new->x->val[hd++] = lst->x->val[--tl];
	return new;
}

static Val
listhead(VM *vm, List *lst)
{
	Listx *x;
	x = lst->x;
	if(listxlen(x) == 0)
		vmerr(vm, "head on empty list");
	return x->val[x->hd];
}

static void
listpop(VM *vm, List *lst, Val *vp)
{
	Listx *x;
	x = lst->x;
	if(listxlen(x) == 0)
		return; 	/* nil */
	*vp = x->val[x->hd];
	listxaddroots(x, 0, 1);
	x->val[x->hd] = Xundef;
	x->hd++;
}

static List*
listtail(VM *vm, List *lst)
{
	List *new;
	if(listxlen(lst->x) == 0)
		vmerr(vm, "tail on empty list");
	new = listcopy(lst);
	new->x->hd++;
	return new;
}

static u32
hashlist(Val v)
{
	List *l;
	Listx *x;
	u32 i, len, m;
	l = vallist(v);
	x = l->x;
	m = Vkind(v);
	len = listxlen(x);
	for(i = 0; i < len; i++)
		m ^= hashval(x->val[x->hd+i]);
	return m;
}

static int
equallist(List *a, List *b)
{
	Listx *xa, *xb;
	u32 len, m;
	xa = a->x;
	xb = b->x;
	len = listxlen(xa);
	if(len != listxlen(xb))
		return 0;
	for(m = 0; m < len; m++)
		if(!eqval(xa->val[xa->hd+m], xb->val[xb->hd+m]))
			return 0;
	return 1;
}

static int
equallistv(Val a, Val b)
{
	return equallist(vallist(a), vallist(b));
}

static void
listexpand(List *lst)
{
	Listx *x, *nx;
	u32 len, newsz;

	x = lst->x;
	if(x->sz)
		newsz = x->sz*2;
	else
		newsz = 1;
	nx = mklistx(newsz);
	len = listxlen(x);
	if(x->hd == 0){
		/* expanding to the left */
		nx->hd = x->sz;
		nx->tl = x->tl+x->sz;
	}else{
		/* expanding to the right */
		nx->hd = x->hd;
		nx->tl = x->tl;
	}
	memcpy(&nx->val[nx->hd], &x->val[x->hd], len*sizeof(Val));
	_mklist(x);		/* preserve potential concurrent iterlist's
				   view; same strategy as tabexpand. */
	lst->x = nx;
}

static Listx*
maybelistexpand(List *lst)
{
	Listx *x;
again:
	x = lst->x;
	if(x->hd == 0 || x->tl == x->sz){
		listexpand(lst);
		goto again;	/* at most twice iff full on both ends */
	}
	return lst->x;
}

/* maybe listdel and listins should slide whichever half is shorter */

enum {
	SlideIns,
	SlideDel,
};

/* if op==SlideIns, expect room to slide by 1 in either direction;
   however, currently we always slide to right */
static void
slide(Listx *x, u32 idx, int op)
{
	Val *t;
	u32 m;
	m = listxlen(x)-idx;
	if(m <= 1 && op == SlideDel)
		/* deleting last element; nothing to do */
		return;		
	if(x->oval == 0)
		x->oval = emalloc(x->sz*sizeof(Val));
	memcpy(&x->oval[x->hd], &x->val[x->hd], idx*sizeof(Val));
	switch(op){
	case SlideIns:
		memcpy(&x->oval[x->hd+idx+1], &x->val[x->hd+idx],
		       m*sizeof(Val));
		break;
	case SlideDel:
		memcpy(&x->oval[x->hd+idx], &x->val[x->hd+idx+1],
		       (m-1)*sizeof(Val));
		break;
	}
	t = x->val;
	x->val = x->oval;
	x->oval = t;
}

static List*
listdel(VM *vm, List *lst, Imm idx)
{
	Listx *x;
	u32 m, len;
	x = lst->x;
	len = listxlen(x);
	if(idx >= len)
		vmerr(vm, "listdel out of bounds");
	m = len-idx;
	listxaddroots(x, idx, m);
	slide(x, idx, SlideDel);
	x->val[x->tl-1] = Xundef;
	x->tl--;
	return lst;
}

static List*
listins(VM *vm, List *lst, Imm idx, Val v)
{
	Listx *x;
	u32 m, len;
	x = lst->x;
	len = listxlen(x);
	if(idx > len)
		vmerr(vm, "listins out of bounds");
	x = maybelistexpand(lst);
	if(idx == 0)
		x->val[--x->hd] = v;
	else if(idx == len)
		x->val[x->tl++] = v;
	else{
		m = len-idx;
		listxaddroots(x, idx, m);
		slide(x, idx, SlideIns);
		x->tl++;
		x->val[x->hd+idx] = v;
	}
	return lst;
}

void
_listappend(List *lst, Val v)
{
	Listx *x;
	u32 idx;
	x = lst->x;
	idx = listxlen(x);
	x = maybelistexpand(lst);
	x->val[x->tl++] = v;
}

List*
listpush(VM *vm, List *lst, Val v)
{
	return listins(vm, lst, 0, v);
}

List*
listappend(VM *vm, List *lst, Val v)
{
	return listins(vm, lst, listxlen(lst->x), v);
}

static List*
listconcat(VM *vm, List *l1, List *l2)
{
	List *rv;
	u32 m, len1, len2;

	len1 = listxlen(l1->x);
	len2 = listxlen(l2->x);
	rv = mklistn(len1+len2);
	for(m = 0; m < len1; m++)
		listappend(vm, rv, listref(vm, l1, m));
	for(m = 0; m < len2; m++)
		listappend(vm, rv, listref(vm, l2, m));
	return rv;
}

static List*
listslice(VM *vm, List *l, Imm b, Imm e)
{
	List *rv;
	Imm m;

	rv = mklistn(e-b);
	for(m = b; m < e; m++)
		listappend(vm, rv, listref(vm, l, m));
	return rv;
}

static Head*
iterlist(Head *hd, Ictx *ictx)
{
	List *lst;
	Listx *x;
	lst = (List*)hd;
	if(ictx->n == 0)
		ictx->x = lst->x;
	x = ictx->x;
	if(ictx->n >= x->sz)
		return GCiterdone;
	// x->val may change from call to call because of slide,
	// but will always point to buffer of markable Vals
	return valhead(x->val[ictx->n++]);
}

static Rec*
mkrec(Rd *rd)
{
	Imm m;
	Rec *r;
	r = (Rec*)halloc(&heap[Qrec]);
	r->rd = rd;
	r->nf = rd->nf;
	r->field = emalloc(r->nf*sizeof(Val));
	for(m = 0; m < r->nf; m++)
		r->field[m] = Xnil;
	return r;
}

static Head*
iterrec(Head *hd, Ictx *ictx)
{
	Rec *r;
	r = (Rec*)hd;
	if(ictx->n < r->nf)
		return valhead(r->field[ictx->n++]);
	switch(ictx->n-r->nf){
	case 0:
		ictx->n++;
		return (Head*)r->rd;
	default:
		return GCiterdone;
	}
}

static int
freerec(Head *hd)
{
	Rec *r;
	r = (Rec*)hd;
	efree(r->field);
	return 1;
}

static void
recis(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Rd *rd;
	Str *mn;
	Rec *r;

	rd = valrd(disp[0]);
	mn = valstr(disp[1]);
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to %.*s",
		      (int)mn->len, mn->s);
	if(Vkind(argv[0]) != Qrec){
		*rv = mkvalcval2(cval0);
		return;
	}
	r = valrec(argv[0]);
	if(r->rd == rd)
		*rv = mkvalcval2(cval1);
	else
		*rv = mkvalcval2(cval0);
}

static void
recmk(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Rd *rd;
	Str *mn;
	Rec *r;
	Imm m;

	rd = valrd(disp[0]);
	mn = valstr(disp[1]);
	if(argc != 0 && argc != rd->nf)
		vmerr(vm, "wrong number of arguments to %.*s",
		      (int)mn->len, mn->s);
	r = mkrec(rd);
	for(m = 0; m < argc; m++)
		r->field[m] = argv[m];
	*rv = mkvalrec(r);
}

static void
recfmt(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Rd *rd;
	Str *mn;
	Rec *r;
	unsigned len;
	char *buf;

	rd = valrd(disp[0]);
	mn = valstr(disp[1]);

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to %.*s",
		      (int)mn->len, mn->s);
	if(Vkind(argv[0]) != Qrec)
		vmerr(vm, "operand 1 to %.*s must be a %.*s record",
		      (int)mn->len, mn->s, (int)rd->name->len, rd->name->s);
	r = valrec(argv[0]);
	len = 1+rd->name->len+1+16+1+1;
	buf = emalloc(len);
	snprint(buf, len, "<%.*s %p>", (int)rd->name->len, rd->name->s, r);
	*rv = mkvalstr(mkstr0(buf));
	efree(buf);
}

static void
recget(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Rd *rd;
	Rec *r;
	Cval *ndx;
	Str *mn;
	
	rd = valrd(disp[0]);
	mn = valstr(disp[1]);
	ndx = valcval(disp[2]);

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to %.*s",
		      (int)mn->len, mn->s);
	if(Vkind(argv[0]) != Qrec)
		vmerr(vm, "operand 1 to %.*s must be a %.*s record",
		      (int)mn->len, mn->s, (int)rd->name->len, rd->name->s);
	r = valrec(argv[0]);
	if(r->rd != rd)
		vmerr(vm, "operand 1 to %.*s must be a %.*s record",
		      (int)mn->len, mn->s, (int)rd->name->len, rd->name->s);

	/* weak test for compatible record descriptor */
	if(r->nf <= ndx->val)
		vmerr(vm, "attempt to call %.*s on incompatible %.*s record",
		      (int)mn->len, mn->s, (int)rd->name->len, rd->name->s);

	*rv = r->field[ndx->val];
}

static void
recset(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Rec *r;
	Rd *rd;
	Cval *ndx;
	Str *mn;
	
	rd = valrd(disp[0]);
	mn = valstr(disp[1]);
	ndx = valcval(disp[2]);

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to %.*s",
		      (int)mn->len, mn->s);
	if(Vkind(argv[0]) != Qrec)
		vmerr(vm, "operand 1 to %.*s must be a %.*s record",
		      (int)mn->len, mn->s, (int)rd->name->len, rd->name->s);
	r = valrec(argv[0]);
	if(r->rd != rd)
		vmerr(vm, "operand 1 to %.*s must be a %.*s record",
		      (int)mn->len, mn->s, (int)rd->name->len, rd->name->s);

	/* weak test for compatible record descriptor */
	if(r->nf <= ndx->val)
		vmerr(vm, "attempt to call %.*s on incompatible %.*s record",
		      (int)mn->len, mn->s, (int)rd->name->len, rd->name->s);

	gcwb(thegc, r->field[ndx->val]);
	r->field[ndx->val] = argv[1];
	*rv = argv[1];
}

static Rd*
mkrd(VM *vm, Str *name, List *fname, Closure *fmt)
{
	Rd *rd;
	Imm n;
	Closure *cl;
	unsigned len;
	Str *f;
	Val mn;
	char *buf;

	rd = hget(vm->top->env->rd, name->s, (unsigned)name->len);
	if(rd == 0){
		rd = (Rd*)halloc(&heap[Qrd]);
		hput(vm->top->env->rd,
		     xstrndup(name->s, (unsigned)name->len),
		     (unsigned)name->len, rd);
	}else{
		gcwb(thegc, mkvalstr(rd->name));
		gcwb(thegc, mkvallist(rd->fname));
		gcwb(thegc, mkvalcl(rd->is));
		gcwb(thegc, mkvalcl(rd->mk));
		gcwb(thegc, mkvalcl(rd->fmt));
		gcwb(thegc, mkvaltab(rd->get));
		gcwb(thegc, mkvaltab(rd->set));
	}

	listlen(mkvallist(fname), &rd->nf);
	rd->name = name;
	rd->fname = fname;

	len = 3+name->len+1;
	buf = emalloc(len);

	snprint(buf, len, "is%.*s", (int)name->len, name->s);
	rd->is = mkccl(buf, recis, 2, mkvalrd(rd), mkvalstr(mkstr0(buf)));
	snprint(buf, len, "%.*s", (int)name->len, name->s);
	rd->mk = mkccl(buf, recmk, 2, mkvalrd(rd), mkvalstr(mkstr0(buf)));
	if(fmt == 0){
		snprint(buf, len, "fmt%.*s", (int)name->len, name->s);
		rd->fmt = mkccl(buf, recfmt, 2,
				mkvalrd(rd), mkvalstr(mkstr0(buf)));
	}else
		rd->fmt = fmt;
	efree(buf);

	rd->get = mktab();
	rd->set = mktab();
	for(n = 0; n < rd->nf; n++){
		f = valstr(listref(vm, fname, n));
		len = name->len+3+f->len+1;
		buf = emalloc(len);

		/* get method */
		snprint(buf, len, "%.*s%.*s",
			 (int)name->len, name->s, (int)f->len, f->s);
		mn = mkvalstr(mkstr0(buf));
		cl = mkccl(buf, recget, 3,
			   mkvalrd(rd), mn, mkvallitcval(Vuint, n));
		_tabput(rd->get, mkvalstr(f), mkvalcl(cl));

		/* set method */
		snprint(buf, len, "%.*sset%.*s",
			 (int)name->len, name->s, (int)f->len, f->s);
		mn = mkvalstr(mkstr0(buf));
		cl = mkccl(buf, recset, 3,
			   mkvalrd(rd), mn, mkvallitcval(Vuint, n));
		_tabput(rd->set, mkvalstr(f), mkvalcl(cl));

		efree(buf);
	}
	return rd;
}

static Xtypename*
mkxtn()
{
	Xtypename *xtn;
	xtn = (Xtypename*)halloc(&heap[Qxtn]);
	return xtn;
}

static Head*
iterxtn(Head *hd, Ictx *ictx)
{
	Xtypename *xtn;

	xtn = (Xtypename*)hd;
	switch(xtn->tkind){
	case Tvoid:
	case Tbase:
		return GCiterdone;
	case Tstruct:
	case Tunion:
		switch(ictx->n++){
		case 0:
			return (Head*)xtn->tag;
		case 1:
			return (Head*)xtn->field;
		case 2:
			return valhead(xtn->sz);
		default:
			return GCiterdone;
		}
	case Tenum:
		switch(ictx->n++){
		case 0:
			return (Head*)xtn->tag;
		case 1:
			return (Head*)xtn->link;
		case 2:
			return (Head*)xtn->konst;
		default:
			return GCiterdone;
		}
	case Tundef:
	case Tptr:
		if(ictx->n++ > 0)
			return GCiterdone;
		else
			return (Head*)xtn->link;
	case Tarr:
		switch(ictx->n++){
		case 0:
			return valhead(xtn->cnt);
		case 1:
			return (Head*)xtn->link;
		default:
			return GCiterdone;
		}
	case Tfun:
		switch(ictx->n++){
		case 0:
			return (Head*)xtn->link;
		case 1:
			return (Head*)xtn->param;
		default:
			return GCiterdone;
		}
		break;
	case Ttypedef:
		switch(ictx->n++){
		case 0:
			return (Head*)xtn->link;
		case 1:
			return (Head*)xtn->tid;
		default:
			return GCiterdone;
		}
	case Tbitfield:
		switch(ictx->n++){
		case 0:
			return valhead(xtn->sz);
		case 1:
			return valhead(xtn->bit0);
		case 2:
			return (Head*)xtn->link;
		default:
			return GCiterdone;
		}
	case Tconst:
		switch(ictx->n++){
		case 0:
			return (Head*)xtn->link;
		default:
			return GCiterdone;
		}
	case Txaccess:
		switch(ictx->n++){
		case 0:
			return (Head*)xtn->link;
		case 1:
			return (Head*)xtn->get;
		case 2:
			return (Head*)xtn->put;
		default:
			return GCiterdone;
		}

	}
	return 0;
}

static As*
mkas()
{
	As *as;
	as = (As*)halloc(&heap[Qas]);
	return as;
}

static Head*
iteras(Head *hd, Ictx *ictx)
{
	/* FIXME: is it really necessary
	   to mark dispatch and the other
	   cached functions? */
	As *as;
	as = (As*)hd;
	switch(ictx->n++){
	case 0:
		return (Head*)as->mtab;
	case 1:
		return (Head*)as->name;
	case 2:
		return (Head*)as->get;
	case 3:
		return (Head*)as->put;
	case 4:
		return (Head*)as->map;
	case 5:
		return (Head*)as->dispatch;
	default:
		return GCiterdone;
	}
}

static Dom*
mkdom(Ns *ns, As *as, Str *name)
{
	Dom *dom;
	dom = (Dom*)halloc(&heap[Qdom]);
	dom->ns = ns;
	dom->as = as;
	dom->name = name;
	return dom;
}

static Head*
iterdom(Head *hd, Ictx *ictx)
{
	Dom *dom;
	dom = (Dom*)hd;
	switch(ictx->n++){
	case 0:
		return (Head*)dom->as;
	case 1:
		return (Head*)dom->ns;
	case 2:
		return (Head*)dom->name;
	default:
		return GCiterdone;
	}
}

static Ns*
mkns()
{
	Ns *ns;
	ns = (Ns*)halloc(&heap[Qns]);
	return ns;
}

static Head*
iterns(Head *hd, Ictx *ictx)
{
	/* FIXME: is it really necessary
	   to mark dispatch and the other
	   cached functions? */
	Ns *ns;
	unsigned n;
	enum { lastfield = 7 };

	ns = (Ns*)hd;
	n = ictx->n++;
	switch(n){
	case 0:
		return (Head*)ns->lookaddr;
	case 1:
		return (Head*)ns->looksym;
	case 2:
		return (Head*)ns->looktype;
	case 3:
		return (Head*)ns->enumtype;
	case 4:
		return (Head*)ns->enumsym;
	case 5:
		return (Head*)ns->name;
	case 6:
		return (Head*)ns->dispatch;
	case lastfield:
		return (Head*)ns->mtab;
	}
	n -= lastfield;
	if(n >= Vnbase) /* assume elements at+above nbase are aliases */
		return GCiterdone;
	return (Head*)ns->base[n];
}

Env*
mkenv()
{
	Env *env;
	env = emalloc(sizeof(Env));
	env->var = mkht();
	env->rd = mkht();
	env->con = mkxenv(0);
	return env;
}

int
envbinds(Env *env, char *id)
{
	if(hget(env->var, id, strlen(id)))
		return 1;
	else
		return 0;
}

Val*
envgetbind(Env *env, char *id)
{
	Val *v;

	v = hget(env->var, id, strlen(id));
	if(!v){
		v = emalloc(sizeof(Val));
		*v = Xundef;
		hput(env->var, xstrdup(id), strlen(id), v);
	}
	return v;
}

Val*
envget(Env *env, char *id)
{
	return hget(env->var, id, strlen(id));
}

static void
envbind(Env *env, char *id, Val val)
{
	Val *v;

	v = envgetbind(env, id);
	*v = val;
}

static int
envlookup(Env *env, char *id, Val *val)
{
	Val *v;

	v = envgetbind(env, id);
	if(Vkind(*v) == Qundef)
		return 0;
	*val = *v;
	return 1;
}

static void
freebinding(void *u, char *id, void *v)
{
	efree(id);
	efree((Val*)v);
}

static void
freerd(void *u, char *id, void *v)
{
	efree(id);
}

void
freeenv(Env *env)
{
	hforeach(env->var, freebinding, 0);
	hforeach(env->rd, freerd, 0);
	xenvforeach(env->con, freeconst, 0);
	freeht(env->var);
	freeht(env->rd);
	freexenv(env->con);
	efree(env);
}

Toplevel*
mktoplevel(Xfd *in, Xfd *out, Xfd *err)
{
	Toplevel *top;

	top = emalloc(sizeof(Toplevel));
	top->env = mktopenv();

	/* persistent storage for stdio xfds */
	top->in = *in;
	top->out = *out;
	top->err = *err;

	builtinfd(top->env, "stdin",
		  mkfdfn(mkstr0("<stdin>"), Fread, &top->in));
	builtinfd(top->env, "stdout",
		  mkfdfn(mkstr0("<stdout>"), Fwrite, &top->out));
	builtinfd(top->env, "stderr",
		  mkfdfn(mkstr0("<stderr>"), Fwrite, &top->err));
	return top;
}

void
freetoplevel(Toplevel *top)
{
	freeenv(top->env);
	efree(top);
}

Cval*
mkcval(Dom *dom, Xtypename *type, Imm val)
{
	Cval *cv;
	cv = (Cval*)halloc(&heap[Qcval]);
	cv->dom = dom;
	cv->type = type;
	cv->val = val;
	return cv;
}

Cval*
mklitcval(Cbase base, Imm val)
{
	return mkcval(litdom, litbase[base], val);
}

Val
mkvalcval(Dom *dom, Xtypename *t, Imm imm)
{
	return (Val)mkcval(dom, t, imm);
}

Val
mkvallitcval(Cbase base, Imm imm)
{
	return (Val)mklitcval(base, imm);
}

Val
mkvalcval2(Cval *cv)
{
	return (Val)cv;
}

Val
mkvalbox(Val boxed)
{
	Box *box;
	box = (Box*)halloc(&heap[Qbox]);
	box->v = boxed;
	return (Val)box;
}

Val
mkvalpair(Val car, Val cdr)
{
	Pair *pair;
	pair = (Pair*)halloc(&heap[Qpair]);
	pair->car = car;
	pair->cdr = cdr;
	return (Val)pair;
}

Range*
mkrange(Cval *beg, Cval *len)
{
	Range *r;
	r = (Range*)halloc(&heap[Qrange]);
	r->beg = beg;
	r->len = len;
	return r;
}

Val
mkvalrange(Cval *beg, Cval *len)
{
	Range *r;
	r = mkrange(beg, len);
	return (Val)r;
}

Val
mkvalrange2(Range *r)
{
	return (Val)r;
}

static void
putbox(VM *vm, Val box, Val boxed)
{
	Box *b;
//	if(Vkind(box != Qbox)
//		fatal("putbox on non-box");
//	if(Vkind(boxed) == Qbox)
//		fatal("boxing boxes is insane");
	b = (Box*)box;
	gcwb(thegc, b->v);
	b->v = boxed;
}

static void
putval(VM *vm, Val v, Location *loc)
{
	Val *dst;

	switch(LOCKIND(loc->loc)){
	case Lreg:
		switch(LOCIDX(loc->loc)){
		case Rac:
			vm->ac = v;
			break;
		case Rsp:
		case Rfp:
		case Rpc:
		case Rcl:
		default:
			fatal("bug");
		}
		break;
	case Lparam:
		dst = &vm->stack[(vm->fp+1)+LOCIDX(loc->loc)];
		if(LOCBOX(loc->loc))
			putbox(vm, *dst, v);
		else
			*dst = v;
		break;
	case Llocal:
		dst = &vm->stack[(vm->fp-1)-LOCIDX(loc->loc)];
		if(LOCBOX(loc->loc))
			putbox(vm, *dst, v);
		else
			*dst = v;
		break;
	case Ldisp:
		dst = &vm->clx->display[LOCIDX(loc->loc)];
		if(LOCBOX(loc->loc))
			putbox(vm, *dst, v);
		else
			*dst = v;
		break;
	case Ltopl:
		dst = loc->var->val;
		*dst = v;
		break;
	default:
		fatal("bug");
	}
}

Tab*
doinsncnt(VM *vm)
{
	Tab *t;
	Imm pc;
	Head *p;
	Code *c;
	Insn *in;
	Src *src;
	Str *str;
	static char buf[256];
	char *fn;
	Val v;
	Cval *cv;

	t = mktab();
	p = heap[Qcode].alloc;
	while(p){
		if(Vcolor(p) == GCfree)
			goto next;
		c = (Code*)p;
		in = &c->insn[0];		
		for(pc = 0; pc < c->ninsn; pc++, in++){
			src = addr2line(c, pc);
			if(src == 0)
				continue;
			fn = src->filename;
			if(fn == 0)
				fn = "<stdin>";
			snprint(buf, sizeof(buf), "%s:%u", fn, src->line);
			str = mkstr0(buf);
			v = tabget(t, mkvalstr(str));
			if(v){
				cv = valcval(v);
				cv->val += in->cnt;
			}else
				tabput(vm, t, mkvalstr(str),
				       mkvallitcval(Vuvlong, in->cnt));
		}
	next:
		p = p->alink;
	}
	return t;
}

Src*
addr2line(Code *code, Imm pc)
{
	return code->insn[pc].src;
}

static void
printsrc(Xfd *xfd, Closure *cl, Imm pc)
{
	Code *code;
	Src *src;
	char *fn;
	
	code = cl->code;
	if(cl->cfn || cl->ccl){
		cprintf(xfd, "%20s\t(builtin %s)\n", cl->id,
			cl->cfn ? "function" : "closure");
		return;
	}

	src = addr2line(code, pc);
	if(src == 0){
		cprintf(xfd, "%20s\t(no source information)\n", cl->id);
		return;
	}
	fn = src->filename;
	if(fn == 0)
		fn = "<stdin>";
	cprintf(xfd, "%20s\t(%s:%u)\n", cl->id, fn, src->line);
}

static void
fvmbacktrace(VM *vm)
{
	Imm pc, fp, narg;
	Closure *cl;
	Xfd *xfd;

	xfd = &vm->top->out;

	pc = vm->pc-1;		/* vm loop increments pc after fetch */
	fp = vm->fp;
	cl = vm->clx;
	while(fp != 0){
		if(strcmp(cl->id, "$halt")){
//			cprintf(xfd, "fp=%05lld pc=%08lld ", fp, pc);
			printsrc(xfd, cl, pc);
		}
		narg = stkimm(vm->stack[fp]);
		pc = stkimm(vm->stack[fp+narg+1]);
		pc--; /* pc was insn following call */
		cl = valcl(vm->stack[fp+narg+2]);
		fp = stkimm(vm->stack[fp+narg+3]);
	}
}

static void
vmbacktrace(VM *vm)
{
	fvmbacktrace(vm);
}

void
vmerr(VM *vm, char *fmt, ...)
{
	va_list args;
	cprintf(&vm->top->out, "error: ");
	va_start(args, fmt);
	cvprintf(&vm->top->out, fmt, args);
	va_end(args);
	cprintf(&vm->top->out, "\n");
	fvmbacktrace(vm);
	nexterror(vm);
}

static Val
getval(VM *vm, Location *loc)
{
	Val p;

	switch(LOCKIND(loc->loc)){
	case Lreg:
		switch(LOCIDX(loc->loc)){
		case Rac:
			return vm->ac;
		case Rsp:
		case Rfp:
		case Rcl:
		case Rpc:
		default:
			fatal("bug");
		}
		break;
	case Lparam:
		p = vm->stack[(vm->fp+1)+LOCIDX(loc->loc)];
		if(LOCBOX(loc->loc))
			return valboxed(p);
		else
			return p;
	case Llocal:
		p = vm->stack[(vm->fp-1)-LOCIDX(loc->loc)];
		if(LOCBOX(loc->loc))
			return valboxed(p);
		else
			return p;
	case Ldisp:
		p = vm->clx->display[LOCIDX(loc->loc)];
		if(LOCBOX(loc->loc))
			return valboxed(p);
		else
			return p;
	case Ltopl:
		p = *(loc->var->val);
		if(p == Xundef)
			vmerr(vm, "reference to unbound variable: %s",
			      loc->var->id);
		return p;
	default:
		fatal("bug");
	}
}

static Cval*
getcval(VM *vm, Location *loc)
{
	Val p;

	switch(LOCKIND(loc->loc)){
	case Lreg:
		switch(LOCIDX(loc->loc)){
		case Rac:
			return valcval(vm->ac);
		case Rsp:
		case Rfp:
		case Rpc:
		case Rcl:
		default:
			fatal("bug");
		}
		break;
	case Lparam:
		p = vm->stack[(vm->fp+1)+LOCIDX(loc->loc)];
		if(LOCBOX(loc->loc))
			return valboxedcval(p);
		return valcval(p);
	case Llocal:
		p = vm->stack[(vm->fp-1)-LOCIDX(loc->loc)];
		if(LOCBOX(loc->loc))
			return valboxedcval(p);
		return valcval(p);
	case Ldisp:
		p = vm->clx->display[LOCIDX(loc->loc)];
		if(LOCBOX(loc->loc))
			return valboxedcval(p);
		return valcval(p);
	case Ltopl:
		p = *(loc->var->val);
		if(Vkind(p) == Qundef)
			vmerr(vm, "reference to unbound variable: %s",
			      loc->var->id);
		return valcval(p);
	default:
		fatal("bug");
	}
}

static Val
getvalrand(VM *vm, Operand *r)
{
	Val v;
	switch(r->okind){
	case Oloc:
		return getval(vm, &r->u.loc);
	case Okon:
		v = r->u.kon;
		/* any mutable vals must be copied */
		switch(Vkind(v)){
		case Qstr:
			v = mkvalstr(strcopy(valstr(v)));
			break;
		default:
			break;
		}
		return v;
	case Onil:
		return Xnil;
	default:
		fatal("bug");
	}
}

static Cval*
getcvalrand(VM *vm, Operand *r)
{
	switch(r->okind){
	case Oloc:
		return getcval(vm, &r->u.loc);
	case Okon:
		/* FIXME: check for cval? */
		return valcval(r->u.kon);
	default:
		fatal("bug");
	}
}

static void
putvalrand(VM *vm, Val v, Operand *r)
{
	if(r->okind != Oloc)
		fatal("bad destination");
	putval(vm, v, &r->u.loc);
}

static void
putcvalrand(VM *vm, Cval *cv, Operand *r)
{
	if(r->okind != Oloc)
		fatal("bad destination");
	putval(vm, mkvalcval2(cv), &r->u.loc);
}

static Imm
getbeint(char *s, unsigned nb)
{
	unsigned i;
	Imm w;
	w = 0;
	for(i = 0; i < nb; i++){
		w <<= 8;
		w |= (s[i]&0xff);
	}
	return w;
}

static Imm
str2imm(Xtypename *xtn, Str *str)
{
	char *s;

	xtn = chasetype(xtn);
	if(xtn->tkind != Tbase && xtn->tkind != Tptr)
		fatal("str2imm on non-scalar type");

	s = str->s;
	switch(xtn->rep){
	case Rs08le:
		return *(s8*)s;
	case Rs16le:
		return *(s16*)s;
	case Rs32le:
		return *(s32*)s;
	case Rs64le:
		return *(s64*)s;
	case Ru08le:
		return *(u8*)s;
	case Ru16le:
		return *(u16*)s;
	case Ru32le:
		return *(u32*)s;
	case Ru64le:
		return *(u64*)s;
	case Ru08be:
		return (u8)getbeint(s, 1);
	case Ru16be:
		return (u16)getbeint(s, 2);
	case Ru32be:
		return (u32)getbeint(s, 4);
	case Ru64be:
		return (u64)getbeint(s, 8);
	case Rs08be:
		return (s8)getbeint(s, 1);
	case Rs16be:
		return (s16)getbeint(s, 2);
	case Rs32be:
		return (s32)getbeint(s, 4);
	case Rs64be:
		return (s64)getbeint(s, 8);
	default:
		fatal("bug");
	}	
}

static void
putbeint(char *p, Imm w, unsigned nb)
{
	int i;
	for(i = nb-1; i >= 0; i--){
		p[i] = w&0xff;
		w >>= 8;
	}
}

static Str*
imm2str(VM *vm, Xtypename *xtn, Imm imm)
{
	Str *str;
	char *s;

	xtn = chasetype(xtn);
	if(xtn->tkind != Tbase && xtn->tkind != Tptr)
		fatal("imm2str on non-scalar type");

	switch(xtn->rep){
	case Rs08le:
		str = mkstrn(vm, sizeof(s8));
		s = str->s;
		*(s8*)s = (s8)imm;
		return str;
	case Rs16le:
		str = mkstrn(vm, sizeof(s16));
		s = str->s;
		*(s16*)s = (s16)imm;
		return str;
	case Rs32le:
		str = mkstrn(vm, sizeof(s32));
		s = str->s;
		*(s32*)s = (s32)imm;
		return str;
	case Rs64le:
		str = mkstrn(vm, sizeof(s64));
		s = str->s;
		*(s64*)s = (s64)imm;
		return str;
	case Ru08le:
		str = mkstrn(vm, sizeof(u8));
		s = str->s;
		*(u8*)s = (u8)imm;
		return str;
	case Ru16le:
		str = mkstrn(vm, sizeof(u16));
		s = str->s;
		*(u16*)s = (u16)imm;
		return str;
	case Ru32le:
		str = mkstrn(vm, sizeof(u32));
		s = str->s;
		*(u32*)s = (u32)imm;
		return str;
	case Ru64le:
		str = mkstrn(vm, sizeof(u64));
		s = str->s;
		*(u64*)s = (u64)imm;
		return str;
	case Rs08be:
		str = mkstrn(vm, sizeof(s8));
		s = str->s;
		putbeint(s, imm, 1);
		return str;
	case Rs16be:
		str = mkstrn(vm, sizeof(s16));
		s = str->s;
		putbeint(s, imm, 2);
		return str;
	case Rs32be:
		str = mkstrn(vm, sizeof(s32));
		s = str->s;
		putbeint(s, imm, 4);
		return str;
	case Rs64be:
		str = mkstrn(vm, sizeof(s64));
		s = str->s;
		putbeint(s, imm, 8);
		return str;
	case Ru08be:
		str = mkstrn(vm, sizeof(u8));
		s = str->s;
		putbeint(s, imm, 1);
		return str;
	case Ru16be:
		str = mkstrn(vm, sizeof(u16));
		s = str->s;
		putbeint(s, imm, 2);
		return str;
	case Ru32be:
		str = mkstrn(vm, sizeof(u32));
		s = str->s;
		putbeint(s, imm, 4);
		return str;
	case Ru64be:
		str = mkstrn(vm, sizeof(u64));
		s = str->s;
		putbeint(s, imm, 8);
		return str;
	default:
		fatal("bug");
	}	
}

/* transform representation of VAL used for type OLD to type NEW.
   OLD and NEW must be chased types with defined representations. */
static Imm
_rerep(Imm val, Xtypename *old, Xtypename *new)
{
	/* FIXME: non-trivial cases are : real <-> int,
	   real <-> alternate real
	   integer truncation
	   (so div and shr work)
	*/
	switch((new->rep<<5)|old->rep){
		#include "rerep.switch" /* re-cast val */
	}
 	return val;
}


static Imm
rerep(Imm val, Xtypename *old, Xtypename *new)
{
	old = chasetype(old);
	new = chasetype(new);
	if(old->rep == Rundef || new->rep == Rundef)
		fatal("undef!");
	return _rerep(val, old, new);
}

Cval*
typecast(VM *vm, Xtypename *xtn, Cval *cv)
{
	Xtypename *old, *new;
	old = chasetype(cv->type);
	new = chasetype(xtn);
	if(new->rep == Rundef){
		/* steal pointer representation from old */
		if(old->rep == Rundef)
			/* this is possible? */
			fatal("whoa.");
		if(old->tkind != Tptr)
			vmerr(vm, "attempt to cast to type "
			      "with undefined representation");
		new->rep = old->rep;
	}
	return mkcval(cv->dom, xtn, _rerep(cv->val, old, new));
}

Cval*
domcastbase(VM *vm, Dom *dom, Cval *cv)
{
	Xtypename *xtn, *old, *new;
	Str *es;

	xtn = cv->type;
	if(dom == vm->litdom)
		xtn = chasetype(xtn);
	if(xtn->tkind != Tbase)
		vmerr(vm, "operand must be of base type");

	/* FIXME: do we really want to lookup the type in the new domain? */
	xtn = dom->ns->base[xtn->basename];
	if(xtn == 0){
		es = fmtxtn(cv->type);
		vmerr(vm, "cast to domain that does not define %.*s",
		      (int)es->len, es->s);
	}
	old = chasetype(cv->type);
	new = chasetype(xtn);
	if(old->rep == Rundef || new->rep == Rundef)
		vmerr(vm, " attempt to cast to type "
		      "with undefined representation");
	return mkcval(dom, xtn, rerep(cv->val, old, new));
}

Cval*
domcast(VM *vm, Dom *dom, Cval *cv)
{
	Xtypename *xtn, *old, *new;
	Str *es;

	/* FIXME: do we really want to lookup the type in the new domain? */
	if(dom == vm->litdom)
		xtn = dolooktype(vm, chasetype(cv->type), dom->ns);
	else
		xtn = dolooktype(vm, cv->type, dom->ns);
	if(xtn == 0){
		es = fmtxtn(cv->type);
		vmerr(vm, "cast to domain that does not define %.*s",
		      (int)es->len, es->s);
	}
	old = chasetype(cv->type);
	new = chasetype(xtn);
	if(old->rep == Rundef || new->rep == Rundef)
		vmerr(vm, " attempt to cast to type "
		      "with undefined representation");
	return mkcval(dom, xtn, rerep(cv->val, old, new));
}

static void
dompromote(VM *vm, ikind op, Cval *op1, Cval *op2, Cval **rv1, Cval **rv2)
{
	Xtypename *b1, *b2;
	static char *domerr
		= "attempt to combine cvalues of incompatible domains"; 

	if(op1->dom == op2->dom)
		goto out;

	if(op1->dom == vm->litdom)
		op1 = domcast(vm, op2->dom, op1);
	else if(op2->dom == vm->litdom)
		op2 = domcast(vm, op1->dom, op2);
	else{
		b1 = chasetype(op1->type);
		b2 = chasetype(op2->type);
		if(b1->tkind != Tptr && b2->tkind != Tptr){
			op1 = domcastbase(vm, vm->litdom, op1);
			op2 = domcastbase(vm, vm->litdom, op2);
		}else if(b1->tkind == Tptr && b2->tkind == Tptr)
			vmerr(vm, domerr);
		else if(b1->tkind == Tptr){
			if(op == Iadd || op == Isub)
				op2 = domcast(vm, op1->dom, op2);
			else
				vmerr(vm, domerr);
		}else /* b2->tkind == Tptr */ {
			if(op == Iadd || op == Isub)
				op1 = domcast(vm, op2->dom, op1);
			else
				vmerr(vm, domerr);
		}
	}
out:
	*rv1 = op1;
	*rv2 = op2;
}

static Cval*
intpromote(VM *vm, Cval *cv)
{
	Xtypename *base;

	base = chasetype(cv->type);
	if(base->tkind != Tbase)
		return cv;
	switch(base->basename){
	case Vuchar:
	case Vushort:
	case Vchar:
	case Vshort:
		return typecast(vm, cv->dom->ns->base[Vint], cv);
	default:
		break;
	}
	return cv;
}

static Xtypename*
commontype(Xtypename *t1, Xtypename *t2)
{
	Xtypename *p1, *p2;
	p1 = t1;
	while(1){
		p2 = t2;
		while(1){
			if(equalxtn(p1, p2))
				return p1;
			if(p2->tkind != Ttypedef && p2->tkind != Tenum)
				break;
			p2 = p2->link;
		}
		if(p1->tkind != Ttypedef && p1->tkind != Tenum)
			break;
		p1 = p1->link;
	}
	fatal("cannot determine common type");
}

static void
usualconvs(VM *vm, Cval *op1, Cval *op2, Cval **rv1, Cval **rv2)
{
	/* FIXME: why assign ranks to types below int promotion? */
	static unsigned rank[Vnbase] = {
		[Vchar] = 0,
		[Vuchar] = 0,
		[Vshort] = 1,
		[Vushort] = 1,
		[Vint] = 2,
		[Vuint] = 2,
		[Vlong] = 3,
		[Vulong] = 3,
		[Vvlong] = 4,
		[Vuvlong] = 4,
	};
	static unsigned uvariant[Vnbase] = {
		[Vchar] = Vuchar,
		[Vshort] = Vushort,
		[Vint] = Vuint,
		[Vlong] = Vulong,
		[Vvlong] = Vuvlong,
	};
	Cbase c1, c2, nc;
	Xtypename *t1, *t2, *b1, *b2, *nxtn;

	op1 = intpromote(vm, op1);
	op2 = intpromote(vm, op2);

	t1 = op1->type;
	t2 = op2->type;
	b1 = chasetype(t1);
	b2 = chasetype(t2);
	if(b1->tkind != Tbase || b2->tkind != Tbase){
		/* presumably one of them is a Tptr; nothing else is sane. */
		*rv1 = op1;
		*rv2 = op2;
		return;
	}

	c1 = b1->basename;
	c2 = b2->basename;
	if(c1 == c2){
		/* combinations of distinct typedefs
		   and/or enums yield the first type
		   they have in common (not necessarily
		   the base type). */
		if(t1->tkind == Ttypedef || t2->tkind == Ttypedef){
			nxtn = commontype(t1, t2);
			if(t1 != nxtn)
				op1 = typecast(vm, nxtn, op1);
			if(t2 != nxtn)
				op2 = typecast(vm, nxtn, op2);
		}
		*rv1 = op1;
		*rv2 = op2;
		return;
	}

	if((isunsigned[c1] && isunsigned[c2])
	   || (!isunsigned[c1] && !isunsigned[c2])){
		if(rank[c1] < rank[c2])
			nc = c2;
		else
			nc = c1;
	}else if(isunsigned[c1] && rank[c1] >= rank[c2])
		nc = c1;
	else if(isunsigned[c2] && rank[c2] >= rank[c1])
		nc = c2;
	else if(!isunsigned[c1] && repsize[b1->rep] > repsize[b2->rep])
		nc = c1;
	else if(!isunsigned[c2] && repsize[b2->rep] > repsize[b1->rep])
		nc = c2;
	else if(!isunsigned[c1])
		nc = uvariant[c1];
	else
		nc = uvariant[c2];

	nxtn = op1->dom->ns->base[nc];          /* op2 has the same domain */
	if(c1 != nc)
		op1 = typecast(vm, nxtn, op1);
	if(c2 != nc)
		op2 = typecast(vm, nxtn, op2);
	*rv1 = op1;
	*rv2 = op2;
}

static void
xcallc(VM *vm)
{
	Imm argc;
	Val *argv;
	Val rv;

	if(vm->clx->cfn == 0 && vm->clx->ccl == 0)
		vmerr(vm, "bad closure for builtin call");

	rv = Xnil;
	argc = stkimm(vm->stack[vm->fp]);
	argv = &vm->stack[vm->fp+1];
	if(vm->clx->cfn)
		vm->clx->cfn(vm, argc, argv, &rv);
	else
		vm->clx->ccl(vm, argc, argv, vm->clx->display, &rv);
	vm->ac = rv;
}

static void
xspec(VM *vm, Operand *op)
{
	Val v;
	Cval *cv;
	Imm idx;

	v = getvalrand(vm, op);
	cv = valcval(v);
	idx = cv->val;
	xprintf("spec %d\n", idx);
	cgspec(vm, vm->clx, idx, vm->ac);
	vmsetcl(vm, vm->cl);
	vm->sp = vm->fp;
	vm->pc = vm->clx->entry;
//	vm->ibuf = vm->clx->code->insn;
//	setgo(&vm->ibuf[vm->pc], vm->clx->code->ninsn-vm->pc);
}

static int
Strcmp(Str *s1, Str *s2)
{
	unsigned char *p1, *p2;
	Imm l1, l2;

	p1 = (unsigned char*)s1->s;
	p2 = (unsigned char*)s2->s;
	l1 = s1->len;
	l2 = s2->len;
	while(l1 && l2){
		if(*p1 < *p2)
			return -1;
		else if(*p1 > *p2)
			return 1;
		p1++;
		p2++;
		l1--;
		l2--;
	}
	if(l1)
		return 1;
	else if(l2)
		return -1;
	else
		return 0;
}

static Imm
xstrcmp(VM *vm, ikind op, Str *s1, Str *s2)
{
	int x;

	switch(op){
	case Icmpeq:
		return equalstr(s1, s2);
	case Icmpneq:
		return !equalstr(s1, s2);
	case Icmpgt:
		x = Strcmp(s1, s2);
		return x > 0 ? 1 : 0;
	case Icmpge:
		x = Strcmp(s1, s2);
		return x >= 0 ? 1 : 0;
	case Icmplt:
		x = Strcmp(s1, s2);
		return x < 0 ? 1 : 0;
	case Icmple:
		x = Strcmp(s1, s2);
		return x <= 0 ? 1 : 0;
	default:
		vmerr(vm, "attempt to apply %s to string operands", opstr[op]);
	}
}

static void
xunop(VM *vm, ikind op, Operand *op1, Operand *dst)
{
	Val v;
	Cval *cv, *cvr;
	Imm imm, nv;
	
	v = getvalrand(vm, op1);
	if(Vkind(v) != Qcval)
		vmerr(vm, "incompatible operand for unary %s", opstr[op]);
	cv = intpromote(vm, valcval(v));
	imm = cv->val;

	switch(op){
	case Ineg:
		nv = -imm;
		cvr = mkcval(cv->dom, cv->type, nv);
		break;
	case Iinv:
		nv = ~imm;
		cvr = mkcval(cv->dom, cv->type, nv);
		break;
	case Inot:
		if(imm)
			cvr = cval0;
		else
			cvr = cval1;
		break;
	default:
		fatal("unknown unary operator %d", op);
	}
	putcvalrand(vm, cvr, dst);
}

static Imm
truncimm(Imm v, Rkind rep)
{
	switch(rep){
	case Rs08be:
	case Rs08le:
		return (s8)v;
	case Rs16be:
	case Rs16le:
		return (s16)v;
	case Rs32be:
	case Rs32le:
		return (s32)v;
	case Rs64be:
	case Rs64le:
		return (s64)v;
	case Ru08be:
	case Ru08le:
		return (u8)v;
	case Ru16be:
	case Ru16le:
		return (u16)v;
	case Ru32be:
	case Ru32le:
		return (u32)v;
	case Ru64be:
	case Ru64le:
		return (u64)v;
	default:
		fatal("bug");
	}
}

static Cval*
xcvalptralu(VM *vm, ikind op, Cval *op1, Cval *op2,
	    Xtypename *t1, Xtypename *t2)
{
	Xtypename *sub, *pt;
	Cval *ptr;
	Imm sz, osz, n;

	if(op != Iadd && op != Isub)
		vmerr(vm, "attempt to apply %s to pointer operand", opstr[op]);

	if(t1->tkind == Tptr && t2->tkind == Tptr){
		if(op != Isub)
			vmerr(vm, "attempt to apply %s to pointer operands",
			      opstr[op]);
		sub = chasetype(t1->link);
		/* special case: sizeof(void)==1 for pointer arith */
		if(sub->tkind == Tvoid)
			sz = 1;
		else
			sz = typesize(vm, sub);
		sub = chasetype(t2->link);
		if(sub->tkind == Tvoid)
			osz = 1;
		else
			osz = typesize(vm, sub);
		if(sz != osz)
			vmerr(vm, "attempt to subtract pointers to "
			      "objects of different sizes");
		if(sz == 0)
			vmerr(vm, "attempt to subtract pointers to "
			      "zero-sized objects");
		n = (op1->val-op2->val)/sz;
		n = truncimm(n, t1->rep);
		/* FIXME: define ptrdiff_t? */
		return mkcval(vm->litdom, vm->litbase[Vlong], n);
	}

	/* exactly one operand is a pointer */

	if(t1->tkind == Tptr){
		sub = chasetype(t1->link);
		ptr = op1;
		n = op2->val;
	}else if(op == Isub)
		vmerr(vm, "invalid right-hand pointer operand to -");
	else{
		sub = chasetype(t2->link);
		ptr = op2;
		n = op1->val;
	}

	/* special case: sizeof(void)==1 for pointer arith */
	if(sub->tkind == Tvoid)
		sz = 1;
	else
		sz = typesize(vm, sub);
	/* FIXME: what about undefined sub types? */
	if(op == Iadd)
		n = ptr->val+n*sz;
	else
		n = ptr->val-n*sz;
	pt = chasetype(ptr->type);
	n = truncimm(n, pt->rep);
	return mkcval(ptr->dom, ptr->type, n);
}

static Cval*
xcvalalu1dom(VM *vm, ikind op, Cval *op1, Cval *op2)
{
	Imm i1, i2, rv;
	Xtypename *t1, *t2;

	if(op1->dom != op2->dom)
		fatal("bug");
	usualconvs(vm, op1, op2, &op1, &op2);
	t1 = chasetype(op1->type);
	t2 = chasetype(op2->type);
	if(t1->tkind == Tptr || t2->tkind == Tptr)
		return xcvalptralu(vm, op, op1, op2, t1, t2);

	i1 = op1->val;
	i2 = op2->val;
	
	switch(op){
	case Iadd:
		rv = i1+i2;
		break;
	case Idiv:
		if(i2 == 0)
			vmerr(vm, "divide by zero");
		if(isunsigned[t1->basename] && isunsigned[t2->basename])
			rv = i1/i2;
		else if(isunsigned[t1->basename])
			rv = i1/(s64)i2;
		else if(isunsigned[t2->basename])
			rv = (s64)i1/i2;
		else
			rv = (s64)i1/(s64)i2;
		break;
	case Imod:
		if(i2 == 0)
			vmerr(vm, "divide by zero");
		rv = i1%i2;
		break;
	case Imul:
		rv = i1*i2;
		break;
	case Isub:
		rv = i1-i2;
		break;
	case Iand:
		rv = i1&i2;
		break;
	case Ixor:
		rv = i1^i2;
		break;
	case Ior:
		rv = i1|i2;
		break;
	default:
		fatal("bug");
	}		

	rv = truncimm(rv, t1->rep);
	return mkcval(op1->dom, op1->type, rv);
}

Cval*
xcvalalu(VM *vm, ikind op, Cval *op1, Cval *op2)
{
	dompromote(vm, op, op1, op2, &op1, &op2);
	return xcvalalu1dom(vm, op, op1, op2);
}

static Cval*
xcvalshift(VM *vm, ikind op, Cval *op1, Cval *op2)
{
	Imm i1, i2, rv;

	/* no need to rationalize domains */
	op1 = intpromote(vm, op1);
	op2 = intpromote(vm, op2);
	i1 = op1->val;
	i2 = op2->val;

	/* following C99:
	   - (both) if op2 is negative or >= width of op1,
	            the result is undefined;
	   - (<<)   if op1 is signed and op1*2^op2 is not representable in
	            typeof(op1), the result is undefined;
	   - (>>)   if op1 is signed and negative, the result is
                    implementation-defined; gcc's >> performs sign extension
	*/
	switch(op){
	case Ishl:
		rv = i1<<i2;
		break;
	case Ishr:
		rv = i1>>i2;
		break;
	default:
		fatal("bug");
	}
	return mkcval(op1->dom, op1->type, rv);
}

static Cval*
xcvalcmp(VM *vm, ikind op, Cval *op1, Cval *op2)
{
	Imm i1, i2, rv;

	dompromote(vm, op, op1, op2, &op1, &op2);
	usualconvs(vm, op1, op2, &op1, &op2);
	i1 = op1->val;
	i2 = op2->val;

	/* We're intentionally relaxed about whether one operand is
	   pointer so that expressions like (p == 0x<addr>) can be
	   written without cast clutter. */

	if(isunsigned[op1->type->basename])
		switch(op){
		case Icmpeq:
			rv = i1==i2;
			break;
		case Icmpneq:
			rv = i1!=i2;
			break;
		case Icmpgt:
			rv = i1>i2;
			break;
		case Icmpge:
			rv = i1>=i2;
			break;
		case Icmplt:
			rv = i1<i2;
			break;
		case Icmple:
			rv = i1<=i2;
			break;
		default:
			fatal("bug");
		}
	else
		switch(op){
		case Icmpeq:
			rv = (s64)i1==(s64)i2;
			break;
		case Icmpneq:
			rv = (s64)i1!=(s64)i2;
			break;
		case Icmpgt:
			rv = (s64)i1>(s64)i2;
			break;
		case Icmpge:
			rv = (s64)i1>=(s64)i2;
			break;
		case Icmplt:
			rv = (s64)i1<(s64)i2;
			break;
		case Icmple:
			rv = (s64)i1<=(s64)i2;
			break;
		default:
			fatal("bug");
		}
	if(rv)
		return cval1;
	else
		return cval0;
}

static void
xbinop(VM *vm, ikind op, Operand *op1, Operand *op2, Operand *dst)
{
	Val v1, v2;
	Cval *cv1, *cv2, *cvr;
	Str *s1, *s2;
	Imm nv;

	v1 = getvalrand(vm, op1);
	v2 = getvalrand(vm, op2);

	if(Vkind(v1) == Qcval && Vkind(v2) == Qcval){
		cv1 = valcval(v1);
		cv2 = valcval(v2);
		switch(op){
		case Iadd:
		case Iand:
		case Idiv:
		case Imod:
		case Imul:
		case Ior:
		case Isub:
		case Ixor:
			cvr = xcvalalu(vm, op, cv1, cv2);
			break;
		case Ishl:
		case Ishr:
			cvr = xcvalshift(vm, op, cv1, cv2);
			break;
		case Icmplt:
		case Icmple:
		case Icmpgt:
		case Icmpge:
		case Icmpeq:
		case Icmpneq:
			cvr = xcvalcmp(vm, op, cv1, cv2);
			break;
		default:
			fatal("bug");
		}
		putcvalrand(vm, cvr, dst);
		return;
	}

	if(Vkind(v1) == Qstr && Vkind(v2) == Qstr){
		s1 = valstr(v1);
		s2 = valstr(v2);
dostr:
		if(op == Iadd)
			putvalrand(vm, mkvalstr(strconcat(vm, s1, s2)), dst);
		else{
			nv = xstrcmp(vm, op, s1, s2);
			if(nv)
				putvalrand(vm, mkvalcval2(cval1), dst);
			else
				putvalrand(vm, mkvalcval2(cval0), dst);
		}
		return;
	}

	if(Vkind(v1) == Qstr && Vkind(v2) == Qcval){
		cv2 = valcval(v2);
		if(isstrcval(cv2)){
			gcprotect(vm, v1);
			s1 = valstr(v1);
			s2 = stringof(vm, cv2);
			gcunprotect(vm, v1);
			goto dostr;
		}
		/* fall through */
	}

	if(Vkind(v2) == Qstr && Vkind(v1) == Qcval){
		cv1 = valcval(v1);
		if(isstrcval(cv1)){
			gcprotect(vm, v2);
			s1 = stringof(vm, cv1);
			s2 = valstr(v2);
			gcunprotect(vm, v2);
			goto dostr;
		}
		/* fall through */
	}

	/* all other types support only == and != */
	if(op != Icmpeq && op != Icmpneq)
		vmerr(vm, "incompatible operands for binary %s", opstr[op]);

	nv = eqval(v1, v2);
	if(op == Icmpneq)
		nv = !nv;
	if(nv)
		putvalrand(vm, mkvalcval2(cval1), dst);
	else
		putvalrand(vm, mkvalcval2(cval0), dst);
}

static void
xclo(VM *vm, Operand *dl, Ctl *label, Operand *dst)
{
	Closure *cl;
	Cval *cv;
	Imm m, len;

	/* dl is number of values to copy from stack into display */
	/* label points to instruction in current closure's code */
	/* captured variables are in display order on stack,
	   from low to high stack address */

	cv = getcvalrand(vm, dl);
	len = cv->val;

	cl = mkcl(vm->clx->code, label->insn, len, label->label);
	for(m = 0; m < len; m++)
		cl->display[m] = vm->stack[vm->sp+m];
	vm->sp += m;

	putvalrand(vm, mkvalcl(cl), dst);
}

static void
xkg(VM *vm, Operand *dst)
{
	Closure *k;
	Imm len;

	len = Maxstk-vm->sp;
	k = mkcl(kcode, 0, len, 0);
	memcpy(k->display, &vm->stack[vm->sp], len*sizeof(Val));
	k->fp = vm->fp;

	putvalrand(vm, mkvalcl(k), dst);
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
	v = getvalrand(vm, src);
	putvalrand(vm, v, dst);
}

static int
zeroval(Val v)
{
	Cval *cv;

	switch(Vkind(v)){
	case Qcval:
		cv = valcval(v);
		return cv->val == 0;
	default:
		return 0;
	}
}

static void
xjnz(VM *vm, Operand *src, Ctl *label)
{
	Val v;
	v = getvalrand(vm, src);
	if(!zeroval(v))
		vm->pc = label->insn;
}

static void
xjz(VM *vm, Operand *src, Ctl *label)
{
	Val v;
	v = getvalrand(vm, src);
	if(zeroval(v))
		vm->pc = label->insn;
}

static void
checkoverflow(VM *vm, unsigned dec)
{
	if(dec > vm->sp)
		vmerr(vm, "stack overflow");
}

static void
vmpush(VM *vm, Val v)
{
	checkoverflow(vm, 1);
	vm->stack[--vm->sp] = v;
}

static void
vmpushi(VM *vm, Imm imm)
{
	checkoverflow(vm, 1);
	imm = (imm<<1)|1;
	vm->stack[--vm->sp] = (Val)(uintptr_t)imm;
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
	v = getvalrand(vm, op);
	vmpush(vm, v);
}

static void
xpushi(VM *vm, Operand *op)
{
	Val v;
	Cval *cv;

	v = getvalrand(vm, op);
	cv = valcval(v);
	vmpushi(vm, cv->val);
}

static void
xbox(VM *vm, Operand *op)
{
	Val v;
	v = getvalrand(vm, op);
	// specializer can trigger redundant boxing of parameters
	// when jumping from original to new function
	if(Vkind(v) == Qbox)
		return;
	putvalrand(vm, mkvalbox(v), op);
}

static void
xbox0(VM *vm, Operand *op)
{
	putvalrand(vm, mkvalbox(Xundef), op);
}

static void
xref(VM *vm, Operand *x, Operand *type, Operand *cval, Operand *dst)
{
	Val xv, typev, cvalv, rv;
	Xtypename *t, *b, *pt;
	Dom *d;
	Cval *cv;
	Imm imm;

	typev = getvalrand(vm, type);
	cvalv = getvalrand(vm, cval);
	xv = getvalrand(vm, x);
	if(Vkind(xv) != Qdom)
		vmerr(vm, "attempt to derive location from non-domain");
	d = valdom(xv);
	t = valxtn(typev);
	cv = valcval(cvalv);
	b = chasetype(t);
	switch(b->tkind){
	case Tvoid:
	case Tbase:
	case Tundef:
	case Tptr:
	case Tstruct:
	case Tunion:
	case Tfun:
	case Tenum:
	case Tbitfield: /* bitfield bonus: C wouldn't let you do this */
		/* construct pointer */
		pt = mkptrxtn(t, d->ns->base[Vptr]->rep);
		imm = truncimm(cv->val, pt->rep);
		rv = mkvalcval(d, pt, imm);
		break;
	case Tarr:
		/* construct pointer to first element */
		pt = mkptrxtn(t->link, d->ns->base[Vptr]->rep);
		imm = cv->val;
		imm = truncimm(imm, pt->rep);
		rv = mkvalcval(d, pt, imm);
		break;
	case Tconst:
		vmerr(vm, "attempt to use enumeration constant as location");
	case Ttypedef:
		fatal("bug");
	case Txaccess:
		vmerr(vm, "attempt to construct pointer to value of "
		      "extended access type");
	}
	putvalrand(vm, rv, dst);
}

static int
dobitfieldgeom(Xtypename *b, BFgeom *bfg)
{
	Cval *bit0, *bs;
	Xtypename *bb;
		
	bit0 = valcval(b->bit0);
	bs = valcval(b->sz);
	bfg->bp = bit0->val;
	bfg->bs = bs->val;
	bb = chasetype(b->link);
	bfg->isbe = isbigendian[bb->rep];
	return bitfieldgeom(bfg);
}

static void
xcval(VM *vm, Operand *x, Operand *type, Operand *cval, Operand *dst)
{
	Val xv, typev, cvalv, rv, p, argv[2];
	Imm imm;
	Dom *d;
	Xtypename *t, *b, *pt;
	Cval *cv;
	Str *s, *es;
	BFgeom bfg;

	xv = getvalrand(vm, x);
	typev = getvalrand(vm, type);
	cvalv = getvalrand(vm, cval);

	t = valxtn(typev);
	cv = valcval(cvalv);
	b = chasetype(t);

	/* special case: enum constants can be referenced through namespace */
	if(b->tkind == Tconst){
		switch(Vkind(xv)){
		case Qdom:
			d = valdom(xv);
			rv = mkvalcval(d, b->link, cv->val);
			goto out;
		case Qns:
			d = mkdom(valns(xv), vm->litdom->as, 0);
			rv = mkvalcval(d, b->link, cv->val);
			goto out;
		default:
			fatal("bug");
		}
	}

	if(Vkind(xv) != Qdom)
		vmerr(vm, "attempt to access address space through non-domain");
	d = valdom(xv);
	switch(b->tkind){
	case Tbitfield:
		if(b->link->tkind == Tundef){
			es = fmtxtn(b->link->link);
			vmerr(vm, "attempt to read object of undefined type: "
			      "%.*s", (int)es->len, es->s);
		}
		if(0 > dobitfieldgeom(b, &bfg))
			vmerr(vm, "invalid bitfield access");
//		xprintf("get %lld %lld\n", cv->val+bfg.addr, bfg.cnt);
		s = callget(vm, d->as, cv->val+bfg.addr, bfg.cnt);
		imm = bitfieldget(s->s, &bfg);
		rv = mkvalcval(d, b->link, imm);
		break;
	case Tbase:
	case Tptr:
		/* FIXME: check type of cv */
		s = callget(vm, d->as, cv->val, typesize(vm, t));
		imm = str2imm(t, s);
		rv = mkvalcval(d, t, imm);
		break;
	case Tarr:
		/* construct pointer to first element */
		pt = mkptrxtn(t->link, d->ns->base[Vptr]->rep);
		imm = truncimm(cv->val, pt->rep);
		rv = mkvalcval(d, pt, imm);
		break;
	case Txaccess:
		argv[0] = xv;
		p = dovm(vm, t->get, 1, argv);
		if(Vkind(p) != Qcval)
			vmerr(vm,
			      "get method of extended access type returned "
			      "non-cval");
		rv = mkvalcval2(typecast(vm, t->link, valcval(p)));
		break;
	case Tvoid:
	case Tfun:
	case Tstruct:
	case Tunion:
		vmerr(vm,
		      "attempt to read %s-valued object from address space",
		      tkindstr[b->tkind]);
	case Tundef:
		es = fmtxtn(b->link);
		vmerr(vm, "attempt to read object of undefined type: %.*s",
		      (int)es->len, es->s);
	case Tenum:
	case Ttypedef:
	case Tconst:
		fatal("bug");
	}
out:
	putvalrand(vm, rv, dst);
}

static int
iscvaltype(Xtypename *t)
{
	static int okay[Tntkind] = {
		[Tbase] = 1,
		[Tptr] = 1,
		[Tbitfield] = 1,
	};

	t = chasetype(t);
	return okay[t->tkind];
}

static int
isptrtype(Xtypename *t)
{
	t = chasetype(t);
	return t->tkind == Tptr;
}

static Xtypename*
nsvoidstar(Ns *ns)
{
	return mkptrxtn(ns->base[Vvoid], ns->base[Vptr]->rep);
}

static Cval*
str2voidstar(Ns *ns, Str *s)
{
	Dom *d;
	d = mkdom(ns, mksas(s), 0);
	return mkcval(d, nsvoidstar(ns), 0);
}

static void
xxcast(VM *vm, Operand *typeordom, Operand *o, Operand *dst)
{
	Val tv, ov, rv;
	Cval *cv;
	Dom *d;
	Xtypename *t;

	ov = getvalrand(vm, o);
	if(Vkind(ov) != Qcval && Vkind(ov) != Qstr)
		vmerr(vm, "operand 2 to extended cast operator must be a"
		      " cvalue or string");
	tv = getvalrand(vm, typeordom);
	if(Vkind(tv) != Qxtn && Vkind(ov) == Qstr)
		vmerr(vm, "illegal conversion");
	if(Vkind(tv) == Qxtn){
		t = valxtn(tv);
		if(!iscvaltype(t))
			vmerr(vm, "illegal type conversion");
		if(Vkind(ov) == Qstr){
			if(!isptrtype(t))
				vmerr(vm, "illegal type conversion");
			cv = str2voidstar(vm->litns, valstr(ov));
		}else
			cv = valcval(ov);
		rv = mkvalcval2(typecast(vm, t, cv));
	}else if(Vkind(tv) == Qdom){
		d = valdom(tv);
		rv = mkvalcval2(domcast(vm, d, valcval(ov)));
	}else if(Vkind(tv) == Qns){
		cv = valcval(ov);
		d = mkdom(valns(tv), cv->dom->as, 0);
		rv = mkvalcval2(domcast(vm, d, valcval(ov)));
	}else if(Vkind(tv) == Qas){
		cv = valcval(ov);
		d = mkdom(cv->dom->ns, valas(tv), 0);
		rv = mkvalcval2(domcast(vm, d, valcval(ov)));
	}else
		vmerr(vm, "bad type for operand 1 to extended cast operator");
	putvalrand(vm, rv, dst);
}

static void
xlist(VM *vm, Operand *op1, Operand *op2, Operand *dst)
{
	Val v;
	Imm sp, n, m, i;
	List *lst;
	Val rv;

	v = getvalrand(vm, op1);
	sp = vm->fp+valimm(v);
	n = stkimm(vm->stack[sp]);
	v = getvalrand(vm, op2);
	m = valimm(v);

	lst = mklist();
	for(i = m; i < n; i++)
		listappend(vm, lst, vm->stack[sp+1+i]);
	rv = mkvallist(lst);
	putvalrand(vm, rv, dst);
}

static void
xsizeof(VM *vm, Operand *op, Operand *dst)
{
	Val v, rv;
	Imm imm;
	Xtypename *xtn;
	Cval *cv;

	v = getvalrand(vm, op);
	if(Vkind(v) == Qcval){
		cv = valcval(v);
		xtn = cv->type;
	}else if(Vkind(v) == Qxtn)
		xtn = valxtn(v);
	else
		vmerr(vm, "operand 1 to sizeof must be a type or cvalue");
	imm = typesize(vm, xtn);
	rv = mkvalcval(vm->litdom, vm->litbase[Vuint], imm);
	putvalrand(vm, rv, dst);
}

static void
nilfn(VM *vm, Imm argc, Val *argv, Val *rv)
{
}

/* dispatch for abstract address spaces */
static void
nasdispatch(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Str *cmd;

	if(argc < 2)
		vmerr(vm,
		      "wrong number of arguments to address space dispatch");
	checkarg(vm, "nasdispatch", argv, 1, Qstr);
	cmd = valstr(argv[1]);
	if(equalstrc(cmd, "map")){
		if(argc != 2)
			vmerr(vm, "wrong number of arguments to map");
		*rv = mkvalvec(mkvec(0));
		return;
	}
	vmerr(vm, "attempt to access abstract address space");
}

static As*
mknas()
{
	As *as;
	Tab *mtab;
	mtab = mktab();
	_tabput(mtab,
		mkvalstr(mkstr0("dispatch")),
		mkvalcl(mkcfn("nasdispatch", nasdispatch)));
	as = mkastab(mtab, mkstr0("nullas"));
	return as;
}

static void
sasget(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Str *s, *dat;
	Range *r;
	Cval *beg, *end;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to get");
	checkarg(vm, "sasget", argv, 1, Qrange);
	s = valstr(disp[0]);
	r = valrange(argv[1]);
	beg = r->beg;
	end = xcvalalu(vm, Iadd, beg, r->len);
	if(beg->val > s->len)	/* FIXME: >=? */
		vmerr(vm, "address space access out of bounds");
	if(end->val > s->len)
		vmerr(vm, "address space access out of bounds");
	if(beg->val > end->val)
		vmerr(vm, "address space access out of bounds");
	dat = strslice(s, beg->val, end->val);
	*rv = mkvalstr(dat);
}

static void
sasput(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Str *s, *dat;
	Range *r;
	Cval *beg, *end;

	if(argc != 3)
		vmerr(vm, "wrong number of arguments to put");
	checkarg(vm, "sasput", argv, 1, Qrange);
	checkarg(vm, "sasput", argv, 2, Qstr);
	s = valstr(disp[0]);
	r = valrange(argv[1]);
	dat = valstr(argv[2]);
	beg = r->beg;
	if(r->len->val == 0 && beg->val <= s->len)
		/* special case: empty string */
		return;
	end = xcvalalu(vm, Iadd, beg, r->len);
	if(beg->val >= s->len)
		vmerr(vm, "address space access out of bounds");
	if(end->val > s->len)
		vmerr(vm, "address space access out of bounds");
	if(beg->val > end->val)
		vmerr(vm, "address space access out of bounds");
	if(dat->len < r->len->val)
		vmerr(vm, "short put");
	/* FIXME: rationalize with l1_strput */
	memcpy(s->s+beg->val, dat->s, dat->len);
}

static void
sasmap(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Val val;
	Vec *v;
	Str *s;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to map");
	s = valstr(disp[0]);
	v = mkvec(1);
	val = mkvalrange(cvalnull,
			 mkcval(vm->litdom,
				vm->litbase[Vptr], s->len));
	_vecset(v, 0, val);
	*rv = mkvalvec(v);
}

As*
mksas(Str *s)
{
	Tab *mtab;
	mtab = mktab();
	_tabput(mtab, mkvalstr(mkstr0("get")),
		mkvalcl(mkccl("sasget", sasget, 1, mkvalstr(s))));
	_tabput(mtab, mkvalstr(mkstr0("put")),
		mkvalcl(mkccl("sasput", sasput, 1, mkvalstr(s))));
	_tabput(mtab, mkvalstr(mkstr0("map")),
		mkvalcl(mkccl("sasmap", sasmap, 1, mkvalstr(s))));
	return mkastab(mtab, 0);
}

static void
masget(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Str *s, *dat;
	Range *r;
	Cval *beg, *end;
	Imm o;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to get");
	checkarg(vm, "masget", argv, 1, Qrange);
	s = valstr(disp[0]);
	r = valrange(argv[1]);
	beg = r->beg;
	end = xcvalalu(vm, Iadd, beg, r->len);
	o = (Imm)s->s;
	if(beg->val < o)
		vmerr(vm, "address space access out of bounds");
	if(beg->val > o+s->len)	/* FIXME: >=? */
		vmerr(vm, "address space access out of bounds");
	if(end->val > o+s->len)
		vmerr(vm, "address space access out of bounds");
	if(beg->val > end->val)
		vmerr(vm, "address space access out of bounds");
	dat = strslice(s, beg->val-o, end->val-o); /* yee-haw! */
	*rv = mkvalstr(dat);
}

static void
masput(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Str *s, *dat;
	Range *r;
	Cval *beg, *end;
	Imm o;

	if(argc != 3)
		vmerr(vm, "wrong number of arguments to put");
	checkarg(vm, "masput", argv, 1, Qrange);
	checkarg(vm, "masput", argv, 2, Qstr);
	s = valstr(disp[0]);
	r = valrange(argv[1]);
	dat = valstr(argv[2]);
	beg = r->beg;
	o = (Imm)s->s;
	if(r->len->val == 0 && beg->val <= o+s->len)
		/* special case: empty string */
		return;
	end = xcvalalu(vm, Iadd, beg, r->len);
	if(beg->val < o)
		vmerr(vm, "address space access out of bounds");
	if(beg->val >= o+s->len)
		vmerr(vm, "address space access out of bounds");
	if(end->val > o+s->len)
		vmerr(vm, "address space access out of bounds");
	if(beg->val > end->val)
		vmerr(vm, "address space access out of bounds");
	if(dat->len < r->len->val)
		vmerr(vm, "short put");
	/* FIXME: rationalize with l1_strput */
	memcpy((char*)beg->val, dat->s, dat->len);
}

static void
masmap(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Val val;
	Vec *v;
	Str *s;
	Imm o;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to map");
	s = valstr(disp[0]);
	v = mkvec(1);
	o = (Imm)s->s;
	val = mkvalrange(mkcval(vm->litdom, vm->litbase[Vptr], o),
			 mkcval(vm->litdom, vm->litbase[Vptr], o+s->len));
	_vecset(v, 0, val);
	*rv = mkvalvec(v);
}

As*
mkmas(Str *s)
{
	Tab *mtab;
	mtab = mktab();
	_tabput(mtab, mkvalstr(mkstr0("get")),
		mkvalcl(mkccl("masget", masget, 1, mkvalstr(s))));
	_tabput(mtab, mkvalstr(mkstr0("put")),
		mkvalcl(mkccl("masput", masput, 1, mkvalstr(s))));
	_tabput(mtab, mkvalstr(mkstr0("map")),
		mkvalcl(mkccl("masmap", masmap, 1, mkvalstr(s))));
	return mkastab(mtab, 0);
}

As*
mkzas(VM *vm, Imm len)
{
	return mksas(mkstrn(vm, len));
}

Val
mkattr(Val o)
{
	Tab *tab;

	if(Vkind(o) != Qtab && Vkind(o) != Qcval)
		fatal("bug");
	if(Vkind(o) == Qtab)
		tab = tabcopy(valtab(o));
	else{
		tab = mktab();
		_tabput(tab, mkvalstr(mkstr0("offset")), o);
	}
	return mkvaltab(tab);
}

Val
attroff(Val o)
{
	Tab *tab;
	Val vp;

	if(Vkind(o) != Qtab)
		fatal("bug");
	tab = valtab(o);
	vp = tabget(tab, mkvalstr(mkstr0("offset")));
	if(vp)
		return vp;
	return Xnil;
}

static Val
copyattr(VM *vm, Val attr, Val newoff)
{
	Tab *t;
	Val off;
	Val key;
#if 0
	if(Vkind(attr) == Qcval)
		if(newoff)
			return mkvalcval2(newoff);
		else
			return attr;
#endif
	if(Vkind(attr) != Qtab)
		fatal("bug");
	t = tabcopy(valtab(attr));
	key = mkvalstr(mkstr0("offset"));
	if(newoff)
		off = newoff;
	else
		off = tabget(t, key);
	tabput(vm, t, key, off);
	return mkvaltab(t);
}

Xtypename*
chasetype(Xtypename *xtn)
{
	if(xtn->tkind == Ttypedef || xtn->tkind == Tenum)
		return chasetype(xtn->link);
	return xtn;
}

/* call looktype operator of NS on typename XTN.
   NB: on failure, we throw away valuable information: which part
   of the type was undefined.  consider adding a new return
   parameter that, if result is 0, points to the undefined type name */
static Xtypename*
_dolooktype(VM *vm, Xtypename *xtn, Ns *ns)
{
	Val argv[2], rv;
	Xtypename *tmp, *new;
	Vec *vec;
	Imm i;

	switch(xtn->tkind){
	case Tvoid:
		return mkvoidxtn();
	case Tbase:
		return ns->base[xtn->basename];
	case Ttypedef:
	case Tstruct:
	case Tunion:
	case Tenum:
		argv[0] = mkvalns(ns);
		argv[1] = mkvalxtn(xtn);
		if(ns->looktype == 0)
			return 0;
		rv = dovm(vm, ns->looktype, 2, argv);
		if(Vkind(rv) == Qnil)
			return 0;
		return valxtn(rv);
	case Tptr:
		new = gcprotect(vm, mkxtn());
		new->tkind = Tptr;
		new->link = _dolooktype(vm, xtn->link, ns);
		gcunprotect(vm, new);
		if(new->link == 0)
			return 0;
		tmp = ns->base[Vptr];
		new->rep = tmp->rep;
		return new;
	case Tarr:
		new = gcprotect(vm, mkxtn());
		new->tkind = Tarr;
		new->link = _dolooktype(vm, xtn->link, ns);
		gcunprotect(vm, new);
		if(new->link == 0)
			return 0;
		new->cnt = xtn->cnt;
		return new;
	case Tfun:
		new = gcprotect(vm, mkxtn());
		new->tkind = Tfun;
		new->link = _dolooktype(vm, xtn->link, ns);
		if(new->link == 0)
			return 0;
		new->param = mkvec(xtn->param->len);
		for(i = 0; i < xtn->param->len; i++){
			vec = veccopy(valvec(vecref(xtn->param, i)));
			vecset(vm, new->param, i, mkvalvec(vec));
			tmp = _dolooktype(vm, valxtn(vecref(vec, Typepos)), ns);
			if(tmp == 0)
				return 0;
			vecset(vm, vec, Typepos, mkvalxtn(tmp)); 
		}
		gcunprotect(vm, new);
		return new;
	case Tundef:
		/* FIXME: do we want this? */
		return _dolooktype(vm, xtn->link, ns);
	case Tconst:
		vmerr(vm, "looktype is undefined on enumeration constants");
	case Tbitfield:
		new = gcprotect(vm, mkxtn());
		new->tkind = Tbitfield;
		new->sz = xtn->sz;
		new->bit0 = xtn->bit0;
		new->link = _dolooktype(vm, xtn->link, ns);
		gcunprotect(vm, new);
		if(new->link == 0)
			return 0;
		return new;
	case Txaccess:
		vmerr(vm, "looktype is undefined on extend access types");
	}
	fatal("bug");
}

/* NS must have initialized base cache */
static Xtypename*
dolooktype(VM *vm, Xtypename *xtn, Ns *ns)
{
	switch(xtn->tkind){
	case Tvoid:
		return mkvoidxtn();
	case Tbase:
		return ns->base[xtn->basename];
	default:
		return _dolooktype(vm, xtn, ns);
	}
}

static void
nscache1base(VM *vm, Ns *ns, Cbase cb)
{
	Val rv, argv[2];
	Xtypename *xtn;
	argv[0] = mkvalns(ns);
	xtn = mkxtn();
	xtn->tkind = Tbase;
	xtn->basename = cb;
	argv[1] = mkvalxtn(xtn);
	rv = dovm(vm, ns->looktype, 2, argv);
	if(Vkind(rv) == Qnil)
		vmerr(vm, "name space does not define %s", cbasename[cb]);
	ns->base[cb] = valxtn(rv);
}

static void
nscachebase(VM *vm, Ns *ns)
{
	Cbase cb;
	for(cb = Vchar; cb < Vnbase; cb++)
		nscache1base(vm, ns, cb);
	nscache1base(vm, ns, Vptr);
}

/* enumsym for namespaces constructed by @names */
static void
stdenumsym(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to enumsym");
	*rv = disp[0];
}

/* enumtype for namespaces constructed by @names */
static void
stdenumtype(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to enumtype");
	*rv = disp[0];
}

/* looksym for namespaces constructed by @names */
static void
stdlooksym(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Tab *sym;
	Val vp;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to looksym");
	checkarg(vm, "looksym", argv, 1, Qstr);
	sym = valtab(disp[0]);
	vp = tabget(sym, argv[1]);
	if(vp)
		*rv = vp;
}

/* looktype for namespaces constructed by @names */
static void
stdlooktype(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Tab *type;
	Val vp;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to looktype");
	checkarg(vm, "looktype", argv, 1, Qxtn);
	type = valtab(disp[0]);
	vp = tabget(type, argv[1]);
	if(vp)
		*rv = vp;
}

/* lookaddr for namespaces constructed by @names */
static void
stdlookaddr(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	List *l;
	Cval *cv, *a;
	Imm addr, m, i, b, n;
	Vec *sym;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to lookaddr");
	checkarg(vm, "lookaddr", argv, 1, Qcval);
	l = vallist(disp[0]);
	cv = valcval(argv[1]);
	addr = cv->val;
	listlen(disp[0], &n);
	if(n == 0){
		*rv = Xnil;
		return;
	}
	sym = valvec(listref(vm, l, 0));
	a = valcval(attroff(vecref(sym, Attrpos)));
	if(a->val > addr){
		*rv = Xnil;
		return;
	}

	/* binary search */
	b = 0;
	while(1){
		i = n/2;
		m = b+i;
		if(i == 0)
			break;
		sym = valvec(listref(vm, l, m));
		a = valcval(attroff(vecref(sym, Attrpos)));
		if(addr < a->val)
			n = i;
		else{
			b = m;
			n = n-i;
		}
	}
	sym = valvec(listref(vm, l, m));
	*rv = mkvalvec(sym);
}

typedef
struct NSctx {
	Ns *ons;		/* name space being extended */
	Tab *otype, *osym;	/* bindings passed to @names */
	Tab *rawtype, *rawsym;	/* @names declarations */
	Tab *type, *sym;	/* resulting bindings */
	Tab *undef;		/* undefined type names */
	Rkind ptrrep;		/* pointer representation */
} NSctx;

static Xtypename* resolvetypename(VM *vm, Xtypename *xtn, NSctx *ctx);

static Xtypename*
resolvetid(VM *vm, Val xtnv, NSctx *ctx)
{
	Val rv;
	Xtypename *xtn, *new, *def;

	/* have we already defined this type in the new namespace? */
	rv = tabget(ctx->type, xtnv);
	if(rv)
		return valxtn(rv);

	/* do we have an unprocessed definition for the type? */
	rv = tabget(ctx->rawtype, xtnv);
	if(rv){
		if(Vkind(rv) != Qxtn)
			vmerr(vm, "invalid raw type table");
		xtn = valxtn(xtnv);
		new = mktypedefxtn(xtn->tid, 0);

		/* bind before recursive resolve call to stop cycles */
		tabput(vm, ctx->type, xtnv, mkvalxtn(new));

		def = valxtn(rv);
		if(def->tkind != Ttypedef)
			vmerr(vm, "invalid typedef in raw type table");
		if(!equalstr(def->tid, xtn->tid))
			vmerr(vm, "invalid typedef in raw type table");
		new->link = resolvetypename(vm, def->link, ctx);
		if(new->link == new)
			vmerr(vm, "circular definition in name space: "
			      "typedef %.*s",
			      (int)def->tid->len, def->tid->s);
		return new;
	}

	/* does the ns from which we inherit have a definition? */
	rv = tabget(ctx->otype, xtnv);
	if(rv){
		tabput(vm, ctx->type, xtnv, rv);
		return valxtn(rv);
	}

	tabput(vm, ctx->undef, xtnv, xtnv);
	xtn = valxtn(xtnv);
	return mkundefxtn(xtn);
}

/* determine a common type for all constants defined in an enumeration;
   cast the constants to this type in place.  return the type. */
static Xtypename*
doenconsts(VM *vm, Vec *v, Ns *ns)
{
	Dom *d;
	Vec *e;
	Imm m;
	Cval *a, *cv;
	Xtypename *t;
	Val val;

	/* abstract domain for name space being extended */
	d = mkdom(ns, mknas(), 0);

	/* determine type that contains all constant values */
	a = mkcval(d, d->ns->base[Vint], 0);
	for(m = 0; m < v->len; m++){
		e = valvec(vecref(v, m)); /* FIXME check sanity */
		a = xcvalalu1dom(vm, Iadd, a,
				 domcastbase(vm, d, valcval(vecref(e, 1))));
	}
	t = a->type;

	/* cast all values to new type */
	for(m = 0; m < v->len; m++){
		e = valvec(vecref(v, m)); /* FIXME check sanity */
		cv = typecast(vm, t, domcastbase(vm, d, valcval(vecref(e, 1))));
		val = mkvalcval2(cv);
		vecset(vm, e, 1, val);
	}

	return t;
}

static Xtypename*
resolvetag(VM *vm, Val xtnv, NSctx *ctx)
{
	Val rv, v, vp, sz;
	Xtypename *xtn, *new, *tmp;
	Vec *vec, *fld, *fv;
	Imm i;
	Str *es;

	/* have we already defined this type in the new namespace? */
	rv = tabget(ctx->type, xtnv);
	if(rv)
		return valxtn(rv);

	/* do we have an unprocessed definition for the type? */
	rv = tabget(ctx->rawtype, xtnv);
	if(rv){
		if(Vkind(rv) != Qxtn)
			vmerr(vm, "invalid raw type table");
		xtn = valxtn(rv);
		switch(xtn->tkind){
		case Tstruct:
		case Tunion:
			fld = xtn->field;
			sz = xtn->sz;
			if(fld == 0 || Vkind(sz) == Qnil)
				goto error;
			new = mkxtn();
			new->tkind = xtn->tkind;
			new->tag = xtn->tag;
			new->field = mkvec(fld->len);
			new->sz = sz;

			/* bind before recursive resolve call to stop cycles */
			tabput(vm, ctx->type, xtnv, mkvalxtn(new));

			for(i = 0; i < fld->len; i++){
				vec = valvec(vecref(fld, i));
				vp = vecref(vec, Typepos);
				tmp = valxtn(vp);
				tmp = resolvetypename(vm, tmp, ctx);
				v = mkvalxtn(tmp);
				fv = mkvec(3);
				_vecset(fv, Typepos, v);
				_vecset(fv, Idpos, vecref(vec, Idpos));
				_vecset(fv, Attrpos, vecref(vec, Attrpos));
				v = mkvalvec(fv);
				_vecset(new->field, i, v);
			}
			return new;
		case Tenum:
			if(xtn->konst == 0)
				goto error;
			tmp = doenconsts(vm, valvec(xtn->konst), ctx->ons);
			new = mkxtn();
			new->tkind = Tenum;
			new->tag = xtn->tag;
			new->konst = xtn->konst;

			tabput(vm, ctx->type, xtnv, mkvalxtn(new));
			new->link = resolvetypename(vm, tmp, ctx);
			return new;
		default:
		error:
			es = fmtxtn(xtn);
			vmerr(vm, "bad definition for tagged type: %.*s",
			      (int)es->len, es->s);
		}

	}

	/* does the ns from which we inherit have a definition? */
	rv = tabget(ctx->otype, xtnv);
	if(rv){
		tabput(vm, ctx->type, xtnv, rv);
		return valxtn(rv);
	}

	tabput(vm, ctx->undef, xtnv, xtnv);
	xtn = valxtn(xtnv);
	return mkundefxtn(xtn);
}

static Xtypename*
resolvebase(VM *vm, Val xtnv, NSctx *ctx)
{
	Val rv;
	Xtypename *xtn;

	/* have we already defined this type in the new namespace? */
	rv = tabget(ctx->type, xtnv);
	if(rv)
		return valxtn(rv);

	/* does the ns from which we inherit have a definition? */
	rv = tabget(ctx->otype, xtnv);
	if(rv){
		tabput(vm, ctx->type, xtnv, rv);
		return valxtn(rv);
	}
	
	xtn = valxtn(xtnv);
	vmerr(vm, "undefined base type %s", cbasename[xtn->basename]);
}

static Xtypename*
resolvetypename(VM *vm, Xtypename *xtn, NSctx *ctx)
{
	Vec *vec, *pv;
	Val v;
	Xtypename *tmp;
	Imm i;
	Xtypename *new;

	switch(xtn->tkind){
	case Tvoid:
		return mkvoidxtn();
	case Tbase:
		if(xtn->rep != Rundef){
			new = mkxtn();
			new->tkind = Tbase;
			new->basename = xtn->basename;
			xtn = new;
		}
		return resolvebase(vm, mkvalxtn(xtn), ctx);
	case Tptr:
		new = mkxtn();
		new->tkind = xtn->tkind;
		new->rep = ctx->ptrrep;
		new->link = resolvetypename(vm, xtn->link, ctx);
		return new;
	case Ttypedef:
		new = resolvetid(vm, mkvalxtn(xtn), ctx);
		return new;
	case Tenum:
	case Tstruct:
	case Tunion:
		new = resolvetag(vm, mkvalxtn(xtn), ctx);
		return new;
	case Tbitfield:
		new = mkxtn();
		new->tkind = xtn->tkind;
		new->bit0 = xtn->bit0;
		new->sz = xtn->sz;
		new->link = resolvetypename(vm, xtn->link, ctx);
		return new;
	case Tconst:
		new = mkxtn();
		new->tkind = xtn->tkind;
		new->link = resolvetypename(vm, xtn->link, ctx);
		return new;
	case Txaccess:
		new = mkxtn();
		new->tkind = xtn->tkind;
		new->link = resolvetypename(vm, xtn->link, ctx);
		new->get = xtn->get;
		new->put = xtn->put;
		return new;
	case Tarr:
		new = mkxtn();
		new->tkind = xtn->tkind;
		new->cnt = xtn->cnt;
		new->link = resolvetypename(vm, xtn->link, ctx);
		return new;
	case Tfun:
		new = mkxtn();
		new->tkind = Tfun;
		new->link = resolvetypename(vm, xtn->link, ctx);
		new->param = mkvec(xtn->param->len);
		for(i = 0; i < xtn->param->len; i++){
			vec = valvec(vecref(xtn->param, i));
			tmp = valxtn(vecref(vec, Typepos));
			tmp = resolvetypename(vm, tmp, ctx);
			pv = mkvec(2);
			v = mkvalxtn(tmp);
			_vecset(pv, Typepos, v);
			_vecset(pv, Idpos, vecref(vec, Idpos));
			v = mkvalvec(pv);
			_vecset(new->param, i, v);
		}
		return new;
	case Tundef:
		new = mkxtn();
		new->tkind = Tundef;
		new->link = xtn->link;
		return new;
	}
	fatal("bug");
}

static void
calldispatch(VM *vm, Imm argc, Val *argv, Val *disp, Val *rv)
{
	Closure *dcl;
	Val *xargv;

	dcl = valcl(disp[0]);
	xargv = emalloc((1+argc)*sizeof(Val));
	xargv[0] = argv[0];	/* this */
	xargv[1] = disp[1];	/* name of method to call */
	memcpy(xargv+2, argv+1, (argc-1)*sizeof(Val));
	if(waserror(vm)){
		efree(xargv);
		nexterror(vm);
	}
	*rv = dovm(vm, dcl, argc+1, xargv);
	efree(xargv);
	poperror(vm);
}

static void
ascache1method(As *as, char *id, Closure **p)
{
	Val v;
	Str *ids;

	ids = mkstr0(id);

	/* is method in method table? */
	v = tabget(as->mtab, mkvalstr(ids));
	if(v){
		*p = valcl(v);
		return;
	}

	/* no dispatch, no dice */
	if(as->dispatch == 0){
		*p = 0;
		return;
	}

	/* bind a new caller to dispatch and hope for the best */
	*p = mkccl(id, calldispatch, 2, mkvalcl(as->dispatch), mkvalstr(ids));
}

static void
ascachemethod(As *as)
{
	/* dispatch must be resolved first */
	ascache1method(as, "dispatch", &as->dispatch);

	ascache1method(as, "get", &as->get);
	ascache1method(as, "put", &as->put);
	ascache1method(as, "map", &as->map);
}

As*
mkastab(Tab *mtab, Str *name)
{
	As *as;
	as = mkas();
	as->mtab = mtab;
	as->name = name;
	ascachemethod(as);
	return as;
}

static void
nscache1method(Ns *ns, char *id, Closure **p)
{
	Val v;
	Str *ids;

	ids = mkstr0(id);

	/* is method in method table? */
	v = tabget(ns->mtab, mkvalstr(ids));
	if(v){
		*p = valcl(v);
		return;
	}

	/* no dispatch, no dice */
	if(ns->dispatch == 0){
		*p = 0;
		return;
	}

	/* bind a new caller to dispatch and hope for the best */
	*p = mkccl(id, calldispatch, 2, mkvalcl(ns->dispatch), mkvalstr(ids));
}

static void
nscachemethod(Ns *ns)
{
	/* dispatch must be resolved first */
	nscache1method(ns, "dispatch", &ns->dispatch);

	nscache1method(ns, "looktype", &ns->looktype);
	nscache1method(ns, "enumtype", &ns->enumtype);
	nscache1method(ns, "looksym", &ns->looksym);
	nscache1method(ns, "enumsym", &ns->enumsym);
	nscache1method(ns, "lookaddr", &ns->lookaddr);
}

Ns*
mknstab(Tab *mtab, Str *name)
{
	Ns *ns;
	ns = mkns();
	ns->mtab = mtab;
	ns->name = name;
	nscachemethod(ns);
	return ns;
}
       
static Ns*
mknsfn(Closure *looktype, Closure *enumtype,
       Closure *looksym, Closure *enumsym,
       Closure *lookaddr, Str *name)
{
	Tab *mtab;
	mtab = mktab();
	_tabput(mtab, mkvalstr(mkstr0("looktype")), mkvalcl(looktype));
	_tabput(mtab, mkvalstr(mkstr0("enumtype")), mkvalcl(enumtype));
	_tabput(mtab, mkvalstr(mkstr0("looksym")), mkvalcl(looksym));
	_tabput(mtab, mkvalstr(mkstr0("enumsym")), mkvalcl(enumsym));
	_tabput(mtab, mkvalstr(mkstr0("lookaddr")), mkvalcl(lookaddr));
	return mknstab(mtab, name);
}

static Ns*
mknstype(Tab *type, Str *name)
{
	return mknsfn(mkccl("looktype", stdlooktype, 1, mkvaltab(type)),
		      mkccl("enumtype", stdenumtype, 1, mkvaltab(type)),
		      mkcfn("looksym", nilfn),
		      mkccl("enumsym", stdenumsym, 1, mkvaltab(mktab())),
		      mkcfn("lookaddr", nilfn),
		      name);
}

static void
symcmp(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *sa, *sb;
	Cval *a, *b;
	sa = valvec(argv[0]);
	sb = valvec(argv[1]);
	a = valcval(attroff(vecref(sa, Attrpos)));
	b = valcval(attroff(vecref(sb, Attrpos)));
	if(a->val < b->val)
		*rv = mkvalcval2(cvalminus1);
	else if(a->val > b->val)
		*rv = mkvalcval2(cval1);
	else
		*rv = mkvalcval2(cval0);
}

static Ns*
mknstypesym(VM *vm, Tab *type, Tab *sym, Str *name)
{
	Val rv, vp, op;
	Vec *vec, *s;
	List *ls;
	Imm m, len;
	Val argv[2];

	gcprotect(vm, type);
	gcprotect(vm, sym);
	gcprotect(vm, name);

	/* create sorted list of symbols with offsets for lookaddr*/
	vec = tabenum(sym);
	len = vec->len/2;
	ls = mklist();
	for(m = 0; m < len; m++){
		vp = vecref(vec, len+m);	/* symbol #m */
		s = valvec(vp);
		op = vecref(s, Attrpos);
		if(Vkind(op) == Qnil)
			continue;
		_listappend(ls, vp);
	}
	argv[0] = mkvallist(ls);
	argv[1] = mkvalcl(mkcfn("symcmp", symcmp));
	gcprotect(vm, argv[0]);
	gcprotect(vm, argv[1]);

	l1_sort(vm, 2, argv, &rv);

	gcunprotect(vm, argv[1]);
	gcunprotect(vm, argv[0]);
	gcunprotect(vm, name);
	gcunprotect(vm, sym);
	gcunprotect(vm, type);

	return mknsfn(mkccl("looktype", stdlooktype, 1, mkvaltab(type)),
		      mkccl("enumtype", stdenumtype, 1, mkvaltab(type)),
		      mkccl("looksym", stdlooksym, 1, mkvaltab(sym)),
		      mkccl("enumsym", stdenumsym, 1, mkvaltab(sym)),
		      mkccl("lookaddr", stdlookaddr, 1, mkvallist(ls)),
		      name);
}

static Ns*
mknsraw(VM *vm, Ns *ons, Tab *rawtype, Tab *rawsym, Str *name)
{
	Val v, idv, vecv, vp;
	Vec *vec, *kvec, *nvec;
	Tabx *x;
	Tabidx *tk;
	Xtypename *xtn, *tmp;
	NSctx ctx;
	u32 i, j;
	Ns *ns;
	Imm m;
	Str *as;
	Val xargv[1];

	ctx.rawtype = rawtype;
	ctx.rawsym = rawsym;

	ctx.type = mktab();
	ctx.sym = mktab();
	ctx.undef = mktab();
	gcprotect(vm, ctx.type);
	gcprotect(vm, ctx.sym);
	gcprotect(vm, ctx.undef);

	ctx.ons = ons;
	xargv[0] = mkvalns(ons);
	if(ons->enumtype == 0)
		vmerr(vm, "parent name space does not define enumtype");
	if(ons->enumsym == 0)
		vmerr(vm, "parent name space does not define enumsym");
	ctx.otype = valtab(dovm(vm, ons->enumtype, 1, xargv));
	gcprotect(vm, ctx.otype);
	ctx.osym = valtab(dovm(vm, ons->enumsym, 1, xargv));
	gcprotect(vm, ctx.osym);

	/* get pointer representation from parent name space */
	xtn = mkxtn();		/* will be garbage */
	xtn->tkind = Tbase;
	xtn->basename = Vptr;
	vp = tabget(ctx.otype, mkvalxtn(xtn));
	if(vp == 0)
		vmerr(vm,
		      "derived name space must define pointer representation");
	xtn = valxtn(vp);
	ctx.ptrrep = xtn->rep;
	xtn = 0;

	x = ctx.rawtype->x;
	for(i = 0; i < x->sz; i++){
		tk = x->idx[i];
		while(tk){
			v = x->key[tk->idx];
			if(Vkind(v) != Qxtn)
				vmerr(vm, "invalid raw type table");
			xtn = valxtn(v);
			resolvetypename(vm, xtn, &ctx);
			tk = tk->link;
		}
	}

	x = ctx.rawsym->x;
	for(i = 0; i < x->sz; i++){
		tk = x->idx[i];
		while(tk){
			/* id -> [ xtn, id, attr ] */
			idv = x->key[tk->idx];
			vecv = x->val[tk->idx];
			if(Vkind(idv) != Qstr)
				vmerr(vm, "invalid raw symbol table");
			if(Vkind(vecv) != Qvec)
				vmerr(vm, "invalid raw symbol table");
			vec = valvec(vecv);
			if(!issym(vec))
				vmerr(vm, "invalid raw symbol table");
			xtn = valxtn(vecref(vec, Typepos));
			xtn = resolvetypename(vm, xtn, &ctx);
			if(xtn != 0){
				v = mkvalxtn(xtn);
				vecset(vm, vec, Typepos, v); /* reuse vec */
				tabput(vm, ctx.sym, idv, vecv);
			}
			tk = tk->link;
		}
	}

	/* inherit sym and type definitions not shadowed by @names */
	x = ctx.otype->x;
	for(i = 0; i < x->sz; i++){
		tk = x->idx[i];
		while(tk){
			vp = x->key[tk->idx];
			if(!tabget(ctx.type, vp))
				tabput(vm, ctx.type, vp, x->val[tk->idx]);
			tk = tk->link;
		}
	}
	x = ctx.osym->x;
	for(i = 0; i < x->sz; i++){
		tk = x->idx[i];
		while(tk){
			vp = x->key[tk->idx];
			if(!tabget(ctx.sym, vp))
				tabput(vm, ctx.sym, vp, x->val[tk->idx]);
			tk = tk->link;
		}
	}

	/* add enumeration constants to symtab */
	x = ctx.type->x;
	for(i = 0; i < x->sz; i++){
		tk = x->idx[i];
		while(tk){
			xtn = valxtn(x->val[tk->idx]);
			if(xtn->tkind != Tenum)
				goto next;
			tmp = mkconstxtn(xtn);
			vec = valvec(xtn->konst);
			for(j = 0; j < vec->len; j++){
				kvec = valvec(vecref(vec, j));
				nvec = mkvec(3);
				_vecset(nvec, Typepos, mkvalxtn(tmp));
				_vecset(nvec, Idpos, vecref(kvec, 0));
				_vecset(nvec, Attrpos, mkattr(vecref(kvec, 1)));
				tabput(vm, ctx.sym,
				       vecref(kvec, 0), mkvalvec(nvec));
			}
		next:
			tk = tk->link;
		}
	}

	vec = tabenum(ctx.undef);
	m = vec->len/2;
	if(m > 0 && cqctflags['w']){
		xprintf("warning: name space references undefined "
			"type%s:\n", m > 1 ? "s" : "");
		while(m != 0){
			m--;
			vp = vecref(vec, m);
			xtn = valxtn(vp);
			as = fmtxtn(xtn);
			xprintf("\t%.*s\n", (int)as->len, as->s);
		}
	}

	ns = mknstypesym(vm, ctx.type, ctx.sym, name);
	gcprotect(vm, ns);
	nscachebase(vm, ns);
	gcunprotect(vm, ns);

	gcunprotect(vm, ctx.osym);
	gcunprotect(vm, ctx.otype);
	gcunprotect(vm, ctx.undef);
	gcunprotect(vm, ctx.sym);
	gcunprotect(vm, ctx.type);

	return ns;
}

static char*
myroot()
{
	u32 x;
	char *p;
	int longsz;
	int ptrsz;
	int flags;
	enum {
		lw = 1<<0,
		lp = 1<<1,
		be = 1<<2,
	};
	static char *root[] = {
		[0] 		= "c32le",
		[lw]		= "c64le",
		[lp|lw]		= "clp64le",
		[be] 		= "c32be",
		[be|lw]		= "c64be",
		[be|lp|lw]	= "clp64be",
	};
	char *r;

	flags = 0;
	longsz = 8*sizeof(long);
	if(longsz == 64)
		flags |= lw;
	ptrsz = 8*sizeof(void*);
	if(ptrsz == 64)
		flags |= lp;
	x = 0x01020304;
	p = (char*)&x;
	if(*p == 0x01)
		flags |= be;

	r = root[flags];
	if(r == 0)
		return "unsupported";
	return r;
}

static void* gotab[Iopmax];

static void
setgo(Insn *i, Imm lim)
{
	Imm k;
	for(k = 0; k < lim; k++){
		i->go = gotab[i->kind];
		i++;
	}
}

static void
vmsetcl(VM *vm, Val val)
{
	Closure *cl;

	if(Vkind(val) != Qcl)
		vmerr(vm, "attempt to apply non-procedure");
	cl = valcl(val);
	vm->clx = cl;
	vm->ibuf = vm->clx->code->insn;
	if(vm->ibuf->go == 0)
		setgo(vm->ibuf, vm->clx->code->ninsn);
}

jmp_buf*
_pusherror(VM *vm)
{
	Err *ep;
	if(vm->edepth >= vm->emax){
		vm->err = erealloc(vm->err, vm->emax*sizeof(Err),
				   2*vm->emax*sizeof(Err));
		vm->emax *= 2;
	}
	ep = &vm->err[vm->edepth++];
	ep->pdepth = vm->pdepth;
	ep->fp = vm->fp;
	ep->sp = vm->sp;
	ep->pc = vm->pc;
	ep->cl = vm->cl;
	return &ep->esc;
}

void
nexterror(VM *vm)
{
	Err *ep;
	if(vm->edepth == 0)
		fatal("bad error stack discipline");
	vm->edepth--;
	ep = &vm->err[vm->edepth];
	while(vm->pdepth > ep->pdepth)
		gcprotpop(vm);
	vm->fp = ep->fp;
	vm->sp = ep->sp;
	vm->pc = ep->pc;
	vm->cl = ep->cl;
	vmsetcl(vm, vm->cl);
	longjmp(ep->esc, 1);
}

void
poperror(VM *vm)
{
	if(vm->edepth == 0)
		fatal("bad error stack discipline");
	vm->edepth--;
}

static void
gcprotpush(VM *vm)
{
	if(vm->pdepth >= vm->pmax){
		vm->prot = erealloc(vm->prot,
				    vm->pmax*sizeof(Root*),
				    2*vm->pmax*sizeof(Root*));
		vm->pmax *= 2;
	}
	vm->pdepth++;
}

static void
gcprotpop(VM *vm)
{
	freerootlist(&vm->rs, vm->prot[vm->pdepth-1]);
	vm->prot[vm->pdepth-1] = 0;
	vm->pdepth--;
}

static void
_gcunprotect(VM *vm, Val v, unsigned depth)
{
	Root *r, **pr;
	
	pr = &vm->prot[depth];
	r = *pr;
	while(r){
		if(r->hd == v){
			*pr = r->link;
			freeroot(&vm->rs, r);
			break;
		}
		pr = &r->link;
		r = *pr;
	}
}

static void*
_gcprotect(VM *vm, void *obj, unsigned depth)
{
	Root *r;

	if(obj == 0)
		return 0;
	r = newroot(&vm->rs);
	r->hd = obj;
	r->link = vm->prot[depth];
	vm->prot[depth] = r;	
	return obj;
}

void
gcunprotect(VM *vm, void *v)
{
	_gcunprotect(vm, v, vm->pdepth-1);
}

void*
gcprotect(VM *vm, void *v)
{
  	return _gcprotect(vm, v, vm->pdepth-1);
}

void
gcunpersist(VM *vm, void *v)
{
	_gcunprotect(vm, v, 0);
}

void*
gcpersist(VM *vm, void *v)
{
	return _gcprotect(vm, v, 0);
}

void
cqctgcprotect(VM *vm, Val v)
{
	gcprotect(vm, v);
}

void
cqctgcunprotect(VM *vm, Val v)
{
	gcunprotect(vm, v);
}

void
cqctgcpersist(VM *vm, Val v)
{
	gcpersist(vm, v);
}

void
cqctgcunpersist(VM *vm, Val v)
{
	gcunpersist(vm, v);
}

void
builtinnil(Env *env, char *name)
{
	envbind(env, name, Xnil);
}

void
builtinfd(Env *env, char *name, Fd *fd)
{
	Val val;
	val = mkvalfd(fd);
	envbind(env, name, val);
}

void
builtinfn(Env *env, char *name, Closure *cl)
{
	Val val;
	val = mkvalcl(cl);
	envbind(env, name, val);
	if(name[0] == '%')
		envbind(env, name+1, val);		
}

static void
builtinns(Env *env, char *name, Ns *ns)
{
	Val val;
	val = mkvalns(ns);
	envbind(env, name, val);
}

static void
builtindom(Env *env, char *name, Dom *dom)
{
	Val val;
	val = mkvaldom(dom);
	envbind(env, name, val);
}

static void
builtincval(Env *env, char *name, Cval *cv)
{
	Val val;
	val = mkvalcval2(cv);
	envbind(env, name, val);
}

static void
builtinval(Env *env, char *name, Val val)
{
	envbind(env, name, val);
}

static void
vmresetctl(VM *vm)
{
	while(vm->pdepth > 1)	/* don't pop persistent level */
		gcprotpop(vm);
	vm->edepth = 0;
	vm->fp = 0;
	vm->sp = Maxstk;
	vm->ac = Xnil;
	vm->cl = mkvalcl(panicthunk());
}

static void
vmresettop(VM *vm)
{
	Val val;
	
	if(!envlookup(vm->top->env, "halt", &val))
		fatal("bad vm environment");
	vm->halt = valcl(val);
	if(!envlookup(vm->top->env, "litdom", &val))
		fatal("bad vm environment");
	vm->litdom = valdom(val);
	vm->litns = vm->litdom->ns;
	vm->litbase = vm->litns->base;
}

Fd*
vmstdout(VM *vm)
{
	Val v;
	if(!envlookup(vm->top->env, "stdout", &v))
		vmerr(vm, "stdout is undefined");
	if(Vkind(v) != Qfd)
		vmerr(vm, "stdout not bound to a file descriptor");
	return valfd(v);
}

Val
dovm(VM *vm, Closure *cl, Imm argc, Val *argv)
{
	static int once;
	Insn *i;
	Cval *cv;
	Val val;
	Imm m, narg, onarg;
	u64 s0, s1;

	if(!once){
		once = 1;
		gotab[Iadd]	= &&Iadd;
		gotab[Iand]	= &&Iand;
		gotab[Iargc]	= &&Iargc;
		gotab[Ibox]	= &&Ibox;
		gotab[Ibox0]	= &&Ibox0;
		gotab[Icall]	= &&Icall;
		gotab[Icallc]	= &&Icallc;
		gotab[Icallt]	= &&Icallt;
		gotab[Iclo]	= &&Iclo;
		gotab[Icmpeq] 	= &&Icmpeq;
		gotab[Icmpgt] 	= &&Icmpgt;
		gotab[Icmpge] 	= &&Icmpge;
		gotab[Icmplt] 	= &&Icmplt;
		gotab[Icmple] 	= &&Icmple;
		gotab[Icmpneq] 	= &&Icmpneq;
		gotab[Icval] 	= &&Icval;
		gotab[Idiv] 	= &&Idiv;
		gotab[Iframe] 	= &&Iframe;
		gotab[Ihalt] 	= &&Ihalt;
		gotab[Iinv] 	= &&Iinv;
		gotab[Ijmp] 	= &&Ijmp;
		gotab[Ijnz] 	= &&Ijnz;
		gotab[Ijz] 	= &&Ijz;
		gotab[Ikg] 	= &&Ikg;
		gotab[Ikp] 	= &&Ikp;
		gotab[Ilist]	= &&Ilist;
		gotab[Imod] 	= &&Imod;
		gotab[Imov] 	= &&Imov;
		gotab[Imul] 	= &&Imul;
		gotab[Ineg] 	= &&Ineg;
		gotab[Inot] 	= &&Inot;
		gotab[Ior] 	= &&Ior;
		gotab[Inop] 	= &&Inop;
		gotab[Ipanic] 	= &&Ipanic;
		gotab[Ipush] 	= &&Ipush;
		gotab[Ipushi] 	= &&Ipushi;
		gotab[Iref] 	= &&Iref;
		gotab[Iret] 	= &&Iret;
		gotab[Ishl] 	= &&Ishl;
		gotab[Ishr] 	= &&Ishr;
		gotab[Isizeof]	= &&Isizeof;
		gotab[Ispec] 	= &&Ispec;
		gotab[Isub] 	= &&Isub;
		gotab[Isubsp] 	= &&Isubsp;
		gotab[Ivargc]	= &&Ivargc;
		gotab[Ixcast] 	= &&Ixcast;
		gotab[Ixor] 	= &&Ixor;
	}
	
	gcprotpush(vm);

	/* for recursive entry, store current context */
	vmpushi(vm, vm->fp);	/* fp */
	vmpush(vm, vm->cl);	/* cl */
	vmpushi(vm, vm->pc);	/* pc */
	vmpushi(vm, 0);		/* narg */
	vm->fp = vm->sp;

	/* push frame for halt thunk */
	vmpushi(vm, vm->fp);		/* fp */
	vmpush(vm, mkvalcl(vm->halt));	/* cl */
	vmpushi(vm, vm->halt->entry);	/* pc */
	for(m = argc; m > 0; m--)
		vmpush(vm, argv[m-1]);
	vmpushi(vm, argc);		/* narg */
	vm->fp = vm->sp;

	/* switch to cl */
	vm->cl = mkvalcl(cl);
	vmsetcl(vm, vm->cl);
	vm->pc = vm->clx->entry;

	s0 = 0;
	s1 = rdtsc();
	goto enter;
	while(1){
		s1 = rdtsc();
// FIXME: rollback happens on multiprocessors
//		if(s1 < s0)
//			printf("rollback\n");
		i->cnt += s1-s0;
	enter:
		s0 = s1;
		i = &vm->ibuf[vm->pc++];
//		i->cnt++;
		tick++;
		goto *(i->go);
		fatal("bug");
	Inop:
		continue;
	Ispec:
		xspec(vm, &i->op1);
		continue;
	Icallc:
		xcallc(vm);
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
	Icmplt:
	Icmple:
	Icmpgt:
	Icmpge:
	Icmpeq:
	Icmpneq:
		xbinop(vm, i->kind, &i->op1, &i->op2, &i->dst);
		continue;
	Isubsp:
		val = getvalrand(vm, &i->op1);
		vm->sp -= valimm(val);
		continue;
	Imov:
		xmov(vm, &i->op1, &i->dst);
		continue;
	Ipush:
		xpush(vm, &i->op1);
		continue;
	Ipushi:
		xpushi(vm, &i->op1);
		continue;
	Iargc:
		val = getvalrand(vm, &i->op1);
		cv = valcval(val);
		if(stkimm(vm->stack[vm->fp]) != cv->val)
			vmerr(vm, "wrong number of arguments to %s",
			      vm->clx->id);
		continue;
	Ivargc:
		val = getvalrand(vm, &i->op1);
		cv = valcval(val);
		if(stkimm(vm->stack[vm->fp]) < cv->val)
			vmerr(vm, "insufficient number of arguments to %s",
			      vm->clx->id);
		continue;
	Icall:
		vm->cl = getvalrand(vm, &i->op1);
		vmsetcl(vm, vm->cl);
		vm->pc = vm->clx->entry;
		vm->fp = vm->sp;
		continue;
	Icallt:
		vm->cl = getvalrand(vm, &i->op1);
		vmsetcl(vm, vm->cl);
		/* shift current arguments over previous arguments */
		narg = stkimm(vm->stack[vm->sp]);
		onarg = stkimm(vm->stack[vm->fp]);
		vm->fp = vm->fp+onarg-narg;
		memmove(&vm->stack[vm->fp], &vm->stack[vm->sp],
			(narg+1)*sizeof(Val));
		vm->sp = vm->fp;
		vm->pc = vm->clx->entry;
		continue;
	Iframe:
		vmpushi(vm, vm->fp);
		vmpush(vm, vm->cl);
		vmpushi(vm, i->dstlabel->insn);
		continue;
	Ipanic:
		fatal("vm panic");
	Ihalt:
		/* Ihalt is exactly like Iret... */
		vm->sp = vm->fp+stkimm(vm->stack[vm->fp])+1; /* narg+1 */
		vm->fp = stkimm(vm->stack[vm->sp+2]);
		vm->cl = vm->stack[vm->sp+1];
		vmsetcl(vm, vm->cl);
		vm->pc = stkimm(vm->stack[vm->sp]);
		vmpop(vm, 3);

		/* ...except that it returns from dovm */
		gcprotpop(vm);
		return vm->ac;
	Iret:
		vm->sp = vm->fp+stkimm(vm->stack[vm->fp])+1; /* narg+1 */
		vm->fp = stkimm(vm->stack[vm->sp+2]);
		vm->cl = vm->stack[vm->sp+1];
		vmsetcl(vm, vm->cl);
		vm->pc = stkimm(vm->stack[vm->sp]);
		vmpop(vm, 3);
		if(vm->flags&VMirq)
			vmerr(vm, "interrupted");
		thegc->gcpoll(thegc, vm);
		continue;
	Ijmp:
		vm->pc = i->dstlabel->insn;
		if(vm->flags&VMirq)
			vmerr(vm, "interrupted");
		thegc->gcpoll(thegc, vm);
		continue;
	Ijnz:
		xjnz(vm, &i->op1, i->dstlabel);
		if(vm->flags&VMirq)
			vmerr(vm, "interrupted");
		thegc->gcpoll(thegc, vm);
		continue;
	Ijz:
		xjz(vm, &i->op1, i->dstlabel);
		if(vm->flags&VMirq)
			vmerr(vm, "interrupted");
		thegc->gcpoll(thegc, vm);
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
	Icval:
		xcval(vm, &i->op1, &i->op2, &i->op3, &i->dst);
		continue;
	Iref:
		xref(vm, &i->op1, &i->op2, &i->op3, &i->dst);
		continue;
	Ixcast:
		xxcast(vm, &i->op1, &i->op2, &i->dst);
		continue;
	Ilist:
		xlist(vm, &i->op1, &i->op2, &i->dst);
		continue;
	Isizeof:
		xsizeof(vm, &i->op1, &i->dst);
		continue;
	}
}

void
checkarg(VM *vm, char *fn, Val *argv, unsigned arg, Qkind qkind)
{
	if(Vkind(argv[arg]) != qkind)
		vmerr(vm, "operand %d to %s must be a %s",
		      arg+1, fn, qname[qkind]);
}

int
isbasecval(Cval *cv)
{
	Xtypename *t;
	t = chasetype(cv->type);
	if(t->tkind != Tbase)
		return 0;
	return 1;
}

int
isnegcval(Cval *cv)
{
	Xtypename *t;
	t = chasetype(cv->type);
	if(t->tkind != Tbase)
		return 0;
	if(isunsigned[t->basename])
		return 0;
	return (s64)cv->val < 0;
}

int
isnatcval(Cval *cv)
{
	Xtypename *t;
	t = chasetype(cv->type);
	if(t->tkind != Tbase)
		return 0;
	if(isunsigned[t->basename])
		return 1;
	return (s64)cv->val >= 0;
}

int
iszerocval(Cval *cv)
{
	return cv->val == 0;
}

static int
docmp(VM *vm, Val a, Val b, Closure *cmp)
{
	Val argv[2], rv;
	Cval *cv;

	argv[0] = a;
	argv[1] = b;
	rv = dovm(vm, cmp, 2, argv);
	if(Vkind(rv) != Qcval)
		vmerr(vm, "comparison function must return an integer cvalue");
	cv = valcval(rv);
	if(!isbasecval(cv))
		vmerr(vm, "comparison function must return an integer cvalue");
	if(isnegcval(cv))
		return -1;
	if(iszerocval(cv))
		return 0;
	return 1;
}

static void
doswap(VM *vm, Val *vs, Imm i, Imm j)
{
	Val t;
	gcwb(thegc, vs[i]);
	gcwb(thegc, vs[j]);
	t = vs[i];
	vs[i] = vs[j];
	vs[j] = t;
}

static void
dosort(VM *vm, Val *vs, Imm n, Closure *cmp)
{
	Val pv;
	Imm lo, hi, p;
	if(n < 2)
		return;

	lo = 0;
	hi = n;
	p = n>>1;		/* weak pivot */
	pv = vs[p];
	doswap(vm, vs, p, lo);
	while(1){
		do
			lo++;
		while(lo < n && docmp(vm, vs[lo], pv, cmp) < 0);
		do
			hi--;
		while(0 < hi && docmp(vm, vs[hi], pv, cmp) > 0);
		if(hi < lo)
			break;
		doswap(vm, vs, lo, hi);
	}
	doswap(vm, vs, 0, hi);
	dosort(vm, vs, hi, cmp);
	dosort(vm, vs+hi+1, n-hi-1, cmp);
}

static void
l1_sort(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *l;
	Listx *x;
	Vec *v;
	Val *vs;
	Closure *cmp;
	Imm n;
	
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to sort");
	if(Vkind(argv[0]) != Qlist && Vkind(argv[0]) != Qvec)
		vmerr(vm, "operand 1 to sort must a list or vector");
	checkarg(vm, "sort", argv, 1, Qcl);
	cmp = valcl(argv[1]);
	switch(Vkind(argv[0])){
	case Qlist:
		l = vallist(argv[0]);
		x = l->x;
		n = x->tl-x->hd;
		vs = &x->val[x->hd];
		break;
	case Qvec:
		v = valvec(argv[0]);
		n = v->len;
		vs = v->vec;
		break;
	default:
		return;
	}
	*rv = argv[0];
	if(n < 2)
		return;
	dosort(vm, vs, n, cmp);
}

static Val
dobsearch(VM *vm, Val key, Val *vs, Imm n, Closure *cmp)
{
	Imm m, i, b;
	int rv;

	b = 0;
	while(n > 1){
		i = n/2;
		m = b+i;
		rv = docmp(vm, key, vs[m], cmp);
		if(rv == 0)
			return vs[m];
		if(rv < 0)
			n = i;
		else{
			b = m;
			n = n-i;
		}
	}
	if(n && docmp(vm, key, vs[b], cmp) == 0)
		return vs[b];
	return Xnil;
}

static void
l1_bsearch(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *l;
	Listx *x;
	Vec *v;
	Val *vs;
	Closure *cmp;
	Imm n;
	
	if(argc != 3)
		vmerr(vm, "wrong number of arguments to bsearch");
	if(Vkind(argv[1]) != Qlist && Vkind(argv[1]) != Qvec)
		vmerr(vm, "operand 2 to bsearch must a list or vector");
	checkarg(vm, "bsearch", argv, 2, Qcl);
	cmp = valcl(argv[2]);
	switch(Vkind(argv[1])){
	case Qlist:
		l = vallist(argv[1]);
		x = l->x;
		n = x->tl-x->hd;
		vs = &x->val[x->hd];
		break;
	case Qvec:
		v = valvec(argv[1]);
		n = v->len;
		vs = v->vec;
		break;
	default:
		return;
	}
	*rv = dobsearch(vm, argv[0], vs, n, cmp);
}

static void
l1_looktype(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Dom *dom;
	Ns *ns;
	Xtypename *xtn;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to looktype");

	if(Vkind(argv[0]) == Qns)
		ns = valns(argv[0]);
	else if(Vkind(argv[0]) == Qdom){
		dom = valdom(argv[0]);
		ns = dom->ns;
	}else
		vmerr(vm,
		      "operand 1 to looktype must be a namespace or domain");
		      
	if(Vkind(argv[1]) != Qxtn)
		vmerr(vm, "operand 2 to looktype must be a typename");
	xtn = valxtn(argv[1]);

	xtn = dolooktype(vm, xtn, ns);
	if(xtn)
		*rv = mkvalxtn(xtn);
}

static void
l1_looksym(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Dom *dom;
	Ns *ns;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to looksym");

	if(Vkind(argv[0]) == Qns)
		ns = valns(argv[0]);
	else if(Vkind(argv[0]) == Qdom){
		dom = valdom(argv[0]);
		ns = dom->ns;
	}else
		vmerr(vm,
		      "operand 1 to looksym must be a namespace or domain");
		      
	if(Vkind(argv[1]) != Qstr)
		vmerr(vm, "operand 2 to looksym must be a string");
	*rv = dovm(vm, ns->looksym, argc, argv);
}

static void
l1_domof(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val arg0;
	Cval *cv;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to domof");
	arg0 = argv[0];
	if(Vkind(arg0) != Qcval)
		vmerr(vm,
		      "operand 1 to domof must be a cvalue");
	cv = valcval(arg0);
	*rv = mkvaldom(cv->dom);
}

int
isstrcval(Cval *cv)
{
	Xtypename *t;
	t = chasetype(cv->type);
	if(t->tkind != Tptr)
		return 0;
	t = chasetype(t->link);
	if(t->tkind != Tbase)
		return 0;
	if(t->basename != Vchar && t->basename != Vuchar)
		return 0;
	return 1;
}

static Str*
valstrorcval(VM *vm, char *fn, Val *argv, unsigned arg)
{
	Cval *cv;
	if(Vkind(argv[arg]) == Qstr)
		return valstr(argv[arg]);
	else if(Vkind(argv[arg]) == Qcval){
		cv = valcval(argv[arg]);
		if(isstrcval(cv))
			return stringof(vm, cv);
	}
	vmerr(vm, "operand %d to %s must be a string, char* cvalue, "
	      "or uchar* cvalue",
	      arg+1, fn);
}

static Str*
valstrorcvalornil(VM *vm, char *fn, Val *argv, unsigned arg)
{
	Cval *cv;
	if(Vkind(argv[arg]) == Qstr)
		return valstr(argv[arg]);
	else if(Vkind(argv[arg]) == Qcval){
		cv = valcval(argv[arg]);
		if(isstrcval(cv))
			return stringof(vm, cv);
	}else if(Vkind(argv[arg]) == Qnil)
		return 0;
	vmerr(vm, "operand %d to %s must be a string, char* cvalue, "
	      "uchar* cvalue, or nil",
	      arg+1, fn);
}

void
callput(VM *vm, As *as, Imm off, Imm len, Str *s)
{
	Val argv[3];

	argv[0] = mkvalas(as);
	argv[1] = mkvalrange(mkcval(vm->litdom, vm->litns->base[Vptr], off),
			     mkcval(vm->litdom, vm->litns->base[Vptr], len));
	argv[2] = mkvalstr(s);
	if(s->len < len)
		vmerr(vm, "attempt to put short string into longer range");
	dovm(vm, as->put, 3, argv);
}

Str*
callget(VM *vm, As *as, Imm off, Imm len)
{
	Val rv, argv[2];
	Str *s;

	argv[0] = mkvalas(as);
	argv[1] = mkvalrange(mkcval(vm->litdom, vm->litns->base[Vptr], off),
			     mkcval(vm->litdom, vm->litns->base[Vptr], len));
	rv = dovm(vm, as->get, 2, argv);
	if(Vkind(rv) != Qstr)
		vmerr(vm, "address space get method returned non-string");
	s = valstr(rv);
	if(s->len != len)
		vmerr(vm, "address space get method returned wrong number of "
		      "bytes");
	return s;
}

Str*
getbytes(VM *vm, Cval *addr, Imm n)
{
	return callget(vm, addr->dom->as, addr->val, n);
}

Vec*
callmap(VM *vm, As *as)
{
	Val argv[1], rv;
	argv[0] = mkvalas(as);
	rv = dovm(vm, as->map, 1, argv);
	if(Vkind(rv) != Qvec)
		vmerr(vm, "address space map returned invalid value");
	return valvec(rv);
}

Range*
mapstab(VM *vm, Vec *map, Imm addr, Imm len)
{
	Imm m;
	Val rp;
	Range *r;

	for(m = 0; m < map->len; m++){
		rp = vecref(map, m);
		if(Vkind(rp) != Qrange)
			vmerr(vm, "address space map returned invalid value");
		r = valrange(rp);
		if(r->beg->val > addr)
			return 0;
		if(r->beg->val+r->len->val >= addr+len)
			return r;
	}
	return 0;
}

/* assume CV has been vetted by isstrcval */
Str*
stringof(VM *vm, Cval *cv)
{
	Str *s;
	char *buf, *q;
	Vec *v;
	Range *r;
	Imm l, m, n, o;
	static unsigned unit = 128;
	
	/* effectively a call to unit */
	v = callmap(vm, cv->dom->as);
	r = mapstab(vm, v, cv->val, 0);	/* FIXME: type sanity */
	if(r == 0)
		vmerr(vm, "out-of-bounds address space access");

	l = 0;
	m = r->beg->val+r->len->val-cv->val;
	o = cv->val;
	buf = 0;
	while(m > 0){
		n = MIN(m, unit);
		if(buf == 0)
			buf = emalloc(unit);
		else
			buf = erealloc(buf, l, l+n);
		s = callget(vm, cv->dom->as, o, n);
		memcpy(buf+l, s->s, s->len);
		q = strnchr(buf+l, '\0', s->len);
		if(q){
			l += q-(buf+l);
			break;
		}
		l += s->len;
		o += s->len;
		m -= s->len;
	}
	s = mkstr(buf, l);	/* FIXME: mkstr copies buf; should steal */
	efree(buf);
	return s;
}

int
ismapped(VM *vm, As *as, Imm addr, Imm len)
{
	Vec *v;
	Range *r;

	if(len == 0)
		return 0;
	if(addr+len < len)
		/* bogus length */
		return 0;	
	v = callmap(vm, as);
	r = mapstab(vm, v, addr, len);	/* FIXME: type sanity */
	return r != 0;
}

static void
l1_backtrace(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to backtrace");
	vmbacktrace(vm);
}

static void
l1_tabkeys(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val arg0;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to tabkeys");
	arg0 = argv[0];
	if(Vkind(arg0) != Qtab)
		vmerr(vm, "operand 1 to tabkeys must be a table");
	*rv = mkvalvec(tabenumkeys(valtab(arg0)));
}

static void
l1_tabvals(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val arg0;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to tabvals");
	arg0 = argv[0];
	if(Vkind(arg0) != Qtab)
		vmerr(vm, "operand 1 to tabvals must be a table");
	*rv = mkvalvec(tabenumvals(valtab(arg0)));
}

static void
l1_myrootns(VM *vm, Imm argc, Val *argv, Val *rv)
{
	char *r;
	r = myroot();
	if(!envlookup(vm->top->env, r, rv))
		vmerr(vm, "my root name space is undefined: %s", r);
}

static void
dotypepredicate(VM *vm, Imm argc, Val *argv, Val *rv, char *id, unsigned kind)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to %s", id);
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to %s must be a ctype", id);
	xtn = valxtn(argv[0]);
	if(xtn->tkind == kind)
		*rv = mkvalcval2(cval1);
	else
		*rv = mkvalcval2(cval0);
}

static void
l1_isvoid(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "isvoid", Tvoid);
}

static void
l1_isxaccess(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "isxaccess", Txaccess);
}

static void
l1_isundeftype(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "isundeftype", Tundef);
}

static void
l1_isbase(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "isbase", Tbase);
}

static void
l1_isstruct(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "isstruct", Tstruct);
}

static void
l1_isunion(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "isunion", Tunion);
}

static void
l1_issu(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to issu");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to issu must be a ctype");
	xtn = valxtn(argv[0]);
	if(xtn->tkind == Tstruct || xtn->tkind == Tunion)
		*rv = mkvalcval2(cval1);
	else
		*rv = mkvalcval2(cval0);
}

static void
l1_isenum(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "isenum", Tenum);
}

static void
l1_isenumconst(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "isenumconst", Tconst);
}

static void
l1_isbitfield(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "isbitfield", Tbitfield);
}

static void
l1_isptr(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "isptr", Tptr);
}

static void
l1_isarray(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "isarray", Tarr);
}

static void
l1_isfunc(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "isfunc", Tfun);
}

static void
l1_istypedef(VM *vm, Imm argc, Val *argv, Val *rv)
{
	dotypepredicate(vm, argc, argv, rv, "istypedef", Ttypedef);
}

static void
l1_baseid(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to baseid");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to baseid must be a base ctype");
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Tbase)
		vmerr(vm, "operand 1 to baseid must be a base ctype");
	*rv = mkvalstr(mkstr0(cbasename[xtn->basename]));
}

static void
l1_subtype(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;
	static char *err = 
		"operand 1 to subtype must be an "
		"array, pointer, enum, or enum constant";

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to subtype");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, err);
		      
	xtn = valxtn(argv[0]);
	if(xtn->tkind == Ttypedef)
		xtn = chasetype(xtn);
	if(xtn->tkind != Tptr && xtn->tkind != Tarr
	   && xtn->tkind != Tenum && xtn->tkind != Tconst
	   && xtn->tkind != Txaccess && xtn->tkind != Tundef)
		vmerr(vm, err);

	if(xtn->link)
		*rv = mkvalxtn(xtn->link);
}

static void
l1_rettype(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to retttype");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to rettype must be a function ctype");
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Tfun)
		vmerr(vm, "operand 1 to rettype must be a function ctype");
	*rv = mkvalxtn(xtn->link);
}

static void
l1_suekind(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to suekind");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to suekind must be a tagged ctype");
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Tstruct && xtn->tkind != Tunion
	   && xtn->tkind != Tenum)
		vmerr(vm, "operand 1 to suekind must be a tagged ctype");
	*rv = mkvalstr(mkstr0(tkindstr[xtn->tkind]));
}

static void
l1_suetag(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to suetag");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to suetag must be a tagged ctype");
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Tstruct && xtn->tkind != Tunion
	   && xtn->tkind != Tenum)
		vmerr(vm, "operand 1 to sutag must be a tagged ctype");
	*rv = mkvalstr(xtn->tag);
}

static void
l1_susize(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to susize");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm,
		      "operand 1 to susize must be a struct or union ctype");
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Tstruct && xtn->tkind != Tunion)
		vmerr(vm,
		      "operand 1 to susize must be a struct or union ctype");
	*rv = xtn->sz;
}

static void
l1_bitfieldwidth(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to bitfieldwidth");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to bitfieldwidth "
		      "must be a bitfield ctype");
		      
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Tbitfield)
		vmerr(vm, "operand 1 to bitfieldwidth "
		      "must be a bitfield ctype");
	*rv = xtn->sz;
}

static void
l1_bitfieldcontainer(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to bitfieldcontainer");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to bitfieldcontainer "
		      "must be a bitfield ctype");
		      
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Tbitfield)
		vmerr(vm, "operand 1 to bitfieldcontainer "
		      "must be a bitfield ctype");
	*rv = mkvalxtn(xtn->link);
}


static void
l1_bitfieldpos(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to bitfieldpos");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to bitfieldpos must be a bitfield ctype");
		      
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Tbitfield)
		vmerr(vm, "operand 1 to bitfieldpos must be a bitfield ctype");
	*rv = xtn->bit0;
}

static void
l1_xaccessget(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to xaccessget");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to xaccessget must be an "
		      "extended access bitfield ctype");
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Txaccess)
		vmerr(vm, "operand 1 to xaccessget must be an "
		      "extended access bitfield ctype");
	*rv = mkvalcl(xtn->get);
}

static void
l1_xaccessput(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to xaccessput");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to xaccessput must be an "
		      "extended access bitfield ctype");
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Txaccess)
		vmerr(vm, "operand 1 to xaccessput must be an "
		      "extended access bitfield ctype");
	*rv = mkvalcl(xtn->put);
}

static void
l1_arraynelm(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to arraynelm");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to arraynelm must be an array ctype");
		      
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Tarr)
		vmerr(vm, "operand 1 to arraynelm must be an array ctype");
	*rv = xtn->cnt;
}

static void
l1_typedefid(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to typedefid");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to typedefid must be a typedef ctype");
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Ttypedef)
		vmerr(vm, "operand 1 to typedefid must be a typedef ctype");
	*rv = mkvalstr(xtn->tid);
}

static void
l1_typedeftype(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to typedeftype");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm,
		      "operand 1 to typedeftype must be a typedef ctype");
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Ttypedef)
		vmerr(vm,
		      "operand 1 to typedeftype must be a typedef ctype");
	if(xtn->link)
		*rv = mkvalxtn(xtn->link);
}

static void
l1_params(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to params");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to params must be a function ctype");
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Tfun)
		vmerr(vm, "operand 1 to params must be a function ctype");
	*rv = mkvalvec(xtn->param);
}

static void
l1_paramtype(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	Val vp;
	static char *err
		= "operand 1 to paramtype must be a vector returned by params";

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to paramtype");
	if(Vkind(argv[0]) != Qvec)
		vmerr(vm, err);
	v = valvec(argv[0]);
	if(v->len < 2)
		vmerr(vm, err);
	vp = v->vec[Typepos];
	if(Vkind(vp) != Qxtn)
		vmerr(vm, err);
	*rv = vp;
}

static void
l1_paramid(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	Val vp;
	static char *err
		= "operand 1 to paramid must be a vector returned by params";

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to paramid");
	if(Vkind(argv[0]) != Qvec)
		vmerr(vm, err);
	v = valvec(argv[0]);
	if(v->len < 2)
		vmerr(vm, err);
	vp = v->vec[Idpos];
	if(Vkind(vp) != Qstr && Vkind(vp) != Qnil)
		vmerr(vm, err);
	*rv = vp;
}

static void
l1_fields(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to fields");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm,
		      "operand 1 to fields must be a struct or union ctype");
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Tstruct && xtn->tkind != Tunion)
		vmerr(vm,
		      "operand 1 to fields must be a struct or union ctype");
	if(xtn->field == 0)
		return;		/* nil */
	*rv = mkvalvec(xtn->field);
}

static Val
rlookfield(VM *vm, Xtypename *su, Val tag)
{
	Xtypename *t;
	Val vp, rp, id, o;
	Vec *f, *r;
	Imm i;

	for(i = 0; i < su->field->len; i++){
		vp = vecref(su->field, i);
		f = valvec(vp);
		id = vecref(f, Idpos);
		if(Vkind(id) == Qstr){
			if(equalstrv(tag, id))
				return vp;
			else
				continue;
		}
		/* assume id is nil */
		t = chasetype(valxtn(vecref(f, Typepos)));
		if(t->tkind != Tstruct && t->tkind != Tunion)
			continue;
		/* recursively search embedded anonymous field */
		rp = rlookfield(vm, t, tag);
		if(rp == 0)
			continue;
		r = valvec(rp);
		gcprotect(vm, r);
		o = mkvalcval2(xcvalalu(vm, Iadd,
					valcval(attroff(vecref(f, Attrpos))),
					valcval(attroff(vecref(r, Attrpos)))));
		gcunprotect(vm, r);
		r = veccopy(r);
		_vecset(r, Attrpos, copyattr(vm, vecref(r, Attrpos), o));
		return mkvalvec(r);
	}
	return 0;
}

static void
l1_lookfield(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;
	Val vp;
	static char *err1
		= "operand 1 to lookfield must be a struct or union ctype";
	static char *err2
		= "operand 2 to lookfield must be a string";

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to lookfield");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, err1);
	xtn = valxtn(argv[0]);
	xtn = chasetype(xtn);
	if(xtn->tkind != Tstruct && xtn->tkind != Tunion)
		vmerr(vm, err1);
	if(Vkind(argv[1]) != Qstr)
		vmerr(vm, err2);
	vp = rlookfield(vm, xtn, argv[1]);
	if(vp)
		*rv = vp;
}

static void
l1_fieldtype(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	Val vp;
	static char *err
		= "operand 1 to fieldtype must be a vector returned by fields";

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to fieldtype");
	if(Vkind(argv[0]) != Qvec)
		vmerr(vm, err);
	v = valvec(argv[0]);
	if(v->len < 3)
		vmerr(vm, err);
	vp = v->vec[Typepos];
	if(Vkind(vp) != Qxtn)
		vmerr(vm, err);
	*rv = vp;
}

static void
l1_fieldid(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	Val vp;
	static char *err
		= "operand 1 to fieldid must be a vector returned by fields";

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to fieldid");
	if(Vkind(argv[0]) != Qvec)
		vmerr(vm, err);
	v = valvec(argv[0]);
	if(v->len < 3)
		vmerr(vm, err);
	vp = v->vec[Idpos];
	if(Vkind(vp) != Qstr && Vkind(vp) != Qnil)
		vmerr(vm, err);
	*rv = vp;
}

static void
l1_fieldattr(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	static char *err
		= "operand 1 to fieldattr must be a vector returned by fields";

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to fieldattr");
	if(Vkind(argv[0]) != Qvec)
		vmerr(vm, err);
	v = valvec(argv[0]);
	if(v->len < 3)
		vmerr(vm, err);
	*rv = vecref(v, Attrpos);
}

static void
l1_fieldoff(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	Val vp;
	static char *err
		= "operand 1 to fieldoff must be a vector returned by fields";

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to fieldoff");
	if(Vkind(argv[0]) != Qvec)
		vmerr(vm, err);
	v = valvec(argv[0]);
	if(v->len < 3)
		vmerr(vm, err);
	vp = vecref(v, Attrpos);
	if(Vkind(vp) == Qnil)
		return;		/* nil */
	vp = attroff(vp);
	*rv  = vp;
}

static void
l1_enumconsts(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;
	static char *err
		= "operand 1 to enumconsts must be a defined enum ctype";
	if(argc != 1)
		vmerr(vm, err);
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, err);
	xtn = valxtn(argv[0]);
	if(xtn->tkind != Tenum)
		vmerr(vm, err);
	if(xtn->konst == 0)
		vmerr(vm, err);
	*rv = mkvalvec(xtn->konst);
}

static void
l1_symtype(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	Val vp;
	static char *err
		= "operand 1 to symtype must be a vector returned by looksym";

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to symtype");
	if(Vkind(argv[0]) != Qvec)
		vmerr(vm, err);
	v = valvec(argv[0]);
	if(v->len < 3)
		vmerr(vm, err);
	vp = v->vec[Typepos];
	if(Vkind(vp) != Qxtn)
		vmerr(vm, err);
	*rv = vp;
}

static void
l1_symid(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	Val vp;
	static char *err
		= "operand 1 to symid must be a vector returned by looksym";

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to symid");
	if(Vkind(argv[0]) != Qvec)
		vmerr(vm, err);
	v = valvec(argv[0]);
	if(v->len < 3)
		vmerr(vm, err);
	vp = v->vec[Idpos];
	if(Vkind(vp) != Qstr && Vkind(vp) != Qnil)
		vmerr(vm, err);
	*rv = vp;
}

static void
l1_symattr(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	static char *err
		= "operand 1 to symattr must be a vector returned by looksym";

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to symattr");
	if(Vkind(argv[0]) != Qvec)
		vmerr(vm, err);
	v = valvec(argv[0]);
	if(v->len < 3)
		vmerr(vm, err);
	*rv = vecref(v, Attrpos);
}

static void
l1_symoff(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	Val vp;
	static char *err
		= "operand 1 to symoff must be a vector returned by looksym";

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to symoff");
	if(Vkind(argv[0]) != Qvec)
		vmerr(vm, err);
	v = valvec(argv[0]);
	if(v->len < 3)
		vmerr(vm, err);
	vp = vecref(v, Attrpos);
	if(Vkind(vp) == Qnil)
		return;		/* nil */
	*rv = attroff(vp);
}

/* the purpose of typeof on types is to strip the
   enum Tconst and bitfield Tbitfield status from
   lvalue expressions passed to typeof.
   for example: if A is a const of enum X in DOM,
   then we ensure that
   	typeof(dom`A) == typeof(x = dom`A) == enum X
   otherwise, typeof(dom`A) would be Tconst(enum X)

   FIXME:  perhaps this should be generalized
           so that typeof(dom`A) == typeof(x = dom`A).

   e.g.,
	currently, given
		int a[3];
	that
		typeof(a) is int[3]
*/
static void
l1_typeof(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Cval *cv;
	Xtypename *t;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to $typeof");
	if(Vkind(argv[0]) == Qcval){
		cv = valcval(argv[0]);
		t = cv->type;
	}else if(Vkind(argv[0]) == Qxtn){
		t = valxtn(argv[0]);
		if(t->tkind == Tbitfield || t->tkind == Tconst
		   || t->tkind == Txaccess)
			t = t->link;
	}else
		vmerr(vm, "operand 1 to $typeof must be a cvalue or type");
	*rv = mkvalxtn(t);
}

static void
l1_mkctype_void(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_void");
	*rv = mkvalxtn(mkvoidxtn());
}

static void
domkctype_base(Cbase name, Val *rv)
{
	Xtypename *xtn;
	xtn = mkbasextn(name, Rundef);
	*rv = mkvalxtn(xtn);
}

static void
l1_mkctype_char(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_char");
	domkctype_base(Vchar, rv);
}

static void
l1_mkctype_short(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_short");
	domkctype_base(Vshort, rv);
}

static void
l1_mkctype_int(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_int");
	domkctype_base(Vint, rv);
}

static void
l1_mkctype_long(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_long");
	domkctype_base(Vlong, rv);
}

static void
l1_mkctype_vlong(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_vlong");
	domkctype_base(Vvlong, rv);
}

static void
l1_mkctype_uchar(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_uchar");
	domkctype_base(Vuchar, rv);
}

static void
l1_mkctype_ushort(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_ushort");
	domkctype_base(Vushort, rv);
}

static void
l1_mkctype_uint(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_uint");
	domkctype_base(Vuint, rv);
}

static void
l1_mkctype_ulong(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_ulong");
	domkctype_base(Vulong, rv);
}

static void
l1_mkctype_uvlong(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_uvlong");
	domkctype_base(Vuvlong, rv);
}

static void
l1_mkctype_float(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_float");
	domkctype_base(Vfloat, rv);
}

static void
l1_mkctype_double(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_double");
	domkctype_base(Vdouble, rv);
}

static void
l1_mkctype_ldouble(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkctype_ldouble");
	domkctype_base(Vlongdouble, rv);
}

static void
l1_mkctype_ptr(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn, *pxtn;
	if(argc != 1 && argc != 2)
		vmerr(vm, "wrong number of arguments to mkctype_ptr");
	if(Vkind(argv[0]) != Qxtn)
		vmerr(vm, "operand 1 to mkctype_ptr must be a pointer ctype");
	xtn = valxtn(argv[0]);
	if(argc == 1)
		xtn = mkptrxtn(xtn, Rundef);
	else{
		if(Vkind(argv[1]) != Qxtn)
			vmerr(vm, "operand 2 to mkctype_ptr "
			      "must define a pointer type");
		checkarg(vm, "mkctype_ptr", argv, 1, Qxtn);
		pxtn = valxtn(argv[1]);
		pxtn = chasetype(pxtn);
		if((pxtn->tkind != Tptr && pxtn->tkind != Tbase)
		   || pxtn->rep == Rundef)
			vmerr(vm, "operand 2 to mkctype_ptr "
			      "must define a pointer type");
		xtn = mkptrxtn(xtn, pxtn->rep);
	}
	*rv = mkvalxtn(xtn);
}

static void
l1_mkctype_typedef(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn, *sub;
	Str *s;

	switch(argc){
	case 1:
		checkarg(vm, "mkctype_typedef", argv, 0, Qstr);
		s = valstr(argv[0]);
		xtn = mkxtn();
		xtn->tkind = Ttypedef;
		xtn->tid = s;
		break;
	case 2:
		checkarg(vm, "mkctype_typedef", argv, 0, Qstr);
		checkarg(vm, "mkctype_typedef", argv, 1, Qxtn);
		s = valstr(argv[0]);
		sub = valxtn(argv[1]);
		xtn = mkxtn();
		xtn->tkind = Ttypedef;
		xtn->tid = s;
		xtn->link = sub;
		break;
	default:
		vmerr(vm, "wrong number of arguments to mkctype_typedef");
	}
	*rv = mkvalxtn(xtn);
}

static int
issym(Vec *sym)
{
	Val x;

	if(sym->len != 2 && sym->len != 3)
		return 0;
	x = vecref(sym, Typepos);
	if(Vkind(x) != Qxtn)
		return 0;
	x = vecref(sym, Idpos);
	if(Vkind(x) != Qstr && Vkind(x) != Qnil)
		return 0;
	if(sym->len < 3)
		return 1;
	x = vecref(sym, Attrpos);
	if(Vkind(x) != Qtab && Vkind(x) != Qnil)
		return 0;
	return 1;
}

static int
issymvec(Vec *v)
{
	Imm m;
	Val e;
	Vec *sym;
	for(m = 0; m < v->len; m++){
		e = vecref(v, m);
		if(Vkind(e) != Qvec)
			return 0;
		sym = valvec(e);
		if(!issym(sym))
			return 0;
	}
	return 1;
}

static void
domkctype_su(VM *vm, char *fn, Tkind tkind, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;
	Str *s;
	Vec *f;

	switch(argc){
	case 1:
		/* TAG */
		checkarg(vm, fn, argv, 0, Qstr);
		s = valstr(argv[0]);
		xtn = mkxtn();
		xtn->tkind = tkind;
		xtn->tag = s;
		xtn->sz = Xnil;
		break;
	case 3:
		/* TAG FIELDS SIZE */
		checkarg(vm, fn, argv, 0, Qstr);
		checkarg(vm, fn, argv, 1, Qvec);
		checkarg(vm, fn, argv, 2, Qcval);
		s = valstr(argv[0]);
		f = valvec(argv[1]);
		if(!issymvec(f))
			vmerr(vm, "bad field vector", fn);
		xtn = mkxtn();
		xtn->tkind = tkind;
		xtn->tag = s;
		xtn->field = f;
		xtn->sz = argv[2];
		break;
	default:
		vmerr(vm, "wrong number of arguments to %s", fn);
	}
	*rv = mkvalxtn(xtn);
}

static void
l1_mkctype_struct(VM *vm, Imm argc, Val *argv, Val *rv)
{
	domkctype_su(vm, "mkctype_struct", Tstruct, argc, argv, rv);
}

static void
l1_mkctype_union(VM *vm, Imm argc, Val *argv, Val *rv)
{
	domkctype_su(vm, "mkctype_union", Tunion, argc, argv, rv);
}

static void
l1_mkctype_array(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn, *sub;

	switch(argc){
	case 1:
		/* TYPE */
		checkarg(vm, "mkctype_array", argv, 0, Qxtn);
		sub = valxtn(argv[0]);
		xtn = mkxtn();
		xtn->tkind = Tarr;
		xtn->link = sub;
		xtn->cnt = Xnil;
		break;
	case 2:
		/* TYPE CNT */
		checkarg(vm, "mkctype_array", argv, 0, Qxtn);
		checkarg(vm, "mkctype_array", argv, 1, Qcval);
		sub = valxtn(argv[0]);
		xtn = mkxtn();
		xtn->tkind = Tarr;
		xtn->link = sub;
		xtn->cnt = argv[1];
		break;
	default:
		vmerr(vm, "wrong number of arguments to mkctype_array");
	}
	*rv = mkvalxtn(xtn);
}

static void
l1_mkctype_fn(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn, *sub;
	Vec *p;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to mkctype_fn");
	checkarg(vm, "mkctype_fn", argv, 0, Qxtn);
	checkarg(vm, "mkctype_fn", argv, 1, Qvec);
	sub = valxtn(argv[0]);
	p = valvec(argv[1]);
	if(!issymvec(p))
		vmerr(vm, "bad parameter vector");
	xtn = mkxtn();
	xtn->tkind = Tfun;
	xtn->link = sub;
	xtn->param = p;
	*rv = mkvalxtn(xtn);
}

static void
l1_mkctype_bitfield(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn, *sub;
	/* TYPE WIDTH POS */
	if(argc != 3)
		vmerr(vm, "wrong number of arguments to mkctype_bitfield");
	checkarg(vm, "mkctype_bitfield", argv, 0, Qxtn);
	checkarg(vm, "mkctype_bitfield", argv, 1, Qcval);
	checkarg(vm, "mkctype_bitfield", argv, 2, Qcval);
	sub = valxtn(argv[0]);
	xtn = mkxtn();
	xtn->tkind = Tbitfield;
	xtn->link = sub;
	xtn->sz = argv[1];
	xtn->bit0 = argv[2];
	*rv = mkvalxtn(xtn);
}

static void
l1_mkctype_enum(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn, *t;
	Str *s;
	Vec *c;

	switch(argc){
	case 1:
		/* TAG */
		checkarg(vm, "mkctype_enum", argv, 0, Qstr);
		s = valstr(argv[0]);
		xtn = mkxtn();
		xtn->tkind = Tenum;
		xtn->tag = s;
		xtn->konst = 0;
		break;
	case 2:
		/* TAG CONSTS */
		checkarg(vm, "mkctype_enum", argv, 0, Qstr);
		checkarg(vm, "mkctype_enum", argv, 1, Qvec);
		s = valstr(argv[0]);
		c = valvec(argv[1]);
		xtn = mkxtn();
		xtn->tkind = Tenum;
		xtn->tag = s;
		xtn->konst = c;
		break;
	case 3:
		/* TAG CONSTS TYPE */
		checkarg(vm, "mkctype_enum", argv, 0, Qstr);
		checkarg(vm, "mkctype_enum", argv, 1, Qvec);
		checkarg(vm, "mkctype_enum", argv, 2, Qxtn);
		s = valstr(argv[0]);
		c = valvec(argv[1]);
		t = valxtn(argv[2]);
		xtn = mkxtn();
		xtn->tkind = Tenum;
		xtn->tag = s;
		xtn->link = t;
		xtn->konst = c;
		break;
	default:
		vmerr(vm, "wrong number of arguments to mkctype_enum");
	}
	*rv = mkvalxtn(xtn);
}

static void
l1_mkctype_const(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn, *t;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to mkctype_const");
	checkarg(vm, "mkctype_const", argv, 0, Qxtn);
	t = valxtn(argv[0]);
	xtn = mkxtn();
	xtn->tkind = Tconst;
	xtn->link = t;
	*rv = mkvalxtn(xtn);
}

static void
l1_mkctype_xaccess(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Xtypename *xtn;

	if(argc != 3)
		vmerr(vm, "wrong number of arguments to mkctype_xaccess");
	checkarg(vm, "mkctype_xaccess", argv, 0, Qxtn);
	checkarg(vm, "mkctype_xaccess", argv, 1, Qcl);
	checkarg(vm, "mkctype_xaccess", argv, 2, Qcl);
	xtn = mkxtn();
	xtn->tkind = Txaccess;
	xtn->link = valxtn(argv[0]);
	xtn->get = valcl(argv[1]);
	xtn->put = valcl(argv[2]);
	*rv = mkvalxtn(xtn);
}

static void
mksymorfieldorparam(char *what, VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *vec;
	Val attr;

	checkarg(vm, what, argv, 0, Qxtn);
	if(argc > 1)
		if(Vkind(argv[1]) != Qstr && Vkind(argv[1]) != Qnil)
			vmerr(vm, "operand 2 to %s must be a string or nil",
			      what);
	if(argc == 3)
		if(Vkind(argv[2]) != Qcval
		   && Vkind(argv[2]) != Qtab
		   && Vkind(argv[2]) != Qnil)
			vmerr(vm,
			      "operand 3 to %s must be a table, cvalue, or nil",
			      what);
	vec = mkvec(3);
	_vecset(vec, 0, argv[0]);
	if(argc > 1)
		_vecset(vec, 1, argv[1]);
	else
		_vecset(vec, 1, Xnil);
	if(argc > 2 && Vkind(argv[2]) == Qcval)
		attr = mkattr(argv[2]);
	else if(argc > 2)
		attr = argv[2];
	else
		attr = Xnil;
	_vecset(vec, 2, attr);
	*rv = mkvalvec(vec);
}

static void
l1_mksym(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 2 && argc != 3)
		vmerr(vm, "wrong number of arguments to mksym");
	mksymorfieldorparam("mksym", vm, argc, argv, rv);
}

static void
l1_mkfield(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 2 && argc != 3)
		vmerr(vm, "wrong number of arguments to mkfield");
	mksymorfieldorparam("mkfield", vm, argc, argv, rv);	
}

static void
l1_mkparam(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc < 1 || argc > 2)
		vmerr(vm, "wrong number of arguments to mkparam");
	mksymorfieldorparam("mkparam", vm, argc, argv, rv);	
}

static void
l1_isnil(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to isnil");
	if(Vkind(argv[0]) == Qnil)
		*rv = mkvalcval2(cval1);
	else
		*rv = mkvalcval2(cval0);
}

static void
l1_error(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Str *s;
	l1_sprintfa(vm, argc, argv, rv);
	s = valstr(*rv);
	vmerr(vm, "%.*s", (int)s->len, s->s);
}

static void
l1_fault(VM *vm, Imm argc, Val *argv, Val *rv)
{
	vmerr(vm, "memory access fault");
}

static void
l1_strput(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Str *s, *t;
	Cval *off, *cv;
	Imm o;

	if(argc != 3)
		vmerr(vm, "wrong number of arguments to strput");
	if(Vkind(argv[0]) != Qstr)
		vmerr(vm, "operand 1 to strput must be a string");
	if(Vkind(argv[1]) != Qcval)
		vmerr(vm, "operand 2 to strput must be an offset");
	if(Vkind(argv[2]) != Qstr && Vkind(argv[2]) != Qcval)
		vmerr(vm, "operand 3 to strput must be a string or character");
	s = valstr(argv[0]);
	off = valcval(argv[1]);
	o = off->val;		/* FIXME: use type */
	if(o >= s->len)
		vmerr(vm, "strput out of bounds");
	if(Vkind(argv[2]) == Qstr){
		t = valstr(argv[2]);
		if(o+t->len > s->len)
			vmerr(vm, "strput out of bounds");
		memcpy(s->s+o, t->s, t->len);
	}else{
		cv = valcval(argv[2]);
		s->s[o] = (char)cv->val;
	}
}

static void
l1_put(VM *vm, Imm argc, Val *iargv, Val *rv)
{
	Dom *d;
	Xtypename *t, *b;
	Cval *addr, *cv;
	Val argv[3];
	Str *bytes, *es;
	BFgeom bfg;
	Imm imm;

	if(argc != 4)
		vmerr(vm, "wrong number of arguments to put");
	checkarg(vm, "put", iargv, 0, Qdom);
	checkarg(vm, "put", iargv, 1, Qcval);
	checkarg(vm, "put", iargv, 2, Qxtn);
	checkarg(vm, "put", iargv, 3, Qcval);
	d = valdom(iargv[0]);
	addr = valcval(iargv[1]);
	t = valxtn(iargv[2]);
	cv = valcval(iargv[3]);

	b = chasetype(t);
	switch(b->tkind){
	case Tbase:
	case Tptr:
		cv = typecast(vm, t, cv);
		gcprotect(vm, cv);
		bytes = imm2str(vm, t, cv->val);
		callput(vm, d->as, addr->val, typesize(vm, t), bytes);
		gcunprotect(vm, cv);
		*rv = mkvalcval2(cv);
		break;
	case Tbitfield:
		if(b->link->tkind == Tundef){
			es = fmtxtn(b->link->link);
			vmerr(vm, "attempt to write object of undefined type: "
			      "%.*s", (int)es->len, es->s);
		}
		if(0 > dobitfieldgeom(b, &bfg))
			vmerr(vm, "invalid bitfield access");

		/* get contents of bitfield container */
		bytes = callget(vm, d->as, addr->val+bfg.addr, bfg.cnt);

		/* update bitfield container */
		imm = bitfieldput(bytes->s, &bfg, cv->val);

		/* put updated bitfield container */
		callput(vm, d->as, addr->val+bfg.addr, bfg.cnt, bytes);

		/* return value of bitfield (not container) */
		*rv = mkvalcval(d, b->link, imm);
		break;
	case Txaccess:
		if(b->link->tkind == Tundef){
			es = fmtxtn(b->link->link);
			vmerr(vm, "attempt to write object of undefined type: "
			      "%.*s", (int)es->len, es->s);
		}
		cv = typecast(vm, b->link, cv);
		argv[0] = mkvaldom(d);
		argv[1] = mkvalcval2(cv);
		dovm(vm, b->put, 2, argv);
		*rv = mkvalcval2(cv);
		break;
	case Tconst:
		vmerr(vm, "attempt to use enumeration constant as location");
		break;
	case Tundef:
		es = fmtxtn(b->link);
		vmerr(vm,
		      "attempt to write object of undefined type: %.*s",
		      (int)es->len, es->s);
	case Tenum:
	case Ttypedef:
	case Tvoid:
	case Tstruct:
	case Tunion:
	case Tfun:
	case Tarr:
		vmerr(vm,
		      "attempt to write %s-valued object to address space",
		      tkindstr[b->tkind]);
	}
}

static void
l1_foreach(VM *vm, Imm argc, Val *iargv, Val *rv)
{
	List *l;
	Vec *k, *v;
	Tab *t;
	Closure *cl;
	Imm m, i, len, len2;
	Val* argv;
	
	if(argc < 2)
		vmerr(vm, "wrong number of arguments to foreach");
	checkarg(vm, "foreach", iargv, 0, Qcl);

	// tables: only one container operand
	if(Vkind(iargv[1]) == Qtab){
		if(argc > 2) 
			vmerr(vm, "bad combination of containers");
		cl = valcl(iargv[0]);
		t = valtab(iargv[1]);
		k = tabenumkeys(t);
		v = tabenumvals(t);
		argv = emalloc(2*sizeof(Val));
		gcprotect(vm, k);
		gcprotect(vm, v);
		for(m = 0; m < v->len; m++){
			argv[0] = vecref(k, m);
			argv[1] = vecref(v, m);
			dovm(vm, cl, 2, argv);
		}
		gcunprotect(vm, v);
		gcunprotect(vm, k);
		efree(argv);
		return;
	}

	// list and vector: any number, but must have equal length
	len = 0;
	for(m = 1; m < argc; m++){
		switch(Vkind(iargv[m])){
		case Qlist:
			l = vallist(iargv[m]);
			len2 = listxlen(l->x);
			break;
		case Qvec:
			v = valvec(iargv[m]);
			len2 = v->len;
			break;
		default:
			vmerr(vm,
			      "operand %u to foreach must be a list or vector",
			      (unsigned)m);
		}
		if(m == 1)
			len = len2;
		else if(len != len2)
			vmerr(vm,
			      "container operands must be of the same length");
	}
	cl = valcl(iargv[0]);
	argv = emalloc((argc-1)*sizeof(Val));
	for(i = 0; i < len; i++){
		for(m = 1; m < argc; m++){
			switch(Vkind(iargv[m])){
			case Qlist:
				l = vallist(iargv[m]);
				argv[m-1] = listref(vm, l, i);
				break;
			case Qvec:
				v = valvec(iargv[m]);
				argv[m-1] = vecref(v, i);
				break;
			default:
				fatal("bug");
			}
		}
		dovm(vm, cl, argc-1, argv);
	}
	efree(argv);
}

static void
l1_map(VM *vm, Imm argc, Val *iargv, Val *rv)
{
	List *l, *r;
	Vec *k, *v;
	Tab *t;
	Closure *cl;
	Imm m, i, len, len2;
	Val x, *argv;

	if(argc < 2)
		vmerr(vm, "wrong number of arguments to map");
	checkarg(vm, "map", iargv, 0, Qcl);

	// tables: only one container operand
	if(Vkind(iargv[1]) == Qtab){
		if(argc > 2) 
			vmerr(vm, "bad combination of containers");
		r = mklist();
		gcprotect(vm, mkvallist(r));
		cl = valcl(iargv[0]);
		t = valtab(iargv[1]);
		k = tabenumkeys(t);
		v = tabenumvals(t);
		argv = emalloc(2*sizeof(Val));
		gcprotect(vm, k);
		gcprotect(vm, v);
		for(m = 0; m < v->len; m++) {
			argv[0] = vecref(k, m);
			argv[1] = vecref(v, m);
			x = dovm(vm, cl, 2, argv);
			listins(vm, r, m, x);
		}
		gcunprotect(vm, v);
		gcunprotect(vm, k);
		efree(argv);
		*rv = mkvallist(r);
		return;
	}

	// list and vector: any number, but must have equal length
	len = 0;
	for(m = 1; m < argc; m++){
		switch(Vkind(iargv[m])){
		case Qlist:
			l = vallist(iargv[m]);
			len2 = listxlen(l->x);
			break;
		case Qvec:
			v = valvec(iargv[m]);
			len2 = v->len;
			break;
		default:
			vmerr(vm,
			      "operand %u to map must be a list or vector",
			      (unsigned)m);
		}
		if(m == 1)
			len = len2;
		else if(len != len2)
			vmerr(vm,
			      "container operands must be of the same length");
	}

	cl = valcl(iargv[0]);
	r = mklist();
	gcprotect(vm, mkvallist(r));
	argv = emalloc((argc-1)*sizeof(Val));
	for(i = 0; i < len; i++){
		for(m = 1; m < argc; m++){
			switch(Vkind(iargv[m])){
			case Qlist:
				l = vallist(iargv[m]);
				argv[m-1] = listref(vm, l, i);
				break;
			case Qvec:
				v = valvec(iargv[m]);
				argv[m-1] = vecref(v, i);
				break;
			default:
				fatal("bug");
			}
		}
		x = dovm(vm, cl, m-1, argv);
		listins(vm, r, i, x);
	}
	efree(argv);
	*rv = mkvallist(r);
}

static void
l1_close(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Fd *fd;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to close");
	checkarg(vm, "close", argv, 0, Qfd);
	fd = valfd(argv[0]);
	if(fd->flags&Fclosed)
		return;
	fd->flags |= Fclosed;
	if(fd->flags&Ffn){
		if(fd->u.fn.close)
			fd->u.fn.close(&fd->u.fn);
	}else
		if(fd->u.cl.close)
			dovm(vm, fd->u.cl.close, 0, 0);
}

static void
l1_fdname(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Fd *fd;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to fdname");
	checkarg(vm, "fdname", argv, 0, Qfd);
	fd = valfd(argv[0]);
	*rv = mkvalstr(fd->name);
}

static void
l1_mkfd(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Fd *fd;
	Closure *r, *w, *c;
	Str *n;

	if(argc != 3 && argc != 4)
		vmerr(vm, "wrong number of arguments to mkfd");
	if(Vkind(argv[0]) != Qcl && Vkind(argv[0]) != Qnil)
		vmerr(vm, "argument 1 to mkfd must be a function or nil");
	if(Vkind(argv[1]) != Qcl && Vkind(argv[1]) != Qnil)
		vmerr(vm, "argument 2 to mkfd must be a function or nil");
	if(Vkind(argv[2]) != Qcl && Vkind(argv[2]) != Qnil)
		vmerr(vm, "argument 3 to mkfd must be a function or nil");

	r = w = c = 0;
	if(Vkind(argv[0]) == Qcl)
		r = valcl(argv[0]);
	if(Vkind(argv[1]) == Qcl)
		w = valcl(argv[1]);
	if(Vkind(argv[2]) == Qcl)
		c = valcl(argv[2]);
	if(argc == 4){
		checkarg(vm, "mkfd", argv, 3, Qstr);
		n = valstr(argv[3]);
	}else
		n = mkstr0("");
	fd = mkfdcl(n, Fread|Fwrite, r, w, c);
	*rv = mkvalfd(fd);
}

static void
l1_mknas(VM *vm, Imm argc, Val *argv, Val *rv)
{
	As *as;
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mknas");
	as = mknas();
	*rv = mkvalas(as);
}

static void
l1_mksas(VM *vm, Imm argc, Val *argv, Val *rv)
{
	As *as;
	Str *s;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to mksas");
	checkarg(vm, "mksas", argv, 0, Qstr);
	s = valstr(argv[0]);
	as = mksas(s);
	*rv = mkvalas(as);
}

static void
l1_mkzas(VM *vm, Imm argc, Val *argv, Val *rv)
{
	As *as;
	Cval *cv;
	Xtypename *t;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to mkzas");
	checkarg(vm, "mkzas", argv, 0, Qcval);
	cv = valcval(argv[0]);
	t = chasetype(cv->type);
	if(t->tkind != Tbase)
		vmerr(vm, "operand 1 to mkzas must be an integer cvalue");
	as = mkzas(vm, cv->val);
	*rv = mkvalas(as);
}

static void
l1_mkas(VM *vm, Imm argc, Val *argv, Val *rv)
{
	As *as;
	Str *name;
	Tab *mtab;

	if(argc != 1 && argc != 2)
		vmerr(vm, "wrong number of arguments to mkas");
	checkarg(vm, "mkas", argv, 0, Qtab);
	name = 0;
	if(argc == 2){
		checkarg(vm, "mkas", argv, 1, Qstr);
		name = valstr(argv[1]);
	}
	mtab = valtab(argv[0]);
	as = mkastab(mtab, name);
	*rv = mkvalas(as);
}

static void
l1_mknsraw(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Ns *ons, *ns;
	Tab *rawtype, *rawsym;
	Str *name;

	if(argc != 3 && argc != 4)
		vmerr(vm, "wrong number of arguments to mknsraw");
	checkarg(vm, "mkns", argv, 0, Qns);
	checkarg(vm, "mkns", argv, 1, Qtab);
	checkarg(vm, "mkns", argv, 2, Qtab);
	name = 0;
	if(argc == 4){
		checkarg(vm, "mknsraw", argv, 3, Qstr);
		name = valstr(argv[3]);
	}
	ons = valns(argv[0]);
	rawtype = valtab(argv[1]);
	rawsym = valtab(argv[2]);
	ns = mknsraw(vm, ons, rawtype, rawsym, name);
	*rv = mkvalns(ns);
}

static void
l1_mkattr(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to mkattr");
	if(Vkind(argv[0]) != Qcval && Vkind(argv[0]) != Qtab)
		vmerr(vm, "argument 1 to mkattr must be a table or cvalue");
	*rv = mkattr(argv[0]);
}

static void
l1_malloc(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Cval *len;
	Str *s;
	As *as;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to malloc");
	checkarg(vm, "malloc", argv, 0, Qcval);
	len = valcval(argv[0]);
	if(!isnatcval(len))
		vmerr(vm, "malloc expects a non-negative length");
	s = mkstrn(vm, len->val);
	as = mkmas(s);
	*rv = mkvalcval(mkdom(vm->litns, as, mkstr0("malloc")),
			mkptrxtn(vm->litbase[Vchar],
				 vm->litbase[Vptr]->rep),
			(Imm)s->s);
}

static void
l1_attroff(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to attroff");
	checkarg(vm, "attroff", argv, 0, Qtab);
	*rv = attroff(argv[0]);
}

static void
l1_mkns(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Ns *ns;
	Str *name;
	Tab *mtab;

	if(argc != 1 && argc != 2)
		vmerr(vm, "wrong number of arguments to mkns");
	checkarg(vm, "mkns", argv, 0, Qtab);
	name = 0;
	if(argc == 2){
		checkarg(vm, "mkns", argv, 1, Qstr);
		name = valstr(argv[1]);
	}
	mtab = valtab(argv[0]);
	ns = mknstab(mtab, name);
	gcprotect(vm, ns);
	nscachebase(vm, ns);
	gcunprotect(vm, ns);
	*rv = mkvalns(ns);
}

static void
l1_mkdom(VM *vm, Imm argc, Val *argv, Val *rv)
{
	As *as;
	Ns *ns;
	Dom *dom;
	Str *name;

	if(argc != 2 && argc != 3)
		vmerr(vm, "wrong number of arguments to mkdom");
	checkarg(vm, "mkdom", argv, 0, Qns);
	checkarg(vm, "mkdom", argv, 1, Qas);
	name = 0;
	if(argc == 3){
		checkarg(vm, "mkdom", argv, 2, Qstr);
		name = valstr(argv[2]);
	}
	ns = valns(argv[0]);
	as = valas(argv[1]);
	dom = mkdom(ns, as, name);
	*rv = mkvaldom(dom);
}

static void
l1_nameof(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Dom *dom;
	Ns *ns;
	As *as;
	Str *name;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to nameof");
	if(Vkind(argv[0]) == Qdom){
		dom = valdom(argv[0]);
		name = dom->name;
	}else if(Vkind(argv[0]) == Qns){
		ns = valns(argv[0]);
		name = ns->name;
	}else if(Vkind(argv[0]) == Qas){
		as = valas(argv[0]);
		name = as->name;
	}else
		vmerr(vm, "operand 1 to nameof must be a domain, name space"
		      " or address space");
	if(name)
		*rv = mkvalstr(name);
}

static void
l1_asof(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Dom *dom;
	Cval *cv;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to asof");
	if(Vkind(argv[0]) == Qdom)
		dom = valdom(argv[0]);
	else if(Vkind(argv[0]) == Qcval){
		cv = valcval(argv[0]);
		dom = cv->dom;
	}else
		vmerr(vm, "operand 1 to asof must be a domain or cvalue");
	*rv = mkvalas(dom->as);
}

static void
l1_nsof(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Dom *dom;
	Cval *cv;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to nsof");
	if(Vkind(argv[0]) == Qdom)
		dom = valdom(argv[0]);
	else if(Vkind(argv[0]) == Qcval){
		cv = valcval(argv[0]);
		dom = cv->dom;
	}else
		vmerr(vm, "operand 1 to nsof must be a domain or cvalue");
	*rv = mkvalns(dom->ns);
}

static void
l1_callmethod(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val this, id, args, v, *xargv;
	Dom *dom;
	As *as;
	Ns *ns;
	Str *s;
	Closure *cl, *dcl;
	Imm ll, xargc;

	if(argc != 3)
		vmerr(vm, "wrong number of arguments to callmethod");
	checkarg(vm, "callmethod", argv, 1, Qstr);
	checkarg(vm, "callmethod", argv, 2, Qlist);
	this = argv[0];
	id = argv[1];
	args = argv[2];
	dcl = 0;
	if(Vkind(this) == Qas){
		as = valas(this);
		v = tabget(as->mtab, id);
		if(v == 0)
			dcl = as->dispatch;
	}else if(Vkind(this) == Qns){
		ns = valns(this);
		v = tabget(ns->mtab, id);
		if(v == 0)
			dcl = ns->dispatch;
	}else if(Vkind(this) == Qdom){
		dom = valdom(this);
		/* search as, then ns */
		v = tabget(dom->as->mtab, id);
		if(v == 0)
			v = tabget(dom->ns->mtab, id);
		if(v == 0)
			dcl = dom->as->dispatch;
		if(dcl == 0)
			dcl = dom->ns->dispatch;
	}else
		vmerr(vm,
		      "operand 2 to callmethod must be an address space, "
		      "name space, or domain");

	if(v == 0 && dcl == 0){
		s = valstr(id);
		vmerr(vm, "callmethod target must define %.*s or dispatch",
		      (int)s->len, s->s);
	}

	listlen(args, &ll);
	if(v){
		cl = valcl(v);
		xargc = ll+1;
		xargv = emalloc(xargc*sizeof(Val));
		xargv[0] = this;
		listcopyv(vallist(args), 0, ll, xargv+1);
	}else{
		cl = dcl;
		xargc = ll+2;
		xargv = emalloc(xargc*sizeof(Val));
		xargv[0] = this;
		xargv[1] = id;
		listcopyv(vallist(args), 0, ll, xargv+2);
	}
	if(waserror(vm)){
		efree(xargv);
		nexterror(vm);
	}
	*rv = dovm(vm, cl, xargc, xargv);
	poperror(vm);
	efree(xargv);
}

static void
l1_nslookaddr(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val arg0;
	Dom *dom;
	Ns *ns;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to nslookaddr");
	arg0 = argv[0];
	if(Vkind(arg0) != Qns && Vkind(arg0) != Qdom)
		vmerr(vm,
		      "operand 1 to nslookaddr must be a namespace or domain");
	if(Vkind(arg0) == Qns)
		ns = valns(arg0);
	else{
		dom = valdom(arg0);
		ns = dom->ns;
	}
	if(ns->lookaddr)
		*rv = mkvalcl(ns->lookaddr);
}

static void
l1_nsenumsym(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val arg0;
	Dom *dom;
	Ns *ns;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to nsenumsym");
	arg0 = argv[0];
	if(Vkind(arg0) != Qns && Vkind(arg0) != Qdom)
		vmerr(vm,
		      "operand 1 to nsenumsym must be a namespace or domain");
	if(Vkind(arg0) == Qns)
		ns = valns(arg0);
	else{
		dom = valdom(arg0);
		ns = dom->ns;
	}
	if(ns->enumsym)
		*rv = mkvalcl(ns->enumsym);
}

static void
l1_nsenumtype(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val arg0;
	Dom *dom;
	Ns *ns;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to nsenumtype");
	arg0 = argv[0];
	if(Vkind(arg0) != Qns && Vkind(arg0) != Qdom)
		vmerr(vm,
		      "operand 1 to nsenumtype must be a namespace or domain");
	if(Vkind(arg0) == Qns)
		ns = valns(arg0);
	else{
		dom = valdom(arg0);
		ns = dom->ns;
	}
	if(ns->enumtype)
		*rv = mkvalcl(ns->enumtype);
}

static void
l1_nsptr(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val arg0;
	Dom *dom;
	Ns *ns;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to nsptr");
	arg0 = argv[0];
	if(Vkind(arg0) != Qns && Vkind(arg0) != Qdom)
		vmerr(vm,
		      "operand 1 to nsptr must be a namespace or domain");
	if(Vkind(arg0) == Qns)
		ns = valns(arg0);
	else{
		dom = valdom(arg0);
		ns = dom->ns;
	}
	*rv = mkvalxtn(ns->base[Vptr]);
}

static void
l1_mkrange(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to mkrange");
	checkarg(vm, "mkrange", argv, 0, Qcval);
	checkarg(vm, "mkrange", argv, 1, Qcval);
	/* FIXME: check sanity */
	*rv = mkvalrange(valcval(argv[0]), valcval(argv[1]));
}

static void
l1_rangebeg(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Range *r;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to rangebeg");
	checkarg(vm, "rangebeg", argv, 0, Qrange);
	r = valrange(argv[0]);
	*rv = mkvalcval2(r->beg);
}

static void
l1_rangelen(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Range *r;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to rangelen");
	checkarg(vm, "rangelen", argv, 0, Qrange);
	r = valrange(argv[0]);
	*rv = mkvalcval2(r->len);
}

static void
l1_mkstr(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Cval *cv;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to mkstr");
	checkarg(vm, "mkstr", argv, 0, Qcval);
	cv = valcval(argv[0]);
	if(!isnatcval(cv))
		vmerr(vm, "operand 1 to mkstr must be a non-negative integer");
	*rv = mkvalstr(mkstrn(vm, cv->val));
}

static void
l1_strlen(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Str *s;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to strlen");
	s = valstrorcval(vm, "strlen", argv, 0);
	*rv = mkvalcval(vm->litdom, vm->litbase[Vuvlong], s->len);
}

static void
l1_substr(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Cval *b, *e;
	Str *s;
	if(argc != 3)
		vmerr(vm, "wrong number of arguments to substr");
	s = valstrorcval(vm, "substr", argv, 0);
	checkarg(vm, "substr", argv, 1, Qcval);
	checkarg(vm, "substr", argv, 2, Qcval);
	b = valcval(argv[1]);
	e = valcval(argv[2]);
	if(b->val > s->len)
		vmerr(vm, "substring out of bounds");
	if(e->val > s->len)
		vmerr(vm, "substring out of bounds");
	if(b->val > e->val)
		vmerr(vm, "substring out of bounds");
	*rv = mkvalstr(strslice(s, b->val, e->val));
}

static void
l1_strref(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Str *str;
	Cval *cv;
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to strref");
	checkarg(vm, "strref", argv, 1, Qcval);
	cv = valcval(argv[1]);
	if(!isnatcval(cv))
		vmerr(vm, "operand 2 to strref must be "
		      "a non-negative integer");
	str = valstrorcval(vm, "strref", argv, 0);
	if(cv->val >= str->len)
		vmerr(vm, "strref out of bounds");
	*rv = mkvallitcval(Vuchar, str->s[cv->val]);
}

static void
l1_strstr(VM *vm, Imm argc, Val *argv, Val *rv)
{
	char *s1, *s2, *p;
	Str *s;
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to strstr");
	s = valstrorcval(vm, "strstr", argv, 0);
	s1 = str2cstr(s);
	s = valstrorcval(vm, "strstr", argv, 1);
	s2 = str2cstr(s);
	p = strstr(s1, s2);
	if(p)
		*rv = mkvallitcval(Vuvlong, p-s1);
	efree(s1);
	efree(s2);
}

static void
l1_memset(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Str *s;
	unsigned char b;
	Cval *bcv, *lcv;
	Imm lim;

	if(argc != 2 && argc != 3)
		vmerr(vm, "wrong number of arguments to memset");
	s = valstrorcval(vm, "memset", argv, 0);
	checkarg(vm, "memset", argv, 1, Qcval);
	bcv = valcval(argv[1]);
	b = bcv->val&0xff;
	if(argc == 3){
		checkarg(vm, "memset", argv, 2, Qcval);
		lcv = valcval(argv[2]);
		lim = lcv->val;
	}else
		lim = s->len;
	memset(s->s, b, lim);
}

static void
l1_memcpy(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Cval *ncv, *scv, *dcv;
	Str *buf;

	if(argc != 3)
		vmerr(vm, "wrong number of argument to memcpy");

	checkarg(vm, "memcpy", argv, 0, Qcval);
	checkarg(vm, "memcpy", argv, 1, Qcval);
	checkarg(vm, "memcpy", argv, 2, Qcval);
	dcv = valcval(argv[0]);
	scv = valcval(argv[1]);
	ncv = valcval(argv[2]);
	if(!isstrcval(dcv))
		vmerr(vm, "operand 1 to memcpy must be a char* or "
		      "unsigned char*");
	if(!isstrcval(scv))
		vmerr(vm, "operand 2 to memcpy must be a char* or "
		      "unsigned char*");
	buf = callget(vm, scv->dom->as, scv->val, ncv->val);
	callput(vm, dcv->dom->as, dcv->val, ncv->val, buf);
}

static void
l1_stringof(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Str *s;
	Cval *cv;
	static char *err =
		"operand 1 to stringof must be a "
		"char* or unsigned char* cvalue";

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to stringof");
	checkarg(vm, "stringof", argv, 0, Qcval);
	cv = valcval(argv[0]);
	if(!isstrcval(cv))
		vmerr(vm, err);
	s = stringof(vm, cv);
	*rv = mkvalstr(s);
}

static void
l1_strton(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Str *s;
	Liti liti;
	Cval *cv;
	char *err;
	unsigned radix;

	if(argc != 1 && argc != 2)
		vmerr(vm, "wrong number of arguments to strton");
	checkarg(vm, "strton", argv, 0, Qstr);
	radix = 0;
	if(argc == 2){
		checkarg(vm, "strton", argv, 1, Qcval);
		cv = valcval(argv[1]);
		if(!isnatcval(cv))
			vmerr(vm, "operand 2 to strton must be "
			      "non-negative");
		radix = cv->val;
	}
		
	s = valstrorcval(vm, "strton", argv, 0);
	if(0 != parseliti(s->s, s->len, &liti, radix, &err))
		vmerr(vm, err);
	*rv = mkvalcval(vm->litdom, vm->litbase[liti.base], liti.val);
}

static void
l1_split(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Str *s;
	char *p, *q, *e, *set;
	Cval *lim;
	Imm n;
	List *r;
	int intok, mflag;

	r = mklist();
	r = gcprotect(vm, r); /* valstrorcval(ornil) */

	/* subject string */
	if(argc != 1 && argc != 2 && argc != 3)
		vmerr(vm, "wrong number of arguments to split");
	s = valstrorcval(vm, "split", argv, 0);
	p = s->s;
	e = s->s+s->len;

	/* split limit */
	lim = 0;
	if(argc == 3){
		checkarg(vm, "split", argv, 2, Qcval);
		lim = valcval(argv[2]);
		if(!isnatcval(lim))
			vmerr(vm, "split expects a non-negative limit");
		if(lim->val == 0){
			*rv = mkvallist(r);
			return;
		}
	}

	/* delimiter set */
	if(argc > 1 && (s = valstrorcvalornil(vm, "split", argv, 1))){
		set = str2cstr(s);
		mflag = 0;
	}else{
		set = xstrdup(" \t\n");
		mflag = 1;
	}

	if(!mflag){
		n = 0;
		q = p;
		while(q < e && (!lim || n < lim->val)){
			if(strchr(set, *q)){
				listins(vm, r, n++, mkvalstr(mkstr(p, q-p)));
				p = q+1;
				q = p;
			}else
				q++;
		}
		if(p < e)
			listins(vm, r, n, mkvalstr(mkstr(p, e-p)));
	}else{
		n = 0;
		q = p;
		intok = 0;
		while(q < e && (!lim || n < lim->val)){
			if(strchr(set, *q)){
				if(intok)
					listins(vm, r, n++,
						mkvalstr(mkstr(p, q-p)));
				intok = 0;
				p = q+1;
				while(p < e && strchr(set, *p))
					p++;
				if(p >= e)
					break;
				q = p;
			}else{
				q++;
				intok = 1;
			}
		}
		if(intok)
			listins(vm, r, n, mkvalstr(mkstr(p, e-p)));
	}
	efree(set);
	gcunprotect(vm, r);
	*rv = mkvallist(r);
}

static void
l1_mkvec(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Cval *cv;
	Val v;

	if(argc != 1 && argc != 2)
		vmerr(vm, "wrong number of arguments to mkvec");
	checkarg(vm, "mkvec", argv, 0, Qcval);
	cv = valcval(argv[0]);
	if(argc == 2)
		v = argv[1];
	else
		v = Xnil;
	if(!isnatcval(cv))
		vmerr(vm, "operand 1 to mkvec must be a non-negative integer");
	*rv = mkvalvec(mkvecinit(cv->val, v));
}

static void
l1_vector(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	Imm i;

	v = mkvec(argc);
	for(i = 0; i < argc; i++)
		_vecset(v, i, argv[i]);
	*rv = mkvalvec(v);
}

static void
l1_veclen(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to veclen");
	checkarg(vm, "veclen", argv, 0, Qvec);
	v = valvec(argv[0]);
	*rv = mkvalcval(vm->litdom, vm->litbase[Vuvlong], v->len);
}

static void
l1_vecref(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	Cval *cv;
	Val vp;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to vecref");
	checkarg(vm, "vecref", argv, 0, Qvec);
	checkarg(vm, "vecref", argv, 1, Qcval);
	v = valvec(argv[0]);
	cv = valcval(argv[1]);
	if(!isnatcval(cv))
		vmerr(vm, "operand 2 to vecref must be "
		      "a non-negative integer");
	if(cv->val >= v->len)
		vmerr(vm, "vecref out of bounds");
	vp = vecref(v, cv->val);
	*rv = vp;
}

static void
l1_vecset(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Vec *v;
	Cval *cv;

	if(argc != 3)
		vmerr(vm, "wrong number of arguments to vecset");
	checkarg(vm, "vecset", argv, 0, Qvec);
	checkarg(vm, "vecset", argv, 1, Qcval);
	v = valvec(argv[0]);
	cv = valcval(argv[1]);
	if(!isnatcval(cv))
		vmerr(vm, "operand 2 to vecset must be "
		      "a non-negative integer");
	if(cv->val >= v->len)
		vmerr(vm, "vecset out of bounds");
	vecset(vm, v, cv->val, argv[2]);
}

static void
l1_mktab(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mktab");
	*rv = mkvaltab(mktab());
}

static void
l1_tabdelete(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Tab *t;
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to tabdelete");
	checkarg(vm, "tabdelete", argv, 0, Qtab);
	if(Vkind(argv[1]) == Qundef)
		vmerr(vm, "attempt to access table with undefined key value");
	t = valtab(argv[0]);
	tabdel(vm, t, argv[1]);
}

static void
l1_tablook(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Tab *t;
	Val vp;
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to tablook");
	checkarg(vm, "tablook", argv, 0, Qtab);
	if(Vkind(argv[1]) == Qundef)
		vmerr(vm, "attempt to access table with undefined key value");
	t = valtab(argv[0]);
	vp = tabget(t, argv[1]);
	if(vp)
		*rv = vp;
	/* otherwise return nil */
}

static void
l1_tabinsert(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Tab *t;
	if(argc != 3)
		vmerr(vm, "wrong number of arguments to tabinsert");
	checkarg(vm, "tabinsert", argv, 0, Qtab);
	if(Vkind(argv[1]) == Qundef)
		vmerr(vm, "attempt to access table with undefined key value");
	t = valtab(argv[0]);
	tabput(vm, t, argv[1], argv[2]);
}

static void
l1_tabenum(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Tab *t;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to tabenum");
	checkarg(vm, "tabenum", argv, 0, Qtab);
	t = valtab(argv[0]);
	*rv = mkvalvec(tabenum(t));
}

static void
l1_ismapped(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Cval *addr, *len;
	Imm sz;
	if(argc != 1 && argc != 2)
		vmerr(vm, "wrong number of arguments to ismapped");
	checkarg(vm, "ismapped", argv, 0, Qcval);
	addr = valcval(argv[0]);
	sz = 0;
	if(argc == 2){
		checkarg(vm, "ismapped", argv, 1, Qcval);
		len = valcval(argv[1]);
		if(!isnatcval(len))
			vmerr(vm, "ismapped expects a non-negative length");
		sz = len->val;
	}
	if(sz == 0)
		sz = typesize(vm, addr->type);
	if(ismapped(vm, addr->dom->as, addr->val, sz))
		*rv = mkvalcval2(cval1);
	else
		*rv = mkvalcval2(cval0);
}

static void
l1_getbytes(VM *vm, Imm iargc, Val *iargv, Val *rv)
{
	Cval *addr, *len;
	Imm n;
	Xtypename *t;

	if(iargc != 1 && iargc != 2)
		vmerr(vm, "wrong number of arguments to getbytes");
	checkarg(vm, "getbytes", iargv, 0, Qcval);
	addr = valcval(iargv[0]);
	t = chasetype(addr->type);
	if(t->tkind != Tptr)
		vmerr(vm, "operand 1 to getbytes must be a pointer");
	if(iargc == 2){
		checkarg(vm, "getbytes", iargv, 1, Qcval);
		len = valcval(iargv[1]);
		n = len->val;
	}else
		n = typesize(vm, t->link);
	*rv = mkvalstr(getbytes(vm, addr, n));
}

static void
l1_putbytes(VM *vm, Imm iargc, Val *iargv, Val *rv)
{
	Cval *addr;
	Str *str;
	Xtypename *t;

	if(iargc != 2)
		vmerr(vm, "wrong number of arguments to putbytes");
	checkarg(vm, "putbytes", iargv, 0, Qcval);
	checkarg(vm, "putbytes", iargv, 1, Qstr);
	addr = valcval(iargv[0]);
	str = valstr(iargv[1]);
	t = chasetype(addr->type);
	if(t->tkind != Tptr)
		vmerr(vm, "operand 1 to putbytes must be a pointer");
	callput(vm, addr->dom->as, addr->val, str->len, str);
}

static void
l1_apply(VM *vm, Imm iargc, Val *iargv, Val *rv)
{
	Imm ll, argc, m;
	Val *argv, *ap, *ip, vp;
	Closure *cl;
	List *lst;

	if(iargc < 2)
		vmerr(vm, "wrong number of arguments to apply");
	checkarg(vm, "apply", iargv, 0, Qcl);
	cl = valcl(iargv[0]);
	if(listlen(iargv[iargc-1], &ll) == 0)
		vmerr(vm, "final operand to apply must be a proper list");
	argc = iargc-2+ll;
	argv = emalloc(argc*sizeof(Val));
	ap = argv;
	ip = &iargv[1];
	for(m = 0; m < iargc-2; m++)
		*ap++ = *ip++;
	lst = vallist(*ip);
	for(m = 0; m < ll; m++){
		vp = listref(vm, lst, m);
		*ap++ = vp;
	}
	if(waserror(vm)){
		efree(argv);
		nexterror(vm);
	}
	vp = dovm(vm, cl, argc, argv);
	*rv = vp;
	efree(argv);
	poperror(vm);
}

static void
l1_isempty(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *lst;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to isempty");
	if(Vkind(argv[0]) == Qlist){
		lst = vallist(argv[0]);
		if(listxlen(lst->x) != 0)
			*rv = mkvalcval2(cval0);
		else
			*rv = mkvalcval2(cval1);
	}else
		vmerr(vm, "isempty defined only for lists");
}

static void
l1_length(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *lst;
	Vec *vec;
	Str *str;
	Tab *tab;
	Imm len;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to length");
	switch(Vkind(argv[0])){
	default:
		vmerr(vm, "operand 1 to length must be a container");
	case Qlist:
		lst = vallist(argv[0]);
		len = listxlen(lst->x);
		break;
	case Qstr:
		str = valstr(argv[0]);
		len = str->len;
		break;
	case Qvec:
		vec = valvec(argv[0]);
		len = vec->len;
		break;
	case Qtab:
		tab = valtab(argv[0]);
		len = tab->cnt;
		break;
	}
	*rv = mkvalcval(vm->litdom, vm->litbase[Vuvlong], len);
}

static void
l1_count(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val v;
	List *lst;
	Vec *vec;
	Str *str;
	Imm len, i, m;
	Listx *x;
	char c;
	Cval *cv;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to count");
	v = argv[1];
	m = 0;
	switch(Vkind(argv[0])){
	default:
		vmerr(vm, "operand 1 to count must be a list, string, "
		      "or vector");
	case Qlist:
		lst = vallist(argv[0]);
		x = lst->x;
		len = listxlen(x);
		for(i = 0; i < len; i++)
			if(eqval(v, x->val[x->hd+i]))
				m++;
		break;
	case Qstr:
		if(Vkind(v) != Qcval)
			vmerr(vm, "operand 2 to count must a character when"
			      " operand 1 is a string");
		cv = valcval(v);
		c = (char)cv->val;
		str = valstr(argv[0]);
		len = str->len;
		for(i = 0; i < len; i++)
			if(c == str->s[i])
				m++;
		break;
	case Qvec:
		vec = valvec(argv[0]);
		len = vec->len;
		for(i = 0; i < len; i++)
			if(eqval(v, vec->vec[i]))
				m++;
		break;
	}
	*rv = mkvalcval(vm->litdom, vm->litbase[Vuvlong], m);
}

static void
l1_index(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val v;
	List *lst;
	Vec *vec;
	Str *str;
	Imm len, i;
	Listx *x;
	char c;
	Cval *cv;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to index");
	v = argv[1];
	switch(Vkind(argv[0])){
	default:
		vmerr(vm, "operand 1 to index must be a list, string, "
		      "or vector");
	case Qlist:
		lst = vallist(argv[0]);
		x = lst->x;
		len = listxlen(x);
		for(i = 0; i < len; i++)
			if(eqval(v, x->val[x->hd+i]))
				goto gotit;
		break;
	case Qstr:
		if(Vkind(v) != Qcval)
			vmerr(vm, "operand 2 to index must a character when"
			      " operand 1 is a string");
		cv = valcval(v);
		c = (char)cv->val;
		str = valstr(argv[0]);
		len = str->len;
		for(i = 0; i < len; i++)
			if(c == str->s[i])
				goto gotit;
		break;
	case Qvec:
		vec = valvec(argv[0]);
		len = vec->len;
		for(i = 0; i < len; i++)
			if(eqval(v, vec->vec[i]))
				goto gotit;
		break;
	}
	return;   /* nil */
gotit:
	*rv = mkvalcval(vm->litdom, vm->litbase[Vuvlong], i);
}

static void
l1_ismember(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val v;
	List *lst;
	Vec *vec;
	Str *str;
	Imm len, i;
	Listx *x;
	char c;
	Cval *cv;
	Tab *tab;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to ismember");
	*rv = mkvalcval2(cval0);
	v = argv[1];
	switch(Vkind(argv[0])){
	default:
		vmerr(vm, "operand 1 to ismember must be a list, string, "
		      "table, or vector");
	case Qlist:
		lst = vallist(argv[0]);
		x = lst->x;
		len = listxlen(x);
		for(i = 0; i < len; i++)
			if(eqval(v, x->val[x->hd+i]))
				goto gotit;
		break;
	case Qstr:
		if(Vkind(v) != Qcval)
			vmerr(vm, "operand 2 to ismember must a character when"
			      " operand 1 is a string");
		cv = valcval(v);
		c = (char)cv->val;
		str = valstr(argv[0]);
		len = str->len;
		for(i = 0; i < len; i++)
			if(c == str->s[i])
				goto gotit;
		break;
	case Qvec:
		vec = valvec(argv[0]);
		len = vec->len;
		for(i = 0; i < len; i++)
			if(eqval(v, vec->vec[i]))
				goto gotit;
		break;
	case Qtab:
		tab = valtab(argv[0]);
		if(tabget(tab, argv[1]))
			goto gotit;
		break;
	}
	return;
gotit:
	*rv = mkvalcval2(cval1);;
}

static void
l1_delete(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val v;
	List *lst;
	Imm len, i;
	Listx *x;
	Tab *tab;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to delete");
	v = argv[1];
	switch(Vkind(argv[0])){
	default:
		vmerr(vm, "operand 1 to delete must be a list or table");
	case Qlist:
		lst = vallist(argv[0]);
		x = lst->x;
		len = listxlen(x);
		for(i = 0; i < len; i++)
			if(eqval(v, x->val[x->hd+i]))
				listdel(vm, lst, i);
		break;
	case Qtab:
		tab = valtab(argv[0]);
		tabdel(vm, tab, v);
		break;
	}
	*rv = argv[0];
}

static void
l1_null(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to null");
	*rv = Xnulllist;
}

static void
l1_car(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Pair *p;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to car");
	checkarg(vm, "car", argv, 0, Qpair);
	p = valpair(argv[0]);
	*rv = p->car;
}

static void
l1_cdr(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Pair *p;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to cdr");
	checkarg(vm, "cdr", argv, 0, Qpair);
	p = valpair(argv[0]);
	*rv = p->cdr;
}

static void
l1_cons(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to cons");
	*rv = mkvalpair(argv[0], argv[1]);
}

static void
l1_list(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *l;
	Imm i;

	l = mklist();
	for(i = 0; i < argc; i++)
		listappend(vm, l, argv[i]);
	*rv = mkvallist(l);
}

static void
l1_mklist(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Cval *cv;
	Val v;

	if(argc != 1 && argc != 2)
		vmerr(vm, "wrong number of arguments to mklist");
	checkarg(vm, "mklist", argv, 0, Qcval);
	cv = valcval(argv[0]);
	if(argc == 2)
		v = argv[1];
	else
		v = Xnil;
	if(!isnatcval(cv))
		vmerr(vm, "operand 1 to mklist must be "
		      "a non-negative integer");
	*rv = mkvallist(mklistinit(cv->val, v));
}

static void
l1_listref(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val vp;
	List *lst;
	Cval *cv;
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to listref");
	checkarg(vm, "listref", argv, 0, Qlist);
	checkarg(vm, "listref", argv, 1, Qcval);
	cv = valcval(argv[1]);
	if(!isnatcval(cv))
		vmerr(vm, "operand 2 to listref must be "
		      "a non-negative integer");
	lst = vallist(argv[0]);
	vp = listref(vm, lst, cv->val);
	*rv = vp;
}

static void
l1_listdel(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *lst;
	Cval *cv;
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to listdel");
	checkarg(vm, "listdel", argv, 0, Qlist);
	checkarg(vm, "listdel", argv, 1, Qcval);
	cv = valcval(argv[1]);
	if(!isnatcval(cv))
		vmerr(vm, "operand 2 to listdel must be "
		      "a non-negative integer");
	lst = listdel(vm, vallist(argv[0]), cv->val);
	*rv = mkvallist(lst);
}

static void
l1_listset(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *lst;
	Cval *cv;
	if(argc != 3)
		vmerr(vm, "wrong number of arguments to listset");
	checkarg(vm, "listset", argv, 0, Qlist);
	checkarg(vm, "listset", argv, 1, Qcval);
	cv = valcval(argv[1]);
	if(!isnatcval(cv))
		vmerr(vm, "operand 2 to listset must be "
		      "a non-negative integer");
	lst = vallist(argv[0]);
	lst = listset(vm, lst, cv->val, argv[2]);
	*rv = mkvallist(lst);
}

static void
l1_listins(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *lst;
	Cval *cv;
	if(argc != 3)
		vmerr(vm, "wrong number of arguments to listins");
	checkarg(vm, "listins", argv, 0, Qlist);
	checkarg(vm, "listins", argv, 1, Qcval);
	cv = valcval(argv[1]);
	if(!isnatcval(cv))
		vmerr(vm, "operand 2 to listins must be "
		      "a non-negative integer");
	lst = listins(vm, vallist(argv[0]), cv->val, argv[2]);
	*rv = mkvallist(lst);
}

static void
l1_pop(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val arg;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to pop");
	arg = argv[0];
	if(Vkind(arg) == Qlist)
		listpop(vm, vallist(argv[0]), rv);
	else if(Vkind(arg) == Qtab)
		tabpop(vm, valtab(argv[0]), rv);
	else
		vmerr(vm, "operand 1 to pop must be a list or table");
}

static void
l1_head(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Val vp;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to head");
	checkarg(vm, "head", argv, 0, Qlist);
	vp = listhead(vm, vallist(argv[0]));
	*rv = vp;
}

static void
l1_tail(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *lst;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to tail");
	checkarg(vm, "tail", argv, 0, Qlist);
	lst = listtail(vm, vallist(argv[0]));
	*rv = mkvallist(lst);
}

static void
l1_push(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *lst;
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to push");
	checkarg(vm, "push", argv, 0, Qlist);
	lst = listpush(vm, vallist(argv[0]), argv[1]);
	*rv = mkvallist(lst);
}

static void
l1_append(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *lst;
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to append");
	checkarg(vm, "append", argv, 0, Qlist);
	lst = listappend(vm, vallist(argv[0]), argv[1]);
	*rv = mkvallist(lst);
}

static void
l1_reverse(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to reverse");
	if(Vkind(argv[0]) == Qlist)
		*rv = mkvallist(listreverse(vallist(argv[0])));
	else
		vmerr(vm, "operand 1 to reverse must be a list");
}

static void
l1_copy(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to append");
	if(Vkind(argv[0]) == Qlist)
		*rv = mkvallist(listcopy(vallist(argv[0])));
	else if(Vkind(argv[0]) == Qvec)
		*rv = mkvalvec(veccopy(valvec(argv[0])));
	else if(Vkind(argv[0]) == Qtab)
		*rv = mkvaltab(tabcopy(valtab(argv[0])));
	else if(Vkind(argv[0]) == Qstr)
		*rv = mkvalstr(strcopy(valstr(argv[0])));
	else
		vmerr(vm, "operand 1 to copy must be a container");
}

static void
l1_concat(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *lst;
	Str *str;
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to concat");
	if(Vkind(argv[0]) == Qlist && Vkind(argv[1]) == Qlist){
		lst = listconcat(vm, vallist(argv[0]), vallist(argv[1]));
		*rv = mkvallist(lst);
	}else if(Vkind(argv[0]) == Qstr && Vkind(argv[1]) == Qstr){
		str = strconcat(vm, valstr(argv[0]), valstr(argv[1]));
		*rv = mkvalstr(str);
	}else
		vmerr(vm, "operands to concat must be lists or strings");
}

static void
l1_slice(VM *vm, Imm argc, Val *argv, Val *rv)
{
	List *l;
	Cval *b, *e;
	u32 len;

	if(argc != 3)
		vmerr(vm, "wrong number of arguments to slice");
	checkarg(vm, "slice", argv, 0, Qlist);
	checkarg(vm, "slice", argv, 1, Qcval);
	checkarg(vm, "slice", argv, 2, Qcval);
	l = vallist(argv[0]);
	b = valcval(argv[1]);
	e = valcval(argv[2]);
	len = listxlen(l->x);
	if(b->val > len)
		vmerr(vm, "slice out of bounds");
	if(e->val > len)
		vmerr(vm, "slice out of bounds");
	if(b->val > e->val)
		vmerr(vm, "slice out of bounds");
	*rv = mkvallist(listslice(vm, l, b->val, e->val));
}

static void
l1_rdof(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Rec *r;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to rdof");
	checkarg(vm, "rdof", argv, 0, Qrec);
	r = valrec(argv[0]);
	*rv = mkvalrd(r->rd);
}

static void
l1_mkrd(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Closure *fmt;
	List *lst;
	Imm n, nf;
	Val v;

	if(argc != 2 && argc != 3)
		vmerr(vm, "wrong number of arguments to mkrd");
	checkarg(vm, "mkrd", argv, 0, Qstr);
	checkarg(vm, "mkrd", argv, 1, Qlist);
	
	fmt = 0;
	if(argc == 3){
		checkarg(vm, "mkrd", argv, 1, Qcl);
		fmt = valcl(argv[2]);
	}
	listlen(argv[1], &nf);
	lst = vallist(argv[1]);
	for(n = 0; n < nf; n++){
		v = listref(vm, lst, n);
		if(Vkind(v) != Qstr)
			vmerr(vm, "operand 2 to mkrd must be a "
			      "list of field names");
	}
	*rv = mkvalrd(mkrd(vm, valstr(argv[0]), lst, fmt));
}

static void
l1_rdname(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Rd *rd;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to rdname");
	checkarg(vm, "rdname", argv, 0, Qrd);
	rd = valrd(argv[0]);
	*rv = mkvalstr(rd->name);
}

static void
l1_rdis(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Rd *rd;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to rdis");
	checkarg(vm, "rdis", argv, 0, Qrd);
	rd = valrd(argv[0]);
	*rv = mkvalcl(rd->is);
}


static void
l1_rdmk(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Rd *rd;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to rdmk");
	checkarg(vm, "rdmk", argv, 0, Qrd);
	rd = valrd(argv[0]);
	*rv = mkvalcl(rd->mk);
}

static void
l1_rdfmt(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Rd *rd;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to rdfmt");
	checkarg(vm, "rdfmt", argv, 0, Qrd);
	rd = valrd(argv[0]);
	*rv = mkvalcl(rd->fmt);
}

static void
l1_rdsetfmt(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Rd *rd;
	Closure *cl;
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to rdsetfmt");
	checkarg(vm, "rdsetfmt", argv, 0, Qrd);
	checkarg(vm, "rdsetfmt", argv, 1, Qcl);
	rd = valrd(argv[0]);
	cl = valcl(argv[1]);
	rd->fmt = cl;
}

static void
l1_rdfields(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Rd *rd;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to rdfields");
	checkarg(vm, "rdname", argv, 0, Qrd);
	rd = valrd(argv[0]);
	*rv = mkvallist(rd->fname);
}

static void
l1_rdgettab(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Rd *rd;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to rdgettab");
	checkarg(vm, "rdgettab", argv, 0, Qrd);
	rd = valrd(argv[0]);
	*rv = mkvaltab(rd->get);
}

static void
l1_rdsettab(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Rd *rd;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to rdsettab");
	checkarg(vm, "rdsettab", argv, 0, Qrd);
	rd = valrd(argv[0]);
	*rv = mkvaltab(rd->set);
}

static void
l1_cntrget(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to cntrget");
	switch(Vkind(argv[0])){
	default:
		vmerr(vm, "operand 1 to cntrget must be a container");
	case Qlist:
		l1_listref(vm, argc, argv, rv);
		break;
	case Qstr:
		l1_strref(vm, argc, argv, rv);
		break;
	case Qvec:
		l1_vecref(vm, argc, argv, rv);
		break;
	case Qtab:
		l1_tablook(vm, argc, argv, rv);
		break;
	}
}

static void
l1_cntrput(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Cval *cv;
	if(argc != 3)
		vmerr(vm, "wrong number of arguments to cntrput");
	switch(Vkind(argv[0])){
	default:
		vmerr(vm, "operand 1 to cntrput must be a container");
	case Qlist:
		l1_listset(vm, argc, argv, rv);
		*rv = argv[2];
		break;
	case Qstr:
		l1_strput(vm, argc, argv, rv);
		if(Vkind(argv[2]) == Qcval){
			cv = valcval(argv[2]);
			cv = typecast(vm, cv->dom->ns->base[Vchar], cv);
			*rv = mkvalcval2(cv);
		}else
			*rv = argv[2];
		break;
	case Qvec:
		l1_vecset(vm, argc, argv, rv);
		*rv = argv[2];
		break;
	case Qtab:
		l1_tabinsert(vm, argc, argv, rv);
		*rv = argv[2];
		break;
	}
}

static void
l1_equal(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Qkind kind;
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to equal");
	kind = Vkind(argv[0]);
	if(kind != Vkind(argv[1]))
		*rv = mkvalcval2(cval0);
	else if(kind == Qlist){
		if(equallistv(argv[0], argv[1]))
			*rv = mkvalcval2(cval1);
		else
			*rv = mkvalcval2(cval0);
	}else
		vmerr(vm, "equal defined only for lists");
}

static void
l1_toupper(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Cval *cv;
	int x;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to toupper");
	checkarg(vm, "toupper", argv, 0, Qcval);
	cv = valcval(argv[0]);
	x = xtoupper(cv->val);
	*rv = mkvalcval(cv->dom, cv->type, x);
}

static void
l1_tolower(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Cval *cv;
	int x;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to tolower");
	checkarg(vm, "tolower", argv, 0, Qcval);
	cv = valcval(argv[0]);
	x = xtolower(cv->val);
	*rv = mkvalcval(cv->dom, cv->type, x);
}

static void
l1_isxctype(VM *vm, Imm argc, Val *argv, Val *rv, char *name, int (*fn)(int))
{
	Cval *cv;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to %s", name);
	checkarg(vm, name, argv, 0, Qcval);
	cv = valcval(argv[0]);
	if(fn((int)cv->val))
		*rv = mkvalcval2(cval1);
	else
		*rv = mkvalcval2(cval0);
}

static void
l1_isalnum(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "isalnum", xisalnum);
}

static void
l1_isalpha(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "isalpha", xisalpha);
}

static void
l1_isascii(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "isascii", xisascii);
}

static void
l1_isblank(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "isblank", xisblank);
}

static void
l1_iscntrl(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "iscntrl", xiscntrl);
}

static void
l1_isdigit(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "isdigit", xisdigit);
}

static void
l1_isgraph(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "isgraph", xisgraph);
}

static void
l1_islower(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "islower", xislower);
}

static void
l1_isodigit(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "isodigit", xisodigit);
}

static void
l1_isprint(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "isprint", xisprint);
}

static void
l1_ispunct(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "ispunct", xispunct);
}

static void
l1_isspace(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "isspace", xisspace);
}

static void
l1_isupper(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "isupper", xisupper);
}

static void
l1_isxdigit(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isxctype(vm, argc, argv, rv, "isxdigit", xisxdigit);
}

static void
l1_isx(VM *vm, Imm argc, Val *argv, Val *rv, char *name, Qkind kind)
{
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to %s", name);
	if(Vkind(argv[0]) == kind)
		*rv = mkvalcval2(cval1);
	else
		*rv = mkvalcval2(cval0);
}

static void
l1_isas(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isas", Qas);
}

static void
l1_isctype(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isctype", Qxtn);
}

static void
l1_iscvalue(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "iscval", Qcval);
}

static void
l1_isdom(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isdom", Qdom);
}

static void
l1_isfd(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isfd", Qfd);
}

static void
l1_islist(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "islist", Qlist);
}

static void
l1_isns(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isns", Qns);
}

static void
l1_isnull(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isnull", Qnull);
}

static void
l1_ispair(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "ispair", Qpair);
}

static void
l1_isprocedure(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isprocedure", Qcl);
}

static void
l1_isrange(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isrange", Qrange);
}

static void
l1_isrec(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isrec", Qrec);
}

static void
l1_isrd(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isrd", Qrd);
}

static void
l1_isstring(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isstring", Qstr);
}

static void
l1_istable(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "istable", Qtab);
}

static void
l1_isvector(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isvector", Qvec);
}

static void
l1_isundefined(VM *vm, Imm argc, Val *argv, Val *rv)
{
	l1_isx(vm, argc, argv, rv, "isundefined", Qundef);
}

static void
l1_gc(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if((thegc->state&GCenabled) == 0)
		vmerr(vm, "GC is disabled");
	if(thegc->gcrun)
		xprintf("gc already running\n");
	else{
		dogc(thegc);
		dogc(thegc);
		gcfinal(thegc, vm);
	}
}

static void
l1_gcenable(VM *vm, Imm argc, Val *argv, Val *rv)
{
	gcenable(vm);
}

static void
l1_gcdisable(VM *vm, Imm argc, Val *argv, Val *rv)
{
	gcdisable(vm);
}

static void
l1_finalize(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Head *hd;
	Closure *cl, *ocl;
	if(argc != 2)
		vmerr(vm, "wrong number of arguments to finalize");
	hd = (Head*)argv[0];
	if(Vkind(argv[1]) == Qcl){
		cl = valcl(argv[1]);
		ocl = valcl(tabget(finals, hd));
		if(ocl){
			gcunpersist(vm, ocl);
			// gcwb is done by tabput
 			// gcwb(thegc, mkvalcl(ocl));
		}
		gcpersist(vm, cl);
		tabput(vm, finals, hd, mkvalcl(cl));
		Vsetfinal(hd, 1);
//	        xprintf("set final on %p (%d)\n", hd, Vfinal(hd));
	}else if(Vkind(argv[1]) == Qnil){
		tabdel(vm, finals, hd);
		Vsetfinal(hd, 0);
	}else
		vmerr(vm, "argument 1 to finalize must be a function or nil");
}

static void
l1_meminuse(VM *vm, Imm argc, Val *argv, Val *rv)
{
	*rv = mkvallitcval(Vuvlong, cqctmeminuse);
}

static void
l1_memtotal(VM *vm, Imm argc, Val *argv, Val *rv)
{
	*rv = mkvallitcval(Vuvlong, cqctmemtotal);
}

static void
l1_heapstat(VM *vm, Imm argc, Val *argv, Val *rv)
{
	char *s;

	s = 0;
	if(argc != 0 && argc != 1)
		vmerr(vm, "wrong number of arguments to heapstat");
	if(argc == 1){
		checkarg(vm, "heapstat", argv, 0, Qstr);
		s = str2cstr(valstr(argv[0]));
	}
	heapstat(s);
}

static void
l1_eval(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Str *str;
	Val cl;

	char *s;
	checkarg(vm, "eval", argv, 0, Qstr);
	str = valstr(argv[0]);
	s = str2cstr(str);
	cl = cqctcompile(s, "<eval-input>", vm->top, 0);
	efree(s);
	if(cl == 0)
		return;
	*rv = dovm(vm, valcl(cl), 0, 0);
}

static void
l1_resettop(VM *vm, Imm argc, Val *argv, Val *rv)
{
	if(thegc->gcrun){
		xprintf("gc is running; try again\n");
		return;
	}
	freeenv(vm->top->env);
	vm->top->env = mktopenv();
	vmresettop(vm);
}

static void
l1_loadpath(VM *vm, Imm argc, Val *argv, Val *rv)
{
	char **lp;
	List *l;
	if(argc != 0)
		vmerr(vm, "wrong number of arguments to loadpath");
	l = mklist();
	lp = cqctloadpath;
	while(*lp){
		listappend(vm, l, mkvalstr(mkstr0(*lp)));
		lp++;
	}
	*rv = mkvallist(l);
}

static void
l1_setloadpath(VM *vm, Imm argc, Val *argv, Val *rv)
{
	char **lp;
	List *l;
	Imm i, m;
	Val v;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to loadpath");
	checkarg(vm, "setloadpath", argv, 0, Qlist);
	l = vallist(argv[0]);
	m = listxlen(l->x);
	for(i = 0; i < m; i++){
		v = listref(vm, l, i);
		if(Vkind(v) != Qstr)
			vmerr(vm, "argument 1 to setloadpath "
			      "must be a list of strings");
	}
	lp = emalloc((m+1)*sizeof(char*));
	for(i = 0; i < m; i++)
		lp[i] = str2cstr(valstr(listref(vm, l, i)));
	efree(cqctloadpath);
	cqctloadpath = copystrv(lp);
	for(i = 0; i < m; i++)
		efree(lp[i]);
	efree(lp);
}

static Val
expr2list(Expr *e)
{
	List *l;
	Expr *p;

	if(e == 0)
		return Xnil;
	l = mklist();
	_listappend(l, mkvalstr(mkstr0(S[e->kind])));
	_listappend(l, mkvalstr(mkstr0(e->src.filename)));
	_listappend(l, mkvallitcval(Vuint, e->src.line));
	switch(e->kind){
	case Eid:
		_listappend(l, mkvalstr(mkstr0(e->id)));
		break;
	case Econst:
		_listappend(l, mkvallitcval(e->liti.base, e->liti.val));
		break;
	case Econsts:
		_listappend(l, mkvalstr(mkstr(e->lits->s, e->lits->len)));
		break;
	case Ebinop:
	case Egop:
		_listappend(l, mkvalstr(mkstr0(S[e->op])));
		_listappend(l, expr2list(e->e1));
		_listappend(l, expr2list(e->e2));
		break;
	case Eelist:
		p = e;
		while(p->kind == Eelist){
			_listappend(l, expr2list(p->e1));
			p = p->e2;
		}
		break;
	default:
		_listappend(l, expr2list(e->e1));
		_listappend(l, expr2list(e->e2));
		_listappend(l, expr2list(e->e3));
		_listappend(l, expr2list(e->e4));
		break;
	}
	return mkvallist(l);
}

static void
l1_parse(VM *vm, Imm argc, Val *argv, Val *rv)
{
	U ctx;
	Expr *e;
	char *whence;
	char *buf;

	if(argc != 1 && argc != 2)
		vmerr(vm, "wrong number of arguments to parse");
	checkarg(vm, "parse", argv, 0, Qstr);
	if(argc == 2)
		checkarg(vm, "parse", argv, 1, Qstr);
	buf = str2cstr(valstr(argv[0]));
	whence = "<stdin>";
	if(argc == 2)
		whence = str2cstr(valstr(argv[1]));
	memset(&ctx, 0, sizeof(ctx));
	ctx.out = &vm->top->out;
	e = doparse(&ctx, buf, whence);
	if(e == 0)
		vmerr(vm, "could not parse expression");
	*rv = expr2list(e);
	freeexpr(e);
}

static void
l1_cval2str(VM *vm, Imm argc, Val *argv, Val *rv)
{
	Str *s;
	Cval *cv;
	if(argc != 1)
		vmerr(vm, "wrong number of arguments to cval2str");
	checkarg(vm, "cval2str", argv, 0, Qcval);
	cv = valcval(argv[0]);
	s = imm2str(vm, cv->type, cv->val);
	*rv = mkvalstr(s);
}

char*
cqctsprintval(VM *vm, Val v)
{
	Val argv[2], rv;

	Str *s;
	s = mkstrk("%a", 2, Sperm);
	argv[0] = mkvalstr(s);
	argv[1] = v;
	l1_sprintfa(vm, 2, argv, &rv);
	return str2cstr(valstr(rv));
}

typedef
struct NSroot {
	Rkind base[Vnbase];
	Cbase ptr;
	Cbase xint8, xint16, xint32, xint64;
	Cbase xuint8, xuint16, xuint32, xuint64;
	char *name;
} NSroot;

static NSroot c32le = {
.base = {
	[Vchar]=	Rs08le,
	[Vshort]=	Rs16le,
	[Vint]=		Rs32le,
	[Vlong]=	Rs32le,
	[Vvlong]=	Rs64le,
	[Vuchar]=	Ru08le,
	[Vushort]=	Ru16le,
	[Vuint]=	Ru32le,
	[Vulong]=	Ru32le,
	[Vuvlong]=	Ru64le,
	[Vfloat]=	Rundef,
	[Vdouble]=	Rundef,
	[Vlongdouble]=	Rundef,
	},
.ptr = Vulong,
.xint8 = Vchar,
.xint16 = Vshort,
.xint32 = Vlong,
.xint64 = Vvlong,
.xuint8 = Vuchar,
.xuint16 = Vushort,
.xuint32 = Vulong,
.xuint64 = Vuvlong,
.name = "c32le",
};

static NSroot c32be = {
.base = {
	[Vchar]=	Rs08be,
	[Vshort]=	Rs16be,
	[Vint]=		Rs32be,
	[Vlong]=	Rs32be,
	[Vvlong]=	Rs64be,
	[Vuchar]=	Ru08be,
	[Vushort]=	Ru16be,
	[Vuint]=	Ru32be,
	[Vulong]=	Ru32be,
	[Vuvlong]=	Ru64be,
	[Vfloat]=	Rundef,
	[Vdouble]=	Rundef,
	[Vlongdouble]=	Rundef,
	},
.ptr = Vulong,
.xint8 = Vchar,
.xint16 = Vshort,
.xint32 = Vlong,
.xint64 = Vvlong,
.xuint8 = Vuchar,
.xuint16 = Vushort,
.xuint32 = Vulong,
.xuint64 = Vuvlong,
.name = "c32be",
};

static NSroot c64le = {
.base = {
	[Vchar]=	Rs08le,
	[Vshort]=	Rs16le,
	[Vint]=		Rs32le,
	[Vlong]=	Rs64le,
	[Vvlong]=	Rs64le,
	[Vuchar]=	Ru08le,
	[Vushort]=	Ru16le,
	[Vuint]=	Ru32le,
	[Vulong]=	Ru64le,
	[Vuvlong]=	Ru64le,
	[Vfloat]=	Rundef,
	[Vdouble]=	Rundef,
	[Vlongdouble]=	Rundef,
	},
.ptr = Vuint,
.xint8 = Vchar,
.xint16 = Vshort,
.xint32 = Vint,
.xint64 = Vlong,
.xuint8 = Vuchar,
.xuint16 = Vushort,
.xuint32 = Vuint,
.xuint64 = Vulong,
.name = "c64le",
};

static NSroot c64be = {
.base = {
	[Vchar]=	Rs08be,
	[Vshort]=	Rs16be,
	[Vint]=		Rs32be,
	[Vlong]=	Rs64be,
	[Vvlong]=	Rs64be,
	[Vuchar]=	Ru08be,
	[Vushort]=	Ru16be,
	[Vuint]=	Ru32be,
	[Vulong]=	Ru64be,
	[Vuvlong]=	Ru64be,
	[Vfloat]=	Rundef,
	[Vdouble]=	Rundef,
	[Vlongdouble]=	Rundef,
	},
.ptr = Vuint,
.xint8 = Vchar,
.xint16 = Vshort,
.xint32 = Vint,
.xint64 = Vlong,
.xuint8 = Vuchar,
.xuint16 = Vushort,
.xuint32 = Vuint,
.xuint64 = Vulong,
.name = "c64be",
};

static NSroot clp64le = {
.base = {
	[Vchar]=	Rs08le,
	[Vshort]=	Rs16le,
	[Vint]=		Rs32le,
	[Vlong]=	Rs64le,
	[Vvlong]=	Rs64le,
	[Vuchar]=	Ru08le,
	[Vushort]=	Ru16le,
	[Vuint]=	Ru32le,
	[Vulong]=	Ru64le,
	[Vuvlong]=	Ru64le,
	[Vfloat]=	Rundef,
	[Vdouble]=	Rundef,
	[Vlongdouble]=	Rundef,
	},
.ptr = Vulong,
.xint8 = Vchar,
.xint16 = Vshort,
.xint32 = Vint,
.xint64 = Vlong,
.xuint8 = Vuchar,
.xuint16 = Vushort,
.xuint32 = Vuint,
.xuint64 = Vulong,
.name = "clp64le",
};

static NSroot clp64be = {
.base = {
	[Vchar]=	Rs08be,
	[Vshort]=	Rs16be,
	[Vint]=		Rs32be,
	[Vlong]=	Rs64be,
	[Vvlong]=	Rs64be,
	[Vuchar]=	Ru08be,
	[Vushort]=	Ru16be,
	[Vuint]=	Ru32be,
	[Vulong]=	Ru64be,
	[Vuvlong]=	Ru64be,
	[Vfloat]=	Rundef,
	[Vdouble]=	Rundef,
	[Vlongdouble]=	Rundef,
	},
.ptr = Vulong,
.xint8 = Vchar,
.xint16 = Vshort,
.xint32 = Vint,
.xint64 = Vlong,
.xuint8 = Vuchar,
.xuint16 = Vushort,
.xuint32 = Vuint,
.xuint64 = Vulong,
.name = "clp64be",
};

static Xtypename*
mkvoidxtn()
{
	Xtypename *xtn;
	xtn = mkxtn();
	xtn->tkind = Tvoid;
	return xtn;
}

static Xtypename*
mkundefxtn(Xtypename *t)
{
	Xtypename *xtn;
	xtn = mkxtn();
	xtn->tkind = Tundef;
	xtn->link = t;
	return xtn;
}

static Xtypename*
mkbasextn(Cbase name, Rkind rep)
{
	Xtypename *xtn;
	xtn = mkxtn();
	xtn->tkind = Tbase;
	xtn->basename = name;
	xtn->rep = rep;
	return xtn;
}

static Xtypename*
mkptrxtn(Xtypename *t, Rkind rep)
{
	Xtypename *xtn;
	xtn = mkxtn();
	xtn->tkind = Tptr;
	xtn->link = t;
	xtn->rep = rep;
	return xtn;
}

static Xtypename*
mkconstxtn(Xtypename *t)
{
	Xtypename *xtn;
	xtn = mkxtn();
	xtn->tkind = Tconst;
	xtn->link = t;
	return xtn;
}

static Xtypename*
mktypedefxtn(Str *tid, Xtypename *t)
{
	Xtypename *xtn;
	xtn = mkxtn();
	xtn->tkind = Ttypedef;
	xtn->tid = tid;
	xtn->link = t;
	return xtn;
}

static Tab*
basetab(NSroot *def, Xtypename **base)
{
	Cbase cb;
	Val kv, vv;
	Tab *type;
	Str *tn;

	for(cb = Vchar; cb < Vnbase; cb++)
		base[cb] = mkbasextn(cb, def->base[cb]);
	base[Vptr] = base[def->ptr];

	type = mktab();
	for(cb = Vchar; cb < Vnbase; cb++){
		kv = mkvalxtn(mkbasextn(cb, Rundef));
		vv = mkvalxtn(base[cb]);
		_tabput(type, kv, vv);
	}

	/* map pointer to integer representation */
	kv = mkvalxtn(mkbasextn(Vptr, Rundef));
	vv = mkvalxtn(base[Vptr]);
	_tabput(type, kv, vv);

	/* define stdint-like integer typedefs */

	tn = mkstr0("uintptr");
	kv = mkvalxtn(mktypedefxtn(tn, 0));
	vv = mkvalxtn(mktypedefxtn(tn, base[def->ptr]));
	_tabput(type, kv, vv);

	tn = mkstr0("int8");
	kv = mkvalxtn(mktypedefxtn(tn, 0));
	vv = mkvalxtn(mktypedefxtn(tn, base[def->xint8]));
	_tabput(type, kv, vv);
	tn = mkstr0("int16");
	kv = mkvalxtn(mktypedefxtn(tn, 0));
	vv = mkvalxtn(mktypedefxtn(tn, base[def->xint16]));
	_tabput(type, kv, vv);
	tn = mkstr0("int32");
	kv = mkvalxtn(mktypedefxtn(tn, 0));
	vv = mkvalxtn(mktypedefxtn(tn, base[def->xint32]));
	_tabput(type, kv, vv);
	tn = mkstr0("int64");
	kv = mkvalxtn(mktypedefxtn(tn, 0));
	vv = mkvalxtn(mktypedefxtn(tn, base[def->xint64]));
	_tabput(type, kv, vv);

	tn = mkstr0("uint8");
	kv = mkvalxtn(mktypedefxtn(tn, 0));
	vv = mkvalxtn(mktypedefxtn(tn, base[def->xuint8]));
	_tabput(type, kv, vv);
	tn = mkstr0("uint16");
	kv = mkvalxtn(mktypedefxtn(tn, 0));
	vv = mkvalxtn(mktypedefxtn(tn, base[def->xuint16]));
	_tabput(type, kv, vv);
	tn = mkstr0("uint32");
	kv = mkvalxtn(mktypedefxtn(tn, 0));
	vv = mkvalxtn(mktypedefxtn(tn, base[def->xuint32]));
	_tabput(type, kv, vv);
	tn = mkstr0("uint64");
	kv = mkvalxtn(mktypedefxtn(tn, 0));
	vv = mkvalxtn(mktypedefxtn(tn, base[def->xuint64]));
	_tabput(type, kv, vv);

	return type;
}

static Ns*
mkrootns(NSroot *def)
{
	Tab *type;
	Ns *ns;
	Xtypename *base[Vnallbase];

	memset(base, 0, sizeof(base)); /* values will be seen by GC */
	type = basetab(def, base);
	ns = mknstype(type, mkstr0(def->name));
	memcpy(ns->base, base, sizeof(base));
	return ns;
}

static Dom*
mklitdom()
{
	Dom *dom;
	dom = mkdom(mkrootns(&clp64le), mknas(), mkstr0("litdom"));

	litdom = dom;
	litns = dom->ns;
	litbase = litns->base;

	return dom;
}

static Env*
mktopenv()
{
	Env *env;
	Dom *litdom;

	env = mkenv();
	
	builtinfn(env, "halt", haltthunk());
	builtinfn(env, "callcc", callcc());

	FN(append);
	FN(apply);
	FN(arraynelm);
	FN(asof);
	FN(attroff);
	FN(backtrace);
	FN(baseid);
	FN(bitfieldcontainer);
	FN(bitfieldpos);
	FN(bitfieldwidth);
	FN(bsearch);
	FN(callmethod);
	FN(car);
	FN(cdr);
	FN(close);
	FN(cntrget);
	FN(cntrput);
	FN(concat);
	FN(cons);
	FN(copy);
	FN(count);
	FN(cval2str);
	FN(delete);
	FN(domof);
	FN(enumconsts);
	FN(equal);
	FN(eval);
	FN(error);
	FN(fault);
	FN(fdname);
	FN(fieldattr);
	FN(fieldid);
	FN(fieldoff);
	FN(fields);
	FN(fieldtype);
	FN(finalize);
	FN(foreach);
	FN(gc);
	FN(gcenable);
	FN(gcdisable);
	FN(getbytes);
	FN(head);
	FN(heapstat);
	FN(index);
	FN(isalnum);
	FN(isalpha);
	FN(isarray);
	FN(isas);
	FN(isascii);
	FN(isbase);
	FN(isblank);
	FN(isbitfield);
	FN(iscntrl);
	FN(isctype);
	FN(iscvalue);
	FN(isdigit);
	FN(isdom);
	FN(isempty);
	FN(isenum);
	FN(isenumconst);
	FN(isfd);
	FN(isfunc);
	FN(isgraph);
	FN(islist);
	FN(islower);
	FN(ismapped);
	FN(ismember);
	FN(isnil);
	FN(isns);
	FN(isnull);
	FN(isodigit);
	FN(ispair);
	FN(isprint);
	FN(isprocedure);
	FN(isptr);
	FN(ispunct);
	FN(isrange);
	FN(isrec);
	FN(isrd);
	FN(isspace);
	FN(isstring);
	FN(isstruct);
	FN(issu);
	FN(istable);
	FN(istypedef);
	FN(isundefined);
	FN(isundeftype);
	FN(isunion);
	FN(isupper);
	FN(isvector);
	FN(isvoid);
	FN(isxaccess);
	FN(isxdigit);
	FN(length);
	FN(list);
	FN(listdel);
	FN(listins);
	FN(listref);
	FN(listset);
	FN(loadpath);
	FN(lookfield);
	FN(looksym);
	FN(looktype);
	FN(malloc);
	FN(map);
	FN(memcpy);
	FN(meminuse);
	FN(memset);
	FN(memtotal);
	FN(mkas);
	FN(mkattr);
	FN(mkctype_array);
	FN(mkctype_bitfield);
	FN(mkctype_char);
	FN(mkctype_const);
	FN(mkctype_double);
	FN(mkctype_enum);
	FN(mkctype_float);
	FN(mkctype_fn);
	FN(mkctype_int);
	FN(mkctype_ldouble);
	FN(mkctype_long);
	FN(mkctype_ptr);
	FN(mkctype_short);
	FN(mkctype_struct);
	FN(mkctype_typedef);
	FN(mkctype_uchar);
	FN(mkctype_uint);
	FN(mkctype_ulong);
	FN(mkctype_union);
	FN(mkctype_ushort);
	FN(mkctype_uvlong);
	FN(mkctype_vlong);
	FN(mkctype_void);
	FN(mkctype_xaccess);
	FN(mkdom);
	FN(mkfd);
	FN(mkfield);
	FN(mklist);
	FN(mknas);
	FN(mkns);
	FN(mknsraw);
	FN(mkparam);
	FN(mkrange);
	FN(mkrd);
	FN(mksas);
	FN(mkstr);
	FN(mksym);
	FN(mktab);
	FN(mkvec);
	FN(mkzas);
	FN(myrootns);
	FN(nameof);
	FN(nsof);
	FN(nsenumsym);
	FN(nsenumtype);
	FN(nslookaddr);
	FN(nsptr);
	FN(null);
	FN(paramid);
	FN(params);
	FN(paramtype);
	FN(parse);
	FN(pop);
	FN(push);
	FN(putbytes);
	FN(rangebeg);
	FN(rangelen);
	FN(rdfields);
	FN(rdfmt);
	FN(rdgettab);
	FN(rdis);
	FN(rdmk);
	FN(rdname);
	FN(rdof);
	FN(rdsetfmt);
	FN(rdsettab);
	FN(resettop);
	FN(rettype);
	FN(reverse);
	FN(setloadpath);
	FN(slice);
	FN(sort);
	FN(split);
	FN(sprintfa);
	FN(stringof);
	FN(strlen);
	FN(strput);
	FN(strref);
	FN(strstr);
	FN(strton);
	FN(substr);
	FN(subtype);
	FN(suekind);
	FN(suetag);
	FN(susize);
	FN(symattr);
	FN(symid);
	FN(symoff);
	FN(symtype);
	FN(tabdelete);
	FN(tabenum);
	FN(tabinsert);
	FN(tabkeys);
	FN(tablook);
	FN(tabvals);
	FN(tail);
	FN(tolower);
	FN(toupper);
	FN(typedefid);
	FN(typedeftype);
	FN(veclen);
	FN(vecref);
	FN(vecset);
	FN(vector);
	FN(xaccessget);
	FN(xaccessput);

	fns(env);		/* configuration-specific functions */

	/* FIXME: these bindings should be immutable */
	litdom = mklitdom();
	builtinval(env, "nil", Xnil);
	builtindom(env, "litdom", litdom);
	builtinns(env, "c32le", mkrootns(&c32le));
	builtinns(env, "c32be", mkrootns(&c32be));
	builtinns(env, "c64le", mkrootns(&c64le));
	builtinns(env, "c64be", mkrootns(&c64be));
	builtinns(env, "clp64le", mkrootns(&clp64le));
	builtinns(env, "clp64be", mkrootns(&clp64be));
	cvalnull = mkcval(litdom, litdom->ns->base[Vptr], 0);
	builtincval(env, "NULL", cvalnull);
	builtinnil(env, "$$");

	finals = mktab();
	finals->weak = 1;
	cval0 = mkcval(litdom, litdom->ns->base[Vint], 0);
	cval1 = mkcval(litdom, litdom->ns->base[Vint], 1);
	cvalminus1 = mkcval(litdom, litdom->ns->base[Vint], -1);

	/* stashed in env only to protect them from gc */
	builtincval(env, "$0", cval0);
	builtincval(env, "$1", cval1);
	builtincval(env, "$-1", cvalminus1);

	/* expanded source may call these magic functions */
	builtinfn(env, "$put", mkcfn("$put", l1_put));
	builtinfn(env, "$typeof", mkcfn("$typeof", l1_typeof));

	return env;
}

VM*
cqctmkvm(Toplevel *top)
{
	VM *vm, **vmp;

	vm = emalloc(sizeof(VM));
	vm->top = top;
	vm->pmax = GCinitprot;
	vm->prot = emalloc(vm->pmax*sizeof(Root*));
	vm->emax = Errinitdepth;
	vm->err = emalloc(vm->emax*sizeof(Err));
	
	gcprotpush(vm);		/* persistent references */
	vmresetctl(vm);

	/* register vm with fault handler */
	vmp = vms;
	while(*vmp){
		vmp++;
		if(vmp > vms+Maxvms)
			fatal("too many vms");
	}
	*vmp = vm;
	nvms++;

	/* vm is now callable */
	vmresettop(vm);		/* populate toplevel */
	return vm;
}

void
cqctfreevm(VM *vm)
{
	VM **vmp;

	if(nvms == 1){
		/* last one -- switch to serial gc and run finalizers */
		/* FIXME: can this been done in finivm (otherwise,
		   never called in -x mode) */
		if(thegc->state&GCenabled)
			thegc->gckill(thegc);
		thegc->gcinit = serialgcinit;
		thegc->gcpoll = serialgcpoll;
		thegc->gckill = serialgckill;
		thegc->gcinit(thegc);

		// run finalizers until they are gone or one of them errors
		if(!waserror(vm)){
			do{
				gcfinal(thegc, vm);
				dogc(thegc);
				dogc(thegc);
			}while(!rootsetempty(&thegc->finals));
			poperror(vm);
		}
	}
	gcprotpop(vm);

	freefreeroots(&vm->rs);
	efree(vm->prot);
	efree(vm->err);
	vmp = vms;
	while(vmp < vms+Maxvms){
		if(*vmp == vm){
			*vmp = 0;
			break;
		}
		vmp++;
	}
	efree(vm);
	nvms--;
}

static void
vmfaulthook()
{
	VM **vmp;
	vmp = vms;
	while(vmp < vms+Maxvms){
		if(*vmp){
			xprintf("backtrace of vm %p:\n", *vmp);
			fvmbacktrace(*vmp);
			xprintf("\n");
		}
		vmp++;
	}
}

void
initvm(int gcthread, u64 heapmax)
{
	Head *hd;

	GCiterdone = emalloc(1); /* unique pointer */
	thegc = emalloc(sizeof(GC));
	if(heapmax){
		heapmax *= 1024*1024; /* heapmax was in MB */
		thegc->heapmax = heapmax;
		thegc->heaphi = 2*heapmax/3;
	}
	if(gcthread){
		thegc->gcinit = concurrentgcinit;
		thegc->gcpoll = concurrentgcpoll;
		thegc->gckill = concurrentgckill;
	}else{
		thegc->gcinit = serialgcinit;
		thegc->gcpoll = serialgcpoll;
		thegc->gckill = serialgckill;
	}

	thegc->gcinit(thegc);

	hd = emalloc(sizeof(Head));
	Vsetkind(hd, Qundef);
	Xundef = hd;

	hd = emalloc(sizeof(Head));
	Vsetkind(hd, Qnil);
	Xnil = hd;

	hd = emalloc(sizeof(Head));
	Vsetkind(hd, Qnull);
	Xnulllist = hd;

	kcode = contcode();
	cccode = callccode();

	cqctfaulthook(vmfaulthook, 1);
}

void
finivm()
{
	unsigned i;
	Heap *hp;

	/* concurrent gc should be off (cqctfreevm) */

	/* drop persistent refs (FIXME: these should go into gc persist) */
	kcode = 0;
	cccode = 0;

	/* run two epochs without mutator to collect all objects;
	   then gcreset to free rootsets */
	gcclearfinal(thegc);	    /* clear finalizer list  */
	thegc->state |= GCshutdown; /* don't schedule new finalizers */
	dogc(thegc);
	dogc(thegc);
	gcreset(thegc);
//	heapstat("the end");

	freefreeroots(&thegc->roots);
	freefreeroots(&thegc->stores);
	freefreeroots(&thegc->finals);
	efree(thegc);
	efree(GCiterdone);
	efree(Xundef);
	efree(Xnil);
	efree(Xnulllist);

	for(i = 0; i < Qnkind; i++){
		hp = &heap[i];
		if(hp->id)
			freeheap(hp);
	}
	cqctfaulthook(vmfaulthook, 0);
}

int
cqctcallfn(VM *vm, Val cl, int argc, Val *argv, Val *rv)
{
	if(waserror(vm))
		return -1;
	if(Vkind(cl) != Qcl)
		return -1;
	vm->flags &= ~VMirq;
	*rv = dovm(vm, valcl(cl), argc, argv);
	poperror(vm);
	return 0;
}

int
cqctcallthunk(VM *vm, Val cl, Val *rv)
{
	if(waserror(vm))
		return -1;
	if(Vkind(cl) != Qcl)
		return -1;
	vm->flags &= ~VMirq;
	*rv = dovm(vm, valcl(cl), 0, 0);
	poperror(vm);
	return 0;
}

Cbase
cqctvalcbase(Val v)
{
	Cval *cv;
	Xtypename *t;
	if(Vkind(v) != Qcval)
		return (Cbase)-1;
	cv = valcval(v);
	t = chasetype(cv->type);
	switch(t->tkind){
	case Tbase:
		return t->basename;
	case Tptr:
		return cv->dom->ns->base[Vptr]->basename;
	default:
		/* only scalar types in cvalues */
		fatal("bug");
	}
}

Val
cqctmkfd(Xfd *xfd, char *name)
{
	Fd *fd;
	Str *n;
	
	if(name)
		n = mkstr0(name);
	else
		n = mkstr0("");
	fd = mkfdfn(n, Fread|Fwrite, xfd);
	return mkvalfd(fd);
}

/* these cqctval<type> routines and their inverses assume litdom is clp64le */

int8_t
cqctvalint8(Val v)
{
	Cval *cv, *rv;
	Xtypename *t;

	if(Vkind(v) != Qcval)
		return -1;
	cv = valcval(v);
	t = litdom->ns->base[clp64le.xint8];
	rv = mkcval(litdom, t, rerep(cv->val, cv->type, t));
	return rv->val;
}

int16_t
cqctvalint16(Val v)
{
	Cval *cv, *rv;
	Xtypename *t;

	if(Vkind(v) != Qcval)
		return -1;
	cv = valcval(v);
	t = litdom->ns->base[clp64le.xint16];
	rv = mkcval(litdom, t, rerep(cv->val, cv->type, t));
	return rv->val;
}

int32_t
cqctvalint32(Val v)
{
	Cval *cv, *rv;
	Xtypename *t;

	if(Vkind(v) != Qcval)
		return -1;
	cv = valcval(v);
	t = litdom->ns->base[clp64le.xint32];
	rv = mkcval(litdom, t, rerep(cv->val, cv->type, t));
	return rv->val;
}

int64_t
cqctvalint64(Val v)
{
	Cval *cv, *rv;
	Xtypename *t;

	if(Vkind(v) != Qcval)
		return -1;
	cv = valcval(v);
	t = litdom->ns->base[clp64le.xint64];
	rv = mkcval(litdom, t, rerep(cv->val, cv->type, t));
	return rv->val;
}

uint8_t
cqctvaluint8(Val v)
{
	Cval *cv, *rv;
	Xtypename *t;

	if(Vkind(v) != Qcval)
		return -1;
	cv = valcval(v);
	t = litdom->ns->base[clp64le.xuint8];
	rv = mkcval(litdom, t, rerep(cv->val, cv->type, t));
	return rv->val;
}

uint16_t
cqctvaluint16(Val v)
{
	Cval *cv, *rv;
	Xtypename *t;

	if(Vkind(v) != Qcval)
		return -1;
	cv = valcval(v);
	t = litdom->ns->base[clp64le.xint16];
	rv = mkcval(litdom, t, rerep(cv->val, cv->type, t));
	return rv->val;
}

uint32_t
cqctvaluint32(Val v)
{
	Cval *cv, *rv;
	Xtypename *t;

	if(Vkind(v) != Qcval)
		return -1;
	cv = valcval(v);
	t = litdom->ns->base[clp64le.xuint32];
	rv = mkcval(litdom, t, rerep(cv->val, cv->type, t));
	return rv->val;
}

uint64_t
cqctvaluint64(Val v)
{
	Cval *cv, *rv;
	Xtypename *t;

	if(Vkind(v) != Qcval)
		return -1;
	cv = valcval(v);
	t = litdom->ns->base[clp64le.xuint64];
	rv = mkcval(litdom, t, rerep(cv->val, cv->type, t));
	return rv->val;
}

char*
cqctvalcstr(Val v)
{
	if(Vkind(v) != Qstr)
		return 0;
	return str2cstr(valstr(v));
}

char*
cqctvalcstrshared(Val v)
{
	Str *s;
	if(Vkind(v) != Qstr)
		return 0;
	s = valstr(v);
	return s->s;
}

uint64_t
cqctvalcstrlen(Val v)
{
	Str *s;
	if(Vkind(v) != Qstr)
		return 0;
	s = valstr(v);
	return (uint64_t)s->len;
}

Val
cqctcstrval(char *s)
{
	return mkvalstr(mkstr0(s));
}

Val
cqctcstrnval(char *s, uint64_t len)
{
	return mkvalstr(mkstr(s, len));
}

Val
cqctcstrvalshared(char *s)
{
	return mkvalstr(mkstrk(s, strlen(s), Sperm));
}

Val
cqctcstrnvalshared(char *s, uint64_t len)
{
	return mkvalstr(mkstrk(s, len, Sperm));
}

Val
cqctint8val(int8_t x)
{
	return mkvallitcval(clp64le.xint8, x);
}

Val
cqctint16val(int16_t x)
{
	return mkvallitcval(clp64le.xint16, x);
}

Val
cqctint32val(int32_t x)
{
	return mkvallitcval(clp64le.xint32, x);
}

Val
cqctint64val(int64_t x)
{
	return mkvallitcval(clp64le.xint64, x);
}

Val
cqctuint8val(uint8_t x)
{
	return mkvallitcval(clp64le.xuint8, x);
}

Val
cqctuint16val(uint16_t x)
{
	return mkvallitcval(clp64le.xuint16, x);
}

Val
cqctuint32val(uint32_t x)
{
	return mkvallitcval(clp64le.xuint32, x);
}

Val
cqctuint64val(uint64_t x)
{
	return mkvallitcval(clp64le.xuint64, x);
}

void
cqctfreecstr(char *s)
{
	efree(s);
}

void
cqctenvbind(Toplevel *top, char *name, Val v)
{
	Val *vp;
	vp = envget(top->env, name);
	if(vp)
		gcwb(thegc, *vp);
	envbind(top->env, name, v);
}

Val
cqctenvlook(Toplevel *top, char *name)
{
	Val *rv;
	rv = envget(top->env, name);
	if(rv)
		return *rv;
	return 0;
}

uint64_t
cqctlength(Val v)
{
	List *lst;
	Vec *vec;
	Str *str;
	Tab *tab;

	switch(Vkind(v)){
	default:
		return (uint64_t)-1;
	case Qlist:
		lst = vallist(v);
		return listxlen(lst->x);
	case Qstr:
		str = valstr(v);
		return str->len;
	case Qvec:
		vec = valvec(v);
		return vec->len;
	case Qtab:
		tab = valtab(v);
		return tab->cnt;
	}
}

Val*
cqctlistvals(Val v)
{
	List *lst;
	Listx *x;
	if(Vkind(v) != Qlist)
		return 0;
	lst = vallist(v);
	x = lst->x;
	return &x->val[x->hd];
}

Val*
cqctvecvals(Val v)
{
	Vec *vec;
	if(Vkind(v) != Qvec)
		return 0;
	vec = valvec(v);
	return vec->vec;
}
