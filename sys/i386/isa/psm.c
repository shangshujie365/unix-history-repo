/*-
 * Copyright (c) 1992, 1993 Erik Forsberg.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * THIS SOFTWARE IS PROVIDED BY ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 * NO EVENT SHALL I BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *  Ported to 386bsd Oct 17, 1992
 *  Sandi Donno, Computer Science, University of Cape Town, South Africa
 *  Please send bug reports to sandi@cs.uct.ac.za
 *
 *  Thanks are also due to Rick Macklem, rick@snowhite.cis.uoguelph.ca -
 *  although I was only partially successful in getting the alpha release
 *  of his "driver for the Logitech and ATI Inport Bus mice for use with
 *  386bsd and the X386 port" to work with my Microsoft mouse, I nevertheless
 *  found his code to be an invaluable reference when porting this driver
 *  to 386bsd.
 *
 *  Further modifications for latest 386BSD+patchkit and port to NetBSD,
 *  Andrew Herbert <andrew@werple.apana.org.au> - 8 June 1993
 *
 *  Cloned from the Microsoft Bus Mouse driver, also by Erik Forsberg, by
 *  Andrew Herbert - 12 June 1993
 *
 *  Modified for PS/2 mouse by Charles Hannum <mycroft@ai.mit.edu>
 *  - 13 June 1993
 *
 *  Modified for PS/2 AUX mouse by Shoji Yuen <yuen@nuie.nagoya-u.ac.jp>
 *  - 24 October 1993
 */

#include "psm.h"

#if NPSM > 0

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/file.h>
#include <sys/proc.h>
#include <sys/conf.h>
#ifdef DEVFS
#include <sys/devfsext.h>
#endif /*DEVFS*/

#include <machine/mouse.h>
#include <machine/clock.h>

#include <i386/isa/isa_device.h>

#define PSM_DATA	0x00	/* Offset for data port, read-write */
#define PSM_CNTRL	0x04	/* Offset for control port, write-only */
#define PSM_STATUS	0x04	/* Offset for status port, read-only */

/* status bits */
#define	PSM_OUTPUT_ACK	0x02	/* output acknowledge */

/* controller commands */
#define	PSM_INT_ENABLE	0x47	/* enable controller interrupts */
#define	PSM_INT_DISABLE	0x65	/* disable controller interrupts */
#define	PSM_DISABLE	0xa7	/* disable auxiliary port */
#define	PSM_ENABLE	0xa8	/* enable auxiliary port */
#define	PSM_AUX_TEST	0xa9	/* test auxiliary port */

/* mouse commands */
#define PSM_SET_SCALE11	0xe6	/* set 1:1 scaling */
#define PSM_SET_SCALE21 0xe7	/* set 2:1 scaling */
#define	PSM_SET_RES	0xe8	/* set resolution */
#define	PSM_GET_SCALE	0xe9	/* set scaling factor */
#define	PSM_SET_STREAM	0xea	/* set streaming mode */
#define	PSM_SET_SAMPLE	0xf3	/* set sampling rate */
#define	PSM_DEV_ENABLE	0xf4	/* mouse on */
#define	PSM_DEV_DISABLE	0xf5	/* mouse off */
#define	PSM_RESET	0xff	/* reset */

#define PSMUNIT(dev)	(minor(dev) >> 1)

#ifndef min
#define min(x,y) (x < y ? x : y)
#endif  min

static int psmprobe(struct isa_device *);
static int psmattach(struct isa_device *);
static void psm_poll_status(int);

static int psmaddr[NPSM];	/* Base I/O port addresses per unit */

#define PSM_CHUNK	128	/* chunk size for read */
#define PSM_BSIZE	1024	/* buffer size */

struct ringbuf {
	int count, first, last;
	char queue[PSM_BSIZE];
};

