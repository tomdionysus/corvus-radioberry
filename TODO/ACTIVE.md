# Active work

Open work only, ordered by dependency: take the first unchecked item whose
dependencies are done. Each item names its acceptance test. A finished
item is removed from here and recorded at the top of
[TODO/COMPLETED.md](COMPLETED.md) with its date.

## Where things stand

- Released: v0.7.0 (2026-11-20; Raspberry Pi 4 and 5, Linux 6.12 and
  6.18). The radio core is this project's own (the core takeover, done:
  [2026-11-17-core-takeover.md](2026-11-17-core-takeover.md)); the images
  have their own registers and the driver translates clients' Hermes-Lite
  2 commands; one image per board (`rpi5-pio`, `rpi4-i2s`), 10CL025 only;
  the optional SoapySDR module; DKMS skips kernels older than 6.12.
- Planned, not started: transmit predistortion in the gateware
  ([2026-11-20-dpd.md](2026-11-20-dpd.md), branch `item/dpd`). Its first
  stage is a bench measurement with the board owner; nothing else starts
  before it.
- Branch `item/postinst-kernel-check`: the install says when no driver
  was built for the running kernel (section 3).
- Waiting on the board owner: the stage 0 bench session of the
  predistortion plan; stage 8 of the takeover (own filter coefficients,
  optional); the PA HAT bias of the board on the Pi 5 (shelved).

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
  with Quartus Prime Lite 23.1std.1 in Docker on an x86-64 machine, one
  build at a time; bitfiles are committed and the Pi only installs them.
  The radio core is `gateware/rtl/core/` (`rbv_core.sv` and its blocks,
  each checked against the Hermes-Lite 2 block it replaced); the archive
  in `gateware/source/` supplies the coarse NCO table and the receive
  filter's coefficients and is the simulations' reference. Simulations
  run on a build Pi (`VERILATOR=1 tools/sim.sh`, or `SIM_LOCAL=1` for
  Icarus); `tools/ci-pi.sh` runs six of them. RTL is written for the
  device and Quartus, never bent to a tool's subset. A change is swept
  over three seeds of each image, the release too; a release whose image
  sources are unchanged rebuilds each image once and checks it
  byte-identical to the tested seed. Every Quartus run's data goes to the
  toolchain session (Miach). The radio never appears as a sound card.
- The driver picks its transport from the SoC: Pi 5 PIO with `rpi5-pio`,
  Pi 4 I2S with `rpi4-i2s`. One overlay, `dtoverlay=radioberry`, on both.
- CL016: not supported; a variant only if someone with the board will
  test it (README roadmap).
- The daemon assumes nothing about the image: gateware details (FIFO
  sizes, latencies) belong to the gateware and the driver.
- No transmit test without the board owner's explicit permission for that
  specific test, with the dummy load and amplifier state confirmed. Any
  PTT is a transmission. Transmit code is proved first against fakes and
  in loopback.
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
- Workflow: each feature or milestone on its own branch (a part of a
  larger one merges into its parent first), squashed into one commit on
  `develop` when complete; `develop` merged into `main` only at a version
  tag. No pull requests.
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
- Version string from `git describe`, embedded at configure time; the
  driver's version string follows the release.

## 2. Gateware (`gateware/`, `driver/`)

- [ ] [Gateware review](../gateware/docs/review.md) findings still open:
      1.3 to 1.7, 2.3 to 2.5, 3.4 to 3.7, 4.3 to 4.5, 5, 6, 7.2, 7.3,
      7.5, 8, 9.3 to 9.5, 10.2, 10.4. Taken as they come up; none is
      being worked on.
- [ ] Measurements left from the takeover, on the board: the receive
      noise floor at each rate with the input on the dummy load and the
      LNA at its lowest, against stage 1's image (the CIC pruning's
      tolerance, estimated +0.2 dB at 48 ksps); the transmit spectrum
      (dither on) where a receiver can measure it safely; the receive
      response per rate against the computed one (review 5, 6, 8).
      Acceptance: the figures recorded beside the computed ones.
