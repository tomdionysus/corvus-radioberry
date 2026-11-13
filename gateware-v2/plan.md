# Plan: gateware projects

Two gateware images from one radio core, one per host board. Background:
[Hardware discovery](hardware_discovery.md),
[Evaluation of the current gateware source](gateware_evaluation.md),
[Interface strategies: Raspberry Pi 4](interface_strategies_rpi4.md),
[Interface strategies: Raspberry Pi 5](interface_strategies_rpi5.md).

## Decided

- The Raspberry Pi 4 and the Raspberry Pi 5 run different gateware.
- Raspberry Pi 5: the present PIO interface, with the gateware behind it
  cleaned up.
- Raspberry Pi 4: a new I2S interface.
- Each board gets the greatest capability its hardware allows; neither
  constrains the other.
- The radio never appears as a sound card. On the Raspberry Pi 4 the
  driver programs the PCM block itself; ALSA and the stock I2S driver are
  not used, for the radio or for testing it.

Not pursued: SMI, a stream over SPI, and one image serving both boards.
They stay described in the strategy documents.

Nothing here is built. Figures marked unverified are measured by the link
test image before the project that depends on them starts.

## 1. What can and cannot be remapped

The board fixes which FPGA pin each header wire reaches. The gateware
chooses what each wire carries and in which direction.

| Wires | Pi GPIO | Constraint |
|---|---|---|
| General, either direction | 5, 12, 18, 19, 20, 21, 25 | none |
| SPI0 | 8, 9, 10, 11 | commands, in both images |
| Pi to FPGA | 4, 6 | FPGA pins 55 and 54, probably input-only clock pins |
| After configuration only | 13, 24 | shared with the loader |
| Never | 22, 26, 27 | configuration pins |
| Unproven | 7, 16, 17, 23 | schematic only |

## 2. Structure

```
gateware-v2/
  rtl/core/           radio core, AD9866 interface and set-up, reset, PA lines
  rtl/control/        SPI0 command and status module
  rtl/transport/pio/  Raspberry Pi 5 streams
  rtl/transport/i2s/  Raspberry Pi 4 streams
  projects/rpi5-pio/  top level, pins, constraints
  projects/rpi4-i2s/  top level, pins, constraints
  projects/linktest/  test image
  sim/                testbenches and host-side models
```

- **Core.** The Hermes-Lite 2 radio (`radio.v` and its DSP) and the
  AD9866 sample interface, reused. Replaced around it: the AD9866 set-up
  module, the reset and the PA line logic.
- **Contract.** Four ports in the radio's clock domain: receive stream
  out, transmit stream in, command in, status out. Every clock crossing
  and FIFO sits inside a transport or the control module.
- **Control.** One SPI0 module for both images.
- **Simulation.** Each transport has a model of its host side; each
  project a testbench that passes known samples both ways and injects a
  stray clock edge.

The driver chooses the image by board, as it already chooses its
transport.

## 3. Shared work: core and control

Fixes from the evaluation that both images get.

| Finding | Fix | Changes what the host sees |
|---|---|---|
| T1, drive not written after a load | The set-up module always writes the host's gain and starts from the lowest | no |
| T2, SPI framing never recovers | Bit count reset when chip select rises | no |
| T4, PA lines combinational across clock domains | Registered in one domain, low out of reset | no |
| T5, PA enable does not gate the PA board | For the owner to decide: one explicit permission gating all four lines | yes, if changed |
| T6, PTT unsynchronised | Synchronised into the radio's clock | no |
| R1 to R3, reset | Waits for PLL lock, released synchronously in each domain, and can be requested by a command | a new command address |
| R5, debounce | Corrected | no |
| H6, receiver count unbounded | Limited to the receivers built | no |
| H7, AD9866 commands dropped | Queued until the serial port is free | no |
| H8, the host is blind | Reply bytes 1 and 2, zero today, carry overload, TX FIFO level and underrun, PLL lock and a fault flag | only in bytes that are zero now |
| H1, GPIO 13 driven from both ends | The paddle pass-through leaves pin 13; the driver releases GPIO 13 after configuration | driver change |

## 4. Project: link test image

Not a radio. Leaves the AD9866 untouched and the PA lines low; cannot
transmit.

- Drives and reads every header wire one at a time: proves or rules out
  GPIO 7, 16, 17 and 23, and shows which FPGA pins are input-only.
- Sends a counter as I2S on GPIO 18 to 21 at each bit clock from 3.072
  to 24.576 MHz, and takes one back.
- Host side: a measurement kernel module that programs the PCM block and
  a cyclic DMA transfer in each direction itself, as the SMI measurement
  module did for SMI. It becomes the base of the driver's transport.
