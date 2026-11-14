#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host side of the link test image, stage 1.

Runs on the Raspberry Pi as root with the test image loaded, the radio's
overlay removed and dtparam=spi=on. Talks to the image's register map
through /dev/spidev0.0 and moves header pins with pinctrl.

  linktest.py id | clock [SECONDS] | pins | cn301 | ad | rx | bist | all
"""
import json
import subprocess
import sys
import time

import spidev

ID, VERSION, SCRATCH, STATUS, SYSCNT = 0x00, 0x01, 0x02, 0x03, 0x04
FCNT_CLK69, FCNT_RXCLK, FCNT_RXSYNC = 0x05, 0x06, 0x07
PIN_IN, PIN_OE, PIN_OUT, CN301_IN = 0x08, 0x09, 0x0A, 0x0B
AD_CTRL, AD_SER, AD_SER_RES = 0x10, 0x11, 0x12
RX_PINS, RX_BITS, RX_MINMAX, RX_SNAP = 0x18, 0x19, 0x1A, 0x1B
BIST = 0x20

# Index in PIN_IN -> FPGA pin. 0 to 18 can be driven; 19 and 20 cannot.
FPGA_PINS = [31, 32, 33, 44, 43, 42, 49, 50, 13, 39, 46, 113, 112, 28, 6, 7, 8, 10, 11, 54, 55]
NIO = 19
# Header GPIOs that may be moved. Left out: 0 to 3 (ID EEPROM, I2C), 8 to
# 11 (SPI0), 14 and 15 (console), 22, 26 and 27 (CONF_DONE, nSTATUS,
# nCONFIG), 24 (DCLK, not a user pin).
GPIOS = [4, 5, 6, 7, 12, 13, 16, 17, 18, 19, 20, 21, 23, 25]
CN301_NAMES = ["tip (120)", "ring (121)", "paddle left (141)", "paddle right (142)",
               "115", "119", "125", "132", "133", "135"]


class Link:
    def __init__(self, hz=10_000_000):
        self.spi = spidev.SpiDev()
        self.spi.open(0, 0)
        self.spi.mode = 0
        self.spi.max_speed_hz = hz

    def xfer(self, write, addr, data=0):
        out = [(0x80 if write else 0) | addr, 0,
               (data >> 24) & 0xFF, (data >> 16) & 0xFF, (data >> 8) & 0xFF, data & 0xFF]
        r = self.spi.xfer2(out)
        return (r[0] << 8) | r[1], (r[2] << 24) | (r[3] << 16) | (r[4] << 8) | r[5]

    def rd(self, addr):
        head, q = self.xfer(False, addr)
        if head != 0xA55A:
            raise RuntimeError("reply header 0x%04x, want 0xa55a" % head)
        return q

    def wr(self, addr, data):
        self.xfer(True, addr, data)


def pinctrl(*args):
    return subprocess.run(["pinctrl", *args], check=True, capture_output=True, text=True).stdout


def gpio_level(gpio):
    return int(pinctrl("lev", str(gpio)).strip())


def t_id(link):
    head, q = link.xfer(False, ID)
    res = {"reply_header": "0x%04x" % head, "id": "0x%08x" % q,
           "version": "0x%08x" % link.rd(VERSION)}
    link.wr(SCRATCH, 0x12345678)
    a = link.rd(SCRATCH)
    link.wr(SCRATCH, 0xEDCBA987)
    b = link.rd(SCRATCH)
    res["scratch_ok"] = a == 0x12345678 and b == 0xEDCBA987
    st = link.rd(STATUS)
    res["pll_locked"] = bool(st & 1)
    res["pll_lock_lost_since_clear"] = bool(st & 2)
    link.wr(STATUS, 2)
    res["ok"] = head == 0xA55A and q == 0x52425431 and res["scratch_ok"] and res["pll_locked"]
    return res


def t_clock(link, seconds=10.0):
    def stamp():
        t0 = time.monotonic_ns()
        c = link.rd(SYSCNT)
        t1 = time.monotonic_ns()
        return (t0 + t1) / 2, c, t1 - t0

    ta, ca, wa = stamp()
    # The 32-bit counter wraps every 55.9 s; count wraps by expected rate.
    time.sleep(seconds)
    tb, cb, wb = stamp()
    dt = (tb - ta) / 1e9
    # A short interval first gives the rate with which to count wraps.
    tc, cc, _ = stamp()
    time.sleep(0.2)
    td, cd, _ = stamp()
    rough = ((cd - cc) & 0xFFFFFFFF) / ((td - tc) / 1e9)
    wraps = round((rough * dt - ((cb - ca) & 0xFFFFFFFF)) / 2**32)
    ticks = ((cb - ca) & 0xFFFFFFFF) + wraps * 2**32
    f = ticks / dt
    window = 2**24 / f
    res = {"seconds": round(dt, 3), "pin53_hz": round(f, 1),
           "pin53_ppm_from_76p8": round((f / 76.8e6 - 1) * 1e6, 1),
           "read_window_us": [wa / 1e3, wb / 1e3]}
    for name, reg in (("pin69_hz", FCNT_CLK69), ("pin71_rxclk_hz", FCNT_RXCLK),
                      ("pin77_rxsync_hz", FCNT_RXSYNC)):
        res[name] = round(link.rd(reg) * 16 / window, 0)
    return res


def t_pins(link):
    link.wr(PIN_OE, 0)
    res = {"pi_drives": {}, "fpga_drives": {}, "idle_pin_in": "0x%06x" % link.rd(PIN_IN)}
    for g in GPIOS:
        pinctrl("set", str(g), "op", "dl")
        low = link.rd(PIN_IN)
        pinctrl("set", str(g), "op", "dh")
        high = link.rd(PIN_IN)
        pinctrl("set", str(g), "ip", "pn")
        follows = [FPGA_PINS[i] for i in range(len(FPGA_PINS)) if (high >> i) & 1 and not (low >> i) & 1]
        res["pi_drives"]["gpio%d" % g] = follows
    # The other way, for each wire found above that the FPGA can drive.
    for g in GPIOS:
        for pin in res["pi_drives"]["gpio%d" % g]:
            i = FPGA_PINS.index(pin)
            if i >= NIO:
                res["fpga_drives"]["pin%d" % pin] = "input-only FPGA pin"
                continue
            pinctrl("set", str(g), "ip", "pn")
            link.wr(PIN_OUT, 0)
            link.wr(PIN_OE, 1 << i)
            low = gpio_level(g)
            link.wr(PIN_OUT, 1 << i)
            high = gpio_level(g)
            link.wr(PIN_OE, 0)
            res["fpga_drives"]["pin%d" % pin] = {"gpio": g, "ok": low == 0 and high == 1}
    return res


def t_cn301(link):
    v = link.rd(CN301_IN)
    return {CN301_NAMES[i]: (v >> i) & 1 for i in range(10)}


def ad_xfer(link, word, oemask):
    link.wr(AD_SER, (oemask << 16) | word)
    time.sleep(0.001)
    if link.rd(STATUS) & 4:
        raise RuntimeError("AD9866 serial engine stuck busy")
    r = link.rd(AD_SER_RES)
    return (r >> 16) & 0xFFFF, r & 0xFFFF


def t_ad(link):
    """Probes the AD9866 serial port. A write is {0, 00, addr[4:0],
    data[7:0]}, as the radio gateware sends it. A read is tried as the
    same word with the top bit set and the data half released; both the
    data line and the data-out line are captured."""
    res = {}
    link.wr(AD_CTRL, 0b10)          # in reset, mode high
    time.sleep(0.01)
    link.wr(AD_CTRL, 0b11)          # release reset
    time.sleep(0.05)

    def dump():
        d = {}
        for a in range(0x20):
            sdio, sdo = ad_xfer(link, 0x8000 | (a << 8), 0xFF00)
            d["0x%02x" % a] = {"sdio": "0x%02x" % (sdio & 0xFF), "sdo": "0x%02x" % (sdo & 0xFF)}
        return d

    res["after_reset"] = dump()
    # Register 0x08 (receive filter target) is written by the radio
    # gateware with 0x40; write two values and read each back.
    back = []
    for value in (0x40, 0x41):
        ad_xfer(link, (0x08 << 8) | value, 0xFFFF)
        sdio, sdo = ad_xfer(link, 0x8000 | (0x08 << 8), 0xFF00)
        back.append({"wrote": "0x%02x" % value, "sdio": "0x%02x" % (sdio & 0xFF),
                     "sdo": "0x%02x" % (sdo & 0xFF)})
    res["reg08_write_read"] = back
    return res


def t_rx(link, seconds=0.5):
    link.wr(RX_PINS, 0)
    time.sleep(seconds)
    pins = link.rd(RX_PINS)
    bits = link.rd(RX_BITS)
    mm = link.rd(RX_MINMAX)

    def s12(v):
        return v - 4096 if v & 0x800 else v

    seen1, seen0 = pins & 0x7F, (pins >> 8) & 0x7F
    names = ["rx0", "rx1", "rx2", "rx3", "rx4", "rx5", "rxsync"]
    return {
        "pins_toggling": [names[i] for i in range(7) if (seen1 >> i) & 1 and (seen0 >> i) & 1],
        "pins_stuck_high": [names[i] for i in range(7) if (seen1 >> i) & 1 and not (seen0 >> i) & 1],
        "pins_stuck_low": [names[i] for i in range(7) if not (seen1 >> i) & 1 and (seen0 >> i) & 1],
        "sample_bits_toggling": "0x%03x" % ((bits & 0xFFF) & ~((bits >> 16) & 0xFFF) & 0xFFF),
        "min": s12(mm & 0xFFF), "max": s12((mm >> 16) & 0xFFF),
        "snapshot": s12(link.rd(RX_SNAP) & 0xFFF),
    }


def tone_level(link, hz):
    """Level of one frequency in a raw receive capture (1024 samples at
    76.8 Msps), by Goertzel, with the mean of four neighbouring bins for
    comparison. Rising-edge data, high half before the half with sync."""
    import math
    link.wr(0x40, 0)
    time.sleep(0.01)
    raw = []
    for i in range(2048):
        link.wr(0x41, i)
        raw.append(link.rd(0x42) & 0x7F)
    s = []
    for i in range(1, len(raw)):
        if raw[i] >> 6:
            v = ((raw[i - 1] & 0x3F) << 6) | (raw[i] & 0x3F)
            s.append(v - 4096 if v & 0x800 else v)
    n = len(s)
    mean = sum(s) / n

    def g(f):
        w = 2 * math.pi * f / 76.8e6
        c = 2 * math.cos(w)
        s1 = s2 = 0.0
        for x in s:
            s0 = (x - mean) + c * s1 - s2
            s2, s1 = s1, s0
        return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0)) / n

    bin_hz = 76.8e6 / n
    side = [g(hz + k * bin_hz) for k in (-6, -4, 4, 6)]
    return {"at_tone": round(g(hz), 2), "neighbours": round(sum(side) / 4, 2), "samples": n}


def t_bist(link):
    link.wr(BIST, 1)
    time.sleep(0.1)
    st = link.rd(STATUS)
    e = link.rd(BIST)
    return {"done": bool(st & 0x10), "ram_errors": e & 0xFFFF, "multiplier_errors": e >> 16,
            "ok": bool(st & 0x10) and e == 0}


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    link = Link()
    tests = {"id": t_id, "clock": t_clock, "pins": t_pins, "cn301": t_cn301,
             "ad": t_ad, "rx": t_rx, "bist": t_bist}
    if what in ("pa", "tone"):
        # ARMED TESTS. Both act on the transmit side and are for the board
        # owner to run, with the dummy load on and any external amplifier
        # off. They refuse unless --i-am-the-owner is given.
        #   pa BITS MS        PA lines for MS ms (bits: 1 exttr, 2 inttr,
        #                     4 envpa, 8 envbias)
        #   tone HZ AMP MS    tone on the AD9866 transmit bus for MS ms at
        #                     the lowest transmit gain, then the level of
        #                     that frequency in the receive capture
        if "--i-am-the-owner" not in sys.argv:
            raise SystemExit("armed test: run it yourself with --i-am-the-owner, "
                             "dummy load on, external amplifier off")
        args = [a for a in sys.argv[2:] if not a.startswith("--")]
        ms = int(args[-1], 0)
        if not 1 <= ms <= 10000:
            raise SystemExit("MS must be 1 to 10000")
        if what == "tone":
            hz, amp = float(args[0]), int(args[1], 0)
            if not 0 < hz < 30e6 or not 0 <= amp <= 0xFFF:
                raise SystemExit("HZ 0 to 30 MHz, AMP 0 to 4095")
            ad_xfer(link, (0x0A << 8) | 0x40, 0xFFFF)   # lowest transmit gain
            if not link.rd(0x50) >> 29 & 1:
                raise SystemExit("the image did not take the low transmit gain")
            link.wr(0x54, int(round(hz * 2**32 / 76.8e6)) & 0xFFFFFFFF)
        link.wr(0x53, ms)
        link.wr(0x50, 0x41524D31)
        link.wr(0x50, 0x54583221)
        st = link.rd(0x50)
        if not st >> 31:
            raise SystemExit("did not arm: 0x%08x" % st)
        if what == "pa":
            link.wr(0x51, int(args[0], 0) & 0xF)
            print("armed, PA lines 0x%x for %d ms" % (int(args[0], 0) & 0xF, ms))
            time.sleep(ms / 1000 + 0.05)
        else:
            link.wr(0x52, (amp << 16) | 1)
            time.sleep(min(0.05, ms / 2000))
            level = tone_level(link, hz)
            time.sleep(max(0, ms / 1000 - 0.05) + 0.05)
            print(json.dumps({"tone_hz": hz, "amplitude": amp, "rx_level_at_tone": level}))
        link.wr(0x50, 0)   # disarm, in case it has not lapsed yet
        st = link.rd(0x50)
        print("disarmed" if not st >> 31 else "STILL ARMED 0x%08x" % st)
        return
    if what == "rxtone":
        # rxtone HZ: level of HZ in the receive capture, transmitter idle
        print(json.dumps({"tone_hz": float(sys.argv[2]),
                          "rx_level_at_tone": tone_level(link, float(sys.argv[2]))}))
        return
    if what == "cap":
        # cap [FILE]: raw receive bus capture, then an analysis of each
        # edge and each pairing of halves.
        link.wr(0x40, 0)
        time.sleep(0.01)
        if not link.rd(0x40) & 1:
            raise RuntimeError("capture did not complete")
        raw = []
        for i in range(2048):
            link.wr(0x41, i)
            raw.append(link.rd(0x42))
        if len(sys.argv) > 2:
            with open(sys.argv[2], "w") as f:
                f.write("\n".join("%04x" % v for v in raw) + "\n")

        def s12(v):
            return v - 4096 if v & 0x800 else v

        out = {}
        for edge, shift in (("rising", 0), ("falling", 7)):
            e = [(v >> shift) & 0x7F for v in raw]
            sync = [x >> 6 for x in e]
            d = [x & 0x3F for x in e]
            alt = sum(1 for i in range(1, len(e)) if sync[i] != sync[i - 1])
            res = {"sync_high": sum(sync), "sync_changes": alt}
            for name, hi_off in (("high_before_sync", -1), ("high_after_sync", 1)):
                samples = []
                for i in range(1, len(e) - 1):
                    if sync[i]:
                        samples.append(s12((d[i + hi_off] << 6) | d[i]))
                if samples:
                    diffs = [abs(samples[i] - samples[i - 1]) for i in range(1, len(samples))]
                    res[name] = {"min": min(samples), "max": max(samples),
                                 "mean": round(sum(samples) / len(samples), 1),
                                 "mean_step": round(sum(diffs) / len(diffs), 1)}
            out[edge] = res
        print(json.dumps(out, indent=1))
        return
    if what == "i2s-start":
        # i2s-start DIV RXPH MODE RXOFF [FS_SHORT]: bit clock = 122.88 MHz / DIV
        div, rxph, mode, rxoff = (int(a, 0) for a in sys.argv[2:6])
        short = int(sys.argv[6], 0) if len(sys.argv) > 6 else 0
        link.wr(0x30, 0)
        time.sleep(0.01)
        link.wr(0x30, (rxoff << 24) | (rxph << 16) | (div << 8) | (short << 2) | (mode << 1) | 1)
        return
    if what == "i2s-stop":
        link.wr(0x30, 0)
        return
    if what == "i2s-stat":
        link.wr(0x31, 0)
        time.sleep(0.001)
        names = ["tx_words", "rx_words", "rx_errors", "rx_last", "first_error_got", "first_error_want"]
        vals = [link.rd(0x32 + i) for i in range(6)]
        out = {n: (v if i < 3 else "0x%08x" % v) for i, (n, v) in enumerate(zip(names, vals))}
        out["snapshot_taken"] = bool(link.rd(0x31) & 1)
        print(json.dumps(out))
        return
    if what == "ad-write":
        ad_xfer(link, (int(sys.argv[2], 0) << 8) | int(sys.argv[3], 0), 0xFFFF)
        return
    if what == "ad-read":
        sdio, _ = ad_xfer(link, 0x8000 | (int(sys.argv[2], 0) << 8), 0xFF00)
        print("0x%02x" % (sdio & 0xFF))
        return
    if what == "ad-reset":
        link.wr(AD_CTRL, 0b10)
        time.sleep(0.01)
        if len(sys.argv) > 2 and sys.argv[2] == "release":
            link.wr(AD_CTRL, 0b11)
        return
    if what == "all":
        out = {name: fn(link) for name, fn in tests.items()}
    elif what == "clock" and len(sys.argv) > 2:
        out = t_clock(link, float(sys.argv[2]))
    else:
        out = tests[what](link)
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
