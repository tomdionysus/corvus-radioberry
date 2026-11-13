// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * rbsmi_dma: a measurement module for TODO/RPI4-SMI.md, Raspberry Pi 4
 * only. Reads the RadioBerry's RX bus with the BCM2711's SMI and a cyclic
 * DMA transfer into coherent memory, parses each DMA period with the
 * driver's own code (rb_smi.h, rb_logic.h) and counts what it finds, so
 * the stream's continuity and the CPU cost of SMI with DMA can be
 * measured. Not part of the driver and not installed.
 *
 * Needs the rbsmi-measure overlay (the SMI block enabled, its clock
 * running, the six RX pins in their SMI function), the radioberry overlay
 * off, and the gateware loaded and streaming (rbload, then a general
 * command with run set, sent over spidev). Loading starts the stream;
 * /sys/kernel/debug/rbsmi_dma/stats reports; unloading stops it.
 */
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_dma.h>
#include <linux/of_irq.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "rb_smi.h"

/* SMI registers (the Raspberry Pi kernel's bcm2835_smi.h names). */
#define SMICS 0x00
#define SMIL 0x04
#define SMIA 0x08
#define SMID 0x0c
#define SMIDSR0 0x10
#define SMIDC 0x30
#define SMICS_ENABLE BIT(0)
#define SMICS_DONE BIT(1)
#define SMICS_START BIT(3)
#define SMICS_CLEAR BIT(4)
#define SMICS_INTD BIT(9) /* interrupt when a transfer is done */
#define SMICS_RUN (SMICS_ENABLE | SMICS_INTD)
#define SMIDC_DMAEN BIT(28)
#define SMI_WIDTH_18 2u /* RWIDTH field value, measured with rbsmi */

static unsigned int setup = 2, strobe = 10, hold = 2, pace = 16;
module_param(setup, uint, 0444);
module_param(strobe, uint, 0444);
module_param(hold, uint, 0444);
module_param(pace, uint, 0444);
MODULE_PARM_DESC(setup, "read setup, SMI clock cycles (1-63)");
MODULE_PARM_DESC(strobe, "read strobe, SMI clock cycles (1-127)");
MODULE_PARM_DESC(hold, "read hold, SMI clock cycles (0-63)");
MODULE_PARM_DESC(pace, "read pace, SMI clock cycles (0-127)");

static unsigned int receivers = 1;
module_param(receivers, uint, 0444);
MODULE_PARM_DESC(receivers, "receivers the gateware streams, for the frame check");

static unsigned int period_words = 16384;
module_param(period_words, uint, 0444);
MODULE_PARM_DESC(period_words, "SMI words per DMA period");

/*
 * An SMI transfer has a fixed length and the strobes stop at its end.
 * START written while a transfer is active is ignored (measured with
 * polled reads); with DMA a transfer sometimes ends at its length and
 * sometimes runs on (measured, cause unknown), and an end does not fall
 * on a DMA period boundary. So the SMI's done interrupt restarts it.
 */
static unsigned int transfer_words = 1u << 30;
module_param(transfer_words, uint, 0444);
MODULE_PARM_DESC(transfer_words, "SMI words per transfer (default 2^30, about 2 minutes)");

#define RBSMI_PERIODS 32

struct rbsmi {
	void __iomem *regs;
	struct dma_chan *chan;
	dma_cookie_t cookie;
	u32 *buf;
	dma_addr_t buf_dma;
	size_t buf_bytes;
	unsigned int next_period; /* the next period to parse, 0..RBSMI_PERIODS-1 */
	u32 *copy; /* one period, cached */
	u32 *values;
	struct rb_smi_parser smi;
	struct rb_rx_parser rx;
	u64 frames, periods, callbacks;
	u64 restarts; /* from the done interrupt */
	u64 irqs;
	int irq;
	u64 stalls; /* watchdog restarts: no callback for a whole tick */
	u64 seen_callbacks; /* callbacks at the watchdog's last tick */
	struct timer_list watchdog;
	u32 bits_any;
	ktime_t start;
	spinlock_t lock; /* the counters, read by the stats file */
	struct dentry *dir;
};

