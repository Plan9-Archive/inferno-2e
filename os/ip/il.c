#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"ip.h"

enum				/* Connection state */
{
	Ilclosed,
	Ilsyncer,
	Ilsyncee,
	Ilestablished,
	Illistening,
	Ilclosing,
	Ilopening,		/* only for file server */
};

char	*ilstates[] = 
{ 
	"Closed",
	"Syncer",
	"Syncee",
	"Established",
	"Listening",
	"Closing",
	"Opening",		/* only for file server */
};

enum				/* Packet types */
{
	Ilsync,
	Ildata,
	Ildataquery,
	Ilack,
	Ilquery,
	Ilstate,
	Ilclose,
};

char	*iltype[] = 
{	
	"sync",
	"data",
	"dataquery",
	"ack",
	"query",
	"state",
	"close" 
};

enum
{
	Seconds		= 1000,
	Iltickms 	= 50,		/* time base */
	AckDelay	= 2*Iltickms,	/* max time twixt message rcvd & ack sent */
	MaxTimeout 	= 30*Seconds,	/* max time between rexmit */
	QueryTime	= 10*Seconds,	/* time between subsequent queries */
	DeathTime	= 5*QueryTime,

	MaxRexmit 	= 16,		/* max retransmissions before hangup */
	Defaultwin	= 20,

	LogAGain	= 3,
	AGain		= 1<<LogAGain,
	LogDGain	= 2,
	DGain		= 1<<LogDGain,

	DefByteRate	= 100,		/* assume a megabit link */
	DefRtt		= 50,		/* cross country on a great day */
};

enum
{
	Nqt=	8,
};

typedef struct Ilcb Ilcb;
struct Ilcb			/* Control block */
{
	int	state;		/* Connection state */
	Conv	*conv;
	QLock	ackq;		/* Unacknowledged queue */
	Block	*unacked;
	Block	*unackedtail;
	ulong	unackedbytes;
	QLock	outo;		/* Out of order packet queue */
	Block	*outoforder;
	ulong	next;		/* Id of next to send */
	ulong	recvd;		/* Last packet received */
	ulong	acksent;	/* Last packet acked */
	ulong	start;		/* Local start id */
	ulong	rstart;		/* Remote start id */
	int	window;		/* Maximum receive window */
	int	rxquery;	/* number of queries on this connection */
	int	rxtot;		/* number of retransmits on this connection */
	int	rexmit;		/* number of retransmits of *unacked */
	ulong	qt[Nqt+1];	/* state table for query messages */
	int	qtx;		/* ... index into qt */

	/* timers */
	ulong	lastxmit;	/* time of last xmit */
	ulong	lastrecv;	/* time of last recv */
	ulong	timeout;	/* retransmission time for *unacked */
	ulong	acktime;	/* time to send next ack */
	ulong	querytime;	/* time to send next query */

	/* adaptive measurements */
	int	delay;		/* Average of the fixed rtt delay */
	int	rate;		/* Average uchar rate */
	int	mdev;		/* Mean deviation of rtt */
	int	maxrtt;		/* largest rtt seen */
	ulong	rttack;		/* The ack we are waiting for */
	int	rttlen;		/* Length of rttack packet */
	ulong	rttms;	/* Time we issued rttack packet */
};

enum
{
	IL_IPSIZE 	= 20,
	IL_HDRSIZE	= 18,	
	IL_LISTEN	= 0,
	IL_CONNECT	= 1,
	IP_ILPROTO	= 40,
};

typedef struct Ilhdr Ilhdr;
struct Ilhdr
{
	uchar	vihl;		/* Version and header length */
	uchar	tos;		/* Type of service */
	uchar	length[2];	/* packet length */
	uchar	id[2];		/* Identification */
	uchar	frag[2];	/* Fragment information */
	uchar	ttl;		/* Time to live */
	uchar	proto;		/* Protocol */
	uchar	cksum[2];	/* Header checksum */
	uchar	src[4];		/* Ip source */
	uchar	dst[4];		/* Ip destination */
	uchar	ilsum[2];	/* Checksum including header */
	uchar	illen[2];	/* Packet length */
	uchar	iltype;		/* Packet type */
	uchar	ilspec;		/* Special */
	uchar	ilsrc[2];	/* Src port */
	uchar	ildst[2];	/* Dst port */
	uchar	ilid[4];	/* Sequence id */
	uchar	ilack[4];	/* Acked sequence */
};


typedef struct Ilpriv Ilpriv;
struct Ilpriv
{

