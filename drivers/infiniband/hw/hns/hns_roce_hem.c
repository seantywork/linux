/*
 * Copyright (c) 2016 Hisilicon Limited.
 * Copyright (c) 2007, 2008 Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "hns_roce_device.h"
#include "hns_roce_hem.h"
#include "hns_roce_common.h"

#define HEM_INDEX_BUF			BIT(0)
#define HEM_INDEX_L0			BIT(1)
#define HEM_INDEX_L1			BIT(2)
struct hns_roce_hem_index {
	u64 buf;
	u64 l0;
	u64 l1;
	u32 inited; /* indicate which index is available */
};

bool hns_roce_check_whether_mhop(struct hns_roce_dev *hr_dev, u32 type)
{
	int hop_num = 0;

	switch (type) {
	case HEM_TYPE_QPC:
		hop_num = hr_dev->caps.qpc_hop_num;
		break;
	case HEM_TYPE_MTPT:
		hop_num = hr_dev->caps.mpt_hop_num;
		break;
	case HEM_TYPE_CQC:
		hop_num = hr_dev->caps.cqc_hop_num;
		break;
	case HEM_TYPE_SRQC:
		hop_num = hr_dev->caps.srqc_hop_num;
		break;
	case HEM_TYPE_SCCC:
		hop_num = hr_dev->caps.sccc_hop_num;
		break;
	case HEM_TYPE_QPC_TIMER:
		hop_num = hr_dev->caps.qpc_timer_hop_num;
		break;
	case HEM_TYPE_CQC_TIMER:
		hop_num = hr_dev->caps.cqc_timer_hop_num;
		break;
	case HEM_TYPE_GMV:
		hop_num = hr_dev->caps.gmv_hop_num;
		break;
	default:
		return false;
	}

	return hop_num;
}

static bool hns_roce_check_hem_null(struct hns_roce_hem **hem, u64 hem_idx,
				    u32 bt_chunk_num, u64 hem_max_num)
{
	u64 start_idx = round_down(hem_idx, bt_chunk_num);
	u64 check_max_num = start_idx + bt_chunk_num;
	u64 i;

	for (i = start_idx; (i < check_max_num) && (i < hem_max_num); i++)
		if (i != hem_idx && hem[i])
			return false;

	return true;
}

static bool hns_roce_check_bt_null(u64 **bt, u64 ba_idx, u32 bt_chunk_num)
{
	u64 start_idx = round_down(ba_idx, bt_chunk_num);
	int i;

	for (i = 0; i < bt_chunk_num; i++)
		if (i != ba_idx && bt[start_idx + i])
			return false;

	return true;
}

static int hns_roce_get_bt_num(u32 table_type, u32 hop_num)
{
	if (check_whether_bt_num_3(table_type, hop_num))
		return 3;
	else if (check_whether_bt_num_2(table_type, hop_num))
		return 2;
	else if (check_whether_bt_num_1(table_type, hop_num))
		return 1;
	else
		return 0;
}

static int get_hem_table_config(struct hns_roce_dev *hr_dev,
				struct hns_roce_hem_mhop *mhop,
				u32 type)
{
	struct device *dev = hr_dev->dev;

	switch (type) {
	case HEM_TYPE_QPC:
		mhop->buf_chunk_size = 1 << (hr_dev->caps.qpc_buf_pg_sz
					     + PAGE_SHIFT);
		mhop->bt_chunk_size = 1 << (hr_dev->caps.qpc_ba_pg_sz
					     + PAGE_SHIFT);
		mhop->ba_l0_num = hr_dev->caps.qpc_bt_num;
		mhop->hop_num = hr_dev->caps.qpc_hop_num;
		break;
	case HEM_TYPE_MTPT:
		mhop->buf_chunk_size = 1 << (hr_dev->caps.mpt_buf_pg_sz
					     + PAGE_SHIFT);
		mhop->bt_chunk_size = 1 << (hr_dev->caps.mpt_ba_pg_sz
					     + PAGE_SHIFT);
		mhop->ba_l0_num = hr_dev->caps.mpt_bt_num;
		mhop->hop_num = hr_dev->caps.mpt_hop_num;
		break;
	case HEM_TYPE_CQC:
		mhop->buf_chunk_size = 1 << (hr_dev->caps.cqc_buf_pg_sz
					     + PAGE_SHIFT);
		mhop->bt_chunk_size = 1 << (hr_dev->caps.cqc_ba_pg_sz
					    + PAGE_SHIFT);
		mhop->ba_l0_num = hr_dev->caps.cqc_bt_num;
		mhop->hop_num = hr_dev->caps.cqc_hop_num;
		break;
	case HEM_TYPE_SCCC:
		mhop->buf_chunk_size = 1 << (hr_dev->caps.sccc_buf_pg_sz
					     + PAGE_SHIFT);
		mhop->bt_chunk_size = 1 << (hr_dev->caps.sccc_ba_pg_sz
					    + PAGE_SHIFT);
		mhop->ba_l0_num = hr_dev->caps.sccc_bt_num;
		mhop->hop_num = hr_dev->caps.sccc_hop_num;
		break;
	case HEM_TYPE_QPC_TIMER:
		mhop->buf_chunk_size = 1 << (hr_dev->caps.qpc_timer_buf_pg_sz
					     + PAGE_SHIFT);
		mhop->bt_chunk_size = 1 << (hr_dev->caps.qpc_timer_ba_pg_sz
					    + PAGE_SHIFT);
		mhop->ba_l0_num = hr_dev->caps.qpc_timer_bt_num;
		mhop->hop_num = hr_dev->caps.qpc_timer_hop_num;
		break;
	case HEM_TYPE_CQC_TIMER:
		mhop->buf_chunk_size = 1 << (hr_dev->caps.cqc_timer_buf_pg_sz
					     + PAGE_SHIFT);
		mhop->bt_chunk_size = 1 << (hr_dev->caps.cqc_timer_ba_pg_sz
					    + PAGE_SHIFT);
		mhop->ba_l0_num = hr_dev->caps.cqc_timer_bt_num;
		mhop->hop_num = hr_dev->caps.cqc_timer_hop_num;
		break;
	case HEM_TYPE_SRQC:
		mhop->buf_chunk_size = 1 << (hr_dev->caps.srqc_buf_pg_sz
					     + PAGE_SHIFT);
		mhop->bt_chunk_size = 1 << (hr_dev->caps.srqc_ba_pg_sz
					     + PAGE_SHIFT);
		mhop->ba_l0_num = hr_dev->caps.srqc_bt_num;
		mhop->hop_num = hr_dev->caps.srqc_hop_num;
		break;
	case HEM_TYPE_GMV:
		mhop->buf_chunk_size = 1 << (hr_dev->caps.gmv_buf_pg_sz +
					     PAGE_SHIFT);
		mhop->bt_chunk_size = 1 << (hr_dev->caps.gmv_ba_pg_sz +
					    PAGE_SHIFT);
		mhop->ba_l0_num = hr_dev->caps.gmv_bt_num;
		mhop->hop_num = hr_dev->caps.gmv_hop_num;
		break;
	default:
		dev_err(dev, "table %u not support multi-hop addressing!\n",
			type);
		return -EINVAL;
	}

	return 0;
}

