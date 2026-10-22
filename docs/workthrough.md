# Lab 1 Workthrough — NVMeVirt: Conventional FTL vs. ZNS

This file records the actual steps, failures, fixes, and commands used to complete
Lab 1. The final report is in [`report.md`](report.md).

## 1. Goal and environment

Lab requirements:

- Create and run two namespaces: one conventional FTL and one ZNS.
- Verify both namespaces and their Identify information from Linux.
- Compare NVMeVirt's conventional and ZNS code paths.
- Instrument NVMeVirt to report internal parallelism.
- Run workloads on ZoneFS and F2FS and compare parallelism.

Environment:

| Item | Value |
|---|---|
| VM | QEMU/KVM, 24 vCPUs, 20 GiB RAM |
| OS | Debian 13 (trixie) |
| Kernel | `6.12.113+deb13-amd64` |
| Reserved memory | `memmap=16385M$6G` (16 GiB storage + 1 MiB metadata) |
| OS-visible memory | about 4 GiB, because 16 GiB is reserved for NVMeVirt |
| Repo | `snu-csl/nvmevirt`, commit `c93e88c` |
| Secure Boot | initially enabled, later disabled in the VM firmware |

The machine was already configured with the `memmap` reservation. `free -g`
showing only about 4 GiB is expected and is **not** a problem.

## 2. Secure Boot and kernel command line

The first `insmod` failed with:

```text
insmod: ERROR: could not insert module ./nvmev.ko: Key was rejected by service
```

`mokutil --sb-state` reported `SecureBoot enabled`, and
`/sys/kernel/security/lockdown` was `[integrity]`. Because this is a headless
VM, enrolling a MOK through MokManager was not practical. The VM firmware was
therefore configured with **Secure Boot disabled**, followed by a normal reboot.

A local MOK key was still generated and the module was signed, so the build is
ready for a MOK-based setup if Secure Boot is re-enabled:

```bash
sudo openssl req -new -x509 -newkey rsa:2048 \
  -keyout /root/MOK.priv -outform DER -out /root/MOK.der \
  -nodes -days 36500 -subj "/CN=Lab1 NVMeVirt Module Signing/"
sudo chmod 600 /root/MOK.priv
sudo /usr/src/linux-headers-$(uname -r)/scripts/sign-file \
  sha256 /root/MOK.priv /root/MOK.der ./nvmev.ko
```

The final GRUB command line is:

```text
memmap=16385M$6G isolcpus=2,3,4,5 iommu=off intremap=off quiet
```

`isolcpus` keeps the NVMeVirt dispatcher/worker threads away from the rest of
the system. `iommu=off` and `intremap=off` are required because NVMeVirt assumes
that PRP values are physical addresses; with an active IOMMU the nvme driver
passes IOVAs and NVMeVirt logs `ioremap: invalid physical address`.

## 3. Build baseline

Packages:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) \
  nvme-cli fio f2fs-tools zonefs-tools python3-matplotlib
```

The upstream default (`CONFIG_NVMEVIRT_NVM=y`) built successfully on kernel
6.12. This was done before changing the configuration so that kernel-header or
API problems could be isolated from the mixed-namespace changes.

## 4. Dual-namespace design

Upstream already contains the skeleton for two namespaces
(`NR_NAMESPACES`, `NS_SSD_TYPE_0/1`, `NS_CAPACITY_0/1`), but every existing
`BASE_SSD` model sets `NR_NAMESPACES=1`. A new lab-only build target was added
instead of modifying the existing models.

### `Kbuild`

```make
CONFIG_NVMEVIRT_MIXED := y

ccflags-$(CONFIG_NVMEVIRT_MIXED) += -DBASE_SSD=LAB_MIXED
nvmev-$(CONFIG_NVMEVIRT_MIXED) += ssd.o conv_ftl.o zns_ftl.o \
    zns_read_write.o zns_mgmt_send.o zns_mgmt_recv.o \
    channel_model.o pqueue/pqueue.o
```

`simple_ftl.o` and `kv_ftl.o` are not compiled in this target. The calls to
those initializers in `main.c` are guarded by `#if (BASE_SSD == LAB_MIXED)` so
that no unresolved symbols remain.

### `ssd_config.h`

New model `LAB_MIXED=5`:

