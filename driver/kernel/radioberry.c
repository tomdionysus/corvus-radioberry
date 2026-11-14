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
 * Each board has its own gateware image and transport, chosen at probe
 * from the SoC; commands go to both images in the same framed protocol
 * (rb_frame.h), and the rest of the driver is shared:
 *
 * - Pi 5 (BCM2712), radioberry-rpi5-pio.rbf: two RP1 PIO state machines,
 *   built only on the stock rp1-pio kernel API. RX DMA writes the
 *   driver's own coherent buffers, TX DMA reads rp1-pio's bounce buffers:
 *   up to RB_DMA_SLOTS periods are in flight per direction and a
 *   FIFO-priority kernel thread refills them, so the state machines never
 *   wait for a restart.
 * - Pi 4 (BCM2711), radioberry-rpi4-i2s.rbf: an I2S link into the
 *   BCM2711's PCM block, the FPGA as clock master. The driver finds the
 *   block itself, puts GPIO 18 to 21 in their PCM function and programs it
 *   directly with cyclic DMA; its device tree node stays disabled, so no
 *   sound card appears.
 *
 * The PIO programs and the overall design follow the original RadioBerry
 * Pi 5 driver, released into the public domain under the Unlicense.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
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
#include <linux/of_dma.h>
#include <linux/of_platform.h>
#include <linux/pio_rp1.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "radioberry_ioctl.h"
#include "rb_logic.h"
#include "rb_frame.h"
#include "rb_pio.h"

#define RB_DRIVER_VERSION ((0u << 16) | (5u << 8) | 0u)
#define RB_DRIVER_VERSION_STRING "0.5.0"

#define RB_DMA_SLOTS 4
#define RB_RX_SLOT_BYTES (32 * 1024)
#define RB_RX_MIN_PERIOD 256
#define RB_TX_SLOT_BYTES 1024 /* 256 samples, 5.3 ms at 48 ksps */
#define RB_CHUNK_BYTES 4096

static bool load_gateware = true;
module_param(load_gateware, bool, 0444);
MODULE_PARM_DESC(load_gateware, "Load the gateware into the FPGA on probe (default 1)");

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

/* The PIO bus's RX ceiling at the default divider (gateware/docs/interfaces.md). */
#define RB_PIO_RX_VALUES_MAX 2370000

/* rp1-pio's own values (drivers/misc/rp1-pio.c): FIFO depth, DMACTRL base. */
#define RB_PIO_FIFO_DEPTH 8
#define RB_PIO_DMACTRL_DEFAULT 0x80000100

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

struct rb_stream {
	int sm;
	int prog_offset;
	bool prog_loaded;
	u8 *slot[RB_DMA_SLOTS];
	dma_addr_t slot_dma[RB_DMA_SLOTS]; /* RX: the slot's address for the RP1 DMA */
	u32 slot_bytes[RB_DMA_SLOTS];
	unsigned int submitted; /* free running slot counters */
	unsigned int completed;
	atomic_t inflight;
};

struct rb_dev;

/* How the RX and TX streams reach the gateware, and which image it runs. */
struct rb_transport {
	const char *name;
	u32 rx_values_max;
	/* Starts both streams and their kernel thread; undoes itself on failure. */
	int (*up)(struct rb_dev *rb);
	/* Stops everything up() started. Safe to call when it is not running. */
	void (*down)(struct rb_dev *rb);
	/*
	 * Stops RX after rx_paused is set, returning once no RX is clocked;
	 * restarts it after rx_paused is cleared. Safe while not running.
	 */
	void (*rx_pause)(struct rb_dev *rb);
	void (*rx_resume)(struct rb_dev *rb);
	const char *firmware;
	u8 caps_transport; /* the link its image's CAPS register reports */
};

struct rb_dev {
	struct spi_device *spi;
	struct device *dev;
	const struct rb_transport *tp;
	struct gpio_desc *nconfig, *dclk, *data, *conf_done, *nstatus;
	struct rb_identity id;

	struct mutex cmd_lock; /* SPI commands */
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
	struct device *pio_dma_dev; /* the RP1 DMA controller, a reference held */
	struct rb_stream rx, tx;

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

	/* Pi 4 I2S transport. */
	void __iomem *pcm;
	struct dma_chan *pcm_rx, *pcm_tx;
	dma_cookie_t pcm_rx_cookie, pcm_tx_cookie;
	u32 *pcm_buf;		/* RX, coherent, RB_I2S_PERIODS periods */
	dma_addr_t pcm_buf_dma;
	u32 *pcm_tbuf;		/* TX, coherent, the same size */
	dma_addr_t pcm_tbuf_dma;
	phys_addr_t pcm_fifo;
	unsigned int pcm_period;	/* words, each way */
	unsigned int pcm_next;		/* next RX period to take */
	unsigned int pcm_tfill;		/* next TX period to fill */
	struct rb_i2s_tx_sched pcm_sched;
	u32 *pcm_copy, *pcm_values;	/* one period each, cached */
	bool pcm_streaming;
	u64 pcm_tx_late;	/* TX DMA reached periods not yet filled */
	u64 pcm_tx_fifo_errors;	/* PCM TX FIFO ran dry (the image realigns) */
	unsigned int i2s_div;		/* the divider the streams run at */
	unsigned int i2s_div_want;	/* for the current receivers and rate */

