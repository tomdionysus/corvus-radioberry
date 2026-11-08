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
- The PIO gateware's RX lock-up (`docs/fault-rx-lockup.md`) is worked
  around by driver 0.4.0, which holds its RX clock still across every
  receiver-count command and reports a stalled stream in the log and in
  sysfs. Accepted as the fix (Standing decisions); the gateware defect
  itself is an owner-gated item in section 5.
- The driver test harness `rbdrivertest` passes 20 consecutive runs on
  the Pi and runs in `tools/ci-pi.sh` when the device is present and free.
- The daemon's device layer (`daemon/src/device/`) is built and tested
  against a fake device and, in `rbdrivertest`, against the driver.
- `radioberryd` serves Protocol 1 receive over UDP and TCP; piHPSDR and
  WSJT-X decoded 40 m FT8 off air through it. It does not transmit.
- `ft8rx` (`examples/ft8`, optional): a receive-only Protocol 1 FT8
  decoder; 23 messages in 6 live 40 m slots through `radioberryd`.
  Samples decode only Q first (PSK Reporter confirms that spectrum);
  piHPSDR reads I first and also decodes, so the daemon is left as is.
  Received frequencies read 162 Hz low at 7.075 MHz (about -23 ppm,
  five stations against PSK Reporter); `docs/examples.md`.
- Next: Protocol 1 transmit (section 3), on the session and transmit
  policy already merged.

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
- The PIO gateware's RX lock-up (`docs/fault-rx-lockup.md`) is resolved
  by the driver's RX clock hold (`rx_clock_hold_us`, default 200), which
  measured 0 lock-ups in 4500 receiver-count changes against 17 in 1800
  without it. This is accepted as the fix. It is not reinvestigated and
  the gateware item in section 5 is not started unless the stall
  indicator fires with the hold on: sysfs `rx_stalled` = 1, `rx_stalls`
  > 0, or "RX stream stalled" in the kernel log. If that happens, record
  the circumstances in the fault document's section 8 and reopen from
  there; otherwise, a stream that stops is a new fault, not this one.
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

Nothing open.

## 3. Daemon (`daemon/`)

- [ ] Frequency calibration: a ppm correction in `radioberryd` applied
      to every RX and TX frequency command, measured against known
      signals (the RX offset is -162 Hz at 7.075 MHz, about -23 ppm, from
      PSK Reporter). Acceptance: `ft8rx` frequencies within 10 Hz of the
      PSK Reporter median on two bands. Needed before transmit on air.
- [ ] Protocol 1 transmit (receive is done): MOX from the owning client
      to PTT through the transmit policy, the client's TX I/Q to the
      driver at 48 ksps with the ring kept within the gateware's TX
      buffer latency, address-0 C3 reporting TX FIFO underflow and
      overflow as the HL2 does (piHPSDR reads its bits 7..6 while
      transmitting), drive and PA enable from 0x09 through the policy,
      CWX, and MOX cleared on every stream fault, all through `TxPolicy`
      and `Session`; the policy's options on the command line
      (`--allow-tx`, `--allow-pa`, `--drive-ceiling`, `--tx-watchdog-s`).
      Acceptance: every path
      proved against the fake device, then section 6's transmit steps
      with the owner's go-ahead.
- [ ] Protocol 2: discovery, General, DDC and DUC specific, High Priority
      both ways, DDC I/Q at 48 to 384 ksps, DUC I/Q at 192 ksps resampled
      with liquid-dsp, Mic/Line and Wideband declared absent, client
      ownership and timeout, fail-safe on sequence gaps. `ft8rx`
      (`examples/ft8`) gains Protocol 2, selectable at the command line.
      Acceptance: piHPSDR in Protocol 2 mode receives, and `ft8rx`
      decodes over Protocol 2.
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

- [ ] Fix the RX lock-up at source. Not taken in order: only on the
      owner's explicit instruction, since it reopens "built unmodified"
      (`docs/fault-rx-lockup.md`, section 9). The changes: synchronise the
      RX FIFO clear on both sides with the real `dcfifo` parameters, give
      `rx_pi_pio` a reset or safe-state recovery, remove its inferred
      latches, constrain the Pi RX clock at 20 MHz; version the image so
      `rbprobe` tells it apart. Acceptance: with `rx_clock_hold_us=0`,
      0 lock-ups in at least 600 harness runs against the shipped image's
      rate of about 6% per run, and no regression in `rbcat` streaming to
      the PIO bus ceiling.
- [ ] Test the CL016 image on a CL016 board, receive and transmit, when
      one is available.

## 6. Hardware validation on the development Pi

Transmit items each need the owner's go-ahead at the time.

- [ ] SparkSDR and Quisk receive and tune via Protocol 1.
- [ ] Receive via Protocol 2.
- [ ] Transmit at zero drive into a dummy load, PA disabled.
- [ ] Transmit into a dummy load with PA enabled, output measured.
- [ ] FT8 and corvus-protocol end-to-end run, logged in `docs/`.
