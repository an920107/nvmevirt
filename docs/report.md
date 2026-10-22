# Lab 1 Report — NVMeVirt with ZNS vs. Traditional FTL

**Course:** NVM Lab 1  
**Date:** 2026-10-22  
**Environment:** Debian 13, Linux 6.12.113, QEMU/KVM, NVMeVirt commit `c93e88c`  
**Repository:** <https://github.com/an920107/nvmevirt/tree/lab1>

---

## 1. Objectives

This lab studies how NVMeVirt implements a conventional FTL namespace and a
Zoned Namespace (ZNS), and how the two designs affect host-visible parallelism.
The deliverables are:

1. Two namespaces running on one virtual NVMe controller, with Identify output.
2. Benchmarks on both namespaces.
3. A code-path comparison between conventional FTL and ZNS.
4. Modified NVMeVirt logging/statistics for ZNS parallelism.
5. A comparison of ZoneFS and F2FS parallelism on the ZNS namespace.

---

## 2. Experimental setup

### 2.1 Platform

| Item | Value |
|---|---|
| VM | QEMU/KVM, 24 vCPUs, 20 GiB RAM |
| OS | Debian 13 (trixie) |
| Kernel | `6.12.113+deb13-amd64` |
| Reserved memory | `memmap=16385M$6G` |
| OS-visible memory | ~4 GiB (16 GiB reserved for NVMeVirt) |
| NVMeVirt | commit `c93e88c` |

The kernel command line used for the experiments is:

```text
memmap=16385M$6G isolcpus=2,3,4,5 iommu=off intremap=off quiet
```

`isolcpus` isolates the NVMeVirt dispatcher and I/O worker CPUs. `iommu=off` and
`intremap=off` are required because NVMeVirt's data path assumes PRP values are
physical addresses; an active IOMMU supplies IOVAs instead.

Secure Boot was disabled in the VM firmware. A local MOK key and a sign-file
helper are included in `scripts/` for environments where Secure Boot must remain
enabled.

### 2.2 Dual-namespace configuration

The stock NVMeVirt tree has the skeleton for two namespaces
(`NR_NAMESPACES`, `NS_SSD_TYPE_0/1`, `NS_CAPACITY_0/1`), but every existing
`BASE_SSD` model sets `NR_NAMESPACES=1`. A lab-only `LAB_MIXED` target was added
in `ssd_config.h` and selected in `Kbuild`.

| Property | Namespace 1 | Namespace 2 |
|---|---|---|
| Type | Conventional FTL | ZNS |
| Device | `/dev/nvme0n1` | `/dev/nvme0n2` |
| Capacity | 7.47 GiB logical (8 GiB physical) | 8 GiB |
| FTL | `conv_ftl.c` (4 partitions) | none |
| Addressing | LPN to mapping table to PPA | zone/write-pointer based |
| Zone size | — | 16 MiB |
| Zones | — | 512 |
| NAND | 8 channels x 8 LUNs, 16 MiB blocks | same |
| Die per zone | — | 1 |
| Write buffer | 8 MiB global | shared global buffer |

Both namespaces use the same NAND timing and geometry. This is deliberate: it
isolates the FTL-vs-ZNS difference from differences between two unrelated SSD
models. With `DIES_PER_ZONE=1`, zone `z` maps to channel `z % 8`, which makes the
zone/channel relationship visible in the statistics.

The conventional namespace has 7% over-provisioning (`OP_AREA_PERCENT=0.07`), so
its physical 8 GiB becomes about 7.47 GiB logical.

### 2.3 Verified Identify information

The full output is saved in `results/identify.txt`. The relevant fields are:

| Field | Value |
|---|---|
| Controller `nn` | 2 |
| `nvme0n1` NSZE | `0xef4099` (about 7.47 GiB) |
| `nvme0n2` NSZE | `0x1000000` (8 GiB) |
| ZNS `zoc` | 0 |
| ZNS `ozcs` | 0 |
| ZNS max active resources `mar` | `0x1ff` (511, 0-based) |
| ZNS max open resources `mor` | `0x1ff` (511, 0-based) |
| ZNS zone size | 16 MiB |
| ZNS zone count | 512 |
| ZNS zone type | sequential write required |

`nvme zns report-zones` reports 512 zones with `Cap: 0x8000` (16 MiB) and
`Type: 0x2`. `blkzone report` reports the same geometry and
`zcond: em` (empty) on a freshly formatted device.

---

## 3. Conventional FTL vs. ZNS in NVMeVirt

### 3.1 Command handling

