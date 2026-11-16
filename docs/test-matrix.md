# OS and kernel test matrix

How to test the driver, daemon and clients on every Raspberry Pi OS
release and kernel a board can plausibly run, on one Pi, without
changing its normal system. The scripts are in
[tools/matrix/](../tools/matrix/). Everything here is receive only:
nothing sets PTT or the transmit permission, and `allow_tx` stays `false`.

## Which combinations

| Board | OS | Kernels in its archive | Notes |
|---|---|---|---|
| Pi 5 | trixie | 6.12.25 to 6.18.60 | the supported system |
| Pi 5 | bookworm | 6.1.47 to 6.1.73, 6.6.20 to 6.6.74, 6.12.19 to 6.12.115 | |
| Pi 4 | trixie | 6.12.25 to 6.18.60 | |
| Pi 4 | bookworm | as the Pi 5, `-rpi-v8` | |
| Pi 4 | bullseye | 6.1.21 (`-v8+`), the last bullseye kernel | the Pi 5 cannot run bullseye |

List a release's kernels for one board with

```
curl -fsSL http://archive.raspberrypi.com/debian/dists/RELEASE/main/binary-arm64/Packages.gz |
    gunzip | awk '/^Package: linux-image-[0-9].*rpi-2712$/ { print $2 }' | sort -uV
```

(`rpi-v8` for the Pi 4). A kernel the driver cannot be built for is
recorded as such, not booted.

## 1. A second OS on the same Pi

The Pi boots its normal system (trixie) as usual and the second OS only
through a one-time tryboot (`reboot "0 tryboot"`), so any failure returns
to the normal system at the next reset. Each tryboot reads
`/boot/firmware/tryboot.txt`, whose `os_prefix=` selects a directory of
boot files on the shared FAT partition.

1. Make room. The root file system cannot shrink while mounted, so
   [shrink-root.sh](../tools/matrix/shrink-root.sh) builds a private
   initramfs for the running kernel that does it before mounting, and arms
   one tryboot into it; `panic=10` makes any failure in the initramfs
   reboot into the normal system. Check first, then shrink:

   ```
   sh tools/matrix/shrink-root.sh dry /dev/nvme0n1p2
   reboot "0 tryboot"          # then: cat /run/rbshrink.log
   sh tools/matrix/shrink-root.sh real /dev/nvme0n1p2 830G
   reboot "0 tryboot"          # then: cat /run/rbshrink.log; df -h /
   ```

   `rbshrink.log` shows `e2fsck` and `resize2fs` exit codes (0 is clean)
   and the minimum size. On an SD card the device is `/dev/mmcblk0p2`.

2. Shrink partition 2 to just above the file system and add partition 3,
   with the system running (the file system already fits):

   ```
   sh tools/matrix/add-partition.sh /dev/nvme0n1 831 64
   ```

   Then reboot normally once to check the normal system still boots.

3. Download the release's image (Bookworm:
   `https://downloads.raspberrypi.com/raspios_oldstable_arm64/images/`),
   check its `.sha256`, and install its root on partition 3:

   ```
   sh tools/matrix/setup-second-os.sh 2026-10-06-raspios-bookworm-arm64.img.xz /dev/nvme0n1p3 bw
   reboot "0 tryboot"
   ```

   The script's header lists what it changes in the image and why. Two
   of them are needed for the image to come up headless at all: the image
   keeps Wi-Fi off (NetworkManager `WirelessEnabled=false` and an rfkill
   block) until its first-boot wizard sets a country, and its
   `raspi-firmware` hook refuses to install kernels unless `/boot/firmware`
   is a mount point.

4. While working on the second OS, touch `/run/rb-keep` over SSH at least
   every 30 minutes; otherwise its guard reboots it into the normal system.

A bullseye image (Pi 4 only) mounts its boot partition at `/boot`, names
its kernel `kernel8.img` and configures Wi-Fi with `wpa_supplicant`, not
NetworkManager: `setup-second-os.sh` covers Bookworm only.

## 2. The packages on the second OS

