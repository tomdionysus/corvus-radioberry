# Gateware provenance

Status 2026-11-08.

## Source

`source/Hermes-Lite2-3e467fd-radioberry-pio.tar.xz` (167992 bytes, SHA-256
a4d5dd1585447ace697385e53966092c0a09a6501867709f38877c50dc1e91fe) is
`git archive` of the Hermes-Lite 2 repository
(https://github.com/softerhardware/Hermes-Lite2) at commit
3e467fd51572b4bbf9a28283c7562f1cb541e313 ("radioberry cw improvement",
2026-07-30), limited to `gateware/rtl/`, the board files
`gateware/boards/radioberry-pio/` (pin, file and general settings, and the
timing constraints `timing.sdc`), the variant projects
`gateware/variants/radioberry_pio_cl025/` and
`gateware/variants/radioberry_pio_cl016/`, and the top-level `README.md`:

```
git archive --format=tar --prefix=Hermes-Lite2-3e467fd/ 3e467fd \
    gateware/rtl gateware/boards/radioberry-pio \
    gateware/variants/radioberry_pio_cl025 \
    gateware/variants/radioberry_pio_cl016 README.md | xz -9e
```

This is the newest change to the Radioberry PIO gateware: it sets
`VERSION_MAJOR` 75 and `VERSION_MINOR` 3 in
`gateware/rtl/radioberry/pi-pio/radioberry_core.v`. Later commits in that
repository touch only the Hermes-Lite 2 Ethernet code, which the
Radioberry variants do not use.

## Bitfiles

`tools/build-gateware.sh` compiles both variants from the archive,
unmodified, with Quartus Prime Lite 23.1std.1 (the version the upstream
project files were last saved with, `LAST_QUARTUS_VERSION` in each
`radioberry.qsf`) and the fitter seed below, then checks timing against
the upstream constraints, and writes:

| File | FPGA | Size | SHA-256 |
|---|---|---|---|
| `rpi-5/CL025/radioberry.rbf` | Cyclone 10 LP 10CL025YE144C8G | 4 | 372888 | a25f30f4bc9ba3c3f7fbe3406f4a4a27da99be7e90c5272d1b6c05ee0acecedc |
| `rpi-5/CL016/radioberry.rbf` | Cyclone 10 LP 10CL016YE144C8G | not built: timing | - | - |

The script fills in this table from its output. The same tool version and
seed reproduce the images byte for byte; another Quartus version gives
different bytes. The `.rbf` is the raw binary the project files ask
Quartus for (`GENERATE_RBF_FILE ON`), loaded by passive serial least
significant bit first.

## Timing

Against the upstream `timing.sdc`, in all three Quartus timing corners:

- Both variants: the path from the SPI command toggle (`cmd_cnt`, clocked
  by the SPI slave's `done`) into the first stage of its synchroniser
  (`syncio_cmd_rqst|sync_chain`) shows negative setup slack in the 10 MHz
  domain. It is an intended clock-domain crossing into a synchroniser that
  the constraints do not mark as false; it is not a timing failure.
- CL025, seed 4: every other path meets timing.
- CL016: no fitter seed from 1 to 8 meets the AD9866 TX output
  constraints (2.5 ns output delay at 153.6 MHz). `rffe_ad9866_tx[1]`
  misses by about 3.2 ns, its route to the pin taking longer than the
  6.5 ns period, and the other TX data pins and `rffe_ad9866_txsync` by up
  to 0.29 ns. The upstream constraints relax only `rffe_ad9866_tx[4]`
  (`set_max_delay 10`). No CL016 image is built until this is resolved;
  the development board is a CL025.

The build script allows only the synchroniser crossing; a variant with
any other negative slack is not written and the script exits non-zero. Its logs and timing reports go to `build-gateware/` (not
committed).

## Licences

The Hermes-Lite 2 RTL is GPL-2.0-or-later by its file headers; the
openHPSDR DSP cores under `gateware/rtl/radio_openhpsdr1/` are
LGPL-2.0-or-later; the Radioberry-specific RTL under
`gateware/rtl/radioberry/` is by Johan Maas, PA3GSB. The bitfiles also
contain Quartus generated IP (PLL and FIFO megafunctions), covered by the
Quartus licence described in `QUARTUS.md`. See the attribution table in
the top-level `README.md`.
