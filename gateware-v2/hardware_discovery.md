# Hardware discovery

What is known about the RadioBerry boards in hand: the chips, the
interfaces between them and the physical wiring. The boards are unbranded,
carry no revision marking and are assumed to be clones of the RadioBerry
2.x, each with the PA5W V2 amplifier board ([PA5W V2](../docs/pa.md)).
Nothing here is taken from a design file on trust.

## Evidence rules

Every statement carries one of three grades.

| Grade | Meaning |
|---|---|
| proven | A function was seen working on the hardware that cannot work unless the statement is true |
| source | Read from the source of the gateware image that runs on the boards, but the function was not exercised or not observed |
| schematic | From the original RadioBerry 2.x beta5 schematic only. The boards are not known to follow it |

Sections 2 to 9 hold proven statements, with the few source-grade rows
marked in their tables. Section 10 holds what the link test image has
measured since, which settles several items listed in section 11 and
corrects section 6: the clock on pin 53 is 76.8 MHz only once the AD9866
has been programmed. Section 11 holds everything else.

The proof route for wiring: the working image is built from
`pins.tcl` in the gateware source
([gateware/README.md](../gateware/README.md)), which names an FPGA pin for
every signal, and the driver's overlay
([driver/radioberry.dts](../driver/radioberry.dts)) names the Raspberry Pi
GPIO for each. When a function works end to end, each pin on both sides of
it is wired as those two files say.

An inference from the gateware source is graded proven only where the
observed behaviour could not have come about any other way given that
source; each such row names the reasoning. The source itself is assessed
in [Evaluation of the current gateware source](gateware_evaluation.md).

The observations were made on the development boards. They have not been
repeated on each of the three boards.

## 1. Observations relied on

| Ref | Observed |
|---|---|
| O1 | The FPGA configures by passive serial from the Pi with the image built for the 10CL025YE144C8G: CONF_DONE rises and nSTATUS stays high. The image built for the 10CL016 leaves CONF_DONE low |
| O2 | After configuration the SPI identity reply reads gateware 75.3, six receivers, one transmitter, FPGA type 2, bit for bit, about 1.7 s after CONF_DONE |
| O3 | Commands over SPI take effect: receiver count, receiver frequency, sample rate |
| O4 | The RX stream parses with no resyncs or dropped words, index nibble checked on every value, from one receiver at 48 ksps to six at 192 ksps |
| O5 | FT8 is decoded from the RX stream on 40 m, 30 m and 20 m, at frequencies that match independent spots of the same stations |
| O6 | RX frames arrive at 48 001.5 to 48 001.6 per second at 48 ksps, timed by the Pi's clock |
| O7 | The gateware takes TX words at 48 000 per second, and throttles a faster writer with its ready line |
| O8 | FT8 transmissions through the PA board were decoded by distant stations at the expected frequency, upper sideband, with no discrete spurs or image above a floor about 46 dB down |
| O9 | Transmit output changes with the drive value in command 0x09 |
| O10 | A transmission keyed with drive 0 straight after an FPGA load, and with PA enable clear, produced full RF output through the PA board (seen once) |
| O11 | `i2cdetect -y 1` finds no device with the PA board stacked |
| O12 | The same board works on a Raspberry Pi 4 and a Raspberry Pi 5 |

Measurements behind O1, O4 to O7: [Test results](../docs/testing.md).

## 2. Chips

| Statement | Grade | From |
|---|---|---|
| The FPGA accepts a 10CL025YE144C8G bitstream (372 888 bytes) and rejects a 10CL016 one | proven | O1 |
| The FPGA is in a package whose pin numbers 12 to 144 match the 144-pin assignment in `pins.tcl` | proven | O2, O4, O8 |
| The FPGA has no configuration memory in use: the Pi loads it by passive serial | proven | O1 |
| The six-receiver design fits it and meets its timing constraints: 20 379 of 24 624 logic elements, all 132 nine-bit multipliers, 460 800 of 608 256 memory bits, 46 pins, one of four PLLs | proven | fitter report, O4 |
| The converter behaves as an AD9866 in its full-duplex interface mode: 12-bit receive and transmit samples, each as two 6-bit halves on separate ports, 76.8 MHz sample rate | proven | O5, O8 |
| The converter accepts AD9866 register writes on a three-wire serial port | proven | O9 |
| Nothing answers on the Pi's I2C bus 1, on the RadioBerry or the PA board | proven | O11 |