The trixie packages do not install on Bookworm (they need its newer C
and C++ libraries). Build them on the Bookworm system itself:
see [Results, Bookworm](#bookworm) for what that needs.

## 3. Kernel matrix

[kt.sh](../tools/matrix/kt.sh) stages one kernel (installs it with its
headers, builds the driver with DKMS, copies kernel, initramfs, DTBs and
overlays to `k-RELEASE/` and writes `tryboot.txt`) and, after booting it,
runs the checks:

- `rbprobe`: radio can run
- CPU per core and per thread at one receiver, 192 ksps, Protocol 1
- `ft8rx` on 20 m, four 15 s periods each over Protocol 1 and 2
- `rbcat --seconds 10 --tx-zeros` with the daemon stopped
- `rbdrivertest`, the driver harness

[kmatrix.sh](../tools/matrix/kmatrix.sh) runs it for a list of kernels
from a workstation, each kernel staged from the boot before and entered
by tryboot, and stops if a boot comes up as another system:

```
# the normal system (trixie)
TOOLS=/root/rbtools sh tools/matrix/kmatrix.sh root@PI /root/kt RELEASE...
# the second OS on partition 3
FW=/mnt/fw BASE=/mnt/fw/tryboot.txt.bw CMDLINE=/mnt/fw/k-bw/cmdline.txt \
    TOOLS=/root/rbtools HOSTNAME_EXPECT=HOST-bw \
    sh tools/matrix/kmatrix.sh root@PI /root/ktbw RELEASE...
```

`TOOLS` is a directory on the Pi with `ft8rx` and `rbdrivertest` built
for that OS. Each kernel's results are in `OUTDIR/RELEASE.txt` on the Pi.

## 4. Client CPU and decode times

One measurement: the client (Quisk, or piHPSDR over Protocol 1 or 2) at a
sample rate and WSJT-X in WSPR or FT8 mode in the desktop session, from a
period boundary, with `allow_tx: false`, Quisk's `tx_ip = "disable"` and
WSJT-X on PTT by VOX with `PctTx=0`. What is measured:

- CPU over one whole period from `/proc`: per process (children that
  exited count against their parent, so WSJT-X's `wsprd` is included), per
  core, and per thread of the driver and daemon; with it the CPU clock,
  the temperature and the supply's under-voltage alarm
  (`vcgencmd get_throttled`).
- The wall and CPU time of each `wsprd` run (WSPR) and each `jt9` burst
  (FT8), over N periods.
- WSJT-X's own `wsprd` command on a saved period, offline. The same
  period decoded with nothing else running and while a client streams
  separates the decoder from the radio.
- `ft8rx` and `wsprrx`, and `wsprrx --file` on the saved period.

Before a run on a Pi:

1. `apt install quisk wsjtx`, and on trixie also
   `python3-zombie-telnetlib`: trixie's Quisk 4.2.32 imports `telnetlib`,
   which Python 3.13 no longer has, and does not start without it. Build
   piHPSDR from source with
   `make GPIO=OFF MIDI=OFF AUDIO=PULSE` (the same commit on every OS
   compared).
2. PipeWire loopbacks in the first user's session, `rx_audio_sink` to
   `rx_audio_source` (client audio to WSJT-X), as
   `~/.config/pipewire/pipewire.conf.d/rx-loopback.conf`.
3. A WSJT-X.ini to start from, in `$INI` (the user's own; back it up, the
   runs overwrite `~/.config/WSJT-X.ini`), and a piHPSDR props file for
   the radio in `$PROPS`.
4. `allow_tx: false` in `/etc/radioberry/config.yaml`.

What starting the clients unattended needs:

- Quisk: a config-file radio (`ConfigFileRadio` in `quisk_settings.json`)
  takes its `Hardware` class from `~/.quisk_conf.py`, not from
  `hardware_file_name`; `rx_udp_ip` must be empty (it is a new address
  to give the radio, and its generic default is 192.168.2.160); and the
  Hermes sample rate comes from `vardecim_set` in `quisk_init.json`, not
  from `sample_rate`. Check the rate Quisk saved after the run.
- piHPSDR 3.x keeps its files in `~/.config/pihpsdr/` and names a radio's
  props by the MAC discovery reports. It autostarts only when discovery
  finds one radio: its Protocol 1 discovery also searches loopback, so the
  daemon's LAN address is hidden from it with an `nft` rule for the run;
  its Protocol 2 discovery skips loopback and finds the radio once.

## 5. Repeating it on a Pi 4

The same scripts, with these differences:

- Kernels are `-rpi-v8`; `kt.sh` stages `kernel8.img` and `initramfs8`.
- The root is usually on an SD card: `/dev/mmcblk0p2`, disk
  `/dev/mmcblk0`, often an MBR table (`add-partition.sh` handles both).
- The Pi 4 driver uses the PCM block and DMA, not the RP1 PIO, so the
  6.6 and 6.12.19 results above (both about the RP1 PIO) say nothing
  about it; run every kernel.
- Bullseye (6.1.21, the last bullseye kernel) runs only on the Pi 4. The
  driver does not build there (no `<linux/pio_rp1.h>`) without compiling
  out the Pi 5 transport; `setup-second-os.sh` covers Bookworm only.
- The Pi 4 has less CPU headroom: measure the clients at each rate up to
  the link's limit (four receivers at 192 ksps, [rpi4.md](rpi4.md)), and
  watch the supply's under-voltage alarm.

## Results

Raspberry Pi 5, 10CL025 RadioBerry, 2026-11-15 and 16. Trixie: the
supported system, all eleven kernels in its archive passed for 0.5.0
([kernels.md](kernels.md)).

### Bookworm

Raspberry Pi OS Bookworm image 2026-10-06 (Debian 12), on its own
partition of the same Pi.

The released packages (built on trixie) do not install: `radioberry`
needs libc6 2.38, libstdc++6 13.1, libfmt10, libgpiod3, libliquid1 1.7,
libspdlog1.15 and libyaml-cpp0.8, which Bookworm does not have.
`radioberry-dkms` (architecture `all`) installs.

Building 0.5.0 on Bookworm needs:

| What | Why |
|---|---|
| Catch2 3 and libgpiod 2.1 from `bookworm-backports` | Bookworm has Catch2 2.13 and libgpiod 1.6.3; CMake requires `libgpiodcxx >= 2.0` (only `rbprobe` uses libgpiod). The packages built this way depend on `libgpiod3` from backports at run time. |
| `yaml-cpp::yaml-cpp` aliased to `yaml-cpp` in CMake | yaml-cpp 0.7 exports its target without the namespace |
| `ARRAY_SIZE` from `<linux/kernel.h>` in `rb_logic.h` | 6.6 kernels have no `<linux/array_size.h>`; needed for 6.6 only |
| `-Wno-restrict` for the examples (`wsprrx`) | gcc 12 warns falsely inside libstdc++ (`-Werror`); the packages are not affected |

With these, the build's 330 tests pass and the packages install.

Packaging on Bookworm (debhelper 13.11, DKMS 3.0.10), all found on this
system:

- The package does not enable `radioberry.service`: the install script has
  no `deb-systemd-helper enable` step, as the unit is installed under
  `/usr/lib/systemd/system`. Enable it by hand
  (`systemctl enable --now radioberry`).
- DKMS builds the driver at install only for the running kernel and the
  newest kernel by name, so with `6.12.115+rpt-rpi-v8` and `-rpi-2712`
  installed it builds the Pi 4's and skips the Pi 5's. Booting that
  kernel then gives no driver (checked: no module, no `/dev/radioberry`,
  service inactive); nothing builds it at boot.
- A kernel the driver cannot be built for (6.1, 6.6.51, 6.6.62) fails its
  own installation while `radioberry-dkms` is installed, and apt stays
  broken until `radioberry-dkms` is removed.

Kernels, each booted with the Bookworm-built packages and checked as in
section 3 (FT8 on 40 m from 6.12.25 on, 20 m before):

| Kernel | Driver builds | Result |
|---|---|---|
| 6.1.47 to 6.1.73 | no: no `<linux/pio_rp1.h>` (checked on 6.1.73) | |
| 6.6.20 to 6.6.51 | no: no `<linux/pio_rp1.h>` (checked on 6.6.51; the header arrived in 6.6.60) | |
| 6.6.62 | no: its `pio_rp1.h` lacks `rp1_pio_sm_xfer_data`, `rp1_pio_sm_config_xfer`, `rp1_pio_open`, `rp1_pio_close` | |
| 6.6.74 | yes (with the `ARRAY_SIZE` change) | passed |
| 6.12.19 | yes | **failed**: probe passes, but no receive data (driver: "RX stream stalled: no words for 200 ms"); the same in two boots |
| 6.12.20 | yes | passed (20 m closed: 0 FT8 decodes, packets received, none lost) |
| 6.12.25, .34, .47, .62, .75, .87, .93, .96, .109, .115 | yes | passed |

"Passed": `rbprobe` radio can run; `ft8rx` decodes over Protocol 1 and 2
with no packets lost; `rbcat` 48 000 frames per second with no resyncs or
dropped words, TX taken at 48 000 words per second; `rbdrivertest` all
12 tests.

### Client CPU and decode times, Pi 5

One receiver, 40 m, receive only, measured as in section 4 over one
whole period (CPU as % of one core; the Pi has four). "Pi" is the whole
Pi's CPU use. WSJT-X decodes from the client's audio through a PipeWire
loopback; "decode" is each `wsprd` run's wall time.

Bookworm, kernel 6.12.115, Quisk 4.2.10, WSJT-X 2.6.1, Protocol 1:

| Rate | Pi | Quisk | WSJT-X | radioberryd | driver thread | WSPR decode |
|---|---|---|---|---|---|---|
| idle, no client | 0% | | | 0.0 | 0.0 | |
| 48 ksps | 7% | 20.0 | 1.6 | 2.3 | 0.5 | 0.2 s, 0.2 s |
| 96 ksps | 8% | 22.4 | 3.2 | 2.7 | 0.5 | 0.2 s, 1.8 s |
| 192 ksps | 11% | 25.7 | 4.9 | 3.2 | 0.5 | 1.8 s, 3.9 s |
| 384 ksps | 12% | 28.7 | 2.9 | 3.8 | 0.5 | 2.2 s, 1.4 s |
| FT8, 48 ksps | 9% | 16.9 | 15.4 | 1.9 | 0.4 | |
| FT8, 192 ksps | 14% | 24.4 | 18.0 | 3.1 | 0.5 | |

The supply dipped once, for 2 s, during the 384 ksps run (kernel:
"Undervoltage detected", then "Voltage normalised").

Trixie, kernel 6.12.75, Quisk 4.2.32, WSJT-X 2.7.0, Protocol 1:

| Rate | Pi | Quisk | WSJT-X | radioberryd | driver thread | WSPR decode |
|---|---|---|---|---|---|---|
| idle, no client | 0% | | | 0.0 | 0.0 | |
| 48 ksps | 9% | 27.9 | 1.7 | 2.6 | 0.4 | 0.8 s |
| 96 ksps | 10% | 31.6 | 1.7 | 2.9 | 0.4 | 0.8 s, 0.2 s |
| 192 ksps | 12% | 34.3 | 5.7 | 3.3 | 0.5 | 5.1 s |
| 384 ksps | 12% | 37.0 | 1.6 | 3.9 | 0.4 | 0.2 s, 0.2 s |
| FT8, 48 ksps | 14% | 27.2 | 17.0 | 2.3 | 0.4 | |
| FT8, 192 ksps | 12% | 28.7 | 8.0 | 2.9 | 0.4 | |

piHPSDR 3.1 (commit 82aa8c7, built on each system), WSJT-X decoding:

| System, protocol | Rate | Pi | piHPSDR | WSJT-X | radioberryd | driver thread | WSPR decode |
|---|---|---|---|---|---|---|---|
| Bookworm, P1 | FT8, 48 ksps | 11% | 14.8 | 19.5 | 2.4 | 0.5 | |
| Bookworm, P1 | FT8, 192 ksps | 11% | 21.9 | 20.7 | 3.3 | 0.5 | |
| Bookworm, P1 | FT8, 384 ksps | 12% | 28.7 | 18.3 | 4.3 | 0.5 | |
| Bookworm, P1 | WSPR, 192 ksps | 10% | 25.3 | 1.6 | 3.6 | 0.5 | 3.2 s, 0.2 s |
| Bookworm, P2 | FT8, 48 ksps | 11% | 14.3 | 22.1 | 3.0 | | |
| Bookworm, P2 | FT8, 192 ksps | 16% | 20.5 | 18.8 | 3.5 | 0.5 | |
| Bookworm, P2 | FT8, 384 ksps | 11% | 27.0 | 14.4 | 4.3 | 0.5 | |
| Bookworm, P2 | WSPR, 192 ksps | 9% | 24.7 | 3.6 | 3.9 | 0.5 | 2.4 s |
| Trixie, P1 | FT8, 48 ksps | 9% | 16.0 | 16.1 | 2.7 | 0.5 | |
| Trixie, P1 | FT8, 192 ksps | 12% | 23.1 | 16.7 | 3.3 | 0.5 | |
| Trixie, P1 | FT8, 384 ksps | 11% | 27.3 | 11.8 | 3.8 | 0.5 | |
| Trixie, P1 | WSPR, 192 ksps | 9% | 24.7 | 6.1 | 3.4 | 0.5 | 0.2 s, 5.5 s |
| Trixie, P2 | FT8, 192 ksps | 12% | 23.1 | 15.4 | 3.5 | 0.4 | |
| Trixie, P2 | FT8, 384 ksps | 15% | 26.5 | 16.4 | 4.1 | 0.4 | |
| Trixie, P2 | WSPR, 192 ksps | 10% | 24.7 | 3.8 | 3.8 | 0.5 | 2.4 s, 2.6 s |

piHPSDR over Protocol 2 at 48 ksps received nothing in three of four
starts (twice on trixie, once on Bookworm): radioberryd logged the
stream, piHPSDR showed -200 dBm and an empty panadapter, using about 4% of
a core. At 192 and 384 ksps it worked at every start, and `ft8rx` over
Protocol 2 at 48 ksps works. Cause not found.

This project's clients, one receiver at 48 ksps:

| System | Client | Pi | client | radioberryd | Result |
|---|---|---|---|---|---|
| Bookworm | `ft8rx` P1 | 1% | 1.6 | 2.0 | 4 decodes, 0 packets lost |
| Bookworm | `ft8rx` P2 | 1% | 1.6 | 1.7 | 7 decodes, 0 lost |
| Bookworm | `wsprrx` P1 | 1% | 1.1 | 2.1 | 0 decodes in 2 periods, 0 lost |
| Trixie | `ft8rx` P1 | 1% | 1.6 | 2.1 | 8 decodes, 0 lost |
| Trixie | `ft8rx` P2 | 1% | 1.7 | 1.8 | 12 decodes, 0 lost |
| Trixie | `wsprrx` P1 | 1% | 0.9 | 2.1 | 1 decode in 2 periods, 0 lost |

A WSPR decode's time depends on what is in the period (signals, noise),
so one saved period was also decoded offline with WSJT-X's own command
(`wsprd -C 500 -o 4 -d`) and
with `wsprrx --file`, three times each:

| System | wsprd, nothing else running | wsprd, Quisk streaming 384 ksps | wsprrx `--file` |
|---|---|---|---|
| Bookworm | 1.34 s | 1.47 to 1.48 s | 1.31 s |
| Trixie | 1.31 to 1.32 s | 1.35 to 1.38 s | 1.33 s |

Wall and CPU time were within 0.02 s of each other in every run: the
decoder was never waiting for a core.

### What this shows

- radioberryd uses 2 to 4% of one core and its driver thread 0.5%, rising
  with the sample rate, the same on Bookworm and trixie and with every
  client; idle with no client, both use nothing.
- The client and WSJT-X's decoder are the largest costs (15 to 37% and up
  to 22% of one core); the whole Pi stays at 7 to 16% of its four cores.
- A WSPR decode takes 0.2 to 5.5 s on the Pi 5, and the same period
  decodes in the same time on either system; streaming at 384 ksps adds at
  most 10%.
