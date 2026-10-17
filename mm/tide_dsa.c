// SPDX-License-Identifier: GPL-2.0
/*
 * TIDE DSA copy backend.
 *
 * Copies the pages of a migration transaction with Intel DSA work queues
 * through dmaengine.  A bounded number of DSA slots admits transactions to
 * the DSA lane; an admitted transaction is submitted as one hardware batch
 * descriptor when the idxd driver provides one, or as one memcpy descriptor
 * per page pair otherwise.  Failed copies are reported back so that the
 * caller can redo them on the CPU.
 */

#define pr_fmt(fmt) "tide: " fmt

#include <linux/bitmap.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/sysctl.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tide_dsa.h>
#include <linux/vmstat.h>
#include <linux/wait.h>
#include "../drivers/dma/idxd/idxd.h"

#define TIDE_DSA_MAX_CHANNELS 8
#define TIDE_DSA_COPY_TIMEOUT_MS 5000

/* tide_dsa.disable=1 on the kernel command line keeps every copy on the CPU. */
static bool tide_dsa_disabled __read_mostly;
module_param_named(disable, tide_dsa_disabled, bool, 0444);
MODULE_PARM_DESC(disable, "Disable TIDE DSA copy offload (default: false)");

struct tide_dsa_copy_req {
	struct dmaengine_unmap_data *unmap;
	dma_cookie_t cookie;
	unsigned long deadline;
	unsigned long completed_jiffies;
	bool completed;
	struct completion done;
	struct dmaengine_result result;
};

/* Serializes one-time DMA channel discovery only; never held across DMA. */
static DEFINE_MUTEX(tide_dsa_lock);
static DEFINE_SPINLOCK(tide_dsa_slot_lock);
static DECLARE_WAIT_QUEUE_HEAD(tide_dsa_admission_wq);
static unsigned int tide_dsa_slot_depth;
static struct dma_chan *tide_dsa_channels[TIDE_DSA_MAX_CHANNELS];
static unsigned int tide_dsa_nr_channels;

/*
 * Batch descriptor support is provided by the DMA driver through
 * tide_dsa_batch_ops.  mm/ maps the pages, hands bus addresses to the
 * provider and waits on the single tx the provider returns; it never sees
 * a hardware descriptor.  Contexts are pooled because every admitted
 * transaction needs exactly one and the number of DSA slots is small.
 */
#define TIDE_DSA_BATCH_MAX_ENTRIES 128

struct tide_dsa_batch_ctx {
	struct list_head node;
	struct dma_chan *chan;
	void *provider;
	unsigned int max_entries;
	struct tide_dsa_batch_entry entries[TIDE_DSA_BATCH_MAX_ENTRIES];
};

static const struct tide_dsa_batch_ops *tide_dsa_batch_ops __read_mostly;
static DEFINE_SPINLOCK(tide_dsa_batch_pool_lock);
static LIST_HEAD(tide_dsa_batch_pool);
static atomic_t tide_dsa_batch_chan_rr = ATOMIC_INIT(0);

int tide_dsa_register_batch_ops(const struct tide_dsa_batch_ops *ops)
{
	if (!ops || !ops->max_entries || !ops->alloc || !ops->free ||
	    !ops->prep || !ops->entry_status)
		return -EINVAL;
	if (cmpxchg(&tide_dsa_batch_ops, NULL, ops))
		return -EBUSY;
	pr_info("DSA batch descriptor provider registered\n");
	return 0;
}
EXPORT_SYMBOL_GPL(tide_dsa_register_batch_ops);

void tide_dsa_unregister_batch_ops(const struct tide_dsa_batch_ops *ops)
{
	struct tide_dsa_batch_ctx *ctx, *tmp;
	unsigned long flags;
	LIST_HEAD(drain);

	if (!ops || cmpxchg(&tide_dsa_batch_ops, ops, NULL) != ops)
		return;

	spin_lock_irqsave(&tide_dsa_batch_pool_lock, flags);
	list_splice_init(&tide_dsa_batch_pool, &drain);
	spin_unlock_irqrestore(&tide_dsa_batch_pool_lock, flags);
	list_for_each_entry_safe(ctx, tmp, &drain, node) {
		ops->free(ctx->chan, ctx->provider);
		kfree(ctx);
	}
}
EXPORT_SYMBOL_GPL(tide_dsa_unregister_batch_ops);