	/* The image's framed control protocol (both boards). */
	u8 *frame_tx, *frame_rx;	/* DMA-safe */
	u8 frame_seq;
	u8 status2;		/* from the last reply */
	u8 flags;		/* run, permission, PA allowed, PTT (0x80) last sent */
	bool flags_sent;
	u32 control;		/* CONTROL last written */
	bool control_sent;
	bool loopback;		/* sysfs tx_loopback */
	struct delayed_work keepalive; /* rewrites KEYING while it is set */
	u8 reply[RB_CMD_BYTES];	/* identity in the reply layout, once valid */
	bool identified;

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

static int rb_frame_command(struct rb_dev *rb, const u8 *tx, u8 *rx);

/*
 * Sends one command block (RB_CMD_BYTES, the radioberry_command layout) to
 * the image and fills rx[] with the reply in the same layout, if rx is set.
 */
static int rb_command_locked(struct rb_dev *rb, const u8 *tx, u8 *rx)
{
	u8 reply[RB_CMD_BYTES];
	int ret;

	lockdep_assert_held(&rb->cmd_lock);
	ret = rb_frame_command(rb, tx, reply);
	if (ret)
		return ret;
	if (rx)
		memcpy(rx, reply, RB_CMD_BYTES);
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
 * queued before the pause are dropped.
 */
static void rb_rx_gate(struct rb_dev *rb)
{
	unsigned long flags;

	lockdep_assert_held(&rb->cmd_lock);
	if (!READ_ONCE(rb->run) && !rb->rx_paused) {
		WRITE_ONCE(rb->rx_paused, true);
		rb->tp->rx_pause(rb);
		rb_trace(rb, "RX paused: run clear\n");
	} else if (READ_ONCE(rb->run) && rb->rx_paused) {
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
		rb_rx_gate(rb);
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
		dev_err(rb->dev, "CONF_DONE stayed low: %s is built for a 10CL025 FPGA\n",
			rb->tp->firmware);
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
	const char *name = rb->tp->firmware;
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, name, rb->dev);
	if (ret) {
		dev_err(rb->dev, "cannot load /lib/firmware/%s: %d\n", name, ret);
		return ret;
	}
	ret = rb_configure_fpga(rb, fw->data, fw->size);
	if (!ret)
		dev_info(rb->dev, "gateware %s configured (%zu bytes)\n", name, fw->size);
	release_firmware(fw);
	/*
	 * The image starts with its registers cleared: identify it again and
	 * send it the flags and CONTROL afresh.
	 */
	rb->identified = false;
	rb->flags_sent = false;
	rb->control_sent = false;
	if (ret)
		return ret;

	/*
	 * The image sets up the AD9866 within a few milliseconds of loading;
	 * rb_read_identity() polls until it reports set up and running.
	 */
	msleep(20);
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
		/*
		 * By DMA address, so the RP1 DMA writes the slot itself: given only
		 * a buffer, rp1-pio before 6.18 receives into its own bounce buffer
		 * and never copies it back.
		 */
		ret = rp1_pio_sm_xfer_data(rb->pio, s->sm, RP1_PIO_DIR_FROM_SM, s->slot_bytes[idx],
					   s->slot[idx], s->slot_dma[idx], rb_rx_dma_done, rb);
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
 * Notes the first RB_STALL_MS without RX words while run is set, and the
 * resumption, so a stopped stream is visible in the log and in sysfs
 * (rx_stalled, rx_stalls).
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
	/*
	 * DMA request levels that hold for any burst the RP1 DMA uses (up to
	 * the FIFO depth): RX asks with the FIFO full, TX with it empty. With
	 * 6.12's rp1-pio the levels did not match the bursts: each RX burst
	 * returned the last word again in place of words not yet there, and
	 * TX bursts overfilled the FIFO, losing about half the samples.
	 */
	struct rp1_pio_sm_set_dmactrl_args dmactrl = { 0 };
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
		if (tx)
			s->slot[i] = kzalloc(RB_TX_SLOT_BYTES, GFP_KERNEL);
		else
			s->slot[i] = dma_alloc_coherent(rb->pio_dma_dev, RB_RX_SLOT_BYTES,
							&s->slot_dma[i], GFP_KERNEL);
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
	dmactrl.sm = (u16)s->sm;
	dmactrl.is_tx = tx;
	dmactrl.ctrl = RB_PIO_DMACTRL_DEFAULT | (tx ? 0 : RB_PIO_FIFO_DEPTH);
	ret = rp1_pio_sm_set_dmactrl(rb->pio, &dmactrl);
	if (ret < 0) {
		dev_err(rb->dev, "cannot set the %s DMA request level: %d\n", tx ? "TX" : "RX", ret);
		return ret;
	}
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
		 * its 64-byte record for each TX one (RX transfers, given by DMA
		 * address, have none; docs/kernel-debug.md).
		 */
		rp1_pio_close(rb->pio);
		rb->pio = NULL;
	}
	for (i = 0; i < RB_DMA_SLOTS; i++) {
		if (rb->rx.slot[i])
			dma_free_coherent(rb->pio_dma_dev, RB_RX_SLOT_BYTES, rb->rx.slot[i],
					  rb->rx.slot_dma[i]);
		kfree(rb->tx.slot[i]);
		rb->rx.slot[i] = NULL;
		rb->tx.slot[i] = NULL;
	}
	if (rb->pio_dma_dev) {
		put_device(rb->pio_dma_dev);
		rb->pio_dma_dev = NULL;
	}
	rb_stream_reset(&rb->rx);
	rb_stream_reset(&rb->tx);
}

/*
 * The RP1 DMA controller that serves the PIO (its first "dmas" entry), for
 * RX slots it can write directly. Returns it with a reference held.
 */
static struct device *rb_pio_dma_device(void)
{
	struct device_node *pio_np, *dma_np;
	struct platform_device *pdev;

