# Gateware provenance

Status 2026-11-08.

## Source

`source/Hermes-Lite2-3e467fd-radioberry-pio.tar.xz` (166588 bytes, SHA-256
1e7db1538e035bb8838a5726d95c9b924ff5e186913cae170dfc82285b4b26d7) is
`git archive` of the Hermes-Lite 2 repository
(https://github.com/softerhardware/Hermes-Lite2) at commit
3e467fd51572b4bbf9a28283c7562f1cb541e313 ("radioberry cw improvement",
2026-07-30), limited to `gateware/rtl/`, the variant projects
`gateware/variants/radioberry_pio_cl025/` and
`gateware/variants/radioberry_pio_cl016/`, and the top-level `README.md`:

```
git archive --format=tar --prefix=Hermes-Lite2-3e467fd/ 3e467fd \
    gateware/rtl gateware/variants/radioberry_pio_cl025 \
    gateware/variants/radioberry_pio_cl016 README.md | xz -9e
```

This is the newest change to the Radioberry PIO gateware: it sets
`VERSION_MAJOR` 75 and `VERSION_MINOR` 3 in
`gateware/rtl/radioberry/pi-pio/radioberry_core.v`. Later commits in that
repository touch only the Hermes-Lite 2 Ethernet code, which the
Radioberry variants do not use.

## Bitfiles

`tools/build-gateware.sh` compiles both variants from the archive with
Quartus Prime Lite 23.1std.1, the version the upstream project files were
last saved with (`LAST_QUARTUS_VERSION` in each `radioberry.qsf`), and
writes:

| File | FPGA | Size | SHA-256 |
|---|---|---|---|
| `rpi-5/CL025/radioberry.rbf` | Cyclone 10 LP 10CL025YE144C8G | not built yet | not built yet |
| `rpi-5/CL016/radioberry.rbf` | Cyclone 10 LP 10CL016YE144C8G | not built yet | not built yet |

The script fills in this table from its output. The `.rbf` is the raw
binary the project files ask Quartus for (`GENERATE_RBF_FILE ON`), loaded
by passive serial least significant bit first. Quartus output is not
byte-reproducible across tool versions, so the hashes identify this
repository's build, not the upstream source.

## Licences

The Hermes-Lite 2 RTL is GPL-2.0-or-later by its file headers; the
openHPSDR DSP cores under `gateware/rtl/radio_openhpsdr1/` are
LGPL-2.0-or-later; the Radioberry-specific RTL under
`gateware/rtl/radioberry/` is by Johan Maas, PA3GSB. The bitfiles also
contain Quartus generated IP (PLL and FIFO megafunctions), covered by the
Quartus licence described in `QUARTUS.md`. See the attribution table in
the top-level `README.md`.
