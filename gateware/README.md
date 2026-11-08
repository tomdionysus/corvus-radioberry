# Gateware: what the driver depends on

Status 2026-11-08.

The FPGA runs the Hermes-Lite 2 gateware in its Radioberry PIO variants,
`radioberry_pio_cl025` and `radioberry_pio_cl016`, version 75.3, built in
this repository from the source in `source/`. `PROVENANCE.md` names the
source commit and the build; `QUARTUS.md` describes the toolchain and how
to obtain it; `tools/build-gateware.sh` builds `rpi-5/<type>/radioberry.rbf`.

Everything below is read from that source (paths relative to
`gateware/rtl/` in the archive) and from the upstream Pi 5 driver's PIO
programs. Statements marked *not yet measured* follow from the source and
have not been checked on hardware.

Only the PIO variants work with this project's driver. The Radioberry-2.x
repository also carries older GPIO-mode images (version 73.3: an 8-bit RX
bus and TX over a second SPI chip select); `rbprobe` recognises them and
reports them as unusable.

## 1. Variants

| | CL025 | CL016 |
|---|---|---|
| Device | 10CL025YE144C8G | 10CL016YE144C8G |
| Version | 75.3 | 75.3 |
| Receivers (`NR`) | 6 | 4 |
| Transmitters (`NT`) | 1 | 1 |
| CW (`CW`) | 1, basic: key and PTT from the jack | 1, basic |
| `FPGA_TYPE` reported | 2 | 1 |
| TX FIFO (`DSIQ_FIFO_DEPTH`) | 16384 bytes | 16384 bytes |

Source: `variants/radioberry_pio_cl0xx/radioberry.v` and
`radioberry/pi-pio/radioberry_core.v`. The receiver count is reported over
SPI (section 4) and is the authority at run time.

## 2. Configuration and reset

- Passive serial on GPIO 13 (DATA), 24 (DCLK), 27 (nCONFIG), read back on
  22 (CONF_DONE) and 26 (nSTATUS). Bytes least significant bit first. A
  bitstream for the other device leaves CONF_DONE low (measured on the
  development board, see `docs/platform.md`).
- After configuration `radioberry/reset_handler.v` holds the design in
  reset for 2^24 - 16 cycles of the 10 MHz internal clock, about 1.68 s.
  During reset the SPI slave is held in reset and answers with its reset
  pattern (all zero bits except the last byte, `0xff`), so the version
  reads as 0.255. The driver must wait at least 1.7 s after CONF_DONE
  before trusting the identity word, and should poll until the major
  version is non-zero. *Not yet measured.*

## 3. Clocks

- The AD9866 runs from the 76.8 MHz reference; `clk_internal` (command
  and control logic) is 10 MHz.
- Both stream clocks are driven by the Pi. The gateware never times out
  waiting for a Pi clock edge.

## 4. SPI command path

`radioberry/spi_slave.v`, instantiated with `WIDTH` 48, `mlb` 1:

- `spi0` chip select 0 only (`pi_spi_ce[0]`); CE1 is unused. SPI mode 3:
  the slave changes MISO on the falling edge and samples MOSI on the rising
  edge. 48 bits per transfer, most significant bit first. Every completed
  48-bit transfer is one command (`cmd_cnt` toggles on `done`), whether or
  not its content changed.
- Transfer bytes, Pi to FPGA:

  | Byte | Bits | Meaning |
  |---|---|---|
  | 0 | 47..43 | unused |
  | 0 | 42 | TX permission (`temp_enabletx`): gates every CN301 TX output |
  | 0 | 41 | CWX enable (CW keying from TX I/Q LSBs) |
  | 0 | 40 | run |
  | 1 | 39 | unused |
  | 1 | 38..33 | command address (Hermes-Lite 2 C0 bits 7..1) |
  | 1 | 32 | PTT (Hermes-Lite 2 C0 bit 0, MOX) |
  | 2..5 | 31..0 | command data (Hermes-Lite 2 C1..C4) |

  Bytes 1 to 5 are therefore exactly a Hermes-Lite 2 Protocol 1 C0..C4
  command block, and byte 0 carries run, CWX and the TX permission bit.
  **PTT travels in every command**, so any command sent with bit 32 set
  keys the transmitter (subject to section 6).
- Reply bytes, FPGA to Pi, captured when the transfer starts:

  | Byte | Meaning |
  |---|---|
  | 0 | bit 2: external CW key (jack tip, debounced); bit 0: `cw_on` or external PTT; others 0 |
  | 1, 2 | 0 |
  | 3 | bits 7..6 transmitters, 5..2 receivers, 1..0 FPGA type (1 = CL016, 2 = CL025) |
  | 4 | gateware major version (75) |
  | 5 | gateware minor version (2) |

- Command address 0x00, data bits 6..3, is the number of receivers minus
  one. Changing it clears the RX FIFO and restarts the receivers. The rest
  of the command set is the Hermes-Lite 2 one
  (https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol); address
  0x09 bit 19 is PA enable and bit 18 T/R relay disable, as used in
  section 6; address 0x17 bits 6..0 are the TX buffer latency and 12..8 the
  PTT hang time (section 7).

## 5. RX stream (FPGA to Pi)

`radioberry/pi-pio/rx_pi_pio.v` and the `usiq_fifo` in `fifos.v`:

- Pins: data GPIO 18 to 21 (D0 to D3, four bits wide), clock GPIO 6 (Pi
  output), sample-ready GPIO 25 (FPGA output). State changes on the rising
  edge of the Pi's clock; the Pi samples the data while the clock is low.
