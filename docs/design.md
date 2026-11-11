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
                           |  rx parser, frame ring |
                           |  tx ring, transport    |
                           |  spi control, stats    |
                           +-----------|------------+
                                       | GPIO registers (Pi 4) or rp1-pio (Pi 5), spi0, gpiod
                           +-----------|------------------------+
                           | RadioBerry 2.x gateware (HL2 75.3) |
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
  lines to the PA. Details: [gateware/README.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/README.md).
- FPGA configuration: passive serial over GPIO 13, 24 and 27, with 22 and
  26 read back. Volatile; reloaded on every driver probe.
- Host: each board's limits are in Transports, below.
- PA board: manual bias, nothing on I2C, no interlock. PTT, key and paddle
  inputs go to the FPGA, not to the Pi ([PA5W V2](pa.md)).
- Protocol 1 clients expect 48, 96, 192 or 384 ksps receive; Protocol 2
  clients send transmit I/Q at 192 ksps. The gateware takes 48 ksps.

## Driver

- SPI driver bound to the overlay's `sdr,radioberry` device. The five
  configuration lines are gpiod descriptors from the same node.
- `probe()`: claim GPIO, load the gateware with `request_firmware`, verify
  CONF_DONE and nSTATUS, poll the identity over SPI until the gateware
  leaves reset, then start the board's transport (chosen from the SoC;
  Transports, below). Any failure unwinds completely.
- RX: the transport delivers `index << 24 | value` words. The parser
  checks the metadata nibble sequence, resynchronises on a miss and counts
  resyncs, dropped words and overruns. Whole frames go into a ring; an
  overrun drops whole frames and sets a flag. After a receiver-count
  change, old-layout words are discarded until two clean frames of the
  new layout arrive.
- TX: the transport drains a ring of samples. When the ring is empty the
  driver sends nothing and counts an underrun if PTT is set; it never
  invents samples, so the gateware's PTT hang-time watchdog ends transmit
  when samples stop.
- Control: one versioned ioctl carries the six command bytes out and the
  six reply bytes in, serialised by a mutex. Identity and statistics are
  separate read-only ioctls. A receiver-count change is sent with the RX
  clock held still ([receive lock-up](fault-rx-lockup.md)).
- Pure logic (word parsing, resync, frame assembly, TX packing,
  statistics) lives in headers with no kernel dependencies and is compiled
  into a userspace test binary.

## Transports

The `rpi-v8` kernel boots on both boards and DKMS builds one module per
kernel, so the module carries both transports and chooses at probe from
the SoC compatible (`brcm,bcm2711` or `brcm,bcm2712`). Both feed the same
RX parser and drain the same TX ring.

### Raspberry Pi 4: CPU-clocked

- A SCHED_FIFO kernel thread clocks both buses through the GPIO set,
  clear and level registers
  ([driver/kernel/rb_bitbang.h](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/kernel/rb_bitbang.h)).
- The stream GPIOs come from the overlay (`rx-clk-gpios` and the rest,
  inputs with pull-downs) through gpiolib; the driver maps the registers
  of their GPIO block, which must be `brcm,bcm2711-gpio`.
- The thread clocks up to 256 RX words, then the pending TX words, then
  sleeps 0.5 to 1 ms when the gateware had nothing.
- RX clock hold: a mutex the thread holds while clocking.
- The RX bus (`rx_pi_pio.v`) advances on each rising clock edge; with more
  than 12 values in its FIFO it raises sample-ready for one clock, then
  gives the index nibble and six data nibbles. Pausing the clock pauses
  the bus; the FIFO (1012 values) absorbs about 10 ms at one receiver at
  48 ksps.
- Not implemented, if more receivers or less CPU are wanted: SMI
  programmed reads with DMA for RX (read strobe GPIO 6, SD10 to SD13 on
  GPIO 18 to 21, SD17 on GPIO 25), which needs a Pi 4-only overlay because
  any fragment naming the SMI block fails to merge onto the Pi 5 tree; or
  DMA writing the GPIO set and clear registers paced by PWM or PCM, which
  cannot react to TX ready.

### Raspberry Pi 5: RP1 PIO

- Two PIO state machines with DMA; the stream GPIOs belong to the RP1 PIO.
- RX: `rp1_pio_sm_xfer_data()` with four periods in flight, refilled by a
  SCHED_FIFO kernel thread (the completion callback runs in softirq and
  the call allocates). A period is one millisecond of samples at the
  current rate and receiver count.
- TX: bounce-buffered DMA in periods of 256 samples, fed by the same
  thread.
- RX clock hold: the RX state machine is stopped.
- Capacity: with PIO clock divider 2 (100 MHz from the 200 MHz RP1 clock,
  five PIO cycles per nibble) one value takes 42 PIO cycles, at most about
  2.38 million values per second.

`rp1-pio` facts the driver depends on:

- The RP1 has one PIO block of four state machines. Kernel clients get
  bounce-buffered DMA, at most four buffers per direction; cyclic DMA is
  only reachable through the character device.
- `rp1_pio` is an in-tree module, loaded at boot, instance `pio0`. Its
  kernel API is `include/linux/pio_rp1.h`, with argument structs in
  `include/uapi/misc/rp1_pio_if.h`.
- `rp1_pio_sm_xfer_data()` with `dma_addr` 0 never blocks, returns
  `-EBUSY` when all buffers are in flight, allocates with `GFP_KERNEL`
  (process context only), copies FROM_SM data into the caller's buffer
  and calls the caller's callback from the DMA completion (softirq).
- `rp1_pio_add_program()` takes programs written for offset 0; the RP1
  firmware relocates jump targets to the load offset.
- `rp1_pio_close()` disables and unclaims the client's state machines and
  frees their DMA.
- `rp1_pio_open()` returns `-EPROBE_DEFER` until an RP1 PIO device has
  probed, which never happens on a Pi 4, so the transport is chosen before
  it is called.

## Daemon

- Device layer behind an interface, so protocol and policy tests run
  against a fake device. Two device threads (`rb-rx-device`,
  `rb-tx-device`) move data between the driver and rings; a sender thread
  hands RX blocks to both protocol servers; the main thread serves
  Protocol 1 and runs the session and policy timers; a separate thread
  serves the Protocol 2 sockets. Real-time priorities are in
  [radioberryd](daemon.md#real-time).
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
  jack drives the DAC regardless of software
  ([gateware/README.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/README.md)).

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
- [tools/ci-pi.sh](https://github.com/tomdionysus/corvus-radioberry/blob/main/tools/ci-pi.sh) is the merge gate
  ([CONTRIBUTING.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/CONTRIBUTING.md)).