static struct rbsmi *rbsmi;

/*
 * The period the DMA is filling, from the transfer's residue: callbacks
 * can arrive late and merged, so they are not counted as periods.
 */
static unsigned int rbsmi_filling(struct rbsmi *s)
{
	struct dma_tx_state state;
	size_t pos;

	dmaengine_tx_status(s->chan, s->cookie, &state);
	pos = s->buf_bytes - min_t(size_t, state.residue, s->buf_bytes);
	return (pos / ((size_t)period_words * 4)) % RBSMI_PERIODS;
}

static void rbsmi_period(void *arg)
{
	struct rbsmi *s = arg;
	unsigned int filling = rbsmi_filling(s);
	unsigned int n = (filling + RBSMI_PERIODS - s->next_period) % RBSMI_PERIODS;
	unsigned int i, k, got;
	unsigned long flags;
	u64 frames = 0;
	u32 any = 0;

	for (k = 0; k < n; k++) {
		const u32 *words = s->copy;

		/*
		 * The coherent buffer is uncached: one bulk copy into cached
		 * memory, then every word read from there.
		 */
		memcpy(s->copy, s->buf + (size_t)s->next_period * period_words,
		       (size_t)period_words * 4);
		got = rb_smi_parse(&s->smi, words, period_words, s->values);
		for (i = 0; i < period_words; i++)
			any |= words[i];
		for (i = 0; i < got; i++)
			if (rb_rx_feed(&s->rx, s->values[i]))
				frames++;
		s->next_period = (s->next_period + 1) % RBSMI_PERIODS;
	}

	spin_lock_irqsave(&s->lock, flags);
	s->frames += frames;
	s->periods += n;
	s->callbacks++;
	s->bits_any |= any;
	spin_unlock_irqrestore(&s->lock, flags);
}

/* A transfer ended: start the next at once; DONE is cleared by writing 1. */
static irqreturn_t rbsmi_irq(int irq, void *arg)
{
	struct rbsmi *s = arg;
	u32 cs = readl(s->regs + SMICS);

	if (!(cs & SMICS_DONE))
		return IRQ_NONE;
	writel(SMICS_RUN | SMICS_DONE | SMICS_START, s->regs + SMICS);
	spin_lock(&s->lock);
	s->irqs++;
	s->restarts++;
	spin_unlock(&s->lock);
	return IRQ_HANDLED;
}

/*
 * Every 20 ms: a whole tick without a callback means the stream stopped
 * (a period lasts 1.8 ms at the default timing). Log it with the SMI and
 * DMA state, count it and restart the SMI transfer.
 */
#define RBSMI_WATCHDOG_MS 20

static void rbsmi_watchdog(struct timer_list *t)
{
	struct rbsmi *s = timer_container_of(s, t, watchdog);
	struct dma_tx_state state;
	enum dma_status status;
	unsigned long flags;
	bool stalled;
	u32 cs;

	spin_lock_irqsave(&s->lock, flags);
	stalled = s->callbacks == s->seen_callbacks;
	s->seen_callbacks = s->callbacks;
	if (stalled)
		s->stalls++;
	spin_unlock_irqrestore(&s->lock, flags);
	if (stalled) {
		cs = readl(s->regs + SMICS);
		status = dmaengine_tx_status(s->chan, s->cookie, &state);
		pr_warn_ratelimited("rbsmi_dma: stalled after %llu periods: SMICS 0x%08x SMIL 0x%08x dma status %d residue %u next period %u\n",
				    s->periods, cs, readl(s->regs + SMIL), status, state.residue,
				    s->next_period);
		writel(SMICS_RUN | SMICS_DONE | SMICS_START, s->regs + SMICS);
	}
	mod_timer(&s->watchdog, jiffies + msecs_to_jiffies(RBSMI_WATCHDOG_MS));
}

