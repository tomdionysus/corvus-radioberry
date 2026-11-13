# Changelog

Changes per released version, newest first. Versions are git tags; the
packages carry the same number.

## Unreleased

- Raspberry Pi 4: RX read by SMI with DMA with the new `radioberry-smi`
  overlay; receive costs about 1% of the CPU instead of a whole core,
  and up to 1 000 000 RX values per second (one receiver at 384 ksps).
  Falls back to the CPU-clocked bus without the overlay.
- ABI: `RADIOBERRY_TRANSPORT_SMI`; `rbctl` and `rbprobe` report the SMI
  transport, and `rbprobe` advises the overlay on a Pi 4.
- `rbsmi` and `rbsmi_dma`: SMI measurement tools (development).
- CI: ThreadSanitizer build and tests.
- `wsprrx`: example WSPR receive client, the sister of `ft8rx`; also
  decodes `.c2` files and saves received slots as them.
- Docs: frequency calibration procedure; the Raspberry Pi 4 receive CPU
  finding; Code of Conduct.

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
