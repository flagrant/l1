#include "sys.h"
#include "util.h"
#include "syscqct.h"

void
cwarn(U *ctx, Expr *e, char *fmt, ...)
{
	va_list args;
	if(e->src.line)
		cprintf(ctx->out,
			"%s:%u: warning: ",
			e->src.filename ? e->src.filename : "<stdin>",
			e->src.line);
	else
		cprintf(ctx->out,
			"<lost-location!>: warning: ");
	va_start(args, fmt);
	cvprintf(ctx->out, fmt, args);
	va_end(args);
}

void
cwarnln(U *ctx, Expr *e, char *fmt, ...)
{
	va_list args;
	if(e->src.line)
		cprintf(ctx->out,
			"%s:%u: warning: ",
			e->src.filename ? e->src.filename : "<stdin>",
			e->src.line);
	else
		cprintf(ctx->out,
			"<lost-location!>: warning: ");
	va_start(args, fmt);
	cvprintf(ctx->out, fmt, args);
	cprintf(ctx->out, "\n");
	va_end(args);
}

void
cerror(U *ctx, Expr *e, char *fmt, ...)
{
	va_list args;
	if(e && e->src.line)
		cprintf(ctx->out,
			"%s:%u: ",
			e->src.filename ? e->src.filename : "<stdin>",
			e->src.line);
	else if(e)
		cprintf(ctx->out, "<lost-location!>: ");
	va_start(args, fmt);
	cvprintf(ctx->out, fmt, args);
	cprintf(ctx->out, "\n");
	va_end(args);
	longjmp(ctx->jmp, 1);
}

void
cposterror(U *ctx, Expr *e, char *fmt, ...)
{
	va_list args;
	if(e->src.line)
		cprintf(ctx->out,
			"%s:%u: ",
			e->src.filename ? e->src.filename : "<stdin>",
			e->src.line);
	else
		cprintf(ctx->out, "<lost-location!>: ");
	va_start(args, fmt);
	cvprintf(ctx->out, fmt, args);
	cprintf(ctx->out, "\n");
	va_end(args);
	ctx->errors++;
}

static Expr*
Z0(unsigned kind)
{
	return newexpr(kind, 0, 0, 0, 0);
}

static Expr*
Z1(unsigned kind, Expr *e1)
{
	return newexpr(kind, e1, 0, 0, 0);
}

static Expr*
Z2(unsigned kind, Expr *e1, Expr *e2)
{
	return newexpr(kind, e1, e2, 0, 0);
}

Expr*
Zif(Expr *cond, Expr *true)
{
	return newexpr(Eif, cond, true, 0, 0);
}

Expr*
Zifelse(Expr *cond, Expr *true, Expr *false)
{
	return newexpr(Eif, cond, true, false, 0);
}

Expr*
Zcons(Expr *hd, Expr *tl)
{
	return Z2(Eelist, hd, tl);
}

Expr*
Zset(Expr *l, Expr *r)
{
	return Z2(Eg, l, r);
}

Expr*
Zret(Expr *e)
{
	return Z1(Eret, e);
}

Expr*
Zsizeof(Expr *e)
{
	return Z1(E_sizeof, e);
}

Expr*
Zxcast(Expr *type, Expr *cval)
{
	return newbinop(Excast, type, cval);
}

Expr*
Zbinop(unsigned op, Expr *x, Expr *y)
{
       return newbinop(op, x, y);
}

Expr*
Zadd(Expr *x, Expr *y)
{
	return Zbinop(Eadd, x, y);
}

Expr*
Zsub(Expr *x, Expr *y)
{
	return Zbinop(Esub, x, y);
}

/* arguments in usual order */
Expr*
Zcall(Expr *fn, unsigned narg, ...)
{
	Expr *e;
	va_list args;

	va_start(args, narg);
	e = nullelist();
	while(narg-- > 0)
		e = Zcons(va_arg(args, Expr*), e);
	va_end(args);
	return Z2(Ecall, fn, e);
}

/* arguments in usual order */
Expr*
Zapply(Expr *fn, Expr *args)
{
	return Z2(Ecall, fn, invert(args));
}

Expr*
Zconsts(char *s)
{
	Expr *e;
	e = newexpr(Econsts, 0, 0, 0, 0);
	e->lits = mklits(s, strlen(s));
	return e;
}

Expr*
Zuint(Imm val)
{
	return mkconst(Vuint, val);
}

Expr*
Zint(Imm val)
{
	return mkconst(Vint, val);
}

Expr*
Znil(void)
{
	return Z0(Enil);
}

Expr*
Zstr(char *s)
{
	Expr *e;
	e = newexpr(Econsts, 0, 0, 0, 0);
	e->lits = mklits(s, strlen(s));
	return e;
}

Expr*
Zcval(Expr *dom, Expr *type, Expr *val)
{
	return newexpr(E_cval, dom, type, val, 0);
}

Expr*
Zref(Expr *dom, Expr *type, Expr *val)
{
	return newexpr(E_ref, dom, type, val, 0);
}

Expr*
Zlocals(unsigned n, ...)
{
	unsigned m;
	va_list args;
	Expr *l;

	l = nullelist();
	va_start(args, n);
	for(m = 0; m < n; m++)
		l = Zcons(doid(va_arg(args, char*)), l);
	va_end(args);

	return invert(l);
}

Expr*
Zlambda(Expr *args, Expr *body)
{
	return newexpr(Elambda, args, body, 0, 0);
}

Expr*
Zlambdn(Expr *args, Expr *body, Expr *name, Expr *spec)
{
	return newexpr(Elambda, args, body, name, spec);
}

Expr*
Zblock(Expr *locs, ...)
{
	Expr *se, *te;
	va_list args;

	te = nullelist();
	va_start(args, locs);
	while(1){
		se = va_arg(args, Expr*);
		if(se == 0)
			break;
		te = Zcons(se, te);
	}
	return newexpr(Eblock, locs, invert(te), 0, 0);
}

Expr*
Zids2strs(Expr *l)
{
	Expr *te;
	te = nullelist();
	while(l->kind == Eelist){
		te = Z2(Eelist, Zconsts(l->e1->id), te);
		l = l->e2;
	}
	return Zapply(G("list"), invert(te));
}

Expr*
Zkon(Val v)
{
	Expr *e;
	e = newexpr(Ekon, 0, 0, 0, 0);
	if(v == 0)
		fatal("bug");
	e->xp = v;
	return e;
}

void
putsrc(Expr *e, Src *src)
{
	Expr *p;

	if(e == 0)
		return;

	if(e->src.line != 0)
		return;
	e->src = *src;
	switch(e->kind){
	case Eelist:
		p = e;
		while(p->kind == Eelist){
			putsrc(p->e1, src);
			p = p->e2;
			if(p->src.line != 0)
				return;
			p->src = *src;
		}
		break;
	default:
		putsrc(e->e1, src);
		putsrc(e->e2, src);
		putsrc(e->e3, src);
		putsrc(e->e4, src);
		break;
	}
}
