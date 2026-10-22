// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/sched/clock.h>
#include <linux/jiffies.h>
#include <linux/seq_file.h>

#include "nvmev.h"
#include "ssd.h"
#include "zns_ftl.h"

extern unsigned int stats_interval_ms;

static void __init_descriptor(struct zns_ftl *zns_ftl)
{
	struct zone_descriptor *zone_descs;
	uint32_t zone_size = zns_ftl->zp.zone_size;
	uint32_t nr_zones = zns_ftl->zp.nr_zones;
	uint64_t zslba = 0;
	uint32_t i = 0;
	const uint32_t zrwa_buffer_size = zns_ftl->zp.zrwa_buffer_size;
	const uint32_t zone_wb_size = zns_ftl->zp.zone_wb_size;

	zns_ftl->zone_descs = kzalloc(sizeof(struct zone_descriptor) * nr_zones, GFP_KERNEL);
	zns_ftl->report_buffer = kmalloc(
		sizeof(struct zone_report) + sizeof(struct zone_descriptor) * nr_zones, GFP_KERNEL);

	if (zrwa_buffer_size)
		zns_ftl->zrwa_buffer = kmalloc(sizeof(struct buffer) * nr_zones, GFP_KERNEL);

	if (zone_wb_size)
		zns_ftl->zone_write_buffer = kmalloc(sizeof(struct buffer) * nr_zones, GFP_KERNEL);

	zone_descs = zns_ftl->zone_descs;

	for (i = 0; i < nr_zones; i++) {
		zone_descs[i].state = ZONE_STATE_EMPTY;
		zone_descs[i].type = ZONE_TYPE_SEQ_WRITE_REQUIRED;

		zone_descs[i].zslba = zslba;
		zone_descs[i].wp = zslba;
		zslba += BYTE_TO_LBA(zone_size);
		zone_descs[i].zone_capacity = BYTE_TO_LBA(zone_size);

		if (zrwa_buffer_size)
			buffer_init(&(zns_ftl->zrwa_buffer[i]), zrwa_buffer_size);

		if (zone_wb_size)
			buffer_init(&(zns_ftl->zone_write_buffer[i]), zone_wb_size);

		NVMEV_ZNS_DEBUG("[%d] zslba 0x%llx zone capacity 0x%llx, wp 0x%llx\n", i,
			zone_descs[i].zslba, zone_descs[i].zone_capacity, zone_descs[i].wp);
	}
}

static void __remove_descriptor(struct zns_ftl *zns_ftl)
{
	if (zns_ftl->zp.zrwa_buffer_size)
		kfree(zns_ftl->zrwa_buffer);

	if (zns_ftl->zp.zone_wb_size)
		kfree(zns_ftl->zone_write_buffer);

	kfree(zns_ftl->report_buffer);
	kfree(zns_ftl->zone_descs);

	kfree(zns_ftl->stats.zone_ios);
	kfree(zns_ftl->stats.zone_bytes);
	kfree(zns_ftl->stats.zone_out);
	kfree(zns_ftl->stats.chan_ios);
	kfree(zns_ftl->stats.chan_bytes);
	kfree(zns_ftl->stats.chan_out);
}

static void __init_resource(struct zns_ftl *zns_ftl)
{
	struct zone_resource_info *res_infos = zns_ftl->res_infos;

	res_infos[ACTIVE_ZONE] = (struct zone_resource_info){
		.total_cnt = zns_ftl->zp.nr_zones,
		.acquired_cnt = 0,
	};

	res_infos[OPEN_ZONE] = (struct zone_resource_info){
		.total_cnt = zns_ftl->zp.nr_zones,
		.acquired_cnt = 0,
	};

	res_infos[ZRWA_ZONE] = (struct zone_resource_info){
		.total_cnt = zns_ftl->zp.nr_zones,
		.acquired_cnt = 0,
	};
}

