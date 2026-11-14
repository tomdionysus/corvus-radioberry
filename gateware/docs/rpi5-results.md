# rpi5-pio results

Measurements made with the `rpi5-pio` image on one RadioBerry (10CL025,
PA5W board) on a Raspberry Pi 5, driver transport `rp1-pio` with the
framed control protocol, packages built from commit 6052f18. The image's
design is in [rtl/rpi5](../rtl/rpi5); the wire it speaks is the released PIO
image's ([interfaces.md](interfaces.md), Pi 5 link).

## The image

- The `rpi4-i2s` image's control registers, framed SPI protocol, AD9866
  set-up, keying watchdog and PA logic, and the Hermes-Lite 2 radio core
  (six receivers, one transmitter), unchanged.
- Receive wire ([rbv_pio_rx.sv](../rtl/rpi5/rbv_pio_rx.sv)) and transmit
  wire ([rbv_pio_tx.sv](../rtl/rpi5/rbv_pio_tx.sv)) run on the Pi's own RX
  and TX clocks; the receive FIFO's read side and the transmit FIFO's
  write side do too. Neither FIFO is ever cleared and nothing in the Pi
  clock domains is reset, so a Pi clock may stop and start at any edge.
  The released image cleared its receive FIFO asynchronously in that
  domain, which is what locked it up
  ([docs/fault-rx-lockup.md](../../docs/fault-rx-lockup.md)).
- Pins as the released PIO image: RX clock GPIO 6 (FPGA 54), data GPIO 18
  to 21 (33, 44, 43, 42), sample-ready GPIO 25 (31); TX clock GPIO 4
  (55), data GPIO 5 (32), ready GPIO 12 (49). The RX nibble, sample-ready
  and TX ready leave from the I/O-cell registers.
- CAPS 0x00002216: six receivers, transmitter, transport 2 (RP1 PIO),
  FPGA 2 (10CL025).

## Simulation

`tools/sim.sh rpi5`: the image with a radio-core stand-in, against a model
of two RP1 PIO state machines that executes the driver's own instruction
words ([rb_pio.h](../../driver/kernel/rb_pio.h)) with the PIO's two-flop input
synchronisers, 3 ns from the Pi to the FPGA and 11 ns back. All checks
pass:

| Check | Result |
|---|---|
| RX stream, 2 receivers (1.2 M values/s) | in order, no frame lost |
| 6 pauses of 20 to 25 us (the driver's RX hold) | in order, no frame lost |
| Pause of 1.2 ms (FIFO fills) | whole frames refused, order kept |
| Receiver count 2 to 3 with the clock running | in order |
| Receiver count 3 to 1 with the clock held | in order |
| Program restart mid-value, 5 times | in order within 10 us each time |
| Run off and on | in order |
| TX, 64 words at the driver's divider (8) | every word exact |
| TX faster than the core drains | FIFO fills, ready holds the Pi, 0 overflows |

## Timing

Quartus 23.1std.1, seed 1. Every clock meets setup and hold in every
model. RX clock pin to the RX data and sample-ready pins: required within
15 ns of the Pi's edge reaching the FPGA, slack +5.2 ns (slow 85 C). The
AD9866 TX bus under the strict constraint: bits 0 to 3 and 5 and TX sync
+0.06 to +0.25 ns; bit 4 (pin 105) -2.72 ns, the same as the `rpi4-i2s`
build ([hardware-discovery.md](hardware-discovery.md) section 4).

## Receive

Daemon `radioberryd`, client `ft8rx`, 40 m FT8, antenna connected:

| Protocol | Slots | Decodes | Packets | Lost |
|---|---|---|---|---|
| 1 | 3 | 4 | 18 896 | 0 |
| 2 | 3 | 8 | 10 740 | 0 |

The released image on the same board and antenna just before: 6 decodes
in 2 slots, 0 lost. With no client the image's count of values sent did
not move over 10 s (the RX clock is stopped). `rbdrivertest`: 12 of 12
test cases. Driver unit tests: 43 of 43. RX stalls: 0.

## Transmit

Each keyed run below was a single run with the board owner's go-ahead.
Frequency 14.120 MHz, +1 kHz tone, 3 s keyed, daemon stopped, tools
writing `/dev/radioberry` directly.

Digital loopback (TxDAC and IAMP powered down, PA lines held low), drive
0, tone 0.49 of full scale:

| Measurement | Result |
|---|---|
| Unkeyed | exact zero |
| +1 kHz tone | -18.7 dBFS |
| Opposite sideband | -144.7 dBFS |
| Carrier | -138.2 dBFS |
| +-3 kHz, +7 kHz | -130.9 to -153.7 dBFS |
| Image overflows, underruns | 0, 0 |

Into a 50 ohm dummy load through the PA5W, drive 128 (AD9866 gain code
20), PA enable clear, tone 0.8 of full scale: 1.4 W on the owner's meter
(the `rpi4-i2s` image: 1.35 W). Image overflows and underruns 0; keying,
transmitting and PA allowed all back to 0 after the run.

On air through piHPSDR and WSJT-X, 20 m FT8, PA allowed, drive ceiling
150 (drive 150, then 115):

| Measurement | Result |
|---|---|
| Transmissions | 41, 549.7 s in all |
| Samples to the gateware | 26 363 460 (47 960 a second) |
| TX blocks dropped | 0 |
| Image overflows, underruns | 0, 0 |
| RX stalls | 0 |
| PSK Reporter spots, first 4 minutes | about 240: Europe, North America, the Canaries, Japan, Korea, eastern Russia |
| Contacts | an FT8 contact |

## Open

- The Pi 5 receives its own transmission much stronger than the Pi 4 does
  with the same tools: 25 dB in loopback (-18.7 against -43.6 dBFS) and
  48 dB keyed into the dummy load (-16.9 against -64.9 dBFS), while the
  output power matches. Cause not known.
- Not tested: the Pi 4 on this driver build (it shares the identity check
  and the reload handling); a receiver-count stress run with the driver's
  RX hold turned off (`rx_clock_hold_us=0`).
