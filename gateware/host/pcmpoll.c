// SPDX-License-Identifier: GPL-2.0-or-later
//
// Raspberry Pi 4 side of the I2S link test, polled. Programs the BCM2711
// PCM block through /dev/mem as a clock and frame slave, two 32-bit
// channels in a 64-bit frame, then for a given time reads the receive
// FIFO and checks the word sequence the FPGA sends, and keeps the
// transmit FIFO filled with the same kind of sequence for the FPGA to
// check. No kernel driver and no ALSA are involved.
//
// The loop polls, so at high bit clocks the 64-word FIFOs can overflow or
// run dry when the process is held up; those events are counted from the
// block's error flags and are the host's, not the link's. A sequence
// break with neither flag set since the last check is a link error.
//
//   pcmpoll SECONDS MODE [TXPOS]
//     MODE   0 counting sequence, 1 xorshift32
//     TXPOS  position of the first transmit bit in the frame (default 0)
//
// Before: pinctrl set 18-21 a0. Run as root, ideally chrt -f 90 on an
// otherwise idle core.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PCM_BASE 0xfe203000u  // BCM2711: bus address 0x7e203000
enum { CS = 0, FIFO = 1, MODE = 2, RXC = 3, TXC = 4, DREQ = 5, INTEN = 6, INTSTC = 7 };
enum {
    CS_EN = 1u << 0,
    CS_RXON = 1u << 1,
    CS_TXON = 1u << 2,
    CS_TXCLR = 1u << 3,
    CS_RXCLR = 1u << 4,
    CS_TXERR = 1u << 15,
    CS_RXERR = 1u << 16,
    CS_TXD = 1u << 19,
    CS_RXD = 1u << 20,
    CS_SYNC = 1u << 24,
    MODE_FSM = 1u << 21,
    MODE_CLKM = 1u << 23,
};

static uint32_t next_word(uint32_t w, int mode) {
    if (!mode) return w + 1;
    w ^= w << 13;
    w ^= w >> 17;
    w ^= w << 5;
    return w;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint32_t chan(unsigned pos1, unsigned pos2) {
    // 32-bit channels: width extension set, width field 8 (8 + 16 + 8).
    return (1u << 31) | (1u << 30) | (pos1 << 20) | (8u << 16) | (1u << 15) | (1u << 14) |
           (pos2 << 4) | 8u;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: pcmpoll SECONDS MODE [TXPOS]\n");
        return 2;
    }
    double seconds = atof(argv[1]);
    int mode = atoi(argv[2]);
    unsigned txpos = argc > 3 ? (unsigned)atoi(argv[3]) : 0;

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("/dev/mem");
        return 1;
    }
    volatile uint32_t* pcm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PCM_BASE);
    if (pcm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    pcm[CS] = 0;
    usleep(1000);
    pcm[MODE] = MODE_CLKM | MODE_FSM | (63u << 10) | 32u;
    pcm[RXC] = chan(0, 32);
    pcm[TXC] = chan(txpos, (txpos + 32) % 64);
    pcm[CS] = CS_EN;
    pcm[CS] = CS_EN | CS_TXCLR | CS_RXCLR;
    usleep(1000);
    pcm[CS] = CS_EN | CS_TXERR | CS_RXERR;

    uint32_t tx = 0x1000;
    unsigned preload = 0;
    while ((pcm[CS] & CS_TXD) && preload < 64) {
        pcm[FIFO] = tx;
        tx = next_word(tx, mode);
        preload++;
    }
    pcm[CS] = CS_EN | CS_RXON | CS_TXON;

    uint64_t rx_words = 0, tx_words = preload, breaks = 0, link_errors = 0;
    uint64_t rx_overruns = 0, tx_underruns = 0;
    uint32_t prev = 0, first_got = 0, first_want = 0;
    int seen = 0, settle = 1;
    double t0 = now(), t_end = t0 + seconds, t_settle = t0 + 0.2;
    unsigned spin = 0;

    for (;;) {
        uint32_t cs = pcm[CS];
        int host_gap = 0;
        if (cs & (CS_RXERR | CS_TXERR)) {
            if (cs & CS_RXERR) rx_overruns++;
            if (cs & CS_TXERR) tx_underruns++;
            pcm[CS] = CS_EN | CS_RXON | CS_TXON | (cs & (CS_RXERR | CS_TXERR));
            host_gap = 1;
        }
        while (cs & CS_RXD) {
            uint32_t w = pcm[FIFO];
            if (seen && w != next_word(prev, mode)) {
                breaks++;
                if (!host_gap && !settle) {
                    if (!link_errors) {
                        first_got = w;
                        first_want = next_word(prev, mode);
                    }
                    link_errors++;
                }
            }
            prev = w;
            seen = 1;
            rx_words++;
            cs = pcm[CS];
        }
        while (cs & CS_TXD) {
            pcm[FIFO] = tx;
            tx = next_word(tx, mode);
            tx_words++;
            cs = pcm[CS];
        }
        if ((++spin & 0x3ff) == 0) {
            double t = now();
            if (settle && t > t_settle) {
                // Start-up is over: count from here.
                settle = 0;
                rx_overruns = tx_underruns = breaks = link_errors = 0;
                rx_words = tx_words = 0;
                t0 = t;
            }
            if (t > t_end) break;
        }
    }
    double dt = now() - t0;
    pcm[CS] = 0;

    printf(
        "{\"seconds\": %.3f, \"mode\": %d, \"txpos\": %u, \"rx_words\": %llu, \"tx_words\": %llu, "
        "\"rx_words_per_s\": %.0f, \"bit_clock_hz\": %.0f, \"sequence_breaks\": %llu, "
        "\"rx_overruns\": %llu, \"tx_underruns\": %llu, \"link_errors\": %llu, "
        "\"first_error_got\": \"0x%08x\", \"first_error_want\": \"0x%08x\", \"last_word\": "
        "\"0x%08x\"}\n",
        dt, mode, txpos, (unsigned long long)rx_words, (unsigned long long)tx_words, rx_words / dt,
        rx_words / dt * 32, (unsigned long long)breaks, (unsigned long long)rx_overruns,
        (unsigned long long)tx_underruns, (unsigned long long)link_errors, first_got, first_want,
        prev);
    return 0;
}
