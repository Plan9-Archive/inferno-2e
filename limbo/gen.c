#include "limbo.h"

static	int	addrmode[Rend] =
{
	/* Rreg */	Afp,
	/* Rmreg */	Amp,
	/* Roff */	Aoff,
	/* Rdesc */	Adesc,
	/* Rconst */	Aimm,
	/* Ralways */	Aerr,
	/* Radr */	Afpind,
	/* Rmadr */	Ampind,
	/* Rcant */	Aerr,
	/* Rpc */	Apc,
	/* Rmpc */	Aerr,
	/* Rareg */	Aerr,
	/* Ramreg */	Aerr,
	/* Raadr */	Aerr,
	/* Ramadr */	Aerr,
};

static	Decl	*wtemp;
static	Decl	*bigtemp;
static	int	ntemp;
static	Node	retnode;
static	Inst	zinst;

	int	*blockstack;
	int	blockdep;
	int	nblocks;
static	int	lenblockstack;

void
genstart(void)
{
	Decl *d;

	d = mkdecl(&nosrc, Dlocal, tint);
	d->sym = enter(".ret", 0);
	d->offset = IBY2WD * REGRET;

	retnode = znode;
	retnode.op = Oname;
	retnode.addable = Rreg;
	retnode.decl = d;
	retnode.ty = tint;

	zinst.op = INOP;
	zinst.sm = Anone;
	zinst.dm = Anone;
	zinst.mm = Anone;

	firstinst = allocmem(sizeof *firstinst);
	*firstinst = zinst;
	lastinst = firstinst;

	blocks = -1;
	blockdep = 0;
	nblocks = 0;
}

/*
 * manage nested control flow blocks
 */
int
pushblock(void)
{
	if(blockdep >= lenblockstack){
		lenblockstack = blockdep + 32;
		blockstack = reallocmem(blockstack, lenblockstack * sizeof *blockstack);
	}
	blockstack[blockdep++] = blocks;
	return blocks = nblocks++;
}

void
repushblock(int b)
{
	blockstack[blockdep++] = blocks;
	blocks = b;
}

void
popblock(void)
{
	blocks = blockstack[blockdep -= 1];
}

void
tinit(void)
{
	wtemp = nil;
	bigtemp = nil;
}

Decl*
tdecls(void)
{
	Decl *d;

	for(d = wtemp; d != nil; d = d->next){
		if(d->tref != 1)
			fatal("temporary %s has %d references", d->sym->name, d->tref-1);
	}

	for(d = bigtemp; d != nil; d = d->next){
		if(d->tref != 1)
			fatal("temporary %s has %d references", d->sym->name, d->tref-1);
	}

	return appdecls(wtemp, bigtemp);
}

Node*
talloc(Node *n, Type *t, Node *nok)
{
	Decl *d, *ok;
	Desc *desc;
	char buf[StrSize];

	ok = nil;
	if(nok != nil)
		ok = nok->decl;
	if(ok == nil || ok->tref == 0 || tattr[ok->ty->kind].big != tattr[t->kind].big)
		ok = nil;
	*n = znode;
	n->op = Oname;
	n->addable = Rreg;
	n->ty = t;
	if(tattr[t->kind].big){
		desc = mktdesc(t);
		if(ok != nil && ok->desc == desc){
			ok->tref++;
			ok->refs++;
			n->decl = ok;
			return n;
		}
		for(d = bigtemp; d != nil; d = d->next){
			if(d->tref == 1 && d->desc == desc){
				d->tref++;
				d->refs++;
				n->decl = d;
				return n;
			}
		}
		d = mkdecl(&nosrc, Dlocal, t);
		d->desc = desc;
		d->tref = 2;
		d->refs = 1;
		n->decl = d;
		seprint(buf, buf+sizeof(buf), ".b%d", ntemp++);
		d->sym = enter(buf, 0);
		d->next = bigtemp;
		bigtemp = d;
		return n;
	}
	if(ok != nil
	&& tattr[ok->ty->kind].isptr == tattr[t->kind].isptr
	&& ok->ty->size == t->size){
		ok->tref++;
		n->decl = ok;
		return n;
	}
	for(d = wtemp; d != nil; d = d->next){
		if(d->tref == 1
		&& tattr[d->ty->kind].isptr == tattr[t->kind].isptr
		&& d->ty->size == t->size){
			d->tref++;
			n->decl = d;
			return n;
		}
	}
	d = mkdecl(&nosrc, Dlocal, t);
	d->tref = 2;
	d->refs = 1;
	n->decl = d;
	seprint(buf, buf+sizeof(buf), ".t%d", ntemp++);
	d->sym = enter(buf, 0);
	d->next = wtemp;
	wtemp = d;
	return n;
}