int hns_roce_calc_hem_mhop(struct hns_roce_dev *hr_dev,
			   struct hns_roce_hem_table *table, unsigned long *obj,
			   struct hns_roce_hem_mhop *mhop)
{
	struct device *dev = hr_dev->dev;
	u32 chunk_ba_num;
	u32 chunk_size;
	u32 table_idx;
	u32 bt_num;

	if (get_hem_table_config(hr_dev, mhop, table->type))
		return -EINVAL;

	if (!obj)
		return 0;

	/*
	 * QPC/MTPT/CQC/SRQC/SCCC alloc hem for buffer pages.
	 * MTT/CQE alloc hem for bt pages.
	 */
	bt_num = hns_roce_get_bt_num(table->type, mhop->hop_num);
	chunk_ba_num = mhop->bt_chunk_size / BA_BYTE_LEN;
	chunk_size = table->type < HEM_TYPE_MTT ? mhop->buf_chunk_size :
			      mhop->bt_chunk_size;
	table_idx = *obj / (chunk_size / table->obj_size);
	switch (bt_num) {
	case 3:
		mhop->l2_idx = table_idx & (chunk_ba_num - 1);
		mhop->l1_idx = table_idx / chunk_ba_num & (chunk_ba_num - 1);
		mhop->l0_idx = (table_idx / chunk_ba_num) / chunk_ba_num;
		break;
	case 2:
		mhop->l1_idx = table_idx & (chunk_ba_num - 1);
		mhop->l0_idx = table_idx / chunk_ba_num;
		break;
	case 1:
		mhop->l0_idx = table_idx;
		break;
	default:
		dev_err(dev, "table %u not support hop_num = %u!\n",
			table->type, mhop->hop_num);
		return -EINVAL;
	}
	if (mhop->l0_idx >= mhop->ba_l0_num)
		mhop->l0_idx %= mhop->ba_l0_num;

	return 0;
}

static struct hns_roce_hem *hns_roce_alloc_hem(struct hns_roce_dev *hr_dev,
					       unsigned long hem_alloc_size)
{
	struct hns_roce_hem *hem;
	int order;
	void *buf;

	order = get_order(hem_alloc_size);
	if (PAGE_SIZE << order != hem_alloc_size) {
		dev_err(hr_dev->dev, "invalid hem_alloc_size: %lu!\n",
			hem_alloc_size);
		return NULL;
	}

	hem = kmalloc(sizeof(*hem), GFP_KERNEL);
	if (!hem)
		return NULL;

	buf = dma_alloc_coherent(hr_dev->dev, hem_alloc_size,
				 &hem->dma, GFP_KERNEL);
	if (!buf)
		goto fail;

	hem->buf = buf;
	hem->size = hem_alloc_size;

	return hem;

fail:
	kfree(hem);
	return NULL;
}

void hns_roce_free_hem(struct hns_roce_dev *hr_dev, struct hns_roce_hem *hem)
{
	if (!hem)
		return;

	dma_free_coherent(hr_dev->dev, hem->size, hem->buf, hem->dma);

	kfree(hem);
}

static int calc_hem_config(struct hns_roce_dev *hr_dev,
			   struct hns_roce_hem_table *table, unsigned long obj,
			   struct hns_roce_hem_mhop *mhop,
			   struct hns_roce_hem_index *index)
{
	struct device *dev = hr_dev->dev;
	unsigned long mhop_obj = obj;
	u32 l0_idx, l1_idx, l2_idx;
	u32 chunk_ba_num;
	u32 bt_num;
	int ret;

	ret = hns_roce_calc_hem_mhop(hr_dev, table, &mhop_obj, mhop);
	if (ret)
		return ret;

	l0_idx = mhop->l0_idx;
	l1_idx = mhop->l1_idx;
	l2_idx = mhop->l2_idx;
	chunk_ba_num = mhop->bt_chunk_size / BA_BYTE_LEN;
	bt_num = hns_roce_get_bt_num(table->type, mhop->hop_num);
	switch (bt_num) {
	case 3:
		index->l1 = l0_idx * chunk_ba_num + l1_idx;
		index->l0 = l0_idx;
		index->buf = l0_idx * chunk_ba_num * chunk_ba_num +
			     l1_idx * chunk_ba_num + l2_idx;
		break;
	case 2:
		index->l0 = l0_idx;
		index->buf = l0_idx * chunk_ba_num + l1_idx;
		break;
	case 1:
		index->buf = l0_idx;
		break;
	default:
		dev_err(dev, "table %u not support mhop.hop_num = %u!\n",
			table->type, mhop->hop_num);
		return -EINVAL;
	}

	if (unlikely(index->buf >= table->num_hem)) {
		dev_err(dev, "table %u exceed hem limt idx %llu, max %lu!\n",
			table->type, index->buf, table->num_hem);
		return -EINVAL;
	}

	return 0;
}

static void free_mhop_hem(struct hns_roce_dev *hr_dev,
			  struct hns_roce_hem_table *table,
			  struct hns_roce_hem_mhop *mhop,
			  struct hns_roce_hem_index *index)
{
	u32 bt_size = mhop->bt_chunk_size;
	struct device *dev = hr_dev->dev;

	if (index->inited & HEM_INDEX_BUF) {
		hns_roce_free_hem(hr_dev, table->hem[index->buf]);
		table->hem[index->buf] = NULL;
	}

	if (index->inited & HEM_INDEX_L1) {
		dma_free_coherent(dev, bt_size, table->bt_l1[index->l1],
				  table->bt_l1_dma_addr[index->l1]);
		table->bt_l1[index->l1] = NULL;
	}

