#include <lib9.h>

typedef struct	Instr	Instr;
struct	Instr
{
	ulong	w;
	ulong	addr;
	uchar	op;			/* super opcode */

	uchar	cond;			/* bits 28-31 */
	uchar	store;			/* bit 20 */

	uchar	rd;			/* bits 12-15 */
	uchar	rn;			/* bits 16-19 */
	uchar	rs;			/* bits 0-11 */

	long	imm;			/* rotated imm */
	char*	curr;			/* fill point in buffer */
	char*	end;			/* end of buffer */
	char*	err;			/* error message */
};

typedef struct Opcode Opcode;
struct Opcode
{
	char*	o;
	void	(*f)(Opcode*, Instr*);
	char*	a;
};

static	void	format(char*, Instr*, char*);
static	int	arminst(ulong, char, char*, int);
static	int	armdas(ulong, char*, int);

static
char*	cond[16] =
{
	"EQ",	"NE",	"CS",	"CC",
	"MI",	"PL",	"VS",	"VC",
	"HI",	"LS",	"GE",	"LT",
	"GT",	"LE",	0,	"NV"
};

static
char*	shtype[4] =
{
	"<<",	">>",	"->",	"@>"
};

static int
get4(ulong addr, long *v)
{
	*v = *(ulong*)addr;
	return 1;	
}

static char *
_hexify(char *buf, ulong p, int zeros)
{
	ulong d;

	d = p/16;
	if(d)
		buf = _hexify(buf, d, zeros-1);
	else
		while(zeros--)
			*buf++ = '0';
	*buf++ = "0123456789abcdef"[p&0x0f];
	return buf;
}

int
armclass(long w)
{
	int op;

	op = (w >> 25) & 0x7;
	switch(op) {
	case 0:	/* data processing r,r,r */
		op = ((w >> 4) & 0xf);
		if(op == 0x9) {
			op = 48+16;		/* mul */
			if(w & (1<<24)) {
				op += 2;
				if(w & (1<<22))
					op++;	/* swap */
				break;
			}
			if(w & (1<<21))
				op++;		/* mla */
			break;
		}
		op = (w >> 21) & 0xf;
		if(w & (1<<4))
			op += 32;
		else
		if(w & (31<<7))
			op += 16;
		break;
	case 1:	/* data processing i,r,r */
		op = (48) + ((w >> 21) & 0xf);
		break;
	case 2:	/* load/store byte/word i(r) */
		op = (48+20) + ((w >> 22) & 0x1) + ((w >> 19) & 0x2);
		break;
	case 3:	/* load/store byte/word (r)(r) */
		op = (48+20+4) + ((w >> 22) & 0x1) + ((w >> 19) & 0x2);
		break;
	case 4:	/* block data transfer (r)(r) */
		op = (48+20+4+4) + ((w >> 20) & 0x1);
		break;
	case 5:	/* branch / branch link */
		op = (48+20+4+4+2) + ((w >> 24) & 0x1);
		break;
	case 7:	/* coprocessor crap */
		op = (48+20+4+4+2+2) + ((w >> 3) & 0x2) + ((w >> 20) & 0x1);
		break;
	default:
		op = (48+20+4+4+2+2+4);
		break;
	}
	return op;
}

static int
decode(ulong pc, Instr *i)
{
	long w;

	get4(pc, &w);
	i->w = w;
	i->addr = pc;
	i->cond = (w >> 28) & 0xF;
	i->op = armclass(w);
	return 1;
}

static void
bprint(Instr *i, char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	i->curr = doprint(i->curr, i->end, fmt, arg);
	va_end(arg);
}

static void
armdps(Opcode *o, Instr *i)
{
	i->store = (i->w >> 20) & 1;
	i->rn = (i->w >> 16) & 0xf;
	i->rd = (i->w >> 12) & 0xf;
	i->rs = (i->w >> 0) & 0xf;
	if(i->rn == 15 && i->rs == 0) {
		if(i->op == 8) {
			format("MOVW", i,"CPSR, R%d");
			return;
		} else
		if(i->op == 10) {
			format("MOVW", i,"SPSR, R%d");
			return;
		}
	} else
	if(i->rn == 9 && i->rd == 15) {
		if(i->op == 9) {
			format("MOVW", i, "R%s, CPSR");
			return;
		} else
		if(i->op == 11) {
			format("MOVW", i, "R%s, SPSR");
			return;
		}
	}
	format(o->o, i, o->a);
}