void
tfree(Node *n)
{
	if(n == nil || n->decl == nil || n->decl->tref == 0)
		return;
	if(n->decl->tref == 1)
		fatal("double free of temporary %s", n->decl->sym->name);
	n->decl->tref--;
}

/*
 * realloc a temporary after it's been freed
 */
Node*
tacquire(Node *n)
{
	if(n == nil || n->decl == nil || n->decl->tref == 0)
		return n;
	if(n->decl->tref != 1)
		fatal("tacquire ref != 1: %d", n->decl->tref);
	n->decl->tref++;
	return n;
}

void
trelease(Node *n)
{
	if(n == nil || n->decl == nil || n->decl->tref == 0)
		return;
	if(n->decl->tref == 1)
		fatal("double release of temporary %s", n->decl->sym->name);
	n->decl->tref--;
}

Inst*
mkinst(void)
{
	Inst *in;

	in = lastinst->next;
	if(in == nil){
		in = allocmem(sizeof *in);
		*in = zinst;
		lastinst->next = in;
	}
	lastinst = in;
	in->block = blocks;
	if(blocks < 0)
		fatal("mkinst no block");
	return in;
}

Inst*
nextinst(void)
{
	Inst *in;

	in = lastinst->next;
	if(in != nil)
		return in;
	in = allocmem(sizeof(*in));
	*in = zinst;
	lastinst->next = in;
	return in;
}

/*
 * allocate a node for returning
 */
Node*
retalloc(Node *n, Node *nn)
{
	if(nn->ty == tnone)
		return nil;
	*n = znode;
	n->op = Oind;
	n->addable = Radr;
	n->left = dupn(1, &n->src, &retnode);
	n->ty = nn->ty;
	return n;
}

Inst*
genrawop(Src *src, int op, Node *s, Node *m, Node *d)
{
	Inst *in;

	in = mkinst();
	in->op = op;
	in->src = *src;
if(in->sm != Anone || in->mm != Anone || in->dm != Anone)
fatal("bogus mkinst in genrawop: %I\n", in);
	if(s != nil){
		in->s = genaddr(s);
		in->sm = addrmode[s->addable];
	}
	if(m != nil){
		in->m = genaddr(m);
		in->mm = addrmode[m->addable];
		if(in->mm == Ampind || in->mm == Afpind)
			fatal("illegal addressing mode in register %n", m);
	}
	if(d != nil){
		in->d = genaddr(d);
		in->dm = addrmode[d->addable];
	}
	return in;
}

Inst*
genop(Src *src, int op, Node *s, Node *m, Node *d)
{
	Inst *in;
	int iop;

	iop = disoptab[op][opind[d->ty->kind]];
	if(iop == 0)
		fatal("can't deal with op %s on %n %n %n in genop", opname[op], s, m, d);
	in = mkinst();
	in->op = iop;
	in->src = *src;
	if(s != nil){
		in->s = genaddr(s);
		in->sm = addrmode[s->addable];
	}
	if(m != nil){
		in->m = genaddr(m);
		in->mm = addrmode[m->addable];
		if(in->mm == Ampind || in->mm == Afpind)
			fatal("illegal addressing mode in register %n", m);
	}
	if(d != nil){
		in->d = genaddr(d);
		in->dm = addrmode[d->addable];
	}
	return in;
}

Inst*
genbra(Src *src, int op, Node *s, Node *m)
{
	Type *t;
	Inst *in;
	int iop;

	t = s->ty;
	if(t == tany)
		t = m->ty;
	iop = disoptab[op][opind[t->kind]];
	if(iop == 0)
		fatal("can't deal with op %s on %n %n in genbra", opname[op], s, m);
	in = mkinst();
	in->op = iop;
	in->src = *src;
	if(s != nil){
		in->s = genaddr(s);
		in->sm = addrmode[s->addable];
	}
	if(m != nil){
		in->m = genaddr(m);
		in->mm = addrmode[m->addable];
		if(in->mm == Ampind || in->mm == Afpind)
			fatal("illegal addressing mode in register %n", m);
	}
	return in;
}

