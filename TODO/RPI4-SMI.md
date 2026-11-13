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

## Step 1 results (2026-11-13, Pi 4, kernel 6.18.60)

Measured with `tools/rbsmi` (`rbsmi`, polled reads from userspace, and
`rbsmi_dma`, a measurement kernel module with DMA;
[docs/tools.md](../docs/tools.md)), receive only, one receiver unless
given. Every run was checked frame by frame with the driver's parser.

- The kernel's `/dev/smi` cannot be used: `bcm2835_smi` reads only 8 and
  16 bits wide ("Unsupported data width for read"), and the RX bus needs
  SD17. Its DMA timed out. The stock `smi` overlay takes GPIO 2 to 25.
  So the driver programs the SMI registers and a DMA channel itself and
  uses no `bcm2835_smi` symbol, which also removes any load-time
  dependency on the Pi 5. A six-pin overlay
  (`tools/rbsmi/rbsmi-measure-overlay.dts`) leaves SPI, I2C and the TX
  pins alone.
- Layout and sampling: RWIDTH 2 gives 18-bit reads, one word per strobe,
  SD0 to SD17 in bits 0 to 17; only bits 10 to 13 and 17 ever toggle.
  `rb_smi.h` parses them with no resyncs or dropped words at one receiver
  at 48, 192 and 384 ksps and two at 192. Switching GPIO 6 to SMI made no
  stray value: 5 words discarded while the parser settled, as with the
  other transports.
- DMA: a cyclic `dmaengine` transfer from the SMI FIFO into coherent
  memory works, on the channel the SMI node names (`dmas`, request 4).
  The slave address must be the CPU physical address (0xfe60000c): the
  bus address fails in `dma_map_phys`.
- Strobe rate: timing 2,10,2,16 (setup, strobe, hold, pace) gives 8.9 to
  9.0 million strobes a second, clean; that caps the bus at about 1.12
  million values a second (8 strobes a value), so two receivers at 384
  ksps and six at 96 come out short of frames, without resyncs. Faster
  timings reached at most 10.6 million strobes a second with resyncs and
  a core held busy: not usable.
- Continuity: an SMI transfer has a length (SMIL) and the strobes stop at
  its end, and then so does the DMA. START written while a transfer is
  active is ignored (polled reads: a second START before the end gave N
  words, after it 2N). With DMA a transfer sometimes ends at its length
  and sometimes runs on (runs of 1 000 000-word transfers: ends every
  0.14 s in bursts, or none for 60 s), cause unknown; and an end does not
  fall on a DMA period boundary, so its last period never completes and
  no DMA callback follows. Restarting from the callback therefore stalls
  the stream (seen in 2 runs at about 330 s, and in bursts with short
  transfers). The SMI's own interrupt on DONE (CS bit 9, the SMI node's
  interrupt) restarts it: 1 000 000-word transfers, 421 restarts in 60 s,
  no stall, every frame (384 000 a second) with no resyncs or drops.
- The DMA callback finds the position from the transfer's residue, not by
  counting callbacks, which can arrive merged.
- CPU: the coherent buffer is uncached on the Pi 4, and reading it word
  by word cost about 19 ns a word (4 to 7% of the four cores at 384
  ksps). One bulk copy of each period into cached memory, then parsing
  there: all four cores together, idle baseline 0.05%, 20 s each, 1.13%
  at one receiver at 48 ksps, 0.53% at 192, 0.64% at 384, 1.33% at the
  1.12 million ceiling. The CPU transport takes 14 to 17% of one core at
  48 ksps and all of one at 96 and above.
- Not yet run on a 6.12 kernel.

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
- No `bcm2835_smi` symbol is used (step 1), so the module loads on the
  Pi 5 as now; the Pi 5 gate checks it.
- Gate at the end of every driver step, on the Pi 5 with the radio:
  `tools/ci-pi.sh` (unit tests, checkpatch, sparse, clang-tidy),
  `rbdrivertest`, `ft8rx` over Protocol 1 and 2 with no packets lost,
  idle with no client at 0% for the driver's thread. A failure stops the
  item until it is fixed.
