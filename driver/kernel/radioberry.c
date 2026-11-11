// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RadioBerry 2.x on the Raspberry Pi 5 and Raspberry Pi 4.
 *
 * Binds to the "sdr,radioberry" device on spi0 created by the radioberry
 * overlay. On probe it loads the gateware through passive serial, reads
 * the gateware identity over SPI, starts the RX and TX streams, and creates
 * /dev/radioberry (see include/radioberry_ioctl.h for the interface). RX
 * moves only while the last command sent had run set; otherwise its clock
 * stands still and the bus and the stream thread are idle.
 * Writing 1 to the sysfs attribute "reload" while the device is closed
 * stops the streams, reloads the gateware, re-reads its identity and
 * restarts the streams.
 *
 * The streams have two transports, chosen at probe from the SoC; the rest
 * of the driver is shared:
 *
 * - Pi 5 (BCM2712): two RP1 PIO state machines, built only on the stock
 *   rp1-pio kernel API. Stream DMA uses rp1-pio's bounce buffers: up to
 *   RB_DMA_SLOTS periods are in flight per direction and a FIFO-priority
 *   kernel thread refills them, so the state machines never wait for a
 *   restart.
 * - Pi 4 (BCM2711), which has no PIO: a FIFO-priority kernel thread
 *   clocks both buses through the GPIO block's set, clear and level
 *   registers with the same handshakes (rb_bitbang.h), at a lower rate.
 *
 * The PIO programs and the overall design follow the original RadioBerry
 * Pi 5 driver, released into the public domain under the Unlicense.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pio_rp1.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "radioberry_ioctl.h"
#include "rb_logic.h"
#include "rb_pio.h"

/* BCM2711 GPIO block registers for the CPU-clocked buses. */
#define RB_BCM2711_GPSET0 0x1c
#define RB_BCM2711_GPCLR0 0x28
#define RB_BCM2711_GPLEV0 0x34

struct rb_bb_io {
	void __iomem *base;
};

static inline void rb_bb_set(struct rb_bb_io *io, u32 mask)
{
	writel_relaxed(mask, io->base + RB_BCM2711_GPSET0);
}

static inline void rb_bb_clr(struct rb_bb_io *io, u32 mask)
{
	writel_relaxed(mask, io->base + RB_BCM2711_GPCLR0);
}

static inline u32 rb_bb_lev(struct rb_bb_io *io)
{
	return readl_relaxed(io->base + RB_BCM2711_GPLEV0);
}

#include "rb_bitbang.h"

#define RB_DRIVER_VERSION ((0u << 16) | (5u << 8) | 0u)
#define RB_DRIVER_VERSION_STRING "0.5.0"

#define RB_FIRMWARE_NAME "radioberry.rbf"
#define RB_DMA_SLOTS 4
#define RB_RX_SLOT_BYTES (32 * 1024)
#define RB_RX_MIN_PERIOD 256
#define RB_TX_SLOT_BYTES 1024 /* 256 samples, 5.3 ms at 48 ksps */
#define RB_CHUNK_BYTES 4096

static bool load_gateware = true;
module_param(load_gateware, bool, 0444);
MODULE_PARM_DESC(load_gateware, "Load " RB_FIRMWARE_NAME " into the FPGA on probe (default 1)");

static unsigned int rx_clkdiv = RB_RX_CLKDIV_DEFAULT;
module_param(rx_clkdiv, uint, 0444);
MODULE_PARM_DESC(rx_clkdiv, "RX state machine clock divider (default 2)");

static unsigned int tx_clkdiv = RB_TX_CLKDIV_DEFAULT;
module_param(tx_clkdiv, uint, 0444);
MODULE_PARM_DESC(tx_clkdiv, "TX state machine clock divider (default 8)");

static unsigned int rx_ring_kib = 512;
module_param(rx_ring_kib, uint, 0444);
MODULE_PARM_DESC(rx_ring_kib, "RX ring size in KiB, rounded up to a power of two (default 512)");

static unsigned int tx_ring_kib = 64;
module_param(tx_ring_kib, uint, 0444);
MODULE_PARM_DESC(tx_ring_kib, "TX ring size in KiB, rounded up to a power of two (default 64)");

/*
 * Pi 4 (CPU-clocked buses) only. The RX limit is what the daemon lets
 * clients ask for: two receivers at 48 ksps or one at 96. Above it the
 * thread takes a whole core (docs/testing.md).
 */
#define RB_BB_RX_VALUES_MAX_DEFAULT 192000
static unsigned int bb_rx_values_max = RB_BB_RX_VALUES_MAX_DEFAULT;
module_param(bb_rx_values_max, uint, 0444);
MODULE_PARM_DESC(bb_rx_values_max,
		 "Pi 4: RX values per second offered to userspace (default 192000)");

static unsigned int bb_settle_reads;
module_param(bb_settle_reads, uint, 0444);
MODULE_PARM_DESC(bb_settle_reads, "Pi 4: extra pin reads after each bus clock pulse (default 0)");

#define RB_BB_RX_BATCH 256     /* words per RX run, at most 2048 clocks */
#define RB_BB_RX_MAX_IDLE 64   /* idle clocks before the thread rests */
#define RB_BB_TX_MAX_POLLS 64  /* polling edges per word before giving up */
#define RB_BB_IDLE_MIN_US 500  /* rest with nothing to move: the gateware's */
#define RB_BB_IDLE_MAX_US 1000 /* RX FIFO lasts 10 ms at 96 000 values/s */

/* The PIO bus's RX ceiling, measured (gateware/README.md section 5). */
#define RB_PIO_RX_VALUES_MAX 2370000

static unsigned int debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "0: quiet; 1: rate-limited resyncs, overruns, underruns, commands; "
			"2: every command and receiver-count change, unlimited");

