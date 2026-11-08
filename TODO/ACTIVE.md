# Active work

Ordered by dependency. An item moves to `COMPLETED.md` with its date when it
is built, tested on the development Pi and merged. Each item names its
acceptance test.

## Standing decisions

- C++20 throughout; C only in the kernel module and shared ABI headers.
- Unit tests on every component, aiming at complete coverage; kernel
  module logic compiled into a userspace test binary. Catch2 v3, gcov with
  lcov reports, ASan and UBSan test builds.
- Dependencies from Debian trixie packages only: catch2, libyaml-cpp-dev,
  nlohmann-json3-dev, libspdlog-dev, libliquid-dev, libgpiod-dev,
  libi2c-dev, libsystemd-dev.
- Build and install with CMake throughout, including a kbuild wrapper
  target for the module; `cmake --install` places everything.
- Gateware: the Hermes-Lite 2 Radioberry PIO variants, built unmodified
  from source (`gateware/source/`) with Quartus Prime Lite 23.1std.1 on an
  x86-64 machine by `tools/build-gateware.sh`; the bitfiles are committed
  and the Pi only installs them. Upstream's free rpi-5 images are the
  older GPIO-mode gateware and are not used.
- Development FPGA is a 10CL025 (verified 2026-11-08). Both the CL016
  and CL025 rpi-5 images are kept in `gateware/` and the driver supports
  both.
- No transmit test on the development board without the owner's explicit
  permission for that test. Transmit code is proved first against fakes.
- Names: daemon `radioberryd`, unit `radioberry.service`, config
  `/etc/radioberry/config.yaml`, module `radioberry.ko`, C++ namespace
  `radioberry`, tools `rbprobe`, `rbcat`, `rbctl`, `rbload`, `p1probe`,
  `p2probe`.
- Config files in YAML via yaml-cpp. Tool output in three forms: human
  text (default), `--json` via nlohmann/json, `--yaml` via yaml-cpp.
- Logging with spdlog: stderr in the foreground, journal as a service.
- Runtime privileges: system user and group `radioberry`, udev rule for
  `/dev/radioberry`, unit grants CAP_SYS_NICE and CAP_IPC_LOCK; never root.
- CI is `tools/ci-pi.sh` run over SSH on the development Pi; it is the
  merge gate. No hosted CI.
- Workflow: one branch per TODO item, squash-merged into `develop`;
  `develop` merged into `main` only at a version tag. No pull requests.
- Protocol 2 transmit at 192 ksps is resampled in the daemon with
  liquid-dsp; discovery advertises the gateware's receiver count (four
  DDCs on the CL016, six on the CL025, read over SPI), one DUC, 48 to
  384 ksps, no wideband, diversity or audio. (Corrected 2026-11-08 from
  "up to four" after reading the variant sources; the decision itself is
  unchanged.)
