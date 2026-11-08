# Completed work

Newest first. Each entry gives the date, what was done, its acceptance
result, and where it lives.

- 2026-11-08: Protocol 1 receive, Hermes-Lite 2 dialect (`daemon/src/p1/`,
  `radioberryd`, installed to sbin; transmit is still open). Discovery
  with the HL2's 60-byte reply, start and stop, 1032-byte frames for the
  client's receiver count with receivers beyond the gateware or the RX
  capacity at the rate sent as zero, the HL2 response words with ADC
  measurements reported as absent, ACK echoes, commands forwarded on
  change, UDP and TCP (piHPSDR's 1032-byte records). The first client to
  start owns the radio until it stops or is silent for 3 s, then the
  gateware is put in the safe state. Receive only: MOX ignored, 0x09
  sent with drive, VNA and PA enable cleared. Measured on the
  development Pi: expected packet rates with no sequence gaps over
  loopback up to six receivers at 192 ksps and three at 384 ksps;
  piHPSDR (headless) discovered, autostarted, tuned and streamed with
  no sequence errors; over Wi-Fi to the workstation 0.3% of UDP packets
  were lost after leaving the Pi. Acceptance by the owner on the Pi's
  desktop: piHPSDR received 40 m off air, and WSJT-X (audio through a
  PipeWire loopback, CAT through piHPSDR's TS-2000 server) decoded 40 m
  FT8, including a CQ checked against its callsign and grid. SparkSDR
  and Quisk are a follow-up check in section 6.
- 2026-11-08: Daemon device layer (`daemon/src/device/`, library
  `radioberryd_device`). `Control` holds the gateware's command registers
  and sends each changed one once, latest value; PTT, run, CWX and the TX
  permission bit are stamped onto every command from one transmit state,
  so a protocol command cannot carry them, and a PTT hang time of 31 is
  refused; `safe()` clears the transmit state and sends 0x09 with drive,
  VNA and PA enable cleared ahead of everything else. An RX thread sends
  the commands and reads frames into a lock-free single-producer
  single-consumer ring, so every block knows its receiver layout; a TX
  thread writes TX blocks from a second ring, finishing partial writes.
  Driver statistics are polled every 100 ms and on an RX overrun, and
  faults (resync, dropped words, overrun, full RX queue, stall after
  250 ms, TX underrun, invalid TX block, device error) go to one sink.
  The `Device` interface gained `wait()` (poll) and
  `open_device_nonblocking()`. Unit tests with a fake device: 100% line
  coverage of the layer except `monitor.cpp` (98.4%); release, ASan and
  UBSan builds pass. On the development Pi, `rbdrivertest` runs the layer
  against the driver for one second at 48 ksps with zero TX samples and
  PTT clear: between 47 000 and 49 000 frames, no sequence gaps, no
  faults; 10 consecutive harness passes, `rx_stalls` 0.
- 2026-11-08: Driver test harness `tools/driver-tests/` (`rbdrivertest`,
  Catch2) against `/dev/radioberry`: exclusive open, info against sysfs,
  whole-frame reads for every receiver count with no resyncs or dropped
  words, non-blocking reads to EAGAIN, `poll()` for frames, TX space and
  RX overrun, short read and partial-sample write errors, unknown ioctls,
  safe command on close (reply shows `cw_on` clear), TX ring fill and
  drain at 48 ksps with PTT clear, and no RX stall during the run (sysfs
  `rx_stalls` unchanged). Receive only: PTT and the TX permission bit are
  never set. Acceptance on the development Pi with driver 0.4.0: 20
  consecutive passes, `rx_stalls` 0 before and after, no "RX stream
  stalled" in the kernel log. `tools/ci-pi.sh` runs it when the device is
  present and free.
- 2026-11-08: Driver 0.4.0: the PIO gateware's RX lock-up resolved by
  a driver workaround (`docs/fault-rx-lockup.md`). Diagnosed from the Pi
  alone: the fault is an asynchronous crossing at the RX FIFO clear that
  every receiver-count change asserts, against the Pi-driven read clock;
  no command recovers it, TX keeps running, the rate moves with fitter
  placement (seven seeds, 0 to 8 lock-ups per 150 harness runs) and the
  state machine is one-hot without safe-state recovery. The driver now
  stops its RX PIO state machine from just before a receiver-count
  command until `rx_clock_hold_us` (200) after it, and a stall detector
  logs "RX stream stalled" and exposes sysfs `rx_stalled` and
  `rx_stalls`; `debug` gained level 2 (trace every command). Measured:
  0 lock-ups in 4500 changes with the hold against 17 in 1800 without,
  no change in stream counters. Acceptance on the development Pi with
  the installed 0.4.0 and its defaults: 100 harness runs (600 changes),
  0 stalls, 0 harness failures. The gateware source fix is an
  owner-gated item in `TODO/ACTIVE.md` section 5. Design note added to
  `docs/plan-v1.md` 4.5 and 9.
- 2026-11-08: Driver 0.3.0: words in flight across a receiver-count
  change are discarded, not counted as resyncs. After a change the RX
  parser settles until two consecutive clean frames of the new layout
  (old-layout words cannot form two: shown for every pair of counts 1 to
  6 in the unit tests), withholding frames and counting what it throws
  away in the new `rx_discarded_words` statistic, which takes two of the
  reserved words (ABI 1 size and offsets unchanged, checked by a test).
  `rbcat` reports it for the measurement and the settling time, `rbctl`
  in its statistics. Acceptance on the development Pi: `rbcat` through
  1, 6, 1, 6, 3 (384 ksps), 1, 6, 2, 5, 1 receivers showed zero resyncs
  and zero dropped words while settling and measuring, with 77 to 1113
  words discarded per change.
- 2026-11-08: Driver 0.2.0: sysfs `reload` and the `debug` parameter.
  Writing 1 to `reload` (refused with EBUSY while the device is open,
  which it also keeps closed meanwhile) stops the streams, sends the safe
  command, reloads the gateware, re-reads the identity, resets parser,
  rings and statistics and restarts the PIO streams; on failure `open()`
  returns ENODEV. Stream setup is now one routine shared by probe and
  reload, which also resets stream state on stop. `debug` (0644) logs
  rate-limited commands and replies, RX resyncs and dropped words per
  period, RX overruns and TX underruns. Acceptance on the development
  Pi: three reloads in a row, about 5.5 s each, same PIO program offsets
  each time; `rbcat` then streamed 10 s with zero resyncs; EBUSY while
  `rbcat` ran, EINVAL for a non-boolean write.
- 2026-11-08: Userspace against the installed ABI header.
  `RADIOBERRY_ABI_INCLUDE_DIR` points the `radioberry_abi` target at an
  installed `radioberry_ioctl.h`; configuration fails on an ABI version
  mismatch and warns if the text differs. The driver's unit tests keep
  the source header. `tools/ci-pi.sh` builds `rbprobe`, `rbcat` and
  `rbctl` against `/usr/local/include` when the header is installed and
  fails if any userspace compile command names `driver/include`.
  Acceptance on the development Pi: 19 userspace compiles, none with the
  source-tree include (32 of 34 in the default build).
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
