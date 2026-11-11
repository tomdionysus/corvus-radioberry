# Tools

`rbprobe`, `rbcat`, `rbctl`, `rbload`, `rbbus`, `p1probe` and `p2probe`
are installed in `/usr/bin` (`/usr/local/bin` when built from source). `rbcat` and `rbctl` open
`/dev/radioberry`, which is exclusive: stop `radioberry.service` first.
`rbload` needs the driver unloaded. `p1probe` and `p2probe` are network
clients of `radioberryd`.

## rbprobe

Reports the Pi, kernel, overlay, driver, installed gateware, FPGA type and
gateware identity, PA board (I2C scan) and daemon state. `--json` and
`--yaml` give the same content. Run it as root to see device holders and
the I2C bus.

Exit status: 0 the radio can run, 1 it cannot (each blocker is listed
under `findings`), 2 a usage error.

`rbprobe --identify` finds out which FPGA the board carries by loading
each of this project's gateware images in turn until one configures the
chip. It
reprograms the FPGA, so it refuses to run while the driver is loaded. It
looks for `.rbf` files in `/usr/local/share/radioberry/gateware`,
`/usr/share/radioberry/gateware` and `/lib/firmware`; `--gateware-dir`
replaces that list. Images are recognised by hash; the original GPIO-mode
images (73.3) are reported as unusable with this driver.

## rbcat

Receives from `/dev/radioberry` without the daemon.

| Option | Meaning |
|---|---|
| `--rate 48\|96\|192\|384` | sample rate, ksps (default 48) |
| `--receivers N` | receiver count, 1 to 12 (default 1) |
| `--freq HZ[,HZ...]` | receiver frequencies; one value sets all (default 14074000) |
| `--lna DB` | LNA gain, -12 to 48 dB (default 20) |
| `--seconds S` | stream for S seconds (default: until SIGINT or SIGTERM) |
| `--settle S` | settling time before measuring (default 0.5 s) |
| `--interval S` | progress line on stderr every S seconds |
| `--output none` | discard the frames (default) |
| `--output iq24` | frames to stdout as read (24-bit big-endian I and Q per receiver); the report goes to stderr |
| `--output iq16` | the top 16 bits of each value as little-endian int16 |
| `--tx-zeros` | also write zero TX samples, about 50 ms queued, and report TX word rate, underruns and the lowest queue level |
| `--device PATH` | device (default `/dev/radioberry`) |
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
`--device PATH` opens another device; `--json` and `--yaml` set the report
format.

A command that can key the transmitter or arm the PA (`--ptt`,
`--tx-permit`, `--cwx`, or address 0x09 with drive, VNA or PA enable) is
refused unless `--allow-tx` is given. A PTT hang time of 31 at address
0x17 is always refused. On exit the driver sends the safe command, so
run, PTT, the TX permission bit, PA enable and drive last only while
`rbctl` runs.

Exit status: 0 success, 1 a device error (including EBUSY), 2 a usage
error or refused command.

## rbload

`rbload [IMAGE]` loads a gateware image into the FPGA through its passive
serial configuration lines, without the driver, and checks nSTATUS and
CONF_DONE. IMAGE defaults to `/lib/firmware/radioberry.rbf`.

- Only images `rbprobe` knows as this project's gateware are loaded;
  `--force` loads an unknown image or another family's.
- `--fpga CL025|CL016` refuses an image for the other FPGA, even with
  `--force`.
- It refuses to run while `radioberry.ko` is loaded, since the driver owns
  the configuration lines: `sudo systemctl stop radioberry`,
  `sudo rmmod radioberry`, then `rbload`. To keep the loaded image when
  the driver comes back, load it with
  `sudo modprobe radioberry load_gateware=0`.
- `--json` and `--yaml` give the report as JSON or YAML.

Exit status: 0 the FPGA configured, 1 it did not (an image for the other
FPGA leaves CONF_DONE low) or the image cannot be read, 2 a usage error or
a refused image.

