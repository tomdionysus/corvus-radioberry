# Raspberry Pi 4: RX over SMI with DMA

Plan for a Pi 4 RX transport that reads the RadioBerry's RX bus with the
BCM2711's Secondary Memory Interface (SMI) and DMA instead of clocking it
from the CPU. Tracked in [ACTIVE.md](ACTIVE.md), section 2.

## Why

On a Pi 4 the driver's thread clocks the RX bus and holds one CPU core at
100% from one receiver at 96 ksps up, most of it polling for the next
value. Reported by a user at 192 ksps and reproduced:
[docs/finding-rpi4-cpu.md](../docs/finding-rpi4-cpu.md).

## The fit

| RX bus signal | GPIO | SMI function |
|---|---|---|
| RX clock | 6 | read strobe (SOE_N) |
| D0 to D3 | 18 to 21 | SD10 to SD13 |
| sample-ready | 25 | SD17 |

- With 18-bit reads, each strobe stores one word holding that clock's
  nibble and ready bit; DMA moves the words to memory and the CPU touches
  no GPIO register for RX.
- SMI strobes at a fixed rate and cannot wait for sample-ready. The
  gateware takes clocks with sample-ready low as idle (the CPU transport
  already gives them), so the parser drops idle words: find ready high,
  take the next seven nibbles, build `index << 24 | value` as now.
- The strobe rate must exceed eight times the value rate: at least 3.1
  million per second for one receiver at 192 ksps.
- Only the RX pins change to the SMI function. SPI and the TX bus (GPIO
  4, 5, 12) stay as they are, and TX stays clocked by the thread, which
  then sleeps whenever there are no TX words.
- Nothing found records the pinout as designed for SMI.

## Known about the kernel

On all 11 tested `rpt-rpi-v8` kernels (6.12.25 to 6.18.60) the Pi 4 has
the modules `bcm2835_smi` and `bcm2835_smi_dev` and the overlays `smi`
and `smi-dev`. The kernel headers carry
`include/linux/broadcom/bcm2835_smi.h`, and `bcm2835_smi` exports
`bcm2835_smi_get`, `bcm2835_smi_set_regs_from_settings`,
`bcm2835_smi_read_buf` and `bcm2835_smi_user_dma`. None of it has been
run.

## Unknowns

Step 1 answers each before any driver work.

1. Strobe timing: where in the strobe SMI latches the data relative to
   the gateware's advance on the rising edge; at which strobe rates the
   words are clean; whether the clock's idle level changing when SMI
   takes GPIO 6 gives a spurious edge (the parser resyncs on
   sample-ready; check that it does).
2. Continuity: whether the strobes run without a gap from one DMA
   transfer to the next. The gateware's RX FIFO holds 1012 values, 2.6 ms
   at one receiver at 192 ksps, so any gap must be shorter and the strobe
   rate high enough to catch up after it.
3. Word layout: how 18-bit reads land in memory, which sets the buffer
   size and the parse cost.
4. Whether the `smi` overlay's pin claims (it takes the whole SMI pin
   set) leave SPI, I2C and the TX and control pins usable, or a narrower
   pin group is needed.

## Keeping the Pi 5 working

The Pi 5 path is the working product and nothing in this item may change
its behaviour.

- Additive only: a third `struct rb_transport` (`smi`) beside `pio` and
  `cpu`. No edit to any `rb_pio_*` function. `rb_select_transport` keeps
  its `brcm,bcm2712` test first and unchanged; SMI is chosen only on
  `brcm,bcm2711` and only when the device tree gives the driver an SMI
  node.