static struct psm_softc {	/* Driver status information */
	struct ringbuf inq;	/* Input queue */
	struct selinfo	rsel;	/* Process selecting for Input */
	unsigned char state;	/* Mouse driver state */
	unsigned char status;	/* Mouse button status */
	unsigned char button;	/* Previous mouse button status bits */
	int x, y;		/* accumulated motion in the X,Y axis */
#ifdef DEVFS
	void	*devfs_token;
	void	*n_devfs_token;
#endif
} psm_softc[NPSM];

#define PSM_OPEN	1		/* Device is open */
#define PSM_ASLP	2		/* Waiting for mouse data */

struct isa_driver psmdriver = { psmprobe, psmattach, "psm" };

static	d_open_t	psmopen;
static	d_close_t	psmclose;
static	d_read_t	psmread;
static	d_ioctl_t	psmioctl;
static	d_select_t	psmselect;

#define CDEV_MAJOR 21
static	struct	cdevsw psm_cdevsw =
	{ psmopen,	psmclose,	psmread,	nowrite,	/*21*/
	  psmioctl,	nostop,		nullreset,	nodevtotty,
	  psmselect,	nommap,		NULL,	"psm",	NULL,	-1 };

static struct kern_devconf kdc_psm[NPSM] = { {
	0, 0, 0,		/* filled in by dev_attach */
	"psm", 0, { MDDT_ISA, 0, "tty" },
	isa_generic_externalize, 0, 0, ISA_EXTERNALLEN,
	&kdc_isa0,		/* parent */
	0,			/* parentdata */
	DC_UNCONFIGURED,	/* state */
	"PS/2 Mouse",
	DC_CLS_MISC		/* class */
} };

static inline void
psm_registerdev(struct isa_device *id)
{
	if(id->id_unit)
		kdc_psm[id->id_unit] = kdc_psm[0];
	kdc_psm[id->id_unit].kdc_unit = id->id_unit;
	kdc_psm[id->id_unit].kdc_isa = id;
	dev_attach(&kdc_psm[id->id_unit]);
}

static void
psm_write_dev(int ioport, u_char value)
{
	psm_poll_status(ioport);
	outb(ioport+PSM_CNTRL, 0xd4);
	psm_poll_status(ioport);
	outb(ioport+PSM_DATA, value);
}

static inline void
psm_command(int ioport, u_char value)
{
	psm_poll_status(ioport);
	outb(ioport+PSM_CNTRL, 0x60);
	psm_poll_status(ioport);
	outb(ioport+PSM_DATA, value);
}

static int 
psmprobe(struct isa_device *dvp)
{
	/* XXX: Needs a real probe routine. */
	int ioport, c, unit;

	psm_registerdev(dvp);

	ioport=dvp->id_iobase;
	unit=dvp->id_unit;

#ifndef PSM_NO_RESET
	psm_write_dev(ioport, PSM_RESET);	/* Reset aux device */
#endif
	psm_poll_status(ioport);
	outb(ioport+PSM_CNTRL, PSM_AUX_TEST);
	psm_poll_status(ioport);
	outb(ioport+PSM_CNTRL, 0xaa);
	c = inb(ioport+PSM_DATA);
	if(c & 0x04) {
/*		printf("PS/2 AUX mouse is not found\n");*/
		psm_command(ioport, PSM_INT_DISABLE);
		psmaddr[unit] = 0;	/* Device not found */
		return (0);
	}
/*	printf("PS/2 AUX mouse found.  Installing driver\n");*/
	return (4);
}

static int
psmattach(struct isa_device *dvp)
{
	int unit = dvp->id_unit;
	int ioport = dvp->id_iobase;
	struct psm_softc *sc = &psm_softc[unit];

	/* Save I/O base address */
	psmaddr[unit] = ioport;

	/* Disable mouse interrupts */
	psm_poll_status(ioport);
	outb(ioport+PSM_CNTRL, PSM_ENABLE);
#ifdef 0
	psm_write(ioport, PSM_SET_RES);
	psm_write(ioport, 0x03);	/* 8 counts/mm */
	psm_write(ioport, PSM_SET_SCALE);
	psm_write(ioport, 0x02);	/* 2:1 */
	psm_write(ioport, PSM_SET_SCALE21);
	psm_write(ioport, PSM_SET_SAMPLE);
	psm_write(ioport, 0x64);	/* 100 samples/sec */
	psm_write(ioport, PSM_SET_STREAM);
#endif
	psm_poll_status(ioport);
	outb(ioport+PSM_CNTRL, PSM_DISABLE);
	psm_command(ioport, PSM_INT_DISABLE);

	/* Setup initial state */
	sc->state = 0;
	kdc_psm[unit].kdc_state = DC_IDLE;

	/* Done */
	return (0); /* XXX eh? usually 1 indicates success */
}