static struct tide_dsa_batch_ctx *
tide_dsa_get_batch_ctx(const struct tide_dsa_batch_ops *ops,
		       struct dma_chan *chan, unsigned int nr_entries)
{
	struct tide_dsa_batch_ctx *ctx = NULL, *iter;
	unsigned long flags;

	spin_lock_irqsave(&tide_dsa_batch_pool_lock, flags);
	list_for_each_entry(iter, &tide_dsa_batch_pool, node) {
		if (iter->chan == chan && iter->max_entries >= nr_entries) {
			list_del(&iter->node);
			ctx = iter;
			break;
		}
	}
	spin_unlock_irqrestore(&tide_dsa_batch_pool_lock, flags);
	if (ctx)
		return ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	INIT_LIST_HEAD(&ctx->node);
	ctx->chan = chan;
	ctx->max_entries = min_t(unsigned int, TIDE_DSA_BATCH_MAX_ENTRIES,
				 ops->max_entries(chan));
	if (ctx->max_entries < nr_entries)
		goto err_free;
	ctx->provider = ops->alloc(chan, ctx->max_entries);
	if (!ctx->provider)
		goto err_free;
	return ctx;

err_free:
	kfree(ctx);
	return NULL;
}

static void tide_dsa_put_batch_ctx(struct tide_dsa_batch_ctx *ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&tide_dsa_batch_pool_lock, flags);
	list_add(&ctx->node, &tide_dsa_batch_pool);
	spin_unlock_irqrestore(&tide_dsa_batch_pool_lock, flags);
}

static unsigned int tide_dsa_slot_limit(void)
{
	return clamp_t(unsigned int,
		       READ_ONCE(sysctl_numa_balancing_dsa_inflight_limit),
		       1, TIDE_DSA_MAX_SLOTS);
}

/*
 * The reservation depth counts the DSA slots in use.  An admitted
 * transaction owns one slot from admission until its DMA completions have
 * been consumed; the slot is the only bound on DSA concurrency, no lock
 * spans submit and wait.
 */
static bool
tide_dsa_reserve_service(struct tide_dsa_slot *token)
{
	unsigned long flags;
	bool admitted = false;

	spin_lock_irqsave(&tide_dsa_slot_lock, flags);
	if (tide_dsa_slot_depth <
	    tide_dsa_slot_limit()) {
		tide_dsa_slot_depth++;
		token->held = true;
		admitted = true;
	}
	spin_unlock_irqrestore(&tide_dsa_slot_lock, flags);

	return admitted;
}

static void
tide_dsa_release_service(struct tide_dsa_slot *token)
{
	unsigned long flags;

	spin_lock_irqsave(&tide_dsa_slot_lock, flags);
	if (!token->held || !tide_dsa_slot_depth) {
		spin_unlock_irqrestore(&tide_dsa_slot_lock, flags);
		return;
	}
	tide_dsa_slot_depth--;
	token->held = false;
	spin_unlock_irqrestore(&tide_dsa_slot_lock, flags);
	wake_up(&tide_dsa_admission_wq);
}

/* Non-blocking admission; a refused transaction stays on the CPU path. */
bool tide_dsa_try_admit(struct tide_dsa_slot *token)
{
	if (!token)
		return false;
	if (token->held)
		return true;
	return tide_dsa_reserve_service(token);
}

/* Blocking admission; callers must invoke this before unmapping pages. */
void tide_dsa_admit(struct tide_dsa_slot *token)
{
	if (!token || token->held)
		return;
	if (tide_dsa_reserve_service(token))
		return;

	wait_event(tide_dsa_admission_wq, tide_dsa_reserve_service(token));
}

