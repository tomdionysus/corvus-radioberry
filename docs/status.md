# Status

| Component | Directory | Working | Notes |
|---|---|---|---|
| Kernel driver `radioberry.ko`, overlay, ABI header | `driver/` | yes | Loads the board's gateware at boot; receive and transmit streams |
| Raspberry Pi 4 (I2S) | `driver/`, `gateware/` | yes | Six receivers at 96 ksps, four at 192 or two at 384; FT8 transmitted and spotted |
| Raspberry Pi 5 (RP1 PIO) | `driver/`, `gateware/` | yes | Six receivers at 192 ksps or three at 384; FT8 contacts |
| `radioberryd`, Protocol 1 | `daemon/` | yes | Hermes-Lite 2 dialect, receive and transmit, UDP and TCP; FT8 contacts with piHPSDR and WSJT-X |
| `radioberryd`, Protocol 2 | `daemon/` | yes | Receive and transmit; FT8 contacts with piHPSDR and WSJT-X; no wideband, diversity, PureSignal or radio-side CW keyer |
| `radioberry.service` | `daemon/` | yes | Unprivileged, options in `/etc/radioberry/config.yaml`, logs to the journal |
| Gateware for the 10CL025 | `gateware/` | yes | One image per board, built from source |
| Gateware for the 10CL016 | `gateware/` | no | Not built; wanted, if someone with a CL016 board will test it |
| Platform probe `rbprobe` | `tools/rbprobe/` | yes | Host, boot, driver, gateware, FPGA, PA board and daemon state; text, JSON or YAML |
| Stream tool `rbcat` | `tools/rbcat/` | yes | Receive streams and measurements without the daemon |
| Command tool `rbctl` | `tools/rbctl/` | yes | Single gateware commands; anything that can transmit needs `--allow-tx` |
| Protocol test clients `p1probe`, `p2probe` | `tools/protoprobe/` | yes | Discovery, receive and zero-drive transmit checks over Protocol 1 and 2 |
| Example FT8 client `ft8rx` | `examples/ft8/` | yes | Receive only, Protocol 1 or 2; optional build |
| Example WSPR client `wsprrx` | `examples/wspr/` | yes | Receive only, Protocol 1 or 2, or `.c2` files; optional build |

Open work, in order:
[TODO/ACTIVE.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/TODO/ACTIVE.md).