	/* non-MIB stats */
	ulong		csumerr;		/* checksum errors */
	ulong		hlenerr;		/* header length error */
	ulong		lenerr;			/* short packet */
	ulong		order;			/* out of order */
	ulong		rexmit;			/* retransmissions */
	ulong		dup;
	ulong		dupb;

	Rendez		ilr;

	/* keeping track of the ack kproc */
	int	ackprocstarted;
	QLock	apl;
};

/* state for query/dataquery messages */


void	ilrcvmsg(Conv*, Block*);
void	ilsendctl(Conv*, Ilhdr*, int, ulong, ulong, int);
void	ilackq(Ilcb*, Block*);
void	ilprocess(Conv*, Ilhdr*, Block*);
void	ilpullup(Conv*);
void	ilhangup(Conv*, char*);
void	ilfreeq(Ilcb*);
void	ilrexmit(Ilcb*);
void	ilbackoff(Ilcb*);
void	ilsettimeout(Ilcb*);
char*	ilstart(Conv*, int);
void	ilackproc(void*);
void	iloutoforder(Conv*, Ilhdr*, Block*);
void	iliput(Proto*, uchar*, Block*);
void	iladvise(Proto*, Block*, char*);
int	ilnextqt(Ilcb*);
void	ilcbinit(Ilcb*);
int	later(ulong, ulong, char*);

	int 	ilcksum = 1;
static 	int 	initseq = 25001;
static	char	*etime = "connection timed out";

static char*
ilconnect(Conv *c, char **argv, int argc)
{
	char *e;

	e = Fsstdconnect(c, argv, argc);
	if(e != nil)
		return e;
	return ilstart(c, IL_CONNECT);
}

static int
ilstate(Conv *c, char *state, int n)
{
	Ilcb *ic;

	ic = (Ilcb*)(c->ptcl);
	return snprint(state, n, "%s del %5.5d Br %5.5d md %5.5d una %5.5lud rex %5.5d rxq %5.5d max %5.5d",
		ilstates[ic->state],
		ic->delay>>LogAGain, ic->rate>>LogAGain, ic->mdev>>LogDGain,
		ic->unackedbytes, ic->rxtot, ic->rxquery, ic->maxrtt);
}

static int
ilinuse(Conv *c)
{
	Ilcb *ic;

	ic = (Ilcb*)(c->ptcl);
	return ic->state != Ilclosed;

}

/* called with c locked */
static char*
ilannounce(Conv *c, char **argv, int argc)
{
	char *e;

	e = Fsstdannounce(c, argv, argc);
	if(e != nil)
		return e;
	e = ilstart(c, IL_LISTEN);
	if(e != nil)
		return e;
	Fsconnected(c, nil);

	return nil;
}

static void
ilclose(Conv *c)
{
	Ilcb *ic;

	ic = (Ilcb*)c->ptcl;

	qclose(c->rq);
	qclose(c->wq);
	qclose(c->eq);

	switch(ic->state) {
	case Ilclosing:
	case Ilclosed:
		break;
	case Ilsyncer:
	case Ilsyncee:
	case Ilestablished:
		ic->state = Ilclosing;
		ilsettimeout(ic);
		ilsendctl(c, nil, Ilclose, ic->next, ic->recvd, 0);
		break;
	case Illistening:
		ic->state = Ilclosed;
		ipmove(c->laddr, IPnoaddr);
		c->lport = 0;
		break;
	}
	ilfreeq(ic);
	qunlock(c);
}

void
ilkick(Conv *c, int l)
{
	Ilhdr *ih;
	Ilcb *ic;
	int dlen;
	ulong id, ack;
	Block *bp;
	Fs *f;

	USED(l);

	f = c->p->f;
	ic = (Ilcb*)c->ptcl;

	bp = qget(c->wq);
	if(bp == nil)
		return;

	switch(ic->state) {
	case Ilclosed:
	case Illistening:
	case Ilclosing:
		freeblist(bp);
		qhangup(c->rq, nil);
		return;
	}

	dlen = blocklen(bp);

	/* Make space to fit il & ip */
	bp = padblock(bp, IL_IPSIZE+IL_HDRSIZE);
	ih = (Ilhdr *)(bp->rp);

	/* Ip fields */
	ih->frag[0] = 0;
	ih->frag[1] = 0;
	v6tov4(ih->dst, c->raddr);
	v6tov4(ih->src, c->laddr);
	ih->proto = IP_ILPROTO;

	/* Il fields */
	hnputs(ih->illen, dlen+IL_HDRSIZE);
	hnputs(ih->ilsrc, c->lport);
	hnputs(ih->ildst, c->rport);

	qlock(&ic->ackq);
	id = ic->next++;
	hnputl(ih->ilid, id);
	ack = ic->recvd;
	hnputl(ih->ilack, ack);
	ic->acksent = ack;
	ic->acktime = msec + AckDelay;
	ih->iltype = Ildata;
	ih->ilspec = 0;
	ih->ilsum[0] = 0;
	ih->ilsum[1] = 0;

	/* Checksum of ilheader plus data (not ip & no pseudo header) */
	if(ilcksum)
		hnputs(ih->ilsum, ptclcsum(bp, IL_IPSIZE, dlen+IL_HDRSIZE));

	ilackq(ic, bp);
	qunlock(&ic->ackq);

	/* Start the round trip timer for this packet if the timer is free */
	if(ic->rttack == 0) {
		ic->rttack = id;
		ic->rttms = msec;
		ic->rttlen = dlen + IL_IPSIZE + IL_HDRSIZE;
	}

	if(later(msec, ic->timeout, nil))
		ilsettimeout(ic);
	ipoput(f, bp, 0, c->ttl);
}