static int rbsmi_stats_show(struct seq_file *m, void *v)
{
	struct rbsmi *s = m->private;
	unsigned long flags;
	u64 ns = ktime_to_ns(ktime_sub(ktime_get(), s->start));

	spin_lock_irqsave(&s->lock, flags);
	seq_printf(m, "seconds %llu.%03llu\n", ns / NSEC_PER_SEC,
		   (ns % NSEC_PER_SEC) / NSEC_PER_MSEC);
	seq_printf(m, "timing %u,%u,%u,%u\n", setup, strobe, hold, pace);
	seq_printf(m, "periods %llu\n", s->periods);
	seq_printf(m, "callbacks %llu\n", s->callbacks);
	seq_printf(m, "restarts %llu\n", s->restarts);
	seq_printf(m, "irqs %llu\n", s->irqs);
	seq_printf(m, "stalls %llu\n", s->stalls);
	seq_printf(m, "strobes %llu\n", s->smi.strobes);
	seq_printf(m, "idle_strobes %llu\n", s->smi.idle);
	seq_printf(m, "values %llu\n", s->smi.values);
	seq_printf(m, "frames %llu\n", s->frames);
	seq_printf(m, "resyncs %llu\n", s->rx.resyncs);
	seq_printf(m, "dropped_words %llu\n", s->rx.dropped);
	seq_printf(m, "discarded_words %llu\n", s->rx.discarded);
	seq_printf(m, "bits_seen 0x%x\n", s->bits_any);
	seq_printf(m, "smics 0x%08x\n", readl(s->regs + SMICS));
	seq_printf(m, "smil 0x%08x\n", readl(s->regs + SMIL));
	spin_unlock_irqrestore(&s->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rbsmi_stats);

/*
 * @phys_base is the SMI block's CPU physical address: this kernel's
 * bcm2835-dma maps a slave address itself, so it takes that rather than
 * the legacy bus address (0x7e600000) in the device tree.
 */
static int rbsmi_start(struct rbsmi *s, struct device_node *np, phys_addr_t phys_base)
{
	struct dma_slave_config cfg = {
		.direction = DMA_DEV_TO_MEM,
		.src_addr = phys_base + SMID,
		.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.src_maxburst = 1,
	};
	struct dma_async_tx_descriptor *desc;
	int ret;

	s->chan = of_dma_request_slave_channel(np, "rx-tx");
	if (IS_ERR(s->chan)) {
		ret = PTR_ERR(s->chan);
		s->chan = NULL;
		pr_err("rbsmi_dma: no DMA channel: %d\n", ret);
		return ret;
	}
	ret = dmaengine_slave_config(s->chan, &cfg);
	if (ret)
		return ret;

	s->buf_bytes = (size_t)period_words * 4 * RBSMI_PERIODS;
	s->buf = dma_alloc_coherent(s->chan->device->dev, s->buf_bytes, &s->buf_dma, GFP_KERNEL);
	if (!s->buf)
		return -ENOMEM;
	desc = dmaengine_prep_dma_cyclic(s->chan, s->buf_dma, s->buf_bytes,
					 (size_t)period_words * 4, DMA_DEV_TO_MEM,
					 DMA_PREP_INTERRUPT);
	if (!desc) {
		pr_err("rbsmi_dma: cannot prepare the cyclic transfer\n");
		return -EIO;
	}
	desc->callback = rbsmi_period;
	desc->callback_param = s;

	/* 18-bit reads at the given timing, DMA requests on, then start. */
	writel(0, s->regs + SMICS);
	writel(0, s->regs + SMIA);
	writel(SMI_WIDTH_18 << 30 | setup << 24 | hold << 16 | pace << 8 | strobe,
	       s->regs + SMIDSR0);
	writel(SMIDC_DMAEN | 32 << 18 | 32 << 12 | 1 << 6 | 63, s->regs + SMIDC);
	writel(transfer_words, s->regs + SMIL);
	writel(SMICS_ENABLE | SMICS_CLEAR, s->regs + SMICS);

	s->irq = of_irq_get(np, 0);
	if (s->irq <= 0) {
		pr_err("rbsmi_dma: no SMI interrupt: %d\n", s->irq);
		s->irq = 0;
		return -ENODEV;
	}
	ret = request_irq(s->irq, rbsmi_irq, 0, "rbsmi_dma", s);
	if (ret) {
		pr_err("rbsmi_dma: cannot have the SMI interrupt %d: %d\n", s->irq, ret);
		s->irq = 0;
		return ret;
	}

	s->cookie = dmaengine_submit(desc);
	dma_async_issue_pending(s->chan);
	s->start = ktime_get();
	writel(SMICS_RUN | SMICS_START, s->regs + SMICS);
	timer_setup(&s->watchdog, rbsmi_watchdog, 0);
	mod_timer(&s->watchdog, jiffies + msecs_to_jiffies(RBSMI_WATCHDOG_MS));
	return 0;
}

static void rbsmi_stop(struct rbsmi *s)
{
	if (s->watchdog.function)
		timer_delete_sync(&s->watchdog);
	if (s->regs)
		writel(0, s->regs + SMICS);
	if (s->irq)
		free_irq(s->irq, s);
	if (s->chan) {
		dmaengine_terminate_sync(s->chan);
		if (s->buf)
			dma_free_coherent(s->chan->device->dev, s->buf_bytes, s->buf, s->buf_dma);
		dma_release_channel(s->chan);
	}
	if (s->regs)
		writel(0, s->regs + SMIDC);
}

static int __init rbsmi_init(void)
{
	struct device_node *np;
	struct resource res;
	struct rbsmi *s;
	int ret;

	if (setup < 1 || setup > 63 || strobe < 1 || strobe > 127 || hold > 63 || pace > 127 ||
	    receivers < 1 || receivers > 6 || period_words < 64 || period_words > 262144 ||
	    transfer_words < 4096)
		return -EINVAL;
	np = of_find_compatible_node(NULL, NULL, "brcm,bcm2835-smi");
	if (!np || !of_device_is_available(np)) {
		of_node_put(np);
		pr_err("rbsmi_dma: no enabled SMI node (load the rbsmi-measure overlay)\n");
		return -ENODEV;
	}
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		of_node_put(np);
		return -ENOMEM;
	}
	spin_lock_init(&s->lock);
	rb_smi_init(&s->smi);
	rb_rx_init(&s->rx, receivers);
	rb_rx_set_receivers(&s->rx, receivers);
	s->values = kcalloc(rb_smi_max_values(period_words), sizeof(u32), GFP_KERNEL);
	s->copy = kvcalloc(period_words, sizeof(u32), GFP_KERNEL);
	ret = of_address_to_resource(np, 0, &res);
	if (!s->values || !s->copy || ret) {
		ret = s->values && s->copy ? -ENODEV : -ENOMEM;
		goto fail;
	}
	s->regs = ioremap(res.start, resource_size(&res));
	if (!s->regs) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = rbsmi_start(s, np, res.start);
	if (ret)
		goto fail;
	of_node_put(np);
	s->dir = debugfs_create_dir("rbsmi_dma", NULL);
	debugfs_create_file("stats", 0444, s->dir, s, &rbsmi_stats_fops);
	rbsmi = s;
	pr_info("rbsmi_dma: streaming, timing %u,%u,%u,%u, %u words a period\n", setup, strobe,
		hold, pace, period_words);
	return 0;

fail:
	rbsmi_stop(s);
	if (s->regs)
		iounmap(s->regs);
	kvfree(s->copy);
	kfree(s->values);
	kfree(s);
	of_node_put(np);
	return ret;
}

static void __exit rbsmi_exit(void)
{
	struct rbsmi *s = rbsmi;

	debugfs_remove_recursive(s->dir);
	rbsmi_stop(s);
	iounmap(s->regs);
	kvfree(s->copy);
	kfree(s->values);
	kfree(s);
}

module_init(rbsmi_init);
module_exit(rbsmi_exit);
MODULE_DESCRIPTION("RadioBerry RX over SMI with DMA: measurement only");
MODULE_LICENSE("GPL");