#define rb_debug(rb, fmt, ...)                                              \
	do {                                                                \
		if (READ_ONCE(debug))                                       \
			dev_info_ratelimited((rb)->dev, fmt, ##__VA_ARGS__); \
	} while (0)

#define rb_trace(rb, fmt, ...)                                   \
	do {                                                     \
		if (READ_ONCE(debug) >= 2)                       \
			dev_info((rb)->dev, fmt, ##__VA_ARGS__); \
	} while (0)

/* RX words must arrive within this time while run is set, or the stream is stalled. */
#define RB_STALL_MS 200

/*
 * Workaround for the PIO gateware's receive lock-up, docs/fault-rx-lockup.md.
 * The gateware clears its RX FIFO asynchronously at every receiver-count
 * change, and the FIFO's read side and the state machine that streams it to
 * the Pi run on the clock this driver's RX state machine drives. If that
 * clock has an edge while the clear is asserted or released, the read-out
 * can lock up for good (only a gateware reload recovers). With the RX state
 * machine stopped across the command the clock stands still and the window
 * cannot be hit: measured 0 lock-ups in 4500 changes against 17 in 1800
 * without the hold. 0 disables the hold (for measuring the fault).
 */
#define RB_RX_CLOCK_HOLD_US_DEFAULT 200
static unsigned int rx_clock_hold_us = RB_RX_CLOCK_HOLD_US_DEFAULT;
module_param(rx_clock_hold_us, uint, 0644);
MODULE_PARM_DESC(rx_clock_hold_us,
		 "Stop the RX state machine, so the Pi's RX clock is still, from before a receiver-count command until this many microseconds after it (default 200; 0: never, exposes the gateware's RX lock-up)");

struct rb_stream {
	int sm;
	int prog_offset;
	bool prog_loaded;
	u8 *slot[RB_DMA_SLOTS];
	u32 slot_bytes[RB_DMA_SLOTS];
	unsigned int submitted; /* free running slot counters */
	unsigned int completed;
	atomic_t inflight;
};

struct rb_dev;

/* How the RX and TX streams reach the gateware. */
struct rb_transport {
	const char *name;
	u32 rx_values_max; /* 0: bb_rx_values_max */
	/* Starts both streams and their kernel thread; undoes itself on failure. */
	int (*up)(struct rb_dev *rb);
	/* Stops everything up() started. Safe to call when it is not running. */
	void (*down)(struct rb_dev *rb);
	/* Stops the RX clock (true) until rx_release(); false if not running. */
	bool (*rx_hold)(struct rb_dev *rb);
	void (*rx_release)(struct rb_dev *rb);
	/*
	 * Stops RX after rx_paused is set, returning once no RX is clocked;
	 * restarts it after rx_paused is cleared. Safe while not running.
	 */
	void (*rx_pause)(struct rb_dev *rb);
	void (*rx_resume)(struct rb_dev *rb);
};

struct rb_dev {
	struct spi_device *spi;
	struct device *dev;
	const struct rb_transport *tp;
	struct gpio_desc *nconfig, *dclk, *data, *conf_done, *nstatus;
	struct rb_identity id;

	struct mutex cmd_lock; /* SPI commands */
	u8 *spi_tx, *spi_rx;   /* DMA-safe command buffers */
	bool ptt;
	bool run; /* run bit of the last command sent */
	/*
	 * RX stopped because run is clear (written under cmd_lock): the RX
	 * clock stands still, so the bus and the thread are idle while nothing
	 * streams (rb_rx_gate()).
	 */
	bool rx_paused;

	/* Stall detection, worker thread only. */
	u64 rx_words_seen;
	unsigned long rx_last_progress;
	bool rx_stalled;
	u64 rx_stalls;

	struct rp1_pio_client *pio;
	struct rb_stream rx, tx;

	/* Pi 4: CPU-clocked buses. bb_lock is held while the thread clocks. */
	struct rb_bb_io bbio;
	struct rb_bb_bus bb;
	struct mutex bb_lock;
	struct gpio_desc *bb_rx_clk, *bb_tx_clk, *bb_tx_data, *bb_rx_ready, *bb_tx_ready;
	struct gpio_descs *bb_rx_data;
	u32 *bb_rx_words;
	u32 *bb_tx_words;
	unsigned int bb_tx_count, bb_tx_pos; /* words in bb_tx_words, words sent */

	spinlock_t rx_lock; /* parser, rx ring, rx statistics */
	struct rb_rx_parser parser;
	struct rb_ring rx_ring;
	u8 *rx_ring_buf;
	u32 rx_period;
	unsigned int receivers;
	unsigned int rate_code;
	bool rx_overrun;
	wait_queue_head_t rx_wait;

	spinlock_t tx_lock; /* tx ring, tx statistics */
	struct rb_ring tx_ring;
	u8 *tx_ring_buf;
	bool tx_starved;
	wait_queue_head_t tx_wait;

	struct mutex io_lock; /* read and write bounce buffers */
	u8 *io_buf;
	u32 *pack_buf;

	struct task_struct *worker;
	wait_queue_head_t work_wait;

	atomic_t open;    /* held by an open file or a reload */
	bool running;     /* streams up; open() refused otherwise */
	struct miscdevice misc;
	struct radioberry_stats stats;
};

/* ---- SPI control ---------------------------------------------------- */

static int rb_command_locked(struct rb_dev *rb, const u8 *tx, u8 *rx)
{
	struct spi_transfer t = {
		.tx_buf = rb->spi_tx,
		.rx_buf = rb->spi_rx,
		.len = RB_CMD_BYTES,
	};
	int ret;

	lockdep_assert_held(&rb->cmd_lock);
	memcpy(rb->spi_tx, tx, RB_CMD_BYTES);
	ret = spi_sync_transfer(rb->spi, &t, 1);
	if (ret)
		return ret;
	if (rx)
		memcpy(rx, rb->spi_rx, RB_CMD_BYTES);
	rb->ptt = rb_cmd_ptt(tx);
	WRITE_ONCE(rb->run, (tx[0] & RADIOBERRY_CMD_RUN) != 0);
	return 0;
}

/*
 * RX moves only while run is set. The gateware streams RX whatever run
 * says, so with run clear (closed, or open with no client streaming) the
 * driver stops the RX clock and the bus and the thread go idle. On resume
 * the parser settles, as after a receiver-count change, since the words
 * that follow the pause need not continue the frame in progress, and frames
 * queued before the pause are dropped. settle_us: the command that set run
 * also changed the receiver count, so the clock stays still that long for
 * the gateware's RX FIFO clear (rx_clock_hold_us).
 */
static void rb_rx_gate(struct rb_dev *rb, unsigned int settle_us)
{
	unsigned long flags;

	lockdep_assert_held(&rb->cmd_lock);
	if (!READ_ONCE(rb->run) && !rb->rx_paused) {
		WRITE_ONCE(rb->rx_paused, true);
		rb->tp->rx_pause(rb);
		rb_trace(rb, "RX paused: run clear\n");
	} else if (READ_ONCE(rb->run) && rb->rx_paused) {
		if (settle_us)
			usleep_range(settle_us, settle_us + settle_us / 4);
		spin_lock_irqsave(&rb->rx_lock, flags);
		rb_rx_set_receivers(&rb->parser, rb->receivers);
		rb_ring_reset(&rb->rx_ring);
		rb->rx_overrun = false;
		spin_unlock_irqrestore(&rb->rx_lock, flags);
		WRITE_ONCE(rb->rx_paused, false);
		rb->tp->rx_resume(rb);
		wake_up(&rb->work_wait);
		rb_trace(rb, "RX resumed: run set\n");
	}
}

static int rb_command_safe(struct rb_dev *rb, u8 *rx)
{
	u8 tx[RB_CMD_BYTES];
	int ret;

	rb_cmd_safe(tx);
	mutex_lock(&rb->cmd_lock);
	ret = rb_command_locked(rb, tx, rx);
	if (!ret)
		rb_rx_gate(rb, 0);
	mutex_unlock(&rb->cmd_lock);
	return ret;
}

/* The FPGA the gateware reports, by name. */
static const char *rb_fpga_name(u8 fpga_type)
{
	switch (fpga_type) {
	case RADIOBERRY_FPGA_CL025:
		return "10CL025";
	case RADIOBERRY_FPGA_CL016:
		return "10CL016";
	default:
		return "unknown";
	}
}

/* Polls the identity word until the gateware has left its reset. */
static int rb_read_identity(struct rb_dev *rb)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(4000);
	u8 rx[RB_CMD_BYTES];
	int ret;

	do {
		ret = rb_command_safe(rb, rx);
		if (ret)
			return ret;
		rb_reply_parse(rx, &rb->id);
		if (rb_identity_valid(&rb->id))
			return 0;
		msleep(100);
	} while (time_before(jiffies, deadline));

	dev_err(rb->dev, "gateware identity not valid (reply %6ph); is the FPGA configured?\n", rx);
	return -ETIMEDOUT;
}

/* ---- gateware ------------------------------------------------------- */

static int rb_wait_nstatus(struct rb_dev *rb, int level, unsigned int timeout_us)
{
	unsigned int waited;

	for (waited = 0; waited < timeout_us; waited += 10) {
		if (gpiod_get_value_cansleep(rb->nstatus) == level)
			return 0;
		usleep_range(10, 20);
	}
	return -ETIMEDOUT;
}

static int rb_configure_fpga(struct rb_dev *rb, const u8 *image, size_t size)
{
	size_t i;
	int bit;

	gpiod_set_value_cansleep(rb->dclk, 0);
	gpiod_set_value_cansleep(rb->data, 0);
	gpiod_set_value_cansleep(rb->nconfig, 0);
	udelay(5);
	if (rb_wait_nstatus(rb, 0, 1000)) {
		gpiod_set_value_cansleep(rb->nconfig, 1);
		dev_err(rb->dev, "nSTATUS stayed high while nCONFIG was low\n");
		return -EIO;
	}
	gpiod_set_value_cansleep(rb->nconfig, 1);
	if (rb_wait_nstatus(rb, 1, 10000)) {
		dev_err(rb->dev, "nSTATUS stayed low after nCONFIG was released\n");
		return -EIO;
	}
	udelay(5);

	for (i = 0; i < size; i++) {
		for (bit = 0; bit < 8; bit++) {
			gpiod_set_value_cansleep(rb->data, (image[i] >> bit) & 1);
			gpiod_set_value_cansleep(rb->dclk, 1);
			gpiod_set_value_cansleep(rb->dclk, 0);
		}
		if ((i & 0xfff) == 0xfff)
			cond_resched();
	}
	gpiod_set_value_cansleep(rb->data, 0);

	if (!gpiod_get_value_cansleep(rb->nstatus)) {
		dev_err(rb->dev, "nSTATUS went low: the FPGA rejected the gateware\n");
		return -EIO;
	}
	if (!gpiod_get_value_cansleep(rb->conf_done)) {
		dev_err(rb->dev, "CONF_DONE stayed low: %s does not match this FPGA\n",
			RB_FIRMWARE_NAME);
		return -EIO;
	}
	for (i = 0; i < 64; i++) {
		gpiod_set_value_cansleep(rb->dclk, 1);
		gpiod_set_value_cansleep(rb->dclk, 0);
	}
	return 0;
}

static int rb_load_gateware(struct rb_dev *rb)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, RB_FIRMWARE_NAME, rb->dev);
	if (ret) {
		dev_err(rb->dev, "cannot load /lib/firmware/%s: %d\n", RB_FIRMWARE_NAME, ret);
		return ret;
	}
	ret = rb_configure_fpga(rb, fw->data, fw->size);
	if (!ret)
		dev_info(rb->dev, "gateware %s configured (%zu bytes)\n", RB_FIRMWARE_NAME,
			 fw->size);
	release_firmware(fw);
	if (ret)
		return ret;

	/* The gateware holds itself in reset for about 1.7 s after loading. */
	msleep(1800);
	return 0;
}

