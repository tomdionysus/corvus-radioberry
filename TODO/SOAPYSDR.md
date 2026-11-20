# SoapySDR module: what is left

The module (`soapy/`, package `soapysdr-module-radioberry`) is built,
documented ([docs/soapy.md](../docs/soapy.md),
[soapy/docs/design.md](../soapy/docs/design.md)) and tested on the
Raspberry Pi 5 and Pi 4. Released in 0.6.0.

Board owner's decisions: direct to `/dev/radioberry`; receive and transmit;
nothing that exists changes; an optional package, and the whole stack
builds without SoapySDR installed; driver key `radioberry`; `DRIVE` in
AD9866 dB; only the program unkeys the radio (no idle unkey); release
0.6.0.

## To do

- [ ] Quisk keyed on the Pi 4 (USB, no audio in): the image counted 180
      TX underruns in about 8 s keyed; the burst from a program that
      writes continuously counted none. Acceptance: the same count on the
      Pi 5, and what Quisk writes while keyed.
- [ ] `rbsoapytest` ([hardware tests](../soapy/docs/hardware-tests.md)) on
      the Raspberry Pi 5: not run there.
- [ ] The driver's TX underrun count rose by 5 across each loopback burst
      on the Pi 4, none of them while the burst was written; the image
      took every sample and counted no underrun. Acceptance: where the 5
      are counted, from the driver.
- [ ] Quisk on the Pi 4: three "RX samples dropped" warnings as it
      started, none in the 30 s after. Acceptance: the same start on the
      Pi 5 compared.

## Results

trixie, module from the packages, `radioberryd` stopped. Pi 5: 6.12.75.
Pi 4: 6.18.60.

| Check | Pi 5 | Pi 4 |
|---|---|---|
| `SoapySDRUtil` | | `--find` lists the radio with the service running; `--make` gives the busy message; `--probe` with it stopped |
| Rates, 60 s each | held, no overflow: six channels at 48, 96, 192 ksps, three at 384 ksps; `SoapySDRUtil` about 2% of a core | held, no overflow: six channels at 48 and 96 ksps, four at 192, two at 384; `SoapySDRUtil` 4 to 5% of a core |
| FT8 from a capture, `jt9` | six channels on six bands at once, 20 m 45 decodes a period; 0 overflows | six bands at once at 48 ksps (20 m 20 and 11 decodes, 40 m 13 and 9, 30 m, 17 m and 15 m fewer, 80 m none), four at 192 ksps; 0 overflows |
| Clients receiving | GNU Radio (45 decodes), Quisk 4.2.32, CubicSDR 0.2.7 | GNU Radio 3.10.12 (21 decodes), Quisk 4.2.32, CubicSDR 0.2.7 |
| Idle, device open | 0 context switches, 0 CPU ticks in 30 s | 0 context switches, 0 CPU ticks, no I2S or DMA interrupt in 30 s |
| `allow_tx: false` | TX refused; no command with run, permission, PA or PTT | TX activation `SOAPY_SDR_NOT_SUPPORTED`, writes fail, keying 0, no TX sample taken |
| Loopback burst | tone -20.4 dBFS, image below -146 dBFS, 0 image TX underruns, unkeyed when the last sample was taken | tone -21.2 dBFS, image -154 dBFS, the image took every sample, 0 image TX overflows and underruns, unkeyed 0.11 s after the last write |
| Program killed while keyed | keying, control, status2 0 | keying 0 |
| `rbsoapytest` | | 4 tests and the 2 loopback tests pass |
| Dummy load, `DRIVE` 5 dB | PTT and PA on and off cleanly | external PA in line: PTT and PA on and off cleanly, drive 67, unkeyed 0.11 s after the last write, 0 image TX overflows and underruns |
| Quisk keyed, PA HAT, external PA | external amplifier and tuner in line: 1.9 W at the tuner | dummy load, `DRIVE` 5 dB: keyed and unkeyed through rigctl, keying 0 after |
| Existing stack | `rbdrivertest` 12/12; `ft8rx` 0 lost over Protocol 1 and 2 | `rbdrivertest` 12/12; `ft8rx` 0 lost over Protocol 1 (23 decodes) and 2 (30) |
| Packages | with and without the module, the three existing packages equal to 0.5.0 in files, modes, control fields and scripts | built on the Pi 4, 374 tests pass; installed over 0.4.1 packages, `config.yaml` kept |
| CI | full run with the module, and on a machine without SoapySDR: all stages, coverage 100%; module tests clean under TSan | |

The loopback tone levels in the table are 6 dB low (the window's gain was
not removed); `rbsoapytest` reports -14.4 dBFS for the same burst on the
Pi 4.
