/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

// get_fn_name is slowing down the kprocread 
// 	have an array of translated fns
// 	or a "next" iterator, since we're walking in order
//
// irqsave locks
//
// kprof struct should be a ptr, have them per core
// 		we'll probably need to track the length still, so userspace knows how
// 		big it is
//
// 		will also want more files in the kprof dir for each cpu or something
// 		
// maybe don't use slot 0 and 1 as total and 'not kernel' ticks
//
// fix the failed assert XXX

#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>
#include <ip.h>
#include <oprofile.h>

#define LRES	3		/* log of PC resolution */
#define CELLSIZE	8	/* sizeof of count cell */

struct kprof
{
	uintptr_t	minpc;
	uintptr_t	maxpc;
	int	nbuf;
	int	time;
	uint64_t	*buf;	/* keep in sync with cellsize */
	size_t		buf_sz;
	spinlock_t lock;
};
struct kprof kprof;

/* output format. Nice fixed size. That makes it seekable.
 * small subtle bit here. You have to convert offset FROM FORMATSIZE units
 * to CELLSIZE units in a few places.
 */
char *outformat = "%016llx %29.29s %016llx\n";
#define FORMATSIZE 64
enum{
	Kprofdirqid,
	Kprofdataqid,
	Kprofctlqid,
	Kprofoprofileqid,
};
struct dirtab kproftab[]={
	{".",		{Kprofdirqid, 0, QTDIR},0,	DMDIR|0550},
	{"kpdata",	{Kprofdataqid},		0,	0600},
	{"kpctl",	{Kprofctlqid},		0,	0600},
	{"kpoprofile",	{Kprofoprofileqid},	0,	0600},
};

static struct chan*
kprofattach(char *spec)
{
	uint32_t n;

	/* allocate when first used */
	kprof.minpc = 0xffffffffc2000000; //KERN_LOAD_ADDR;
	kprof.maxpc = (uintptr_t)&etext;
	kprof.nbuf = (kprof.maxpc-kprof.minpc) >> LRES;
	n = kprof.nbuf*CELLSIZE;
	if(kprof.buf == 0) {
		printk("Allocate %d bytes\n", n);
		kprof.buf = kzmalloc(n, KMALLOC_WAIT);
		if(kprof.buf == 0)
			error(Enomem);
	}
	kproftab[1].length = kprof.nbuf * FORMATSIZE;
	kprof.buf_sz = n;
	/* NO, I'm not sure how we should do this yet. */
	int alloc_cpu_buffers(void);
	alloc_cpu_buffers();
	return devattach('K', spec);
}

static void
kproftimer(uintptr_t pc)
{
	if(kprof.time == 0)
		return;

	/*
	 * if the pc corresponds to the idle loop, don't consider it.

	if(m->inidle)
		return;
	 */
	/*
	 *  if the pc is coming out of spllo or splx,
	 *  use the pc saved when we went splhi.

	if(pc>=PTR2UINT(spllo) && pc<=PTR2UINT(spldone))
		pc = m->splpc;
	 */

//	ilock(&kprof);
	/* this is weird. What we do is assume that all the time since the last
	 * measurement went into this PC. It's the best
	 * we can do I suppose. And we are sampling at 1 ms. for now.
	 * better ideas welcome.
	 */
	kprof.buf[0] += 1; //Total count of ticks.
	if(kprof.minpc<=pc && pc<kprof.maxpc){
		pc -= kprof.minpc;
		pc >>= LRES;
		kprof.buf[pc] += 1;
	}else
		kprof.buf[1] += 1; // Why?
//	iunlock(&kprof);
}

static void setup_timers(void)
{
	void handler(struct alarm_waiter *waiter)
	{
		struct timer_chain *tchain = &per_cpu_info[core_id()].tchain;
		kproftimer(per_cpu_info[core_id()].rip);
		set_awaiter_rel(waiter, 1000);
		__set_alarm(tchain, waiter);
	}
	struct timer_chain *tchain = &per_cpu_info[core_id()].tchain;
	struct alarm_waiter *waiter = kmalloc(sizeof(struct alarm_waiter), 0);
	init_awaiter(waiter, handler);
	set_awaiter_rel(waiter, 1000);
	set_alarm(tchain, waiter);
}
static void
kprofinit(void)
{
	if(CELLSIZE != sizeof kprof.buf[0])
		panic("kprof size");
}

static struct walkqid*
kprofwalk(struct chan *c, struct chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, kproftab, ARRAY_SIZE(kproftab), devgen);
}