/* ---- streams -------------------------------------------------------- */

/*
 * Feeds RX words (index << 24 | value) from either transport to the parser
 * and queues completed frames. Returns true if a reader should be woken.
 */
static bool rb_rx_consume(struct rb_dev *rb, const u32 *words, unsigned int n)
{
	unsigned int frame_bytes;
	unsigned long flags;
	bool queued = false;
	u64 resyncs, dropped, overruns = 0;
	unsigned int i;

	spin_lock_irqsave(&rb->rx_lock, flags);
	resyncs = rb->parser.resyncs;
	dropped = rb->parser.dropped;
	frame_bytes = rb_rx_frame_bytes(&rb->parser);
	for (i = 0; i < n; i++) {
		/* Parse while closed to stay in sync, but queue nothing. */
		if (!rb_rx_feed(&rb->parser, words[i]) || !atomic_read(&rb->open))
			continue;
		if (rb_ring_put(&rb->rx_ring, rb->parser.frame, frame_bytes)) {
			rb->stats.rx_frames++;
			queued = true;
		} else {
			rb->stats.rx_overrun_frames++;
			rb->rx_overrun = true;
			overruns++;
		}
	}
	rb->stats.rx_words += n;
	resyncs = rb->parser.resyncs - resyncs;
	dropped = rb->parser.dropped - dropped;
	spin_unlock_irqrestore(&rb->rx_lock, flags);

	if (resyncs || dropped)
		rb_debug(rb, "RX period of %u words: %llu resyncs, %llu words dropped\n", n,
			 resyncs, dropped);
	if (overruns)
		rb_debug(rb, "RX ring full: %llu frames dropped\n", overruns);
	return queued || rb->rx_overrun;
}

static void rb_rx_dma_done(void *param)
{
	struct rb_dev *rb = param;
	struct rb_stream *s = &rb->rx;
	unsigned int idx = s->completed++ % RB_DMA_SLOTS;
	const u32 *words = (const u32 *)s->slot[idx];
	unsigned int n = s->slot_bytes[idx] / 4;
	bool wake = rb_rx_consume(rb, words, n);

	atomic_dec(&s->inflight);
	if (wake)
		wake_up_interruptible(&rb->rx_wait);
	wake_up(&rb->work_wait);
}

static void rb_tx_dma_done(void *param)
{
	struct rb_dev *rb = param;

	rb->tx.completed++;
	atomic_dec(&rb->tx.inflight);
	wake_up(&rb->work_wait);
}

static int rb_rx_submit(struct rb_dev *rb)
{
	struct rb_stream *s = &rb->rx;
	unsigned int idx;
	int ret;

	while (atomic_read(&s->inflight) < RB_DMA_SLOTS) {
		idx = s->submitted % RB_DMA_SLOTS;
		s->slot_bytes[idx] = READ_ONCE(rb->rx_period);
		atomic_inc(&s->inflight);
		ret = rp1_pio_sm_xfer_data(rb->pio, s->sm, RP1_PIO_DIR_FROM_SM, s->slot_bytes[idx],
					   s->slot[idx], 0, rb_rx_dma_done, rb);
		if (ret) {
			atomic_dec(&s->inflight);
			if (ret == -EBUSY)
				return 0;
			dev_err_ratelimited(rb->dev, "RX DMA submit failed: %d\n", ret);
			return ret;
		}
		s->submitted++;
	}
	return 0;
}

static int rb_tx_submit(struct rb_dev *rb)
{
	struct rb_stream *s = &rb->tx;
	unsigned long flags;
	unsigned int idx;
	bool underrun;
	u32 bytes;
	int ret;

	while (atomic_read(&s->inflight) < RB_DMA_SLOTS) {
		idx = s->submitted % RB_DMA_SLOTS;
		spin_lock_irqsave(&rb->tx_lock, flags);
		bytes = rb_ring_get(&rb->tx_ring, s->slot[idx], RB_TX_SLOT_BYTES);
		underrun = !bytes && rb->ptt && !atomic_read(&s->inflight) && !rb->tx_starved;
		if (underrun) {
			rb->stats.tx_underruns++;
			rb->tx_starved = true;
		}
		if (bytes)
			rb->tx_starved = false;
		spin_unlock_irqrestore(&rb->tx_lock, flags);
		if (underrun)
			rb_debug(rb, "TX underrun with PTT set\n");
		if (!bytes)
			return 0;
		wake_up_interruptible(&rb->tx_wait);

		atomic_inc(&s->inflight);
		ret = rp1_pio_sm_xfer_data(rb->pio, s->sm, RP1_PIO_DIR_TO_SM, bytes, s->slot[idx],
					   0, rb_tx_dma_done, rb);
		if (ret) {
			atomic_dec(&s->inflight);
			dev_err_ratelimited(rb->dev, "TX DMA submit failed: %d; %u bytes dropped\n",
					    ret, bytes);
			return ret;
		}
		s->submitted++;
		spin_lock_irqsave(&rb->tx_lock, flags);
		rb->stats.tx_words += bytes / 4;
		spin_unlock_irqrestore(&rb->tx_lock, flags);
	}
	return 0;
}

static bool rb_work_pending(struct rb_dev *rb)
{
	unsigned long flags;
	bool tx_data;

	if (kthread_should_stop() || atomic_read(&rb->rx.inflight) < RB_DMA_SLOTS)
		return true;
	if (atomic_read(&rb->tx.inflight) >= RB_DMA_SLOTS)
		return false;
	spin_lock_irqsave(&rb->tx_lock, flags);
	tx_data = rb_ring_used(&rb->tx_ring) != 0;
	spin_unlock_irqrestore(&rb->tx_lock, flags);
	return tx_data;
}

/*
 * The gateware has been seen to stop delivering RX words for good until
 * it is reloaded. Notes the first RB_STALL_MS without words while run is
 * set, and the resumption, so the condition is visible in the log and in
 * sysfs (rx_stalled, rx_stalls).
 */
static void rb_check_stall(struct rb_dev *rb)
{
	unsigned long flags;
	u64 words;

	spin_lock_irqsave(&rb->rx_lock, flags);
	words = rb->stats.rx_words;
	spin_unlock_irqrestore(&rb->rx_lock, flags);

	if (words != rb->rx_words_seen) {
		rb->rx_words_seen = words;
		rb->rx_last_progress = jiffies;
		if (rb->rx_stalled) {
			rb->rx_stalled = false;
			dev_warn(rb->dev, "RX stream resumed\n");
		}
		return;
	}
	if (!READ_ONCE(rb->run)) {
		rb->rx_last_progress = jiffies;
		return;
	}
	if (!rb->rx_stalled &&
	    time_after(jiffies, rb->rx_last_progress + msecs_to_jiffies(RB_STALL_MS))) {
		rb->rx_stalled = true;
		rb->rx_stalls++;
		dev_warn(rb->dev,
			 "RX stream stalled: no words for %u ms with run set (receivers %u, rate code %u, %d RX periods in flight)\n",
			 RB_STALL_MS, rb->receivers, rb->rate_code, atomic_read(&rb->rx.inflight));
	}
}

static int rb_worker(void *arg)
{
	struct rb_dev *rb = arg;

	while (!kthread_should_stop()) {
		wait_event_interruptible_timeout(rb->work_wait, rb_work_pending(rb),
						 msecs_to_jiffies(100));
		rb_check_stall(rb);
		/* Back off on a submit error rather than spin on it. */
		if (rb_rx_submit(rb) || rb_tx_submit(rb))
			usleep_range(10000, 11000);
	}
	return 0;
}

static int rb_setup_pins(struct rb_dev *rb, int sm, const unsigned int *pins,
			 const bool *outputs, unsigned int count)
{
	struct rp1_pio_sm_set_pindirs_args dirs = { .sm = (u16)sm };
	struct rp1_pio_sm_set_pins_args low = { .sm = (u16)sm };
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		struct rp1_gpio_set_function_args fn = {
			.gpio = (u16)pins[i], .fn = RP1_GPIO_FUNC_PIO,
		};
		struct rp1_gpio_set_pulls_args pulls = {
			.gpio = (u16)pins[i], .up = 0, .down = outputs[i] ? 0 : 1,
		};

		ret = rp1_pio_gpio_set_function(rb->pio, &fn);
		if (ret < 0)
			return ret;
		ret = rp1_pio_gpio_set_pulls(rb->pio, &pulls);
		if (ret < 0)
			return ret;
		dirs.mask |= BIT(pins[i]);
		if (outputs[i]) {
			dirs.dirs |= BIT(pins[i]);
			low.mask |= BIT(pins[i]);
		}
	}
	ret = rp1_pio_sm_set_pindirs(rb->pio, &dirs);
	if (ret < 0)
		return ret;
	return rp1_pio_sm_set_pins(rb->pio, &low);
}

