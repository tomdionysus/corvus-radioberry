# Changelog

Changes per released version, newest first. Versions are git tags; the
packages carry the same number.

## 0.7.0 - 2026-11-20

- Gateware: the radio core is this project's own, block by block, each
  checked against the Hermes-Lite 2 block it replaced; no Hermes-Lite 2
  or openHPSDR module is compiled into the images. They are 38 to 39%
  smaller than 0.6.1's (13 267 to 13 961 logic elements against 21 903 to
  22 607) and leave 8 of the 66 memory blocks free; every seed swept
  meets timing on both. Receive: one shared folded filter, pruned CIC
  decimators with no DC offset, and the ADC bus captured on its own clock
  about 100 ps later, which raises its worst timing margin from 0.010 to
  0.109 ns. Transmit: a CIC interpolator about half the size, a
  multiplier up-converter with dither in place of the CORDIC (spurs 5 to
  11 dB lower), a raised-cosine CW envelope (key clicks above -80 dB to
  1.7 kHz from the carrier, 5 kHz before). The image has its own
  registers; PureSignal and the gateware predistortion are gone.
- Driver: turns clients' Hermes-Lite 2 commands into the image's
  registers and computes the NCO phase words; it needs the new images
  (ID RBV3) and they need it.
- The key jack keys only with the transmit permission, so the keying
  watchdog stops it; `radioberryd`'s `jack_keying` (default off) lets it
  key while a client owns the radio, and the SoapySDR module the same
  while a program has the device open.
- Tests: the daemon and probe tests take ports no other socket holds, and
  the SoapySDR tests no longer read a loaded driver's sysfs. `ci-pi.sh`
  runs six gateware testbenches under Verilator where it is installed.

## 0.6.1 - 2026-11-18

- Driver (Raspberry Pi 5): transmit works after a gateware reload through
  sysfs `reload`. TX transfers still in flight are now completed before
  the PIO is closed; on 6.12 kernels each one closed in flight made every
  later TX transfer fail (`TX DMA submit failed: -110`) until the module
  was reloaded.
- Driver reports version 0.6.1 through the version ioctl too (0.6.0
  reported 0.5.0 there).
- Gateware images unchanged from 0.6.0.

## 0.6.0 - 2026-11-16

- SoapySDR module, `soapysdr-module-radioberry` (optional): receive on the
  gateware's receivers, transmit at 48 ksps, under `radioberryd`'s
  transmit rules. Build option `RADIOBERRY_BUILD_SOAPY`; package build
  profile `pkg.radioberry.nosoapy`.
- Packages: DKMS skips kernels older than 6.12 instead of failing the
  install, and the install names an unsupported running kernel.
- `rbprobe` reports the original project's SoapySDR module when it is
  installed.
- Gateware: the Hermes-Lite 2 core is built to the SystemVerilog standard
  through a patch applied at build time; both images are unchanged.
- Tests: `rbsoapytest`, the SoapySDR module on the radio; the OS and
  kernel test matrix with Raspberry Pi OS Bookworm results on the Pi 5.
- No Python in the repository.

## 0.5.0 - 2026-11-14

- New gateware, one image per board, both for the 10CL025: `rpi5-pio`
  (Raspberry Pi 5, RP1 PIO) and `rpi4-i2s` (Raspberry Pi 4, I2S into the
  PCM block with DMA). Both use a framed control protocol with a CRC, a
  keying register that clears itself unless renewed, read-back of board
  state, and link and transmit counters.
- Driver: loads the image for its board; on the Pi 4 it takes the PCM
  block and its pins itself, so `dtoverlay=radioberry` alone is enough on
  both boards. The receive clock hold, the CPU-clocked Pi 4 buses and the
  48-bit command path are gone. Sysfs `counters` and `tx_loopback`.
- `radioberryd`: transmit samples go to the radio only while keyed; no
  false RX stall when the first client starts after an idle period.
- Packages: no FPGA question; an upgrade installs both images and the
  plain overlay. The installed config sets `frequency_ppm: 22.9` and
  `realtime: false`.
- The 10CL016 is no longer supported.
- Removed: `rbload`, `rbbus`.
- `wsprrx`: example WSPR receive client, the sister of `ft8rx`; also
  decodes `.c2` files.
- Docs: frequency calibration procedure; Code of Conduct; README per
  component (driver, daemon, gateware); gateware review.
- Tests: 100% line coverage; all 11 Raspberry Pi OS 6.12 and 6.18
  kernels pass on both boards.

## 0.4.1 - 2026-11-11

- Driver: the Raspberry Pi 5 RP1 PIO path works on 6.12 kernels as on
  6.18.
- Packages: an upgrade loads the new driver in place of the running one.
- Docs: kernels page with the 6.12 and 6.18 differences and the tested
  kernels on both boards.

## 0.4.0 - 2026-11-11

- Raspberry Pi 4 support: one driver, overlay and package set for the
  Pi 4 and Pi 5. On the Pi 4 a kernel thread clocks the buses.
- Driver: RX moves only while run is set; `radioberryd` sets run only
  while a client streams, so an idle radio costs no CPU.
- Driver version follows the release version.
- `radioberryd`: the RX ring's consumer sleeps instead of spinning;
  Protocol 1 TX buffer latency at least 40 ms; one summary line per
  transmission.
- Packages: `i2c-dev` loaded at boot; systemd reloaded before the service
  restarts.
- `rbbus`: measures the CPU-clocked buses from userspace.

## 0.3.0 - 2026-11-10

- Debian packages `radioberry`, `radioberry-dkms` and `radioberry-dev`,
  installable from an apt repository; the driver builds through DKMS and
  the install asks for the FPGA type.

## 0.2.0 - 2026-11-09

- `radioberryd`: Protocol 2 receive and transmit; runs as
  `radioberry.service` with real-time threads; configuration in
  `/etc/radioberry/config.yaml`; every drive change logged while
  transmitting.
- Tools: `rbload`, `p1probe`, `p2probe`.
- Driver: checkpatch and sparse clean; tested under a debug kernel
  (KASAN, lockdep, kmemleak, UBSAN).
- CI: clang-tidy; GitHub Actions build, Codecov and CodeQL.
- Documentation restructured under `docs/`.

## 0.1.1 - 2026-11-09

- README: `ft8rx` is receive only.

## 0.1.0 - 2026-11-09

- Gateware: Hermes-Lite 2 RadioBerry PIO variants for the CL025 and
  CL016, built from source; the CL016 ships with a documented timing
  violation.
- Kernel driver for the Raspberry Pi 5 (RP1 PIO), overlay and ABI header;
  the gateware's RX lock-up worked around with the RX clock hold.
- `radioberryd`: Protocol 1 receive and transmit, one owning client,
  transmit and PA policy with fail-safes, frequency calibration, I/Q
  convention option.
- Tools: `rbprobe`, `rbcat`, `rbctl`; the driver hardware harness
  `rbdrivertest`; the Pi CI gate.
- `ft8rx`: example FT8 receive client.
- On air: FT8 received, transmitted and a contact completed through
  piHPSDR and WSJT-X.

## 0.0.1 - 2026-11-08

- Initial documentation, plan and licence.