| Property | Value |
|---|---|
| Namespaces | 2 |
| NS0 | `SSD_TYPE_CONV` |
| NS1 | `SSD_TYPE_ZNS` |
| Capacity | split evenly (8 GiB each with the current memmap) |
| NAND channels | 8 |
| LUNs per channel | 8 |
| Flash page | 32 KiB |
| Oneshot page | 64 KiB |
| Block size | 16 MiB |
| ZNS zone size | 16 MiB |
| Dies per zone | 1 |
| ZNS zones | 512 |
| Write buffer | 8 MiB global (`ZONE_WB_SIZE=0`) |
| OP area | 7% (`0.07`, not `7`) |

The two namespaces deliberately share the same NAND geometry and timing. This
isolates the FTL-vs-ZNS difference from differences between two unrelated SSD
models.

With `DIES_PER_ZONE=1`, zone `z` maps to one die and therefore to channel
`z % 8`. This makes the channel/zone relationship visible in the statistics.

### Capacity split

`NVMEV_NAMESPACE_INIT()` originally gave all remaining capacity to the first
namespace when `NS_CAPACITY(i)==0`. It now splits the remaining capacity among
the remaining namespaces:

```c
if (NS_CAPACITY(i) == 0)
    size = remaining_capacity / (nr_ns - i);
else
    size = min(NS_CAPACITY(i), remaining_capacity);
```

If `NS_CAPACITY(i)` is zero and several namespaces remain, each gets an equal
share. The array is allocated with `kzalloc`, which also ensures that the new
`complete_io_cmd` callback is NULL for namespaces that do not use it.

## 5. Parallelism instrumentation

### Public interface

A new proc file was added:

```text
/proc/nvmev/parallel
```

Writing `reset` clears the counters. Reading it prints:

- conventional namespace: per-partition, per-channel NAND ops/bytes/busy time;
- ZNS namespace: total host I/Os/bytes, current and maximum active zones and
  channels, per-channel host bytes and NAND counters, and non-zero per-zone
  counters.

Example:

```text
zns: zones=512 channels=8 total_ios=1025 total_bytes=134221824 \
     active_zones=0 max_active_zones=7 active_channels=0 max_active_channels=7
zns_chan: ch=0 host_ios=16777216 host_bytes=16777216 nand_ops=256 \
          nand_bytes=16777216 busy_ns=21495808 out=0
...
zns_zone: zid=0 ios=... bytes=... out=0
```

A periodic line is also emitted to `dmesg`:

```text
NVMEVirt: ZNS parallel: az cur=0 avg=2 max=9 | ac cur=0 avg=1 max=8 \
          ios=... bytes=...
```

`stats_interval_ms` (module parameter, default 1000) controls the interval.

### Implementation points

| File | Change |
|---|---|
| `nvmev.h` | added `complete_io_cmd` callback and `proc_parallel` |
| `main.c` | fair capacity split, `LAB_MIXED` name, proc read/write, periodic tick |
| `io.c` | call `complete_io_cmd` on successful host completion |
| `zns_ftl.h/.c` | per-zone/per-channel stats, zone-to-channel helper, tick/dump/reset |
| `zns_read_write.c` | start accounting after a successful read/write/zone-append |
| `ssd.h/.c` | atomic per-channel `stat_ops`, `stat_bytes`, `stat_busy_ns` |
| `conv_ftl.h/.c` | dump/reset conventional channel statistics |

The zone concurrency counter is host-visible concurrency: a zone is counted as
active from the moment its command is accepted until its completion is posted.
The channel `busy_ns` counter measures NAND channel transfer time, not NAND
program time, so it is used together with per-zone/per-channel byte counts.

## 6. Build and load

```bash
cd ~/nvmevirt
make clean
make -j$(nproc)
sudo insmod ./nvmev.ko \
  memmap_start=6G memmap_size=16385M cpus=2,3,4,5
```

Expected dmesg:

```text
NVMeVirt: Storage: 0x180100000-0x580100000 (16384 MiB)
NVMeVirt: FTL physical space: 8589934592, logical space: 8027976254
NVMeVirt: ns 0/2: size 7656 MiB
NVMeVirt: zone_size=16777216(Byte),16(MB), # zones=512 # die/zone=1
NVMeVirt: ns 1/2: size 8192 MiB
NVMeVirt: Virtual NVMe device created
```