Inst*
genchan(Src *src, Type *mt, Node *d)
{
	Inst *in;
	Desc *td;
	Addr reg;
	int op, regm;

	regm = Anone;
	reg.decl = nil;
	reg.reg = 0;
	reg.offset = 0;
	op = chantab[mt->kind];
	if(op == 0)
		fatal("can't deal with op %d in genchan", mt->kind);

	switch(mt->kind){
	case Tadt:
	case Tadtpick:
	case Ttuple:
		td = mktdesc(mt);
		if(td->nmap != 0){
			op++;		/* sleazy */
			usedesc(td);
			regm = Adesc;
			reg.decl = mt->decl;
		}else{
			regm = Aimm;
			reg.offset = mt->size;
		}
		break;
	}
	in = mkinst();
	in->op = op;
	in->src = *src;
	in->s = reg;
	in->sm = regm;
	if(d != nil){
		in->d = genaddr(d);
		in->dm = addrmode[d->addable];
	}
	return in;
}

Inst*
genmove(Src *src, int how, Type *mt, Node *s, Node *d)
{
	Inst *in;
	Desc *td;
	Addr reg;
	int op, regm;

	regm = Anone;
	reg.decl = nil;
	reg.reg = 0;
	reg.offset = 0;
	op = movetab[how][mt->kind];
	if(op == 0)
		fatal("can't deal with op %d on %n %n in genmove", how, s, d);

	switch(mt->kind){
	case Tadt:
	case Tadtpick:
	case Ttuple:
		if(mt->size == 0 && how == Mas)
			return nil;
		td = mktdesc(mt);
		if(td->nmap != 0){
			op++;		/* sleazy */
			usedesc(td);
			regm = Adesc;
			reg.decl = mt->decl;
		}else{
			regm = Aimm;
			reg.offset = mt->size;
		}
		break;
	}
	in = mkinst();
	in->op = op;
	in->src = *src;
	if(s != nil){
		in->s = genaddr(s);
		in->sm = addrmode[s->addable];
	}
	in->m = reg;
	in->mm = regm;
	if(d != nil){
		in->d = genaddr(d);
		in->dm = addrmode[d->addable];
	}
	return in;
}

void
patch(Inst *b, Inst *dst)
{
	Inst *n;

	for(; b != nil; b = n){
		n = b->branch;
		b->branch = dst;
	}
}

/*
 * follow all possible paths from n,
 * marking reached code, compressing branches, and reclaiming unreached insts
 */
void
reach(Inst *in)
{
	Inst *last;

	foldbranch(in);
	last = in;
	for(in = in->next; in != nil; in = in->next){
		if(!in->reach)
			last->next = in->next;
		else
			last = in;
	}
	lastinst = last;
}

/*
 * follow all possible paths from n,
 * marking reached code, compressing branches, and eliminating tail recursion
 */
void
foldbranch(Inst *in)
{
	Inst *b, *next;
	Label *lab;
	int i, n;

	while(in != nil && !in->reach){
		in->reach = 1;
		if(in->branch != nil)
			while(in->branch->op == IJMP){
				if(in == in->branch || in->branch == in->branch->branch)
					break;
				in->branch = in->branch->branch;
			}
		switch(in->op){
		case IGOTO:
		case ICASE:
		case ICASEC:
			foldbranch(in->d.decl->ty->cse->iwild);
			lab = in->d.decl->ty->cse->labs;
			n = in->d.decl->ty->cse->nlab;
			for(i = 0; i < n; i++)
				foldbranch(lab[i].inst);
			return;
		case IRET:
		case IEXIT:
			return;
		case IJMP:
			b = in->branch;
			switch(b->op){
			case ICASE:
			case ICASEC:
			case IRET:
			case IEXIT:
				next = in->next;
				*in = *b;
				in->next = next;
				b->reach = 1;
				continue;
			}
			foldbranch(in->branch);
			return;
		default:
			if(in->branch != nil)
				foldbranch(in->branch);
			break;
		}

		in = in->next;
	}
}

/*
 * convert the addressable node into an operand
 * see the comment for sumark
 */
