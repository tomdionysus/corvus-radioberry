# Design

## Components

```
+-------------------+      +------------------------+
| SDR client        | UDP  | radioberryd (C++20)    |
| piHPSDR, WSJT-X   |<---->|  protocol 1, 2         |
| via piHPSDR, ...  | TCP  |  session, tx policy    |
+-------------------+      |  dsp (liquid-dsp)      |
                           |  device layer          |
                           +-----------|------------+
                                       | /dev/radioberry: read, write, ioctl, poll
                           +-----------|------------+
                           | radioberry.ko (C)      |
                           |  probe, gateware load  |
                           |  rx DMA, frame ring    |
                           |  tx ring, DMA          |
                           |  spi control, stats    |
                           +-----------|------------+
                                       | rp1-pio, spi0, gpiod
                           +-----------|------------------------+
                           | Radioberry 2.x gateware (HL2 75.3) |
                           | AD9866, CN301 -> PA5W V2           |
                           +------------------------------------+

rbprobe reads every layer above without needing the daemon.
```

Directories: `driver/`, `daemon/`, `tools/`, `gateware/`, `examples/`.
C++20 everywhere except the kernel module and the ioctl ABI header, which
are C. CMake throughout, with a custom target that invokes kbuild for the
module. Dependencies are Debian trixie packages: Catch2 v3, yaml-cpp,
nlohmann/json, spdlog, liquid-dsp, libgpiod, libi2c, libsystemd.

## Fixed constraints

These come from hardware and gateware this project does not change.

- Gateware: the Hermes-Lite 2 `radioberry_pio_cl025` and
  `radioberry_pio_cl016` variants (75.3), built unmodified. They define
  the RX wire format (4-bit bus on GPIO 18 to 21, clock on GPIO 6,
  sample-ready on GPIO 25), the TX wire format (1-bit serial on GPIO 5,
  clock GPIO 4, ready GPIO 12), the six-byte SPI command on `spi0` CE0,
  the Hermes-Lite command set, the FIFO depths, and the CN301 control
  lines to the PA. Details: `gateware/README.md`.
- FPGA configuration: passive serial over GPIO 13, 24 and 27, with 22 and
  26 read back. Volatile; reloaded on every driver probe.
- Host: the RP1 has one PIO block of four state machines. Kernel clients
  of the stock `rp1-pio` get bounce-buffered DMA, at most four buffers per
  direction; cyclic DMA is only reachable through its character device.
- PA board: manual bias, nothing on I2C, no interlock. PTT, key and paddle
  inputs go to the FPGA, not to the Pi (`pa.md`).
- Protocol 1 clients expect 48, 96, 192 or 384 ksps receive; Protocol 2
  clients send transmit I/Q at 192 ksps. The gateware takes 48 ksps.

## Driver

- SPI driver bound to the overlay's `sdr,radioberry` device. The five
  configuration lines are gpiod descriptors from the same node; the stream
  pins are handed to the RP1 PIO at run time.
- `probe()`: claim GPIO, load the gateware with `request_firmware`, verify
  CONF_DONE and nSTATUS, poll the identity over SPI until the gateware
  leaves reset, then claim two PIO state machines and start their DMA. Any
  failure unwinds completely.
- RX: `rp1_pio_sm_xfer_data()` with four periods in flight, refilled by a
  SCHED_FIFO kernel thread (the completion callback runs in softirq and
  the call allocates). A period is one millisecond of samples at the
  current rate and receiver count. The period callback parses words,
  checks the metadata nibble sequence, resynchronises on a miss and counts
  resyncs, dropped words and overruns. Whole frames go into a ring; an
  overrun drops whole frames and sets a flag. After a receiver-count
  change, old-layout words are discarded until two clean frames of the
  new layout arrive.
- TX: bounce-buffered DMA in periods of 256 samples from a ring, fed by
  the same thread. When the ring is empty the driver sends nothing and
  counts an underrun if PTT is set; it never invents samples, so the
  gateware's PTT hang-time watchdog ends transmit when samples stop.
- Control: one versioned ioctl carries the six command bytes out and the
  six reply bytes in, serialised by a mutex. Identity and statistics are
  separate read-only ioctls. A receiver-count change is sent with the RX
  clock held still (`fault-rx-lockup.md`).
- Pure logic (word parsing, resync, frame assembly, TX packing,
  statistics) lives in headers with no kernel dependencies and is compiled
  into a userspace test binary.

## Daemon

- Device layer behind an interface, so protocol and policy tests run
  against a fake device. Two device threads (`rb-rx-device`,
  `rb-tx-device`) move data between the driver and rings; a sender thread
  hands RX blocks to both protocol servers; the main thread serves
  Protocol 1 and runs the session and policy timers; a separate thread
  serves the Protocol 2 sockets. Real-time priorities are in `daemon.md`.
- Protocol code sets command registers; the RX device thread sends them
  between its reads, so it always knows the frame layout it reads, and
  stamps PTT, run and the TX permission bit onto each from one transmit
  state.
- Protocol 1, Hermes-Lite 2 dialect: discovery, start and stop, 1032-byte
  frames at 48 to 384 ksps within the RX capacity, the C0 command set
  including PA enable and drive, UDP and TCP.
- Protocol 2: discovery, General, DDC and DUC specific, High Priority both
  ways, as many DDCs as the gateware has receivers, DDC I/Q at 48 to
  384 ksps, DUC I/Q at 192 ksps decimated with liquid-dsp, sequence
  checking.
- Session: one client owns the radio. Timeout or any stream fault clears
  MOX, zeroes drive, latches a fault the client clears by releasing MOX,
  and logs the cause.
- Transmit policy: transmit and PA enable only when the daemon allows
  them and the client asks; a drive ceiling and a transmit watchdog;
  every PA state change logged. No interlock is claimed. The gateware's
  levers are the TX permission bit and the PTT bit in every command, and
  its own PTT hang-time watchdog, which is never disabled. A CW key on the
  jack drives the DAC regardless of software (`gateware/README.md`).

## Platform probe

`rbprobe` reports host, boot configuration, driver and installed
gateware, FPGA identity (from sysfs, or by configuration with
`--identify` when the driver is not loaded), PA board and daemon state,
as text, JSON or YAML with identical content. It exits non-zero when the
radio cannot run. Its probes sit behind an interface so the report logic
is tested against an in-memory machine.

## Testing

- Catch2 unit tests for every component, aiming at complete coverage
  (gcov and lcov); ASan and UBSan builds run the same tests.
- The kernel module's glue is exercised by a userspace harness against
  `/dev/radioberry` on the Pi (`tools/driver-tests/`).
- `tools/ci-pi.sh` is the merge gate (`CONTRIBUTING.md`).
- Every transmit test on hardware needs the owner's go-ahead at the time.
