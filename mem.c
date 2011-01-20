#include "sys.h"
#include "util.h"
#include "syscqct.h"

typedef
enum
{
	Mhole,
	Msys,
	Mfree,
	Mdata,
	Mcode,
	Nm,
} Mkind;

char *mkindname[] = {
	"hole",
	"sys",
	"free",
	"data",
	"code",
	"BAD",
};

/* #define to ensure 64-bit constants */
#define Segsize   4096ULL
#define Seguse    (Segsize-sizeof(void*))
#define Segmask   ~(Segsize-1)
#define	GCthresh  10*1024*Segsize
#define Seghunk	  4*1024*Segsize
#define Align     4

/* n must be a power-of-2 */
#define roundup(l,n)   (((uintptr_t)(l)+((n)-1))&~((n)-1))
#define rounddown(l,n) (((uintptr_t)(l))&~((n)-1))

enum
{
	GCradix = 4,
};

typedef
enum
{
	G0,
	G1,
	G2,
	G3,
	Ngen,
	Glock,
	Gstatic,
	Clean=0xff,
	Dirty=0x00,
} Gen;

typedef
enum
{
	Fold = 1,
	Fbig = Fold<<1,
	Foul = Fbig<<1,
} Flag;

typedef struct Seg Seg;
struct Seg
{
	void *a, *e;
	u8 card;
	Pair *p;		/* protected objects */
	u32 nprotect;
	Mkind mt;
	Gen gen;
	Flag flags;
	void *link;
};

typedef
struct Segmap
{
	void *lo, *hi;
	void *free;
	Seg *map;
} Segmap;

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
struct M
{
	void *h, *t;		/* head and tail of segment list */
	void *scan;
	Gen gen;
} M;

typedef
struct Guard
{
	Pair *gd[Ngen];		/* guarded objects: (obj . guardian) pairs  */
} Guard;

typedef
struct Heap
{
	void *d, *c;		/* current data and code segment */
	M m[Nm][Ngen];		/* metatypes */
	u32 g, tg;		/* collect generation and target generation */
	u64 na;			/* bytes allocated since last gc */
	u64 ma;			/* bytes allocated to trigger collect */
	u64 inuse;		/* bytes currently allocated to active segs */
	u64 notinuse;		/* bytes currently allocated to free segs */
	Guard ug;		/* user guard list */
	Guard sg;		/* system guard list */
	Pair *guards[Qnkind];	/* system per-type guardians */
	unsigned disable;
	u32 gctrip, gcsched[Ngen], ingc;
} Heap;

static int freecl(Head*);
static int freefd(Head*);
static int freerec(Head*);
static int freestr(Head*);
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
	[Qtab]	 = { "table",  sizeof(Tab), 1, 0, itertab },
	[Qundef] = { "undef", sizeof(Head), 0, 0, 0 },
	[Qvec]	 = { "vector", sizeof(Vec), 0, freevec, itervec },
	[Qxtn]	 = { "typename", sizeof(Xtypename), 1, 0, iterxtn },
};

static Segmap	segmap;
static Heap	H;
static unsigned	alldbg = 0;

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
	Strmmap *m;
	str = (Str*)hd;
	// printf("freestr(%.*s)\n", (int)str->len, str->s);
	switch(str->skind){
	case Smmap:
		m = (Strmmap*)str;
		xmunmap(m->s, m->mlen);
		break;
	case Smalloc:
	case Sperm:
		fatal("bug");
	}
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
	Tab *t;
	t = (Tab*)hd;
	switch(ictx->n++){
	case 0:
		return (Val*)&t->ht;
	default:
		return GCiterdone;
	}
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
s2a(Seg *s)
{
	u64 o;
	if(s < segmap.map)
		fatal("bug");
	o = s-segmap.map;
	return segmap.lo+o*Segsize;
}

static Seg*
a2s(void *a)
{
	u64 o;
	o = (a-segmap.lo)/Segsize;
	return segmap.map+o;
}

