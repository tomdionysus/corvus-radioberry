# History: the two reference projects

corvus-radioberry starts from two existing projects, kept as read-only
reference checkouts beside this repository. Neither is modified here. This
note records what each one is, how it is laid out, and where the
authoritative details live, so later work in this repository can cite them
without re-reading them.

Survey date: 2026-11-08.

## 1. Radioberry-2.x (upstream, pa3gsb)

Origin: https://github.com/pa3gsb/Radioberry-2.x
Checked-out state: master at `b144078` (2026-10-24, "pi5 image doc"),
tag `radioberry@rpi5`.
Licence: Unlicense (public domain) for the Pi driver and daemon sources.

### Intent

Radioberry is a Raspberry Pi HAT ("cape") built around an AD9866 12-bit
RX/TX front end and an Intel Cyclone 10 FPGA (10CL016 or 10CL025). The
gateware is the Hermes-Lite 2 OpenHPSDR design, so the board presents itself
to SDR hosts as a Hermes-Lite (board id 6). The repository holds everything
the author (Johan Maas, PA3GSB) needs to run the board from several single
board computers, plus hardware files and a USB variant ("juice").

The author's current focus is the Raspberry Pi 5 only. The rpi-4 and rpi-3
trees are marked deprecated and receive bug fixes at most. The rpi-5 GPIO
bit-bang driver has been archived; only the RP1 PIO plus DMA driver is
supported.

This checkout worked for the user previously under an older Linux. It is the
ground truth for the driver ABI and for how the gateware is driven.

### Structure

```
Radioberry-2.x/
  Makefile              top-level build; still points at SBC/rpi-4 paths
  README.md             build and install steps (pihpsdr + wdsp submodules)
  .gitmodules           pihpsdr (g0orx) and wdsp (g0orx), shallow
  docs/                 AD9866 datasheet, photos, pinout spreadsheets
  hardware/             KiCad schematics, BOM scripts, PA HAT
  juice/                USB (FTDI FT2232H) Radioberry variant, Win/Linux
  SBC/
    rpi-5/              current target
      device_driver/
        pio-mode/driver/   kernel module radioberry.ko (driver 5.54)
        firmware/          userspace Protocol 1 daemon (firmware 5.0.7)
        systemd/           radioberry.service + /etc/init.d/radioberryd
      utility/gatewareloader/  standalone FPGA loader
      archive/           retired GPIO-mode rpi-5 driver
      image/             PDF describing the published Pi 5 image
    rpi-4/              deprecated: GPIO/IRQ driver (4.02), daemon, Soapy
    rpi-3/              oldest: file-descriptor firmware, Verilog, tools
    Tinkerboard/        ASUS Tinker Board port
```

### Pi 5 kernel driver (SBC/rpi-5/device_driver/pio-mode/driver)

Files: `rb2-rp1-pio.c` (char device, ioctl, read, write),
`src/rb2-rx-stream.c` and `src/rb2-tx-stream.c` (PIO programs, DMA, kfifo),
`src/rb2-trx-control.c` (SPI control transfer), `src/rb2-load-fpga.c`
(bit-bangs the .rbf into the FPGA via request_firmware), `src/rb2-rpi5.c`
(direct RP1 GPIO register access), `radioberry.dts` overlay.

Behaviour that any replacement daemon must honour:

- `/dev/radioberry` is exclusive (mutex_trylock on open, EBUSY otherwise).
- One ioctl, `RADIOBERRY_IOC_COMMAND` (`_IOW('x', 1, __u8)`), carrying
  `struct rb_info_arg_t`. The driver packs rb_command, command and the four
  command_data bytes into a 6-byte SPI transfer on spi0 CE0 and returns the
  gateware's reply: major/minor version, fpga type, receiver and transmitter
  counts, driver version. Command 0x00 also sets the driver's receiver count
  from C4 bits 5:3.
- `read()` ignores the requested count. With N receivers it drains
  `(504 / (6N + 2)) * N * 8` bytes from the kfifo, strips the 4-bit metadata
  nibble from each 4-byte word, and returns 3 bytes per word. For one
  receiver that is 63 complex IQ24 samples, 378 bytes, per call. It blocks
  up to 1 s and returns EAGAIN on timeout.
- `write()` accepts any length of 4-byte I16/Q16 samples, waits up to 1 s for
  kfifo space, and DMA starts once 16384 bytes are queued.
- RX PIO uses GPIO 6 (clock out), 25 (ready), 18-21 (4-bit data).
  TX PIO uses GPIO 4 (clock), 5 (serial data), 12 (ready).
- Gateware loading uses GPIO 22, 26, 27, 13, 24 and expects
  `/lib/firmware/radioberry.rbf`.
- The Makefile hard-codes `KBUILD_EXTRA_SYMBOLS` to a `Module.symvers` in
  a private `rp1-driver` checkout under the author's home directory, so the
  module is built against an out-of-tree rp1-pio symbol set. This, plus the one-argument
  `class_create()`, ties it to a specific Raspberry Pi kernel generation.

### Pi 5 userspace daemon (SBC/rpi-5/device_driver/firmware)

`radioberry.c` implements OpenHPSDR Protocol 1 (the Hermes-Lite dialect) on
UDP and TCP port 1024. It opens the device, polls the gateware version until
non-zero, then runs threads for packet reception, TX writing, SPI control,
I2C measurement, and a periodic timer. It supports PA bias and current or
temperature measurement over I2C (`bias.c`, `measure.c`), band filter
switching (`filters.h`), and registers the radio with the author's website
after 60 s (`register.c`). Each outgoing 1032-byte frame is built from two
device reads. TX samples arrive 8 bytes per slot (4 EER bytes discarded, 4
IQ bytes written to the device).

