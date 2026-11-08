# Plan v1

The first release of corvus-radioberry: scope, architecture, the reasoning
behind each design choice, and the order of work. `TODO/ACTIVE.md` carries
the itemised work with acceptance tests; this document is the design they
implement. Status 2026-11-08: sections 4 (driver) and 6 (probe) are built
and describe the code as it is; section 5 (daemon) is not started.

## 1. Goal

Replace the upstream Radioberry 2.x software on the Raspberry Pi 5 with a
stack that is documented, performant and robust: a kernel driver that
builds against the stock Raspberry Pi OS kernel (6.18 and newer), a radio
daemon speaking OpenHPSDR Protocol 1 and Protocol 2, a platform probe, and
explicit control of the AURSINC PA5W V2 amplifier. The FPGA gateware is
taken as a fixed input: the upstream source, built unmodified.

The motivation is the state of the upstream stack: undocumented wire
formats and ABI, a driver that depends on a private kernel patch, high
latency and syscall rates, silent error handling, and a daemon with no
client ownership or transmit fail-safe. Section 4 lists the specific
defects the design addresses.

## 2. Fixed constraints

These are not design choices. They come from hardware we do not change.

- Gateware: the Hermes-Lite 2 `radioberry_pio_cl025` and
  `radioberry_pio_cl016` variants (version 75.3), built unmodified from
  the upstream source with Quartus Prime Lite on an x86-64 machine. They
  define
  the RX wire format (4-bit parallel bus on GPIO 18 to 21 with a clock on
  GPIO 6 and a sample-ready line on GPIO 25, each 32-bit word carrying a
  4-bit metadata nibble and 24 bits of I or Q), the TX wire format (1-bit
  serial on GPIO 5 clocked by GPIO 4 with a ready line on GPIO 12, 32-bit
  words of 16-bit I and Q), the six-byte SPI command on `spi0` CE0, the
  AD9866 at 76.8 MHz, the Hermes-Lite command set, the FIFO depths, the
  absence of hardware timestamps, and the CN301 control lines to the PA.
- FPGA configuration: passive serial over GPIO 13 (DATA), 24 (DCLK),
  27 (nCONFIG), with 22 (CONF_DONE) and 26 (nSTATUS) read back. Volatile;
  reloaded on every driver probe.
- Host: Raspberry Pi 5, RP1 with one PIO block of four state machines
  shared with any other PIO consumer, stock `rp1-pio` kernel API. Kernel
  clients get bounce-buffered DMA (at most four buffers per direction);
  cyclic DMA is reachable only through rp1-pio's own character device.
- PA board: manual bias, no telemetry on I2C, analogue current output
  only, no interlock. PTT, key and paddle inputs are wired to the
  Radioberry's FPGA, not to the Pi.
- Protocol sample rates: Protocol 1 clients expect 48, 96, 192 or
  384 ksps receive; Protocol 2 clients send transmit I/Q at 192 ksps.

## 3. Components

```
+-------------------+      +------------------------+
| SDR client        | UDP  | corvus-radioberry      |
| piHPSDR, SparkSDR |<---->| daemon (C++20)         |
| Quisk, Thetis     |      |  protocol1 | protocol2 |
+-------------------+      |  session, fail-safe    |
                           |  dsp (liquid-dsp)      |
                           |  pa policy             |
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
                           +-----------|------------+
                           | Radioberry 2.x gateware (HL2 75.3) |
                           | AD9866, CN301 -> PA5W V2           |
                           +------------------------------------+

rbprobe (C++20) reads every layer above without needing the daemon.
```

Directory per component: `driver/`, `daemon/`, `tools/`, `gateware/`.
Language: C++20 everywhere except the kernel module and the ioctl ABI
header, which are C. Build and install: CMake throughout, with a custom
target that invokes kbuild for the module. Dependencies only from Debian
trixie packages: Catch2 v3 for tests, yaml-cpp for configuration and YAML
output, nlohmann/json for JSON output, spdlog for logging, liquid-dsp for signal processing, libgpiod and
libi2c for hardware access. Names: `radioberryd`, `radioberry.service`,
`/etc/radioberry/config.yaml`, namespace `radioberry`. The daemon runs as
the `radioberry` system user with device access by udev rule and the
capabilities needed for real-time threads.

## 4. Driver design

### 4.1 Defects in the upstream driver that this replaces

- Built against `rp1_pio_sm_buffer_virt`, which exists only in a private
  rp1-pio patch; the module does not compile on the stock kernel.
- GPIO driven by `ioremap` of the RP1 register block behind the kernel's
  pinctrl, in parallel with libgpiod consumers elsewhere.
- Gateware loaded in `module_init` instead of on probe; init returns the
  platform-register status regardless of later failures; error paths free
  the context twice.
- RX: two 24 KB bounce buffers restarted from a workqueue after every
  completion. That is 64 ms of samples per buffer at 48 ksps and a restart
  gap in which the PIO FIFO can overflow. FIFO overflow is handled by
  resetting the FIFO silently; misaligned words are dropped one at a time
  with a log line each.
- `read()` ignores the requested size, returns a fixed 63-sample chunk for
  one receiver, and has no `poll()`.
- TX: DMA starts only once 16 KB (85 ms at 48 ksps) is queued, and stalls
  the PIO program when the queue empties; underruns are invisible.
- `write()` allocates with `kmalloc` per call; the upstream daemon calls it
  once per sample, 48000 times a second.
- The ioctl path has no lock, allocates per call and ignores the
  `copy_from_user` result. No statistics, no versioned ABI, printk on
  every entry point.

### 4.2 Structure

- SPI driver bound to the overlay's `sdr,radioberry` device on `spi0`
  chip select 0 (SPI mode 3, 12.5 MHz). The five configuration lines are
  gpiod descriptors from the same node; the stream pins are handed to the
  RP1 PIO at run time.
- `probe()`: claim GPIO, load gateware via `request_firmware`, verify
  CONF_DONE and nSTATUS, poll the gateware identity over SPI until it
  leaves its reset, then claim two PIO state machines and start their DMA.
  Any failure unwinds completely and fails the probe with a specific
  error. Identity is published in sysfs. A sysfs `reload` attribute will
  repeat the gateware load.
- One character device, exclusive open, with `read`, `write`, `ioctl`
  and `poll`. Closing it sends the gateware a safe command (run, PTT, TX
  permission, PA enable and drive all clear).
- Pure logic (RX word parsing and metadata resync, frame assembly, TX
  word packing, statistics) lives in headers with no kernel dependencies
  so the same code compiles into a userspace test binary.

### 4.3 RX path

- The stock `rp1-pio` gives kernel clients cyclic DMA only through its
  character device, so the driver uses the exported bounce-buffered
  `rp1_pio_sm_xfer_data()`: four periods in flight, refilled by a
  FIFO-priority kernel thread (the completion callback runs in softirq
  context and the call allocates). The period is one millisecond of
  samples at the current rate and receiver count. With periods queued
  ahead there is no restart gap.
- The period callback parses words, checks the metadata nibble sequence,
  resynchronises on a miss and increments counters (resync events, dropped
  words, overruns) instead of logging.
- Whole frames (24-bit big-endian I/Q per receiver) go into a ring.
  `read()` returns as many whole frames as fit the request, blocking or
  not, and `poll()` reports a readable frame. Ring overrun sets a flag
  readable by ioctl and `POLLERR`; data is dropped in whole frames and
  counted. While
  the device is closed the parser keeps sync and nothing is queued.
- Receiver count and sample rate follow the gateware command the daemon
  issues; the driver observes the command and resizes its parsing. The
  gateware has six receivers on the CL025 and four on the CL016
  (`gateware/README.md`). Words in flight across a receiver-count change
  count as resyncs.
- Capacity: with the upstream PIO clocking one RX value takes 42 PIO
  cycles at 100 MHz, at most about 2.38 million values per second,
  measured at 2.37 million: six receivers at 192 ksps or three at
  384 ksps. Up to that limit, streaming on the development board ran with
  no resyncs. The daemon limits receiver count per rate accordingly.

### 4.4 TX path

- Bounce-buffered DMA to the TX state machine, periods of 256 samples,
  fed from a ring by the same kernel thread.
- `write()` accepts any whole number of 16-bit I/Q samples and appends to
  the ring. When the ring is empty the driver sends nothing and counts an
  underrun if PTT is set; it never invents samples, so the gateware's own
  PTT hang-time watchdog ends transmit when samples stop.
- Low and high watermarks, underrun and overrun counts, and ring fill are
  readable by ioctl, so the daemon can pace Protocol 1 and Protocol 2
  transmit streams against the real device clock.

### 4.5 Control path

- One ioctl family, versioned, with a struct carrying the six command
  bytes out and the six reply bytes in. Serialised with a mutex; the SPI
  transfer completes before return. Gateware identity and driver
  statistics are separate read-only ioctls.
- The ABI header is installed for the daemon and tools and is the only
  interface between them and the module.

### 4.6 Performance targets

| Measure | Upstream | Target |
|---|---|---|
| RX latency, device to userspace | 64 ms at 48 ksps | 1 to 2 ms, rate independent |
| TX start-up buffering | 85 ms at 48 ksps | a few ms, configurable |
| Syscalls per second, TX at 48 ksps | 48000 | under 200 |
| Stream fault reporting | none | counters and `POLLERR` |
| Driver build | private kernel patch | stock kernel headers |

Measured so far: the driver builds on stock headers, and receive runs at
exact rates with no stream faults up to the PIO bus ceiling. Latency and
TX buffering are still to be measured with `rbcat`; if DMA completion
proves late, the period is raised, not the architecture changed.

## 5. Daemon design

