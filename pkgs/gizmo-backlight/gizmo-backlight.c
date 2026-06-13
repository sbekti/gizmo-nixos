// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <unistd.h>

#define CPLD_BASE 0x0280
#define CPLD_GIZMO_BACKLIGHT_OFFSET 0x09
#define BACKLIGHT_PORT (CPLD_BASE + CPLD_GIZMO_BACKLIGHT_OFFSET)

static int usage(char const *name, int rc)
{
	FILE *stream = rc == 0 ? stdout : stderr;

	fprintf(stream, "usage: %s <-q|-s brightness>\n", name);
	fprintf(stream, "  -q: read the CPLD backlight brightness byte\n");
	fprintf(stream, "  -s: set brightness byte, 0..255 or 0x00..0xff\n");
	fprintf(stream, "      0 turns the backlight off; 255 is maximum brightness\n");
	return rc;
}

static int allow_backlight_port(void)
{
	if (ioperm(BACKLIGHT_PORT, 1, 1) != 0) {
		fprintf(stderr, "ioperm(0x%04x): %s\n", BACKLIGHT_PORT,
			strerror(errno));
		return errno ? errno : 1;
	}

	return 0;
}

static int parse_brightness(char const *text, unsigned char *brightness)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 0);
	if (errno != 0 || !text[0] || (end && *end) || value > 255)
		return -EINVAL;

	*brightness = (unsigned char)value;
	return 0;
}

int main(int argc, char **argv)
{
	int opt;
	int query = 0;
	int set = 0;
	unsigned char brightness = 0;
	int ret;

	while ((opt = getopt(argc, argv, "hqs:")) != -1) {
		switch (opt) {
		case 'q':
			query = 1;
			break;
		case 's':
			if (parse_brightness(optarg, &brightness) != 0) {
				fprintf(stderr, "invalid brightness: %s\n", optarg);
				return 2;
			}
			set = 1;
			break;
		case 'h':
			return usage(argv[0], 0);
		default:
			return usage(argv[0], 2);
		}
	}

	if (optind != argc || query == set)
		return usage(argv[0], 2);

	ret = allow_backlight_port();
	if (ret != 0)
		return ret;

	if (set) {
		outb(brightness, BACKLIGHT_PORT);
		printf("backlight brightness: %u (0x%02x)\n", brightness,
		       brightness);
	} else {
		brightness = inb(BACKLIGHT_PORT);
		printf("backlight brightness: %u (0x%02x)\n", brightness,
		       brightness);
	}

	if (ioperm(BACKLIGHT_PORT, 1, 0) != 0) {
		fprintf(stderr, "ioperm release(0x%04x): %s\n", BACKLIGHT_PORT,
			strerror(errno));
		return errno ? errno : 1;
	}

	return 0;
}