The chip markings have not been read. A 10CL025 bitstream configuring
proves bitstream compatibility, not the part number printed on the chip.

## 3. Raspberry Pi header to FPGA

All 3.3 V signalling, as the Pi's GPIO.

| Function | Pi GPIO (BCM) | Header pin | FPGA pin | Direction | Grade |
|---|---|---|---|---|---|
| nCONFIG | 27 | 13 | 14 | Pi to FPGA | proven, O1 |
| DCLK | 24 | 18 | 12 | Pi to FPGA | proven, O1 |
| DATA0 | 13 | 33 | 13 | Pi to FPGA | proven, O1 |
| CONF_DONE | 22 | 15 | 92 | FPGA to Pi | proven, O1 |
| nSTATUS | 26 | 37 | 9 | FPGA to Pi | proven, O1 |
| SPI clock | 11 | 23 | 58 | Pi to FPGA | proven, O2 |
| SPI MOSI | 10 | 19 | 60 | Pi to FPGA | proven, O3 |
| SPI MISO | 9 | 21 | 59 | FPGA to Pi | proven, O2 |
| SPI CE0 | 8 | 24 | 51 | Pi to FPGA | proven, O2 |
| RX clock | 6 | 31 | 54 | Pi to FPGA | proven, O4 |
| RX data 0 | 18 | 12 | 33 | FPGA to Pi | proven, O4 |
| RX data 1 | 19 | 35 | 44 | FPGA to Pi | proven, O4 |
| RX data 2 | 20 | 38 | 43 | FPGA to Pi | proven, O4 |
| RX data 3 | 21 | 40 | 42 | FPGA to Pi | proven, O4 |
| RX sample-ready | 25 | 22 | 31 | FPGA to Pi | proven, O4 |
| TX clock | 4 | 7 | 55 | Pi to FPGA | proven, O7 |
| TX data | 5 | 29 | 32 | Pi to FPGA | proven, O8 |
| TX ready | 12 | 32 | 49 | FPGA to Pi | proven, O7 |

- For the five configuration lines O1 proves the Pi GPIO and the function
  it reaches. Their FPGA pin numbers are from the schematic's symbol, not
  from a test; the gateware source confirms only pin 13 as DATA0.
- The four RX data lines are each proven separately: with six receivers
  the index nibble runs 0 to 11, which exercises every bit, and it is
  checked on every value.
- FPGA pins 12 (DCLK) and 13 (DATA0) become ordinary I/O once the FPGA is
  configured. The running gateware drives pin 13 with the level of FPGA
  pin 142 (a paddle input with a pull-up), so the FPGA drives GPIO 13 in
  user mode, high when no paddle contact is closed. The driver leaves
  GPIO 13 as an output at 0 after configuring. The wire to pin 13 is
  proven (O1), so if both readings are right the two ends drive it against
  each other while the radio runs (source and driver code; not measured).
- The SPI MISO wire is released by the FPGA when CE0 is high (source).
- FPGA pins the design does not use are inputs with weak pull-ups, the
  build tool's default (source).

Protocols on these wires that are proven to work (they are the gateware's,
not the board's; the detail is in
[gateware/README.md](../gateware/README.md)):

- Configuration: passive serial, least significant bit first.
- SPI: mode 3, 48-bit transfers, most significant bit first, on CE0.
- RX: the Pi clocks GPIO 6; the FPGA changes data on the rising edge.
- TX: the Pi clocks GPIO 4 and presents one bit per rising edge on GPIO 5
  while GPIO 12 is high.

## 4. FPGA to AD9866

| Signal (gateware name) | FPGA pin | Direction | Grade |
|---|---|---|---|
| 76.8 MHz clock in (`rffe_ad9866_clk76p8`) | 53 | to FPGA | proven, O2, O6 |
| RX data bits 0 to 5 (`rffe_ad9866_rx`) | 80, 83, 85, 86, 87, 98 | to FPGA | proven as a bus, O5 |
| RX sync (`rffe_ad9866_rxsync`) | 77 | to FPGA | proven on the assumption below, O5 |
| TX data bits 0 to 5 (`rffe_ad9866_tx`) | 99, 100, 101, 103, 105, 106 | to AD9866 | proven as a bus, O8 |
| TX sync (`rffe_ad9866_txsync`) | 76 | to AD9866 | proven on the assumption below, O8 |
| TX quiet, active low (`rffe_ad9866_txquiet_n`) | 72 | to AD9866 | source; see below |
| Serial enable, active low (`rffe_ad9866_sen_n`) | 65 | to AD9866 | proven, O9 |
| Serial clock (`rffe_ad9866_sclk`) | 66 | to AD9866 | proven, O9 |
| Serial data (`rffe_ad9866_sdio`) | 68 | to AD9866 | proven, O9 |
| Mode, driven high (`rffe_ad9866_mode`) | 111 | to AD9866 | source |
| Reset, active low (`rffe_ad9866_rst_n`) | 114 | to AD9866 | source |
| RX clock (`rffe_ad9866_rxclk`) | 71 | to FPGA | source; the gateware declares it and never uses it |

