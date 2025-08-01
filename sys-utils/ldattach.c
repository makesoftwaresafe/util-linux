/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Line discipline loading daemon open a serial device and attach a line
 * discipline on it.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include "c.h"
#include "cctype.h"
#include "all-io.h"
#include "nls.h"
#include "strutils.h"
#include "closestream.h"

#include <signal.h>
#include <sys/socket.h>
#include <linux/if.h>

#include <linux/tty.h>		/* for N_GSM0710 */

#ifdef LINUX_GSMMUX_H
# include <linux/gsmmux.h>	/* Add by guowenxue */
#else
struct gsm_config
{
	unsigned int adaption;
	unsigned int encapsulation;
	unsigned int initiator;
	unsigned int t1;
	unsigned int t2;
	unsigned int t3;
	unsigned int n2;
	unsigned int mru;
	unsigned int mtu;
	unsigned int k;
	unsigned int i;
	unsigned int unused[8];		/* Padding for expansion without
					   breaking stuff */
};
# define GSMIOC_GETCONF		_IOR('G', 0, struct gsm_config)
# define GSMIOC_SETCONF		_IOW('G', 1, struct gsm_config)
#endif

#ifndef N_GIGASET_M101
# define N_GIGASET_M101 16
#endif

#ifndef N_PPS
# define N_PPS 18
#endif

#ifndef N_GSM0710
# define N_GSM0710 21
#endif

#define MAXINTROPARMLEN 32

/* attach a line discipline ioctl */
#ifndef TIOCSETD
# define TIOCSETD   0x5423
#endif

static int debug = 0;

struct ld_table {
	const char *name;
	int value;
};

/* currently supported line disciplines, plus some aliases */
static const struct ld_table ld_discs[] = {
	{ "TTY",		N_TTY },
	{ "SLIP",		N_SLIP },
	{ "MOUSE",		N_MOUSE },
	{ "PPP",		N_PPP },
	{ "STRIP",		N_STRIP },
	{ "AX25",		N_AX25 },
	{ "X25",		N_X25 },
	{ "6PACK",		N_6PACK },
	{ "R3964",		N_R3964 },
	{ "IRDA",		N_IRDA },
	{ "HDLC",		N_HDLC },
	{ "SYNC_PPP",		N_SYNC_PPP },
	{ "SYNCPPP",		N_SYNC_PPP },
	{ "HCI",		N_HCI },
	{ "GIGASET_M101",	N_GIGASET_M101 },
	{ "M101",		N_GIGASET_M101 },
	{ "GIGASET",		N_GIGASET_M101 },
	{ "PPS",		N_PPS },
	{ "GSM0710",		N_GSM0710},
	{ NULL,	0 }
};

/* known c_iflag names */
static const struct ld_table ld_iflags[] =
{
	{ "IGNBRK",	IGNBRK },
	{ "BRKINT",	BRKINT },
	{ "IGNPAR",	IGNPAR },
	{ "PARMRK",	PARMRK },
	{ "INPCK",	INPCK },
	{ "ISTRIP",	ISTRIP },
	{ "INLCR",	INLCR },
	{ "IGNCR",	IGNCR },
	{ "ICRNL",	ICRNL },
	{ "IUCLC",	IUCLC },
	{ "IXON",	IXON },
	{ "IXANY",	IXANY },
	{ "IXOFF",	IXOFF },
	{ "IMAXBEL",	IMAXBEL },
	{ "IUTF8",	IUTF8 },
	{ NULL,		0 }
};

static void __attribute__((__format__ (__printf__, 1, 2)))
	dbg(char *fmt, ...)
{
	va_list args;

	if (debug == 0)
		return;
	fflush(NULL);
	va_start(args, fmt);
#ifdef HAVE_VWARNX
	vwarnx(fmt, args);
#else
	fprintf(stderr, "%s: ", program_invocation_short_name);
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
#endif
	va_end(args);
	fflush(NULL);
}

static int lookup_table(const struct ld_table *tab, const char *str)
{
	const struct ld_table *t;

	for (t = tab; t && t->name; t++)
		if (!c_strcasecmp(t->name, str))
			return t->value;
	return -1;
}

static void print_table(FILE * out, const struct ld_table *tab)
{
	const struct ld_table *t;
	int i;

	for (t = tab, i = 1; t && t->name; t++, i++) {
		fprintf(out, "  %-12s", t->name);
		if (!(i % 5))
			fputc('\n', out);
	}
}

static int parse_iflag(char *str, int *set_iflag, int *clr_iflag)
{
	int iflag;
	char *s;

	for (s = strtok(str, ","); s != NULL; s = strtok(NULL, ",")) {
		if (*s == '-')
			s++;
		if ((iflag = lookup_table(ld_iflags, s)) < 0)
			iflag = strtos32_or_err(s, _("invalid iflag"));
		if (s > str && *(s - 1) == '-')
			*clr_iflag |= iflag;
		else
			*set_iflag |= iflag;
	}
	dbg("iflag (set/clear): %d/%d", *set_iflag, *clr_iflag);
	return 0;
}