	if (index->inited & HEM_INDEX_L0) {
		dma_free_coherent(dev, bt_size, table->bt_l0[index->l0],
				  table->bt_l0_dma_addr[index->l0]);
		table->bt_l0[index->l0] = NULL;
	}
}

static int alloc_mhop_hem(struct hns_roce_dev *hr_dev,
			  struct hns_roce_hem_table *table,
			  struct hns_roce_hem_mhop *mhop,
			  struct hns_roce_hem_index *index)
{
	u32 bt_size = mhop->bt_chunk_size;
	struct device *dev = hr_dev->dev;
	u64 bt_ba;
	u32 size;
	int ret;

	/* alloc L1 BA's chunk */
	if ((check_whether_bt_num_3(table->type, mhop->hop_num) ||
	     check_whether_bt_num_2(table->type, mhop->hop_num)) &&
	     !table->bt_l0[index->l0]) {
		table->bt_l0[index->l0] = dma_alloc_coherent(dev, bt_size,
					    &table->bt_l0_dma_addr[index->l0],
					    GFP_KERNEL);
		if (!table->bt_l0[index->l0]) {
			ret = -ENOMEM;
			goto out;
		}
		index->inited |= HEM_INDEX_L0;
	}

	/* alloc L2 BA's chunk */
	if (check_whether_bt_num_3(table->type, mhop->hop_num) &&
	    !table->bt_l1[index->l1])  {
		table->bt_l1[index->l1] = dma_alloc_coherent(dev, bt_size,
					    &table->bt_l1_dma_addr[index->l1],
					    GFP_KERNEL);
		if (!table->bt_l1[index->l1]) {
			ret = -ENOMEM;
			goto err_alloc_hem;
		}
		index->inited |= HEM_INDEX_L1;
		*(table->bt_l0[index->l0] + mhop->l1_idx) =
					       table->bt_l1_dma_addr[index->l1];
	}

	/*
	 * alloc buffer space chunk for QPC/MTPT/CQC/SRQC/SCCC.
	 * alloc bt space chunk for MTT/CQE.
	 */
	size = table->type < HEM_TYPE_MTT ? mhop->buf_chunk_size : bt_size;
	table->hem[index->buf] = hns_roce_alloc_hem(hr_dev, size);
	if (!table->hem[index->buf]) {
		ret = -ENOMEM;
		goto err_alloc_hem;
	}

	index->inited |= HEM_INDEX_BUF;
	bt_ba = table->hem[index->buf]->dma;

	if (table->type < HEM_TYPE_MTT) {
		if (mhop->hop_num == 2)
			*(table->bt_l1[index->l1] + mhop->l2_idx) = bt_ba;
		else if (mhop->hop_num == 1)
			*(table->bt_l0[index->l0] + mhop->l1_idx) = bt_ba;
	} else if (mhop->hop_num == 2) {
		*(table->bt_l0[index->l0] + mhop->l1_idx) = bt_ba;
	}

	return 0;
err_alloc_hem:
	free_mhop_hem(hr_dev, table, mhop, index);
out:
	return ret;
}

static int set_mhop_hem(struct hns_roce_dev *hr_dev,
			struct hns_roce_hem_table *table, unsigned long obj,
			struct hns_roce_hem_mhop *mhop,
			struct hns_roce_hem_index *index)
{
	struct device *dev = hr_dev->dev;
	u32 step_idx;
	int ret = 0;

	if (index->inited & HEM_INDEX_L0) {
		ret = hr_dev->hw->set_hem(hr_dev, table, obj, 0);
		if (ret) {
			dev_err(dev, "set HEM step 0 failed!\n");
			goto out;
		}
	}

	if (index->inited & HEM_INDEX_L1) {
		ret = hr_dev->hw->set_hem(hr_dev, table, obj, 1);
		if (ret) {
			dev_err(dev, "set HEM step 1 failed!\n");
			goto out;
		}
	}

	if (index->inited & HEM_INDEX_BUF) {
		if (mhop->hop_num == HNS_ROCE_HOP_NUM_0)
			step_idx = 0;
		else
			step_idx = mhop->hop_num;
		ret = hr_dev->hw->set_hem(hr_dev, table, obj, step_idx);
		if (ret)
			dev_err(dev, "set HEM step last failed!\n");
	}
out:
	return ret;
}

static int hns_roce_table_mhop_get(struct hns_roce_dev *hr_dev,
				   struct hns_roce_hem_table *table,
				   unsigned long obj)
{
	struct hns_roce_hem_index index = {};
	struct hns_roce_hem_mhop mhop = {};
	struct device *dev = hr_dev->dev;
	int ret;

	ret = calc_hem_config(hr_dev, table, obj, &mhop, &index);
	if (ret) {
		dev_err(dev, "calc hem config failed!\n");
		return ret;
	}

	mutex_lock(&table->mutex);
	if (table->hem[index.buf]) {
		refcount_inc(&table->hem[index.buf]->refcount);
		goto out;
	}

	ret = alloc_mhop_hem(hr_dev, table, &mhop, &index);
	if (ret) {
		dev_err(dev, "alloc mhop hem failed!\n");
		goto out;
	}

	/* set HEM base address to hardware */
	if (table->type < HEM_TYPE_MTT) {
		ret = set_mhop_hem(hr_dev, table, obj, &mhop, &index);
		if (ret) {
			dev_err(dev, "set HEM address to HW failed!\n");
			goto err_alloc;
		}
	}

	refcount_set(&table->hem[index.buf]->refcount, 1);
	goto out;

err_alloc:
	free_mhop_hem(hr_dev, table, &mhop, &index);
out:
	mutex_unlock(&table->mutex);
	return ret;
}

int hns_roce_table_get(struct hns_roce_dev *hr_dev,
		       struct hns_roce_hem_table *table, unsigned long obj)
{
	struct device *dev = hr_dev->dev;
	unsigned long i;
	int ret = 0;

	if (hns_roce_check_whether_mhop(hr_dev, table->type))
		return hns_roce_table_mhop_get(hr_dev, table, obj);

	i = obj / (table->table_chunk_size / table->obj_size);

	mutex_lock(&table->mutex);

	if (table->hem[i]) {
		refcount_inc(&table->hem[i]->refcount);
		goto out;
	}

