# Status

| Component | Directory | Working | Notes |
|---|---|---|---|
| Kernel driver `radioberry.ko`, overlay, ABI header | `driver/` | ✓ | Loads the gateware at boot; receive and transmit streams |
| Raspberry Pi 4 transport (CPU-clocked) | `driver/` | ✓ | Two receivers at 48 ksps or one at 96 |
| Raspberry Pi 5 transport (RP1 PIO) | `driver/` | ✓ | Six receivers at 192 ksps or three at 384 |
| `radioberryd`, Protocol 1 | `daemon/` | ✓ | Hermes-Lite 2 dialect, receive and transmit, UDP and TCP; FT8 contacts with piHPSDR and WSJT-X |
| `radioberryd`, Protocol 2 | `daemon/` | ✓ | Receive and transmit; FT8 contacts with piHPSDR and WSJT-X; no wideband, diversity, PureSignal or radio-side CW keyer |
| `radioberry.service` | `daemon/` | ✓ | Unprivileged, options in `/etc/radioberry/config.yaml`, logs to the journal |
| Gateware, CL025 (HL2 RadioBerry PIO variant 75.3) | `gateware/` | ✓ | Built from source; its receive lock-up is worked around by the driver |
| Gateware, CL016 | `gateware/` | ✗ | Built, not tested on a CL016 board; misses AD9866 TX output timing |
| Platform probe `rbprobe` | `tools/rbprobe/` | ✓ | Host, boot, driver, gateware, FPGA, PA board and daemon state; text, JSON or YAML |
| Stream tool `rbcat` | `tools/rbcat/` | ✓ | Receive streams and measurements without the daemon |
| Command tool `rbctl` | `tools/rbctl/` | ✓ | Single gateware commands; anything that can transmit needs `--allow-tx` |
| Gateware loader `rbload` | `tools/rbload/` | ✓ | Loads the FPGA without the driver; checks the image family and CONF_DONE |
| Pi 4 bus measurement `rbbus` | `tools/rbbus/` | ✓ | Measures the Pi 4's CPU-clocked buses |
| Protocol test clients `p1probe`, `p2probe` | `tools/protoprobe/` | ✓ | Discovery, receive and zero-drive transmit checks over Protocol 1 and 2 |
| Example FT8 client `ft8rx` | `examples/ft8/` | ✓ | Receive only, Protocol 1 or 2; optional build |

Open work, in order:
[TODO/ACTIVE.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/TODO/ACTIVE.md).
