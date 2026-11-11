# Requirements

- Raspberry Pi 4 Model B or Raspberry Pi 5, running Raspberry Pi OS
  (Debian 13 trixie, 64-bit) with its current kernel; each board's
  minimum is in [Raspberry Pi 4](rpi4.md) and [Raspberry Pi 5](rpi5.md).
- RadioBerry 2.x HAT: AD9866 front end, Intel Cyclone 10 LP 10CL025 or
  10CL016, 0 to 30 MHz.
- Optional: the AURSINC "RadioBerry Preamp Board V2" (PCB marked PA5W V2)
  stacked on the RadioBerry, 1.5 to 3.5 W output ([PA5W V2](pa.md)).

The driver uses GPIO 4, 5, 6, 12, 13, 18 to 22 and 24 to 27, SPI0 and I2C1.

Use this project's gateware, installed with it. The original RadioBerry
software's gateware images do not work with this driver; `rbprobe` names
them if one is installed.