static int
kprofstat(struct chan *c, uint8_t *db, int n)
{
	return devstat(c, db, n, kproftab, ARRAY_SIZE(kproftab), devgen);
}

static struct chan*
kprofopen(struct chan *c, int omode)
{
	if(c->qid.type & QTDIR){
		if(omode != OREAD)
			error(Eperm);
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static void
kprofclose(struct chan*unused)
{
}

static long
kprofread(struct chan *c, void *va, long n, int64_t off)
{
	uint64_t w, *bp;
	char *a, *ea;
	uintptr_t offset = off;
	uint64_t pc;
	int snp_ret, ret = 0;
	/* the oprofile queue */
	extern struct queue *opq;

	switch((int)c->qid.path){
	case Kprofdirqid:
		return devdirread(c, va, n, kproftab, ARRAY_SIZE(kproftab), devgen);

	case Kprofdataqid:

		if (n < FORMATSIZE){
			n = 0;
			break;
		}
		a = va;
		ea = a + n;

		/* we check offset later before deref bp.  offset / FORMATSIZE is how
		 * many entries we're skipping/offsetting. */
		bp = kprof.buf + offset/FORMATSIZE;
		pc = kprof.minpc + ((offset/FORMATSIZE)<<LRES);
		while((a < ea) && (n >= FORMATSIZE)){
			/* what a pain. We need to manage the
			 * fact that the *prints all make room for
			 * \0
			 */
			char print[FORMATSIZE+1];
			char *name;
			int amt_read;

			if (pc >= kprof.maxpc)
				break;
			/* pc is also our exit for bp.  should be in lockstep */
			// XXX this assert fails, fix it!
			//assert(bp < kprof.buf + kprof.nbuf);
			/* do not attempt to filter these results based on w < threshold.
			 * earlier, we computed bp/pc based on assuming a full-sized file,
			 * and skipping entries will result in read() calls thinking they
			 * received earlier entries when they really received later ones.
			 * imagine a case where there are 1000 skipped items, and read()
			 * asks for chunks of 32.  it'll get chunks of the next 32 valid
			 * items, over and over (1000/32 times). */
			w = *bp++;

			if (pc == kprof.minpc)
				name = "Total";
			else if (pc == kprof.minpc + 8)
				name = "User";
			else
				name = get_fn_name(pc);

			snp_ret = snprintf(print, sizeof(print), outformat, pc, name, w);
			assert(snp_ret == FORMATSIZE);
			if ((pc != kprof.minpc) && (pc != kprof.minpc + 8))
				kfree(name);

			amt_read = readmem(offset % FORMATSIZE, a, n, print, FORMATSIZE);
			offset = 0;	/* future loops have no offset */

			a += amt_read;
			n -= amt_read;
			ret += amt_read;

			pc += (1 << LRES);
		}
		n = ret;
		break;
	case Kprofoprofileqid:
		n = qread(opq, va, n);
		break;
	default:
		n = 0;
		break;
	}
	return n;
}

static void kprof_clear(struct kprof *kp)
{
	spin_lock(&kp->lock);
	memset(kp->buf, 0, kp->buf_sz);
	spin_unlock(&kp->lock);
}

static long
kprofwrite(struct chan *c, void *a, long n, int64_t unused)
{
	uintptr_t pc;
	switch((int)(c->qid.path)){
	case Kprofctlqid:
		if(strncmp(a, "startclr", 8) == 0){
			kprof_clear(&kprof);
			kprof.time = 1;
		}else if(strncmp(a, "start", 5) == 0) {
			kprof.time = 1;
			setup_timers();
		} else if(strncmp(a, "stop", 4) == 0) {
			kprof.time = 0;
		} else if(strncmp(a, "clear", 5) == 0) {
			kprof_clear(&kprof);
		}else if(strncmp(a, "opstart", 8) == 0) {
			/* maybe have enable/disable for it. */
		}else if(strncmp(a, "opstop", 6) == 0) {
		} else  {
			error("startclr|start|stop|clear|opstart|opstop");
		}
		break;

		/* The format is a long as text. We strtoul, and jam it into the
		 * trace buffer.
		 */
	case Kprofoprofileqid:
		pc = strtoul(a, 0, 0);
		oprofile_add_trace(pc);
		break;
	default:
		error(Ebadusefd);
	}
	return n;
}

struct dev kprofdevtab __devtab = {
	'K',
	"kprof",

	devreset,
	kprofinit,
	devshutdown,
	kprofattach,
	kprofwalk,
	kprofstat,
	kprofopen,
	devcreate,
	kprofclose,
	kprofread,
	devbread,
	kprofwrite,
	devbwrite,
	devremove,
	devwstat,
};
