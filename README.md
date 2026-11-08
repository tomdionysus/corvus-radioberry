# corvus-radioberry

A complete, self-contained software stack for the Radioberry 2.x HF
transceiver HAT on a Raspberry Pi 5: Linux kernel driver, userspace radio
daemon speaking OpenHPSDR Protocol 1 and Protocol 2, DSP support library,
platform diagnostics, and control of the AURSINC PA5W V2 amplifier board,
with the FPGA gateware built from its GPL source. Every component is built
from this repository against the current Raspberry Pi OS kernel (6.18 and
newer) with no dependency on out-of-tree kernel patches.

The Radioberry and the upstream software this project builds on are the
work of Johan Maas, PA3GSB. He offers a ready-to-run Raspberry Pi 5 image
of his software for EUR 5 at https://pa3gsb.gumroad.com/l/radioberry. If
the Radioberry is useful to you, buying it supports his work.

The code is C++20, with C only where the kernel or a shared ABI header
requires it. Every component carries Catch2 unit tests with the aim of
complete coverage; the kernel module's pure logic is compiled into a
userspace test binary for the same purpose. All dependencies come from
Debian trixie packages.

## Status

| Component | Directory | State |
|---|---|---|
| Documentation of the hardware and host | `docs/` | current |
| Work tracking | `TODO/` | current |
| Platform probe (`rbprobe`) | `tools/rbprobe/` | working; gateware identity via the driver waits for the driver |
| FPGA gateware (Hermes-Lite 2 Radioberry PIO variants, 10CL025 and 10CL016) | `gateware/` | both built; CL025 verified on hardware; CL016 untested and misses AD9866 TX output timing (see `gateware/PROVENANCE.md`) |
| Kernel driver (`radioberry.ko`, RP1 PIO plus DMA) and overlay | `driver/` | working: loads gateware, receive streaming verified; transmit path not yet exercised |
| Radio daemon `radioberryd` (Protocol 1 and Protocol 2, PA control) | `daemon/` | planned |
| Further tooling (loader, stream dump, protocol probes) | `tools/` | planned |

`rbprobe`, the gateware, the kernel module, the overlay and the ABI
header install today. `TODO/ACTIVE.md` is the authoritative list of
what is being built and in which order.

## Target hardware and host

- Raspberry Pi 5 running Raspberry Pi OS (Debian 13 trixie, 64-bit),
  kernel 6.18.60+rpt-rpi-2712 or newer. The stock in-tree `rp1-pio` driver
  is used as-is.
- Radioberry 2.x HAT (AD9866 front end, Intel Cyclone 10 LP), 0 to 30 MHz.
  The development board carries a 10CL025; the 10CL016 is supported by
  shipping its bitfile as well.
- AURSINC "Radioberry Preamp Board V2" (PCB marked PA5W V2) stacked on the
  Radioberry: 1.5 to 3.5 W output, manual BIAS1/BIAS2 trimmers, INA199A1
  current sense on the PA_CUR header, no I2C devices.
- Development and test host: one dedicated Pi 5 with the hardware above
  attached, reached over SSH. Builds run on the Pi.

Details: `docs/platform.md` (host), `docs/pa.md` (amplifier),
`docs/history.md` (the upstream projects this replaces).

## Protocols

The daemon implements both OpenHPSDR Ethernet protocols:

- Protocol 1 (Metis/Hermes USB-over-Ethernet framing, UDP port 1024,
  1032-byte frames) in the Hermes-Lite 2 dialect, including the HL2
  extensions for PA enable, drive and the slow-ADC status words. This is
  what every Hermes-Lite-capable client expects today (piHPSDR, SparkSDR,
  Quisk, SDR Console, Thetis).
- Protocol 2 (openHPSDR Ethernet Protocol v4.x, UDP ports 1024 to 1029 and
  1035 upward) with 24-bit I/Q, discovery, DDC/DUC configuration and
  high-priority control. piHPSDR and Thetis speak it natively.

Both run from the same driver and gateware; the daemon selects the protocol
per client at discovery time. Protocol 2 transmit streams arrive at
192 ksps and are resampled in the daemon to the gateware rate.

## DSP library

liquid-dsp (MIT) is the DSP library for everything in the daemon that is not
done in the gateware: rate conversion between protocol and gateware sample
rates, filtering, NCO mixing, measurement, and PureSignal support. Debian
trixie ships 1.7.0 as `libliquid-dev`; upstream is 1.8.x. The minimum
supported version is 1.7.0 and the Debian package is used for builds on the
Pi. wdsp is not used.

## Requirements on the Pi

```
sudo apt install build-essential cmake device-tree-compiler \
    linux-headers-rpi-2712 libliquid-dev libgpiod-dev libi2c-dev i2c-tools \
    libyaml-cpp-dev nlohmann-json3-dev libspdlog-dev libsystemd-dev \
    catch2 lcov clang-format-19 rsync
```

