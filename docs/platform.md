# Platform

## Supported hardware

- Raspberry Pi 5 running Raspberry Pi OS (Debian 13 trixie, 64-bit),
  kernel 6.18.60+rpt-rpi-2712 or newer. The stock in-tree `rp1-pio` driver
  is used as it is.
- Radioberry 2.x HAT: AD9866 front end, Intel Cyclone 10 LP 10CL025 or
  10CL016, 0 to 30 MHz.
- AURSINC "Radioberry Preamp Board V2" (PCB marked PA5W V2) stacked on the
  Radioberry: 1.5 to 3.5 W output, manual bias, no I2C devices (`pa.md`).

## The development Pi

| Item | Value |
|---|---|
| Board | Raspberry Pi 5 Model B, 8 GB, NVMe root |
| OS | Raspberry Pi OS 64-bit (Debian 13 trixie), kernel 6.18.60+rpt-rpi-2712 |
| Toolchain | gcc 14.2, cmake 3.31, clang-format 19, lcov 2.0 |
| FPGA | 10CL025: CL016 images leave CONF_DONE low, CL025 images configure it |
| Network | Wi-Fi only |
| Also installed | piHPSDR and wdsp, built from source, for client tests |

The Radioberry's sample clock runs about 31 to 33 ppm fast against the
Pi's clock: `rbcat` measures 48001.5 to 48001.6 RX frames per second at
48 ksps.

`config.txt` has `dtparam=i2c_arm=on` and `dtoverlay=radioberry`. Its
`dtparam=spi=off` is overridden by the overlay. `i2cdetect -y 1` finds no
devices: no PA ADC at 0x34, no bias pot at 0x2C, no filter board at 0x20
to 0x22.

With the driver bound, GPIO 4, 5, 6, 12, 18 to 21 and 25 belong to the
RP1 PIO (stream pins) and GPIO 13, 22, 24, 26 and 27 to the driver (FPGA
configuration).

## Gateware images

`rbprobe` identifies images by hash. The driver needs this project's 75.3
PIO build (`gateware/PROVENANCE.md`). The original Radioberry-2.x images
below are 73.3 GPIO-mode gateware (8-bit RX bus, TX over SPI chip select
1) and do not work with it.

| Image (Radioberry-2.x, commit 269e402) | Family | FPGA | Size | SHA-256 |
|---|---|---|---|---|
| `SBC/rpi-5/archive/releases/dev/CL025/radioberry.rbf` | rpi-5 GPIO | 10CL025 | 334915 | fe6a4e8acb67fbf80aa679affc8f2e0e0abe15908938cb2aed10804b461105cd |
| `SBC/rpi-5/archive/releases/dev/CL016/radioberry.rbf` | rpi-5 GPIO | 10CL016 | 275438 | 5f146a8c2113a888bd6b7d08d8ac9f62b796e79dd950ecd47b31b5ec1ebe9e48 |
| `SBC/rpi-4/releases/dev/CL016/radioberry.rbf` | rpi-4 GPIO | 10CL016 | 276541 | aa5c36f42456bc5dee622a694db2b36aa31dadfbca2372e00e1cd952e7217da3 |

## Kernel facts the driver depends on

- `rp1_pio` is an in-tree module, loaded at boot, instance `pio0`. Its
  kernel API is `include/linux/pio_rp1.h`, with argument structs in
  `include/uapi/misc/rp1_pio_if.h`.
- `rp1_pio_sm_config_xfer()` allocates up to four coherent bounce buffers
  per state machine and direction. `rp1_pio_sm_xfer_data()` with
  `dma_addr` 0 never blocks, returns `-EBUSY` when all buffers are in
  flight, allocates with `GFP_KERNEL` (so it must be called from process
  context), copies FROM_SM data into the caller's buffer and calls the
  caller's callback from the DMA completion (softirq). Cyclic DMA exists
  only through the character device's ioctls.
- `rp1_pio_sm_buffer_virt`, which the original driver needs, is not
  exported by the stock kernel.
- `rp1_pio_add_program()` takes programs written for offset 0; the RP1
  firmware relocates jump targets to the load offset.
- `rp1_pio_close()` disables and unclaims the client's state machines and
  frees their DMA.

## Real-time behaviour and RX packet jitter

`radioberryd` serving one receiver at 48 ksps over Protocol 1 to a
receive-only test client (381 packets per second, one every 2.625 ms), 40
to 70 s per run. A packet's lateness is its arrival time less its ideal
time from its sequence number, with the clock drift between the two
machines fitted out; the earliest packet is 0.

| Client, conditions | p50 | p99 | p99.9 | max | sequence breaks |
|---|---|---|---|---|---|
| On the Pi (loopback), idle | 0.50 ms | 0.89 ms | 0.90 ms | 3.9 ms | 0 |
| On the Pi, 4 busy loops, client SCHED_FIFO 30, daemon `--no-realtime` | 2.03 ms | 4.70 ms | 7.00 ms | 9.8 ms | 0 |
| On the Pi, 4 busy loops, client SCHED_FIFO 30, daemon real-time | 0.50 ms | 0.88 ms | 0.89 ms | 3.0 ms | 0 |
| Laptop on the same Wi-Fi, Wi-Fi power save on | 0.74 ms | 90.9 ms | 123.6 ms | 128.3 ms | 3 |
| Laptop, Wi-Fi power save off | 0.75 ms | 107.1 ms | 126.3 ms | 129.6 ms | 12 |
| Laptop, daemon real-time with DSCP EF marking | 0.73 ms | 109.5 ms | 125.9 ms | 128.7 ms | 0 |

- Real-time scheduling keeps delivery under load as on time as on an idle
  Pi. Thread CPU affinity is left to the scheduler; nothing measured
  called for pinning.
- Over Wi-Fi the tail comes from the Pi's own background scans: while
  associated it scans all 42 channels (2.4 and 5 GHz) every 33.7 s for
  3.65 s, and each scan is a train of stalls of about 130 ms every 130 to
  170 ms, matching the Pi's `iw event` scan log. Wi-Fi power save made no
  difference. Outside the scans Wi-Fi adds about 0.25 ms at p50.
- A 130 ms stall exceeds the gateware's 85 ms TX FIFO, so a Protocol 2
  client transmitting over this Wi-Fi would run the FIFO dry during a
  scan.
- With DSCP EF and SO_PRIORITY 6, one run showed no stalls outside the
  scans and no sequence breaks, against 3 to 18 breaks in runs without;
  one run does not separate that from chance.