### Reference material inside the repository

- `docs/AD9866.pdf`: front end datasheet.
- `docs/misc/*.xls`: FPGA pinouts for 10CL016 and 10CL025.
- `SBC/rpi-5/image/radioberry@rpi5.pdf`: the released Pi 5 image.
- `SBC/rpi-4/device_driver/driver/README.md`: step by step driver install,
  still the clearest written procedure.
- Gateware: https://github.com/softerhardware/Hermes-Lite2 and its protocol
  wiki https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol
- Project wiki: https://github.com/pa3gsb/Radioberry-2.x/wiki

## 2. radioberry-p2 (Protocol 2 draft)

Checked-out state: single commit `39059de` (2026-08-07, "Initial Commit Base
Code") on `main`, clean working tree.
Licence: none stated.

### Intent

A deliberately small OpenHPSDR Protocol 2 server for Radioberry on the Pi 5.
It talks to the existing `/dev/radioberry` ABI above and exposes one DDC
and one DUC at 48 ksps, 24-bit, over UDP. It is a hardware adapter, not an
SDR framework: no audio, no PureSignal, no wideband, no DSP, no resampling,
no TCP, no firmware programming. The user reports it somewhat worked but
has issues.

Its README states two boundaries that explain most of the "issues":

1. The TX profile is 48 ksps. Unmodified piHPSDR always sends 192 ksps TX IQ
   over Protocol 2, so TX from stock piHPSDR will overrun the 16-packet
   queue and trip the fail-safe. A piHPSDR change is required, and the
   README refuses to add a 192 to 48 ksps resampler.
2. The code was compile and test complete but never had a hardware
   validation pass (discovery, RX, frequency, zero-drive TX, TX continuity).

### Structure

```
radioberry-p2/
  CMakeLists.txt          C++20, pthreads only, warnings as -Wall -Wextra
                          -Wpedantic -Wconversion -Wshadow
  README.md               scope, architecture, ABI assumptions, run notes
  src/
    main.cpp              option parsing, MAC lookup, signal handling
    radioberry.hpp/.cpp   IRadioBerry seam and RadioBerryDevice
    protocol2.hpp/.cpp    packet encode/decode, phase words, IQ24 to IQ16
    server.hpp/.cpp       Protocol2Server: control, RX and TX threads
    spsc_ring.hpp         lock-free single producer single consumer ring
  systemd/radioberry-p2.service
  tests/
    protocol2_test.cpp    packet layout and conversion checks
    server_test.cpp       control translation and fail-safe checks
    radioberry_fake.hpp   FakeRadioBerry used by server_test
    test.hpp              CHECK macro
```

### Design summary

- Ports: binds 1024 to 1029 for host packets and 1035 for DDC0 IQ.
  Discovery replies from 1024, high-priority status from 1025.
- `RadioBerryDevice` reuses the Protocol 1 command bytes: 0x00 run/receivers,
  0x02 TX frequency, 0x04 RX frequency, 0x12 drive with the PA bit. The
  rb_command byte carries RUN in bit 0 and PA permission in bit 2. PA enable
  is off unless `--enable-pa` is given, because the I2C interlocks from the
  Protocol 1 daemon are not reproduced.
- RX thread assembles 238-sample packets from the driver's 63-sample reads.
- TX thread pops 240-sample packets from the ring, narrows IQ24 to IQ16 by
  keeping the top 16 bits, and writes one 960-byte block per packet.
- Session: first non-discovery packet claims the radio; 1 s inactivity
  releases it and drops MOX, RUN and DDC. Sequence gap, queue overrun or
  device write failure clears MOX, zeroes drive and latches a fault until
  the host sends MOX off.
- Frequencies are expected as phase words (General packet byte 37 bit 3) and
  converted back to Hz with a 122.88 MHz reference before being sent to the
  gateware as Hz.

### References it was written against

- Radioberry Pi 5 daemon:
  https://github.com/pa3gsb/Radioberry-2.x/blob/master/SBC/rpi-5/device_driver/firmware/radioberry.c
- Radioberry Pi 5 PIO driver:
  https://github.com/pa3gsb/Radioberry-2.x/blob/master/SBC/rpi-5/device_driver/pio-mode/driver/rb2-rp1-pio.c
- ioctl definition:
  https://github.com/pa3gsb/Radioberry-2.x/blob/master/SBC/rpi-5/device_driver/firmware/radioberry_ioctl.h
- piHPSDR Protocol 2 implementation, header and discovery:
  https://github.com/dl1ycf/pihpsdr/blob/master/src/new_protocol.c
  https://github.com/dl1ycf/pihpsdr/blob/master/src/new_protocol.h
  https://github.com/dl1ycf/pihpsdr/blob/master/src/new_discovery.c

## 3. How the two relate

Radioberry-2.x owns the hardware, the gateware image, the kernel driver and
the Protocol 1 daemon. radioberry-p2 replaces only the daemon, keeping the
driver and gateware unchanged, and swaps Protocol 1 for a narrow Protocol 2
profile. Both daemons need the Protocol 1 daemon stopped first, since the
device is exclusive and both bind UDP 1024.

Points to keep in mind for work in this repository:

- The driver ABI (63-sample reads, 4-byte TX samples, 6-byte SPI commands)
  is defined by Radioberry-2.x, not by either daemon.
- The driver build is pinned to a particular Pi kernel through the rp1-pio
  symbols and the device tree overlay, which is the likely reason the older
  Linux worked and a newer one may not.
- radioberry-p2's 48 ksps TX decision is the main interoperability gap with
  stock piHPSDR and other Protocol 2 hosts.