	table->hem[i] = hns_roce_alloc_hem(hr_dev, table->table_chunk_size);
	if (!table->hem[i]) {
		ret = -ENOMEM;
		goto out;
	}

	/* Set HEM base address(128K/page, pa) to Hardware */
	ret = hr_dev->hw->set_hem(hr_dev, table, obj, HEM_HOP_STEP_DIRECT);
	if (ret) {
		hns_roce_free_hem(hr_dev, table->hem[i]);
		table->hem[i] = NULL;
		dev_err(dev, "set HEM base address to HW failed, ret = %d.\n",
			ret);
		goto out;
	}

	refcount_set(&table->hem[i]->refcount, 1);
out:
	mutex_unlock(&table->mutex);
	return ret;
}

static void clear_mhop_hem(struct hns_roce_dev *hr_dev,
			   struct hns_roce_hem_table *table, unsigned long obj,
			   struct hns_roce_hem_mhop *mhop,
			   struct hns_roce_hem_index *index)
{
	struct device *dev = hr_dev->dev;
	u32 hop_num = mhop->hop_num;
	u32 chunk_ba_num;
	u32 step_idx;
	int ret;

	index->inited = HEM_INDEX_BUF;
	chunk_ba_num = mhop->bt_chunk_size / BA_BYTE_LEN;
	if (check_whether_bt_num_2(table->type, hop_num)) {
		if (hns_roce_check_hem_null(table->hem, index->buf,
					    chunk_ba_num, table->num_hem))
			index->inited |= HEM_INDEX_L0;
	} else if (check_whether_bt_num_3(table->type, hop_num)) {
		if (hns_roce_check_hem_null(table->hem, index->buf,
					    chunk_ba_num, table->num_hem)) {
			index->inited |= HEM_INDEX_L1;
			if (hns_roce_check_bt_null(table->bt_l1, index->l1,
						   chunk_ba_num))
				index->inited |= HEM_INDEX_L0;
		}
	}

	if (table->type < HEM_TYPE_MTT) {
		if (hop_num == HNS_ROCE_HOP_NUM_0)
			step_idx = 0;
		else
			step_idx = hop_num;

		ret = hr_dev->hw->clear_hem(hr_dev, table, obj, step_idx);
		if (ret)
			dev_warn(dev, "failed to clear hop%u HEM, ret = %d.\n",
				 hop_num, ret);

		if (index->inited & HEM_INDEX_L1) {
			ret = hr_dev->hw->clear_hem(hr_dev, table, obj, 1);
			if (ret)
				dev_warn(dev, "failed to clear HEM step 1, ret = %d.\n",
					 ret);
		}

		if (index->inited & HEM_INDEX_L0) {
			ret = hr_dev->hw->clear_hem(hr_dev, table, obj, 0);
			if (ret)
				dev_warn(dev, "failed to clear HEM step 0, ret = %d.\n",
					 ret);
		}
	}
}

static void hns_roce_table_mhop_put(struct hns_roce_dev *hr_dev,
				    struct hns_roce_hem_table *table,
				    unsigned long obj,
				    int check_refcount)
{
	struct hns_roce_hem_index index = {};
	struct hns_roce_hem_mhop mhop = {};
	struct device *dev = hr_dev->dev;
	int ret;

	ret = calc_hem_config(hr_dev, table, obj, &mhop, &index);
	if (ret) {
		dev_err(dev, "calc hem config failed!\n");
		return;
	}

	if (!check_refcount)
		mutex_lock(&table->mutex);
	else if (!refcount_dec_and_mutex_lock(&table->hem[index.buf]->refcount,
					      &table->mutex))
		return;

	clear_mhop_hem(hr_dev, table, obj, &mhop, &index);
	free_mhop_hem(hr_dev, table, &mhop, &index);

	mutex_unlock(&table->mutex);
}

void hns_roce_table_put(struct hns_roce_dev *hr_dev,
			struct hns_roce_hem_table *table, unsigned long obj)
{
	struct device *dev = hr_dev->dev;
	unsigned long i;
	int ret;

	if (hns_roce_check_whether_mhop(hr_dev, table->type)) {
		hns_roce_table_mhop_put(hr_dev, table, obj, 1);
		return;
	}

	i = obj / (table->table_chunk_size / table->obj_size);

	if (!refcount_dec_and_mutex_lock(&table->hem[i]->refcount,
					 &table->mutex))
		return;

	ret = hr_dev->hw->clear_hem(hr_dev, table, obj, HEM_HOP_STEP_DIRECT);
	if (ret)
		dev_warn_ratelimited(dev, "failed to clear HEM base address, ret = %d.\n",
				     ret);

	hns_roce_free_hem(hr_dev, table->hem[i]);
	table->hem[i] = NULL;

	mutex_unlock(&table->mutex);
}

void *hns_roce_table_find(struct hns_roce_dev *hr_dev,
			  struct hns_roce_hem_table *table,
			  unsigned long obj, dma_addr_t *dma_handle)
{
	struct hns_roce_hem_mhop mhop;
	struct hns_roce_hem *hem;
	unsigned long mhop_obj = obj;
	unsigned long obj_per_chunk;
	unsigned long idx_offset;
	int offset, dma_offset;
	void *addr = NULL;
	u32 hem_idx = 0;
	int i, j;

	mutex_lock(&table->mutex);

	if (!hns_roce_check_whether_mhop(hr_dev, table->type)) {
		obj_per_chunk = table->table_chunk_size / table->obj_size;
		hem = table->hem[obj / obj_per_chunk];
		idx_offset = obj % obj_per_chunk;
		dma_offset = offset = idx_offset * table->obj_size;
	} else {
		u32 seg_size = 64; /* 8 bytes per BA and 8 BA per segment */

		if (hns_roce_calc_hem_mhop(hr_dev, table, &mhop_obj, &mhop))
			goto out;
		/* mtt mhop */
		i = mhop.l0_idx;
		j = mhop.l1_idx;
		if (mhop.hop_num == 2)
			hem_idx = i * (mhop.bt_chunk_size / BA_BYTE_LEN) + j;
		else if (mhop.hop_num == 1 ||
			 mhop.hop_num == HNS_ROCE_HOP_NUM_0)
			hem_idx = i;

		hem = table->hem[hem_idx];
		dma_offset = offset = obj * seg_size % mhop.bt_chunk_size;
		if (mhop.hop_num == 2)
			dma_offset = offset = 0;
	}

	if (!hem)
		goto out;

	*dma_handle = hem->dma + dma_offset;
	addr = hem->buf + offset;

out:
	mutex_unlock(&table->mutex);
	return addr;
}

