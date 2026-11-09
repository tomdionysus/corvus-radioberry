# Example clients

Built with `-DRADIOBERRY_BUILD_EXAMPLES=ON` (the `default` and `asan`
presets set it). Not installed.

## ft8rx: FT8 over Protocol 1 or 2 (`examples/ft8`)

A receive-only openHPSDR client: it asks for one receiver at 48 ksps tuned
1500 Hz above the FT8 dial frequency, decimates to 3200 sps with
liquid-dsp, decodes each 15 s UTC slot and prints one line per message
(UTC, SNR in 2500 Hz, DT, audio frequency, text), as WSJT-X does. It never
sets MOX.

- Protocol 1 (the default): it sends zero TX samples and answers every
  packet with a host packet, which keeps the session.
- Protocol 2 (`--protocol 2`): it uses DDC0 and sends High Priority every
  100 ms. Protocol 2 carries no LNA gain for this radio, so `--gain` is
  refused and `radioberryd --p2-rx-gain` applies.

```
ft8rx [--protocol 1|2] [--host 127.0.0.1] [--port 1024] [--dial 14074000]
      [--gain 20] [--slots N] [--iq-order qi|iq]
```

The clock must be on UTC to within a second. Run it on the Pi against
`radioberryd`, or from any machine with `--host`.

The decoder is a C++ port of ft8_lib
([examples/ft8/src/ft8/](../examples/ft8/src/ft8/), GPL-2.0-or-later);
the message text code is ft8_lib's own C
([examples/ft8/src/ft8_lib/](../examples/ft8/src/ft8_lib/), MIT).
`ft8_tests` checks the decimator's passband and
stopband and decodes messages synthesised at 48 ksps in noise through the
same chain.

## Sample order

The Hermes-Lite 2 radio core sends each receiver's I first and takes I
first (`radio.v`), and the driver and `radioberryd` pass the order
through. HL2 software reads it that way: piHPSDR hands the first value to
WDSP as I and decodes real FT8 through `radioberryd`.

To a client using plain complex baseband (I real, Q imaginary, positive
frequencies above the tuned frequency) that stream is the mirror image,
in both directions:

- Receive: `ft8rx` decodes FT8 only with each sample's second value taken
  as I (7 messages in two slots that way, none in two slots the other
  way), and the decoded frequencies then match PSK Reporter spots of the
  same stations.
- Transmit: a 1 kHz tone sent as I = cos, Q = sin goes out 1 kHz below the
  TX frequency.

`radioberryd --iq-convention standard` swaps I and Q in both directions
for plain complex-baseband clients; the default, `hl2`, passes samples
through for HL2 software. `ft8rx` defaults to `--iq-order qi` for the
default daemon; against `--iq-convention standard` use `--iq-order iq`.

## Frequency calibration

Compare decoded frequencies with the median of PSK Reporter spots for the
same stations over the same ten minutes. On the development board every
station read 161 to 163 Hz low on 40 m, about -23 ppm. With
`radioberryd --frequency-ppm 22.9` the same comparison gave 0 to +1 Hz on
40 m and -2 to 0 Hz on 30 m: one ppm figure corrects every band. Each
board needs its own measurement.

## Measured

40 m on the development Pi against `radioberryd` over loopback: 6 slots,
23 messages decoded (SNR -17 to +2 dB, DT -0.2 to 0.5 s), 37 078 packets,
none lost.