static int
psmopen(dev_t dev, int flag, int fmt, struct proc *p)
{
	struct psm_softc *sc;
	int ioport;
	int unit = PSMUNIT(dev);

	/* Validate unit number */
	if (unit >= NPSM)
		return (ENXIO);

	/* Get device data */
	sc = &psm_softc[unit];
	ioport = psmaddr[unit];

	/* If device does not exist */
	if (ioport == 0)
		return (ENXIO);

	/* Disallow multiple opens */
	if (sc->state & PSM_OPEN)
		return (EBUSY);

	/* Initialize state */
	kdc_psm[unit].kdc_state = DC_BUSY;
	sc->state |= PSM_OPEN;
	sc->rsel.si_flags = 0;
	sc->rsel.si_pid = 0;
	sc->status = 0;
	sc->button = 0;
	sc->x = 0;
	sc->y = 0;

	/* Allocate and initialize a ring buffer */
	sc->inq.count = sc->inq.first = sc->inq.last = 0;

	/* Enable Bus Mouse interrupts */
	psm_write_dev(ioport, PSM_DEV_ENABLE);
	
	psm_poll_status(ioport);
	outb(ioport+PSM_CNTRL, PSM_ENABLE);
	psm_command(ioport, PSM_INT_ENABLE);

	/* Successful open */
#ifdef	DEVFS
	sc->devfs_token = 
		devfs_add_devswf(&psm_cdevsw, unit << 1, DV_CHR, 0, 0, 0666, 
				 "psm%d", unit);
	sc->n_devfs_token = 
		devfs_add_devswf(&psm_cdevsw, (unit<<1)+1, DV_CHR,0, 0, 0666,
				 "npsm%d", unit);
#endif

	return (0);
}

static void
psm_poll_status(int ioport)
{
	u_char c;

	while(c = inb(ioport+PSM_STATUS) & 0x03)
		if(c & PSM_OUTPUT_ACK == PSM_OUTPUT_ACK) {
			/* XXX - Avoids some keyboard hangs during probe */
			DELAY(6);
			inb(ioport+PSM_DATA);
	}
	return;
}

static int
psmclose(dev_t dev, int flag, int fmt, struct proc *p)
{
	int unit, ioport;
	struct psm_softc *sc;

	/* Get unit and associated info */
	unit = PSMUNIT(dev);
	sc = &psm_softc[unit];
	ioport = psmaddr[unit];

	/* Disable further mouse interrupts */
	psm_command(ioport, PSM_INT_DISABLE);
	psm_poll_status(ioport);
	outb(ioport+PSM_CNTRL, PSM_DISABLE);

	/* Complete the close */
	sc->state &= ~PSM_OPEN;
	kdc_psm[unit].kdc_state = DC_IDLE;

	/* close is almost always successful */
	return (0);
}