- "Proven as a bus": receive and transmit work with these pins in this
  order. A fault on a low-order bit alone would not stop FT8 working, so
  the individual bit lines are not each proven. The transmit spectrum in
  O8 (nothing above a floor about 46 dB down) bounds how wrong the
  high-order TX bits can be.
- The data interface that works, both directions clocked in the FPGA at
  153.6 MHz, twice the 76.8 MHz clock on pin 53:
  - Receive: the AD9866 sends each 12-bit sample as two 6-bit halves. The
    half that arrives with RX sync high is the low half; the half before
    it is the high half.
  - Transmit: the FPGA sends the low half with TX sync high and the high
    half with TX sync low. TX quiet is high for the whole transmission and
    low otherwise; outside a transmission the TX data and TX sync pins are
    held low.
- The order of the halves is proven by inference: the source takes a
  sample only when RX sync is high and puts that half low. Halves the
  other way round would put the noisy low bits on top, and weak FT8
  signals could not be decoded (O5).
- The sync lines are proven on one assumption, stated here because the
  source allows one other way. RX sync stuck high would make the gateware
  take a sample on every 153.6 MHz edge and keep one of the two phases; a
  fixed, fortunate phase would then receive correctly. An unconnected TX
  sync would leave the AD9866 to pair the halves by itself. Receive and
  transmit have worked on every load, on two host boards, so both lines
  are taken as connected.
- TX quiet: the source holds pin 72 low except while transmitting. A
  transmitter that works (O8) shows the AD9866 is not held quiet during a
  transmission; it does not show the pin is what releases it.
- Whether mode and reset are wired cannot be told from a working radio:
  the full-duplex mode is proven (section 2), but a strap on the board
  would do the same as pin 111. The build has no half-duplex option set,
  so pin 111 is driven high throughout (source).

## 5. AD9866 serial port and the configuration that works

Proven by O9 as a whole: the port works and the transmit gain register
takes effect. The other registers' effects have not been observed one by
one (source).

- Frame: 16 bits, most significant first, serial enable low for the frame:
  three zero bits (the first is the write flag), a 5-bit register address,
  8 data bits. Serial clock 5 MHz; data changes on its falling edge.
- Write only. The gateware has no read-back pin.
- Written once after every gateware reset, in this order:

  | Register | Value | Comment in the source |
  |---|---|---|
  | 0x04 | 0x36 | none |
  | 0x06 | 0x14 | disable CLKOUT2 |
  | 0x07 | 0x21 | start DC offset calibration, RX filter on |
  | 0x08 | 0x40 | RX filter -3 dB at about 34 MHz |
  | 0x09 | 0x7f | none (RX gain register) |
  | 0x0a | 0x7f | none (TX gain register) |
  | 0x0b | 0x00 | no RX gain on PGA |
  | 0x0c | 0x43 | TX two's complement and interpolation factor |
  | 0x0d | 0x03 | RX two's complement |
  | 0x12 | 0x00 | none |

  Registers 0x00 to 0x03, 0x05, 0x0e to 0x11 and 0x13 are left at their
  reset values.
- Afterwards: command 0x0a writes register 0x09 (RX gain) as `01` and a
  6-bit code; command 0x09 writes register 0x0a (TX gain) as `01` and a
  6-bit code from 0 to 39 computed from the drive byte `d` as
  `((d + 4) >> 3) + (d >> 5)`; command 0x3b writes any register.

## 6. Clock