static void
ilcreate(Conv *c)
{
	c->rq = qopen(64*1024, 0, 0, c);
	c->wq = qopen(64*1024, 0, 0, 0);
}

int
ilxstats(Proto *il, char *buf, int len)
{
	int n;
	Ilpriv *ipriv;

	ipriv = il->priv;

	n = snprint(buf, len,
		"il: csum %lud hlen %lud len %lud order %lud rexmit %lud",
		ipriv->csumerr, ipriv->hlenerr, ipriv->lenerr, ipriv->order, ipriv->rexmit);
	n += snprint(buf+n, len-n, " dupp %lud dupb %lud\n",
		ipriv->dup, ipriv->dupb);
	return n;
}

void
ilackq(Ilcb *ic, Block *bp)
{
	Block *np;
	int n;

	n = blocklen(bp);

	/* Enqueue a copy on the unacked queue in case this one gets lost */
	np = copyblock(bp, n);
	if(ic->unacked)
		ic->unackedtail->list = np;
	else
		ic->unacked = np;
	ic->unackedtail = np;
	np->list = nil;
	ic->unackedbytes += n;
}

static
void
ilrttcalc(Ilcb *ic, Block *bp)
{
	int rtt, tt, pt, delay, rate;

	/* add in clock resolution hack */
	rtt = (msec + TK2MS(1) - 1) - ic->rttms;
	delay = ic->delay;
	rate = ic->rate;

	/* Guard against zero wrap */
	if(rtt > 120000 || rtt < 0)
		return;

	/* this block had to be transmitted after the one acked so count its size */
	ic->rttlen += blocklen(bp)  + IL_IPSIZE + IL_HDRSIZE;

	if(ic->rttlen < 256){
		/* guess fixed delay as rtt of small packets */
		delay += rtt - (delay>>LogAGain);
		if(delay < AGain)
			delay = AGain;
		ic->delay = delay;
	} else {
		/* if packet took longer than avg rtt delay, recalc rate */
		tt = rtt - (delay>>LogAGain);
		if(tt > 0){
			rate += ic->rttlen/tt - (rate>>LogAGain);
			if(rate < AGain)
				rate = AGain;
			ic->rate = rate;
		}
	}

	/* mdev */
	pt = ic->rttlen/(rate>>LogAGain) + (delay>>LogAGain);
	ic->mdev += abs(rtt-pt) - (ic->mdev>>LogDGain);

	if(rtt > ic->maxrtt)
		ic->maxrtt = rtt;
}

void
ilackto(Ilcb *ic, ulong ackto, Block *bp)
{
	Ilhdr *h;
	ulong id;

	if(ic->rttack == ackto)
		ilrttcalc(ic, bp);

	/* Cancel if we've passed the packet we were interested in */
	if(ic->rttack <= ackto)
		ic->rttack = 0;

	qlock(&ic->ackq);
	while(ic->unacked) {
		h = (Ilhdr *)ic->unacked->rp;
		id = nhgetl(h->ilid);
		if(ackto < id)
			break;

		bp = ic->unacked;
		ic->unacked = bp->list;
		bp->list = nil;
		ic->unackedbytes -= blocklen(bp);
		freeblist(bp);
		ic->rexmit = 0;
		ilsettimeout(ic);
	}
	qunlock(&ic->ackq);
}

