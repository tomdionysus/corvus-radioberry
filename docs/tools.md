# Tools

`rbprobe`, `rbcat` and `rbctl` (`tools/`) are installed in
`/usr/local/bin`. `rbcat` and `rbctl` open `/dev/radioberry`, which is
exclusive: stop `radioberry.service` first.

## rbprobe

Reports the Pi, kernel, overlay, driver, installed gateware, FPGA type and
gateware identity, PA board (I2C scan) and daemon state. `--json` and
`--yaml` give the same content. Run it as root to see device holders and
the I2C bus.

Exit status: 0 the radio can run, 1 it cannot (each blocker is listed
under `findings`), 2 a usage error.

`rbprobe --identify` finds out which FPGA the board carries by loading
each known rpi-5 gateware image in turn until one configures the chip. It
reprograms the FPGA, so it refuses to run while the driver is loaded. It
looks for `.rbf` files in `/usr/local/share/radioberry/gateware`,
`/usr/share/radioberry/gateware` and `/lib/firmware`; `--gateware-dir`
replaces that list. Images are recognised by hash; the upstream GPIO-mode
images (73.3) are reported as unusable with this driver.

## rbcat

Receives from `/dev/radioberry` without the daemon.

| Option | Meaning |
|---|---|
| `--rate 48\|96\|192\|384` | sample rate, ksps |
| `--receivers N` | receiver count |
| `--freq HZ[,HZ...]` | receiver frequencies |
| `--lna DB` | LNA gain, -12 to 48 dB |
| `--seconds S` | stream for S seconds (default: until SIGINT or SIGTERM) |
| `--settle S` | settling time before measuring (default 0.5 s) |
| `--interval S` | progress line every S seconds |
| `--output iq24` | frames to stdout as read (24-bit big-endian I and Q per receiver); the report goes to stderr |
| `--output iq16` | the top 16 bits of each value as little-endian int16 |
| `--tx-zeros` | also write zero TX samples, about 50 ms queued, and report TX word rate, underruns and the lowest queue level |
| `--json`, `--yaml` | report format |

It reports frame rate, RMS and peak level per receiver, and the driver's
resync, dropped-word and overrun counts over the measurement and over the
settling time. It never sets PTT or the TX permission bit; with
`--tx-zeros` the gateware drains and discards the samples.

Exit status: 0 a clean stream (no faults after settling, frame rate
within 1%, and with `--tx-zeros` a TX word rate within 1% of 48000 per
second, no underruns and a queue that never ran empty), 1 stream faults
or a device error, 2 a usage error.

## rbctl

`rbctl ADDRESS DATA` sends one gateware command (Hermes-Lite 2 address and
32-bit C1..C4 data, decimal or 0x hex; run set unless `--no-run`) and
prints the command bytes, the gateware's reply, and the driver's info and
statistics. With no arguments it prints only the info and statistics.

A command that can key the transmitter or arm the PA (`--ptt`,
`--tx-permit`, `--cwx`, or address 0x09 with drive, VNA or PA enable) is
refused unless `--allow-tx` is given. A PTT hang time of 31 at address
0x17 is always refused. On exit the driver sends the safe command, so
run, PTT, the TX permission bit, PA enable and drive last only while
`rbctl` runs.

Exit status: 0 success, 1 a device error (including EBUSY), 2 a usage
error or refused command.
