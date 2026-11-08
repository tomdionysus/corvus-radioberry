# Completed work

Newest first. Each entry gives the date, what was done, its acceptance
result, and where it lives.

- 2026-11-08: `rbctl` in `tools/rbctl/`: sends one gateware command
  (address and data, flags `--ptt`, `--tx-permit`, `--cwx`, `--no-run`)
  and prints the command bytes, decoded reply, driver info and
  statistics as text, JSON or YAML; info and statistics alone without a
  command. `transmit_hazards()` and `disables_tx_watchdog()` in
  `tools/common/` name what can key the transmitter or arm the PA (PTT,
  TX permission, CWX, and at 0x09 drive, VNA, PA enable) and PTT hang
  time 31; `rbctl` refuses hazards without `--allow-tx` and hang time 31
  always, and `rbcat` refuses both. On the development Pi: info and
  statistics, an LNA command with its reply, PTT and hang-time refusals
  before the device is opened, EBUSY while `rbcat` runs. `--allow-tx` not
  exercised on hardware.
- 2026-11-08: `rbcat --tx-zeros`: writes zero TX samples, topping the
  driver's TX ring up to 2400 samples (50 ms) from the read loop, and
  reports TX words, word rate, underruns and the lowest ring level; the
  verdict requires the rate within 1% of 48000 per second, no underruns
  and a ring that never ran empty. Commands are unchanged (receive only),
  and the report records the driver's PTT flag and any reply showing
  cw_on or external PTT. Acceptance on the development Pi, 60 s: TX
  words 480 000 per 10 s interval, zero underruns, ring never below 2144
  samples, PTT and TX permission clear in all 3 commands, driver PTT flag
  clear; RX clean throughout.
- 2026-11-08: `rbcat` in `tools/rbcat/`: sets rate, receiver count,
  frequencies (duplex set, so receiver 1 follows its own frequency) and
  LNA gain with PTT and the TX permission bit always clear (any command
  that could transmit is refused), streams RX frames to stdout as iq24 or
  iq16 or discards them, and reports frame rate, RMS and peak level per
  receiver and the driver's counters over the measurement and the
  settling time, as text, JSON or YAML; exit 0 only for a clean stream.
  Logic tested against a fake device and clock. Shared by the tools in
  `tools/common/`: the gateware command encoding and Hermes-Lite 2
  commands (`command.hpp`), the `Device` interface over `/dev/radioberry`
  (`device.hpp`), and the report tree and renderers moved from `rbprobe`
  (`node.hpp`, `render.hpp`). Acceptance on the development Pi: 600 s at
  48 ksps, 28 800 576 frames at 48000.9 frames/s, zero resyncs, dropped
  words and overruns after settling and during it. Also clean: six
  receivers at 192 ksps and three at 384 ksps.
- 2026-11-08: Kernel driver, overlay and ABI header. `driver/kernel/`
  (`radioberry.c`; pure logic in `rb_logic.h` and `rb_pio.h`, unit tested
  in `driver/tests/`, PIO register words checked against upstream's),
  `driver/radioberry.dts`, `driver/include/radioberry_ioctl.h` (ABI 1:
  info, command, statistics), sysfs identity on `spi0.0`. SPI driver on
  the stock 6.18 `rp1-pio` API with bounce-buffered DMA kept four periods
  deep by a FIFO-priority kernel thread; safe command on close; nothing
  queued while closed. On the development Pi: loads at boot, gateware
  75.3, 10CL025, 6 receivers, both PIO state machines claimed, no
  warnings; receive streaming exact in rate with zero resyncs over 185
  million words, up to 2.37 million values per second (6 receivers at
  192 ksps, 3 at 384 ksps), the PIO bus ceiling. `rbprobe` reports the
  driver's identity and recognises this project's overlay. Upstream's init
  script and service moved out of the system paths on the Pi.
- 2026-11-08: Gateware built from source. `gateware/source/` (Hermes-Lite 2
  3e467fd, 75.3: RTL, board files, both PIO variants),
  `tools/build-gateware.sh` with `tools/gateware/` (Dockerfile, compile
  and timing-report Tcl), `gateware/QUARTUS.md`, `gateware/PROVENANCE.md`.
  CL025 built with seed 4, timing met, byte-reproducible, verified on the
  development board. CL016 built with seed 5 and shipped with a documented
  AD9866 TX output timing violation; untested. Upstream's free rpi-5
  images are 73.3 GPIO-mode gateware and are not used; `rbprobe`
  classifies them. README credits PA3GSB's EUR 5 ready-to-run image.
- 2026-11-08: Gateware interface notes, `gateware/README.md`: SPI command
  and reply words, RX nibble and metadata format, TX bit format and
  handshake, FIFO depths, CN301 output logic, jack inputs, post-load reset,
  PTT hang-time watchdog, PIO bus capacity.
- 2026-11-08: Repository and build skeleton: top-level CMake with
  `driver/`, `daemon/`, `tools/`, `gateware/`, `FPGATYPE`, kbuild wrapper
  `cmake/KernelModule.cmake`, `CMakePresets.json` (default, asan,
  coverage), lcov `coverage` target, `.editorconfig`, `.clang-format`,
  `tools/check-format.sh`, `tools/check-spdx.sh`, and the merge gate
  `tools/ci-pi.sh` (local or `--remote`). Acceptance: exits 0 on a fresh
  clone on the development Pi.
- 2026-11-08: `rbprobe` in `tools/rbprobe/` with the shared `tools/common/`
  library (SHA-256, gateware image catalogue, passive serial FPGA loader
  over libgpiod 2). Probes behind a `Source` interface, tested against an
  in-memory machine; text, JSON and YAML from one document tree;
  `--identify` finds the FPGA type by configuration. Acceptance on the
  development Pi before the driver existed: 10CL025, wrong installed
  image, no I2C devices, driver absent, exit 1.
- 2026-11-08: v1 design in `docs/plan-v1.md`; standing decisions in
  `TODO/ACTIVE.md`; working conventions in `CONTRIBUTING.md`.
- 2026-11-08: Development Pi surveyed and prepared (`docs/platform.md`):
  FPGA identified as 10CL025, I2C enabled (bus empty), build dependencies
  installed. The stock 6.18 `rp1-pio` lacks only `rp1_pio_sm_buffer_virt`
  of what the upstream driver uses.
- 2026-11-08: Amplifier board identified and documented in `docs/pa.md`:
  AURSINC Radioberry Preamp Board V2 (PA5W V2), manual bias (already set),
  INA199A1 current sense, no I2C devices.
- 2026-11-08: Upstream projects Radioberry-2.x and radioberry-p2 surveyed in
  `docs/history.md`. Repository created with GPL-2.0-or-later.
