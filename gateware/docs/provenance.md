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

[tools/build.sh](../tools/build.sh) unpacks it for every build. The images
use its radio core (`gateware/rtl/radio_openhpsdr1/radio.v` and the
modules it instantiates, listed in each project file) unmodified; the
RadioBerry and Hermes-Lite 2 board logic in the archive is not used.

## Images

Built with Quartus Prime Lite 23.1std.1 in the Docker image from
[tools/quartus-image.sh](../tools/quartus-image.sh). The same Quartus
version, source and seed reproduce an image byte for byte.

| File | Project | Seed | Size | SHA-256 |
|---|---|---|---|---|
| `images/rpi5-pio/CL025/radioberry-rpi5-pio.rbf` | `projects/rpi5-pio` | 1 | 388042 | 89a1bc0c1406f0768c356ab528ce6d425c9625839c9f1fa8bac36bfb75aa487e |
| `images/rpi4-i2s/CL025/radioberry-rpi4-i2s.rbf` | `projects/rpi4-i2s` | 3 | 392121 | bc1c30c5c5a5daae710d246479beff13c507f73bdfe68cd1231b2206b8fd3900 |

Device: Cyclone 10 LP 10CL025YE144C8G. The `.rbf` is the raw bitstream,
loaded by passive serial least significant bit first.

## Licences

This project's RTL (`rtl/`) is GPL-2.0-or-later. The Hermes-Lite 2 RTL is
GPL-2.0-or-later by its file headers; the openHPSDR DSP cores under
`gateware/rtl/radio_openhpsdr1/` in the archive are LGPL-2.0-or-later.
The bitfiles also contain Quartus-generated IP (PLL and RAM
megafunctions), covered by the Quartus licence
([toolchain.md](toolchain.md)). See also
[docs/licensing.md](../../docs/licensing.md).