int hns_roce_init_hem_table(struct hns_roce_dev *hr_dev,
			    struct hns_roce_hem_table *table, u32 type,
			    unsigned long obj_size, unsigned long nobj)
{
	unsigned long obj_per_chunk;
	unsigned long num_hem;

	if (!hns_roce_check_whether_mhop(hr_dev, type)) {
		table->table_chunk_size = hr_dev->caps.chunk_sz;
		obj_per_chunk = table->table_chunk_size / obj_size;
		num_hem = DIV_ROUND_UP(nobj, obj_per_chunk);

		table->hem = kcalloc(num_hem, sizeof(*table->hem), GFP_KERNEL);
		if (!table->hem)
			return -ENOMEM;
	} else {
		struct hns_roce_hem_mhop mhop = {};
		unsigned long buf_chunk_size;
		unsigned long bt_chunk_size;
		unsigned long bt_chunk_num;
		unsigned long num_bt_l0;
		u32 hop_num;

		if (get_hem_table_config(hr_dev, &mhop, type))
			return -EINVAL;

		buf_chunk_size = mhop.buf_chunk_size;
		bt_chunk_size = mhop.bt_chunk_size;
		num_bt_l0 = mhop.ba_l0_num;
		hop_num = mhop.hop_num;

		obj_per_chunk = buf_chunk_size / obj_size;
		num_hem = DIV_ROUND_UP(nobj, obj_per_chunk);
		bt_chunk_num = bt_chunk_size / BA_BYTE_LEN;

		if (type >= HEM_TYPE_MTT)
			num_bt_l0 = bt_chunk_num;

		table->hem = kcalloc(num_hem, sizeof(*table->hem),
					 GFP_KERNEL);
		if (!table->hem)
			goto err_kcalloc_hem_buf;

		if (check_whether_bt_num_3(type, hop_num)) {
			unsigned long num_bt_l1;

			num_bt_l1 = DIV_ROUND_UP(num_hem, bt_chunk_num);
			table->bt_l1 = kcalloc(num_bt_l1,
					       sizeof(*table->bt_l1),
					       GFP_KERNEL);
			if (!table->bt_l1)
				goto err_kcalloc_bt_l1;

			table->bt_l1_dma_addr = kcalloc(num_bt_l1,
						 sizeof(*table->bt_l1_dma_addr),
						 GFP_KERNEL);

			if (!table->bt_l1_dma_addr)
				goto err_kcalloc_l1_dma;
		}

		if (check_whether_bt_num_2(type, hop_num) ||
			check_whether_bt_num_3(type, hop_num)) {
			table->bt_l0 = kcalloc(num_bt_l0, sizeof(*table->bt_l0),
					       GFP_KERNEL);
			if (!table->bt_l0)
				goto err_kcalloc_bt_l0;

			table->bt_l0_dma_addr = kcalloc(num_bt_l0,
						 sizeof(*table->bt_l0_dma_addr),
						 GFP_KERNEL);
			if (!table->bt_l0_dma_addr)
				goto err_kcalloc_l0_dma;
		}
	}

	table->type = type;
	table->num_hem = num_hem;
	table->obj_size = obj_size;
	mutex_init(&table->mutex);

	return 0;

err_kcalloc_l0_dma:
	kfree(table->bt_l0);
	table->bt_l0 = NULL;

err_kcalloc_bt_l0:
	kfree(table->bt_l1_dma_addr);
	table->bt_l1_dma_addr = NULL;

err_kcalloc_l1_dma:
	kfree(table->bt_l1);
	table->bt_l1 = NULL;

err_kcalloc_bt_l1:
	kfree(table->hem);
	table->hem = NULL;

err_kcalloc_hem_buf:
	return -ENOMEM;
}

static void hns_roce_cleanup_mhop_hem_table(struct hns_roce_dev *hr_dev,
					    struct hns_roce_hem_table *table)
{
	struct hns_roce_hem_mhop mhop;
	u32 buf_chunk_size;
	u64 obj;
	int i;

	if (hns_roce_calc_hem_mhop(hr_dev, table, NULL, &mhop))
		return;
	buf_chunk_size = table->type < HEM_TYPE_MTT ? mhop.buf_chunk_size :
					mhop.bt_chunk_size;

	for (i = 0; i < table->num_hem; ++i) {
		obj = i * buf_chunk_size / table->obj_size;
		if (table->hem[i])
			hns_roce_table_mhop_put(hr_dev, table, obj, 0);
	}

	kfree(table->hem);
	table->hem = NULL;
	kfree(table->bt_l1);
	table->bt_l1 = NULL;
	kfree(table->bt_l1_dma_addr);
	table->bt_l1_dma_addr = NULL;
	kfree(table->bt_l0);
	table->bt_l0 = NULL;
	kfree(table->bt_l0_dma_addr);
	table->bt_l0_dma_addr = NULL;
}

void hns_roce_cleanup_hem_table(struct hns_roce_dev *hr_dev,
				struct hns_roce_hem_table *table)
{
	struct device *dev = hr_dev->dev;
	unsigned long i;
	int obj;
	int ret;

	if (hns_roce_check_whether_mhop(hr_dev, table->type)) {
		hns_roce_cleanup_mhop_hem_table(hr_dev, table);
		mutex_destroy(&table->mutex);
		return;
	}

	for (i = 0; i < table->num_hem; ++i)
		if (table->hem[i]) {
			obj = i * table->table_chunk_size / table->obj_size;
			ret = hr_dev->hw->clear_hem(hr_dev, table, obj, 0);
			if (ret)
				dev_err(dev, "clear HEM base address failed, ret = %d.\n",
					ret);

			hns_roce_free_hem(hr_dev, table->hem[i]);
		}

	mutex_destroy(&table->mutex);
	kfree(table->hem);
}