void tide_dsa_release_admission(struct tide_dsa_slot *token)
{
	if (token && token->held)
		tide_dsa_release_service(token);
}

/*
 * Admission for the promotion executor.  Runs before the transaction is
 * isolated and unmapped, so a transaction that waits for, or is refused,
 * a DSA slot keeps its pages mapped meanwhile.  Timeout and partial flushes
 * stay on the CPU path.
 */
void tide_dsa_admit_promotion(const struct tide_migrate_ctx *promotion_context)
{
	unsigned int threshold;

	if (!promotion_context || !promotion_context->admission)
		return;
	if (tide_dsa_disabled)
		return;

	threshold = max_t(unsigned int, 1,
			  READ_ONCE(sysctl_numa_balancing_promote_batch_pages));
	switch (promotion_context->copy_mode) {
	case TIDE_COPY_DYNAMIC:
		/*
		 * Only a full transaction may take the DSA lane: one built to
		 * the configured size whose input is either exactly that many
		 * base pages or a compound page covering at least that many.
		 */
		if (!promotion_context->full_dispatch ||
		    promotion_context->target_pages != threshold ||
		    promotion_context->nr_input_pages < threshold)
			break;
		tide_dsa_try_admit(promotion_context->admission);
		break;
	case TIDE_COPY_DSA:
		if (promotion_context->nr_input_pages >= threshold)
			tide_dsa_admit(promotion_context->admission);
		break;
	default:
		break;
	}
}

/*
 * Admission for a caller that is blocked on the migration and cannot defer
 * it: reclaim-driven demotion and huge-page hint faults.  Only the dynamic
 * copy mode is handled here, and only transactions that reach the size
 * threshold of their direction take the DSA lane.
 */
void tide_dsa_admit_blocked(const struct tide_migrate_ctx *promotion_context,
			    bool to_toptier)
{
	unsigned int threshold;

	if (!promotion_context || !promotion_context->admission)
		return;
	if (tide_dsa_disabled)
		return;
	if (promotion_context->copy_mode != TIDE_COPY_DYNAMIC)
		return;

	/*
	 * A promotion transaction is built to the configured size; a demotion
	 * holds whatever one reclaim pass isolated, which is much smaller, so
	 * it uses its own fixed threshold.
	 */
	threshold = to_toptier ?
		max_t(unsigned int, 1,
		      READ_ONCE(sysctl_numa_balancing_promote_batch_pages)) :
		TIDE_DSA_DEMOTE_BATCH_PAGES;
	if (promotion_context->nr_input_pages < threshold)
		return;

	tide_dsa_try_admit(promotion_context->admission);
}

unsigned int tide_get_copy_mode(void)
{
	return READ_ONCE(sysctl_numa_balancing_copy_mode);
}

static void tide_dsa_complete_result(void *param,
				     const struct dmaengine_result *result)
{
	struct tide_dsa_copy_req *req = param;

	if (!result) {
		req->result.result = DMA_TRANS_ABORTED;
		req->result.residue = 0;
	} else {
		req->result = *result;
	}

	req->completed_jiffies = jiffies;
	req->completed = true;
	complete(&req->done);
}

/* Accept only enabled, dedicated, kernel-type idxd work queues. */
static bool tide_dsa_filter_chan(struct dma_chan *chan, void *param)
{
	struct idxd_dma_chan *idxd_chan;
	struct idxd_wq *wq;

	if (!chan)
		return false;

	idxd_chan = container_of(chan, struct idxd_dma_chan, chan);
	wq = idxd_chan->wq;
	if (!wq)
		return false;

	return wq->state == IDXD_WQ_ENABLED && wq->type == IDXD_WQT_KERNEL &&
	       wq_dedicated(wq);
}

