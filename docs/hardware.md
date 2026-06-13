# Gizmo Hardware Notes

These notes document the hardware assumptions encoded by the NixOS modules and
helper tools. They are bring-up notes, not a stable hardware specification.

## Firmware And Boot

- The current images are built for x86_64 USB boot.
- The rescue image treats onboard firmware as read-only and uses flashrom only
  for reads.
- The stock firmware-compatible boot partition uses systemd-boot-style files:
  `loader/loader.conf` and `loader/entries/<entry>.conf`.
- The CPLD watchdog has an approximately 15 minute timeout.
- `watchdog-control -d` disables the watchdog through the Gizmo CPLD I/O port.

The images blacklist Intel SPI kernel MTD drivers. Firmware reads use
flashrom's internal programmer, and the kernel SPI auto-probe is noisy on this
platform.

## Display

The kiosk profile uses a fixed clockwise portrait orientation:

- Xorg `modesetting`
- preferred mode `1200x1920`
- `Rotate "right"`
- virtual screen `1920x1200`
- software cursor enabled
- page flipping disabled

i915 PSR, FBC, and DC are disabled. These settings avoid black flashes and
framebuffer jumps observed on the rotated display.

Dynamic orientation from the accelerometer is not implemented.

## CPLD GPIO

`gizmo-cpld-i2c` exposes confirmed CPLD GPIO banks:

- `gizmo-gpio-enables`: `hdmi_en`, `nfc_en`, `backlight_en`,
  `bt_ant_hi_pwr_en`, `usb_a_en`, `usb_b_en`, `usb_b_hi_power_en`,
  `usb_a_hi_power_en`
- `gizmo-gpio-resets`: `lanc_rst`, `tpm_rst`, `voice_rst`,
  `touchpanel_rst`, `i2c_switch_rst`, `dsi_bridge_rst`, `bluetooth_rst`
- `gizmo-gpgpio`: four general-purpose CPLD GPIO lines without confirmed
  source names
- `gizmo-intc`: `touchpanel_intr`, `nfc_intr`, `current_sense_intr`,
  `ambient_light_sensor_intr`, `audio_codec_intr`, `i2c_intr`, `edp_intr`,
  `accelerometer_intr`

`gizmo-intc` uses CPLD IRQ 7 by default. If IRQ registration fails, the driver
falls back to exposing interrupt lines as input-only GPIOs so diagnostics can
still read their state.

## CPLD I2C

The CPLD OpenCores I2C controller is exposed through the `gizmo-cpld-i2c`
module. The common module loads sensor drivers only after the CPLD bus has
settled because immediate module autoload can race the polling adapter.

Known devices on the CPLD OpenCores bus:

- `0x10`: BMC150 accelerometer
- `0x12`: BMC150 magnetometer
- `0x38`: FT7511 touchscreen
- `0x44`: ISL29023 ambient light sensor
- `0x72`: PCA9543 I2C mux

The mux child adapters expose the side LED controllers and NFC device.

## Touchscreen

The touchscreen is an FT7511 at I2C address `0x38`.

Expected settings:

- max touches: `10`
- active report period: `100 Hz`
- max X: `1200`
- max Y: `1920`
- reset GPIO: `gizmo-gpio-resets:touchpanel_rst`
- interrupt GPIO: `gizmo-intc:touchpanel_intr`

The out-of-tree `gizmo-ft7511` driver uses named CPLD reset and interrupt GPIOs.
Polling remains available through `gizmo.common.touchscreen.pollMs` if the
interrupt domain is not available.

For the clockwise portrait kiosk profile, the module sets:

```nix
gizmo.common.touchscreen.swapXY = true;
gizmo.common.touchscreen.invertY = true;
```

## Sensors

The BMC150 accelerometer, BMC150 magnetometer, and ISL29023 ambient light sensor
are on the CPLD OpenCores I2C adapter.

The image loads:

- `bmc150_accel_i2c`
- `bmc150_magn_i2c`
- `isl29018`

Useful checks:

```sh
gizmo-list-sensors
gizmo-watch-accelerometer
gizmo-watch-magnetometer
gizmo-watch-ambient-light
```

## NFC

The PN547 NFC controller is expected at I2C address `0x28` on the right mux
channel. The image loads the `gizmo-pn544` misc driver and exposes `/dev/pn544`,
matching the PN544 ioctl ABI used by original-style userspace.

Useful checks:

```sh
gizmo-test-nfc
gizmo-test-nfc --nci-reset
gizmo-test-nfc --nci-discover --seconds 15
```

The raw `/dev/pn544` driver does not make the chip poll for cards by itself.
Userspace must send NCI discovery commands or run a full NFC stack.

By default, the driver uses GPIO lookups for:

- `gizmo-gpio-enables:nfc_en`
- `gizmo-intc:nfc_intr`

Legacy Linux GPIO overrides remain available through:

```nix
gizmo.common.nfc.venGpio = null;
gizmo.common.nfc.irqGpio = null;
gizmo.common.nfc.firmGpio = null;
```

## LEDs

The surround LEDs sit behind the PCA9543 mux at `0x72` on the CPLD OpenCores
I2C bus. The current bring-up path uses `gizmo-led-rainbow` to drive LP55231
devices directly because the kernel LED class driver has timed out
intermittently during post-init on this bus.

Mux channel detection:

- right side is the channel with PN547 NFC at `0x28`
- left side is the other channel
- if the right-side signature is not found, the services default to channel 1
  as right and channel 0 as left

Left-side LP55231 devices:

- `0x32`: left RGB LEDs 1-3
- `0x33`: left RGB LEDs 4-6
- `0x34`: left RGB LEDs 7-9
- `0x35`: left RGB LEDs 10-12

Right-side devices:

- `0x28`: PN547 NFC
- `0x32`: right RGB LEDs 1-3
- `0x33`: right RGB LEDs 4-6
- `0x34`: right RGB LEDs 7-9
- `0x35`: right RGB LEDs 10-12

LP55231 platform notes:

- `num_channels = 9`
- channel order: blue, blue, blue, green, green, green, red, red, red
- logical LED mapping: `0x32` handles 1-3, `0x33` handles 4-6,
  `0x34` handles 7-9, and `0x35` handles 10-12

LED boot animation is controlled by:

```nix
gizmo.common.leds.animateOnBoot = true;
```

The public kiosk image disables the animation by default.

## Audio

Current playback is prepared through the HDA ALC298 device. The common module
unmutes Master and downstream playback controls, then uses Master as the
default volume knob.

Tune the default volume with:

```nix
gizmo.common.audio.masterVolumePercent = 50;
```
