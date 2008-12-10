#ifndef _BISONFLAW_UTIL_H_
#define _BISONFLAW_UTIL_H_
typedef struct HT HT;
HT *mkht();
void freeht(HT *ht);
void hput(HT *ht, char *k, unsigned len, void *v);
void *hget(HT *ht, char *k, unsigned len);
void hforeach(HT *ht, void (*f)(void *u, char *k, void *v), void *u);
unsigned long hnent(HT *ht);
void* hrefval(HT *ht, unsigned long idx);

void fatal(char *fmt, ...) NORETURN;
char* xstrdup(char *s);
char* xstrndup(char *s, unsigned long len);
void* emalloc(size_t size);
void* erealloc(void *p, size_t old, size_t new);
void tvdiff(struct timeval *a, struct timeval *b, struct timeval *c);
char* strnchr(char *s, int c, unsigned long len);
void efree(void *p);
size_t xstrnlen(char *s, size_t max);

#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))

/* misplaced */
char* readfile(char *filename);
Imm xread(int fd, char *p, Imm len);
Imm xwrite(int fd, char *p, Imm len);
void xprintf(char *fmt, ...);
void xvprintf(char *fmt, va_list args);
void msg(char *fmt, ...);
void vmsg(char *fmt, va_list args);

int xisalnum(int c);
int xisalpha(int c);
int xisascii(int c);
int xisblank(int c);
int xiscntrl(int c);
int xisdigit(int c);
int xisgraph(int c);
int xislower(int c);
int xisodigit(int c);
int xisprint(int c);
int xispunct(int c);
int xisspace(int c);
int xisupper(int c);
int xisxdigit(int c);

void xabort(void) NORETURN;

typedef uintptr_t Thread;
void		chanclose(int c);
int		chanreadb(int c, char *b);
int		chanwriteb(int c, char *b);
void		newchan(int *left, int *right);
Thread		newthread(void* (*fn)(void*), void *arg);
void		setproftimer(u32 usec, void(*fn)());
void		threadexit(void *vp);
void		threadinit();
void		threadwait(Thread t);
int		xpopen(Imm argc, char **argv);

extern u64 cqctmeminuse;
extern u64 cqctmemtotal;

#endif /* _BISONFLAW_UTIL_H_ */
