#include "sys.h"
#include "util.h"
#include "syscqct.h"

static Expr* compile_rval(U *ctx, Expr *e, unsigned lfree);
static Expr* compile0(U *ctx, Expr* e);

static int
islval(Expr *e)
{
	switch(e->kind){
	case Etick:
		return 1;
	case Ederef:
		return 1;
	case Ecast:
		return islval(e->e2);
	case Edot:
		return islval(e->e1);
	default:
		return 0;
	}
}

static Expr*
rvalblock(Expr *body, unsigned lfree)
{
	Expr *e;
	if(lfree)
		e = Zlocals(1, "$val");
	else
		e = Zlocals(4, "$val", "$dom", "$type", "$addr");
	return newexpr(Eblock, e, body, 0, 0);
}

static Expr*
lvalblock(Expr *body)
{
	Expr *e;
	e = Zlocals(1, "$tmp");
	return newexpr(Eblock, e, body, 0, 0);
}

static Expr*
compile_lval(U *ctx, Expr *e, int needaddr)
{
	Expr *se, *te, *dom;
	Type *t;
	Decl *d;

	if(e == 0)
		return 0;

	switch(e->kind){
	case Ecast:
		te = nullelist();

		// compile lvalue reference to expression,
		// using dom, type bindings
		se = compile_lval(ctx, e->e2, needaddr);
		te = Zcons(se, te);

		// clobber type with cast operand 
		d = e->e1->xp;
		t = d->type;
		if(t->dom)
			dom = doid(t->dom);
		else
			dom = doid("$dom");

		se = Zblock(Zlocals(1, "$tn"),
			    Zset(doid("$tn"), gentypename(t, compile0, ctx)),
			    Zset(doid("$type"),
				 Zcall(doid("%looktype"), 2,
				       dom, doid("$tn"))),
			    Zif(Zcall(doid("%isnil"), 1, doid("$type")),
				Zcall(doid("%error"), 2,
				      Zconsts("undefined type: %t"),
				      doid("$tn"))),
			    NULL);
		te = Zcons(se, te);

		e->e2 = 0;
		freeexpr(e);
		return lvalblock(invert(te));
	case Etick:
		te = nullelist();

		// $dom = dom;
		se = Zset(doid("$dom"), e->e1);
		te = Zcons(se, te);

		// $tmp = nslooksym(domns($dom))($dom,sym)
		se = Zcall(doid("%nsof"), 1, doid("$dom"));
		se = Zcall(doid("%nslooksym"), 1, se);
		se = Zcall(se, 2, doid("$dom"), Zconsts(e->e2->id));
		se = Zset(doid("$tmp"), se);
		te = Zcons(se, te);
		
		// if(isnil($tmp)) error("undefined symbol: %s", sym);
		se = newexpr(Eif,
			     Zcall(doid("%isnil"), 1, doid("$tmp")),
			     Zcall(doid("%error"), 2,
				   Zconsts("undefined symbol: %s"),
				   Zconsts(e->e2->id)),
			     0, 0);
		te = Zcons(se, te);

		// $type = symtype($tmp);
		se = Zset(doid("$type"),
			  Zcall(doid("%symtype"), 1, doid("$tmp")));
		te = Zcons(se, te);

		// $addr = symval($tmp, 2);
		if(needaddr){
			se = Zset(doid("$addr"),
				  Zcall(doid("%symval"), 1, doid("$tmp")));
			te = Zcons(se, te);

			// if(isnil($addr)) error("symbol lacks address: %s");
			se = newexpr(Eif,
				     Zcall(doid("%isnil"), 1, doid("$addr")),
				     Zcall(doid("%error"), 2,
					   Zconsts("symbol lacks address: %s"),
					   Zconsts(e->e2->id)),
				     0, 0);
			te = Zcons(se, te);
		}
		
		freeexpr(e->e2);
		e->e1 = 0;
		e->e2 = 0;
		freeexpr(e);
		return lvalblock(invert(te));
	case Ederef:
		te = nullelist();

		// $tmp = compile_rval(ctx, e->e1);
		if(needaddr || !islval(e->e1)){
			se = Zset(doid("$tmp"), compile_rval(ctx, e->e1, 0));
			te = Zcons(se, te);

			// $type = subtype($typeof($tmp));
			se = Zset(doid("$type"),
				  Zcall(doid("%subtype"), 1,
					Zcall(doid("$typeof"), 1,
					      doid("$tmp"))));
			te = Zcons(se, te);

			// $dom = domof($tmp);
			se = Zset(doid("$dom"),
				  Zcall(doid("%domof"), 1, doid("$tmp")));
			te = Zcons(se, te);

			// $addr = {nsptr($dom)}$tmp
			if(needaddr){
				se = Zset(doid("$addr"),
					  Zxcast(Zcall(doid("%nsptr"), 1,
						       doid("$dom")),
						 doid("$tmp")));
				te = Zcons(se, te);
			}
		}else{
			// compile lvalue reference to pointer,
			// using dom, type bindings
			se = compile_lval(ctx, e->e1, 0);
			te = Zcons(se, te);

			// $type = subtype($type);
			se = Zset(doid("$type"),
				  Zcall(doid("%subtype"), 1, doid("$type")));
			te = Zcons(se, te);
		}
		e->e1 = 0;
		freeexpr(e);
		return lvalblock(invert(te));
	case Edot:
		te = nullelist();

		// compile lvalue reference to containing struct,
		// using dom, type, addr bindings.
		se = compile_lval(ctx, e->e1, needaddr);
		te = Zcons(se, te);
		
		// $tmp = lookfield(type, field);
		se = Zset(doid("$tmp"),
			  Zcall(doid("%lookfield"), 2,
				doid("$type"), Zconsts(e->e2->id)));
		te = Zcons(se, te);

		// if(isnil($tmp)) error("undefined field: %s", sym);
		se = newexpr(Eif,
			     Zcall(doid("%isnil"), 1, doid("$tmp")),
			     Zcall(doid("%error"), 2,
				   Zconsts("undefined field: %s"),
				   Zconsts(e->e2->id)),
			     0, 0);
		te = Zcons(se, te);

		// $type = fieldtype($tmp);
		se = Zset(doid("$type"),
			  Zcall(doid("%fieldtype"), 1, doid("$tmp")));
		te = Zcons(se, te);
				     
		// $addr = $addr + fieldoff($tmp)
		if(needaddr){
			se = Zset(doid("$addr"),
				  Zadd(doid("$addr"),
				       Zcall(doid("%fieldoff"), 1,
					     doid("$tmp"))));
			te = Zcons(se, te);
		}
		
		e->e1 = 0;
		freeexpr(e);
		return lvalblock(invert(te));
	default:
		cerror(ctx, e, "expression is not an lvalue");
	}
}

