# Active work

Open work only, ordered by dependency: take the first unchecked item whose
dependencies are done. Each item names its acceptance test. A finished
item is removed from here and recorded at the top of
[TODO/COMPLETED.md](COMPLETED.md) with its date.

## Where things stand

- Planned, not started: transmit predistortion in the gateware
  ([2026-11-20-dpd.md](2026-11-20-dpd.md), branch `item/dpd`); its first
  stage is a bench measurement with the board owner.
- Released: v0.7.0 (Raspberry Pi 4 and 5, Linux 6.12 and 6.18):
  the radio core is this project's own (the core takeover,
  [2026-11-17-core-takeover.md](2026-11-17-core-takeover.md): stages 1
  to 7, the ADC capture clock, the memory blocks, the transmit CIC); the
  driver translates Hermes-Lite 2 commands to the image's registers; one
  image per board (`rpi5-pio`, `rpi4-i2s`), 10CL025 only; the optional
  SoapySDR module; DKMS skips kernels older than 6.12.
- Branch `item/postinst-kernel-check`: the install says when no driver
  was built for the running kernel (section 3).
- Waiting on the board owner: the PA HAT bias of the board now on the
  Pi 5 (shelved); stage 8 (own filter coefficients, optional).

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
  bitfiles are committed and the Pi only installs them. The radio core
  is this project's `gateware/rtl/core/` (`rbv_core.sv` and its blocks,
  each checked against the Hermes-Lite 2 block it replaced:
  [2026-11-17-core-takeover.md](2026-11-17-core-takeover.md)), with its
  own registers; the driver turns clients' Hermes-Lite 2 commands into
  them. The archive in `gateware/source/` supplies the NCO tables and the
  filter coefficients and is the simulations' reference.
  Simulations run on a build Pi (`SIM_LOCAL=1 tools/sim.sh`), Quartus in
  Docker on the Mac, one container at a time.
  RTL is written for the device and Quartus, never bent to a tool's
  subset; Quartus sweeps of ten seeds are the judge. The radio never
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

## 2. Gateware (`gateware/`, `driver/`)

- [ ] [Gateware review](../gateware/docs/review.md) findings: the
      takeover has moved 1.2, 5, 6, 7.4, 7.5, 8 and 10.1; the rest are
      not being worked on now.
- [ ] Take over the radio core block by block:
      [2026-11-17-core-takeover.md](2026-11-17-core-takeover.md). Stages
      1, 2, 3, 5 and 6a to 6c done; 6d (keying) built and simulated, its
      board and keyed runs open (keyed runs with the board owner). Then
      stage 7 (the core's own top). Stage 4 (the frequency calculation
      moved from the FPGA into the kernel driver) goes with the release.
