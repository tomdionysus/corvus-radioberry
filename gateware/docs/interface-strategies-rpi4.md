# Interface strategies: Raspberry Pi 4

Ways the Raspberry Pi 4 and the FPGA could exchange samples and commands
if the gateware were designed from scratch for this hardware. A list of
options with what is known about each, not a decision. The Raspberry Pi 5
list is in [Interface strategies: Raspberry Pi 5](interface-strategies-rpi5.md).

## Grades used

| Grade | Meaning |
|---|---|
| measured | Measured on this hardware |
| pin table | The pin's function is in the BCM2711 function table printed by `pinctrl -c bcm2711 funcs` |
| unverified | A capability or limit of a peripheral or driver that has not been checked here |

## 1. What the hardware gives

From [Hardware discovery](hardware-discovery.md).

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

The Raspberry Pi 4 limit that started this: clocking a bus from the CPU
costs a core from 192 000 values per second
([CPU cost of receive](../../docs/finding-rpi4-cpu.md)).

## 3. Strategies

### A. CPU-clocked buses (as now)

- The driver's thread toggles GPIO 6 and GPIO 4 and reads or writes the
  data pins through the GPIO registers.
- Capacity: 768 000 values per second with no loss, a whole core from
  192 000 (measured).
- Needs nothing from the Pi but GPIO. Every other strategy is judged
  against its CPU cost.

### B. SMI reads with DMA, present wire protocol

- The BCM2711 Secondary Memory Interface strobes GPIO 6 (SOE) and
  latches SD10 to SD13 (GPIO 18 to 21) and SD17 (GPIO 25) on each strobe;
  DMA moves the words to memory (pin table).
- Measured: 8.9 to 9.0 million strobes per second with no errors; faster
  timings gave resyncs. That is 1.12 million values per second with the
  present eight-strobe value, for about 1% of the four cores.
- Measured quirks: the stock `bcm2835_smi` driver cannot read 18 bits, so
  the driver programs SMI itself; a transfer has a length and has to be
  restarted from the SMI's own interrupt; DMA requests must stop before
  SMI is disabled.
- Open: transmit on this transport locked up the desktop in testing, and
  the cause is not established.
- The strobe is free-running: SMI cannot wait for the FPGA, so idle
  strobes are stored and discarded.

### C. SMI reads with DMA, a word designed for the strobe

- Same engine as B, with the gateware built for a free-running strobe
  and more of the 18 latched bits used.
- Data pins on proven wires that SMI can latch (pin table): SD4
  (GPIO 12), SD5 (GPIO 13), SD10 to SD13 (GPIO 18 to 21), SD16 (GPIO 24),
  SD17 (GPIO 25): eight bits per strobe. SD0 to SD3 are the SPI0 pins.
  SD8, SD9 and SD15 (GPIO 16, 17, 23) are schematic-only wires.
- Capacity if the measured strobe rate holds with eight bits: seven data
  bits and a valid bit at 9 million strobes per second is 63 Mbit/s,
  enough for six receivers at 192 ksps. Unverified: the strobe rate with
  eight outputs switching, and FPGA pin 12 as an output.
- Cost: GPIO 12, 13 and 24 are taken, so transmit and its ready line
  move elsewhere (D, F or G), and GPIO 13 and 24 are shared with
  configuration.
- The strobe arrives on FPGA pin 54. If that is a clock pin, the
  read-out can be clocked by it directly.

### D. SMI writes for transmit

- SMI can also write, strobing SWE on GPIO 7 with data on the SD pins
  and two address bits on GPIO 5 and 4 (pin table).
- GPIO 7 to the FPGA is a schematic-only wire. Without it SMI cannot
  write.
- Unverified: mixing SMI reads and writes under DMA.

### E. PCM (I2S), the FPGA as clock master

- The BCM2711 PCM block on GPIO 18 (bit clock), 19 (frame sync), 20
  (data in) and 21 (data out) (pin table), with the Pi as slave: the FPGA
  drives the bit clock and frame sync from the radio's own clock.
- Both directions at once: receive on GPIO 20, transmit on GPIO 21.
- The sample clock is the radio's, as with a sound card. Transmit
  samples are consumed at exactly the sample rate, so there is no ready
  line, no fill level to manage and no start-up latency logic; frame sync
  frames every word.
- The stock `bcm2835-i2s` driver with DMA through ALSA exists
  (unverified at these rates; a custom kernel stream path may not be
  needed at all).
- Capacity: the PCM block carries two slots of up to 32 bits per frame
  (unverified against the datasheet figure). As a raw 64-bit transport
  with the frame rate raised, the limit is the bit clock; a 25 MHz slave
  bit clock limit is commonly quoted (unverified). That would carry one
  receiver at 384 ksps or two at 192, not six.
