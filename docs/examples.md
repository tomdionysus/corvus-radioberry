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
  refused and the daemon's `p2_rx_gain` applies.

```
ft8rx [--protocol 1|2] [--host 127.0.0.1] [--port 1024] [--dial 14074000]
      [--gain 20] [--slots N] [--iq-order qi|iq]
```

The clock must be on UTC to within a second. Run it on the Pi against
`radioberryd`, or from any machine with `--host`.

The decoder is a C++ port of ft8_lib
([examples/ft8/src/ft8/](https://github.com/tomdionysus/corvus-radioberry/blob/main/examples/ft8/src/ft8/), GPL-2.0-or-later);
the message text code is ft8_lib's own C
([examples/ft8/src/ft8_lib/](https://github.com/tomdionysus/corvus-radioberry/blob/main/examples/ft8/src/ft8_lib/), MIT).
`ft8_tests` checks the decimator's passband and
stopband and decodes messages synthesised at 48 ksps in noise through the
same chain.

## wsprrx: WSPR over Protocol 1 or 2 (`examples/wspr`)

The sister of `ft8rx`, with the same options and receive path: one
receiver at 48 ksps tuned 1500 Hz above the WSPR dial frequency,
decimated to 375 sps with liquid-dsp, each even two-minute UTC slot
decoded, and one line per message (UTC, SNR in 2500 Hz, DT, frequency in
MHz, drift in Hz across the transmission, message) as `wsprd` prints
them. It never sets MOX.

```
wsprrx [--protocol 1|2] [--host 127.0.0.1] [--port 1024] [--dial 14095600]
       [--gain 20] [--slots N] [--iq-order qi|iq] [--save DIR] [--file F.c2 ...]
```

WSPR dial frequencies include 7038600 (40 m) and 14095600 (20 m, the
default). `--save` writes each slot received as a `.c2` file, as
`wsprsim` writes and `wsprd` reads; `--file` decodes such files instead
of a radio. Message types 1, 2 (compound callsigns) and 3
(hashed callsign, six-character locator) are decoded; a type 3 callsign
shows as `<...>` until the same run has heard it in a type 1 or 2
message.

The decoder is written here from the WSPR protocol description
([examples/wspr/src/wspr/](https://github.com/tomdionysus/corvus-radioberry/blob/main/examples/wspr/src/wspr/),
GPL-2.0-or-later): spectrum peaks as candidates, a sync search over
time, frequency and drift, the tones measured directly, and Fano
sequential decoding. It does not subtract decoded signals or retry with
other parameters as `wsprd` does. On the same `wsprsim` files (WSJT-X
2.7.0, one message per file, 3 per SNR) it decoded every file down to
-28 dB, 1 of 6 at -29 and -30 dB, where `wsprd` decoded all; its SNR
was within 1 dB of `wsprd`'s, DT within 0.1 s and frequency the same to
the hertz. A slot takes about 3 s to decode on a Pi 4.

`wspr_tests` checks the packing and channel symbols against `wsprsim`'s,
unpacks all three message types, decodes signals synthesised in noise at
375 sps and at 48 ksps through the decimator, and decodes nothing from
noise alone.

## Sample order

The Hermes-Lite 2 radio core sends each receiver's I first and takes I
first (`radio.v`), and the driver and `radioberryd` pass the order
through. HL2 software reads it that way: piHPSDR hands the first value to
WDSP as I and decodes real FT8 through `radioberryd`.

To a client using plain complex baseband (I real, Q imaginary, positive
frequencies above the tuned frequency) that stream is the mirror image,
in both directions:

- Receive: `ft8rx` decodes FT8 only with each sample's second value taken
  as I; `wsprrx` uses the same default.
- Transmit: a 1 kHz tone sent as I = cos, Q = sin goes out 1 kHz below the
  TX frequency.

`iq_convention: standard` in the daemon's config swaps I and Q in both
directions for plain complex-baseband clients; the default, `hl2`, passes
samples through for HL2 software. `ft8rx` defaults to `--iq-order qi` for
the default daemon; against `iq_convention: standard` use
`--iq-order iq`.
