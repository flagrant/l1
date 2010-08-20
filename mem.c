#include "sys.h"
#include "util.h"
#include "syscqct.h"

typedef
enum
{
	Mmal,
	Mcode,
	Mmax,
} Mkind;

enum
{
	Segsize = 4096,
	Segmask = ~(Segsize-1),
	GCthresh = 1024*Segsize,
	Ngen = 3,
	Gprot,
};

typedef struct Seg Seg;
struct Seg
{
	void *addr, *scan, *a, *e;
	Mkind kind;
	Pair *p;		/* protected objects */
	u32 nprotect;
	u32 gen;
	Seg *link;
};

typedef
struct Qtype
{
	char *id;
	u32 sz;
	u32 clearit;
	Freeheadfn free1;
	Val* (*iter)(Head *hd, Ictx *ictx);
} Qtype;

typedef
struct GCstat
{
	u64 na;
	u64 ta;
	u64 inuse;
	u64 ngc;
	u64 guards;
	u64 nprotect;
	u64 nseg;
} GCstat;

typedef
struct M
{
	Seg *h, *t;		/* segment list head and tail */
	u32 gen;
} M;

typedef
struct Heap
{
	Seg *d, *c;		/* current data and code segment */
	M data[Ngen];		/* data segments */
	M code[Ngen];		/* code segments */
	M prot;			/* protected segments */
	u64 na;			/* allocated bytes since last gc */
	u64 ma;			/* allocated bytes threshold */
	u64 ta;			/* allocated bytes since beginning */
	u64 inuse;		/* currently allocated bytes */
	Pair *g;		/* guarded objects */
	Pair *guards[Qnkind];
	u64 ngc;		/* number of gcs */
	u64 nseg;		/* number of segments */
	unsigned disable;
} Heap;

static int freecl(Head*);
static int freefd(Head*);
static int freerec(Head*);
static int freestr(Head*);
static int freetab(Head*);
static int freevec(Head*);

static Val* iteras(Head*, Ictx*);
static Val* iterbox(Head*, Ictx*);
static Val* itercl(Head*, Ictx*);
static Val* itercode(Head*, Ictx*);
static Val* itercval(Head*, Ictx*);
static Val* iterdom(Head*, Ictx*);
static Val* iterfd(Head*, Ictx*);
static Val* iterns(Head*, Ictx*);
static Val* iterpair(Head*, Ictx*);
static Val* iterrange(Head*, Ictx*);
static Val* iterrd(Head*, Ictx*);
static Val* iterrec(Head*, Ictx*);
static Val* itertab(Head*, Ictx*);
static Val* itervec(Head*, Ictx*);
static Val* iterxtn(Head*, Ictx*);

static Qtype qs[Qnkind] = {
	[Qas]	 = { "as", sizeof(As), 1, 0, iteras },
	[Qbox]	 = { "box", sizeof(Box), 0, 0, iterbox },
	[Qcval]  = { "cval", sizeof(Cval), 0, 0, itercval },
	[Qcl]	 = { "closure", sizeof(Closure), 1, freecl, itercl },
	[Qcode]	 = { "code", sizeof(Code), 1, freecode, itercode },
	[Qdom]	 = { "domain", sizeof(Dom), 0, 0, iterdom },
	[Qfd]	 = { "fd", sizeof(Fd), 0, freefd, iterfd },
	[Qlist]	 = { "list", sizeof(List), 0, freelist, iterlist },
	[Qnil]	 = { "nil", sizeof(Head), 0, 0, 0 },
	[Qns]	 = { "ns", sizeof(Ns), 1, 0, iterns },
	[Qnull]	 = { "null", sizeof(Head), 0, 0, 0 },
	[Qpair]	 = { "pair", sizeof(Pair), 0, 0, iterpair },
	[Qrange] = { "range", sizeof(Range), 0, 0, iterrange },
	[Qrd]    = { "rd", sizeof(Rd), 0, 0, iterrd },
	[Qrec]	 = { "record", sizeof(Rec), 0, freerec, iterrec },
	[Qstr]	 = { "string", sizeof(Str), 1, freestr, 0 },
	[Qtab]	 = { "table",  sizeof(Tab), 1, freetab, itertab },
	[Qundef] = { "undef", sizeof(Head), 0, 0, 0 },
	[Qvec]	 = { "vector", sizeof(Vec), 0, freevec, itervec },
	[Qxtn]	 = { "typename", sizeof(Xtypename), 1, 0, iterxtn },
};

