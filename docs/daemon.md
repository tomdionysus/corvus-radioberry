# radioberryd

`radioberryd` serves the radio to SDR clients over openHPSDR Protocol 1
(Hermes-Lite 2 dialect, UDP and TCP) and Protocol 2 (UDP). Clients that
support the Hermes-Lite 2, such as piHPSDR, find it by discovery.

## Service and configuration

`radioberry.service` runs `radioberryd --config /etc/radioberry/config.yaml`
as the `radioberry` system user with CAP_SYS_NICE and CAP_IPC_LOCK only.
It logs to the journal (`journalctl -u radioberry`), stops when the
driver goes, and restarts after a failure. A udev rule gives
`/dev/radioberry` to the `radioberry` group.

`/etc/radioberry/config.yaml` lists every key below with its default and a
description. The daemon reads it at start:
`sudo systemctl restart radioberry` after a change. Each key is also a
command-line option, which overrides the file.

Stop the service before running anything else that opens
`/dev/radioberry` (`rbcat`, `rbctl`, the driver tests).

## Options

| `config.yaml` | Option | Default | Meaning |
|---|---|---|---|
| | `--config PATH` | | read options from this YAML file first |
| `device` | `--device PATH` | `/dev/radioberry` | driver device |
| `address` | `--address IPV4` | all | address to listen on |
| `port` | `--port N` | `1024` | Protocol 1 port and Protocol 2 base port |
| `mac` | `--mac XX:XX:XX:XX:XX:XX` | the interface's | MAC reported by discovery |
| `client_timeout_ms` | `--client-timeout-ms N` | `3000` | silence that ends a client's stream |
| `frequency_ppm` | `--frequency-ppm P` | `0` | the radio's frequency error, positive if it tunes high ([Frequency calibration](calibration.md)) |
| `iq_convention` | `--iq-convention C` | `hl2` | `hl2`: I/Q as the Hermes-Lite 2 sends it; `standard`: I and Q swapped both ways, for plain complex-baseband clients ([sample order](examples.md#sample-order)) |
| `p2_rx_gain` | `--p2-rx-gain DB` | `14` | Protocol 2 LNA gain, -12 to 48 dB, less the client's step attenuation |
| `protocols` | `--protocols LIST` | `1,2` | protocols to serve: `1`, `2` or `1,2` |
| `realtime` | `--realtime BOOL` | `true` | SCHED_FIFO threads and locked memory |
| `allow_tx` | `--allow-tx BOOL` | `false` | let the owning client transmit |
| `allow_pa` | `--allow-pa BOOL` | `false` | pass on the client's PA enable while transmitting |
| `drive_ceiling` | `--drive-ceiling N` | `150` as installed (`0` without the file) | highest drive level sent, 0 to 255 |
| `tx_watchdog_s` | `--tx-watchdog-s N` | `120` | longest continuous MOX, seconds |
| `log_level` | `--log-level LEVEL` | `info` | trace, debug, info, warn or error |

## Sessions and transmit

- The first client to start a stream owns the radio; others are ignored
  until it stops or stays silent for `client_timeout_ms`.
- Without `allow_tx: true` the daemon receives only and refuses MOX.
- With it, the owning client transmits while it holds MOX. Drive is
  capped at `drive_ceiling`, PA enable is passed on only with
  `allow_pa: true`, and `tx_watchdog_s` ends any transmission held longer.
- These faults drop MOX, which stays dropped until the client releases
  it: an invalid TX block, a full RX queue, a device error, a full TX
  queue, a gap in the host sequence (Protocol 1 host packets, Protocol 2
  TX I/Q), and the TX watchdog. RX resyncs, overruns and stalls are
  logged and do not drop MOX. Driver TX underruns are logged at debug
  level only: the driver counts one each time its short ring empties
  between client bursts while the gateware's FIFO still holds samples.
- TX buffer: the daemon never sets the PTT hang time or TX buffer
  latency (address 0x17). A Protocol 1 client's value is passed on as
  sent; otherwise the gateware's defaults apply.
- TX samples go to the driver only while MOX is granted. Clients send
  them between overs too, as zeros; those are dropped in the daemon.
  Protocol 2 samples still pass through the decimator, so its filter holds
  the client's signal when transmission starts.
- PA allowed (`RADIOBERRY_CMD_PA_ALLOWED`) is set with MOX only with
  `allow_pa: true`; gateware-v2 images gate every PA line on it.
- No PA interlock is claimed.

## Idle

With no client the daemon makes no system calls: each thread blocks
until a packet arrives, a command is queued or it is stopped, and the
client timeout and TX watchdog run only while a client owns the radio.
Run is clear, so the driver streams nothing either.

## Protocol 2

- Ports: discovery and General on the base port (1024), the other host
  packets on 1025 to 1029, DDC I/Q from 1035 up, one port per receiver.
- Discovery reports board 6 with the gateware's version, which piHPSDR
  takes as a Hermes-Lite 2, and the gateware's receiver count as DDCs.
- The gateware runs every receiver at one rate, the highest any enabled
  DDC asks for (48 to 384 ksps, within the board's capacity:
  [Raspberry Pi 4](rpi4.md), [Raspberry Pi 5](rpi5.md)); a DDC asking for
  less is decimated in the daemon.
- TX I/Q arrives at 192 ksps and is decimated to the gateware's 48 ksps.
- The silent Mic/Line packets that clock the client's transmitter and the
  High Priority status are paced by the radio's sample clock.
- The client's step attenuation is taken off `p2_rx_gain`.
- Sessions, transmit policy and fail-safes are as for Protocol 1; a gap in
  the TX I/Q sequence is a stream fault.

## Real-time

The two device threads run at SCHED_FIFO 20 and the network threads at
SCHED_FIFO 10, below the driver's kernel thread (SCHED_FIFO 50), and
memory is locked. UDP sockets carry DSCP EF and SO_PRIORITY 6. Run by hand
without CAP_SYS_NICE and CAP_IPC_LOCK, `radioberryd` logs a warning and
runs without them.

## Clients

- piHPSDR lists the radio once per protocol, per network adapter and per
  address the daemon listens on: choose one entry, set `protocols` to the
  one you use, or set `address` to listen on one address only.
- piHPSDR: set its per-band PA calibration to about 45 to 50.
- WSJT-X through piHPSDR: Kenwood TS-2000 rig type on piHPSDR's CAT port
  (TCP 19090).
- `ft8rx` is a receive-only example client ([Example clients](examples.md)).
