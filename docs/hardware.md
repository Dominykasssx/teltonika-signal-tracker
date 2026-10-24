# Hardware

## Router

| Item | Value | Source |
|---|---|---|
| Model | Teltonika RUTX14 | `device_code RUTX14000000` in the router's configuration backup |
| Firmware in the backup | `RUTX_R_00.07.19.1` | `device_fw_version` in the backup |
| Firmware this project targets | `RUTX_R_00.07.25.3` | The only RUTX SDK Teltonika publishes |
| Modem id | `1-1` | `/etc/config/simcard` in the backup (use it for `option modem`) |

**Upgrade the router to RUTX_R_00.07.25.3 before installing.** RUTOS packages only work on
the firmware version their SDK was built for, and the 7.19.1 SDK cannot be downloaded
(Teltonika's server returns 403 for that URL).

The RUTX14 modem model is not recorded in the backup. Check it with `gsmctl -m` and
confirm `gsmctl -q` prints `RSRP:`/`RSRQ:`/`SINR:` lines on your SIM.

### USB serial drivers on RUTOS 7.25.3

Checked in the SDK's root filesystem image (`assets/rootfs.img`). These kernel modules
ship and autoload via `/etc/modules.d`: `cp210x`, `ch341`, `ch343`, `ftdi_sio`, `pl2303`,
`cdc-acm` (plus `usb-serial`/`option`).

The modem's own AT ports are also `ttyUSB*` devices (`option` driver). That's why the
daemon finds the ESP32 by **USB vendor:product id** through sysfs and never assumes
`/dev/ttyUSB0`. The default ids:

| USB id | Bridge |
|---|---|
| `10c4:ea60` | Silicon Labs CP210x |
| `1a86:7523` | WCH CH340 |
| `1a86:55d4` | WCH CH9102 |
| `0403:6001`, `0403:6015` | FTDI |
| `303a:1001` | Espressif USB-Serial/JTAG (native USB on S3/C3 boards) |

If several matching devices are connected, auto-detection refuses to guess. Set
`option device '/dev/ttyUSBx'` in that case.

RUTOS **Serial Utilities** (console, over-IP, modem modes) can also open USB serial
adapters. Do not configure them for the ESP32's port. The daemon takes an exclusive
lock (`flock` + `TIOCEXCL`), but root-run services can still override that.

## ESP32 board

The request named an "ESP32-S1", which is not an Espressif part number, and the board
itself could not be inspected. The firmware therefore targets the **original ESP32**
(e.g. ESP32-WROOM-32 / DevKitC). That chip has no native USB device, so the board's
**USB-to-UART bridge** (CP2102, CH340 or CH9102) carries the protocol on UART0. All three
bridge drivers are present on the RUTX.

If the board is really an ESP32-S2, S3 or C3 with native USB, the router side already
accepts it (`cdc-acm`, `303a:1001`), but the firmware's UART transport in `main.c` must
be replaced with USB-Serial/JTAG (S3/C3) or TinyUSB CDC (S2).

Opening the port on the router can reset typical dev boards: DTR/RTS are wired to
EN/GPIO0. The daemon releases both lines after opening, and a reset there is harmless
because the boot event is part of the protocol.

## Wiring

```
RUTX USB-A ──USB cable── ESP32 board (data + 5 V for the ESP32 only)

ESP32 GPIO18 ─────────────── servo signal (orange/yellow)
ESP32 GND ───────┬────────── servo GND (brown/black)
                 └────────── external supply GND
external 5-6 V ───────────── servo V+ (red)      [+ 470-1000 µF capacitor across V+/GND]
```

- **Power the servo from its own supply**, sized for the servo's stall current (often
  1–3 A for metal-gear servos that can turn a router). Do not take servo power from the
  ESP32 board or the router's USB port. Check the RUTX14 datasheet for the USB port's
  current limit; it is meant for low-power peripherals.
- **Common ground** between the servo supply and the ESP32 is required.
- The signal is 3.3 V PWM at 50 Hz, which most 5 V hobby servos accept. Use a level
  shifter if your servo does not.
- The signal GPIO is set by `CONFIG_SERVO_GPIO` (default 18). Avoid strapping pins
  0, 2, 5, 12 and 15.

## Servo and mechanics

- A standard positional servo is required (typically 180° travel). Continuous-rotation
  "360°" servos are speed-controlled, cannot hold an angle, and are not supported.
- Set the firmware's hard limits (`CONFIG_SERVO_MIN_ANGLE`, `CONFIG_SERVO_MAX_ANGLE`) to
  what the platform can reach without binding or straining the antenna, power and
  Ethernet cables. The router's UCI range must lie inside these limits, and both sides
  enforce them.
- Calibrate `CONFIG_SERVO_MIN_PULSE_US` / `MAX_PULSE_US` for your servo so
  `0..CONFIG_SERVO_TRAVEL_DEG` maps to the real travel without hitting the end stops.
- `CONFIG_SERVO_MS_PER_60DEG` must reflect the speed **under the router's load**. The
  firmware reports a move as finished based on this estimate.

## Known hardware limitations

- **No position feedback.** The system knows only the *commanded* angle. Stalls,
  slipping or manual rotation go undetected, and the WebUI says so.
- **Move completion is estimated** from the configured servo speed, not measured.
- **No homing.** After a reboot of the ESP32 or the router, the position is unknown until
  the next move. Nothing is moved automatically.
- **Stop releases the servo.** It switches the PWM signal off; a stop mid-move leaves the
  platform somewhere between the two angles, and an unpowered servo may be back-driven.
- **Angular range** is limited by the servo (usually ≤ 180°) and cabling. Directions
  outside it are never measured.
- **Measurement resolution**: gsmctl reports whole numbers, and modem values fluctuate
  by a few dB, so nearby fine-step angles are often indistinguishable.
