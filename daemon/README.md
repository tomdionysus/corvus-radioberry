# Daemon

`radioberryd` serves the RadioBerry to SDR clients such as piHPSDR and
WSJT-X over openHPSDR Protocol 1 (Hermes-Lite 2 dialect, UDP and TCP) and
Protocol 2 (UDP). It talks to the radio only through the driver's
`/dev/radioberry`, and runs as `radioberry.service`. Configuring and using
it: [radioberryd](../docs/daemon.md).

## Layout

| Path | Contents |
|---|---|
| `src/device/` | the device layer: command registers, RX and TX pumps, statistics and faults |
| `src/p1/`, `src/p2/` | Protocol 1 and Protocol 2 servers |
| `src/dsp/` | the Protocol 2 transmit decimator (liquid-dsp) |
| `src/session.*`, `src/tx_policy.*` | who owns the radio, and when it may transmit |
| `src/daemon.*`, `src/options.*`, `src/main.cpp` | the daemon, its options and config file |
| `systemd/` | the unit, udev rule, sysusers entry, modules-load entry and the installed `config.yaml` |
| `tests/` | unit tests (Catch2) against a fake device |
| `docs/` | design |

## Building

From the repository root, on a Raspberry Pi with the build dependencies
installed ([Building from source](../docs/building.md)):

```
cmake -S . -B build
cmake --build build --target radioberryd daemon_tests
build/daemon/daemon_tests
```

`build/daemon/radioberryd --help` lists every option. To run a build by
hand, stop the service first, since only one program can hold
`/dev/radioberry`:

```
sudo systemctl stop radioberry
sudo build/daemon/radioberryd --config /etc/radioberry/config.yaml
```

`sudo cmake --install build` installs it with the unit and its files; the
packages put the same files under `/usr`.

## Documents

| Document | Contents |
|---|---|
| [docs/design.md](docs/design.md) | threads, device layer, protocols, session and transmit policy |
| [radioberryd](../docs/daemon.md) | configuration, sessions and transmit, Protocol 2, clients |
| [Frequency calibration](../docs/calibration.md) | measuring `frequency_ppm` |
