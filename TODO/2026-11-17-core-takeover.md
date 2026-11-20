# Taking over the radio core, block by block

Done: released in 0.7.0 (2026-11-20). Kept as the record of how each
stage was checked; what is left (stage 8, the board measurements) is in
[ACTIVE.md](ACTIVE.md).

The gateware wrapped the Hermes-Lite 2 radio core. Everything around it
was already this project's: the Pi transport (I2S on a Pi 4, RP1 PIO on a
Pi 5), the framed SPI command protocol, the AD9866 start-up sequencer,
the keying watchdog and PA gating. This plan replaces the core one block
at a time until it is a set of this project's own components. What each
finished stage did and measured is in [COMPLETED.md](COMPLETED.md).

## Why

- **Size and margin.** The core's datapaths ran at full width for every
  receiver; the receivers' CIC decimators alone were 45% of the device.
- **Our own tools.** Blocks written for the device and for a young
  synthesis, placement and routing flow: registered, pipelined, no
  vendor-specific inference tricks, each with its own testbench.
- **Only what the product uses.** Features no client can reach are not
  carried.
- **Work done where it is cheap.** What is one instruction on the Pi and
  a wide multiplier in the FPGA (stage 4) belongs on the Pi.
- **A varied, verified diet for our own toolchain.** Each stage gives it a
  new design shape with a reference, Quartus fits, and a bitstream proven
  on the board.

## Principles

1. **Nothing is lost.** No function a client can reach, no receiver count
   or rate, no measured figure gets worse.
2. **One block at a time.** Every stage ends with working images for both
   boards; the next starts from there.
3. **Each block is checked against the one it replaces.** Bit for bit
   where the arithmetic is kept; otherwise within a tolerance stated
   before the work and measured after.
4. **The same gates every stage** (below), on both images.
5. **The RTL is written for the device and for Quartus**, not bent to any
   tool's subset; subset gaps go to the toolchain as requests, and it
   rebuilds from the unchanged sources.
6. **Transmit work is tested keyed only with the board owner's go-ahead
   for each run**, the setup confirmed for that run (which Pi and board,
   dummy load or antenna, external amplifier in line or not).

## Gates for every stage

| Gate | What |
|---|---|
| Simulation | The block against the one it replaces in Icarus (`tools/sim.sh`, on a build Pi with `SIM_LOCAL=1`), bit for bit or within the stated tolerance; the radio-level comparison against the stage before (`radioeq`); the image testbenches (`rpi4`, `rpi4tx`, `rpi5`); for anything that keys, `failsafe` |
| Quartus | Ten seeds of each image (`rpi4-i2s`, `rpi5-pio`): every seed meets timing; logic, memory and multiplier use recorded against the stage before |
| Board, receive | On the Pi 5: `rbcat` at the most receivers the link carries per rate (frame rate exact, stream clean); FT8 over Protocol 1 and 2 with nothing lost. The Pi 4 at the release gate, not at every stage (board owner) |
| Board, transmit (stages that touch it) | The loopback self-test (tone and image levels as before); keyed runs into the dummy load, power and spectrum against the stage before; an on-air FT8 contact |
| Toolchain data | Every Quartus seed's placement, routing, paths, project and the timing analyser's full dump kept and handed over with what the run was |

## Where it stands

Branch `item/gateware-core`. Logic elements as the fitter reports them,
ten seeds of each image; every seed of both images meets timing.