static void
armdpi(Opcode *o, Instr *i)
{
	ulong v;
	int c;

	v = (i->w >> 0) & 0xff;
	c = (i->w >> 8) & 0xf;
	while(c) {
		v = (v<<30) | (v>>2);
		c--;
	}
	i->imm = v;
	i->store = (i->w >> 20) & 1;
	i->rn = (i->w >> 16) & 0xf;
	i->rd = (i->w >> 12) & 0xf;
	i->rs = i->w&0x0f;

		/* RET is encoded as ADD #0,R14,R15 */
	if(i->w == 0xe282f000){
		format("RET", i, "");
		return;
	} else
	format(o->o, i, o->a);
}

static void
armsdti(Opcode *o, Instr *i)
{
	ulong v;

	v = (i->w >> 0) & 0xfff;
	if(!(i->w & (1<<23)))
		v = -v;
	i->store = ((i->w >> 23) & 0x2) | ((i->w >>21) & 0x1);
	i->imm = v;
	i->rn = (i->w >> 16) & 0xf;
	i->rd = (i->w >> 12) & 0xf;
		/* convert load of offset(PC) to a load immediate */
	if(i->rn == 15 && (i->w & (1<<20)) && get4(i->addr+v+8, &i->imm) > 0)
		format(o->o, i, "$#%i,R%d");
	else
		format(o->o, i, o->a);
}

static void
armsdts(Opcode *o, Instr *i)
{
	i->store = ((i->w >> 23) & 0x2) | ((i->w >>21) & 0x1);
	i->rs = (i->w >> 0) & 0xf;
	i->rn = (i->w >> 16) & 0xf;
	i->rd = (i->w >> 12) & 0xf;
	format(o->o, i, o->a);
}

static void
armbdt(Opcode *o, Instr *i)
{
	i->store = (i->w >> 21) & 0x3;		/* S & W bits */
	i->rn = (i->w >> 16) & 0xf;
	i->imm = i->w & 0xffff;
	if(i->w == 0xe8fd8000)
		format("RFE", i, "");
	else
		format(o->o, i, o->a);
}

static void
armund(Opcode *o, Instr *i)
{
	format(o->o, i, o->a);
}

static void
armcdt(Opcode *o, Instr *i)
{
	format(o->o, i, o->a);
}

static void
armunk(Opcode *o, Instr *i)
{
	format(o->o, i, o->a);
}

static void
armb(Opcode *o, Instr *i)
{
	ulong v;

	v = i->w & 0xffffff;
	if(v & 0x800000)
		v |= ~0xffffff;
	i->imm = (v<<2) + i->addr + 8;
	format(o->o, i, o->a);
}

static void
armco(Opcode *o, Instr *i)		/* coprocessor instructions */
{
	int op, p, cp;

	char buf[1024];

	i->rn = (i->w >> 16) & 0xf;
	i->rd = (i->w >> 12) & 0xf;
	i->rs = i->w&0xf;
	cp = (i->w >> 8) & 0xf;
	p = (i->w >> 5) & 0x7;
	if(i->w&0x10) {
		op = (i->w >> 20) & 0x0f;
		snprint(buf, sizeof(buf), "#%x, #%x, R%d, C(%d), C(%d), #%x\n", cp, op, i->rd, i->rn, i->rs, p);
	} else {
		op = (i->w >> 21) & 0x07;
		snprint(buf, sizeof(buf), "#%x, #%x, C(%d), C(%d), C(%d), #%x\n", cp, op, i->rd, i->rn, i->rs, p);
	}
	format(o->o, i, buf);
}