Observed devices:

```text
nvme0n1   7.5G  none          # conventional FTL
nvme0n2   8.0G  host-managed  # ZNS, 512 x 16 MiB zones
```

Identify commands:

```bash
sudo nvme list
sudo nvme id-ctrl /dev/nvme0
sudo nvme id-ns /dev/nvme0n1
sudo nvme id-ns /dev/nvme0n2
sudo nvme zns id-ns /dev/nvme0n2
sudo nvme zns report-zones /dev/nvme0n2 | head
sudo blkzone report /dev/nvme0n2 | head
```

The saved output is in `results/identify.txt`.

## 7. Problems found during bring-up

### 7.1 Secure Boot

As described above. Resolved by disabling Secure Boot in the VM firmware. A MOK
key and signing script are still included for reproducibility.

### 7.2 Conventional namespace was only 1 GiB

First mixed build reported:

```text
FTL physical space: 8589934592, logical space: 1073741824
```

`OP_AREA_PERCENT=7` was interpreted as 700% over-provisioning, because
`conv_init_params()` computes `pba_pcent = (1 + op_area_pcent) * 100`. Changed
to `0.07`, giving `physical/logical * 100 = 107` and a 7.47 GiB logical volume.

### 7.3 `ioremap: invalid physical address`

With the IOMMU active, PRP values are IOVAs and `pfn_valid()` is false. Adding
`iommu=off` (together with `intremap=off`) removed the warnings during normal
data I/O.

### 7.4 Zone-management commands were treated as data commands

`__do_perform_io()` processed every host command, including zone management
receive/send. For zone-management commands, PRP2 is not necessarily a data PRP
list, so NVMeVirt read unrelated memory as PRP entries and produced a burst of
`ioremap: invalid physical address` warnings. A guard now skips the data copy
for opcodes other than read/write/zone-append:

```c
switch (cmd->opcode) {
case nvme_cmd_read:
case nvme_cmd_write:
case nvme_cmd_zone_append:
    break;
default:
    return 0;
}
```

This also fixed a user-space `malloc(): corrupted top size` crash in
`nvme zns report-zones`.

### 7.5 Corrupted `fixdep` after early invalid PRP writes

After several runs with the early module, `make` began failing with
`Segmentation fault` in `fixdep`, even for a trivial dependency file. This was
collateral damage from the invalid PRP writes described above: the kernel build
tool binary had been corrupted in the page cache/disk. The module was unloaded
and the toolchain was repaired with:

```bash
sudo apt-get install --reinstall -y linux-kbuild-6.12.113+deb13
```

After the `__do_perform_io` guard and a clean rebuild, `nvme zns report-zones`
and the benchmark matrix ran without `ioremap: invalid` or `fixdep` failures.

### 7.6 Fortify warning in zone report

`memcpy(report->zd, ...)` triggered a field-spanning warning because `zd` is a
one-element flexible array. Changed to an explicit byte offset:

```c
memcpy((u8 *)report + offsetof(struct zone_report, zd), ...);
```

## 8. File systems

### ZoneFS

```bash
sudo mkzonefs -f /dev/nvme0n2
sudo mkdir -p /mnt/zonefs
sudo mount -t zonefs /dev/nvme0n2 /mnt/zonefs
```

ZoneFS exposed 511 sequential zone files (`seq/0` ... `seq/510`); zone 0 is
reserved for the zonefs superblock. Files must be written with direct I/O and
appended, and must not be unlinked. The fio options used were:

```text
--allow_file_create=0 --unlink=0 --file_append=1 --direct=1 --ioengine=psync
```

### F2FS on ZNS

ZNS has no conventional zones, so F2FS metadata was placed on a loop device:

```bash
sudo truncate -s 2G /var/tmp/f2fs-meta.img
LOOP=$(sudo losetup -f --show /var/tmp/f2fs-meta.img)
sudo mkfs.f2fs -f -m -c /dev/nvme0n2 "$LOOP"
sudo mount -t f2fs "$LOOP" /mnt/f2fs
```

The resulting volume was about 10 GiB (2 GiB metadata + 8 GiB zoned data).
`mkfs.f2fs` reported 512 zones, 0 conventional zones, and formatted successfully.

### F2FS on FTL

