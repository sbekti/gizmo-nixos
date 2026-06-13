// SPDX-License-Identifier: MIT
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef I2C_SLAVE_FORCE
#define I2C_SLAVE_FORCE 0x0706
#endif

#define DEVICES_PER_SIDE 4
#define LEDS_PER_SIDE 12
#define LED_COUNT 24
#define LP55231_CHANNELS 9
#define PRIDE_COLOR_COUNT 11

struct color {
	int red;
	int green;
	int blue;
};

static unsigned short const lp55231_addrs[DEVICES_PER_SIDE] = {
	0x32, 0x33, 0x34, 0x35,
};

static int left_bus = -1;
static int right_bus = -1;
static int brightness = 48;
static int comet = PRIDE_COLOR_COUNT;
static long frame_delay_us = 50000;
static volatile sig_atomic_t keep_running = 1;

static struct color const pride_colors[PRIDE_COLOR_COUNT] = {
	/* Classic rainbow flag */
	{ 255, 0, 0 },
	{ 255, 96, 0 },
	{ 255, 220, 0 },
	{ 0, 180, 0 },
	{ 0, 72, 255 },
	{ 128, 0, 160 },
	/* Trans flag */
	{ 91, 206, 250 },
	{ 245, 169, 184 },
	{ 255, 255, 255 },
	{ 245, 169, 184 },
	{ 91, 206, 250 },
};

static void handle_signal(int signal_number)
{
	(void)signal_number;
	keep_running = 0;
}

static int env_int(char const *name, int fallback)
{
	char const *value = getenv(name);
	char *end = NULL;
	long parsed;

	if (!value || !value[0])
		return fallback;

	parsed = strtol(value, &end, 10);
	if ((end && *end) || parsed < 0 || parsed > 255)
		return fallback;

	return (int)parsed;
}

static long env_long(char const *name, long fallback)
{
	char const *value = getenv(name);
	char *end = NULL;
	long parsed;

	if (!value || !value[0])
		return fallback;

	parsed = strtol(value, &end, 10);
	if ((end && *end) || parsed < 1000)
		return fallback;

	return parsed;
}

static bool read_first_line(char const *path, char *buffer, size_t size)
{
	FILE *file = fopen(path, "r");

	if (!file)
		return false;

	if (!fgets(buffer, (int)size, file)) {
		fclose(file);
		return false;
	}

	buffer[strcspn(buffer, "\n")] = '\0';
	fclose(file);
	return true;
}

static void discover_buses(void)
{
	char path[128];
	char name[128];
	int ocores_bus = -1;

	for (int bus = 0; bus < 64; bus++) {
		snprintf(path, sizeof(path), "/sys/class/i2c-dev/i2c-%d/name", bus);
		if (!read_first_line(path, name, sizeof(name)))
			continue;
		if (strstr(name, "i2c-ocores")) {
			ocores_bus = bus;
			break;
		}
	}

	if (ocores_bus < 0)
		return;

	/*
	 * The hardware notes say mux channel 0 is left and channel 1 is right.
	 * LEFT_BUS/RIGHT_BUS environment overrides are available if a future unit
	 * reports the channels differently.
	 */
	for (int bus = 0; bus < 64; bus++) {
		char expected[64];

		snprintf(path, sizeof(path), "/sys/class/i2c-dev/i2c-%d/name", bus);
		if (!read_first_line(path, name, sizeof(name)))
			continue;

		snprintf(expected, sizeof(expected), "i2c-%d-mux", ocores_bus);
		if (!strstr(name, expected))
			continue;

		if (strstr(name, "chan_id 0"))
			left_bus = bus;
		else if (strstr(name, "chan_id 1"))
			right_bus = bus;
	}
}

static int open_bus(int bus)
{
	char path[32];

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	return open(path, O_RDWR | O_CLOEXEC);
}

static int set_addr(int fd, unsigned short addr)
{
	if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0)
		return -errno;
	return 0;
}

static int write_reg(int fd, uint8_t reg, uint8_t value)
{
	uint8_t data[2] = { reg, value };

	if (write(fd, data, sizeof(data)) != (ssize_t)sizeof(data))
		return -errno;
	return 0;
}