void
iliput(Proto *il, uchar*, Block *bp)
{
	char *st;
	Ilcb *ic;
	Ilhdr *ih;
	uchar raddr[IPaddrlen];
	uchar laddr[IPaddrlen];
	ushort sp, dp, csum;
	int plen, illen;
	Conv *s, **p, *new, *spec, *gen;
	Ilpriv *ipriv;

	ipriv = il->priv;

	ih = (Ilhdr *)bp->rp;
	plen = blocklen(bp);
	if(plen < IL_IPSIZE+IL_HDRSIZE){
		netlog(il->f, Logil, "il: hlenerr\n");
		ipriv->hlenerr++;
		goto raise;
	}

	illen = nhgets(ih->illen);
	if(illen+IL_IPSIZE > plen){
		netlog(il->f, Logil, "il: lenerr\n");
		ipriv->lenerr++;
		goto raise;
	}

	sp = nhgets(ih->ildst);
	dp = nhgets(ih->ilsrc);
	v4tov6(raddr, ih->src);

	if((csum = ptclcsum(bp, IL_IPSIZE, illen)) != 0) {
		if(ih->iltype < 0 || ih->iltype > Ilclose)
			st = "?";
		else
			st = iltype[ih->iltype];
		ipriv->csumerr++;
		netlog(il->f, Logil, "il: cksum %ux %ux, pkt(%s id %lud ack %lud %I/%d->%d)\n",
			csum, st, nhgetl(ih->ilid), nhgetl(ih->ilack), raddr, sp, dp);
		goto raise;
	}

	qlock(il);

	for(p = il->conv; *p; p++) {
		s = *p;
		if(s->lport == sp)
		if(s->rport == dp)
		if(ipcmp(s->raddr, raddr) == 0) {
			qlock(s);
			qunlock(il);
			if(waserror()){
				qunlock(s);
				nexterror();
			}
			ilprocess(s, ih, bp);
			qunlock(s);
			poperror();
			return;
		}
	}

	if(ih->iltype != Ilsync){
		qunlock(il);
		if(ih->iltype < 0 || ih->iltype > Ilclose)
			st = "?";
		else
			st = iltype[ih->iltype];
		netlog(il->f, Logil, "il: no channel, pkt(%s id %lud ack %lud %I/%ud->%ud)\n",
			st, nhgetl(ih->ilid), nhgetl(ih->ilack), raddr, sp, dp); 
		goto raise;
	}

	gen = nil;
	spec = nil;
	for(p = il->conv; *p; p++) {
		s = *p;
		ic = (Ilcb*)s->ptcl;
		if(ic->state != Illistening)
			continue;

		if(s->rport == 0 && ipcmp(s->raddr, IPnoaddr) == 0) {
			if(s->lport == sp) {
				spec = s;
				break;
			}
			if(s->lport == 0)
				gen = s;
		}
	}

	if(spec)
		s = spec;
	else
	if(gen)
		s = gen;
	else {
		qunlock(il);
		goto raise;
	}

	v4tov6(laddr, ih->dst);
	new = Fsnewcall(s, raddr, dp, laddr, sp);
	if(new == nil){
		qunlock(il);
		netlog(il->f, Logil, "il: bad newcall %I/%ud->%ud\n", raddr, sp, dp);
		ilsendctl(s, ih, Ilclose, 0, nhgetl(ih->ilid), 0);
		goto raise;
	}

	ic = (Ilcb*)new->ptcl;

	ic->conv = new;
	ic->state = Ilsyncee;
	ilcbinit(ic);
	ic->rstart = nhgetl(ih->ilid);

	qlock(new);
	qunlock(il);
	if(waserror()){
		qunlock(new);
		nexterror();
	}
	ilprocess(new, ih, bp);
	qunlock(new);
	poperror();
	return;

raise:
	freeblist(bp);
	return;
}