static Expr*
compile_rval(U *ctx, Expr *e, unsigned lfree)
{
	Expr *se, *te, *p;

	if(e == 0)
		return 0;

	switch(e->kind){
	case Etick:
	case Edot:
	case Ederef:
		te = nullelist();
		se = compile_lval(ctx, e, 1);
		te = Zcons(se, te);
		se = Zcval(doid("$dom"), doid("$type"), doid("$addr"));
		te = Zcons(se, te);
		return rvalblock(invert(te), lfree);
	case Eref:
		te = nullelist();
		se = compile_lval(ctx, e->e1, 1);
		te = Zcons(se, te);
		se = Zref(doid("$dom"), doid("$type"), doid("$addr"));
		te = Zcons(se, te);
		e->e1 = 0;
		freeexpr(e);
		return rvalblock(invert(te), lfree);
	case Eg:
		if(!islval(e->e1)){
			if(e->e1->kind != Eid)
				cerror(ctx, e, "invalid assignment");
			e->e1 = compile_rval(ctx, e->e1, 0);
			e->e2 = compile_rval(ctx, e->e2, 0);
			return e;
		}

		te = nullelist();

		se = compile_lval(ctx, e->e1, 1);
		te = Zcons(se, te);

		se = Zset(doid("$val"), compile_rval(ctx, e->e2, 0));
		te = Zcons(se, te);

		se = Zcall(doid("$put"), 4,
			   doid("$dom"), doid("$addr"), doid("$type"),
			   doid("$val"));
		te = Zcons(se, te);

		e->e1 = 0;
		e->e2 = 0;
		freeexpr(e);
		return rvalblock(invert(te), lfree);
	case Egop:
		if(!islval(e->e1)){
			/* FIXME: if we translate ordinary cval Egop here
			   into equivalent source, do we generate same
			   or similar code as compile.c on Egop? */
			if(e->e1->kind != Eid)
				cerror(ctx, e, "invalid assignment");
			e->e1 = compile_rval(ctx, e->e1, 0);
			e->e2 = compile_rval(ctx, e->e2, 0);
			return e;
		}

		te = nullelist();

		/* reuse lval bindings */
		se = Zset(doid("$val"), compile_rval(ctx, e->e1, 1));
		te = Zcons(se, te);

		se = Zset(doid("$val"),
			  Zbinop(e->op, doid("$val"),
				 compile_rval(ctx, e->e2, 0)));
		te = Zcons(se, te);

		se = Zcall(doid("$put"), 4,
			   doid("$dom"), doid("$addr"), doid("$type"), 
			   doid("$val"));
		te = Zcons(se, te);

		e->e1 = 0;
		e->e2 = 0;
		freeexpr(e);
		return rvalblock(invert(te), lfree);
	case Epostinc:
	case Epostdec:
		if(!islval(e->e1)){
			/* FIXME: if we translate ordinary cval ++ here
			   into equivalent source, do we generate same
			   or similar code as compile.c on ++? */
			if(e->e1->kind != Eid)
				cerror(ctx, e, "invalid assignment");
			e->e1 = compile_rval(ctx, e->e1, 0);
			return e;
		}

		te = nullelist();

		/* reuse lval bindings */
		se = Zset(doid("$val"), compile_rval(ctx, e->e1, 1));
		te = Zcons(se, te);

		se = Zcall(doid("$put"), 4,
			   doid("$dom"), doid("$addr"), doid("$type"),
			   (e->kind == Epostinc
			    ? Zadd(doid("$val"), Zuint(1))
			    : Zsub(doid("$val"), Zuint(1))));
		te = Zcons(se, te);

		se = doid("$val");
		te = Zcons(se, te);

		e->e1 = 0;
		freeexpr(e);
		return rvalblock(invert(te), lfree);
	case Epreinc:
	case Epredec:
		if(!islval(e->e1)){
			/* FIXME: if we translate ordinary cval ++ here
			   into equivalent source, do we generate same
			   or similar code as compile.c on ++? */
			if(e->e1->kind != Eid)
				cerror(ctx, e, "invalid assignment");
			e->e1 = compile_rval(ctx, e->e1, 0);
			return e;
		}

		te = nullelist();

		/* reuse lval bindings */
		se = Zset(doid("$val"), compile_rval(ctx, e->e1, 1));
		te = Zcons(se, te);

		if(e->kind == Epreinc)
			se = Zadd(doid("$val"), Zuint(1));
		else
			se = Zsub(doid("$val"), Zuint(1));
		se = Zset(doid("$val"), se);
		te = Zcons(se, te);

		se = Zcall(doid("$put"), 4,
			   doid("$dom"), doid("$addr"), doid("$type"),
			   doid("$val"));
		te = Zcons(se, te);

		e->e1 = 0;
		freeexpr(e);
		return rvalblock(invert(te), lfree);
	case Esizeofe:
		if(!islval(e->e1)){
			se = Zsizeof(compile_rval(ctx, e->e1, 0));
			e->e1 = 0;
			freeexpr(e);
			return se;
		}

		te = nullelist();

		se = compile_lval(ctx, e->e1, 0);
		te = Zcons(se, te);

		se = Zsizeof(Zcall(doid("$typeof"), 1, doid("$type")));
		te = Zcons(se, te);
		
		e->e1 = 0;
		freeexpr(e);
		return rvalblock(invert(te), lfree);
	case Etypeofe:
		if(!islval(e->e1)){
			se = Zcall(doid("$typeof"), 1,
				   compile_rval(ctx, e->e1, 0));
			e->e1 = 0;
			freeexpr(e);
			return se;
		}

		te = nullelist();

		se = compile_lval(ctx, e->e1, 0);
		te = Zcons(se, te);

		se = Zcall(doid("$typeof"), 1, doid("$type"));
		te = Zcons(se, te);
		
		e->e1 = 0;
		freeexpr(e);
		return rvalblock(invert(te), lfree);
	case Eelist:
		p = e;
		while(p->kind == Eelist){
			p->e1 = compile_rval(ctx, p->e1, 0);
			p = p->e2;
		}
		return e;
	default:
		e->e1 = compile_rval(ctx, e->e1, 0);
		e->e2 = compile_rval(ctx, e->e2, 0);
		e->e3 = compile_rval(ctx, e->e3, 0);
		e->e4 = compile_rval(ctx, e->e4, 0);
		return e;
	}
}

