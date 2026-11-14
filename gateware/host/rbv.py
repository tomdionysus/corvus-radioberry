#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Framed control protocol of the gateware images, over /dev/spidev0.0
(SPI mode 3). For bring-up and checks; the driver speaks the same protocol.

  rbv.py rd board|radio ADDR
  rbv.py wr board|radio ADDR VALUE
  rbv.py status
  rbv.py ad-read REG
"""
import sys
import time

import spidev


def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


class Rbv:
    def __init__(self, hz=10_000_000):
        self.spi = spidev.SpiDev()
        self.spi.open(0, 0)
        self.spi.mode = 3
        self.spi.max_speed_hz = hz
        self.seq = 0

    def xfer(self, write, board, addr, data=0):
        body = [(0x80 if write else 0) | (0x40 if board else 0) | (addr & 0x3F),
                (data >> 24) & 0xFF, (data >> 16) & 0xFF, (data >> 8) & 0xFF, data & 0xFF,
                self.seq, 0]
        self.seq = (self.seq + 1) & 0xFF
        r = self.spi.xfer2(body + [crc8(body)])
        if r[0] != 0xA5 or crc8(r[:7]) != r[7]:
            raise RuntimeError("bad reply %s" % " ".join("%02x" % x for x in r))
        return r[1], (r[2] << 24) | (r[3] << 16) | (r[4] << 8) | r[5]

    def rd(self, board, addr):
        return self.xfer(False, board, addr)[1]

    def wr(self, board, addr, data):
        self.xfer(True, board, addr, data)


def main():
    r = Rbv()
    a = sys.argv[1:]
    if a[0] == "rd":
        print("0x%08x" % r.rd(a[1] == "board", int(a[2], 0)))
    elif a[0] == "wr":
        r.wr(a[1] == "board", int(a[2], 0), int(a[3], 0))
    elif a[0] == "status":
        names = ["id", "version", "caps", "status", "control", "keying", "i2s", "crc_err",
                 "frames", "fifo_full", "i2s_tx", "i2s_rx", "ad_read", "scratch", "ad_writes"]
        for i, n in enumerate(names):
            print("%-10s 0x%08x" % (n, r.rd(True, i)))
    elif a[0] == "ad-read":
        r.wr(True, 0x0C, int(a[1], 0))
        time.sleep(0.001)
        print("0x%02x" % (r.rd(True, 0x0C) & 0xFF))


if __name__ == "__main__":
    main()