static int tide_dsa_init_channels_locked(void)
{
	dma_cap_mask_t mask;
	unsigned int nr_channels = 0;
	unsigned int i;

	lockdep_assert_held(&tide_dsa_lock);

	if (tide_dsa_nr_channels)
		return 0;

	if (tide_dsa_disabled) {
		pr_info_once("DSA copy offload disabled by boot parameter\n");
		return -ENODEV;
	}

	/* Defer channel discovery until the system is fully up. */
	if (system_state < SYSTEM_RUNNING)
		return -EAGAIN;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	for (i = 0; i < TIDE_DSA_MAX_CHANNELS; i++) {
		struct dma_chan *chan;

		chan = dma_request_channel(mask, tide_dsa_filter_chan, NULL);
		if (!chan)
			break;
		tide_dsa_channels[nr_channels++] = chan;
	}

	if (!nr_channels) {
		pr_warn("no DSA memcpy channel available, falling back to CPU copy\n");
		return -ENODEV;
	}

	/* Publish the channel table before its count becomes visible. */
	smp_store_release(&tide_dsa_nr_channels, nr_channels);
	pr_info("initialized %u DSA channels\n", nr_channels);
	return 0;
}

/* Discovers the DMA channels once; later callers only read the table. */
static int tide_dsa_ensure_channels(void)
{
	int rc;

	/* Pairs with smp_store_release() in tide_dsa_init_channels_locked(). */
	if (smp_load_acquire(&tide_dsa_nr_channels))
		return 0;

	mutex_lock(&tide_dsa_lock);
	rc = tide_dsa_init_channels_locked();
	mutex_unlock(&tide_dsa_lock);
	return rc;
}

static int tide_dsa_wait_for_req(struct tide_dsa_copy_req *req)
{
	unsigned long remaining = 0;
	bool timed_out;
	long timeout;
	int rc = 0;

	if (time_before(jiffies, req->deadline))
		remaining = req->deadline - jiffies;
	timeout = wait_for_completion_timeout(&req->done, remaining);
	timed_out = !timeout || time_after(req->completed_jiffies, req->deadline);
	if (timed_out) {
		pr_warn_ratelimited("timed out waiting for DSA cookie %d\n",
				    req->cookie);
		/* The provider cannot terminate DMA; its callback is the quiescence point. */
		if (!timeout)
			wait_for_completion(&req->done);
	}

	if (!req->completed || req->result.result != DMA_TRANS_NOERROR) {
		pr_warn_ratelimited("DSA copy failed for cookie %d result=%d residue=%u\n",
				    req->cookie, req->result.result, req->result.residue);
		rc = req->result.result == DMA_TRANS_ABORTED ? -ECANCELED : -EIO;
	}

	dmaengine_unmap_put(req->unmap);
	req->unmap = NULL;
	return rc;
}

static void tide_dsa_init_req(struct tide_dsa_copy_req *req)
{
	req->unmap = NULL;
	req->cookie = 0;
	req->deadline = 0;
	req->completed_jiffies = 0;
	req->completed = false;
	init_completion(&req->done);
	req->result.result = DMA_TRANS_ABORTED;
	req->result.residue = 0;
}

static int tide_dsa_submit_tx(struct dma_chan *chan,
			      struct dma_async_tx_descriptor *tx,
			      struct tide_dsa_copy_req *req)
{
	tx->callback = NULL;
	tx->callback_result = tide_dsa_complete_result;
	tx->callback_param = req;

	req->deadline = jiffies + msecs_to_jiffies(TIDE_DSA_COPY_TIMEOUT_MS);
	req->cookie = dmaengine_submit(tx);
	if (dma_submit_error(req->cookie))
		return -EIO;

	dma_async_issue_pending(chan);
	return 0;
}

static int tide_dsa_submit_copy(struct dma_chan *chan, struct page *dst,
				struct page *src, unsigned int nr_pages,
				struct tide_dsa_copy_req *req)
{
	struct dma_async_tx_descriptor *tx;
	struct dmaengine_unmap_data *unmap;
	struct device *dev = chan->device->dev;
	size_t len = (size_t)nr_pages << PAGE_SHIFT;
	dma_addr_t src_dma, dst_dma;