- Uses four proven wires and leaves the rest free.
- The same four pins carry I2S on the Raspberry Pi 5.

### F. SPI as the stream transport, the Pi as master

- A second SPI controller on GPIO 18 to 21: SPI6 (CE0 18, MISO 19,
  MOSI 20, SCLK 21) (pin table). SPI1 is on the same pins but is the
  auxiliary controller, without DMA (unverified).
- The Pi runs full-duplex transfers of a block at a time: transmit
  samples go out while receive samples come in. Chip select frames every
  block, so a stray clock edge costs one block.
- The FPGA cannot start a transfer. Either the Pi transfers on a timer,
  or the FPGA raises a line (GPIO 25) when a block is ready.
- Capacity: the bit rate is the SPI clock. Unverified: the clock this
  path sustains; 12.5 MHz is measured on SPI0 for commands, and tens of
  MHz are commonly used. 30 MHz would carry one receiver at 384 ksps.
- Stock kernel SPI with DMA; each transfer costs a kernel round trip,
  so block size sets the CPU cost and the latency (unverified).
- The same pins are SPI1 on the Raspberry Pi 5.

### G. Everything over SPI0

- Commands, status, receive and transmit in one framed protocol on the
  four proven SPI0 wires, with one more wire as an interrupt.
- Capacity and cost as F. Frees every other wire.
- Transmit alone is light enough for this whatever carries receive:
  1.5 Mbit/s, with the FIFO level returned in the reply for flow control.

### H. Pi clock outputs as a building block

- GPCLK0, 1 and 2 are on GPIO 4, 5 and 6 (pin table): the Pi can put a
  continuous clock into FPGA pins 55 and 54 with no CPU work.
- Not a data path by itself. It could clock a bus whose data the Pi
  captures with another block, and the present design already spends
  GPIO 4 and 6 as clocks.

### Ruled out or unlikely

| Option | Why |
|---|---|
| SDIO (SD1 on GPIO 22 to 27) | GPIO 22, 26 and 27 are the FPGA's configuration pins |
| UART (UART3 on GPIO 4 and 5, UART5 on 12 and 13) | A few Mbit/s at most: one receiver at 48 ksps and nothing above |
| SPI slave on the Pi (GPIO 8 to 11) | The BCM2711 slave block is on the SPI0 pins; its throughput and driver support are unverified and it has a poor record |
| DPI | Output from the Pi only, and it takes GPIO 0 to 27 |
| A gateware-generated clock into the Pi on GPIO 4 or 6 | Those FPGA pins may be input-only |

## 4. Comparison

| | Wires used | Receive capacity | CPU cost | Transmit | Framing recovers | Also on the Pi 5 | Status |
|---|---|---|---|---|---|---|---|
| A. CPU-clocked | 9 | 0.77 M values/s | a core | same engine | RX yes, TX no (present protocol) | no (the Pi 5 uses PIO) | measured |
| B. SMI, present word | 6 for RX | 1.12 M values/s | about 1% | CPU-clocked | RX yes | the same wire protocol runs under PIO | measured; transmit fault open |
| C. SMI, new word | 9 for RX | about 2.6 M values/s if 9 M strobes hold | as B | needs D, F or G | by design | PIO can produce the same strobe | unverified |
| E. PCM, FPGA master | 4 | 25 Mbit/s if the quoted limit holds | DMA only | included, sample-timed | yes, frame sync | yes, same pins | unverified |
| F. SPI6 stream | 4 or 5 | the SPI clock | per-block kernel work | included | yes, chip select | yes, same pins | unverified |
| G. SPI0 only | 4 or 5 | the SPI clock | as F | included | yes | yes | unverified |

Reading of the table, mine and open to challenge:

- Only E, F and G give one gateware interface that both boards drive
  with stock peripherals on the same pins.
- Only B and C have a measured engine on this board, and C is the only
  listed route to six receivers at 192 ksps on a Raspberry Pi 4.
- E removes the transmit flow-control problem instead of solving it.

## 5. What to measure before choosing

| Question | Test |
|---|---|
| SMI strobe rate with eight data bits | A test image driving SD4, SD5, SD10 to SD13, SD16, SD17 with a counter; read with the existing measurement module |
| FPGA pin 12 as an output; pins 54 and 55 as clock inputs | The Cyclone 10 LP pin-out, then the same test image |
| PCM slave bit clock limit and slot format | A test image sending a counter as I2S at rising bit clocks; `arecord` raw capture, check for gaps |
| SPI6 with DMA: clock limit and cost per block | A test image answering SPI with a counter; `spidev` transfers at rising clocks and block sizes |
| GPIO 7, 16, 17, 23 wires | The pin test in [Hardware discovery](hardware-discovery.md) |