static Opcode opcodes[] =
{
	"AND%C%S",	armdps,	"R%s,R%n,R%d",
	"EOR%C%S",	armdps,	"R%s,R%n,R%d",
	"SUB%C%S",	armdps,	"R%s,R%n,R%d",
	"RSB%C%S",	armdps,	"R%s,R%n,R%d",
	"ADD%C%S",	armdps,	"R%s,R%n,R%d",
	"ADC%C%S",	armdps,	"R%s,R%n,R%d",
	"SBC%C%S",	armdps,	"R%s,R%n,R%d",
	"RSC%C%S",	armdps,	"R%s,R%n,R%d",
	"TST%C%S",	armdps,	"R%s,R%n,",
	"TEQ%C%S",	armdps,	"R%s,R%n,",
	"CMP%C%S",	armdps,	"R%s,R%n,",
	"CMN%C%S",	armdps,	"R%s,R%n,",
	"ORR%C%S",	armdps,	"R%s,R%n,R%d",
	"MOVW%C%S",	armdps,	"R%s,R%d",
	"BIC%C%S",	armdps,	"R%s,R%n,R%d",
	"MVN%C%S",	armdps,	"R%s,R%d",

	"AND%C%S",	armdps,	"(R%s%h#%m),R%n,R%d",
	"EOR%C%S",	armdps,	"(R%s%h#%m),R%n,R%d",
	"SUB%C%S",	armdps,	"(R%s%h#%m),R%n,R%d",
	"RSB%C%S",	armdps,	"(R%s%h#%m),R%n,R%d",
	"ADD%C%S",	armdps,	"(R%s%h#%m),R%n,R%d",
	"ADC%C%S",	armdps,	"(R%s%h#%m),R%n,R%d",
	"SBC%C%S",	armdps,	"(R%s%h#%m),R%n,R%d",
	"RSC%C%S",	armdps,	"(R%s%h#%m),R%n,R%d",
	"TST%C%S",	armdps,	"(R%s%h#%m),R%n,",
	"TEQ%C%S",	armdps,	"(R%s%h#%m),R%n,",
	"CMP%C%S",	armdps,	"(R%s%h#%m),R%n,",
	"CMN%C%S",	armdps,	"(R%s%h#%m),R%n,",
	"ORR%C%S",	armdps,	"(R%s%h#%m),R%n,R%d",
	"MOVW%C%S",	armdps,	"(R%s%h#%m),R%d",
	"BIC%C%S",	armdps,	"(R%s%h#%m),R%n,R%d",
	"MVN%C%S",	armdps,	"(R%s%h#%m),R%d",

	"AND%C%S",	armdps,	"(R%s%hR%m),R%n,R%d",
	"EOR%C%S",	armdps,	"(R%s%hR%m),R%n,R%d",
	"SUB%C%S",	armdps,	"(R%s%hR%m),R%n,R%d",
	"RSB%C%S",	armdps,	"(R%s%hR%m),R%n,R%d",
	"ADD%C%S",	armdps,	"(R%s%hR%m),R%n,R%d",
	"ADC%C%S",	armdps,	"(R%s%hR%m),R%n,R%d",
	"SBC%C%S",	armdps,	"(R%s%hR%m),R%n,R%d",
	"RSC%C%S",	armdps,	"(R%s%hR%m),R%n,R%d",
	"TST%C%S",	armdps,	"(R%s%hR%m),R%n,",
	"TEQ%C%S",	armdps,	"(R%s%hR%m),R%n,",
	"CMP%C%S",	armdps,	"(R%s%hR%m),R%n,",
	"CMN%C%S",	armdps,	"(R%s%hR%m),R%n,",
	"ORR%C%S",	armdps,	"(R%s%hR%m),R%n,R%d",
	"MOVW%C%S",	armdps,	"(R%s%hR%m),R%d",
	"BIC%C%S",	armdps,	"(R%s%hR%m),R%n,R%d",
	"MVN%C%S",	armdps,	"(R%s%hR%m),R%d",

	"AND%C%S",	armdpi,	"$#%i,R%n,R%d",
	"EOR%C%S",	armdpi,	"$#%i,R%n,R%d",
	"SUB%C%S",	armdpi,	"$#%i,R%n,R%d",
	"RSB%C%S",	armdpi,	"$#%i,R%n,R%d",
	"ADD%C%S",	armdpi,	"$#%i,R%n,R%d",
	"ADC%C%S",	armdpi,	"$#%i,R%n,R%d",
	"SBC%C%S",	armdpi,	"$#%i,R%n,R%d",
	"RSC%C%S",	armdpi,	"$#%i,R%n,R%d",
	"TST%C%S",	armdpi,	"$#%i,R%n,",
	"TEQ%C%S",	armdpi,	"$#%i,R%n,",
	"CMP%C%S",	armdpi,	"$#%i,R%n,",
	"CMN%C%S",	armdpi,	"$#%i,R%n,",
	"ORR%C%S",	armdpi,	"$#%i,R%n,R%d",
	"MOVW%C%S",	armdpi,	"$#%i,,R%d",
	"BIC%C%S",	armdpi,	"$#%i,R%n,R%d",
	"MVN%C%S",	armdpi,	"$#%i,,R%d",

	"MUL%C%S",	armdpi,	"R%s,R%m,R%n",
	"MULA%C%S",	armdpi,	"R%s,R%m,R%n,R%d",
	"SWPW",		armdpi,	"R%s,(R%n),R%d",
	"SWPB",		armdpi,	"R%s,(R%n),R%d",

	"MOVW%C%p",	armsdti,"R%d,#%i(R%n)",
	"MOVB%C%p",	armsdti,"R%d,#%i(R%n)",
	"MOVW%C%p",	armsdti,"#%i(R%n),R%d",
	"MOVB%C%p",	armsdti,"#%i(R%n),R%d",

	"MOVW%C%p",	armsdts,"R%d,%D(R%s%h#%m)(R%n)",
	"MOVB%C%p",	armsdts,"R%d,%D(R%s%h#%m)(R%n)",
	"MOVW%C%p",	armsdts,"%D(R%s%h#%m)(R%n),R%d",
	"MOVB%C%p",	armsdts,"%D(R%s%h#%m)(R%n),R%d",

	"MOVM%C%P%a",	armbdt,	"R%n,[%r]",
	"MOVM%C%P%a",	armbdt,	"[%r],R%n",

	"B%C",		armb,	"%b",
	"BL%C",		armb,	"%b",

	"CDP%C",	armco,	"",
	"CDP%C",	armco,	"",
	"MCR%C",	armco,	"",
	"MRC%C",	armco,	"",

	"UNK",		armunk,	"",
};