static int rb_setup_stream(struct rb_dev *rb, struct rb_stream *s, bool tx)
{
	static const unsigned int rx_pins[] = { RB_GPIO_RX_CLK, RB_GPIO_RX_READY, RB_GPIO_RX_D0,
						RB_GPIO_RX_D0 + 1, RB_GPIO_RX_D0 + 2,
						RB_GPIO_RX_D0 + 3 };
	static const bool rx_out[] = { true, false, false, false, false, false };
	static const unsigned int tx_pins[] = { RB_GPIO_TX_CLK, RB_GPIO_TX_DATA, RB_GPIO_TX_READY };
	static const bool tx_out[] = { true, true, false };
	const struct rb_pio_program *prog = tx ? &rb_tx_program : &rb_rx_program;
	unsigned int dir = tx ? RP1_PIO_DIR_TO_SM : RP1_PIO_DIR_FROM_SM;
	struct rp1_pio_sm_claim_args claim = { .mask = 0 };
	struct rp1_pio_add_program_args add = { .num_instrs = prog->length,
						.origin = RP1_PIO_ORIGIN_ANY };
	struct rp1_pio_sm_set_enabled_args off = { 0 };
	struct rp1_pio_sm_clear_fifos_args clear = { 0 };
	struct rp1_pio_sm_init_args init = { 0 };
	struct rb_sm_config cfg;
	unsigned int i;
	int ret;

	s->sm = rp1_pio_sm_claim(rb->pio, &claim);
	if (s->sm < 0) {
		dev_err(rb->dev, "no free PIO state machine for %s: %d\n", tx ? "TX" : "RX", s->sm);
		return s->sm;
	}
	off.mask = BIT(s->sm);
	rp1_pio_sm_set_enabled(rb->pio, &off);

	for (i = 0; i < RB_DMA_SLOTS; i++) {
		s->slot[i] = kzalloc(tx ? RB_TX_SLOT_BYTES : RB_RX_SLOT_BYTES, GFP_KERNEL);
		if (!s->slot[i])
			return -ENOMEM;
	}
	ret = rp1_pio_sm_config_xfer(rb->pio, s->sm, dir, tx ? RB_TX_SLOT_BYTES : RB_RX_SLOT_BYTES,
				     RB_DMA_SLOTS);
	if (ret < 0) {
		dev_err(rb->dev, "%s DMA configuration failed: %d\n", tx ? "TX" : "RX", ret);
		return ret;
	}

	memcpy(add.instrs, prog->instructions, prog->length * sizeof(u16));
	ret = rp1_pio_add_program(rb->pio, &add);
	if (ret < 0) {
		dev_err(rb->dev, "cannot load the %s PIO program: %d\n", tx ? "TX" : "RX", ret);
		return ret;
	}
	s->prog_offset = ret;
	s->prog_loaded = true;

	ret = tx ? rb_setup_pins(rb, s->sm, tx_pins, tx_out, ARRAY_SIZE(tx_pins))
		 : rb_setup_pins(rb, s->sm, rx_pins, rx_out, ARRAY_SIZE(rx_pins));
	if (ret < 0) {
		dev_err(rb->dev, "cannot hand the %s pins to PIO: %d\n", tx ? "TX" : "RX", ret);
		return ret;
	}

	cfg = tx ? rb_tx_sm_config(s->prog_offset, tx_clkdiv)
		 : rb_rx_sm_config(s->prog_offset, rx_clkdiv);
	init.sm = (u16)s->sm;
	init.initial_pc = (u16)s->prog_offset;
	init.config.clkdiv = cfg.clkdiv;
	init.config.execctrl = cfg.execctrl;
	init.config.shiftctrl = cfg.shiftctrl;
	init.config.pinctrl = cfg.pinctrl;
	ret = rp1_pio_sm_init(rb->pio, &init);
	if (ret < 0) {
		dev_err(rb->dev, "cannot configure the %s state machine: %d\n", tx ? "TX" : "RX",
			ret);
		return ret;
	}
	clear.sm = (u16)s->sm;
	rp1_pio_sm_clear_fifos(rb->pio, &clear);
	return 0;
}

/* Starts TX, and RX unless it is paused (rb_rx_gate()), with the thread. */
static int rb_start_streams(struct rb_dev *rb)
{
	struct rp1_pio_sm_set_enabled_args on = {
		.mask = (u16)((rb->rx_paused ? 0 : BIT(rb->rx.sm)) | BIT(rb->tx.sm)),
		.enable = 1,
	};
	int ret;

	ret = rp1_pio_sm_set_enabled(rb->pio, &on);
	if (ret < 0) {
		dev_err(rb->dev, "cannot start the state machines: %d\n", ret);
		return ret;
	}
	rb->worker = kthread_run(rb_worker, rb, "radioberry");
	if (IS_ERR(rb->worker)) {
		ret = PTR_ERR(rb->worker);
		rb->worker = NULL;
		return ret;
	}
	sched_set_fifo(rb->worker);
	return 0;
}

/* State of a stream with no state machine claimed and nothing in flight. */
static void rb_stream_reset(struct rb_stream *s)
{
	s->sm = -1;
	s->prog_offset = 0;
	s->prog_loaded = false;
	s->submitted = 0;
	s->completed = 0;
	atomic_set(&s->inflight, 0);
}

static void rb_pio_down(struct rb_dev *rb)
{
	struct rp1_pio_sm_set_enabled_args off = { 0 };
	unsigned int i;

	if (rb->worker) {
		kthread_stop(rb->worker);
		rb->worker = NULL;
	}
	if (rb->pio) {
		if (rb->rx.sm >= 0)
			off.mask |= BIT(rb->rx.sm);
		if (rb->tx.sm >= 0)
			off.mask |= BIT(rb->tx.sm);
		if (off.mask)
			rp1_pio_sm_set_enabled(rb->pio, &off);
		/*
		 * Unclaims the state machines and frees their DMA. Transfers still in
		 * flight are terminated without their callbacks; rp1-pio then leaks
		 * its 64-byte record for each (docs/kernel-debug.md).
		 */
		rp1_pio_close(rb->pio);
		rb->pio = NULL;
	}
	for (i = 0; i < RB_DMA_SLOTS; i++) {
		kfree(rb->rx.slot[i]);
		kfree(rb->tx.slot[i]);
		rb->rx.slot[i] = NULL;
		rb->tx.slot[i] = NULL;
	}
	rb_stream_reset(&rb->rx);
	rb_stream_reset(&rb->tx);
}

/*
 * Opens the RP1 PIO, claims and configures both state machines and starts
 * them with their kernel thread. Undoes everything on failure.
 */
static int rb_pio_up(struct rb_dev *rb)
{
	int ret;

	rb->pio = rp1_pio_open();
	if (IS_ERR(rb->pio)) {
		ret = PTR_ERR(rb->pio);
		rb->pio = NULL;
		dev_err(rb->dev, "cannot open the RP1 PIO: %d\n", ret);
		return ret;
	}
	ret = rb_setup_stream(rb, &rb->rx, false);
	if (!ret)
		ret = rb_setup_stream(rb, &rb->tx, true);
	if (!ret)
		ret = rb_start_streams(rb);
	if (ret) {
		rb_pio_down(rb);
		return ret;
	}
	dev_info(rb->dev, "RX on PIO state machine %d (program at %d), TX on %d (program at %d)\n",
		 rb->rx.sm, rb->rx.prog_offset, rb->tx.sm, rb->tx.prog_offset);
	return 0;
}

/* Stops the RX state machine, so the RX clock stands still. */
static bool rb_pio_rx_hold(struct rb_dev *rb)
{
	struct rp1_pio_sm_set_enabled_args off = { .mask = (u16)BIT(rb->rx.sm) };

	return rb->pio && rp1_pio_sm_set_enabled(rb->pio, &off) == 0;
}

static void rb_pio_rx_release(struct rb_dev *rb)
{
	struct rp1_pio_sm_set_enabled_args on = { .mask = (u16)BIT(rb->rx.sm), .enable = 1 };

	rp1_pio_sm_set_enabled(rb->pio, &on);
}

/*
 * The RX state machine stops, so the clock stands still and nothing reaches
 * the DMA; the periods in flight wait for it and the thread sleeps.
 */
static void rb_pio_rx_pause(struct rb_dev *rb)
{
	if (rb->pio && rb->rx.sm >= 0)
		rb_pio_rx_hold(rb);
}

static void rb_pio_rx_resume(struct rb_dev *rb)
{
	if (rb->pio && rb->rx.sm >= 0)
		rb_pio_rx_release(rb);
}

static const struct rb_transport rb_pio_transport = {
	.name = "rp1-pio",
	.rx_values_max = RB_PIO_RX_VALUES_MAX,
	.up = rb_pio_up,
	.down = rb_pio_down,
	.rx_hold = rb_pio_rx_hold,
	.rx_release = rb_pio_rx_release,
	.rx_pause = rb_pio_rx_pause,
	.rx_resume = rb_pio_rx_resume,
};

/* ---- CPU-clocked buses (Pi 4) --------------------------------------- */

/* Moves TX words from the ring to the gateware; called with bb_lock held. */
static bool rb_bb_tx(struct rb_dev *rb)
{
	unsigned long flags;
	unsigned int sent;
	bool underrun;
	u32 bytes;

	if (rb->bb_tx_pos == rb->bb_tx_count) {
		spin_lock_irqsave(&rb->tx_lock, flags);
		bytes = rb_ring_get(&rb->tx_ring, (u8 *)rb->bb_tx_words, RB_TX_SLOT_BYTES);
		underrun = !bytes && rb->ptt && !rb->tx_starved;
		if (underrun) {
			rb->stats.tx_underruns++;
			rb->tx_starved = true;
		}
		if (bytes)
			rb->tx_starved = false;
		spin_unlock_irqrestore(&rb->tx_lock, flags);
		if (underrun)
			rb_debug(rb, "TX underrun with PTT set\n");
		if (!bytes)
			return false;
		wake_up_interruptible(&rb->tx_wait);
		rb->bb_tx_count = bytes / 4;
		rb->bb_tx_pos = 0;
	}
	sent = rb_bb_tx_run(&rb->bb, rb->bb_tx_words + rb->bb_tx_pos,
			    rb->bb_tx_count - rb->bb_tx_pos, RB_BB_TX_MAX_POLLS);
	rb->bb_tx_pos += sent;
	spin_lock_irqsave(&rb->tx_lock, flags);
	rb->stats.tx_words += sent;
	spin_unlock_irqrestore(&rb->tx_lock, flags);
	return true;
}

/* While RX is paused: wake for TX words, for resume, or to stop. */
static bool rb_bb_paused_wake(struct rb_dev *rb)
{
	unsigned long flags;
	bool tx_data;

	if (kthread_should_stop() || !READ_ONCE(rb->rx_paused))
		return true;
	spin_lock_irqsave(&rb->tx_lock, flags);
	tx_data = rb_ring_used(&rb->tx_ring) != 0;
	spin_unlock_irqrestore(&rb->tx_lock, flags);
	return tx_data;
}

static int rb_bb_worker(void *arg)
{
	struct rb_dev *rb = arg;
	unsigned int got;
	bool paused, tx_busy;

	while (!kthread_should_stop()) {
		mutex_lock(&rb->bb_lock);
		/* Read under bb_lock: rb_bb_rx_pause() waits for this run. */
		paused = READ_ONCE(rb->rx_paused);
		got = paused ? 0 :
			       rb_bb_rx_run(&rb->bb, rb->bb_rx_words, RB_BB_RX_BATCH,
					    RB_BB_RX_MAX_IDLE);
		tx_busy = rb_bb_tx(rb);
		mutex_unlock(&rb->bb_lock);
		if (got && rb_rx_consume(rb, rb->bb_rx_words, got))
			wake_up_interruptible(&rb->rx_wait);
		rb_check_stall(rb);
		if (got || tx_busy)
			cond_resched();
		else if (paused)
			wait_event_interruptible_timeout(rb->work_wait, rb_bb_paused_wake(rb),
							 msecs_to_jiffies(100));
		else
			usleep_range(RB_BB_IDLE_MIN_US, RB_BB_IDLE_MAX_US);
	}
	return 0;
}

static void rb_bb_put_gpios(struct rb_dev *rb)
{
	struct gpio_desc **out[] = { &rb->bb_rx_clk, &rb->bb_tx_clk, &rb->bb_tx_data,
				     &rb->bb_rx_ready, &rb->bb_tx_ready };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(out); i++) {
		if (!IS_ERR_OR_NULL(*out[i]))
			gpiod_put(*out[i]);
		*out[i] = NULL;
	}
	if (!IS_ERR_OR_NULL(rb->bb_rx_data))
		gpiod_put_array(rb->bb_rx_data);
	rb->bb_rx_data = NULL;
}

static void rb_bb_down(struct rb_dev *rb)
{
	if (rb->worker) {
		kthread_stop(rb->worker);
		rb->worker = NULL;
	}
	if (rb->bbio.base) {
		rb_bb_clr(&rb->bbio, RB_BB_RX_CLK | RB_BB_TX_CLK | RB_BB_TX_DATA);
		iounmap(rb->bbio.base);
		rb->bbio.base = NULL;
	}
	rb_bb_put_gpios(rb);
	kfree(rb->bb_rx_words);
	kfree(rb->bb_tx_words);
	rb->bb_rx_words = NULL;
	rb->bb_tx_words = NULL;
	rb->bb_tx_count = 0;
	rb->bb_tx_pos = 0;
}

/*
 * The stream pins as the overlay lists them, claimed through gpiolib (which
 * sets their direction and the inputs' pull-downs), and the GPIO block's
 * registers mapped for the clocking itself.
 */
static int rb_bb_up(struct rb_dev *rb)
{
	struct device_node *np;
	int ret;

	rb->bb_rx_clk = gpiod_get(rb->dev, "rx-clk", GPIOD_OUT_LOW);
	rb->bb_tx_clk = gpiod_get(rb->dev, "tx-clk", GPIOD_OUT_LOW);
	rb->bb_tx_data = gpiod_get(rb->dev, "tx-data", GPIOD_OUT_LOW);
	rb->bb_rx_ready = gpiod_get(rb->dev, "rx-ready", GPIOD_IN);
	rb->bb_tx_ready = gpiod_get(rb->dev, "tx-ready", GPIOD_IN);
	rb->bb_rx_data = gpiod_get_array(rb->dev, "rx-data", GPIOD_IN);
	if (IS_ERR(rb->bb_rx_clk) || IS_ERR(rb->bb_tx_clk) || IS_ERR(rb->bb_tx_data) ||
	    IS_ERR(rb->bb_rx_ready) || IS_ERR(rb->bb_tx_ready) || IS_ERR(rb->bb_rx_data)) {
		dev_err(rb->dev, "cannot claim the stream GPIOs (an older overlay?)\n");
		ret = -ENODEV;
		goto fail;
	}
	if (rb->bb_rx_data->ndescs != 4) {
		dev_err(rb->dev, "rx-data-gpios needs 4 GPIOs, not %u\n", rb->bb_rx_data->ndescs);
		ret = -EINVAL;
		goto fail;
	}

	/* The set, clear and level registers of the block those GPIOs are on. */
	np = of_parse_phandle(rb->dev->of_node, "rx-clk-gpios", 0);
	if (!np || !of_device_is_compatible(np, "brcm,bcm2711-gpio")) {
		of_node_put(np);
		dev_err(rb->dev, "the stream GPIOs are not on the BCM2711 GPIO block\n");
		ret = -ENODEV;
		goto fail;
	}
	rb->bbio.base = of_iomap(np, 0);
	of_node_put(np);
	if (!rb->bbio.base) {
		ret = -ENOMEM;
		goto fail;
	}

	rb->bb_rx_words = kcalloc(RB_BB_RX_BATCH, sizeof(u32), GFP_KERNEL);
	rb->bb_tx_words = kzalloc(RB_TX_SLOT_BYTES, GFP_KERNEL);
	if (!rb->bb_rx_words || !rb->bb_tx_words) {
		ret = -ENOMEM;
		goto fail;
	}
	memset(&rb->bb, 0, sizeof(rb->bb));
	rb->bb.io = &rb->bbio;
	rb->bb.settle_reads = bb_settle_reads;
	rb->bb_tx_count = 0;
	rb->bb_tx_pos = 0;

	rb->worker = kthread_run(rb_bb_worker, rb, "radioberry");
	if (IS_ERR(rb->worker)) {
		ret = PTR_ERR(rb->worker);
		rb->worker = NULL;
		goto fail;
	}
	sched_set_fifo(rb->worker);
	dev_info(rb->dev, "RX and TX clocked by the CPU, up to %u RX values per second\n",
		 bb_rx_values_max);
	return 0;

fail:
	rb_bb_down(rb);
	return ret;
}

