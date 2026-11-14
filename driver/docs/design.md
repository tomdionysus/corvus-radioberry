# Driver design

## Probe and control

- SPI driver bound to the overlay's `sdr,radioberry` device on `spi0`
  chip select 0. The five FPGA configuration lines are gpiod descriptors
  from the same node.
- `probe()` picks the board's transport from the SoC (`brcm,bcm2711` or
  `brcm,bcm2712`), loads that board's image with `request_firmware`,
  checks CONF_DONE and nSTATUS, and polls the image's status over SPI
  until it reports the AD9866 set up and the radio running. It then
  checks the image's ID and CAPS registers (the right image for this
  board) and starts the transport. Any failure unwinds completely.
- Control is the images' framed protocol
  ([rb_frame.h](../kernel/rb_frame.h),
  [gateware/docs/interfaces.md](../../gateware/docs/interfaces.md)): 8-byte
  frames with a CRC, each reading or writing one register. A command from
  userspace (the six bytes of `radioberry_command`) becomes a radio-space
  write of its Hermes-Lite 2 address and data, plus CONTROL (run, PA
  allowed, loopback) when it changes and KEYING (PTT, permission). KEYING
  clears itself in the image unless rewritten within a second, so while it
  is set a delayed work item rewrites it every 250 ms: a host that stops
  stops the transmitter. The reply bytes are built from the identity and
  the image's second status byte.
- One mutex serialises commands. Closing the device sends a safe command
  (run, PTT and permission clear) and then the defaults (one receiver at
  48 ksps), so the next client starts from a known layout.

## Receive and transmit

- RX: the transport delivers `index << 24 | value` words. The parser checks
  the index sequence, resynchronises on a miss and counts resyncs, dropped
  words and overruns. Whole frames go into a ring; an overrun drops whole
  frames and sets a flag. After a receiver-count change, old-layout words
  are discarded until two clean frames of the new layout arrive.
- RX moves only while run is set: with run clear the transport stops its
  link, and the bus, the DMA and the driver's thread are idle.
- TX: the transport drains a ring of samples. When the ring is empty the
  driver sends nothing and counts an underrun if PTT is set; it never
  invents samples.
- Pure logic (command inspection, parsing, frame assembly, the ring, the
  frame protocol, TX packing) lives in headers with no kernel dependencies
  ([rb_logic.h](../kernel/rb_logic.h), [rb_frame.h](../kernel/rb_frame.h),
  [rb_pio.h](../kernel/rb_pio.h)) and is compiled into a userspace test
  binary.

## Raspberry Pi 4: I2S

- The image drives an I2S link into the BCM2711's PCM block, the FPGA as
  clock master: GPIO 18 bit clock, 19 frame sync, 20 data to the Pi, 21
  data from the Pi.
- The driver finds the PCM block by its compatible string
  (`brcm,bcm2835-i2s`), requires its node to be disabled (no audio overlay
  claims it), puts GPIO 18 to 21 in their PCM function itself, maps its
  registers and requests its two DMA channels. No ALSA device exists.
- Cyclic DMA each way over 16 periods. A period's length follows the
  receivers and rate; the bit clock divider the image runs at is chosen
  for the receive rate (`rb_i2s_div()`), and set while the link is
  stopped.
- TX words go as pairs: the sample, then a flags word with a sequence
  number and a check, which the image verifies. The driver fills the TX
  periods a few ahead of the DMA.
- With run clear the link and both DMA channels stop: no bit clock and no
  interrupts until run is set again.

## Raspberry Pi 5: RP1 PIO

- Two PIO state machines with DMA, running the driver's programs
  ([rb_pio.h](../kernel/rb_pio.h)): RX on GPIO 6 (clock), 18 to 21 (data)
  and 25 (sample-ready); TX on GPIO 4 (clock), 5 (data) and 12 (ready).
  The wire is in [gateware/docs/interfaces.md](../../gateware/docs/interfaces.md).
- RX: `rp1_pio_sm_xfer_data()` with four periods in flight, refilled by a
  SCHED_FIFO kernel thread (the completion callback runs in softirq and
  the call allocates). A period is one millisecond of samples at the
  current rate and receiver count. The RX buffers are the driver's own,
  allocated on the RP1 DMA device and passed by DMA address.
- TX: bounce-buffered DMA in periods of 256 samples, fed by the same
  thread.
- With run clear the RX state machine stops; the periods in flight wait
  for it and the thread sleeps.
- Capacity: with PIO clock divider 2 (100 MHz from the 200 MHz RP1 clock)
  one value takes 42 PIO cycles: about 2.38 million values per second.

`rp1-pio` facts the driver depends on:

- The RP1 has one PIO block of four state machines. Kernel clients get
  bounce-buffered DMA, at most four buffers per direction.
- `rp1_pio` is an in-tree module, loaded at boot, instance `pio0`. Its
  kernel API is `include/linux/pio_rp1.h`, with argument structs in
  `include/uapi/misc/rp1_pio_if.h`.
- `rp1_pio_sm_xfer_data()` never blocks, returns `-EBUSY` when all buffers
  are in flight, allocates with `GFP_KERNEL` (process context only) and
  calls the caller's callback from the DMA completion (softirq).
- `rp1_pio_add_program()` takes programs written for offset 0; the RP1
  firmware relocates jump targets to the load offset.
- `rp1_pio_close()` disables and unclaims the client's state machines and
  frees their DMA.
- `rp1_pio_open()` returns `-EPROBE_DEFER` until an RP1 PIO device has
  probed, which never happens on a Pi 4, so the transport is chosen before
  it is called.
- Before 6.18, rp1-pio's DMA request levels did not match its bursts; the
  driver sets DMACTRL itself.
