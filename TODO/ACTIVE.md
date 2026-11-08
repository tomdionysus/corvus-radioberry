# Active work

Open work only, ordered by dependency: take the first unchecked item whose
dependencies are done. Each item names its acceptance test. A finished
item is removed from here and recorded at the top of `COMPLETED.md` with
its date.

## Where things stand (2026-11-08)

- Built, installed and verified on the development Pi: `rbprobe`, the
  CL025 gateware (75.3, built from source), the kernel module and overlay
  (load at boot, gateware identity read, receive streaming verified up to
  the PIO bus capacity), the ABI header, and the CI gate
  `tools/ci-pi.sh`.
- `rbcat` built and verified on the Pi: 10 minutes at 48 ksps with no
  stream faults; zero TX samples at 48000 words per second with PTT and
  the TX permission bit clear and no underruns.
- `rbprobe` on the Pi reports one blocker: no daemon.
- Nothing has transmitted. The driver's TX path has carried zero samples
  with PTT and the TX permission bit clear (`rbcat --tx-zeros`), never
  with PTT set.
- `rbctl` sends single gateware commands; it refuses anything that can
  transmit without `--allow-tx`, which has not been used on hardware.
- Next: section 2 (driver remainder), then the daemon.

## Standing decisions

- C++20 throughout; C only in the kernel module and the ABI header.
- Unit tests on every component, aiming at complete coverage; kernel
  module logic compiled into a userspace test binary. Catch2 v3, gcov with
  lcov reports, ASan and UBSan test builds.
- Dependencies from Debian trixie packages only: catch2, libyaml-cpp-dev,
  nlohmann-json3-dev, libspdlog-dev, libliquid-dev, libgpiod-dev,
  libi2c-dev, libsystemd-dev.
- Build and install with CMake throughout, including the kbuild wrapper for
  the module; `cmake --install` places everything.
- Gateware: the Hermes-Lite 2 Radioberry PIO variants (75.3), built
  unmodified from `gateware/source/` with Quartus Prime Lite 23.1std.1 on
  an x86-64 machine by `tools/build-gateware.sh`; the bitfiles are
  committed and the Pi only installs them. Upstream's free rpi-5 images are
  older GPIO-mode gateware and are not used. CL016 ships with a documented
  AD9866 TX output timing violation (`gateware/PROVENANCE.md`).
- Development FPGA is a 10CL025. The driver supports CL016 and CL025.
- No transmit test on the development board without the owner's explicit
  permission for that specific test. Transmit code is proved first against
  fakes. Writing samples to the TX path with PTT and the TX permission bit
  clear is not transmitting.
- Names: daemon `radioberryd`, unit `radioberry.service`, config
  `/etc/radioberry/config.yaml`, module `radioberry.ko`, C++ namespace
  `radioberry`, tools `rbprobe`, `rbcat`, `rbctl`, `rbload`, `p1probe`,
  `p2probe`.
- Config files in YAML via yaml-cpp. Tool output in three forms: human
  text (default), `--json` via nlohmann/json, `--yaml` via yaml-cpp.
- Logging with spdlog: stderr in the foreground, journal as a service.
- Runtime privileges: system user and group `radioberry`, udev rule for
  `/dev/radioberry`, unit grants CAP_SYS_NICE and CAP_IPC_LOCK; never root.
- CI is `tools/ci-pi.sh` on the development Pi (directly or with
  `--remote`); it is the merge gate. No hosted CI.
- Workflow: one branch per TODO item, squash-merged into `develop`;
  `develop` merged into `main` only at a version tag. No pull requests.
- Protocol 2: transmit I/Q at 192 ksps resampled in the daemon with
  liquid-dsp; discovery advertises the gateware's receiver count (four
  DDCs on the CL016, six on the CL025), one DUC, 48 to 384 ksps, no
  wideband, diversity or audio.
- RX capacity: at most about 2.37 million values per second over the PIO
  bus (six receivers at 192 ksps, three at 384 ksps). The daemon refuses
  receiver and rate combinations above it.
- Code style: clang-format 19, Google base, 4-space indent, 100 columns;
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`. The kernel
  module and ABI header use kernel style and are not clang-formatted.
- Version string from `git describe`, embedded at configure time.

## 2. Kernel driver remainder (`driver/`)

- [ ] Userspace test harness `tools/driver-tests/` against
      `/dev/radioberry`: exclusive open, frame alignment for every
      receiver count, non-blocking reads, `poll()`, short read and write
      errors, safe command on close (reply shows `cw_on` clear).

## 3. Daemon (`daemon/`)

- [ ] Device layer over `/dev/radioberry`: command queue, RX frame reader,
      TX block writer, statistics and fault reporting. Unit tests with a
      fake device.
- [ ] Protocol 1, Hermes-Lite 2 dialect: discovery, start/stop, 1032-byte
      frames for the gateware's receiver count within the RX capacity, the
      HL2 C0 command set, PA enable and drive, ADC status words reported
      as absent, TCP transport. Acceptance: piHPSDR, SparkSDR and Quisk
      connect, receive and tune (transmit is section 6).
- [ ] Session: protocol selection at discovery, one owning client, clean
      hand-over on timeout.
- [ ] PA and transmit policy: `--allow-pa`, drive ceiling, TX watchdog,
      envpa only while a client holds MOX, every PA state change logged,
      no interlock claimed. TX permission bit 42 clear unless transmit is
      allowed, PTT bit 32 clear in every command without MOX, PTT hang
      time never 31 (`gateware/README.md` sections 4, 6, 7). Every
      fail-safe path proved by a unit test.
- [ ] Protocol 2: discovery, General, DDC and DUC specific, High Priority
      both ways, DDC I/Q at 48 to 384 ksps, DUC I/Q at 192 ksps resampled
      with liquid-dsp, Mic/Line and Wideband declared absent, client
      ownership and timeout, fail-safe on sequence gaps. Acceptance:
      piHPSDR in Protocol 2 mode receives.
- [ ] Real-time setup: thread priorities, CPU affinity, socket buffer
      sizes, `SO_PRIORITY`; RX packet jitter over Wi-Fi measured and
      recorded in `docs/platform.md`.
- [ ] systemd unit `radioberry.service` as user `radioberry` with
      CAP_SYS_NICE and CAP_IPC_LOCK, udev rule, system user, config file
      `/etc/radioberry/config.yaml`, `--version`, spdlog to the journal,
      all installed by `cmake --install`. Acceptance: `rbprobe` exits 0 on
      the Pi.
- [ ] Integration test: a fake client against the daemon on loopback for
      both protocols, with fault injection for every fail-safe path.

## 4. Protocol tools (`tools/`)

- [ ] `p1probe` and `p2probe`: minimal protocol clients for discovery,
      receive and zero-drive transmit, used by the integration tests.
- [ ] `rbload`: standalone gateware loader (libgpiod 2, reusing
      `tools/common`) with CONF_DONE and nSTATUS verification and image
      family check, for use with the driver unloaded.

## 5. Gateware

- [ ] Test the CL016 image on a CL016 board, receive and transmit, when
      one is available.

## 6. Hardware validation on the development Pi

Transmit items each need the owner's go-ahead at the time.

- [ ] Receive: known signal on 20 m visible in piHPSDR via Protocol 1.
- [ ] Receive via Protocol 2.
- [ ] Transmit at zero drive into a dummy load, PA disabled.
- [ ] Transmit into a dummy load with PA enabled, output measured.
- [ ] FT8 and corvus-protocol end-to-end run, logged in `docs/`.
