# Core rework: evaluation and plan

The gateware reworked so that tools other than Quartus can build it: simpler,
faster, and smaller where it can be.

Nothing the images do now is lost, in function or in performance. A gain,
such as more receivers, is a stretch goal and not the aim.

Each statement below is marked by where it comes from: **measured** (a
report or a run), **source** (read from the RTL, not run), or **to check**
(a lead, not yet shown).

## 1. Why

Both images meet timing on some fitter seeds only, by tens of picoseconds
on the seed that is shipped (section 3). A flow without Quartus's fitter
will not reach that: open place and route is reported to trail vendor
fitters by 10 to 30% on the critical path. The device is also nearly full
(section 2), which makes every fitter's work harder.

The target, on internal register-to-register paths, is 1 to 2 ns of slack
in Quartus at 153.6 MHz, on every seed of a sweep, and a smaller design.

## 2. Size today

**Measured**, from the fitter reports of the released images
(10CL025YE144C8G).

| | `rpi4-i2s` | `rpi5-pio` | Device |
|---|---|---|---|
| Logic elements | 22 536 (92%) | 21 995 (89%) | 24 624 |
| Registers | 18 725 | 18 303 | |
| Multiplier elements (9-bit) | 132 (100%) | 132 (100%) | 132 |
| M9K blocks | 50 (76%) | 50 (76%) | 66 |
| Memory bits | 400 384 (66%) | 400 384 (66%) | 608 256 |

Where the `rpi4-i2s` logic is:

| Block | Logic elements | Multiplier elements | M9K |
|---|---|---|---|
| Hermes-Lite 2 radio core (`radio`) | 20 124 | 132 | 37 |
| Six receivers (`receiver_nco`) | 15 083 | 96 | 28 |
| Transmit CORDIC (`cpl_cordic`) | 1 762 | | |
| Transmit interpolating CIC (`CicInterpM5`) | 1 028 | | |
| Three mixers with their NCOs (`mix2`) | 1 058 | 24 | 6 |
| VNA scanner (`vna_scanner`) | 385 | | |
| Transmit interpolating FIR (`FirInterp8_1024`) | 182 | 4 | 3 |
| One multiplier in `radio` | 72 | 8 | |
| The rest of `radio` | 775 | | |
| This project's logic around the core | 2 412 | | 13 |
| I2S link (`rbv_i2s`) | 586 | | |
| AD9866 sequencer (`rbv_adseq`) | 296 | | |
| Framed SPI (`rbv_spi`) | 286 | | |
| Two FIFOs (`rbv_afifo`) | 337 | | 12 |
| Top level | 945 | | 1 |

One receiver, 2 457 to 2 684 logic elements:

| Part | Logic elements | Notes |
|---|---|---|
| Two 5-stage CICs, variable rate (`varcic`, I and Q) | about 1 400 | 43-bit accumulators |
| Decimate-by-8 FIR (`firX8R8`, four `fir256`) | about 800 | 16 multiplier elements, sample RAMs and coefficient ROMs |
| Two 3-stage CICs, decimate by 5 (`cic`, I and Q) | about 490 | 25-bit accumulators |

Every multiplier element is used; a design with more receivers has to use
fewer of them per receiver.

## 3. Timing today

**Measured.** Worst setup slack per clock of the released images, slow
model (the seed shipped):

| Clock | `rpi4-i2s` | `rpi5-pio` |
|---|---|---|
| `clk[1]`, 153.6 MHz | +0.012 ns | +0.018 ns |
| AD9866 TX bus (`virt_adc`) | +0.051 ns | +0.060 ns |
| `clk[2]`, 122.88 MHz (I2S link) | +0.158 ns | none |
| `clk[0]`, 76.8 MHz | +0.580 ns | +0.918 ns |

`ad_tx[4]` (FPGA pin 105) misses the AD9866's setup by 2.7 ns under the
strict constraint on both images and is relaxed in the constraints
([hardware-discovery.md](hardware-discovery.md) section 4).

Three seeds of an earlier build of `rpi4-i2s` (the transmit bring-up), none
of which met timing:

