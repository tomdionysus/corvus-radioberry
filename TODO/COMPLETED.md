# Completed work

Newest first. Each entry gives the date, what was done, and where the
result lives.

- 2026-11-08: Gateware switched to a source build. Upstream's free rpi-5
  bitfiles are the 73.3 GPIO-mode gateware (they report 73.3 and no
  receiver count over SPI); its PIO gateware ships only in a paid image.
  Removed those bitfiles from `gateware/`, replaced the source archive with
  Hermes-Lite 2 3e467fd (75.3), added `tools/build-gateware.sh`,
  `tools/gateware/Dockerfile` and `gateware/QUARTUS.md`, rewrote
  `gateware/PROVENANCE.md`, updated `gateware/README.md` for 75.3, and
  classified the upstream images in `rbprobe` as rpi-5 GPIO. README credits
  PA3GSB's EUR 5 ready-to-run image.
- 2026-11-08: Gateware (section 4): the upstream rpi-5 CL025 and CL016
  bitfiles copied into `gateware/rpi-5/`, with `gateware/PROVENANCE.md`
  and the corresponding Hermes-Lite 2 source (commit 7472bd1, version
  75.2) as `gateware/source/`. `gateware/README.md` documents from the
  Verilog and the upstream PIO programs the SPI command and reply words,
  the RX nibble and metadata format, the TX bit format and handshake, FIFO
  depths, the CN301 output logic, the jack inputs, the 1.7 s reset after
  configuration and the gateware's PTT hang-time watchdog. Findings that
  changed documents: the CL025 build has six receivers, not four (plan and
  standing decision text corrected); upstream RX clocking cannot carry six
  receivers at 384 ksps (plan section 4.3); a CW key on the jack keys the
  DAC without software (plan section 5). CL025 image installed on the
  development Pi; `rbprobe` there now reports only the missing driver and
  daemon as blockers.
- 2026-11-08: Repository and build skeleton (section 2): `driver/`,
  `daemon/` and `gateware/` CMake subdirectories, `FPGATYPE` (CL016 or
  CL025, default CL025) selecting the image installed as
  `/lib/firmware/radioberry.rbf`, a kbuild wrapper in
  `cmake/KernelModule.cmake`, `CMakePresets.json` with default, asan and
  coverage presets, an lcov `coverage` target, `.editorconfig`,
  `tools/check-format.sh`, `tools/check-spdx.sh` and the merge gate
  `tools/ci-pi.sh` (local or `--remote`). Acceptance: `tools/ci-pi.sh`
  exits 0 on a fresh clone on the development Pi, 49 tests in each of the
  three builds, 98.3 percent line coverage. `rbprobe --identify` now also
  searches the installed `rpi-5/<type>/` layout.
- 2026-11-08: `rbprobe` (section 1) built in `tools/rbprobe/`, with the
  shared `tools/common/` library (SHA-256, catalogue of known upstream
  gateware images, passive serial FPGA loader over libgpiod 2). Probes sit
  behind a `Source` interface; 48 Catch2 tests against an in-memory
  machine, 98 percent line coverage of the unit-tested code, clean under
  ASan and UBSan. On the development Pi, `rbprobe --identify` reports
  10CL025 (configured in about 4 s), the wrong installed image (rpi-4
  CL016), no I2C devices, the driver absent, and exits 1. Loading the
  CL016 image alone left CONF_DONE low as expected; the FPGA was left on
  CL025. A minimal top-level CMake, `cmake/Version.cmake` and
  `.clang-format` were added ahead of section 2.
- 2026-11-08: v1 plan written to `docs/plan-v1.md`: fixed constraints,
  component architecture, driver and daemon design with the upstream
  defects each part addresses, performance targets, testing policy and
  order of work. Standing decisions recorded in `TODO/ACTIVE.md`.
- 2026-11-08: FPGA identified as a 10CL025 by loading the upstream rpi-5
  CL016 and CL025 images with the libgpiod 2 loader on the Pi: CL016 left
  CONF_DONE low, CL025 configured. Found that the installed
  `/lib/firmware/radioberry.rbf` is the rpi-4 CL016 image, wrong on both
  counts. Recorded in `docs/platform.md`.
- 2026-11-08: Repository initialised on branch `main` with the GitHub
  remote. GPL-2.0-or-later chosen and `LICENSE` added. README written with
  scope, protocols, DSP choice and third-party attributions. `TODO/`
  created.
- 2026-11-08: I2C enabled on the development Pi (`dtparam=i2c_arm=on`,
  `i2c-dev` module) and the Pi rebooted. `i2cdetect -y 1` shows no devices:
  the PA5W V2 has no MAX11613 and no MCP4662. Recorded in `docs/pa.md` and
  `docs/platform.md`.
- 2026-11-08: Development Pi surveyed over SSH; result in
  `docs/platform.md`. Established that the stock 6.18 `rp1-pio` lacks
  `rp1_pio_sm_buffer_virt`, which is the only API the upstream driver uses
  that does not exist, and that stock `rp1_pio_sm_xfer_data` bounces
  through its own DMA buffers.
- 2026-11-08: Amplifier board identified and documented in `docs/pa.md`:
  AURSINC Radioberry Preamp Board V2 (PA5W V2), manual bias, INA199A1
  current sense, lineage from the PA3GSB 5 W preamp; hish8612 PA30 hat and
  JI1UDD HL2-PA30 schematics read and summarised.
- 2026-11-08: Upstream projects Radioberry-2.x and radioberry-p2 surveyed;
  result in `docs/history.md`.