	pio_np = of_find_compatible_node(NULL, NULL, "raspberrypi,rp1-pio");
	if (!pio_np)
		return NULL;
	dma_np = of_parse_phandle(pio_np, "dmas", 0);
	of_node_put(pio_np);
	if (!dma_np)
		return NULL;
	pdev = of_find_device_by_node(dma_np);
	of_node_put(dma_np);
	return pdev ? &pdev->dev : NULL;
}

/*
 * Opens the RP1 PIO, claims and configures both state machines and starts
 * them with their kernel thread. Undoes everything on failure.
 */
static int rb_pio_up(struct rb_dev *rb)
{
	int ret;

	rb->pio_dma_dev = rb_pio_dma_device();
	if (!rb->pio_dma_dev) {
		dev_err(rb->dev, "cannot find the RP1 PIO's DMA controller\n");
		return -ENODEV;
	}
	rb->pio = rp1_pio_open();
	if (IS_ERR(rb->pio)) {
		ret = PTR_ERR(rb->pio);
		rb->pio = NULL;
		dev_err(rb->dev, "cannot open the RP1 PIO: %d\n", ret);
		rb_pio_down(rb);
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

/*
 * The RX state machine stops, so the clock stands still and nothing reaches
 * the DMA; the periods in flight wait for it and the thread sleeps.
 */
static void rb_pio_rx_enable(struct rb_dev *rb, bool on)
{
	struct rp1_pio_sm_set_enabled_args args = { .enable = on };

	if (!rb->pio || rb->rx.sm < 0)
		return;
	args.mask = (u16)BIT(rb->rx.sm);
	rp1_pio_sm_set_enabled(rb->pio, &args);
}

static void rb_pio_rx_pause(struct rb_dev *rb)
{
	rb_pio_rx_enable(rb, false);
}

static void rb_pio_rx_resume(struct rb_dev *rb)
{
	rb_pio_rx_enable(rb, true);
}

/* ---- I2S into the PCM block (Pi 4) ---------------------------------- */

/* BCM2711 PCM registers and bits (BCM2711 ARM Peripherals, chapter 7). */
#define RB_PCM_CS 0x00
#define RB_PCM_FIFO 0x04
#define RB_PCM_MODE 0x08
#define RB_PCM_RXC 0x0c
#define RB_PCM_TXC 0x10
#define RB_PCM_DREQ 0x14
#define RB_PCM_CS_EN BIT(0)
#define RB_PCM_CS_RXON BIT(1)
#define RB_PCM_CS_TXON BIT(2)
#define RB_PCM_CS_TXCLR BIT(3)
#define RB_PCM_CS_RXCLR BIT(4)
#define RB_PCM_CS_DMAEN BIT(9)
#define RB_PCM_CS_TXERR BIT(15)
#define RB_PCM_CS_RXERR BIT(16)
#define RB_PCM_MODE_FSM BIT(21)
#define RB_PCM_MODE_CLKM BIT(23)

#define RB_I2S_FIRMWARE "radioberry-rpi4-i2s.rbf"
#define RB_PIO_FIRMWARE "radioberry-rpi5-pio.rbf"
#define RB_I2S_PERIODS 16
#define RB_I2S_MAX_PERIOD 4096 /* words */
#define RB_I2S_TX_LEAD 4	   /* TX periods kept filled ahead of the DMA, about 8 ms */
#define RB_KEEPALIVE_MS 250	   /* KEYING rewrites; the image clears it after 1 s */
/* What the image reports to clients: the Hermes-Lite 2 core it carries. */
#define RB_HL2_MAJOR 75
#define RB_HL2_MINOR 3

/* One frame each way. -EIO when the reply's header or CRC is wrong. */
static int rb_frame_xfer(struct rb_dev *rb, bool write, bool board, u8 addr, u32 data,
			 u32 *value, u8 *status)
{
	struct spi_transfer t = {
		.tx_buf = rb->frame_tx,
		.rx_buf = rb->frame_rx,
		.len = RB_FRAME_BYTES,
	};
	u8 st, st2;
	u32 v;
	int ret, tries;

	for (tries = 0; tries < 3; tries++) {
		rb_frame_build(rb->frame_tx, write, board, addr, data, rb->frame_seq++);
		ret = spi_sync_transfer(rb->spi, &t, 1);
		if (ret)
			return ret;
		if (rb_frame_reply(rb->frame_rx, &st, &v, &st2)) {
			rb->status2 = st2;
			if (value)
				*value = v;
			if (status)
				*status = st;
			return 0;
		}
	}
	dev_err_ratelimited(rb->dev, "control frame %8ph: bad reply %8ph\n", rb->frame_tx,
			    rb->frame_rx);
	return -EIO;
}

static int rb_breg_read(struct rb_dev *rb, u8 addr, u32 *value)
{
	return rb_frame_xfer(rb, false, true, addr, 0, value, NULL);
}

static int rb_breg_write(struct rb_dev *rb, u8 addr, u32 value)
{
	return rb_frame_xfer(rb, true, true, addr, value, NULL, NULL);
}

/*
 * Identity in the command reply layout (rb_reply_parse()), once the image is
 * running: its transmitter, the receivers it carries, the Hermes-Lite 2 core
 * level clients see. Zeros until then, so rb_read_identity() keeps polling.
 * The image must be built for this board's link (its CAPS register says
 * which).
 */
static int rb_identify(struct rb_dev *rb, u8 status)
{
	u32 id, caps;
	int ret;

	if (rb->identified)
		return 0;
	if ((status & (RB_FS_SET_UP | RB_FS_RUNNING)) != (RB_FS_SET_UP | RB_FS_RUNNING))
		return 0;
	ret = rb_breg_read(rb, RB_BREG_ID, &id);
	if (!ret)
		ret = rb_breg_read(rb, RB_BREG_CAPS, &caps);
	if (ret)
		return ret;
	if (id != RB_ID_RBV2 || RB_CAPS_TRANSPORT(caps) != rb->tp->caps_transport) {
		dev_err_ratelimited(rb->dev,
				    "%s is not the image for this board: id 0x%08x caps 0x%08x\n",
				    rb->tp->firmware, id, caps);
		return 0;
	}
	memset(rb->reply, 0, sizeof(rb->reply));
	rb->reply[3] = (u8)((RB_CAPS_TX(caps) << 6) | (RB_CAPS_RECEIVERS(caps) << 2) |
				(RB_CAPS_FPGA(caps) & 3));
	rb->reply[4] = RB_HL2_MAJOR;
	rb->reply[5] = RB_HL2_MINOR;
	rb->identified = true;
	return 0;
}

/*
 * The PCM block is a bit-clock slave: its FIFO clears (and its channel
 * order) act on bit clock edges. So every FIFO clear and every start of
 * transmission happens with the image's link enabled. A clear made with
 * the clock stopped is lost, and a word left in the TX FIFO shifts every
 * later frame by one word (measured on the Pi 4: every frame then failed
 * the image's check).
 */
#define RB_PCM_CLR_US 100 /* many bit clocks at the slowest divider */

/*
 * Stops both DMA streams and empties the PCM FIFOs while the bit clock
 * still runs. The block stays enabled and idle; rb_i2s_link_stop() turns it
 * off once the link has stopped.
 */
static void rb_i2s_stream_stop(struct rb_dev *rb)
{
	if (!rb->pcm || !rb->pcm_streaming)
		return;
	writel(RB_PCM_CS_EN, rb->pcm + RB_PCM_CS);
	dmaengine_terminate_sync(rb->pcm_rx);
	dmaengine_terminate_sync(rb->pcm_tx);
	writel(RB_PCM_CS_EN | RB_PCM_CS_RXCLR | RB_PCM_CS_TXCLR, rb->pcm + RB_PCM_CS);
	usleep_range(RB_PCM_CLR_US, 2 * RB_PCM_CLR_US);
	rb->pcm_streaming = false;
}

/*
 * Stops the streams, then the image's link (the bit clock), then the PCM
 * block. `div` is written with the link disabled, for its next start.
 */
static int rb_i2s_link_stop(struct rb_dev *rb, unsigned int div)
{
	int ret;

	rb_i2s_stream_stop(rb);
	ret = rb_breg_write(rb, RB_BREG_I2S, (u32)div << 8);
	if (rb->pcm)
		writel(0, rb->pcm + RB_PCM_CS);
	return ret;
}

/*
 * Keeps the TX buffer filled RB_I2S_TX_LEAD periods ahead of the period the
 * TX DMA is reading, each period from the ring by the transmit schedule.
 * True if samples were taken from the ring.
 */
static bool rb_i2s_tx_fill_from(struct rb_dev *rb, unsigned int period, unsigned int reading)
{
	unsigned int frames = period / 2, ahead, slots, take, got;
	unsigned long flags;
	bool took = false;
	u32 *dst;

	ahead = (rb->pcm_tfill + RB_I2S_PERIODS - reading) % RB_I2S_PERIODS;
	if (ahead == 0 || ahead > RB_I2S_TX_LEAD + 1) {
		/* The DMA has reached periods not filled: they went as they were. */
		rb->pcm_tx_late++;
		rb->pcm_tfill = (reading + 1) % RB_I2S_PERIODS;
	}
	while ((rb->pcm_tfill + RB_I2S_PERIODS - reading) % RB_I2S_PERIODS <= RB_I2S_TX_LEAD) {
		/* The slots this period has: what the schedule will want. */
		slots = (rb->pcm_sched.acc + frames * rb->i2s_div) / RB_I2S_TX_DIV;
		spin_lock_irqsave(&rb->tx_lock, flags);
		take = min_t(u32, slots, rb_ring_used(&rb->tx_ring) / 4);
		got = rb_ring_get(&rb->tx_ring, (u8 *)rb->pcm_values, take * 4) / 4;
		rb->stats.tx_words += got;
		if (got < slots && rb->ptt)
			rb->stats.tx_underruns++;
		spin_unlock_irqrestore(&rb->tx_lock, flags);
		if (got)
			took = true;
		/* Built in cached memory, then one copy into the uncached buffer. */
		rb_i2s_tx_frames(&rb->pcm_sched, rb->i2s_div, rb->pcm_copy, frames, rb->pcm_values,
				 got, &slots);
		dst = rb->pcm_tbuf + (size_t)rb->pcm_tfill * period;
		memcpy(dst, rb->pcm_copy, (size_t)period * 4);
		rb->pcm_tfill = (rb->pcm_tfill + 1) % RB_I2S_PERIODS;
	}
	return took;
}

/* As rb_i2s_tx_fill_from(), from the period the TX DMA is reading now. */
static bool rb_i2s_tx_fill(struct rb_dev *rb, unsigned int period, size_t bytes)
{
	struct dma_tx_state state;
	unsigned int reading;

	dmaengine_tx_status(rb->pcm_tx, rb->pcm_tx_cookie, &state);
	reading = (unsigned int)(((bytes - min_t(size_t, state.residue, bytes)) % bytes) /
				 ((size_t)period * 4));
	return rb_i2s_tx_fill_from(rb, period, reading);
}

static void rb_i2s_dma_done(void *param);

/*
 * Starts the link at the divider and RX and TX DMA with periods sized for
 * it, from stopped (rb_i2s_link_stop()): the PCM block is set up idle, the
 * image's link started, the FIFOs cleared with the bit clock running, the
 * TX FIFO primed by DMA, then both directions switched on. TX starts with
 * an idle period and the lead periods filled from the ring, its schedule
 * from the start; the image starts its sequence check again whenever the
 * link is disabled, which every call here follows.
 */
static int rb_i2s_stream_start(struct rb_dev *rb, unsigned int div)
{
	struct dma_slave_config rcfg = {
		.direction = DMA_DEV_TO_MEM,
		.src_addr = rb->pcm_fifo,
		.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.src_maxburst = 2,
	};
	struct dma_slave_config tcfg = {
		.direction = DMA_MEM_TO_DEV,
		.dst_addr = rb->pcm_fifo,
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.dst_maxburst = 2,
	};
	struct dma_async_tx_descriptor *rdesc, *tdesc;
	/* Two 32-bit channels in a 64-bit frame, MSB first, no delay. */
	u32 ch = BIT(31) | BIT(30) | 0 << 20 | 8 << 16 | BIT(15) | BIT(14) | 32 << 4 | 8;
	size_t bytes;
	int ret;

	rb->pcm_period = rb_i2s_period_words(div);
	bytes = (size_t)rb->pcm_period * 4 * RB_I2S_PERIODS;
	rb->pcm_next = 0;
	rb->pcm_tfill = 1;
	memset(&rb->pcm_sched, 0, sizeof(rb->pcm_sched));
	memset(rb->pcm_tbuf, 0, bytes);
	/* Period 0 goes idle; the lead periods after it are filled now. */
	if (rb_i2s_tx_fill_from(rb, rb->pcm_period, 0))
		wake_up_interruptible(&rb->tx_wait);
	ret = dmaengine_slave_config(rb->pcm_rx, &rcfg);
	if (!ret)
		ret = dmaengine_slave_config(rb->pcm_tx, &tcfg);
	if (ret)
		return ret;
	rdesc = dmaengine_prep_dma_cyclic(rb->pcm_rx, rb->pcm_buf_dma, bytes,
					  (size_t)rb->pcm_period * 4, DMA_DEV_TO_MEM,
					  DMA_PREP_INTERRUPT);
	tdesc = dmaengine_prep_dma_cyclic(rb->pcm_tx, rb->pcm_tbuf_dma, bytes,
					  (size_t)rb->pcm_period * 4, DMA_MEM_TO_DEV, 0);
	if (!rdesc || !tdesc) {
		if (rdesc)
			dmaengine_desc_free(rdesc);
		if (tdesc)
			dmaengine_desc_free(tdesc);
		return -EIO;
	}
	rdesc->callback = rb_i2s_dma_done;
	rdesc->callback_param = rb;

	writel(0, rb->pcm + RB_PCM_CS);
	writel(RB_PCM_MODE_CLKM | RB_PCM_MODE_FSM | 63 << 10 | 32, rb->pcm + RB_PCM_MODE);
	writel(ch, rb->pcm + RB_PCM_RXC);
	writel(ch, rb->pcm + RB_PCM_TXC);
	writel(RB_PCM_CS_EN, rb->pcm + RB_PCM_CS);
	ret = rb_breg_write(rb, RB_BREG_I2S, rb_i2s_link_reg(div));
	if (ret) {
		writel(0, rb->pcm + RB_PCM_CS);
		dmaengine_desc_free(rdesc);
		dmaengine_desc_free(tdesc);
		return ret;
	}
	writel(RB_PCM_CS_EN | RB_PCM_CS_RXCLR | RB_PCM_CS_TXCLR, rb->pcm + RB_PCM_CS);
	usleep_range(RB_PCM_CLR_US, 2 * RB_PCM_CLR_US);
	writel(0x10 << 24 | 0x30 << 16 | 0x30 << 8 | 0x20, rb->pcm + RB_PCM_DREQ);
	writel(RB_PCM_CS_EN | RB_PCM_CS_DMAEN | RB_PCM_CS_RXERR | RB_PCM_CS_TXERR,
	       rb->pcm + RB_PCM_CS);
	rb->pcm_rx_cookie = dmaengine_submit(rdesc);
	rb->pcm_tx_cookie = dmaengine_submit(tdesc);
	dma_async_issue_pending(rb->pcm_rx);
	dma_async_issue_pending(rb->pcm_tx);
	/* The TX DMA fills the PCM FIFO, starting with frame word 0. */
	usleep_range(1000, 2000);
	rb->pcm_streaming = true;
	writel(RB_PCM_CS_EN | RB_PCM_CS_DMAEN | RB_PCM_CS_RXON | RB_PCM_CS_TXON,
	       rb->pcm + RB_PCM_CS);
	return 0;
}

/*
 * Sets the image's bit clock for the divider and restarts both DMA streams
 * with periods to match (force: even with the divider unchanged). The link
 * stops for the change: words in flight in the image's FIFO wait for it,
 * and the parser resynchronises on the next frame start anyway.
 */
static int rb_i2s_set_div(struct rb_dev *rb, unsigned int div, bool force)
{
	int ret;

	if (rb->pcm_streaming && div == rb->i2s_div && !force)
		return 0;
	ret = rb_i2s_link_stop(rb, div);
	if (ret)
		return ret;
	rb->i2s_div = div; /* the TX schedule's divider, from the first fill */
	ret = rb_i2s_stream_start(rb, div);
	if (ret)
		return ret;
	rb_trace(rb, "I2S bit clock %u Hz, %u words a DMA period\n", RB_I2S_FAST_HZ / div,
		 rb->pcm_period);
	return 0;
}

/* The periods the DMA has filled since the last call, from its residue. */
static void rb_i2s_dma_done(void *param)
{
	struct rb_dev *rb = param;
	unsigned int period = READ_ONCE(rb->pcm_period);
	size_t bytes = (size_t)period * 4 * RB_I2S_PERIODS;
	struct dma_tx_state state;
	unsigned int filling, n, k, values;
	bool wake = false;
	u32 cs;

	dmaengine_tx_status(rb->pcm_rx, rb->pcm_rx_cookie, &state);
	filling = (unsigned int)(((bytes - min_t(size_t, state.residue, bytes)) % bytes) /
				 ((size_t)period * 4));
	n = (filling + RB_I2S_PERIODS - rb->pcm_next) % RB_I2S_PERIODS;
	for (k = 0; k < n; k++) {
		/* The coherent buffer is uncached: one bulk copy, then parse. */
		memcpy(rb->pcm_copy, rb->pcm_buf + (size_t)rb->pcm_next * period,
		       (size_t)period * 4);
		values = rb_i2s_values(rb->pcm_copy, period, rb->pcm_values);
		if (values && rb_rx_consume(rb, rb->pcm_values, values))
			wake = true;
		rb->pcm_next = (rb->pcm_next + 1) % RB_I2S_PERIODS;
	}
	if (wake)
		wake_up_interruptible(&rb->rx_wait);

	cs = readl(rb->pcm + RB_PCM_CS);
	if (cs & (RB_PCM_CS_RXERR | RB_PCM_CS_TXERR))
		writel(cs & ~(RB_PCM_CS_RXCLR | RB_PCM_CS_TXCLR), rb->pcm + RB_PCM_CS);
	if (cs & RB_PCM_CS_RXERR)
		rb_debug(rb, "PCM RX FIFO overflowed\n");
	if (cs & RB_PCM_CS_TXERR) {
		/*
		 * The PCM block sent zeros and may have slipped a word between
		 * its two channels; the image finds the new word phase itself.
		 */
		rb->pcm_tx_fifo_errors++;
		rb_debug(rb, "PCM TX FIFO ran dry\n");
	}

	if (rb_i2s_tx_fill(rb, period, bytes))
		wake_up_interruptible(&rb->tx_wait);

	rb_check_stall(rb);
}

#define RB_FLAG_PTT 0x80

static u32 rb_keying(u8 flags)
{
	return (flags & RB_FLAG_PTT ? RB_KEYING_PTT : 0) |
	       (flags & RADIOBERRY_CMD_TX_PERMIT ? RB_KEYING_PERMIT : 0);
}

/* CONTROL from the flags and the loopback setting, written when it changes. */
static int rb_write_control(struct rb_dev *rb)
{
	u32 control = (rb->flags & RADIOBERRY_CMD_RUN ? RB_CONTROL_RUN : 0) |
		      (rb->flags & RADIOBERRY_CMD_PA_ALLOWED ? RB_CONTROL_PA_ALLOWED : 0) |
		      (rb->loopback ? RB_CONTROL_LOOPBACK : 0);
	int ret;

	lockdep_assert_held(&rb->cmd_lock);
	if (rb->control_sent && control == rb->control)
		return 0;
	ret = rb_breg_write(rb, RB_BREG_CONTROL, control);
	if (ret)
		return ret;
	rb->control = control;
	rb->control_sent = true;
	return 0;
}

/*
 * The image clears KEYING unless it is written again within a second, so a
 * host that stops (a crash, a hung kernel) stops the transmitter. While
 * PTT or the permission bit is set this rewrites it every
 * RB_KEEPALIVE_MS.
 */
static void rb_keepalive(struct work_struct *work)
{
	struct rb_dev *rb = container_of(to_delayed_work(work), struct rb_dev, keepalive);
	u32 keying;

	mutex_lock(&rb->cmd_lock);
	keying = rb->flags_sent ? rb_keying(rb->flags) : 0;
	if (keying && rb_breg_write(rb, RB_BREG_KEYING, keying))
		dev_err_ratelimited(rb->dev, "KEYING keepalive failed\n");
	mutex_unlock(&rb->cmd_lock);
	if (keying)
		schedule_delayed_work(&rb->keepalive, msecs_to_jiffies(RB_KEEPALIVE_MS));
}

/*
 * One command block, in the image's framed protocol: the Hermes-Lite 2
 * command to the radio space; run and PA allowed, when they change, to the
 * board's CONTROL register, PTT and the permission bit to KEYING (kept
 * alive while set); on the Pi 4 a receiver-count or rate change sets the
 * I2S bit clock. rx[] gets the reply in the command layout: byte 0 from
 * status 2 (the core's CW or the jack's PTT, the jack's key), then the
 * identity.
 */
static int rb_frame_command(struct rb_dev *rb, const u8 *tx, u8 *rx)
{
	u8 flags = (u8)((tx[0] & (RADIOBERRY_CMD_RUN | RADIOBERRY_CMD_TX_PERMIT |
				  RADIOBERRY_CMD_PA_ALLOWED)) |
			(rb_cmd_ptt(tx) ? RB_FLAG_PTT : 0));
	bool first = !rb->flags_sent;
	unsigned int receivers, rate_code;
	u8 status;
	int ret;

	ret = rb_frame_xfer(rb, true, false, rb_cmd_addr(tx), rb_cmd_data(tx), NULL, &status);
	if (ret)
		return ret;
	/* Keying off before PA allowed goes; PA allowed before keying goes on. */
	if (!first && rb_keying(flags) != rb_keying(rb->flags) &&
	    !rb_keying(flags)) {
		ret = rb_breg_write(rb, RB_BREG_KEYING, 0);
		if (ret)
			return ret;
	}
	rb->flags = flags;
	rb->flags_sent = true;
	ret = rb_write_control(rb);
	if (ret)
		return ret;
	if (rb_keying(flags)) {
		ret = rb_breg_write(rb, RB_BREG_KEYING, rb_keying(flags));
		if (ret)
			return ret;
		mod_delayed_work(system_wq, &rb->keepalive,
				 msecs_to_jiffies(RB_KEEPALIVE_MS));
	} else if (first) {
		ret = rb_breg_write(rb, RB_BREG_KEYING, 0);
		if (ret)
			return ret;
	}

	if (rb->pcm && rb_cmd_general(tx, &receivers, &rate_code)) {
		rb->i2s_div_want = rb_i2s_div(rate_code, receivers);
		/* Paused: the link starts at this divider when run is set. */
		if (!rb->rx_paused) {
			ret = rb_i2s_set_div(rb, rb->i2s_div_want, false);
			if (ret)
				return ret;
		}
	}

	ret = rb_identify(rb, status);
	if (ret)
		return ret;
	if (rb->identified) {
		memcpy(rx, rb->reply, RB_CMD_BYTES);
		rx[0] = rb_frame_reply_flags(rb->status2);
	} else {
		memset(rx, 0, RB_CMD_BYTES);
	}
	return 0;
}

static void rb_i2s_down(struct rb_dev *rb)
{
	if (rb->pcm) {
		mutex_lock(&rb->cmd_lock);
		if (rb_i2s_link_stop(rb, rb->i2s_div_want))
			dev_err_ratelimited(rb->dev, "stopping the I2S link failed\n");
		mutex_unlock(&rb->cmd_lock);
	}
	if (rb->pcm_rx) {
		if (rb->pcm_buf)
			dma_free_coherent(rb->pcm_rx->device->dev,
					  (size_t)RB_I2S_MAX_PERIOD * 4 * RB_I2S_PERIODS,
					  rb->pcm_buf, rb->pcm_buf_dma);
		dma_release_channel(rb->pcm_rx);
	}
	if (rb->pcm_tx) {
		if (rb->pcm_tbuf)
			dma_free_coherent(rb->pcm_tx->device->dev,
					  (size_t)RB_I2S_MAX_PERIOD * 4 * RB_I2S_PERIODS,
					  rb->pcm_tbuf, rb->pcm_tbuf_dma);
		dma_release_channel(rb->pcm_tx);
	}
	rb->pcm_rx = NULL;
	rb->pcm_buf = NULL;
	rb->pcm_tx = NULL;
	rb->pcm_tbuf = NULL;
	if (rb->pcm)
		iounmap(rb->pcm);
	rb->pcm = NULL;
	kvfree(rb->pcm_copy);
	kvfree(rb->pcm_values);
	rb->pcm_copy = NULL;
	rb->pcm_values = NULL;
	rb->i2s_div = 0;
}

/* BCM2711 GPIO block: function select and pull registers. */
#define RB_GPFSEL(pin) (0x00 + ((pin) / 10) * 4)
#define RB_GPFSEL_SHIFT(pin) (((pin) % 10) * 3)
#define RB_GPFSEL_ALT0 4u
#define RB_GPPULL(pin) (0xe4 + ((pin) / 16) * 4)
#define RB_GPPULL_SHIFT(pin) (((pin) % 16) * 2)
#define RB_I2S_PIN_FIRST 18 /* GPIO 18 to 21: bit clock, frame sync, data in, data out */
#define RB_I2S_PINS 4

/*
 * Puts GPIO 18 to 21 in their PCM function (alt 0) with no pulls. They are
 * the RadioBerry's wires to the FPGA and nothing else claims them; the
 * block's own pin control driver is not asked because the PCM node, whose
 * pin states would carry them, stays disabled.
 */
static int rb_i2s_pins(struct rb_dev *rb)
{
	struct device_node *np;
	void __iomem *gpio;
	unsigned int pin;
	u32 v;

	np = of_find_compatible_node(NULL, NULL, "brcm,bcm2711-gpio");
	if (!np) {
		dev_err(rb->dev, "no BCM2711 GPIO block in the device tree\n");
		return -ENODEV;
	}
	gpio = of_iomap(np, 0);
	of_node_put(np);
	if (!gpio)
		return -ENOMEM;
	for (pin = RB_I2S_PIN_FIRST; pin < RB_I2S_PIN_FIRST + RB_I2S_PINS; pin++) {
		v = readl(gpio + RB_GPPULL(pin));
		v &= ~(3u << RB_GPPULL_SHIFT(pin));
		writel(v, gpio + RB_GPPULL(pin));
		v = readl(gpio + RB_GPFSEL(pin));
		v &= ~(7u << RB_GPFSEL_SHIFT(pin));
		v |= RB_GPFSEL_ALT0 << RB_GPFSEL_SHIFT(pin);
		writel(v, gpio + RB_GPFSEL(pin));
	}
	iounmap(gpio);
	return 0;
}

/*
 * The BCM2711's PCM block, found by its compatible string. Its node must
 * stay disabled (no audio overlay or dtparam=i2s=on), so the stock I2S
 * driver never binds it and no sound card exists; only its registers and
 * its DMA requests are used.
 */
static int rb_i2s_up(struct rb_dev *rb)
{
	struct device_node *np;
	struct resource res;
	unsigned int receivers, rate_code;
	unsigned long flags;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "brcm,bcm2835-i2s");
	if (!np) {
		dev_err(rb->dev, "no PCM block in the device tree\n");
		return -ENODEV;
	}
	if (of_device_is_available(np)) {
		dev_err(rb->dev,
			"the PCM block is enabled for audio (dtparam=i2s=on or an audio overlay); the RadioBerry needs it\n");
		ret = -EBUSY;
		goto out;
	}
	ret = rb_i2s_pins(rb);
	if (ret)
		goto out;
	ret = of_address_to_resource(np, 0, &res);
	if (ret)
		goto out;
	rb->pcm = ioremap(res.start, resource_size(&res));
	rb->pcm_fifo = res.start + RB_PCM_FIFO;
	rb->pcm_rx = of_dma_request_slave_channel(np, "rx");
	if (IS_ERR(rb->pcm_rx)) {
		ret = PTR_ERR(rb->pcm_rx);
		rb->pcm_rx = NULL;
		dev_err(rb->dev, "no RX DMA channel for the PCM block: %d\n", ret);
		goto out;
	}
	rb->pcm_tx = of_dma_request_slave_channel(np, "tx");
	if (IS_ERR(rb->pcm_tx)) {
		ret = PTR_ERR(rb->pcm_tx);
		rb->pcm_tx = NULL;
		dev_err(rb->dev, "no TX DMA channel for the PCM block: %d\n", ret);
		goto out;
	}
	rb->pcm_buf = dma_alloc_coherent(rb->pcm_rx->device->dev,
					 (size_t)RB_I2S_MAX_PERIOD * 4 * RB_I2S_PERIODS,
					 &rb->pcm_buf_dma, GFP_KERNEL);
	rb->pcm_tbuf = dma_alloc_coherent(rb->pcm_tx->device->dev,
					  (size_t)RB_I2S_MAX_PERIOD * 4 * RB_I2S_PERIODS,
					  &rb->pcm_tbuf_dma, GFP_KERNEL);
	rb->pcm_copy = kvcalloc(RB_I2S_MAX_PERIOD, sizeof(u32), GFP_KERNEL);
	rb->pcm_values = kvcalloc(RB_I2S_MAX_PERIOD, sizeof(u32), GFP_KERNEL);
	if (!rb->pcm || !rb->pcm_buf || !rb->pcm_tbuf || !rb->pcm_copy || !rb->pcm_values) {
		ret = -ENOMEM;
		goto out;
	}

	spin_lock_irqsave(&rb->rx_lock, flags);
	receivers = rb->receivers;
	rate_code = rb->rate_code;
	spin_unlock_irqrestore(&rb->rx_lock, flags);
	mutex_lock(&rb->cmd_lock);
	rb->i2s_div_want = rb_i2s_div(rate_code, receivers);
	if (rb->rx_paused)
		ret = rb_i2s_link_stop(rb, rb->i2s_div_want);
	else
		ret = rb_i2s_set_div(rb, rb->i2s_div_want, false);
	mutex_unlock(&rb->cmd_lock);
	if (ret)
		goto out;
	of_node_put(np);
	dev_info(rb->dev, "RX and TX over I2S with the PCM block, RX up to %u values per second\n",
		 RB_I2S_VALUES_MAX);
	return 0;

out:
	of_node_put(np);
	rb_i2s_down(rb);
	return ret;
}

/*
 * Run clear: nothing streams, so nothing runs. The PCM block and both DMA
 * channels stop and the image's link is disabled: no bit clock, no DMA, no
 * interrupts until run is set again. Called with cmd_lock held.
 */
static void rb_i2s_rx_pause(struct rb_dev *rb)
{
	lockdep_assert_held(&rb->cmd_lock);
	if (!rb->pcm)
		return;
	if (rb_i2s_link_stop(rb, rb->i2s_div_want))
		dev_err_ratelimited(rb->dev, "stopping the I2S link failed\n");
	rb_trace(rb, "I2S link and DMA stopped: run clear\n");
}

static void rb_i2s_rx_resume(struct rb_dev *rb)
{
	lockdep_assert_held(&rb->cmd_lock);
	if (!rb->pcm)
		return;
	if (rb_i2s_set_div(rb, rb->i2s_div_want, true))
		dev_err_ratelimited(rb->dev, "starting the I2S link failed\n");
}

static const struct rb_transport rb_i2s_transport = {
	.name = "i2s",
	.rx_values_max = RB_I2S_VALUES_MAX,
	.up = rb_i2s_up,
	.down = rb_i2s_down,
	.rx_pause = rb_i2s_rx_pause,
	.rx_resume = rb_i2s_rx_resume,
	.firmware = RB_I2S_FIRMWARE,
	.caps_transport = RB_CAPS_TRANSPORT_I2S,
};

static const struct rb_transport rb_pio_transport = {
	.name = "rp1-pio",
	.rx_values_max = RB_PIO_RX_VALUES_MAX,
	.up = rb_pio_up,
	.down = rb_pio_down,
	.rx_pause = rb_pio_rx_pause,
	.rx_resume = rb_pio_rx_resume,
	.firmware = RB_PIO_FIRMWARE,
	.caps_transport = RB_CAPS_TRANSPORT_PIO,
};

/* The transport for this SoC, or NULL. */
static const struct rb_transport *rb_select_transport(void)
{
	if (of_machine_is_compatible("brcm,bcm2712"))
		return &rb_pio_transport;
	if (of_machine_is_compatible("brcm,bcm2711"))
		return &rb_i2s_transport;
	return NULL;
}

static u32 rb_rx_values_max(const struct rb_dev *rb)
{
	return rb->tp->rx_values_max;
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

/*
 * The general settings a fresh client assumes: one receiver at 48 ksps.
 * Sent on close, so a client that left more receivers or another rate
 * behind, or was killed, cannot leave the next one reading frames of the
 * wrong layout: a client that wants the defaults never sends them.
 */
static void rb_observe_command(struct rb_dev *rb, const u8 *tx);

static void rb_command_defaults(struct rb_dev *rb)
{
	u8 tx[RB_CMD_BYTES] = { 0 }; /* address 0, data 0, run clear */
	int ret;

	mutex_lock(&rb->cmd_lock);
	ret = rb_command_locked(rb, tx, NULL);
	if (!ret) {
		rb_observe_command(rb, tx);
		rb_rx_gate(rb);
	}
	mutex_unlock(&rb->cmd_lock);
	if (ret)
		dev_err(rb->dev, "default settings on close failed: %d\n", ret);
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
	rb_command_defaults(rb);
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
		info.transport = rb->tp == &rb_i2s_transport ? RADIOBERRY_TRANSPORT_I2S :
							       RADIOBERRY_TRANSPORT_PIO;
		info.rx_values_max = rb_rx_values_max(rb);
		spin_lock_irqsave(&rb->rx_lock, flags);
		info.active_receivers = (u8)rb->receivers;
		info.rx_rate_code = (u8)rb->rate_code;
		spin_unlock_irqrestore(&rb->rx_lock, flags);
		return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
	}
	case RADIOBERRY_IOC_COMMAND: {
		struct radioberry_command c;

		if (copy_from_user(&c, uarg, sizeof(c)))
			return -EFAULT;
		rb_cmd_sanitise(c.tx);
		mutex_lock(&rb->cmd_lock);
		ret = rb_command_locked(rb, c.tx, c.rx);
		if (!ret) {
			rb->stats.commands++;
			rb_observe_command(rb, c.tx);
			rb_rx_gate(rb);
		}
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

/* How the streams reach the gateware: "rp1-pio" (Pi 5) or "i2s" (Pi 4). */
static ssize_t transport_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rb_dev *rb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", rb->tp->name);
}
static DEVICE_ATTR_RO(transport);

/*
 * 1 puts the image's AD9866 in digital loopback: TxDAC and IAMP powered
 * down, every PA line held low, transmit samples back on the receive bus.
 * For a self-test of the transmit path; the transmitter still has to be
 * keyed (PTT and permission) for samples to flow. Written to the image at
 * once.
 */
static ssize_t tx_loopback_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rb_dev *rb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(rb->loopback) ? 1 : 0);
}

static ssize_t tx_loopback_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct rb_dev *rb = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	mutex_lock(&rb->cmd_lock);
	WRITE_ONCE(rb->loopback, on);
	ret = rb->flags_sent ? rb_write_control(rb) : 0;
	mutex_unlock(&rb->cmd_lock);
	return ret ? ret : (ssize_t)count;
}
static DEVICE_ATTR_RW(tx_loopback);