| Statement | Grade | From |
|---|---|---|
| FPGA pin 53 carries a continuous clock of 76.8 MHz nominal | proven | O2, O6, by inference: the PLL passes pin 53 through unchanged to the receivers, which divide by 5, 40 and 8, 1600 in all, for the 48 ksps frame rate. 48 001.5 frames per second can only come from 76.8024 MHz, to the accuracy of the Pi's clock. A 38.4 MHz clock would give 24 000 |
| That clock runs before and during the gateware's 1.68 s reset | proven | O2: the reset is counted from it and ends |
| The clock is stable enough for FT8 and WSPR | proven | O5, O8 |
| On one board the reference is off by 22.9 ppm: uncorrected FT8 decodes read 161 to 163 Hz low on 40 m, and with the correction 0 to 2 Hz off on 40 m and 30 m | proven | [Test results](../docs/testing.md) |

Since measured (section 10): the board's oscillator runs at 38.4 MHz, and
pin 53 carries 76.8 MHz only after the AD9866's register 0x04 has been
written to double it. Which part is the oscillator is still unread.

## 7. PA header (CN301) and the PA board

| Statement | Grade | From |
|---|---|---|
| The RadioBerry's TX output, through the PA board, reaches the antenna connector at a few watts while the gateware keys its CN301 outputs | proven | O8 |
| The PA board transmits with PA enable clear, when `envpa` and `envbias` are low. Its transmit-receive switching follows `exttr` (FPGA 144) or `inttr` (FPGA 143), or both | proven | O10, seen once; the PA enable state is the daemon's setting, not read back |
| The PA board's bias is set by its two trimmers and by nothing on the RadioBerry or the Pi | proven | O10, O11 |
| The PA board has no I2C device | proven | O11 |

| CN301 signal (gateware name) | FPGA pin | Direction | Grade |
|---|---|---|---|
| `io_pa_exttr` | 144 | out | proven that 144 or 143 keys the PA board |
| `io_pa_inttr` | 143 | out | as above |
| `io_pwr_envpa` | 136 | out | source |
| `io_pwr_envbias` | 137 | out | source |
| `io_phone_tip` (key in) | 120 | in, weak pull-up | source |
| `io_phone_ring` (PTT in) | 121 | in, weak pull-up | source |
| `io_cwl`, `io_cwr` (paddle) | 141, 142 | in, weak pull-up | source |

The source fixes what each output does, with `keyed` standing for
(transmitting or PTT on the jack) and run and TX permission:

- `exttr` = keyed.
- `inttr` = keyed and (PA enable or not T/R disable).
- `envpa` = `envbias` = keyed and PA enable.

So in O10, with PA enable clear, pins 136 and 137 were low and pin 144
was high; pin 143 was high unless T/R disable was set.

The PA board's markings and connectors: [PA5W V2](../docs/pa.md).

## 8. Speeds measured on the wiring

| Path | Measured | Result |
|---|---|---|
| SPI, Pi to FPGA | 12.5 MHz clock | every transfer correct |
| RX bus, Raspberry Pi 5 | GPIO 6 clocked at 20 MHz within a value, 2.37 million values per second | no stream errors |
| RX bus, Raspberry Pi 4, CPU-clocked | up to 768 000 values per second | no stream errors |
| RX bus, Raspberry Pi 4, SMI read strobes on GPIO 6 | 9 million strobes per second | no stream errors |
| RX bus, Raspberry Pi 4, SMI | 10.6 million strobes per second | resyncs; whether the Pi or the board is the limit is not known |
| TX bus, Raspberry Pi 5 | GPIO 4 clocked at 3.57 MHz, 102 000 words per second | throttled correctly by ready |
| FPGA to AD9866 data | 153.6 MHz | receive and transmit work |

The gateware's timing constraints cover the RX clock only to 4.8 MHz and
the TX clock to 16 MHz. The RX bus working at 20 MHz is a measurement, not
a guarantee from the build.

## 9. Hazards established

- After an FPGA load the AD9866's transmit gain is at its highest. O10: a
  keyed transmission at drive 0 gave full output. The source gives the
  cause (source grade, not confirmed by measurement): the start-up
  sequence writes 0x7f to the TX gain register, and command 0x09 writes a
  new gain only when it differs from the gateware's own copy, which
  starts at 0, so drive 0 to 3 is never written.
- PA enable does not gate the PA board (section 7). Any PTT with the TX
  permission bit and run set keys it.
- While run is set, a key on the PA board's jack drives the DAC without
  the TX permission bit; only the PA lines are held off. With run clear
  the transmit state machine is held idle and nothing drives the DAC
  (source; [gateware/README.md](../gateware/README.md) section 6 says run
  is not needed, and the source says it is).
