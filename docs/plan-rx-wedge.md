# Subplan: the receive stream lock-up

Status 2026-11-08. Main development (`TODO/ACTIVE.md`, sections 2 to 6)
is paused until this subplan reaches a decision. The driver test harness
that exposes the fault is parked on branch `item/2-driver-harness`,
unmerged.

## 1. The fault

With the PIO gateware (Hermes-Lite 2 `radioberry_pio_cl025` 75.3, built
here from unmodified source) the FPGA sometimes stops delivering receive
samples and does not resume.

Established on the development board:

- When it happens, sample-ready (GPIO 25) stays low while the Pi's RX
  clock (GPIO 6) keeps toggling. The SPI control path stays alive:
  the identity reply is correct and commands are accepted.
- The fault is in the FPGA, not in the driver, PIO or DMA: unloading and
  reloading the module with `load_gateware=0` does not recover it;
  reloading the gateware (sysfs `reload`) does.
- Trigger seen so far: the harness test "reads return whole frames for
  every receiver count" (opens the device once, then for 1 to 6
  receivers sends the general command and one frequency command per
  receiver, reads for 250 ms, and closes). It locked the FPGA in 7 of 35
  runs, roughly 3 to 4% of receiver-count changes, in one case on the
  first command after open.
- Nine other command patterns, driven from Python at a similar pace
  (receiver counts stepping 1 to 6 and alternating 1/6, 1/2, 2/3; open
  and close around each change; run cleared and set again; the PA word
  with run set), made about 500 changes with no lock-up. With a 3% rate
  per change that outcome has a probability well under one in a million,
  so the harness does something necessary that is not yet identified.
- A further receiver-count change while locked (1 to 2 to 1, and a
  later one with the device held open and reading) does not recover it,
  although it asserts the same FIFO clear that a receiver-count change
  always asserts. Only a gateware reload recovers.
- While locked, the TX path keeps running: 16384 zero samples written
  with PTT clear drained within 0.6 s, as at 48 ksps (four cases). The
  AD9866 clock domain is alive; the fault is in the receive chain or
  its FIFO.
- Every captured lock-up begins at the receiver-count command itself
  (no RX words after it), with the same bytes and the same 30 to 40 us
  command spacing as the hundreds of changes that did not lock up.
- Recovery ladder, run on nine lock-ups with the device held open and
  reading: receiver-count change 1 to 2 and back, rate change 48 to 96
  ksps and back, `run` cleared for 1 s then set, count change while
  `run` was clear, LNA command, PA word zero. None restarted the
  stream in any of the nine. Only the gateware reload does.
- The owner ran upstream's GPIO-mode gateware (73.3) on this board
  without noticing any such fault. The PIO variant has no known-good
  reference run anywhere.

Source facts from `gateware/source/`, relevant but not yet shown to be
causal:

- Both variants buffer receive samples in `usiq_fifo` (`rtl/fifos.v`),
  an Intel `dcfifo` with an asynchronous clear driven by every
  receiver-count change (`reset_channels`) and by the gateware reset. The
  GPIO variant reads it on the FPGA's own `clk_ad9866`; the PIO variant
  reads it on `pi_rx_clk`, the clock the Pi drives in. The FIFO
  instantiation names a parameter `wrsync_aclr_synch`; whether Quartus
  recognises that name has not been checked.
- In both variants the receiver-count comparison that fires
  `reset_channels` runs in the 10 MHz domain on `cmd_addr` and `cmd_data`,
  which are assigned combinationally from the SPI slave's `rdata`.
  `rdata` is registered: `spi_slave.v` writes it only on the 48th SPI
  clock, so the comparison never sees a half-shifted command.
- The generated FIFO netlist (Quartus map pass, `build-gateware/diag/`)
  has no clear synchroniser on either side: `rdptr_g[].clrn` and
  `wrptr_g[].clrn` are `!aclr` directly, and the source's parameter
  `wrsync_aclr_synch` appears nowhere in it. The documented Intel
  parameters are `write_aclr_synch` and `read_aclr_synch`, default OFF;
  the source's name is ignored without a warning. Every FIFO pointer,
  synchroniser stage and flag register is reset by `aclr`, so a
  corrupted FIFO would be repaired by the next clear.
