# Interfaces

What the images present to the Raspberry Pi. The driver is the other side
of all of it ([driver/kernel/radioberry.c](../../driver/kernel/radioberry.c),
[rb_frame.h](../../driver/kernel/rb_frame.h),
[rb_pio.h](../../driver/kernel/rb_pio.h)).

## Pins

| Pi GPIO | FPGA pin | Pi 5 image (`rpi5-pio`) | Pi 4 image (`rpi4-i2s`) |
|---|---|---|---|
| 8 | 51 | SPI chip select (CE0) | SPI chip select (CE0) |
| 9 | 59 | SPI MISO | SPI MISO |
| 10 | 60 | SPI MOSI | SPI MOSI |
| 11 | 58 | SPI clock | SPI clock |
| 4 | 55 | TX clock from the Pi | unused |
| 5 | 32 | TX data from the Pi | unused |
| 6 | 54 | RX clock from the Pi | unused |
| 12 | 49 | TX ready to the Pi | unused |
| 18 | 33 | RX data bit 0 | I2S bit clock to the Pi |
| 19 | 44 | RX data bit 1 | I2S frame sync to the Pi |
| 20 | 43 | RX data bit 2 | I2S data to the Pi |
| 21 | 42 | RX data bit 3 | I2S data from the Pi |
| 25 | 31 | RX sample-ready to the Pi | unused |

The FPGA is configured by passive serial from GPIO 13 (DATA0), 24 (DCLK)
and 27 (nCONFIG), with CONF_DONE on 22 and nSTATUS on 26. FPGA pins 54
and 55 are clock inputs and input-only. Unused pins are inputs with weak
pull-ups. Board-side pins (AD9866, PA lines, jack) are in each project
file; the board itself is described in
[hardware-discovery.md](hardware-discovery.md).

## Control: framed SPI

SPI mode 3, 12.5 MHz, 8-byte frames each way, most significant bit first,
each ending in a CRC-8. Every frame reads or writes one 32-bit register;
the reply carries the register's value, a status byte and a second status
byte. The frame layout and the status bits are in
[rb_frame.h](../../driver/kernel/rb_frame.h).

