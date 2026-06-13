// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/io.h>
#include <unistd.h>

static unsigned long parse_port(char const *s)
{
	char *end = NULL;
	unsigned long value = strtoul(s, &end, 0);

	if (!s[0] || (end && *end)) {
		fprintf(stderr, "invalid port: %s\n", s);
		exit(2);
	}

	if (value > 0xffff) {
		fprintf(stderr, "port out of range: %#lx\n", value);
		exit(2);
	}

	return value;
}

int main(int argc, char **argv)
{
	unsigned long base;
	unsigned long count;
	unsigned long i;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <base-port> <count>\n", argv[0]);
		return 2;
	}

	base = parse_port(argv[1]);
	count = parse_port(argv[2]);

	if (count == 0 || base + count - 1 > 0xffff) {
		fprintf(stderr, "invalid range: %#lx + %#lx\n", base, count);
		return 2;
	}

	if (ioperm(base, count, 1) != 0) {
		perror("ioperm");
		return errno ? errno : 1;
	}

	for (i = 0; i < count; i++) {
		if (i % 16 == 0)
			printf("%04lx:", base + i);
		printf(" %02x", inb(base + i));
		if (i % 16 == 15 || i + 1 == count)
			putchar('\n');
	}

	if (ioperm(base, count, 0) != 0) {
		perror("ioperm");
		return errno ? errno : 1;
	}

	return 0;
}
