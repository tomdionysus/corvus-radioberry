# Gateware review

An external review of the gateware, with where corvus-radioberry stands on
each finding. The review was written against release 0.4.1; this page
reads it against this project's own images, `rpi4-i2s` (Raspberry Pi 4) and
`rpi5-pio` (Raspberry Pi 5).

Both images are this project's own logic (control registers, AD9866
set-up, the links to the Pi, keying and PA lines) around the Hermes-Lite 2
radio core, which is used unmodified: receivers, decimation filters, NCOs
and the transmit DSP. Findings about the radio core are therefore open
until that core is measured.

The review's scope boundary holds: gateware can make the existing radio
reliable, well timed, observable and numerically careful. It cannot improve
the AD9866's 12-bit converters, remove intermodulation that happens before
the ADC, or add RF preselection the board does not have. The AD9866's
programmable receive low-pass filter (15 to 35 MHz) is not a band-pass
preselector.

These findings are not being worked on now.

## Findings

| # | Finding | Progress | Done |
|---|---|---|---|
| 1.1 | Timing must close on every clock of the image actually built | Both CL025 images meet setup and hold on every clock, in every timing model | ✓ |
| 1.2 | Timing must not depend on a lucky fitter seed | `rpi4-i2s`: seeds 1 to 4 compiled, seed 1 misses by 2 ps, seed 3 is used. `rpi5-pio`: seed 1 only, not swept | ✗ |
| 1.3 | No exception may make a failing path pass | AD9866 TX bit 4 (FPGA pin 105) has -2.72 ns setup slack under the strict constraint in both images and passes only through a relaxed `set_max_delay`, as in the Hermes-Lite 2 constraints. Both boards transmit with it | ✗ |
| 1.4 | AD9866 bus constraints come from the datasheet and board skew | Input and output delays are taken from the Hermes-Lite 2 constraints. The TX hold model (0 ns) is looser than the datasheet's 1.5 ns; measured hold slack is +3.085 ns | ✗ |
| 1.5 | No unconstrained paths go unexplained | Not counted | ✗ |
| 1.6 | Timing reports are kept with each release | Kept in the build directory, not with the release | ✗ |
| 1.7 | CL016 timing closes | The CL016 is not supported; a CL016 variant needs someone with the board to test it | ✗ |
| 2.1 | A receiver-count change must not stop the stream | Neither image ever clears a FIFO; on the Pi 5 the receive FIFO's read side runs on the Pi's own clock. Simulation: in order across 2 to 3 receivers with the clock running, 3 to 1 with it held, and program restarts mid-value. Hardware: `rbdrivertest` passes on both boards | ✓ |
| 2.2 | No samples of a removed receiver reach clients after a change | The frame index restarts at a change and the driver delivers nothing until two clean frames of the new layout have arrived | ✓ |
| 2.3 | Every transition is tested, including rapid ones and the driver's RX hold turned off | 2 to 3 and 3 to 1 in simulation; not every pair, not back-to-back, not on hardware with the hold off | ✗ |
| 2.4 | A stalled stream is detected and recovered | The driver detects and reports a stall (`rx_stalled`, `rx_stalls`); the gateware has no watchdog and nothing recovers automatically | ✗ |
| 2.5 | Data left over from before a pause is not delivered as new | Up to 1024 values held in the FIFO across a long pause are delivered first when RX resumes | ✗ |
| 3.1 | Every clock domain and crossing is documented | Listed in each image's top-level header | ✓ |
| 3.2 | Each crossing uses a safe structure | Commands by toggle handshake with the data held still; levels by two-flop synchronisers; streams by Gray-coded FIFOs | ✓ |
| 3.3 | Reset release is synchronous in each domain | The radio reset is asserted asynchronously and released synchronously; the Pi clock domains need no reset | ✓ |
| 3.4 | Multi-bit values never cross unsynchronised | Status counters are read across domains without a handshake (documented as approximate while their clock runs) | ✗ |
| 3.5 | Timing exceptions are no broader than the synchroniser inputs | Whole domains are cut with `set_clock_groups -asynchronous` | ✗ |
| 3.6 | Quartus metastability and crossing reports are reviewed | Not run | ✗ |
| 3.7 | The radio core's own crossings are audited | Not audited | ✗ |
| 4.1 | The AD9866 gain actually set is observable | The client's LNA gain maps to the AD9866 gain code; AD9866 registers read back through the `AD_READ` board register | ✓ |
| 4.2 | ADC clipping is reported | A sticky "ADC hit full scale" status bit | ✓ |
| 4.3 | Near-clipping, peak and RMS levels are reported | Not implemented | ✗ |
| 4.4 | Gain control honours AD9866 settling, with hysteresis and a manual override | Gain is set by the client only; there is no automatic gain control | ✗ |
| 4.5 | The AD9866 receive low-pass filter setting is checked | Not audited | ✗ |
| 5 | Fixed-point widths, truncation and rounding through the receive chain are documented and measured | Radio core unmodified, not audited | ✗ |
| 6 | Decimation filter passband, stopband and alias rejection are measured at every rate | Radio core unmodified, not measured | ✗ |
| 7.1 | TX FIFO overflow and underflow are detected and counted | Both images count them (`tx_overflows`, `tx_underruns`); on the Pi 5 the wire's ready line prevents overflow | ✓ |
| 7.2 | The output during a TX underflow is defined and tested | Left to the radio core, not specified or tested | ✗ |
| 7.3 | TX underruns do not occur in normal use | 482 underruns counted across the Pi 4's 30 m transmissions, none on 40 m or on the Pi 5; cause not known | ✗ |
| 7.4 | A host that stops cannot leave the transmitter keyed | KEYING clears itself unless rewritten within about a second; PA lines stay low unless run, PTT, permission and PA allowed are all set | ✓ |
| 7.5 | PTT transitions and CW keying are shaped and measured | The radio core shapes CW; PTT transitions are not shaped; key clicks not measured | ✗ |
| 8 | NCO and CORDIC spurs are measured before any change | Radio core unmodified, not measured; the TX loopback self-test shows the carrier at -138 dBFS and the opposite sideband at -145 dBFS | ✗ |
| 9.1 | The gateware is identified and versioned | ID, VERSION and CAPS registers in the framed control protocol | ✓ |
| 9.2 | Stream and link events are counted | Frames, CRC errors, FIFO-full events, values sent, TX samples, overflows, underruns, keying watchdog trips, AD9866 writes, sticky status bits; in sysfs `i2s_counters` and `rbprobe` | ✓ |
| 9.3 | Clocks are measured against an independent reference | Not in the radio images | ✗ |
| 9.4 | Counts are kept per receiver | Not implemented | ✗ |
| 9.5 | AD9866 set-up is verified by reading back | Readback exists (`AD_READ`) but is not run automatically | ✗ |
| 10.1 | RTL testbenches cover the images | Icarus Verilog testbenches for `rpi4-i2s` receive and transmit and for `rpi5-pio`, the last running the driver's own PIO programs | ✓ |
| 10.2 | A bit-accurate DSP reference model exists | Not built; findings 5, 6 and 8 need it | ✗ |
| 10.3 | Repeatable hardware checks exist | `rbdrivertest`, `ft8rx`, a TX loopback self-test, keyed runs into a dummy load | ✓ |
| 10.4 | Tests run automatically and their results are kept per release | The testbenches do not run in CI; results are not kept per release | ✗ |

## References

- [AD9866 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ad9866.pdf):
  receive gain and low-pass filter, AGC timing, digital interface timing
- [Hermes-Lite 2 receiver simulation](https://github.com/softerhardware/Hermes-Lite2/wiki/Receiver-Simulation):
  a method for findings 5, 6 and 8
- [Quartus timing analyser: metastability analysis](https://docs.altera.com/r/docs/683243/25.3/quartus-prime-pro-edition-user-guide-timing-analyzer/metastability-analysis)
  and [synchroniser constraints](https://docs.altera.com/r/docs/683243/25.3/quartus-prime-pro-edition-user-guide-timing-analyzer/constraining-synchronizers-in-cdcs)
