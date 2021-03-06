/*
 * arch/i86/drivers/char/tty.c - A simple tty driver
 * (C) 1997 Chad Page et. al
 *
 * Modified by Greg Haerr <greg@censoft.com> for screen editors
 * 
 * 03/27/2001 : Modified for raw mode support (HarKal)
 */

/* 
 * This new tty sub-system builds around the character queue to provide a
 * VFS interface to the character drivers (what a mouthful! :)  
 */

#include <linuxmt/types.h>
#include <linuxmt/config.h>
#include <linuxmt/sched.h>
#include <linuxmt/fs.h>
#include <linuxmt/fcntl.h>
#include <linuxmt/errno.h>
#include <linuxmt/mm.h>
#include <linuxmt/termios.h>
#include <linuxmt/chqueue.h>
#include <linuxmt/ntty.h>
#include <linuxmt/init.h>
#include <linuxmt/debug.h>
#include <linuxmt/heap.h>

/* default termios, set at init time, not reset at open*/
struct termios def_vals = {
    BRKINT|ICRNL,					/* c_iflag*/
    OPOST|ONLCR,					/* c_oflag*/
    (tcflag_t) (B9600 | CS8),				/* c_cflag*/
    (tcflag_t) (ISIG | ICANON | ECHO | ECHOE),		/* c_lflag*/
    0,							/* c_line*/
    { 3,	/* VINTR*/
    28,		/* VQUIT*/
    /*127*/ 8,	/* VERASE*/
    21,		/* VKILL*/
    4,		/* VEOF*/
    0,		/* VTIME*/
    1,		/* VMIN*/
    0,		/* VSWTC*/
    17,		/* VSTART*/
    19,		/* VSTOP*/
    26,		/* VSUSP*/
    0,		/* VEOL*/
    18,		/* VREPRINT*/
    15,		/* VDISCARD*/
    23,		/* VWERASE*/
    22,		/* VLNEXT*/
    0,		/* VEOL2*/
    0,
    0 }
};

#define TAB_SPACES 8

struct tty ttys[MAX_TTYS];
#if defined(CONFIG_CONSOLE_DIRECT) || defined(CONFIG_SIBO_CONSOLE_DIRECT)
extern struct tty_ops dircon_ops;
#endif
#ifdef CONFIG_CONSOLE_BIOS
extern struct tty_ops bioscon_ops;
#endif
#ifdef CONFIG_CHAR_DEV_RS
extern struct tty_ops rs_ops;
#endif
#ifdef CONFIG_PSEUDO_TTY
extern struct tty_ops ttyp_ops;
#endif

int tty_intcheck(register struct tty *ttyp, unsigned char key)
{
    sig_t sig = 0;

    if ((ttyp->termios.c_lflag & ISIG) && ttyp->pgrp) {
	if (key == ttyp->termios.c_cc[VINTR])
	    sig = SIGINT;
	if (key == ttyp->termios.c_cc[VSUSP])
	    sig = SIGTSTP;
	if (sig) {
	    debug_tty("TTY signal %d to pgrp %d pid %d\n", sig, ttyp->pgrp, current->pid);
	    kill_pg(ttyp->pgrp, sig, 1);
	}
    }
    return sig;
}

/* Turns a dev_t variable into its tty, or NULL if it's not valid */

struct tty *determine_tty(dev_t dev)
{
    register struct tty *ttyp = &ttys[0];
    unsigned short minor = MINOR(dev);

    /* handle /dev/tty*/
    if (minor == 255 && current->pgrp && (current->pgrp == ttyp->pgrp))
	return current->tty;

    do {
	if (ttyp->minor == minor)
	    return ttyp;
    } while (++ttyp < &ttys[MAX_TTYS]);

    return NULL;
}

int tty_allocq(struct tty *tty, int insize, int outsize)
{
    if ((tty->inq_buf = heap_alloc(insize, HEAP_TAG_TTY)) == NULL)
	return -ENOMEM;
    if ((tty->outq_buf = heap_alloc(outsize, HEAP_TAG_TTY)) == NULL) {
	heap_free(tty->inq_buf);
	return -ENOMEM;
    }
    chq_init(&tty->inq, tty->inq_buf, insize);
    chq_init(&tty->outq, tty->outq_buf, outsize);
    return 0;
}

void tty_freeq(struct tty *tty)
{
    heap_free(tty->inq_buf);
    heap_free(tty->outq_buf);
}

int tty_open(struct inode *inode, struct file *file)
{
    register struct tty *otty;
    register __ptask currentp = current;
    int err;

    if (!(otty = determine_tty(inode->i_rdev)))
	return -ENODEV;

    debug_tty("TTY open pid %d\n", currentp->pid);
#if 0
    memcpy(&otty->termios, &def_vals, sizeof(struct termios));
#endif

    if ((file->f_flags & O_EXCL) && (otty->flags & TTY_OPEN))
	return -EBUSY;

    /* don't call driver on /dev/tty open*/
    if (MINOR(inode->i_rdev) == 255)
	return 0;

    err = otty->ops->open(otty);
    if (!err) {
	if (!(file->f_flags & O_NOCTTY) && currentp->session == currentp->pid && otty->pgrp == 0
		&& currentp->tty == NULL) {
	    debug_tty("TTY setting pgrp %d pid %d\n", currentp->pgrp, currentp->pid);
	    otty->pgrp = currentp->pgrp;
	    currentp->tty = otty;
	}
	otty->flags |= TTY_OPEN;
    }
    return err;
}

