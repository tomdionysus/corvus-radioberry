// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * rbpcm_dma: measurement module for the gateware-v2 I2S link test,
 * Raspberry Pi 4 only. Drives the BCM2711 PCM block directly as a clock
 * and frame slave (two 32-bit words in a 64-bit frame) with one cyclic
 * DMA transfer in each direction. It checks the word sequence the link
 * test image sends and sends it one of its own, so continuity both ways
 * and the CPU cost of the stream can be measured. No ALSA and no sound
 * card: the PCM node stays disabled and only its DMA requests are used.
 * Not part of any driver and not installed.
 *
 * Needs GPIO 18 to 21 in their PCM function (pinctrl set 18-21 a0) and
 * the link test image running its I2S tester. Loading starts the stream;
 * /sys/kernel/debug/rbpcm_dma/stats reports; unloading stops it.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_dma.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define PCM_CS 0x00
#define PCM_FIFO 0x04
#define PCM_MODE 0x08
#define PCM_RXC 0x0c
#define PCM_TXC 0x10
#define PCM_DREQ 0x14

#define CS_EN BIT(0)
#define CS_RXON BIT(1)
#define CS_TXON BIT(2)
#define CS_TXCLR BIT(3)
#define CS_RXCLR BIT(4)
#define CS_DMAEN BIT(9)
#define CS_TXERR BIT(15)
#define CS_RXERR BIT(16)
#define MODE_FSM BIT(21)
#define MODE_CLKM BIT(23)

static unsigned int period_words = 4096;
module_param(period_words, uint, 0444);
MODULE_PARM_DESC(period_words, "words per DMA period, each direction");

static unsigned int periods = 16;
module_param(periods, uint, 0444);
MODULE_PARM_DESC(periods, "periods in each cyclic buffer");

static unsigned int seq_mode = 1;
module_param(seq_mode, uint, 0444);
MODULE_PARM_DESC(seq_mode, "0 counting sequence, 1 xorshift32 (must match the image)");

static unsigned int txpos;
module_param(txpos, uint, 0444);
MODULE_PARM_DESC(txpos, "frame bit at which the Pi's first transmit word starts");

static u32 next_word(u32 w)
{
	if (!seq_mode)
		return w + 1;
	w ^= w << 13;
	w ^= w >> 17;
	w ^= w << 5;
	return w;
}

struct rbpcm {
	void __iomem *regs;
	struct dma_chan *rx_chan, *tx_chan;
	dma_cookie_t rx_cookie, tx_cookie;
	u32 *rx_buf, *tx_buf;
	dma_addr_t rx_dma, tx_dma;
	size_t buf_bytes;
	u32 *copy;
	unsigned int rx_next;	/* next RX period to check */
	size_t tx_last_pos;	/* TX DMA position at the last callback, bytes */
	u64 tx_consumed;	/* words the TX DMA has read, all time */
	u64 tx_written;		/* words written into the TX buffer, all time */
	u32 tx_word;
	u32 rx_prev;
	bool rx_seen;
	u64 rx_words, rx_breaks, rx_first_break;
	u32 break_got, break_want;
	u64 rx_errflags, tx_errflags;
	u64 callbacks, cb_ns, cb_max_ns;
	u64 late_tx;		/* callbacks that found the TX buffer nearly drained */
	ktime_t start;
	spinlock_t lock;
	struct dentry *dir;
};

static struct rbpcm *rbpcm;

static size_t dma_pos(struct dma_chan *chan, dma_cookie_t cookie, size_t bytes)
{
	struct dma_tx_state st;

	dmaengine_tx_status(chan, cookie, &st);
	return (bytes - min_t(size_t, st.residue, bytes)) % bytes;
}

/* Fill the TX buffer up to one period short of what the DMA has not read. */
static void rbpcm_tx_fill(struct rbpcm *s)
{
	size_t total = s->buf_bytes / 4;
	u64 limit = s->tx_consumed + total - period_words;

	while (s->tx_written < limit) {
		s->tx_buf[s->tx_written % total] = s->tx_word;
		s->tx_word = next_word(s->tx_word);
		s->tx_written++;
	}
}