static void __attribute__((__noreturn__)) usage(void)
{
	FILE *out = stdout;

	fputs(USAGE_HEADER, out);
	fprintf(out, _(" %s [options] <ldisc> <device>\n"), program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Attach a line discipline to a serial line.\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -d, --debug             print verbose messages to stderr\n"), out);
	fputs(_(" -s, --speed <value>     set serial line speed\n"), out);
	fputs(_(" -c, --intro-command <string> intro sent before ldattach\n"), out);
	fputs(_(" -p, --pause <seconds>   pause between intro and ldattach\n"), out);
	fputs(_(" -7, --sevenbits         set character size to 7 bits\n"), out);
	fputs(_(" -8, --eightbits         set character size to 8 bits\n"), out);
	fputs(_(" -n, --noparity          set parity to none\n"), out);
	fputs(_(" -e, --evenparity        set parity to even\n"), out);
	fputs(_(" -o, --oddparity         set parity to odd\n"), out);
	fputs(_(" -1, --onestopbit        set stop bits to one\n"), out);
	fputs(_(" -2, --twostopbits       set stop bits to two\n"), out);
	fputs(_(" -i, --iflag [-]<iflag>  set input mode flag\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fprintf(out, USAGE_HELP_OPTIONS(25));

	fputs(_("\nOptions relevant to GSM0710:\n"), out);
	fputs(_(" -m, --mtu <value>  set CMUX maximum frame size\n"), out);

	fputs(_("\nKnown <ldisc> names:\n"), out);
	print_table(out, ld_discs);
	fputs(USAGE_SEPARATOR, out);

	fputs(_("\nKnown <iflag> names:\n"), out);
	print_table(out, ld_iflags);

	fprintf(out, USAGE_MAN_TAIL("ldattach(8)"));
	exit(EXIT_SUCCESS);
}

static int my_cfsetspeed(struct termios *ts, int speed)
{
	/* Standard speeds
	 * -- cfsetspeed() is able to translate number to Bxxx constants
	 */
	if (cfsetspeed(ts, speed) == 0)
		return 0;

	/* Nonstandard speeds
	 * -- we have to bypass glibc and set the speed manually (because glibc
	 *    checks for speed and supports Bxxx bit rates only)...
	 */
#if _HAVE_STRUCT_TERMIOS_C_ISPEED
# ifndef BOTHER
#  define BOTHER 0010000		/* non standard rate */
# endif
	dbg("using non-standard speeds");
	ts->c_ospeed = ts->c_ispeed = speed;
	ts->c_cflag &= ~CBAUD;
	ts->c_cflag |= BOTHER;
	return 0;
#else
	return -1;
#endif
}

static void handler(int s)
{
	dbg("got SIG %i -> exiting", s);
	_exit(EXIT_SUCCESS);
}

static void gsm0710_set_conf(int tty_fd, unsigned int mtu)
{
	struct gsm_config c;

	/*  get n_gsm configuration */
	ioctl(tty_fd, GSMIOC_GETCONF, &c);
	/*  we are initiator and need encoding 0 (basic) */
	c.initiator = 1;
	c.encapsulation = 0;
	c.mru = mtu;
	c.mtu = mtu;
	/*  set the new configuration */
	ioctl(tty_fd, GSMIOC_SETCONF, &c);
}

int main(int argc, char **argv)
{
	int tty_fd;
	struct termios ts;
	int speed = 0, bits = '-', parity = '-', stop = '-';
	int set_iflag = 0, clr_iflag = 0;
	int ldisc;
	int optc;
	char *dev;
	int intropause = 1;
	char *introparm = NULL;
	unsigned int gsm_mtu = 127; /* traditional default */

	static const struct option opttbl[] = {
		{"speed", required_argument, NULL, 's'},
		{"sevenbits", no_argument, NULL, '7'},
		{"eightbits", no_argument, NULL, '8'},
		{"noparity", no_argument, NULL, 'n'},
		{"evenparity", no_argument, NULL, 'e'},
		{"oddparity", no_argument, NULL, 'o'},
		{"onestopbit", no_argument, NULL, '1'},
		{"twostopbits", no_argument, NULL, '2'},
		{"iflag", required_argument, NULL, 'i'},
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'V'},
		{"debug", no_argument, NULL, 'd'},
	        {"intro-command", required_argument, NULL, 'c'},
	        {"pause", required_argument, NULL, 'p'},
	        {"mtu", required_argument, NULL, 'm'},
		{NULL, 0, NULL, 0}
	};

	signal(SIGKILL, handler);
	signal(SIGINT, handler);

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	close_stdout_atexit();

	/* parse options */
	if (argc == 0)
		errx(EXIT_FAILURE, _("bad usage"));

	while ((optc =
		getopt_long(argc, argv, "dhV78neo12s:i:c:p:m:", opttbl,
			    NULL)) >= 0) {
		switch (optc) {
		case 'd':
			debug = 1;
			break;
		case '1':
		case '2':
			stop = optc;
			break;
		case '7':
		case '8':
			bits = optc;
			break;
		case 'n':
		case 'e':
		case 'o':
			parity = optc;
			break;
		case 's':
			speed = strtos32_or_err(optarg, _("invalid speed argument"));
			break;
		case 'p':
			intropause = strtou32_or_err(optarg, _("invalid pause argument"));
			if (intropause > 10)
				errx(EXIT_FAILURE, "invalid pause: %s", optarg);
			break;
		case 'm':
			gsm_mtu = str2num_or_err(optarg, 10, _("invalid MTU argument"), 1, 32768);
			break;
		case 'c':
			introparm = optarg;
			break;
		case 'i':
			parse_iflag(optarg, &set_iflag, &clr_iflag);
			break;

		case 'V':
			print_version(EXIT_SUCCESS);
		case 'h':
			usage();
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}

	if (argc - optind != 2) {
		warnx(_("not enough arguments"));
		errtryhelp(EXIT_FAILURE);
	}
	/* parse line discipline specification */
	ldisc = lookup_table(ld_discs, argv[optind]);
	if (ldisc < 0)
		ldisc = strtos32_or_err(argv[optind], _("invalid line discipline argument"));

	/* ldisc specific option settings */
	if (ldisc == N_GIGASET_M101) {
		/* device specific defaults for line speed and data format */
		if (speed == 0)
			speed = 115200;
		if (bits == '-')
			bits = '8';
		if (parity == '-')
			parity = 'n';
		if (stop == '-')
			stop = '1';
	}

	/* open device */
	dev = argv[optind + 1];
	if ((tty_fd = open(dev, O_RDWR | O_NOCTTY)) < 0)
		err(EXIT_FAILURE, _("cannot open %s"), dev);
	if (!isatty(tty_fd))
		errx(EXIT_FAILURE, _("%s is not a serial line"), dev);

	dbg("opened %s", dev);

	/* set line speed and format */
	if (tcgetattr(tty_fd, &ts) < 0)
		err(EXIT_FAILURE,
		    _("cannot get terminal attributes for %s"), dev);
	cfmakeraw(&ts);
	if (speed && my_cfsetspeed(&ts, speed) < 0)
		errx(EXIT_FAILURE, _("speed %d unsupported"), speed);

	switch (stop) {
	case '1':
		ts.c_cflag &= ~CSTOPB;
		break;
	case '2':
		ts.c_cflag |= CSTOPB;
		break;
	case '-':
		break;
	default:
		abort();
	}
	switch (bits) {
	case '7':
		ts.c_cflag = (ts.c_cflag & ~CSIZE) | CS7;
		break;
	case '8':
		ts.c_cflag = (ts.c_cflag & ~CSIZE) | CS8;
		break;
	case '-':
		break;
	default:
		abort();
	}
	switch (parity) {
	case 'n':
		ts.c_cflag &= ~(PARENB | PARODD);
		break;
	case 'e':
		ts.c_cflag |= PARENB;
		ts.c_cflag &= ~PARODD;
		break;
	case 'o':
		ts.c_cflag |= (PARENB | PARODD);
		break;
	case '-':
		break;
	default:
		abort();
	}

	ts.c_cflag |= CREAD;	/* just to be on the safe side */
	ts.c_iflag |= set_iflag;
	ts.c_iflag &= ~clr_iflag;

	if (tcsetattr(tty_fd, TCSAFLUSH, &ts) < 0)
		err(EXIT_FAILURE,
		    _("cannot set terminal attributes for %s"), dev);

	dbg("set to raw %d %c%c%c: cflag=0x%x",
	    speed, bits, parity, stop, ts.c_cflag);

	if (introparm && *introparm)
	{
		dbg("intro command is '%s'", introparm);
		if (write_all(tty_fd, introparm, strlen(introparm)) != 0)
			err(EXIT_FAILURE,
			    _("cannot write intro command to %s"), dev);

		if (intropause) {
			dbg("waiting for %d seconds", intropause);
			sleep(intropause);
		}
	}

	/* Attach the line discipline. */
	if (ioctl(tty_fd, TIOCSETD, &ldisc) < 0)
		err(EXIT_FAILURE, _("cannot set line discipline"));

	dbg("line discipline set to %d", ldisc);

	/* ldisc specific post-attach actions */
	if (ldisc == N_GSM0710)
		gsm0710_set_conf(tty_fd, gsm_mtu);

	/* Go into background if not in debug mode. */
	if (!debug && daemon(0, 0) < 0)
		err(EXIT_FAILURE, _("cannot daemonize"));

	/* Sleep to keep the line discipline active. */
	pause();

	exit(EXIT_SUCCESS);
}
