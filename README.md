# corvus-radioberry

Version 0.1.1

A self-contained software stack for the Radioberry 2.x HF transceiver HAT
on a Raspberry Pi 5: Linux kernel driver, a radio daemon speaking
openHPSDR Protocol 1 and Protocol 2 (DSP by liquid-dsp), platform
diagnostics, and control of the AURSINC PA5W V2 amplifier board, with the
FPGA gateware built from its GPL source. Every component is built from
this repository against the current Raspberry Pi OS kernel (6.18 and
newer) with no out-of-tree kernel patches.

The Radioberry and the upstream software this project builds on come from
the Radioberry project (https://github.com/pa3gsb/Radioberry-2.x). A
ready-to-run Raspberry Pi 5 image of the upstream software is sold at
https://pa3gsb.gumroad.com/l/radioberry; buying it supports the
Radioberry's author.

The code is C++20, with C only where the kernel or a shared ABI header
requires it, tested with Catch2. All dependencies are Debian trixie
packages. Licensed GPL-2.0-or-later.

Documentation:

| Document | Contents |
|---|---|
| `docs/platform.md` | supported hardware, the development Pi, kernel facts, real-time and jitter measurements |
| `docs/pa.md` | the PA5W V2 amplifier board |
| `docs/design.md` | architecture of the driver, daemon and probe |
| `docs/driver.md` | kernel driver: device interface, sysfs, module parameters |
| `docs/daemon.md` | `radioberryd`: options, service, Protocols 1 and 2, client notes |
| `docs/tools.md` | `rbprobe`, `rbcat`, `rbctl` |
| `docs/examples.md` | `ft8rx`, sample order, frequency calibration |
| `docs/fault-rx-lockup.md` | the gateware's receive lock-up and the driver's workaround |
| `gateware/README.md` | the gateware: interface the driver depends on |
| `gateware/PROVENANCE.md`, `gateware/QUARTUS.md` | gateware source, build, timing, toolchain |
| `docs/licensing.md` | licence and third-party works |
| `CONTRIBUTING.md` | repository layout, CI, working conventions |

## Status

| Component | Directory | Working | Notes |
|---|---|---|---|
| Platform probe `rbprobe` | `tools/rbprobe/` | yes | Host, boot, driver, gateware, FPGA, PA board and daemon state; text, JSON or YAML |
| Stream tool `rbcat` | `tools/rbcat/` | yes | Receive streams and measurements without the daemon |
| Command tool `rbctl` | `tools/rbctl/` | yes | Single gateware commands; anything that can transmit needs `--allow-tx` |
| Gateware, CL025 (HL2 Radioberry PIO variant 75.3) | `gateware/` | yes | Built from source, verified on hardware; its receive lock-up is worked around by the driver |
| Gateware, CL016 | `gateware/` | no | Built, not tested on a CL016 board; misses AD9866 TX output timing |
| Kernel driver `radioberry.ko`, overlay, ABI header | `driver/` | yes | Loads the gateware at boot; receive and transmit streams |
| `radioberryd`, Protocol 1 | `daemon/` | yes | Hermes-Lite 2 dialect, receive and transmit, UDP and TCP; FT8 contacts with piHPSDR and WSJT-X |
| `radioberryd`, Protocol 2 | `daemon/` | yes | Receive and transmit; FT8 contacts with piHPSDR and WSJT-X; no wideband, diversity, PureSignal or radio-side CW keyer |
| `radioberry.service` | `daemon/` | yes | Unprivileged, options in `/etc/radioberry/config.yaml`, logs to the journal |
| Example FT8 client `ft8rx` | `examples/ft8/` | yes | Receive only, Protocol 1 or 2; optional build |
| Gateware loader `rbload` | `tools/` | no | Not started |
| Protocol test clients `p1probe`, `p2probe` | `tools/` | no | Not started |

Open work, in order, is in `TODO/ACTIVE.md`.

## Build and install

On a Raspberry Pi 5 running Raspberry Pi OS (Debian 13 trixie, 64-bit,
kernel 6.18 or newer), from the repository root:

```
sudo apt install build-essential cmake device-tree-compiler \
    linux-headers-rpi-2712 libliquid-dev libgpiod-dev libi2c-dev i2c-tools \
    libyaml-cpp-dev nlohmann-json3-dev libspdlog-dev libsystemd-dev \
    catch2 lcov clang-format-19 rsync
cmake -S . -B build -DFPGATYPE=CL025   # or CL016
cmake --build build -j
ctest --test-dir build
sudo cmake --install build
```

Add to `/boot/firmware/config.txt`, then reboot (first install only):

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
| `rbprobe`, `rbcat`, `rbctl` | `/usr/local/bin/` |
| `radioberryd` | `/usr/local/sbin/` |
| `radioberry.service`, sysusers.d entry, udev rule | `/usr/local/lib/systemd/system/`, `/usr/local/lib/sysusers.d/`, `/usr/local/lib/udev/rules.d/` |
| daemon config, if there is none | `/etc/radioberry/config.yaml` |

**Transmit and the PA ship OFF, for safety.** The installed
`/etc/radioberry/config.yaml` lists every option with its default and a
one-line description; `allow_tx` and `allow_pa` are `false`, and
`drive_ceiling` is 150. To transmit, set `allow_tx: true` and
`sudo systemctl restart radioberry`. To use the PA as well, connect an
antenna or dummy load rated for its output first, then set
`allow_pa: true` and restart.

Build options: the module is built when headers for the running kernel
are installed (`-DKERNEL_RELEASE=` picks another kernel,
`-DRADIOBERRY_BUILD_MODULE=OFF` skips it).
`-DRADIOBERRY_ABI_INCLUDE_DIR=/usr/local/include` builds the tools and
daemon against the installed ABI header; configuration fails if its ABI
version differs from the source tree's. `-DRADIOBERRY_BUILD_EXAMPLES=ON`
builds `ft8rx`.
