// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights rsvd. */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/dmaengine.h>
#include <linux/tide_dsa.h>
#include <uapi/linux/idxd.h>
#include "../dmaengine.h"
#include "registers.h"
#include "idxd.h"

static inline struct idxd_wq *to_idxd_wq(struct dma_chan *c)
{
	struct idxd_dma_chan *idxd_chan;

	idxd_chan = container_of(c, struct idxd_dma_chan, chan);
	return idxd_chan->wq;
}

void idxd_dma_complete_txd(struct idxd_desc *desc,
			   enum idxd_complete_type comp_type)
{
	struct dma_async_tx_descriptor *tx;
	struct dmaengine_result res;
	int complete = 1;

	if (desc->completion->status == DSA_COMP_SUCCESS)
		res.result = DMA_TRANS_NOERROR;
	else if (desc->completion->status)
		res.result = DMA_TRANS_WRITE_FAILED;
	else if (comp_type == IDXD_COMPLETE_ABORT)
		res.result = DMA_TRANS_ABORTED;
	else
		complete = 0;

	tx = &desc->txd;
	if (complete && tx->cookie) {
		dma_cookie_complete(tx);
		dma_descriptor_unmap(tx);
		dmaengine_desc_get_callback_invoke(tx, &res);
		tx->callback = NULL;
		tx->callback_result = NULL;
	}
}

static void op_flag_setup(unsigned long flags, u32 *desc_flags)
{
	*desc_flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if (flags & DMA_PREP_INTERRUPT)
		*desc_flags |= IDXD_OP_FLAG_RCI;
}

static inline void set_completion_address(struct idxd_desc *desc,
					  u64 *compl_addr)
{
		*compl_addr = desc->compl_dma;
}

static inline void idxd_prep_desc_common(struct idxd_wq *wq,
					 struct dsa_hw_desc *hw, char opcode,
					 u64 addr_f1, u64 addr_f2, u64 len,
					 u64 compl, u32 flags)
{
	hw->flags = flags;
	hw->opcode = opcode;
	hw->src_addr = addr_f1;
	hw->dst_addr = addr_f2;
	hw->xfer_size = len;
	/*
	 * For dedicated WQ, this field is ignored and HW will use the WQCFG.priv
	 * field instead. This field should be set to 1 for kernel descriptors.
	 */
	hw->priv = 1;
	hw->completion_addr = compl;
}

static struct dma_async_tx_descriptor *
idxd_dma_submit_memcpy(struct dma_chan *c, dma_addr_t dma_dest,
		       dma_addr_t dma_src, size_t len, unsigned long flags)
{
	struct idxd_wq *wq = to_idxd_wq(c);
	u32 desc_flags;
	struct idxd_device *idxd = wq->idxd;
	struct idxd_desc *desc;

	if (wq->state != IDXD_WQ_ENABLED)
		return NULL;

	if (len > idxd->max_xfer_bytes)
		return NULL;

	op_flag_setup(flags, &desc_flags);
	desc = idxd_alloc_desc(wq, IDXD_OP_BLOCK);
	if (IS_ERR(desc))
		return NULL;

	idxd_prep_desc_common(wq, desc->hw, DSA_OPCODE_MEMMOVE,
			      dma_src, dma_dest, len, desc->compl_dma,
			      desc_flags);

	desc->txd.flags = flags;

	return &desc->txd;
}

#ifdef CONFIG_NUMA_BALANCING
/*
 * TIDE batch-descriptor provider.  mm/tide_dsa.c passes in mapped bus
 * addresses; the descriptor list, its completion records and the idxd_desc
 * that carries the batch stay inside the driver.
 */
struct idxd_tide_batch {
	struct device *dev;
	unsigned int max_entries;
	unsigned int nr_entries;
	struct dsa_hw_desc *descs;
	dma_addr_t descs_dma;
	struct dsa_completion_record *comps;
	dma_addr_t comps_dma;
};

static bool idxd_tide_chan_ok(struct dma_chan *c)
{
	return c && c->device &&
	       c->device->device_prep_dma_memcpy == idxd_dma_submit_memcpy;
}

static unsigned int idxd_tide_batch_max_entries(struct dma_chan *c)
{
	struct idxd_wq *wq;

	if (!idxd_tide_chan_ok(c))
		return 0;
	wq = to_idxd_wq(c);
	return min_t(unsigned int, wq->max_batch_size, wq->idxd->max_batch_size);
}