- Before release: the 11-kernel matrix on both boards and the release
  gate on each (clean-down, install per the README, FT8 receive, the
  board owner's contact).

## Where it stands (2026-11-13)

Steps 1 to 4 are done on the Pi 4 (kernel 6.18.60); the acceptance on
the Pi 4 is met (below). Left: the Pi 5 gate on the radio, the 11-kernel
matrix on both boards, the release gate on each.

- Driver: `rb_smi_transport` as designed in step 3, with two findings
  from the hardware: the SMI's done interrupt restarts transfers (a
  restart from the DMA callback stalls, since a transfer's end does not
  fall on a period boundary), and RX hold and pause parse every word
  read before the stop and drop the value in progress on release (pause
  and resume otherwise gave one resync in 3 of 20 client sessions; none
  in 40 after).
- `RADIOBERRY_TRANSPORT_SMI` (3) in the ABI header, an added value only;
  `rbctl` prints `smi`; `rbprobe` advises the overlay on a Pi 4 running
  `cpu` and warns when the overlay is there but SMI did not start.
- Overlay `driver/radioberry-smi.dts`, built and installed with the
  first, copied by the package's postinst, checked by
  `tools/check-overlay.sh` on the Pi 4 tree; it does not apply to the
  Pi 5 tree ("can't find symbol 'smi'"), and the `[pi4]` section keeps
  the firmware from trying.
- CI: `rb_smi.h` and the measurement module under checkpatch; a TSan
  build beside ASan.
- Measured on the Pi 4 with the board ([docs/finding-rpi4-cpu.md](../docs/finding-rpi4-cpu.md#the-fix)):
  `rbdrivertest` passed (12 test cases, 2 runs, no stalls); `ft8rx` over
  Protocol 1 and 2 on 40 m, no packets lost; one receiver at 192 ksps
  through `radioberryd` for 10 minutes, none lost; the driver's thread
  4% under Protocol 1 (TX) and 0% under Protocol 2; piHPSDR at 192 ksps
  every core below 30%. Without the overlay the Pi 4 came up on `cpu`
  and passed `rbdrivertest` 7 times in 8 (the failure's test not
  captured), `ft8rx` with no packets lost.

## Steps

One branch, `item/rpi4-smi`; each step is one or more commits that leave
both boards working.

1. Measurement, no driver change: done on 6.18.60 (results above); the
   10-minute acceptance run is recorded there. Still to do: the same on
   a 6.12 kernel.
2. Parser: done. `driver/kernel/rb_smi.h`, plain C as `rb_bitbang.h`,
   tested in `rbsmi_tests` against the gateware simulator (sampling
   before each edge, as SMI does), split buffers, a lost value and a
   strobe rate too slow. Move or copy those tests into the driver test
   binary with step 3.
3. Transport. `rb_smi_transport` in the driver, built on what
   `rbsmi_dma` does: the SMI node from a phandle in the Pi 4 overlay, its
   registers mapped, its clock (125 MHz, assigned in the device tree)
   taken with `clk_prepare_enable`, its DMA channel, a cyclic transfer
   into coherent memory, the position from the residue, each period
   copied to cached memory before parsing, SMI transfers restarted by the
   SMI's done interrupt, a watchdog that counts and restarts a stalled
   stream, and timing 2,10,2,16. The SMI node
   stays disabled so `bcm2835_smi` does not bind it. RX periods are
   parsed and handed to `rb_rx_consume` as the other transports do;
   `rx_hold` and `rx_pause` stop the SMI transfer (and so the clock),
   `rx_release` and `rx_resume` restart it; `down` undoes all of it and
   is safe after a failed `up`. The thread stays for TX only. Sysfs
   `transport` reads `smi`. The RX clock hold around receiver-count
   changes and the stall check must work as on the other transports.
   Acceptance on the Pi 4 with the SMI overlay: `rbdrivertest` passes;
   and without the overlay the Pi 4 comes up on `cpu` and passes as
   before. Then the Pi 5 gate.
4. Overlay and packaging. `radioberry-smi.dtbo` for the Pi 4 (SMI block,
   pin functions, DMA), built and installed by the packages and checked
   by `tools/check-overlay.sh` against the Pi 4 tree; `config.txt` takes
   it in a `[pi4]` section so one SD card image serves both boards.
   `rbprobe` reports the transport and, on a Pi 4 without the overlay,
   says how to enable SMI. The DKMS package builds on every tested
   kernel of both boards. Then the Pi 5 gate.
5. Capacity and documents. Measure the SMI transport's ceiling and CPU
   cost (step 1 suggests about 1.1 million values a second), set its
   `rx_values_max` from the measurement, and bring
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