/* expand syntax of various C forms for subsequent stages */
static Expr*
expandc(U *ctx, Expr *e)
{
	Expr *se, *p;

	if(e == 0)
		return e;
	switch(e->kind){
	case Earef: /* for compile_rval */
		/* rewrite: E1[E2] => *(E1+E2) */
		se = newexpr(Ederef, 
			     Zadd(expandc(ctx, e->e1), expandc(ctx, e->e2)),
			     0, 0, 0);
		e->e1 = 0;
		e->e2 = 0;
		freeexpr(e);
		return se;
	case Earrow: /* for compile_rval */
		/* rewrite: E->field => (*E).field */
		se = newexpr(Edot,
			     newexpr(Ederef, expandc(ctx, e->e1), 0, 0, 0),
			     e->e2, 0, 0);
		e->e1 = 0;
		e->e2 = 0;
		freeexpr(e);
		return se;
	case Eswitch: /* for cg */
		/*
		   switch(E){       { @local $tmp;
		   case V1:     =>    switch($tmp = E){
		      ...             case $tmp==V1:
		   }                      ...
                                      }
                                    }
		*/ 
		se = Zblock(Zlocals(1, "$tmp"),
			   newexpr(Eswitch,
				   Zset(doid("$tmp"), expandc(ctx, e->e1)),
				   expandc(ctx, e->e2), 0, 0),
			   NULL);
		e->e1 = 0;
		e->e2 = 0;
		freeexpr(e);
		return se;
	case Ecase:
		se = newexpr(Ecase,
			     Zbinop(Eeq, doid("$tmp"), expandc(ctx, e->e1)),
			     expandc(ctx, e->e2), 0, 0);
		e->e1 = 0;
		e->e2 = 0;
		freeexpr(e);
		return se;
	case Ecomma:
		se = Zblock(nullelist(),
			    expandc(ctx, e->e1),
			    expandc(ctx, e->e2),
			    NULL);
		e->e1 = 0;
		e->e2 = 0;
		freeexpr(e);
		return se;
	case Econd:
		se = Zifelse(expandc(ctx, e->e1),
			     expandc(ctx, e->e2),
			     expandc(ctx, e->e3));
		e->e1 = 0;
		e->e2 = 0;
		e->e3 = 0;
		freeexpr(e);
		return se;
	case Eelist:
		p = e;
		while(p->kind == Eelist){
			p->e1 = expandc(ctx, p->e1);
			p = p->e2;
		}
		return e;
	default:
		e->e1 = expandc(ctx, e->e1);
		e->e2 = expandc(ctx, e->e2);
		e->e3 = expandc(ctx, e->e3);
		e->e4 = expandc(ctx, e->e4);
		return e;
	}
}

