#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

/* Centronix parallel (printer) port */

/* base addresses */
static int lptbase[] = {
	0x378,	/* lpt1 */
	0x3bc,	/* lpt2 */
	0x278	/* lpt3 (sic) */
};
#define NDEV	(sizeof lptbase/sizeof lptbase[0])

/* offsets, and bits in the registers */
enum
{
	/* data latch register */
	Qdlr=		0x0,
	/* printer status register */
	Qpsr=		0x1,
	Fnotbusy=	0x80,
	Fack=		0x40,
	Fpe=		0x20,
	Fselect=	0x10,
	Fnoerror=	0x08,
	/* printer control register */
	Qpcr=		0x2,
	Fie=		0x10,
	Fselectin=	0x08,
	Finitbar=	0x04,
	Faf=		0x02,
	Fstrobe=	0x01,
	/* fake `data register' */
	Qdata=		0x3,
};

static int	lptready(void*);
static void	outch(int, int);
static void	lptintr(Ureg*, void*);

static Rendez	lptrendez;

Dirtab lptdir[]={
	"dlr",		{Qdlr},		1,		0666,
	"psr",		{Qpsr},		5,		0444,
	"pcr",		{Qpcr},		0,		0222,
	"data",		{Qdata},	0,		0222,
};

static int
lptgen(Chan *c, Dirtab *tab, int ntab, int i, Dir *dp)
{
	Qid qid;
	char name[NAMELEN];

	if(tab==0 || i>=ntab)
		return -1;
	tab += i;
	qid = tab->qid;
	if(qid.path < Qdata)
		qid.path += lptbase[c->dev];
	qid.vers = c->dev;
	sprint(name, "lpt%lud%s", c->dev+1, tab->name);
	devdir(c, qid, name, tab->length, eve, tab->perm, dp);
	return 1;
}

static Chan*
lptattach(char *spec)
{
	Chan *c;
	int i  = (spec && *spec) ? strtol(spec, 0, 0) : 1;
	static int set;

	if(!set){
		outb(lptbase[i-1]+Qpcr, 0);	/* turn off interrupts */
		set = 1;
		intrenable(VectorLPT, lptintr, 0, BUSUNKNOWN);
	}
	if(i < 1 || i > NDEV)
		error(Ebadarg);
	c = devattach('P', spec);
	c->dev = i-1;
	return c;
}

static int
lptwalk(Chan *c, char *name)
{
	return devwalk(c, name, lptdir, nelem(lptdir), lptgen);
}

static void
lptstat(Chan *c, char *dp)
{
	devstat(c, dp, lptdir, nelem(lptdir), lptgen);
}

static Chan*
lptopen(Chan *c, int omode)
{
	return devopen(c, omode, lptdir, nelem(lptdir), lptgen);
}

static void
lptclose(Chan *)
{
}

static long
lptread(Chan *c, void *a, long n, ulong)
{
	char str[16];
	int size;
	ulong o;

	if(c->qid.path == CHDIR)
		return devdirread(c, a, n, lptdir, nelem(lptdir), lptgen);
	size = sprint(str, "0x%2.2ux\n", inb(c->qid.path));
	o = c->offset;
	if(o >= size)
		return 0;
	if(o+n > size)
		n = size-c->offset;
	memmove(a, str+o, n);
	return n;
}

static long
lptwrite(Chan *c, void *a, long n, ulong)
{
	char str[16], *p;
	long base, k;

	if(n <= 0)
		return 0;
	if(c->qid.path != Qdata){
		if(n > sizeof str-1)
			n = sizeof str-1;
		memmove(str, a, n);
		str[n] = 0;
		outb(c->qid.path, strtoul(str, 0, 0));
		return n;
	}
	p = a;
	k = n;
	base = lptbase[c->dev];
	if(waserror()){
		outb(base+Qpcr, Finitbar);
		nexterror();
	}
	while(--k >= 0)
		outch(base, *p++);
	poperror();
	return n;
}

static void
outch(int base, int c)
{
	int status, tries;

	for(tries = 0;; tries++){
		status = inb(base+Qpsr);
		if(!(status & Fselect) || !(status & Fnoerror))
			error(Eio);
		if(status & Fnotbusy)
			break;
		if(tries > 1000){
			outb(base+Qpcr, Finitbar|Fie);
			tsleep(&lptrendez, lptready, (void *)base, MS2HZ);
		}
	}
	outb(base+Qdlr, c);
	outb(base+Qpcr, Finitbar|Fstrobe);
	outb(base+Qpcr, Finitbar);
}

static int
lptready(void *base)
{
	return inb((int)base+Qpsr)&Fnotbusy;
}

static void
lptintr(Ureg *, void *)
{
	wakeup(&lptrendez);
}

Dev lptdevtab = {
	'P',
	"lpt",

	devreset,
	devinit,
	lptattach,
	devdetach,
	devclone,
	lptwalk,
	lptstat,
	lptopen,
	devcreate,
	lptclose,
	lptread,
	devbread,
	lptwrite,
	devbwrite,
	devremove,
	devwstat,
};
