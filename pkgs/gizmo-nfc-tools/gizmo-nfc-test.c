// SPDX-License-Identifier: MIT
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define PN544_MAGIC 0xe9
#define PN544_SET_PWR _IOW(PN544_MAGIC, 0x01, unsigned int)
#define PN54X_CLK_REQ _IOW(PN544_MAGIC, 0x02, unsigned int)

#define PN544_PWR_OFF 0
#define PN544_PWR_ON 1
#define PN544_PWR_FW 2

static const uint8_t nci_core_reset[] = { 0x20, 0x00, 0x01, 0x00 };
static const uint8_t nci_core_init[] = { 0x20, 0x01, 0x00 };
static const uint8_t nci_rf_discover_map[] = {
	0x21, 0x00, 0x0d,
	0x04,
	0x01, 0x01, 0x01,
	0x02, 0x01, 0x01,
	0x03, 0x01, 0x01,
	0x04, 0x01, 0x02,
};
static const uint8_t nci_rf_discover[] = {
	0x21, 0x03, 0x07,
	0x03,
	0x00, 0x01,
	0x01, 0x01,
	0x02, 0x01,
};
static const uint8_t nci_rf_deactivate_idle[] = { 0x21, 0x06, 0x01, 0x00 };

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--device /dev/pn544] [--power-only] [--nci-reset]\n"
		"          [--nci-discover] [--seconds N] [--firmware-mode]\n"
		"\n"
		"Default behavior powers the controller off, powers it on normally,\n"
		"then performs a nonblocking read probe.\n"
		"\n"
		"--nci-reset sends NCI CORE_RESET and expects a response.\n"
		"--nci-discover sends CORE_RESET, CORE_INIT, RF_DISCOVER_MAP, and\n"
		"RF_DISCOVER, then prints NFC frames while you wave a tag/card.\n",
		argv0);
}

static void dump_hex(const char *label, const uint8_t *buf, ssize_t len)
{
	ssize_t i;

	printf("%s (%zd bytes):", label, len);
	for (i = 0; i < len; i++)
		printf(" %02x", buf[i]);
	printf("\n");
	fflush(stdout);
}

static ssize_t nci_frame_len(const uint8_t *buf, ssize_t len)
{
	ssize_t frame_len;

	if (len < 3)
		return len;

	frame_len = 3 + buf[2];
	if (frame_len <= len)
		return frame_len;

	return len;
}

static void describe_nci_frame(const uint8_t *buf, ssize_t len)
{
	if (len < 3)
		return;

	if (buf[0] == 0x40 && buf[1] == 0x00 && len >= 4)
		printf("  decoded: CORE_RESET_RSP status=0x%02x\n", buf[3]);
	else if (buf[0] == 0x60 && buf[1] == 0x00)
		printf("  decoded: CORE_RESET_NTF\n");
	else if (buf[0] == 0x40 && buf[1] == 0x01 && len >= 4)
		printf("  decoded: CORE_INIT_RSP status=0x%02x\n", buf[3]);
	else if (buf[0] == 0x41 && buf[1] == 0x00 && len >= 4)
		printf("  decoded: RF_DISCOVER_MAP_RSP status=0x%02x\n", buf[3]);
	else if (buf[0] == 0x41 && buf[1] == 0x03 && len >= 4)
		printf("  decoded: RF_DISCOVER_RSP status=0x%02x\n", buf[3]);
	else if (buf[0] == 0x61 && buf[1] == 0x05)
		printf("  decoded: RF_INTF_ACTIVATED_NTF; tag/card detected\n");
	else if (buf[0] == 0x61 && buf[1] == 0x06)
		printf("  decoded: RF_DEACTIVATE_NTF\n");
}

static int set_power(int fd, unsigned int value, const char *label)
{
	if (ioctl(fd, PN544_SET_PWR, &value) < 0) {
		fprintf(stderr, "PN544_SET_PWR(%s=%u) failed: %s\n",
			label, value, strerror(errno));
		return -1;
	}

	printf("PN544_SET_PWR(%s=%u) ok\n", label, value);
	return 0;
}

static ssize_t read_frame_timeout(int fd, const char *label, int timeout_ms,
				  uint8_t *buf, size_t len)
{
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN,
	};
	ssize_t ret;

	ret = poll(&pfd, 1, timeout_ms);
	if (ret < 0) {
		fprintf(stderr, "%s: poll failed: %s\n", label, strerror(errno));
		return -1;
	}
	if (ret == 0) {
		printf("%s: timed out after %d ms\n", label, timeout_ms);
		return 0;
	}
	if (!(pfd.revents & POLLIN)) {
		printf("%s: poll returned revents=0x%x without POLLIN\n",
		       label, pfd.revents);
		return 0;
	}

	ret = read(fd, buf, len);
	if (ret < 0) {
		if (errno == EAGAIN) {
			printf("%s: no frame pending (EAGAIN)\n", label);
			return 0;
		}
		fprintf(stderr, "%s: read failed: %s\n", label, strerror(errno));
		return -1;
	}

	ret = nci_frame_len(buf, ret);
	dump_hex(label, buf, ret);
	describe_nci_frame(buf, ret);
	return ret;
}