| Topic | Conventional FTL | ZNS |
|---|---|---|
| Namespace init | `conv_init_namespace()` | `zns_init_namespace()` |
| I/O dispatch | `conv_proc_nvme_io_cmd()` | `zns_proc_nvme_io_cmd()` |
| Data commands | NVM read/write | NVM read/write, zone append |
| Extra commands | — | zone management send/receive, ZNS Identify, report zones |
| Identify CSI | `NVME_CSI_NVM` | `NVME_CSI_ZNS` |
| Completion | `conv_write()` / `conv_read()` | `zns_write()` / `zns_read()` |

ZNS adds the Zoned Namespace command set: zone append, zone open/close/finish/
reset/offline, report zones, ZNS Identify namespace/controller, and the ZNS
namespace descriptor. The conventional namespace only exposes the NVM command
set; zone management is not visible to the host.

### 3.2 Data placement

The conventional FTL hides placement completely:

- Logical pages are mapped through a page-level mapping table.
- The namespace is split into `SSD_PARTITIONS=4` FTL partitions.
- Pages are striped across partitions, channels, LUNs, and planes.
- The FTL selects physical pages and performs garbage collection internally.
- The host can issue random writes anywhere in the logical address space.

ZNS exposes placement:

- The host writes sequential LBAs within a zone.
- `__lpn_to_ppa()` maps a zone and offset to a die/channel.
- Each zone has an explicit write pointer and state (`EMPTY`, `IMPLICIT_OPEN`,
  `EXPLICIT_OPEN`, `CLOSED`, `FULL`, `READ_ONLY`, `OFFLINE`).
- The host must reset a zone before rewriting it.
- There is no FTL mapping table or FTL garbage collection.

In this lab configuration, `DIES_PER_ZONE=1`, so a zone is served by one die and
therefore one channel. Concurrent writes to different zones can use different
channels.

### 3.3 Zone management vs. hidden FTL

| Aspect | Conventional FTL | ZNS |
|---|---|---|
| Who manages erase/GC | FTL, hidden from host | host/file system via zone reset |
| Write ordering | arbitrary | sequential within a zone |
| Address translation | mapping table | zone + write pointer |
| Over-provisioning | FTL-managed OP area | host-visible zone capacity/layout |
| Parallelism source | FTL striping | number of concurrently active zones |
| Failure of random write | supported | not allowed |

The conventional path therefore gives the host a simple block device but hides
parallelism from the file system: the FTL decides how to spread I/O. The ZNS
path gives the file system control over placement, but the file system must
actually open and write multiple zones concurrently to exploit internal
parallelism.

### 3.4 Timing model

Both namespaces use the same `ssd.c` NAND/channel timing model. The only
namespace-specific differences are placement and command handling. The
conventional FTL uses a global write buffer and FTL GC; the ZNS namespace uses
the shared write buffer (`ZONE_WB_SIZE=0`) and zone descriptors. This makes the
comparison primarily architectural rather than a comparison of two different
device models.

---

## 4. Parallelism instrumentation

### 4.1 Metrics

A new `/proc/nvmev/parallel` interface was added. It reports:

- **ZNS host concurrency**: total host I/O count and bytes, current/maximum
  active zones, current/maximum active channels.
- **Per-zone counters**: I/O count and bytes for every zone that received I/O.
- **Per-channel counters**: host bytes, NAND operations, NAND bytes, NAND
  channel busy time, and outstanding host I/O count.
- **Conventional counters**: per-partition, per-channel NAND operations, bytes,
  and busy time.

A zone is counted as active from the moment its command is accepted until its
completion is posted. This is the host-visible concurrency. Channel busy time
measures NAND channel transfer time, not NAND program time, and is therefore
reported together with per-channel bytes.

A periodic `dmesg` line is emitted every `stats_interval_ms` (default 1000 ms):

```text
NVMEVirt: ZNS parallel: az cur=0 avg=2 max=9 | ac cur=0 avg=1 max=8 ...
```

### 4.2 Implementation

| File | Role |
|---|---|
| `nvmev.h` | `complete_io_cmd` callback, `proc_parallel` |
| `main.c` | fair capacity split, proc interface, periodic tick |
| `io.c` | completion callback hook |
| `zns_ftl.h/.c` | ZNS stats, zone-to-channel mapping, dump/reset |
| `zns_read_write.c` | start accounting on successful data commands |
| `ssd.h/.c` | atomic per-channel NAND counters |
| `conv_ftl.h/.c` | conventional channel statistics |

The completion hook is important: zone concurrency must decrease when the host
sees the command complete, not when the NAND operation is scheduled. This is why
the counter is updated in `io.c` at completion time.

---

## 5. Benchmark results

The benchmark matrix was run by `scripts/run_experiments.sh`; raw data and
snapshots are under `results/`. The generated table is `results/summary.md`.

