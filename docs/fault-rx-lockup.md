# Fault: the receive stream lock-up in the PIO gateware

Status: worked around in the driver since 0.4.0 (2026-11-08); the
gateware defect itself is open (`TODO/ACTIVE.md`, section 5). This
document records what the fault is, what was measured, what was tried,
what was decided, how the workaround works, where its limits are, and
what a real fix in the gateware would have to change.

## 1. Summary

With the Hermes-Lite 2 Radioberry PIO gateware (75.3, `radioberry_pio_cl025`,
built here from unmodified source) the FPGA sometimes stops delivering
receive samples at a receiver-count change and never resumes. Only
reloading the gateware recovers it. The trigger is the asynchronous clear
of the receive FIFO that every receiver-count change asserts, taken by the
Pi-clocked read-out logic while the Pi's RX clock is running. The rate
depends on the fitter's placement (0 to 8 lock-ups per 150 test runs across
seven builds of identical logic) and is about 6% per run of the triggering
test with the shipped image.

The driver avoids the fault by stopping its RX PIO state machine, and with
it the clock it drives into the FPGA, from just before a receiver-count
command until 200 us after it. Measured: 0 lock-ups in 4500 changes
against 17 in 1800 without the hold, at no measurable cost to the stream.
A stall detector in the driver reports the condition in the log and in
sysfs should it ever occur anyway.

## 2. The fault as observed

All on the development board (10CL025, Raspberry Pi 5), receive only; no
command in this work set PTT or the TX permission bit.

- When it happens, sample-ready (GPIO 25) stays low while the Pi's RX
  clock (GPIO 6) keeps toggling. The SPI control path stays alive: the
  identity reply is correct and commands are accepted.
- Every captured lock-up begins at the receiver-count command itself: no
  RX words arrive after it. The command bytes and the 30 to 40 us spacing
  to the next command are the same as in the hundreds of changes that did
  not lock up.
- While locked, the TX path keeps running: 16384 zero samples written with
  PTT clear drained within 0.6 s, as at 48 ksps (13 cases). The AD9866
  clock domain is alive; the fault is in the receive chain or its FIFO.
- No command recovers it. A ladder run on nine lock-ups with the device
  held open and reading tried: receiver-count change 1 to 2 and back, rate
  change 48 to 96 ksps and back, `run` cleared for 1 s then set, a count
  change while `run` was clear, an LNA command, the PA word zero. None
  restarted the stream. A further receiver-count change asserts the same
  FIFO clear that triggers the fault, and does not clear it.
- Unloading and reloading the kernel module with `load_gateware=0` does
  not recover it; reloading the gateware (sysfs `reload`, about 5.5 s)
  recovers it every time. The fault is in the FPGA, not in the driver,
  PIO or DMA.
- The trigger pattern is the harness test "reads return whole frames for
  every receiver count": open the device once, then for 1 to 6 receivers
  send the general command and one frequency command per receiver, read
  for 250 ms, and close. Nine other command patterns driven from a script
  at a similar pace (counts stepping 1 to 6, alternating 1/6, 1/2, 2/3,
  open and close around each change, run cleared and set, the PA word
  with run set) made about 500 changes with no lock-up. What the harness
  does that they do not has not been identified; every rate below is for
  the harness pattern.
- Upstream's GPIO-mode gateware (73.3) ran on this board before this
  project without the fault being noticed. The PIO variant has no
  known-good reference run anywhere.

## 3. What the gateware source and the build reports show

From `gateware/source/` (the unmodified Hermes-Lite 2 tree) and the
Quartus reports of full compiles. Relevant, and together sufficient for
the mechanism in section 5, but no single item was shown causal on its
own.

- Both gateware variants buffer receive samples in `usiq_fifo`
  (`rtl/fifos.v`), an Intel `dcfifo` of 1024 words with an asynchronous
  clear (`aclr`) driven by `reset_channels`, which every receiver-count
  change asserts, and by the gateware reset. The GPIO variant reads the
  FIFO on the FPGA's own clock; the PIO variant reads it on `pi_rx_clk`,
  the clock the Pi drives in.
- The source asks for a synchronised clear with the parameter
  `wrsync_aclr_synch("ON")`. That is not a `dcfifo` parameter name. The
  documented ones are `write_aclr_synch` and `read_aclr_synch`, both
  default OFF. Quartus ignores the unknown name without a warning, and
  the generated FIFO netlist confirms it: the read and write pointer
  registers' clear inputs are `!aclr` directly, with no synchroniser on
  either side.