- Acceptance, on a Raspberry Pi 4: the highest bit clock that records
  with no gap for 10 minutes each way; the slot widths and frame lengths
  the PCM block accepts as slave; what the block does when the FPGA's
  clock stops and restarts; the CPU cost of the stream.

## 5. Project: rpi5-pio

The present wire protocol and pins, unchanged, so the released driver
keeps working while the gateware behind it is replaced.

| Function | GPIO | Direction |
|---|---|---|
| RX clock | 6 | Pi to FPGA |
| RX data 0 to 3 | 18 to 21 | FPGA to Pi |
| RX sample-ready | 25 | FPGA to Pi |
| TX clock | 4 | Pi to FPGA |
| TX data | 5 | Pi to FPGA |
| TX ready | 12 | FPGA to Pi |

Stage 1, no host change:

- Receive read-out: registered outputs, no latches, a state machine with
  a reset, the FIFO clear synchronised into the Pi's clock, the frame
  index reset with it (H2 to H5). The driver's clock hold becomes
  unnecessary and stays harmless.
- Transmit intake: the word count resets when the TX clock has been
  still for a set time, so a slipped bit is cleared at the next gap (T3).
  The TX FIFO is cleared by reset.
- Timing constraints written for the clocks the Pi drives: 20 MHz
  receive, the PIO transmit rate.
- The shared fixes of section 3.
- Acceptance: the release gate's tests with the released driver; the
  lock-up harness with the clock hold disabled, 0 lock-ups in 750 runs
  against 17 in 1800 for the present image; the capacity figure of 2.37
  million values per second kept.

Stage 2, with a driver change, only if wanted after stage 1:

- A frame line or marker on transmit, so a slipped bit costs one sample
  at once.
- FPGA-paced receive with 32 data bits per word, if more than six
  receivers at 192 ksps is wanted.

## 6. Project: rpi4-i2s

The FPGA is clock master; the Pi is slave. One wire each way.

| Function | GPIO | Direction | Pi 4 function |
|---|---|---|---|
| Bit clock | 18 | FPGA to Pi | PCM_CLK |
| Word select | 19 | FPGA to Pi | PCM_FS |
| Receive data | 20 | FPGA to Pi | PCM_DIN |
| Transmit data | 21 | Pi to FPGA | PCM_DOUT |
| Attention (optional) | 25 | FPGA to Pi | GPIO interrupt |

GPIO 4, 5, 6 and 12 carry nothing.

- Frame: two 32-bit slots. Receive: 24-bit I and 24-bit Q; the eight
  spare bits of each slot carry receiver index, overload and a sequence
  count. Transmit: 16-bit I and Q; spare bits carry keying.
- Bit clocks 3.072, 6.144, 12.288 and 24.576 MHz, integer divisions of
  the PLL's 245.76 MHz. Several receivers share the wire by raising the
  frame rate.
- No ready line, no host clocking, no fill-level logic: the FPGA takes
  one transmit sample per frame.
- Capacity, measured with the link test image and DMA
  ([Link test results](linktest_results.md)): loss-free both ways at a
  61.44 MHz bit clock for 10 minutes, idle and under full load. That is
  61.4 Mbit/s each way on one wire. Six receivers at 192 ksps are
  55.3 Mbit/s of 24-bit values, but the first receive image sends one
  value per 32-bit word (24 bits of value, a valid bit and the index),
  1.92 million values a second; with a quarter kept spare the driver
  offers 1.536 million: four receivers at 192 ksps, two at 384, six at
  96. Six at 192 needs a denser word (four values in three words). The
  FPGA must sample the Pi's data about two bit periods after its own
  clock edge at 61.44 MHz.
- Host: a third transport in the driver beside `pio` and `cpu`, which
  programs the PCM block's registers and runs cyclic DMA in both
  directions through `dmaengine`. A Raspberry Pi 4-only overlay gives the
  driver the PCM block and its pins and keeps the block's node disabled,
  so the stock I2S driver never binds it and no sound card exists.
  `/dev/radioberry` and the daemon are unchanged. A stopped bit clock
  (FPGA not configured, or reloading) is a normal state the transport
  waits through.
- Acceptance: FT8 receive over both protocols with no samples lost; a
  keyed test into a dummy load with the owner present; the stream's CPU
  cost under 5% of one core at one receiver at 192 ksps; the desktop
  responsive during a transmission.

## 6a. First milestone: the receive chain on the Raspberry Pi 4