- `rst_all` into the radio block is tied to 0: the receivers' decimators
  and FIR are never reset after configuration. `run` is used in
  `radio.v` only by the transmit state machine and does not gate RX.
- `rx_pi_pio.v`, the Pi-clocked state machine that reads the FIFO and
  drives the nibble stream, has no reset of any kind; the FIFO's flags
  it waits on (`us_tvalid`, `us_tlength`) are changed asynchronously to
  its clock by the clear. Quartus infers latches for its `us_stream`
  output (map log warning 10240). The project leaves "Safe State
  Machine" off and state machine processing on "Auto"; the encoding
  Quartus chose for `state` is not shown in the map report and has not
  been confirmed (the timing report names the bits `state.START`,
  `state.RXMETA`, which is how Quartus names one-hot bits, but that
  inference is unverified).
- The timing constraints assume a 4.8 MHz Pi RX clock; the upstream PIO
  program and ours drive 20 MHz. The report's `pi_rx_clk` paths still
  fit a 25 ns half period (data delays 6 to 10 ns). The outputs to the
  Pi are false-pathed.

Nothing in this section identifies the cause. Everything in section 3
is designed to.

## 2. Rules for this work

- Receive only. No command sets PTT (bit 32) or the TX permission bit
  (bit 42). Where TX samples are written they are zero, with both bits
  clear, which `CONTRIBUTING.md` allows without permission.
- The gateware source is not modified for any shipped image.
- No JTAG adapter or other diagnosis hardware is available, and none
  will be. Everything is observed from the Pi: the SPI reply, the
  stream pins through `pinctrl`, the driver's counters and the
  behaviour of rebuilt images. The plan is written for that.
- Results are recorded in section 5 as measured rates with the number of
  trials, dated. A hypothesis is written down as a hypothesis until an
  experiment in section 3 has confirmed or refuted it.
- Nothing from this work is merged into `develop` until section 4 has a
  decision, except this document and read-only tooling.

## 3. Diagnosis

Each step names what it measures and what each outcome would mean. A
"run" of the harness test is one open with 6 receiver-count changes;
with the lock-up at about 20% per run, 100 changes per configuration
give a usable rate. One lock-up costs 5.5 s to recover.

### Phase 1: read-only checks, no hardware

1. `rtl/radioberry/spi_slave.v`: is `rdata` registered on `done`, or
   the live shift register? Live: the 10 MHz receiver-count comparison
   sees half-shifted bits during every SPI transfer and can fire
   spurious FIFO clears, so the spacing of consecutive commands matters
   (the harness sends them about 30 us apart, Python more slowly).
   Registered: spacing is not a mechanism.
2. The Quartus map report of our build (`tools/build-gateware.sh` keeps
   the logs): does it warn about an unrecognised `wrsync_aclr_synch`?
   Warned: the clear is unsynchronised on both FIFO sides in the built
   image. Not warned: the write side is synchronised as the source
   intends.
3. The Intel FIFO IP user guide, the section on `aclr` and the
   `write_aclr_synch` and `read_aclr_synch` parameters, read before any
   further statement about what an unsynchronised clear can do.

### Phase 2: make the lock-up observable and cheap to reproduce

4. The driver's `debug` parameter logs every command with a timestamp.
   Record the exact command bytes and inter-command timing of the
   harness and of the Python loops, and diff them. This replaces guessing
   about what the harness does differently.
5. Driver (diagnostic build, not merged): a dead-stream detector, no RX
   words for 100 ms while run is set, that logs once and counts. A
   lock-up is then detected in 100 ms instead of a 15 s timeout.
6. Test loop: 100 changes per configuration, reload on lock-up, rate and
   trial count written to section 5.

### Phase 3: when locked, find which clock domain is dead

