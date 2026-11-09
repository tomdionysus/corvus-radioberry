# corvus-radioberry

A self-contained software stack for the Radioberry 2.x HF transceiver HAT
on a Raspberry Pi 5: Linux kernel driver, a radio daemon speaking
OpenHPSDR Protocol 1 and Protocol 2 (DSP by liquid-dsp), platform
diagnostics, and control of the AURSINC PA5W V2 amplifier board, with the
FPGA gateware built from its GPL source. Every component is built
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

## Status (2026-11-09, v0.1.1)

| Component | Directory | Working | Notes |
|---|---|---|---|
| Platform probe `rbprobe` | `tools/rbprobe/` | ✓ | Reports host, boot, driver, gateware, FPGA, PA board and daemon state; text, JSON or YAML |
| Stream tool `rbcat` | `tools/rbcat/` | ✓ | Receive streams and measurements without the daemon; zero TX samples with PTT clear |
| Command tool `rbctl` | `tools/rbctl/` | ✓ | Single gateware commands; anything that can transmit needs `--allow-tx` |
| Gateware, CL025 (HL2 Radioberry PIO variant 75.3) | `gateware/` | ✓ | Built from source and verified on hardware; its receive-stream lock-up (`docs/fault-rx-lockup.md`) is worked around by the driver |
| Gateware, CL016 | `gateware/` | ✗ | Built, not tested on a CL016 board; misses AD9866 TX output timing (`gateware/PROVENANCE.md`) |
| Kernel driver `radioberry.ko`, overlay, ABI header | `driver/` | ✓ | Loads the gateware at boot; receive and transmit streams; sysfs `reload`, stall indicator, `debug` parameter |
| Radio daemon `radioberryd`, Protocol 1 | `daemon/` | ✓ | Hermes-Lite 2 dialect, receive and transmit, UDP and TCP; transmit policy and fail-safes; frequency calibration; FT8 contacts on 40 m with piHPSDR and WSJT-X |
| Radio daemon `radioberryd`, Protocol 2 | `daemon/` | ✓ | Receive and transmit; FT8 contacts on 40 m with piHPSDR (Protocol 2) and WSJT-X; no wideband, diversity, PureSignal or radio-side CW keyer |
| `radioberryd` as a systemd service | `daemon/` | ✗ | Not started; run by hand for now |
| Example FT8 client `ft8rx` | `examples/ft8/` | ✓ | Receive only over Protocol 1 or 2: decodes FT8, never transmits; optional build |
| Gateware loader `rbload` | `tools/` | ✗ | Not started |
| Protocol test clients `p1probe`, `p2probe` | `tools/` | ✗ | Not started |

The design of the whole stack is in `docs/plan-v1.md`; the open work, in
order, is in `TODO/ACTIVE.md`.

## Target hardware and host

- Raspberry Pi 5 running Raspberry Pi OS (Debian 13 trixie, 64-bit),
  kernel 6.18.60+rpt-rpi-2712 or newer. The stock in-tree `rp1-pio` driver
  is used as it is.
- Radioberry 2.x HAT (AD9866 front end, Intel Cyclone 10 LP 10CL025 or
  10CL016), 0 to 30 MHz.
- AURSINC "Radioberry Preamp Board V2" (PCB marked PA5W V2) stacked on the
  Radioberry: 1.5 to 3.5 W output, manual BIAS1/BIAS2 trimmers, INA199A1
  current sense on the PA_CUR header, no I2C devices.

Details: `docs/platform.md` (development host), `docs/pa.md` (amplifier),
`gateware/README.md` (FPGA interface), `docs/history.md` (the upstream
projects this replaces).

## Requirements on the Pi

```
sudo apt install build-essential cmake device-tree-compiler \
    linux-headers-rpi-2712 libliquid-dev libgpiod-dev libi2c-dev i2c-tools \
    libyaml-cpp-dev nlohmann-json3-dev libspdlog-dev libsystemd-dev \
    catch2 lcov clang-format-19 rsync
```

`/boot/firmware/config.txt` needs:

```
dtparam=i2c_arm=on
dtoverlay=radioberry
```

The `radioberry` overlay is built and installed by this repository. It
enables `spi0` and `rp1_pio`, creates the `sdr,radioberry` device on
`spi0` chip select 0 (SPI mode 3, 12.5 MHz) with the FPGA configuration
GPIOs, and disables the spidev nodes.

## Gateware

The FPGA runs the Hermes-Lite 2 `radioberry_pio_cl025` or
`radioberry_pio_cl016` variant, version 75.3. The bitfiles in
`gateware/rpi-5/<type>/radioberry.rbf` are built here from the unmodified
source in `gateware/source/` with Quartus Prime Lite on an x86-64 machine
(`tools/build-gateware.sh`; toolchain and licence in `gateware/QUARTUS.md`)
and committed, so the Pi only installs them. `gateware/PROVENANCE.md`
records the source commit, fitter seeds, hashes and timing results.

## Build and install

On the Pi, from the repository root:

```
cmake -S . -B build -DFPGATYPE=CL025   # or CL016
cmake --build build -j
ctest --test-dir build
sudo cmake --install build
sudo reboot                            # first install of the overlay only
rbprobe
```

`cmake --install` places:

| What | Where |
|---|---|
| kernel module | `/lib/modules/$(uname -r)/updates/radioberry.ko` (then `depmod`) |
| overlay | `/boot/firmware/overlays/radioberry.dtbo` |
| gateware for `FPGATYPE` | `/lib/firmware/radioberry.rbf` |
| both gateware images | `/usr/local/share/radioberry/gateware/rpi-5/<type>/` |
| ABI header | `/usr/local/include/radioberry_ioctl.h` |
| `rbprobe`, `rbcat`, `rbctl` | `/usr/local/bin/` |

The module is built when headers for the running kernel are installed
(`-DKERNEL_RELEASE=` picks another kernel, `-DRADIOBERRY_BUILD_MODULE=OFF`
skips it). `-DRADIOBERRY_ABI_INCLUDE_DIR=/usr/local/include` builds the
tools and daemon against the installed ABI header instead of
`driver/include`; configuration fails if its ABI version differs from the
source tree's. `CMakePresets.json` has `default`, `asan` (AddressSanitizer and
UndefinedBehaviorSanitizer) and `coverage` presets;
`cmake --build --preset coverage --target coverage` writes an lcov report
to `build-coverage/coverage/html`.

## Using it

- `rbprobe` reports the Pi, kernel, overlay, driver, installed gateware,
  FPGA type and gateware identity, PA board (I2C scan) and daemon state.
  `--json` and `--yaml` give the same content. Exit status 0 means the
  radio can run, 1 that it cannot (each blocker is listed under
  `findings`), 2 a usage error. Run it as root to see device holders and
  the I2C bus.
- `rbprobe --identify` finds out which FPGA the board carries by loading
  each known rpi-5 gateware image in turn until one configures the chip.
  It reprograms the FPGA, so it refuses to run while the driver is loaded.
  It looks for `.rbf` files in `/usr/local/share/radioberry/gateware`,
  `/usr/share/radioberry/gateware` and `/lib/firmware`; `--gateware-dir`
  replaces that list.
- The driver loads the gateware when it binds, at boot, and publishes the
  gateware identity in `/sys/bus/spi/devices/spi0.0/` (`gateware_version`,
  `fpga_type`, `receivers`, `transmitters`). Writing 1 to `reload`
  there reloads `/lib/firmware/radioberry.rbf`, re-reads the identity and
  restarts the streams (about 5.5 s; EBUSY while `/dev/radioberry` is
  open; statistics restart from zero; if it fails, opening the device
  gives ENODEV until a reload succeeds). Module parameters:
  `load_gateware` (default 1), `rx_clkdiv` (2), `tx_clkdiv` (8),
  `rx_ring_kib` (512), `tx_ring_kib` (64), `rx_clock_hold_us` (200) and
  `debug` (0); the last two are writable at run time in
  `/sys/module/radioberry/parameters/`. `debug` 1 logs rate-limited
  diagnostics: each command with its reply, RX resyncs and dropped words
  per DMA period, RX ring overruns and TX underruns; 2 traces every
  command and receiver-count change without rate limiting.