- The receiver-count comparison that fires `reset_channels` runs in the
  10 MHz command domain on the SPI slave's registered `rdata`, written
  only on the 48th SPI clock, so it never sees a half-shifted command.
  Command spacing is not a mechanism (and was measured not to be, section
  4).
- `rx_pi_pio.v`, the Pi-clocked state machine that reads the FIFO and
  drives the nibble stream to the Pi, has no reset of any kind. The FIFO
  flags it waits on (`us_tvalid`, `us_tlength`) change asynchronously to
  its clock when the clear fires. Quartus infers latches for its
  `us_stream` output (synthesis warning 10240).
- The project leaves "Safe State Machine" off and state machine
  processing on "Auto". The synthesis report shows that Quartus
  re-encoded the source's 5-bit binary `state` (eight states, with a
  `default` branch back to START) as 8 one-hot bits with the START bit
  inverted: START is all zeros, every other state is `state.START` = 1
  plus its own bit. Quartus adds recovery from illegal state vectors only
  when Safe State Machine is on, so the source's `default` branch is not
  what the built machine does in an illegal state; what the minimised
  logic does there has not been determined.
- The timing analyser checks recovery and removal of the FIFO clear only
  against the 76.8 MHz write clock, which shares a clock group with the
  10 MHz domain that asserts the clear. `pi_rx_clk` is in a separate
  asynchronous group (`set_clock_groups -asynchronous`), so the clear's
  release against the read-side registers that `rx_pi_pio` samples is
  never analysed.
- `rst_all` into the radio block is tied to 0: the receivers' decimators
  and FIR are never reset after configuration. `run` is used in `radio.v`
  only by the transmit state machine and does not gate RX.
- The timing constraints assume a 4.8 MHz Pi RX clock; the upstream PIO
  program and ours drive 20 MHz. The `pi_rx_clk` paths still fit a 25 ns
  half period (data delays 6 to 10 ns), and the outputs to the Pi are
  false-pathed.

## 4. What was tried, and what each result ruled in or out

One run of the harness test is six receiver-count changes. Lock-ups were
detected by the driver's stall detector (no RX words for 200 ms with run
set) and recovered by a gateware reload. SoC temperature 27 to 32 C unless
stated. All on 2026-11-08, the shipped CL025 image (fitter seed 4) unless
stated.

| Configuration | Lock-ups | Runs | Meaning |
|---|---|---|---|
| first observation, harness in its own loop | 7 | 35 | fault established, about 20% per run |
| nine scripted command patterns | 0 | about 500 changes | the harness pattern is necessary; what in it is not known |
| harness, `debug=2` (every command traced) | 6 | 150 | tracing does not change the rate |
| harness, `debug=0` | 1 | 50 | |
| SoC 42 to 49 C, CPU burners | 2 + 2 + 3 | 150 | moderate heat is not a strong factor |
| 100 us minimum gap between SPI commands, hot | 2 | 50 | command spacing is not the trigger |
| recovery ladder on each lock-up | 9 (0 recovered) | 150 | no command recovers; state no command resets |
| TX drain while locked | TX alive in all 13 | | AD9866 clock domain alive |
| RX clock held still across each count change, 200 us | 0 | 150 | the Pi-driven read clock is involved |
| same, the day's control for the sweep, hold off | 8 | 150 | |
| fitter seed 1 (FIR path misses timing by 0.163 ns) | 3 | 150 | |
| fitter seed 2 (misses by 0.016 ns) | 6 | 150 | |
| fitter seed 3 (misses by 0.199 ns) | 0 | 150 | |
| fitter seed 5 (timing met) | 1 | 150 | |
| fitter seed 6 (timing met) | 0, then 6 | 150, then 600 | the best seed still fails at about 1% per run |
| fitter seed 7 (timing met) | 2 | 150 | |
| RX clock held, 200 us, long run | 0 | 600 | with the earlier 150: 0 in 750 runs, 4500 changes |
| hold off and on, stream counters compared | passes 6 of 6 each | 12 | no resyncs, no dropped words, same discard count either way |

