# Driver

`radioberry.ko`, the Linux kernel driver for the RadioBerry 2.x on a
Raspberry Pi 4 or 5, and the device tree overlay that enables it. It loads
the board's gateware image into the FPGA, streams receive and transmit
samples, and gives userspace one device, `/dev/radioberry`.

| Board | Link to the FPGA | Gateware image |
|---|---|---|
| Raspberry Pi 5 | two RP1 PIO state machines with DMA | `radioberry-rpi5-pio.rbf` |
| Raspberry Pi 4 | I2S into the BCM2711 PCM block with DMA | `radioberry-rpi4-i2s.rbf` |

The same module and overlay serve both boards; the driver picks the link
from the SoC. Using the device, sysfs and the module parameters:
[Kernel driver](../docs/driver.md).

## Layout

| Path | Contents |
|---|---|
| `kernel/radioberry.c` | the module |
| `kernel/rb_logic.h`, `rb_frame.h`, `rb_pio.h` | pure logic with no kernel dependencies: RX parsing, rings, the control protocol, the PIO programs |
| `kernel/Kbuild` | kbuild file |
| `include/radioberry_ioctl.h` | the userspace ABI, installed as `radioberry-dev` |
| `radioberry.dts` | the overlay, `dtoverlay=radioberry` on both boards |
| `tests/` | unit tests of the pure logic (Catch2) |
| `docs/` | design and test records |

## Building

Most people get the driver from the `radioberry-dkms` package, which DKMS
rebuilds for every kernel update. To build it yourself on the Pi you need
the headers of the running kernel (`sudo apt install
linux-headers-$(uname -r)`).

With the rest of the project, from the repository root:

```
cmake -S . -B build
cmake --build build --target radioberry_module radioberry_overlay
```

The module is `build/driver/kbuild/radioberry.ko`, the overlay
`build/driver/radioberry.dtbo`. `sudo cmake --install build` installs them
with everything else ([Building from source](../docs/building.md)).

On its own, with kbuild:

```
make -C /lib/modules/$(uname -r)/build M=$PWD/driver/kernel \
    RB_INCLUDE=$PWD/driver/include modules
dtc -@ -I dts -O dtb -o radioberry.dtbo driver/radioberry.dts
```

To try a build without installing it, stop the service, swap the module
and start the service again:

```
sudo systemctl stop radioberry
sudo rmmod radioberry
sudo insmod driver/kernel/radioberry.ko
sudo systemctl start radioberry
```

The driver loads its gateware image from `/lib/firmware`
(`radioberry-rpi5-pio.rbf` or `radioberry-rpi4-i2s.rbf`), which the
packages and `cmake --install` put there.

## Testing

- Unit tests of the pure logic: `build/driver/driver_tests` (also run by
  `ctest --test-dir build`).
- On the Pi with the RadioBerry fitted: `rbdrivertest`, through
  `/dev/radioberry` ([docs/driver-tests.md](docs/driver-tests.md)).
- Static checks: `checkpatch.pl` and `sparse`, run by
  [tools/ci-pi.sh](../tools/ci-pi.sh).
- Under a kernel with the runtime checkers on:
  [docs/kernel-debug.md](docs/kernel-debug.md).

## Documents

| Document | Contents |
|---|---|
| [docs/design.md](docs/design.md) | how the driver works on each board |
| [docs/driver-tests.md](docs/driver-tests.md) | the hardware test harness |
| [docs/kernel-debug.md](docs/kernel-debug.md) | a run under a debug kernel |
| [Kernel driver](../docs/driver.md) | using it: device, sysfs, parameters |
| [Kernels](../docs/kernels.md) | supported and tested kernels |
| [gateware/docs/interfaces.md](../gateware/docs/interfaces.md) | what the gateware presents to the driver |
