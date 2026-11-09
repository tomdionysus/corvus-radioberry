# Fault: receive stream lock-up in the PIO gateware

## Summary

With the Hermes-Lite 2 Radioberry PIO gateware (75.3,
`radioberry_pio_cl025`, built from unmodified source) the FPGA can stop
delivering receive samples at a receiver-count change and never resume.
Only reloading the gateware recovers it. The trigger is the asynchronous
clear of the receive FIFO that every receiver-count change asserts, taken
by the Pi-clocked read-out logic while the Pi's RX clock is running.
Without the workaround, about 6% of runs of the triggering test lock up
with the shipped image.

The driver avoids it by stopping its RX PIO state machine, and with it the
clock it drives into the FPGA, from just before a receiver-count command
until 200 us after it: 0 lock-ups in 4500 changes, against 17 in 1800
without, at no measurable cost to the stream. A stall detector reports the
condition should it occur anyway. The gateware defect itself is open
(`TODO/ACTIVE.md`).

## Symptoms

- Sample-ready (GPIO 25) stays low while the Pi's RX clock (GPIO 6) keeps
  toggling. SPI control stays alive: identity replies are correct and
  commands are accepted.
- Every lock-up begins at the receiver-count command itself; no RX words
  arrive after it. The command bytes and spacing are the same as in the
  changes that do not lock up.
- TX keeps running: zero samples written with PTT clear drain at 48 ksps.
  The AD9866 clock domain is alive; the fault is in the receive chain.
- No command recovers it (receiver-count changes, rate changes, run
  cleared and set, LNA and PA commands). Reloading the module with
  `load_gateware=0` does not; reloading the gateware always does.
- Only one pattern triggers it: the harness test "reads return whole
  frames for every receiver count" (open once, then for 1 to 6 receivers
  send the general command and one frequency per receiver, read 250 ms,
  close). Nine other scripted patterns made about 500 changes with no
  lock-up.

## What the gateware source and build reports show

- RX samples are buffered in `usiq_fifo` (`rtl/fifos.v`), a `dcfifo` of
  1024 words with an asynchronous clear driven by `reset_channels`, which
  every receiver-count change asserts. The PIO variant reads it on
  `pi_rx_clk`, the clock the Pi drives in; the GPIO variant reads it on
  the FPGA's own clock.
- The source asks for a synchronised clear with `wrsync_aclr_synch("ON")`,
  which is not a `dcfifo` parameter. The real ones, `write_aclr_synch`
  and `read_aclr_synch`, default OFF. Quartus ignores the unknown name
  silently; the generated netlist clears both pointer sets directly from
  `aclr` with no synchroniser.
- `rx_pi_pio.v`, the Pi-clocked state machine that reads the FIFO, has no
  reset. The FIFO flags it waits on change asynchronously to its clock
  when the clear fires. Quartus infers latches for its `us_stream` output.
- Safe State Machine is off. Quartus re-encoded the source's binary
  `state` as one-hot with START inverted, and adds no recovery from
  illegal vectors, so the source's `default` branch is not what the built
  machine does in an illegal state.
- The FIFO clear's recovery and removal are timed only against the
  76.8 MHz write clock; `pi_rx_clk` is in an asynchronous clock group, so
  the clear's release against the read side is never analysed.
- The constraints assume a 4.8 MHz Pi RX clock; the PIO program drives
  20 MHz. The paths still fit (data delays 6 to 10 ns).
- Command spacing is not a mechanism: the receiver-count comparison runs
  on registered SPI data, and a 100 us minimum command gap had no effect.

## Measurements

One run of the harness test is six receiver-count changes. Lock-ups were
detected by the stall detector and recovered by a gateware reload. SoC 27
to 32 C unless stated; shipped CL025 image (fitter seed 4) unless stated.

| Configuration | Lock-ups / runs |
|---|---|
| harness, no hold | 7 / 35, 6 / 150 (`debug=2`), 1 / 50, 8 / 150 |
| SoC 42 to 49 C, no hold | 7 / 150 |
| 100 us minimum gap between SPI commands, hot | 2 / 50 |
| fitter seeds 1, 2, 3, 5, 7, no hold | 3, 6, 0, 1, 2 / 150 each |
| fitter seed 6, no hold | 0 / 150, then 6 / 600 |
| RX clock held 200 us | 0 / 150, then 0 / 600 |

