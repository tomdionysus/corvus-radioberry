// SPDX-License-Identifier: GPL-2.0-or-later
//
// Polled capture from the BCM2711 PCM block as clock and frame slave (two
// 32-bit channels in a 64-bit frame), for checking the rpi4-i2s image at a
// low bit clock. Writes N valid words (bit 31 set) to stdout as hex and
// reports idle words and FIFO errors on stderr. Before: pinctrl set 18-21 a0.
//
//   pcmcap N
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define PCM_BASE 0xfe203000u
enum { CS = 0, FIFO = 1, MODE = 2, RXC = 3, TXC = 4 };

int main(int argc, char** argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000;
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

    uint32_t ch = (1u << 31) | (1u << 30) | (0u << 20) | (8u << 16) | (1u << 15) | (1u << 14) |
                  (32u << 4) | 8u;
    pcm[CS] = 0;
    usleep(1000);
    pcm[MODE] = (1u << 23) | (1u << 21) | (63u << 10) | 32u;
    pcm[RXC] = ch;
    pcm[TXC] = ch;
    pcm[CS] = 1;
    pcm[CS] = 1 | (1u << 3) | (1u << 4);
    usleep(1000);
    pcm[CS] = 1 | (1u << 15) | (1u << 16);
    pcm[CS] = 1 | (1u << 1);

    long got = 0, idle = 0, errs = 0;
    while (got < n) {
        uint32_t cs = pcm[CS];
        if (cs & (1u << 16)) {
            errs++;
            pcm[CS] = 1 | (1u << 1) | (1u << 16);
        }
        while (pcm[CS] & (1u << 20)) {
            uint32_t w = pcm[FIFO];
            if (w & 0x80000000u) {
                if (got < n) printf("%08x\n", w);
                got++;
            } else
                idle++;
        }
    }
    pcm[CS] = 0;
    fprintf(stderr, "%ld valid, %ld idle, %ld fifo errors\n", got, idle, errs);
    return 0;
}