| Stage | Commit | rpi5-pio | rpi4-i2s |
|---|---|---|---|
| Before (the shared receive filter) | `0211dc3` | 19 828 (one seed) | 20 371 to 20 451 |
| 1. `radio.v` in the tree, the unreachable removed | `bfeb533` | 19 538 to 19 632 | 20 104 to 20 184 |
| 2. Receive CIC decimators | `19c1441` | 14 684 to 14 751 | 15 259 to 15 299 |
| 3. Mixers and NCOs | `c653e37` | 14 647 to 14 720 | 15 192 to 15 276 |
| 5. Receive sender; 6b. TX CIC interpolator | `1b9d8bd` | 14 524 to 14 584 | 15 087 to 15 128 |
| Variable CIC output rounded halves to even (no DC offset) | `6c11187` | 14 521 to 14 613 | 15 112 to 15 198 |
| 6a. TX interpolating FIR | `72da3fe` | 14 549 to 14 624 | 15 169 to 15 176 (two seeds) |
| 6c. TX up-converter (multiplier mixer, dither) in place of the CORDIC | `b338416` | 13 226 to 13 278 | 13 814 to 13 868 |
| 6d. Jack keying under the permission bit; raised-cosine CW envelope | `b8b7fc9` | 13 377 to 13 446 | 13 971 to 14 033 |
| 7 and 4. The core's own top and registers; phase words from the driver | `5c1edc7` | 13 350 to 13 409 | 13 929 to 14 008 |
| ADC capture on its own clock, about 102 ps later (three seeds each) | `7828030` | 13 387 to 13 413 | 13 954 to 13 981 |

No Hermes-Lite 2 module is compiled into the images any more, and since
stage 7 no Hermes-Lite 2 top: the core takes its own registers and the
driver translates. What is still Hermes-Lite 2: the NCO tables and the
filter coefficients (stage 8, optional).

Stage 7 checks: Miach proved both images equivalent to their sources
(rpi5-pio 28 289 obligations, rpi4-i2s 29 118, none differ or undecided)
and gated all 20 seeds (bitstream writer identical on all; every clock
passes at all three corners on 18; rpi4 seeds 1 and 5 show clk[1] -15 ps
on the ADC input registers in Miach's model where Quartus has +0.010:
Miach's known pessimism there, its finding, not the fit's). Full CI at `57b591c` passed with checkpatch,
sparse and clang-tidy: 394 tests, ASan/UBSan 390, line coverage 100%
(5567 lines).

