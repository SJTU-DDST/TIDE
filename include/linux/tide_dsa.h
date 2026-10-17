/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TIDE DSA copy backend interface.
 *
 * DSA slot admission and DSA page copies for migration transactions
 * (mm/tide_dsa.c), and the batch-descriptor provider interface that a DMA
 * driver (drivers/dma/idxd) registers.
 */
#ifndef _LINUX_TIDE_DSA_H
#define _LINUX_TIDE_DSA_H

#include <linux/errno.h>
#include <linux/migrate.h>
#include <linux/types.h>

struct dma_chan;
struct dma_async_tx_descriptor;
struct module;

/*
 * Upper bound on DSA slots, i.e. on migration transactions copied by DSA at
 * the same time.  The runtime value is kernel.numa_balancing_dsa_inflight_limit.
 * Admission happens before a transaction is unmapped; a transaction that
 * cannot get a slot stays on the CPU path.  Each admitted transaction uses
 * one provider batch descriptor.
 */
#define TIDE_DSA_MAX_SLOTS 32

/* Minimum demotion transaction, in base pages, that may take the DSA lane. */
#define TIDE_DSA_DEMOTE_BATCH_PAGES 8

/* One memmove inside a provider batch; mm/ maps the bus addresses. */
struct tide_dsa_batch_entry {
	dma_addr_t src;
	dma_addr_t dst;
	size_t len;
};

/*
 * Provider batch-descriptor support.  The DMA provider (drivers/dma/idxd)
 * owns every hardware descriptor, completion record and driver descriptor
 * object; mm/ only sees an opaque context plus the dmaengine tx it submits
 * and waits on like any other transfer.
 */
struct tide_dsa_batch_ops {
	struct module *owner;
	unsigned int (*max_entries)(struct dma_chan *chan);
	void *(*alloc)(struct dma_chan *chan, unsigned int max_entries);
	void (*free)(struct dma_chan *chan, void *ctx);
	struct dma_async_tx_descriptor *(*prep)(struct dma_chan *chan, void *ctx,
			const struct tide_dsa_batch_entry *entries,
			unsigned int nr_entries, unsigned long flags);
	int (*entry_status)(void *ctx, unsigned int idx);
};

enum tide_dsa_try_batch_disposition {
	TIDE_DSA_TRY_BATCH_UNAVAILABLE,
	TIDE_DSA_TRY_BATCH_NO_SLOT,
	TIDE_DSA_TRY_BATCH_DSA_CLAIMED,
};

#if defined(CONFIG_NUMA_BALANCING) && defined(CONFIG_DMA_ENGINE)
int tide_dsa_register_batch_ops(const struct tide_dsa_batch_ops *ops);
void tide_dsa_unregister_batch_ops(const struct tide_dsa_batch_ops *ops);
unsigned int tide_get_copy_mode(void);
bool tide_dsa_try_admit(struct tide_dsa_slot *token);
void tide_dsa_admit(struct tide_dsa_slot *token);
void tide_dsa_release_admission(struct tide_dsa_slot *token);
void tide_dsa_admit_promotion(const struct tide_migrate_ctx *promotion_context);
void tide_dsa_admit_blocked(const struct tide_migrate_ctx *promotion_context,
			    bool to_toptier);
int tide_dsa_try_copy_batch_status(struct tide_dsa_copy_pair *pairs,
				   unsigned int nr_pairs,
				   unsigned long *completed_bitmap,
				   struct tide_dsa_slot *token,
				   enum tide_dsa_try_batch_disposition *disposition);
int tide_dsa_copy_batch(struct tide_dsa_copy_pair *pairs, unsigned int nr_pairs);
int tide_dsa_copy_batch_admitted(struct tide_dsa_copy_pair *pairs,
				   unsigned int nr_pairs,
				   struct tide_dsa_slot *token);
#else
static inline int
tide_dsa_register_batch_ops(const struct tide_dsa_batch_ops *ops)
{
	return -EOPNOTSUPP;
}

static inline void
tide_dsa_unregister_batch_ops(const struct tide_dsa_batch_ops *ops)
{
}

static inline unsigned int tide_get_copy_mode(void)
{
	return TIDE_COPY_CPU;
}

static inline bool
tide_dsa_try_admit(struct tide_dsa_slot *token)
{
	return false;
}

static inline void
tide_dsa_admit(struct tide_dsa_slot *token)
{
}

static inline void
tide_dsa_release_admission(struct tide_dsa_slot *token)
{
}

static inline void
tide_dsa_admit_promotion(const struct tide_migrate_ctx *promotion_context)
{
}

static inline void
tide_dsa_admit_blocked(const struct tide_migrate_ctx *promotion_context,
		       bool to_toptier)
{
}

static inline int
tide_dsa_try_copy_batch_status(struct tide_dsa_copy_pair *pairs,
			       unsigned int nr_pairs,
			       unsigned long *completed_bitmap,
			       struct tide_dsa_slot *token,
			       enum tide_dsa_try_batch_disposition *disposition)
{
	if (disposition)
		*disposition = TIDE_DSA_TRY_BATCH_UNAVAILABLE;
	return -EOPNOTSUPP;
}

static inline int tide_dsa_copy_batch(struct tide_dsa_copy_pair *pairs,
					unsigned int nr_pairs)
{
	return -EOPNOTSUPP;
}

static inline int
tide_dsa_copy_batch_admitted(struct tide_dsa_copy_pair *pairs,
			       unsigned int nr_pairs,
			       struct tide_dsa_slot *token)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _LINUX_TIDE_DSA_H */
