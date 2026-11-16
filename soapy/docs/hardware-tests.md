# SoapySDR hardware tests

`rbsoapytest` ([tests/hw/](../tests/hw/)) tests the module through SoapySDR
on a Raspberry Pi 4 or 5 with the RadioBerry attached. It is a SoapySDR
client: it uses the module SoapySDR finds, the installed package's or a
build tree's through `SOAPY_SDR_PLUGIN_PATH`. The unit tests
([tests/](../tests/)) cover the module's logic against a fake device.

## Running it

Built with the unit tests when `RADIOBERRY_BUILD_SOAPY` is on; not part of
`ctest` and not installed. Stop anything that holds `/dev/radioberry`
first:

```
sudo systemctl stop radioberry
sudo build/soapy/rbsoapytest
sudo build/soapy/rbsoapytest "[loopback]"
sudo systemctl start radioberry
```

Catch2's options apply. `RADIOBERRY_SOAPY_ARGS` replaces the device
arguments (`driver=radioberry`); `RADIOBERRY_SOAPY_SECONDS` sets how long
each rate and the idle check run (5). Each test writes its own config
file; `/etc/radioberry/config.yaml` is not read. sysfs is read from
`/sys/bus/spi/devices/spi0.0/`.

## Tests that never key

Run by default, with `allow_tx: false`.

| Test | Checks |
|---|---|
| SoapySDR finds the radio and it identifies itself | one device for `driver=radioberry`; the hardware information and channel counts match sysfs |
| every rate holds at the most channels the link carries | at 48, 96, 192 and 384 ksps, on as many channels as the module activates: the rate within 1%, no overflow, timeout or error, no driver overrun or resync, samples on every channel; no stall |
| an open device with no stream costs nothing | the module's threads make no context switch and use no CPU, and the image sends nothing to the Pi |
| transmit is refused when the config does not allow it | `DRIVE` stays 0; TX activation gives `SOAPY_SDR_NOT_SUPPORTED`; every write fails; the image's keying register stays 0 and it takes no TX sample |

## Loopback tests

Hidden; run with `"[loopback]"`, as root. They key the radio in the
AD9866's digital loopback (sysfs `tx_loopback`), in which the image holds
the PA lines low, with `allow_pa: false` and `DRIVE` 0, and they do not
key unless the image's control register shows loopback on and the PA not
allowed. `tx_loopback` is 0 again after each. They are transmit tests
under the safety rules in [CONTRIBUTING.md](../../CONTRIBUTING.md).

| Test | Checks |
|---|---|
| a burst in loopback comes back as its tone and unkeys | two seconds of a tone 1 kHz above 14.120 MHz: keyed while written; received above -30 dBFS with the image 90 dB below it; the image took every sample, with no sequence gap, bad frame, overflow or underrun; no driver underrun while the burst is written; unkeyed within 0.5 s of the last write |
| a program killed while keyed leaves the radio unkeyed | a child process keys and writes; killed with `SIGKILL`, the keying register is 0 within a second |
