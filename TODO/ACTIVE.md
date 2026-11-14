# Active work

Open work only, ordered by dependency: take the first unchecked item whose
dependencies are done. Each item names its acceptance test. A finished
item is removed from here and recorded at the top of
[TODO/COMPLETED.md](COMPLETED.md) with its date.

## Where things stand

- Released: v0.4.1 (Raspberry Pi 4 and 5, Linux 6.12 and 6.18), as Debian
  packages from the apt repository and on GitHub, with the Hermes-Lite 2
  PIO gateware.
- Development branch `item/new-direction`, to be 0.5.0: this project's own
  gateware ([gateware/README.md](../gateware/README.md)), one image per
  board, 10CL025 only. Raspberry Pi 5: `rpi5-pio`, receive and transmit,
  an FT8 contact on 20 m. Raspberry Pi 4: `rpi4-i2s`, I2S into the PCM
  block driven by the driver directly (no ALSA), receive and transmit, on
  air through piHPSDR and WSJT-X; 13% CPU at 192 ksps. Both use the
  framed SPI control protocol
  ([driver/kernel/rb_frame.h](../driver/kernel/rb_frame.h)). The driver
  has only these two transports and sets up the Pi 4's PCM pins itself.

## Standing decisions

- C++20 throughout; C only in the kernel module and the ABI header.
- Unit tests on every component with 100% line coverage, enforced by the
  coverage build; kernel module logic compiled into a userspace test
  binary. Catch2 v3, gcov with lcov reports, ASan and UBSan test builds.
- Testing effort: each test once; a repeat needs a reason (a flaky test,
  odd behaviour). ASan, TSan and the full CI run for releases, not for
  every commit.
- Dependencies from Debian trixie packages only: catch2, libyaml-cpp-dev,
  nlohmann-json3-dev, libspdlog-dev, libliquid-dev, libgpiod-dev,
  libi2c-dev, libsystemd-dev.
- Build with CMake throughout, including the kbuild wrapper for the
  module. Users install the Debian packages (the module through DKMS,
  built by [tools/build-deb.sh](../tools/build-deb.sh)); `cmake --install`
  is for development only.
- Gateware: one image per board, 10CL025 only, built from `gateware/`
  with Quartus Prime Lite 23.1std.1 in Docker on an x86-64 machine;
  bitfiles are committed and the Pi only installs them. The Hermes-Lite 2
  radio core is read unmodified from `gateware/source/`. The radio never
  appears as a sound card.
- The driver picks its transport from the SoC: Pi 5 PIO with `rpi5-pio`,
  Pi 4 I2S with `rpi4-i2s`. One overlay, `dtoverlay=radioberry`, on both.
- CL016: not supported; a variant only if someone with the board will
  test it (README roadmap).
- The daemon assumes nothing about the image: gateware details (FIFO
  sizes, latencies) belong to the gateware and the driver.
- No transmit test without the board owner's explicit permission for that
  specific test, with the dummy load and external PA state confirmed. Any
  PTT is a transmission. Transmit code is proved first against fakes.
- Names: daemon `radioberryd`, unit `radioberry.service`, config
  `/etc/radioberry/config.yaml`, module `radioberry.ko`, C++ namespace
  `radioberry`, tools `rbprobe`, `rbcat`, `rbctl`, `p1probe`, `p2probe`.
- Installed config: `frequency_ppm: 22.9`, `realtime: false`, transmit
  off.
- Config files in YAML via yaml-cpp. Tool output in three forms: human
  text (default), `--json` via nlohmann/json, `--yaml` via yaml-cpp.
- Logging with spdlog: stderr in the foreground, journal as a service.
- Runtime privileges: system user and group `radioberry`, udev rule for
  `/dev/radioberry`, unit grants CAP_SYS_NICE and CAP_IPC_LOCK; never root.
- CI is [tools/ci-pi.sh](../tools/ci-pi.sh) on a Pi (directly or with
  `--remote`), run before a release. GitHub Actions runs its userspace
  half on every push ([.github/workflows/ci.yml](../.github/workflows/ci.yml)),
  uploads coverage to Codecov, and runs CodeQL.
- Workflow: one branch per TODO item, squash-merged into `develop`;
  `develop` merged into `main` only at a version tag. No pull requests.
- Protocol 2: transmit I/Q at 192 ksps resampled in the daemon with
  liquid-dsp; discovery advertises the gateware's receiver count, one DUC,
  48 to 384 ksps, no wideband, diversity or audio.
- RX capacity: about 2.37 million values per second over the Pi 5's PIO
  bus (six receivers at 192 ksps), 1.536 million over the Pi 4's I2S link
  (four at 192 ksps). The daemon runs fewer receivers than asked for above
  it, never fewer than one.