/*
 * The image's link and transmit counters and the driver's own. On the Pi 5
 * words_to_pi counts values, and tx_seq_gaps, tx_bad_frames, tx_realigns
 * and the driver_ lines stay 0.
 */
static ssize_t counters_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	static const struct {
		u8 reg;
		const char *name;
	} regs[] = {
		{ RB_BREG_I2S_TX, "words_to_pi" },  { RB_BREG_I2S_RX, "tx_samples" },
		{ RB_BREG_SEQ_GAPS, "tx_seq_gaps" }, { RB_BREG_TX_BAD, "tx_bad_frames" },
		{ RB_BREG_TX_OVF, "tx_overflows" },  { RB_BREG_TX_UNDER, "tx_underruns" },
		{ RB_BREG_REALIGNS, "tx_realigns" }, { RB_BREG_STATUS2, "status2" },
		{ RB_BREG_CONTROL, "control" },
		{ RB_BREG_KEYING, "keying" },	     { RB_BREG_I2S, "link" },
	};
	struct rb_dev *rb = dev_get_drvdata(dev);
	ssize_t n = 0;
	unsigned int i;
	u32 v;
	int ret = 0;

	mutex_lock(&rb->cmd_lock);
	for (i = 0; i < ARRAY_SIZE(regs) && !ret; i++) {
		ret = rb_breg_read(rb, regs[i].reg, &v);
		if (!ret)
			n += sysfs_emit_at(buf, n, "%s 0x%08x %u\n", regs[i].name, v, v);
	}
	mutex_unlock(&rb->cmd_lock);
	if (ret)
		return ret;
	n += sysfs_emit_at(buf, n, "driver_tx_late %llu\ndriver_tx_fifo_errors %llu\n",
			   READ_ONCE(rb->pcm_tx_late), READ_ONCE(rb->pcm_tx_fifo_errors));
	return n;
}
static DEVICE_ATTR_RO(counters);

