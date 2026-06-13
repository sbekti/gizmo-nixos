// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/io.h>
#include <unistd.h>

#define WATCHDOG_PORT 0x029c
#define WATCHDOG_DISABLE_VALUE 0x0f

static int usage(char const *name, int rc)
{
	FILE *stream = rc == 0 ? stdout : stderr;

	fprintf(stream, "usage: %s <-d|-q>\n", name);
	fprintf(stream, "  -d: disable the CPLD watchdog\n");
	fprintf(stream, "  -q: read the watchdog control port\n");
	return rc;
}

static int allow_watchdog_port(void)
{
	if (ioperm(WATCHDOG_PORT, 1, 1) != 0) {
		fprintf(stderr, "ioperm(0x%04x): %s\n", WATCHDOG_PORT, strerror(errno));
		return errno ? errno : 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int opt;
	int disable = 0;
	int query = 0;
	int ret;

	while ((opt = getopt(argc, argv, "dhq")) != -1) {
		switch (opt) {
		case 'd':
			disable = 1;
			break;
		case 'q':
			query = 1;
			break;
		case 'h':
			return usage(argv[0], 0);
		default:
			return usage(argv[0], 2);
		}
	}

	if (optind != argc || disable == query)
		return usage(argv[0], 2);

	ret = allow_watchdog_port();
	if (ret != 0)
		return ret;

	if (disable) {
		outb(WATCHDOG_DISABLE_VALUE, WATCHDOG_PORT);
		printf("watchdog disabled: wrote 0x%02x to 0x%04x\n",
		       WATCHDOG_DISABLE_VALUE, WATCHDOG_PORT);
	} else {
		printf("watchdog port 0x%04x: 0x%02x\n", WATCHDOG_PORT,
		       inb(WATCHDOG_PORT));
	}

	if (ioperm(WATCHDOG_PORT, 1, 0) != 0) {
		fprintf(stderr, "ioperm release(0x%04x): %s\n", WATCHDOG_PORT,
			strerror(errno));
		return errno ? errno : 1;
	}

	return 0;
}
