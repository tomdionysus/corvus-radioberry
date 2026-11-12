# Active work

Open work only, ordered by dependency: take the first unchecked item whose
dependencies are done. Each item names its acceptance test. A finished
item is removed from here and recorded at the top of
[TODO/COMPLETED.md](COMPLETED.md) with its date.

## Where things stand

- Released: v0.4.1 (Raspberry Pi 4 and 5, Linux 6.12 and 6.18,
  [docs/kernels.md](../docs/kernels.md)), as Debian packages from the
  apt repository and on GitHub.
- Working on both boards: the CL025 gateware (75.3, built from source),
  the driver (0.4.1; RP1 PIO on the Pi 5, CPU-clocked on the Pi 4; the PIO
  gateware's RX lock-up worked around,
  [docs/fault-rx-lockup.md](../docs/fault-rx-lockup.md)), `rbprobe`,
  `rbcat`, `rbctl`, `rbload`, `rbbus`, `p1probe`, `p2probe`, the driver
  harness `rbdrivertest` and the CI gate
  [tools/ci-pi.sh](../tools/ci-pi.sh).
- `radioberryd`: Protocol 1 (HL2 dialect) receive and transmit over UDP
  and TCP, and Protocol 2 receive and transmit over UDP, one owning
  client, transmit only with `--allow-tx` through the transmit policy and
  fail-safes, frequency calibration (`--frequency-ppm`, +22.9 on the
  development board), I/Q convention option, real-time threads; runs as
  `radioberry.service`.
- On air: piHPSDR and WSJT-X through `radioberryd` received and decoded
  FT8, transmitted it (decoded by distant stations on PSK Reporter), and
  completed FT8 contacts over Protocol 1 and Protocol 2. The transmit
  chain measured clean with a HackRF (no spurs or image above a broadband
  floor about 46 dB down).
- `ft8rx` (`examples/ft8`, optional): receive-only FT8 decoder over
  Protocol 1 or 2.
- Known limits: on the Pi 4, one receiver at 96 ksps or more, or three
  or more at 48, holds the driver's thread at 100% of one CPU core,
  mostly polling (SMI item, section 2), and so did piHPSDR at one
  receiver at 48 ksps (section 2); each Pi 4 FT8 TX period drops some TX
  blocks (section 2).
- Next, in order: the Pi 4 SMI measurement (section 2,
  [RPI4-SMI.md](RPI4-SMI.md) step 1); `rbprobe` reporting an install of
  the original software (section 4); the integration test's fault
  injection (section 3). Fuzzing the packet parsers, servers and the
  driver's ioctl and sysfs interface waits until the code settles. The
  Pi 4 TX drops, the HL2 drive item and the Protocol 2 output checks
  wait for the board owner's go-ahead to transmit.

## Standing decisions

- C++20 throughout; C only in the kernel module and the ABI header.
- Unit tests on every component, aiming at complete coverage; kernel
  module logic compiled into a userspace test binary. Catch2 v3, gcov with
  lcov reports, ASan and UBSan test builds.
- Dependencies from Debian trixie packages only: catch2, libyaml-cpp-dev,
  nlohmann-json3-dev, libspdlog-dev, libliquid-dev, libgpiod-dev,
  libi2c-dev, libsystemd-dev.
- Build with CMake throughout, including the kbuild wrapper for the
  module. Users install the Debian packages (the module through DKMS,
  built by [tools/build-deb.sh](../tools/build-deb.sh)); `cmake --install`
  is for development only.
- Gateware: the Hermes-Lite 2 RadioBerry PIO variants (75.3), built
  unmodified from `gateware/source/` with Quartus Prime Lite 23.1std.1 on
  an x86-64 machine by
  [tools/build-gateware.sh](../tools/build-gateware.sh); the bitfiles are
  committed and the Pi only installs them. The original RadioBerry
  software's free rpi-5 images are older GPIO-mode gateware and are not
  used. CL016 ships with a documented AD9866 TX output timing violation
  ([gateware/PROVENANCE.md](../gateware/PROVENANCE.md)).
- Development FPGA is a 10CL025. The driver supports CL016 and CL025.
- The PIO gateware's RX lock-up
  ([docs/fault-rx-lockup.md](../docs/fault-rx-lockup.md)) is resolved by
  the driver's RX clock hold (`rx_clock_hold_us`, default 200), which
  measured 0 lock-ups in 4500 receiver-count changes against 17 in 1800
  without it. This is accepted as the fix. It is not reinvestigated and
  the gateware item in section 5 is not started unless the stall
  indicator fires with the hold on: sysfs `rx_stalled` reads 1,
  `rx_stalls` is above 0, or "RX stream stalled" is in the kernel log. If
  that happens, record the circumstances in the fault document's
  Measurements section and reopen from there; otherwise, a stream that
  stops is a new fault, not this one.