The ADC input capture, the worst path of every seed from stage 2 to 7
(+0.008 to +0.018 ns), takes its own clock since `7828030`: setup +0.109
/ +0.317 / +0.319 ns and hold +0.465 / +0.344 / +0.174 (slow 85 C, slow
0 C, fast 0 C), the same on every seed (the registers are at the pins).
Pi 4 with that image (seed 3): rbprobe passes, FT8 P1 4 / P2 6 decodes
with 0 lost. Miach (2026-11-19): both images equivalent (28 303 and
29 132 obligations, the PLL a black box); its writer reproduces the rpi4
seeds byte for byte but differs on the rpi5 seeds by 30 to 39 bytes, and
it cannot yet time a phase-shifted PLL output: both its findings, being
fixed; it does not gate this change yet. Pi 4 keyed runs (same image):
12 W each on the board owner's meter with PA enable clear and set, keyed
3.00 s, 0 gaps, bad frames, overflows or underruns, unkeyed clean; on
air 40 m FT8, 30 PSK Reporter spots in 13 countries in half an hour
(-25 to -6 dB), every transmission whole; image tx_underruns 827 and a
short time keyed with no RF (TODO/ACTIVE.md 6b; the board owner's view:
probably WSJT-X, or a nearby kilowatt station). The board owner called
the Pi 4 run. Left for it: Miach's gate, the Pi 5's receive check, keyed
runs and on air on the Pi 5, and the Pi 5's global clocks (below).
Miach re-gated it after its fixes (develop `2f980a3`): writer
byte-identical on all six seeds; clk[3] derived from the PLL counters,
the ADC input slack within 2 ps of Quartus's; both images equivalent. Not
covered by Miach: the five rpi5 registers on the Pi RX clock's general
routing (it refuses them; Quartus's timing stands).
Memory blocks (merge `aea16cc`, item/mem-blocks): 65 to 57 of 66 M9Ks in
both images. Sweep, three seeds each: all meet; rpi5 13 672 to 13 733
logic elements, rpi4 14 277; 72 of 132 multiplier elements. Miach: both
images equivalent (28 658 and 29 487 obligations); its seed gate pending.
Pi 4 with rpi4 seed 3 (sha256 323d2eaf...): rbprobe passes, FT8 P1 5 /
P2 6 decodes with 0 lost; keyed runs 12 W each (PA enable clear and
set), 0 gaps, bad frames, overflows or underruns, unkeyed clean; on air
one full FT8 contact by the board owner, 7 transmissions whole, 0 TX
blocks dropped, image counters clean (0 underruns) over 5.3 M samples.
Pi 5 with rpi5 seed 3 (sha256 5be20a1f..., the stage 7 driver): it also
covers the ADC capture change and the Pi RX clock on general routing.
rbdrivertest 12/12 (27 544 assertions), rbprobe passes, FT8 P1 10 / P2 10
decodes with 0 lost, 0 stalls; keyed runs 12 W each (PA enable clear and
set), 0 gaps, bad frames, overflows, underruns or realigns, unkeyed
clean; our receive tone -22.9 / -22.8 dBFS where stage 7 on this board
gave -48.6 (26 dB more of the transmit signal at the receive input,
products below -117 dBFS; not explained). On air 40 m FT8: 9 PSK
Reporter spots in 6 countries in 20 minutes (-19 to -6 dB), 7
transmissions whole, 0 TX blocks dropped, image counters clean over
4.8 M samples; the Pi 5 reported under-voltage at some time since boot
(throttled 0x50000), as with its supply before. Miach (develop
`eecbbc1`) gated all six seeds: equivalent, writer byte-identical, every
clock meets at three corners (worst setup +45 to +51 ps, hold +108 to
+143); not covered: the five rpi5 Pi RX clock registers (refused), and
its hold on pin-fed general-routing clocks (optimistic, being fixed), so
Quartus's hold stands there. The candidate has passed every check.

TX CIC (merge `7adab5a`, item/tx-cic, for this release by the board
owner's word): comb state in one memory block, the fifth comb and first
integrator cancelled, the rounding folded in; bit-identical to the
original module (2.4 M clocks), corecmp now compares it against the
previous version. Sweep, three seeds each: all meet, 58 of 66 M9Ks; rpi5
13 267 to 13 349 logic elements, rpi4 13 936 to 13 961. Pi 5 with rpi5
seed 3 (sha256 cffc107f...): rbdrivertest 12/12, rbprobe passes, FT8 P1
5 / P2 9 decodes with 0 lost; keyed runs 12 W each (PA enable clear and
set), products below -114 dBFS as before, 0 gaps, bad frames, overflows,
underruns or realigns; on air one full FT8 contact by the board owner, 4
transmissions whole, 0 TX blocks dropped, image counters clean over
3.1 M samples. Pi 4 with rpi4 seed 3 (sha256 25943ca3...): rbprobe
passes, FT8 P1 7 / P2 8 decodes with 0 lost; keyed runs 12 W each, 0
gaps, bad frames, overflows or underruns (one realign at a stream's
start), unkeyed clean; on air one full FT8 contact by the board owner,
10 transmissions whole, 0 TX blocks dropped, image counters clean (0
underruns) over 7.0 M samples, no under-voltage. Left: Miach's gate.
On the Pi 5 image the new clock took the global network pin 54 (the Pi's
RX clock) had: six clocks for that side's five networks, and Quartus does
not move one to another side. Its own choice, the Pi RX clock (pin 54, 142
registers) on general routing, meets timing on all three seeds (setup
+5.6 ns, hold +0.48). Forcing the Pi clocks global was tried (seed 1):
both global costs the board clock its network; the board and Pi RX clocks
global with the Pi TX clock on general routing fails the AD9866 TX bus by
1.05 ns. Kept: Quartus's choice, checked on the Pi 5 board (rbdrivertest,
FT8) when the board is back on it.

## Left over from finished stages

- Stage 2's tolerance, confirmed on the board: the receive noise floor at
  each rate with the receive input on the dummy load and the LNA at its
  lowest, stage 1's image against the current one. The estimate is about
  0.2 dB more noise at 48 ksps and under 0.1 dB at 384 ksps. Needs the
  board on the dummy load.
- Stage 6c: the transmit spectrum on the board (dither on against the
  released image), where a receiver can measure it safely. Simulated
  only so far; on the board the power and the on-air result are as
  before.
- `rbcat`'s raw commands can still send the VNA bit; the driver's
  translation ignores it.
- 6d: the jack's key and PTT are not tested on the board (the board
  owner cannot key them and accepts the simulation, 2026-11-19); PTT hang
  time and T/R sequencing are unchanged from the original.