- One stray edge is permanent on two of the header's wires (source). The
  SPI slave counts 48 clock edges with no reset from chip select, and the
  TX intake counts 33 edges per word with no other framing. An extra or
  missing edge on GPIO 11 or GPIO 4 misaligns every later command or
  sample until the gateware is reloaded. The RX bus does not have this
  weakness: every value is framed.
- GPIO 13 is driven from both ends (section 3).

## 10. Measured with the link test image

Measured on one board on a Raspberry Pi 4 with the link test image
([rtl/linktest](rtl/linktest), host tool [host/linktest.py](host/linktest.py)),
which is written from scratch and shares no code with the radio gateware.
Where a row contradicts an earlier section or section 11, this section
is the newer and stands.

### The FPGA

| Statement | How |
|---|---|
| A design that shares no code with the radio gateware configures and runs | Identity and scratch registers over SPI0, mode 0, 10 MHz |
| Block RAM and embedded multipliers work | Self-test: 0 errors in two passes over 1024 x 18 bits and 65 536 products checked against logic-cell multipliers |
| FPGA pins 54 and 55 (GPIO 6 and 4) are input-only | The build tool lists 22 to 25, 52 to 55, 88 to 91 and 126 to 129 as dedicated clock inputs; both read the Pi's level |
| FPGA pin 12 (DCLK, GPIO 24) is not available to a design | The fitter reserves it in passive serial mode whatever the project asks. GPIO 24 carries nothing after configuration |

### Header wires

Each wire was driven from the Pi and read by the FPGA, then driven by the
FPGA and read by the Pi. No wire moved more than one FPGA pin.

| Pi GPIO | FPGA pin | Pi to FPGA | FPGA to Pi |
|---|---|---|---|
| 4 | 55 | yes | input-only pin |
| 5 | 32 | yes | yes |
| 6 | 54 | yes | input-only pin |
| 7 | 50 | yes | yes |
| 12 | 49 | yes | yes |
| 13 | 13 | yes | yes |
| 16 | 46 | yes | yes |
| 17 | 113 | yes | yes |
| 18 | 33 | yes | yes |
| 19 | 44 | yes | yes |
| 20 | 43 | yes | yes |
| 21 | 42 | yes | yes |
| 23 | 39 | yes | yes |
| 25 | 31 | yes | yes |

- GPIO 7, 16, 17 and 23, schematic-only until now, are real wires in
  both directions.
- FPGA pins 6, 7, 8, 10, 11, 28 and 112 follow no header GPIO tested.
- The ten PA header inputs (FPGA 120, 121, 141, 142, 115, 119, 125, 132,
  133, 135) all read high with the PA board fitted and nothing keyed.

### Clock

| State of the AD9866 | Pin 53 | Pin 69 | Pin 71 (RXCLK) | RX sync edges |
|---|---|---|---|---|
| Held in reset | 38.4005 MHz | the same | 76.801 MHz | none |
| Reset released, registers at their defaults | 38.4007 MHz | the same | 76.801 MHz | 38.4007 MHz |
| Register 0x04 written with 0x36 | 76.8016 MHz | the same | 153.603 MHz | 76.8016 MHz |

- The board's oscillator runs the AD9866 at 38.4 MHz. The 76.8 MHz the
  radio gateware depends on exists only after register 0x04 is written:
  the AD9866 doubles its clock.
- The datasheet ([reference/ad9866.pdf](reference/ad9866.pdf), Table 11)
  accounts for every figure. At reset register 0x04 is 0x01: PLL
  multiplier x2, ADC clocked straight from OSCIN. So the DAC clock is
  76.8 MHz, CLKOUT1 (DAC clock / 2) is 38.4 MHz and RXCLK (nibble rate)
  76.8 MHz. 0x36 sets multiplier x4, divider /2 and the ADC from the PLL:
  DAC clock 153.6 MHz, ADC 76.8 Msps, CLKOUT1 76.8 MHz, RXCLK 153.6 MHz.
  Pins 53 and 69 both carry CLKOUT1.
- Pin 69 carries the same clock as pin 53, and pin 71 a clock at twice
  that rate.
- Frequencies are against the Pi's clock, about 14 to 21 ppm high over
  3 to 20 s.
- Not reconciled: the radio gateware counts its 1.68 s start-up reset on
  this clock while it holds the AD9866 in reset, which at 38.4 MHz would
  take 3.36 s, yet its identity was found valid 1.8 s after loading.