- One 24-bit value takes eight clocks: a start nibble (0, with
  sample-ready high, emitted only when the FIFO holds more than 12
  values), then the metadata nibble, then the 24 data bits as six nibbles
  most significant first.
- The metadata nibble is the index of the value within the current frame
  (0, 1, 2 ...), reset to 0 after the frame's last value. A frame is one
  sample period for all receivers in the order I0, Q0, I1, Q1, ... and
  holds 2 x receivers values, so the index runs 0 to 2n - 1 (at most 11 on
  the CL025). Values are signed 24-bit two's complement.
- The upstream PIO program (`rb2-rx-stream.c`) toggles the clock while
  waiting, jumps on sample-ready, clocks seven nibbles (metadata and data)
  and autopushes at 28 bits, shifting left. Each 32-bit word the Pi reads
  is `index << 24 | sample`, top four bits zero.
- The RX FIFO holds 1024 values; the gateware stops writing (drops) above
  1012. With upstream clocking (PIO clock divider 2, so 100 MHz from the
  200 MHz RP1 clock, five PIO cycles per nibble) one value takes 42 PIO
  cycles: at most about 2.38 million values per second. One receiver needs
  2 x rate values per second, so the upstream program carries six
  receivers at 192 ksps (2.30 M/s) but only three at 384 ksps. *Not yet
  measured.*

## 6. CN301 outputs and what keys the transmitter

`radioberry/control.v` and `radio_openhpsdr1/radio.v`:

- `int_tx_on = (tx_on | ext_ptt) & run & tx_permission`, where `tx_on`
  comes from the radio's transmit state machine, `ext_ptt` is the debounced
  jack ring (PTT on the PA board's MIC jack), `run` is SPI bit 40 and
  `tx_permission` is SPI bit 42. `ext_ptt` also feeds the radio's transmit
  state machine.
- `exttr` (CN301 pin 3, `ptt_out`) = `int_tx_on`.
- `inttr` (CN301 pin 4) = `int_tx_on & (pa_enable | ~tr_disable)`.
- `envpa` (CN301 pin 8) and `envbias` = `int_tx_on & pa_enable`.
- `pa_enable` and `tr_disable` latch from command address 0x09, data bits
  19 and 18, and keep their value until the next 0x09 command.
- The radio leaves its idle state on any of: SPI PTT (bit 32) together
  with valid TX FIFO data, which transmits the FIFO's samples; the CW key
  on the jack tip (`ext_keydown`, unconditional in the basic CW build),
  which transmits a CW carrier; CWX keying; external PTT on the jack ring,
  which holds the radio in its pre-transmit state with the DAC enabled and
  no samples flowing until one of the others follows. **None of these needs
  `run` or the TX permission bit.** Those two gate only the CN301 lines,
  which switch the T/R relay and bias the PA. A key on the jack therefore
  drives the DAC with the PA unswitched whenever the gateware is loaded.
  *Not yet measured.*
- Consequences for software: keep SPI bit 42 clear except while a client
  is allowed to transmit, clear bit 32 in every command unless MOX is
  wanted, and send address 0x09 with bit 19 clear whenever PA control is
  not allowed. External PTT on the jack is honoured by the gateware itself
  while bit 42 and run are set.

## 7. TX stream (Pi to FPGA)

`radioberry/pi-pio/tx_pi_pio.v`, the `dsiq_fifo` in `fifos.v` and the TX
state machine in `radio.v`:

- Pins: data GPIO 5 (Pi output), clock GPIO 4 (Pi output), ready GPIO 12
  (FPGA output, registered on the Pi's clock).
- With ready high, the rising clock edge that finds the shifter idle starts
  a word and carries no data; the next 32 rising edges carry 32 bits, most
  significant first. The upstream PIO program uses its ready-polling edge
  as that start edge, then pulls one 32-bit word and shifts it out.
- Word format: I in bits 31..16 and Q in bits 15..0, each signed 16-bit.
  The gateware stores four bytes per word, in order I high, I low, Q high,
  Q low.
- TX sample rate: 48 ksps into the gateware's interpolators, whatever the
  RX rate.
- The FIFO holds 16384 bytes (4096 samples, 85 ms at 48 ksps). Ready falls
  8 bytes before full. If it does fill, writes are dropped until it drains
  to a quarter (1024 samples) and a word boundary is reached.
- While not transmitting, the radio drains the FIFO at the sample rate and
  discards the samples. On entering transmit it stops reading for the TX
  buffer latency (default 20 ms, address 0x17) to fill the FIFO.
- Gateware watchdog: in transmit, if the FIFO runs dry for the PTT hang
  time (default 12 ms, address 0x17 bits 12..8) the radio returns to idle
  by itself. A hang time of 31 disables this and transmit then ends only
  when PTT is cleared. Software must never send 31 there.
- Upstream PIO throughput: divider 8 (25 MHz), seven cycles per bit,
  about 233 cycles per word: at most about 107 000 words per second, more
  than twice the 48 ksps needed. *Not yet measured.*

## 8. Keyer and jack inputs

- Jack tip (`io_phone_tip`): CW key, debounced on the 1 ms tick; keys CW
  directly in the basic CW build.
- Jack ring (`io_phone_ring`): external PTT, debounced; feeds `ext_ptt` in
  `control.v` (section 6).
- `io_cwl` and `io_cwr` are passed straight through to `pi_cwl` and
  `pi_cwr` for a Pi-side keyer such as piHPSDR's.