- The PIO gateware can lock up its receive stream at a receiver-count
  change (`docs/fault-rx-lockup.md`). The driver works around it by
  holding its RX clock still from just before a receiver-count command
  until `rx_clock_hold_us` after it (0 disables the hold). Should the
  stream stop anyway, the driver logs "RX stream stalled" after 200 ms
  without RX words while run is set, sysfs `rx_stalled` reads 1 and
  `rx_stalls` counts the events; writing 1 to `reload` recovers.
- `/dev/radioberry` is the interface for the daemon and tools: exclusive
  open, whole RX frames from `read()`, TX samples to `write()`, gateware
  commands, identity and statistics by ioctl. `driver/include/radioberry_ioctl.h`
  documents it. Closing the device sends the gateware a command that
  clears run, PTT, the TX permission bit, PA enable and drive.
- `rbcat` receives from `/dev/radioberry` without the daemon: it sets the
  sample rate (`--rate 48|96|192|384`), receiver count (`--receivers`),
  frequencies (`--freq HZ[,HZ...]`) and LNA gain (`--lna`, -12 to 48 dB),
  streams for `--seconds` (or until SIGINT or SIGTERM) after a settling
  time (`--settle`, 0.5 s), and reports frame rate, RMS and peak level
  per receiver, and the driver's resync, dropped-word and overrun counts
  over the measurement and over the settling time, as text, `--json` or
  `--yaml`. `--output iq24` writes the frames to stdout as read (24-bit
  big-endian I and Q per receiver), `--output iq16` the top 16 bits of
  each value as little-endian int16; the report then goes to stderr.
  `--interval S` prints a progress line every S seconds. `--tx-zeros`
  also writes zero TX samples, keeping about 50 ms queued in the driver,
  and reports the TX word rate, underruns and the lowest queue level; the
  gateware drains and discards them because PTT is clear. It never sets
  PTT or the TX permission bit. Exit status 0 means a clean stream (no
  faults after settling, frame rate within 1%, and with `--tx-zeros` a
  TX word rate within 1% of 48000 per second, no underruns and a queue
  that never ran empty), 1 stream faults or a device error, 2 a usage
  error.
- `rbctl ADDRESS DATA` sends one gateware command (Hermes-Lite 2
  address and 32-bit C1..C4 data, decimal or 0x hex; run set unless
  `--no-run`) and prints the command bytes, the gateware's reply, and the
  driver's info and statistics; with no arguments it prints only the info
  and statistics. A command that can key the transmitter or arm the PA
  (`--ptt`, `--tx-permit`, `--cwx`, or address 0x09 with drive, VNA or
  PA enable) is refused unless `--allow-tx` is given; a PTT hang time of
  31 at address 0x17 is always refused. On exit the driver's close sends
  the safe command, so run, PTT, the TX permission bit, PA enable and
  drive last only while `rbctl` runs. Exit status 0 on success, 1 on a
  device error (including EBUSY while another program holds the device),
  2 on a usage error or refused command.
- Receive capacity over the PIO bus is about 2.37 million values per
  second: up to six receivers at 192 ksps or three at 384 ksps.
- `radioberryd` serves the radio to SDR clients over openHPSDR Protocol 1
  in the Hermes-Lite 2 dialect, on UDP and TCP port 1024, so clients that
  support the HL2 (piHPSDR and others) find it by discovery. It is not
  yet installed as a service; run it as root, for example
  `radioberryd --frequency-ppm 22.9`. It receives only unless started
  with `--allow-tx`; then the client that owns the radio may transmit
  while it holds MOX, the drive is capped by `--drive-ceiling N` (default
  0), PA enable is passed on only with `--allow-pa`, and `--tx-watchdog-s`
  (default 120) ends any transmission held longer. A client silent for 3 s
  loses the radio, and any stream fault while transmitting drops MOX until
  the client releases it. `--frequency-ppm` corrects every frequency for
  the board's reference error (measure it against known signals;
  `docs/examples.md`). `--iq-convention standard` swaps I and Q both ways
  for clients using plain complex baseband; the default `hl2` is what HL2
  software expects. It claims no PA interlock. `radioberryd --help` lists
  every option.