## Finished stages' notes

### 6d. Keying (done)

Built and simulated (`b8b7fc9`, `abbefe1`, `3390368`): the jack's key and
PTT key only with the TX permission bit, so the keying watchdog covers
them; the daemon's `jack_keying` option; the CW envelope a raised cosine
(`rtl/core/cw_shape.sv`); `tools/sim.sh failsafe` proves every keying
path stops when the host does. Figures in [COMPLETED.md](COMPLETED.md).

### 4. The frequency calculation in the kernel driver (done with stage 7)

A client's frequency in Hz becomes the NCO's phase word (the step of its
32-bit phase each 76.8 MHz clock) by

    phase = (Hz * 1876499845 + 2^24) >> 25

which the core computes in the FPGA: a 32 x 32 bit multiplier (8
multiplier elements), a three-clock wait in the command state machine,
and the 76.8 MHz clock's worst internal path, for a sum the Pi does in a
few instructions a few times a second.

- The driver, which every command passes through, computes the phase
  word (the same integer formula, 64-bit, no floating point) for the
  frequency addresses (0x01 to 0x08, 0x12 to 0x16) and sends it in place
  of the frequency. The gateware stores it: the multiplier, the wait and
  the path go.
- The daemon and the SoapySDR module do not change: they send Hz, the
  daemon after its frequency correction, as now. Clients see nothing.
- The image says which it expects (a bit of its capabilities register);
  the driver converts only for an image that says phase words, so an
  older image still works with the new driver. A new image under an old
  driver would tune wrong: the driver and the images ship together, in
  one release under one version number.
- Checks: the driver's formula against the core's for every frequency of
  a sweep and random ones, in the driver's unit tests (bit for bit); the
  radio core against the stage before with the testbench sending phase
  words, every output on every clock; the gates above; on the board, the
  receivers and the transmitter on frequency (FT8 decodes; the loopback
  tone where it was).

Built as planned except the capabilities bit: stage 7's images take only
phase words, and the driver requires their ID (RBV3).

### 7. The core's own top (done)

From first principles, leaving the Hermes-Lite 2 interface behind (board
owner, 2026-11-19): the design and his decisions are in
[2026-11-19-core-top.md](2026-11-19-core-top.md). Built with stage 4
(`5c1edc7`, `168ce51`, `ce27991`): `rtl/core/rbv_core.sv`, the images'
registers from 0x20, ID RBV3, the driver's `rb_hl2_translate()`.
`tools/sim.sh corecmp` against `b8b7fc9`: 0 mismatches over 800 800
clocks; `failsafe`, `rpi4`, `rpi4tx`, `rpi5` pass; driver unit tests
pass. rpi5-pio seed 1: 13 409 logic elements, 76.8 MHz worst setup
+2.84 ns. Pi 5 with the new driver and image: `rbprobe` passes, `rbcat`
clean, FT8 over Protocol 1 (10 decodes) and 2 (13) with nothing lost,
loopback tone -18.7 dBFS, image -142.0, counters clean. CPU over 20 s of
`p1probe` at 48 to 384 ksps, new against the packaged driver: driver
thread 0.35 to 0.45% against 0.35 to 0.5% of a core, `radioberryd` 3.0
to 5.0% against 3.4 to 5.3%.

