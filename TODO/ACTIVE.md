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
- Gateware is copied in as prebuilt upstream bitfiles, never compiled here.
- Development FPGA is a 10CL025 (verified 2026-11-08).
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
  liquid-dsp; discovery advertises the gateware's receiver count (up to
  four DDCs), one DUC, 48 to 384 ksps, no wideband, diversity or audio.
- Code style: clang-format 19, Google base, 4-space indent, 100 columns;
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`.
- Version string from `git describe`, embedded at configure time.
- `rbprobe` is the first deliverable.

## 0. Decisions to close

- None. All pre-implementation decisions were closed on 2026-11-08 and
  are listed under Standing decisions.

## 1. Platform probe (`tools/rbprobe`)

- [ ] C++20 binary, dependencies libgpiod, libi2c, yaml-cpp,
      nlohmann/json and spdlog.
      Probes behind an interface so the report logic is tested against
      fake sysfs, device tree, GPIO and I2C contents.
- [ ] Host: model, revision, serial, OS, kernel, matching headers,
      bootloader date, PMIC rails and throttle flags, Ethernet and Wi-Fi
      link state.
- [ ] Boot: overlay active (`sdr,radioberry` node, `spi0.0` bound,
      `rp1_pio` loaded), `i2c_arm` on, spidev disabled.
- [ ] Driver and firmware: module present for the running kernel, loaded,
      `/dev/radioberry` present and free; `/lib/firmware/radioberry.rbf`
      identified by hash against a table of known upstream images, with
      family (rpi-4 GPIO, rpi-5 PIO, juice) and FPGA type.
- [ ] FPGA: CONF_DONE and nSTATUS levels; with the driver loaded, gateware
      major and minor, FPGA type, receiver and transmitter counts; mismatch
      against the installed image flagged.
- [ ] PA: I2C bus 1 scan classified (MAX11613, MCP4662, N2ADR, Alex,
      VA2SAJ); "manual-bias board, no telemetry" when empty.
- [ ] Software: daemon version and service state, open UDP ports,
      liquid-dsp and piHPSDR versions.
- [ ] Human text report (default), `--json` and `--yaml` with identical
      content; non-zero exit when the radio cannot run.
      Acceptance: run on the development Pi reports 10CL025, wrong
      installed image, no I2C devices, driver absent, and exits non-zero.

## 2. Repository and build skeleton

- [ ] Top-level `CMakeLists.txt` with subdirectories `driver/`, `daemon/`,
      `tools/`, `gateware/`; the module built through a custom target that
      invokes kbuild; `FPGATYPE` cache variable; install rules for module,
      overlay, firmware, binaries, udev rule, system user and unit.
- [ ] `.editorconfig`, `.clang-format` (Google base, 4 spaces, 100
      columns), SPDX header check and format check under `tools/`.
- [ ] Catch2 wiring with gcov, lcov report target, ASan and UBSan presets.
- [ ] `tools/ci-pi.sh`: builds everything on the Pi over SSH, runs unit
      tests, sanitizer builds, format and SPDX checks, prints coverage.
      Acceptance: exits 0 on a clean checkout on the Pi.

## 3. Kernel driver (`driver/`)

- [ ] Port the RP1 PIO plus DMA driver to the stock 6.18 `rp1-pio` API:
      remove `KBUILD_EXTRA_SYMBOLS`, replace the two
      `rp1_pio_sm_buffer_virt` uses with kernel buffers passed through
      `rp1_pio_sm_xfer_data`, keep the RX metadata resync and the TX
      kfifo. Acceptance: `modprobe radioberry` on the Pi creates
      `/dev/radioberry`, `dmesg` shows both state machines claimed, no
      warnings.
- [ ] Device tree overlay built from `driver/radioberry.dts`, installed
      to `/boot/firmware/overlays/`. Acceptance: overlay present after
      reboot, `spi0.0` is `radioberry_ctrl_spi`.
- [ ] Gateware loading from `/lib/firmware/radioberry.rbf` via
      `request_firmware`, with CONF_DONE and nSTATUS checks reported as
      errors, not printk only. Acceptance: ioctl returns non-zero gateware
      major version.
- [ ] Stable ioctl ABI header `driver/include/radioberry_ioctl.h`
      installed to `/usr/local/include`; versioned struct with
      gateware info and command path. Acceptance: daemon and tools build
      against the installed header only.
- [ ] Read path returns whole sample frames for 1 to 4 receivers, with
      `poll()` support and a non-blocking mode. Acceptance: `tools/rbcat`
      streams 10 minutes at 48 ksps with zero resync messages.
- [ ] Write path accepts arbitrary block lengths, reports FIFO underrun
      and overrun counters through a sysfs or ioctl query.
- [ ] Module parameters and sysfs: FPGA type, receiver count, PIO clock
      divider, debug level.
- [ ] Unit test harness under `tools/driver-tests/` (userspace, against
      `/dev/radioberry`).

## 4. Gateware (`gateware/`)

- [ ] Copy in the upstream `radioberry_pio_cl025` and `radioberry_pio_cl016`
      bitfiles with a provenance file (upstream repo, tag or commit, hash)
      and the corresponding source archive required by GPL-2.0-or-later.
- [ ] Document, from the upstream Verilog, the facts the driver depends
      on: RX 4-bit bus and metadata nibble sequence, TX serial format and
      `pi_tx_ready` handshake, TX and RX FIFO depths, SPI command set,
      CN301 outputs (envpa, inttr, exttr, ptt_out) and keyer inputs.
      Written to `gateware/README.md`.
- [ ] Replace `/lib/firmware/radioberry.rbf` on the development Pi with
      the rpi-5 CL025 image (currently the rpi-4 CL016 image).

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
      change. No interlock is claimed.
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
- [ ] PA idle bias set with BIAS1 and BIAS2 reading PA_CUR; values
      recorded in `docs/pa.md`.
- [ ] Transmit into a dummy load with PA enabled, output measured.
- [ ] FT8 and corvus-protocol end-to-end run, logged in `docs/`.