void hns_roce_cleanup_hem(struct hns_roce_dev *hr_dev)
{
	if (hr_dev->caps.flags & HNS_ROCE_CAP_FLAG_SRQ)
		hns_roce_cleanup_hem_table(hr_dev,
					   &hr_dev->srq_table.table);
	hns_roce_cleanup_hem_table(hr_dev, &hr_dev->cq_table.table);
	if (hr_dev->caps.qpc_timer_entry_sz)
		hns_roce_cleanup_hem_table(hr_dev,
					   &hr_dev->qpc_timer_table);
	if (hr_dev->caps.cqc_timer_entry_sz)
		hns_roce_cleanup_hem_table(hr_dev,
					   &hr_dev->cqc_timer_table);
	if (hr_dev->caps.flags & HNS_ROCE_CAP_FLAG_QP_FLOW_CTRL)
		hns_roce_cleanup_hem_table(hr_dev,
					   &hr_dev->qp_table.sccc_table);
	if (hr_dev->caps.trrl_entry_sz)
		hns_roce_cleanup_hem_table(hr_dev,
					   &hr_dev->qp_table.trrl_table);

	if (hr_dev->caps.gmv_entry_sz)
		hns_roce_cleanup_hem_table(hr_dev, &hr_dev->gmv_table);

	hns_roce_cleanup_hem_table(hr_dev, &hr_dev->qp_table.irrl_table);
	hns_roce_cleanup_hem_table(hr_dev, &hr_dev->qp_table.qp_table);
	hns_roce_cleanup_hem_table(hr_dev, &hr_dev->mr_table.mtpt_table);
}

struct hns_roce_hem_item {
	struct list_head list; /* link all hems in the same bt level */
	struct list_head sibling; /* link all hems in last hop for mtt */
	void *addr;
	dma_addr_t dma_addr;
	size_t count; /* max ba numbers */
	int start; /* start buf offset in this hem */
	int end; /* end buf offset in this hem */
	bool exist_bt;
};

/* All HEM items are linked in a tree structure */
struct hns_roce_hem_head {
	struct list_head branch[HNS_ROCE_MAX_BT_REGION];
	struct list_head root;
	struct list_head leaf;
};

static struct hns_roce_hem_item *
hem_list_alloc_item(struct hns_roce_dev *hr_dev, int start, int end, int count,
		    bool exist_bt)
{
	struct hns_roce_hem_item *hem;

	hem = kzalloc(sizeof(*hem), GFP_KERNEL);
	if (!hem)
		return NULL;

	if (exist_bt) {
		hem->addr = dma_alloc_coherent(hr_dev->dev, count * BA_BYTE_LEN,
					       &hem->dma_addr, GFP_KERNEL);
		if (!hem->addr) {
			kfree(hem);
			return NULL;
		}
	}

	hem->exist_bt = exist_bt;
	hem->count = count;
	hem->start = start;
	hem->end = end;
	INIT_LIST_HEAD(&hem->list);
	INIT_LIST_HEAD(&hem->sibling);

	return hem;
}

static void hem_list_free_item(struct hns_roce_dev *hr_dev,
			       struct hns_roce_hem_item *hem)
{
	if (hem->exist_bt)
		dma_free_coherent(hr_dev->dev, hem->count * BA_BYTE_LEN,
				  hem->addr, hem->dma_addr);
	kfree(hem);
}

static void hem_list_free_all(struct hns_roce_dev *hr_dev,
			      struct list_head *head)
{
	struct hns_roce_hem_item *hem, *temp_hem;

	list_for_each_entry_safe(hem, temp_hem, head, list) {
		list_del(&hem->list);
		hem_list_free_item(hr_dev, hem);
	}
}

static void hem_list_link_bt(void *base_addr, u64 table_addr)
{
	*(u64 *)(base_addr) = table_addr;
}

/* assign L0 table address to hem from root bt */
static void hem_list_assign_bt(struct hns_roce_hem_item *hem, void *cpu_addr,
			       u64 phy_addr)
{
	hem->addr = cpu_addr;
	hem->dma_addr = (dma_addr_t)phy_addr;
}

static inline bool hem_list_page_is_in_range(struct hns_roce_hem_item *hem,
					     int offset)
{
	return (hem->start <= offset && offset <= hem->end);
}

static struct hns_roce_hem_item *hem_list_search_item(struct list_head *ba_list,
						      int page_offset)
{
	struct hns_roce_hem_item *hem, *temp_hem;
	struct hns_roce_hem_item *found = NULL;

	list_for_each_entry_safe(hem, temp_hem, ba_list, list) {
		if (hem_list_page_is_in_range(hem, page_offset)) {
			found = hem;
			break;
		}
	}

	return found;
}

static bool hem_list_is_bottom_bt(int hopnum, int bt_level)
{
	/*
	 * hopnum    base address table levels
	 * 0		L0(buf)
	 * 1		L0 -> buf
	 * 2		L0 -> L1 -> buf
	 * 3		L0 -> L1 -> L2 -> buf
	 */
	return bt_level >= (hopnum ? hopnum - 1 : hopnum);
}

/*
 * calc base address entries num
 * @hopnum: num of mutihop addressing
 * @bt_level: base address table level
 * @unit: ba entries per bt page
 */
static u64 hem_list_calc_ba_range(int hopnum, int bt_level, int unit)
{
	u64 step;
	int max;
	int i;

	if (hopnum <= bt_level)
		return 0;
	/*
	 * hopnum  bt_level   range
	 * 1	      0       unit
	 * ------------
	 * 2	      0       unit * unit
	 * 2	      1       unit
	 * ------------
	 * 3	      0       unit * unit * unit
	 * 3	      1       unit * unit
	 * 3	      2       unit
	 */
	step = 1;
	max = hopnum - bt_level;
	for (i = 0; i < max; i++)
		step = step * unit;

	return step;
}

/*
 * calc the root ba entries which could cover all regions
 * @regions: buf region array
 * @region_cnt: array size of @regions
 * @unit: ba entries per bt page
 */
int hns_roce_hem_list_calc_root_ba(const struct hns_roce_buf_region *regions,
				   int region_cnt, int unit)
{
	struct hns_roce_buf_region *r;
	int total = 0;
	u64 step;
	int i;

	for (i = 0; i < region_cnt; i++) {
		r = (struct hns_roce_buf_region *)&regions[i];
		/* when r->hopnum = 0, the region should not occupy root_ba. */
		if (!r->hopnum)
			continue;

		if (r->hopnum > 1) {
			step = hem_list_calc_ba_range(r->hopnum, 1, unit);
			if (step > 0)
				total += (r->count + step - 1) / step;
		} else {
			total += r->count;
		}
	}

	return total;
}

