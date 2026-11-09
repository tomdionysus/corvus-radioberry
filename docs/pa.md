# PA: the amplifier board

The supported amplifier is the AURSINC "Radioberry Preamp Board V2" (PCB
marked "PA5W V2" and "RB_PA5W"), a two-layer board mounted on the
Radioberry. It connects through a header on the Radioberry itself, not
the Raspberry Pi's 40-pin header. It is a Hermes-Lite 2 style push-pull
PA stage, manually biased, current-sensed by an INA199A1, and keyed by
the Radioberry's CN301 lines. The seller supports it only with the
Radioberry Juice USB board; its use on a Raspberry Pi is verified by this
project only.

## The board

- 1.5 to 3.5 W out; DC in on the BAT connector (silkscreen "5-8V"; an 8 V
  lead is supplied).
- Two bias trimmers, BIAS1 and BIAS2, "2.7V Max" on each. No digital
  potentiometer.
- PA current: PA_CUR header, the output of an INA199A1 with a 40 milliohm
  shunt, 2 V per 1 A. Expect at least 2 V (1 A) when transmitting; drive
  present, TX LED on and current low means a failed FET.
- Drive needed: at least 7 V peak-to-peak into the board from the
  Radioberry.
- No I2C devices: `i2cdetect -y 1` finds nothing with the board stacked
  (no MAX11613 ADC, no MCP4662 bias pot). There is no temperature or
  current telemetry.
- No switched multi-band low-pass filter bank is visible; any real LPF has
  to be external.

| Marking | Meaning |
|---|---|
| RB_RX, RB_TX (SMA, left edge) | cabled to the Radioberry RX in and TX out |
| ANT (SMA, right edge) | antenna, 1.5 to 3.5 W out |
| BAT (2-pin, right edge) | DC in |
| BIAS1, BIAS2 | bias trimmers |
| MIC=PTT jack, "L=dot R=DASH" jack | PTT and paddle inputs |
| PA_CUR (2x5 header) | PA current sense output and related lines |
| TX LED, SW1 | TX indicator, pushbutton |
| T1, L1 to L5, Q2/Q3 area | push-pull output stage and output transformer |
| U8, U12, RLY1 | driver op-amp, current sense amplifier, T/R relay |

## Radioberry CN301

CN301 pins are direct FPGA pins: 1 5V, 2 3V3, 3 ptt_out / exttr (FPGA
144), 4 inttr (143), 5 keyer dash (142), 6 keyer dot (141), 7 preamp bias
(137), 8 envpa (136), 9 and 10 unused (135, 133), 13 PTT in (121), 14 key
in (120), 15/16 FPGA SDA/SCL (119/115), 17/18 SDA/SCL from the Pi header,
19/20 GND. What drives each output is in
[gateware/README.md](../gateware/README.md) section 6.

The board's PTT, key and paddle inputs go to the FPGA's CN301 pins, not to
the Pi, so they work whatever software runs.

## Setting the bias

With no drive, raise BIAS1 and BIAS2 from minimum until PA_CUR shows the
intended idle current, never exceeding 2.7 V on either trimmer. The
target idle current for the AURSINC FETs is not published. The
development board's bias is set.

## Software

- `radioberryd` passes on a client's PA enable (command 0x09 bit 19, which
  drives envpa) only with `--allow-pa`; the installed config has it off.
- There are no thermal or current interlocks: the board has no sensors
  the software can read. Protection is the operator, the trimmer ceiling,
  the drive ceiling and the transmit watchdog.
- The first test of any new code path that can key the transmitter is
  into a dummy load.

## Unknown

- The PA_CUR header pinout beyond the current output.
- Which CN301 line switches the T/R relay: ptt_out (FPGA 144), inttr (143)
  or envpa (136).

## References

- AURSINC product page: https://www.aursinc.com/products/aursinc-radioberry-preamp-board-v2-output-1-5w-3w-for-hf-power-ampfier-10w-100w-pa-for-raspberry-pi-4-4b
- Test notes for the board: https://jumbo5566.github.io/pro/rb/2024/04/17/rb016_025.html
- Radioberry CN301 pinout: https://github.com/pa3gsb/Radioberry-2.x/wiki/Radioberry-In-Output
- Hermes-Lite 2 protocol (PA enable, bias, ADC fields): https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol
