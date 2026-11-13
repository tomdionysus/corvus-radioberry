# Interface strategies: Raspberry Pi 5

Ways the Raspberry Pi 5 and the FPGA could exchange samples and commands
if the gateware were designed from scratch for this hardware. A list of
options with what is known about each, not a decision. The Raspberry Pi 4
list is in [Interface strategies: Raspberry Pi 4](interface_strategies_rpi4.md).

On a Raspberry Pi 5 the header belongs to the RP1 chip, behind PCIe. Its
peripherals differ from the Raspberry Pi 4's: it has programmable I/O
(PIO) and multi-lane I2S, and it has no SMI.

## Grades used

| Grade | Meaning |
|---|---|
| measured | Measured on this hardware |
| pin table | The pin's function is in the RP1 function table printed by `pinctrl funcs` on a Raspberry Pi 5 |
| unverified | A capability or limit of a peripheral or driver that has not been checked here |

## 1. What the hardware gives

From [Hardware discovery](hardware_discovery.md).

| Wires | Pi GPIO | Notes |
|---|---|---|
| Proven, FPGA general I/O | 5, 12, 18, 19, 20, 21, 25 | Either direction |
| Proven, SPI0 | 8, 9, 10, 11 | FPGA general I/O behind them |
| Proven, into FPGA pins 55 and 54 | 4, 6 | Used only as inputs so far; these FPGA pins may be input-only clock pins (unknown). Treat as Pi to FPGA |
| Proven, configuration, free after configuration | 13, 24 | FPGA pins 13 and 12. Pin 13 is driven by the present gateware; whether pin 12 can be an output is unknown. The Pi drives both during every reload |
| Configuration only | 22, 26, 27 | CONF_DONE, nSTATUS, nCONFIG: not available to a design |
| Schematic only | 7, 16, 17, 23 | Not proven to exist on these boards |
| Not to the FPGA | 2, 3 | I2C, to the PA header only |

## 2. What has to be carried

| Stream | Bit rate |
|---|---|
| One receiver at 48 ksps (24-bit I and Q) | 2.3 Mbit/s |
| One receiver at 192 ksps | 9.2 Mbit/s |
| One receiver at 384 ksps, or two at 192 | 18.4 Mbit/s |
| Six receivers at 192 ksps, or three at 384 | 55.3 Mbit/s |
| Transmit at 48 ksps (16-bit I and Q) | 1.5 Mbit/s |
| Transmit at 192 ksps | 6.1 Mbit/s |
| Commands and status | a few kbit/s, low latency |

The Raspberry Pi 5 already carries the top line: 2.37 million values per
second, 56.9 Mbit/s of samples, with the present protocol (measured).

## 3. Strategies

### A. PIO with DMA, present wire protocol (as now)

- Two RP1 PIO state machines clock the buses: RX on GPIO 6, 18 to 21 and
  25, TX on GPIO 4, 5 and 12. The kernel's `rp1-pio` driver moves the
  words by DMA.
- Measured: 2.37 million values per second with no stream errors, GPIO 6
  at 20 MHz within a value; transmit at 102 000 words per second against
  48 000 needed.
- Facts of the engine the driver depends on are in
  [Design](../docs/design.md): one PIO block of four state machines,
  bounce-buffered DMA with at most four buffers per direction, programs
  of at most 32 instructions.
- Weaknesses are the protocol's, not the engine's: 28 bits are moved for
  every 24 of data plus a start nibble on the wire, the transmit stream
  has no framing that recovers, and the constraints the bus runs under
  were written for 4.8 MHz
  ([Evaluation](gateware_evaluation.md)).

### B. PIO with DMA, a protocol designed for it

PIO can produce or follow almost any wire protocol on any header pin
(pin table: every pin from GPIO 4 to 27 has a PIO function), within its
limits: inputs read as a run of consecutive GPIOs, 32 instructions, four
state machines.

Variations:

- **FPGA-paced receive.** The FPGA drives a clock on a general I/O wire
  (GPIO 25) and data on GPIO 18 to 21; the state machine waits on the
  clock and takes four bits per edge. The radio's clock paces the stream,
  no idle polling, no start nibble, and a 32-bit word carries 32 bits of
  data instead of 24.
- **Wider receive.** Consecutive proven data wires are GPIO 18 to 21
  only. A read of GPIO 18 to 25 also takes GPIO 24 and 25 (proven),
  GPIO 22 (CONF_DONE, constant) and GPIO 23 (schematic only): six useful
  bits in eight.
- **Framed transmit.** A frame line (GPIO 12 driven by the Pi, or a
  start pattern) so that a slipped bit costs one sample.
- **The Raspberry Pi 4's strobe.** A state machine can produce the same
  free-running read strobe on GPIO 6 that SMI produces on a Raspberry
  Pi 4, so a gateware built for that strobe (strategy C in the
  Raspberry Pi 4 list) serves both boards.

Unverified: where the ceiling moves to once the protocol stops being the
limit. The 2.37 million words per second measured is 9.5 MB/s through
the DMA path; what that path sustains beyond it has not been measured.

### C. I2S, the FPGA as clock master