| Seed | `clk[1]` worst path | `clk[2]` worst path |
|---|---|---|
| 1 | +0.013 ns, `fir256` `reg_q` to `Imult` | -0.254 ns, `rbv_i2s` `ph` to `word_a` |
| 2 | +0.008 ns, pin `ad_rx[0]` to `rx_d1[0]` | -0.063 ns, `rbv_i2s` `ph` to `word_a` |
| 3 | -0.221 ns, `fir256` RAM address register to its multiplier | -0.328 ns, `rbv_i2s` `ph` to `chk_a` |

The same two areas miss when another tool synthesizes the design and
Quartus fits it: of eleven seeds, one met timing, the others missed by
0.04 to 0.50 ns in `fir256`'s multiplier path on `clk[1]` or in `rbv_i2s`'s
check registers on `clk[2]`.

**Measured.** Ten seeds of the released `rpi4-i2s` (v0.6.0), worst setup
slack, slow model; 6 of 10 meet timing:

| Seed | `clk[1]` 153.6 MHz | `clk[2]` 122.88 MHz | `clk[0]` 76.8 MHz | TX bus | Worst path where it misses |
|---|---|---|---|---|---|
| 1 | -0.067 | +0.324 | +0.634 | +0.051 | `fir256` coefficient register to its multiplier |
| 2 | +0.018 | +0.376 | +0.668 | +0.051 | |
| 3 | +0.012 | +0.158 | +0.580 | +0.051 | (the seed shipped) |
| 4 | +0.002 | -0.123 | +0.512 | +0.060 | `rbv_i2s` `ph` to `w_prev` |
| 5 | +0.015 | +0.251 | +0.586 | +0.051 | |
| 6 | +0.018 | +0.372 | +0.422 | +0.051 | |
| 7 | -0.240 | +0.352 | +0.677 | +0.051 | `tx_sync` to the mixers' NCO phase (`nco2` `angle1`), then `fir256` |
| 8 | +0.014 | +0.139 | +0.833 | +0.051 | |
| 9 | +0.008 | +0.539 | +0.871 | +0.051 | |
| 10 | -0.149 | +0.148 | +0.474 | +0.051 | `fir256` sample register to its multiplier |

The other synthesis tool's netlist of v0.6.0, fitted by Quartus (three
seeds), misses in the same two places: `clk[1]` -0.23 to -0.51 ns, all
`fir256` operand register to multiplier across all six receivers;
`clk[2]` -0.076 ns, `rbv_i2s` `ph` to its check registers.

Every seed: `ad_tx[4]` -2.708 to -2.720 ns under the strict constraint
(relaxed). Seven of ten `clk[1]` results are under +0.02 ns. Seed 7 shows
a third internal path not seen before: `tx_sync` into the NCO phase
accumulators (step 5 or 7). The `rpi5-pio` baseline is still to run.

Two kinds of path are tight, and they need different remedies:

- **Internal paths** (register to register): `fir256`'s multiply, the I2S
  word and check logic. A register in the right place, or a restructured
  block, gives these margin.
- **Pin paths**: the `ad_rx` capture (+0.008 to +0.012 ns) and the TX bus.
  These are set by the pin, its I/O register and the constraint; a
  pipeline register cannot move them. The capture clock's phase or the I/O
  cell's delay chain can. They are about the same for any fitter.

## 4. The receive chain

**Source** (`receiver_nco.v`, `cic.v`, `varcic.v`, `firfilt.v`).

One receiver, from the mixer's 18-bit output at 76.8 MHz:

| Stage | Decimation | Output rate at 48 / 96 / 192 / 384 ksps |
|---|---|---|
| `cic`, 3 stages, 25 bits | 5 | 15.36 MHz |
| `varcic`, 5 stages, 43 bits | 40 / 20 / 10 / 5 | 384 / 768 / 1 536 / 3 072 kHz |
| `firX8R8`, 976 taps, polyphase | 8 | 48 / 96 / 192 / 384 kHz |