static int hem_list_alloc_mid_bt(struct hns_roce_dev *hr_dev,
				 const struct hns_roce_buf_region *r, int unit,
				 int offset, struct list_head *mid_bt,
				 struct list_head *btm_bt)
{
	struct hns_roce_hem_item *hem_ptrs[HNS_ROCE_MAX_BT_LEVEL] = { NULL };
	struct list_head temp_list[HNS_ROCE_MAX_BT_LEVEL];
	struct hns_roce_hem_item *cur, *pre;
	const int hopnum = r->hopnum;
	int start_aligned;
	int distance;
	int ret = 0;
	int max_ofs;
	int level;
	u64 step;
	int end;

	if (hopnum <= 1)
		return 0;

	if (hopnum > HNS_ROCE_MAX_BT_LEVEL) {
		dev_err(hr_dev->dev, "invalid hopnum %d!\n", hopnum);
		return -EINVAL;
	}

	if (offset < r->offset) {
		dev_err(hr_dev->dev, "invalid offset %d, min %u!\n",
			offset, r->offset);
		return -EINVAL;
	}

	distance = offset - r->offset;
	max_ofs = r->offset + r->count - 1;
	for (level = 0; level < hopnum; level++)
		INIT_LIST_HEAD(&temp_list[level]);

	/* config L1 bt to last bt and link them to corresponding parent */
	for (level = 1; level < hopnum; level++) {
		if (!hem_list_is_bottom_bt(hopnum, level)) {
			cur = hem_list_search_item(&mid_bt[level], offset);
			if (cur) {
				hem_ptrs[level] = cur;
				continue;
			}
		}

		step = hem_list_calc_ba_range(hopnum, level, unit);
		if (step < 1) {
			ret = -EINVAL;
			goto err_exit;
		}

		start_aligned = (distance / step) * step + r->offset;
		end = min_t(u64, start_aligned + step - 1, max_ofs);
		cur = hem_list_alloc_item(hr_dev, start_aligned, end, unit,
					  true);
		if (!cur) {
			ret = -ENOMEM;
			goto err_exit;
		}
		hem_ptrs[level] = cur;
		list_add(&cur->list, &temp_list[level]);
		if (hem_list_is_bottom_bt(hopnum, level))
			list_add(&cur->sibling, &temp_list[0]);

		/* link bt to parent bt */
		if (level > 1) {
			pre = hem_ptrs[level - 1];
			step = (cur->start - pre->start) / step * BA_BYTE_LEN;
			hem_list_link_bt(pre->addr + step, cur->dma_addr);
		}
	}

	list_splice(&temp_list[0], btm_bt);
	for (level = 1; level < hopnum; level++)
		list_splice(&temp_list[level], &mid_bt[level]);

	return 0;

err_exit:
	for (level = 1; level < hopnum; level++)
		hem_list_free_all(hr_dev, &temp_list[level]);

	return ret;
}

static struct hns_roce_hem_item *
alloc_root_hem(struct hns_roce_dev *hr_dev, int unit, int *max_ba_num,
	       const struct hns_roce_buf_region *regions, int region_cnt)
{
	const struct hns_roce_buf_region *r;
	struct hns_roce_hem_item *hem;
	int ba_num;
	int offset;

	ba_num = hns_roce_hem_list_calc_root_ba(regions, region_cnt, unit);
	if (ba_num < 1)
		return ERR_PTR(-ENOMEM);

	if (ba_num > unit)
		return ERR_PTR(-ENOBUFS);

	offset = regions[0].offset;
	/* indicate to last region */
	r = &regions[region_cnt - 1];
	hem = hem_list_alloc_item(hr_dev, offset, r->offset + r->count - 1,
				  ba_num, true);
	if (!hem)
		return ERR_PTR(-ENOMEM);

	*max_ba_num = ba_num;

	return hem;
}

static int alloc_fake_root_bt(struct hns_roce_dev *hr_dev, void *cpu_base,
			      u64 phy_base, const struct hns_roce_buf_region *r,
			      struct list_head *branch_head,
			      struct list_head *leaf_head)
{
	struct hns_roce_hem_item *hem;

	/* This is on the has_mtt branch, if r->hopnum
	 * is 0, there is no root_ba to reuse for the
	 * region's fake hem, so a dma_alloc request is
	 * necessary here.
	 */
	hem = hem_list_alloc_item(hr_dev, r->offset, r->offset + r->count - 1,
				  r->count, !r->hopnum);
	if (!hem)
		return -ENOMEM;

	/* The root_ba can be reused only when r->hopnum > 0. */
	if (r->hopnum)
		hem_list_assign_bt(hem, cpu_base, phy_base);
	list_add(&hem->list, branch_head);
	list_add(&hem->sibling, leaf_head);

	/* If r->hopnum == 0, 0 is returned,
	 * so that the root_bt entry is not occupied.
	 */
	return r->hopnum ? r->count : 0;
}

static int setup_middle_bt(struct hns_roce_dev *hr_dev, void *cpu_base,
			   int unit, const struct hns_roce_buf_region *r,
			   const struct list_head *branch_head)
{
	struct hns_roce_hem_item *hem, *temp_hem;
	int total = 0;
	int offset;
	u64 step;

	step = hem_list_calc_ba_range(r->hopnum, 1, unit);
	if (step < 1)
		return -EINVAL;

	/* if exist mid bt, link L1 to L0 */
	list_for_each_entry_safe(hem, temp_hem, branch_head, list) {
		offset = (hem->start - r->offset) / step * BA_BYTE_LEN;
		hem_list_link_bt(cpu_base + offset, hem->dma_addr);
		total++;
	}

	return total;
}

