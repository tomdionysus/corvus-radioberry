/* SPDX-License-Identifier: GPL-2.0-or-later WITH Linux-syscall-note */
/*
 * The interface between radioberry.ko and userspace: /dev/radioberry.
 *
 * read()  returns whole RX frames. A frame is one sample period for every
 *         active receiver: I0 Q0 I1 Q1 ... each a 24-bit big-endian two's
 *         complement value, so 6 bytes per receiver. RX moves only while
 *         the last command sent had RADIOBERRY_CMD_RUN set (from release
 *         0.4.0): with run clear, as after open, the RX bus and the driver's
 *         streaming are idle and nothing arrives. A read shorter than
 *         one frame fails with EINVAL; otherwise it returns as many whole
 *         frames as fit. Blocks unless O_NONBLOCK. poll() reports POLLIN
 *         when at least one frame is queued and POLLERR after an RX
 *         overrun until RADIOBERRY_IOC_STATS is called.
 * write() accepts whole TX samples of 4 bytes: I then Q, each a 16-bit
 *         big-endian two's complement value, at the gateware's 48 ksps.
 *         Partial samples are rejected with EINVAL. Blocks when the TX
 *         ring is full unless O_NONBLOCK.
 * ioctl() see below. The device is exclusive: a second open fails with
 *         EBUSY. Closing it sends the gateware a command that clears run,
 *         PTT, the TX permission bit, PA enable and drive.
 */
#ifndef RADIOBERRY_IOCTL_H
#define RADIOBERRY_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define RADIOBERRY_ABI_VERSION 1

/* Byte 0 of a command: flags outside the Hermes-Lite 2 C0..C4 block. */
#define RADIOBERRY_CMD_RUN (1u << 0)
#define RADIOBERRY_CMD_CWX (1u << 1)
#define RADIOBERRY_CMD_TX_PERMIT (1u << 2)
/*
 * The PA board may be keyed (its T/R relay, bias and enable lines). Only
 * gateware-v2 images act on it; the PIO gateware ignores this bit.
 */
#define RADIOBERRY_CMD_PA_ALLOWED (1u << 3)

/* Byte 1 of a command is Hermes-Lite 2 C0: address << 1 | PTT. */
#define RADIOBERRY_CMD_PTT (1u << 0)

#define RADIOBERRY_FPGA_CL016 1
#define RADIOBERRY_FPGA_CL025 2

struct radioberry_info {
	__u32 abi_version;    /* RADIOBERRY_ABI_VERSION */
	__u32 driver_version; /* major << 16 | minor << 8 | patch */
	__u8 gateware_major;
	__u8 gateware_minor;
	__u8 fpga_type;       /* RADIOBERRY_FPGA_* */
	__u8 receivers;       /* receivers built into the gateware */
	__u8 transmitters;
	__u8 active_receivers; /* receivers in the current RX frame */
	__u8 rx_rate_code;     /* 0..3 = 48, 96, 192, 384 ksps */
	__u8 transport;        /* RADIOBERRY_TRANSPORT_*; 0 up to release 0.3.0 */
	/* RX values per second the streams carry; 0 up to release 0.3.0,
	 * whose driver carries 2 370 000.
	 */
	__u32 rx_values_max;
	__u32 reserved[3];
};

#define RADIOBERRY_TRANSPORT_PIO 1 /* Raspberry Pi 5: RP1 PIO state machines */
#define RADIOBERRY_TRANSPORT_CPU 2 /* Raspberry Pi 4: clocked by a kernel thread */
#define RADIOBERRY_TRANSPORT_I2S 4 /* Raspberry Pi 4: I2S into the PCM block (gateware-v2) */

/*
 * One gateware command. tx[0] holds RADIOBERRY_CMD_* flags, tx[1..5] the
 * Hermes-Lite 2 C0..C4 bytes. On return rx[] holds the gateware's reply
 * word (see gateware/README.md section 4).
 */
struct radioberry_command {
	__u8 tx[6];
	__u8 rx[6];
	__u32 reserved;
};

struct radioberry_stats {
	__u64 rx_words;          /* words read from the RX state machine */
	__u64 rx_frames;         /* frames queued for read() */
	__u64 rx_resyncs;        /* metadata sequence breaks */
	__u64 rx_dropped_words;  /* words discarded while resynchronising */
	__u64 rx_overrun_frames; /* frames dropped because the ring was full */
	__u64 tx_words;          /* words handed to the TX state machine */
	__u64 tx_underruns;      /* TX ring ran empty while PTT was set */
	__u64 commands;          /* commands sent */
	__u32 rx_ring_frames;    /* frames waiting now */
	__u32 tx_ring_words;     /* words waiting now */
	__u32 rx_period_bytes;   /* current RX DMA period */
	__u32 flags;             /* RADIOBERRY_STATS_* */
	__u64 rx_discarded_words; /* old-layout words after a receiver-count change */
	__u32 reserved[2];
};

#define RADIOBERRY_STATS_RX_OVERRUN (1u << 0) /* since the last STATS call */
#define RADIOBERRY_STATS_PTT (1u << 1)        /* last command had PTT set */

#define RADIOBERRY_IOC_MAGIC 0xb5
#define RADIOBERRY_IOC_INFO _IOR(RADIOBERRY_IOC_MAGIC, 0, struct radioberry_info)
#define RADIOBERRY_IOC_COMMAND _IOWR(RADIOBERRY_IOC_MAGIC, 1, struct radioberry_command)
#define RADIOBERRY_IOC_STATS _IOR(RADIOBERRY_IOC_MAGIC, 2, struct radioberry_stats)

#endif /* RADIOBERRY_IOCTL_H */