- [ ] The ADC input's data window measured on the board: since 0.7.0 the
      capture clock is about 102 ps later than the core's, which centres
      it in the window the inherited constraint gives (+0.109 setup,
      +0.174 hold at the worst corners). A larger margin needs the
      AD9866's real output timing on this board's clock path. Acceptance:
      the constraint checked against a measured window.
- [ ] TX bit 4 (FPGA pin 105): its output buffer (5.43 ns slow, 3.16 ns
      fast at 2 mA, the only drive 3.3-V LVCMOS offers) misses the
      AD9866's window under the strict constraint, and no launch phase
      fits; relaxed in the constraints and kept by the board owner's
      decision. `tools/worst.sh` prints its strict setup and every TX
      pin's hold under the datasheet's 1.5 ns. Investigate only if bits 4
      or 10 of transmitted samples show errors.
- [ ] The board's frequency range, measured: receive response from a
      signal source swept down from 30 MHz until it falls away, and the
      transmit side into the dummy load (keyed: the board owner's
      go-ahead per run). Acceptance: the limits in the hardware docs.
- [ ] Receive level of the radio's own transmission: keyed into the
      dummy load at 12 W, the receiver tuned to the tone saw -22.7 to
      -22.9 dBFS on the Pi 5 with 0.7.0's images and -48.6 with stage 7's
      on the same board, and -33 to -61 dBFS on the Pi 4 across images.
      The path (leakage on the board) is not known. Matters for the
      predistortion plan. Acceptance: explained by a measurement.
- [ ] Pi 4 transmit, image TX underruns: 482 on 30 m (0.5.0 era), 2062
      in bursts with 0.6.0 (4 of 5 within 2 s of an under-voltage), 827
      in one session of 2026-11-19 that also had a daemon restart and a
      sample-rate change mid-session, and 0 in three later contacts.
      Acceptance: the cause found from a counter log, or none in a month
      of use without restarts.
- [ ] Transmit drive curve: power rose about 1.06 dB per AD9866 gain code
      against the datasheet's 0.5 dB. Cause unknown (PA gain with level,
      TxDAC/IAMP split, the board's register 0x0e). Measurement first.
- [ ] Receiver-count stress run on hardware now that the driver holds no
      RX clock (review finding 2.3).
- [ ] Six receivers starting at once on the Pi 5: the first RX period has
      2 resyncs and 4 dropped words (driver debug log), from
      `radioberryd` (`p2probe --receivers 6`) and the SoapySDR module
      alike, not every start. Acceptance: the cause found, or shown to
      be the receiver-count change landing in the first period.
- [ ] Optional: a lock-phase status bit in the I2S image, so each TX
      stream's realign is attributable (explanation now inferred).
- [ ] A CL016 variant, if someone with a CL016 board will test it.

## 3. Kernel driver remainder (`driver/`)

- [ ] Branch `item/postinst-kernel-check`: the install names the running
      kernel and the fix when no driver was built for it. Acceptance: a
      packaged install test on a kernel DKMS skips.
- [ ] Upstream review: the driver has not been through review on the
      kernel mailing lists.
- [ ] Bookworm, if it is to be supported: the four source changes and the
      packaging findings in [test-matrix.md](../docs/test-matrix.md)
      (service not enabled, DKMS skipping the Pi 5's newer kernel at
      install, libgpiod from backports at run time), a `bookworm` suite in
      the repository, and the kernel matrix there. The board owner
      decides.

## 4. Daemon (`daemon/`)

- [ ] HL2 drive compatibility: the HL2 gateware takes 0x09's top four
      bits as AD9866 TX gain codes 0 to 15, the RadioBerry images map the
      whole byte to codes 0 to 39, and the PA hat is not the HL2's, so HL2
      software sends far too little drive. With the measured curve
      (section 2), decide whether `radioberryd` maps HL2 drive to
      RadioBerry codes. Current piHPSDR (dl1ycf 900b1c94,
      `radio_calc_drive_level`) sends the 0x09 byte as a 0.5 dB attenuator
      step and the rest in the TX I/Q amplitude.