static void zns_init_params(struct znsparams *zpp, struct ssdparams *spp, uint64_t capacity)
{
	*zpp = (struct znsparams){
		.zone_size = ZONE_SIZE,
		.nr_zones = capacity / ZONE_SIZE,
		.dies_per_zone = DIES_PER_ZONE,
		.nr_active_zones = capacity / ZONE_SIZE, // max
		.nr_open_zones = capacity / ZONE_SIZE, // max
		.nr_zrwa_zones = MAX_ZRWA_ZONES,
		.zone_wb_size = ZONE_WB_SIZE,
		.zrwa_size = ZRWA_SIZE,
		.zrwafg_size = ZRWAFG_SIZE,
		.zrwa_buffer_size = ZRWA_BUFFER_SIZE,
		.lbas_per_zrwa = ZRWA_SIZE / spp->secsz,
		.lbas_per_zrwafg = ZRWAFG_SIZE / spp->secsz,
	};

	NVMEV_ASSERT((capacity % zpp->zone_size) == 0);
	/* It should be 4KB aligned, according to lpn size */
	NVMEV_ASSERT((zpp->zone_size % spp->pgsz) == 0);

	NVMEV_INFO("zone_size=%u(Byte),%u(MB), # zones=%d # die/zone=%d \n", zpp->zone_size,
		   BYTE_TO_MB(zpp->zone_size), zpp->nr_zones, zpp->dies_per_zone);
}

static void zns_init_ftl(struct zns_ftl *zns_ftl, struct znsparams *zpp, struct ssd *ssd,
			 void *mapped_addr)
{
	*zns_ftl = (struct zns_ftl){
		.zp = *zpp, /*copy znsparams*/

		.ssd = ssd,
		.storage_base_addr = mapped_addr,
	};

	__init_descriptor(zns_ftl);
	__init_resource(zns_ftl);

	spin_lock_init(&zns_ftl->stats.lock);
	zns_ftl->stats.zone_ios =
		kcalloc(zpp->nr_zones, sizeof(u64), GFP_KERNEL);
	zns_ftl->stats.zone_bytes =
		kcalloc(zpp->nr_zones, sizeof(u64), GFP_KERNEL);
	zns_ftl->stats.zone_out =
		kcalloc(zpp->nr_zones, sizeof(u32), GFP_KERNEL);
	zns_ftl->stats.chan_ios =
		kcalloc(ssd->sp.nchs, sizeof(u64), GFP_KERNEL);
	zns_ftl->stats.chan_bytes =
		kcalloc(ssd->sp.nchs, sizeof(u64), GFP_KERNEL);
	zns_ftl->stats.chan_out =
		kcalloc(ssd->sp.nchs, sizeof(u32), GFP_KERNEL);
	zns_ftl->stats.last_jiffies = jiffies;
}

void zns_stats_begin(struct nvmev_ns *ns, uint32_t zid, uint64_t bytes)
{
	struct zns_ftl *zns_ftl;
	struct zns_stats *st;
	uint32_t ch;

	if (!ns || ns->csi != NVME_CSI_ZNS)
		return;

	zns_ftl = (struct zns_ftl *)ns->ftls;
	if (!zns_ftl || zid >= zns_ftl->zp.nr_zones)
		return;
	st = &zns_ftl->stats;
	ch = zns_zone_to_channel(zns_ftl, zid);
	if (ch >= zns_ftl->ssd->sp.nchs)
		return;

	spin_lock(&st->lock);
	st->zone_out[zid]++;
	if (st->zone_out[zid] == 1) {
		st->active_zones++;
		if (st->active_zones > st->max_active_zones)
			st->max_active_zones = st->active_zones;
		if (st->active_zones > st->interval_max_zones)
			st->interval_max_zones = st->active_zones;
	}

	st->chan_out[ch]++;
	if (st->chan_out[ch] == 1) {
		st->active_channels++;
		if (st->active_channels > st->max_active_channels)
			st->max_active_channels = st->active_channels;
		if (st->active_channels > st->interval_max_channels)
			st->interval_max_channels = st->active_channels;
	}

	st->zone_ios[zid]++;
	st->zone_bytes[zid] += bytes;
	st->chan_ios[ch] += bytes;
	st->chan_bytes[ch] += bytes;
	st->total_ios++;
	st->total_bytes += bytes;
	st->interval_ios++;
	st->interval_bytes += bytes;
	spin_unlock(&st->lock);
}