static void init_chip(int fd, unsigned short addr)
{
	if (set_addr(fd, addr) < 0)
		return;

	write_reg(fd, 0x00, 0x40);
	usleep(2000);
	/* Keep 0x7e here: earlier values left only red visibly active on Gizmo. */
	write_reg(fd, 0x36, 0x7e);
	write_reg(fd, 0x04, 0x01);
	write_reg(fd, 0x05, 0xff);

	for (int chan = 0; chan < LP55231_CHANNELS; chan++)
		write_reg(fd, (uint8_t)(0x26 + chan), 50);
}

static void set_pwm(int fd, unsigned short addr, int chan, int value)
{
	if (chan < 0 || chan >= LP55231_CHANNELS)
		return;
	if (set_addr(fd, addr) < 0)
		return;
	write_reg(fd, (uint8_t)(0x16 + chan), (uint8_t)value);
}

static void logical_to_target(int led, int *bus, unsigned short *addr, int *slot)
{
	int side_led;
	int device;

	/*
	 * Walk a continuous 24-LED ring: right side first, then the left side in
	 * reverse physical order so the chase does not jump at the side boundary.
	 */
	if (led < LEDS_PER_SIDE) {
		*bus = right_bus;
		side_led = led;
	} else {
		*bus = left_bus;
		side_led = (LED_COUNT - 1) - led;
	}

	device = side_led / 3;
	*slot = side_led % 3;
	*addr = lp55231_addrs[device];
}

static void set_logical_led(int left_fd, int right_fd, int led, int red, int green, int blue)
{
	unsigned short addr;
	int bus;
	int fd;
	int slot;

	logical_to_target(led, &bus, &addr, &slot);
	fd = (bus == left_bus) ? left_fd : right_fd;

	if (fd < 0)
		return;

	/* LP55231 channel order on this board is 0-2 blue, 3-5 green, 6-8 red. */
	set_pwm(fd, addr, 0 + slot, blue);
	set_pwm(fd, addr, 3 + slot, green);
	set_pwm(fd, addr, 6 + slot, red);
}

static void pride_color(int index, int level, int *red, int *green, int *blue)
{
	struct color color = pride_colors[index % PRIDE_COLOR_COUNT];

	*red = color.red * level / 255;
	*green = color.green * level / 255;
	*blue = color.blue * level / 255;
}

static void all_off(int left_fd, int right_fd)
{
	for (int led = 0; led < LED_COUNT; led++)
		set_logical_led(left_fd, right_fd, led, 0, 0, 0);
}

int main(void)
{
	int left_fd;
	int right_fd;
	int frame = 0;

	brightness = env_int("MAX_BRIGHTNESS", brightness);
	comet = env_int("COMET_LENGTH", comet);
	frame_delay_us = env_long("FRAME_DELAY_US", frame_delay_us);
	left_bus = env_int("LEFT_BUS", -1);
	right_bus = env_int("RIGHT_BUS", -1);

	if (comet < 1)
		comet = 1;
	if (comet > LED_COUNT)
		comet = LED_COUNT;

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	for (int attempt = 0; attempt < 100 && keep_running; attempt++) {
		if (left_bus < 0 || right_bus < 0)
			discover_buses();
		if (left_bus >= 0 && right_bus >= 0)
			break;
		usleep(200000);
	}

	if (left_bus < 0 || right_bus < 0) {
		fprintf(stderr, "gizmo-led-rainbow: LED mux buses not found; exiting\n");
		return 0;
	}

	left_fd = open_bus(left_bus);
	right_fd = open_bus(right_bus);
	if (left_fd < 0 || right_fd < 0) {
		fprintf(stderr, "gizmo-led-rainbow: could not open LED I2C buses left=%d right=%d\n",
			left_bus, right_bus);
		return 0;
	}

	for (int dev = 0; dev < DEVICES_PER_SIDE; dev++) {
		init_chip(left_fd, lp55231_addrs[dev]);
		init_chip(right_fd, lp55231_addrs[dev]);
	}

	all_off(left_fd, right_fd);

	while (keep_running) {
		int head = frame % LED_COUNT;

		for (int i = 0; i < comet; i++) {
			int led = (head - i + LED_COUNT) % LED_COUNT;
			int level = brightness * (comet - i) / comet;
			int red;
			int green;
			int blue;

			pride_color(i, level, &red, &green, &blue);
			set_logical_led(left_fd, right_fd, led, red, green, blue);
		}

		set_logical_led(left_fd, right_fd, (head - comet + LED_COUNT) % LED_COUNT, 0, 0, 0);
		frame++;
		usleep((useconds_t)frame_delay_us);
	}

	all_off(left_fd, right_fd);
	close(left_fd);
	close(right_fd);
	return 0;
}