### AD9866 serial port

- Reading works, on the data line itself: the chip answers on SDIO
  (FPGA 68) when the FPGA releases the line for the data half of a frame
  whose first bit is 1. The separate data-out pin (FPGA 67) stayed high
  throughout; the chip was not switched to use it.
- A write followed by a read returns the value written (register 0x08:
  0x40, then 0x41). The serial port is proven in both directions.
- Values read after a reset, before anything is written:

  | Register | Value | Register | Value |
  |---|---|---|---|
  | 0x00 | 0x00 | 0x0a | 0x7f |
  | 0x01 | 0x00 | 0x0b | 0x24 |
  | 0x02 | 0x7f | 0x0c | 0x41 |
  | 0x03 | 0xff | 0x0d | 0x01 |
  | 0x04 | 0x01 | 0x0e, 0x0f | 0x00 |
  | 0x05 | 0x00 | 0x10 | 0x44 |
  | 0x06 | 0x44 | 0x11 | 0x62 |
  | 0x07 | 0x01 | 0x12 | 0x01 |
  | 0x08 | 0x80 | 0x13, 0x14 | 0x00 |
  | 0x09 | 0x00 | 0x15 | 0x62 |

  Registers 0x16 to 0x1f read 0x00.
- The transmit gain register (0x0a) is 0x7f straight out of reset,
  before any start-up write: per the datasheet, bit 6 selects gain set
  over the serial port and bits 5 to 0 are gain code 63, the highest. The
  hazard in section 9 does not depend on the radio gateware's start-up
  sequence. The TxPGA spans 7.5 dB on the DAC output and 19.5 dB on the
  IAMP outputs, in 0.5 dB steps.
- These values are the datasheet's reset defaults for MODE = 1 (full
  duplex) and CONFIG = 0, register for register.
- FPGA pin 111 drives the AD9866's MODE pin (proven): reset with pin 111
  low, the chip comes up with the half-duplex defaults (0x0b = 0x20,
  0x0c = 0x40, 0x0d = 0x00); with it high, the full-duplex ones (0x24,
  0x41, 0x01). Registers 0x02, 0x04, 0x05 and 0x06 are the same either
  way, as the datasheet gives them for CONFIG = 0: the CONFIG pin is held
  low on the board.
- The serial frame matches the datasheet: an 8-bit instruction (read
  bit, two bits of byte count minus one, five address bits) then one
  data byte, most significant bit first, three-wire by default.
- The reset line (FPGA 114) is wired and works: with it low there is no
  RX sync, and the registers read their defaults after it.
- The chip has analog and digital loopback (register 0x0d bits 7 and 6).
  Digital loopback returns the transmit port's data on the receive port;
  with TX quiet held low (the fast DAC and IAMP power-down) it can prove
  each transmit data line. Not yet run.

### AD9866 receive bus

- With the chip at its defaults (38.4 Msps): all six data pins and RX
  sync toggle, all twelve sample bits toggle, samples span -311 to 299.
- At 76.8 Msps, with RX sync and the data both captured in the I/O-cell
  registers at the rising edge of the image's 153.6 MHz clock: samples
  span about -330 to 290 counts at the lowest receive gain, with the
  antenna port on a dummy load, and sit about 13 counts above zero.
- Bus timing at 153.6 MHz, from a raw capture of 2048 consecutive
  half-samples at both clock edges: the data changes before the falling
  edge and RX sync after it, up to about 3 ns later than the data. At the
  rising edge both are settled. A first build that took the data through
  the I/O-cell registers and RX sync through fabric routing paired the
  halves wrongly and read full scale; the radio gateware puts both in the
  I/O cells, as this image now does.
- The AD9866's RX clock output (FPGA pin 71) runs at 153.6 MHz, a clock
  for this bus from the chip itself. Neither the radio gateware nor this
  image uses it. Pin 71 is not one of the FPGA's dedicated clock inputs.
- Receive gain works: register 0x09 written with 0x40 left the samples
  at the same level; with 0x5f and 0x7f the samples hit both ends of the
  range. Which part of the input path saturates is not established.

## 11. Not proven

### From the gateware source only

- Pi GPIO 17 (header 11) to FPGA 113, and GPIO 13 to FPGA 13 in user mode:
  the paddle pass-through outputs `pi_cwl` and `pi_cwr`. Never exercised.