static void rbpcm_period(void *arg)
{
	struct rbpcm *s = arg;
	ktime_t t0 = ktime_get();
	size_t total = s->buf_bytes / 4;
	unsigned int filling, n, k, i;
	size_t tx_pos;
	u64 words = 0, breaks = 0, ns;
	unsigned long flags;
	u32 cs;

	/* Receive: check every complete period. */
	filling = dma_pos(s->rx_chan, s->rx_cookie, s->buf_bytes) / (period_words * 4);
	n = (filling + periods - s->rx_next) % periods;
	for (k = 0; k < n; k++) {
		memcpy(s->copy, s->rx_buf + (size_t)s->rx_next * period_words,
		       (size_t)period_words * 4);
		for (i = 0; i < period_words; i++) {
			u32 w = s->copy[i];

			if (s->rx_seen && w != next_word(s->rx_prev)) {
				if (!s->rx_breaks && !breaks) {
					s->rx_first_break = s->rx_words + words;
					s->break_got = w;
					s->break_want = next_word(s->rx_prev);
				}
				breaks++;
			}
			s->rx_prev = w;
			s->rx_seen = true;
			words++;
		}
		s->rx_next = (s->rx_next + 1) % periods;
	}

	/* Transmit: account for what the DMA has read, then refill. */
	tx_pos = dma_pos(s->tx_chan, s->tx_cookie, s->buf_bytes);
	s->tx_consumed += ((tx_pos + s->buf_bytes - s->tx_last_pos) % s->buf_bytes) / 4;
	s->tx_last_pos = tx_pos;
	if (s->tx_written - s->tx_consumed < period_words)
		s->late_tx++;
	rbpcm_tx_fill(s);

	/* FIFO error flags: count and clear (write 1). */
	cs = readl(s->regs + PCM_CS);
	if (cs & (CS_RXERR | CS_TXERR))
		writel(cs & ~(CS_RXCLR | CS_TXCLR), s->regs + PCM_CS);

	ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	spin_lock_irqsave(&s->lock, flags);
	s->rx_words += words;
	s->rx_breaks += breaks;
	if (cs & CS_RXERR)
		s->rx_errflags++;
	if (cs & CS_TXERR)
		s->tx_errflags++;
	s->callbacks++;
	s->cb_ns += ns;
	if (ns > s->cb_max_ns)
		s->cb_max_ns = ns;
	spin_unlock_irqrestore(&s->lock, flags);
	(void)total;
}

