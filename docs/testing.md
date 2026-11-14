# Test results

Results from hardware. The tests themselves: [CONTRIBUTING.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/CONTRIBUTING.md)
(CI), [driver hardware tests](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/docs/driver-tests.md),
[driver under a debug kernel](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/docs/kernel-debug.md),
and the gateware's [simulation, timing and hardware results](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/docs/rpi5-results.md).

## Test systems

| | Raspberry Pi 4 | Raspberry Pi 5 |
|---|---|---|
| Board | Model B Rev 1.1, 4 GB | Model B, 8 GB, NVMe root |
| OS | Raspberry Pi OS 64-bit (trixie), kernel 6.18.60+rpt-rpi-v8 | Raspberry Pi OS 64-bit (trixie), kernel 6.12.75+rpt-rpi-2712 |
| RadioBerry | 10CL025, PA5W V2 | 10CL025, PA5W V2 |
| Network | Wi-Fi | Wi-Fi only |

The RadioBerry's sample clock runs about 31 to 33 ppm fast against the
Pi's: `rbcat` measures 48001.5 to 48001.6 RX frames per second at 48 ksps.
`i2cdetect -y 1` with the PA5W V2 stacked finds no devices.

## Raspberry Pi 4

- `ft8rx` on 20 m: 67 messages in three slots over Protocol 1 (18 856
  packets) and 69 over Protocol 2 (9 066 packets), none lost.
- piHPSDR at one receiver at 192 ksps used at most 13% CPU.
- Idle, no client: the daemon made no system calls in 20 s and used no CPU
  in 60 s; the PCM block's DMA interrupt fired 0 times a second.
- Transmit: digital loopback self-test, +1 kHz tone at -43.6 dBFS with the
  opposite sideband at -69.3 dBFS; into a dummy load, about 1 dB of output
  per AD9866 gain code over codes 10 to 25, compression near 4.5 W; on air
  through piHPSDR and WSJT-X, FT8 decoded across Europe and in North
  America.

## Raspberry Pi 5

- `ft8rx` on 40 m: 4 messages in three slots over Protocol 1 (18 896
  packets) and 8 over Protocol 2 (10 740 packets), none lost.
- `rbdrivertest`: 12 of 12 test cases.
- Transmit: digital loopback self-test, +1 kHz tone at -18.7 dBFS with the
  opposite sideband at -144.7 dBFS; 1.4 W into a dummy load at drive 128;
  on air on 20 m FT8 through piHPSDR and WSJT-X, 41 transmissions with no
  samples dropped, spots in Europe, North America, Japan and Korea, and a
  contact.
- Sample order: `ft8rx` decoded 7 messages in two slots with each
  sample's second value as I, none in two slots the other way, and the
  decoded frequencies matched PSK Reporter spots of the same stations.
- Frequency calibration: decodes read 161 to 163 Hz low on 40 m (about
  -23 ppm) against PSK Reporter medians; with `frequency_ppm: 22.9`, 0 to
  +1 Hz on 40 m and -2 to 0 Hz on 30 m.

### Real-time behaviour and RX packet jitter

`radioberryd` serving one receiver at 48 ksps over Protocol 1 to a
receive-only test client (381 packets per second), 40 to 70 s per run.
Lateness is arrival time less ideal time from the sequence number, clock
drift fitted out; the earliest packet is 0.

| Client, conditions | p50 | p99 | p99.9 | max | sequence breaks |
|---|---|---|---|---|---|
| On the Pi (loopback), idle | 0.50 ms | 0.89 ms | 0.90 ms | 3.9 ms | 0 |
| On the Pi, 4 busy loops, client SCHED_FIFO 30, daemon `realtime: false` | 2.03 ms | 4.70 ms | 7.00 ms | 9.8 ms | 0 |
| On the Pi, 4 busy loops, client SCHED_FIFO 30, daemon `realtime: true` | 0.50 ms | 0.88 ms | 0.89 ms | 3.0 ms | 0 |
| Laptop on the same Wi-Fi, power save on | 0.74 ms | 90.9 ms | 123.6 ms | 128.3 ms | 3 |
| Laptop, power save off | 0.75 ms | 107.1 ms | 126.3 ms | 129.6 ms | 12 |
| Laptop, daemon real-time with DSCP EF | 0.73 ms | 109.5 ms | 125.9 ms | 128.7 ms | 0 |

- Real-time scheduling keeps delivery under load as on time as idle.
- The Wi-Fi tail is the Pi's own background scans: every 33.7 s for
  3.65 s, a train of stalls of about 130 ms. A client transmitting over
  that Wi-Fi can run the transmit FIFO dry during a scan; use Ethernet.