- `radioberryd` also serves openHPSDR Protocol 2 over UDP: discovery and
  General on port 1024, the other host packets on 1025 to 1029, DDC I/Q
  from 1035 to 1040 (with `--port N`, N and the same offsets). It reports
  board 6 with the gateware's version, which piHPSDR takes as a
  Hermes-Lite 2, and the gateware's receiver count as DDCs. The gateware
  runs every receiver at one rate, the highest any enabled DDC asks for
  (48 to 384 ksps, within the receive capacity below); a DDC asking for
  less is decimated in the daemon. TX I/Q arrives at 192 ksps and is
  decimated to the gateware's 48 ksps. The silent Mic/Line packets that
  clock the client's transmitter and the High Priority status are paced
  by the radio's sample clock. The client's step attenuation is taken off
  the LNA gain set by `--p2-rx-gain` (default 14 dB). The same session,
  transmit policy and fail-safes apply as for Protocol 1; a gap in the TX
  I/Q sequence is a stream fault. piHPSDR lists the radio once per
  protocol; choose the Protocol 2 entry, or turn Protocol 1 off in its
  discovery settings.
- With piHPSDR (HL2 type): its HL2 drive model assumes the Hermes-Lite 2
  gateware, which uses the drive byte differently, so lower its per-band
  PA calibration until the output is right (about 45 to 50 on the
  development board). For WSJT-X through piHPSDR, use the Kenwood TS-2000
  rig type on piHPSDR's CAT port (TCP 19090).
- `ft8rx` (`examples/ft8`, built with `-DRADIOBERRY_BUILD_EXAMPLES=ON`) is
  a receive-only Protocol 1 or 2 client that decodes FT8 and prints it as
  WSJT-X does (`docs/examples.md`).

## Repository layout

```
docs/      design (plan-v1.md), RX lock-up fault report (fault-rx-lockup.md), host, amplifier, upstream survey
gateware/  gateware source, built bitfiles per FPGA, provenance, toolchain
driver/    kernel module, device tree overlay, ABI header, logic tests
daemon/    radioberryd: device layer, session, transmit policy, Protocols 1 and 2
examples/  ft8rx, an example FT8 client (optional build)
tools/     rbprobe, rbcat, rbctl, shared library, CI and check scripts, gateware build
TODO/      ACTIVE.md (open work) and COMPLETED.md (done work)
```

Development happens on `develop`, one branch per TODO item squash-merged
into it; `main` receives `develop` only at version tags.
`tools/ci-pi.sh` is the merge gate: on the Pi it runs the format check
(`tools/check-format.sh`, clang-format 19), the SPDX check
(`tools/check-spdx.sh`), the release, sanitizer and coverage builds with
their tests, prints the coverage summary, and, when the ABI header is
installed, builds the tools against it and checks that no userspace
compile sees `driver/include`; from a workstation,
`tools/ci-pi.sh --remote [user@]host` copies the working tree to the Pi
and runs it there. Working conventions are in `CONTRIBUTING.md`.

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
| liquid-dsp | Joseph Gaeddert | MIT | DSP library linked by the daemon and `ft8rx` | https://github.com/jgaeddert/liquid-dsp |
| ft8_lib | Karlis Goba | MIT | Message text code vendored in `examples/ft8/src/ft8_lib/` with its licence; the FT8 decoder in `examples/ft8/src/ft8/` is Corvus HF's port of it | https://github.com/kgoba/ft8_lib |
| Corvus HF FT8 decoder | T. Cully | GPL-2.0-or-later here (the author's own code, also published as Apache-2.0 in Corvus HF) | `examples/ft8/src/ft8/` | Corvus HF |
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