`firX8R8` is four `fir256` engines on the 153.6 MHz clock, each serving
two of the eight polyphase branches. An engine takes one input sample in
four; for each it runs 122 multiply-accumulates, I and Q in parallel on
two 18 by 18 multipliers, then waits. A product is cut from 36 to 24 bits
with rounding before it is accumulated; `firX8R8` adds the eight branch
sums in 24 bits.

In `fir256` the multiply and the add are already on separate cycles
(`reg_q`, `reg_coef` into the multiplier, `Rmult`, then the accumulator),
and the sample RAM and coefficient ROM have registered outputs.

How busy an engine is (122 taps and about 6 cycles of overhead per input
sample, at 153.6 MHz):

| Output rate | Engine busy |
|---|---|
| 48 ksps | 8% |
| 96 ksps | 16% |
| 192 ksps | 32% |
| 384 ksps | 64% |

All receivers run at one rate. The links carry, at most
([rpi4.md](../../docs/rpi4.md), [rpi5.md](../../docs/rpi5.md)):

| | 48 ksps | 96 ksps | 192 ksps | 384 ksps |
|---|---|---|---|---|
| Pi 4 (I2S) | 6 | 6 | 4 | 2 |
| Pi 5 (PIO) | 6 | 6 | 6 | 3 |

So the most filter work any link can use is three receivers at 384 ksps
or six at 192 ksps: 1.152 million output samples a second, which is 976
times that in multiply-accumulates for I and again for Q. At 153.6 MHz a
multiplier does 153.6 million a second: about 7.3 multipliers each for I
and Q, 15 in all, 30 multiplier elements. The six receivers have 48
multipliers, 96 elements.

## 5. Findings and leads

1. **The 18 by 18 multiply in `fir256` is the tight `clk[1]` path**
   (measured, section 3). To check: whether its input registers are in
   logic elements instead of the multiplier block's own, so that routing
   takes the margin; the baseline sweep's path reports show it.
