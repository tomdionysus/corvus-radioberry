# Gateware provenance

## Source

[source/Hermes-Lite2-3e467fd-radioberry-pio.tar.xz](source/Hermes-Lite2-3e467fd-radioberry-pio.tar.xz) (167992 bytes, SHA-256
a4d5dd1585447ace697385e53966092c0a09a6501867709f38877c50dc1e91fe) is
`git archive` of the Hermes-Lite 2 repository
(https://github.com/softerhardware/Hermes-Lite2) at commit
3e467fd51572b4bbf9a28283c7562f1cb541e313 ("radioberry cw improvement"),
limited to `gateware/rtl/`, the board files
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

This is the newest change to the RadioBerry PIO gateware: it sets
`VERSION_MAJOR` 75 and `VERSION_MINOR` 3 in
`gateware/rtl/radioberry/pi-pio/radioberry_core.v`. Later commits in that
repository touch only the Hermes-Lite 2 Ethernet code, which the
RadioBerry variants do not use.

## Bitfiles

[tools/build-gateware.sh](../tools/build-gateware.sh) compiles both variants from the archive,
unmodified, with Quartus Prime Lite 23.1std.1 (the version the upstream
project files were last saved with, `LAST_QUARTUS_VERSION` in each
`radioberry.qsf`) and the fitter seed below, then checks timing against
the upstream constraints, and writes:

| File | FPGA | Seed | Size | SHA-256 |
|---|---|---|---|---|
| `rpi-5/CL025/radioberry.rbf` | Cyclone 10 LP 10CL025YE144C8G | 4 | 372888 | a25f30f4bc9ba3c3f7fbe3406f4a4a27da99be7e90c5272d1b6c05ee0acecedc |
| `rpi-5/CL016/radioberry.rbf` | Cyclone 10 LP 10CL016YE144C8G | 5 | 274527 | fc975a3671496db6e45456f7cc0f1008df67f033b4fd17e80bc89cdfbfef4d20 |

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
- CL016, seed 5: the AD9866 TX data outputs miss their output delay
  (2.5 ns at 153.6 MHz) in the worst corner: `rffe_ad9866_tx[1]` by
  3.23 ns, its route to the pin being longer than the 6.5 ns period;
  `rffe_ad9866_tx[0,2,3,5]` by up to 0.28 ns; `rffe_ad9866_txsync` by
  0.09 ns. No seed from 1 to 8 meets these constraints, and the upstream
  constraints relax only `rffe_ad9866_tx[4]` (`set_max_delay 10`). Every
  other path meets timing. The image ships with this violation; transmit
  quality on a CL016 board has not been tested.

The build script allows exactly these exceptions (the synchroniser
crossing on both, the AD9866 TX outputs down to -3.5 ns on the CL016); a
variant with any other negative slack is not written and the script exits
non-zero. Its logs and timing reports go to `build-gateware/` (not
committed).

## Licences

The Hermes-Lite 2 RTL is GPL-2.0-or-later by its file headers; the
openHPSDR DSP cores under `gateware/rtl/radio_openhpsdr1/` are
LGPL-2.0-or-later; the RadioBerry-specific RTL under
`gateware/rtl/radioberry/` is from the RadioBerry project. The bitfiles
also contain Quartus generated IP (PLL and FIFO megafunctions), covered by
the Quartus licence described in [gateware/QUARTUS.md](QUARTUS.md). See
[docs/licensing.md](../docs/licensing.md).