	tide_dsa_init_req(req);

	unmap = dmaengine_get_unmap_data(dev, 2, GFP_KERNEL);
	if (!unmap)
		return -ENOMEM;

	unmap->len = len;
	src_dma = dma_map_page(dev, src, 0, len, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, src_dma))
		goto err_unmap;

	unmap->addr[0] = src_dma;
	unmap->to_cnt = 1;

	dst_dma = dma_map_page(dev, dst, 0, len, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, dst_dma))
		goto err_unmap;

	unmap->addr[1] = dst_dma;
	unmap->from_cnt = 1;

	tx = dmaengine_prep_dma_memcpy(chan, dst_dma, src_dma, len,
				       DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!tx)
		goto err_unmap;

	if (tide_dsa_submit_tx(chan, tx, req))
		goto err_unmap;

	req->unmap = unmap;
	return 0;

err_unmap:
	dmaengine_unmap_put(unmap);
	return -EIO;
}

static void tide_dsa_unmap_batch_entries(struct dma_chan *chan,
					 struct tide_dsa_batch_ctx *ctx,
					 unsigned int nr_entries)
{
	struct device *dev = chan->device->dev;
	unsigned int i;

	for (i = 0; i < nr_entries; i++) {
		dma_unmap_page(dev, ctx->entries[i].dst, ctx->entries[i].len,
			       DMA_FROM_DEVICE);
		dma_unmap_page(dev, ctx->entries[i].src, ctx->entries[i].len,
			       DMA_TO_DEVICE);
	}
}

/*
 * One transaction, one provider batch descriptor.  Either every pair is
 * handed to the provider or nothing is submitted, so a failure here lets
 * the caller fall back to per-pair descriptors with the pages untouched.
 */
static int tide_dsa_submit_batch_desc(const struct tide_dsa_batch_ops *ops,
				      struct dma_chan *chan,
				      struct tide_dsa_batch_ctx *ctx,
				      struct tide_dsa_copy_pair *pairs,
				      unsigned int nr_pairs,
				      struct tide_dsa_copy_req *req)
{
	struct device *dev = chan->device->dev;
	struct dma_async_tx_descriptor *tx;
	unsigned int mapped;

	tide_dsa_init_req(req);

	for (mapped = 0; mapped < nr_pairs; mapped++) {
		struct tide_dsa_batch_entry *entry = &ctx->entries[mapped];

		entry->len = (size_t)pairs[mapped].nr_pages << PAGE_SHIFT;
		entry->src = dma_map_page(dev, pairs[mapped].src, 0, entry->len,
					  DMA_TO_DEVICE);
		if (dma_mapping_error(dev, entry->src))
			goto err_unmap;
		entry->dst = dma_map_page(dev, pairs[mapped].dst, 0, entry->len,
					  DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, entry->dst)) {
			dma_unmap_page(dev, entry->src, entry->len, DMA_TO_DEVICE);
			goto err_unmap;
		}
	}

	tx = ops->prep(chan, ctx->provider, ctx->entries, nr_pairs,
		       DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!tx)
		goto err_unmap;

	if (tide_dsa_submit_tx(chan, tx, req))
		goto err_unmap;

	return 0;

err_unmap:
	tide_dsa_unmap_batch_entries(chan, ctx, mapped);
	return -EIO;
}

static bool tide_dsa_batch_desc_eligible(unsigned int nr_pairs)
{
	/* A hardware batch needs at least two entries. */
	return nr_pairs >= 2 && nr_pairs <= TIDE_DSA_BATCH_MAX_ENTRIES;
}

/*
 * Returns -EAGAIN when nothing was submitted and the caller should use
 * per-pair descriptors; any other value means the provider ran the batch
 * and @completed_bitmap reflects the per-entry completion records.
 */