2. **`rbv_i2s`'s word and check logic misses on `clk[2]` on every seed
   that misses** (measured). It is this project's RTL: two adders and a
   comparison feed register enables in one cycle (the other tool's report).
   The comparison can be computed a cycle earlier with the pins unchanged
   (to check).
3. **The receive FIR engines are idle most of the time and sized for a
   case no link carries** (source, section 4). A filter engine shared
   between receivers, with the same arithmetic, would use about 30
   multiplier elements for 96, and less logic. The arithmetic can be kept
   bit for bit, so it can be checked against the original sample for
   sample. To check: the logic a shared engine's sample and state memory
   and its scheduling cost.
4. **The CICs are the largest block of plain logic**, about 1 900 logic
   elements a receiver, 11 300 in all (measured). `varcic`'s integrators
   run on one clock in five and its combs on one in 25 to 200 (source).
   To check: whether stages can share adders, or state can move to block
   memory, with the same results.
5. **Quartus merges some of the filters' memories** between receivers
   (measured: the fitter attributes 12, 8, 8, 0, 0 and 0 M9K blocks to the
   six receivers). Not analysed; a tool that does not merge them needs 24
   sample RAMs and up to 24 coefficient ROMs.
6. **The core carries what this configuration does not use**: `receiver.v`,
   `receiver_ciconly.v`, `receiver_4000.v`, other decimation ratios in
   `varcic`, the EER transmit filter (source). The project files already
   list only the files the images use.
7. **Not measured yet**: fixed-point widths and rounding through the
   chain, the decimation filters' passband, stopband and alias rejection
   at each rate, and NCO and CORDIC spurs ([review.md](review.md) items 5,
   6 and 8).

## 6. Tools and method

| Tool | Gives | Does not give |
|---|---|---|
| Quartus (Docker, [toolchain.md](toolchain.md)) | the timing figures that count, logic by block | speed: about 4 minutes a seed |
| Icarus Verilog (`tools/sim.sh`) | simulation: a new block against the original | |
| The other synthesis tool, on the workstation | logic counts and table depth of a block in seconds; whether its Verilog subset accepts the RTL | timing, simulation, equivalence of two designs |

Table depth is a proxy for speed, for comparing two versions of one
block; it predicts no slack.

**The original core is the reference.** A replaced block is simulated
beside the Hermes-Lite 2 original with the same input and matches it
sample for sample, or differs only by a latency that is stated. The
archive in `source/` is not edited: new RTL goes in `rtl/`, and edits to
the core that remain go in the build-time patch.

How RTL is written, so that either tool builds it:

- Plain inferred RTL in place of vendor megafunctions: memories with one
  clock, a write and a registered read; multipliers from `*` with
  registers on both operands and on the product. The PLL stays a vendor
  block.
- An adder not on the same cycle as a multiply.
- No long `if` / `else if` chains feeding a clock enable in the cycle
  they are computed.
- Every register with a reset or an initial value; no latches; one clock
  in each `always` block; synchronous resets.

A change is accepted when:

1. its simulation against the original passes;
2. every seed of a sweep meets timing, with the margin of section 1 on
   the paths it touches;
3. the image uses no more logic, multipliers or memory than before;
4. on both boards, the loopback tone and image levels and FT8 receive are
   as before ([driver hardware tests](../../driver/docs/driver-tests.md),
   [SoapySDR hardware tests](../../soapy/docs/hardware-tests.md)).

## 7. Plan

1. **Baseline.** A ten-seed sweep of each released image: slack per clock
   and the worst internal paths by endpoint. Logic by block for
   `rpi5-pio`. A testbench that drives one original receiver with a
   recorded input and keeps its output, as the reference for steps 3 to 5.
2. **`rbv_i2s` check logic.** The comparison a cycle earlier; the link's
   simulations unchanged; `clk[2]` with margin on every seed.
3. **`fir256` multiply path.** The multiplier's operands and product in
   the multiplier block's registers; the same structure; outputs equal to
   the original's, with any added latency stated.
4. **Shared receive filter.** A design note first: engines, memory,
   schedule, and the logic and multipliers it should take; then the block,
   bit for bit against the six original filters at every rate the links
   carry.
5. **CICs.** Only if step 1 and a design note show a saving with the same
   results.
6. **Pin paths.** The `ad_rx` capture and the TX bus, by clock phase or
   I/O delay, each measured on the board.
7. **Audit** of finding 7, by simulation of the reference and confirmed
   on the board.

Each step ends with its figures added to this document.

## 8. Results so far

### The reference receiver

`tools/sim.sh rxref RATE N` simulates one original receiver
(`sim/rx_ref_tb.sv`) at the images' clocks with a fixed input: tones at
+3 kHz (amplitude 60 000 of 131 072) and +31 kHz (30 000), and
pseudo-random noise. **Measured**, Icarus:

| | 384 ksps | 48 ksps |
|---|---|---|
| Output samples, checksum | 2 000, `e8a3d720f0fcac68` | 500, `ae7bd412344714d0` |
| +3 kHz | -13.2 dBFS | -13.2 dBFS |
| -3 kHz (its mirror) | -120.6 dBFS | -121.1 dBFS |
| +31 kHz | -19.3 dBFS | outside the band |
| +100 kHz (384), +20 kHz (48) | -145.4 dBFS | -129.8 dBFS |

Three things the core relies on that a simulator, or another tool, does
not give it, found on the way (**measured**, in simulation):

1. **Its plain memory models differ from the vendor blocks it is built
   with.** Without `USE_ALTSYNCRAM`, `firram36.v` and `firromH.v` read in
   one clock; the `altsyncram` blocks the images use read in two (address
   and output registered). `fir256` counts its pipeline to choose which
   products it adds, so the plain models give a different filter. The
   reference uses Quartus's own models (`altera_mf.v`). A rework writes
   one model, inferred, that every tool reads the same way.
2. **Registers without an initial value that are read before they are
   written**: `cic_comb`'s output (the next stage samples it on the first
   strobe) and `firX8R8`'s output accumulators (the first output strobe
   comes before they are loaded). The FPGA powers them up at zero; a
   simulator carries an unknown value through every later sample. The
   testbench sets them as the device does. A rework gives every register
   a value.
3. **The 76.8 MHz and 153.6 MHz clocks must edge together.** `firX8R8`
   (76.8 MHz) reads each `fir256` engine's sum (153.6 MHz) on the same
   edge as the engine clears it.

### Step 2: `rbv_i2s`

The sampling instant, the word-end and frame-half flags, the flags
word's check and the next expected sequence number are each worked out a
cycle ahead into registers, from the registered settings. No pin changes
timing.

**Measured:**

- `tools/sim.sh i2seq`: the new module beside the one released in
  v0.6.0, the same settings and data, every output compared on every
  clock: 1 735 672 cycles over dividers 2, 3, 4, 5, 8 and 13, every
  sampling phase and seven bit offsets, with 246 samples passed, 1 382
  bad checks, 105 sequence gaps and 5 realigns; no difference.
- `tools/sim.sh rpi4` and `rpi4tx`: pass.
- The other synthesis tool's mapping:

| | Tables | Carry bits | Registers | Depth (tables) |
|---|---|---|---|---|
| v0.6.0 | 193 | 210 | 513 | 7 |
| Reworked | 250 | 218 | 517 | 5 |

- Quartus across seeds: with step 3, below.

### Step 3: `fir256`'s multiplier registers

`rtl/core/firX8R8.sv` replaces the core's `firfilt.v` in both images.
`firX8R8` is unchanged. In `fir256` the multipliers' operand registers
(`reg_q`, `reg_coef`) and product registers (`Rmult`, `Imult`) become
free-running registers with no enable and no clear (`a_r`, `a_i`, `b_r`,
`b_i`, `p_r`, `p_i`), the coefficient register once for each multiplier,
so a fitter can place them in the multiplier blocks. The pipeline keeps
its length. The original held its operands when its count reached zero
and cleared its product register on a new sample: it added zero for the
first product of a run and never added the last, the only cycles where
free-running registers differ, and one flag (`prev_en`) gives the zero.

The core's `rst_all` is the start-up reset, held from the PLL's lock until
the AD9866 is set up, before any sample flows. After a reset in mid-run
the original could add a product held from before it; the reworked
engine adds zero. Neither clears its sum on that reset, so their first
output after it is meaningless in both.

**Measured**, `tools/sim.sh rxref RATE N rework` against the original:

| | 384 ksps | 48 ksps |
|---|---|---|
| Outputs compared | 2 000 | 500 |
| Equal | all after the first | all after the first |
| Checksum, both | `e8a3d720f0fcac68` | `ae7bd412344714d0` |

The first output after power-up is unknown in the original's simulation
(its sums have no initial value) and 0 in the reworked one; the FPGA
powers both up at 0.

The other tool maps one engine to the same counts before and after (24
tables, 70 carry bits, 122 and 123 registers, depth 3, two multiplier
blocks): it already placed the original's registers in the multiplier
blocks. Quartus did not for the original (its failing path runs from
`reg_coef` in a logic element); its figures are below.

### Steps 2 and 3 in Quartus

**Measured.** Ten seeds of `rpi4-i2s` with steps 2 and 3 (commit
`50014b4`), worst setup slack, slow model, beside the released image's
(section 3). **10 of 10 meet timing** (released: 6 of 10).

| Seed | `clk[1]` released | `clk[1]` steps 2+3 | `clk[2]` released | `clk[2]` steps 2+3 | `clk[0]` steps 2+3 |
|---|---|---|---|---|---|
| 1 | -0.067 | +0.010 | +0.324 | +1.585 | +0.677 |
| 2 | +0.018 | +0.012 | +0.376 | +1.807 | +0.657 |
| 3 | +0.012 | +0.008 | +0.158 | +1.709 | +0.824 |
| 4 | +0.002 | +0.012 | -0.123 | +1.550 | +0.657 |
| 5 | +0.015 | +0.008 | +0.251 | +1.822 | +0.755 |
| 6 | +0.018 | +0.008 | +0.372 | +1.146 | +0.524 |
| 7 | -0.240 | +0.008 | +0.352 | +1.474 | +0.762 |
| 8 | +0.014 | +0.010 | +0.139 | +1.389 | +0.748 |
| 9 | +0.008 | +0.010 | +0.539 | +1.493 | +0.680 |
| 10 | -0.149 | +0.008 | +0.148 | +1.500 | +0.749 |