static void idxd_tide_batch_free(struct dma_chan *c, void *ctx)
{
	struct idxd_tide_batch *batch = ctx;

	if (!batch)
		return;
	if (batch->comps)
		dma_free_coherent(batch->dev,
				  batch->max_entries * sizeof(*batch->comps),
				  batch->comps, batch->comps_dma);
	if (batch->descs)
		dma_free_coherent(batch->dev,
				  batch->max_entries * sizeof(*batch->descs),
				  batch->descs, batch->descs_dma);
	kfree(batch);
}

static void *idxd_tide_batch_alloc(struct dma_chan *c, unsigned int max_entries)
{
	struct idxd_tide_batch *batch;

	if (!idxd_tide_chan_ok(c) || max_entries < 2)
		return NULL;

	batch = kzalloc(sizeof(*batch), GFP_KERNEL);
	if (!batch)
		return NULL;
	batch->dev = c->device->dev;
	batch->max_entries = max_entries;
	/* Coherent memory is page aligned, which meets the 64-byte list alignment. */
	batch->descs = dma_alloc_coherent(batch->dev,
					  max_entries * sizeof(*batch->descs),
					  &batch->descs_dma, GFP_KERNEL);
	batch->comps = dma_alloc_coherent(batch->dev,
					  max_entries * sizeof(*batch->comps),
					  &batch->comps_dma, GFP_KERNEL);
	if (!batch->descs || !batch->comps) {
		idxd_tide_batch_free(c, batch);
		return NULL;
	}
	return batch;
}

static struct dma_async_tx_descriptor *
idxd_tide_batch_prep(struct dma_chan *c, void *ctx,
		       const struct tide_dsa_batch_entry *entries,
		       unsigned int nr_entries, unsigned long flags)
{
	struct idxd_tide_batch *batch = ctx;
	struct idxd_device *idxd;
	struct idxd_desc *desc;
	struct idxd_wq *wq;
	u32 desc_flags;
	unsigned int i;

	if (!idxd_tide_chan_ok(c) || !batch || !entries)
		return NULL;
	wq = to_idxd_wq(c);
	idxd = wq->idxd;
	if (wq->state != IDXD_WQ_ENABLED)
		return NULL;
	/* A batch needs at least two entries and must fit the WQ batch limit. */
	if (nr_entries < 2 || nr_entries > batch->max_entries ||
	    nr_entries > wq->max_batch_size)
		return NULL;

	memset(batch->comps, 0, nr_entries * sizeof(*batch->comps));
	for (i = 0; i < nr_entries; i++) {
		struct dsa_hw_desc *hw = &batch->descs[i];

		if (!entries[i].len || entries[i].len > idxd->max_xfer_bytes)
			return NULL;
		memset(hw, 0, sizeof(*hw));
		idxd_prep_desc_common(wq, hw, DSA_OPCODE_MEMMOVE,
				      entries[i].src, entries[i].dst,
				      entries[i].len,
				      batch->comps_dma + i * sizeof(*batch->comps),
				      IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR);
	}
	batch->nr_entries = nr_entries;

	op_flag_setup(flags, &desc_flags);
	desc = idxd_alloc_desc(wq, IDXD_OP_BLOCK);
	if (IS_ERR(desc))
		return NULL;

	/* desc_list_addr and desc_count alias src_addr and xfer_size. */
	idxd_prep_desc_common(wq, desc->hw, DSA_OPCODE_BATCH, batch->descs_dma,
			      0, nr_entries, desc->compl_dma, desc_flags);
	desc->txd.flags = flags;

	/*
	 * Submission pushes the batch descriptor with MOVDIR64B/ENQCMDS, whose
	 * direct store is weakly ordered against older WB stores; fence the
	 * descriptor list and completion records ahead of it.
	 */
	wmb();
	return &desc->txd;
}

static int idxd_tide_batch_entry_status(void *ctx, unsigned int idx)
{
	struct idxd_tide_batch *batch = ctx;
	u8 status;

	if (!batch || idx >= batch->nr_entries)
		return -EINVAL;
	status = batch->comps[idx].status & DSA_COMP_STATUS_MASK;
	return status == DSA_COMP_SUCCESS ? 0 : -EIO;
}