- Code style: clang-format 19, Google base, 4-space indent, 100 columns;
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`.
- Version string from `git describe`, embedded at configure time.
- `rbprobe` is the first deliverable.

## 0. Decisions to close

- None. All pre-implementation decisions were closed on 2026-11-08 and
  are listed under Standing decisions.

## 1. Platform probe (`tools/rbprobe`)

- [x] C++20 binary, dependencies libgpiod, libi2c, yaml-cpp,
      nlohmann/json and spdlog.
      Probes behind an interface so the report logic is tested against
      fake sysfs, device tree, GPIO and I2C contents.
- [x] Host: model, revision, serial, OS, kernel, matching headers,
      bootloader date, PMIC rails and throttle flags, Ethernet and Wi-Fi
      link state.
- [x] Boot: overlay active (`sdr,radioberry` node, `spi0.0` bound,
      `rp1_pio` loaded), `i2c_arm` on, spidev disabled.
- [x] Driver and firmware: module present for the running kernel, loaded,
      `/dev/radioberry` present and free; `/lib/firmware/radioberry.rbf`
      identified by hash against a table of known upstream images, with
      family (rpi-4 GPIO, rpi-5 PIO, juice) and FPGA type.
- [x] FPGA: CONF_DONE and nSTATUS levels; FPGA type by `--identify`
      (configuration with each known rpi-5 image); mismatch against the
      installed image flagged. Gateware major and minor, receiver and
      transmitter counts through the driver moved to section 3.
- [x] PA: I2C bus 1 scan classified (MAX11613, MCP4662, N2ADR, Alex,
      VA2SAJ); "manual-bias board, no telemetry" when empty.
- [x] Software: daemon version and service state, open UDP ports,
      liquid-dsp and piHPSDR versions (piHPSDR version only when packaged;
      a source build reports its path).
- [x] Human text report (default), `--json` and `--yaml` with identical
      content; non-zero exit when the radio cannot run.
      Acceptance: run on the development Pi reports 10CL025, wrong
      installed image, no I2C devices, driver absent, and exits non-zero.

## 2. Repository and build skeleton

- [x] Top-level `CMakeLists.txt` with subdirectories `driver/`, `daemon/`,
      `tools/`, `gateware/`; the module built through a custom target that
      invokes kbuild (`cmake/KernelModule.cmake`); `FPGATYPE` cache
      variable; install rules for module, firmware and binaries. The
      overlay, udev rule, system user and unit install rules arrive with
      the driver and daemon items that create those files.
- [x] `.editorconfig`, `.clang-format` (Google base, 4 spaces, 100
      columns), SPDX header check and format check under `tools/`.
- [x] Catch2 wiring with gcov, lcov report target, ASan and UBSan presets.
- [x] `tools/ci-pi.sh`: builds everything on the Pi over SSH, runs unit
      tests, sanitizer builds, format and SPDX checks, prints coverage.
      Acceptance: exits 0 on a clean checkout on the Pi.

## 3. Kernel driver (`driver/`)

- [x] Driver on the stock 6.18 `rp1-pio` API (`driver/kernel/`): SPI
      driver bound to the overlay's `sdr,radioberry` device, gateware
      load over gpiod, two PIO state machines with bounce-buffered DMA
      kept four periods deep by a FIFO-priority kernel thread, RX metadata
      resync, TX ring, safe command on close. Acceptance met 2026-11-08:
      the module loads at boot, creates `/dev/radioberry`, `dmesg` shows
      both state machines claimed, no warnings.
- [x] Device tree overlay `driver/radioberry.dts` installed to
      `/boot/firmware/overlays/`. Acceptance met: after reboot `spi0.0` is
      `sdr,radioberry` (modalias `spi:radioberry`), bound to the driver.
- [x] Gateware loading via `request_firmware` with CONF_DONE and nSTATUS
      errors failing the probe, and the identity polled until the
      gateware leaves its reset. Acceptance met: gateware 75.3 read on
      every probe (about 2.3 s from load to identity).
- [ ] Stable ioctl ABI header `driver/include/radioberry_ioctl.h`
      installed to `/usr/local/include` (done; ABI version 1 with info,
      command and statistics). Acceptance: daemon and tools build against
      the installed header only.
- [x] `rbprobe` reports gateware version, FPGA type, receiver and
      transmitter counts when the driver is bound, from the driver's sysfs
      attributes (no need to open the exclusive device), and flags a
      mismatch with the installed image. Acceptance met on the
      development Pi.
- [ ] Read path: whole frames for 1 to the gateware's receiver count,
      `poll()`, non-blocking mode (implemented; 185 million words without
      a resync in a test program). Acceptance: `tools/rbcat` streams
      10 minutes at 48 ksps with zero resyncs.
- [ ] Write path: whole samples, ring, underrun counter (implemented, not
      yet exercised). Acceptance: `rbcat`-style writer pushes zero samples
      with PTT and TX permission clear and the TX word counter advances at
      48 ksps.
- [ ] Module parameters and sysfs: `load_gateware`, `rx_clkdiv`,
      `tx_clkdiv`, ring sizes and the identity attributes exist; still to
      add a gateware reload attribute and a debug level.
- [ ] Unit test harness under `tools/driver-tests/` (userspace, against
      `/dev/radioberry`).

## 4. Gateware (`gateware/`)

- [x] Source archive of the `radioberry_pio_cl025` and
      `radioberry_pio_cl016` variants (Hermes-Lite 2 3e467fd, 75.3) with
      `gateware/PROVENANCE.md`.
- [x] Document, from the upstream Verilog, the facts the driver depends
      on: RX 4-bit bus and metadata nibble sequence, TX serial format and
      `pi_tx_ready` handshake, TX and RX FIFO depths, SPI command set,
      CN301 outputs (envpa, inttr, exttr, ptt_out) and keyer inputs.
      Written to `gateware/README.md`.
- [x] Build script `tools/build-gateware.sh` (Docker or a local Quartus)
      and toolchain notes `gateware/QUARTUS.md`.
- [x] CL025 bitfile built (seed 4, timing met), committed and catalogued
      in `rbprobe` as rpi-5 PIO.
- [x] CL016 bitfile built (seed 5) and catalogued; shipped with its AD9866
      TX output timing violation documented in `gateware/PROVENANCE.md`.
- [ ] Test the CL016 image on a CL016 board, receive and transmit, when
      one is available.
- [x] Install the CL025 build on the development Pi: the driver reads
      gateware 75.3, 10CL025, 6 receivers, 1 transmitter (2026-11-08).

## 5. Daemon (`daemon/`)

- [ ] Device layer over `/dev/radioberry`: ioctl command queue, RX frame
      assembler, TX block writer, fault reporting. Unit tests with a fake
      device.
- [ ] Protocol 1, Hermes-Lite 2 dialect: discovery, start/stop, 1032-byte
      frames with 1 to 4 receivers, all C0 commands the HL2 defines, PA
      enable and drive, ADC status words (reported as absent), TCP
      transport. Acceptance: piHPSDR, SparkSDR and Quisk connect, receive,
      tune, and transmit into a dummy load.
- [ ] Protocol 2: discovery, General, DDC and DUC specific, High Priority
      in both directions, DDC I/Q at 48 to 384 ksps, DUC I/Q at 192 ksps
      with liquid-dsp resampling, Mic/Line and Wideband declared absent,
      client ownership and timeout, fail-safe on sequence gaps.
      Acceptance: piHPSDR in Protocol 2 mode receives and transmits.
- [ ] Protocol selection at discovery; one owning client; clean
      hand-over on timeout.
- [ ] PA control policy: `--allow-pa` flag, drive ceiling, TX watchdog,
      envpa only while a client holds MOX, logging of every PA state
      change. No interlock is claimed. SPI TX permission bit 42 clear
      unless transmit is allowed, PTT bit 32 clear in every command
      without MOX, PTT hang time never 31 (`gateware/README.md`
      sections 4, 6, 7).
- [ ] Real-time setup: thread priorities, CPU affinity, socket buffer
      sizes, `SO_PRIORITY`; measured RX packet jitter over Wi-Fi and
      Ethernet recorded in `docs/platform.md`.
- [ ] systemd unit `radioberry.service` running as user `radioberry`
      with CAP_SYS_NICE and CAP_IPC_LOCK, udev rule, config file
      `/etc/radioberry/config.yaml`, `--version`, spdlog to the journal.
- [ ] Unit tests (protocol encoders and decoders, resampler, device
      fake) and an integration test that runs a fake client against the
      daemon on loopback.

## 6. Tools (`tools/`)

- [ ] `rbload`: standalone gateware loader (libgpiod 2) with CONF_DONE
      and nSTATUS verification and image family check.
- [ ] `rbcat`: raw I/Q dump and sample-rate check from `/dev/radioberry`.
- [ ] `rbctl`: send a single gateware command, read version and status.
- [ ] `p1probe` and `p2probe`: minimal protocol clients for discovery,
      receive and zero-drive transmit, used by the integration tests.
- [ ] `pa-bias.md` procedure and a `rbpa` helper that reads PA_CUR if an
      ADC path is ever added.

## 7. Hardware validation on the development Pi

- [ ] Driver loads, gateware version read.
- [ ] Receive: known signal on 20 m visible in piHPSDR via Protocol 1.
- [ ] Receive via Protocol 2.
- [ ] Transmit at zero drive into a dummy load, PA disabled.
- [x] PA idle bias set with BIAS1 and BIAS2. Already done on the
      development board before this project; see `docs/pa.md`.
- [ ] Transmit into a dummy load with PA enabled, output measured.
- [ ] FT8 and corvus-protocol end-to-end run, logged in `docs/`.