- `clk[2]` (the I2S link): every seed gains 0.6 to 1.7 ns; the worst
  path is now `rbv_i2s`'s `ph` to `tx_words` or its check to `seq_gaps`.
- `clk[1]`: on every seed the worst path is now the ADC capture pin,
  `ad_rx[0]` to `rx_d1[0]` (+0.008 to +0.012 ns): a pin path, which no
  internal change moves (step 6). The worst internal paths on `clk[1]`
  (seed 10, from the detailed report) are the mixers' sine and cosine
  ROM address registers into their multipliers, at +0.24 to +0.29 ns;
  the receive filter no longer appears among them.
- The TX bus and `ad_tx[4]`'s strict figure are unchanged.

## 9. Step 4: options for the receive filter

**Source**: the filter's 976 coefficients (`firrom/coefL4.py`) are
symmetric, `h[n] = h[975 - n]` for every n (checked), as a linear-phase
low-pass is; the largest is 82 575, within 18 bits. The samples are 16
bits, sign-extended to 18.

Each output sample needs 976 products for I and 976 for Q. At 153.6 MHz
one multiplier does 153.6 million a second. The figures below are
estimates from that arithmetic, not builds.

| | How | Multiplier elements (now 96) | Six receivers at 384 ksps | Output |
|---|---|---|---|---|
| A | Symmetric folding in each receiver: the two samples that share a coefficient are added (17 bits), then multiplied once: 488 products an output. Two multipliers a channel a receiver keep up at 384 ksps (61% busy). | 48 | kept | not bit for bit: see below |
| B | One engine shared by all receivers, the original arithmetic, sized for what the links carry (1.152 million output samples a second at most) | about 30 | not computed (no link carries it) | bit for bit |
| A and B | Both | about 16 | not computed | not bit for bit |

Folding changes the rounding: the original rounds each of its 976
products to the accumulator's 24 bits and adds them; folding rounds 488
sums of two. The outputs then differ from the original's in their lowest
bits, and the rounding noise is lower, not higher: about 488 rounding
errors an output instead of 976, each within half a step of the 24-bit
result, far below the noise of the 12-bit ADC behind it. The acceptance
for A would be the original's output within a stated tolerance, with the
same passband, stopband and image figures, measured.

