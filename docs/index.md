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
- [Raspberry Pi 4](rpi4.md) and [Raspberry Pi 5](rpi5.md): capacity, parameters, tested kernels
- [Kernels](kernels.md): 6.12 and 6.18 differences, tested kernels
- [radioberryd](daemon.md): configuration, protocols, clients
- [Frequency calibration](calibration.md): measuring `frequency_ppm`
- [Tools](tools.md): `rbprobe`, `rbcat`, `rbctl`, `p1probe`, `p2probe`
- [Kernel driver](driver.md): device interface, sysfs, parameters
- [Debian packages and apt repositories](packaging.md)
- [Building from source](building.md)
- [The PA5W V2 amplifier board](pa.md)
- [Example clients](examples.md): `ft8rx`, `wsprrx`, sample order
- [Status](status.md): what works

## Development

- [Design](design.md)
- [Test results](testing.md)
- [Driver](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/README.md):
  building it, [design](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/docs/design.md),
  [hardware tests](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/docs/driver-tests.md)
- [Daemon](https://github.com/tomdionysus/corvus-radioberry/blob/main/daemon/README.md):
  building it, [design](https://github.com/tomdionysus/corvus-radioberry/blob/main/daemon/docs/design.md)
- [Gateware](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/README.md):
  building it, [interfaces](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/docs/interfaces.md),
  [provenance](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/docs/provenance.md),
  [review](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/docs/review.md)
- [Contributing](https://github.com/tomdionysus/corvus-radioberry/blob/main/CONTRIBUTING.md)
- [Licensing](licensing.md)