static HT	*segtab;
static Heap	H;

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

static int
freerec(Head *hd)
{
	Rec *r;
	r = (Rec*)hd;
	efree(r->field);
	return 1;
}

static int
freestr(Head *hd)
{
	Str *str;
	str = (Str*)hd;
	// printf("freestr(%.*s)\n", (int)str->len, str->s);
	switch(str->skind){
	case Smmap:
		xmunmap(str->s, str->mlen);
		break;
	case Smalloc:
		efree(str->s);
		break;
	case Sperm:
		break;
	}
	return 1;
}

static void
freetabx(Tabx *x)
{
	efree(x->val);
	efree(x->key);
	efree(x->idx);
	efree(x);
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

static int
freevec(Head *hd)
{
	Vec *vec;
	vec = (Vec*)hd;
	efree(vec->vec);
	return 1;
}

static Val*
iteras(Head *hd, Ictx *ictx)
{
	/* FIXME: is it really necessary
	   to mark dispatch and the other
	   cached functions? */
	As *as;
	as = (As*)hd;
	switch(ictx->n++){
	case 0:
		return (Val*)&as->mtab;
	case 1:
		return (Val*)&as->name;
	case 2:
		return (Val*)&as->get;
	case 3:
		return (Val*)&as->put;
	case 4:
		return (Val*)&as->map;
	case 5:
		return (Val*)&as->dispatch;
	default:
		return GCiterdone;
	}
}

static Val*
iterbox(Head *hd, Ictx *ictx)
{
	Box *box;
	box = (Box*)hd;
	if(ictx->n > 0)
		return GCiterdone;
	ictx->n = 1;
	return &box->v;
}

static Val*
itercl(Head *hd, Ictx *ictx)
{
	Closure *cl;
	cl = (Closure*)hd;
	if(ictx->n > cl->dlen)
		return GCiterdone;
	if(ictx->n == cl->dlen){
		ictx->n++;
		return (Val*)&cl->code;
	}
	return &cl->display[ictx->n++];
}

static Val*
itercode(Head *hd, Ictx *ictx)
{
	Code *code;
	code = (Code*)hd;
	if(ictx->n > 0)
		return GCiterdone;
	ictx->n++;
	return (Val*)&code->konst;
}

static Val*
itercval(Head *hd, Ictx *ictx)
{
	Cval *cval;
	cval = (Cval*)hd;

	switch(ictx->n++){
	case 0:
		return (Val*)&cval->dom;
	case 1:
		return (Val*)&cval->type;
	default:
		return GCiterdone;
	}
}

static Val*
iterdom(Head *hd, Ictx *ictx)
{
	Dom *dom;
	dom = (Dom*)hd;
	switch(ictx->n++){
	case 0:
		return (Val*)&dom->as;
	case 1:
		return (Val*)&dom->ns;
	case 2:
		return (Val*)&dom->name;
	default:
		return GCiterdone;
	}
}

static Val*
iterfd(Head *hd, Ictx *ictx)
{
	Fd *fd;
	fd = (Fd*)hd;
	switch(ictx->n++){
	case 0:
		return (Val*)&fd->name;
	case 1:
		if(fd->flags&Ffn)
			return GCiterdone;
		return (Val*)&fd->u.cl.close;
	case 2:
		return (Val*)&fd->u.cl.read;
	case 3:
		return (Val*)&fd->u.cl.write;
	default:
		return GCiterdone;
	}
}

static Val*
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
		return (Val*)&ns->lookaddr;
	case 1:
		return (Val*)&ns->looksym;
	case 2:
		return (Val*)&ns->looktype;
	case 3:
		return (Val*)&ns->enumtype;
	case 4:
		return (Val*)&ns->enumsym;
	case 5:
		return (Val*)&ns->name;
	case 6:
		return (Val*)&ns->dispatch;
	case lastfield:
		return (Val*)&ns->mtab;
	}
	n -= lastfield;
	if(n >= Vnallbase)
		return GCiterdone;
	return (Val*)&ns->base[n];
}