```bash
sudo mkfs.f2fs -f /dev/nvme0n1
sudo mount -t f2fs /dev/nvme0n1 /mnt/f2fs-ftl
```

This is the FTL baseline: F2FS sees a normal random-write block device.

## 9. Benchmark methodology

The full matrix is automated by `scripts/run_experiments.sh`:

- Raw FTL: sequential write at 1 and 8 jobs, random 4 KiB write.
- Raw ZNS: sequential write at 1, 8, 32 jobs, one zone per job.
- ZoneFS: one zone file per job at 1, 8, 32 jobs.
- F2FS on ZNS: 1, 8, 32 files/jobs.
- F2FS on FTL: the same matrix as the FTL baseline.

For each case the script saves:

- `results/<case>.json` — fio JSON;
- `results/<case>.stats` — `/proc/nvmev/parallel` snapshot;
- `results/<case>.dmesg` — interval parallelism lines.

Raw ZNS runs reset zones with `blkzone reset`. ZoneFS runs truncate zone files
to zero to reset their write pointers. F2FS runs are re-formatted for each case.

## 10. Results summary

The generated table is in `results/summary.md`; the main numbers are:

| Workload | 1 job | 8 jobs | 32 jobs |
|---|---:|---:|---:|
| Raw FTL seq write (MiB/s) | 1251.8 | 1085.6 | — |
| Raw FTL 4K random write (MiB/s) | 569.5 | — | — |
| Raw ZNS seq write (MiB/s) | 62.0 | 151.1 | 449.1 |
| ZoneFS seq write (MiB/s) | 62.0 | 234.4 | 543.5 |
| F2FS on ZNS seq write (MiB/s) | 64.8 | 60.2 | 61.1 |
| F2FS on FTL seq write (MiB/s) | 2133.3 | 638.4 | 607.0 |

Concurrency:

| Workload | max active zones | max active channels |
|---|---:|---:|
| Raw ZNS 1/8/32 | 2 / 9 / 23 | 2 / 8 / 8 |
| ZoneFS 1/8/32 | 2 / 7 / 14 | 2 / 7 / 8 |
| F2FS on ZNS 1/8/32 | 2 / 2 / 2 | 2 / 2 / 2 |

Key observations:

- A single FTL write already spreads bytes evenly over all eight channels
  (channel CV ≈ 0.001), because the hidden FTL stripes logical pages across
  partitions/channels.
- A single ZNS stream uses one zone and one channel. ZoneFS at 8 and 32 jobs
  reaches 7 and 8 concurrent channels and scales to 234/544 MiB/s.
- F2FS on ZNS stays at about 60 MiB/s regardless of job count and shows at most
  2 active zones/channels. F2FS rotates over zones/channels over time (the
  time-averaged channel shares are still balanced), but it does not expose
  concurrent zone writes to the device.
- F2FS on FTL reaches about 0.6–2.1 GiB/s and always uses all channels; the
  variation is caused by FTL GC/write-buffer behaviour, not by the file system
  hiding parallelism.

## 11. Reproduce

```bash
cd ~/nvmevirt
make clean && make -j$(nproc)
sudo /usr/src/linux-headers-$(uname -r)/scripts/sign-file \
  sha256 /root/MOK.priv /root/MOK.der ./nvmev.ko   # optional

sudo insmod ./nvmev.ko memmap_start=6G memmap_size=16385M cpus=2,3,4,5
sudo bash scripts/run_experiments.sh
python3 scripts/summarize.py
python3 scripts/plot_results.py
```

## 12. Limitations

- `WRITE_EARLY_COMPLETION=1` makes the host-visible throughput differ from the
  NAND program limit. The statistics measure host-visible zone/channel
  concurrency and NAND channel transfer time, not full NAND program latency.
- The ZNS model uses `DIES_PER_ZONE=1` to make the zone-to-channel mapping
  visible; real ZNS devices often spread a zone over many dies.
- The two namespaces have independent simulated channel/PCIe models; the
  experiment does not model cross-namespace contention.
- F2FS metadata is on a loop device, so its metadata I/O is not part of the
  NVMeVirt statistics. This is intentional isolation, but it is a modelling
  limitation.
- The 1-job F2FS-on-FTL result is noticeably higher than the 8/32-job results;
  this is consistent with FTL write buffering/GC, but it means the FTL curve is
  not a clean linear-scaling baseline.
