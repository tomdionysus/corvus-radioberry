# Licensing

This repository is licensed under the GNU General Public License, version
2 or (at your option) any later version; see [LICENSE](https://github.com/tomdionysus/corvus-radioberry/blob/main/LICENSE). Every source file
carries an SPDX header:

```
// SPDX-License-Identifier: GPL-2.0-or-later
```

GPL-2.0-or-later is required by the kernel module, which links against
`EXPORT_SYMBOL_GPL` symbols of the Raspberry Pi `rp1-pio` driver, and is
compatible with every work below. Code taken from a third-party work keeps
that work's licence header and copyright line beneath the SPDX line.

## Third-party works

| Work | Licence | Use here | Reference |
|---|---|---|---|
| RadioBerry 2.x driver, daemon, overlays, gateware loader | Unlicense (public domain), stated in each source file | ABI, pin map and behaviour reference | https://github.com/pa3gsb/Radioberry-2.x |
| Hermes-Lite 2 gateware, RadioBerry PIO variants | GPL-2.0-or-later (file headers) | Source in `gateware/source/`, unmodified; built here, bitfiles in `gateware/rpi-5/` | https://github.com/softerhardware/Hermes-Lite2 |
| openHPSDR DSP cores used by Hermes-Lite 2 (CIC, FIR, CORDIC, receiver) | LGPL-2.0-or-later (file headers) | Contained in the bitfiles | https://github.com/softerhardware/Hermes-Lite2/tree/master/gateware/rtl/radio_openhpsdr1 |
| Hermes-Lite 2 protocol description | project wiki | Protocol 1 dialect, PA and bias commands | https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol |
| openHPSDR Ethernet Protocol v4.x and Protocol 1 firmware sources | GPL-2.0 | Protocol 2 reference | https://github.com/TAPR/OpenHPSDR-Firmware |
| Raspberry Pi Linux kernel, `rp1-pio` driver | GPL-2.0 | Kernel API the driver is built on | https://github.com/raspberrypi/linux |
| liquid-dsp | MIT | DSP library linked by the daemon, `ft8rx` and `wsprrx` | https://github.com/jgaeddert/liquid-dsp |
| lookup3 hash (Bob Jenkins) | public domain | `hashlittle`, written out in `examples/wspr/src/wspr/codec.cpp` for WSPR's type 3 callsign hash | https://burtleburtle.net/bob/c/lookup3.c |
| WSJT-X `wsprsim` and `wsprd` | GPL-3.0 | Reference output for `wspr_tests` and the decoder comparison; not linked, no code taken | https://wsjt.sourceforge.io/wsjtx.html |
| ft8_lib | MIT | Message text code vendored in `examples/ft8/src/ft8_lib/` with its licence; the decoder in `examples/ft8/src/ft8/` is a C++ port of it, GPL-2.0-or-later here | https://github.com/kgoba/ft8_lib |
| piHPSDR | GPL-2.0 | Reference client; not linked | https://github.com/dl1ycf/pihpsdr |
| Quartus Prime Lite Edition and its generated IP (PLL, FIFO) | Quartus Prime licence agreement, accepted by whoever builds | Gateware toolchain ([gateware/QUARTUS.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/QUARTUS.md)); not redistributed; its IP is inside the bitfiles | https://www.altera.com/downloads |

"Hermes", "Hermes-Lite", "openHPSDR" and "RadioBerry" are the names of
their respective projects.
