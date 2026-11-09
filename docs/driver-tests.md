# Driver hardware tests

`rbdrivertest` ([tools/driver-tests/](../tools/driver-tests/)) tests
`radioberry.ko` through `/dev/radioberry` on a Raspberry Pi 5 with the
Radioberry attached. The unit tests cover the driver's pure logic in
userspace ([driver/tests/](../driver/tests/)); this harness covers the
kernel glue: the character device, the ioctls, the DMA streams and the
gateware.

It is receive only. No command sets PTT or the TX permission bit, and
every TX sample written is zero, so the gateware drains and discards
them. It needs no permission to run under the safety rules in
[CONTRIBUTING.md](../CONTRIBUTING.md).

## Running it

It is built with the unit tests (`RADIOBERRY_BUILD_TESTS`, on by
default) but is not part of `ctest` and is not installed, since it needs
the hardware and an idle device. Stop anything that holds
`/dev/radioberry` first:

```
sudo systemctl stop radioberry
sudo build/tools/driver-tests/rbdrivertest
sudo systemctl start radioberry
```

It is a Catch2 binary, so Catch2's options apply (a test name or a tag
such as `[read]` runs part of it). `RADIOBERRY_DEVICE=/dev/other` tests
another device node; sysfs is always read from
`/sys/bus/spi/devices/spi0.0/`.

[tools/ci-pi.sh](../tools/ci-pi.sh) runs it at the end of every CI run
when `/dev/radioberry` exists and can be opened, and reports it skipped
otherwise ("no /dev/radioberry", or "/dev/radioberry is busy" while
`radioberry.service` or another program holds it).

## What it checks

| Test | Checks |
|---|---|
| the device opens once at a time | a second open gives EBUSY; the device opens again after close |
| info agrees with sysfs | ABI version, gateware version, receivers, transmitters and FPGA type match `/sys/bus/spi/devices/spi0.0/` |
| reads return whole frames for every receiver count | for 1 to the gateware's receiver count at 48 ksps: reads stop at frame boundaries, more than 8000 frames in 200 ms, no resyncs or dropped words, a read shorter than one frame gives EINVAL |
| non-blocking reads drain to EAGAIN | a non-blocking reader empties the ring and gets EAGAIN |
| poll reports frames, TX space and RX overrun | POLLIN and POLLOUT when ready; three receivers at 384 ksps left unread overrun the ring, `poll()` reports POLLERR, the statistics report and then clear the overrun |
| short reads and partial TX samples are rejected | reads under a frame and writes of partial samples give EINVAL; a zero-length write returns 0 |
| non-blocking writes fill the TX ring and it drains at 48 ksps | zero samples fill the ring to EAGAIN with PTT clear, then drain at 48 000 words a second (within 512) with no underruns |
| unknown ioctls fail with ENOTTY | an unknown ioctl number |
| after close the gateware reports no transmit state | closing sends the safe command: PTT clear in the statistics and in the gateware's reply |
| the daemon's device layer streams through the driver | `radioberryd`'s device layer for one second: 47 000 to 49 000 frames, no sequence gaps, no dropped blocks, no faults, TX zeros written with PTT clear |
| the RX stream never stalled during the run | runs last: sysfs `rx_stalled` is 0 and `rx_stalls` did not change, so no receive lock-up ([fault-rx-lockup.md](fault-rx-lockup.md)) happened during the run |

The receiver-count test is also the pattern that triggers the gateware's
receive lock-up when the driver's RX clock hold is disabled
(`rx_clock_hold_us=0`); [fault-rx-lockup.md](fault-rx-lockup.md)
describes running it in a loop to measure the lock-up rate.

[kernel-debug.md](kernel-debug.md) describes a run of this harness and
more under a debug kernel.
