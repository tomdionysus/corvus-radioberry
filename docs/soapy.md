# SoapySDR

The optional `soapysdr-module-radioberry` package makes the radio a
[SoapySDR](https://github.com/pothosware/SoapySDR) device (driver key
`radioberry`) for programs such as GNU Radio, CubicSDR and Quisk. It uses
the driver directly, not `radioberryd`.

## Install

With `radioberry` installed and working ([Quickstart](quickstart.md)):

```
sudo apt install soapysdr-module-radioberry
sudo usermod -aG radioberry $USER    # then log in again
```

The radio has one user at a time: stop `radioberryd` before a SoapySDR
program opens it, and start it again after.

```
sudo systemctl stop radioberry
SoapySDRUtil --probe="driver=radioberry"
sudo systemctl start radioberry
```

If only SoapySDR programs use the radio, stop `radioberryd` for good; it
then stays off across reboots and package upgrades:

```
sudo systemctl disable --now radioberry
```

`sudo systemctl enable --now radioberry` brings it back.

## Device arguments

| Argument | Default | Meaning |
|---|---|---|
| `device` | `/dev/radioberry` | driver device |
| `config` | `/etc/radioberry/config.yaml` | config file |

From the config file ([radioberryd](daemon.md#options)): `frequency_ppm`,
`p2_rx_gain` (the LNA gain at start), and the transmit keys `allow_tx`,
`allow_pa`, `drive_ceiling` and `tx_watchdog_s`. A program cannot raise
these limits.

## Receive

| | |
|---|---|
| Channels | the gateware's receivers; a stream takes channels 0 to n-1, each with its own frequency |
| Rates | 48, 96, 192, 384 ksps, one for all channels, as many channels as the board carries at the rate ([Pi 4](rpi4.md), [Pi 5](rpi5.md)) |
| Formats | `CF32`, `CS16`, `CS32` (native) |
| Frequency | 0 to 38.4 MHz; outside, clamped with a warning |
| Gain | `LNA`, -12 to 48 dB |

## Transmit

One channel at 48 ksps, `CF32` or `CS16`, 0 to 38.4 MHz. Needs
`allow_tx: true`.

Activating the TX stream is key down and deactivating it key up. The
radio keys at the first write and unkeys after a write with
`SOAPY_SDR_END_BURST`, or when the stream is deactivated or closed, once
the last sample has been sent. `tx_watchdog_s` limits a transmission.

Drive is the gain element `DRIVE`, in 0.5 dB steps of the AD9866's TX
gain, up to the step `drive_ceiling` allows. With `allow_pa: true` the PA
is enabled while keyed.

## Sensors

`transmitting`, `rx_overruns`, `rx_resyncs`, `tx_underruns`,
`rx_stalled`, `ptt_input`, `cw_key`.

## Building

`-DRADIOBERRY_BUILD_SOAPY=ON`, with `libsoapysdr-dev` installed
([Building from source](building.md)).