void tty_release(struct inode *inode, struct file *file)
{
    register struct tty *rtty;

    rtty = determine_tty(inode->i_rdev);
    if (!rtty)
	return;

    debug_tty("TTY close pid %d\n", current->pid);

    /* no action if closing /dev/tty*/
    if (MINOR(inode->i_rdev) == 255)
	return;

    /* don't release pgrp for /dev/tty, only real tty*/
    if (current->pid == rtty->pgrp) {
	debug_tty("TTY release pgrp %d, sending SIGHUP\n", current->pid);
	kill_pg(rtty->pgrp, SIGHUP, 1);
	rtty->pgrp = 0;
    }
    rtty->flags &= ~TTY_OPEN;
    rtty->ops->release(rtty);
}

/*
 * Gets 1 byte from tty output buffer, with processing
 * Used by the function reading bytes from the output
 * buffer to send them to the tty device, using the
 * construct:
 *
 * while (tty->outq.len > 0) {
 * 	ch = tty_outproc(tty);
 * 	write_char_to_device(device, (char)ch);
 * 	cnt++;
 * }
 *
 */
int tty_outproc(register struct tty *tty)
{
    int t_oflag;	/* WARNING: highly arch dependent, termios.c_oflag truncated to 16 bits*/
    int ch;

    ch = tty->outq.base[tty->outq.start];
    if ((t_oflag = (int)tty->termios.c_oflag) & OPOST) {
	switch(tty->ostate & ~0x0F) {
	case 0x20:		/* Expand tabs to spaces */
	    ch = ' ';
	    if (--tty->ostate & 0xF)
		break;
	case 0x10:		/* Expand NL -> CR NL */
	    tty->ostate = 0;
	    break;
	default:
	    switch(ch) {
	    case '\n':
		if (t_oflag & ONLCR) {
		    ch = '\r';				/* Expand NL -> CR NL */
		    tty->ostate = 0x10;
		}
		break;
#if 0
	    case '\r':
		if (t_oflag & OCRNL)
		    ch = '\n';				/* Map CR to NL */
		break;
#endif
	    case '\t':
		if ((t_oflag & TABDLY) == TAB3) {
		    ch = ' ';				/* Expand tabs to spaces */
		    tty->ostate = 0x20 + TAB_SPACES - 1;
		}
	    }
	}
    }
    if (!tty->ostate) {
	tty->outq.start++;
	tty->outq.start &= (tty->outq.size - 1);
	tty->outq.len--;
    }
    return ch;
}

void tty_echo(register struct tty *tty, unsigned char ch)
{
    if ((tty->termios.c_lflag & ECHO)
		|| ((tty->termios.c_lflag & ECHONL) && (ch == '\n'))) {
	if ((ch == tty->termios.c_cc[VERASE]) && (tty->termios.c_lflag & ECHOE)) {
	    chq_addch(&tty->outq, '\b');
	    chq_addch(&tty->outq, ' ');
	    chq_addch(&tty->outq, '\b');
	} else
	    chq_addch(&tty->outq, ch);
	tty->ops->write(tty);
    }
}

/*
 *	Write to a terminal
 *
 */

size_t tty_write(struct inode *inode, struct file *file, char *data, size_t len)
{
    register struct tty *tty = determine_tty(inode->i_rdev);
    int i, s;

    i = 0;
    while (i < len) {
	s = chq_wait_wr(&tty->outq, file->f_flags & O_NONBLOCK);
	if (s < 0) {
	    if (i == 0)
		i = s;
	    break;
	}
	chq_addch(&tty->outq, get_user_char((void *)(data++)));
	tty->ops->write(tty);
	i++;
    }
    return i;
}

