# Test results

Results from hardware. The tests themselves: [CONTRIBUTING.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/CONTRIBUTING.md)
(CI), [Driver hardware tests](driver-tests.md),
[Driver under a debug kernel](kernel-debug.md), and
[the gateware's receive lock-up](fault-rx-lockup.md).

## Test systems

| | Raspberry Pi 4 | Raspberry Pi 5 |
|---|---|---|
| Board | Model B Rev 1.1, 4 GB | Model B, 8 GB, NVMe root |
| OS | Raspberry Pi OS 64-bit (trixie), kernel 6.18.50+rpt-rpi-v8 | Raspberry Pi OS 64-bit (trixie), kernel 6.18.60+rpt-rpi-2712 |
| Toolchain | as Pi 5 | gcc 14.2, cmake 3.31, clang-format 19, lcov 2.0 |
| RadioBerry | 10CL025 | 10CL025 |
| Network | Wi-Fi | Wi-Fi only |

The RadioBerry's sample clock runs about 31 to 33 ppm fast against the
Pi's: `rbcat` measures 48001.5 to 48001.6 RX frames per second at 48 ksps.
CL016 images leave CONF_DONE low on a 10CL025. `i2cdetect -y 1` with the
PA5W V2 stacked finds no devices.

## Raspberry Pi 4

CPU at 1.5 GHz.

- `rbdrivertest` passes. Lock-up harness (the receiver-count test, hold
  200 us): 0 stalls in 700 runs; one run of the 700 failed without a
  stall, its output not kept.
- `rbcat`, one receiver at 48 ksps for 10 minutes: no resyncs, drops or
  overruns; the driver's thread took 38% of one core.
- `ft8rx` decodes over both protocols with no packets lost; WSJT-X through
  piHPSDR transmits FT8, decoded on PSK Reporter.
- Idle: with `/dev/radioberry` closed the driver's thread
  took 0% of a core over 10 s; with `radioberryd` running and no client,
  0.0% for the driver's thread and 0.2% for all the daemon's threads over
  30 s (before RX was gated on run: a whole core for the thread from boot). 100 client
  sessions, alternating `p1probe` and `p2probe` and one and two receivers,
  all passed with no stalls; `ft8rx` afterwards decoded 87 and 86 messages
  in four slots over Protocols 1 and 2 with no packets lost, and the
  thread went back to 0%.
- `rbbus` (the driver's bus code in userspace, `chrt -f 50`, 30 s per
  setting, `rbload` before each):

  | Setting | RX values/s | Clean | CPU, one core |
  |---|---|---|---|
  | 1 receiver at 48 ksps | 96 035 | yes | 22% |
  | 2 at 48 | 192 005 | yes | 29% |
  | 1 at 96 | 192 011 | yes | 100% |
  | 3 at 48 | 288 007 | yes | 100% |
  | 4 at 48; 1 at 192; 2 at 96 | 384 010 to 384 041 | yes | 100% |
  | 1 at 384 | 767 905 | yes | 100% |
  | 6 at 96, `--idle-sleep 0` | 1 098 240 of 1 152 000 | no, 4.7% of frames lost | 100% |
  | with TX at 48 000 words/s: 1 at 48; 2 at 48 | as without | yes | 64%; 69% |
  | with TX: 1 at 96; 1 at 192 | as without | yes | 100% |

  No setting needed `--settle`. Above 192 000 values per second, and at
  one receiver at 96 ksps, the loop never reaches its idle sleep. Hence
  the driver's default limit of 192 000.

To repeat the `rbbus` runs: comment out `dtoverlay=radioberry`, set
`dtparam=spi=on` and reboot; then for each setting `sudo rbload` and
`sudo chrt -f 50 rbbus --seconds 30` with its options; restore the
overlay and reboot.

## Raspberry Pi 5

- Receive capacity: 2.37 million values per second at saturation, no
  stream errors below it.
- `ft8rx` on 40 m over loopback: 6 slots, 23 messages (SNR -17 to +2 dB,
  DT -0.2 to 0.5 s), 37 078 packets, none lost.
- Sample order: `ft8rx` decoded 7 messages in two slots with each
  sample's second value as I, none in two slots the other way, and the
  decoded frequencies matched PSK Reporter spots of the same stations.
- Frequency calibration: decodes read 161 to 163 Hz low on 40 m (about
  -23 ppm) against PSK Reporter medians; with `--frequency-ppm 22.9`, 0 to
  +1 Hz on 40 m and -2 to 0 Hz on 30 m.

### Real-time behaviour and RX packet jitter

`radioberryd` serving one receiver at 48 ksps over Protocol 1 to a
receive-only test client (381 packets per second), 40 to 70 s per run.
Lateness is arrival time less ideal time from the sequence number, clock
drift fitted out; the earliest packet is 0.

| Client, conditions | p50 | p99 | p99.9 | max | sequence breaks |
|---|---|---|---|---|---|
| On the Pi (loopback), idle | 0.50 ms | 0.89 ms | 0.90 ms | 3.9 ms | 0 |
| On the Pi, 4 busy loops, client SCHED_FIFO 30, daemon `--no-realtime` | 2.03 ms | 4.70 ms | 7.00 ms | 9.8 ms | 0 |
| On the Pi, 4 busy loops, client SCHED_FIFO 30, daemon real-time | 0.50 ms | 0.88 ms | 0.89 ms | 3.0 ms | 0 |
| Laptop on the same Wi-Fi, power save on | 0.74 ms | 90.9 ms | 123.6 ms | 128.3 ms | 3 |
| Laptop, power save off | 0.75 ms | 107.1 ms | 126.3 ms | 129.6 ms | 12 |
| Laptop, daemon real-time with DSCP EF | 0.73 ms | 109.5 ms | 125.9 ms | 128.7 ms | 0 |

- Real-time scheduling keeps delivery under load as on time as idle.
- The Wi-Fi tail is the Pi's own background scans: every 33.7 s for
  3.65 s, a train of stalls of about 130 ms. That exceeds the gateware's
  85 ms TX FIFO, so a Protocol 2 client transmitting over that Wi-Fi runs
  the FIFO dry during a scan.
