# Active work

Open work only, ordered by dependency: take the first unchecked item whose
dependencies are done. Each item names its acceptance test. A finished
item is removed from here and recorded at the top of
[TODO/COMPLETED.md](COMPLETED.md) with its date.

## Where things stand

- Released: v0.4.1 (Raspberry Pi 4 and 5, Linux 6.12 and 6.18,
  [docs/kernels.md](../docs/kernels.md)), as Debian packages from the
  apt repository and on GitHub. Its Pi 5 path (RP1 PIO with the
  Hermes-Lite 2 PIO gateware 75.3) is unchanged on the development branch.
- Development branch `item/new-direction`: gateware-v2
  ([gateware-v2/plan.md](../gateware-v2/plan.md)), separate gateware per
  board. Raspberry Pi 4: the `rpi4-i2s` image, an I2S link into the
  BCM2711 PCM block driven by the driver directly (no ALSA), the framed
  SPI control protocol ([driver/kernel/rb_frame.h](../driver/kernel/rb_frame.h)),
  receive and transmit. On air through piHPSDR and WSJT-X: FT8 decoded by
  stations across Europe and in North America; 13% CPU at 192 ksps.
  Transmit power into a dummy load, drive 32 to 170: about 1 dB per AD9866
  gain code over codes 10 to 25, compression starting near 4.5 W.
- `radioberryd`: Protocol 1 (HL2 dialect) and Protocol 2, one owning
  client, transmit only with `--allow-tx` through the transmit policy and
  fail-safes; it no longer sets the TX buffer latency or PTT hang time
  (address 0x17): the gateware owns them.
- Next, in order: the Pi 5 `rpi5-pio` image and its driver transport
  (section 1, first item); then transmit on the Pi 5 with the board owner.

## Standing decisions

- C++20 throughout; C only in the kernel module and the ABI header.
- Unit tests on every component, aiming at complete coverage; kernel
  module logic compiled into a userspace test binary. Catch2 v3, gcov with
  lcov reports, ASan and UBSan test builds.
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
- Gateware: one image per board and FPGA. Released Pi 5 path: the
  Hermes-Lite 2 RadioBerry PIO variants (75.3) built unmodified from
  `gateware/source/` ([tools/build-gateware.sh](../tools/build-gateware.sh)).
  gateware-v2 images: `gateware-v2/projects/`, built by
  `gateware-v2/tools/build.sh`; the Hermes-Lite 2 radio core read
  unmodified from `gateware/source/`. Quartus Prime Lite 23.1std.1 in
  Docker on an x86-64 machine; bitfiles are committed and the Pi only
  installs them. The radio never appears as a sound card.
- The driver picks its transport from the SoC and each transport names its
  image: Pi 5 PIO; Pi 4 I2S when the `radioberry-i2s` overlay is loaded,
  else the CPU-clocked buses with the PIO image. The `rpi4-i2s` image is
  built for the 10CL025 only.
- Development FPGA is a 10CL025. The driver supports CL016 and CL025.
- The PIO gateware's RX lock-up
  ([docs/fault-rx-lockup.md](../docs/fault-rx-lockup.md)) is worked around
  by the driver's RX clock hold (`rx_clock_hold_us`, default 200) on the
  released image; the `rpi5-pio` image fixes it at source.
- The daemon assumes nothing about the image: gateware details (FIFO
  sizes, latencies) belong to the gateware and the driver.
- No transmit test without the board owner's explicit permission for that
  specific test, with the dummy load and external PA state confirmed. Any
  PTT is a transmission. Transmit code is proved first against fakes.
- Names: daemon `radioberryd`, unit `radioberry.service`, config
  `/etc/radioberry/config.yaml`, module `radioberry.ko`, C++ namespace
  `radioberry`, tools `rbprobe`, `rbcat`, `rbctl`, `rbload`, `p1probe`,
  `p2probe`.
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
  (four at 192 ksps), 192 000 on the Pi 4's CPU-clocked buses. The daemon
  runs fewer receivers than asked for above it, never fewer than one.
