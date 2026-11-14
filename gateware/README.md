# Gateware

The FPGA images for the RadioBerry 2.x: one for each Raspberry Pi, for
the Cyclone 10 LP 10CL025. The driver loads the image for its board at
boot; the Pi never compiles gateware. The images here are built from this
directory with Quartus Prime Lite in Docker and committed.

| Image | Board | Link to the Pi | File |
|---|---|---|---|
| `rpi5-pio` | Raspberry Pi 5 | RP1 PIO: a 4-bit receive bus and a 1-bit transmit bus, each clocked by the Pi | `images/rpi5-pio/CL025/radioberry-rpi5-pio.rbf` |
| `rpi4-i2s` | Raspberry Pi 4 | I2S into the BCM2711 PCM block, the FPGA as clock master | `images/rpi4-i2s/CL025/radioberry-rpi4-i2s.rbf` |
| `linktest` | Raspberry Pi 4 | a test image for measuring the board and the I2S link; not a radio | built only |

Both radio images control the board through the same framed SPI protocol
(8-byte frames with a CRC) and the same registers, and receive with six
receivers and transmit with one.

## Source

- `rtl/`: this project's own RTL, SystemVerilog, GPL-2.0-or-later:
  control registers and the SPI protocol, AD9866 start-up, keying and PA
  lines, and the links to the Pi.
- `source/`: the Hermes-Lite 2 radio core (receivers, decimation, NCOs,
  transmit DSP), a `git archive` of the Hermes-Lite 2 repository used
  unmodified: [docs/provenance.md](docs/provenance.md) has the commit,
  hashes, licences and the fitter seed of each image.

## Layout

| Directory | Contents |
|---|---|
| `rtl/common/` | shared modules: SPI protocol, CRC, AD9866 sequencer, dual-clock FIFO, I2S link |
| `rtl/rpi4/`, `rtl/rpi5/` | each image's top level and its link to the Pi |
| `rtl/linktest/` | the link test image |
| `projects/` | Quartus project (`.qsf`) and timing constraints (`.sdc`) per image |
| `sim/` | testbenches, the radio-core stand-in and a model of the RP1 PIO running the driver's programs |
| `images/` | the committed bitfiles |
| `source/` | the Hermes-Lite 2 source archive |
| `tools/` | build, simulation and timing scripts, and the two Dockerfiles |
| `host/` | Pi-side programs for the link test image |
| `docs/` | provenance, toolchain, interfaces, design and test records |

## Building

You need an x86-64 machine with Docker (Linux, or an Intel Mac with
Docker Desktop): Quartus does not run on ARM. The simulator runs anywhere
Docker does.

1. **Quartus.** Download Quartus Prime Lite 23.1std.1 for Linux from the
   Altera download centre, as two files:
   `QuartusLiteSetup-23.1std.1.993-linux.run` and
   `cyclone10lp-23.1std.1.993.qdz`. Read the licence agreement on the
   download page. Then, from this directory:

   ```
   tools/quartus-image.sh --installers ~/Downloads --accept-quartus-eula
   ```

   This builds the Docker image `radioberry-quartus:23.1std.1` once (about
   10 GB). The flag states that you accept the Quartus licence; nothing in
   this repository accepts it for you. Details and a build without Docker:
   [docs/toolchain.md](docs/toolchain.md).

2. **Simulate.** Each testbench prints `PASS`:

   ```
   tools/sim.sh rpi5
   tools/sim.sh rpi4
   tools/sim.sh rpi4tx
   ```

3. **Compile.** A few minutes each:

   ```
   tools/build.sh rpi5-pio
   tools/build.sh rpi4-i2s
   ```

   The bitfile goes to `images/<image>/CL025/` with its size and SHA-256
   printed; Quartus's reports go to `build/<image>/`.

4. **Check timing.** Every clock must show positive setup and hold slack:

   ```
   tools/worst.sh rpi5-pio
   ```

   The `strict` lines give the AD9866 transmit bus without its one
   relaxed constraint; TX bit 4 (FPGA pin 105) is negative there in both
   images, a known risk ([docs/review.md](docs/review.md), finding 1.3).
   If a clock fails, try another fitter seed (`SEED=2 tools/build.sh
   rpi5-pio`) and record the one used in the project file.

5. **Commit** the new bitfile together with its row in
   [docs/provenance.md](docs/provenance.md).

To install a new image on a Pi without rebuilding the packages, copy it
over `/usr/lib/firmware/radioberry-<image>.rbf` and write 1 to
`/sys/bus/spi/devices/spi0.0/reload` with `radioberry.service` stopped.

## Documents

| Document | Contents |
|---|---|
| [docs/provenance.md](docs/provenance.md) | source archive, image hashes and seeds, licences |
| [docs/toolchain.md](docs/toolchain.md) | Quartus Prime Lite: what to download, licence, Docker and native installs |
| [docs/interfaces.md](docs/interfaces.md) | pins, the control protocol and registers, the PIO and I2S links |
| [docs/hardware-discovery.md](docs/hardware-discovery.md) | what was measured about the board itself |
| [docs/plan.md](docs/plan.md) | the rewrite plan |
| [docs/evaluation.md](docs/evaluation.md) | the evaluation of the Hermes-Lite 2 based gateware that led to the rewrite |
| [docs/interface-strategies-rpi4.md](docs/interface-strategies-rpi4.md), [docs/interface-strategies-rpi5.md](docs/interface-strategies-rpi5.md) | the link options considered for each Pi |
| [docs/linktest-results.md](docs/linktest-results.md) | the I2S link measured with the link test image |
| [docs/rpi5-results.md](docs/rpi5-results.md) | the Pi 5 image: simulation, timing, receive and transmit results |
| [docs/review.md](docs/review.md) | an external review: findings and progress |