## rbbus

Measures the RX bus, and optionally the TX bus, clocked by the CPU on a
Raspberry Pi 4, with the bus code the driver uses there
(`driver/kernel/rb_bitbang.h`) running in userspace through
`/dev/gpiomem`. Gateware commands go over spidev. It runs only on a Pi 4
(BCM2711) and only while `/dev/radioberry` is absent: remove
`dtoverlay=radioberry`, set `dtparam=spi=on`, reboot, and load the
gateware with `rbload` before each run.

| Option | Meaning |
|---|---|
| `--seconds S` | stream time (default 10) |
| `--receivers N` | receivers, 1 to 6 (default 1) |
| `--rate 48\|96\|192\|384` | sample rate, ksps (default 48) |
| `--settle N` | extra pin reads after each clock pulse (default 0) |
| `--idle-sleep US` | sleep when the gateware has nothing to send (default 1000; 0 spins) |
| `--tx` | also clock TX words of zeros at 48 000 per second |
| `--spi DEVICE` | default `/dev/spidev0.0` |
| `--json`, `--yaml` | report format |

It reports values and clocks per second, frames against the number
expected, the parser's resyncs and dropped words, TX words per second and
ready waits, and the CPU time used. PTT, the TX permission bit and CWX are
never set; the run ends with the command that clears run, PTT, the TX
permission bit, PA enable and drive. Run it under `chrt -f 50` to see what
the driver's real-time thread would.

Exit status: 0 every frame arrived in sequence (at least 98% of those
expected) and, with `--tx`, the TX word rate kept up, 1 frames were lost
or the run could not start, 2 a usage error or the wrong board.

## p1probe and p2probe

Minimal openHPSDR clients for checking a radio or `radioberryd`:
`p1probe` speaks Protocol 1 (Hermes-Lite 2 dialect, UDP), `p2probe`
Protocol 2. Each discovers the radio, streams from it after a settling
time, and reports the radio's identity, the packet rate against the rate
the configuration implies, sequence breaks and lost packets, and RMS and
peak level per receiver.

| Option | Meaning |
|---|---|
| `HOST` | radio address (default 127.0.0.1) |
| `--port N` | radio port, Protocol 2's base port (default 1024) |
| `--discover-only` | discovery only, no stream |
| `--rate 48\|96\|192\|384` | sample rate, ksps (default 48) |
| `--receivers N` | receivers to stream (default 1; up to 7 for Protocol 1, 8 for Protocol 2) |
| `--freq HZ` | receive and transmit frequency (default 7074000) |
| `--lna DB` | `p1probe` only: LNA gain, -12 to 48 (default: not sent) |
| `--seconds S` | measurement time (default 2) |
| `--settle S` | time streamed before measuring (default 0.2) |
| `--tolerance PCT` | largest packet rate error that passes (default 2) |
| `--mox` | zero-drive transmit during the measurement (below) |
| `--json`, `--yaml` | report format |

Drive is always 0 and PA enable is never sent. Without `--mox` neither
probe sets MOX. `p1probe` sends zero TX samples in its host packets
throughout, paced by the radio's receive clock. `p2probe` sends High
Priority every 100 ms and no TX I/Q.

With `--mox` the probe holds MOX through the measurement:

- `p1probe` sets C0 bit 0 in its host packets.
- `p2probe` sets the High Priority MOX bit and sends zero TX I/Q at
  192 ksps, clocked by the radio's Mic/Line packets.

This keys the transmitter if the radio allows it (`radioberryd` with
`allow_tx`), at drive 0.

A radio that reports itself busy is asked again for about a second, in
case its last client has only just stopped.

Exit status: 0 every check passed (a discovery reply, the radio not busy
with another client, the packet rate within the tolerance, no sequence
breaks), 1 a check failed or the radio did not answer, 2 a usage error.