static void*
mapmem(u64 sz)
{
	void *p;
	p = mmap(0, sz, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
	if(p == MAP_FAILED)
		fatal("out of memory");
	if((uintptr_t)p%Segsize)
		fatal("unaligned segment");
	return p;
}

static void
unmapseg(void *a, u32 sz)
{
	if(0 > munmap(a, sz))
		fatal("munmap: %s", strerror(errno));
}

static void
returnsegs()
{
#if 0
	void *q;
	while(H.notinuse > H.inuse){
		if(segfree == 0)
			fatal("bug");
		q = *(void**)segfree;
		unmapseg(segfree, Segsize);
		H.notinuse -= Segsize;
		segfree = q;
	}
#endif
}

static void
segmark(void *p, void *e, Mkind mt)
{
	u64 n;
	Seg *s, *ms, *es;

	p = (void*)rounddown(p, Segsize);
	e = (void*)roundup(e, Segsize);
	
	n = (e-p)/Segsize;
	s = a2s(p);
	es = s+n;
	while(s < es){
		ms = a2s(s);
		if(mt != Msys && ms->mt != Msys)
			fatal("bug");
		s->mt = mt;
		s++;
	}
}

static void
freerange(void *p, void *e)
{
	void *f, **q;

	/* this should be unnecessary */
	p = (void*)rounddown(p, Segsize);
	e = (void*)roundup(e, Segsize);

	segmark(p, e, Mfree);
	f = segmap.free;
	while(p < e){
		q = (void**)p;
		*q = f;
		f = p;
		p += Segsize;
	}
	segmap.free = f;
	H.notinuse += (e-p)/Segsize;  // FIXME!
}

static void
resizesegmap(void *p, void *e)
{
	u64 nseg, onseg;
	Seg *s, *es, *os;
	void *olo, *ohi;

	olo = segmap.lo;
	ohi = segmap.hi;
	onseg = (ohi-olo)/Segsize;

	if(e <= olo)
		segmap.lo = p;
	if(p >= ohi)
		segmap.hi = e;
	nseg = (segmap.hi-segmap.lo)/Segsize;
	if(nseg*sizeof(Seg) >= e-p)
		fatal("resizesegmap: segment table overflow");
	s = p;
	es = (Seg*)roundup(s+nseg, Segsize);

	/* copy old segment table to its offset in new table */
	memcpy(s+(olo-segmap.lo)/Segsize, segmap.map, onseg*sizeof(Seg));

	/* switch maps */
	os = segmap.map;
	segmap.map = s;

	/* mark segment table segments */
	segmark(s, es, Msys);

	/* return segments of old segment table to free list */
	freerange(os, os+onseg);

	/* mark new hole (if any) */
	if(p > ohi)
		segmark(ohi, p, Mhole);
	else if(e < olo)
		segmark(e, olo, Mhole);

	/* add remaining segments of new table to free list */
	freerange(es, e);
}

static void
initsegmap()
{
	u64 sz, nseg;
	void *p, *e, *es;
	Seg *s;
		
	sz = Seghunk;

	/* segment map must fit within some segments */
	nseg = sz/Segsize;
	if(nseg*sizeof(Seg) >= sz)
		fatal("bad segment configuration");

	p = mapmem(sz);
	e = p+sz;
	segmap.lo = p;
	segmap.hi = e;
	segmap.map = segmap.lo;
	segmap.free = 0;

	s = segmap.map;
	es = (void*)roundup(s+nseg, Segsize);
	segmark(s, es, Msys);
	freerange(es, e);
}

static void
growsegmap()
{
	u64 sz;
	void *p, *e;
		
	if(H.ingc)
		fatal("segmap update within gc");
	sz = Seghunk;
	p = mapmem(sz);
	e = p+sz;
	if(e <= segmap.lo || p >= segmap.hi)
		resizesegmap(p, e);
	else
		freerange(p, e);
}

static Seg*
allocseg(Mkind kind)
{
	Seg *s, *ms;
	void *p, **q;

	if(segmap.free == 0){
		growsegmap();
		if(segmap.free == 0)
			fatal("bug");
	}

	p = segmap.free;
	q = (void**)p;
	segmap.free = *q;
	s = a2s(p);
	ms = a2s(s);
	if(ms->mt != Msys)
		fatal("bug");
	s->a = p;
	s->e = p+Seguse;
	s->card = Clean;
	memset(s->a, 0, Segsize);
	if(s->mt != Mfree)
		fatal("bug");
	s->mt = kind;

	/* FIXME: this should be unnecessary */
	s->p = 0;
	s->nprotect = 0;
	s->gen = 0;
	s->flags = 0;
	s->link = 0;

	H.na += Segsize;
	H.notinuse -= Segsize;
	H.inuse += Segsize;

	return s;
}

static Seg*
mkseg(Mkind kind)
{
	return allocseg(kind);
}

static void
freeseg(Seg *s)
{
	void *a, **q;

	s->mt = Mfree;
	a = s2a(s);
	q = (void**)a;
	*q = segmap.free;
	segmap.free = a;
	H.notinuse += Segsize;
	H.inuse -= Segsize;
}

static Seg*
lookseg(void *a)
{
	return a2s(a);
}

static void
mclr(M *m)
{
	m->h = 0;
	m->t = 0;
	m->scan = 0;
}

static void*
minit(M *m, Seg *s)
{
	s->gen = m->gen;
	s->link = 0;
	m->h = s2a(s);
	m->t = s2a(s);
	m->scan = s2a(s);
	return s2a(s);
}

static void*
minsert(M *m, Seg *s)
{
	Seg *t;
	void *p;

	if(m->h == 0)
		return minit(m, s);
	s->gen = m->gen;
	s->link = 0;
	t = a2s(m->t);
	p = s2a(s);
	t->link = p;
	m->t = p;
	return p;
}

#if 0
static u64
guarded()
{
	u32 i;
	u64 m;
	Pair *p;
	m = 0;
	for(i = 0; i < Ngen; i++){
		p = H.sg.gd[i];
		while(p != (Pair*)Xnil){
			m++;
			p = (Pair*)p->cdr;
		}
	}
	for(i = 0; i < Ngen; i++){
		p = H.ug.gd[i];
		while(p != (Pair*)Xnil){
			m++;
			p = (Pair*)p->cdr;
		}
	}
	return m;
}
#endif

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
	s = a2s(H.c);
	if(s->a+sz <= s->e){
		h = s->a;
		s->a += sz;
		memset(h, 0, sz);
		Vsetkind(h, Qcode);
		return h;
	}
	H.c = minsert(&H.m[Mcode][s->gen], mkseg(Mcode));
	goto again;
}

