# Building from source

Most people install the packages ([Quickstart](quickstart.md)). To build
and install from source instead, on a Raspberry Pi 4 or 5 running
Raspberry Pi OS (Debian 13 trixie, 64-bit), from the repository root:

```
sudo apt install build-essential cmake pkg-config device-tree-compiler \
    linux-headers-$(uname -r) libliquid-dev libgpiod-dev libi2c-dev \
    libyaml-cpp-dev nlohmann-json3-dev libspdlog-dev libsystemd-dev catch2

cmake -S . -B build -DFPGATYPE=CL025   # CL016 if the RadioBerry has a CL016 FPGA
cmake --build build -j
ctest --test-dir build
sudo cmake --install build
```

The FPGA type is printed on the chip: `10CL025YE144C8G` is a CL025,
`10CL016YE144C8G` a CL016. Then enable the driver, configure and start the
service as in the [Quickstart](quickstart.md), steps 2 to 5.

To build the Debian packages instead, see [Debian packages](packaging.md).

## Build options

| Option | Effect |
|---|---|
| `-DFPGATYPE=CL025\|CL016` | which gateware is installed as `/lib/firmware/radioberry.rbf` (default CL025) |
| `-DKERNEL_RELEASE=` | build the module for another kernel (default: the running one, if its headers are installed) |
| `-DRADIOBERRY_BUILD_MODULE=OFF` | skip the module |
| `-DRADIOBERRY_ABI_INCLUDE_DIR=/usr/local/include` | build the tools and daemon against the installed ABI header; configuration fails if its ABI version differs from the source tree's |
| `-DRADIOBERRY_BUILD_TESTS=OFF` | skip the unit tests (and the need for Catch2) |
| `-DRADIOBERRY_BUILD_EXAMPLES=ON` | build `ft8rx` and `wsprrx` |

## What `cmake --install` places

| What | Where |
|---|---|
| kernel module | `/lib/modules/$(uname -r)/updates/radioberry.ko` (then `depmod`) |
| overlay | `/boot/firmware/overlays/radioberry.dtbo` |
| gateware for `FPGATYPE` | `/lib/firmware/radioberry.rbf` |
| both gateware images | `/usr/local/share/radioberry/gateware/rpi-5/<type>/` |
| ABI header | `/usr/local/include/radioberry_ioctl.h` |
| `rbprobe`, `rbcat`, `rbctl`, `rbload`, `rbbus`, `p1probe`, `p2probe` | `/usr/local/bin/` |
| `radioberryd` | `/usr/local/sbin/` |
| `radioberry.service`, sysusers.d entry, udev rule | `/usr/local/lib/systemd/system/`, `/usr/local/lib/sysusers.d/`, `/usr/local/lib/udev/rules.d/` |
| `i2c-dev` loaded at boot | `/usr/local/lib/modules-load.d/radioberry.conf` |
| daemon config, if there is none | `/etc/radioberry/config.yaml` |

After a kernel update, build and install again; the packages do this
automatically.