static int tide_dsa_copy_batch_desc(const struct tide_dsa_batch_ops *ops,
				    struct tide_dsa_copy_pair *pairs,
				    unsigned int nr_pairs,
				    unsigned long *completed_bitmap,
				    struct tide_dsa_copy_req *req,
				    unsigned int *nr_submitted)
{
	/* Pairs with smp_store_release() in tide_dsa_init_channels_locked(). */
	unsigned int nr_channels = smp_load_acquire(&tide_dsa_nr_channels);
	struct tide_dsa_batch_ctx *ctx;
	struct dma_chan *chan;
	unsigned int i;
	int first_error = 0;
	int rc;

	if (!nr_channels)
		return -EAGAIN;

	chan = tide_dsa_channels[atomic_inc_return(&tide_dsa_batch_chan_rr) %
				 nr_channels];
	ctx = tide_dsa_get_batch_ctx(ops, chan, nr_pairs);
	if (!ctx)
		return -EAGAIN;

	rc = tide_dsa_submit_batch_desc(ops, chan, ctx, pairs, nr_pairs, req);
	if (rc) {
		tide_dsa_put_batch_ctx(ctx);
		return -EAGAIN;
	}
	*nr_submitted = nr_pairs;

	rc = tide_dsa_wait_for_req(req);
	if (rc)
		first_error = rc;

	/* The provider is quiescent; per-entry completion records are authoritative. */
	for (i = 0; i < nr_pairs; i++) {
		if (ops->entry_status(ctx->provider, i)) {
			if (!first_error)
				first_error = -EIO;
			continue;
		}
		if (completed_bitmap)
			__set_bit(i, completed_bitmap);
		count_vm_events(PGMIGRATE_DSA_PAGES, pairs[i].nr_pages);
	}

	tide_dsa_unmap_batch_entries(chan, ctx, nr_pairs);
	tide_dsa_put_batch_ctx(ctx);
	return first_error;
}

/*
 * Submit every pair of one transaction, then wait for every submitted
 * request.  The caller owns a DSA slot; no lock is held across the DMA
 * round trip.
 */
static int tide_dsa_copy_batch_service(struct tide_dsa_copy_pair *pairs,
				       unsigned int nr_pairs,
				       unsigned long *completed_bitmap,
				       struct tide_dsa_copy_req *reqs,
				       unsigned int *nr_submitted)
{
	/* Pairs with smp_store_release() in tide_dsa_init_channels_locked(). */
	unsigned int nr_channels = smp_load_acquire(&tide_dsa_nr_channels);
	const struct tide_dsa_batch_ops *ops;
	unsigned int i;
	int first_error = 0;
	int rc;

	*nr_submitted = 0;
	if (!nr_channels)
		return -ENODEV;

	ops = READ_ONCE(tide_dsa_batch_ops);
	if (ops && tide_dsa_batch_desc_eligible(nr_pairs)) {
		rc = tide_dsa_copy_batch_desc(ops, pairs, nr_pairs,
					      completed_bitmap, &reqs[0],
					      nr_submitted);
		if (rc != -EAGAIN)
			return rc;
	}

	for (i = 0; i < nr_pairs; i++) {
		struct dma_chan *chan;

		chan = tide_dsa_channels[i % nr_channels];
		rc = tide_dsa_submit_copy(chan, pairs[i].dst, pairs[i].src,
					  pairs[i].nr_pages, &reqs[i]);
		if (rc) {
			first_error = rc;
			break;
		}
		(*nr_submitted)++;
	}

	for (i = 0; i < *nr_submitted; i++) {
		rc = tide_dsa_wait_for_req(&reqs[i]);
		if (rc) {
			if (!first_error)
				first_error = rc;
			continue;
		}
		if (completed_bitmap)
			__set_bit(i, completed_bitmap);
		count_vm_events(PGMIGRATE_DSA_PAGES, pairs[i].nr_pages);
	}

	return first_error;
}

/*
 * Blocking copy.  @token may carry a slot that the caller acquired before
 * unmapping; otherwise the slot is acquired here, which is only acceptable
 * for callers that could not admit in advance (demotion).
 */