- C++20, single process, three real-time threads pinned to CPUs: network
  control, RX stream, TX stream. Lock-free single-producer single-consumer
  rings between them. No sleeps in loops; `epoll` on the sockets and the
  device, timers for the status cadence.
- Device layer behind an interface so every protocol and policy test runs
  against a fake device. The radioberry-p2 draft already has this shape
  and its fail-safe semantics; they are carried forward.
- Protocol 1, Hermes-Lite 2 dialect: discovery, start and stop, 1032-byte
  frames for the gateware's receivers at 48 to 384 ksps within the RX
  capacity (section 4.3), the full C0 command
  set including PA enable and drive, ADC status words reported as absent,
  TCP transport. Every client that speaks to a Hermes-Lite 2 works
  unchanged.
- Protocol 2, openHPSDR Ethernet Protocol v4.x: discovery, General, DDC
  and DUC specific, High Priority both ways, as many DDCs as the gateware
  reports (four or six), DDC I/Q at 48 to 384 ksps,
  DUC I/Q at 192 ksps resampled with liquid-dsp to the gateware rate,
  Mic/Line and Wideband declared absent, sequence checking.
- Session: the first non-discovery packet claims the radio; other clients
  are ignored until a timeout releases it. Timeout or any stream fault
  (sequence gap, queue overrun, device write failure) clears MOX, zeroes
  drive, latches a fault that the client must clear by releasing MOX, and
  logs the cause.
- PA policy: PA enable is off unless the daemon is started with it allowed
  and the client requests it; a drive ceiling and a transmit watchdog are
  enforced; every PA state change is logged. No interlock is claimed, and
  the daemon says so at start-up. The gateware gives three levers, all
  used: the TX permission bit in every SPI command (gates the CN301 T/R
  and PA lines), the PTT bit in every command, and its own PTT hang-time
  watchdog, which must never be disabled. A CW key on the jack drives the
  DAC regardless of software (`gateware/README.md` section 6).
- DSP: liquid-dsp for resampling, filtering, NCO and measurement. wdsp is
  not used.
- Operations: systemd unit running unprivileged, YAML config file,
  `--version`, spdlog to the journal, counters exposed for `rbprobe` and
  tests. No network access other than the protocol ports; nothing phones
  home.

## 6. Platform probe

`rbprobe` gives a trustworthy statement of what the machine is: host,
boot configuration, driver and installed gateware, FPGA configuration and
identity (from the driver's sysfs attributes, or by configuration with
`--identify` when the driver is not loaded), PA board classification and
daemon state, as human text, JSON or YAML with identical content. It exits
non-zero when the radio cannot run. Its probes sit behind an interface so
the report logic is tested against an in-memory machine.

## 7. Testing

- Every component has Catch2 unit tests aiming at complete coverage,
  measured with gcov and reported by lcov; ASan and UBSan builds run the
  same tests. The daemon and tools reach coverage with fake device, fake
  sockets and fixture files. The kernel module's pure logic is compiled
  into a userspace test binary; its kernel glue is exercised by a userspace
  harness against `/dev/radioberry` on the Pi.
- Integration tests run a fake client against the daemon over loopback for
  both protocols, including fault injection for every fail-safe path.
- Hardware validation is a fixed sequence on the development Pi: driver
  probe and gateware identity (done); receive via Protocol 1 and Protocol 2;
  transmit at zero drive into a dummy load with PA disabled; transmit with
  PA enabled and output measured; an FT8 and corvus-protocol end-to-end
  pass. PA idle bias is already set on the development board. Every
  transmit step needs the owner's go-ahead at the time.
- `tools/ci-pi.sh` builds everything on the Pi over SSH, runs the full
  test set, sanitizer builds, format and SPDX checks, and prints coverage;
  it is the gate for squash-merging an item branch into `develop`. There
  is no hosted CI.

## 8. Order of work

1. `rbprobe` (done).
2. Repository skeleton: CMake, formatting, SPDX check, Catch2 and coverage
   wiring, Pi build script (done).
3. Gateware built from source with provenance and interface notes (done).
4. Driver: probe and gateware load, control path, RX and TX streams,
   statistics (done); `rbcat` and `rbctl`, the remaining driver items and
   the userspace test harness (next).
5. Daemon: device layer, Protocol 1, session and PA policy, then
   Protocol 2 with the resampler. `p1probe` and `p2probe` alongside.
6. Hardware validation sequence, with results written into `docs/`.

## 9. Decisions closed on 2026-11-08

- Protocol 2 transmit resampling lives in the daemon.
- Protocol 2 discovery advertises the gateware's receiver count (four
  DDCs on the CL016, six on the CL025), one DUC, 48 to 384 ksps, no
  wideband, diversity or audio.
- The gateware is built here from the unmodified Hermes-Lite 2 source;
  CL016 ships with its documented timing violation.
- No external ADC for PA_CUR in v1; PA current is not reported.
- Tooling and conventions as listed in `TODO/ACTIVE.md` under Standing
  decisions.