Every register is in board space (the frame's space bit set); radio
space is not used (writes ignored, reads 0). Clients speak Hermes-Lite 2
commands; the driver turns them into these registers
(`rb_hl2_translate()` in [rb_frame.h](../../driver/kernel/rb_frame.h)).

| Address | Name | Access | Meaning |
|---|---|---|---|
| 0x00 | ID | ro | 0x52425633 ("RBV3") |
| 0x01 | VERSION | ro | major in 31..16, minor in 15..0 |
| 0x02 | CAPS | ro | 3..0 receivers, 4 transmitter present, 11..8 link (1 I2S, 2 PIO), 15..12 FPGA (2 = 10CL025) |
| 0x03 | STATUS | rw | 0 PLL locked, 1 AD9866 set up, 2 radio running, 3 link enabled, 4 receive FIFO was full, 5 ADC hit full scale, 6 a frame had a bad CRC; write 1s to clear 4..6 |
| 0x04 | CONTROL | rw | 0 run, 1 restart (self-clearing), 2 PA allowed, 3 AD9866 digital loopback |
| 0x05 | KEYING | rw | 0 PTT, 1 transmit permission; clears itself unless rewritten within about a second |
| 0x06 | LINK | rw / ro | Pi 4: 0 enable, 15..8 bit clock divider (122.88 MHz / divider), 23..16 and 29..24 sampling point of the data from the Pi. Pi 5: reads 0 |
| 0x07 | CRC_ERR | ro | frames with a bad CRC |
| 0x08 | FRAMES | ro | frames accepted |
| 0x09 | FIFO_FULL | ro | times the receive FIFO refused a frame |
| 0x0a | LINK_TX | ro | words (Pi 4) or values (Pi 5) sent to the Pi |
| 0x0b | LINK_RX | ro | transmit samples received from the Pi |
| 0x0c | AD_READ | rw | write 4..0 = AD9866 register to read; reads 31 busy, 7..0 value |
| 0x0d | SCRATCH | rw | |
| 0x0e | AD_WRITES | ro | AD9866 register writes made |
| 0x0f | SEQ_GAPS | ro | Pi 4: transmit samples whose sequence number skipped; Pi 5: 0 |
| 0x10 | TX_OVF | ro | transmit samples dropped, FIFO full |
| 0x11 | TX_UNDER | ro | radio clocks the core wanted a sample while transmitting and found none |
| 0x12 | STATUS2 | rw | as the reply's second status byte; write 1s to clear bits 3 and 4 (and 6 on the Pi 4) |
| 0x13 | TX_BAD | ro | Pi 4: frames whose flags failed their check; Pi 5: 0 |
| 0x14 | REALIGNS | ro | Pi 4: times the sample/flags word phase moved; Pi 5: 0 |
| 0x20 | RX_CONFIG | rw | 3..0 receivers less one, 9..8 rate (0 to 3: 48, 96, 192, 384 ksps) |
| 0x21 | TX_PHASE | rw | the transmitter's NCO phase step a 76.8 MHz clock (Hz x 2^32 / 76.8 MHz) |
| 0x22 + r | RX_PHASE | rw | receiver r's phase step |
| 0x30 | LNA | rw | 5..0 the AD9866 receive gain code |
| 0x31 | TX_CONFIG | rw | 7..0 drive, 8 PA enable, 9 T/R relay disable |
| 0x32 | TX_TIMING | rw | 6..0 TX start latency (ms), 12..8 PTT hang (ms; 31 holds until PTT drops) |
| 0x33 | CW_HANG | rw | 9..0 CW hang time |

The registers from 0x20 read back the last value written.

The radio core transmits only while CONTROL run and KEYING permission are
set and something keys it: KEYING PTT, or the key jack's key or PTT input
(debounced over 6 ms). KEYING clears itself unless rewritten, so a host
that stops ends every one of them. The PA lines additionally need CONTROL
PA allowed and loopback clear; envpa and envbias also need PA enable
(TX_CONFIG bit 8). CW from the jack's key has a raised-cosine
envelope, 4.1 ms up and down.

## Pi 5 link: RP1 PIO

Both buses are clocked by the Pi's PIO state machines; everything in the
FPGA changes on the rising edge of the Pi's clock. The FIFOs on the Pi's
side of each bus are never cleared, so the Pi may stop and start its
clocks at any edge.

**Receive** (GPIO 6 clock, 18 to 21 data, 25 sample-ready). Eight rising
edges a value. After the edge that ends a value, and after every edge
while idle, the data nibble is 0 and sample-ready says whether a value
follows; the Pi tests it before its next edge. If it was high, the next
edge puts the value's index in its frame on the data pins, and the six
after it the 24-bit value, most significant nibble first. The Pi reads
each 32-bit word as `index << 24 | value`. A frame is one sample period
for every receiver, I0, Q0, I1, Q1 and so on, so the index runs 0 to 2n - 1
and restarts after the last value and at a receiver-count change. At the
driver's default divider (RP1 PIO at 100 MHz) the bus carries 2.38 million
values a second.

**Transmit** (GPIO 4 clock, 5 data, 12 ready). Ready, registered on every
rising edge, says a whole word fits. The edge that finds the shifter idle
with ready high starts a word and carries no data; the 32 edges after it
carry the word, most significant bit first: I in 31..16, Q in 15..0, each
signed 16-bit. At the driver's default divider (25 MHz) a word takes about
9 us, twice the 48 000 samples a second needed.

## Pi 4 link: I2S

The FPGA drives the bit clock and frame sync; the BCM2711 PCM block is
clock and frame consumer. A frame is 64 bit clocks, two 32-bit words,
frame sync high for the first 32, most significant bit first. The FPGA
changes its outputs on the rising bit clock edge; the Pi samples on the
falling edge.

**To the Pi**, every word slot carries the next receive value with bit 31
set (`index << 24 | value` below it, as on the Pi 5), or 0 when there is
none. **From the Pi**, each transmit sample is a pair of words: the
sample (I in 31..16, Q in 15..0), then flags: bit 31 valid, 30..24 0x2a,
23..16 the XOR of the sample's four bytes and the sequence number's two,
15..0 the sequence number. The image finds the pair's word phase itself
and counts sequence gaps, failed checks and phase moves
([rtl/common/rbv_i2s.sv](../rtl/common/rbv_i2s.sv)). The bit clock is
122.88 MHz divided by the LINK register's divider; the driver picks the
divider from the receivers and the rate.
