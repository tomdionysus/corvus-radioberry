# Raspberry Pi 4: desktop lost while transmitting

Status: open. Found in the Pi 4 acceptance test of branch `item/rpi4-smi`
on 2026-11-13. Blocks the release.

## Symptom

piHPSDR and WSJT-X on the Pi 4's own desktop, used over VNC on the Pi's
Wi-Fi, SMI transport. On transmit the desktop stops responding, at 192
and at 48 ksps; MOX cannot be released from the desktop meanwhile, and
once the owner had to cut the power to end it. Stopping `radioberryd`
ended a transmission and the desktop recovered at once. The 0.4.0
acceptance test on the `cpu` transport had no lock-ups.

## The owner's assessment

The fault is in the TX path, interacting with the SMI and its DMA. The
owner also believes the transmitted data is corrupt. Both are the lines
to follow; the owner directs each step.

## What was measured

All on the Pi 4, kernel 6.18.60, gateware 75.3, SMI transport.

| Step | Build | Result |
|---|---|---|
| 1 | daemon with TX gated to MOX and batched sends (`6fc7958`), driver `c3c96c1` | desktop lost on every Protocol 2 transmission; piHPSDR printed `full_tx_buffer ... fexchange0: error`; one transmission dropped 58 TX blocks |
| 2 | code read | `rb_bb_tx()` reported busy while the gateware's TX FIFO was full, so the TX thread (SCHED_FIFO 50) looped on `cond_resched()`; Protocol 2's 100 ms TX latency keeps that 85 ms FIFO full |
| 3 | driver with that fixed (`f05d897`) | desktop still lost. During a transmission the TX thread used 55 to 70% of a core in `rb_bb_tx` (perf: 13.9% of all samples), the system 0 to 8% idle with 10 to 15 runnable |
| 4 | same, driver thread SCHED_OTHER, daemon `realtime: false` | desktop still lost. A normal-priority task on the Pi woke at most 5 ms late each second, CPU 17 to 60% idle; pings to the Pi's Wi-Fi from the workstation every 0.2 s lost or 0.5 to 4.8 s late from PTT on to PTT off, 2 to 14 ms before and after |
| 5 | morning packages (`0.4.1+git16.gc3c96c1`: driver `c3c96c1`, daemon before `6fc7958`) reinstalled | desktop still lost on transmit |
| 6 | same, no RF: `rbcat --rate 192 --seconds 15`, SMI receive only | Wi-Fi ping 90/90, slowest 9 ms |
| 7 | same, no RF: `rbcat --rate 192 --seconds 15 --tx-zeros` (TX words clocked by the CPU at 48 000 per second, PTT clear) | Wi-Fi ping 90/90, slowest 9 ms |
| 8 | same: `p1probe --rate 192 --mox --seconds 15` through `radioberryd` (PTT and T/R relay keyed, drive 0, PA enable clear, zero samples) | Wi-Fi ping 100/100, slowest 9 ms; no under-voltage; 48 120 TX samples/s, 0 TX blocks dropped |
| 9 | same, piHPSDR and WSJT-X calling on Protocol 2 | six transmissions of 13.6 to 13.9 s, 0 TX blocks dropped, WSJT-X starting one each period; pings lost for 6 s from PTT on in the first, 0.4 to 7 s late in the next four, lost from the sixth onward including the 17 s between transmissions, until the owner cut the power; wake-up lateness on the Pi 1 to 10 ms (one 20.7 ms); the kernel logged `sched: DL replenish lagged too much` once, in a 5 s ping stall |
| 10 | same, piHPSDR and WSJT-X, 192 then 48 ksps (owner) | ten transmissions from 14:32:15, 13.8 to 14.0 s, 47 633 to 48 024 samples/s; the first dropped 100 TX blocks, the rest none. The Pi's own ping to its router, each second: 1 to 11 ms outside transmissions; during each, lost in 1 to 4 seconds and 50 to 981 ms in others. Wi-Fi signal unchanged (-61 to -62 dBm), TX bitrate 72.2 Mb/s dipping to 65; throttle flags 0x0 throughout (no under-voltage) and no kernel message; wake-up lateness 1 to 16 ms, 44.7 to 79.5 ms in the second after PTT on. Owner: the desktop locked at 48 ksps too, and the TX data looked corrupt |
| 11 | released 0.4.1 (driver and daemon), `cpu` transport, SMI overlay removed, same config; piHPSDR and WSJT-X transmitting at 48 ksps and at the higher rate (owner) | owner: noticeable slowdown, but no lock-up of the desktop or the network at either rate |
| 12 | released 0.4.1, `cpu`, no RF, daemon stopped: `rbcat --rate 192` (a) alone, (b) with `--tx-zeros`, (c) with `--tx-zeros`, `stress-ng --cpu 4` and a stream out over the Wi-Fi | 12 s each, router ping every 0.2 s: 60/60 in each, slowest 128, 5 and 90 ms; (c) carried 7.3 MB/s over the Wi-Fi with every core busy; streams clean |
| 13 | SMI packages again, no RF: (a) to (c) as step 12, and (d) `p2probe --rate 192` through `radioberryd` with the same load | router ping 60/60 in each, slowest 7, 5, 101 and 69 ms; Wi-Fi stream 7.3 MB/s in (c) and (d); SMI DMA 545 interrupts a second in all four (one per period); CPU 0 in softirq 5.5%, 3.5%, 19% and 22%; streams clean, 48 000 TX words a second taken in (b) and (c) |
| 14 | SMI, `p2probe --mox --rate 192` through `radioberryd` for 24 s (PTT and T/R relay keyed, drive ceiling 0, PA not allowed, zero samples): (a) alone, (b) with the step 12 load | router ping 60/60 in each, slowest 11 and 105 ms; (b) carried 6.7 MB/s over the Wi-Fi; SMI DMA 545 interrupts a second; 47 997 and 47 991 TX samples a second, 0 TX blocks dropped |