### 5.1 Raw namespaces

| Workload | 1 job | 8 jobs | 32 jobs |
|---|---:|---:|---:|
| Raw FTL sequential write (MiB/s) | 1251.8 | 1085.6 | — |
| Raw FTL 4 KiB random write (MiB/s) | 569.5 (145,798 IOPS) | — | — |
| Raw ZNS sequential write (MiB/s) | 62.0 | 151.1 | 449.1 |

The FTL can absorb random 4 KiB writes and already uses all eight channels with
one job. The ZNS namespace cannot accept random writes; a single sequential
stream is confined to one zone and one channel, so throughput starts at about
62 MiB/s and scales with the number of concurrently written zones.

### 5.2 ZoneFS vs. F2FS on ZNS

| Workload | 1 job | 8 jobs | 32 jobs |
|---|---:|---:|---:|
| ZoneFS sequential write (MiB/s) | 62.0 | 234.4 | 543.5 |
| F2FS on ZNS sequential write (MiB/s) | 64.8 | 60.2 | 61.1 |
| F2FS on FTL sequential write (MiB/s) | 2133.3 | 638.4 | 607.0 |

![Throughput vs. concurrency](/results/throughput_vs_jobs.png)

ZoneFS exposes one file per zone and each fio job writes its own zone file.
Increasing the job count therefore increases the number of active zones and
channels:

| Workload | max active zones (1/8/32) | max active channels (1/8/32) |
|---|---:|---:|
| Raw ZNS | 2 / 9 / 23 | 2 / 8 / 8 |
| ZoneFS | 2 / 7 / 14 | 2 / 7 / 8 |
| F2FS on ZNS | 2 / 2 / 2 | 2 / 2 / 2 |

![Active zones](/results/active_zones.png)

F2FS does not scale on ZNS. Even with 32 concurrent fio jobs, the maximum number
of host-visible active zones/channels stays at 2. F2FS is a log-structured file
system: it writes through its active logs and allocates sections/zones in a
mostly sequential manner. The file system therefore serialises the actual zone
writes, even though multiple user threads are issuing I/O. The time-averaged
per-channel byte distribution is still fairly even because F2FS rotates across
zones/channels over time; the lack of *concurrent* channel use is the key
difference.

### 5.3 Channel utilisation

![Channel share at 8 jobs](/results/channel_share_j8.png)

At 8 jobs:

| Workload | channel share CV | interpretation |
|---|---:|---|
| Raw FTL (1 job) | 0.0012 | FTL already stripes a single stream over all channels |
| ZoneFS (8 jobs) | 0.0001 | eight jobs map to eight different channels |
| F2FS on ZNS (8 jobs) | 0.0415 | balanced over time, but only 2 channels active at once |
| F2FS on FTL (8 jobs) | 0.1341 | all channels used, with FTL GC imbalance |

The low CV of ZoneFS comes from one job per zone and `zone z -> channel z % 8`.
The low CV of F2FS on ZNS comes from time-averaged rotation, not concurrency;
the max-active-channel counter is the decisive metric.

---

## 6. Discussion

### 6.1 FTL hides parallelism; ZNS exposes it

A conventional FTL is an address-translation layer. The host sees a flat block
device and the FTL decides which physical pages, channels, and dies are used.
Consequently, one sequential stream can already occupy all eight channels; the
host and file system do not need to know anything about the internal layout.
The cost is that the host cannot control placement, and the FTL must maintain
mapping tables and GC.

ZNS removes the FTL mapping table and exposes zones. The host must write
sequentially within a zone, but different zones can be written concurrently. In
this model, a single zone is served by one die/channel, so a single stream
cannot use the full device. The file system/application must open multiple
zones to exploit the internal parallelism. This is exactly what ZoneFS makes
easy: one file per zone and one writer per file.

### 6.2 ZoneFS vs. F2FS

ZoneFS is a thin mapping from zones to files. It does not hide the sequential
write constraint, and it does not impose its own allocation policy. The
application chooses how many zones to write concurrently. With 8 and 32
concurrent writers, ZoneFS reaches 7 and 8 active channels and scales to
234/544 MiB/s.

F2FS is a full POSIX file system with a log-structured allocator. It must
maintain metadata, checkpoints, NAT/SIT, and GC. Its active-log design
concentrates writes into a limited number of sections/zones at a time. In this
simulation, 32 concurrent fio jobs still result in at most 2 active zones and 2
active channels, and throughput stays around 60 MiB/s. F2FS is not "broken"; it
is optimising for file-system-level sequentiality and metadata consistency rather
than exposing device-level zone parallelism.

