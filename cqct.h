extern char cqctflags[];

typedef
enum {
	Qundef = 0,
	Qnil,
	Qnull,
	Qas,
	Qbox,
	Qcl,
	Qcode,
	Qcval,
	Qdom,
	Qfd,
	Qlist,
	Qns,
	Qpair,
	Qrange,
	Qrd,
	Qrec,
	Qstr,
	Qtab,
	Qvec,
	Qxtn,
	Qnkind
} Qkind;


/* base C types */
typedef
enum Cbase {
	Vundef=0,
	Vchar,
	Vshort,
	Vint,
	Vlong,
	Vvlong,
	Vuchar,
	Vushort,
	Vuint,
	Vulong,
	Vuvlong,
	Vfloat,
	Vdouble,
	Vlongdouble,
	Vnbase,
	Vptr = Vnbase,		/* alias for some other base type */
	Vvoid,
	Vnallbase,
} Cbase;

/* type representations */ 
typedef
enum Rkind {
	Rundef,
	Ru08le,
	Ru16le,
	Ru32le,
	Ru64le,
	Rs08le,
	Rs16le,
	Rs32le,
	Rs64le,
	Ru08be,
	Ru16be,
	Ru32be,
	Ru64be,
	Rs08be,
	Rs16be,
	Rs32be,
	Rs64be,
	Rnrep, 
} Rkind;

typedef struct Closure Closure;
typedef struct Toplevel Toplevel;
typedef struct VM VM;
typedef struct Head Head;
typedef struct Head* Val;
typedef struct Heap Heap;

enum
{
	Vkindoff  = 0,
	Vkindbits = 5,
	Vkindmask = (1<<Vkindbits)-1,

	Vcoloroff = 5,
	Vcolorbits = 3,
	Vcolormask = (1<<Vcolorbits)-1,

	Vinrsoff = 8,
	Vinrsbits = 1,
	Vinrsmask = (1<<Vinrsbits)-1,

	Vfinaloff = 9,
	Vfinalbits = 1,
	Vfinalmask = (1<<Vfinalbits)-1,
};

#define Vkind(p)          ((((p)->bits)>>Vkindoff)&Vkindmask)
#define Vsetkind(p, v)	  ((p)->bits = ((p)->bits&~(Vkindmask<<Vkindoff))|(((v)&Vkindmask)<<Vkindoff))

#define Vcolor(p)         ((((p)->bits)>>Vcoloroff)&Vcolormask)
#define Vsetcolor(p, v)	  ((p)->bits = ((p)->bits&~(Vcolormask<<Vcoloroff))|(((v)&Vcolormask)<<Vcoloroff))

#define Vinrs(p)         ((((p)->bits)>>Vinrsoff)&Vinrsmask)
#define Vsetinrs(p, v)	  ((p)->bits = ((p)->bits&~(Vinrsmask<<Vinrsoff))|(((v)&Vinrsmask)<<Vinrsoff))

#define Vfinal(p)         ((((p)->bits)>>Vfinaloff)&Vfinalmask)
#define Vsetfinal(p, v)	  ((p)->bits = ((p)->bits&~(Vfinalmask<<Vfinaloff))|(((v)&Vfinalmask)<<Vfinaloff))


struct Head {
	uint32_t bits;
	Head *alink;
	Head *link;
};

typedef struct Xfd Xfd;
struct Xfd {
	uint64_t (*read)(Xfd*, char*, uint64_t);
	uint64_t (*write)(Xfd*, char*, uint64_t);
	void (*close)(Xfd*);
	int fd;
};

int		cqctcallfn(VM *vm, Val cl, int argc, Val *argv, Val *rv);
int		cqctcallthunk(VM *vm, Val cl, Val *rv);
Val		cqctcompile(char *s, char *src, Toplevel *top, char *argsid);
Val		cqctcstrnval(char *s, uint64_t len);
Val		cqctcstrnvalshared(char *s, uint64_t len);
Val		cqctcstrval(char *s);
Val		cqctcstrvalshared(char *s);
void		cqctenvbind(Toplevel *top, char *name, Val v);
Val		cqctenvlook(Toplevel *top, char *name);
int		cqcteval(VM *vm, char *s, char *src, Val *rv);
int		cqctfaulthook(void (*h)(void), int in);
void		cqctfini(Toplevel *top);
void		cqctfreecstr(char *s);
void		cqctfreevm(VM *vm);
void		cqctgcdisable(VM *vm);
void		cqctgcenable(VM *vm);
void		cqctgcprotect(VM *vm, Val v);
void		cqctgcunprotect(VM *vm, Val v);
void		cqctgcpersist(VM *vm, Val v);
void		cqctgcunpersist(VM *vm, Val v);
Toplevel*	cqctinit(int gct, uint64_t hmax, char **lp,
			 Xfd *in, Xfd *out, Xfd *err);
Val		cqctint8val(int8_t);
Val		cqctint16val(int16_t);
Val		cqctint32val(int32_t);
Val		cqctint64val(int64_t);
void		cqctinterrupt(VM *vm);
uint64_t	cqctlength(Val v);
Val*		cqctlistvals(Val v);
Val		cqctmkfd(Xfd *xfd, char *name);
VM*		cqctmkvm(Toplevel *top);
char*		cqctsprintval(VM *vm, Val v);
Val		cqctuint8val(uint8_t);
Val		cqctuint16val(uint16_t);
Val		cqctuint32val(uint32_t);
Val		cqctuint64val(uint64_t);
Cbase		cqctvalcbase(Val);
int8_t		cqctvalint8(Val);
int16_t		cqctvalint16(Val);
int32_t		cqctvalint32(Val);
int64_t		cqctvalint64(Val);
char*		cqctvalcstr(Val);
uint64_t	cqctvalcstrlen(Val);
char*		cqctvalcstrshared(Val);
uint8_t		cqctvaluint8(Val);	
uint16_t	cqctvaluint16(Val);
uint32_t	cqctvaluint32(Val);
uint64_t	cqctvaluint64(Val);
Val*		cqctvecvals(Val v);
