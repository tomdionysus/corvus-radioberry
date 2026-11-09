# Active work

Open work only, ordered by dependency: take the first unchecked item whose
dependencies are done. Each item names its acceptance test. A finished
item is removed from here and recorded at the top of `COMPLETED.md` with
its date.

## Where things stand (2026-11-09, v0.1.0)

- Working on the development Pi: the CL025 gateware (75.3, built from
  source), the driver (0.4.0; the PIO gateware's RX lock-up worked around,
  `docs/fault-rx-lockup.md`), `rbprobe`, `rbcat`, `rbctl`, the driver
  harness `rbdrivertest` and the CI gate `tools/ci-pi.sh`.
- `radioberryd`: Protocol 1 (HL2 dialect) receive and transmit over UDP
  and TCP, and Protocol 2 over UDP (FT8 contact on 40 m through piHPSDR
  in Protocol 2), one owning client, transmit
  only with `--allow-tx` through the transmit policy and fail-safes,
  frequency calibration (`--frequency-ppm`, +22.9 on the development
  board), I/Q convention option. Not yet a service: started by hand.
- On air: piHPSDR and WSJT-X through `radioberryd` received and decoded
  FT8, transmitted it (decoded by distant stations on PSK Reporter), and
  completed an FT8 contact. The transmit chain measured clean with a
  HackRF (no spurs or image above a broadband floor about 46 dB down).
- `ft8rx` (`examples/ft8`, optional): receive-only FT8 decoder over
  Protocol 1 or 2.
- Next: the Protocol 2 TX underruns, then real-time setup and the
  systemd service; the HL2 drive item and the Protocol 2 output checks
  wait for the owner's go-ahead to transmit.

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

- [ ] HL2 drive compatibility: the HL2 gateware takes 0x09's top four
      bits as AD9866 TX gain codes 0 to 15, the Radioberry build maps the
      whole byte to codes 0 to 39 (`ad9866ctrl.v`), and the PA hat is not
      the HL2's, so HL2 software sends far too little drive (piHPSDR:
      drive 46 for 80% at its default calibration, about 0.4 W). Measure
      output against gain code across the range, then decide whether
      `radioberryd` maps HL2 drive to Radioberry codes. Until then piHPSDR
      is calibrated by hand (its per-band PA calibration, about 45 to 50).
- [ ] Protocol 2 TX underruns: through piHPSDR in Protocol 2 the driver
      counted about 94 TX underruns per second while transmitting (1 300
      in a 13.9 s FT8 transmission), at 47 850 to 47 998 samples per
      second delivered and 0 blocks dropped; Protocol 1 transmissions
      logged none. The driver counts one when its ring is empty with PTT
      set and no DMA in flight, which happens whenever the gateware's TX
      FIFO has room and takes samples at PIO speed, not only when the
      FIFO runs dry. piHPSDR sends no TX samples while receiving in
      Protocol 2 (none to port 1029 in a receive capture), so the FIFO
      starts each transmission empty and settles at the gateware's TX
      buffer latency (address 0x17; 20 ms by default, or what an earlier
      client set; piHPSDR sends 40 ms in Protocol 1), never full. The
      Protocol 2 server now sets 0x17 when a stream starts: 100 ms
      latency, more than the 85 ms FIFO, and 20 ms hang. Acceptance: a
      13.9 s FT8 transmission through piHPSDR in Protocol 2 into the dummy
      load logs underruns no higher than Protocol 1's, and decodes as
      before.
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
- [ ] Protocol 2 transmit output: power into the dummy load against
      Protocol 1 at the same piHPSDR settings, and the HackRF spectrum
      against Protocol 1's (no spurs or image above a floor about 46 dB
      down); TX I/Q is decimated from 192 to 48 ksps in the daemon.
- [ ] corvus-protocol end-to-end run, logged in `docs/` (FT8 is done).
- [ ] Two-tone linearity. A test client's two tones (700 and 1900 Hz)
      measured third-order products at -13 to -15 dBc at every drive from
      64 to 170, and fifth-order products rising with drive (-44 to
      -28 dBc), while piHPSDR's carrier and FT8 showed no products above
      a floor about 46 dB down. Establish whether the two-tone result is
      the test client or the radio (repeat with piHPSDR's own two-tone
      generator, and in the duplex receiver into a dummy load), then
      measure the drive at which compression sets in.