Keyed run list (Pi 5, the stage 7 driver and image, dummy load with the
external amplifier in line; each run on the board owner's go-ahead with
the setup confirmed; `radioberryd` stopped unless stated):

1. `txkey-drive-060 128`: 3 s, a +1 kHz tone at 0.8 of full scale,
   14.120 MHz, PA enable clear. Read: his meter against the stage 6c
   image's 13 W, our receive tone, the image counters, keyed for 3.00 s.
2. The same with `pa-enable`.
3. `radioberryd` with his config, piHPSDR and WSJT-X: one FT8 period
   keyed, then unkey. Read: "PTT on/off" and the transmission's length
   in the journal, the image counters, TX start within the 20 ms
   latency, unkeyed after the client's hang.
4. On air, 40 m FT8 with the antenna: one contact or spots.

Done 2026-11-19, all four passed (driver `168ce51`, image of `5c1edc7`).
Runs 1 and 2: 12 W each on the board owner's meter (stage 6c: 12 and
13 W), our receive tone -48.6 / -48.5 dBFS, keyed 3.00 s, 0 gaps, bad
frames, overflows, underruns or realigns, unkeyed clean. Runs 3 and 4:
WSJT-X through piHPSDR, every transmission 13.9 s with 0 TX blocks
dropped, image counters clean over 5.3 M samples; on air 22 PSK
Reporter spots in 11 countries in 20 minutes (-4 to -23 dB). The board
owner called it.

Pi 4, the same day (driver built from `168ce51` on the Pi 4, stage 7
rpi4-i2s seed 10): rbprobe passes, FT8 P1 7 / P2 9 decodes with 0 lost.
Runs 1 and 2: 12-13 W each on the board owner's meter, our receive tone
-45.1 / -32.8 dBFS, keyed 3.00 s, 0 gaps, bad frames, overflows or
underruns, one realign (the word-phase lock at a stream's start, as
before), unkeyed clean. On air: one full FT8 contact by the board owner
(piHPSDR + WSJT-X): 8 transmissions, 0 TX blocks dropped, image counters
clean over 5.5 M samples, no under-voltage. The loopback self-test was
not run on the Pi 4 (its 0.6.0 tool is on the Pi 5, which was off).

The build unpacks the Hermes-Lite 2 archive only for its tables; the
archive stays as the reference the simulations compare against, and its
credits stay in the documentation.

## Stages to come

### 8. Our own filter coefficients (optional)

The receive and transmit filters still use the Hermes-Lite 2 project's
coefficients (by James Ahlstrom N2ADR and Phil Harman VK6APH). For
components wholly this project's, design filters to the same or a
tighter specification with this project's own tool, written in C++ (no
Python in the repository), and measure the response against the
original's.

## Alongside, not part of the takeover

- Pin paths: the ADC capture (`ad_rx` into its input register, the worst
  153.6 MHz path on every seed, +0.008 to +0.018 ns; our own toolchain's
  timing model, 12 to 25 ps more pessimistic there, puts it below zero on
  7 of stage 6c's 20 seeds) and TX bit 4 (pin 105, -2.7 ns under the strict
  constraint). Remedy: the capture and output clocks' phase, or the I/O
  delay chains. Needs board time. The board owner chose (2026-11-19) to
  move the capture clock; experiments in progress (worktree
  `exp/adc-capture-phase`): a second 153.6 MHz PLL output for the ADC
  input registers only. The PLL at its 614.4 MHz VCO moves in 203 ps
  steps: at 203 ps the slow setup is about +0.21 and the fast hold about
  +0.07 ns (today +0.010 / +0.269); a finer VCO step is being tried.
  TX bit 4, measured 2026-11-19 (stage 7 seed 1): pin 105's output buffer
  takes 5.43 ns slow / 3.16 ns fast, the other TX pins 2.65 / 1.47. The
  3.3-V LVCMOS standard offers 2 mA only (more drive needs another I/O
  standard), and no launch phase serves: setup needs bit 4 at least 2.72
  ns earlier, the AD9866's 1.5 ns hold allows at most about 1.7. The board
  owner's decision: keep it (no fault seen on either board); with the
  next image change, the TX constraints take the datasheet's hold and the
  relaxation's comment these figures.
