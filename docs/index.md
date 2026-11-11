# corvus-radioberry

Software for the
[RadioBerry 2.x HF transceiver HAT](https://github.com/pa3gsb/Radioberry-2.x/wiki)
on a Raspberry Pi 4 or 5: a kernel driver, `radioberryd` serving openHPSDR
Protocol 1 and Protocol 2 to clients such as piHPSDR and WSJT-X,
diagnostic tools, and the FPGA gateware. Installed as Debian packages.

Source: [github.com/tomdionysus/corvus-radioberry](https://github.com/tomdionysus/corvus-radioberry).

## Getting started

- [Quickstart](quickstart.md): install, enable, configure, connect
- [Coming from the original software](original-software.md)
- [Troubleshooting](troubleshooting.md)
- [Reporting an issue](reporting-issues.md)

## Reference

- [Requirements and supported hardware](platform.md)
- [Raspberry Pi 4](rpi4.md) and [Raspberry Pi 5](rpi5.md): capacity and parameters
- [radioberryd](daemon.md): configuration, frequency calibration, protocols, clients
- [Tools](tools.md): `rbprobe`, `rbcat`, `rbctl`, `rbload`, `rbbus`, `p1probe`, `p2probe`
- [Kernel driver](driver.md): device interface, sysfs, parameters
- [Debian packages and apt repositories](packaging.md)
- [Building from source](building.md)
- [The PA5W V2 amplifier board](pa.md)
- [Example clients](examples.md): `ft8rx`, sample order
- [Status](status.md): what works

## Development

- [Design](design.md)
- [Test results](testing.md)
- [Driver hardware tests](driver-tests.md)
- [Driver under a debug kernel](kernel-debug.md)
- [The gateware's receive lock-up](fault-rx-lockup.md)
- [Gateware interface](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/README.md),
  [provenance](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/PROVENANCE.md),
  [toolchain](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/QUARTUS.md)
- [Contributing](https://github.com/tomdionysus/corvus-radioberry/blob/main/CONTRIBUTING.md)
- [Licensing](licensing.md)