static Expr*
expandaref(U *ctx, Expr *e)
{
	Expr *p, *te, *a, *i;

	if(e == 0)
		return e;
	/* FIXME: unify with expanddot */
	switch(e->kind){
	case Eref:
	case Edot:
		if(e->e1->kind == Earef){
			/* E->E1 must be a lvalue, so don't expand it
			   (but do expand its children) */
			e->e1->e1 = expandaref(ctx, e->e1->e1);
			e->e1->e2 = expandaref(ctx, e->e1->e2);
		}else
			e->e1 = expandaref(ctx, e->e1);
		return e;
	case Eg:
		e->e2 = expandaref(ctx, e->e2);
		if(e->e1->kind != Earef)
			return e;
		e->e1->e2 = expandaref(ctx, e->e1->e2);
		if(islval(e->e1->e1))
			return e;
		a = expandaref(ctx, e->e1->e1);
		i = e->e1->e2;
		e->e1->e1 = doid("$a");
		e->e1->e2 = doid("$i");
		te = Zblock(Zlocals(2, "$a", "$i"),
			    Zset(doid("$a"), a),
			    Zset(doid("$i"), i),
			    Zifelse(Zcall(doid("%iscvalue"), 1, doid("$a")),
				    e,
				    Zcall(doid("%cntrput"), 3,
					  doid("$a"),
					  doid("$i"),
					  copyexpr(e->e2))),
			    NULL);
		putsrc(te, &e->src);
		return te;
	case Egop:
		e->e2 = expandaref(ctx, e->e2);
		if(e->e1->kind != Earef)
			return e;
		e->e1->e2 = expandaref(ctx, e->e1->e2);
		if(islval(e->e1->e1))
			return e;
		a = expandaref(ctx, e->e1->e1);
		i = e->e1->e2;
		e->e1->e1 = doid("$a");
		e->e1->e2 = doid("$i");
		te = Zblock(Zlocals(2, "$a", "$i"),
			    Zset(doid("$a"), a),
			    Zset(doid("$i"), i),
			    Zifelse(Zcall(doid("%iscvalue"), 1, doid("$a")),
				    e,
				    Zcall(doid("%cntrput"), 3,
					  doid("$a"),
					  doid("$i"),
					  Zbinop(e->op,
						 Zcall(doid("%cntrget"), 2,
						       doid("$a"), doid("$i")),
						 copyexpr(e->e2)))),
			    NULL);
		putsrc(te, &e->src);
		return te;
	case Epreinc:
	case Epredec:
		if(e->e1->kind != Earef)
			return e;
		e->e1->e2 = expandaref(ctx, e->e1->e2);
		if(islval(e->e1->e1))
			return e;
		a = expandaref(ctx, e->e1->e1);
		i = e->e1->e2;
		e->e1->e1 = doid("$a");
		e->e1->e2 = doid("$i");
		te = Zblock(Zlocals(2, "$a", "$i"),
			    Zset(doid("$a"), a),
			    Zset(doid("$i"), i),
			    Zifelse(Zcall(doid("%iscvalue"), 1, doid("$a")),
				    e,
				    Zcall(doid("%cntrput"), 3,
					  doid("$a"),
					  doid("$i"),
					  Zbinop(e->kind == Epreinc ?
						 Eadd : Esub,
						 Zcall(doid("%cntrget"), 2,
						       doid("$a"), doid("$i")),
						 Zuint(1)))),
			    NULL);
		putsrc(te, &e->src);
		return te;
	case Epostinc:
	case Epostdec:
		if(e->e1->kind != Earef)
			return e;
		e->e1->e2 = expandaref(ctx, e->e1->e2);
		if(islval(e->e1->e1))
			return e;
		a = expandaref(ctx, e->e1->e1);
		i = e->e1->e2;
		e->e1->e1 = doid("$a");
		e->e1->e2 = doid("$i");
		te = Zblock(Zlocals(2, "$a", "$i"),
			    Zset(doid("$a"), a),
			    Zset(doid("$i"), i),
			    Zifelse(Zcall(doid("%iscvalue"), 1, doid("$a")),
				    e,
				    Zblock(Zlocals(1, "$l"),
					   Zset(doid("$l"),
						Zcall(doid("%cntrget"), 2,
						      doid("$a"), doid("$i"))),
					   Zcall(doid("%cntrput"), 3,
						 doid("$a"),
						 doid("$i"),
						 Zbinop(e->kind == Epostinc ?
							Eadd : Esub,
							doid("$l"),
							Zuint(1))),
					   doid("$l"),
					   NULL)),
			    NULL);
		putsrc(te, &e->src);
		return te;
	case Earef:
		e->e2 = expandaref(ctx, e->e2);
		if(islval(e->e1))
			return e;
		a = expandaref(ctx, e->e1);
		i = e->e2;
		e->e1 = doid("$a");
		e->e2 = doid("$i");
		te = Zblock(Zlocals(2, "$a", "$i"),
			    Zset(doid("$a"), a),
			    Zset(doid("$i"), i),
			    Zifelse(Zcall(doid("%iscvalue"), 1, doid("$a")),
				    e,
				    Zcall(doid("%cntrget"), 2,
					  doid("$a"), doid("$i"))),
			    NULL);
		putsrc(te, &e->src);
		return te;
	case Eelist:
		p = e;
		while(p->kind == Eelist){
			p->e1 = expandaref(ctx, p->e1);
			p = p->e2;
		}
		return e;
	default:
		e->e1 = expandaref(ctx, e->e1);
		e->e2 = expandaref(ctx, e->e2);
		e->e3 = expandaref(ctx, e->e3);
		e->e4 = expandaref(ctx, e->e4);
		return e;
	}
}