### 6.3 F2FS on FTL baseline

F2FS on the conventional namespace reaches about 0.6–2.1 GiB/s and always uses
all channels. The 1-job result is especially high because the FTL write buffer
and early completion decouple host completion from NAND programming; at 8/32
jobs, write buffer contention and FTL GC reduce the observed throughput. This
baseline confirms that the FTL, not the file system, is providing the channel
parallelism.

### 6.4 Why the raw FTL number differs from F2FS-on-FTL

Raw FTL 1-job sequential write was about 1.25 GiB/s, while F2FS-on-FTL 1-job was
about 2.13 GiB/s. The difference comes from request size/queueing and from the
FTL write buffer/early-completion behaviour; F2FS issues large sequential
requests and the host can complete them before all NAND programming finishes.
The absolute numbers should therefore be compared within a workload family, not
across raw and file-system runs.

---

## 7. Limitations

1. **Early completion.** `WRITE_EARLY_COMPLETION=1` means host-visible throughput
   is not identical to NAND program throughput. The parallelism counters measure
   host-visible zone/channel concurrency and channel transfer time, not full NAND
   program occupancy.
2. **One die per zone.** This was chosen to make the zone-to-channel mapping
   visible. Real ZNS devices often spread a zone over many dies; the absolute
   scaling would differ.
3. **Independent namespace models.** Each namespace has its own simulated
   channel/PCIe model, so the experiment does not model cross-namespace
   contention on a shared controller.
4. **F2FS metadata on a loop device.** The loop device isolates metadata I/O from
   NVMeVirt, but it is not a real NVMe conventional namespace. Using
   `/dev/nvme0n1` as the F2FS metadata device is possible and would include
   metadata I/O in the statistics.
5. **Single run per case.** The results are representative but not a statistical
   distribution; host scheduling and FTL GC cause some variance, especially for
   F2FS-on-FTL.

---

## 8. Conclusion

NVMeVirt can expose a conventional FTL namespace and a ZNS namespace from one
virtual controller. The conventional FTL hides address translation and
parallelism: it uses all channels automatically, supports random writes, and
pays for mapping/GC internally. ZNS removes the FTL and exposes zones/write
pointers; parallelism then depends on how many zones the host writes
concurrently.

The instrumentation added in this lab shows this directly. Raw ZNS and ZoneFS
scale from one active channel to all eight as the number of concurrent zone
writers increases. F2FS on ZNS stays at roughly 60 MiB/s and at most two active
zones/channels, because its log-structured allocator serialises zone writes.
F2FS on the conventional FTL is fast and channel-balanced, confirming that the
FTL is the component supplying parallelism there.

---

## 9. Deliverable mapping

| Weight | Deliverable | Evidence |
|---:|---|---|
| 30% | Two namespaces + Identify | `results/identify.txt`, `lsblk`, `nvme id-ctrl/id-ns`, `nvme zns id-ns`, `report-zones` |
| 30% | Benchmarks on both namespaces | `results/*.json`, `results/summary.md` |
| 20% | Code-path comparison | Sections 3 and 6, plus the modified source |
| 10% | Parallelism logging | `/proc/nvmev/parallel`, `results/*.stats`, `results/*.dmesg` |
| 10% | ZoneFS vs. F2FS comparison | Sections 5.2–5.3 and the three figures |

---

## 10. Appendix — key commands

```bash
# Build and load
make clean && make -j$(nproc)
sudo insmod ./nvmev.ko memmap_start=6G memmap_size=16385M cpus=2,3,4,5

# Identify
sudo nvme list
sudo nvme id-ctrl /dev/nvme0
sudo nvme id-ns /dev/nvme0n1
sudo nvme id-ns /dev/nvme0n2
sudo nvme zns id-ns /dev/nvme0n2
sudo nvme zns report-zones /dev/nvme0n2
sudo blkzone report /dev/nvme0n2

# Parallelism snapshot
cat /proc/nvmev/parallel
echo reset | sudo tee /proc/nvmev/parallel

# ZoneFS
sudo mkzonefs -f /dev/nvme0n2
sudo mount -t zonefs /dev/nvme0n2 /mnt/zonefs

# F2FS on ZNS
sudo truncate -s 2G /var/tmp/f2fs-meta.img
LOOP=$(sudo losetup -f --show /var/tmp/f2fs-meta.img)
sudo mkfs.f2fs -f -m -c /dev/nvme0n2 "$LOOP"
sudo mount -t f2fs "$LOOP" /mnt/f2fs

# Run the matrix
sudo bash scripts/run_experiments.sh
python3 scripts/summarize.py
python3 scripts/plot_results.py
```
