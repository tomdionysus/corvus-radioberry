# soapysdr-module-radioberry and SoapyRadioberry: interface comparison

SoapyRadioberry (github.com/paulh002/SoapyRadioberry) extends the original
RadioBerry project's SoapySDR module, and it is what a SoapySDR program
written for the RadioBerry so far will have been developed against. This
note compares the SoapySDR interface the two modules present, so that
such a program runs on soapysdr-module-radioberry unchanged where that is
possible, and says where it cannot be. It is drawn from SoapyRadioberry's
master branch as read on 2026-11-16 (SoapyRadioberry.hpp/.cpp,
SoapyRadioberrySettings.cpp, SoapyRadioberryStreaming.cpp,
radioberry_ioctl.h).

## Different drivers

SoapyRadioberry talks to the original RadioBerry driver: one ioctl,
`_IOW('x', 1, __u8)`, carrying a nine-int command structure.
soapysdr-module-radioberry talks to this project's driver, whose interface
is different (`radioberry_ioctl.h` here, magic 0xb5). Neither module can
drive the other's driver, so the question is only whether a program sees
the same SoapySDR interface. Both register the driver key `radioberry`.

## Where they agree

| | SoapyRadioberry | soapysdr-module-radioberry |
|---|---|---|
| Driver key | `radioberry` | `radioberry` |
| RX rates | 48, 96, 192, 384 ksps | the same |
| TX rate | 48 ksps | the same |
| RX gain range | -12 to 48 dB, `0x40 | (dB + 12)` to the LNA | the same range and encoding |
| Formats | CF32 (native), CS16 for TX | CF32, CS16, CS32 (native) both ways for RX; CF32, CS16 for TX |
| Channel 0 RX and TX, full duplex | yes | yes |
| Frequency | 10 kHz to 30 MHz | 0 to 38.4 MHz |

## Where a program could see a difference

Each with what soapysdr-module-radioberry could do; nothing is changed
until the board owner decides.

1. **Activating streams.** SoapyRadioberry does not implement
   `activateStream`/`deactivateStream`: its streams run from
   `setupStream`. A program that never calls `activateStream` reads and
   writes with SoapyRadioberry, but gets timeouts (RX) and
   `SOAPY_SDR_STREAM_ERROR` (TX) from soapysdr-module-radioberry. SoapySDR
   requires the call, and GNU Radio, CubicSDR and Quisk make it. Option:
   none, or activate a stream implicitly on its first read or write.
2. **Transmit keying.** SoapyRadioberry sets its MOX flag in `setupStream`
   (for any stream) and clears it in `closeStream` of the TX stream.
   soapysdr-module-radioberry keys on the first write after activation
   and unkeys on deactivation, close or `SOAPY_SDR_END_BURST`, after the
   last sample has gone out. A program that sets up a TX stream, writes,
   and closes it works with both.
3. **Transmit gain.** SoapyRadioberry: `setGain(TX, 0, 0..15)`, the four
   drive bits of the HL2 command, no gain names. soapysdr-module-radioberry:
   element `DRIVE` in dB, 0.5 dB steps up to the configured ceiling. The
   same number means a different drive. Option: none (documented), or a
   second TX gain element with SoapyRadioberry's 0 to 15 scale.
4. **Power amplifier.** SoapyRadioberry: setting `PowerAmp`, `Operate` or
   `Standby`. soapysdr-module-radioberry: the PA is enabled while keyed
   when `allow_pa` is true in `/etc/radioberry/config.yaml`; there is no
   setting. Option: a `PowerAmp` setting with the same values, `Operate`
   taking effect only where `allow_pa` permits.
5. **Antenna names.** SoapyRadioberry lists `ANTENNA RX` and `ANTENNA TX`;
   soapysdr-module-radioberry lists `RX` and `TX` and refuses other names
   in `setAntenna`. A program that sets SoapyRadioberry's name gets an
   exception. Option: accept both names.
6. **I2C.** SoapyRadioberry implements `writeI2C`/`readI2C` on
   `/dev/i2c-1` at an address the program gives. soapysdr-module-radioberry
   does not. Option: none; or pass through, if a program needs it (the PA
   board's devices sit on that bus, so the board owner decides).
7. **Bandwidth.** SoapyRadioberry reports 48 kHz; soapysdr-module-radioberry
   reports none. Programs treat both as "not settable".
8. **Hardware information.** Different keys (`firmwareVersion`,
   `gatewareVersion`, `hardwareVersion`, `protocolVersion` against
   `driver_version`, `gateware_version`, `fpga`, `receivers` and others);
   soapysdr-module-radioberry adds sensors and more receive channels.
   Programs that only display these are unaffected.

## Suggested next step

Ask SoapyRadioberry's author which SoapySDR calls his software makes, then
match items 1, 3, 4 and 5 as far as it needs, keeping this project's transmit rules
(`allow_tx`, `allow_pa`, `drive_ceiling`, the watchdog) in force.
