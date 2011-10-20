#include "sys.h"
#include "util.h"
#include "syscqct.h"

static void
newlocal(Expr *s, Expr *id)
{
	Expr *b;
	if(s->kind != Escope)
		bug();
	b = s->e1;
	if(b->kind != Eblock)
		bug();
	sete1(b, putsrc(Zcons(id, b->e1), b->e1->src));
}

static Expr*
expandrec(U *ctx, Expr *e, Expr *s)
{
	char *id, *is;
	unsigned len;
	Expr *p;
	Xenv *xe;

	xe = mkxenv(0);
	p = e->e2;
	while(p->kind == Eelist){
		id = idsym(p->e1);
		if(xenvlook(xe, id)){
			freexenv(xe);
			cerror(ctx, e,
			       "record has multiple bindings for %s",
			       id);
		}
		xenvbind(xe, id, id);
		p = p->e2;
	}
	freexenv(xe);

	id = idsym(e->e1);
	len = 2+strlen(id)+1;
	is = emalloc(len);
	snprint(is, len, "is%s", id);
	p = Zblock(Zlocals(1, "$rd"),
		   Zset(doid("$rd"),
			Zcall(G("mkrd"), 2,
			      Zstr(id),
			      Zids2syms(e->e2))),
		   Zset(doid(id), Zcall(G("rdmk"),
					1, doid("$rd"))),
		   Zset(doid(is), Zcall(G("rdis"),
					1, doid("$rd"))),
		   doid(id), /* warning-suppressing reference */
		   doid(is), /* warning-suppressing reference */
		   doid("$rd"),
		   NULL);
	if(s){
		newlocal(s, doid(id));
		newlocal(s, doid(is));
	}
	efree(is);
	putsrc(p, e->src);
	return p;
}

static Expr*
record(U *ctx, Expr *e, Expr *s)
{
	Expr *p;

	if(e == 0)
		return e;

	switch(e->kind){
	case Escope:
		sete1(e, record(ctx, e->e1, e));
		return e;
	case Edefrec:
		return expandrec(ctx, e, s);
	case Eelist:
		p = e;
		while(p->kind == Eelist){
			sete1(p, record(ctx, p->e1, s));
			p = p->e2;
		}
		return e;
	default:
		sete1(e, record(ctx, e->e1, s));
		sete2(e, record(ctx, e->e2, s));
		sete3(e, record(ctx, e->e3, s));
		sete4(e, record(ctx, e->e4, s));
		return e;
	}
}

Expr*
docompiler(U *ctx, Expr *e)
{
 	/* expr lists ensure we do not have to return a new root Expr */
	if(e->kind != Eelist && e->kind != Enull)
		fatal("bug");
	if(setjmp(ctx->jmp) != 0)
		return 0;	/* error */
	record(ctx, e, 0);
	return e;
}