void
_ilprocess(Conv *s, Ilhdr *h, Block *bp)
{
	Ilcb *ic;
	ulong id, ack;

	id = nhgetl(h->ilid);
	ack = nhgetl(h->ilack);

	ic = (Ilcb*)s->ptcl;

	ic->lastrecv = msec;
	ic->querytime = msec + QueryTime;

	switch(ic->state) {
	default:
		netlog(s->p->f, Logil, "il: unknown state %d\n", ic->state);
	case Ilclosed:
		freeblist(bp);
		break;
	case Ilsyncer:
		switch(h->iltype) {
		default:
			break;
		case Ilsync:
			if(ack != ic->start)
				ilhangup(s, "connection rejected");
			else {
				ic->recvd = id;
				ic->rstart = id;
				ilsendctl(s, nil, Ilack, ic->next, ic->recvd, 0);
				ic->state = Ilestablished;
				Fsconnected(s, nil);
				ilpullup(s);
			}
			break;
		case Ilclose:
			if(ack == ic->start)
				ilhangup(s, "remote close");
			break;
		}
		freeblist(bp);
		break;
	case Ilsyncee:
		switch(h->iltype) {
		default:
			break;
		case Ilsync:
			if(id != ic->rstart || ack != 0)
				ic->state = Ilclosed;
			else {
				ic->recvd = id;
				ilsendctl(s, nil, Ilsync, ic->start, ic->recvd, 0);
			}
			break;
		case Ilack:
			if(ack == ic->start) {
				ic->state = Ilestablished;
				ilpullup(s);
			}
			break;
		case Ilclose:
			if(ack == ic->start)
				ilhangup(s, "remote close");
			break;
		}
		freeblist(bp);
		break;
	case Ilestablished:
		switch(h->iltype) {
		case Ilsync:
			if(id != ic->rstart)
				ilhangup(s, "remote close");
			else
				ilsendctl(s, nil, Ilack, ic->next, ic->rstart, 0);
			freeblist(bp);	
			break;
		case Ildata:
			ilackto(ic, ack, bp);
			iloutoforder(s, h, bp);
			ilpullup(s);
			break;
		case Ildataquery:
			ilackto(ic, ack, bp);
			iloutoforder(s, h, bp);
			ilpullup(s);
			ilsendctl(s, nil, Ilstate, ic->next, ic->recvd, h->ilspec);
			break;
		case Ilack:
			ilackto(ic, ack, bp);
			freeblist(bp);
			break;
		case Ilquery:
			ilackto(ic, ack, bp);
			ilsendctl(s, nil, Ilstate, ic->next, ic->recvd, h->ilspec);
			freeblist(bp);
			break;
		case Ilstate:
			if(ack >= ic->rttack)
				ic->rttack = 0;
			ilackto(ic, ack, bp);
			if(h->ilspec > Nqt)
				h->ilspec = 0;
			if(ic->qt[h->ilspec] > ack){
				ilrexmit(ic);
				ilsettimeout(ic);
			}
			freeblist(bp);
			break;
		case Ilclose:
			freeblist(bp);
			if(ack < ic->start || ack > ic->next) 
				break;
			ilsendctl(s, nil, Ilclose, ic->next, ic->recvd, 0);
			ic->state = Ilclosing;
			ilsettimeout(ic);
			ilfreeq(ic);
			break;
		}
		break;
	case Illistening:
		freeblist(bp);
		break;
	case Ilclosing:
		switch(h->iltype) {
		case Ilclose:
			ic->recvd = id;
			ilsendctl(s, nil, Ilclose, ic->next, ic->recvd, 0);
			if(ack == ic->next)
				ilhangup(s, nil);
			break;
		default:
			break;
		}
		freeblist(bp);
		break;
	}
}

void
ilrexmit(Ilcb *ic)
{
	Ilhdr *h;
	Block *nb;
	Conv *c;
	ulong id;
	Ilpriv *priv;

	nb = nil;
	qlock(&ic->ackq);
	if(ic->unacked)
		nb = copyblock(ic->unacked, blocklen(ic->unacked));
	qunlock(&ic->ackq);

	if(nb == nil){
		print("ilrexmit: copyblock returns nil\n");
		return;
	}

	h = (Ilhdr*)nb->rp;

	h->iltype = Ildataquery;
	hnputl(h->ilack, ic->recvd);
	h->ilspec = ilnextqt(ic);
	h->ilsum[0] = 0;
	h->ilsum[1] = 0;
	hnputs(h->ilsum, ptclcsum(nb, IL_IPSIZE, nhgets(h->illen)));

	c = ic->conv;
	id = nhgetl(h->ilid);
	netlog(c->p->f, Logil, "il: rexmit %d %ud: %d %d: %i %d/%d\n", id, ic->recvd,
		ic->rexmit, ic->timeout,
		c->raddr, c->lport, c->rport);

	ilbackoff(ic);

	ipoput(c->p->f, nb, 0, ic->conv->ttl);

	/* statistics */
	ic->rxtot++;
	priv = c->p->priv;
	priv->rexmit++;
}

/* DEBUG */
void
ilprocess(Conv *s, Ilhdr *h, Block *bp)
{
	Ilcb *ic;

	ic = (Ilcb*)s->ptcl;

	USED(ic);
	netlog(s->p->f, Logilmsg, "%11s rcv %d/%d snt %d/%d pkt(%s id %d ack %d %d->%d) ",
		ilstates[ic->state],  ic->rstart, ic->recvd, ic->start, 
		ic->next, iltype[h->iltype], nhgetl(h->ilid), 
		nhgetl(h->ilack), nhgets(h->ilsrc), nhgets(h->ildst));

	_ilprocess(s, h, bp);

	netlog(s->p->f, Logilmsg, "%11s rcv %d snt %d\n", ilstates[ic->state], ic->recvd, ic->next);
}