Without a keyed transmitter the Wi-Fi shows single pings of 60 to 200 ms
now and then on both transports (steps 12 and 13).

The Pi reported under-voltage (`hwmon1: Undervoltage detected!`) during
the transmissions in steps 1 to 4; none in steps 8 and 10.

## Facts so far

- The Pi's network fails during transmissions with RF (steps 4, 9, 10),
  seen from the workstation and from the Pi itself, while the Pi's
  scheduling stays responsive.
- Not reproduced without RF: SMI receive with TX words clocked at
  48 000 per second (step 7), or PTT keyed at drive 0 with the PA off
  (step 8).
- Under-voltage was not present in step 10, which still failed.
- The `cpu` transport (released 0.4.1) does not lock up on transmit, at
  48 ksps or at the higher rate, on the same Pi, board, config and
  clients (step 11); it slows the system down. The lock-up comes with the
  SMI transport.
- The owner rules out Wi-Fi as the cause.

## From the recorded data

Read from the step 3 trace (perf, `/proc/interrupts` before and after,
`top`, 6 to 8 s of one transmission) and the step 10 per-second log.

- Step 10: while pings to the router were lost or 100 to 900 ms late,
  `iw station dump` still answered each second with the signal, the rate
  and `tx failed 0` unchanged. The network's data path stalls; the link
  statistics do not move.
- Every device interrupt is serviced on CPU 0: the SMI's DMA channel, the
  Wi-Fi's SDIO host (`mmc1`, `mmc-bcm2835` with its own DMA channel), v3d.
  The driver's TX thread ran on CPU 3.
- Step 3: `memcpy` took 5.9% of all samples (a quarter of one core),
  nearly all under `rb_smi_dma_done`, and `rb_smi_parse_to_locked` 1.8%;
  softirq time was 9.2% of the four cores. `rb_bb_tx` took 13.9%.
- Step 3: the SMI's DMA channel interrupted 7 752 times in the window.
  The window was not timed (it ended when `perf` had finished writing);
  at the 545 a second measured in step 13 it would have been 14 s, which
  the other counters allow. No sign from this that the DMA moved more
  words than the strobes give.
