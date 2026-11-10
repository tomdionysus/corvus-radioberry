# corvus-radioberry

[![CI](https://github.com/tomdionysus/corvus-radioberry/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/corvus-radioberry/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/corvus-radioberry/graph/badge.svg)](https://codecov.io/gh/tomdionysus/corvus-radioberry)
[![CodeQL](https://github.com/tomdionysus/corvus-radioberry/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/corvus-radioberry/actions/workflows/codeql.yml)

Version 0.3.0

A self-contained software stack for the
[RadioBerry 2.x HF transceiver HAT](https://github.com/pa3gsb/Radioberry-2.x/wiki)
on a Raspberry Pi 5:

- `radioberry.ko`, the Linux kernel driver
- `radioberryd`, a daemon speaking openHPSDR Protocol 1 and Protocol 2
- signal processing with liquid-dsp
- platform diagnostics
- control of an [AURSINC PA5W V2](https://www.aursinc.com/products/aursinc-radioberry-preamp-board-v2-output-1-5w-3w-for-hf-power-ampfier-10w-100w-pa-for-raspberry-pi-4-4b) amplifier board
- FPGA gateware built from its GPL source

Every component is built from this repository against the current
Raspberry Pi OS kernel (6.18 and newer) with no out-of-tree kernel
patches. In particular, no patched `rp1-pio` is needed: the driver uses
only what the stock kernel's `rp1-pio` driver exports. The original
RadioBerry driver depends on a symbol only a private `rp1-pio` patch
provides, and
[kernel updates break it](https://m0aws.co.uk/?p=3670). After a kernel
update, rebuild and reinstall this one (Build and install, below).

The RadioBerry and its original software come from the
[RadioBerry project](https://github.com/pa3gsb/Radioberry-2.x). A
[ready-to-run Raspberry Pi 5 image](https://pa3gsb.gumroad.com/l/radioberry)
of the original software is sold by the RadioBerry's author; buying it
supports their work. Please consider buying it even if you don't use it:
the RadioBerry hardware is a work of art.

The code is C++20, with C only where the kernel or a shared ABI header
requires it, tested with Catch2. All dependencies are Debian trixie
packages.

Documentation:

| Document | Contents |
|---|---|
| [docs/platform.md](docs/platform.md) | supported hardware, the development Pi, kernel facts, real-time and jitter measurements |
| [docs/pa.md](docs/pa.md) | the PA5W V2 amplifier board |
| [docs/design.md](docs/design.md) | architecture of the driver, daemon and probe |
| [docs/driver.md](docs/driver.md) | kernel driver: device interface, sysfs, module parameters |
| [docs/driver-tests.md](docs/driver-tests.md) | `rbdrivertest`, the driver's hardware tests |
| [docs/kernel-debug.md](docs/kernel-debug.md) | the driver under a debug kernel (KASAN, lockdep, kmemleak): method and results |
| [docs/daemon.md](docs/daemon.md) | `radioberryd`: options, service, Protocols 1 and 2, client notes |
| [docs/tools.md](docs/tools.md) | `rbprobe`, `rbcat`, `rbctl`, `rbload`, `p1probe`, `p2probe` |
| [docs/examples.md](docs/examples.md) | `ft8rx`, sample order, frequency calibration |
| [docs/fault-rx-lockup.md](docs/fault-rx-lockup.md) | the gateware's receive lock-up and the driver's workaround |
| [gateware/README.md](gateware/README.md) | the gateware: interface the driver depends on |
| [gateware/PROVENANCE.md](gateware/PROVENANCE.md), [gateware/QUARTUS.md](gateware/QUARTUS.md) | gateware source, build, timing, toolchain |
| [docs/licensing.md](docs/licensing.md) | licence and third-party works |
| [CONTRIBUTING.md](CONTRIBUTING.md) | repository layout, CI, working conventions |

## Status

| Component | Directory | Working | Notes |
|---|---|---|---|
| Platform probe `rbprobe` | `tools/rbprobe/` | ✓ | Host, boot, driver, gateware, FPGA, PA board and daemon state; text, JSON or YAML |
| Stream tool `rbcat` | `tools/rbcat/` | ✓ | Receive streams and measurements without the daemon |
| Command tool `rbctl` | `tools/rbctl/` | ✓ | Single gateware commands; anything that can transmit needs `--allow-tx` |
| Gateware, CL025 (HL2 RadioBerry PIO variant 75.3) | `gateware/` | ✓ | Built from source, verified on hardware; its receive lock-up is worked around by the driver |
| Gateware, CL016 | `gateware/` | ✗ | Built, not tested on a CL016 board; misses AD9866 TX output timing |
| Kernel driver `radioberry.ko`, overlay, ABI header | `driver/` | ✓ | Loads the gateware at boot; receive and transmit streams |
| `radioberryd`, Protocol 1 | `daemon/` | ✓ | Hermes-Lite 2 dialect, receive and transmit, UDP and TCP; FT8 contacts with piHPSDR and WSJT-X |
| `radioberryd`, Protocol 2 | `daemon/` | ✓ | Receive and transmit; FT8 contacts with piHPSDR and WSJT-X; no wideband, diversity, PureSignal or radio-side CW keyer |
| `radioberry.service` | `daemon/` | ✓ | Unprivileged, options in `/etc/radioberry/config.yaml`, logs to the journal |
| Example FT8 client `ft8rx` | `examples/ft8/` | ✓ | Receive only, Protocol 1 or 2; optional build |
| Gateware loader `rbload` | `tools/rbload/` | ✓ | Loads the FPGA without the driver; checks the image family and CONF_DONE |
| Protocol test clients `p1probe`, `p2probe` | `tools/protoprobe/` | ✓ | Discovery, receive and zero-drive transmit checks over Protocol 1 and 2 |

Open work, in order, is in [TODO/ACTIVE.md](TODO/ACTIVE.md).

## Build and install

Debian packages (`radioberry`, `radioberry-dkms`, `radioberry-dev`), and
how to serve them from your own apt repository, are described in
[docs/packaging.md](docs/packaging.md). To build and install directly
instead, on a Raspberry Pi 5 running Raspberry Pi OS (Debian 13 trixie, 64-bit,
kernel 6.18 or newer), from the repository root:

```
sudo apt install build-essential cmake pkg-config device-tree-compiler \
    linux-headers-rpi-2712 libliquid-dev libgpiod-dev libi2c-dev \
    libyaml-cpp-dev nlohmann-json3-dev libspdlog-dev libsystemd-dev catch2

cmake -S . -B build -DFPGATYPE=CL025   # CL016 if the RadioBerry has a CL016 FPGA
cmake --build build -j
ctest --test-dir build
sudo cmake --install build
```

To tell which FPGA the RadioBerry has, read the marking on the chip's
package: `10CL025YE144C8G` is a CL025, `10CL016YE144C8G` a CL016.

To enable the kernel driver, add the following to
`/boot/firmware/config.txt`, then reboot (first install only):

```
dtparam=i2c_arm=on
dtoverlay=radioberry
```

Then check the radio and start the daemon:

```
rbprobe
sudo systemctl enable --now radioberry
```

`rbprobe` exits 0 when the radio can run and lists each blocker
otherwise. The service starts now and at every boot.

`cmake --install` places:

| What | Where |
|---|---|
| kernel module | `/lib/modules/$(uname -r)/updates/radioberry.ko` (then `depmod`) |
| overlay | `/boot/firmware/overlays/radioberry.dtbo` |
| gateware for `FPGATYPE` | `/lib/firmware/radioberry.rbf` |
| both gateware images | `/usr/local/share/radioberry/gateware/rpi-5/<type>/` |
| ABI header | `/usr/local/include/radioberry_ioctl.h` |
| `rbprobe`, `rbcat`, `rbctl`, `rbload`, `p1probe`, `p2probe` | `/usr/local/bin/` |
| `radioberryd` | `/usr/local/sbin/` |
| `radioberry.service`, sysusers.d entry, udev rule | `/usr/local/lib/systemd/system/`, `/usr/local/lib/sysusers.d/`, `/usr/local/lib/udev/rules.d/` |
| daemon config, if there is none | `/etc/radioberry/config.yaml` |

## Coming from the original software

The original RadioBerry software and this one use the same names:
`radioberry.ko`, `radioberry.dtbo` and `dtoverlay=radioberry`,
`/dev/radioberry`, `radioberry.service` and the device-tree compatible
`sdr,radioberry`. They cannot share a Pi, and a Pi that has had both ends
up with a mix of files: depmod loads whichever module is found first,
whichever overlay was copied last is the one used (the original's places
`sdr,radioberry` on a platform node, so this driver never binds to it and
there is no `/dev/radioberry`), and a unit in `/etc/systemd/system`
overrides the packaged one and runs the original daemon, as root, against
this driver's different ioctl interface.

The original's install script places these files; remove them before
installing this software, or after it if `rbprobe` reports the driver
missing on a Pi that had the original:

| File | Note |
|---|---|
| `/lib/modules/$(uname -r)/kernel/drivers/sdr/radioberry.ko` | one per kernel it was built for; run `sudo depmod` after removing it |
| `/etc/systemd/system/radioberry.service` | `sudo systemctl disable --now radioberry` first |
| `/etc/init.d/radioberryd` | started by that unit |
| `/usr/local/bin/radioberry` | the original daemon |
| `/usr/local/include/radioberry_ioctl.h` | the original's ioctl header; a different interface from this driver's |
| `/boot/firmware/overlays/radioberry.dtbo` | replaced by this software's overlay on install; keep `dtoverlay=radioberry` in `config.txt` |
| `/lib/firmware/radioberry.rbf` | replaced by this software's gateware on install |

Then reboot, and `rbprobe` should exit 0. The `dtoverlay=radioberry` line
the original's installer adds to `config.txt` serves both; add
`dtparam=i2c_arm=on` as above.

## TX & PA

**Transmit and the PA ship OFF, for safety.** The installed
`/etc/radioberry/config.yaml` lists every option with its default and a
one-line description; `allow_tx` and `allow_pa` are `false`, and
`drive_ceiling` is 150. To transmit, set `allow_tx: true` and
`sudo systemctl restart radioberry`. To use the PA as well, connect an
antenna or dummy load rated for its output first, then set
`allow_pa: true` and restart.

## Build options

| Option | Effect |
|---|---|
| `-DFPGATYPE=CL025\|CL016` | which gateware is installed as `/lib/firmware/radioberry.rbf` (default CL025) |
| `-DKERNEL_RELEASE=` | build the module for another kernel (default: the running one, if its headers are installed) |
| `-DRADIOBERRY_BUILD_MODULE=OFF` | skip the module |
| `-DRADIOBERRY_ABI_INCLUDE_DIR=/usr/local/include` | build the tools and daemon against the installed ABI header; configuration fails if its ABI version differs from the source tree's |
| `-DRADIOBERRY_BUILD_TESTS=OFF` | skip the unit tests (and the need for Catch2) |
| `-DRADIOBERRY_BUILD_EXAMPLES=ON` | build `ft8rx` |

## Roadmap / Ongoing work

- If someone out there could test this with a CL016 based RadioBerry, that
  would be great. The CL016 build has a timing issue on the AD9866 TX data
  outputs that moves with the FPGA fitter seed, and no seed tried so far
  meets it ([gateware/PROVENANCE.md](gateware/PROVENANCE.md)). I'd also
  like to crack that.
- There's also a bug in the gateware: the RX FIFO clear is not
  synchronised and the read-out state machine has no reset, so the receive
  stream can lock up at a receiver-count change
  ([docs/fault-rx-lockup.md](docs/fault-rx-lockup.md)). I've worked around
  this in the driver, but the real fix belongs in the gateware itself.
- The AURSINC RadioBerry Preamp Board I'm using is an inexpensive copy with
  manual bias set on a pot. The
  [original design](https://groups.google.com/g/radioberry/c/WQc_afuvViI)
  has an MCP4662 digital potentiometer for the bias and a MAX11613 ADC for
  current and temperature, both on I2C. If that board is available, it
  would be good to get them integrated too.

## AI

This project was developed with the use of agentic engineering,
specifically Claude. If that's a problem for you, there are
[guides for compiling the original driver](https://github.com/pa3gsb/Radioberry-2.x#building).

## Licensing

Licensed under the GNU General Public License, version 2 or (at your
option) any later version; see [LICENSE](LICENSE). Third-party works and
their licences are listed in [docs/licensing.md](docs/licensing.md).
