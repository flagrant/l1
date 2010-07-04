#include "sys.h"
#include "util.h"
#include "syscqct.h"

typedef
enum
{
	Mpersist = 1,
	Mmal,
	Mmax,
} Mkind;

enum
{
	Segsize = 4096,
	Segmask = ~(Segsize-1)
};

typedef struct Seg Seg;
struct Seg
{
	void *addr, *scan, *a, *e;
	Mkind kind;
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
struct Heap
{
	Seg *r;			/* head of persist segments */
	Seg *p;			/* current persist segment */

	Seg *t;			/* head of to segments */
	Seg *m;			/* current mal segment */

	Seg *f;			/* head of from segments */
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
	if(n >= Vnbase) /* assume elements at+above nbase are aliases */
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
	return s;
}

static void
freeseg(Seg *s)
{
	hdelp(segtab, s);
	munmap(s->addr, Segsize);
	printf("freeseg %p %p\n", s->addr, s->addr+Segsize);
	efree(s);
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

static Head*
persist(Qkind kind)
{
	Seg *p;
	Head *h;
	u32 sz;
	sz = qs[kind].sz;
again:
	p = H.p;
	if(p->a+sz <= p->e){
		h = p->a;
		p->a += sz;
		Vsetkind(h, kind);
		return h;
	}
	H.p = mkseg(Mpersist);
	p->link = H.p;
	goto again;
}

Head*
mal(Qkind kind)
{
	Seg *m;
	Head *h;
	u32 sz;
	sz = qs[kind].sz;
again:
	m = H.m;
	if(m->a+sz <= m->e){
		h = m->a;
		m->a += sz;
		Vsetkind(h, kind);
		return h;
	}
	H.m = mkseg(Mmal);
	m->link = H.m;
	goto again;
}

void
gcpoll()
{
}

void
gcwb(Val v)
{
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
		printf("read fwd %p -> %p\n", h, (void*)Vfwdaddr(h));
		*v = (Val)Vfwdaddr(h);
		return;
	}
	s = lookseg(h);
	if(s->kind == Mpersist){
		printf("persist %p\n", h);
		return;
	}
	sz = qs[Vkind(h)].sz;
	nh = mal(Vkind(h));
	memcpy(nh, h, sz);
	Vsetfwd(h, (uintptr_t)nh);
	printf("set fwd %p -> %p %p (%d)\n",
	       h, (void*)Vfwdaddr(h), nh, (int)Vfwd(h));
	*v = nh;
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
		memset(&ictx, 0, sizeof(ictx));
		if(qs[Vkind(h)].iter == 0)
			continue;
		while(1){
			c = qs[Vkind(h)].iter(h, &ictx);
			if(c == (Val*)GCiterdone)
				break;
			copy(c);
		}
	}
	s->scan = s->addr;   // reset for persistent segments
	scan(s->link);
}

static void
toproot(void *u, char *k, void *v)
{
	Val *p;
	p = v;
	printf("toproot %20s %p %p\n", k, p, *p);
	copy(v);
}

void
gc()
{
	u32 m;
	VM **vmp, *vm;
	Seg *s;

	H.f = H.t;
	H.t = H.m = mkseg(Mmal);

	vmp = vms;
	while(vmp < vms+Maxvms){
		vm = *vmp++;
		if(vm == 0)
			continue;
		for(m = vm->sp; m < Maxstk; m++)
			copy(&vm->stack[m]);
		hforeach(vm->top->env->var, toproot, 0);
		// FIXME: vm->top->env->rd
		copy(&vm->ac);
		copy(&vm->cl);
	}

	scan(H.r);
	scan(H.t);

	s = H.f;
	while(s){
		H.f = s->link;
		freeseg(s);
		s = H.f;
	}
}

void*
gcprotect(void *v)
{
	Seg *s;
	Head *h, *nh;
	u32 sz;

	h = v;
	s = lookseg(h);
	if(s->kind == Mpersist)
		fatal("gcprotect on already protected object %p", h);
	if(Vfwd(h))
		fatal("bug");
	sz = qs[Vkind(h)].sz;
	nh = persist(Vkind(h));
	printf("gcprotect %s\n", qs[Vkind(nh)].id);
	memcpy(nh, h, sz);
	Vsetfwd(h, (uintptr_t)nh);
	return nh;
}

void*
gcunprotect(void *v)
{
	Seg *s;
	Head *h, *nh;
	u32 sz;

	h = v;
	s = lookseg(h);
	if(s->kind != Mpersist)
		fatal("gcunprotect on already unprotected object %p", h);
	if(Vfwd(h))
		fatal("bug");
	sz = qs[Vkind(h)].sz;
	nh = mal(Vkind(h));
	memcpy(nh, h, sz);
	Vsetfwd(h, (uintptr_t)nh);
	return nh;
}

void
initmem()
{
	segtab = mkhtp();
	H.t = H.m = mkseg(Mmal);
	H.r = H.p = mkseg(Mpersist);
}

void
finimem()
{
	freeht(segtab);
}