The full RX/TX chain with transmit stubbed: an installed driver and
daemon taking receive from the `rpi4-i2s` image to piHPSDR and WSJT-X,
on this project's code path only (and the Hermes-Lite 2 radio core).
Every place transmit will plug in exists and is inert.

Decided (owner, 2026-11-13):

- Code outside `gateware-v2/` changes additively on this branch: the
  Raspberry Pi 5 `pio` path and the Raspberry Pi 4 `cpu` fallback stay
  unchanged in the diff.
- The bit clock is chosen per receiver configuration: the lowest that
  fits receivers x rate. Bring-up starts fixed at 61.44 MHz.
- Six receivers, as the present image.
- SPI0 carries a framed protocol of this project's own: space (radio or
  board), address, 32-bit data, CRC-8; every register readable,
  including a shadow of each Hermes-Lite 2 command; keying in a register
  of its own. The driver translates the daemon's 6-byte Hermes-Lite 2
  blocks into it; the daemon's command path does not change. The format
  lives only between the driver and the image they ship together, so it
  can be changed later in one driver function and one gateware module.

### Gateware (`projects/rpi4-i2s`)

- Hermes-Lite 2 radio core with six receivers, its transmit path
  present and fed nothing, CW keying from the jack compiled out.
- New around it: AD9866 set-up that always writes the requested value;
  clock start-up (start on 38.4 MHz, program the AD9866, wait for
  76.8 MHz and PLL lock, then release the radio); a reset the host can
  request; PA lines registered and held low.
- SPI0 module for the framed protocol, with read-back and status
  (ADC overload, PLL lock, transport state).
- I2S transport, FPGA clock master on GPIO 18 to 21: receive values as
  `index << 24 | value` words with a valid flag, idle slots invalid; the
  bit clock set by a board register; the Pi-to-FPGA wire clocked and
  sampled, its data discarded.
- Transmit held off in the gateware itself: the keying register has no
  effect in this build, TX quiet and the PA lines stay low.

### Driver

- A third transport, `i2s`, beside `pio` and `cpu`: the PCM block driven
  directly, cyclic DMA both ways, received words handed to the shared
  parser and frame ring. `rx_hold` and `rx_pause` stop the receive side.
- Transmit stub: the TX DMA sends zeros; words written to
  `/dev/radioberry` are counted and discarded.
- Chosen on a Raspberry Pi 4 only when the Pi 4-only overlay
  `radioberry-i2s.dtbo` gives the driver the PCM block, which stays
  disabled so no sound card exists. Loads `radioberry-rpi4-i2s.rbf`.
- The command ioctl translates the 6-byte blocks into the framed
  protocol; the bit clock follows the receiver count and rate.
- The identity ioctl gains a "TX available" capability, clear for this
  transport.

### Daemon and tools

- `radioberryd` refuses MOX when the capability is clear and says why;
  Protocols 1 and 2 otherwise run as now.
- `rbprobe` and `rbload` learn the image and transport.
- Packages carry the image and overlay.

### Acceptance, on the Raspberry Pi 4

1. Installed from packages built on the build host, as a user would.
2. `rbdrivertest` passes; `rbcat` clean at every rate from 48 to
   384 ksps and every receiver count up to six for 60 s each.
3. `ft8rx` over Protocols 1 and 2 with no packets lost.
4. piHPSDR and WSJT-X on the Pi 4's desktop decode FT8, with decodes
   matching PSK Reporter spots.
5. CPU cost measured against [the earlier finding](../docs/finding-rpi4-cpu.md).
6. MOX from piHPSDR is refused and logged; nothing on the transmit side
   moves (checked with the gateware's own status).
7. The Raspberry Pi 5 build and CI unchanged.

### Order

1. Gateware: framed SPI module and I2S receive transport, simulated
   against the driver's parser; then the image with the radio core.
2. Driver transport and overlay, tested with `rbcat` and `rbdrivertest`.
3. Capability flag, daemon gate, tools.
4. Packages, install, acceptance.

## 7. Order

1. Link test image on a Raspberry Pi 4. Independent of everything else.
2. Core, control module and simulation.
3. rpi5-pio stage 1.
4. rpi4-i2s, once the link test has given its figures.

Steps 1 and 2 can run side by side.

## 8. Decisions needed

- Whether PA enable should gate every PA line (T5).
- Whether the 10CL016 stays supported (two more images).
- Whether the reused Hermes-Lite 2 files may be edited or only wrapped.
  Removing the unused VNA scanner needs an edit to `radio.v`; the rest of
  this plan works by wrapping.
- How many receivers the Raspberry Pi 4 image should build. The link
  carries six at 192 ksps; the limit is now the FPGA, which the present
  six-receiver design fills.