static void*
_mal(u64 sz)
{
	Seg *s;
	void *h;
again:
	s = a2s(H.d);
	if(s->a+sz <= s->e){
		h = s->a;
		s->a += sz;
		memset(h, 0, sz);
		return h;
	}
	H.d = minsert(&H.m[Mdata][s->gen], mkseg(Mdata));
	goto again;
}

Head*
mals(Imm len)
{
	Head *h;
	h = _mal(roundup(len,Align));
	Vsetkind(h, Qstr);
	return h;
}

Head*
mal(Qkind kind)
{
	Head *h;
	h = _mal(qs[kind].sz);
	Vsetkind(h, kind);
	return h;
}

static void*
curaddr(Val v)
{
	if(Vfwd(v))
		return Vfwdaddr(v);
	return v;
}

static u32
qsz(Head *h)
{
	Str *s;
	switch(Vkind(h)){
	case Qstr:
		s = (Str*)h;
		switch(s->skind){
		case Smalloc:
			return sizeof(Str)+roundup(s->len,Align);
		case Smmap:
			return sizeof(Strmmap);
		case Sperm:
			return sizeof(Strperm);
		}
		fatal("bug");
	default:
		return qs[Vkind(h)].sz;
	}
	fatal("bug");
}

static u8
copy(Val *v)
{
	Head *h;
	Seg *s;
	Imm sz;
	Head *nh;
	unsigned dbg = alldbg;

	h = *v;
	if(h == 0)
		return Clean;
	if((uintptr_t)h&1)
		return Clean; // stack immediate
	if(Vfwd(h)){
		if(dbg)printf("copy: read fwd %p -> %p\n",
			      h, (void*)Vfwdaddr(h));
		*v = Vfwdaddr(h);
		/* it may have been moved to an older
		   generation.  so we check, in order
		   that we might get a more accurate
		   dirty card for the segment
		   containing V (and thus maybe fewer
		   scans later).  but (FIXME) it might
		   cost more to look up the
		   generation. */
		s = lookseg(*v);
		if(s->flags&Foul)
			fatal("wtf2");
		return s->gen;
	}
	if(Vprot(h)){
		if(dbg)printf("copy: object %p is protected\n", h);
		return Glock; // protected objects do not move
	}
	s = lookseg(h);
	if(s->flags&Foul)
		fatal("wtf3 seg=%p a=%p (%s)", s, h, mkindname[s->mt]);
	if((s->flags&Fold) == 0){
		if(dbg)printf("copy: object %p not in from space (gen %d)\n",
			      h, s->gen);
		return s->gen; // objects in older generations do not move
	}
	sz = qsz(h);
	if(Vkind(h) == Qcode)
		nh = malcode();
	else if(Vkind(h) == Qstr)
		nh = mals(sz);
	else{
		nh = mal(Vkind(h));
		if(dbg)printf("copy %s %p to %p\n",
			      qs[Vkind(h)].id,
			      h, nh);
	}
	memcpy(nh, h, sz);
	Vsetfwd(h, (uintptr_t)nh);
	if(dbg)printf("set fwd %p -> %p %p (%d)\n",
		    h, Vfwdaddr(h), nh, (int)Vfwd(h));
	*v = nh;
	return H.tg;
}