void
ilhangup(Conv *s, char *msg)
{
	Ilcb *ic;
	int callout;

	netlog(s->p->f, Logil, "il: hangup! %I %d/%d: %s\n", s->raddr,
		s->lport, s->rport, msg?msg:"no reason");

	ic = (Ilcb*)s->ptcl;
	callout = ic->state == Ilsyncer;
	ic->state = Ilclosed;

	qhangup(s->rq, msg);
	qhangup(s->wq, msg);

	if(callout)
		Fsconnected(s, msg);
}

void
ilpullup(Conv *s)
{
	Ilcb *ic;
	Ilhdr *oh;
	Block *bp;
	ulong oid, dlen;
	Ilpriv *ipriv;

	ic = (Ilcb*)s->ptcl;
	if(ic->state != Ilestablished)
		return;

	qlock(&ic->outo);
	while(ic->outoforder) {
		bp = ic->outoforder;
		oh = (Ilhdr*)bp->rp;
		oid = nhgetl(oh->ilid);
		if(oid <= ic->recvd) {
			ic->outoforder = bp->list;
			freeblist(bp);
			continue;
		}
		if(oid != ic->recvd+1){
			ipriv = s->p->priv;
			ipriv->order++;
			break;
		}

		ic->recvd = oid;
		ic->outoforder = bp->list;

		bp->list = nil;
		dlen = nhgets(oh->illen)-IL_HDRSIZE;
		bp = trimblock(bp, IL_IPSIZE+IL_HDRSIZE, dlen);
		/*
		 * Upper levels don't know about multiple-block
		 * messages so copy all into one (yick).
		 */
		bp = concatblock(bp);
		if(bp == 0)
			panic("ilpullup");
		bp = packblock(bp);
		if(bp == 0)
			panic("ilpullup2");
		qpassnolim(s->rq, bp);
	}
	qunlock(&ic->outo);
}

void
iloutoforder(Conv *s, Ilhdr *h, Block *bp)
{
	Ilcb *ic;
	uchar *lid;
	Block *f, **l;
	ulong id, newid;
	Ilpriv *ipriv;

	ipriv = s->p->priv;
	ic = (Ilcb*)s->ptcl;
	bp->list = nil;

	id = nhgetl(h->ilid);
	/* Window checks */
	if(id <= ic->recvd || id > ic->recvd+ic->window) {
		netlog(s->p->f, Logil, "il: message outside window %ud <%ud-%ud>: %i %d/%d\n",
			id, ic->recvd, ic->recvd+ic->window, s->raddr, s->lport, s->rport);
		freeblist(bp);
		return;
	}

	/* Packet is acceptable so sort onto receive queue for pullup */
	qlock(&ic->outo);
	if(ic->outoforder == nil)
		ic->outoforder = bp;
	else {
		l = &ic->outoforder;
		for(f = *l; f; f = f->list) {
			lid = ((Ilhdr*)(f->rp))->ilid;
			newid = nhgetl(lid);
			if(id <= newid) {
				if(id == newid) {
					ipriv->dup++;
					ipriv->dupb += blocklen(bp);
					qunlock(&ic->outo);
					freeblist(bp);
					return;
				}
				bp->list = f;
				*l = bp;
				qunlock(&ic->outo);
				return;
			}
			l = &f->list;
		}
		*l = bp;
	}
	qunlock(&ic->outo);
}