static Expr*
expanddot(U *ctx, Expr *e)
{
	Expr *te, *se, *o, *p;
	char *id;

	if(e == 0)
		return e;
	switch(e->kind){
	case Eg:
		e->e2 = expanddot(ctx, e->e2);
		if(e->e1->kind != Edot)
			return e;
		if(islval(e->e1->e1))
			return e;
		id = e->e1->e2->id;
		te = Zblock(Zlocals(1, "$o"),
			    Zset(doid("$o"), expanddot(ctx, e->e1->e1)),
			    Zcall(Zcall(doid("%tablook"), 2,
					Zcall(doid("%rdsettab"), 1,
					      Zcall(doid("%recrd"), 1,
						    doid("$o"))),
					Zconsts(id)), 2,
				  doid("$o"), e->e2),
			    NULL);
		e->e1->e1 = 0;
		e->e2 = 0;
		freeexpr(e);
		return te;
	case Egop:
		e->e2 = expanddot(ctx, e->e2);
		if(e->e1->kind != Edot)
			return e;
		if(islval(e->e1->e1))
			return e;
		id = e->e1->e2->id;
		te = Zblock(Zlocals(2, "$o", "$rd"),
			    Zset(doid("$o"), expanddot(ctx, e->e1->e1)),
			    Zset(doid("$rd"), Zcall(doid("%recrd"), 1,
						    doid("$o"))),
			    Zcall(Zcall(doid("%tablook"), 2,
					Zcall(doid("%rdsettab"),
					      1, doid("$rd")),
					Zconsts(id)), 2,
				  doid("$o"),
				  Zbinop(e->op,
					 Zcall(Zcall(doid("%tablook"), 2,
						     Zcall(doid("%rdgettab"),
							   1, doid("$rd")),
						     Zconsts(id)), 1,
					       doid("$o")),
					 e->e2)),
			    NULL);
		e->e1->e1 = 0;
		e->e2 = 0;
		freeexpr(e);
		return te;
	case Epreinc:
	case Epredec:
		if(e->e1->kind != Edot)
			return e;
		if(islval(e->e1->e1))
			return e;
		id = e->e1->e2->id;
		te = Zblock(Zlocals(2, "$o", "$rd"),
			    Zset(doid("$o"), expanddot(ctx, e->e1->e1)),
			    Zset(doid("$rd"), Zcall(doid("%recrd"), 1,
						    doid("$o"))),
			    Zcall(Zcall(doid("%tablook"), 2,
					Zcall(doid("%rdsettab"),
					      1, doid("$rd")),
					Zconsts(id)), 2,
				  doid("$o"),
				  Zbinop(e->kind == Epreinc ? Eadd : Esub,
					 Zcall(Zcall(doid("%tablook"), 2,
						     Zcall(doid("%rdgettab"),
							   1, doid("$rd")),
						     Zconsts(id)), 1,
					       doid("$o")),
					 Zuint(1))),
			    NULL);
		e->e1->e1 = 0;
		freeexpr(e);
		return te;
	case Epostinc:
	case Epostdec:
		if(e->e1->kind != Edot)
			return e;
		if(islval(e->e1->e1))
			return e;
		id = e->e1->e2->id;
		te = Zblock(Zlocals(3, "$o", "$rd", "$l"),
			    Zset(doid("$o"), expanddot(ctx, e->e1->e1)),
			    Zset(doid("$rd"), Zcall(doid("%recrd"), 1,
						    doid("$o"))),
			    Zset(doid("$l"),
				 Zcall(Zcall(doid("%tablook"), 2,
					     Zcall(doid("%rdgettab"), 1,
						   doid("$rd")),
					     Zconsts(id)), 1,
				       doid("$o"))),
			    Zcall(Zcall(doid("%tablook"), 2,
					Zcall(doid("%rdsettab"),
					      1, doid("$rd")),
					Zconsts(id)), 2,
				  doid("$o"),
				  Zbinop(e->kind == Epostinc ? Eadd : Esub,
					 doid("$l"), Zuint(1))),
			    doid("$l"),
			    NULL);
		e->e1->e1 = 0;
		freeexpr(e);
		return te;
	case Edot:
		if(islval(e->e1))
			return e;

		id = e->e2->id; 
		o = Zset(doid("$o"), expanddot(ctx, e->e1));

		/* record accessor case */
		se = Zcall(Zcall(doid("%tablook"), 2,
				 Zcall(doid("%rdgettab"), 1,
				       Zcall(doid("%recrd"), 1, doid("$o"))),
				 Zconsts(id)),
			   1, doid("$o"));

		/* cval/as/ns/dom case */
		if(!strcmp(id, "ns"))
			te = Zcall(doid("%nsof"), 1, doid("$o"));
		else if(!strcmp(id, "as"))
			te = Zcall(doid("%asof"), 1, doid("$o"));
		else
			te = Zlambdn(doid("$args"),
				     Zret(Zcall(doid("%callmethod"),
						3,
						doid("$o"),
						Zconsts(id),
						doid("$args"))),
				     copyexpr(e->e2));

		te = Zblock(Zlocals(1, "$o"),
			    Zifelse(Zcall(doid("%isrec"), 1, o), se, te),
			    NULL);
		e->e1 = 0;
		freeexpr(e);
		return te;
	case Eelist:
		p = e;
		while(p->kind == Eelist){
			p->e1 = expanddot(ctx, p->e1);
			p = p->e2;
		}
		return e;
	default:
		e->e1 = expanddot(ctx, e->e1);
		e->e2 = expanddot(ctx, e->e2);
		e->e3 = expanddot(ctx, e->e3);
		e->e4 = expanddot(ctx, e->e4);
		return e;
	}
}