static struct attribute *rb_attrs[] = {
	&dev_attr_gateware_version.attr,
	&dev_attr_fpga_type.attr,
	&dev_attr_receivers.attr,
	&dev_attr_transmitters.attr,
	&dev_attr_reload.attr,
	&dev_attr_rx_stalled.attr,
	&dev_attr_rx_stalls.attr,
	&dev_attr_transport.attr,
	&dev_attr_tx_loopback.attr,
	&dev_attr_counters.attr,
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
	rb->frame_tx = kzalloc(RB_FRAME_BYTES, GFP_KERNEL);
	rb->frame_rx = kzalloc(RB_FRAME_BYTES, GFP_KERNEL);
	if (!rb->rx_ring_buf || !rb->tx_ring_buf || !rb->io_buf || !rb->pack_buf ||
	    !rb->frame_tx || !rb->frame_rx)
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
	kfree(rb->frame_tx);
	kfree(rb->frame_rx);
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
	spin_lock_init(&rb->rx_lock);
	spin_lock_init(&rb->tx_lock);
	init_waitqueue_head(&rb->rx_wait);
	init_waitqueue_head(&rb->tx_wait);
	init_waitqueue_head(&rb->work_wait);
	INIT_DELAYED_WORK(&rb->keepalive, rb_keepalive);
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
	cancel_delayed_work_sync(&rb->keepalive);
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
	cancel_delayed_work_sync(&rb->keepalive);
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