Boot configuration in `/boot/firmware/config.txt`:

```
dtparam=i2c_arm=on
dtoverlay=radioberry
```

The overlay is built and installed by this repository; it enables `spi0`,
`rp1_pio` and the `sdr,radioberry` platform device, and disables the
spidev nodes.

The gateware is the Hermes-Lite 2 `radioberry_pio_cl025` and
`radioberry_pio_cl016` variants, version 75.3, built from the source in
`gateware/source/` with Quartus Prime Lite on an x86-64 machine
(`tools/build-gateware.sh`; toolchain and licence in
`gateware/QUARTUS.md`) and committed as
`gateware/rpi-5/<type>/radioberry.rbf`. The Pi only installs the result.
`gateware/PROVENANCE.md` records the source commit and the hashes of the
built images, and `gateware/README.md` the wire formats and control
behaviour the driver and daemon rely on.

## Build and install

CMake throughout, run on the Pi from the repository root:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFPGATYPE=CL025   # or CL016
cmake --build build -j
ctest --test-dir build
sudo cmake --install build
sudo systemctl enable --now radioberry
rbprobe         # verify host, overlay, driver, gateware, PA board
```

`cmake --install` places `radioberry.ko` under `/lib/modules/$(uname -r)/updates/`,
the device tree overlay under `/boot/firmware/overlays/`, the gateware under
`/lib/firmware/radioberry.rbf`, `radioberryd` and the tools under
`/usr/local/bin/`, the udev rule, the `radioberry` system user and group,
the default `/etc/radioberry/config.yaml`, and the systemd unit. The daemon
never runs as root. A reboot is required the first time the overlay is
installed.

`CMakePresets.json` provides `default`, `asan` (AddressSanitizer and
UndefinedBehaviorSanitizer) and `coverage` presets; with the last,
`cmake --build --preset coverage --target coverage` runs the tests and
writes an lcov report to `build-coverage/coverage/html`. The kernel module
is built when headers for the running kernel are installed
(`-DKERNEL_RELEASE=` picks another kernel, `-DRADIOBERRY_BUILD_MODULE=OFF`
skips it). Every gateware image goes to
`/usr/local/share/radioberry/gateware/rpi-5/<type>/`; the one for
`FPGATYPE` (default `CL025`) is also installed as
`/lib/firmware/radioberry.rbf`.

Until the components in the status table exist, only the `cmake`,
`ctest` and `cmake --install` steps work; there is no daemon or service
to enable yet.

## Using the radio

- `rbprobe` prints a report of the Pi, kernel, overlay, driver, gateware
  (type, version, configured or not), PA board (I2C scan, manual-bias
  board when nothing answers) and daemon state. `rbprobe --json` and
  `rbprobe --yaml` give the same content in machine-readable form. Exit
  status 0 means the radio can run, 1 that it cannot (each blocker is
  listed under `findings`), 2 a usage error. Run it as root to see device
  holders and the I2C bus.
- `rbprobe --identify` finds out which FPGA the board carries by loading
  each known rpi-5 gateware image in turn until one configures the chip.
  It reprograms the FPGA, so it refuses to run while the driver is loaded.
  It looks for `.rbf` files in `/usr/local/share/radioberry/gateware`,
  `/usr/share/radioberry/gateware` and `/lib/firmware`; `--gateware-dir`
  replaces that list.
- Start and stop: `systemctl start|stop radioberry`. Logs:
  `journalctl -u radioberry`. Configuration: `/etc/radioberry/config.yaml`.
- Point the SDR client at the Pi's address. Protocol 1 clients discover on
  UDP 1024; Protocol 2 clients discover on the same port and then use ports
  1025 to 1029 and 1035 upward. Protocol 2 advertises as many DDCs as the
  gateware has receivers (up to four) and one DUC. Only one client owns
  the radio at a time.
- The PA stage is never enabled by default. It is enabled only when the
  client sets the Hermes-Lite PA-enable bit and the daemon is started with
  PA control allowed. There is no thermal or current interlock on the
  PA5W V2; the daemon enforces a drive ceiling and a transmit watchdog, and
  the operator sets idle bias with the trimmers while watching PA_CUR
  (2 V per ampere). First transmissions go into a dummy load.

## Repository layout

```
docs/      hardware, host, plan and background references
gateware/  gateware source, built bitfiles per FPGA, provenance, toolchain
driver/    kernel module, device tree overlay, ioctl ABI header
daemon/    radioberryd: device layer, Protocol 1, Protocol 2, DSP, PA
tools/     rbprobe, rbload, rbcat, rbctl, p1probe, p2probe, ci-pi.sh
TODO/      ACTIVE.md (open work) and COMPLETED.md (done work)
```

Development happens on `develop`, one branch per TODO item squash-merged
into it; `main` receives `develop` only at version tags. `tools/ci-pi.sh`,
run against a Pi with the hardware attached, is the merge gate: on the Pi
it runs the format check (`tools/check-format.sh`, clang-format 19), the
SPDX check (`tools/check-spdx.sh`), the release, sanitizer and coverage
builds with their tests, and prints the coverage summary. From a
workstation, `tools/ci-pi.sh --remote [user@]host` copies the working tree
to the Pi and runs it there. The full
working conventions are in `CONTRIBUTING.md`.

## Licence

This repository is licensed under the GNU General Public License,
version 2 or (at your option) any later version. See `LICENSE`. Every
source file carries an SPDX header:

```
// SPDX-License-Identifier: GPL-2.0-or-later
```

GPL-2.0-or-later is required by the kernel module, which links against
`EXPORT_SYMBOL_GPL` symbols of the Raspberry Pi `rp1-pio` driver, and is
compatible with every upstream component listed below. The repository is
private; the licence governs any future distribution.

## Third-party works and attributions

| Work | Author / holder | Licence | Use here | Reference |
|---|---|---|---|---|
| Radioberry 2.x driver, daemon, overlays, gateware loader | Johan Maas, PA3GSB | Unlicense (public domain) as stated in each source file; no repository-level LICENSE file | ABI, pin map and behaviour reference; code may be reused freely | https://github.com/pa3gsb/Radioberry-2.x |
| Hermes-Lite 2 gateware, Radioberry variants (`radioberry_pio_cl025`, `radioberry_pio_cl016`) | Steve Haynal, KF7O; Johan Maas, PA3GSB | GPL-2.0-or-later (file headers) | Source in `gateware/source/`, unmodified; built here and the bitfiles distributed under `gateware/rpi-5/` | https://github.com/softerhardware/Hermes-Lite2 |
| openHPSDR DSP cores used by Hermes-Lite 2 (CIC, FIR, CORDIC, receiver) | Alex Shovkoplyas VE3NEA; Phil Harman VK6APH; Darrell Harmon; Cathy Moss; Steve Haynal KF7O | LGPL-2.0-or-later ("GNU Library General Public License", file headers) | Contained in the redistributed bitfiles | https://github.com/softerhardware/Hermes-Lite2/tree/master/gateware/rtl/radio_openhpsdr1 |
| Hermes-Lite 2 protocol description | softerhardware project | project wiki | Protocol 1 dialect, PA and bias commands, ADC fields | https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol |
| openHPSDR Ethernet Protocol v4.x (Protocol 2 specification) and Protocol 1 firmware sources | OpenHPSDR project, TAPR; Phil Harman VK6PH | GPL-2.0 (repository LICENSE) | Protocol 2 implementation reference | https://github.com/TAPR/OpenHPSDR-Firmware (Protocol 2/Documentation) |
| Raspberry Pi Linux kernel, `rp1-pio` driver | Raspberry Pi Ltd; Phil Elwell | GPL-2.0 | Kernel API the driver is built on | https://github.com/raspberrypi/linux |
| liquid-dsp | Joseph Gaeddert | MIT | DSP library linked by the daemon | https://github.com/jgaeddert/liquid-dsp |
| piHPSDR | John Melton G0ORX; Christoph van Wullen DL1YCF | GPL-2.0 | Reference client for both protocols; not linked | https://github.com/dl1ycf/pihpsdr |
| wdsp | Warren Pratt NR0V | GPL-2.0 | Not used; listed because the upstream stack depends on it | https://github.com/g0orx/wdsp |
| Radioberry HAT PA30 (preamp and ADC HAT design) | hish8612; JG1TWP; PA3GSB | no licence file in repository | Hardware reference for the PA family | https://github.com/hish8612/Radioberry_hat_pa30 |
| HL2-PA30 (30 W amplifier design) | JI1UDD | no licence file in repository | Hardware reference for the PA family | https://github.com/ji1udd/HL2-PA30 |
| AURSINC Radioberry Preamp Board V2 (PA5W V2) | AURSINC (commercial) | none published; test notes by BG7IYN | The amplifier board in use | https://www.aliexpress.com/item/1005008233825206.html , https://jumbo5566.github.io/pro/rb/2024/04/17/rb016_025.html |
| AD9866 datasheet | Analog Devices | datasheet terms | Front-end reference | https://www.analog.com/en/products/ad9866.html |
| Quartus Prime Lite Edition and its generated IP (PLL, FIFO megafunctions) | Altera (formerly Intel) | Quartus Prime licence agreement, accepted by whoever builds | Toolchain for the gateware build (`gateware/QUARTUS.md`); not redistributed; its generated IP is inside the bitfiles | https://www.altera.com/downloads |

Code taken from an upstream work keeps that work's licence header and
copyright line in addition to the SPDX line above. "Hermes", "Hermes-Lite",
"openHPSDR" and "Radioberry" are the names of their respective projects.
