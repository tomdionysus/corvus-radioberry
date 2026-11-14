# Requirements

- Raspberry Pi 4 Model B or Raspberry Pi 5, running Raspberry Pi OS
  (Debian 13 trixie, 64-bit) with a 6.12 or 6.18 kernel; the tested
  kernels are in [Kernels](kernels.md).
- RadioBerry 2.x HAT with the Intel Cyclone 10 LP 10CL025 FPGA (the chip
  is marked `10CL025YE144C8G`): AD9866 front end, 0 to 30 MHz.
- Optional: the AURSINC "RadioBerry Preamp Board V2" (PCB marked PA5W V2)
  stacked on the RadioBerry, 1.5 to 3.5 W output ([PA5W V2](pa.md)).

The driver uses SPI0, I2C1, GPIO 13, 22, 24, 26 and 27 to load the FPGA,
and for the samples GPIO 4, 5, 6, 12, 18 to 21 and 25 on a Pi 5, or GPIO
18 to 21 (the PCM block) on a Pi 4.

Use this project's gateware, installed with it. The original RadioBerry
software's gateware images do not work with this driver; `rbprobe` names
them if one is installed.