The seed sweep compiled the unmodified source seven times with identical
synthesis and different placement; the rate depends on placement, which
a logic or protocol bug would not do. With the hold on and off, six
harness runs each passed, with no resyncs or dropped words either way.

The FPGA's internal state could not be observed: no JTAG adapter was
available, so everything was measured from the Pi.

## Mechanism

Established: the fault is an asynchronous crossing in the receive
read-out, at the FIFO clear, against the Pi's RX clock. It moves with
placement and disappears when the Pi's clock has no edge while the clear
is asserted and released. The bad value is captured in state that no
command and no later clear resets, and that reconfiguration does.

Not established: which register captures it. The candidates are the
one-hot `state` vector of `rx_pi_pio` (an illegal vector would never read
the FIFO again, which fits every observation) and the FIFO's read-side
flags sampled by that state machine. The clock hold removes the clock
edge for both.

## The workaround

`driver/kernel/radioberry.c`, the command ioctl. When a general command
carries a receiver count different from the current one, the driver:

1. stops its RX PIO state machine, so GPIO 6 stands still;
2. waits 20 us;
3. sends the SPI command;
4. sleeps `rx_clock_hold_us` (default 200) to 1.25 times that;
5. re-enables the state machine.

No other command is affected; TX is not involved.
`/sys/module/radioberry/parameters/rx_clock_hold_us` is writable at run
time; 0 disables the hold. 200 us is the only value measured.

Cost: the receiver-count command takes about 220 to 290 us longer and
holds the command mutex meanwhile; the stream pauses for about 260 to
310 us at a point where the layout changes and the driver discards words
anyway.

The indicator: after 200 ms without RX words while run is set, the driver
logs "RX stream stalled" with the receiver count, rate code and periods
in flight, and "RX stream resumed" if words return. Sysfs `rx_stalled`
(1 while stalled) and `rx_stalls` (count since load) are in
`/sys/bus/spi/devices/spi0.0/`.

## Limits

- 0 in 750 runs bounds the rate with the hold at about 0.4% per run at
  95% confidence, consistent with zero, on one board.
- Measured at SoC 27 to 32 C only, and on the shipped image only.
- It covers only the FIFO clear caused by the receiver-count command. A
  gateware that cleared the FIFO for another reason would not be covered;
  the stall detector is there for that.

## Fixing it in the gateware

Fixing it means editing the gateware source, which the project builds
unmodified; that needs a separate decision, a versioned image and a
`gateware/PROVENANCE.md` entry. In order of directness:

1. Synchronise the FIFO clear: replace `wrsync_aclr_synch("ON")` in
   `usiq_fifo` with `read_aclr_synch("ON")` and `write_aclr_synch("ON")`.
2. Give `rx_pi_pio` a reset and safe-state recovery: turn Safe State
   Machine on, or reset it from the same clear synchronised into
   `pi_rx_clk`, or set state machine processing to "User-Encoded".
3. Assign `us_stream` in every branch to remove the inferred latches.
4. Constrain the Pi RX clock at 20 MHz and time the clear against
   `pi_rx_clk`.

A change can only be validated here as a rate difference between two
images under the harness pattern with the hold disabled: a few hundred
runs per image.

## Reproducing it

- Run the harness test in `tools/driver-tests/` in a loop with
  `rx_clock_hold_us=0`, checking `rx_stalled` after each run and writing 1
  to `reload` when it is set. 150 runs take about seven minutes.
- Seed sweep: compile each seed separately with `tools/build-gateware.sh`
  and keep each `build/` directory; `radioberry.map.rpt` holds the state
  machine encoding and `radioberry.sta.rpt` the recovery and removal
  summaries.
- To measure another image, copy it to `/lib/firmware/radioberry.rbf`,
  write 1 to `reload`, and restore the shipped one afterwards.
