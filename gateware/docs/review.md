# Gateware review

An external review of the gateware, with where corvus-radioberry stands on
each finding. The review was written against release 0.4.1; this page
reads it against this project's own images, `rpi4-i2s` (Raspberry Pi 4) and
`rpi5-pio` (Raspberry Pi 5).

Both images are this project's own logic (control registers, AD9866
set-up, the links to the Pi, keying and PA lines) around a radio core
whose blocks are this project's, each simulated against the Hermes-Lite 2
block it replaced, bit for bit or within a stated tolerance
([provenance.md](provenance.md)). The committed images are still release
0.6.0's, which contain the Hermes-Lite 2 blocks; the progress below is
for the tree.

The review's scope boundary holds: gateware can make the existing radio
reliable, well timed, observable and numerically careful. It cannot improve
the AD9866's 12-bit converters, remove intermodulation that happens before
the ADC, or add RF preselection the board does not have. The AD9866's
programmable receive low-pass filter (15 to 35 MHz) is not a band-pass
preselector.

Open work on them is listed in
[TODO/ACTIVE.md](../../TODO/ACTIVE.md).

## Findings

| # | Finding | Progress | Done |
|---|---|---|---|
| 1.1 | Timing must close on every clock of the image actually built | Both CL025 images meet setup and hold on every clock, in every timing model | yes |
| 1.2 | Timing must not depend on a lucky fitter seed | Ten seeds of each image meet timing. The ADC input capture, the worst path of every seed with 8 to 18 ps, now takes its own PLL clock about 102 ps later (+0.109 setup, +0.174 hold in the trial fit) | yes |
| 1.3 | No exception may make a failing path pass | AD9866 TX bit 4 (FPGA pin 105) has -2.72 ns setup slack under the strict constraint in both images and passes only through a relaxed `set_max_delay`, as in the Hermes-Lite 2 constraints. Its buffer's slow-to-fast spread is wider than the AD9866's window allows, so no launch phase fixes it at the 2 mA the I/O standard offers. Both boards transmit with it; kept by the board owner's decision | no |
| 1.4 | AD9866 bus constraints come from the datasheet and board skew | Input and output delays are taken from the Hermes-Lite 2 constraints. The TX hold model (0 ns) is looser than the datasheet's 1.5 ns; `tools/worst.sh` reports every TX pin's hold under 1.5 ns in the fast model on each build | no |
| 1.5 | No unconstrained paths go unexplained | Not counted | no |
| 1.6 | Timing reports are kept with each release | Kept in the build directory, not with the release | no |
| 1.7 | CL016 timing closes | The CL016 is not supported; a CL016 variant needs someone with the board to test it | no |
| 2.1 | A receiver-count change must not stop the stream | Neither image ever clears a FIFO; on the Pi 5 the receive FIFO's read side runs on the Pi's own clock. Simulation: in order across 2 to 3 receivers with the clock running, 3 to 1 with it held, and program restarts mid-value. Hardware: `rbdrivertest` passes on both boards | yes |
| 2.2 | No samples of a removed receiver reach clients after a change | The frame index restarts at a change and the driver delivers nothing until two clean frames of the new layout have arrived | yes |
| 2.3 | Every transition is tested, including rapid ones, with no RX clock hold in the driver | The driver no longer holds the RX clock. 2 to 3 and 3 to 1 in simulation; not every pair, not back-to-back, no hardware stress run | no |
| 2.4 | A stalled stream is detected and recovered | The driver detects and reports a stall (`rx_stalled`, `rx_stalls`); the gateware has no watchdog and nothing recovers automatically | no |
| 2.5 | Data left over from before a pause is not delivered as new | Up to 1024 values held in the FIFO across a long pause are delivered first when RX resumes | no |
| 3.1 | Every clock domain and crossing is documented | Listed in each image's top-level header | yes |
| 3.2 | Each crossing uses a safe structure | Commands by toggle handshake with the data held still; levels by two-flop synchronisers; streams by Gray-coded FIFOs | yes |
| 3.3 | Reset release is synchronous in each domain | The radio reset is asserted asynchronously and released synchronously; the Pi clock domains need no reset | yes |
| 3.4 | Multi-bit values never cross unsynchronised | Status counters are read across domains without a handshake (documented as approximate while their clock runs) | no |
| 3.5 | Timing exceptions are no broader than the synchroniser inputs | Whole domains are cut with `set_clock_groups -asynchronous` | no |
| 3.6 | Quartus metastability and crossing reports are reviewed | Not run | no |
| 3.7 | The radio core's own crossings are audited | Not audited | no |
| 4.1 | The AD9866 gain actually set is observable | The client's LNA gain maps to the AD9866 gain code; AD9866 registers read back through the `AD_READ` board register | yes |
| 4.2 | ADC clipping is reported | A sticky "ADC hit full scale" status bit | yes |
| 4.3 | Near-clipping, peak and RMS levels are reported | Not implemented | no |
| 4.4 | Gain control honours AD9866 settling, with hysteresis and a manual override | Gain is set by the client only; there is no automatic gain control | no |
| 4.5 | The AD9866 receive low-pass filter setting is checked | Not audited | no |
| 5 | Fixed-point widths, truncation and rounding through the receive chain are documented and measured | The CIC decimators' widths are pruned to a stated tolerance and measured against the exact ones in simulation (error rms 0.079 to 0.097 of an output LSB); their output rounds halves to even, with no DC offset; the filter is within -118 dBFS rms of the original's output. Not measured on the board | no |
| 6 | Decimation filter passband, stopband and alias rejection are measured at every rate | Computed from the design (both CICs and the 976-tap filter's coefficients), the same at every rate in proportion: passband within -0.17 to +0.08 dB to 0.4 of the output rate, -3 dB at 0.49; anything that aliases into that passband at least 88.4 dB down. The band from 0.45 to 0.5 of the rate takes aliases from just above 0.5 at -13 dB (the filter's transition). Not measured on the board | no |
| 7.1 | TX FIFO overflow and underflow are detected and counted | Both images count them (`tx_overflows`, `tx_underruns`); on the Pi 5 the wire's ready line prevents overflow | yes |
| 7.2 | The output during a TX underflow is defined and tested | Left to the radio core, not specified or tested | no |
| 7.3 | TX underruns do not occur in normal use | 482 underruns counted across the Pi 4's 30 m transmissions, none on 40 m or on the Pi 5; cause not known | no |
| 7.4 | A host that stops cannot leave the transmitter keyed | KEYING clears itself unless rewritten within about a second, and every keying path needs its permission bit: software PTT and the key jack's key and PTT. A simulation of the image with the real core stops the host under each and checks the core idle, the PA lines low and the AD9866's transmit path off (`tools/sim.sh failsafe`) | yes |
| 7.5 | PTT transitions and CW keying are shaped and measured | CW has a raised-cosine envelope, 4.1 ms up and down; in simulation one element's spectrum is above -80 dB of the carrier to 1.7 kHz from it. PTT transitions are not shaped; nothing measured on the board | no |
| 8 | NCO and CORDIC spurs are measured before any change | Transmit: the CORDIC is replaced by a multiplier mixer with dither, both measured in simulation (worst spur to 38.4 MHz at full drive -101.1 dBc for the CORDIC, -104.4 dBc now; at -20 dBFS -75.5 and -84.8). Receive NCO: the same values as the original's, its spurs not measured. Nothing measured on the board | no |
| 9.1 | The gateware is identified and versioned | ID, VERSION and CAPS registers in the framed control protocol | yes |
| 9.2 | Stream and link events are counted | Frames, CRC errors, FIFO-full events, values sent, TX samples, overflows, underruns, keying watchdog trips, AD9866 writes, sticky status bits; in sysfs `counters` and `rbprobe` | yes |
| 9.3 | Clocks are measured against an independent reference | Not in the radio images | no |
| 9.4 | Counts are kept per receiver | Not implemented | no |
| 9.5 | AD9866 set-up is verified by reading back | Readback exists (`AD_READ`) but is not run automatically | no |
| 10.1 | RTL testbenches cover the images | Icarus Verilog testbenches for `rpi4-i2s` receive and transmit and for `rpi5-pio`, the last running the driver's own PIO programs; one for each core block against the Hermes-Lite 2 block it replaced; the whole core against an earlier revision of itself, every output on every clock | yes |
| 10.2 | A bit-accurate DSP reference model exists | No separate model: the Hermes-Lite 2 RTL is the reference each block was simulated against. Findings 5, 6 and 8 still need response measurements | no |
| 10.3 | Repeatable hardware checks exist | `rbdrivertest`, `ft8rx`, a TX loopback self-test, keyed runs into a dummy load | yes |
| 10.4 | Tests run automatically and their results are kept per release | `tools/ci-pi.sh` runs six testbenches under Verilator where it is installed (both images' receive and transmit, the keying fail-safe, the core against its last Hermes-Lite 2 revision, the CW and up-converter blocks; about 8 minutes on one core); `rpi5` and `radioeq` need Icarus and are run by hand. Results are not kept per release | no |

## References

- [AD9866 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ad9866.pdf):
  receive gain and low-pass filter, AGC timing, digital interface timing
- [Hermes-Lite 2 receiver simulation](https://github.com/softerhardware/Hermes-Lite2/wiki/Receiver-Simulation):
  a method for findings 5, 6 and 8
- [Quartus timing analyser: metastability analysis](https://docs.altera.com/r/docs/683243/25.3/quartus-prime-pro-edition-user-guide-timing-analyzer/metastability-analysis)
  and [synchroniser constraints](https://docs.altera.com/r/docs/683243/25.3/quartus-prime-pro-edition-user-guide-timing-analyzer/constraining-synchronizers-in-cdcs)