- No transmit test on the development board without the board owner's
  explicit permission for that specific test. Transmit code is proved first against
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
- CI is [tools/ci-pi.sh](../tools/ci-pi.sh) on the development Pi (directly or with
  `--remote`); it is the merge gate. GitHub Actions also runs its userspace
  half on every push ([.github/workflows/ci.yml](../.github/workflows/ci.yml):
  no module, no hardware), uploads coverage to Codecov, and runs CodeQL
  ([.github/workflows/codeql.yml](../.github/workflows/codeql.yml)); these
  give the README badges and do not replace the Pi run.
- Workflow: one branch per TODO item, squash-merged into `develop`;
  `develop` merged into `main` only at a version tag. No pull requests.
- Protocol 2: transmit I/Q at 192 ksps resampled in the daemon with
  liquid-dsp; discovery advertises the gateware's receiver count (four
  DDCs on the CL016, six on the CL025), one DUC, 48 to 384 ksps, no
  wideband, diversity or audio.
- RX capacity: at most about 2.37 million values per second over the Pi
  5's PIO bus (six receivers at 192 ksps, three at 384 ksps), and 192 000
  on a Pi 4 (the driver's `bb_rx_values_max`, until the SMI item). The
  daemon runs fewer receivers than asked for above it, never fewer than
  one, since a client's rate cannot be refused.
- Code style: clang-format 19, Google base, 4-space indent, 100 columns;
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`. The kernel
  module and ABI header use kernel style and are not clang-formatted.
- Version string from `git describe`, embedded at configure time.

## 2. Kernel driver remainder (`driver/`)

- [ ] Raspberry Pi 4 RX over SMI with DMA, so one receiver at 192 ksps
      no longer costs a whole CPU core: [RPI4-SMI.md](RPI4-SMI.md).
      Acceptance there.
- [ ] Raspberry Pi 4 at 48 ksps with piHPSDR: the driver's thread at
      102 to 104% of a core and `radioberryd`'s RX threads at about 90%
      together (2 sessions), against 17% for the driver's thread with
      `p1probe` at the same rate and packet rate
      ([docs/testing.md](../docs/testing.md), Raspberry Pi 4). A client
      replaying `p1probe`'s commands gave 100% once, right after
      piHPSDR, and 17% once. Find what puts the driver in that state.
      Acceptance: piHPSDR at one receiver at 48 ksps costs the driver's
      thread what `p1probe` does.
- [ ] Raspberry Pi 4 TX drops: in the 0.4.0 acceptance test each FT8 TX
      period logged 5 to 30 "TX blocks dropped" and 0 to 1 underruns, at
      about 47.6k samples/s. Not investigated. Find the cause and fix it.
      Transmitting needs the board owner's go-ahead at the time.
      Acceptance: no TX blocks dropped across a run of FT8 TX periods on
      the Pi 4.
- [ ] Raspberry Pi 4 on 6.12.75: the harness test "the daemon's device
      layer streams through the driver" failed once in 11 runs (a gap,
      `rx_blocks_dropped` 31, RX queue full); the other 10 passed. It was
      accepted as an anomaly for 0.4.1. Find the cause. Acceptance: the
      cause identified, and fixed if it is ours.
- [ ] Upstream review: the driver has not been through review on the
      kernel mailing lists.

## 3. Daemon (`daemon/`)

- [ ] HL2 drive compatibility: the HL2 gateware takes 0x09's top four
      bits as AD9866 TX gain codes 0 to 15, the RadioBerry build maps the
      whole byte to codes 0 to 39 (`ad9866ctrl.v`), and the PA hat is not
      the HL2's, so HL2 software sends far too little drive (piHPSDR:
      drive 46 for 80% at its default calibration, about 0.4 W). Measure
      output against gain code across the range, then decide whether
      `radioberryd` maps HL2 drive to RadioBerry codes. Until then piHPSDR
      is calibrated by hand (its per-band PA calibration, about 45 to 50).
      Current piHPSDR (dl1ycf 900b1c94, `radio_calc_drive_level`) encodes
      HL2 drive differently from the build used so far: the 0x09 byte is
      a 0.5 dB attenuator step (0, 16, ... 240) and the rest goes into the
      TX I/Q amplitude, so a level of 115 is sent as byte 32 (gateware
      code 5) instead of 115 (code 17); hand calibrations made with the
      older build give far less drive with it.
- [ ] Integration test: a fake client against the daemon on loopback for
      both protocols, with fault injection for every fail-safe path.
      [tools/protoprobe/tests/daemon_test.cpp](../tools/protoprobe/tests/daemon_test.cpp)
      already runs `p1probe` and
      `p2probe` against the daemon and the fake device (streams, MOX
      allowed and refused, a busy radio); the fault injection is open.

## 4. Tools (`tools/`)

- [ ] `rbprobe` reports an install of the original RadioBerry software. Its
      installer is a shell script, so dpkg cannot see it, and both installs
      use the same names (`radioberry.ko`, `radioberry.dtbo` and
      `dtoverlay=radioberry`, `/dev/radioberry`, `radioberry.service`,
      compatible `sdr,radioberry`); with both present, depmod loads ours
      from `updates/dkms/`, whichever overlay was copied last is used
      (theirs puts `sdr,radioberry` on a root platform node, so our driver
      never binds and there is no device), and the unit in
      `/etc/systemd/system` overrides ours and runs their daemon, as root,
      against our driver with a different ioctl ABI. Signals, from
      definitive to supporting: `/dev/radioberry` not a misc device (major
      10) or `/sys/class/radioberry/` present; `/sys/module/radioberry/version`
      and `modinfo` strings of the loaded module and of any `radioberry.ko`
      on disk; the installed `radioberry.dtbo` differing from the packaged
      copy under `/usr`, or decompiling to `sdr,radioberry` at `/`;
      `/lib/modules/$(uname -r)/kernel/drivers/sdr/radioberry.ko`;
      `/etc/systemd/system/radioberry.service` with
      `ExecStart=/etc/init.d/radioberryd`, `/etc/init.d/radioberryd`,
      `/usr/local/bin/radioberry`. Report it as a blocker named "the
      original RadioBerry software", list the files found, and say what to
      remove; remove nothing; point to
      [docs/original-software.md](../docs/original-software.md).
      `postinst` prints the same. Acceptance: tests through
      the fake source for each signal alone and combined, and no finding on
      a Pi with only the packages installed (the development Pi, where the
      original's unit, init script and overlay were moved aside by hand).

## 5. Gateware

- [ ] Fix the RX lock-up at source. Not taken in order: only on the
      project owner's explicit instruction, since it reopens "built
      unmodified" ([docs/fault-rx-lockup.md](../docs/fault-rx-lockup.md),
      "Fixing it in the gateware"). The changes: synchronise the RX FIFO
      clear on both sides with the real `dcfifo` parameters, give
      `rx_pi_pio` a reset or safe-state recovery, remove its inferred
      latches, constrain the Pi RX clock at 20 MHz; version the image so
      `rbprobe` tells it apart. Acceptance: with `rx_clock_hold_us=0`,
      0 lock-ups in at least 600 harness runs against the shipped image's
      rate of about 6% per run, and no regression in `rbcat` streaming to
      the PIO bus ceiling.
- [ ] Test the CL016 image on a CL016 board, receive and transmit, when
      one is available.

## 6. Hardware validation on the development Pi

Transmit items each need the board owner's go-ahead at the time.

- [ ] SparkSDR and Quisk receive and tune via Protocol 1.
- [ ] Protocol 2 transmit output: power into the dummy load against
      Protocol 1 at the same piHPSDR settings, and the HackRF spectrum
      against Protocol 1's (no spurs or image above a floor about 46 dB
      down); TX I/Q is decimated from 192 to 48 ksps in the daemon.
- [ ] Two-tone linearity. A test client's two tones (700 and 1900 Hz)
      measured third-order products at -13 to -15 dBc at every drive from
      64 to 170, and fifth-order products rising with drive (-44 to
      -28 dBc), while piHPSDR's carrier and FT8 showed no products above
      a floor about 46 dB down. Establish whether the two-tone result is
      the test client or the radio (repeat with piHPSDR's own two-tone
      generator, and in the duplex receiver into a dummy load), then
      measure the drive at which compression sets in.

## 7. Development Pi

- [ ] Wi-Fi background scans: while associated, the development Pi scans
      every 33.7 s for 3.65 s, which stalls UDP to a Wi-Fi client for about
      130 ms at a time ([docs/testing.md](../docs/testing.md)); longer than the 85 ms TX FIFO,
      so a Protocol 2 client transmitting over Wi-Fi would run it dry.
      Stop the scans (for example by locking the connection to the access
      point's BSSID, in the netplan configuration so it survives reboots),
      with a rollback in place since Wi-Fi is the Pi's only link; the
      board owner's decision. Acceptance: RX packet lateness to a Wi-Fi client under
      10 ms at p99.9 over 60 s.