- What stays the device's: the PLL instance and how memories and
  multipliers are inferred. Our own toolchain supports them; they are not
  taken over.
- An image built with no vendor tool at all is our own toolchain's goal,
  not this plan's; its images of each stage's core are tested on the
  board as they come (receive only).

## Faster

Proposals; times are from stages 6c and 6d. The board owner adopted 1, 3
and 5 (2026-11-19); 4 and 6 are done; 2 is measured, not adopted.

1. **Fewer seeds between releases.** A ten-seed sweep of both images
   takes about 3.5 hours (6.5 minutes a fit, 3 more for the timing dump).
   Every one of the 33 fits of stages 6a and 6c met timing, and their
   worst path is always the ADC input pin, which no RTL change moves.
   Three seeds of each image per change, about an hour; the board owner
   (2026-11-20): three are enough, at a stage's end and the release too.
2. **Synthesis once per sweep.** The fitter seed does not change
   synthesis. Measured on one stage 6d fit: synthesis 1 min 23 s of the
   flow's 5 min 29 s (fitter 3 min 41 s); with the router data and the
   timing dump a seed takes about 10 minutes. Synthesis once and the
   fitter per seed would save about 12 minutes of each ten-seed sweep of
   one image, under a tenth of it.
3. **Simulations side by side.** `radioeq` takes about an hour and
   `failsafe` 40 minutes, each on one core; they were run one after the
   other. Claimed together on three cores, a stage's simulations end
   with the longest.
4. **A compiled simulator.** Done: `VERILATOR=1 tools/sim.sh` (Verilator
   5.032, `--binary --timing`). Measured on a Pi 5 core, build and run:
   `corecmp` 30 s (Icarus about an hour), `failsafe` 62 s (40 min),
   `rpi4tx` 57 s, `rpi4` 49 s, `cwshape` 7 s, `txupconv` 9 s, all PASS;
   `rpi5` stays on Icarus (its testbench uses a hierarchical defparam).
   Memories then use this tree's model, not Quartus's.
5. **One keyed session a stage.** A written run list (each run's image,
   drive, length and what is read) agreed first, then run in one sitting
   with the board owner, each run still on his go-ahead.
7. **No release sweep when the RTL is unchanged.** Adopted (2026-11-19).
   A sweep shows the RTL meets timing over many placements; it is needed
   when the RTL or constraints change. A release whose image sources are
   unchanged since the last gated sweep builds one seed per image at the
   release commit and checks the bitstream byte-identical to that sweep's
   seed (Quartus is deterministic for the same sources and seed), so the
   shipped image is the tested one. About 20 minutes for both images.
6. **This branch kept level with `develop`.** Merged at each release
   (done for 0.6.1), so its own release is not a large merge.

## Release

The release notes are in CHANGELOG.md under "Unreleased". The committed
images are stage 7's (`c625f6b`, provenance rows and Miach's check); the
candidate (`aea16cc`: stage 7, the ADC capture change, the memory
blocks; three seeds each, passed on both boards) replaces them at the
release, built at the release commit and checked byte-identical to its
tested seed (Faster 7). Left, on the
board owner's word: the version number, merge into `develop`, the release
gate on each board (clean-down, README install from a test repo, FT8
receive, the board owner's contact), signing and upload, then `main` and
the tag.

## Risks

- **RF performance.** The core's chain is proven on air. Each replacement
  is measured against it, not assumed equal.
- **Transmit.** Stage 6 can put a fault on the air.
- **A release crossing driver and gateware** (stage 4): the two must ship
  together.
