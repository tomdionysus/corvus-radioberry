# PA: the amplifier board stacked on the Radioberry

The Pi described in `platform.md` carries a Radioberry 2.x card and, on top
of it, a small HF amplifier board of the "Radioberry PA HAT" family. The
user describes it as a copy of the hish8612 Radioberry HAT PA30 design in
the variant that has only manual bias potentiometers, bought from the
AliExpress listing 1005008233825206. The board has not been inspected
directly in this session. What follows is assembled from the published
schematics of the three related designs, the seller's listing, and the
upstream software that talks to boards of this family.

Survey date: 2026-11-08.

## 1. The family, and which member is on the Pi

There are four boards with overlapping names. Keeping them apart matters,
because two of them have power transistors and two do not.

| Board | What it is | PA FETs on board | Bias | I2C devices |
|---|---|---|---|---|
| pa3gsb "Radioberry preamp" (2020, 5 W) | 12 V, 5 W PA on a HAT, relay T/R | yes, two LDMOS (AFT05MS003NT1, alt RD15HVF1 / RD16HHF1 / RD06HHF1 / PD85004) | MCP4662 digital pot, LP2985-10 LDO | MCP4662 0x2C, MAX11613 0x34 |
| pa3gsb `hardware/hat` (2024, "rb-hat") | 300 mW OPA2674 TX driver, T/R relays, PureSignal sampler | no | none | none |
| hish8612 `Radioberry_hat_pa30` (beta0.2, 2024) | the 2024 hat re-laid out to drive JI1UDD's HL2-PA30, plus ADC, key/paddle/PTT jacks, sidetone | no | none on the hat | MAX11613 0x34 |
| JI1UDD HL2-PA30 (2023) | separate 30 W RD15HVF1 push-pull PA with LPF bank | yes | manual, RV1/RV2 trimmers | MCP23008 0x20 (filters) |
| AURSINC "Radioberry Preamp Board V2", PCB "PA5W V2" | commercial HAT, 1.5 to 3.5 W out, trimmer bias | yes (push-pull, T1 output transformer) | manual, BIAS1/BIAS2 trimmers | none (bus scanned 2026-11-08) |

The board on the Pi is the last row. By its features (output stage and
output transformer on the HAT itself, two bias trimmers, INA199A1 current
sense with a 40 milliohm shunt, "PA5W" silkscreen, 1.5 to 3.5 W out) it is a
derivative of the first row, pa3gsb's 5 W "Radioberry preamp", with the
MCP4662 digital bias replaced by trimmers. The hish8612 PA30 hat has no
transistors and no bias circuit at all, so "hish8612 variant with manual
pots" and "pa3gsb 5 W preamp with pots" describe the same electrical
reality: a Hermes-Lite 2 style PA stage, manually biased, measured by an
INA199A1 and optionally a MAX11613, keyed by the Radioberry's CN301 lines.
Nothing in any of the three upstream repositories, their issues, commits,
or the radioberry Google Group documents a manual-bias variant; the
AURSINC board is the only one found.

## 2. pa3gsb designs

### 2.1 The 5 W "Radioberry preamp" (the electrical ancestor)

Sources: wiki "Radioberry preamp"; in the upstream Radioberry-2.x
repository, the stale netlist `hardware/hat/hat.net` whose header still
names `radioberry-preamp.sch` dated 2020-12-20, and
`hardware/hat/bom/parts.json`.

- 12 V in, about 5 W out, two FETs (Q301/Q302, AFT05MS003NT1 in the
  netlist with the alternates above), relay T/R, ST1S10 buck for the
  logic rail, LP2985-10 LDO for the bias reference.
- MCP4662 dual digital potentiometer at 0x2C sets both gate biases; values
  can be stored in its EEPROM. MAX11613 at 0x34 measures current (INA199A1,
  0.04 ohm shunt) and temperature.
- Firmware behaviour from the wiki: PA switched off if current and
  temperature cannot be measured; off above 50 C; re-enabled after 10 s in
  range.

### 2.2 The 2024 `hardware/hat` (the mechanical ancestor)

Sources: `hardware/hat/*.kicad_sch` (KiCad 8, 2024-08-25), the beta0.1
schematic PDF under `hardware/hat/release/beta0.1`, group thread
"A hat for the radioberry" (2024-04-16).

