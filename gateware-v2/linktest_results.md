# Link test results

Measurements made with the link test image on one RadioBerry on a
Raspberry Pi 4. The image is written from scratch
([rtl/linktest](rtl/linktest)); it is not a radio and cannot transmit.
What it found about the board itself is in
[Hardware discovery](hardware_discovery.md), section 10. This page holds
the I2S link figures.

## Set-up

- FPGA: `linktest` image, I2S tester clocked at 122.88 MHz; the bit
  clock is that divided by a whole number. The AD9866 is programmed to
  give the FPGA 76.8 MHz first (register 0x04 = 0x36).
- Wires: GPIO 18 bit clock and GPIO 19 frame sync from the FPGA, GPIO 20
  data to the Pi, GPIO 21 data to the FPGA. Frame: 64 bits, two 32-bit
  words, frame sync high for the first 32 bits, most significant bit
  first, no bit of delay after the frame sync edge.
- Pi: the BCM2711 PCM block as clock and frame slave, programmed directly
  through `/dev/mem` by [host/pcmpoll.c](host/pcmpoll.c), which polls the
  FIFOs from one core at real-time priority. No kernel driver, no ALSA,
  no DMA.
- Each direction carries a known word sequence (xorshift32 unless
  stated) and the receiver checks every word against the one before.
- Because the Pi side polls, the 64-word FIFOs overflow or run dry when
  the process is held up. Those events are reported by the PCM block's
  own error flags and are counted separately; each causes a break in the
  sequence that is the host's, not the link's.

## FPGA to Pi

About 3.8 s per row.

| Bit clock | Words received | Sequence breaks | FIFO overruns on the Pi |
|---|---|---|---|
| 3.072 MHz (counting sequence) | 460 810 | 0 | 0 |
| 6.144 MHz | 729 624 | 0 | 0 |
| 12.288 MHz | 1 459 228 | 0 | 0 |
| 15.360 MHz | 1 824 035 | 0 | 0 |
| 20.480 MHz | 2 432 085 | 1 to 3 | 1 to 3 |
| 24.576 MHz | 2 918 460 | 1 to 2 | 1 to 3 |
| 30.720 MHz | 3 648 043 | 2 to 3 | 2 |
| 40.960 MHz | 4 863 948 | 3 to 5 | 3 to 5 |
| 61.44 MHz | 7 295 503 | 82 to 94 | 9 to 13 |

- Up to 15.36 MHz: no break at all in any run.
- From 20.48 to 40.96 MHz every break has a FIFO overrun to account for
  it: the polling host, not the wire.
- At 61.44 MHz the PCM block still delivers 1.92 million words per
  second, with breaks beyond what the overrun count explains; not
  separated into host and link.

## Pi to FPGA

The FPGA samples the Pi's data a chosen number of 8.14 ns steps after
its own rising clock edge. The Pi's data arrives late enough that above
about 20 MHz the right moment is in the following bit period.

| Bit clock | Sampling points that work | Words checked | Errors | FIFO underruns on the Pi |
|---|---|---|---|---|
| 3.072 MHz (counting sequence) | middle of the bit | 298 759 | 0 | 0 |
| 6.144 MHz | 5, 10 and 15 of 20 | 405 350 | 0 | 0 |
| 12.288 MHz | 5 and 7 of 10 (2 fails) | 810 906 | 0 | 0 |
| 15.360 MHz | 5, 6, 7 of 8 (1 to 4 fail) | 1 013 676 | 0 | 0 to 1 |
| 20.480 MHz | 5 of 6, and 1 to 4 of the next bit | 1 351 436 | 0 to 2 | 2 to 4 |
| 24.576 MHz | 1 to 4 of the next bit (all four) | 1 621 482 | 2 | 3 to 4 |
| 30.720 MHz | 1 to 3 of the next bit (all three) | 2 027 630 | 2 | 3 to 5 |
| 40.960 MHz | 2 of the next bit (1 fails) | 2 703 115 | 8 | 6 |
| 61.44 MHz | none of those tried | | | |

- The errors at 20 MHz and above come in step with the Pi's transmit
  FIFO running dry (the polling host): an underrun sends zeros, which
  breaks the sequence going in and coming out.
- At 24.576 MHz every sampling point in the following bit period works,
  a window of at least 24 ns. Where the Pi's data actually arrives is
  worked out in the DMA section below.

## With DMA

[host/kmod/rbpcm_dma.c](host/kmod/rbpcm_dma.c) drives the same PCM block
with one cyclic DMA transfer each way (4096-word periods, 16 per buffer),
checks every received word in the DMA callback and keeps the transmit
buffer filled with its own sequence. No ALSA, no sound card: the PCM
node stays disabled and only its DMA requests are used. Run with
[host/run_dma.sh](host/run_dma.sh); each row counts 20 s, from 1 s after
the start.