Addr
genaddr(Node *n)
{
	Addr a;

	a.reg = 0;
	a.offset = 0;
	a.decl = nil;
	if(n == nil)
		return a;
	switch(n->addable){
	case Rreg:
		if(n->decl != nil)
			a.decl = n->decl;
		else
			a = genaddr(n->left);
		break;
	case Rmreg:
		if(n->decl != nil)
			a.decl = n->decl;
		else
			a = genaddr(n->left);
		break;
	case Rdesc:
		a.decl = n->ty->decl;
		break;
	case Roff:
		a.decl = n->decl;
		break;
	case Rconst:
		a.offset = n->val;
		break;
	case Radr:
		a = genaddr(n->left);
		break;
	case Rmadr:
		a = genaddr(n->left);
		break;
	case Rareg:
	case Ramreg:
		a = genaddr(n->left);
		if(n->op == Oadd)
			a.reg += n->right->val;
		break;
	case Raadr:
	case Ramadr:
		a = genaddr(n->left);
		if(n->op == Oadd)
			a.offset += n->right->val;
		break;
	default:
		fatal("can't deal with %n in genaddr", n);
		break;
	}
	return a;
}

int
sameaddr(Node *n, Node *m)
{
	Addr a, b;

	if(n->addable != m->addable)
		return 0;
	a = genaddr(n);
	b = genaddr(m);
	return a.offset == b.offset && a.reg == b.reg && a.decl == b.decl;
}

long
resolvedesc(Decl *mod, long length, Decl *decls)
{
	Desc *g, *d, *last;
	int descid;

	g = gendesc(mod, length, decls);
	g->used = 0;
	last = nil;
	for(d = descriptors; d != nil; d = d->next){
		if(!d->used){
			if(last != nil)
				last->next = d->next;
			else
				descriptors = d->next;
			continue;
		}
		last = d;
	}

	g->next = descriptors;
	descriptors = g;

	descid = 0;
	for(d = descriptors; d != nil; d = d->next)
		d->id = descid++;
	if(g->id != 0)
		fatal("bad global descriptor id");

	return descid;
}

int
resolvemod(Decl *m)
{
	Decl *id, *d;

	for(id = m->ty->ids; id != nil; id = id->next){
		switch(id->store){
		case Dfn:
			id->iface->pc = id->pc;
			id->iface->desc = id->desc;
			break;
		case Dtype:
			if(id->ty->kind != Tadt)
				break;
			for(d = id->ty->ids; d != nil; d = d->next){
				if(d->store == Dfn){
					d->iface->pc = d->pc;
					d->iface->desc = d->desc;
				}
			}
			break;
		}
	}

	return m->ty->tof->decl->init->val;
}

/*
 * fix up all pc's
 * finalize all data offsets
 * fix up instructions with offsets too large
 */
long
resolvepcs(Inst *inst)
{
	Decl *d;
	Inst *in;
	int op;
	ulong r, off;
	long v, pc;

	pc = 0;
	for(in = inst; in != nil; in = in->next){
		if(!in->reach || in->op == INOP)
			fatal("unreachable pc: %I %d", in, pc);
		d = in->s.decl;
		if(d != nil){
			if(in->sm == Adesc){
				if(d->desc != nil)
					in->s.offset = d->desc->id;
			}else
				in->s.reg += d->offset;
		}
		r = in->s.reg;
		off = in->s.offset;
		if((in->sm == Afpind || in->sm == Ampind)
		&& (r >= MaxReg || off >= MaxReg))
			fatal("big offset in %I\n", in);

		d = in->m.decl;
		if(d != nil){
			if(in->mm == Adesc){
				if(d->desc != nil)
					in->m.offset = d->desc->id;
			}else
				in->m.reg += d->offset;
		}
		v = 0;
		switch(in->mm){
		case Anone:
			break;
		case Aimm:
		case Apc:
		case Adesc:
			v = in->m.offset;
			break;
		case Aoff:
			v = in->m.decl->iface->offset;
			break;
		case Afp:
		case Amp:
			v = in->m.reg;
			if(v < 0)
				v = 0x8000;
			break;
		default:
			fatal("can't deal with %I's m mode\n", in);
			break;
		}
		if(v > 0x7fff || v < -0x8000){
			switch(in->op){
			case IALT:
			case IINDX:
warn(in->src.start, "possible bug: temp m too big in %I: %ld %ld %d\n", in, in->m.reg, in->m.reg, MaxReg);
				rewritedestreg(in, IMOVW, RTemp);
				break;
			default:
				op = IMOVW;
				if(isbyteinst[in->op])
					op = IMOVB;
				in = rewritesrcreg(in, op, RTemp, pc++);
				break;
			}
		}

		d = in->d.decl;
		if(d != nil){
			if(in->dm == Apc)
				in->d.offset = d->pc->pc;
			else
				in->d.reg += d->offset;
		}
		r = in->d.reg;
		off = in->d.offset;
		if((in->dm == Afpind || in->dm == Ampind)
		&& (r >= MaxReg || off >= MaxReg))
			fatal("big offset in %I\n", in);

		in->pc = pc;
		pc++;
	}
	for(in = inst; in != nil; in = in->next){
		d = in->d.decl;
		if(d != nil && in->dm == Apc)
			in->d.offset = d->pc->pc;
		if(in->branch != nil){
			in->dm = Apc;
			in->d.offset = in->branch->pc;
		}
	}
	return pc;
}

