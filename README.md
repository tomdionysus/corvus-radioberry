# corvus-radioberry

Version 0.1.1

A self-contained software stack for the Radioberry 2.x HF transceiver HAT
on a Raspberry Pi 5:

- `radioberry.ko`, the Linux kernel driver
- `radioberryd`, a daemon speaking openHPSDR Protocol 1 and Protocol 2
- signal processing with liquid-dsp
- platform diagnostics
- control of an [AURSINC PA5W V2](https://www.aursinc.com/products/aursinc-radioberry-preamp-board-v2-output-1-5w-3w-for-hf-power-ampfier-10w-100w-pa-for-raspberry-pi-4-4b) amplifier board
- FPGA gateware built from its GPL source

Every component is built from this repository against the current
Raspberry Pi OS kernel (6.18 and newer) with no out-of-tree kernel
patches.

The Radioberry and the upstream software this project builds on come from
the [Radioberry project](https://github.com/pa3gsb/Radioberry-2.x). A
[ready-to-run Raspberry Pi 5 image](https://pa3gsb.gumroad.com/l/radioberry)
of the upstream software is sold by the Radioberry's author; buying it
supports their work.

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
| Gateware, CL025 (HL2 Radioberry PIO variant 75.3) | `gateware/` | ✓ | Built from source, verified on hardware; its receive lock-up is worked around by the driver |
| Gateware, CL016 | `gateware/` | ✗ | Built, not tested on a CL016 board; misses AD9866 TX output timing |
| Kernel driver `radioberry.ko`, overlay, ABI header | `driver/` | ✓ | Loads the gateware at boot; receive and transmit streams |
| `radioberryd`, Protocol 1 | `daemon/` | ✓ | Hermes-Lite 2 dialect, receive and transmit, UDP and TCP; FT8 contacts with piHPSDR and WSJT-X |
| `radioberryd`, Protocol 2 | `daemon/` | ✓ | Receive and transmit; FT8 contacts with piHPSDR and WSJT-X; no wideband, diversity, PureSignal or radio-side CW keyer |
| `radioberry.service` | `daemon/` | ✓ | Unprivileged, options in `/etc/radioberry/config.yaml`, logs to the journal |
| Example FT8 client `ft8rx` | `examples/ft8/` | ✓ | Receive only, Protocol 1 or 2; optional build |
| Gateware loader `rbload` | `tools/` | ✓ | Loads the FPGA without the driver; checks the image family and CONF_DONE |
| Protocol test clients `p1probe`, `p2probe` | `tools/` | ✓ | Discovery, receive and zero-drive transmit checks over Protocol 1 and 2 |

Open work, in order, is in `TODO/ACTIVE.md`.

## Build and install

On a Raspberry Pi 5 running Raspberry Pi OS (Debian 13 trixie, 64-bit,
kernel 6.18 or newer), from the repository root:

```
sudo apt install build-essential cmake pkg-config device-tree-compiler \
    linux-headers-rpi-2712 libliquid-dev libgpiod-dev libi2c-dev \
    libyaml-cpp-dev nlohmann-json3-dev libspdlog-dev libsystemd-dev catch2

cmake -S . -B build -DFPGATYPE=CL025   # CL016 if the Radioberry has a CL016 FPGA
cmake --build build -j
ctest --test-dir build
sudo cmake --install build
```

To tell which FPGA the Radioberry has, read the marking on the chip's
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

- If someone out there could test this with a CL016 based Radioberry, that
  would be great. The CL016 build has a timing issue on the AD9866 TX data
  outputs that moves with the FPGA fitter seed, and no seed tried so far
  meets it ([gateware/PROVENANCE.md](gateware/PROVENANCE.md)). I'd also
  like to crack that.
- There's also a bug in the gateware: the RX FIFO clear is not
  synchronised and the read-out state machine has no reset, so the receive
  stream can lock up at a receiver-count change
  ([docs/fault-rx-lockup.md](docs/fault-rx-lockup.md)). I've worked around
  this in the driver, but the real fix belongs in the gateware itself.
- The AURSINC Radioberry Preamp Board I'm using is an inexpensive copy with
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