static int
psmread(dev_t dev, struct uio *uio, int flag)
{
	int s;
	int error = 0;	/* keep compiler quiet, even though initialisation
			   is unnecessary */
	unsigned length;
	struct psm_softc *sc;
	unsigned char buffer[PSM_CHUNK];

	/* Get device information */
	sc = &psm_softc[PSMUNIT(dev)];

	/* Block until mouse activity occured */
	s = spltty();
	while (sc->inq.count == 0) {
		if (minor(dev) & 0x1) {
			splx(s);
			return (EWOULDBLOCK);
		}
		sc->state |= PSM_ASLP;
		error = tsleep((caddr_t)sc, PZERO | PCATCH, "psmrea", 0);
		if (error != 0) {
			splx(s);
			return (error);
		}
	}

	/* Transfer as many chunks as possible */
	while (sc->inq.count > 0 && uio->uio_resid > 0) {
		length = min(sc->inq.count, uio->uio_resid);
		if (length > sizeof(buffer))
			length = sizeof(buffer);

		/* Remove a small chunk from input queue */
		if (sc->inq.first + length >= PSM_BSIZE) {
			bcopy(&sc->inq.queue[sc->inq.first],
		 	      buffer, PSM_BSIZE - sc->inq.first);
			bcopy(sc->inq.queue, &buffer[PSM_BSIZE - sc->inq.first],
			      length - (PSM_BSIZE - sc->inq.first));
		}
		else
			bcopy(&sc->inq.queue[sc->inq.first], buffer, length);

		sc->inq.first = (sc->inq.first + length) % PSM_BSIZE;
		sc->inq.count -= length;

		/* Copy data to user process */
		error = uiomove(buffer, length, uio);
		if (error)
			break;
	}
	sc->x = sc->y = 0;

	/* Allow interrupts again */
	splx(s);
	return (error);
}

static int
psmioctl(dev_t dev, int cmd, caddr_t addr, int flag, struct proc *p)
{
	struct psm_softc *sc;
	struct mouseinfo info;
	int s, error;

	/* Get device information */
	sc = &psm_softc[PSMUNIT(dev)];

	/* Perform IOCTL command */
	switch (cmd) {

	case MOUSEIOCREAD:
		/* Don't modify info while calculating */
		s = spltty();

		/* Build mouse status octet */
		info.status = sc->status;
		if (sc->x || sc->y)
			info.status |= MOVEMENT;

		/* Encode X and Y motion as good as we can */
		if (sc->x > 127)
			info.xmotion = 127;
		else if (sc->x < -128)
			info.xmotion = -128;
		else
			info.xmotion = sc->x;

		if (sc->y > 127)
			info.ymotion = 127;
		else if (sc->y < -128)
			info.ymotion = -128;
		else
			info.ymotion = sc->y;

		/* Reset historical information */
		sc->x = 0;
		sc->y = 0;
		sc->status &= ~BUTCHNGMASK;

		/* Allow interrupts and copy result buffer */
		splx(s);
		error = copyout(&info, addr, sizeof(struct mouseinfo));
		break;

	default:
		error = EINVAL;
		break;
	}

	/* Return error code */
	return (error);
}

void
psmintr(int unit)
{
	struct psm_softc *sc = &psm_softc[unit];
	int ioport = psmaddr[unit];

	sc->inq.queue[sc->inq.last++ % PSM_BSIZE] = inb(ioport+PSM_DATA);
	sc->inq.count++;
	if (sc -> state & PSM_ASLP) {
		sc->state &= ~PSM_ASLP;
		wakeup((caddr_t)sc);
	}
	selwakeup(&sc->rsel);
}

static int
psmselect(dev_t dev, int rw, struct proc *p)
{
	int s, ret;
	struct psm_softc *sc = &psm_softc[PSMUNIT(dev)];

	/* Silly to select for output */
	if (rw == FWRITE)
		return (0);

	/* Return true if a mouse event available */
	s = spltty();
	if (sc->inq.count)
		ret = 1;
	else {
		selrecord(p, &sc->rsel);
		ret = 0;
	}
	splx(s);

	return (ret);
}


static psm_devsw_installed = 0;

static void
psm_drvinit(void *unused)
{
	dev_t dev;

	if( ! psm_devsw_installed ) {
		dev = makedev(CDEV_MAJOR, 0);
		cdevsw_add(&dev,&psm_cdevsw, NULL);
		psm_devsw_installed = 1;
    	}
}

SYSINIT(psmdev,SI_SUB_DRIVERS,SI_ORDER_MIDDLE+CDEV_MAJOR,psm_drvinit,NULL)

#endif /* NPSM > 0 */