/* Waits for the thread to finish its run; it clocks nothing until released. */
static bool rb_bb_rx_hold(struct rb_dev *rb)
{
	if (!rb->worker)
		return false;
	mutex_lock(&rb->bb_lock);
	return true;
}

static void rb_bb_rx_release(struct rb_dev *rb)
{
	mutex_unlock(&rb->bb_lock);
}

/*
 * The thread reads rx_paused under bb_lock before each run, so once the
 * run in progress ends it clocks no more RX and sleeps.
 */
static void rb_bb_rx_pause(struct rb_dev *rb)
{
	mutex_lock(&rb->bb_lock);
	mutex_unlock(&rb->bb_lock);
}

static void rb_bb_rx_resume(struct rb_dev *rb)
{
	wake_up(&rb->work_wait);
}

static const struct rb_transport rb_bb_transport = {
	.name = "cpu",
	.rx_values_max = 0,
	.up = rb_bb_up,
	.down = rb_bb_down,
	.rx_hold = rb_bb_rx_hold,
	.rx_release = rb_bb_rx_release,
	.rx_pause = rb_bb_rx_pause,
	.rx_resume = rb_bb_rx_resume,
};

/* The transport for this SoC, or NULL. */
static const struct rb_transport *rb_select_transport(void)
{
	if (of_machine_is_compatible("brcm,bcm2712"))
		return &rb_pio_transport;
	if (of_machine_is_compatible("brcm,bcm2711"))
		return &rb_bb_transport;
	return NULL;
}

static u32 rb_rx_values_max(const struct rb_dev *rb)
{
	return rb->tp->rx_values_max ? rb->tp->rx_values_max : bb_rx_values_max;
}

static int rb_streams_up(struct rb_dev *rb)
{
	return rb->tp->up(rb);
}

static void rb_stop_streams(struct rb_dev *rb)
{
	rb->tp->down(rb);
}

/*
 * RX parsing for one receiver at 48 ksps, empty rings, zeroed statistics,
 * and RX paused unless run is set (before the streams start).
 */
static void rb_reset_stream_state(struct rb_dev *rb)
{
	unsigned long flags;

	WRITE_ONCE(rb->rx_paused, !READ_ONCE(rb->run));
	spin_lock_irqsave(&rb->rx_lock, flags);
	rb->receivers = 1;
	rb->rate_code = 0;
	rb_rx_init(&rb->parser, 1);
	rb->rx_period = rb_rx_period_bytes(0, 1, RB_RX_MIN_PERIOD, RB_RX_SLOT_BYTES);
	rb_ring_reset(&rb->rx_ring);
	rb->rx_overrun = false;
	memset(&rb->stats, 0, sizeof(rb->stats));
	spin_unlock_irqrestore(&rb->rx_lock, flags);
	rb->rx_words_seen = 0;
	rb->rx_last_progress = jiffies;
	rb->rx_stalled = false;
	spin_lock_irqsave(&rb->tx_lock, flags);
	rb_ring_reset(&rb->tx_ring);
	rb->tx_starved = false;
	spin_unlock_irqrestore(&rb->tx_lock, flags);
}

/*
 * Stops the streams, reloads the gateware, re-reads its identity and
 * restarts the streams. The caller holds rb->open, so no file is open.
 * On failure the streams stay down and open() is refused.
 */
static int rb_reload(struct rb_dev *rb)
{
	int ret;

	WRITE_ONCE(rb->running, false);
	rb_stop_streams(rb);
	ret = rb_command_safe(rb, NULL);
	if (ret)
		dev_err(rb->dev, "safe command before reload failed: %d\n", ret);
	ret = rb_load_gateware(rb);
	if (ret)
		return ret;
	ret = rb_read_identity(rb);
	if (ret)
		return ret;
	dev_info(rb->dev, "reloaded: gateware %u.%u, FPGA %s, %u receivers, %u transmitter(s)\n",
		 rb->id.major, rb->id.minor, rb_fpga_name(rb->id.fpga_type),
		 rb->id.receivers, rb->id.transmitters);
	rb_reset_stream_state(rb);
	ret = rb_streams_up(rb);
	if (ret)
		return ret;
	WRITE_ONCE(rb->running, true);
	return 0;
}

/* ---- character device ----------------------------------------------- */

static struct rb_dev *rb_from_file(struct file *file)
{
	return container_of(file->private_data, struct rb_dev, misc);
}

static int rb_open(struct inode *inode, struct file *file)
{
	struct rb_dev *rb = rb_from_file(file);
	unsigned long flags;

	if (atomic_cmpxchg(&rb->open, 0, 1) != 0)
		return -EBUSY;
	if (!READ_ONCE(rb->running)) {
		atomic_set(&rb->open, 0);
		return -ENODEV;
	}

	spin_lock_irqsave(&rb->rx_lock, flags);
	rb_ring_reset(&rb->rx_ring);
	rb->rx_overrun = false;
	spin_unlock_irqrestore(&rb->rx_lock, flags);
	spin_lock_irqsave(&rb->tx_lock, flags);
	rb_ring_reset(&rb->tx_ring);
	rb->tx_starved = false;
	spin_unlock_irqrestore(&rb->tx_lock, flags);
	return stream_open(inode, file);
}

static int rb_release(struct inode *inode, struct file *file)
{
	struct rb_dev *rb = rb_from_file(file);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&rb->tx_lock, flags);
	rb_ring_reset(&rb->tx_ring);
	spin_unlock_irqrestore(&rb->tx_lock, flags);

	ret = rb_command_safe(rb, NULL);
	if (ret)
		dev_err(rb->dev, "safe command on close failed: %d\n", ret);
	atomic_set(&rb->open, 0);
	return 0;
}

static bool rb_rx_ready(struct rb_dev *rb, unsigned int frame_bytes)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&rb->rx_lock, flags);
	ready = rb_ring_used(&rb->rx_ring) >= frame_bytes;
	spin_unlock_irqrestore(&rb->rx_lock, flags);
	return ready;
}

static ssize_t rb_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct rb_dev *rb = rb_from_file(file);
	unsigned int frame_bytes;
	unsigned long flags;
	size_t done = 0;
	u32 chunk, got;
	int ret;

	spin_lock_irqsave(&rb->rx_lock, flags);
	frame_bytes = rb_rx_frame_bytes(&rb->parser);
	spin_unlock_irqrestore(&rb->rx_lock, flags);
	if (count < frame_bytes)
		return -EINVAL;

	if (!rb_rx_ready(rb, frame_bytes)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(rb->rx_wait, rb_rx_ready(rb, frame_bytes));
		if (ret)
			return ret;
	}

	mutex_lock(&rb->io_lock);
	while (done + frame_bytes <= count) {
		chunk = (u32)min_t(size_t, count - done, RB_CHUNK_BYTES);
		chunk -= chunk % frame_bytes;
		if (!chunk)
			break;
		spin_lock_irqsave(&rb->rx_lock, flags);
		if (rb_rx_frame_bytes(&rb->parser) != frame_bytes) {
			spin_unlock_irqrestore(&rb->rx_lock, flags);
			break; /* receiver count changed under us */
		}
		got = rb_ring_used(&rb->rx_ring);
		got -= got % frame_bytes;
		got = rb_ring_get(&rb->rx_ring, rb->io_buf, min(chunk, got));
		spin_unlock_irqrestore(&rb->rx_lock, flags);
		if (!got)
			break;
		if (copy_to_user(buf + done, rb->io_buf, got)) {
			mutex_unlock(&rb->io_lock);
			return done ? (ssize_t)done : -EFAULT;
		}
		done += got;
	}
	mutex_unlock(&rb->io_lock);
	return (ssize_t)done;
}

static bool rb_tx_space(struct rb_dev *rb, u32 bytes)
{
	unsigned long flags;
	bool space;

	spin_lock_irqsave(&rb->tx_lock, flags);
	space = rb_ring_free(&rb->tx_ring) >= bytes;
	spin_unlock_irqrestore(&rb->tx_lock, flags);
	return space;
}

