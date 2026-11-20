# Provenance

## Radio core source

[../source/Hermes-Lite2-3e467fd-radioberry-pio.tar.xz](../source/Hermes-Lite2-3e467fd-radioberry-pio.tar.xz)
(167992 bytes, SHA-256
a4d5dd1585447ace697385e53966092c0a09a6501867709f38877c50dc1e91fe) is a
`git archive` of the Hermes-Lite 2 repository
(https://github.com/softerhardware/Hermes-Lite2) at commit
3e467fd51572b4bbf9a28283c7562f1cb541e313, limited to `gateware/rtl/`,
the RadioBerry PIO board and variant files, and the top-level
`README.md`:

```
git archive --format=tar --prefix=Hermes-Lite2-3e467fd/ 3e467fd \
    gateware/rtl gateware/boards/radioberry-pio \
    gateware/variants/radioberry_pio_cl025 \
    gateware/variants/radioberry_pio_cl016 README.md | xz -9e
```

[tools/build.sh](../tools/build.sh) unpacks it for every build and applies
[../source/hl2-conformance.patch](../source/hl2-conformance.patch): edits
that make the core conform to IEEE 1800-2005 (body parameters with
defaults, `always_comb`).

What the tree's projects take from the archive:

- The core's top is this project's
  [../rtl/core/rbv_core.sv](../rtl/core/rbv_core.sv); its keying state
  machine keeps the states and timings of the archive's
  `gateware/rtl/radio_openhpsdr1/radio.v`.
- The NCO's sine tables (`gateware/rtl/nco/`) and the receive and
  transmit filters' coefficients (`gateware/rtl/radio_openhpsdr1/firrom/`)
  are read from the archive at build time.
- No module of the archive is compiled: the blocks the core
  instantiates are this project's ([../rtl/core/](../rtl/core/)), each
  simulated against the archive's module it replaced
  ([tools/sim.sh](../tools/sim.sh)). The RadioBerry and Hermes-Lite 2
  board logic in the archive is not used.

The committed images below are built from this tree's own RTL. From the
archive they contain only data: the NCO's coarse sine table
(`gateware/rtl/nco/sincos_coarse.txt`; the fine corrections of its
`sin_fine.txt` are computed, `rtl/core/sincos.sv`) and the receive
filter's coefficients (`gateware/rtl/radio_openhpsdr1/firrom/coefL4*.mif`,
by James Ahlstrom N2ADR and Phil Harman VK6APH).

## Images

Built with Quartus Prime Lite 23.1std.1 in the Docker image from
[tools/quartus-image.sh](../tools/quartus-image.sh). The same Quartus
version, source and seed reproduce an image byte for byte.

| File | Project | Seed | Size | SHA-256 |
|---|---|---|---|---|
| `images/rpi5-pio/CL025/radioberry-rpi5-pio.rbf` | `projects/rpi5-pio` | 3 | 320981 | cffc107fef81016616cd9252d30e052a0053571c3c3451deb02ddf2066110afc |
| `images/rpi4-i2s/CL025/radioberry-rpi4-i2s.rbf` | `projects/rpi4-i2s` | 3 | 324561 | 25943ca3eade77d19fdb0333a61a4288e581de8621420341f61976f7a990b196 |

Each is the seed tested on its board (receive, keyed runs into a dummy
load, an FT8 contact), rebuilt at the release commit byte for byte.

Both were also checked with this project's own toolchain, Miach
(2026-11-20): its bitstream writer reproduces each byte for byte, every
clock meets setup and hold at all three corners on Quartus's routing
(five clock endpoints of the Pi 5 image's Pi RX clock, on general
routing, are not covered by it; Quartus times them), and its equivalence
check of the sources finds no difference (27 770 and 28 599 obligations
proven).

Device: Cyclone 10 LP 10CL025YE144C8G. The `.rbf` is the raw bitstream,
loaded by passive serial least significant bit first.

## Licences

This project's RTL (`rtl/`) is GPL-2.0-or-later. The
Hermes-Lite 2 RTL is GPL-2.0-or-later by its file headers; the openHPSDR
DSP cores under `gateware/rtl/radio_openhpsdr1/` in the archive are
LGPL-2.0-or-later, and none is compiled into the committed images. The
tables and coefficients the images contain (above) carry no licence
header of their own.
The bitfiles also contain Quartus-generated IP (PLL and RAM
megafunctions), covered by the Quartus licence
([toolchain.md](toolchain.md)). See also
[docs/licensing.md](../../docs/licensing.md).