static int
setup_root_hem(struct hns_roce_dev *hr_dev, struct hns_roce_hem_list *hem_list,
	       int unit, int max_ba_num, struct hns_roce_hem_head *head,
	       const struct hns_roce_buf_region *regions, int region_cnt)
{
	const struct hns_roce_buf_region *r;
	struct hns_roce_hem_item *root_hem;
	void *cpu_base;
	u64 phy_base;
	int i, total;
	int ret;

	root_hem = list_first_entry(&head->root,
				    struct hns_roce_hem_item, list);
	if (!root_hem)
		return -ENOMEM;

	total = 0;
	for (i = 0; i < region_cnt && total <= max_ba_num; i++) {
		r = &regions[i];
		if (!r->count)
			continue;

		/* all regions's mid[x][0] shared the root_bt's trunk */
		cpu_base = root_hem->addr + total * BA_BYTE_LEN;
		phy_base = root_hem->dma_addr + total * BA_BYTE_LEN;

		/* if hopnum is 0 or 1, cut a new fake hem from the root bt
		 * which's address share to all regions.
		 */
		if (hem_list_is_bottom_bt(r->hopnum, 0))
			ret = alloc_fake_root_bt(hr_dev, cpu_base, phy_base, r,
						 &head->branch[i], &head->leaf);
		else
			ret = setup_middle_bt(hr_dev, cpu_base, unit, r,
					      &hem_list->mid_bt[i][1]);

		if (ret < 0)
			return ret;

		total += ret;
	}

	list_splice(&head->leaf, &hem_list->btm_bt);
	list_splice(&head->root, &hem_list->root_bt);
	for (i = 0; i < region_cnt; i++)
		list_splice(&head->branch[i], &hem_list->mid_bt[i][0]);

	return 0;
}

static int hem_list_alloc_root_bt(struct hns_roce_dev *hr_dev,
				  struct hns_roce_hem_list *hem_list, int unit,
				  const struct hns_roce_buf_region *regions,
				  int region_cnt)
{
	struct hns_roce_hem_item *root_hem;
	struct hns_roce_hem_head head;
	int max_ba_num;
	int ret;
	int i;

	root_hem = hem_list_search_item(&hem_list->root_bt, regions[0].offset);
	if (root_hem)
		return 0;

	max_ba_num = 0;
	root_hem = alloc_root_hem(hr_dev, unit, &max_ba_num, regions,
				  region_cnt);
	if (IS_ERR(root_hem))
		return PTR_ERR(root_hem);

	/* List head for storing all allocated HEM items */
	INIT_LIST_HEAD(&head.root);
	INIT_LIST_HEAD(&head.leaf);
	for (i = 0; i < region_cnt; i++)
		INIT_LIST_HEAD(&head.branch[i]);

	hem_list->root_ba = root_hem->dma_addr;
	list_add(&root_hem->list, &head.root);
	ret = setup_root_hem(hr_dev, hem_list, unit, max_ba_num, &head, regions,
			     region_cnt);
	if (ret) {
		for (i = 0; i < region_cnt; i++)
			hem_list_free_all(hr_dev, &head.branch[i]);

		hem_list_free_all(hr_dev, &head.root);
	}

	return ret;
}

/* This is the bottom bt pages number of a 100G MR on 4K OS, assuming
 * the bt page size is not expanded by cal_best_bt_pg_sz()
 */
#define RESCHED_LOOP_CNT_THRESHOLD_ON_4K 12800

/* construct the base address table and link them by address hop config */
int hns_roce_hem_list_request(struct hns_roce_dev *hr_dev,
			      struct hns_roce_hem_list *hem_list,
			      const struct hns_roce_buf_region *regions,
			      int region_cnt, unsigned int bt_pg_shift)
{
	const struct hns_roce_buf_region *r;
	int ofs, end;
	int loop;
	int unit;
	int ret;
	int i;

	if (region_cnt > HNS_ROCE_MAX_BT_REGION) {
		dev_err(hr_dev->dev, "invalid region region_cnt %d!\n",
			region_cnt);
		return -EINVAL;
	}

	unit = (1 << bt_pg_shift) / BA_BYTE_LEN;
	for (i = 0; i < region_cnt; i++) {
		r = &regions[i];
		if (!r->count)
			continue;

		end = r->offset + r->count;
		for (ofs = r->offset, loop = 1; ofs < end; ofs += unit, loop++) {
			if (!(loop % RESCHED_LOOP_CNT_THRESHOLD_ON_4K))
				cond_resched();

			ret = hem_list_alloc_mid_bt(hr_dev, r, unit, ofs,
						    hem_list->mid_bt[i],
						    &hem_list->btm_bt);
			if (ret) {
				dev_err(hr_dev->dev,
					"alloc hem trunk fail ret = %d!\n", ret);
				goto err_alloc;
			}
		}
	}

	ret = hem_list_alloc_root_bt(hr_dev, hem_list, unit, regions,
				     region_cnt);
	if (ret)
		dev_err(hr_dev->dev, "alloc hem root fail ret = %d!\n", ret);
	else
		return 0;

err_alloc:
	hns_roce_hem_list_release(hr_dev, hem_list);

	return ret;
}

void hns_roce_hem_list_release(struct hns_roce_dev *hr_dev,
			       struct hns_roce_hem_list *hem_list)
{
	int i, j;

	for (i = 0; i < HNS_ROCE_MAX_BT_REGION; i++)
		for (j = 0; j < HNS_ROCE_MAX_BT_LEVEL; j++)
			hem_list_free_all(hr_dev, &hem_list->mid_bt[i][j]);

	hem_list_free_all(hr_dev, &hem_list->root_bt);
	INIT_LIST_HEAD(&hem_list->btm_bt);
	hem_list->root_ba = 0;
}

void hns_roce_hem_list_init(struct hns_roce_hem_list *hem_list)
{
	int i, j;

	INIT_LIST_HEAD(&hem_list->root_bt);
	INIT_LIST_HEAD(&hem_list->btm_bt);
	for (i = 0; i < HNS_ROCE_MAX_BT_REGION; i++)
		for (j = 0; j < HNS_ROCE_MAX_BT_LEVEL; j++)
			INIT_LIST_HEAD(&hem_list->mid_bt[i][j]);
}

void *hns_roce_hem_list_find_mtt(struct hns_roce_dev *hr_dev,
				 struct hns_roce_hem_list *hem_list,
				 int offset, int *mtt_cnt)
{
	struct list_head *head = &hem_list->btm_bt;
	struct hns_roce_hem_item *hem, *temp_hem;
	void *cpu_base = NULL;
	int loop = 1;
	int nr = 0;

	list_for_each_entry_safe(hem, temp_hem, head, sibling) {
		if (!(loop % RESCHED_LOOP_CNT_THRESHOLD_ON_4K))
			cond_resched();
		loop++;

		if (hem_list_page_is_in_range(hem, offset)) {
			nr = offset - hem->start;
			cpu_base = hem->addr + nr * BA_BYTE_LEN;
			nr = hem->end + 1 - offset;
			break;
		}
	}

	if (mtt_cnt)
		*mtt_cnt = nr;

	return cpu_base;
}