const struct tide_dsa_batch_ops idxd_tide_batch_ops = {
	.owner = THIS_MODULE,
	.max_entries = idxd_tide_batch_max_entries,
	.alloc = idxd_tide_batch_alloc,
	.free = idxd_tide_batch_free,
	.prep = idxd_tide_batch_prep,
	.entry_status = idxd_tide_batch_entry_status,
};
#endif /* CONFIG_NUMA_BALANCING */

static int idxd_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct idxd_wq *wq = to_idxd_wq(chan);
	struct device *dev = &wq->idxd->pdev->dev;

	idxd_wq_get(wq);
	dev_dbg(dev, "%s: client_count: %d\n", __func__,
		idxd_wq_refcount(wq));
	return 0;
}

static void idxd_dma_free_chan_resources(struct dma_chan *chan)
{
	struct idxd_wq *wq = to_idxd_wq(chan);
	struct device *dev = &wq->idxd->pdev->dev;

	idxd_wq_put(wq);
	dev_dbg(dev, "%s: client_count: %d\n", __func__,
		idxd_wq_refcount(wq));
}

static enum dma_status idxd_dma_tx_status(struct dma_chan *dma_chan,
					  dma_cookie_t cookie,
					  struct dma_tx_state *txstate)
{
	return DMA_OUT_OF_ORDER;
}

/*
 * issue_pending() does not need to do anything since tx_submit() does the job
 * already.
 */
static void idxd_dma_issue_pending(struct dma_chan *dma_chan)
{
}

static dma_cookie_t idxd_dma_tx_submit(struct dma_async_tx_descriptor *tx)
{
	struct dma_chan *c = tx->chan;
	struct idxd_wq *wq = to_idxd_wq(c);
	dma_cookie_t cookie;
	int rc;
	struct idxd_desc *desc = container_of(tx, struct idxd_desc, txd);

	cookie = dma_cookie_assign(tx);

	rc = idxd_submit_desc(wq, desc);
	if (rc < 0)
		return rc;

	return cookie;
}

static void idxd_dma_release(struct dma_device *device)
{
	struct idxd_dma_dev *idxd_dma = container_of(device, struct idxd_dma_dev, dma);

	kfree(idxd_dma);
}

int idxd_register_dma_device(struct idxd_device *idxd)
{
	struct idxd_dma_dev *idxd_dma;
	struct dma_device *dma;
	struct device *dev = &idxd->pdev->dev;
	int rc;

	idxd_dma = kzalloc_node(sizeof(*idxd_dma), GFP_KERNEL, dev_to_node(dev));
	if (!idxd_dma)
		return -ENOMEM;

	dma = &idxd_dma->dma;
	INIT_LIST_HEAD(&dma->channels);
	dma->dev = dev;

	dma_cap_set(DMA_PRIVATE, dma->cap_mask);
	dma_cap_set(DMA_COMPLETION_NO_ORDER, dma->cap_mask);
	dma->device_release = idxd_dma_release;

	if (idxd->hw.opcap.bits[0] & IDXD_OPCAP_MEMMOVE) {
		dma_cap_set(DMA_MEMCPY, dma->cap_mask);
		dma->device_prep_dma_memcpy = idxd_dma_submit_memcpy;
	}

	dma->device_tx_status = idxd_dma_tx_status;
	dma->device_issue_pending = idxd_dma_issue_pending;
	dma->device_alloc_chan_resources = idxd_dma_alloc_chan_resources;
	dma->device_free_chan_resources = idxd_dma_free_chan_resources;

	rc = dma_async_device_register(dma);
	if (rc < 0) {
		kfree(idxd_dma);
		return rc;
	}

	idxd_dma->idxd = idxd;
	/*
	 * This pointer is protected by the refs taken by the dma_chan. It will remain valid
	 * as long as there are outstanding channels.
	 */
	idxd->idxd_dma = idxd_dma;
	return 0;
}

void idxd_unregister_dma_device(struct idxd_device *idxd)
{
	dma_async_device_unregister(&idxd->idxd_dma->dma);
}

