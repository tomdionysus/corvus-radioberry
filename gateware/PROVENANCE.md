# Gateware provenance

The bitfiles in this directory are redistributed unmodified. They are not
built here.

## Bitfiles

| File | FPGA | Size | SHA-256 |
|---|---|---|---|
| `rpi-5/CL025/radioberry.rbf` | Cyclone 10 LP 10CL025YE144C8G | 334915 | fe6a4e8acb67fbf80aa679affc8f2e0e0abe15908938cb2aed10804b461105cd |
| `rpi-5/CL016/radioberry.rbf` | Cyclone 10 LP 10CL016YE144C8G | 275438 | 5f146a8c2113a888bd6b7d08d8ac9f62b796e79dd950ecd47b31b5ec1ebe9e48 |

Both are copied from the Radioberry-2.x repository
(https://github.com/pa3gsb/Radioberry-2.x), tag `radioberry@rpi5`, commit
269e4028ae6a883f6a670e5faab9248b285b2d7f ("firmware update and pi5 driver
update", 2025-12-16), paths `SBC/rpi-5/releases/dev/CL025/radioberry.rbf`
and `SBC/rpi-5/releases/dev/CL016/radioberry.rbf`. The same hashes are in
the `rbprobe` image catalogue (`tools/common/src/gateware_catalog.cpp`).

## Corresponding source

`source/Hermes-Lite2-7472bd1-radioberry-pio.tar.xz`
(166448 bytes, SHA-256
e192f80587410b81c89fcea76201d117ad5179ed2fa2211ced54f77a40f11ba4) is
`git archive` of the Hermes-Lite 2 repository
(https://github.com/softerhardware/Hermes-Lite2) at commit
7472bd130449b20eb22962dd0766e903da844d23 ("v75.2 for radioberry pio mode",
2025-11-24), limited to `gateware/rtl/`, the two variant projects
`gateware/variants/radioberry_pio_cl025/` and
`gateware/variants/radioberry_pio_cl016/`, and the top-level `README.md`.
It was made with:

```
git archive --format=tar --prefix=Hermes-Lite2-7472bd1/ 7472bd1 \
    gateware/rtl gateware/variants/radioberry_pio_cl025 \
    gateware/variants/radioberry_pio_cl016 README.md | xz -9e
```

Why this commit: the bitfiles were committed to Radioberry-2.x on
2025-12-16. The last change to the Radioberry PIO gateware in Hermes-Lite 2
before that date is 7472bd1, which sets `VERSION_MAJOR` 75 and
`VERSION_MINOR` 2 in `gateware/rtl/radioberry/pi-pio/radioberry_core.v`;
the next change to the RTL or the two variants is dated 2026-07-30. The
bitfiles have not been rebuilt here to prove byte identity (Quartus output
is not reproducible across tool versions). The driver reads the gateware
version over SPI; version 75.2 on the development board confirms the
match as far as the version register can.

Rebuilding needs Intel Quartus Prime Lite with Cyclone 10 LP support:
`make` in either variant directory, then convert the `.sof` to a raw binary
`.rbf` (passive serial, bit order as the loader in `tools/common` expects:
least significant bit of each byte first).

## Licences

The Hermes-Lite 2 RTL is GPL-2.0-or-later by its file headers; the
openHPSDR DSP cores under `gateware/rtl/radio_openhpsdr1/` are
LGPL-2.0-or-later; the Radioberry-specific RTL under
`gateware/rtl/radioberry/` comes from Johan Maas, PA3GSB. The bitfiles also
contain Intel Quartus generated IP (PLL, FIFO megafunctions). See the
attribution table in the top-level `README.md`.