- Code style: clang-format 19, Google base, 4-space indent, 100 columns;
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`. The kernel
  module and ABI header use kernel style and are not clang-formatted.
- Version string from `git describe`, embedded at configure time.

## 1. gateware-v2 (`gateware-v2/`, `driver/`)

- [ ] `rpi5-pio` image and its driver transport
      ([plan.md](../gateware-v2/plan.md) section 5, stage 1). Gateware: the
      `rpi4-i2s` top level's control, AD9866 set-up, keying and PA logic
      with the present PIO wire protocol and pins (RX: clock GPIO 6, data
      18 to 21, sample-ready 25; TX: clock 4, data 5, ready 12;
      [gateware/README.md](../gateware/README.md) sections 5 and 7), the
      receive read-out rebuilt without the lock-up (synchronous clear,
      registered outputs, a reset), the transmit intake reset after a
      still clock, constraints for the Pi's clocks. Board registers as
      `rb_frame.h`, CAPS transport PIO. Driver: a Pi 5 transport made of
      the PIO stream functions and the framed command path
      (`rb_i2s_command`), the identity check per transport, its own image
      name; the Pi 4 I2S and CPU transports unchanged. Package: the image
      installed and linked as the I2S one is. Acceptance: `ft8rx` decodes
      on the Pi 5 over Protocol 1 and 2 with no packets lost;
      `rbdrivertest` passes on the Pi 5; frame tests and the Pi 4 I2S
      build unchanged. Transmit on the Pi 5 with the board owner after.
- [ ] Pi 4 install instructions: the README quickstart and the
      `postinst` message name only `dtoverlay=radioberry`, which gives a
      Pi 4 the CPU-clocked transport. Add `[pi4] dtoverlay=radioberry-i2s`
      `[all]`, or fold the PCM fragment into one overlay that still
      applies on a Pi 5. Acceptance: a clean Pi 4 following the README
      comes up on the I2S transport.
- [ ] Pi 4 transmit, 30 m: the image counted 482 TX underruns across the
      30 m transmissions (drive 150, capped from 162), none on 40 m before
      or after. Cause unknown. Acceptance: the cause found from a counter
      log of one 30 m transmission (`/root/txcount-log.sh` pattern).
- [ ] Transmit drive curve: power rose about 1.06 dB per AD9866 gain code
      against the datasheet's 0.5 dB. Cause unknown (PA gain with level,
      TxDAC/IAMP split, the board's register 0x0e). Measurement first.
- [ ] TX bit 4 (FPGA pin 105) misses the AD9866 setup by 2.7 ns in the
      slow model and is relaxed in the constraints
      ([hardware_discovery.md](../gateware-v2/hardware_discovery.md)
      section 4). Watch the `strict` line of `tools/worst.sh` on every
      build; investigate if bits 4 or 10 of transmitted samples show
      errors.
- [ ] Documentation for gateware-v2: the TX bus in hardware_discovery, a
      README for gateware-v2 like gateware/README.md, the driver's
      `tx_loopback` and `i2s_counters` sysfs files, docs/daemon.md.
- [ ] Optional: a lock-phase status bit in the I2S image, so each TX
      stream's realign is attributable (explanation now inferred).

## 2. Kernel driver remainder (`driver/`)

- [ ] Raspberry Pi 4 on 6.12.75: the harness test "the daemon's device
      layer streams through the driver" failed once in 11 runs (a gap,
      `rx_blocks_dropped` 31, RX queue full) on the CPU-clocked transport.
      Find the cause. Acceptance: the cause identified, and fixed if it is
      ours.
- [ ] Upstream review: the driver has not been through review on the
      kernel mailing lists.

## 3. Daemon (`daemon/`)

- [ ] HL2 drive compatibility: the HL2 gateware takes 0x09's top four
      bits as AD9866 TX gain codes 0 to 15, the RadioBerry images map the
      whole byte to codes 0 to 39, and the PA hat is not the HL2's, so HL2
      software sends far too little drive. With the measured curve
      (section 1), decide whether `radioberryd` maps HL2 drive to
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
      a Pi with only the packages installed.

## 5. Gateware (released PIO images)

- [ ] Test the CL016 image on a CL016 board, receive and transmit, when
      one is available. (The `rpi4-i2s` image has no CL016 build; not
      pursued.)

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
