# Quickstart

For a Raspberry Pi 4 or 5 running Raspberry Pi OS (Debian 13 trixie,
64-bit) with the RadioBerry fitted. If the original RadioBerry software
was installed, [remove it first](original-software.md).

## 1. Install

Add the package repository (one file, with its signing key inside) and
install:

```
sudo curl -fsSLo /etc/apt/sources.list.d/tomcully.sources https://tomcully.com/apt/tomcully.sources
sudo apt update && sudo apt install radioberry
```

The install asks which FPGA the RadioBerry has: read the chip's marking,
`10CL025...` or `10CL016...`.

## 2. Enable the driver

Add to `/boot/firmware/config.txt`:

```
dtparam=i2c_arm=on
dtoverlay=radioberry
[pi4]
dtoverlay=radioberry-smi
[all]
```

The `[pi4]` lines let a Raspberry Pi 4 receive with SMI and DMA instead of
a CPU core ([Raspberry Pi 4](rpi4.md)); a Pi 5 ignores them. Reboot, then
check:

```
rbprobe
```

It ends with "radio can run: yes", or lists what is wrong
([Troubleshooting](troubleshooting.md)).

## 3. Configure

Edit `/etc/radioberry/config.yaml`; every option is listed there with its
default. The ones most set:

| Key | Default | Set it to |
|---|---|---|
| `frequency_ppm` | `0` | your RadioBerry's frequency error ([calibration](calibration.md)) |
| `allow_tx` | `false` | `true` to transmit |
| `allow_pa` | `false` | `true` to use the PA; connect a rated antenna or dummy load first |
| `drive_ceiling` | `150` | the highest drive sent, 0 to 255 |
| `protocols` | `1,2` | `1` or `2` to serve one protocol only |

## 4. Start the service

The package enables `radioberry.service`; start it, or restart it after
any change to the config:

```
sudo systemctl restart radioberry
journalctl -u radioberry -f
```

## 5. Connect a client

Point piHPSDR or another Hermes-Lite 2 compatible client at the Pi; the
radio appears in discovery over Protocol 1 and Protocol 2. Client notes:
[radioberryd](daemon.md#clients).