- [ ] Integration test: fault injection for every fail-safe path in the
      daemon tests that run `p1probe` and `p2probe` against it
      ([tools/protoprobe/tests/daemon_test.cpp](../tools/protoprobe/tests/daemon_test.cpp)).
- [ ] piHPSDR at 384 ksps on the Pi 4 stalled and exited; the kernel
      counted UDP receive-buffer drops (none on send), so the client fell
      behind. Confirm with per-socket drops (`ss -uamp`) while it runs;
      a larger `net.core.rmem_default` is the board owner's decision.

## 5. Tools (`tools/`)

- [ ] `rbprobe` reports an install of the original RadioBerry software
      (its installer is a shell script dpkg cannot see, and both installs
      use the same names). Signals, from definitive to supporting:
      `/dev/radioberry` not a misc device (major 10) or
      `/sys/class/radioberry/` present; the version strings of the loaded
      module and any `radioberry.ko` on disk; the installed
      `radioberry.dtbo` differing from the packaged copy or decompiling to
      `sdr,radioberry` at `/`;
      `/lib/modules/$(uname -r)/kernel/drivers/sdr/radioberry.ko`;
      `/etc/systemd/system/radioberry.service` with
      `ExecStart=/etc/init.d/radioberryd`, `/etc/init.d/radioberryd`,
      `/usr/local/bin/radioberry`. Report a blocker named "the original
      RadioBerry software" with the files found and what to remove; remove
      nothing; point to [docs/original-software.md](../docs/original-software.md);
      `postinst` prints the same. Acceptance: tests through the fake source
      for each signal alone and combined, and no finding on a Pi with only
      the packages installed.

## 6. SoapySDR module (`soapy/`)

- [ ] The module: what is left after the runs on both boards is in
      [SOAPYSDR.md](SOAPYSDR.md), with their results.
- [ ] Programs written for the SoapyRadioberry module:
      [SOAPYSDR-COMPAT.md](SOAPYSDR-COMPAT.md) lists where the interfaces
      differ and what could be matched. The board owner decides, after
      asking which calls such programs make.

## 7. Tests

- [ ] One SoapySDR module test failed once (1 of 45 cases, 1 assertion)
      in a run on a build Pi under another session's -j4 load,
      2026-11-20; which case was not captured. 33 further full runs and
      100 runs of each new jack keying case passed. Acceptance: the case
      found (run the suite under load with `--durations yes` and keep the
      output) and made independent of timing.

## 8. Hardware validation

Transmit items each need the board owner's go-ahead at the time.

- [ ] Two-tone linearity, measured properly: piHPSDR's own two-tone
      generator into the dummy load at three drives, products read on a
      separate receiver, with and without the amplifier after the PA
      board. An old run with a test client gave -13 to -15 dBc at every
      drive from 64 to 170, which a real amplifier does not do (the test
      signal is suspected). This is stage 0 of the predistortion plan.
- [ ] Keyed with no RF out for about a minute on the Pi 4 (2026-11-19,
      once; the daemon logged whole transmissions). The board owner's
      view: WSJT-X, or RF from a nearby kilowatt station. Acceptance: the
      cause found, or a repeat caught with the TX audio level, the image
      counters and receive stalls logged together.
- [ ] SparkSDR and Quisk receive and tune via Protocol 1.

## 9. Development Pi (Pi 5)

- [ ] Wi-Fi background scans: while associated, the Pi 5 scans every
      33.7 s for 3.65 s, which stalls UDP to a Wi-Fi client for about
      130 ms at a time ([docs/testing.md](../docs/testing.md)). Stop the
      scans (for example by locking the connection to the access point's
      BSSID), with a rollback in place since Wi-Fi is the Pi's only link;
      the board owner's decision. Acceptance: RX packet lateness to a
      Wi-Fi client under 10 ms at p99.9 over 60 s.