static int
isemptyblock(Expr *e)
{
	if(e->kind == Eblock && e->e2->kind == Enull)
		return 1;
	else
		return 0;
}

static Expr*
groomc(U *ctx, Expr *e)
{
	Expr *p;

	if(e == 0)
		return e;

	switch(e->kind){
	case Eif:
		e->e1 = groomc(ctx, e->e1);
		if(isemptyblock(e->e2)){
			freeexpr(e->e2);
			e->e2 = newexpr(Enop, 0, 0, 0, 0);
		}else
			e->e2 = groomc(ctx, e->e2);
		if(e->e3 && isemptyblock(e->e3)){
			freeexpr(e->e3);
			e->e3 = newexpr(Enop, 0, 0, 0, 0);
		}else
			e->e3 = groomc(ctx, e->e3);
		return e;
	case Eelist:
		p = e;
		while(p->kind == Eelist){
			p->e1 = groomc(ctx, p->e1);
			p = p->e2;
		}
		return e;
	default:
		e->e1 = groomc(ctx, e->e1);
		e->e2 = groomc(ctx, e->e2);
		e->e3 = groomc(ctx, e->e3);
		e->e4 = groomc(ctx, e->e4);
		return e;
	}
}

static Expr*
compile0(U *ctx, Expr* e)
{
	groomc(ctx, e);
	expandaref(ctx, e);
	expandc(ctx, e);
	expanddot(ctx, e);
	compile_rval(ctx, e, 0);
	return e;
}

int
docompile0(U *ctx, Expr *e)
{
 	/* expr lists ensure we do not have to return a new root Expr */
	if(e->kind != Eelist && e->kind != Enull)
		fatal("bug");
	if(setjmp(ctx->jmp) != 0)
		return -1;	/* error */
	compile0(ctx, e);
	return 0;
}