int idxd_register_dma_channel(struct idxd_wq *wq)
{
	struct idxd_device *idxd = wq->idxd;
	struct dma_device *dma = &idxd->idxd_dma->dma;
	struct device *dev = &idxd->pdev->dev;
	struct idxd_dma_chan *idxd_chan;
	struct dma_chan *chan;
	int rc, i;

	idxd_chan = kzalloc_node(sizeof(*idxd_chan), GFP_KERNEL, dev_to_node(dev));
	if (!idxd_chan)
		return -ENOMEM;

	chan = &idxd_chan->chan;
	chan->device = dma;
	list_add_tail(&chan->device_node, &dma->channels);

	for (i = 0; i < wq->num_descs; i++) {
		struct idxd_desc *desc = wq->descs[i];

		dma_async_tx_descriptor_init(&desc->txd, chan);
		desc->txd.tx_submit = idxd_dma_tx_submit;
	}

	rc = dma_async_device_channel_register(dma, chan);
	if (rc < 0) {
		kfree(idxd_chan);
		return rc;
	}

	wq->idxd_chan = idxd_chan;
	idxd_chan->wq = wq;
	get_device(wq_confdev(wq));

	return 0;
}

void idxd_unregister_dma_channel(struct idxd_wq *wq)
{
	struct idxd_dma_chan *idxd_chan = wq->idxd_chan;
	struct dma_chan *chan = &idxd_chan->chan;
	struct idxd_dma_dev *idxd_dma = wq->idxd->idxd_dma;

	dma_async_device_channel_unregister(&idxd_dma->dma, chan);
	list_del(&chan->device_node);
	kfree(wq->idxd_chan);
	wq->idxd_chan = NULL;
	put_device(wq_confdev(wq));
}

static int idxd_dmaengine_drv_probe(struct idxd_dev *idxd_dev)
{
	struct device *dev = &idxd_dev->conf_dev;
	struct idxd_wq *wq = idxd_dev_to_wq(idxd_dev);
	struct idxd_device *idxd = wq->idxd;
	int rc;

	if (idxd->state != IDXD_DEV_ENABLED)
		return -ENXIO;

	mutex_lock(&wq->wq_lock);
	wq->type = IDXD_WQT_KERNEL;
	rc = __drv_enable_wq(wq);
	if (rc < 0) {
		dev_dbg(dev, "Enable wq %d failed: %d\n", wq->id, rc);
		rc = -ENXIO;
		goto err;
	}

	rc = idxd_wq_alloc_resources(wq);
	if (rc < 0) {
		idxd->cmd_status = IDXD_SCMD_WQ_RES_ALLOC_ERR;
		dev_dbg(dev, "WQ resource alloc failed\n");
		goto err_res_alloc;
	}

	rc = idxd_wq_init_percpu_ref(wq);
	if (rc < 0) {
		idxd->cmd_status = IDXD_SCMD_PERCPU_ERR;
		dev_dbg(dev, "percpu_ref setup failed\n");
		goto err_ref;
	}

	rc = idxd_register_dma_channel(wq);
	if (rc < 0) {
		idxd->cmd_status = IDXD_SCMD_DMA_CHAN_ERR;
		dev_dbg(dev, "Failed to register dma channel\n");
		goto err_dma;
	}

	idxd->cmd_status = 0;
	mutex_unlock(&wq->wq_lock);
	return 0;

err_dma:
	idxd_wq_quiesce(wq);
err_ref:
	idxd_wq_free_resources(wq);
err_res_alloc:
	__drv_disable_wq(wq);
err:
	wq->type = IDXD_WQT_NONE;
	mutex_unlock(&wq->wq_lock);
	return rc;
}

static void idxd_dmaengine_drv_remove(struct idxd_dev *idxd_dev)
{
	struct idxd_wq *wq = idxd_dev_to_wq(idxd_dev);

	mutex_lock(&wq->wq_lock);
	idxd_wq_quiesce(wq);
	idxd_unregister_dma_channel(wq);
	__drv_disable_wq(wq);
	idxd_wq_free_resources(wq);
	wq->type = IDXD_WQT_NONE;
	mutex_unlock(&wq->wq_lock);
}

static enum idxd_dev_type dev_types[] = {
	IDXD_DEV_WQ,
	IDXD_DEV_NONE,
};

struct idxd_device_driver idxd_dmaengine_drv = {
	.probe = idxd_dmaengine_drv_probe,
	.remove = idxd_dmaengine_drv_remove,
	.name = "dmaengine",
	.type = dev_types,
};
EXPORT_SYMBOL_GPL(idxd_dmaengine_drv);
