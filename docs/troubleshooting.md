# Troubleshooting

Start with `rbprobe`: it lists every blocker and exits 0 when the radio
can run. The daemon logs to `journalctl -u radioberry`.

## No `/dev/radioberry`

- `config.txt` needs `dtoverlay=radioberry` (and `dtparam=i2c_arm=on`);
  reboot after adding it.
- The original RadioBerry software is or was installed:
  [Coming from the original software](original-software.md).
- After a kernel update, DKMS needs that kernel's headers:
  `sudo apt install linux-headers-$(uname -r)`, then reboot.

## The gateware does not load

`rbprobe` reports CONF_DONE low: the installed image is for the other
FPGA. Read the chip's marking (`10CL025...` or `10CL016...`), run
`sudo dpkg-reconfigure radioberry` and reboot.

## Receive stops

`cat /sys/bus/spi/devices/spi0.0/rx_stalled` reads 1 and the log has "RX
stream stalled": stop the daemon, then
`echo 1 | sudo tee /sys/bus/spi/devices/spi0.0/reload` and start it
again.

## "device busy"

Only one program can hold the radio. Stop the daemon
(`sudo systemctl stop radioberry`) before `rbcat`, `rbctl` or the driver
tests.

## Fewer receivers than asked for

The log says "N receivers at R ksps asked for; running M": the board
carries no more ([Raspberry Pi 4](rpi4.md), [Raspberry Pi 5](rpi5.md)).
Choose fewer receivers or a lower rate in the client.

## No transmit

- "MOX refused": set `allow_tx: true` in `/etc/radioberry/config.yaml` and
  `sudo systemctl restart radioberry`. The PA also needs `allow_pa: true`.
- Little or no output: lower piHPSDR's per-band PA calibration (about 45
  to 50). The log's "PTT on: drive N" line shows the drive that reached
  the radio, capped at `drive_ceiling` (150 as installed).

## piHPSDR shows the radio more than once

It lists one entry per protocol, per network adapter and per address the
daemon listens on. Pick one, or in `/etc/radioberry/config.yaml` set
`protocols` to the one you use (`protocols: 1`) or `address` to listen on
one address only.

## Signals are off frequency

Set `frequency_ppm` for your board
([Frequency calibration](calibration.md)).

## Dropouts over Wi-Fi

Use Ethernet for clients, especially when transmitting over Protocol 2.

## Raspberry Pi 4 stutters or drops samples

`vcgencmd get_throttled` other than `0x0` means under-voltage or
throttling; use a 5 V 3 A supply.

## Still stuck

Open an issue: [Reporting an issue](reporting-issues.md).
