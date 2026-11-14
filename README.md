# corvus-radioberry

[![Release](https://img.shields.io/github/v/release/tomdionysus/corvus-radioberry?display_name=release&label=latest)](https://github.com/tomdionysus/corvus-radioberry/releases/latest)
[![Project Status: Active](https://www.repostatus.org/badges/latest/active.svg)](https://www.repostatus.org/#active)
[![CI](https://github.com/tomdionysus/corvus-radioberry/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/corvus-radioberry/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/corvus-radioberry/graph/badge.svg)](https://codecov.io/gh/tomdionysus/corvus-radioberry)
[![CodeQL](https://github.com/tomdionysus/corvus-radioberry/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/corvus-radioberry/actions/workflows/codeql.yml)

A self-contained software stack for the
[RadioBerry 2.x HF transceiver HAT](https://github.com/pa3gsb/Radioberry-2.x/wiki)
on a Raspberry Pi 4 or 5 with a 10CL025 FPGA:

| Component | What it is |
|---|---|
| [Driver](driver/README.md) | `radioberry.ko`, the Linux kernel driver: loads the gateware, streams samples over the Pi 5's PIO or the Pi 4's PCM block |
| [Daemon](daemon/README.md) | `radioberryd`, serving openHPSDR Protocol 1 and Protocol 2 to clients such as piHPSDR and WSJT-X, with control of an [AURSINC PA5W V2](https://www.aursinc.com/products/aursinc-radioberry-preamp-board-v2-output-1-5w-3w-for-hf-power-ampfier-10w-100w-pa-for-raspberry-pi-4-4b) amplifier board |
| [Gateware](gateware/README.md) | the FPGA images, one per Pi, built from GPL source |
| [Tools](docs/tools.md) | `rbprobe`, which checks every layer from the Pi to the FPGA and says what is wrong, and other diagnostics |

It installs as Debian packages against the current Raspberry Pi OS kernel,
with no kernel patches; the driver is rebuilt on every kernel update.

The RadioBerry and its original software come from the
[RadioBerry project](https://github.com/pa3gsb/Radioberry-2.x). A
[ready-to-run Raspberry Pi 5 image](https://pa3gsb.gumroad.com/l/radioberry)
of the original software is sold by the RadioBerry's author; buying it
supports their work. Please consider buying it even if you don't use it:
the RadioBerry hardware is a work of art.

## Install

On Raspberry Pi OS (trixie, 64-bit), add the package repository and
install:

```
sudo curl -fsSLo /etc/apt/sources.list.d/tomcully.sources https://tomcully.com/apt/tomcully.sources
sudo apt update && sudo apt install radioberry
```

Then enable the driver, configure and start the service: the
[Quickstart](docs/quickstart.md) has the steps. Updates arrive with
`sudo apt upgrade`. To build from source instead, see
[Building from source](docs/building.md). Supported and tested kernels:
[docs/kernels.md](docs/kernels.md).

## Getting started

- [Quickstart](docs/quickstart.md): install, enable, configure, connect
- [Coming from the original software](docs/original-software.md)
- [Troubleshooting](docs/troubleshooting.md) and
  [reporting an issue](docs/reporting-issues.md)

All documentation: [docs/index.md](docs/index.md). What works and what
doesn't yet: [docs/status.md](docs/status.md). Changes per version:
[CHANGELOG.md](CHANGELOG.md).

## Roadmap

- I'd like to make a variant of the gateware for CL016 based RadioBerry
  boards, if someone with one will test it for me.
- An external review of the gateware lists what would make it more
  reliable, observable and numerically careful; the
  [gateware review](gateware/docs/review.md) has each finding and how far
  along it is.
- The AURSINC RadioBerry Preamp Board I'm using is an inexpensive copy with
  manual bias set on a pot. The
  [original design](https://groups.google.com/g/radioberry/c/WQc_afuvViI)
  has a digital potentiometer for the bias and an ADC for current and
  temperature, both on I2C. If that board is available, it would be good
  to get them integrated too.

## AI

This project was developed with the use of agentic engineering,
specifically Claude. If that's a problem for you, there are
[guides for compiling the original driver](https://github.com/pa3gsb/Radioberry-2.x#building).

## Contributing

PRs and comments welcome. Please see [CONTRIBUTING.md](CONTRIBUTING.md)
and the [Code Of Conduct](CODE_OF_CONDUCT.md) for community guidelines.

## Licensing

Licensed under the GNU General Public License, version 2 or (at your
option) any later version; see [LICENSE](LICENSE). Third-party works and
their licences are listed in [docs/licensing.md](docs/licensing.md).