- SPI CE1 (GPIO 7) to FPGA 50. Assigned, unused by the logic.
- The mode, reset and RX clock lines of section 4, and the CN301 rows of
  section 7 marked source.

### From the beta5 schematic only

The schematic agrees with every proven connection in sections 3 and 4:
no proven pin contradicts it. That makes it a good guide and nothing more
for the nets below.

- Further FPGA pins wired to the Pi header and unused by the running
  gateware: FPGA 39 to header 16 (GPIO 23), FPGA 46 to header 36
  (GPIO 16). Header pins 8, 10, 27 and 28 unconnected.
- AD9866 pins wired to the FPGA and unused by the running gateware:
  serial data out (SDO) to FPGA 67; CLKOUT1 to FPGA 69 as well as 53.
- Clock: a TCXO into the AD9866's oscillator input, drawn with the label
  38.4 MHz, and the FPGA's clock on pin 53 taken from the AD9866's
  CLKOUT1. If that is how the boards are built, CLKOUT1 runs while the
  AD9866 is held in reset (section 6), and its frequency depends on the
  register values in section 5.
- AD9866 gain pins (PGA0 to PGA5) tied to ground: gain by serial port
  only. Consistent with the running gateware, which has no such pins.
- RF: receive from an SMA through a 35 MHz low-pass filter and an 8:1
  transformer to the AD9866's RX inputs; transmit from one AD9866 output
  pair through a 1:1 transformer and a 50 MHz low-pass filter to an SMA.
- Supply: 5 V from the Pi header into linear regulators for 3.3 V (FPGA
  I/O), 3.3 V (AD9866), 2.5 V and 1.2 V.
- FPGA configuration mode pins (MSEL) all tied to ground.
- CN301 as a 2x10 header: 5 V, 3.3 V, fourteen FPGA pins (144, 143, 142,
  141, 137, 136, 135, 133, 132, 125, 121, 120, 119, 115 on pins 3 to 16),
  the Pi's I2C on 17 and 18, ground on 19 and 20. Optional pull-up and
  termination resistors on some of them, not fitted.

### Unknown

- The part numbers printed on the FPGA, the converter, the oscillator and
  the regulators of each board.
- The oscillator's frequency and what drives FPGA pin 53.
- Which FPGA pins are input-only. Pins 53, 54 and 55 are used only as
  inputs by the running gateware; a new design that drives GPIO 4 or 6
  from the FPGA needs the device's pin-out checked first.
- Which of FPGA 144 and 143 switches the PA board's relay, and what the
  PA board does with `envpa` and `envbias`.
- The PA_CUR header beyond its current-sense output.
- What the PA lines do while the FPGA is not configured: from power-on
  until the driver loads the gateware, and during every reload. The
  device family's documented behaviour is that user pins are then
  released with weak pull-ups, and the PA lines key on a high level. Not
  checked on these boards; whether a weak pull-up is enough to key the PA
  board is not known.
- Whether the three boards are identical. One board gave no RF output on
  transmit when tried; the cause was not investigated.
- Whether any pin of the original 73.x GPIO-mode gateware, which also
  received on one of these boards, adds to section 3. Its pin file is not
  in this repository.

### What would settle each

| Unknown | Check |
|---|---|
| Part numbers, oscillator frequency | Read the chip and oscillator markings on each board |
| Clock source on pin 53 | Continuity from pin 53 to the AD9866's CLKOUT1 pin, or a build that changes register 0x04 or 0x06 and watches the frame rate |
| Spare Pi to FPGA wires (GPIO 16, 17, 23, 7) | A test image that toggles each FPGA pin, read on the Pi, and the reverse |
| Individual AD9866 data bits | A test image that reports each RX bit's activity; a transmit test of one bit at a time into a dummy load |
| Mode and reset lines | A test image that holds each the other way and reports whether receive still works |
| Relay line | A test image that raises 144 and 143 one at a time, with the transmit path idle |
| Input-only pins | The Cyclone 10 LP pin-out for the 144-pin package |
| PA lines with the FPGA unconfigured | With the PA board on a dummy load and its supply current watched, power the Pi with the driver disabled and note the TX LED, the relay and the level on CN301 pins 3 and 4 |
| GPIO 13 driven from both ends | Read GPIO 13's level and direction on the Pi while the radio runs; then set it to input and read it with the paddle open and closed |
| Board to board differences | O1 to O8 repeated on each board |