- Step 13 against step 3: the SMI tasklet costs 5.5% of CPU 0 receiving
  at 192 ksps on an idle system, and about a quarter of a core during
  the step 3 transmission.
- No `mmc`, `brcmfmac`, SMI stall or hold-leak message in any saved
  kernel log.
- Steps 6 to 8 differ from the failing runs in more than RF: zero TX
  samples, `rbcat` or `p1probe` as the client, and a ping as the only
  network traffic. They do not isolate RF.
- Code: on the `cpu` transport one thread clocks RX and TX in turn, so
  the two buses never toggle together; with SMI, GPIO 6 strobes at 9 MHz
  all the time while the thread clocks TX on GPIO 4 and 5. GPIO 5 and 6
  are neighbours on the header and GPIO 12 (TX ready) faces GPIO 6. TX
  clocking itself (`rb_bb_tx`) is the same code on both transports. No
  test sends TX data other than zeros and checks what the gateware took.

## Hypotheses

None is proven. In order of how well the data above supports them, as
ranked before steps 12 and 13. Step 13 then loaded the SMI transport
with every core busy, 7.3 MB/s over the Wi-Fi, CPU 0 at 22% softirq and
TX words clocked, without keying, and the network held: load alone does
not cause the stall, which weakens H1 and H3 as stated. What remains
needs the transmitter keyed: the keyed state itself, non-zero TX data,
the real clients, or RF.

- H1. The SMI's DMA tasklet (a 64 KB copy from uncached memory and a
  parse, per period, in softirq on CPU 0) overloads CPU 0 under transmit
  load and delays what else is serviced there, the Wi-Fi's SDIO
  completion among it. Not known: why its cost rises on transmit, and
  whether the DMA then moves more words than the strobes give.
