# Example clients

Built with `-DRADIOBERRY_BUILD_EXAMPLES=ON` (the `default` and `asan`
presets set it, so `tools/ci-pi.sh` builds and tests them). Not installed.

## ft8rx: FT8 over Protocol 1 or 2 (`examples/ft8`)

A receive-only openHPSDR client: it asks for one receiver at 48 ksps tuned
1500 Hz above the FT8 dial frequency, decimates to 3200 sps with
liquid-dsp, decodes each UTC 15 s slot and prints one line per message
(UTC, SNR in 2500 Hz, DT, audio frequency, text), as WSJT-X does. It never
sets MOX. Over Protocol 1 (the default) it sends zero TX samples and
answers every packet with a host packet, which keeps the radio's session.
Over Protocol 2 (`--protocol 2`) it uses DDC0, sends High Priority every
100 ms to keep the session, and stops with run clear; Protocol 2 carries
no LNA gain for this radio, so `--gain` is refused and `radioberryd
--p2-rx-gain` applies.

```
ft8rx [--protocol 1|2] [--host 127.0.0.1] [--port 1024] [--dial 14074000]
      [--gain 20] [--slots N] [--iq-order qi|iq]
```

The clock must be on UTC to within a second. Run it on the Pi against
`radioberryd`, or from any machine with `--host`.

The FT8 decoder is Corvus HF's C++ port of kgoba/ft8_lib (`src/ft8/`,
GPL-2.0-or-later here); the message text code is ft8_lib's own C
(`src/ft8_lib/`, MIT, with its licence). `ft8_tests` checks the decimator's
passband and stopband and decodes messages synthesised at 48 ksps in noise
through the same chain.

### Sample order

The Hermes-Lite 2 radio core, which the Radioberry gateware is built
from, sends each receiver's I first and takes I first (`radio.v`), and
the driver and `radioberryd` pass the order through. HL2 software reads
it that way: piHPSDR, which has one code path for every HL2 and hands
the first value to WDSP as I with no swap (its Swap IQ setting applies
only to SoapySDR radios), decodes real FT8 through `radioberryd`. So
the stream has the orientation HL2 software expects, as an HL2's does.

To a client using plain complex baseband (I real, Q imaginary, positive
frequencies above the tuned frequency) that stream is the mirror image,
in both directions:

- Receive: `ft8rx` decodes FT8 only with each sample's second value taken
  as I: two slots read that way decoded 7 messages, the two slots after
  them read the other way decoded none (40 m, 2026-11-08). Read that way
  the frequencies are the true ones: OE8DDX, decoded at 7.076381 MHz,
  had 288 PSK Reporter spots in the same ten minutes, 285 within 300 Hz
  of that and none within 300 Hz of the mirrored 7.074619 MHz. Corvus HF
  found the same on a Radioberry.
- Transmit: a 1 kHz tone sent as I = cos, Q = sin went out 1 kHz below
  the TX frequency. A HackRF One near the Radioberry (14.117 MHz TX,
  drive 0, PA disabled, a small antenna) saw it at TX - 1010 Hz, 29 dB
  above the scan median and 21.6 dB above the same frequency before
  transmitting, and nothing above noise at TX + 1 kHz.

Where in WDSP's conventions the difference lies is not established; a
real HL2 was not available to compare.

`radioberryd --iq-convention standard` swaps I and Q in both directions,
so plain complex-baseband clients see the true spectrum without
handling it themselves; the default, `hl2`, passes samples through for
HL2 software. `ft8rx` defaults to `--iq-order qi` for the default
daemon; against `--iq-convention standard` it takes `--iq-order iq`.

### Frequency offset

Decoded frequencies against the median of PSK Reporter spots in the same
ten minutes (40 m, 2026-11-08 20:38 to 20:48 UTC, five stations with
spots, 161 to 654 spots each): every station 161 to 163 Hz low, median
-162 Hz, about -23 ppm at 7.075 MHz. A reference clock off its nominal
frequency would do this; not confirmed.

With `radioberryd --frequency-ppm 22.9`, which corrects every RX and TX
frequency command for that error, the same comparison gave 0 to +1 Hz on
40 m (three stations, 123 to 272 spots each, 21:35 UTC) and -2 to 0 Hz on
30 m (three stations, 86 to 137 spots each, 21:38 to 21:44 UTC), where
uncorrected 30 m would read about 232 Hz low. One ppm figure corrects
both bands. The figure belongs to the development board; another board
needs its own measurement.

### Measured

40 m, 7.074 MHz, on the development Pi against `radioberryd` over
loopback, 2026-11-08 20:42 UTC: 6 slots, 23 messages decoded (SNR -17 to
+2 dB, DT -0.2 to 0.5 s), 37 078 packets, none lost.
