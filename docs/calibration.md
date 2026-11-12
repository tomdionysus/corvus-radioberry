# Frequency calibration

Each RadioBerry's clock is off by a small, fixed proportion, so every
frequency it tunes is off by the same number of parts per million.
`radioberryd` corrects this with one figure, `frequency_ppm`: positive if
the radio tunes high. It applies to every band, receive and transmit.

The figure is measured against carriers on known frequencies (standard
time and frequency stations), with WSJT-X's FreqCal mode.

## What you need

- `radioberryd` receiving, with `frequency_ppm: 0` in
  `/etc/radioberry/config.yaml` (`sudo systemctl restart radioberry`
  after changing it).
- A client with CAT control that WSJT-X can drive, such as piHPSDR, with
  its own frequency correction, if any, at zero.
- WSJT-X 2.7 or later, set up for that client as for FT8.
- An antenna that hears at least one reference station:

| Station | Carriers (MHz) |
|---|---|
| WWV (USA) | 2.5, 5, 10, 15, 20 |
| CHU (Canada) | 3.330, 7.850, 14.670 |
| RWM (Russia) | 4.996, 9.996, 14.996 |

The higher the frequency, the more hertz each ppm is (10 Hz per ppm at
10 MHz), so use the highest carrier you hear well.

## Measure

1. In WSJT-X, choose Mode, FreqCal.
2. In File, Settings, Frequencies, Working Frequencies, keep the FreqCal
   entries for the stations you hear and delete the rest; add any missing
   carrier from the table.
3. Delete `fmt.all` from the WSJT-X log directory (File, Open log
   directory).
4. Set FTol wide enough to catch the carrier: an uncalibrated RadioBerry
   can be a few hundred hertz off at 10 MHz. Tuning the dial by hand to
   one carrier first shows how far off it sits on the waterfall.
5. On the Tools menu, check Execute frequency calibration cycle and
   Measure. WSJT-X spends 30 seconds on each frequency, with the dial
   1500 Hz below the carrier, and records the carrier's offset from
   1500 Hz.
6. Let it run at least one full cycle, then clear Execute frequency
   calibration cycle.

The WSJT-X User Guide's Frequency Calibration chapter covers FreqCal in
full.

## Compute

Either:

- one carrier: `frequency_ppm` = −(offset in Hz) ÷ (carrier in MHz).
  A carrier at 10 MHz measured 229 Hz low (offset −229) gives
  `frequency_ppm: 22.9`.
- every carrier measured: edit `fmt.all` to remove outliers, then Tools,
  Solve for calibration parameters. Its Slope is in ppm with the same
  sign as the offsets; `frequency_ppm` = −Slope. Do not press Apply:
  the correction belongs in `radioberryd`, where it covers every client,
  and WSJT-X's own calibration stays at zero.

## Set and check

1. Put the figure in `/etc/radioberry/config.yaml`
   (`frequency_ppm: 22.9`) and `sudo systemctl restart radioberry`.
2. In FreqCal with Measure cleared, run the cycle again: each carrier
   now sits within a hertz or two of 1500 Hz.

The figure belongs to the RadioBerry, not the Pi: move it with the board.
