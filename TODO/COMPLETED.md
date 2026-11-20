# Completed work

Newest first. Each entry gives the date, what was done, its acceptance
result, and where it lives.

- 2026-11-20: The SoapySDR module follows radioberryd's `jack_keying`
  (the board owner's correction of 2026-11-19's choice to ignore it): the
  key jack may key while a program has the device open. Tests for both
  settings and a line in docs/soapy.md.
- 2026-11-19: The SoapySDR test `without the gateware counter MOX is held
  for the fixed tail` no longer reads the real driver's sysfs on a Pi with
  the module loaded: the module takes the directory from its settings,
  and the test points it at one with no driver attribute. 42 module test
  cases pass.
- 2026-11-19: The daemon and probe tests' ports no longer collide. Both
  suites took ports in the kernel's ephemeral range (by pid, or port 0
  with TCP bound to the number UDP got), where other programs' sockets
  live: `p1probe streams from radioberryd` failed in a CI run that way.
  `daemon/tests/free_ports.hpp` now gives each daemon a range below the
  ephemeral range that no socket holds, UDP or TCP. The 38 daemon and
  probe tests passed three times in a row run four at a time.
- 2026-11-19: Core takeover, stage 6d built and simulated (branch
  `item/gateware-core`; board and keyed runs still open in the plan).
  - The jack's key and PTT keyed the core, powered the TxDAC and set the
    transmit gain with no permission and no watchdog (only the PA lines
    needed permission). Both images (`b8b7fc9`): they pass only with
    KEYING's permission bit, so the keying watchdog stops them with the
    host. `tools/sim.sh rpi4tx`, `rpi4`, `rpi5` pass.
  - `radioberryd` (`abbefe1`): `jack_keying` (default false; needs
    `allow_tx` and a transmitter) holds the permission bit, never PTT,
    while a client owns the radio; a release or a stream fault clears it
    until the owner's next packet. 348 tests pass on a Pi 5.
  - The CW envelope a raised cosine over the level counter's ramp
    (`rtl/core/cw_shape.sv`), the same 4.1 ms and peak as the straight
    line: within 1.3 LSB of the ideal curve (`tools/sim.sh cwshape`).
    Click spectrum of one element, dB under the carrier, straight line /
    raised cosine: 500 Hz -53.2 / -47.6, 1 kHz -67.1 / -74.3, 2 kHz -68.6
    / -89.8, 5 kHz -81.9 / -114.6; above -80 dB to 4 975 / 1 700 Hz.
    `radioeq` against `b338416`: 0 mismatches outside the DAC words.
  - `tools/sim.sh failsafe` (`3390368`): the Pi 4 transmit build with the
    real core; each path keyed under the host's keepalive, which then
    stops. Transmit ended after KEYING cleared: software PTT 1.9 ms (its
    2 ms hang), PTT hang 31 at once, jack PTT at once, the jack's key at
    full level 5.0 ms (1 ms latency, then the ramp down); after each the
    core idle, PA lines and TX quiet low, the AD9866's transmit path off.
  - Pi 5 with the 6d image (Pi 5 seed 1): `rbprobe` passes, `rbcat`
    clean, FT8 over Protocol 1 and 2 with nothing lost; loopback tone
    -18.7 dBFS, image -142.8, carrier -131.1 dBFS, counters clean.
  - Our own toolchain: `b8b7fc9` proven equivalent to its sources for
    both images (rpi5-pio 28 424 obligations, rpi4-i2s 29 253, none
    differing or undecided); every one of the 20 seeds' bitstreams
    rebuilt byte for byte from Quartus's routing. The jack is not tested
    on the board (the board owner accepts the simulation).
- 2026-11-18: Core takeover, stages 5 and 6a to 6c (branch
  `item/gateware-core`). Every seed of ten of both images meets timing at
  `b338416`: the Pi 5 image 13 226 to 13 278 logic elements, the Pi 4
  image 13 814 to 13 868. Our own toolchain rebuilt every stage 6c
  bitstream byte for byte from Quartus's routing.
  - Stage 5 (`1b4db11`): the receive sender (`rx_sender.sv`), bit for
    bit. The variable CIC's output rounded halves to even (`6c11187`): no
    DC offset (loopback 0 Hz bin -131.5 dBFS, -120 before).
  - Stage 6a (`72da3fe`): the transmit interpolating filter
    (`tx_fir.sv`), bit for bit over 9.6 million clocks; its coefficients
    are symmetric, so half are stored. Stage 6b (`1b9d8bd`): the transmit
    CIC interpolator (`tx_cic.sv`), each stage only as wide as it grows,
    bit for bit.
  - Stage 6c (`03b5706`, `b338416`): the CORDIC up-converter (about 1 770
    cells) replaced by `tx_upconv.sv`: the same NCO, a multiplier mixer,
    the CORDIC path's level, rounding, a +-1 LSB triangular dither and
    saturation (the original truncated and wrapped). The board owner
    chose dither on; a one-bit input can turn it off. Simulated
    (`tools/sim.sh txupconv`; worst spur 0 to 38.4 MHz, dBc, CORDIC / new
    without dither / with): 14.074 MHz at -1 dBFS -101.1 / -99.5 /
    -104.4; at -20 dBFS -75.5 / -73.7 / -84.8; 7.074 MHz -96.3 / -94.1 /
    -104.5; 28.074 MHz -97.0 / -94.4 / -103.4. Level within 0.002 % of
    the CORDIC's; DC -66 / -83 / -83 dBFS; SNR and distortion at -1 dBFS
    73.1 / 73.0 / 68.3 dB. Per hertz the dithered floor is about 6 dB
    above the AD9866 TxDAC's typical figure (-144.1 against -149.8
    dBc/Hz; the datasheet's conditions are not this board's). Dithers of
    half and a quarter of an LSB were simulated and not taken (worst spur
    at -20 dBFS -81.7 and -76.5 dBc). `radioeq` against `03b5706`: 0
    mismatches outside the DAC words over 800 768 clocks.
  - Pi 5 with the 6c image: `rbcat` clean, FT8 over Protocol 1 and 2 with
    nothing lost; loopback tone -18.7 dBFS as before, image -144.3,
    carrier -131.3 dBFS, counters clean. Keyed into the dummy load with
    the board owner's external amplifier in line (drive 128, a 1 kHz tone
    at 0.8 of full scale, 3 s): 12 W with the released image, 13 W with
    this one, the receive path hearing both tones within 0.1 dB. On air,
    40 m FT8 through piHPSDR and WSJT-X: 13 transmissions, no block
    dropped, image counters clean, spotted by seven stations in five
    countries; one answered and could not decode the reports sent to it.