| Bit clock | FPGA sampling point | To the Pi: words, breaks, FIFO errors | To the FPGA: words, errors | Callback time, one core |
|---|---|---|---|---|
| 12.288 MHz | 5 of 10 | 7 749 632, 0, 0 | 7 749 218, 0 | 2.1% |
| 15.360 MHz | 6 of 8 | 9 695 232, 0, 0 | 9 691 686, 0 | 2.7% |
| 20.480 MHz | 2 of the next bit | 12 918 784, 0, 0 | 12 918 902, 0 | 3.5% |
| 24.576 MHz | 2 of the next bit | 15 507 456, 0, 0 | 15 506 526, 0 | 4.3% |
| 30.720 MHz | 2 of the next bit | 19 365 888, 0, 0 | 19 367 126, 0 | 5.1% |
| 40.960 MHz | 2 of the next bit | 25 833 472, 0, 0 | 25 832 271, 0 | 6.3% |
| 61.440 MHz | 1 of the next bit | 38 764 544, 0, 0 | 38 762 469, 36 338 064 | 9.6% |
| 61.440 MHz, 10 s | 0 or 1 two bits later | 19 558 400, 0, 0 | 19 556 798, 0 | |

- Every rate is loss-free towards the Pi, up to 61.44 MHz: 1.92 million
  32-bit words a second, 61.4 Mbit/s, more than six receivers at
  192 ksps need (55.3 Mbit/s).
- Towards the FPGA every rate is loss-free once the FPGA samples at the
  right point; at 61.44 MHz that is two bit periods after its own clock
  edge.
- The breaks seen with the polled tester above 15 MHz were the polling
  host: with DMA there are none.
- The callback copies each period out of uncached memory and checks
  every word of the sequence; a transport that only copies would cost
  less. Its longest run was 230 to 311 us.
- `/proc/stat` shows almost none of this as interrupt or softirq time
  (0.03 to 0.18% of the four cores): this kernel does not account
  interrupt time separately, so the callback's own timing is the figure
  to use.
- The FPGA's sampling point, in its own terms: the Pi's data for a bit
  arrives at the FPGA roughly 16 ns after the FPGA drives the rising
  clock edge, to within one 8.14 ns step, allowing for the image's
  two-register input synchroniser. A design should sample about half a
  bit period after that.

### Ten minutes at 61.44 MHz

FPGA sampling point 1, two bit periods after its clock edge.

| Conditions | To the Pi: words, breaks, FIFO errors | To the FPGA: words, errors | Callback, one core | Chip temperature |
|---|---|---|---|---|
| Idle Pi | 1 152 372 736, 0, 0 | 1 152 379 009, 0 | 10.4% | 41 to 42 C |
| `stress-ng --cpu 4` and a bulk stream out over Wi-Fi (rate not recorded) | 1 152 540 672, 0, 0 | 1 152 594 628, 0 | 5.8% | 49 to 73 C |

About 3.7e10 bits each way per run with no error: a bit error rate
below about 1e-10 at 95% confidence, per run and direction. Throttling
flags stayed 0. The callback's share falls under load because the cores
then run at full clock.

### The FPGA's clock stopping

The I2S tester was disabled for 1 s in the middle of a DMA stream at
24.576 MHz, then started again:

- While stopped, the PCM block takes and gives nothing and flags no FIFO
  error; the DMA simply waits.
- On restart the stream resumes by itself. Four words broke the
  sequence each way at the restart (the FPGA restarts its own sequence
  from the beginning), then none in the following 3 s: the two words of
  a frame did not swap.

So an FPGA reload is a pause, not a fault, for this transport.

## What this establishes

- The I2S link works in both directions at once with the frame format
  above, with the PCM block driven directly by DMA and no ALSA.
- It is loss-free in both directions at every bit clock tried, up to
  61.44 MHz, on one wire each way: 61.4 Mbit/s, more than the
  55.3 Mbit/s of six receivers' 24-bit values at 192 ksps. How many
  receivers that gives depends on the word format; with one value per
  32-bit word, as the first receive image sends, it is four at 192 ksps.
- The Pi-to-FPGA direction needs its sampling point set by design.

## What it does not establish

- Behaviour over hours, or on the other two boards.
- The cost of a real transport (copy without checking) inside the
  driver.

## Repeating it

On the Pi, as root, with the radio's overlays out of `config.txt` and
`dtparam=spi=on`:

```
rbload --force linktest.rbf
pinctrl set 13 ip pn
python3 linktest.py ad-reset release
python3 linktest.py ad-write 0x04 0x36
pinctrl set 18-21 a0
./run_i2s.sh DIV SAMPLE_POINT SEQUENCE WORD_OFFSET 0 SECONDS
```

`DIV` is 122.88 MHz divided by the bit clock; `WORD_OFFSET` 1 moves the
sampling point into the following bit period.