static ssize_t rb_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct rb_dev *rb = rb_from_file(file);
	unsigned long flags;
	size_t done = 0;
	u32 chunk;
	int ret;

	if (count % RB_TX_SAMPLE_BYTES)
		return -EINVAL;

	mutex_lock(&rb->io_lock);
	while (done < count) {
		chunk = (u32)min_t(size_t, count - done, RB_CHUNK_BYTES);
		if (!rb_tx_space(rb, chunk)) {
			if (file->f_flags & O_NONBLOCK) {
				mutex_unlock(&rb->io_lock);
				return done ? (ssize_t)done : -EAGAIN;
			}
			mutex_unlock(&rb->io_lock);
			ret = wait_event_interruptible(rb->tx_wait, rb_tx_space(rb, chunk));
			if (ret)
				return done ? (ssize_t)done : ret;
			mutex_lock(&rb->io_lock);
		}
		if (copy_from_user(rb->io_buf, buf + done, chunk)) {
			mutex_unlock(&rb->io_lock);
			return done ? (ssize_t)done : -EFAULT;
		}
		rb_tx_pack(rb->io_buf, rb->pack_buf, chunk / RB_TX_SAMPLE_BYTES);
		spin_lock_irqsave(&rb->tx_lock, flags);
		rb_ring_put(&rb->tx_ring, (const u8 *)rb->pack_buf, chunk);
		spin_unlock_irqrestore(&rb->tx_lock, flags);
		wake_up(&rb->work_wait);
		done += chunk;
	}
	mutex_unlock(&rb->io_lock);
	return (ssize_t)done;
}

static __poll_t rb_poll(struct file *file, poll_table *wait)
{
	struct rb_dev *rb = rb_from_file(file);
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(file, &rb->rx_wait, wait);
	poll_wait(file, &rb->tx_wait, wait);
	spin_lock_irqsave(&rb->rx_lock, flags);
	if (rb_ring_used(&rb->rx_ring) >= rb_rx_frame_bytes(&rb->parser))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (rb->rx_overrun)
		mask |= EPOLLERR;
	spin_unlock_irqrestore(&rb->rx_lock, flags);
	if (rb_tx_space(rb, RB_TX_SAMPLE_BYTES))
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static void rb_observe_command(struct rb_dev *rb, const u8 *tx)
{
	unsigned int receivers, rate_code;
	unsigned long flags;

	if (!rb_cmd_general(tx, &receivers, &rate_code))
		return;
	if (receivers > rb->id.receivers)
		receivers = rb->id.receivers;
	spin_lock_irqsave(&rb->rx_lock, flags);
	if (receivers != rb->receivers) {
		rb_trace(rb, "receivers %u -> %u, rate code %u -> %u\n", rb->receivers, receivers,
			 rb->rate_code, rate_code);
		rb_rx_set_receivers(&rb->parser, receivers);
		rb_ring_reset(&rb->rx_ring);
		rb->receivers = receivers;
	}
	rb->rate_code = rate_code;
	WRITE_ONCE(rb->rx_period, rb_rx_period_bytes(rate_code, receivers, RB_RX_MIN_PERIOD,
						     RB_RX_SLOT_BYTES));
	spin_unlock_irqrestore(&rb->rx_lock, flags);
}

static long rb_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct rb_dev *rb = rb_from_file(file);
	void __user *uarg = (void __user *)arg;
	unsigned long flags;
	int ret;

	switch (cmd) {
	case RADIOBERRY_IOC_INFO: {
		struct radioberry_info info = { 0 };

		info.abi_version = RADIOBERRY_ABI_VERSION;
		info.driver_version = RB_DRIVER_VERSION;
		info.gateware_major = rb->id.major;
		info.gateware_minor = rb->id.minor;
		info.fpga_type = rb->id.fpga_type;
		info.receivers = rb->id.receivers;
		info.transmitters = rb->id.transmitters;
		info.transport = rb->tp == &rb_bb_transport ? RADIOBERRY_TRANSPORT_CPU
							     : RADIOBERRY_TRANSPORT_PIO;
		info.rx_values_max = rb_rx_values_max(rb);
		spin_lock_irqsave(&rb->rx_lock, flags);
		info.active_receivers = (u8)rb->receivers;
		info.rx_rate_code = (u8)rb->rate_code;
		spin_unlock_irqrestore(&rb->rx_lock, flags);
		return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
	}
	case RADIOBERRY_IOC_COMMAND: {
		struct radioberry_command c;
		unsigned int hold = READ_ONCE(rx_clock_hold_us);
		unsigned int receivers, rate_code;
		bool count_change, held = false;

		if (copy_from_user(&c, uarg, sizeof(c)))
			return -EFAULT;
		rb_cmd_sanitise(c.tx);
		mutex_lock(&rb->cmd_lock);
		/*
		 * RX lock-up workaround (see rx_clock_hold_us above and
		 * docs/fault-rx-lockup.md, "The workaround"): a receiver-count
		 * change clears the gateware's RX FIFO, whose read side runs on the
		 * clock this driver's RX state machine drives. With the state
		 * machine stopped that clock is still while the clear is asserted
		 * and released. While RX is paused the clock is already still; a
		 * command that also sets run keeps it so for the hold time
		 * (rb_rx_gate()).
		 */
		count_change = rb_cmd_general(c.tx, &receivers, &rate_code) &&
			       receivers != READ_ONCE(rb->receivers);
		if (hold && count_change && !rb->rx_paused) {
			held = rb->tp->rx_hold(rb);
			if (held)
				usleep_range(20, 30);
		}
		ret = rb_command_locked(rb, c.tx, c.rx);
		if (!ret) {
			rb->stats.commands++;
			rb_observe_command(rb, c.tx);
		}
		if (held) {
			usleep_range(hold, hold + hold / 4);
			rb->tp->rx_release(rb);
			rb_trace(rb, "RX clock held for the receiver-count change\n");
		}
		if (!ret)
			rb_rx_gate(rb, count_change ? hold : 0);
		mutex_unlock(&rb->cmd_lock);
		if (ret)
			return ret;
		if (READ_ONCE(debug) >= 2)
			dev_info(rb->dev, "command %6ph reply %6ph\n", c.tx, c.rx);
		else
			rb_debug(rb, "command %6ph reply %6ph\n", c.tx, c.rx);
		wake_up(&rb->work_wait);
		return copy_to_user(uarg, &c, sizeof(c)) ? -EFAULT : 0;
	}
	case RADIOBERRY_IOC_STATS: {
		struct radioberry_stats st;

		spin_lock_irqsave(&rb->rx_lock, flags);
		rb->stats.rx_resyncs = rb->parser.resyncs;
		rb->stats.rx_dropped_words = rb->parser.dropped;
		rb->stats.rx_discarded_words = rb->parser.discarded;
		rb->stats.rx_ring_frames =
			rb_ring_used(&rb->rx_ring) / rb_rx_frame_bytes(&rb->parser);
		rb->stats.rx_period_bytes = rb->rx_period;
		rb->stats.flags = rb->rx_overrun ? RADIOBERRY_STATS_RX_OVERRUN : 0;
		rb->rx_overrun = false;
		st = rb->stats;
		spin_unlock_irqrestore(&rb->rx_lock, flags);
		spin_lock_irqsave(&rb->tx_lock, flags);
		st.tx_words = rb->stats.tx_words;
		st.tx_underruns = rb->stats.tx_underruns;
		st.tx_ring_words = rb_ring_used(&rb->tx_ring) / 4;
		spin_unlock_irqrestore(&rb->tx_lock, flags);
		if (READ_ONCE(rb->ptt))
			st.flags |= RADIOBERRY_STATS_PTT;
		return copy_to_user(uarg, &st, sizeof(st)) ? -EFAULT : 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations rb_fops = {
	.owner = THIS_MODULE,
	.open = rb_open,
	.release = rb_release,
	.read = rb_read,
	.write = rb_write,
	.poll = rb_poll,
	.unlocked_ioctl = rb_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

/* ---- sysfs ---------------------------------------------------------- */

/*
 * Read-only identity on the SPI device (/sys/bus/spi/devices/spi0.0/), so
 * tools can report it without opening the exclusive character device.
 */
static ssize_t gateware_version_show(struct device *dev, struct device_attribute *attr,
				     char *buf)
{
	struct rb_dev *rb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u.%u\n", rb->id.major, rb->id.minor);
}
static DEVICE_ATTR_RO(gateware_version);

static ssize_t fpga_type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rb_dev *rb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", rb_fpga_name(rb->id.fpga_type));
}
static DEVICE_ATTR_RO(fpga_type);

static ssize_t receivers_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rb_dev *rb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", rb->id.receivers);
}
static DEVICE_ATTR_RO(receivers);

static ssize_t transmitters_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rb_dev *rb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", rb->id.transmitters);
}
static DEVICE_ATTR_RO(transmitters);

/*
 * Writing a true value ("1", "y") reloads the gateware: refused with EBUSY
 * while /dev/radioberry is open, and nothing can open it until the reload
 * has finished. Returns the reload's error if it failed; the device then
 * stays closed to userspace until a reload succeeds.
 */
static ssize_t reload_store(struct device *dev, struct device_attribute *attr, const char *buf,
			    size_t count)
{
	struct rb_dev *rb = dev_get_drvdata(dev);
	bool requested;
	int ret;

	ret = kstrtobool(buf, &requested);
	if (ret)
		return ret;
	if (!requested)
		return (ssize_t)count;
	if (atomic_cmpxchg(&rb->open, 0, 1) != 0)
		return -EBUSY;
	ret = rb_reload(rb);
	atomic_set(&rb->open, 0);
	if (ret) {
		dev_err(rb->dev, "reload failed: %d; /dev/radioberry unavailable\n", ret);
		return ret;
	}
	return (ssize_t)count;
}
static DEVICE_ATTR_WO(reload);

/* 1 while no RX words have arrived for RB_STALL_MS with run set. */
static ssize_t rx_stalled_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rb_dev *rb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(rb->rx_stalled) ? 1 : 0);
}
static DEVICE_ATTR_RO(rx_stalled);