void zns_stats_end(struct nvmev_ns *ns, uint32_t zid)
{
	struct zns_ftl *zns_ftl;
	struct zns_stats *st;
	uint32_t ch;

	if (!ns || ns->csi != NVME_CSI_ZNS)
		return;

	zns_ftl = (struct zns_ftl *)ns->ftls;
	if (!zns_ftl || zid >= zns_ftl->zp.nr_zones)
		return;
	st = &zns_ftl->stats;
	ch = zns_zone_to_channel(zns_ftl, zid);
	if (ch >= zns_ftl->ssd->sp.nchs)
		return;

	spin_lock(&st->lock);
	if (st->zone_out[zid]) {
		st->zone_out[zid]--;
		if (st->zone_out[zid] == 0 && st->active_zones)
			st->active_zones--;
	}
	if (st->chan_out[ch]) {
		st->chan_out[ch]--;
		if (st->chan_out[ch] == 0 && st->active_channels)
			st->active_channels--;
	}
	spin_unlock(&st->lock);
}

void zns_complete_io_cmd(struct nvmev_ns *ns, struct nvme_command *cmd)
{
	struct zns_ftl *zns_ftl;
	uint32_t zid;

	if (!ns || ns->csi != NVME_CSI_ZNS || !cmd)
		return;

	zns_ftl = (struct zns_ftl *)ns->ftls;
	if (!zns_ftl)
		return;

	switch (cmd->common.opcode) {
	case nvme_cmd_read:
	case nvme_cmd_write:
	case nvme_cmd_zone_append:
		zid = lba_to_zone(zns_ftl, cmd->rw.slba);
		zns_stats_end(ns, zid);
		break;
	default:
		break;
	}
}

void zns_stats_tick(struct nvmev_ns *ns)
{
	struct zns_ftl *zns_ftl;
	struct zns_stats *st;
	unsigned long interval;
	u64 avg_zones, avg_channels;
	u32 cur_zones, cur_channels, max_zones, max_channels;
	u64 interval_ios, interval_bytes;

	if (!ns || ns->csi != NVME_CSI_ZNS)
		return;

	zns_ftl = (struct zns_ftl *)ns->ftls;
	if (!zns_ftl)
		return;
	st = &zns_ftl->stats;

	if (!stats_interval_ms)
		return;

	interval = msecs_to_jiffies(stats_interval_ms);
	if (!time_after(jiffies, st->last_jiffies + interval))
		return;

	spin_lock(&st->lock);
	st->interval_sum_zones += st->active_zones;
	st->interval_sum_channels += st->active_channels;
	st->interval_samples++;

	avg_zones = st->interval_sum_zones / max_t(u64, st->interval_samples, 1);
	avg_channels = st->interval_sum_channels / max_t(u64, st->interval_samples, 1);
	cur_zones = st->active_zones;
	cur_channels = st->active_channels;
	max_zones = st->interval_max_zones;
	max_channels = st->interval_max_channels;
	interval_ios = st->interval_ios;
	interval_bytes = st->interval_bytes;

	st->interval_sum_zones = 0;
	st->interval_sum_channels = 0;
	st->interval_samples = 0;
	st->interval_max_zones = st->active_zones;
	st->interval_max_channels = st->active_channels;
	st->interval_ios = 0;
	st->interval_bytes = 0;
	st->last_jiffies = jiffies;
	spin_unlock(&st->lock);

	NVMEV_INFO("ZNS parallel: az cur=%u avg=%llu max=%u | ac cur=%u avg=%llu max=%u | ios=%llu bytes=%llu\n",
		   cur_zones, avg_zones, max_zones, cur_channels, avg_channels,
		   max_channels, interval_ios, interval_bytes);
}