- Shared code (`rb_rx_consume`, the rings, the stall check, the run
  gating, the RX clock hold's callers) changes only if SMI cannot be done
  without it, and each such change is listed in the commit and rechecked
  on the Pi 5 radio before the next step.
- `radioberry.dtbo` does not change: it must keep merging onto both
  boards' trees (`tools/check-overlay.sh`). SMI comes from a second,
  Pi 4-only overlay.
- The Pi 4's `cpu` transport stays, and stays the fallback: without the
  second overlay, or if SMI fails to start, the Pi 4 runs as it does in
  0.4.1.
- The SMI symbols must not become a load-time dependency on the Pi 5: its
  kernel has to load `radioberry.ko` whether or not `bcm2835_smi` exists
  there. Check on a `rpt-rpi-2712` kernel at step 2 before anything else;
  if the dependency cannot be made optional, the driver programs the SMI
  registers and the DMA channel itself.
- Gate at the end of every driver step, on the Pi 5 with the radio:
  `tools/ci-pi.sh` (unit tests, checkpatch, sparse, clang-tidy),
  `rbdrivertest`, `ft8rx` over Protocol 1 and 2 with no packets lost,
  idle with no client at 0% for the driver's thread. A failure stops the
  item until it is fixed.
- Before release: the 11-kernel matrix on both boards and the release
  gate on each (clean-down, install per the README, FT8 receive, the
  board owner's contact).

## Steps

One branch, `item/rpi4-smi`; each step is one or more commits that leave
both boards working.

1. Measurement from userspace, no driver change. On the Pi 4 with the
   `smi` and `smi-dev` overlays and the gateware loaded by `rbload`, a
   tool beside `rbbus` sets the SMI timing, reads buffers through
   `/dev/smi`, parses them and reports clean words, parse errors, resyncs
   and values per second across strobe rates, receiver counts and rates,
   with its CPU cost. Run on one 6.12 and one 6.18 kernel. It answers the
   four unknowns. Acceptance: clean words at 384 000 values per second or
   more for 10 minutes, or a recorded reason SMI cannot do it, in which
   case this plan stops and the finding says so.
2. Parser. `driver/kernel/rb_smi.h`: SMI words in, `index << 24 | value`
   words out, resync on sample-ready, counters for idle words and
   resyncs. Plain C with no kernel dependencies, as `rb_bitbang.h`;
   tested in the userspace driver test binary against captured buffers
   from step 1 and against the gateware simulator. No hardware, nothing
   the Pi 5 runs.
3. Transport. `rb_smi_transport` in the driver: `up` claims the SMI
   instance and a DMA channel, allocates the RX periods and starts the
   thread for TX; RX periods are parsed and handed to `rb_rx_consume` as
   the other transports do; `rx_hold` and `rx_pause` stop the transfer
   (and so the clock), `rx_release` and `rx_resume` restart it; `down`
   undoes all of it and is safe after a failed `up`. Sysfs `transport`
   reads `smi`. The RX clock hold around receiver-count changes and the
   stall check must work as on the other transports. Acceptance on the
   Pi 4 with the SMI overlay: `rbdrivertest` passes; and without the
   overlay the Pi 4 comes up on `cpu` and passes as before. Then the Pi 5
   gate.
4. Overlay and packaging. `radioberry-smi.dtbo` for the Pi 4 (SMI block,
   pin functions, DMA), built and installed by the packages and checked
   by `tools/check-overlay.sh` against the Pi 4 tree; `config.txt` takes
   it in a `[pi4]` section so one SD card image serves both boards.
   `rbprobe` reports the transport and, on a Pi 4 without the overlay,
   says how to enable SMI. The DKMS package builds on every tested
   kernel of both boards. Then the Pi 5 gate.
5. Capacity and documents. Measure the SMI transport's ceiling and CPU
   cost, set its `rx_values_max` from the measurement, and bring
   [docs/rpi4.md](../docs/rpi4.md), the quickstart, the driver, design
   and troubleshooting pages and the finding up to date. The quickstart
   makes SMI the Pi 4 default only after the acceptance below.

## Acceptance

- Pi 4 with SMI: `rbdrivertest` passes; `ft8rx` over Protocol 1 and 2
  with no packets lost; one receiver at 192 ksps through `radioberryd`
  for 10 minutes with no RX blocks dropped, and the driver's CPU cost
  measured and well under a core; piHPSDR at 192 ksps rechecked as in
  the finding.
- Pi 4 without the SMI overlay: as 0.4.1.
- Pi 5: the gate above passed after the last driver change, and the PIO
  transport's code unchanged in the diff.
- The 11-kernel matrix on both boards.