- 2026-11-18: Core takeover, stages 1 to 3 (branch `item/gateware-core`,
  plan [2026-11-17-core-takeover.md](2026-11-17-core-takeover.md)). Every
  seed of ten of both images meets timing at `c653e37`; the Pi 5 image is
  14 647 to 14 720 logic elements (19 828 before), the Pi 4 image 15 192
  to 15 276 (about 20 400 before).
  - Stage 1 (`50d4a22`, `ad4726b`, `bfeb533`): the Hermes-Lite 2 core's
    `radio.v` in the tree as `gateware/rtl/core/radio.sv`, first
    unchanged, then with this project's two patches (the same Pi 5
    bitstream byte for byte), then without what no client reaches: the
    FPGA-side VNA scanner, the board-to-board link, the envelope PWM for
    EER, the debug outputs. Against the version before
    (`tools/sim.sh radioeq`): every output on every clock equal over
    800 768 clocks. About 250 logic elements fewer. Pi 5: `rbcat` clean
    at every rate, FT8 over Protocol 1 and 2 with nothing lost, loopback
    tone -18.7 dBFS as before.
  - Stage 2 (`12c6864`, `45a42e9`, `19c1441`): the receive CIC
    decimators. The variable CIC's combs for all receivers in one engine
    with their state in a memory (`rx_varcomb.sv`; the integrators stay
    each receiver's, in `rx_cic_int.sv`: their state would need about 12
    memory blocks more than the device has free), bit for bit and clock
    for clock. Then the registers pruned (Hogenauer) to the tolerance the
    board owner set, the pruning error's variance at most a quarter of
    the 16-bit output rounding's (rms at most 0.144 output LSB): the
    variable CIC's input shifted by the decimation so that one set of
    widths serves every rate, its integrators 43, 37, 33, 29, 25 bits
    with rounded inputs, its combs 23, 22, 21, 20, 20; the first CIC this
    project's own, its combs 21, 20, 19 bits, its integrators whole, its
    output rounded halves to even. Truncating into an integrator, pruning
    the first CIC's integrators and rounding halves up each left an
    offset (0.38, 0.14 and 0.047 LSB) and were not used. Measured
    against the exact CICs (`tools/sim.sh cicprune`, 24 000 values a
    decimation): error rms 0.079 to 0.097 LSB, mean about 0.001 LSB.
    Estimated, not measured: about 0.2 dB more noise at 48 ksps over the
    ADC's own. About 4 900 logic elements fewer. The Pi 4 image missed
    timing on one seed of ten, in the NCO's table to multiplier path.
  - Stage 3 (`5064682`, `c653e37`): the NCO's sine and cosine
    (`sincos.sv`) with the tables' outputs and the multipliers' inputs in
    registers the device has, and the mixers with their NCOs
    (`mix2.sv`), each mixer's state register kept as its own; both the
    same values on the same clocks as the originals
    (`tools/sim.sh sincoseq`, `mixeq`: 200 000 clocks each, 0
    mismatches). Every seed of both images meets timing.
  - On the board: the Pi 5 received with the stage 2 and stage 3 cores as
    our own toolchain built them (its synthesis, routing and bitstream on
    Quartus's placement): `rbcat` clean at every rate, FT8 over Protocol 1
    and 2 with nothing lost.
- 2026-11-17: Gateware rework, first part (branch `item/gateware-core`,
  [core-rework.md](../gateware/docs/core-rework.md)). `rbv_i2s`'s word and
  check logic registered a cycle early (identical to v0.6.0's over 1.7
  million cycles in simulation). The receive filter replaced by design
  A+B, one folded filter shared by all receivers: equal to design A's
  arithmetic value for value, within -118 dBFS rms of the original, on 52
  of 132 multiplier elements (the original's took all 132). Pi 4 image,
  ten seeds each: v0.6.0 met timing on 6, the reworked I2S logic alone on
  10, with A+B on 10, every seed limited by the ADC input pin. Pi 5
  image with A+B (one seed, meets): `rbcat` clean at three receivers at
  384 ksps and six at 192, FT8 over Protocol 1 (78 decodes) and 2 (103)
  with nothing lost, loopback as v0.6.0, and the board owner's FT8
  contact on air. Designs A and B built and measured, then dropped.
- 2026-11-18: Release 0.6.1, driver only: on the Pi 5, transmit after a
  gateware reload through sysfs (TX transfers in flight are completed
  before the PIO is closed; on 6.12 kernels rp1-pio kept counting them and
  every later TX transfer timed out), with an `rbdrivertest` case for it;
  the version ioctl reports the release's version; three SoapySDR tests
  wait for the unkey the device thread sends (a race the release CI
  found). Gateware images those of 0.6.0, byte for byte (sha256 checked in
  the packages). Full CI with and without SoapySDR (100% line coverage
  both ways). Pi 5 gate: everything removed, README and quickstart
  followed exactly from the live repository, `rbprobe` exit 0, `ft8rx`
  40 m over Protocol 1 and 2 with 0 packets lost, `rbcat` clean at four
  rates, the loopback self-test the same from a fresh driver and after
  each of two reloads; the board owner's FT8 contact on air, 0 TX blocks
  dropped. Repository sources file now names its architectures (no
  `armhf` notice from apt). Pi 4 not tested (off).
- 2026-11-16: Release 0.6.0. Full CI with and without SoapySDR (100% line
  coverage both ways). Release gate on both boards: every RadioBerry trace
  removed, then installed exactly as the README and quickstart say (DKMS
  built the driver for every installed kernel, 6.12.25 to 6.18.60),
  `rbprobe` exit 0, `ft8rx` on 40 m over Protocol 1 and 2 with 0 packets
  lost, idle 0 CPU. FT8 on air through piHPSDR and WSJT-X: Pi 4, 17
  transmissions and a contact, 0 blocks dropped, image TX underrun bursts
  in 5 (an open item); Pi 5, 22 transmissions, 0 blocks dropped, image
  counters all 0; both spotted across Europe. Packages in the apt
  repository and on the GitHub release.
- 2026-11-14: Release 0.5.0 preparation, Pi 5. Packages
  0.4.1+git35.gbcc3654 upgraded in place from 0.4.1+git22 with apt (DKMS
  for all 14 installed kernels, both images linked, new driver loaded
  without a reboot). Kernel matrix, 11 `rpt-rpi-2712` kernels 6.12.25 to
  6.18.60: `rbprobe` exit 0, `ft8rx` 20 m (evening, 6 to 19 decodes per
  protocol) with 0 packets lost, `rbcat` with no resyncs, `rbdrivertest`
  12/12, no under-voltage during the runs, image TX counters (gaps, bad
  frames, overflows, underruns, realigns) all 0, on every kernel. Driver
  version string set to 0.5.0.
- 2026-11-14: Release 0.5.0 preparation, Pi 4 and CI. CI on a cluster Pi
  passes in full with 100% line coverage (4646 lines; 343 tests, 340
  under ASan and UBSan). Pi 4: packages 0.4.1+git33.gb66dfe1 upgraded in
  place from 0.4.1+git26 with apt (DKMS for every kernel, both images
  linked, `postinst` warned about the old `radioberry-i2s` line, which was
  then removed; the new driver loaded without a reboot). Kernel matrix,
  11 `rpt-rpi-v8` kernels 6.12.25 to 6.18.60: `rbprobe` exit 0, `ft8rx`
  20 m 63 to 95 decodes per protocol with 0 packets lost, `rbcat` 48 000
  frames per second with no resyncs, `rbdrivertest` 12/12, no
  under-voltage, on every kernel. The image counted 0 to 2 TX frames
  dropped (flags check) per boot out of about 560 000 unkeyed zero
  samples; when in the run they fell is not known.
- 2026-11-14: Adopted the new gateware. The old `gateware/` (Hermes-Lite 2
  PIO images, CL016 and CL025) removed; `gateware-v2/` is now `gateware/`
  with the Hermes-Lite 2 source archive and the Quartus Dockerfile, its
  own README and `docs/`. Driver: the PIO and I2S transports only, the Pi
  4's PCM pins set by the driver (one overlay on both boards; on the Pi 4
  with the test module, `ft8rx` 20 m 67 decodes over P1 and 69 over P2,
  none lost), no RX clock hold, no 48-bit command path, `rbload` and
  `rbbus` removed. Packages: no FPGA question, both images installed.
  Installed config `frequency_ppm: 22.9`, `realtime: false`. Daemon: no
  false RX stall at the first client after idle. Driver and daemon READMEs
  and `docs/` folders; components table in the README. The Pi 4 SMI work
  (`TODO/RPI4-SMI.md`, branch `item/rpi4-smi`) is superseded by the I2S
  image and its plan removed from here.
- 2026-11-14: Pi 5 transmit through the `rpi5-pio` image: TX bit 4 (pin
  105) strict setup slack -2.72 ns, as the Pi 4 build; digital loopback
  self-test passed (+1 kHz tone at -18.7 dBFS, opposite sideband -144.7,
  carrier -138.2, counters clean); drive 128 into a dummy load 1.4 W (the
  Pi 4 gave 1.35 W); on air through piHPSDR and WSJT-X on 20 m FT8 at
  drive 150: about 240 PSK Reporter spots in five minutes (Europe, North
  America, Japan, Korea) and an FT8 contact. 13 transmissions, about
  47 900 samples a second, 0 blocks dropped, 0 image overflows or
  underruns, 0 RX stalls.
- 2026-11-14: Pi 5 receive through the `rpi5-pio` image (branch
  `item/new-direction`): the `rpi4-i2s` control, AD9866 set-up, keying and
  PA logic with the released PIO wire and pins; the receive FIFO's read
  side on the Pi's RX clock and neither FIFO ever cleared, so a stopped
  Pi clock cannot lock it up. Driver transport `rp1-pio` with the framed
  command path and image `radioberry-rpi5-pio.rbf` (module parameter
  `pio_released_image=1` keeps the released image); packaged and linked
  as the I2S image. Testbench `tools/sim.sh rpi5` runs the driver's PIO
  instruction words: in order across pauses, program restarts and
  receiver-count changes with the clock running; transmit words exact and
  held back by ready. Timing met (RX clock to pins within 15 ns, +5.2 ns
  slack). On the Pi 5: FT8 on 40 m, 4 decodes in 3 slots over Protocol 1
  and 8 over Protocol 2, 0 packets lost; `rbdrivertest` 12/12; driver
  unit tests pass; no RX words move while idle. Transmit not yet tested.
- 2026-11-14: Pi 4 transmit through the `rpi4-i2s` image (branch
  `item/new-direction`): digital loopback self-test passed (+1 kHz tone at
  -43.6 dBFS, counters clean); power into a dummy load measured from drive
  32 to 170 (about 1 dB per gain code, 4.5 W at 170); on air through
  piHPSDR and WSJT-X on 40 and 30 m FT8, decoded by stations across Europe
  and in North America. Key-up loss found and fixed: the daemon's
  Protocol 2 server set a 100 ms TX buffer latency sized for the PIO
  image's flow-controlled FIFO, overflowing the I2S image's 2048-sample
  FIFO by about 2720 samples at every key-up; the daemon now leaves
  address 0x17 to the gateware (f7a38aa), and 26 transmissions after it
  showed 0 overflows. `tools/worst.sh` prints the TX bus's strict setup
  slack (bd7c980).
- 2026-11-13: Pi 4 receive through the `rpi4-i2s` image: I2S link into the
  PCM block with cyclic DMA (61.44 MHz both ways, 0 errors over 10 min
  idle and 10 min under load), driver I2S transport, framed SPI control,
  6 receivers; FT8 decoded over Protocol 1 and 2 with 0 packets lost.
- Superseded 2026-11-13 by the gateware-v2 direction, not done: the Pi 4
  RX over SMI ([RPI4-SMI.md](RPI4-SMI.md), built and tested on
  `item/rpi4-smi`, abandoned for I2S); the Pi 4 CPU cost with piHPSDR on
  the CPU-clocked buses (the I2S transport takes 13% at 192 ksps); the
  Pi 4 TX block drops on the CPU-clocked buses; fixing the released PIO
  gateware's RX lock-up in place (the `rpi5-pio` image replaces it).

- 2026-11-11: v0.4.1 released: the Pi 5 driver works on the 6.12
  kernels as on 6.18 (receive buffers from the RP1 DMA controller passed
  by DMA address, DMA request thresholds set by the driver;
  `docs/kernels.md`), and a package upgrade loads the new driver in
  place of the running one. Every `rpt-rpi-2712` and `rpt-rpi-v8` kernel
  in the archive (6.12.25 to 6.18.60, 11 each) on both boards: DKMS built
  the driver at kernel install, `rbprobe`, `ft8rx` on 40 m over both
  protocols with no packets lost, `rbcat` clean, `rbdrivertest` passed;
  one accepted anomaly (Pi 4, 6.12.75: one of 11 harness runs failed the
  device-layer test with its test queue full). On the Pi 4, the README
  install of 0.4.0 upgraded to 0.4.1 without a reboot or a warning.
- 2026-11-11: v0.4.0 released: Raspberry Pi 4 support (the same packages
  and driver on the Pi 4 and Pi 5), `radioberryd --protocols`, true or
  false values for its switches, `i2c-dev` loaded at boot (a clean Pi
  had no `/dev/i2c-1` for `rbprobe`), and the documentation for GitHub
  Pages. The RX ring's consumer sleeps in a futex instead of
  `std::atomic::wait`, which spins and yields first: on a Pi 4 with no
  client, `rb-rx-send` went from 7.6% of a core (70 219 `sched_yield`
  calls in 5 s) to 1.4% (none), and the delivery thread now starts
  before the device, so `rx-queue-full` at start went from 10 of 10
  starts to 0 of 10. The driver moves RX only while run is set, and
  `radioberryd` sets run only while a client streams: on a Pi 4 with no
  client the driver's thread went from a whole core to 0% and the
  daemon's threads to 0.2%; 100 client sessions over both protocols
  passed, `rbdrivertest` (with a new pause and resume test) passed, and
  `ft8rx` decoded over both protocols with no packets lost
  (`docs/testing.md`).
  CI on the Pi 5 (release, ASan and UBSan, coverage 98.9% of lines) and
  on GitHub passed on the merged tree.
- 2026-11-11: Raspberry Pi 4 on hardware (branch `item/platform-rpi4`):
  the same packages installed and DKMS built for `rpi-v8`; the README's
  build steps and `ctest` (319 tests) passed on the Pi 4; the CPU
  transport bound with pull-downs applied, `rbdrivertest` passed, `rbcat`
  streamed one receiver at 48 ksps for 10 minutes clean; `ft8rx` decoded
  over both protocols with no packets lost; FT8 transmitted, decoded on
  PSK Reporter, and a contact completed. `rbbus` measured the bus: clean
  to 768 000 RX values per second, a whole core above 192 000, which
  stays the driver's limit; SMI and DMA not needed. `docs/rpi4.md`; the
  Pi 5 specifics moved to `docs/rpi5.md`.

- 2026-11-10: Raspberry Pi 4 groundwork, Pi 5 unchanged (branch
  `item/platform-rpi4`; docs/rpi4.md). One overlay for both boards
  (`tools/check-overlay.sh` merges it onto both trees in CI); `rbload` and
  `rbprobe` on either board; driver 0.5.0 with the RP1 PIO transport moved
  unchanged behind a transport table and a CPU-clocked transport for the
  Pi 4 (`driver/kernel/rb_bitbang.h`), chosen by SoC at probe;
  `radioberry_info` reports `transport` and `rx_values_max`, and
  `radioberryd` limits receivers by it; `rbbus` for measuring on a Pi 4;
  bus code tested against a simulation of the gateware's bus modules.
  Pi 5 acceptance with the 0.5.0 packages installed: CI and `rbdrivertest`
  pass; booted with the new overlay; lock-up harness 0 stalls in 600 runs
  with the hold on; `rbcat` six receivers at 192 ksps and three at
  384 ksps for 30 s each with no resyncs, drops or overruns; `ft8rx`
  decoded over Protocols 1 and 2 (four slots each) with 0 packets lost.
  Nothing has run on a Pi 4 yet.
- 2026-11-10: v0.3.0 released: the first release as Debian packages
  (`radioberry`, `radioberry-dkms`, `radioberry-dev`), and the README's
  section for a Pi coming from the original software. Release check on
  the development Pi with the packages built from the release tree
  (differing from the tag only in the version strings and this entry)
  installed over the Pi 4 branch build and rebooted: the packaged overlay
  in place, driver 0.4.0 loaded by DKMS, `rbprobe` exits 0; `ft8rx` over
  `radioberryd` on 20 m decoded 1 message in four slots over Protocol 1
  (22532 packets, 0 lost) and 4 over Protocol 2 (12102 packets, 0 lost).
- 2026-11-09: Debian packages `radioberry`, `radioberry-dkms` (driver
  built by DKMS), `radioberry-dev`, built by `tools/build-deb.sh`; the FPGA
  type is a debconf question; the overlay is copied to `/boot/firmware`
  by the install script (dpkg cannot replace files on FAT). On the
  development Pi: DKMS built for all five installed kernels; install,
  reinstall, upgrade from a signed reprepro repository, remove and purge
  passed; after a reboot `rbprobe` exited 0 and a Protocol 1 receive test
  lost no packets. Guide to building, installing and hosting an apt
  repository: `docs/packaging.md`.
- 2026-11-09: v0.2.0 released. Release check on the development Pi with
  the final build installed: `ft8rx` over `radioberryd` decoded FT8 on
  40 m (15 messages in four slots on each protocol) and 20 m (84 over
  Protocol 1, 100 over Protocol 2), no packets lost; `rbprobe` exits 0.
- 2026-11-09: Kernel and C++ checks. `checkpatch.pl` (no errors or
  warnings) and `sparse` (silent) over the driver, and clang-tidy over the
  userspace C++ (fails on any warning), all in CI. Driver: `usleep_range()`
  for short waits, `ARRAY_SIZE`. Debug-kernel session (6.18.60 with KASAN,
  lockdep, DMA API, kmemleak, UBSAN, booted once with tryboot): no reports
  from the driver; `rbdrivertest` passed; kmemleak found `rp1-pio` leaking
  64 bytes per transfer in flight at stream teardown
  (`docs/kernel-debug.md`).
  Chasing a CI hang found and fixed two bugs: Protocol 2 Receiver Specific
  overtaking General was dropped (no DDC stream), and the probe clients'
  poll() was unbounded.
- 2026-11-09: `rbload` (`tools/rbload/`): loads a gateware image through
  the passive serial lines with the driver unloaded, refusing unknown
  images and other families unless `--force`, and an image for the other
  FPGA always. On the development Pi: refused with the driver loaded
  (exit 2); CL025 image configured (exit 0, 4.7 s); CL016 image CONF_DONE
  low (exit 1); upstream GPIO-mode image refused (exit 2); the driver then
  loaded with `load_gateware=0` read gateware 75.3 and `rbcat` streamed
  at 48000.9 frames per second.
- 2026-11-09: `p1probe` and `p2probe` (`tools/protoprobe/`): discovery,
  receive measurement (rate, sequence, levels) and zero-drive MOX over
  Protocol 1 and 2. Unit tests against a scripted radio; end-to-end tests
  against `radioberryd` with the fake device (MOX reaches the device only
  with transmit allowed; a busy radio is reported). Against the live
  daemon, receive only: P1 two receivers at 192 ksps and three at
  384 ksps, P2 six DDCs at 48 ksps and three at 192 ksps, each at the
  expected rate with no sequence breaks. `radioberryd` used 0.56 s of CPU
  in 10.6 s while serving three receivers at 384 ksps.
- 2026-11-09: Documentation restructured. README holds only the overview,
  status and installation (with the `apt install` line); usage moved to
  `docs/driver.md`, `docs/daemon.md` and `docs/tools.md`, licensing to
  `docs/licensing.md`, layout and CI to `CONTRIBUTING.md`. The design is
  `docs/design.md` (from the plan); the reference-project survey was
  removed. Dates, names and callsigns removed from the docs. Installed
  config lists every option with a description, transmit and PA off.
- 2026-11-09: `radioberry.service` (`daemon/systemd/`): the unit runs
  `radioberryd --config /etc/radioberry/config.yaml` as the `radioberry`
  system user with CAP_SYS_NICE and CAP_IPC_LOCK only, bound to
  `dev-radioberry.device`, restarted on failure; a sysusers.d entry, a
  udev rule (`/dev/radioberry` root:radioberry 0660, tagged for systemd)
  and the config file (installed only where absent), all installed by
  `cmake --install`, which also creates the user and reloads udev and
  systemd but enables nothing. `--config` reads a YAML file of options
  (`daemon/src/options.cpp`); under systemd the log goes to the journal.
  `rbprobe` finds the daemon in sbin and lists the Protocol 2 DDC ports.
  Acceptance on the development board: `rbprobe` exits 0 ("radio can
  run: yes") with the service active; the daemon ran as `radioberry`
  with only those two capabilities, all its threads at SCHED_FIFO, and
  delivered loopback RX at p99 0.89 ms.
- 2026-11-09: Real-time setup for `radioberryd` (`daemon/src/realtime.cpp`):
  SCHED_FIFO 20 for the two device threads and 10 for the network
  threads, below the driver's kernel thread (50); memory locked;
  `--no-realtime`; UDP sockets marked DSCP EF and SO_PRIORITY 6. Socket
  buffers stay at 1 MiB; no CPU pinning, as nothing measured called for
  it. RX packet jitter measured and recorded in `docs/platform.md`: with
  four busy loops and the client at SCHED_FIFO 30, lateness p99 4.70 ms
  and maximum 9.8 ms without real-time, 0.88 ms and 3.0 ms with it (idle:
  0.89 and 3.9 ms). Over Wi-Fi p99 is 91 to 110 ms, all of it from the
  Pi's background scans (open item).
- 2026-11-09: Protocol 2 TX underruns. The driver counts an underrun
  when its ring is empty with PTT set and no DMA in flight, which happens
  whenever the gateware's TX FIFO has room, not only when it runs dry.
  piHPSDR sends no TX samples while receiving in Protocol 2, so the FIFO
  started each transmission empty and held only the TX buffer latency
  (address 0x17), never full: about 94 counted underruns per second (1 300
  in a 13.9 s FT8 transmission). The Protocol 2 server now sets 0x17 when
  a stream starts, 100 ms latency (more than the 85 ms FIFO) and 20 ms PTT
  hang (`daemon/src/p2/server.cpp`). Acceptance on the development board,
  piHPSDR in Protocol 2: 13.9 s FT8 transmissions at 47 811 and 47 829
  samples per second, 8 underruns each, 0 blocks dropped; with the
  driver's `debug` at 1 the 8 were logged 6 to 73 ms after PTT on, while
  the FIFO filled, and none after; a 10.7 s transmission logged 0.
- 2026-11-09: First FT8 contacts over Protocol 2, operated by the owner
  on 40 m: piHPSDR (Protocol 2) and WSJT-X on the Pi, `radioberryd
  --frequency-ppm 22.9 --allow-tx --allow-pa --drive-ceiling 150`, drive
  79 to 115, two contacts. PSK Reporter: 17 reports in the first
  three CQ slots, -10 to -25 dB, 15 of them at 7 076 472 to 7 076 494 Hz.
  Each FT8 transmission 12.4 to 13.9 s at 47 850 to 47 998 samples per
  second to the gateware, 0 TX blocks dropped, no faults, and about 94
  driver TX underruns per second (Protocol 1 transmissions logged none;
  see the open item).
- 2026-11-09: Protocol 2 in `radioberryd` (`daemon/src/p2/`): discovery
  (board 6, the gateware's version and receiver count), General, Receiver
  and Transmitter Specific, High Priority both ways, DDC I/Q from ports
  1035 onwards, Mic/Line silence and High Priority status paced by the
  radio's sample clock, TX I/Q at 192 ksps decimated to 48 ksps with
  liquid-dsp (`daemon/src/dsp/decimator.cpp`), DDCs below the gateware's
  rate decimated likewise, step attenuation taken off the LNA gain
  (`--p2-rx-gain`), the shared session, transmit policy and fail-safes,
  a TX I/Q sequence gap a stream fault. Protocol 1 unchanged apart from
  the base-port dispatch. `ft8rx --protocol 2`. Acceptance on the
  development board, receive only: `ft8rx --protocol 2` on 40 m decoded 5
  messages in 4 slots (13 083 packets, 0 lost), and `--protocol 1`
  afterwards 5 in 3 slots (20 529 packets, 0 lost); piHPSDR (installed
  build, Protocol 2 only, loopback network namespace) discovered the
  radio as a Hermes-Lite 2, started two receivers at 48 ksps and showed
  the 20 m spectrum, with 0 sequence errors in its log. A 35 s capture of
  that run: DDC0 and DDC1 201.7 packets/s each (48 000 samples/s),
  Mic/Line 750/s, status 20/s, no sequence breaks in any stream either
  way. The same piHPSDR over Protocol 1 showed the same spectrum lines at
  about 8 kHz spacing. Transmit tested against the fake device only.
- 2026-11-09: `radioberryd` logs the drive actually in use: PTT on logs
  the drive at that moment, and every later change of the effective drive
  while transmitting is logged, with the client's request when the
  ceiling capped it ("drive 64 (client asked for 144, ceiling 64)"). PA
  changes were already logged. Acceptance: unit test in
  `daemon/tests/tx_policy_test.cpp` against captured log output
  (`daemon/src/tx_policy.cpp`).
- 2026-11-09: First complete FT8 contact through `radioberryd`, operated
  by the owner on 40 m: piHPSDR and WSJT-X on the Pi, the RadioBerry
  driving an external 25 W amplifier (the owner's calibration), with a
  station about 2 000 km away. The daemon logged every transmission of
  the exchange on its FT8 slot, 13.9 s each, 47 900 to 48 200 samples
  per second, no underruns, no blocks dropped, no faults.
- 2026-11-08: Hardware validation, FT8 on air through `radioberryd`,
  operated by the owner: piHPSDR (Protocol 1, HL2 type) with WSJT-X on
  the Pi, audio through PipeWire loopbacks, CAT and PTT through
  piHPSDR's TS-2000 server on TCP 19090 (WSJT-X rig type Kenwood
  TS-2000; its OpenHPSDR PiHPSDR type does not pass Test CAT against
  this piHPSDR build, which answers `AI;` with `?;`). `radioberryd
  --frequency-ppm 22.9 --allow-tx --allow-pa --drive-ceiling 150`, a
  20 m end-fed half wave, about 4.8 W on 40 m. CQs keyed on the FT8
  slots, 13.9 s each, about 48 000 samples per second, no underruns, no
  blocks dropped. A HackRF One near the station saw the signal 292 to
  342 Hz above the 7.074 MHz dial, upper side, the same tone pattern in
  each CQ, no discrete spurs, harmonics or image above a broadband floor
  about 46 dB down across +-4 kHz (transmitter noise or the HackRF's
  limits near a strong signal; not separated). PSK Reporter: decoded by
  two stations about 1 500 km away at -19 and -20 dB, at 7.074310 and
  7.074321 MHz. piHPSDR's own transmit chain is the right way round for
  this stream (a TUNE carrier measured 984 Hz above the dial, image
  -77 dBc). piHPSDR's HL2 drive needs its per-band PA calibration
  lowered (about 45 to 50) on this RadioBerry; see the HL2 drive item.
- 2026-11-08: Hardware validation, transmit into a 50 ohm dummy load with
  the PA enabled, output measured by the owner on an inline meter (50 W
  scale), on the owner's go-ahead for each step: a 1 kHz tone at half of
  full scale, 10 s per step, `radioberryd --allow-tx --allow-pa
  --drive-ceiling N`. Drive 64 (TX gain code 10 of 39): no reading;
  drive 128 (code 20): 0.9 W; drive 170 (code 26): 3.2 W, 5.5 dB above
  code 20 over 6 codes. The gateware maps drive to the AD9866 TX gain
  code as ((drive + 4) >> 3) + (drive >> 5). Each step: PTT and PA enable
  on and off together for 10.0 s, about 48 000 samples per second to the
  gateware, no blocks dropped, no faults. The setup's maximum is about
  4.8 W; drive 170 is the owner's earlier distortion-free limit.
- 2026-11-08: Hardware validation, transmit at zero drive into a 50 ohm
  dummy load, PA disabled, on the owner's go-ahead: `radioberryd
  --allow-tx --drive-ceiling 0 --tx-watchdog-s 15` with a Protocol 1
  test client sending a 1 kHz tone at 10% of full scale. The first run
  dropped MOX after 60 ms on 23 driver TX underruns: the driver's ring
  empties for moments while the gateware fills its own FIFO at PIO speed
  (78 527 samples per second over that 60 ms), so driver underruns no
  longer drop MOX (they are counted and summarised per transmission;
  the gateware's PTT hang-time watchdog, the TX watchdog, the client
  timeout and the other stream faults still end a transmission). Then:
  PTT held for 10.0 s and 5.0 s exactly as commanded, 47 972 samples
  per second to the gateware, no TX blocks dropped, receive running
  throughout; the tone in the duplex receiver at -35 dBFS during MOX
  against -146 to -159 dBFS before and after, on the side it was sent,
  the other side 77 to 80 dB below it. `radioberryd` now logs each
  transmission's length, samples, rate, underruns and drops.
- 2026-11-08: Frequency calibration: `radioberryd --frequency-ppm P`
  divides every RX and TX frequency command (0x01, 0x02 to 0x08, 0x12 to
  0x16) by 1 + P/10^6, P being the radio's error, positive if it tunes
  high. The development board measured +22.9 ppm (received frequencies
  162 Hz low at 7.075 MHz against PSK Reporter). Acceptance: with the
  correction, `ft8rx` decodes within 2 Hz of the PSK Reporter median on
  40 m (0 to +1 Hz, three stations) and 30 m (-2 to 0 Hz, three
  stations); `docs/examples.md`.
- 2026-11-08: Protocol 1 transmit, proved against the fake device (on-air
  steps are section 6). The owning client's MOX (the second command
  block's) goes through `TxPolicy` and the session: PTT and the TX
  permission bit only while granted; 0x09 drive capped at the ceiling,
  VNA cleared, PA enable only with MOX and `--allow-pa`; CWX from 0x0f
  bit 24 only with MOX (stricter than the HL2); MOX dropped and latched
  on a host sequence gap, TX queue full, TX underrun, invalid TX block,
  RX queue full or device error, and on the watchdog. The client's TX
  I/Q goes to the driver, the driver's TX ring kept within 100 ms
  (blocks beyond it dropped). Address-0 C3 stays zero, as this HL2 build
  sends it. `radioberryd --allow-tx --allow-pa --drive-ceiling N
  --tx-watchdog-s N`; defaults receive only, ceiling 0, watchdog 120 s.
  Tests: the policy, the server and the whole daemon over loopback (MOX
  keys PTT, drive capped, samples delivered intact, MOX released and
  stop leave nothing that can transmit, a device underrun drops and
  latches MOX); release, ASan and UBSan builds pass.
- 2026-11-08: Example FT8 client `ft8rx` (`examples/ft8`, built with
  `-DRADIOBERRY_BUILD_EXAMPLES=ON`, off by default; the `default` and
  `asan` presets turn it on so the gate builds and tests it). Receive-only
  Protocol 1: one receiver at 48 ksps, decimated to 3200 sps with
  liquid-dsp (unity-gain Kaiser low-pass at 1600 Hz, 1501 taps), decoded
  per UTC slot with a C++ port of ft8_lib (GPL-2.0-or-later; ft8_lib's message code vendored with
  its MIT licence). `ft8_tests`: decimator passband and stopband, and
  synthesised FT8 at 48 ksps in noise decoded through the same chain (one
  message at -10 dB SNR with frequency, DT and SNR checked; three across
  the band). Live on the development Pi, 40 m: 23 messages in 6 slots, no
  packets lost. FT8 decodes only with each sample's second value as I
  (7 messages against none, consecutive slots); `docs/examples.md`.
- 2026-11-08: PA and transmit policy (`daemon/src/tx_policy.cpp`). MOX is
  granted only when transmit is allowed, a client owns the radio, no
  fault is latched and the TX watchdog (default 120 s) has not tripped;
  while granted PTT and the TX permission bit go out in every command,
  otherwise both are clear. Address 0x09 carries the client's drive
  capped at the ceiling, VNA always clear, and PA enable only while MOX
  is granted and the PA is allowed, so envpa is driven only while a
  client holds MOX; CWX only with MOX; PTT hang time 31 refused by the
  device layer. Defaults: transmit off, PA off, drive ceiling 0. Every
  PTT and PA change logged; no interlock claimed. Granting MOX runs
  under the session's lock, the lock its faults make the gateware safe
  under, so a fault cannot fall between a grant and the PTT it sets.
  Unit tests for every fail-safe path, 100% line coverage. Not yet
  wired to a protocol: that is Protocol 1 transmit.
- 2026-11-08: Session (`daemon/src/session.cpp`): one owner across
  protocols; the first client to claim the radio owns it until it
  releases it or is silent for the client timeout (3 s), others are
  refused meanwhile, and losing the owner puts the gateware in the safe
  state. A stream fault while the owner holds MOX (host sequence gap, TX
  queue full, TX underrun, invalid TX block, RX queue full, device
  error) makes the gateware safe and latches until the client sends MOX
  clear; without MOX it is counted only. Protocol 2 discovery is
  recognised and not answered, so clients settle on Protocol 1. The
  Protocol 1 server now takes ownership from the session. Unit tests for
  every path; the daemon's loopback test checks Protocol 2 discovery
  goes unanswered.
- 2026-11-08: Hardware validation, receive via Protocol 1: the owner saw
  40 m signals off air in piHPSDR's waterfall on the development Pi
  through `radioberryd`, and WSJT-X decoded 40 m FT8 from piHPSDR's
  audio (the item named 20 m; band conditions that night favoured 40 m).
- 2026-11-08: Protocol 1 receive, Hermes-Lite 2 dialect (`daemon/src/p1/`,
  `radioberryd`, installed to sbin; transmit is still open). Discovery
  with the HL2's 60-byte reply, start and stop, 1032-byte frames for the
  client's receiver count with receivers beyond the gateware or the RX
  capacity at the rate sent as zero, the HL2 response words with ADC
  measurements reported as absent, ACK echoes, commands forwarded on
  change, UDP and TCP (piHPSDR's 1032-byte records). The first client to
  start owns the radio until it stops or is silent for 3 s, then the
  gateware is put in the safe state. Receive only: MOX ignored, 0x09
  sent with drive, VNA and PA enable cleared. Measured on the
  development Pi: expected packet rates with no sequence gaps over
  loopback up to six receivers at 192 ksps and three at 384 ksps;
  piHPSDR (headless) discovered, autostarted, tuned and streamed with
  no sequence errors; over Wi-Fi to the workstation 0.3% of UDP packets
  were lost after leaving the Pi. Acceptance by the owner on the Pi's
  desktop: piHPSDR received 40 m off air, and WSJT-X (audio through a
  PipeWire loopback, CAT through piHPSDR's TS-2000 server) decoded 40 m
  FT8, including a CQ checked against its callsign and grid. SparkSDR
  and Quisk are a follow-up check in section 6.
- 2026-11-08: Daemon device layer (`daemon/src/device/`, library
  `radioberryd_device`). `Control` holds the gateware's command registers
  and sends each changed one once, latest value; PTT, run, CWX and the TX
  permission bit are stamped onto every command from one transmit state,
  so a protocol command cannot carry them, and a PTT hang time of 31 is
  refused; `safe()` clears the transmit state and sends 0x09 with drive,
  VNA and PA enable cleared ahead of everything else. An RX thread sends
  the commands and reads frames into a lock-free single-producer
  single-consumer ring, so every block knows its receiver layout; a TX
  thread writes TX blocks from a second ring, finishing partial writes.
  Driver statistics are polled every 100 ms and on an RX overrun, and
  faults (resync, dropped words, overrun, full RX queue, stall after
  250 ms, TX underrun, invalid TX block, device error) go to one sink.
  The `Device` interface gained `wait()` (poll) and
  `open_device_nonblocking()`. Unit tests with a fake device: 100% line
  coverage of the layer except `monitor.cpp` (98.4%); release, ASan and
  UBSan builds pass. On the development Pi, `rbdrivertest` runs the layer
  against the driver for one second at 48 ksps with zero TX samples and
  PTT clear: between 47 000 and 49 000 frames, no sequence gaps, no
  faults; 10 consecutive harness passes, `rx_stalls` 0.
- 2026-11-08: Driver test harness `tools/driver-tests/` (`rbdrivertest`,
  Catch2) against `/dev/radioberry`: exclusive open, info against sysfs,
  whole-frame reads for every receiver count with no resyncs or dropped
  words, non-blocking reads to EAGAIN, `poll()` for frames, TX space and
  RX overrun, short read and partial-sample write errors, unknown ioctls,
  safe command on close (reply shows `cw_on` clear), TX ring fill and
  drain at 48 ksps with PTT clear, and no RX stall during the run (sysfs
  `rx_stalls` unchanged). Receive only: PTT and the TX permission bit are
  never set. Acceptance on the development Pi with driver 0.4.0: 20
  consecutive passes, `rx_stalls` 0 before and after, no "RX stream
  stalled" in the kernel log. `tools/ci-pi.sh` runs it when the device is
  present and free.
- 2026-11-08: Driver 0.4.0: the PIO gateware's RX lock-up resolved by
  a driver workaround (`docs/fault-rx-lockup.md`). Diagnosed from the Pi
  alone: the fault is an asynchronous crossing at the RX FIFO clear that
  every receiver-count change asserts, against the Pi-driven read clock;
  no command recovers it, TX keeps running, the rate moves with fitter
  placement (seven seeds, 0 to 8 lock-ups per 150 harness runs) and the
  state machine is one-hot without safe-state recovery. The driver now
  stops its RX PIO state machine from just before a receiver-count
  command until `rx_clock_hold_us` (200) after it, and a stall detector
  logs "RX stream stalled" and exposes sysfs `rx_stalled` and
  `rx_stalls`; `debug` gained level 2 (trace every command). Measured:
  0 lock-ups in 4500 changes with the hold against 17 in 1800 without,
  no change in stream counters. Acceptance on the development Pi with
  the installed 0.4.0 and its defaults: 100 harness runs (600 changes),
  0 stalls, 0 harness failures. The gateware source fix is an
  owner-gated item in `TODO/ACTIVE.md` section 5. Design note added to
  `docs/plan-v1.md` 4.5 and 9.
- 2026-11-08: Driver 0.3.0: words in flight across a receiver-count
  change are discarded, not counted as resyncs. After a change the RX
  parser settles until two consecutive clean frames of the new layout
  (old-layout words cannot form two: shown for every pair of counts 1 to
  6 in the unit tests), withholding frames and counting what it throws
  away in the new `rx_discarded_words` statistic, which takes two of the
  reserved words (ABI 1 size and offsets unchanged, checked by a test).
  `rbcat` reports it for the measurement and the settling time, `rbctl`
  in its statistics. Acceptance on the development Pi: `rbcat` through
  1, 6, 1, 6, 3 (384 ksps), 1, 6, 2, 5, 1 receivers showed zero resyncs
  and zero dropped words while settling and measuring, with 77 to 1113
  words discarded per change.
- 2026-11-08: Driver 0.2.0: sysfs `reload` and the `debug` parameter.
  Writing 1 to `reload` (refused with EBUSY while the device is open,
  which it also keeps closed meanwhile) stops the streams, sends the safe
  command, reloads the gateware, re-reads the identity, resets parser,
  rings and statistics and restarts the PIO streams; on failure `open()`
  returns ENODEV. Stream setup is now one routine shared by probe and
  reload, which also resets stream state on stop. `debug` (0644) logs
  rate-limited commands and replies, RX resyncs and dropped words per
  period, RX overruns and TX underruns. Acceptance on the development
  Pi: three reloads in a row, about 5.5 s each, same PIO program offsets
  each time; `rbcat` then streamed 10 s with zero resyncs; EBUSY while
  `rbcat` ran, EINVAL for a non-boolean write.
- 2026-11-08: Userspace against the installed ABI header.
  `RADIOBERRY_ABI_INCLUDE_DIR` points the `radioberry_abi` target at an
  installed `radioberry_ioctl.h`; configuration fails on an ABI version
  mismatch and warns if the text differs. The driver's unit tests keep
  the source header. `tools/ci-pi.sh` builds `rbprobe`, `rbcat` and
  `rbctl` against `/usr/local/include` when the header is installed and
  fails if any userspace compile command names `driver/include`.
  Acceptance on the development Pi: 19 userspace compiles, none with the
  source-tree include (32 of 34 in the default build).
- 2026-11-08: `rbctl` in `tools/rbctl/`: sends one gateware command
  (address and data, flags `--ptt`, `--tx-permit`, `--cwx`, `--no-run`)
  and prints the command bytes, decoded reply, driver info and
  statistics as text, JSON or YAML; info and statistics alone without a
  command. `transmit_hazards()` and `disables_tx_watchdog()` in
  `tools/common/` name what can key the transmitter or arm the PA (PTT,
  TX permission, CWX, and at 0x09 drive, VNA, PA enable) and PTT hang
  time 31; `rbctl` refuses hazards without `--allow-tx` and hang time 31
  always, and `rbcat` refuses both. On the development Pi: info and
  statistics, an LNA command with its reply, PTT and hang-time refusals
  before the device is opened, EBUSY while `rbcat` runs. `--allow-tx` not
  exercised on hardware.
- 2026-11-08: `rbcat --tx-zeros`: writes zero TX samples, topping the
  driver's TX ring up to 2400 samples (50 ms) from the read loop, and
  reports TX words, word rate, underruns and the lowest ring level; the
  verdict requires the rate within 1% of 48000 per second, no underruns
  and a ring that never ran empty. Commands are unchanged (receive only),
  and the report records the driver's PTT flag and any reply showing
  cw_on or external PTT. Acceptance on the development Pi, 60 s: TX
  words 480 000 per 10 s interval, zero underruns, ring never below 2144
  samples, PTT and TX permission clear in all 3 commands, driver PTT flag
  clear; RX clean throughout.
- 2026-11-08: `rbcat` in `tools/rbcat/`: sets rate, receiver count,
  frequencies (duplex set, so receiver 1 follows its own frequency) and
  LNA gain with PTT and the TX permission bit always clear (any command
  that could transmit is refused), streams RX frames to stdout as iq24 or
  iq16 or discards them, and reports frame rate, RMS and peak level per
  receiver and the driver's counters over the measurement and the
  settling time, as text, JSON or YAML; exit 0 only for a clean stream.
  Logic tested against a fake device and clock. Shared by the tools in
  `tools/common/`: the gateware command encoding and Hermes-Lite 2
  commands (`command.hpp`), the `Device` interface over `/dev/radioberry`
  (`device.hpp`), and the report tree and renderers moved from `rbprobe`
  (`node.hpp`, `render.hpp`). Acceptance on the development Pi: 600 s at
  48 ksps, 28 800 576 frames at 48000.9 frames/s, zero resyncs, dropped
  words and overruns after settling and during it. Also clean: six
  receivers at 192 ksps and three at 384 ksps.
- 2026-11-08: Kernel driver, overlay and ABI header. `driver/kernel/`
  (`radioberry.c`; pure logic in `rb_logic.h` and `rb_pio.h`, unit tested
  in `driver/tests/`, PIO register words checked against upstream's),
  `driver/radioberry.dts`, `driver/include/radioberry_ioctl.h` (ABI 1:
  info, command, statistics), sysfs identity on `spi0.0`. SPI driver on
  the stock 6.18 `rp1-pio` API with bounce-buffered DMA kept four periods
  deep by a FIFO-priority kernel thread; safe command on close; nothing
  queued while closed. On the development Pi: loads at boot, gateware
  75.3, 10CL025, 6 receivers, both PIO state machines claimed, no
  warnings; receive streaming exact in rate with zero resyncs over 185
  million words, up to 2.37 million values per second (6 receivers at
  192 ksps, 3 at 384 ksps), the PIO bus ceiling. `rbprobe` reports the
  driver's identity and recognises this project's overlay. Upstream's init
  script and service moved out of the system paths on the Pi.
- 2026-11-08: Gateware built from source. `gateware/source/` (Hermes-Lite 2
  3e467fd, 75.3: RTL, board files, both PIO variants),
  `tools/build-gateware.sh` with `tools/gateware/` (Dockerfile, compile
  and timing-report Tcl), `gateware/QUARTUS.md`, `gateware/PROVENANCE.md`.
  CL025 built with seed 4, timing met, byte-reproducible, verified on the
  development board. CL016 built with seed 5 and shipped with a documented
  AD9866 TX output timing violation; untested. Upstream's free rpi-5
  images are 73.3 GPIO-mode gateware and are not used; `rbprobe`
  classifies them. README links the upstream ready-to-run image.
- 2026-11-08: Gateware interface notes, `gateware/README.md`: SPI command
  and reply words, RX nibble and metadata format, TX bit format and
  handshake, FIFO depths, CN301 output logic, jack inputs, post-load reset,
  PTT hang-time watchdog, PIO bus capacity.
- 2026-11-08: Repository and build skeleton: top-level CMake with
  `driver/`, `daemon/`, `tools/`, `gateware/`, `FPGATYPE`, kbuild wrapper
  `cmake/KernelModule.cmake`, `CMakePresets.json` (default, asan,
  coverage), lcov `coverage` target, `.editorconfig`, `.clang-format`,
  `tools/check-format.sh`, `tools/check-spdx.sh`, and the merge gate
  `tools/ci-pi.sh` (local or `--remote`). Acceptance: exits 0 on a fresh
  clone on the development Pi.
- 2026-11-08: `rbprobe` in `tools/rbprobe/` with the shared `tools/common/`
  library (SHA-256, gateware image catalogue, passive serial FPGA loader
  over libgpiod 2). Probes behind a `Source` interface, tested against an
  in-memory machine; text, JSON and YAML from one document tree;
  `--identify` finds the FPGA type by configuration. Acceptance on the
  development Pi before the driver existed: 10CL025, wrong installed
  image, no I2C devices, driver absent, exit 1.
- 2026-11-08: v1 design in `docs/plan-v1.md`; standing decisions in
  `TODO/ACTIVE.md`; working conventions in `CONTRIBUTING.md`.
- 2026-11-08: Development Pi surveyed and prepared (`docs/platform.md`):
  FPGA identified as 10CL025, I2C enabled (bus empty), build dependencies
  installed. The stock 6.18 `rp1-pio` lacks only `rp1_pio_sm_buffer_virt`
  of what the upstream driver uses.
- 2026-11-08: Amplifier board identified and documented in `docs/pa.md`:
  AURSINC RadioBerry Preamp Board V2 (PA5W V2), manual bias (already set),
  INA199A1 current sense, no I2C devices.
- 2026-11-08: Upstream projects Radioberry-2.x and radioberry-p2 surveyed in
  `docs/history.md`. Repository created with GPL-2.0-or-later.