/*
 * fixp up a big register constant uses as a source
 * ugly: smashes the instruction
 */
Inst*
rewritesrcreg(Inst *in, int op, int treg, int pc)
{
	Inst *new;
	Addr a;
	int am;

	a = in->m;
	am = in->mm;
	in->mm = Afp;
	in->m.reg = treg;
	in->m.decl = nil;

	new = allocmem(sizeof(*in));
	*new = *in;

	*in = zinst;
	in->src = new->src;
	in->next = new;
	in->op = op;
	in->s = a;
	in->sm = am;
	in->dm = Afp;
	in->d.reg = treg;
	in->pc = pc;
	in->reach = 1;
	in->block = new->block;
	return new;
}

/*
 * fix up a big register constant by moving to the destination
 * after the instruction completes
 */
Inst*
rewritedestreg(Inst *in, int op, int treg)
{
	Inst *n;

	n = allocmem(sizeof(*n));
	*n = zinst;
	n->next = in->next;
	in->next = n;
	n->src = in->src;
	n->op = op;
	n->sm = Afp;
	n->s.reg = treg;
	n->d = in->m;
	n->dm = in->mm;
	n->reach = 1;
	n->block = in->block;

	in->mm = Afp;
	in->m.reg = treg;
	in->m.decl = nil;

	return n;
}

int
instconv(va_list *arg, Fconv *f)
{
	Inst *in;
	char buf[512], *p;
	char *op, *comma;

	in = va_arg(*arg, Inst*);
	op = nil;
	if(in->op >= 0 && in->op < MAXDIS)
		op = instname[in->op];
	if(op == nil)
		op = "??";
	buf[0] = '\0';
	if(in->op == INOP){
		strconv("\tnop", f);
		return sizeof(Inst*);
	}
	p = seprint(buf, buf + sizeof(buf), "\t%s\t", op);
	comma = "";
	if(in->sm != Anone){
		p = addrprint(p, buf + sizeof(buf), in->sm, &in->s);
		comma = ",";
	}
	if(in->mm != Anone){
		p = seprint(p, buf + sizeof(buf), "%s", comma);
		p = addrprint(p, buf + sizeof(buf), in->mm, &in->m);
		comma = ",";
	}
	if(in->dm != Anone){
		p = seprint(p, buf + sizeof(buf), "%s", comma);
		p = addrprint(p, buf + sizeof(buf), in->dm, &in->d);
	}
	
	if(asmsym && in->s.decl != nil && in->sm == Adesc)
		p = seprint(p, buf+sizeof(buf), "	#%D", in->s.decl);
	if(0 && asmsym && in->m.decl != nil)
		p = seprint(p, buf+sizeof(buf), "	#%D", in->m.decl);
	if(asmsym && in->d.decl != nil && in->dm == Apc)
		p = seprint(p, buf+sizeof(buf), "	#%D", in->d.decl);
	if(asmsym)
		p = seprint(p, buf+sizeof(buf), "	#%U", in->src);
	USED(p);
	strconv(buf, f);
	return 0;
}

char*
addrprint(char *buf, char *end, int am, Addr *a)
{
	switch(am){
	case Anone:
		return buf;
	case Aimm:
	case Apc:
	case Adesc:
		return seprint(buf, end, "$%ld", a->offset);
	case Aoff:
		return seprint(buf, end, "$%ld", a->decl->iface->offset);
	case Afp:
		return seprint(buf, end, "%ld(fp)", a->reg);
	case Afpind:
		return seprint(buf, end, "%ld(%ld(fp))", a->offset, a->reg);
	case Amp:
		return seprint(buf, end, "%ld(mp)", a->reg);
	case Ampind:
		return seprint(buf, end, "%ld(%ld(mp))", a->offset, a->reg);
	case Aerr:
	default:
		return seprint(buf, end, "%ld(%ld(?%d?))", a->offset, a->reg, am);
	}
}
