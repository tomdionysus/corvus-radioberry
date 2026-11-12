# Finding: CPU cost of receive on the Raspberry Pi 4

## Summary

On a Raspberry Pi 4, receiving one receiver at 96 ksps or more holds the
driver's kernel thread at 100% of one CPU core. No samples are lost, up
to one receiver at 384 ksps, and the other three cores stay free. Most of
that core is spent polling the RX bus for the next value, not moving
data: at one receiver at 192 ksps, 63% of the bus clocks find no value
ready.

A second effect is open: piHPSDR at one receiver at 48 ksps also held the
thread at 100%, where `p1probe` at the same rate costs 17%.

The planned fix reads the RX bus with the BCM2711's SMI peripheral and
DMA
([TODO/RPI4-SMI.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/TODO/RPI4-SMI.md)).
The Raspberry Pi 5 is not affected: its RX bus is clocked by RP1 PIO, not
the CPU.

## Report

A user with a Pi 4 and version 0.4.1 ran piHPSDR and Quisk at 192 ksps:
one core at 100%, the others above 50%, and a WSJT-X WSPR decode took
about a minute. The original RadioBerry software on the same Pi ran 192
ksps at 30 to 50% per core, with the decode in about 10 s.

## Reproduction

Pi 4 Model B, version 0.4.1 from the apt repository, kernel 6.18.60, CPU
at 1.5 GHz, no throttling, receive only on 7.074 MHz. Each figure is one
run unless a count is given.

piHPSDR on the Pi's desktop, Protocol 1, one receiver at 192 ksps, 60 s:

| | CPU |
|---|---|
| Cores | 19%, 100%, 20%, 19% |
| Driver's thread (`radioberry`) | 101.5% of a core |
| piHPSDR, six busiest threads | 43% |
| `radioberryd`, all threads | 12% |

The one core at 100% is reproduced. The other cores above 50% and the
slow WSPR decode are not: `wsprd -d` decoded a simulated WSPR file in
0.93 to 0.95 s during that stream (5 runs) and in 0.87 to 0.90 s with
nothing streaming (5 runs). The file holds one signal; a decode of real
band audio in WSJT-X was not timed.

## The driver's thread against load

`rbcat` reading `/dev/radioberry` for 20 s per setting; every stream was
clean (no resyncs, dropped words or overruns):

| Setting | RX values/s | Driver's thread |
|---|---|---|
| 1 receiver at 48 ksps | 96 000 | 14% |
| 2 at 48 | 192 000 | 29% |
| 1 at 96 | 192 000 | 100% |
| 1 at 192 | 384 000 | 100% |
| 1 at 384 | 768 000 | 100% |

Through `radioberryd` with `p1probe` and `p2probe`, 30 s per setting, no
packets lost: 17% at one receiver at 48 ksps, 51% at two, 100% at one at
96 and at every higher load, over both protocols.

Two receivers at 48 ksps and one at 96 carry the same 192 000 values per
second and cost 29% and 100%.

## Cause

The thread
([`rb_bb_worker`](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/kernel/radioberry.c))
clocks the RX bus in runs of up to 256 values and sleeps 0.5 to 1 ms only
after a run that moved nothing. A run ends after 64 clocks in a row
without a value, and a run that moved even one value is followed at once
by the next. The gateware offers a value only while more than 12 are
waiting in its FIFO, so once the thread has drained the FIFO to 12 it
takes each new value as it arrives and clocks idly in between. From one
receiver at 96 ksps up, the thread is measured never to sleep.

`rbbus` (the same bus code in userspace, `chrt -f 50`, 20 s per setting,
`rbload` before each) counts them:

| Setting | RX values/s | Idle clocks | CPU, one core |
|---|---|---|---|
| 1 at 48 | 96 047 | 56% | 28% |
| 2 at 48 | 191 995 | 15% | 22% |
| 1 at 96 | 192 054 | 81% | 100% |
| 1 at 192 | 383 970 | 63% | 100% |
| 1 at 384 | 768 018 | 29% | 100% |
| 6 at 96, no idle sleep | 1 103 819, 4.2% of frames lost | 0% | 100% |

While it spins, the loop makes 8.2 to 8.8 million bus clocks per second.
The last row is the bus's ceiling when clocked by the CPU: about 1.1
million values per second with no idle clocks at all.

Why two receivers at 48 ksps let the thread sleep and one at 96 does not
has not been traced in the gateware.

## The original software

The original software uses its own GPIO-mode gateware: an 8-bit bus read
on both clock edges, 12 register accesses per I/Q sample against 48 here,
with an interrupt on sample-ready instead of polling. Its CPU cost was
not measured here; the figures in the report are the user's.

## Open: piHPSDR at 48 ksps

piHPSDR at one receiver at 48 ksps, two sessions of 30 and 60 s:

| | Session 1 | Session 2 |
|---|---|---|
| Driver's thread | 102% | 104% |
| `radioberryd` `rb-rx-device` | 61% | 63% |
| `radioberryd` `rb-rx-send` | 29% | 30% |

`p1probe` at the same rate in the same boot: 17% for the driver's thread
(4 runs). Both clients send about 375 packets per second, with zero TX
samples and MOX clear. A client replaying `p1probe`'s commands at that
rate gave 100% once, started 4 s after piHPSDR was killed, and 17% once,
after a `p1probe` run. The commands piHPSDR sends are therefore not
enough to cause it; the cause is not found
([TODO/ACTIVE.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/TODO/ACTIVE.md),
section 2).

## Consequences

- The driver's limit of 192 000 RX values per second
  (`bb_rx_values_max`) does not bound the CPU cost: one receiver at 96
  ksps is within it and costs a core.
- A client's DSP has three cores. piHPSDR at 192 ksps ran in them with
  about 80% of each idle.
- Reducing the register accesses per sample alone would not free the
  core while the thread polls. A CPU transport that sleeps between
  bursts was not tried; the SMI transport removes the CPU from the RX bus
  altogether.