7. Write zero TX samples with PTT clear and watch `tx_words`. The TX
   FIFO drains at 48 ksps on the same `clk_ad9866` domain the receivers
   use. TX still drains: the ADC clock domain is alive and the fault is
   in the RX chain or its FIFO. TX stops too: the AD9866 clock or its
   PLL has stopped, which points at reset or clock handling, not at the
   FIFO.
8. Also when locked: the SPI reply byte 0, the sysfs identity, and GPIO
   6, 25 and 18 to 21 sampled over a few seconds with `pinctrl`.

### Phase 4: one-variable experiments

9. Harness with a 1 ms pause between SPI commands. Rate falls to zero:
   command spacing is the trigger (consistent with step 1 "live"). Rate
   unchanged: it is not.
10. Harness with the general command only, no frequency commands after
    it. Isolates whether the commands that follow the count change
    matter.
11. Driver stops the RX state machine before any receiver-count command
    and restarts it 1 ms after, so the Pi's RX clock is static while the
    FIFO clear is asserted and released. Rate falls to zero: the
    Pi-driven read clock is involved. Unchanged: it is not.
12. A Python loop made to match the harness exactly according to the
    diff from step 4, to reproduce from a second tool and confirm the
    trigger is in the command stream, not in the harness process.
13. Seed sweep: rebuild the unmodified source with fitter seeds 1 to 8
    (about 4 minutes each; the source stays unmodified, so no decision
    is reopened) and measure the rate on at least three images. Rate
    varies strongly with seed: a placement-sensitive crossing in the
    gateware. Rate constant: a logic bug or a protocol-level trigger.

### If phases 1 to 4 do not settle it

There is no internal visibility into the FPGA. The remaining evidence
would then be the rates from steps 9 to 13 and whichever domain step 7
showed dead. The way forward in that case is fix 2 (detect and reload)
as the stopgap so development can resume, with the lock-up kept as an
open item, and fix 4 considered only for a change whose effect can be
measured as a rate difference between two images built from the same
source.

## 3a. Working hypothesis (2026-11-08, unconfirmed)

Stated as a hypothesis, with the experiment that tests it. The one
block in the receive path that nothing resets and whose inputs change
asynchronously at exactly the moment the lock-ups start is the
`rx_pi_pio` state machine. If a receiver-count clear changes the FIFO
flags within the setup or hold window of a Pi RX clock edge, the state
register can capture an illegal value; with no reset and no safe-state
recovery, an illegal one-hot vector would leave it asserting nothing,
so it never reads the FIFO again, the FIFO fills to its 1012-word
threshold, the assembler waits on `rx_tready` for good, and only
reconfiguration recovers. This fits every fact in section 1: TX alive,
clears and resets ineffective, onset at the clear, a rate of a few per
thousand clears (a sub-nanosecond window against a 50 ns clock), and
no such fault in the GPIO variant, whose FIFO read side ran on the
FPGA's own clock with no Pi-clocked state machine.

Test (step 11): the driver stops the RX state machine, so the Pi's RX
clock is static, from before a receiver-count command until 200 us
after it (`rx_clock_hold_us`, diagnostic driver 0.3.1). With no clock
edge while the flags change, the hypothesis predicts zero lock-ups;
the result is in section 5. Confirmation would also need the encoding
check above and, ideally, a rate that varies with fitter seed
(step 13).

## 4. Fixes, in order of preference, each gated on a finding

1. Driver avoids the trigger (command spacing, ordering, or RX clock
   state during the command), if phase 4 shows one. No gateware change.
2. Driver detects a dead stream and reloads the gateware itself,
   reporting a fault to userspace. A stopgap only: it costs 5.5 s of
   dead air per event, and the daemon would have to tolerate it. Not a
   substitute for 1, 3 or 4.
3. A different fitter seed, if step 13 shows placement sensitivity. The
   source stays unmodified; `gateware/PROVENANCE.md` records the seed
   and the measured rates.
4. A gateware source change, last resort, only with a mechanism
   confirmed by section 3 and the owner's explicit decision to reopen
   the standing decision "built unmodified". The change would then be
   specific to the confirmed mechanism, versioned so `rbprobe` and
   clients can tell the image apart, and documented in
   `gateware/PROVENANCE.md`.

## 5. Results

Dated entries, newest first. Each gives the configuration, the number
of changes or runs, the number of lock-ups, and what it rules in or out.

- 2026-11-08, driver 0.3.1 (diagnostic: stall detector in sysfs
  `rx_stalled`, `debug=2` traces every command, `cmd_gap_us`). Harness
  test "reads return whole frames for every receiver count", one run =
  6 receiver-count changes, lock-up detected 200 ms after the change:
  - cold (30 C), `debug=2`: 0 in 50 runs, then 6 in 100 runs (576
    changes, about 1% per change);
  - cold, `debug=0`: 1 in 50;
  - SoC 42 to 47 C (CPU burners), `debug=0`: 2 in 50;
  - SoC 46 to 49 C, `debug=0`, 100 us minimum gap between SPI commands:
    2 in 50.
  - SoC 46 to 49 C, `debug=2`: 3 in 50;
  - cold, `debug=2`, recovery ladder on each lock-up: 9 in 150 runs,
    0 recovered by any command (section 1);
  - cold, `debug=2`, RX clock held still across each receiver-count
    command (`rx_clock_hold_us=200`): 0 in 150 runs (900 changes),
    run immediately after the 9-in-150 ladder loop on the same driver.
    Under the measured 6% per-run rate the chance of 150 clean runs is
    about 1 in 10000. This is the predicted outcome of the hypothesis
    in section 3a and already exceeds the 500-change acceptance in
    section 6.
  Rules out command spacing as the trigger and moderate SoC temperature
  as a strong factor; the morning's 7 in 35 is not reproduced. Thirteen
  lock-ups with the TX drain test: TX alive in all (section 1).

- 2026-11-08: Harness test "reads return whole frames for every receiver
  count": 7 lock-ups in 35 runs (about 20% per run, 3 to 4% per change).
  Nine Python command patterns, about 500 changes: 0 lock-ups. Driver
  restart with `load_gateware=0` while locked: no recovery. Gateware
  reload: recovery every time. Receiver-count change while locked: no
  recovery.

## 6. Decision

Open, awaiting the owner. Position at the end of 2026-11-08: the fault
is in the PIO gateware's receive read-out, in state that no command
resets (section 3a, unconfirmed as to the exact register but the only
candidate left by elimination). The driver-side avoidance, holding the
Pi's RX clock still across each receiver-count command, measured 0
lock-ups in 900 changes against 9 lock-ups in 900 without it. Proposed
way forward, for the owner to approve:

1. Make the RX clock hold the driver's normal behaviour (fix 1): on in
   the shipped module, 200 us, documented in the README and
   `docs/plan-v1.md`; keep the stall detector and its sysfs attributes
   as a permanent fault indicator; keep `cmd_gap_us` out (ruled out).
   Merge from `item/2-rx-wedge-diag` after a final acceptance run of
   at least 500 changes with the shipped defaults and the usual gate.
2. Resume development; the parked harness branch rebases on it and
   its own acceptance (20 consecutive passes) becomes meaningful.
3. Record the gateware's defects (section 1) as an open gateware item:
   unreset Pi-clocked `rx_pi_pio` state machine with inferred latches,
   FIFO clear unsynchronised on both sides, `wrsync_aclr_synch`
   silently ignored. Whether to fix them at source is the owner's
   decision and reopens "built unmodified"; the driver avoidance does
   not depend on it.
4. Optional confirmation, not needed for 1 to 3: the `state` encoding
   from a Quartus fitter report, and a seed-sweep rate comparison. Development resumes when a fix from section 4 is chosen, applied
and measured at 0 lock-ups over at least 500 changes of the triggering
pattern, or when the owner decides otherwise.