- Anti-aliasing LPF, TC4-1T, OPA2674 with gain 3.85 ("15 x 20 mW rb =
  300 mW"), ADT1.5-1+ output transformer, three Omron G6K-2 relays
  (sampler/RX and PA/antenna T/R), 20 dB sampler pad. SMAs: To Radioberry
  RX, From Sampler, PA/Antenna, From TX Radioberry. 12 V in, polyfuse,
  SMBJ18CA.
- JP201 2x10 onto Radioberry CN301: pin 3 rb_relais (drives relay coils via
  NUD3124), pin 4 rb_exttr (open drain EXTTR), 5 dash, 6 dot, 13 key,
  14 ptt, 19/20 GND, 1 +3V3; the rest unconnected. DB201 external 2x5:
  2 EXTDOT, 4 EXTDASH, 6 EXTKEY, 8 EXTPTT, 10 EXTTR, odd pins GND. EXTTR
  is grounded during TX; the external PA supplies the pull-up, up to 28 V.

### 2.3 Radioberry CN301 and I2C (common to all members)

From wiki "Radioberry In Output" and "GPIO RPI Radioberry":

- CN301 pins are direct FPGA pins: 1 5V, 2 3V3, 3 ptt_out / preamp exttr
  (FPGA 144), 4 preamp inttr (143), 5 keyer dash (142), 6 keyer dot (141),
  7 preamp bias (137), 8 preamp envpa (136), 9 and 10 no use (135, 133),
  13 PTT in (121), 14 key in (120), 15/16 FPGA SDA/SCL (119/115),
  17/18 SDA/SCL from the Pi header, 19/20 GND.
- The Pi's own I2C (GPIO 2/3) reaches the HAT through CN301 17/18. No Pi
  GPIO is used for PTT, filters or bias; those go through the FPGA or I2C.
- Filter boards supported by the daemon: N2ADR at 0x20 (MCP23008), Alex at
  0x21 (PCA9555), VA2SAJ generic at 0x22. Priority if several answer:
  Alex, generic, N2ADR.

## 3. hish8612 Radioberry_hat_pa30 (JG1TWP "Yado-san" and PA3GSB, rev0.2)

Sources: https://github.com/hish8612/Radioberry_hat_pa30 (27 commits,
2024-10-23 to 2025-02-17, no issues), its project page, the four KiCad
sheets, `release/beta0.2/hat.pa30_bom.csv` (98 parts) and `hat.pa30.net`.

- Amplifier sheet is the 2024 pa3gsb hat without relays: SMA "From TX
  Radioberry", anti-aliasing filter, optional attenuator pads, TC4-1T,
  OPA2674 (510/300 ohm feedback), ADT1.5-1+, SMA "To PA30".
- Power sheet: CN101 "+12 V from PA30" (Vtx), CN102 PA supply pass-through
  with two 0.04 ohm resistors in parallel (0.02 ohm) and an INA199A1DC
  giving "Vipa". No regulators.
- Control sheet: one I2C device, U304 MAX11613 at 0x34, channels AIN0 Vfwd,
  AIN1 Vtemp (MCP9700 analog sensor on flying leads to the PA heatsink),
  AIN2 Vipa, AIN3 Vrev. This is the byte order the daemon decodes.
  No DAC, no digital pot, no bias circuit anywhere on the board.
- JP201 onto CN301: 1 +3V3, 3 rb_inttr (NUD3124 open drain to INTTR),
  4 rb_exttr (to EXTTR), 5 dash, 6 dot, 10 sidetone PWM through a two-stage
  RC filter, 13 key, 14 ptt, 15/16 FPGA SCL/SDA via 0 ohm links (for Juice
  use), 17/18 Pi SCL/SDA, 19/20 GND.
