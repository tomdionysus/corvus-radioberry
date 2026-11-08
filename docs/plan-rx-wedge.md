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
- A further receiver-count change while locked (1 to 2 to 1) does not
  recover it, although it asserts the same FIFO clear that a
  receiver-count change always asserts.
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
  Whether `rdata` is registered on `done` or is the live shift register
  has not been checked.

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

- 2026-11-08: Harness test "reads return whole frames for every receiver
  count": 7 lock-ups in 35 runs (about 20% per run, 3 to 4% per change).
  Nine Python command patterns, about 500 changes: 0 lock-ups. Driver
  restart with `load_gateware=0` while locked: no recovery. Gateware
  reload: recovery every time. Receiver-count change while locked: no
  recovery.

## 6. Decision

Open. Development resumes when a fix from section 4 is chosen, applied
and measured at 0 lock-ups over at least 500 changes of the triggering
pattern, or when the owner decides otherwise.
