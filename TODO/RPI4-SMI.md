# Raspberry Pi 4: RX over SMI with DMA

Plan for a Pi 4 RX transport that reads the RadioBerry's RX bus with the
BCM2711's Secondary Memory Interface (SMI) and DMA instead of clocking it
from the CPU. Tracked in [ACTIVE.md](ACTIVE.md), section 2.

## Why

The Pi 4 transport clocks the RX bus from a SCHED_FIFO kernel thread
([driver/kernel/rb_bitbang.h](../driver/kernel/rb_bitbang.h)). Each 24-bit
value takes 8 clock pulses (sample-ready, then the index nibble and six
data nibbles), each pulse a GPIO set, clear and level read: 48 register
accesses per I/Q sample. At one receiver at 192 ksps (384 000 values per
second) that is about 9.2 million accesses per second and a whole CPU
core, which starves the client's own DSP. The driver's limit,
`bb_rx_values_max` 192 000, is two receivers at 48 ksps or one at 96
([docs/rpi4.md](../docs/rpi4.md)); a client's rate cannot be refused, so
192 ksps runs above it.

A user report: on a Pi 4, piHPSDR and Quisk at 192 ksps held one core at
100% and the others above 50%, and a WSJT-X WSPR decode took about a
minute. The original software on the same Pi ran 192 ksps at 30 to 50%
per core with the decode in about 10 s. The original uses its own
GPIO-mode gateware: an 8-bit bus read on both clock edges, 12 register
accesses per I/Q sample, with an interrupt on sample-ready.

Reproduced on the Pi 4 with 0.4.1 and piHPSDR at one receiver at 192
ksps: the driver's thread at 100% of one core, the other cores at about
20%, no samples lost ([docs/testing.md](../docs/testing.md), Raspberry
Pi 4). The access count is not what fills the core: 63% of the RX
clocks at that rate find no value ready, so data takes about a third of
the core and polling the rest, because the thread rests only when a
whole run finds nothing. A CPU transport that rests between bursts, with
the gateware's RX FIFO holding the values meanwhile, might cost about a
third of a core at 192 ksps. Not tried; it is cheaper than SMI and
needs no overlay, so measure it before step 2.

When the Pi 4 support was merged, SMI was recorded as not needed because
the limit stayed at 192 000 values per second
([COMPLETED.md](COMPLETED.md), 2026-11-10/11).

## The fit

| RX bus signal | GPIO | SMI function |
|---|---|---|
| RX clock | 6 | read strobe (SOE_N) |
| D0 to D3 | 18 to 21 | SD10 to SD13 |
| sample-ready | 25 | SD17 |

- With 18-bit reads, each strobe stores one word holding that clock's
  nibble and ready bit; DMA moves the FIFO to memory and the CPU touches
  no GPIO register while streaming.
- SMI strobes at a fixed rate and cannot wait for sample-ready. The
  gateware takes clocks with sample-ready low as idle (the CPU transport
  already gives them, counted as `rx_idle_clocks`), so the parser drops
  idle words: find ready high, take the next seven nibbles, build
  `index << 24 | value` as now.
- The strobe rate must exceed eight times the value rate: at least 3.1
  million per second for one receiver at 192 ksps.
- Only the pins switched to the SMI function change; SPI, the TX bus
  (GPIO 4, 5, 12) and the rest stay as they are. TX stays CPU-clocked.
- Nothing found records the pinout as designed for SMI: neither the HL2
  gateware history nor the original Pi 4 sources mention it. The PIO
  pinout keeps the original GPIO-mode bus's clock (6), ready (25) and data
  pins 18 to 20.

Expected (estimates, not measured): the CPU cost at 192 ksps falls from a
core to a small fraction, parsing words in RAM instead of GPIO accesses;
capacity is bounded by the strobe rate and the gateware, not the CPU.

## Unknowns

Each is answered by step 1 below, before any driver work.

1. Strobe timing: where in the strobe cycle SMI latches the data
   relative to the gateware's rising-edge advance, at which strobe rates
   the words are clean, and whether the clock's idle level changing when
   SMI takes GPIO 6 gives a spurious edge (the parser resyncs on
   sample-ready; check that it does).
2. Kernel support: whether the Raspberry Pi kernel's `bcm2835-smi` driver
   and its `smi` and `smi-dev` overlays exist and work, with DMA, on both
   6.12 and 6.18 `rpt-rpi-v8` kernels. Not yet verified.
3. Word layout: how 18-bit reads land in memory, which sets the buffer
   size and the parse cost.

## Overlay

A fragment naming the SMI block fails to merge onto the Pi 5 tree
([docs/design.md](../docs/design.md), Transports), and DMA and the SMI
clock need device-tree entries, so the Pi 4 needs its own overlay or one
set in a `[pi4]` section of `config.txt`. The quickstart, `rbprobe`,
`tools/check-overlay.sh` and the packaging change with it. This is the
larger part of the work.

## Steps

1. Measurement from userspace, no driver change: on the Pi 4 with the
   `smi` and `smi-dev` overlays, an `rbbus`-style tool sets the SMI
   timing, reads buffers through `/dev/smi`, parses them and reports
   parse errors, resyncs and values per second across strobe rates and
   receiver counts. Run it on a 6.12 and a 6.18 kernel. Acceptance: clean
   words at 384 000 values per second or more, with the CPU cost measured.
2. Driver: a Pi 4 SMI RX transport beside the CPU one, chosen at probe;
   double-buffered DMA periods as on the Pi 5; RX clock hold and run
   gating by stopping the DMA; the parser shared with the existing RX
   path and tested against the gateware simulator. The Pi 5 path
   unchanged.
3. The Pi 4 overlay and its handling in the docs, `rbprobe`, CI and the
   packages.
4. Raise `bb_rx_values_max` to the measured figure and update
   [docs/rpi4.md](../docs/rpi4.md).

Acceptance for the transport: `rbdrivertest` passes; `ft8rx` over
Protocol 1 and 2 with no packets lost; one receiver at 192 ksps through
`radioberryd` with no RX blocks dropped over 10 minutes and the driver's
CPU cost measured; the 11-kernel matrix on the Pi 4; the Pi 5 rechecked
on the radio.
