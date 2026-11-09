# radioberryd

`radioberryd` (`daemon/`) serves the radio to SDR clients over openHPSDR
Protocol 1 (Hermes-Lite 2 dialect, UDP and TCP) and Protocol 2 (UDP).
Clients that support the Hermes-Lite 2, such as piHPSDR, find it by
discovery. `radioberryd --help` lists every option.

## Service and configuration

`radioberry.service` runs `radioberryd --config /etc/radioberry/config.yaml`
as the `radioberry` system user with CAP_SYS_NICE and CAP_IPC_LOCK only.
It logs to the journal (`journalctl -u radioberry`), stops when the
driver goes, and restarts after a failure. A udev rule gives
`/dev/radioberry` to the `radioberry` group.

The config file's keys are the long options with `_` for `-`
(`realtime: false` stands for `--no-realtime`), each with a description.
The installed file
([daemon/systemd/config.yaml](../daemon/systemd/config.yaml)) lists
`radioberryd`'s defaults, except `drive_ceiling`, which it sets to 150.
An option given on the command line overrides the file. The daemon reads it at start:
`sudo systemctl restart radioberry` after a change.

Stop the service before running anything else that opens
`/dev/radioberry` (`rbcat`, `rbctl`, the driver tests). Run by hand
without those capabilities, `radioberryd` logs a warning and runs without
real-time scheduling or locked memory.

## Options

| Option | Default | Meaning |
|---|---|---|
| `--config PATH` | | read options from this YAML file first |
| `--device PATH` | `/dev/radioberry` | driver device |
| `--address IPV4` | all | address to listen on |
| `--port N` | 1024 | Protocol 1 port and Protocol 2 base port |
| `--mac XX:XX:XX:XX:XX:XX` | the interface's | MAC reported by discovery |
| `--client-timeout-ms N` | 3000 | silence that ends a client's stream |
| `--frequency-ppm P` | 0 | the radio's frequency error, positive if it tunes high; every RX and TX frequency is corrected |
| `--iq-convention C` | `hl2` | `hl2`: I/Q as the Hermes-Lite 2 sends it; `standard`: I and Q swapped both ways, for plain complex-baseband clients |
| `--p2-rx-gain DB` | 14 | Protocol 2 LNA gain, -12 to 48 dB, less the client's step attenuation |
| `--no-realtime` | | no SCHED_FIFO threads or locked memory |
| `--allow-tx` | off | let the owning client transmit |
| `--allow-pa` | off | pass on the client's PA enable while transmitting |
| `--drive-ceiling N` | 0 | highest drive level sent, 0 to 255 |
| `--tx-watchdog-s N` | 120 | longest continuous MOX, seconds |
| `--log-level LEVEL` | `info` | trace, debug, info, warn or error |

`--frequency-ppm` is measured per board against known signals
([docs/examples.md](examples.md)).

## Sessions and transmit

- The first client to start a stream owns the radio; others are ignored
  until it stops or stays silent for `--client-timeout-ms`.
- Without `--allow-tx` the daemon receives only and refuses MOX.
- With it, the owning client transmits while it holds MOX. Drive is
  capped at `--drive-ceiling`, PA enable is passed on only with
  `--allow-pa`, and `--tx-watchdog-s` ends any transmission held longer.
- These faults drop MOX, which stays dropped until the client releases
  it: an invalid TX block, a full RX queue, a device error, a full TX
  queue, a gap in the host sequence (Protocol 1 host packets, Protocol 2
  TX I/Q), and the TX watchdog. TX underruns, RX resyncs, overruns and
  stalls are logged and do not drop MOX.
- No PA interlock is claimed.

## Protocol 2

- Ports: discovery and General on the base port (1024), the other host
  packets on 1025 to 1029, DDC I/Q from 1035 up, one port per receiver.
- Discovery reports board 6 with the gateware's version, which piHPSDR
  takes as a Hermes-Lite 2, and the gateware's receiver count as DDCs.
- The gateware runs every receiver at one rate, the highest any enabled
  DDC asks for (48 to 384 ksps, within the receive capacity in
  [docs/driver.md](driver.md)); a DDC asking for less is decimated in the daemon.
- TX I/Q arrives at 192 ksps and is decimated to the gateware's 48 ksps.
- The silent Mic/Line packets that clock the client's transmitter and the
  High Priority status are paced by the radio's sample clock.
- The client's step attenuation is taken off `--p2-rx-gain`.
- Sessions, transmit policy and fail-safes are as for Protocol 1; a gap in
  the TX I/Q sequence is a stream fault.

## Real-time

The two device threads run at SCHED_FIFO 20 and the network threads at
SCHED_FIFO 10, below the driver's kernel thread (SCHED_FIFO 50), and
memory is locked. UDP sockets carry DSCP EF and SO_PRIORITY 6.
Measurements are in [docs/platform.md](platform.md).

## Clients

- piHPSDR lists the radio once per protocol: choose one entry, or turn
  the other protocol off in its discovery settings.
- piHPSDR's HL2 drive model assumes the Hermes-Lite 2 gateware, which uses
  the drive byte differently: lower its per-band PA calibration until the
  output is right (about 45 to 50 on the development board).
- WSJT-X through piHPSDR: Kenwood TS-2000 rig type on piHPSDR's CAT port
  (TCP 19090).
- `ft8rx` is a receive-only example client
  ([docs/examples.md](examples.md)).