/* Stalls seen since the module loaded. */
static ssize_t rx_stalls_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rb_dev *rb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%llu\n", READ_ONCE(rb->rx_stalls));
}
static DEVICE_ATTR_RO(rx_stalls);

/* How the streams reach the gateware: "rp1-pio" (Pi 5) or "cpu" (Pi 4). */
static ssize_t transport_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rb_dev *rb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", rb->tp->name);
}
static DEVICE_ATTR_RO(transport);

static struct attribute *rb_attrs[] = {
	&dev_attr_gateware_version.attr,
	&dev_attr_fpga_type.attr,
	&dev_attr_receivers.attr,
	&dev_attr_transmitters.attr,
	&dev_attr_reload.attr,
	&dev_attr_rx_stalled.attr,
	&dev_attr_rx_stalls.attr,
	&dev_attr_transport.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rb);

/* ---- probe and remove ----------------------------------------------- */

static int rb_alloc_rings(struct rb_dev *rb)
{
	u32 rx_size = roundup_pow_of_two(max(rx_ring_kib, 16u) * 1024u);
	u32 tx_size = roundup_pow_of_two(max(tx_ring_kib, 4u) * 1024u);

	rb->rx_ring_buf = kvzalloc(rx_size, GFP_KERNEL);
	rb->tx_ring_buf = kvzalloc(tx_size, GFP_KERNEL);
	rb->io_buf = kzalloc(RB_CHUNK_BYTES, GFP_KERNEL);
	rb->pack_buf = kzalloc(RB_CHUNK_BYTES, GFP_KERNEL);
	rb->spi_tx = kzalloc(RB_CMD_BYTES, GFP_KERNEL);
	rb->spi_rx = kzalloc(RB_CMD_BYTES, GFP_KERNEL);
	if (!rb->rx_ring_buf || !rb->tx_ring_buf || !rb->io_buf || !rb->pack_buf ||
	    !rb->spi_tx || !rb->spi_rx)
		return -ENOMEM;
	rb_ring_init(&rb->rx_ring, rb->rx_ring_buf, rx_size);
	rb_ring_init(&rb->tx_ring, rb->tx_ring_buf, tx_size);
	return 0;
}

static void rb_free(struct rb_dev *rb)
{
	kvfree(rb->rx_ring_buf);
	kvfree(rb->tx_ring_buf);
	kfree(rb->io_buf);
	kfree(rb->pack_buf);
	kfree(rb->spi_tx);
	kfree(rb->spi_rx);
}

static int rb_get_gpios(struct rb_dev *rb)
{
	rb->nconfig = devm_gpiod_get(rb->dev, "nconfig", GPIOD_OUT_HIGH);
	if (IS_ERR(rb->nconfig))
		return dev_err_probe(rb->dev, PTR_ERR(rb->nconfig), "nconfig GPIO\n");
	rb->dclk = devm_gpiod_get(rb->dev, "dclk", GPIOD_OUT_LOW);
	if (IS_ERR(rb->dclk))
		return dev_err_probe(rb->dev, PTR_ERR(rb->dclk), "dclk GPIO\n");
	rb->data = devm_gpiod_get(rb->dev, "data0", GPIOD_OUT_LOW);
	if (IS_ERR(rb->data))
		return dev_err_probe(rb->dev, PTR_ERR(rb->data), "data0 GPIO\n");
	rb->conf_done = devm_gpiod_get(rb->dev, "conf-done", GPIOD_IN);
	if (IS_ERR(rb->conf_done))
		return dev_err_probe(rb->dev, PTR_ERR(rb->conf_done), "conf-done GPIO\n");
	rb->nstatus = devm_gpiod_get(rb->dev, "nstatus", GPIOD_IN);
	if (IS_ERR(rb->nstatus))
		return dev_err_probe(rb->dev, PTR_ERR(rb->nstatus), "nstatus GPIO\n");
	return 0;
}

static int rb_probe(struct spi_device *spi)
{
	struct rb_dev *rb;
	int ret;

	rb = devm_kzalloc(&spi->dev, sizeof(*rb), GFP_KERNEL);
	if (!rb)
		return -ENOMEM;
	rb->spi = spi;
	rb->dev = &spi->dev;
	rb->tp = rb_select_transport();
	if (!rb->tp)
		return dev_err_probe(rb->dev, -ENODEV,
				     "not a Raspberry Pi 4 or 5 (BCM2711 or BCM2712)\n");
	rb_stream_reset(&rb->rx);
	rb_stream_reset(&rb->tx);
	mutex_init(&rb->cmd_lock);
	mutex_init(&rb->io_lock);
	mutex_init(&rb->bb_lock);
	spin_lock_init(&rb->rx_lock);
	spin_lock_init(&rb->tx_lock);
	init_waitqueue_head(&rb->rx_wait);
	init_waitqueue_head(&rb->tx_wait);
	init_waitqueue_head(&rb->work_wait);
	atomic_set(&rb->rx.inflight, 0);
	atomic_set(&rb->tx.inflight, 0);
	spi_set_drvdata(spi, rb);

	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(rb->dev, ret, "SPI setup\n");

	ret = rb_get_gpios(rb);
	if (ret)
		return ret;

	ret = rb_alloc_rings(rb);
	if (ret)
		goto err_free;

	if (load_gateware) {
		ret = rb_load_gateware(rb);
		if (ret)
			goto err_free;
	} else if (!gpiod_get_value_cansleep(rb->conf_done)) {
		dev_err(rb->dev, "load_gateware=0 but the FPGA is not configured\n");
		ret = -ENODEV;
		goto err_free;
	}

	ret = rb_read_identity(rb);
	if (ret)
		goto err_free;
	dev_info(rb->dev, "gateware %u.%u, FPGA %s, %u receivers, %u transmitter(s)\n",
		 rb->id.major, rb->id.minor, rb_fpga_name(rb->id.fpga_type),
		 rb->id.receivers, rb->id.transmitters);

	rb_reset_stream_state(rb);
	ret = rb_streams_up(rb);
	if (ret)
		goto err_safe;
	rb->running = true;

	rb->misc.minor = MISC_DYNAMIC_MINOR;
	rb->misc.name = "radioberry";
	rb->misc.fops = &rb_fops;
	rb->misc.parent = rb->dev;
	ret = misc_register(&rb->misc);
	if (ret)
		goto err_streams;
	return 0;

err_streams:
	rb_stop_streams(rb);
err_safe:
	rb_command_safe(rb, NULL);
err_free:
	rb_free(rb);
	return ret;
}

static void rb_remove(struct spi_device *spi)
{
	struct rb_dev *rb = spi_get_drvdata(spi);

	misc_deregister(&rb->misc);
	rb_stop_streams(rb);
	rb_command_safe(rb, NULL);
	rb_free(rb);
}

static const struct of_device_id rb_of_match[] = {
	{ .compatible = "sdr,radioberry" },
	{}
};
MODULE_DEVICE_TABLE(of, rb_of_match);

static const struct spi_device_id rb_spi_ids[] = {
	{ "radioberry", 0 },
	{}
};
MODULE_DEVICE_TABLE(spi, rb_spi_ids);

static struct spi_driver rb_driver = {
	.driver = {
		.name = "radioberry",
		.of_match_table = rb_of_match,
		.dev_groups = rb_groups,
	},
	.probe = rb_probe,
	.remove = rb_remove,
	.id_table = rb_spi_ids,
};
module_spi_driver(rb_driver);

MODULE_DESCRIPTION("RadioBerry 2.x SDR on the Raspberry Pi 5 (RP1 PIO) and Pi 4");
MODULE_LICENSE("GPL");
MODULE_VERSION(RB_DRIVER_VERSION_STRING);
MODULE_SOFTDEP("pre: rp1_pio");
