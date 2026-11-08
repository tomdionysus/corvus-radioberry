# Example clients

Built with `-DRADIOBERRY_BUILD_EXAMPLES=ON` (the `default` and `asan`
presets set it, so `tools/ci-pi.sh` builds and tests them). Not installed.

## ft8rx: FT8 over Protocol 1 (`examples/ft8`)

A receive-only openHPSDR Protocol 1 client: it asks for one receiver at
48 ksps tuned 1500 Hz above the FT8 dial frequency, decimates to 3200 sps
with liquid-dsp, decodes each UTC 15 s slot and prints one line per message
(UTC, SNR in 2500 Hz, DT, audio frequency, text), as WSJT-X does. It never
sets MOX and sends zero TX samples. It answers every packet with a host
packet, so it keeps the radio's session while it runs.

```
ft8rx [--host 127.0.0.1] [--port 1024] [--dial 14074000] [--gain 20]
      [--slots N] [--iq-order qi|iq]
```

The clock must be on UTC to within a second. Run it on the Pi against
`radioberryd`, or from any machine with `--host`.

The FT8 decoder is Corvus HF's C++ port of kgoba/ft8_lib (`src/ft8/`,
GPL-2.0-or-later here); the message text code is ft8_lib's own C
(`src/ft8_lib/`, MIT, with its licence). `ft8_tests` checks the decimator's
passband and stopband and decodes messages synthesised at 48 ksps in noise
through the same chain.

### Sample order

On the development Radioberry through `radioberryd`, FT8 decodes only with
each sample's second 24-bit value taken as I (`--iq-order qi`, the
default): two slots read that way decoded 7 messages, the two slots after
them read the other way decoded none (40 m, 2026-11-08). Corvus HF found
the same on a Radioberry. The decoder's own orientation is checked by the
synthesised tests. piHPSDR reads the first value as I and its audio
decoded in WSJT-X through the same daemon; why the two differ is not yet
known.

### Measured

40 m, 7.074 MHz, on the development Pi against `radioberryd` over
loopback, 2026-11-08 20:42 UTC: 6 slots, 23 messages decoded (SNR -17 to
+2 dB, DT -0.2 to 0.5 s), 37 078 packets, none lost.
