# Build, install and use

## 1. ESP32 firmware

Requirements: ESP-IDF 5.x. The project was built with ESP-IDF 5.4.0.

```sh
cd esp32/servo-controller
idf.py set-target esp32
idf.py menuconfig          # "Servo controller": GPIO, limits, pulse widths, speed
idf.py build
idf.py -p <PORT> flash
```

Or with PlatformIO (`platformio.ini` is included):

```sh
cd esp32/servo-controller
pio run -e esp32dev -t upload
```

Notes:

- `sdkconfig.defaults` disables the console and all logging, because UART0 carries the
  protocol. There is therefore no `idf.py monitor` log output.
- Quick check from a PC: open the port at 115200 baud and type
  `{"command":"get_status"}` followed by Enter.
- Host unit tests for the protocol logic: `make -C test test` (needs a native C compiler).

## 2. RUTOS packages

Requirements, from the SDK and Teltonika's wiki:

- Linux x86_64 build host (Ubuntu 22.04 recommended), `python3`, `xz`, build essentials
- Node.js ≥ 20 and npm ≥ 10.5 for the WebUI package
- SDK **RUTX_R_SDK_00.07.25.3**:
  <https://firmware.teltonika-networks.com/7.25.3/RUTX/RUTX_R_SDK_00.07.25.3.tar.gz>
  (MD5 `12cf2c0717b308ff5f677a6d82d51526`)
- Router running **RUTX_R_00.07.25.3** (packages are firmware-specific)

The SDK tarball has no top-level directory, so extract it into its own folder:

```sh
mkdir rutx-sdk && tar -xzf RUTX_R_SDK_00.07.25.3.tar.gz -C rutx-sdk
cd rutx-sdk
source init-build-env                       # every new shell

cp -r <repo>/rutos/apps/* apps/
cat <repo>/rutos/build.conf.apps >> build.conf   # [apps] is the last section

puf apps build signal-tracker
puf apps build vuci-app-signal-tracker-api
puf apps build vuci-app-signal-tracker-ui
ls dist/packages/
```

`puf build` instead produces a complete firmware image with the three apps built in,
with the service enabled at boot (`enable-init` in `build.conf.apps`).

Daemon host tests (no SDK needed): `make -C rutos/apps/signal-tracker test CC=cc`.

### Install on the router

```sh
scp dist/packages/*signal-tracker*.ipk root@192.168.1.1:/tmp/
ssh root@192.168.1.1
opkg install /tmp/signal-tracker_*.ipk /tmp/vuci-app-signal-tracker-api_*.ipk \
             /tmp/vuci-app-signal-tracker-ui_*.ipk
/etc/init.d/signal-tracker enable
/etc/init.d/signal-tracker start
```

Reboot once after the first installation, so the new WebUI menu, API routes, rpcd/ubus
ACLs and file permissions (`/etc/permtab.d`) are all loaded.

### Check from the shell

```sh
ubus call signal_tracker status
ubus call signal_tracker move '{"angle":90}'
ubus call signal_tracker scan_start
logread -e signal-tracker
```

To try everything without hardware, run
`uci set signal_tracker.main.simulate=1 && uci commit signal_tracker && /etc/init.d/signal-tracker restart`.

## 3. WebUI

**Services → Signal Tracker**. The page refreshes every second.

- **Status**: controller connection (device, firmware), last error, **commanded angle**
  and **measured angle**. The measured angle always reads *Not available*, since the
  servo has no feedback. Also current RSRP/RSRQ/SINR and serving cell.
- **Manual Control**: slider and number input limited to the configured range, plus
  **Move** and **Stop**. Move is disabled while disconnected, while a scan runs or while
  a move is in progress. Stop also cancels a running scan and switches the servo signal off.
- **Automatic Optimization**: **Start Scan** / **Cancel Scan**, scan status and phase,
  current angle, progress, best measured angle with its measurements, the starting angle
  (baseline), the final result, warnings for partial scans and cell changes, and a table
  of every measured angle.
- **Configuration**: the options below, stored in UCI. Settings are picked up by the next
  move or scan; a running scan keeps the settings it started with.

Access control uses the standard RUTOS user groups. The rpcd ACL `services/signal_tracker`
grants read access (status) and write access (moves, scans, configuration).

## 4. Configuration (`/etc/config/signal_tracker`, section `main`)

| Option | Default | Meaning |
|---|---|---|
| `enabled` | `1` | Start the service |
| `min_angle` / `max_angle` | `0` / `180` | Allowed range; must fit the firmware limits |
| `coarse_step` | `10` | Coarse sweep step (°) |
| `fine_step` | `2` | Fine sweep step (°), ≤ `coarse_step` |
| `settle_ms` | `4000` | Wait after a move before sampling |
| `samples` | `5` | Samples per angle (1–20) |
| `sample_interval_ms` | `2000` | Time between samples |
| `min_valid_samples` | `3` | Minimum valid samples for an angle (and per metric) |
| `move_timeout_ms` | `5000` | Maximum time for a move to finish |
| `min_improvement` | `0` | Score points the best angle must beat the start by; 0 = off |
| `weight_rsrp` / `weight_rsrq` / `weight_sinr` | `0.5` / `0.2` / `0.3` | Score weights |
| `request_timeout_ms` | `1000` | Serial request timeout |
| `device` | `auto` | `auto` (match by USB id) or a `/dev/...` path |
| `usb_id` (list) | see [hardware](hardware.md) | USB ids accepted by auto-detection |
| `modem` | empty | gsmctl modem id (`-O`), e.g. `1-1`; empty = default modem |
| `simulate` | `0` | Built-in simulator instead of hardware (restart to apply) |
| `sim_best_angle` | `120` | Best angle of the simulated signal field |

Invalid combinations (e.g. `min_angle ≥ max_angle`, `fine_step > coarse_step`) are
reported in the WebUI and block moves and scans until fixed.