void zns_stats_dump(struct nvmev_ns *ns, struct seq_file *m)
{
	struct zns_ftl *zns_ftl;
	struct zns_stats *st;
	u64 *zone_ios = NULL, *zone_bytes = NULL;
	u64 *chan_ios = NULL, *chan_bytes = NULL;
	u32 *chan_out = NULL;
	u32 nr_zones, nchs, zid, ch;
	u64 total_ios, total_bytes;
	u32 active_zones, max_active_zones, active_channels, max_active_channels;
	int i;

	if (!ns || ns->csi != NVME_CSI_ZNS)
		return;

	zns_ftl = (struct zns_ftl *)ns->ftls;
	if (!zns_ftl)
		return;
	st = &zns_ftl->stats;
	nr_zones = zns_ftl->zp.nr_zones;
	nchs = zns_ftl->ssd->sp.nchs;

	zone_ios = kmalloc_array(nr_zones, sizeof(u64), GFP_KERNEL);
	zone_bytes = kmalloc_array(nr_zones, sizeof(u64), GFP_KERNEL);
	chan_ios = kmalloc_array(nchs, sizeof(u64), GFP_KERNEL);
	chan_bytes = kmalloc_array(nchs, sizeof(u64), GFP_KERNEL);
	chan_out = kmalloc_array(nchs, sizeof(u32), GFP_KERNEL);
	if (!zone_ios || !zone_bytes || !chan_ios || !chan_bytes || !chan_out)
		goto out;

	spin_lock(&st->lock);
	total_ios = st->total_ios;
	total_bytes = st->total_bytes;
	active_zones = st->active_zones;
	max_active_zones = st->max_active_zones;
	active_channels = st->active_channels;
	max_active_channels = st->max_active_channels;
	memcpy(zone_ios, st->zone_ios, nr_zones * sizeof(u64));
	memcpy(zone_bytes, st->zone_bytes, nr_zones * sizeof(u64));
	memcpy(chan_ios, st->chan_ios, nchs * sizeof(u64));
	memcpy(chan_bytes, st->chan_bytes, nchs * sizeof(u64));
	memcpy(chan_out, st->chan_out, nchs * sizeof(u32));
	spin_unlock(&st->lock);

	seq_printf(m,
		   "zns: zones=%u channels=%u total_ios=%llu total_bytes=%llu active_zones=%u max_active_zones=%u active_channels=%u max_active_channels=%u\n",
		   nr_zones, nchs, total_ios, total_bytes, active_zones, max_active_zones,
		   active_channels, max_active_channels);

	for (ch = 0; ch < nchs; ch++) {
		struct ssd_channel *sdc = &zns_ftl->ssd->ch[ch];
		u64 busy = atomic64_read(&sdc->stat_busy_ns);
		u64 nand_ops = atomic64_read(&sdc->stat_ops);
		u64 nand_bytes = atomic64_read(&sdc->stat_bytes);

		seq_printf(m,
			   "zns_chan: ch=%u host_ios=%llu host_bytes=%llu nand_ops=%llu nand_bytes=%llu busy_ns=%llu out=%u\n",
			   ch, chan_ios[ch], chan_bytes[ch], nand_ops, nand_bytes, busy,
			   chan_out[ch]);
	}

	for (zid = 0; zid < nr_zones; zid++) {
		if (!zone_ios[zid] && !zone_bytes[zid])
			continue;
		seq_printf(m, "zns_zone: zid=%u ios=%llu bytes=%llu out=%u\n", zid,
			   zone_ios[zid], zone_bytes[zid],
			   zns_ftl->stats.zone_out[zid]);
	}

out:
	kfree(zone_ios);
	kfree(zone_bytes);
	kfree(chan_ios);
	kfree(chan_bytes);
	kfree(chan_out);
}

void zns_stats_reset(struct nvmev_ns *ns)
{
	struct zns_ftl *zns_ftl;
	struct zns_stats *st;
	u32 i;

	if (!ns || ns->csi != NVME_CSI_ZNS)
		return;

	zns_ftl = (struct zns_ftl *)ns->ftls;
	if (!zns_ftl)
		return;
	st = &zns_ftl->stats;

	spin_lock(&st->lock);
	memset(st->zone_ios, 0, zns_ftl->zp.nr_zones * sizeof(u64));
	memset(st->zone_bytes, 0, zns_ftl->zp.nr_zones * sizeof(u64));
	memset(st->zone_out, 0, zns_ftl->zp.nr_zones * sizeof(u32));
	memset(st->chan_ios, 0, zns_ftl->ssd->sp.nchs * sizeof(u64));
	memset(st->chan_bytes, 0, zns_ftl->ssd->sp.nchs * sizeof(u64));
	memset(st->chan_out, 0, zns_ftl->ssd->sp.nchs * sizeof(u32));
	st->total_ios = 0;
	st->total_bytes = 0;
	st->active_zones = 0;
	st->max_active_zones = 0;
	st->active_channels = 0;
	st->max_active_channels = 0;
	st->interval_ios = 0;
	st->interval_bytes = 0;
	st->interval_sum_zones = 0;
	st->interval_sum_channels = 0;
	st->interval_samples = 0;
	st->interval_max_zones = 0;
	st->interval_max_channels = 0;
	st->last_jiffies = jiffies;
	spin_unlock(&st->lock);

	for (i = 0; i < zns_ftl->ssd->sp.nchs; i++) {
		atomic64_set(&zns_ftl->ssd->ch[i].stat_ops, 0);
		atomic64_set(&zns_ftl->ssd->ch[i].stat_bytes, 0);
		atomic64_set(&zns_ftl->ssd->ch[i].stat_busy_ns, 0);
	}
}

