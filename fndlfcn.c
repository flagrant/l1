#include "sys.h"
#include "util.h"
#include "syscqct.h"

#include <dlfcn.h>

void
l1_dlopen(VM *vm, Imm argc, Val *argv, Val *rv)
{
	char* name;
	void* handle;
	int mode;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to dlopen");

	//check argument 2 first in order to avoid potential memory leak
	//(str2cstr creates memory that needs to be freed, and checkarg
	//could error out before reaching the free)
	checkarg(vm, argv, 1, Qcval);

	if(Viskind(argv[0], Qstr)) {

		name = str2cstr(valstr(argv[0]));

	}else if(Viskind(argv[0], Qcval)) {

		if(valimm(argv[1]) != 0){
			vmerr(vm,
			      "operand 1 to %s must be 0 if it is a cvalue",
			      vmfnid(vm));
		}
		name = NULL;

	}else{
		vmerr(vm, "operand 1 to %s must be a string or a cvalue",
		      vmfnid(vm));
	}

	mode = (int)valimm(argv[1]);

	handle = dlopen(name, mode);

	if(name != NULL){
		efree(name);
		name = 0;
	}

	// note: we are intentionally returning 64-bit pointers here
	// even on a 32 bit architecture because:
	// - there is no loss of percision on smaller bit-width systems
	// - litdom does not contain 32-bit pointers
	// - there is no other domain that can be guaranteed to be available
	*rv = mkvalcval(litdom,
	                mkctypeptr(mkctypevoid(),
	                           typerep(litdom->ns->base[Vptr])),
	                (uptr)handle);

}

void
l1_dlclose(VM *vm, Imm argc, Val *argv, Val *rv)
{
	void* handle;
	int ret;

	if(argc != 1)
		vmerr(vm, "wrong number of arguments to dlclose");
	checkarg(vm, argv, 0, Qcval);

	handle = (void*)(uptr)valimm(argv[0]);

	ret = dlclose(handle);

	*rv = mkvallitcval(Vint, ret);
}

void
l1_dlsym(VM *vm, Imm argc, Val *argv, Val *rv)
{
	void* handle;
	void* addr;
	Str *ftnnm;
	char *ftn;

	if(argc != 2)
		vmerr(vm, "wrong number of arguments to dlsym");
	checkarg(vm, argv, 0, Qcval);
	checkarg(vm, argv, 1, Qstr);

	handle = (void*)(uptr)valimm(argv[0]);

	ftnnm = valstr(argv[1]);
	ftn = str2cstr(ftnnm);

	addr = dlsym(handle, ftn);

	efree(ftn);

	*rv = mkvalcval(litdom,
	                mkctypeptr(mkctypevoid(),
	                           typerep(litdom->ns->base[Vptr])),
	                (uptr)addr);

}

void
l1_dlerror(VM *vm, Imm argc, Val *argv, Val *rv)
{
	char *ret;

	if(argc != 0)
		vmerr(vm, "wrong number of arguments to dlerror");

	ret = dlerror();

	*rv = mkvalcval(litdom,
			mkctypeptr(mkctypebase(Vchar,
					      typerep(litdom->ns->base[Vptr])),
				   typerep(litdom->ns->base[Vptr])),
			(uptr)ret);
}

void
l1_mkdlfcnns(VM *vm, Imm argc, Val *argv, Val *rv)
{
	int x;
	Vec *inside,*outside;
	Val r;
	Ns *rns,*dns;
	struct { char *name; int val; } modes[] = {
		{ "RTLD_LAZY", RTLD_LAZY},
		{ "RTLD_NOW", RTLD_NOW},
		{ "RTLD_GLOBAL", RTLD_GLOBAL},
		{ "RTLD_LOCAL", RTLD_LOCAL},
		{ "RTLD_NOLOAD", RTLD_NOLOAD},
		{ "RTLD_NODELETE", RTLD_NODELETE},
#	ifdef RTLD_FIRST
		{ "RTLD_FIRST", RTLD_FIRST},
#	endif
	  { NULL, 0 },
	};
	struct { char *name; void *val; } handles[]={
	  { "RTLD_DEFAULT", RTLD_DEFAULT },
	  { "RTLD_NEXT", RTLD_NEXT },
#	ifdef RTLD_SELF
	  { "RTLD_SELF", RTLD_SELF },
#	endif
#	ifdef RTLD_MAIN_ONLY
	  { "RTLD_MAIN_ONLY", RTLD_MAIN_ONLY },
#	endif
	  { NULL, 0 },
	};
	Ctype *em,*eh;
	Tab *tt,*st;

	if(argc != 0)
		vmerr(vm, "wrong number of arguments to mkdlfcnns");

	r=myrootns(vm->top);
	rns=valns(r);

	for(x=0;modes[x].name;x++) {}
        if(x) {
	        outside=mkvec(x);
		for(x=0;modes[x].name;x++) {
			inside=mkvec(2);
			_vecset(inside,0,mkvalcid(mkcid0(modes[x].name)));
			_vecset(inside,1,mkvallitcval(Vuint, modes[x].val));
			_vecset(outside,x,mkvalvec(inside));
		}
        } else {
		outside=mkvec(0);
        }

	em = mkctypeenum(mkcid0("dlfcn_modes"),rns->base[Vuint],outside);

	for(x=0;handles[x].name;x++) {}
        if(x) {
		outside=mkvec(x);
		for(x=0;handles[x].name;x++) {
			inside=mkvec(2);
			_vecset(inside,0,mkvalcid(mkcid0(handles[x].name)));
			_vecset(inside,1,mkvallitcval(Vulong,
						(unsigned long)handles[x].val));
			_vecset(outside,x,mkvalvec(inside));
		}
        } else {
		outside=mkvec(0);
        }

	eh = mkctypeenum(mkcid0("dlfcn_handles"),
				mkctypeptr(mkctypebase(Vchar,
					       typerep(rns->base[Vptr])),
						   typerep(rns->base[Vptr])),
				outside);

	tt=mktab();
	tabput(tt, mkvalctype(ctypename(em)), mkvalctype(em));
	tabput(tt, mkvalctype(ctypename(eh)), mkvalctype(eh));

	st=mktab();

	dns = mknsraw(vm, rns, tt, st, mkstr0("dlfcnns"));

	*rv = mkvalns(dns);
}

void
fndlfcn(Env env)
{
	FN(dlopen);
	FN(dlsym);
	FN(dlclose);
	FN(dlerror);
	FN(mkdlfcnns);
}

// vim:ts=8:sw=8:noet