- CN1 3.5 mm: tip dot, ring dash ("paddle or straight key, ground to
  key"); CN2 3.5 mm: tip PTT, ring key. All four pulled up to 3V3 with
  2.2 k and 1 uF.
- DB1 2x5 to the PA30: 1 Vrev, 3 Vfwd, 5 INTTR, 7 EXTTR, 9 sidetone, even
  pins GND. "External PTT will ground EXTTR during TX; external PA to
  supply pull-up voltage up to 28 V".
- Sidetone needs the CL025 gateware variant `radioberry_cl025_cwsidetone`
  (CN301 pin 10 PWM at 18.73 kHz, 695 Hz tone, pin 9 key-down LED).
- Required HL2-PA30 modifications (image `rev_hl2_pa30.png`): add a 78M09
  fed from B12V to the fan header, cut the B12V trace, rewire J5 to DB1 and
  J8 to the Radioberry RX.

## 4. JI1UDD HL2-PA30 (the 30 W stage the PA30 hat was designed for)

Sources: https://github.com/ji1udd/HL2-PA30 README, schematic
`Schematic/PA30_v2r1_schematic.pdf` (rev 2.1, 2023-03-12, "not actually
built"; v1 was built), `BOM/PA30_BOM.pdf`.

- RD15HVF1 push-pull, 30 W, 19.5 V drain supply from a laptop adapter,
  3 A polyfuse, P-channel DMP3036 switch keyed by TX. RD16HHF1 works with
  lower output. HL2 drive must be reduced (R55 120 to 750 ohm, 37 to
  26 dBm) "otherwise Q1 and Q2 will be broken".
- Bias is manual only: TA76431F shunt reference gives a 7.8 V rail, two
  10 k Bourns 3296W trimmers RV1/RV2 set the gates, idle 250 mA per
  transistor (500 mA total). The bias rail is switched on only in TX via
  INTTR. There is no DAC and no digital potentiometer.
- Six-band LPF bank (160, 80, 40, 30/20, 17/15, 12/10 m) with 12 EA2-12
  relays, RX HPF, T/R relay, Alex-type FWD/REV detectors with LMC6482
  buffers, PureSignal feedback tap with pads.
- Filter switching: MCP23008 at 0x20 through a TPL7407 driver, so it is
  N2ADR-protocol compatible and the Radioberry daemon drives it unchanged.
  Yado-san confirms 0x34 (MAX11613) and 0x20 (MCP23008) on the same bus.
- Connectors: J5 DB13 2x5 (SDA, EXTTR, SCL, INTTR, 3V3, REV, FWD, GND),
  J7 12 V link, J1/J8 uFL PA in and RX out, J4 antenna, J3 DC 19.5 V,
  J6 fan. No temperature sensor on the PA30 itself.

## 5. The board on the Pi: AURSINC "Radioberry Preamp Board V2" (PA5W V2)

Sources: AliExpress listing 1005008233825206 (AURSINC Store), read in a
browser on 2026-11-08; the "how to work" page it links to,
https://jumbo5566.github.io/pro/rb/2024/04/17/rb016_025.html (BG7IYN,
"Radioberry PAv2 Test", 2024-04-17).

Listing facts:

- Title: "Radioberry Preamp Board V2 Output 1.5W-3W for HF Power Amplifier
  (10W-100W PA) Assemble with Radioberry and Radioberry Juice USB". About
  117 EUR, 65 sold, PCB silkscreen "PA5W V2" and "RB_PA5W".
- Seller text: "turn the radioberry into a small QRP radio. The design is
  based on all the amazing work of Hermes Lite 2 Group." Two-layer board
  plugged on top of the Radioberry.
- The seller states it is "not compatible with Raspberry Pi" and is only
  computer-controlled through the Radioberry Juice USB board stacked below.
  That is a statement about the seller's supported stack, not an electrical
  one: the board sits on the same 40-pin bus and the same CN301 signals as
  the pa3gsb HATs. Treat the Pi path as unsupported by the seller and
  verified only by us.
- In the box: the board, three SMA cables (RX, TX, ANT), one "8 V
  battery-powered wire", header, 32 mm screws, 5 mm nuts, 11 mm standoffs.
- Seller screenshots show SparkSDR setup: General tab, "Tune drive" 255 and
  "PTT hang time" 30; PA tab, tick "Enable PA" (Hermes Lite 2 PA). "Enable
  bias setting" is left unticked, consistent with no MCP4662.

Visible on the board photos and the seller's annotated diagram:

| Marking | Meaning |
|---|---|
| RB_RX, RB_TX (SMA, left edge) | cabled to the Radioberry RX in and TX out; the diagram labels RB_TX "RF in" and the middle SMA "NC" |
| ANT (SMA, right edge) | antenna, "1.5-3.5W out" |
| BAT (2-pin, right edge, red/black lead) | DC in, "DC" and "GND"; silkscreen near the bias area reads "5-8V" |
| BIAS1, BIAS2 | two blue multi-turn trimmers, diagram note "2.7V Max" on each |
| MIC=PTT (3.5 mm jack) and a second jack "L=dot R=DASH" | PTT and paddle inputs, as on the hish8612 board |
| PA_CUR (2x5 header) | PA current sense output and related lines |
| TX LED (red), SW1 | TX indicator, pushbutton |
| T1 (ferrite output transformer), L1-L5, Q2/Q3 area | push-pull output stage; no switched multi-band LPF bank is visible |
| U8, U12 (SOIC), relay RLY1 | driver op-amp, current sense amplifier, T/R relay |
| 2x20 header | Raspberry Pi bus pass-through |

BG7IYN's test page adds:

- Check the drive: at least 7 V peak-to-peak into the board from the
  Radioberry.
- "PA Curr" is the output of an INA199A1DCKR with a 40 milliohm shunt,
  2 V per 1 A. Expect at least 2 V (1 A) when transmitting; if drive is
  present and the TX LED is red but current stays low, the FET has failed.
- The only software step is "Enable PA" in SparkSDR, which sets the
  Hermes-Lite PA-enable bit.

## 6. What the manual-bias board means for software

Measured against the pa3gsb daemon in the upstream Radioberry-2.x
repository, `SBC/rpi-5/device_driver/firmware`:

- `bias.c` probes an MCP4662 at 0x2C on `/dev/i2c-1`. None is present; the
  daemon prints "MCP 4662 not present" and ignores the Hermes-Lite bias
  commands. Bias is set once with BIAS1 and BIAS2 while reading the PA_CUR
  voltage.
- `measure.c` probes a MAX11613 at 0x34. On 2026-11-08, with I2C enabled
  on the Pi and the board stacked, `i2cdetect -y 1` found nothing on the
  bus: there is no MAX11613 and no MCP4662 on the PA5W V2. PA current is
  available only as the analogue PA_CUR voltage.
- With the ADC absent the daemon forces the PA-enable bit off: in
  `handleCommand`, for C0 0x12 it masks bit 19 (C2 bit 3) of the command
  data unless `i2c_measure_module_active` is set. The gateware's "envpa"
  output to the HAT therefore never asserts and SparkSDR's "Enable PA"
  checkbox has no effect through this daemon. `rb_command` bit 2 (PA
  permission) stays set because `pa_temp_ok` defaults to 1.
- The thermal interlocks (PA off above 50 C, re-enable after 10 s) depend
  on the ADC and do not exist on this board. Protection is the operator,
  the 2.7 V trimmer ceiling, and whatever the gateware does with envpa.
- radioberry-p2 makes the same decision explicit: PA bits off unless
  `--enable-pa` is given, with the README warning that no interlock is
  reproduced. For this board `--enable-pa` is the only way to key the PA
  stage, and the first tests must be into a dummy load.
- PTT, key and paddle inputs on the board go to the Radioberry CN301
  PTT/KEY/dot/dash FPGA pins, not to the Pi, so they work regardless of
  which daemon runs.
- No filter board is present, so the N2ADR/Alex/VA2SAJ code paths stay
  idle. Band filtering, if any, is a single fixed network on the HAT; any
  real LPF has to be external.

## 7. Open questions before RF is applied

1. Done 2026-11-08: I2C enabled and scanned, bus empty. If the board's
   ADC were powered only from BAT, a second scan with BAT connected would
   settle it; the hish8612 design powers its ADC from 3V3 on the header,
   so the result is taken as final.
2. Trace the PA_CUR header pinout and the BAT supply range on the actual
   board (silkscreen 5 to 8 V, listing supplies an 8 V lead, the pa3gsb 5 W
   ancestor wanted 12 V).
3. Confirm which CN301 line the board uses for PA enable and T/R, and
   whether the T/R relay follows ptt_out (FPGA 144), inttr (143) or envpa
   (136).
4. Set idle bias: with no drive, raise BIAS1 and BIAS2 from minimum until
   PA_CUR shows the intended idle current, never exceeding 2.7 V on either
   trimmer. The 5 W ancestor's LDMOS pair and the PA30's RD15HVF1 pair both
   run in the low hundreds of milliamps per device; the exact target for the
   AURSINC FETs is not published.
5. First TX into a dummy load at zero drive, then raise drive while
   watching PA_CUR and the Pi's own temperature.

## 8. References

- pa3gsb preamp wiki: https://github.com/pa3gsb/Radioberry-2.x/wiki/Radioberry-preamp
- Radioberry CN301 pinout: https://github.com/pa3gsb/Radioberry-2.x/wiki/Radioberry-In-Output
- Radioberry Pi GPIO map: https://github.com/pa3gsb/Radioberry-2.x/wiki/GPIO-RPI---Radioberry
- pa3gsb hat hardware and stale 5 W preamp netlist: https://github.com/pa3gsb/Radioberry-2.x/tree/master/hardware/hat
- hat announcement thread: https://groups.google.com/g/radioberry/c/Z2Vqnl1Da2A
- hish8612 project page: https://hish8612.github.io/Radioberry_hat_pa30/
- hish8612 repository: https://github.com/hish8612/Radioberry_hat_pa30
- CW sidetone gateware variant: https://github.com/hish8612/Hermes-Lite2/tree/master/gateware/variants/radioberry_cl025_cwsidetone
- JI1UDD HL2-PA30: https://github.com/ji1udd/HL2-PA30
- Yado-san on the PA30 and MCP23008 switching: https://groups.google.com/g/radioberry/c/cOJO7nmkuWg and https://groups.google.com/g/radioberry/c/mBQhL4z155A
- AURSINC listing: https://www.aliexpress.com/item/1005008233825206.html
- BG7IYN test notes: https://jumbo5566.github.io/pro/rb/2024/04/17/rb016_025.html
- Hermes-Lite 2 protocol (PA enable, bias, ADC fields): https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol
- Daemon code: https://github.com/pa3gsb/Radioberry-2.x/tree/master/SBC/rpi-5/device_driver/firmware (`bias.c`, `measure.c`, `filters.h`, `radioberry.c`)