- RP1 I2S on GPIO 18 (bit clock), 19 (word select), 20 (data in) and 21
  (data out), in two instances, I2S0 and I2S1 (pin table). With the Pi as
  slave the FPGA drives the clocks from the radio's own clock.
- Both directions at once, and the sample clock is the radio's, as with
  a sound card: transmit samples are consumed at exactly the sample rate,
  so there is no ready line, no fill level to manage and no start-up
  latency logic. Word select frames every sample.
- More lanes (pin table): data in on GPIO 22, 24 and 26, data out on 23,
  25 and 27. Of those only GPIO 24 (in) and GPIO 25 (out) are usable
  wires here: 22, 26 and 27 are configuration pins and 23 is schematic
  only. So at most two lanes in and two out, and lane 2 without lane 1,
  which the driver may not allow (unverified).
- Capacity per lane is the bit clock. Unverified: the slave bit clock
  limit of the RP1 I2S block and its stock driver, the word formats it
  accepts, and whether a raw 64-bit frame at several times the sample
  rate passes through ALSA unharmed.
- Uses four proven wires for one lane. The same four pins are PCM on the
  Raspberry Pi 4, so one gateware interface serves both.

### D. SPI as the stream transport, the Pi as master

- A second SPI controller on GPIO 18 to 21: SPI1 (CE0 18, MISO 19,
  MOSI 20, SCLK 21) (pin table). The Pi runs full-duplex transfers of a
  block at a time: transmit samples go out while receive samples come in.
  Chip select frames every block.
- The FPGA cannot start a transfer: the Pi transfers on a timer, or the
  FPGA raises a line (GPIO 25) when a block is ready.
- Capacity is the SPI clock. Unverified: the clock the RP1 controller
  sustains with DMA and the cost of each transfer across PCIe; 12.5 MHz
  is measured on SPI0 for commands.
- The same pins are SPI6 on the Raspberry Pi 4.

### E. Everything over SPI0

- Commands, status, receive and transmit in one framed protocol on the
  four proven SPI0 wires, with one more wire as an interrupt.
- Capacity and cost as D. Frees every other wire.

### F. Pi clock outputs as a building block

- GPCLK0, 1 and 2 are on GPIO 4, 5 and 6 (pin table), as on the
  Raspberry Pi 4. Not a data path by itself.

### Ruled out or unlikely

| Option | Why |
|---|---|
| SMI | RP1 has none: the SMI overlay does not apply to the Raspberry Pi 5 device tree (measured) |
| SDIO (SD0 on GPIO 22 to 27) | GPIO 22, 26 and 27 are the FPGA's configuration pins |
| UART (UART2 on GPIO 4 and 5, UART4 on 12 and 13) | A few Mbit/s at most |
| SPI on GPIO 4 to 7 (SPI3) | GPIO 4 and 6 may reach input-only FPGA pins; GPIO 7 is a schematic-only wire |
| DPI | Output from the Pi only |
| CPU-clocked GPIO | Every GPIO access crosses PCIe to RP1; PIO exists for this |

## 4. Comparison

| | Wires used | Receive capacity | CPU cost | Transmit | Framing recovers | Also on the Pi 4 | Status |
|---|---|---|---|---|---|---|---|
| A. PIO, present protocol | 9 | 2.37 M values/s | a kernel thread refilling DMA | PIO, 102 000 words/s | RX yes, TX no | the same wire protocol, CPU-clocked or SMI | measured |
| B. PIO, new protocol | 5 to 9 | above A if the DMA path allows | as A | by design | by design | yes if built on the Pi 4's strobe | unverified |
| C. I2S, FPGA master | 4, or 6 with a second lane | the bit clock, per lane | DMA only | included, sample-timed | yes, word select | yes, same pins | unverified |
| D. SPI1 stream | 4 or 5 | the SPI clock | per-block kernel work | included | yes, chip select | yes, same pins | unverified |
| E. SPI0 only | 4 or 5 | the SPI clock | as D | included | yes | yes | unverified |

Reading of the table, mine and open to challenge:

- The Raspberry Pi 5 has no capacity problem today; A already carries
  six receivers at 192 ksps. What a new design buys here is recovery,
  simplicity and a shared interface, not speed.
- PIO is the one engine that can follow whatever the Raspberry Pi 4
  needs, so the Raspberry Pi 4's choice can lead.
- C, D and E are the options that put both boards on stock peripherals
  with one gateware interface. Whether any of them reaches six receivers
  at 192 ksps is unverified, and on the Raspberry Pi 4 side it looks
  unlikely for C.

## 5. What to measure before choosing

| Question | Test |
|---|---|
| The DMA path's ceiling under PIO | A PIO program that pushes a counter with no pins involved, at rising rates, checked for gaps |
| FPGA-paced PIO receive | A test image driving a clock on GPIO 25 and a counter on GPIO 18 to 21 at rising rates |
| I2S slave bit clock limit, word formats, lane 2 without lane 1 | A test image sending a counter as I2S; `arecord` raw capture at rising bit clocks, then with GPIO 24 as a second lane |
| SPI1 with DMA: clock limit and cost per block | A test image answering SPI with a counter; `spidev` transfers at rising clocks and block sizes |
| FPGA pin 12 as an output; pins 54 and 55 as clock inputs | The Cyclone 10 LP pin-out |