static Val*
iterpair(Head *hd, Ictx *ictx)
{
	Pair *pair;
	pair = (Pair*)hd;

	switch(ictx->n++){
	case 0:
		return &pair->car;
	case 1:
		return &pair->cdr;
	default:
		return GCiterdone;
	}
}

static Val*
iterrange(Head *hd, Ictx *ictx)
{
	Range *range;
	range = (Range*)hd;

	switch(ictx->n++){
	case 0:
		return (Val*)&range->beg;
	case 1:
		return (Val*)&range->len;
	default:
		return GCiterdone;
	}
}

static Val*
iterrd(Head *hd, Ictx *ictx)
{
	Rd *rd;
	rd = (Rd*)hd;
	switch(ictx->n++){
	case 0:
		return (Val*)&rd->name;
	case 1:
		return (Val*)&rd->fname;
	case 2:
		return (Val*)&rd->is;
	case 3:
		return (Val*)&rd->mk;
	case 4:
		return (Val*)&rd->fmt;
	case 5:
		return (Val*)&rd->get;
	case 6:
		return (Val*)&rd->set;
	default:
		return GCiterdone;
	}
}

static Val*
iterrec(Head *hd, Ictx *ictx)
{
	Rec *r;
	r = (Rec*)hd;
	if(ictx->n < r->nf)
		return &r->field[ictx->n++];
	switch(ictx->n-r->nf){
	case 0:
		ictx->n++;
		return (Val*)&r->rd;
	default:
		return GCiterdone;
	}
}

static Val*
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
		return &x->val[idx];
	}
	if(tab->weak){
		/* skip ahead */
		ictx->n = nxt;
		return 0;
	}
	idx = ictx->n++;
	return &x->key[idx];
}

static Val*
itervec(Head *hd, Ictx *ictx)
{
	Vec *vec;
	vec = (Vec*)hd;
	if(ictx->n >= vec->len)
		return GCiterdone;
	return &vec->vec[ictx->n++];
}

static Val*
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
			return (Val*)&xtn->tag;
		case 1:
			return (Val*)&xtn->field;
		case 2:
			return &xtn->attr;
		default:
			return GCiterdone;
		}
	case Tenum:
		switch(ictx->n++){
		case 0:
			return (Val*)&xtn->tag;
		case 1:
			return (Val*)&xtn->link;
		case 2:
			return (Val*)&xtn->konst;
		default:
			return GCiterdone;
		}
	case Tundef:
	case Tptr:
		if(ictx->n++ > 0)
			return GCiterdone;
		else
			return (Val*)&xtn->link;
	case Tarr:
		switch(ictx->n++){
		case 0:
			return &xtn->cnt;
		case 1:
			return (Val*)&xtn->link;
		default:
			return GCiterdone;
		}
	case Tfun:
		switch(ictx->n++){
		case 0:
			return (Val*)&xtn->link;
		case 1:
			return (Val*)&xtn->param;
		default:
			return GCiterdone;
		}
		break;
	case Ttypedef:
		switch(ictx->n++){
		case 0:
			return (Val*)&xtn->link;
		case 1:
			return (Val*)&xtn->tid;
		default:
			return GCiterdone;
		}
	case Tbitfield:
		switch(ictx->n++){
		case 0:
			return &xtn->cnt;
		case 1:
			return &xtn->bit0;
		case 2:
			return (Val*)&xtn->link;
		default:
			return GCiterdone;
		}
	case Tconst:
		switch(ictx->n++){
		case 0:
			return (Val*)&xtn->link;
		default:
			return GCiterdone;
		}
	case Txaccess:
		switch(ictx->n++){
		case 0:
			return (Val*)&xtn->link;
		case 1:
			return (Val*)&xtn->get;
		case 2:
			return (Val*)&xtn->put;
		default:
			return GCiterdone;
		}
	}
	return 0;
}

