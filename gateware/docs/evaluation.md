# Evaluation of the current gateware source

The gateware release 0.4.1 ran on the boards is the Hermes-Lite 2
RadioBerry PIO variant, version 75.3, built unmodified (its
[README](https://github.com/tomdionysus/corvus-radioberry/blob/v0.4.1/gateware/README.md)
and
[provenance](https://github.com/tomdionysus/corvus-radioberry/blob/v0.4.1/gateware/PROVENANCE.md)
in release 0.4.1). This is a reading of that source: what it is made of,
what is sound, and what is wrong or fragile. The hardware facts it rests
on are in [Hardware discovery](hardware-discovery.md).

## Method and limits

- Read in full: the top level and core (`radioberry.v`,
  `radioberry/pi-pio/radioberry_core.v`), `spi_slave.v`,
  `reset_handler.v`, `control.v`, `ad9866ctrl.v`, `ad9866.v`,
  `rx_pi_pio.v`, `tx_pi_pio.v`, `fifos.v`, `sync.v`, `debounce.v`,
  `radio_openhpsdr1/radio.v`, `receiver_nco.v`, the PLL parameters, the
  pin, project and timing constraint files.
- Not read line by line: the filter, CIC, CORDIC and NCO cores under
  `radio_openhpsdr1/` and `nco/`. They are shared with the Hermes-Lite 2
  and are judged here only by the radio working.
- Nothing was simulated and no new measurement was made. Each finding
  says whether it has been seen on hardware. "From source" means the
  behaviour follows from the code and has not been observed.
- File paths are relative to `gateware/rtl/` in the source archive.

## 1. Structure

| Block | File | Clock |
|---|---|---|
| Command receiver, 48-bit SPI slave | `radioberry/spi_slave.v` | SPI clock from the Pi |
| Start-up reset, 1.68 s | `radioberry/reset_handler.v` | 10 MHz |
| PA lines, jack inputs, millisecond ticks | `radioberry/control.v` | 10 MHz |
| AD9866 serial port and start-up registers | `radioberry/ad9866ctrl.v` | 10 MHz |
| AD9866 sample interface | `ad9866.v` | 76.8 and 153.6 MHz |
| Receivers, transmitter, transmit state machine | `radio_openhpsdr1/radio.v` | 76.8 and 153.6 MHz |
| RX FIFO (1024 values) and TX FIFO (16384 bytes) | `fifos.v` | 76.8 MHz on one side, a Pi clock on the other |
| RX read-out to the Pi | `radioberry/pi-pio/rx_pi_pio.v` | RX clock from the Pi |
| TX intake from the Pi | `radioberry/pi-pio/tx_pi_pio.v` | TX clock from the Pi |

One PLL takes the 76.8 MHz clock on pin 53 and makes 76.8 MHz, 153.6 MHz,
245.76 MHz, 10 MHz and 48 kHz. The Pi supplies three more clocks: SPI,
RX and TX. That is seven clock domains, four of them asynchronous to each
other.

Signal path, receive: AD9866, 12 bits at 76.8 MHz; three two-channel
mixers; per receiver a CIC by 5, a variable CIC by 40, 20, 10 or 5, and a
FIR by 8, giving 48 to 384 ksps (division by 1600 at 48 ksps); 24-bit I
and Q into the RX FIFO; four bits at a time to the Pi.

Signal path, transmit: 32-bit words from the Pi, one bit at a time, into
the TX FIFO; 16-bit I and Q at 48 ksps; FIR by 8, CIC by 200; CORDIC up
to the carrier; 12 bits to the AD9866.

## 2. What is sound

- The radio core is the Hermes-Lite 2's and works: FT8 received and
  transmitted, six receivers at 192 ksps on a Raspberry Pi 5, a clean
  transmit spectrum with piHPSDR's signal.
- The RX read-out frames itself. Every value starts with sample-ready and
  carries its index in the frame, so the host can detect and recover from
  a lost or extra clock.
- The TX intake has back-pressure (the ready line), and the transmit
  state machine has its own watchdog: it leaves transmit when the TX FIFO
  stays empty for the PTT hang time.
- The six-receiver build fits the 10CL025 and meets its timing
  constraints apart from one unmarked synchroniser path.
- The command set is the Hermes-Lite 2's, so Protocol 1 clients need no
  translation of meaning, only of transport.

## 3. Findings

Seen: observed on the hardware. From source: follows from the code, not
observed.

### Transmit safety

| # | Finding | Where | Consequence | Status |
|---|---|---|---|---|
| T1 | The TX gain is written to the AD9866 only when it differs from the module's copy, which starts at 0 and is not reset. The start-up sequence, which runs after every gateware reset, writes 0x7f to the gain register | `radioberry/ad9866ctrl.v` | After a load, drive 0 to 3 is never written: the AD9866 keeps the start-up gain, above the gain of full drive. A host cannot set low power without first sending a different drive | Seen: a keyed transmission at drive 0 gave full output. Cause from source |
| T2 | The SPI slave counts 48 clock edges and never resets its count when chip select rises | `radioberry/spi_slave.v` | One extra or missing SPI clock edge, or one transfer of another length, misaligns every later command for good. Command bits land in other fields, PTT, run and TX permission among them. Only reloading the gateware recovers | From source |
| T3 | The TX intake counts one start edge and 32 data edges per word and has no other framing. The TX FIFO is never cleared, not even by the gateware reset | `radioberry/pi-pio/tx_pi_pio.v`, `fifos.v` | One extra or missing TX clock edge shifts every later sample by a bit, for good: full-scale noise in place of the signal. Only reloading recovers | From source. It would explain transmit data that looks corrupt when another fast signal runs beside the TX clock on the header; not tested |
| T4 | The four PA lines are combinational mixes of signals from three clock domains (`tx_on` from 76.8 MHz, run from 10 MHz, TX permission straight from the SPI register) | `radioberry/control.v` | Glitches on the lines that key the PA board are possible at any change of those signals | From source |
| T5 | PA enable gates `envpa` and `envbias` only. `exttr` follows the keyed state whatever PA enable says, and `inttr` does too unless T/R disable is also set | `radioberry/control.v` | PA enable does not stop the PA board transmitting | Seen once |
| T6 | PTT rides in every command and goes into the 76.8 MHz transmit state machine with no synchroniser | `radioberry_core.v`, `radio.v` | Any command sent with bit 32 set keys the transmitter. The missing synchroniser can at worst put the state machine in an unused state for a cycle, which returns to idle | From source |
| T7 | The transmit state machine is forced to idle while run is clear | `radio.v` (`tx_state <= run ? tx_state_next : NOTX`) | With run clear nothing drives the DAC: not PTT, not the key on the jack. With run set, the jack key drives the DAC without the TX permission bit, and only the PA lines are held off | From source. Section 6 of the [0.4.1 gateware README](https://github.com/tomdionysus/corvus-radioberry/blob/v0.4.1/gateware/README.md) says run is not needed; the source says it is |
| T8 | PTT hang time 31 disables the transmit watchdog | `radio.v` | Transmit then ends only when PTT is cleared | From source |

### Host interface

| # | Finding | Where | Consequence | Status |
|---|---|---|---|---|
| H1 | After configuration the gateware drives FPGA pin 13 (GPIO 13) with the level of the paddle input on pin 142, which has a pull-up. The driver leaves GPIO 13 as an output at 0 after configuring | `radioberry_core.v` (`pi_cwr`), the pin file; [driver/kernel/radioberry.c](../../driver/kernel/radioberry.c) | With no paddle contact closed, the FPGA drives the wire high against the Pi driving it low, all the time the radio runs | From source and driver code; not measured |
| H2 | The RX read-out's outputs to the Pi are unregistered, one of them through inferred latches, and the constraints exclude them from timing | `rx_pi_pio.v`, `timing.sdc` | The 20 MHz RX clock works by margin, not by analysis. The constraints assume 4.8 MHz | Seen working at 20 MHz |
| H3 | The RX FIFO is cleared asynchronously on every receiver-count change while the Pi's clock may be running. The parameter meant to synchronise the clear is not a real one, and the read-out state machine has no reset | `fifos.v` (`wrsync_aclr_synch`), `rx_pi_pio.v` | The read-out can lock up until the gateware is reloaded | Seen: [receive lock-up](https://github.com/tomdionysus/corvus-radioberry/blob/v0.4.1/docs/fault-rx-lockup.md). The driver holds its clock still across the command |
| H4 | The receiver-count comparison samples the SPI register's address and data in the 10 MHz domain on every cycle, with no synchroniser and without waiting for the command strobe | `radioberry_core.v` (`channels`) | A sample taken while the 48 bits change can read address 0 with other data: a FIFO clear the host did not ask for, on any command, so outside the driver's clock hold | From source. No lock-up has been seen with the hold on (0 in 750 runs), so it is rare if it happens |
| H5 | The read-out's frame index is not reset when the FIFO is cleared, and the first value after a clear is dropped (the read-out acknowledges a value before sending the next one) | `rx_pi_pio.v` | After a receiver-count change the first frame arrives short and wrongly indexed. The host must discard until a clean frame | Seen: the driver discards a few words at each change |
| H6 | The receiver count from command 0x00 is not limited to the receivers built | `radio.v` (`last_chan`) | A count above the build's gives frames with undefined values, and above eight receivers the 4-bit index wraps | From source |
| H7 | Commands to the AD9866 arriving while its serial port is busy (about 3.4 us per write) are dropped; the acknowledge is not connected | `ad9866ctrl.v`, `radioberry_core.v` | An RX gain and a TX gain command sent back to back can lose the second until the host repeats it | From source |
| H8 | The reply carries the version, receiver count and two key bits only | `radioberry_core.v` (`ret_info`) | No ADC overload flag (it is computed and dropped), no TX FIFO level or underrun flag (computed and dropped), no PLL lock, no command acknowledge, no read-back of any setting | From source |
| H9 | Both streams are clocked edge by edge by the Pi: eight clocks per 24-bit RX value on four wires, 33 clocks per TX sample on one wire | `rx_pi_pio.v`, `tx_pi_pio.v` | On a Raspberry Pi 4 the CPU spends a core on it from one receiver at 96 ksps up | Seen: [CPU cost of receive](https://github.com/tomdionysus/corvus-radioberry/blob/v0.4.1/docs/finding-rpi4-cpu.md) |

### Reset and clocks

| # | Finding | Where | Consequence | Status |
|---|---|---|---|---|
| R1 | The only reset is the start-up counter. It relies on register power-up values, does not wait for the PLL to lock, and cannot be triggered again | `reset_handler.v`, `radioberry_core.v` | Any state the gateware gets stuck in (T2, T3, H3) needs a reload, which takes about 2 s and resets the AD9866 | From source; the reload as the only recovery is seen for H3 |
| R2 | The reset is released without synchronisation into the SPI and TX clock domains, where it is an asynchronous reset | `radioberry_core.v` | Harmless while the Pi keeps those clocks still during the 1.68 s, as the driver does | From source |
| R3 | The PLL's lock output is unused | `radioberry_core.v` | The gateware runs and answers whatever the state of its clock | From source |
| R4 | Two logic signals are used as clocks: the SPI slave's `done` and the read-out's start state (the latch enable in H2) | `radioberry_core.v`, `rx_pi_pio.v`, `timing.sdc` | Paths from them are constrained by hand; the command strobe's first synchroniser stage shows negative slack in every build | Build report |
| R5 | The debounce's "stable" test is true whenever any history bit is set, where it should be true only when all are equal | `debounce.v` (`(&pb_history) \| (\|pb_history)`) | The key and PTT jack inputs are delayed by a millisecond and not debounced: contact bounce passes through | From source |

### Size and waste

| # | Finding | Where | Consequence |
|---|---|---|---|
| W1 | Six receivers use 83% of the logic, 76% of the memory and all 132 multipliers of the 10CL025 | fitter report | No multiplier is left for anything added |
| W2 | The VNA scanner is built although the variant sets `VNA` to 0: the `generate` that would leave it out is commented out | `radio.v` | Logic spent on an unused function |
| W3 | The 245.76 MHz envelope clock is generated and drives nothing | `radioberry_core.v`, `radio.v` (`LRDATA` 0) | A PLL output and a global clock spent |
| W4 | The radio's link inputs (`link_running`, `link_master` and others) are left unconnected | `radioberry_core.v` | The design depends on the tool tying undriven inputs low |
| W5 | The project compiles many files the design does not use (the QS1R and second receiver chains, I2C, external amplifier and tuner, the link), and lists most files twice | `files.tcl`, `radioberry.qsf` | Build noise only |
| W6 | The CL016 build misses the AD9866 TX output timing by up to 3.2 ns | [0.4.1 provenance](https://github.com/tomdionysus/corvus-radioberry/blob/v0.4.1/gateware/PROVENANCE.md) | Transmit quality on a 10CL016 is unknown |
| W7 | TX is fixed at 48 ksps and every receiver shares one sample rate | `radio.v` | A Protocol 2 host must resample its 192 ksps transmit stream |

## 4. Which findings matter most

1. T1, T2 and T3 can each put full power or noise on the air with the
   host believing otherwise, and T2 and T3 cannot be cleared without a
   reload. T1 has happened.
2. H1 is a standing electrical fault between the two boards if the source
   and the driver are read correctly. It can be removed today in the
   driver alone, by making GPIO 13 an input once the FPGA is configured.
3. H3 and H4 are the same weakness: a FIFO clear that is asynchronous to
   the Pi's clock. The driver's hold covers H3 only.
4. H8 leaves the host blind: none of the above can be detected from the
   reply except a misaligned SPI frame, which shows as a wrong version.
5. H9 is the cost the Raspberry Pi 4 pays.

## 5. What a replacement would have to settle

Not a plan; the points this evaluation raises for any new gateware.

- A reset the host can request, and a reply that reports faults.
- Framing that recovers: SPI resynchronised by chip select, TX words
  delimited so that a slipped bit costs one sample.
- Every setting written to the AD9866 from a known state, with the
  host's value always applied.
- PA lines registered in one clock domain, low by default, and gated by
  one explicit permission.
- No shared wire driven from both sides (GPIO 13).
- FIFO clears and resets synchronised into each clock domain they reach.
- A host interface that does not need the Pi to produce every clock
  edge. The wiring available for one is in
  [Hardware discovery](hardware-discovery.md): proven wires in section 3,
  and the further wires the link test proved (GPIO 7, 16, 17, 23) in
  section 10.
- The multiplier budget: six receivers already use all of them.