size_t tty_read(struct inode *inode, struct file *file, char *data, size_t len)
{
    register struct tty *tty = determine_tty(inode->i_rdev);
    int icanon = tty->termios.c_lflag & ICANON;
    int vmin = tty->termios.c_cc[VMIN];
    int vtime = tty->termios.c_cc[VTIME];
    int nonblock = (file->f_flags & O_NONBLOCK) || (!icanon && vtime && !vmin);
    jiff_t timeout;
    int i = 0;
    int ch, k;

    while (i < len) {
	timeout = jiffies + vtime * (HZ / 10);
again:
	if (tty->ops->read) {
	    tty->ops->read(tty);
	    nonblock = 1;
	}

	if (chq_peekch(&tty->inq))
	    ch = chq_getch(&tty->inq);
	else {
	    if (!icanon && !vtime && (i >= vmin))
		break;
	    ch = chq_wait_rd(&tty->inq, nonblock);
	    if (ch < 0) {
		if (!icanon && vtime) {
		    if (jiffies < timeout) {
			schedule();
			goto again;		/* don't reset timer*/
		    } else {
			if (vmin && (i == 0)) {
			    nonblock = 1;
			    continue;		/* VMIN > 0 reset timer*/
			}
		    }
		    ch = 0;	/* return 0 on VTIME timeout*/
		}
		if (i == 0)
		    i = ch;
		break;
	    }
	    ch = chq_getch(&tty->inq);
	}

	    if (icanon) {
		if (ch == tty->termios.c_cc[VERASE]) {
		    if (i > 0) {
			i--;
			k = ((get_user_char((void *)(--data))
			    == '\t') ? TAB_SPACES : 1);
			do {
			    tty_echo(tty, ch);
			} while (--k);
		    }
		    continue;
		}
		if ((tty->termios.c_iflag & ICRNL) && (ch == '\r'))
		    ch = '\n';

		if (ch == tty->termios.c_cc[VEOF])
		    break;
	    } else {
		if (vtime && vmin)	/* start timeout after first character*/
		    nonblock = 1;
	    }
	    put_user_char(ch, (void *)(data++));
	    tty_echo(tty, ch);
	    i++;
	    if (icanon && ((ch == '\n') || (ch == tty->termios.c_cc[VEOL])))
		break;
    }
    return i;
}

int tty_ioctl(struct inode *inode, struct file *file, int cmd, char *arg)
{
    register struct tty *tty = determine_tty(inode->i_rdev);
    int ret;

    switch (cmd) {
    case TCGETS:
	ret = verified_memcpy_tofs(arg, &tty->termios, sizeof(struct termios));
	break;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
	ret = verified_memcpy_fromfs(&tty->termios, arg, sizeof(struct termios));

	/* Inform subdriver of new settings*/
	if (ret == 0 && tty->ops->ioctl != NULL)
	    ret = tty->ops->ioctl(tty, cmd, arg);
	break;
    default:
	ret = ((tty->ops->ioctl == NULL)
	    ? -EINVAL
	    : tty->ops->ioctl(tty, cmd, arg));
    }
    return ret;
}

int tty_select(struct inode *inode, struct file *file, int sel_type)
{
    register struct tty *tty = determine_tty(inode->i_rdev);
    int ret = 0;

    switch (sel_type) {
    case SEL_IN:
	if (tty->inq.len != 0)
	    return 1;
	select_wait(&tty->inq.wait);
	break;
    case SEL_OUT:
	ret = (tty->outq.len != tty->outq.size);
	if (!ret)
	    select_wait(&tty->outq.wait);
	    /*@fallthrough@*/
/*      case SEL_EX: */
/*  	break; */
    }
    return ret;
}

/*@-type@*/

static struct file_operations tty_fops = {
    pipe_lseek,			/* Same behavoir, return -ESPIPE */
    tty_read,
    tty_write,
    NULL,
    tty_select,			/* Select - needs doing */
    tty_ioctl,			/* ioctl */
    tty_open,
    tty_release
#ifdef BLOAT_FS
	,
    NULL,
    NULL,
    NULL
#endif
};

void tty_init(void)
{
    register struct tty *ttyp;
    int i;

    ttyp = &ttys[MAX_TTYS];
    do {
	ttyp--;
	ttyp->minor = (unsigned short int)(-1L); /* set unsigned to -1 */
	memcpy(&ttyp->termios, &def_vals, sizeof(struct termios));
    } while (ttyp > ttys);

#if defined(CONFIG_CONSOLE_DIRECT) || defined(CONFIG_SIBO_CONSOLE_DIRECT) || defined(CONFIG_CONSOLE_BIOS)

    for (i = TTY_MINOR_OFFSET ; i < NR_CONSOLES + TTY_MINOR_OFFSET ; i++) {
#ifdef CONFIG_CONSOLE_BIOS
	ttyp->ops = &bioscon_ops;
#else
	ttyp->ops = &dircon_ops;
#endif
	(ttyp++)->minor = i;
    }
#endif

#ifdef CONFIG_CHAR_DEV_RS
    /* put serial entries after console entries */
    for (i = RS_MINOR_OFFSET; i < NR_SERIAL + RS_MINOR_OFFSET; i++) {
	ttyp->ops = &rs_ops;
	(ttyp++)->minor = i;		/* ttyS0 = RS_MINOR_OFFSET */
    }
#endif

#ifdef CONFIG_PSEUDO_TTY
    /* start at minor = 8 fixed to match pty entries in MAKEDEV */
    /* put slave pseudo tty entries after serial entries */
    for (i = PTY_MINOR_OFFSET; (i) < NR_PTYS + PTY_MINOR_OFFSET; i++) {
	ttyp->ops = &ttyp_ops;
	(ttyp++)->minor = i;
    }
#endif

    register_chrdev(TTY_MAJOR, "tty", &tty_fops);
}
