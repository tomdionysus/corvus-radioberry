# Kernels

The driver is built by DKMS against the installed kernel's headers; one
source serves the 6.12 and 6.18 Raspberry Pi OS kernels on both boards.
Installing a kernel with its headers (`apt` does this for the
`linux-image-rpi-v8` and `linux-image-rpi-2712` metapackages) builds the
driver for it; nothing needs reinstalling.

## Differences between 6.12 and 6.18

The Raspberry Pi 4 driver uses the PCM block and its DMA channels, which
do not differ between the two series. The Raspberry Pi 5
driver streams through the RP1's PIO with the kernel's `rp1-pio` driver
and the `dw-axi-dmac` DMA controller, which differ:

| | 6.12 | 6.18 | Driver |
|---|---|---|---|
| `rp1_pio_sm_xfer_data()` from the PIO, given a buffer and no DMA address | receives into `rp1-pio`'s bounce buffer and never copies it to the buffer: every word reads 0 | copies the bounce buffer to the buffer on completion | allocates its receive buffers from the RP1 DMA controller (`dma_alloc_coherent()`) and passes their DMA addresses, so the DMA writes them directly on both |
| DMA request thresholds (`DMACTRL`) after a FIFO clear | do not match the 8-word bursts: receive bursts repeat the last word, transmit bursts overfill the FIFO | match | sets `DMACTRL` itself: receive requests when the FIFO holds 8 words, transmit when it is empty |
| `rp1_pio_sm_xfer_data()` when its transfer queue is full | blocks until a slot frees | returns `-EBUSY` | keeps at most four transfers in flight per direction and treats `-EBUSY` as "try later" |
| `dw_axi_dma_of_xlate()` | `chan_flags_all` uninitialised | initialised to 0 | none needed; no effect seen |
| Device tree | `dma@188000` as in 6.18 | adds the light channel names (`tx0l`, `rx0l`, ...) | takes the DMA controller from the PIO node's first `dmas` entry, present in both |

## Tested kernels

Each kernel, installed from the Raspberry Pi OS archive with its headers
(DKMS built the driver at install), booted on the board with a 10CL025
RadioBerry, passed:

- `rbprobe`: radio can run
- `ft8rx` on 40 m, four 15 s periods each over Protocol 1 and Protocol 2:
  decodes, no packets lost
- `rbcat --seconds 10 --tx-zeros`: 48 000 frames per second, no resyncs
  or dropped words, TX taken at 48 000 words per second
- `rbdrivertest` ([Driver hardware tests](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/docs/driver-tests.md)): all tests

| Kernel | Raspberry Pi 4 (`rpi-v8`) | Raspberry Pi 5 (`rpi-2712`) |
|---|---|---|
| 6.12.25 | passed | passed |
| 6.12.34 | passed | passed |
| 6.12.47 | passed | passed |
| 6.12.62 | passed | passed |
| 6.12.75 | passed, one accepted anomaly | passed |
| 6.18.29 | passed | passed |
| 6.18.33 | passed | passed |
| 6.18.34 | passed | passed |
| 6.18.39 | passed | passed |
| 6.18.50 | passed | passed |
| 6.18.60 | passed | passed |

Accepted anomaly: on the Pi 4 with 6.12.75, one of 11 `rbdrivertest` runs
failed one test ("the daemon's device layer streams through the
driver"): 31 receive blocks overflowed the test's queue between the
daemon's device layer and the test (`rx-queue-full`). The driver lost
nothing in that boot (`rbcat` and `ft8rx` above), and the other 10 runs
on that kernel passed.

The per-board lists are in [Raspberry Pi 4](rpi4.md#tested-kernels) and
[Raspberry Pi 5](rpi5.md#tested-kernels).