static u8
scan1(Head *h)
{
	Ictx ictx;
	Head **c;
	u8 min, g;
	unsigned dbg = alldbg;

	min = Clean;
	if(qs[Vkind(h)].iter == 0)
		return min;
	memset(&ictx, 0, sizeof(ictx));
	while(1){
		c = qs[Vkind(h)].iter(h, &ictx);
		if(c == (Val*)GCiterdone)
			break;
		if(dbg)printf("scan1 %p (%s) iter %p %p\n",
			    h, qs[Vkind(h)].id,
			    c, *c);
		g = copy(c);
		if(g < min)
			min = g;
	}
	return min;
}

/* m->scan must always point to a location within the
   current segment to be scanned. */
static unsigned
scan(M *m)
{
	Head *h, **c;
	Seg *s;
	Ictx ictx;
	unsigned dbg = alldbg;

	if(m->scan == 0)
		return 0;
	s = lookseg(m->scan);
	c = 0;
	while(1){
		if(s->mt != Mdata && s->mt != Mcode)
			fatal("bug: mt of seg %p (%p) is %d", s, s2a(s), s->mt);
		while(m->scan < s->a){
			h = m->scan;
			if(dbg)printf("scanning %p (%s)\n", h, qs[Vkind(h)].id);
			if(Vkind(h) == Qstr){
				Str *str;
				str = (Str*)h;
			}
			m->scan += qsz(h);
			if(qs[Vkind(h)].iter == 0)
				continue;
			memset(&ictx, 0, sizeof(ictx));
			while(1){
				c = qs[Vkind(h)].iter(h, &ictx);
				if(c == (Val*)GCiterdone)
					break;
				if(dbg)printf("iter %p (%s) -> %p %p\n",
					    h, qs[Vkind(h)].id,
					    c, *c);
				copy(c);
			}
		}
		if(s->link == 0)
			break;
		m->scan = s->link;
		s = a2s(s->link);
	}

	// approximate indication of whether a copy occurred in this call:
	//    1: maybe
	//    0: definitely not
	return (c != 0); // approximate
}