- Code style: clang-format 19, Google base, 4-space indent, 100 columns;
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`. The kernel
  module and ABI header use kernel style and are not clang-formatted.
- Version string from `git describe`, embedded at configure time.

## 1. Release 0.5.0

- [ ] CI on a claimed cluster Pi: format, SPDX, checkpatch, sparse,
      clang-tidy, tests, and 100% line coverage (add tests or remove dead
      code until the gate passes).
- [ ] Packages built from the branch; on each Pi, an upgrade from 0.4.1
      with apt comes up on the new image with `dtoverlay=radioberry` alone
      (the Pi 4 with the old `[pi4] dtoverlay=radioberry-i2s` line removed
      or left, warned by `postinst`). Acceptance: `rbprobe` exits 0,
      `ft8rx` decodes over P1 and P2 with nothing lost, on both boards.
- [ ] Kernel matrix on both boards with the new driver
      ([docs/kernels.md](../docs/kernels.md) lists 0.4.1's results).
- [ ] Release gate per board (clean-down, README install, FT8 receive, the
      owner's contact); version 0.5.0.

## 2. Gateware (`gateware/`, `driver/`)

- [ ] [Gateware review](../gateware/docs/review.md) findings: not being
      worked on now.
- [ ] Pi 5 receive level of its own transmission: with the same tools the
      Pi 5 sees the tone 25 dB stronger in loopback (-18.7 against -43.6
      dBFS) and 48 dB stronger keyed into the dummy load (-16.9 against
      -64.9) than the Pi 4; output power matches (1.4 W against 1.35 W at
      drive 128). Cause unknown. Acceptance: explained by a measurement.
- [ ] Pi 4 transmit, 30 m: the image counted 482 TX underruns across the
      30 m transmissions (drive 150, capped from 162), none on 40 m before
      or after. Cause unknown. Acceptance: the cause found from a counter
      log of one 30 m transmission.
- [ ] Transmit drive curve: power rose about 1.06 dB per AD9866 gain code
      against the datasheet's 0.5 dB. Cause unknown (PA gain with level,
      TxDAC/IAMP split, the board's register 0x0e). Measurement first.
- [ ] TX bit 4 (FPGA pin 105) misses the AD9866 setup by 2.7 ns in the
      slow model and is relaxed in the constraints
      ([hardware-discovery.md](../gateware/docs/hardware-discovery.md)
      section 4). Watch the `strict` line of `tools/worst.sh` on every
      build; investigate if bits 4 or 10 of transmitted samples show
      errors.
- [ ] Receiver-count stress run on hardware now that the driver holds no
      RX clock (review finding 2.3).
- [ ] Optional: a lock-phase status bit in the I2S image, so each TX
      stream's realign is attributable (explanation now inferred).
- [ ] A CL016 variant, if someone with a CL016 board will test it.

## 3. Kernel driver remainder (`driver/`)

- [ ] Upstream review: the driver has not been through review on the
      kernel mailing lists.

## 4. Daemon (`daemon/`)

- [ ] HL2 drive compatibility: the HL2 gateware takes 0x09's top four
      bits as AD9866 TX gain codes 0 to 15, the RadioBerry images map the
      whole byte to codes 0 to 39, and the PA hat is not the HL2's, so HL2
      software sends far too little drive. With the measured curve
      (section 2), decide whether `radioberryd` maps HL2 drive to
      RadioBerry codes. Current piHPSDR (dl1ycf 900b1c94,
      `radio_calc_drive_level`) sends the 0x09 byte as a 0.5 dB attenuator
      step and the rest in the TX I/Q amplitude.
- [ ] Integration test: a fake client against the daemon on loopback for
      both protocols, with fault injection for every fail-safe path.
      [tools/protoprobe/tests/daemon_test.cpp](../tools/protoprobe/tests/daemon_test.cpp)
      already runs `p1probe` and `p2probe` against the daemon and the fake
      device; the fault injection is open.
- [ ] piHPSDR at 384 ksps on the Pi 4 stalled and exited; the kernel
      counted UDP receive-buffer drops (none on send), so the client fell
      behind. Confirm with per-socket drops (`ss -uamp`) while it runs;
      a larger `net.core.rmem_default` is the board owner's decision.

## 5. Tools (`tools/`)

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
      a Pi with only the packages installed.

## 6. Hardware validation

Transmit items each need the board owner's go-ahead at the time.

- [ ] SparkSDR and Quisk receive and tune via Protocol 1.
- [ ] Two-tone linearity: a test client's two tones measured third-order
      products at -13 to -15 dBc at every drive from 64 to 170 on the
      released image, while piHPSDR's carrier and FT8 showed no products
      above a floor about 46 dB down. Establish whether the result is the
      test client or the radio, then measure where compression sets in.

## 7. Development Pi (Pi 5)

- [ ] Wi-Fi background scans: while associated, the Pi 5 scans every
      33.7 s for 3.65 s, which stalls UDP to a Wi-Fi client for about
      130 ms at a time ([docs/testing.md](../docs/testing.md)). Stop the
      scans (for example by locking the connection to the access point's
      BSSID in the netplan configuration), with a rollback in place since
      Wi-Fi is the Pi's only link; the board owner's decision. Acceptance:
      RX packet lateness to a Wi-Fi client under 10 ms at p99.9 over 60 s.