static void*
mapseg()
{
	uintptr_t a;
	void *p;
	p = mmap(0, Segsize, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
	if(p == MAP_FAILED)
		fatal("out of memory");
	a = (uintptr_t)p;
	if(a&~Segmask)
		fatal("unaligned segment");
	return p;
}

static Seg*
mkseg(Mkind kind)
{
	Seg *s;
	s = emalloc(sizeof(Seg));
	s->addr = mapseg();
	s->a = s->scan = s->addr;
	s->e = s->addr+Segsize;
	s->kind = kind;
	hputp(segtab, s->addr, s);
	H.na += Segsize;
	H.ta += Segsize;
	H.inuse += Segsize;
	H.nseg++;
	return s;
}

static void
freeseg(Seg *s)
{
	hdelp(segtab, s);
	munmap(s->addr, Segsize);
	efree(s);
	H.inuse -= Segsize;
	H.nseg--;
}

static Seg*
lookseg(void *a)
{
	Seg *s;
	uintptr_t v;

	v = (uintptr_t)a;
	v &= Segmask;
	s = hgetp(segtab, (void*)v);
	if(s == 0)
		fatal("lookseg bug");
	return s;
}

static Seg*
minit(M *m, Seg *s)
{
	m->h = s;
	m->t = s;
	return s;
}

static Seg*
mappend(M *m, Seg *s)
{
	s->gen = m->gen;
	s->link = 0;
	if(m->h == 0)
		return minit(m, s);
	m->t->link = s;
	m->t = s;
	s->link = 0;
	return s;
}

u64
protected()
{
	Seg *p;
	u64 m;

	m = 0;
	p = H.data[0].h;
	while(p){
		m += p->nprotect;
		p = p->link;
	}
	p = H.code[0].h;
	while(p){
		m += p->nprotect;
		p = p->link;
	}
	p = H.prot.h;
	while(p){
		m += p->nprotect;
		p = p->link;
	}

	return m;
}

u64
guarded()
{
	u64 m;
	Pair *p;
	m = 0;
	p = H.g;
	while(p){
		m++;
		p = (Pair*)p->cdr;
	}
	return m;
}

Str*
gcstat()
{
	GCstat s;

	memset(&s, 0, sizeof(s));
	s.na = H.na;
	s.ta = H.ta;
	s.inuse = H.inuse;
	s.ngc = H.ngc;
	s.guards = guarded();
	s.nprotect = protected();
	s.nseg = H.nseg;
	return mkstr((char*)&s, sizeof(s));
}

u64
meminuse()
{
	return H.inuse;
}

Head*
malcode()
{
	Seg *s;
	Head *h;
	u32 sz;
	sz = qs[Qcode].sz;
again:
	s = H.c;
	if(s->a+sz <= s->e){
		h = s->a;
		s->a += sz;
		Vsetkind(h, Qcode);
		return h;
	}
	H.c = mappend(&H.code[s->gen], mkseg(Mcode));
	goto again;
}

Head*
mal(Qkind kind)
{
	Seg *s;
	Head *h;
	u32 sz;
	sz = qs[kind].sz;
again:
	s = H.d;
	if(s->a+sz <= s->e){
		h = s->a;
		s->a += sz;
		Vsetkind(h, kind);
		return h;
	}
	H.d = mappend(&H.data[s->gen], mkseg(Mmal));
	goto again;
}

static void*
curaddr(Val v)
{
	if(Vfwd(v))
		return Vfwdaddr(v);
	return v;
}

static void
copy(Val *v)
{
	Head *h;
	Seg *s;
	u32 sz;
	Head *nh;

	h = *v;
	if(h == 0)
		return;
	if((uintptr_t)h&1)
		/* pointer tag: stack immediate */
		return;
	if(Vfwd(h)){
		// printf("read fwd %p -> %p\n", h, (void*)Vfwdaddr(h));
		*v = Vfwdaddr(h);
		return;
	}
	if(Vprot(h))
		return; // protected objects do not move
	s = lookseg(h);
	sz = qs[Vkind(h)].sz;
	if(Vkind(h) == Qcode)
		nh = malcode();
	else
		nh = mal(Vkind(h));
	memcpy(nh, h, sz);
	Vsetfwd(h, (uintptr_t)nh);
	if(0)printf("set fwd %p -> %p %p (%d)\n",
		    h, Vfwdaddr(h), nh, (int)Vfwd(h));
	*v = nh;
}

static void
scan1(Head *h)
{
	Ictx ictx;
	Head **c;
	if(qs[Vkind(h)].iter == 0)
		return;
	memset(&ictx, 0, sizeof(ictx));
	while(1){
		c = qs[Vkind(h)].iter(h, &ictx);
		if(c == (Val*)GCiterdone)
			break;
		copy(c);
	}
}

static void
scan(Seg *s)
{
	Head *h, **c;
	Ictx ictx;

	if(s == 0)
		return;
	while(s->scan < s->a){
		h = s->scan;
		s->scan += qs[Vkind(h)].sz;
		if(qs[Vkind(h)].iter == 0)
			continue;
		memset(&ictx, 0, sizeof(ictx));
		while(1){
			c = qs[Vkind(h)].iter(h, &ictx);
			if(c == (Val*)GCiterdone)
				break;
			copy(c);
		}
	}
	scan(s->link);
}

#define car(p)  (((Pair*)p)->car)
#define cdr(p)  (((Pair*)p)->cdr)
#define caar(p) (car(car(p)))
#define cadr(p) (car(cdr(p)))
#define cdar(p) (cdr(car(p)))
#define cddr(p) (cdr(cdr(p)))
#define setcar(p,x) { car(p) = (Head*)(x); }
#define setcdr(p,x) { cdr(p) = (Head*)(x); }

static Pair*
cons(void *a, void *d)
{
	Pair *p;
	p = (Pair*)mal(Qpair);
	setcar(p, a);
	setcdr(p, d);
	return p;
}

static Pair*
lastpair()
{
	return cons(0, 0);
}

Pair*
mkguard()
{
	Pair *p;
	p = lastpair();
	return cons(p, p);
}

void
instguard(Val o, Pair *t)
{
	H.g = cons(cons(o, t), H.g);
}

Head*
pop1guard(Pair *t)
{
	Head *x, *y;
	if(car(t) == cdr(t))
		return 0;
	x = car(t);
	y = car(x);
	setcar(t, cdr(x));
	setcar(x, 0);
	setcdr(x, 0);
	return y;
}

void
push1guard(Val o, Pair *t)
{
	Pair *p;
	p = lastpair();
	setcar(cdr(t), o);
	setcdr(cdr(t), p);
	setcdr(t, p);
}

void
quard(Val o)
{
	Pair *t;
	t = H.guards[Vkind(o)];
	if(t == 0)
		fatal("bug");
	instguard(o, t);
}

static void
updateguards()
{
	Head *phold, *pfinal, *final, *p, *q, *o, **r, *w;
	Seg *b;

	// move guarded objects and guards (and their containing cons) to
	// either pending hold or pending final list
	phold = 0;
	pfinal = 0;
	p = (Head*)H.g;
	while(p){
		q = cdr(p);
		o = caar(p);
		if(Vfwd(o) || Vprot(o)){
			// object is accessible
			setcdr(p, phold);
			phold = p;
		}else{
			// object is inaccessible
			// printf("inaccessible: %p\n", ((Pair*)p->car)->car);
			setcdr(p, pfinal);
			pfinal = p;
		}
		p = q;
	}
	H.g = 0;

	// move each pending final to final if guard is accessible
	while(1){
		final = 0;
		// FIXME: generation safe?
		r = &pfinal;
		p = pfinal;
		if(p == 0)
			break;
		q = cdr(p);
		o = cdar(p);
		if(Vfwd(o) || Vprot(o)){
			// guard is accessible
			*r = q;
			setcdr(p, final);
			final = p;
		}
		if(final == 0)
			break;
		b = H.d;
		w = final;
		while(w){
			copy(&caar(w));
			push1guard(caar(w), curaddr(cdar(w)));
			w = cdr(w);

		}
		scan(b);
		// FIXME: generation safe?
		r = &cdr(p);
		p = q;
	}

	// forward pending hold to fresh guarded list
	p = phold;
	while(p){
		o = cdar(p);
		if(Vfwd(o) || Vprot(o))
			// ...
			instguard(curaddr(caar(p)), curaddr(cdar(p)));
		p = cdr(p);
	}
}

void
gcenable()
{
	H.disable--;
}

void
gcdisable()
{
	H.disable++;
}

void
gcpoll()
{
	if(!H.disable && H.na >= H.ma)
		gc(0, 1);
}

void
gcwb(Val v)
{
}

static void
toproot(void *u, char *k, void *v)
{
	Val *p;
	p = v;
	copy(v);
}

static void
toprd(void *u, void *k, void *v)
{
	Val *p;
	p = v;
	copy(v);
}

static void
reloc1(Code *c)
{
	u32 i;
	u64 b;
	u64 *p;
	void **a;
	p = c->reloc;
	b = (u64)c->insn;
	for(i = 0; i < c->nreloc; i++){
		a = (void**)(b+p[i]);
		*a = curaddr(*a);
	}
}

static void
reloc()
{
	Seg *s;
	Head *h, *p;
	Code *c;

	s = H.code[0].h;
	while(s){
		c = s->addr;
		while((void*)c < s->a){
			reloc1(c);
			c++;
		}
		s = s->link;
	}

	s = H.prot.h;
	while(s){
		p = (Head*)s->p;
		while(p){
			h = car(p);
			if(Vkind(h) == Qcode)
				reloc1((Code*)h);
			p = cdr(p);
		}
		s = s->link;
	}
}

static void
copystack(VM *vm)
{
	Imm pc, fp, sp, narg, m, i, clx;
	u64 sz, mask;
	Closure *cl;

//	fvmbacktrace(vm);
	fp = vm->fp;
	if(fp == 0)
		return;
	pc = vm->pc;
	sp = vm->sp;
	cl = vm->clx;
	while(fp != 0){
		if(pc < 2)
			fatal("no way to find livemask pc %llu", pc);
		if(cl->code->insn[pc-1].kind != Ilive
		   || cl->code->insn[pc-2].kind != Ilive)
			fatal("no live mask for pc %d cl %p", pc, cl);
		sz = cl->code->insn[pc-1].cnt;
		mask = cl->code->insn[pc-2].cnt;
		if(fp-sp < sz)
			fatal("frame size is too large fp %llu sp %llu",
			      fp, sp);
		m = fp-1;
		for(i = 0; i < sz; i++){
			if((mask>>i)&1)
				copy(&vm->stack[m]);
			m--;
		}
		for(i = 0; i < fp-sp-sz; i++){
			copy(&vm->stack[m]);
			m--;
		}
		narg = stkimm(vm->stack[fp]);
		pc = stkimm(vm->stack[fp+narg+1]);
		clx = fp+narg+2;
		cl = valcl(vm->stack[fp+narg+2]);
		sp = fp;
		fp = stkimm(vm->stack[fp+narg+3]);
	}
	// initial frame of stack
	for(i = 0; i < narg; i++)
		copy(&vm->stack[sp+1+i]);
	copy(&vm->stack[clx]);
}

/*
	for generations 0...g copy live data to space for generation tg.
	if tg is g then create a new target space;
        else if tg is g+1 then use existing target space tg;
	else error.
*/
void
gc(u32 g, u32 tg)
{
	u32 i, m;
	VM **vmp, *vm;
	Seg *s, *t, *f, *c, *b;
	Head *h, *p;
	M junk, np;

	if(g != tg && g != tg-1)
		fatal("bug");

	if(0)printf("\ngc\n");
	f = H.data[0].h;
	c = H.code[0].h;
	H.d = minit(&H.data[0], mkseg(Mmal));
	H.c = minit(&H.code[0], mkseg(Mcode));
	minit(&junk, 0);
	minit(&np, 0);

	vmp = vms;
	while(vmp < vms+Maxvms){
		vm = *vmp++;
		if(vm == 0)
			continue;
		copystack(vm);
		for(m = 0; m < vm->edepth; m++)
			copy(&vm->err[m].cl);
		hforeach(vm->top->env->var, toproot, 0);
		hforeachp(vm->top->env->rd, toprd, 0);
		copy(&vm->ac);
		copy(&vm->cl);
		copy((Val*)&vm->clx);
	}
	for(i = 0; i < Qnkind; i++)
		copy((Val*)&H.guards[i]);

	scan(H.data[0].h);
	// scan code (FIXME: why can't this be done before above scan?)
	b = H.d;
	scan(H.code[0].h);
	scan(b);

	// reserve segments with newly protected objects
	s = f;
	while(s){
		t = s->link;
		if(s->nprotect)
			mappend(&H.prot, s);
		else
			mappend(&junk, s);
		s = t;
	}
	s = c;
	while(s){
		t = s->link;
		if(s->nprotect)
			mappend(&H.prot, s);
		else
			mappend(&junk, s);
		s = t;
	}

	// scan protected objects
	// FIXME: isn't this broken if any protected object is code,
	// since we don't scan the code segments again?
	b = H.d;
	s = H.prot.h;
	while(s){
		copy((Val*)&s->p);      // retain list of protected objects!
		p = (Head*)s->p;
		while(p){
			scan1(car(p));  // manual scan of protected object
			p = cdr(p);
		}
		s = s->link;
	}
	scan(b);

	updateguards();

	reloc();

	// call built-in finalizers
	for(i = 0; i < Qnkind; i++)
		if(H.guards[i])
			while((h = pop1guard(H.guards[i])))
				qs[i].free1(h);

	// stage unused protected segments for recycling
	// FIXME: maybe we should do this sooner, before we append
	// newly protected segments?
	s = H.prot.h;
	while(s){
		t = s->link;
		if(s->nprotect == 0)
			mappend(&junk, s);
		else
			mappend(&np, s);
		s = t;
	}
	H.prot = np;

	// recycle segments
	s = junk.h;
	while(s){
		t = s->link;
		freeseg(s);
		s = t;
	}

	H.na = 0;
	H.ngc++;
}

void*
gcprotect(void *v)
{
	Seg *s;
	Head *h;

	if(v == 0)
		return v;
	h = v;
	if(Vfwd(h))
		fatal("bug");
	if(Vprot(h))
		// FIXME: with a counter rather than a bit, we could
		// allow this.
		fatal("gcprotect on already protected object %p", h);
	s = lookseg(h);
	s->p = cons(h, s->p);
	s->nprotect++;
	Vsetprot(h, 1);
	return h;
}

void*
gcunprotect(void *v)
{
	Seg *s;
	Head *h, *p, **r;

	if(v == 0)
		return v;
	h = v;
	if(Vfwd(h))
		fatal("bug");
	if(!Vprot(h))
		fatal("gcunprotect on already unprotected object %p", h);
	s = lookseg(h);
	// FIXME: generation safe?
	r = (Head**)&s->p;
	p = *r;
	while(p){
		if(car(p) == h){
			*r = cdr(p);
			break;
		}
		r = &cdr(p);
		p = *r;
	}
	s->nprotect--;
	Vsetprot(h, 0);
	return h;
}

void
initmem(u64 gcrate)
{
	u32 i;

	segtab = mkhtp();
	for(i = 0; i < Ngen; i++){
		H.code[i].gen = i;
		H.data[i].gen = i;
	}
	H.d = minit(&H.data[0], mkseg(Mmal));
	H.c = minit(&H.code[0], mkseg(Mcode));
	minit(&H.prot, 0);
	H.prot.gen = Gprot;
	H.na = H.ta = 0;
	if(gcrate)
		H.ma = gcrate;
	else
		H.ma = GCthresh;
	for(i = 0; i < Qnkind; i++)
		if(qs[i].free1)
			H.guards[i] = mkguard();
	H.disable = 0;
}

void
finimem()
{
	freeht(segtab);
	// FIXME: release all segments
}