static void
kleenescan(u32 tg)
{
	unsigned again, i;
	do{
		again = 0;
		for(i = 0; i < Nm; i++)
			again |= scan(&H.m[i][tg]);
	}while(again);
}

static Pair*
lastpair()
{
	return cons(Xnil, Xnil);
}

Pair*
mkguard()
{
	Pair *p;
	p = lastpair();
	return cons(p, p);
}

static void
_instguard(Guard *g, Pair *p)
{
	g->gd[H.tg] = cons(p, g->gd[H.tg]);
}

void
instguard(Pair *p)
{
	_instguard(&H.ug, p);
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
	setcar(x, Xnil);
	setcdr(x, Xnil);
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
	_instguard(&H.sg, cons(o, t));
}

static int
islive(Head *o)
{
	Seg *s;
	if(Vfwd(o))
		return 1;
	if(Vprot(o))
		return 1;
	s = lookseg(o);
	if((s->flags&Fold) == 0)
		return 1;
	return 0;
}

static int
isliveseg(Seg *s)
{
	return (s->mt == Mdata || s->mt == Mcode);
}

static void
updateguards(Guard *g)
{
	Head *phold, *pfinal, *p, *q, *o;
	Head *final, *w;
	u32 i;

	// move guarded objects and guards (and their containing cons) to
	// either pending hold or pending final list
	phold = Xnil;
	pfinal = Xnil;
	for(i = 0; i <= H.g; i++){
		p = (Head*)g->gd[i];
		g->gd[i] = (Pair*)Xnil;
		while(p != Xnil){
			q = cdr(p);
			o = caar(p);
			if(islive(o)){
				// object is accessible
				setcdr(p, phold);
				phold = p;
			}else{
				// object is inaccessible
				setcdr(p, pfinal);
				pfinal = p;
			}
			p = q;
		}
	}

	// move each pending final to final if guard is accessible
	while(1){
		final = Xnil;
		p = pfinal;
		pfinal = Xnil;
		while(p != Xnil){
			q = cdr(p);
			o = cdar(p);
			if(islive(o)){
				// guard is accessible
				setcdr(p, final);
				final = p;
			}else{
				setcdr(p, pfinal);
				pfinal = p;
			}
			p = q;
		}
		if(final == Xnil)
			break;
		w = final;
		while(w != Xnil){
			copy(&caar(w));
			push1guard(caar(w), curaddr(cdar(w)));
			w = cdr(w);
		}
		kleenescan(H.tg);
	}

	// forward pending hold to fresh guarded list
	p = phold;
	while(p != Xnil){
		o = cdar(p);
		if(islive(o))
			_instguard(g, cons(curaddr(caar(p)), curaddr(cdar(p))));
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
gcpoll(VM *vm)
{
	static int ingc;
	if(!H.disable && !ingc && H.na >= H.ma){
		ingc++;
		gc(vm);
		ingc--;
	}
}

void
gcwb(Val v)
{
	Seg *s;
	s = lookseg((Head*)v);
	s->card = Dirty;
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
copystack(VM *vm)
{
	Imm pc, fp, sp, narg, m, i, clx;
	u64 sz, mask;
	Closure *cl;

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

static void
mark1old(Seg *s, u32 g)
{
	if(s->flags&Foul)
		return;
	if(s->gen <= g)
		s->flags |= Fold;
	else if(s->gen == Glock && s->nprotect == 0)
		s->flags |= Fold;
}

static void
markold(u32 g)
{
	Seg *s, *es;
	s = a2s(segmap.lo);
	es = a2s(segmap.hi);
	for( ; s < es; s++)
		if(isliveseg(s))
			mark1old(s, g);
}

static u8
scancard(Seg *s)
{
	void *p;
	u8 min, g;

	if(s->flags&Fold)
		fatal("wtf?");
	min = Clean;
	p = s2a(s);
	while(p < s->a){
		if(!Vdead((Head*)p)){
			g = scan1(p);
			if(g < min)
				min = g;
		}
		p += qsz(p);
	}
	return min;
}

/* FIXME: is the real predicate here that the segment is not old? */
static void
scan1card(Seg *s, u32 g)
{
	u8 sg;
	if(s->flags&Foul)
		return;
	if(s->gen <= g || (s->gen != Glock && s->gen >= Ngen))
		return;
	if(s->gen == Glock)
		return; /* FIXME: for now...need different scan for these */
	if(s->card > g)
		return;
	sg = scancard(s);
	if(sg < s->gen)
		s->card = sg;
	else
		s->card = Clean;
}

static void
scancards(u32 g)
{
	Seg *s, *es;
	s = a2s(segmap.lo);
	es = a2s(segmap.hi);
	for( ; s < es; s++)
		if(isliveseg(s))
			scan1card(s, g);
}

static void
scan1locked(Seg *s)
{
	Head *p;
	if(s->flags&Foul)
		return;
	if(s->nprotect == 0)
		return;
	copy((Val*)&s->p); /* retain list of locked objects! */
	p = (Head*)s->p;
	while(p){
		scan1(car(p));
		p = cdr(p);
	}
}

static void
scanlocked()
{
	Seg *s, *es;
	s = a2s(segmap.lo);
	es = a2s(segmap.hi);
	for( ; s < es; s++)
		if(isliveseg(s))
			scan1locked(s);
}

static void
promote1locked(Seg *s)
{
	if(s->flags&Foul)
		return;
	if(s->nprotect == 0)
		return;
	s->gen = Glock;
	s->flags &= ~Fold;
}

static void
promotelocked()
{
	Seg *s, *es;
	s = a2s(segmap.lo);
	es = a2s(segmap.hi);
	for( ; s < es; s++)
		if(isliveseg(s))
			promote1locked(s);
}

static void
reloccode(Code *c)
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
reloc1(Seg *s, u32 tg)
{
	Code *c;
	Head *h, *p;

	if(s->flags&Foul)
		return;
	if(s->mt != Mcode)
		return;
	if(s->gen == tg){
		c = s2a(s);
		while((void*)c < s->a){
			if(!Vdead((Head*)c))
				reloccode(c);
			c++;
		}
		return;
	}
	if(s->gen == Glock){
		p = (Head*)s->p;
		while(p){
			h = car(p);
			if(!Vdead(h))
				reloccode((Code*)h);
			p = cdr(p);
		}
		return;
	}
}

static void
reloc(u32 tg)
{
	Seg *s, *es;
	s = a2s(segmap.lo);
	es = a2s(segmap.hi);
	for( ; s < es; s++)
		if(isliveseg(s))
			reloc1(s, tg);
}

static void
recycle1(Seg *s)
{
	if(s->mt != Mdata && s->mt != Mcode)
		return;
	if((s->flags&Fold) == 0)
		return;
	s->flags = Foul;
	freeseg(s);
}

static void
recycle()
{
	Seg *s, *es;
	s = a2s(segmap.lo);
	es = a2s(segmap.hi);
	for( ; s < es; s++)
		if(isliveseg(s))
			recycle1(s);
}

/*
	for generations 0...g copy live data to space for generation tg.
	if tg is g then create a new target space;
        else if tg is g+1 then use existing target space tg;
	else error.
*/
void
_gc(u32 g, u32 tg)
{
	u32 i, m, mt;
	VM **vmp, *vm;
	Head *h;
	unsigned dbg = alldbg;

	H.ingc++;

	if(g != tg && g != tg-1)
		fatal("bug");
	if(tg >= Ngen)
		return; // FIXME: silently do nothing...caller should know
	H.g = g;
	H.tg = tg;
	if(dbg)printf("gc(%u,%u)\n", g, tg);

	markold(g);
	for(i = 0; i <= g; i++)
		for(mt = 0; mt < Nm; mt++)
			mclr(&H.m[mt][i]);

	if(g == tg){
		H.d = minit(&H.m[Mdata][tg], mkseg(Mdata));
		H.c = minit(&H.m[Mcode][tg], mkseg(Mcode));
	}else{
		if(H.m[Mdata][tg].h)
			H.d = H.m[Mdata][tg].t;
		else
			H.d = minit(&H.m[Mdata][tg], mkseg(Mdata));
		if(H.m[Mcode][tg].h)
			H.c = H.m[Mcode][tg].t;
		else
			H.c = minit(&H.m[Mcode][tg], mkseg(Mcode));
	}

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
	if(dbg)printf("copied vm roots\n");

	// add per-type guards as roots
	for(i = 0; i < Qnkind; i++)
		copy((Val*)&H.guards[i]);
	if(dbg)printf("copied guard roots\n");

	scancards(g);
	if(dbg)printf("scanned cards\n");
	scanlocked();
	kleenescan(tg);
	if(dbg)printf("re-scanned tg data (after prot)\n");

	updateguards(&H.ug);
	updateguards(&H.sg);
	if(dbg)printf("did updateguards\n");

	promotelocked();
	reloc(tg);
	if(dbg)printf("did reloc\n");

	// call built-in finalizers
	for(i = 0; i < Qnkind; i++)
		if(H.guards[i])
			while((h = pop1guard(H.guards[i]))){
				if(dbg)printf("freeing object %p (%s)\n",
					      h, qs[Vkind(h)].id);
				Vsetdead(h, 1);
				qs[i].free1(h);
			}
	recycle();
	if(dbg)printf("did recycle\n");

	if(H.tg != 0){
		H.tg = 0;
		H.d = minit(&H.m[Mdata][H.tg], mkseg(Mdata));
		H.c = minit(&H.m[Mcode][H.tg], mkseg(Mcode));
	}
	H.na = 0;
	if(tg == g && tg == Ngen-1)
		returnsegs();
	if(dbg)printf("returning\n");
	H.ingc--;
}

void
gc(VM *vm)
{
	int i;
	u32 g, tg;

	H.gctrip++;
	for(i = Ngen-1; i >= 0; i--)
		if(H.gctrip%H.gcsched[i] == 0){
			g = i;
			break;
		}
	if(g == Ngen-1)
		tg = g;
	else
		tg = g+1;
	dogc(vm, g, tg);
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
	u32 i, mt, gr;

	initsegmap();
	gr = 1;
	for(i = 0; i < Ngen; i++){
		for(mt = 0; mt < Nm; mt++)
			H.m[mt][i].gen = i;
		H.gcsched[i] = gr;
		gr *= GCradix;
	}
	H.d = minit(&H.m[Mdata][0], mkseg(Mdata));
	H.c = minit(&H.m[Mcode][0], mkseg(Mcode));
	H.na = 0;
	if(gcrate)
		H.ma = gcrate;
	else
		H.ma = GCthresh;

	/* we need nil now to initialize the guarded object lists */
	Xnil = gcprotect(mal(Qnil));

	for(i = 0; i < Qnkind; i++)
		if(qs[i].free1)
			H.guards[i] = mkguard();
	for(i = 0; i < Ngen; i++)
		H.ug.gd[i] = H.sg.gd[i] = (Pair*)Xnil;
	H.disable = 0;
}

void
finimem()
{
	u32 i;

	_gc(Ngen-1, Ngen-1);  // hopefully free all outstanding objects
	for(i = 0; i < Qnkind; i++)
		H.guards[i] = 0;
	_gc(Ngen-1, Ngen-1);  // hopefully free the guardians
	/* FIXME: free all segments */
}