void
ilsendctl(Conv *ipc, Ilhdr *inih, int type, ulong id, ulong ack, int ilspec)
{
	Ilhdr *ih;
	Ilcb *ic;
	Block *bp;
	int ttl;

	bp = allocb(IL_IPSIZE+IL_HDRSIZE);
	bp->wp += IL_IPSIZE+IL_HDRSIZE;

	ih = (Ilhdr *)(bp->rp);

	/* Ip fields */
	ih->proto = IP_ILPROTO;
	hnputs(ih->illen, IL_HDRSIZE);
	ih->frag[0] = 0;
	ih->frag[1] = 0;
	if(inih) {
		hnputl(ih->dst, nhgetl(inih->src));
		hnputl(ih->src, nhgetl(inih->dst));
		hnputs(ih->ilsrc, nhgets(inih->ildst));
		hnputs(ih->ildst, nhgets(inih->ilsrc));
		hnputl(ih->ilid, nhgetl(inih->ilack));
		hnputl(ih->ilack, nhgetl(inih->ilid));
		ttl = MAXTTL;
	}
	else {
		v6tov4(ih->dst, ipc->raddr);
		v6tov4(ih->src, ipc->laddr);
		hnputs(ih->ilsrc, ipc->lport);
		hnputs(ih->ildst, ipc->rport);
		hnputl(ih->ilid, id);
		hnputl(ih->ilack, ack);
		ic = (Ilcb*)ipc->ptcl;
		ic->acksent = ack;
		ic->acktime = msec;
		ttl = ipc->ttl;
	}
	ih->iltype = type;
	ih->ilspec = ilspec;
	ih->ilsum[0] = 0;
	ih->ilsum[1] = 0;

	if(ilcksum)
		hnputs(ih->ilsum, ptclcsum(bp, IL_IPSIZE, IL_HDRSIZE));

	netlog(ipc->p->f, Logilmsg, "ctl(%s id %d ack %d %d->%d)\n",
		iltype[ih->iltype], nhgetl(ih->ilid), nhgetl(ih->ilack), 
		nhgets(ih->ilsrc), nhgets(ih->ildst));

	ipoput(ipc->p->f, bp, 0, ttl);
}

void
ilsettimeout(Ilcb *ic)
{
	ulong pt;

	pt = (ic->delay>>LogAGain)
		+ ic->unackedbytes/(ic->rate>>LogAGain)
		+ (ic->mdev>>(LogDGain-1))
		+ AckDelay;
	if(pt > MaxTimeout)
		pt = MaxTimeout;
	ic->timeout = msec + pt;
}

void
ilbackoff(Ilcb *ic)
{
	ulong pt;
	int i;

	pt = (ic->delay>>LogAGain)
		+ ic->unackedbytes/(ic->rate>>LogAGain)
		+ (ic->mdev>>(LogDGain-1))
		+ AckDelay;
	for(i = 0; i < ic->rexmit; i++)
		pt = pt + (pt>>1);
	if(pt > MaxTimeout)
		pt = MaxTimeout;
	ic->timeout = msec + pt;

	ic->rexmit++;
}

// complain if two numbers not within an hour of each other
#define Tfuture (1000*60*60)
int
later(ulong t1, ulong t2, char *x)
{
	int dt;

	dt = t1 - t2;
	if(dt > 0) {
		if(x != nil && dt > Tfuture)
			print("%s: way future %d\n", x, dt);
		return 1;
	}
	if(dt < -Tfuture) {
		if(x != nil)
			print("%s: way past %d\n", x, -dt);
		return 1;
	}
	return 0;
}

void
ilackproc(void *x)
{
	Ilcb *ic;
	Conv **s, *p;
	Proto *il;
	Ilpriv *ipriv;

	il = x;
	ipriv = il->priv;

loop:
	tsleep(&ipriv->ilr, return0, 0, Iltickms);
	for(s = il->conv; s && *s; s++) {
		p = *s;
		ic = (Ilcb*)p->ptcl;

		switch(ic->state) {
		case Ilclosed:
		case Illistening:
			break;
		case Ilclosing:
			if(later(msec, ic->timeout, "timeout0")) {
				if(ic->rexmit > MaxRexmit){
					ilhangup(p, nil);
					break;
				}
				ilsendctl(p, nil, Ilclose, ic->next, ic->recvd, 0);
				ilbackoff(ic);
			}
			break;

		case Ilsyncee:
		case Ilsyncer:
			if(later(msec, ic->timeout, "timeout1")) {
				if(ic->rexmit > MaxRexmit){
					ilhangup(p, etime);
					break;
				}
				ilsendctl(p, nil, Ilsync, ic->start, ic->recvd, 0);
				ilbackoff(ic);
			}
			break;

		case Ilestablished:
			if(ic->recvd != ic->acksent)
			if(later(msec, ic->acktime, "acktime"))
				ilsendctl(p, nil, Ilack, ic->next, ic->recvd, 0);

			if(later(msec, ic->querytime, "querytime")){
				if(later(msec, ic->lastrecv+DeathTime, "deathtime")){
					netlog(il->f, Logil, "il: hangup: deathtime\n");
					ilhangup(p, etime);
					break;
				}
				ilsendctl(p, nil, Ilquery, ic->next, ic->recvd, ilnextqt(ic));
				ic->querytime = msec + QueryTime;
			}

			if(ic->unacked != nil)
			if(later(msec, ic->timeout, "timeout2")) {
				if(ic->rexmit > MaxRexmit){
					netlog(il->f, Logil, "il: hangup: too many rexmits\n");
					ilhangup(p, etime);
					break;
				}
				ilsendctl(p, nil, Ilquery, ic->next, ic->recvd, ilnextqt(ic));
				ic->rxquery++;
				ilbackoff(ic);
			}
			break;
		}
	}
	goto loop;
}