static int rbpcm_stats_show(struct seq_file *m, void *v)
{
	struct rbpcm *s = m->private;
	u64 ns = ktime_to_ns(ktime_sub(ktime_get(), s->start));
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	seq_printf(m, "seconds %llu.%03llu\n", ns / NSEC_PER_SEC, (ns % NSEC_PER_SEC) / NSEC_PER_MSEC);
	seq_printf(m, "period_words %u\nperiods %u\nseq_mode %u\ntxpos %u\n", period_words, periods,
		   seq_mode, txpos);
	seq_printf(m, "rx_words %llu\n", s->rx_words);
	seq_printf(m, "rx_breaks %llu\n", s->rx_breaks);
	seq_printf(m, "rx_first_break_at %llu\n", s->rx_first_break);
	seq_printf(m, "rx_first_break_got 0x%08x\nrx_first_break_want 0x%08x\n", s->break_got,
		   s->break_want);
	seq_printf(m, "tx_words_consumed %llu\n", s->tx_consumed);
	seq_printf(m, "tx_late %llu\n", s->late_tx);
	seq_printf(m, "rx_fifo_errors %llu\ntx_fifo_errors %llu\n", s->rx_errflags, s->tx_errflags);
	seq_printf(m, "callbacks %llu\ncallback_ns_total %llu\ncallback_ns_max %llu\n", s->callbacks,
		   s->cb_ns, s->cb_max_ns);
	seq_printf(m, "pcm_cs 0x%08x\n", readl(s->regs + PCM_CS));
	spin_unlock_irqrestore(&s->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rbpcm_stats);

static struct dma_async_tx_descriptor *rbpcm_prep(struct dma_chan *chan, dma_addr_t buf,
						  size_t bytes, enum dma_transfer_direction dir,
						  phys_addr_t fifo, struct rbpcm *s, bool callback)
{
	struct dma_slave_config cfg = { .direction = dir };
	struct dma_async_tx_descriptor *desc;

	if (dir == DMA_DEV_TO_MEM) {
		cfg.src_addr = fifo;
		cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		cfg.src_maxburst = 2;
	} else {
		cfg.dst_addr = fifo;
		cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		cfg.dst_maxburst = 2;
	}
	if (dmaengine_slave_config(chan, &cfg))
		return NULL;
	desc = dmaengine_prep_dma_cyclic(chan, buf, bytes, (size_t)period_words * 4, dir,
					 callback ? DMA_PREP_INTERRUPT : 0);
	if (desc && callback) {
		desc->callback = rbpcm_period;
		desc->callback_param = s;
	}
	return desc;
}

static void rbpcm_stop(struct rbpcm *s)
{
	if (s->regs)
		writel(0, s->regs + PCM_CS);
	if (s->rx_chan) {
		dmaengine_terminate_sync(s->rx_chan);
		if (s->rx_buf)
			dma_free_coherent(s->rx_chan->device->dev, s->buf_bytes, s->rx_buf, s->rx_dma);
		dma_release_channel(s->rx_chan);
	}
	if (s->tx_chan) {
		dmaengine_terminate_sync(s->tx_chan);
		if (s->tx_buf)
			dma_free_coherent(s->tx_chan->device->dev, s->buf_bytes, s->tx_buf, s->tx_dma);
		dma_release_channel(s->tx_chan);
	}
}

static u32 rbpcm_chan(unsigned int pos1)
{
	unsigned int pos2 = (pos1 + 32) % 64;

	/* 32-bit channels: width extension set and width field 8 (8 + 16 + 8). */
	return BIT(31) | BIT(30) | pos1 << 20 | 8 << 16 | BIT(15) | BIT(14) | pos2 << 4 | 8;
}

static int rbpcm_start(struct rbpcm *s, struct device_node *np, phys_addr_t phys)
{
	struct dma_async_tx_descriptor *rx_desc, *tx_desc;
	int ret;

	s->rx_chan = of_dma_request_slave_channel(np, "rx");
	if (IS_ERR(s->rx_chan)) {
		ret = PTR_ERR(s->rx_chan);
		s->rx_chan = NULL;
		pr_err("rbpcm_dma: no RX DMA channel: %d\n", ret);
		return ret;
	}
	s->tx_chan = of_dma_request_slave_channel(np, "tx");
	if (IS_ERR(s->tx_chan)) {
		ret = PTR_ERR(s->tx_chan);
		s->tx_chan = NULL;
		pr_err("rbpcm_dma: no TX DMA channel: %d\n", ret);
		return ret;
	}
	s->buf_bytes = (size_t)period_words * 4 * periods;
	s->rx_buf = dma_alloc_coherent(s->rx_chan->device->dev, s->buf_bytes, &s->rx_dma, GFP_KERNEL);
	s->tx_buf = dma_alloc_coherent(s->tx_chan->device->dev, s->buf_bytes, &s->tx_dma, GFP_KERNEL);
	if (!s->rx_buf || !s->tx_buf)
		return -ENOMEM;

	s->tx_word = 0x1000;
	s->tx_written = 0;
	s->tx_consumed = 0;
	rbpcm_tx_fill(s);

	rx_desc = rbpcm_prep(s->rx_chan, s->rx_dma, s->buf_bytes, DMA_DEV_TO_MEM,
			     phys + PCM_FIFO, s, true);
	tx_desc = rbpcm_prep(s->tx_chan, s->tx_dma, s->buf_bytes, DMA_MEM_TO_DEV,
			     phys + PCM_FIFO, s, false);
	if (!rx_desc || !tx_desc) {
		pr_err("rbpcm_dma: cannot prepare the cyclic transfers\n");
		return -EIO;
	}

	/* Slave clock and frame, 64-bit frame, two 32-bit channels each way. */
	writel(0, s->regs + PCM_CS);
	writel(MODE_CLKM | MODE_FSM | 63 << 10 | 32, s->regs + PCM_MODE);
	writel(rbpcm_chan(0), s->regs + PCM_RXC);
	writel(rbpcm_chan(txpos), s->regs + PCM_TXC);
	writel(CS_EN, s->regs + PCM_CS);
	writel(CS_EN | CS_TXCLR | CS_RXCLR, s->regs + PCM_CS);
	usleep_range(1000, 2000);
	/* DMA request levels as the stock driver sets them. */
	writel(0x10 << 24 | 0x30 << 16 | 0x30 << 8 | 0x20, s->regs + PCM_DREQ);
	writel(CS_EN | CS_DMAEN | CS_TXERR | CS_RXERR, s->regs + PCM_CS);

	s->rx_cookie = dmaengine_submit(rx_desc);
	s->tx_cookie = dmaengine_submit(tx_desc);
	dma_async_issue_pending(s->rx_chan);
	dma_async_issue_pending(s->tx_chan);
	/* Let the TX DMA fill the FIFO before transmission starts. */
	usleep_range(1000, 2000);
	s->tx_last_pos = dma_pos(s->tx_chan, s->tx_cookie, s->buf_bytes);
	s->tx_consumed = s->tx_last_pos / 4;
	s->start = ktime_get();
	writel(CS_EN | CS_DMAEN | CS_RXON | CS_TXON, s->regs + PCM_CS);
	return 0;
}

static int __init rbpcm_init(void)
{
	struct device_node *np;
	struct resource res;
	struct rbpcm *s;
	int ret;

	if (period_words < 256 || period_words > 65536 || periods < 4 || periods > 64 ||
	    seq_mode > 1 || txpos > 63)
		return -EINVAL;
	np = of_find_compatible_node(NULL, NULL, "brcm,bcm2835-i2s");
	if (!np)
		return -ENODEV;
	if (of_device_is_available(np)) {
		/* The stock driver would own the block: refuse. */
		pr_err("rbpcm_dma: the PCM node is enabled; leave dtparam=i2s off\n");
		of_node_put(np);
		return -EBUSY;
	}
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		of_node_put(np);
		return -ENOMEM;
	}
	spin_lock_init(&s->lock);
	s->copy = kvcalloc(period_words, sizeof(u32), GFP_KERNEL);
	ret = of_address_to_resource(np, 0, &res);
	if (!s->copy || ret) {
		ret = s->copy ? -ENODEV : -ENOMEM;
		goto fail;
	}
	s->regs = ioremap(res.start, resource_size(&res));
	if (!s->regs) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = rbpcm_start(s, np, res.start);
	if (ret)
		goto fail;
	of_node_put(np);
	s->dir = debugfs_create_dir("rbpcm_dma", NULL);
	debugfs_create_file("stats", 0444, s->dir, s, &rbpcm_stats_fops);
	rbpcm = s;
	pr_info("rbpcm_dma: streaming at %pa, %u words a period, %u periods\n", &res.start,
		period_words, periods);
	return 0;

fail:
	rbpcm_stop(s);
	if (s->regs)
		iounmap(s->regs);
	kvfree(s->copy);
	kfree(s);
	of_node_put(np);
	return ret;
}

static void __exit rbpcm_exit(void)
{
	struct rbpcm *s = rbpcm;

	debugfs_remove_recursive(s->dir);
	rbpcm_stop(s);
	iounmap(s->regs);
	kvfree(s->copy);
	kfree(s);
}

module_init(rbpcm_init);
module_exit(rbpcm_exit);
MODULE_DESCRIPTION("RadioBerry gateware-v2 I2S link over the PCM block with DMA: measurement only");
MODULE_LICENSE("GPL");
