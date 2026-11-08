# Active work

Ordered by dependency. An item moves to `COMPLETED.md` with its date when it
is built, tested on the development Pi and merged. Each item names its
acceptance test.

## Standing decisions

- C++20 throughout; C only in the kernel module and shared ABI headers.
- Unit tests on every component, aiming at complete coverage; kernel
  module logic compiled into a userspace test binary.
- Gateware is copied in as prebuilt upstream bitfiles, never compiled here.
- Development FPGA is a 10CL025 (verified 2026-11-08).
- `rbprobe` is the first deliverable.

## 0. Decisions to close

- [ ] Protocol 2 transmit rate: resample 192 ksps to the gateware rate in
      the daemon with liquid-dsp (`msresamp_crcf`), or add a 192 ksps DUC
      path in the gateware. Recommendation: daemon resampler first.
- [ ] Protocol 2 feature profile: which DDC count, sample rates and
      wideband options are advertised. Minimum: one DDC at 48 to 384 ksps,
      one DUC, no wideband, no diversity.

## 1. Platform probe (`tools/rbprobe`)

- [ ] C++20 binary, dependencies libgpiod and libi2c only. Probes behind
      an interface so the report logic is tested against fake sysfs,
      device tree, GPIO and I2C contents.
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
- [ ] Text report and `--json`; non-zero exit when the radio cannot run.
      Acceptance: run on the development Pi reports 10CL025, wrong
      installed image, no I2C devices, driver absent, and exits non-zero.

## 2. Repository and build skeleton

- [ ] Top-level `Makefile` with `all`, `install`, `uninstall`, `clean`
      and `FPGATYPE` selection; per-component makefiles under `driver/`,
      `daemon/`, `tools/`; CMake for the daemon.
- [ ] `.gitignore`, `.editorconfig`, clang-format configuration, SPDX
      header check script under `tools/`.
- [ ] CI-equivalent script that builds everything on the Pi over SSH and
      runs the unit tests (`tools/ci-pi.sh`).
      Acceptance: script exits 0 on a clean checkout on the Pi.

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
- [ ] systemd unit `corvus-radioberry.service`, config file in
      `/etc/corvus-radioberry.conf`, `--version`, structured logs.
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
