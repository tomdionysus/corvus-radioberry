# Design

```
+-------------------+      +------------------------+
| SDR client        | UDP  | radioberryd (C++20)    |
| piHPSDR, WSJT-X   |<---->|  Protocol 1, 2         |
| via piHPSDR, ...  | TCP  |  session, tx policy    |
+-------------------+      |  device layer          |
                           +-----------|------------+
                                       | /dev/radioberry: read, write, ioctl, poll
                           +-----------|------------+
                           | radioberry.ko (C)      |
                           |  gateware load, control|
                           |  rx parser, rings      |
                           |  I2S (Pi 4), PIO (Pi 5)|
                           +-----------|------------+
                                       | spi0 (control), PCM block or RP1 PIO (samples)
                           +-----------|------------------------+
                           | gateware: rpi4-i2s or rpi5-pio     |
                           |  Hermes-Lite 2 radio core          |
                           | AD9866, CN301 -> PA5W V2           |
                           +------------------------------------+

rbprobe reads every layer above without needing the daemon.
```

Each component has its own README and design notes:
[driver](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/README.md),
[daemon](https://github.com/tomdionysus/corvus-radioberry/blob/main/daemon/README.md),
[gateware](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/README.md).
C++20 everywhere except the kernel module and the ioctl ABI header, which
are C; the gateware is SystemVerilog around the Hermes-Lite 2 core's
Verilog. CMake throughout. Dependencies are Debian trixie packages:
Catch2 v3, yaml-cpp, nlohmann/json, spdlog, liquid-dsp, libgpiod, libi2c,
libsystemd.

## Fixed constraints

- The board: an AD9866 converter and a Cyclone 10 LP 10CL025 FPGA, wired
  to the Pi's GPIO header. The wiring sets which Pi pins each link can use
  ([hardware discovery](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/docs/hardware-discovery.md)).
- FPGA configuration: passive serial over GPIO 13, 24 and 27, with 22 and
  26 read back. Volatile; loaded on every driver probe.
- PA board: manual bias, nothing on I2C, no interlock. PTT, key and paddle
  inputs go to the FPGA, not to the Pi ([PA5W V2](pa.md)).
- Protocol 1 clients expect 48, 96, 192 or 384 ksps receive; Protocol 2
  clients send transmit I/Q at 192 ksps. The radio core takes 48 ksps.

## Testing

- Catch2 unit tests for every component, with complete line coverage
  required (gcov and lcov); ASan and UBSan builds run the same tests.
- The kernel module's glue is exercised by a harness against
  `/dev/radioberry` on the Pi
  ([driver tests](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/docs/driver-tests.md)).
- The gateware has simulation testbenches and is timed by Quartus.
- [tools/ci-pi.sh](https://github.com/tomdionysus/corvus-radioberry/blob/main/tools/ci-pi.sh) is the merge gate
  ([CONTRIBUTING.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/CONTRIBUTING.md)).
