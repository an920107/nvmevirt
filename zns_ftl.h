// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_ZNS_FTL_H
#define _NVMEVIRT_ZNS_FTL_H

#include <linux/types.h>
#include "nvmev.h"
#include "nvme_zns.h"

#define NVMEV_ZNS_DEBUG(string, args...) //printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)

struct seq_file;

// Zoned Namespace Command Set Specification Revision 1.1a
struct znsparams {
	uint32_t nr_zones;
	uint32_t nr_active_zones;
	uint32_t nr_open_zones;
	uint32_t dies_per_zone;
	uint32_t zone_size; //bytes
	uint32_t zone_wb_size;

	/*related to zrwa*/
	uint32_t nr_zrwa_zones;
	uint32_t zrwafg_size;
	uint32_t zrwa_size;
	uint32_t zrwa_buffer_size;
	uint32_t lbas_per_zrwafg;
	uint32_t lbas_per_zrwa;
};

struct zone_resource_info {
	__u32 acquired_cnt;
	__u32 total_cnt;
};

struct zns_stats {
	spinlock_t lock;
	u64 total_ios;
	u64 total_bytes;

	u64 *zone_ios;
	u64 *zone_bytes;
	u32 *zone_out;

	u64 *chan_ios;
	u64 *chan_bytes;
	u32 *chan_out;

	u32 active_zones;
	u32 max_active_zones;
	u32 active_channels;
	u32 max_active_channels;

	u64 interval_ios;
	u64 interval_bytes;
	u64 interval_sum_zones;
	u64 interval_sum_channels;
	u64 interval_samples;
	u32 interval_max_zones;
	u32 interval_max_channels;
	unsigned long last_jiffies;
};

struct zns_ftl {
	struct ssd *ssd;

	struct znsparams zp;
	struct zone_resource_info res_infos[RES_TYPE_COUNT];
	struct zone_descriptor *zone_descs;
	struct zone_report *report_buffer;
	struct buffer *zone_write_buffer;
	struct buffer *zrwa_buffer;
	void *storage_base_addr;

	struct zns_stats stats;
};

/* zns internal functions */
static inline void *get_storage_addr_from_zid(struct zns_ftl *zns_ftl, uint64_t zid)
{
	return (void *)((char *)zns_ftl->storage_base_addr + zid * zns_ftl->zp.zone_size);
}

static inline bool is_zone_resource_avail(struct zns_ftl *zns_ftl, uint32_t type)
{
	return zns_ftl->res_infos[type].acquired_cnt < zns_ftl->res_infos[type].total_cnt;
}

static inline bool is_zone_resource_full(struct zns_ftl *zns_ftl, uint32_t type)
{
	return zns_ftl->res_infos[type].acquired_cnt == zns_ftl->res_infos[type].total_cnt;
}

static inline bool acquire_zone_resource(struct zns_ftl *zns_ftl, uint32_t type)
{
	if (is_zone_resource_avail(zns_ftl, type)) {
		zns_ftl->res_infos[type].acquired_cnt++;
		return true;
	}

	return false;
}

static inline void release_zone_resource(struct zns_ftl *zns_ftl, uint32_t type)
{
	ASSERT(zns_ftl->res_infos[type].acquired_cnt > 0);

	zns_ftl->res_infos[type].acquired_cnt--;
}

static inline void change_zone_state(struct zns_ftl *zns_ftl, uint32_t zid, enum zone_state state)
{
	NVMEV_ZNS_DEBUG("change state zid %d from %d to %d \n", zid, zns_ftl->zone_descs[zid].state,
			state);

	// check if transition is correct
	zns_ftl->zone_descs[zid].state = state;
}

static inline uint32_t lpn_to_zone(struct zns_ftl *zns_ftl, uint64_t lpn)
{
	return (lpn) / (zns_ftl->zp.zone_size / zns_ftl->ssd->sp.pgsz);
}

static inline uint64_t zone_to_slpn(struct zns_ftl *zns_ftl, uint32_t zid)
{
	return (zid) * (zns_ftl->zp.zone_size / zns_ftl->ssd->sp.pgsz);
}

static inline uint32_t lba_to_zone(struct zns_ftl *zns_ftl, uint64_t lba)
{
	return (lba) / (zns_ftl->zp.zone_size / zns_ftl->ssd->sp.secsz);
}

static inline uint64_t zone_to_slba(struct zns_ftl *zns_ftl, uint32_t zid)
{
	return (zid) * (zns_ftl->zp.zone_size / zns_ftl->ssd->sp.secsz);
}

static inline uint64_t zone_to_elba(struct zns_ftl *zns_ftl, uint32_t zid)
{
	return zone_to_slba(zns_ftl, zid + 1) - 1;
}

static inline uint64_t zone_to_elpn(struct zns_ftl *zns_ftl, uint32_t zid)
{
	return zone_to_elba(zns_ftl, zid) / zns_ftl->ssd->sp.secs_per_pg;
}

static inline uint32_t die_to_channel(struct zns_ftl *zns_ftl, uint32_t die)
{
	return (die) % zns_ftl->ssd->sp.nchs;
}

static inline uint32_t die_to_lun(struct zns_ftl *zns_ftl, uint32_t die)
{
	return (die) / zns_ftl->ssd->sp.nchs;
}

static inline uint32_t zns_zone_to_channel(struct zns_ftl *zns_ftl, uint32_t zid)
{
	struct ssdparams *spp = &zns_ftl->ssd->sp;
	uint32_t die = (zid * zns_ftl->zp.dies_per_zone) % spp->tt_luns;

	return die_to_channel(zns_ftl, die);
}

static inline uint64_t lba_to_lpn(struct zns_ftl *zns_ftl, uint64_t lba)
{
	return lba / zns_ftl->ssd->sp.secs_per_pg;
}

/* zns external interface */
void zns_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			uint32_t cpu_nr_dispatcher);
void zns_remove_namespace(struct nvmev_ns *ns);

void zns_zmgmt_recv(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret);
void zns_zmgmt_send(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret);

/* parallelism statistics */
void zns_stats_begin(struct nvmev_ns *ns, uint32_t zid, uint64_t bytes);
void zns_stats_end(struct nvmev_ns *ns, uint32_t zid);
void zns_stats_tick(struct nvmev_ns *ns);
void zns_stats_dump(struct nvmev_ns *ns, struct seq_file *m);
void zns_stats_reset(struct nvmev_ns *ns);
void zns_complete_io_cmd(struct nvmev_ns *ns, struct nvme_command *cmd);
bool zns_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret);
bool zns_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret);
bool zns_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret);
#endif