- H2. RX strobes running while TX is clocked corrupt the TX stream
  (the owner's assessment). Explains corrupt TX data; needs a further
  link to explain the stall.
- H3. The SMI's DMA channel and the SDIO host's contend in the DMA
  controller or on the peripheral bus.
- H4. The TX thread's CPU use or priority. Step 4 and step 11 speak
  against it as the cause.
- H5. RF pick-up or supply dip. Step 11 has the same RF and supply
  without a lock-up; only possible as the further link of H2.
- Excluded: the daemon changes of `6fc7958` (step 5).

## Investigation plan

The owner directs each step. Every step that transmits needs the owner's
go-ahead at the time. The Pi under test runs the released 0.4.1; stages B
onward need the SMI packages (`0.4.1+git16.gc3c96c1`) and overlay back on
it. Each stage states what it decides; stop at the first that settles
the cause.

Done: stage A without transmissions (step 12) and stage B without the
extra counters (step 13). Neither reproduced the stall, so the next
steps are keyed, in this order, each one capture on SMI:

1. Done (step 14): `p2probe --mox --rate 192` through `radioberryd`
   under the step 13 load, PTT and the T/R relay keyed, drive 0, zero
   samples, PA not allowed. No stall: the keyed state, Protocol 2 and
   load together are not enough.
2. piHPSDR and WSJT-X transmitting with `allow_pa: false` and
   `drive_ceiling: 0`: the real clients and their TX data, no RF.
3. The same with the PA and drive as in step 10, into the dummy load.

The first of these to stall names what the stall needs; stages C to E
below then apply to that case.

### A. Baseline on the `cpu` transport (as installed, with transmissions)

The reference the SMI figures are compared with, and the measure of the
slowdown.

1. One capture script, used unchanged in every stage: for 10 s, `perf
   record -a -g`, `/proc/interrupts` and `/proc/softirqs` before and
   after, `mpstat -P ALL 1`, `top -H`, the per-second monitor (lateness,
   link statistics, router ping) and a 0.2 s ping from the workstation.
2. Capture: idle with no client; piHPSDR receiving; piHPSDR and WSJT-X
   through one transmission at 48 ksps.
3. Record: per-core user, system and softirq time; the TX thread's share;
   ping loss and delay.

### B. SMI without transmitting

Decides whether the tasklet's cost and the DMA's word rate are already
off before any transmission, and gives H1 its receive-only figures.

1. Add counters to the driver's sysfs (a debug build, not for release):
   DMA callbacks, words taken from the ring, time spent in the tasklet
   (total and longest), transfer restarts, watchdog restarts, hold leaks.
2. Capture with the script: idle; `rbcat` at 48 and 192 ksps; piHPSDR
   receiving at 48 and 192 ksps with WSJT-X decoding.
3. Compare words per second with the strobe rate (9 million), DMA
   interrupts per period, tasklet time per second, and CPU 0's softirq
   share with stage A.
4. Load without RF: piHPSDR and WSJT-X receiving plus `p1probe`-style MOX
   is not possible with one client, so instead add CPU and network load
   by hand (a `stress-ng` CPU load on all cores, an `iperf3` stream over
   the Wi-Fi) while `rbcat --tx-zeros` runs, and watch the ping. A stall
   here, with no RF and zero TX data, confirms a host-side cause (H1 or
   H3) and rules out H2 and H5 as necessary.

### C. SMI, one transmission, measured

Only if B did not reproduce the stall.

1. One transmission with piHPSDR and WSJT-X into the dummy load, the
   capture script and the stage B counters running.
2. Decides H1: words per second and tasklet time during the transmission
   against stage B; CPU 0's softirq share; whether `mmc1` interrupts and
   the SDIO DMA channel's interrupts stop during ping stalls.
3. If H1 holds, test it directly, one change per transmission: the DMA
   channel's interrupt moved to another core (`smp_affinity`); then the
   period lengthened (fewer, larger callbacks); then the parse moved out
   of the tasklet to the worker thread. The stall going away with the
   interrupt moved confirms H1.

### D. TX data integrity on SMI

Decides H2, independently of the stall. Parts 1 and 2 need no RF.

1. A TX test pattern that is not zero: `rbcat` (or a new option) sends a
   known pattern with PTT clear, on the `cpu` transport and on SMI.
2. Check what the gateware took. Find first whether the gateware has any
   read-back of TX words or of its TX FIFO's state over SPI; if not,
   compare the driver's TX word and clock counters with the gateware's
   ready behaviour (a slipped bit shows as words taken that do not match
   words sent, or a ready pattern that drifts), and consider a
   SignalTap or a spare-pin build that flags a word whose check bits fail.
3. With the owner's go-ahead: a two-tone or single-tone transmission into
   the dummy load on each transport, recorded with the HackRF through the
   attenuated tap, and the spectra compared (carrier, sidebands, noise
   floor, any products at 9 MHz offsets).
4. If corrupt on SMI only: repeat with the SMI strobes paused while TX
   words are clocked (a test build), which separates bus overlap from
   everything else.

### E. If neither H1 nor H2 holds

1. H3: the SDIO host forced to PIO (its `pio_limit`), or the Wi-Fi off
   and the wired port used, during one SMI transmission.
2. H5: the same SMI transmission with the PA disabled (`allow_pa: false`)
   and with drive 0 through piHPSDR.

### Afterwards

- The `cpu` transport's slowdown on transmit (step 3 and stage A: the
  TX thread's half core) is its own item, whatever the cause above.
- Whether `6fc7958` and `f05d897` stay on the branch.
- The Pi under test goes back to the released 0.4.1 after each session
  unless the owner says otherwise.

## Also found

- CPU-clocked TX costs 55 to 70% of a core for the length of a
  transmission (step 3); the TODO item for the `cpu` transport's timer
  sleep does not cover TX.
- A client that reconnects still holding MOX keys the transmitter at
  once: after the daemon restarted, piHPSDR, still in MOX, transmitted for
  6.6 s with nobody keying. The daemon should require MOX released before
  granting it to a new session.
- `6fc7958` (TX gating, batched sends) and `f05d897` (TX thread sleeps on
  a full FIFO) are on the branch; the Pi 4 runs the released 0.4.1.
  Whether they stay is decided when this item closes.