static int read_once(int fd, const char *label)
{
	uint8_t buf[512];
	ssize_t ret;

	ret = read(fd, buf, sizeof(buf));
	if (ret < 0) {
		if (errno == EAGAIN) {
			printf("%s: no frame pending (EAGAIN)\n", label);
			return 0;
		}
		fprintf(stderr, "%s: read failed: %s\n", label, strerror(errno));
		return -1;
	}

	ret = nci_frame_len(buf, ret);
	dump_hex(label, buf, ret);
	describe_nci_frame(buf, ret);
	return 0;
}

static int write_frame(int fd, const char *label, const uint8_t *buf, size_t len)
{
	ssize_t ret;

	ret = write(fd, buf, len);
	if (ret < 0) {
		fprintf(stderr, "%s write failed: %s\n", label, strerror(errno));
		return -1;
	}
	if ((size_t)ret != len) {
		fprintf(stderr, "%s short write: %zd/%zu\n", label, ret, len);
		return -1;
	}

	dump_hex(label, buf, ret);
	return 0;
}

static int send_nci_command(int fd, const char *label,
			    const uint8_t *cmd, size_t cmd_len,
			    uint8_t rsp_mt_gid, uint8_t rsp_oid)
{
	uint8_t buf[512];
	int waited_ms = 0;

	if (write_frame(fd, label, cmd, cmd_len) < 0)
		return -1;

	while (waited_ms < 1500) {
		ssize_t len = read_frame_timeout(fd, "read NCI frame", 500,
						 buf, sizeof(buf));

		if (len < 0)
			return -1;
		waited_ms += 500;
		if (len >= 2 && buf[0] == rsp_mt_gid && buf[1] == rsp_oid)
			return 0;
	}

	fprintf(stderr, "%s: did not receive expected response %02x %02x\n",
		label, rsp_mt_gid, rsp_oid);
	return -1;
}

static int nci_reset(int fd)
{
	uint8_t buf[512];
	int ret;

	ret = send_nci_command(fd, "wrote NCI CORE_RESET",
			       nci_core_reset, sizeof(nci_core_reset),
			       0x40, 0x00);
	if (ret < 0)
		return ret;

	/* Some controllers send a reset notification after the reset response. */
	(void)read_frame_timeout(fd, "optional post-reset NCI frame", 250,
				  buf, sizeof(buf));
	return 0;
}

static int nci_discover(int fd, int seconds)
{
	uint8_t buf[512];
	time_t end;
	bool detected = false;

	if (nci_reset(fd) < 0)
		return -1;
	if (send_nci_command(fd, "wrote NCI CORE_INIT",
			     nci_core_init, sizeof(nci_core_init),
			     0x40, 0x01) < 0)
		return -1;
	if (send_nci_command(fd, "wrote NCI RF_DISCOVER_MAP",
			     nci_rf_discover_map, sizeof(nci_rf_discover_map),
			     0x41, 0x00) < 0)
		return -1;
	if (send_nci_command(fd, "wrote NCI RF_DISCOVER",
			     nci_rf_discover, sizeof(nci_rf_discover),
			     0x41, 0x03) < 0)
		return -1;

	printf("NCI RF discovery is active for %d seconds; wave a tag/card now.\n",
	       seconds);
	end = time(NULL) + seconds;
	while (time(NULL) < end) {
		ssize_t len = read_frame_timeout(fd, "NCI discovery event", 500,
						 buf, sizeof(buf));

		if (len < 0)
			return -1;
		if (len >= 2 && buf[0] == 0x61 && buf[1] == 0x05)
			detected = true;
	}

	if (write_frame(fd, "wrote NCI RF_DEACTIVATE idle",
			nci_rf_deactivate_idle,
			sizeof(nci_rf_deactivate_idle)) == 0) {
		(void)read_frame_timeout(fd, "read NCI deactivate response",
					  500, buf, sizeof(buf));
	}

	if (detected)
		printf("NCI discovery saw at least one RF activation.\n");
	else
		printf("NCI discovery completed without an RF activation notification.\n");

	return 0;
}

int main(int argc, char **argv)
{
	const char *device = "/dev/pn544";
	bool power_only = false;
	bool do_nci_reset = false;
	bool do_nci_discover = false;
	bool firmware_mode = false;
	int discover_seconds = 15;
	int fd;
	int i;
	int exit_code = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			device = argv[++i];
		} else if (!strcmp(argv[i], "--power-only")) {
			power_only = true;
		} else if (!strcmp(argv[i], "--nci-reset")) {
			do_nci_reset = true;
		} else if (!strcmp(argv[i], "--nci-discover")) {
			do_nci_discover = true;
		} else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			discover_seconds = atoi(argv[++i]);
			if (discover_seconds < 1 || discover_seconds > 120) {
				fprintf(stderr, "--seconds must be 1..120\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "--firmware-mode")) {
			firmware_mode = true;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	fd = open(device, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", device, strerror(errno));
		return 1;
	}
	printf("opened %s\n", device);

	if (set_power(fd, PN544_PWR_OFF, "off") < 0)
		exit_code = 1;
	usleep(10000);

	if (set_power(fd, firmware_mode ? PN544_PWR_FW : PN544_PWR_ON,
		      firmware_mode ? "firmware" : "on") < 0)
		exit_code = 1;

	if (!power_only && exit_code == 0) {
		if (do_nci_discover)
			exit_code = nci_discover(fd, discover_seconds) < 0 ? 1 : 0;
		else if (do_nci_reset)
			exit_code = nci_reset(fd) < 0 ? 1 : 0;
		else
			exit_code = read_once(fd, "nonblocking read probe") < 0 ? 1 : 0;
	}

	close(fd);
	return exit_code;
}