The seed sweep compiled the unmodified source seven times with Quartus
Prime Lite 23.1std.1 and fitter seeds 1 to 7; synthesis is identical, only
placement and routing differ. The timing misses on seeds 1 to 3 are on
receiver FIR paths inside the AD9866 clock group, not on the Pi RX clock
domain. Under the shipped image's rate (17 in 300 runs that day) the
chance of a seed's 150 clean runs is about 1 in 10000, so the rate depends
on placement. No seed measured zero over a long run.

Not done, and not possible here: observing the FPGA's internal state. No
JTAG adapter or other diagnosis hardware is available, so everything was
observed from the Pi (SPI replies, stream pins through `pinctrl`, the
driver's counters, the behaviour of rebuilt images).

## 5. The mechanism, as far as it is established

Established by measurement:

- The fault is an asynchronous crossing in the receive read-out, at the
  FIFO clear that every receiver-count change asserts, against the Pi's RX
  clock. It moves with placement (section 4, seed sweep), which a logic or
  protocol bug would not do, and it disappears when the Pi's clock has no
  edge while the clear is asserted and released (0 in 4500 changes).
- The bad value is captured in state that no command and no later clear
  resets, and that reconfiguration does.

Not established, and not determinable from the Pi: which register captures
it. The two candidates sit on the same path. One is the one-hot `state`
vector of `rx_pi_pio`, with no reset and no safe-state recovery: an
illegal vector that asserts no state would never read the FIFO again, the
FIFO would fill to its 1012-word threshold, the sample assembler would
wait on `rx_tready` for good, and only reconfiguration would recover, which
fits every observation. The other is the FIFO's read-side flag registers
(`rdempty`, `rdusedw`), cleared asynchronously and sampled by that state
machine; metastability there would feed the same state register. The
clock hold removes the clock edge for both.

Why the GPIO variant does not show it: its FIFO read side runs on the
FPGA's own clock, and there is no Pi-clocked state machine.

## 6. Decision (2026-11-08)

The owner asked for the mechanism to be confirmed before anything was
merged; the confirmation steps (encoding check, seed sweep, long runs of
both candidate fixes) are the lower half of the table in section 4. With
those in hand the owner approved:

1. The RX clock hold becomes the driver's normal behaviour (section 7),
   with the stall detector kept as a permanent indicator.
2. The gateware stays built unmodified from upstream source; the defects
   in section 3 are recorded as an open gateware item (section 9), to be
   fixed at source only by a separate decision.
3. A different fitter seed is not shipped: the best one measured still
   fails at about 1% per run, so it is not a fix, and the hold does not
   need it.
4. Development resumes.

Alternatives considered and set aside: a minimum gap between SPI commands
(measured, no effect); detecting the stall and reloading the gateware from
the driver (5.5 s of dead air per event, a stopgap only, and not needed
once the hold measured zero); a gateware source change (reopens a standing
decision, and no change could be validated here beyond a rate difference
between two images).

## 7. The workaround

Driver 0.4.0 and later, `driver/kernel/radioberry.c`, the command ioctl.
When a general command carries a receiver count different from the current
one, the driver:

1. stops its RX PIO state machine, so the clock it drives into the FPGA
   (GPIO 6) stands still;
2. waits 20 us;
3. sends the SPI command (the gateware asserts and releases the FIFO
   clear in its 10 MHz domain);
4. sleeps `rx_clock_hold_us` (default 200) to 1.25 times that;
5. re-enables the state machine.

No other command is affected, including the steady stream of general
commands with an unchanged receiver count. TX is not involved. The
parameter is writable at run time in
`/sys/module/radioberry/parameters/rx_clock_hold_us`; 0 disables the hold,
which is how the rates without it in section 4 were measured. 200 us is
the only value measured; it was not minimised, since the cost does not
call for it.

Measured cost: none visible in the stream. With the hold on and off, six
harness runs each, the harness passed every run, no resyncs and no dropped
words were counted either way, and the old-layout words discarded at a
count change were the same order (209 to 476 per run with the hold, 187 to
712 without). The one command that changes the receiver count takes about
220 to 290 us longer and holds the command mutex meanwhile. The stream is
paused for about 260 to 310 us at that change; the FIFO can reach its
threshold in about 220 us at 384 ksps with six receivers (derived from the
sizes, not measured), so the FPGA may drop the first samples after a
change, at a point where the layout changes and the driver discards words
anyway.

The indicator: the driver's worker notes the first 200 ms without RX words
while run is set, logs "RX stream stalled" once with the receiver count,
rate code and periods in flight, counts it, and logs "RX stream resumed"
if words return. Sysfs `rx_stalled` (1 while stalled) and `rx_stalls`
(count since the module loaded) in `/sys/bus/spi/devices/spi0.0/` expose
it; the `debug` parameter at 2 traces every command and every
receiver-count change without rate limiting.

## 8. Limits of the workaround

- It is statistical evidence on one board. 0 in 750 runs (4500 changes)
  bounds the rate with the hold at about 0.4% per run at 95% confidence,
  at least a fourteenfold reduction from the shipped image's 5.7%, and
  consistent with zero. The mechanism argument says it should be exactly
  zero, but rests on a register that could not be identified.
- Measured at SoC 27 to 32 C only. Without the hold, moderate heat did not
  change the rate, so this is a gap, not a known risk.
- The long run used the shipped image (seed 4) only. The mechanism is
  placement-independent by construction; a short confirmation on another
  seed would close that.
- Only the harness pattern was ever able to trigger the fault, so there is
  no second pattern to test the hold against.
- It covers only the FIFO clear the driver itself causes, through the
  receiver-count command. The crossing is still in the gateware. The
  other source of the clear is the gateware reset at configuration, when
  the driver is not streaming. A future gateware that cleared the FIFO
  for another reason would not be covered; the stall detector is there
  for that.
- It depends on being able to stop and restart one RP1 PIO state machine
  mid-program. The zero resyncs above say the restart is clean, or that
  the existing discard path after a count change absorbs it.

## 9. What needs fixing in the gateware

The defects are in upstream's PIO variant source and are recorded as an
open item in `TODO/ACTIVE.md`, section 5. Fixing them means editing the
gateware source, which reopens the standing decision "built unmodified"
and needs the owner's explicit decision; the image would then be versioned
so `rbprobe` and clients can tell it apart, and `gateware/PROVENANCE.md`
would record it. Whichever changes are made, their effect can only be
validated here as a rate difference between two images under the harness
pattern with the hold disabled, which at the shipped image's rate needs a
few hundred runs per image.

In order of how directly each addresses the mechanism:

1. Synchronise the FIFO clear on the read side. Replace the ignored
   `wrsync_aclr_synch("ON")` in `usiq_fifo` (`rtl/fifos.v`) with the real
   parameters `read_aclr_synch("ON")` and `write_aclr_synch("ON")`, so the
   clear's release is synchronised to each clock before it reaches that
   side's pointers and flags. This is what the source author evidently
   intended.
2. Give `rx_pi_pio` a reset and safe-state recovery. Either turn "Safe
   State Machine" on for the project (the recoded machine then returns to
   START from any illegal vector), or reset the state machine from the
   same clear that resets the FIFO, synchronised into `pi_rx_clk`, so a
   receiver-count change restarts the read-out from a known state. The
   source's binary encoding with its `default` branch could also be kept
   by setting state machine processing to "User-Encoded".