static	char *mode[] = { 0, "IA", "DB", "IB" };
static	char *pw[] = { "P", "PW", 0, "W" };
static	char *sw[] = { 0, "W", "S", "SW" };

static void
format(char *mnemonic, Instr *i, char *f)
{
	int j, k, m, n;

	if(mnemonic)
		format(0, i, mnemonic);
	if(f == 0)
		return;
	if(mnemonic)
		if(i->curr < i->end)
			*i->curr++ = '\t';
	for ( ; *f && i->curr < i->end; f++) {
		if(*f != '%') {
			*i->curr++ = *f;
			continue;
		}
		switch (*++f) {

		case 'C':	/* .CONDITION */
			if(cond[i->cond])
				bprint(i, ".%s", cond[i->cond]);
			break;

		case 'S':	/* .STORE */
			if(i->store)
				bprint(i, ".S");
			break;

		case 'P':	/* P & U bits for block move */
			n = (i->w >>23) & 0x3;
			if (mode[n])
				bprint(i, ".%s", mode[n]);
			break;

		case 'D':	/* ~U bit for single data xfer */
			if((i->w & (1<<23)) == 0)
				bprint(i, "-");
			break;

		case 'p':	/* P & W bits for single data xfer*/
			if (pw[i->store])
				bprint(i, ".%s", pw[i->store]);
			break;

		case 'a':	/* S & W bits for single data xfer*/
			if (sw[i->store])
				bprint(i, ".%s", sw[i->store]);
			break;

		case 's':
			bprint(i, "%d", i->rs & 0xf);
			break;
				
		case 'm':
			bprint(i, "%d", (i->w>>7) & 0x1f);
			break;

		case 'h':
			bprint(i, "%s", shtype[(i->w>>5) & 0x3]);
			break;

		case 'n':
			bprint(i, "%d", i->rn);
			break;

		case 'd':
			bprint(i, "%d", i->rd);
			break;

		case 'i':
			bprint(i, "%lux", i->imm);
			break;

		case 'b':
			bprint(i, "%lux", i->imm);
			break;

		case 'r':
			n = i->imm&0xffff;
			j = 0;
			k = 0;
			while(n) {
				m = j;
				while(n&0x1) {
					j++;
					n >>= 1;
				}
				if(j != m) {
					if(k)
						bprint(i, ",");
					if(j == m+1)
						bprint(i, "R%d", m);
					else
						bprint(i, "R%d-R%d", m, j-1);
					k = 1;
				}
				j++;
				n >>= 1;
			}
			break;

		case '\0':
			*i->curr++ = '%';
			return;

		default:
			bprint(i, "%%%c", *f);
			break;
		}
	}
	*i->curr = 0;
}

void
das(ulong *x, int n)
{
	ulong pc;
	Instr i;
	char buf[128];

	pc = (ulong)x;
	while(n > 0) {
		i.curr = buf;
		i.end = buf+sizeof(buf)-1;

		if(decode(pc, &i) < 0)
			sprint(buf, "???");
		else
			(*opcodes[i.op].f)(&opcodes[i.op], &i);

		print("%.8lux %.8lux\t%s\n", pc, i.w, buf);
		pc += 4;
		n--;
	}
}