void zns_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			uint32_t cpu_nr_dispatcher)
{
	struct ssd *ssd;
	struct zns_ftl *zns_ftl;

	struct ssdparams spp;
	struct znsparams zpp;

	const uint32_t nr_parts = 1; /* Not support multi partitions for zns*/
	NVMEV_ASSERT(nr_parts == 1);

	ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
	ssd_init_params(&spp, size, nr_parts);
	ssd_init(ssd, &spp, cpu_nr_dispatcher);

	zns_ftl = kmalloc(sizeof(struct zns_ftl) * nr_parts, GFP_KERNEL);
	zns_init_params(&zpp, &spp, size);
	zns_init_ftl(zns_ftl, &zpp, ssd, mapped_addr);

	*ns = (struct nvmev_ns){
		.id = id,
		.csi = NVME_CSI_ZNS,
		.nr_parts = nr_parts,
		.ftls = (void *)zns_ftl,
		.size = size,
		.mapped = mapped_addr,

		/*register io command handler*/
		.proc_io_cmd = zns_proc_nvme_io_cmd,
		.complete_io_cmd = zns_complete_io_cmd,
	};
	return;
}

void zns_remove_namespace(struct nvmev_ns *ns)
{
	struct zns_ftl *zns_ftl = (struct zns_ftl *)ns->ftls;

	ssd_remove(zns_ftl->ssd);

	__remove_descriptor(zns_ftl);
	kfree(zns_ftl->ssd);
	kfree(zns_ftl);

	ns->ftls = NULL;
}

static void zns_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	uint64_t start, latest;
	uint32_t i;
	struct zns_ftl *zns_ftl = (struct zns_ftl *)ns->ftls;

	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++) {
		latest = max(latest, ssd_next_idle_time(zns_ftl[i].ssd));
	}

	NVMEV_DEBUG("%s latency=%llu\n", __func__, latest - start);

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

bool zns_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;
	NVMEV_ASSERT(ns->csi == NVME_CSI_ZNS);
	/*still not support multi partitions ...*/
	NVMEV_ASSERT(ns->nr_parts == 1);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
	case nvme_cmd_zone_append:
		if (!zns_write(ns, req, ret))
			return false;
		if (ret->status == NVME_SC_SUCCESS) {
			struct zns_ftl *zns_ftl = (struct zns_ftl *)ns->ftls;
			uint32_t zid = lba_to_zone(zns_ftl, cmd->rw.slba);

			zns_stats_begin(ns, zid, LBA_TO_BYTE(cmd->rw.length + 1));
		}
		break;
	case nvme_cmd_read:
		if (!zns_read(ns, req, ret))
			return false;
		if (ret->status == NVME_SC_SUCCESS) {
			struct zns_ftl *zns_ftl = (struct zns_ftl *)ns->ftls;
			uint32_t zid = lba_to_zone(zns_ftl, cmd->rw.slba);

			zns_stats_begin(ns, zid, LBA_TO_BYTE(cmd->rw.length + 1));
		}
		break;
	case nvme_cmd_flush:
		zns_flush(ns, req, ret);
		break;
	case nvme_cmd_zone_mgmt_send:
		zns_zmgmt_send(ns, req, ret);
		break;
	case nvme_cmd_zone_mgmt_recv:
		zns_zmgmt_recv(ns, req, ret);
		break;
	default:
		NVMEV_ERROR("%s: unimplemented command: %s(%d)\n", __func__,
			    nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

	return true;
}