static int
tide_dsa_copy_batch_common(struct tide_dsa_copy_pair *pairs,
			   unsigned int nr_pairs,
			   struct tide_dsa_slot *token)
{
	struct tide_dsa_slot local_token = {};
	struct tide_dsa_copy_req *reqs;
	unsigned int nr_submitted;
	int rc;

	if (!nr_pairs)
		return 0;

	rc = tide_dsa_ensure_channels();
	if (rc) {
		tide_dsa_release_admission(token);
		count_vm_event(PGMIGRATE_DSA_FALLBACK);
		return rc;
	}

	reqs = kvmalloc_array(nr_pairs, sizeof(*reqs), GFP_KERNEL | __GFP_ZERO);
	if (!reqs) {
		tide_dsa_release_admission(token);
		count_vm_event(PGMIGRATE_DSA_FALLBACK);
		return -ENOMEM;
	}

	if (!token)
		token = &local_token;
	tide_dsa_admit(token);
	rc = tide_dsa_copy_batch_service(pairs, nr_pairs, NULL, reqs,
					 &nr_submitted);
	if (rc)
		count_vm_event(PGMIGRATE_DSA_FALLBACK);
	tide_dsa_release_service(token);
	kvfree(reqs);
	return rc;
}

/*
 * Non-blocking copy for the dynamic copy mode.  With a caller-owned @token
 * the admission decision was made before unmapping and is only honoured
 * here; without a token a DSA slot is tried right now.  @completed_bitmap
 * marks the pairs that DSA copied; the caller copies the rest on the CPU.
 */
int tide_dsa_try_copy_batch_status(struct tide_dsa_copy_pair *pairs,
				   unsigned int nr_pairs,
				   unsigned long *completed_bitmap,
				   struct tide_dsa_slot *token,
				   enum tide_dsa_try_batch_disposition *disposition)
{
	struct tide_dsa_slot local_token = {};
	struct tide_dsa_copy_req *reqs;
	unsigned int nr_submitted;
	int rc;

	if (!completed_bitmap || !disposition)
		return -EINVAL;

	*disposition = TIDE_DSA_TRY_BATCH_UNAVAILABLE;
	if (!nr_pairs)
		return 0;

	bitmap_zero(completed_bitmap, nr_pairs);
	if (token) {
		if (!token->held) {
			*disposition = TIDE_DSA_TRY_BATCH_NO_SLOT;
			return 0;
		}
	} else {
		if (!tide_dsa_reserve_service(&local_token)) {
			*disposition = TIDE_DSA_TRY_BATCH_NO_SLOT;
			return 0;
		}
		token = &local_token;
	}

	rc = tide_dsa_ensure_channels();
	if (rc) {
		tide_dsa_release_service(token);
		count_vm_event(PGMIGRATE_DSA_FALLBACK);
		return rc;
	}

	reqs = kvmalloc_array(nr_pairs, sizeof(*reqs), GFP_KERNEL | __GFP_ZERO);
	if (!reqs) {
		tide_dsa_release_service(token);
		count_vm_event(PGMIGRATE_DSA_FALLBACK);
		return -ENOMEM;
	}

	rc = tide_dsa_copy_batch_service(pairs, nr_pairs, completed_bitmap,
					 reqs, &nr_submitted);
	if (nr_submitted)
		*disposition = TIDE_DSA_TRY_BATCH_DSA_CLAIMED;
	if (rc)
		count_vm_event(PGMIGRATE_DSA_FALLBACK);
	tide_dsa_release_service(token);
	kvfree(reqs);
	return rc;
}

int tide_dsa_copy_batch(struct tide_dsa_copy_pair *pairs, unsigned int nr_pairs)
{
	return tide_dsa_copy_batch_common(pairs, nr_pairs, NULL);
}

int tide_dsa_copy_batch_admitted(struct tide_dsa_copy_pair *pairs,
				 unsigned int nr_pairs,
				 struct tide_dsa_slot *token)
{
	return tide_dsa_copy_batch_common(pairs, nr_pairs, token);
}