void
ilcbinit(Ilcb *ic)
{
	ic->start = nrand(0x1000000);
	ic->next = ic->start+1;
	ic->recvd = 0;
	ic->window = Defaultwin;
	ic->unackedbytes = 0;
	ic->unacked = nil;
	ic->outoforder = nil;
	ic->rexmit = 0;
	ic->rxtot = 0;
	ic->rxquery = 0;
	ic->qtx = 1;

	/* timers */
	ic->delay = DefRtt<<LogAGain;
	ic->mdev = DefRtt<<LogDGain;
	ic->rate = DefByteRate<<LogAGain;
	ic->querytime = msec + QueryTime;
	ic->lastrecv = msec;	/* or we'll timeout right away */
	ilsettimeout(ic);
}

char*
ilstart(Conv *c, int type)
{
	Ilcb *ic;
	Ilpriv *ipriv;
	char kpname[NAMELEN];

	ipriv = c->p->priv;

	if(ipriv->ackprocstarted == 0){
		qlock(&ipriv->apl);
		if(ipriv->ackprocstarted == 0){
			sprint(kpname, "#I%dilack", c->p->f->dev);
			kproc(kpname, ilackproc, c->p);
			ipriv->ackprocstarted = 1;
		}
		qunlock(&ipriv->apl);
	}

	ic = (Ilcb*)c->ptcl;
	ic->conv = c;

	if(ic->state != Ilclosed)
		return nil;

	ilcbinit(ic);

	switch(type) {
	default:
		netlog(c->p->f, Logil, "il: start: type %d\n", type);
		break;
	case IL_LISTEN:
		ic->state = Illistening;
		break;
	case IL_CONNECT:
		ic->state = Ilsyncer;
		ilsendctl(c, nil, Ilsync, ic->start, ic->recvd, 0);
		break;
	}

	return nil;
}

void
ilfreeq(Ilcb *ic)
{
	Block *bp, *next;

	qlock(&ic->ackq);
	for(bp = ic->unacked; bp; bp = next) {
		next = bp->list;
		freeblist(bp);
	}
	ic->unacked = nil;
	qunlock(&ic->ackq);

	qlock(&ic->outo);
	for(bp = ic->outoforder; bp; bp = next) {
		next = bp->list;
		freeblist(bp);
	}
	ic->outoforder = nil;
	qunlock(&ic->outo);
}

void
iladvise(Proto *il, Block *bp, char *msg)
{
	Ilhdr *h;
	Ilcb *ic;		
	uchar source[IPaddrlen], dest[IPaddrlen];
	ushort psource;
	Conv *s, **p;

	h = (Ilhdr*)(bp->rp);

	v4tov6(dest, h->dst);
	v4tov6(source, h->src);
	psource = nhgets(h->ilsrc);


	/* Look for a connection, unfortunately the destination port is missing */
	qlock(il);
	for(p = il->conv; *p; p++) {
		s = *p;
		if(s->lport == psource)
		if(ipcmp(s->laddr, source) == 0)
		if(ipcmp(s->raddr, dest) == 0){
			qunlock(il);
			ic = (Ilcb*)s->ptcl;
			switch(ic->state){
			case Ilsyncer:
				ilhangup(s, msg);
				break;
			}
			freeblist(bp);
			return;
		}
	}
	qunlock(il);
	freeblist(bp);
}

int
ilnextqt(Ilcb *ic)
{
	int x;

	qlock(&ic->ackq);
	x = ic->qtx;
	if(++x > Nqt)
		x = 1;
	ic->qtx = x;
	ic->qt[x] = ic->next-1;	/* highest xmitted packet */
	ic->qt[0] = ic->qt[x];	/* compatibility with old implementations */
	qunlock(&ic->ackq);

	return x;
}

void
ilinit(Fs *f)
{
	Proto *il;

	il = smalloc(sizeof(Proto));
	il->priv = smalloc(sizeof(Ilpriv));
	il->name = "il";
	il->kick = ilkick;
	il->connect = ilconnect;
	il->announce = ilannounce;
	il->state = ilstate;
	il->create = ilcreate;
	il->close = ilclose;
	il->rcv = iliput;
	il->ctl = nil;
	il->advise = iladvise;
	il->stats = ilxstats;
	il->inuse = ilinuse;
	il->ipproto = IP_ILPROTO;
	il->nc = Nchans;
	il->ptclsize = sizeof(Ilcb);
	Fsproto(f, il);
}
