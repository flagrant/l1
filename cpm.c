#include "sys.h"
#include "util.h"
#include "syscqct.h"

/* expand (Ecall (Edot (Eid O) (Eid I)) ( ARG ...)) */
static Expr*
expandm(Expr *e)
{
	char *mid;
	Expr *te;
	Expr *o, *m, *a, *p;

	if(e == 0)
		return 0;
	switch(e->kind){
	case Ecall:
		if(e->e1->kind != Edot)
			return e;
		if(e->e1->e1->kind != Eid)
			return e;
		if(e->e1->e2->kind != Eid)
			return e;
		mid = idsym(e->e1->e2);
		if(!strcmp(mid, "ns") || !strcmp(mid, "as"))
			/* handled by cpa */
			return e;
		o = e->e1->e1;
		m = e->e1->e2;
		a = expandm(e->e2);
		te = Zifelse(Zcall(G("isrec"), 1, o),
			     Zapply(Zcall(Zcall(G("tablook"), 2,
						Zcall(G("rdgettab"), 1,
						      Zcall(G("rdof"), 1, o)),
						Zid2sym(m)),
					  1, o),
				    copyexpr(a)),
			     Zapply(G("callmethodx"),
				    Zcons(o, Zcons(Zstr(idsym(m)), a))));
		putsrc(te, e->src);
		return te;
	case Eelist:
		p = e;
		while(p->kind == Eelist){
			sete1(p, expandm(p->e1));
			p = p->e2;
		}
		return e;
	default:
		sete1(e, expandm(e->e1));
		sete2(e, expandm(e->e2));
		sete3(e, expandm(e->e3));
		sete4(e, expandm(e->e4));
		return e;
	}
}

static Expr*
compilem(U *ctx, Expr* e)
{
	return expandm(e);
}

Expr*
docompilem(U *ctx, Expr *e)
{
	if(e->kind != Eelist && e->kind != Enull)
		fatal("bug");
	if(setjmp(ctx->jmp) != 0)
		return 0;
	compilem(ctx, e);
	return e;
}