3. Remove the inferred latches on `us_stream` by assigning it in every
   branch of the combinational block.
4. Constrain the Pi RX clock at the frequency actually driven (20 MHz,
   50 ns period, not 208 ns) so the analyser checks the paths at the real
   rate, and consider whether the clear should be timed against
   `pi_rx_clk` instead of being cut by the asynchronous clock group.

Alternatively, upstream could be asked to make these changes, with this
document as the report; the fault is in their source and would affect any
PIO-variant user whose host changes the receiver count while streaming.

## 10. Reproducing it

- Trigger: the harness test "reads return whole frames for every receiver
  count" in `tools/driver-tests/`, run in a loop with
  `rx_clock_hold_us=0`, checking sysfs `rx_stalled` after each run and
  writing 1 to `reload` when it is set. Expect about 6% of runs to lock up
  with the shipped CL025 image at room temperature; 150 runs take about
  seven minutes including recoveries.
- Seed sweep: `tools/build-gateware.sh --seed-search N` builds with seeds
  1 to N but keeps only the first image that meets timing; for a sweep,
  run the compile for each seed separately and keep each project's `build/`
  directory, whose `radioberry.map.rpt` holds the state machine encoding
  table and `radioberry.sta.rpt` the recovery and removal summaries. Three
  compiles run in parallel in a Docker VM with 6 CPUs and 8 GB; each takes
  about ten minutes.
- Swap an image for a measurement by copying it to
  `/lib/firmware/radioberry.rbf` and writing 1 to `reload`; restore the
  shipped one afterwards.