- [ ] Release `item/gateware-core`. Before the release gate: the receive
      noise floor on the dummy load at each rate, stage 1's image against
      the current one (the CIC pruning's tolerance); the audit of review
      items 5, 6 and 8 (passband, stopband and alias rejection per rate
      against the v0.6.0 image; the transmit spectrum on the board);
      stage 4 of the takeover with its driver change; both images built
      at the release commit and committed with their rows in
      [provenance.md](../gateware/docs/provenance.md). Then the gate on
      both boards (`rbcat` at the most receivers per rate, FT8 over
      Protocol 1 and 2, loopback, an on-air contact). Acceptance: every
      seed of both images meets timing; both boards pass as v0.6.0 did;
      the audit figures no worse.
- [ ] ADC input capture margin: `ad_rx` into its input register has
      +0.008 to +0.018 ns setup on every seed of every image, set by the
      pins (the takeover plan, "Alongside"). Measured on stage 6d's
      rpi5-pio seed 1 (the full timing dump), the seven inputs' setup /
      hold slack: slow 85 C +0.008..+0.027 / +0.553..+0.573 ns, slow 0 C
      +0.216..+0.233 / +0.436..+0.454, fast 0 C +0.218..+0.231 /
      +0.269..+0.283. The window the constraint leaves (the Hermes-Lite 2
      input delays, 0 to 5.0 ns of the 6.51 ns period) is about 0.5 ns;
      a capture clock about 130 ps later (its own PLL output, the rest of
      the 153.6 MHz logic unchanged) would give about +0.14 ns each side
      at every corner, the most this constraint allows. A larger margin
      needs the real data window measured on the board (the AD9866's
      output timing against this board's clock path), not the inherited
      constraint. Acceptance: the capture clock moved and the constraint
      checked against a measured window on the board, setup and hold each
      at least 0.1 ns at every corner, receive unchanged on both boards.
- [ ] The board's frequency range, measured: receive response from a
      signal source swept down from 30 MHz until it falls away, and the
      transmit side into the dummy load (keyed: the board owner's
      go-ahead per run). Acceptance: the lower and upper limits written
      into the hardware documentation.
- [ ] Pi 5 receive level of its own transmission: with the same tools the
      Pi 5 sees the tone 25 dB stronger in loopback (-18.7 against -43.6
      dBFS) and 48 dB stronger keyed into the dummy load (-16.9 against
      -64.9) than the Pi 4; output power matches (1.4 W against 1.35 W at
      drive 128). Cause unknown. Acceptance: explained by a measurement.
- [ ] Pi 4 transmit, image TX underruns: 482 across 30 m transmissions
      (drive 150), none on 40 m then. With 0.6.0 on 40 m (piHPSDR over
      Protocol 2, WSJT-X, PA, drive 115 to 129): 2062 in short bursts in
      5 of 17 transmissions, the driver never late, no block dropped,
      the signal spotted throughout; the Pi's supply logged under-voltage
      9 times, and 4 of the 5 bursts began within 2 s of one. Cause not
      proven. Acceptance: no bursts on a supply that logs no
      under-voltage, or the cause found from a counter log.
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
- [ ] Six receivers starting at once on the Pi 5: the first RX period has
      2 resyncs and 4 dropped words (driver debug log), from
      `radioberryd` (`p2probe --receivers 6`) and the SoapySDR module
      alike, not every start. Acceptance: the cause found, or shown to
      be the receiver-count change landing in the first period.
- [ ] Optional: a lock-phase status bit in the I2S image, so each TX
      stream's realign is attributable (explanation now inferred).
- [ ] A CL016 variant, if someone with a CL016 board will test it.

## 3. Kernel driver remainder (`driver/`)

- [ ] The driver reports its version as 0.5.0 in release 0.6.0
      (`rbcat` shows `driver_version 0.5.0`); one version number for every
      component, so the driver's must follow the release. Acceptance: the
      next release's driver reports that release's version, checked in
      the release gate.
- [ ] Upstream review: the driver has not been through review on the
      kernel mailing lists.
- [ ] Refuse unsupported kernels in DKMS (`BUILD_EXCLUSIVE_KERNEL`) so a
      kernel the driver cannot build for does not fail its own install and
      leave apt broken ([test-matrix.md](../docs/test-matrix.md)); the
      install script names the running kernel and the fix when it is not
      supported. Acceptance: on Bookworm, installing 6.6.51 with
      `radioberry-dkms` installed completes, and a trixie system on 6.1
      (a user's report) gets the message.
- [ ] Bookworm, if it is to be supported: the four source changes and the
      packaging findings in [test-matrix.md](../docs/test-matrix.md)
      (service not enabled, DKMS skipping the Pi 5's newer kernel at
      install, libgpiod from backports at run time), a `bookworm` suite in
      the repository, and the kernel matrix there. 6.12.19 (Bookworm's
      first 6.12) gives no receive data on the Pi 5; 6.6.74 works with
      one include change; 6.6.62 and older do not build. The board owner
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
- [ ] Integration test: a fake client against the daemon on loopback for
      both protocols, with fault injection for every fail-safe path.
      [tools/protoprobe/tests/daemon_test.cpp](../tools/protoprobe/tests/daemon_test.cpp)
      already runs `p1probe` and `p2probe` against the daemon and the fake
      device; the fault injection is open.
- [ ] piHPSDR 3.1 over Protocol 2 at 48 ksps received nothing in three of
      four starts on the Pi 5 (radioberryd logged the stream; piHPSDR
      showed -200 dBm), while 192 and 384 ksps always worked and `ft8rx`
      over Protocol 2 at 48 ksps works. Find which side: a capture of the
      Protocol 2 packets at 48 and 192 ksps, compared.
      Not reproduced on 0.5.0 (the failures were on a pre-0.5.0 build):
      eight starts at 48 ksps on the Pi 5 (trixie), all receiving. In four, captured on lo, piHPSDR asked for DDC0 at
      48 ksps, 24 bits, and `radioberryd` sent DDC0 packets with non-zero
      samples to its port from 3.3 s, at the rate, as at 192 ksps; the
      other four showed 20 m FT8 on piHPSDR's spectrum. Close unless it
      comes back.
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
      `/usr/local/bin/radioberry`. (Its SoapySDR module is already
      reported.) Report it as a blocker named "the
      original RadioBerry software", list the files found, and say what to
      remove; remove nothing; point to
      [docs/original-software.md](../docs/original-software.md).
      `postinst` prints the same. Acceptance: tests through
      the fake source for each signal alone and combined, and no finding on
      a Pi with only the packages installed.

## 6. SoapySDR module (`soapy/`)

- [ ] The module: what is left after the runs on both boards is in
      [SOAPYSDR.md](SOAPYSDR.md), with their results.
- [ ] Programs written for the SoapyRadioberry module:
      [SOAPYSDR-COMPAT.md](SOAPYSDR-COMPAT.md) lists where the interfaces
      differ and what could be matched. The board owner decides, after
      asking which calls such programs make.

## 6a. Tests


## 6a. Tests (continued)

- [ ] One SoapySDR module test failed once (1 of 45 cases, 1 assertion)
      in a run on a build Pi under another session's -j4 load,
      2026-11-20; which case was not captured. 33 further full runs and
      100 runs of each new jack keying case passed. Acceptance: the case
      found (run the suite under load with `--durations yes` and keep the
      output) and made independent of timing.

## 6b. Observed on the Pi 4, 2026-11-19 (image of `7828030`, seed 3)

- [ ] On air with piHPSDR and WSJT-X (40 m FT8), the board owner saw the
      radio key with no RF out for a short time, about 23:33Z; waiting a
      minute cleared it; first time seen. The daemon logged every
      transmission whole (13.8 to 13.9 s, 0 TX blocks dropped; one cut to
      6.5 s at 23:33:52), so the samples reached the gateware; whether
      they carried signal is not recorded. Also in that session: image
      `tx_underruns` 827 (0 in the stage 7 contact earlier that day), one
      receive stall of 200 ms at about 23:18Z, kernel UdpRcvbufErrors 803,
      no under-voltage. Not explained; the board owner's view: probably
      WSJT-X, or RF from a nearby kilowatt station in the shack. Acceptance: the cause found, or the
      next occurrence caught with the TX audio level, the image counters
      and the receive stall times logged together.

## 7. Hardware validation

Transmit items each need the board owner's go-ahead at the time.

- [ ] SparkSDR and Quisk receive and tune via Protocol 1.
- [ ] Two-tone linearity: a test client's two tones measured third-order
      products at -13 to -15 dBc at every drive from 64 to 170 on the
      released image, while piHPSDR's carrier and FT8 showed no products
      above a floor about 46 dB down. Establish whether the result is the
      test client or the radio, then measure where compression sets in.

## 8. Development Pi (Pi 5)

- [ ] Wi-Fi background scans: while associated, the Pi 5 scans every
      33.7 s for 3.65 s, which stalls UDP to a Wi-Fi client for about
      130 ms at a time ([docs/testing.md](../docs/testing.md)). Stop the
      scans (for example by locking the connection to the access point's
      BSSID in the netplan configuration), with a rollback in place since
      Wi-Fi is the Pi's only link; the board owner's decision. Acceptance:
      RX packet lateness to a Wi-Fi client under 10 ms at p99.9 over 60 s.