Memory, each receiver (estimate): 976 samples of 36 bits, read two at a
time (four M9K, as the four engines' sample memories use now), and 488
coefficients of 18 bits (one M9K).

### Design A, built

`rtl/core/firfold.sv`, a drop-in for `firX8R8`. The last 1 024 samples
are kept in two memories (even and odd indices), each with two read
ports, so two multiply units a channel read the four samples of a cycle;
244 cycles an output plus one stall for each input written, against 400
available at 384 ksps. The coefficients are a 256-word initialised array
of pairs in the RTL (244 used), from the images' `.mif` files, read
whole into a register so that a tool makes it one ROM.

**Measured** in simulation (`tools/sim.sh rxref RATE N fold`, and a
model of the folded arithmetic outside the repository):

| | 384 ksps | 48 ksps |
|---|---|---|
| Outputs equal to the folded model | 600 of 600 | 300 of 300 |
| Difference from the original's output, rms | -118.2 dBFS | |
| Difference, worst sample | -106.7 dBFS | |

The coefficients the images load differ from the integer column of the
script that made them at one symmetric pair of taps (420 and 555: 2 723
in the `.mif` files, 2 722 in the script); the files are used.

### Design B, built

`rtl/core/firshared.sv`: one filter for all the receivers, with the
original arithmetic. The receivers keep their CIC stages
(`rtl/core/rx_cic.sv`, the core's `receiver_nco` without its filter);
`rtl/core/radio.sv` (the core's `radio.v`, in this project's tree)
connects them to the filter.

- Eight lanes, lane j taking the taps n = j (mod 8) of one receiver at a
  time, 122 a lane, all in step: 122 cycles a receiver, 16 multipliers.
- Samples in eight memories, one for each index class (mod 8), each with
  one write port and one read port, every receiver's last 128 samples of
  its class: 768 x 32 each for six receivers. Coefficients: one table of
  122 words, eight coefficients a word.
- The core's sender reads every receiver on receiver 0's strobe, so the
  filter sums the receivers in use (0 to `last_chan`) in turn and then
  gives one strobe for all of them. Three receivers at 384 ksps take
  about 386 of the 400 cycles between groups, six at 192 ksps about 752
  of 800: the most either Raspberry Pi link carries. A group that arrives
  while the last is still being summed is dropped, with its output.
- The output comes after the last receiver's sum, which can be before
  or after the original's strobe; the values are the same.

**Measured** in simulation (`tools/sim.sh firshared RATE N LAST`: six
original receivers beside six `rx_cic` and the shared filter, each
receiver with its own input), every output after the first compared:

| Rate | Receivers | Outputs a receiver | Values different |
|---|---|---|---|
| 384 ksps | 0..2 | 600 | 0 of 1 800 |
| 384 ksps | 0 | 200 | 0 of 200 |
| 192 ksps | 0..5 | 300 | 0 of 1 800 |
| 48 ksps | 0..5 | 100 | 0 of 600 |

The patched core elaborates in Icarus with the project's file list.

### Design A+B, built

`rtl/core/firsharedfold.sv`, the same ports and module name as B (so the
same patch). Samples are kept as in B; four lanes, each adding two
samples that share a coefficient and making one product for I and one
for Q (8 multipliers), in two passes of 61 steps: lane p takes the taps
n = p (mod 8) and then n = 7 - p, below 488, with their partners
975 - n. In both passes lane p adds classes 3 - p and 4 + p, so every
memory is still read once a cycle. 122 cycles a receiver, as B: the same
receivers and rates, half B's multipliers. It computes design A's sums,
so its outputs are A's.

**Measured** (`tools/sim.sh firsharedfold`, against six receivers with
design A's filter): the same four cases as B, 0 values different in each
(1 800, 200, 1 800 and 600 compared).

### The memory output register

The first Quartus build of design A (seed 1) missed `clk[1]` by
4.05 ns: a sample memory's output, with no register, went through the
parity select and the pre-adder in the same cycle. The M9K's
clock-to-output alone was 3.2 ns of the 6.5 ns period. Designs B and A+B
had the same shape, a memory read straight into logic or a multiplier's
input. Each memory read now has a second register with no logic before
it, which the M9K provides as its output register, and the coefficient
one more to match; the pipelines are a cycle longer, the arithmetic
unchanged. The simulations above were rerun on the change, with the
same results.

The failing build's resources, for comparison (seed 1, the whole image):
84 of 132 multiplier elements (the released image uses all 132), 52 M9K
(released: 50) and 437 248 memory bits (released: 400 384). Design A
keeps 1 024 samples a receiver, 46 080 bits with its coefficients,
against the original filter's 36 864.

### A, B and A+B in Quartus

**Measured.** Ten seeds of `rpi4-i2s` with each design (commit
`8432799`; steps 2 and 3 included), worst setup slack, slow model.

| | Seeds meeting timing | `clk[1]` worst | Misses in | Logic elements | Multiplier elements | M9K |
|---|---|---|---|---|---|---|
| Released v0.6.0 | 6 of 10 | -0.240 to +0.018 | `fir256` multiply, `rbv_i2s` | 92% | 132 / 132 | 50 |
| Steps 2 and 3 | 10 of 10 | +0.008 to +0.012 | none (ADC pin) | 85% | 132 / 132 | 50 |
| B | 10 of 10 | +0.010 to +0.018 | none (ADC pin) | 84% | 68 / 132 | |
| A+B | 6 of 10 | -0.216 to +0.018 | read address into the sample memories | 82% | 52 / 132 | 57 |
| A | 2 of 10 | -0.550 to +0.018 | `first` into the accumulators | 92% | 84 / 132 | 52 |

Logic, multipliers and M9K are seed 1's (seed 3's for the first two
rows). `clk[2]` had +0.9 to +2.1 ns and `clk[0]` +0.25 to +0.87 ns on every
seed of every design.

- B meets timing on every seed, bit for bit the original.
- A+B misses where its read addresses are worked out (`base - m` and
  constants, the pass select) in the cycle the memories take them.
  Remedy: the addresses held in registers and stepped directly.
- A misses where the accumulators' first-term select (`first`, one
  register for 96 accumulator bits a receiver) meets the 24-bit add.
- Each 768 x 32 sample memory of B and A+B takes 4 M9K where 3 would
  hold it (Quartus's choice for the depth); the `max_depth` synthesis
  attribute is the remedy to try.
- The first builds of A, B and A+B failed in Quartus for reasons the
  simulations could not show: a memory output with no register before
  logic (A: -4.05 ns), rounding and accumulation in one cycle (-0.93),
  a select and adder into a multiplier block's input (-0.74), generate
  loops Quartus 23.1 refuses, and memories in a generate loop that
  Quartus built from registers. Each was fixed before these sweeps.

### B and A+B after the timing fixes

**Measured.** Ten seeds of `rpi4-i2s` with each design at commit
`9c83440` (steps 2 and 3 included), worst setup slack, slow model.

| | Seeds meeting timing | `clk[1]` worst (all the ADC pin) | `clk[2]` | Logic elements | Multiplier elements | M9K |
|---|---|---|---|---|---|---|
| Steps 2 and 3 (released filter) | 10 of 10 | +0.008 to +0.012 | +1.15 to +1.82 | 85% | 132 / 132 | 50 |
| B | 10 of 10 | +0.008 to +0.018 | +1.36 to +1.88 | 86% | 68 / 132 | 58 |
| A+B | 10 of 10 | +0.008 to +0.018 | +1.55 to +1.84 | 83% | 52 / 132 | 59 |

Logic, multipliers and M9K are seed 1's (seed 3's for the first row).

What it took, after the sweeps at `8432799` (section above):

- A+B's read addresses are per-class counters stepped by one, loaded at
  a group's, a pass's or a receiver's start; B's the same.
- Each sample memory's read has two registers inside `fir_sdpram` and
  one after it.
- No `max_depth` attribute. With each 768-word memory three 256-word
  blocks deep (the attribute), Quartus read the blocks through a select
  without their output registers wherever the RTL put them, and A+B
  missed on 2 of 10 seeds there; four blocks a memory costs eight more
  M9K and never failed in that path.

Design A was not taken further: A+B uses less logic and fewer multipliers
and has the same arithmetic.

### Counts from the other synthesis tool

Indicative only (a prototype; each image is judged on a full Quartus
build and seed sweep). Its subset was extended to take these sources
unchanged (memories in generate loops, depths that are not powers of
two, true dual-port memory and others), and its netlist of each matches
the source in Icarus.

| | Tables | Carry bits | Registers | Depth | Memory | Multipliers |
|---|---|---|---|---|---|---|
| Original `firX8R8`, one receiver | 272 | 340 | 548 | 7 | 4 RAM + 4 ROM blocks | 8 |
| A, one receiver | 326 | 284 | 344 | 4 | 2 true dual-port (4 M9K) + ROM | 4 |
| B, six receivers | 439 | 757 | 1 913 | 3 | 8 x 768 x 32 + ROM | 16 |
| A+B, six receivers | 262 | 540 | 1 324 | 4 | 8 x 768 x 32 + ROM | 8 |

For six receivers the original is six of its row (48 multipliers).

## 10. Decided

- **The receive filter is A+B** (`rtl/core/firsharedfold.sv`), in both
  images: one shared, folded filter for all receivers. Design B and the
  step 3 rework of the original filter are removed; design A
  (`rtl/core/firfold.sv`) stays as the simulation reference A+B is
  checked against.
- **No more receivers.** The stretch goal is dropped; the aim is margin
  and building with our own tools.

## 11. After this

The rest of the core is taken over block by block, each checked against
the original: [TODO/2026-11-17-core-takeover.md](../../TODO/2026-11-17-core-takeover.md).
