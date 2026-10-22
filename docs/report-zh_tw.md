# Lab 1 報告 — NVMeVirt：ZNS 與傳統 FTL 比較

**課程：** NVM Lab 1  
**日期：** 2026-10-22  
**環境：** Debian 13、Linux 6.12.113、QEMU/KVM、NVMeVirt commit `c93e88c`  
**程式碼倉庫：** <https://github.com/an920107/nvmevirt/tree/lab1>

---

## 1. 實驗目標

本實驗研究 NVMeVirt 如何實作 conventional FTL namespace 與 Zoned Namespace（ZNS），以及這兩種設計如何影響主機可見的平行度。交付項目如下：

1. 在同一個虛擬 NVMe controller 上執行兩個 namespace，並提供 Identify 輸出。
2. 對兩個 namespace 分別進行 benchmark。
3. 比較 conventional FTL 與 ZNS 的程式碼路徑。
4. 修改 NVMeVirt，使其輸出 ZNS 平行度統計。
5. 比較 ZoneFS 與 F2FS 在 ZNS namespace 上的平行度。

---

## 2. 實驗設定

### 2.1 平台

| 項目 | 內容 |
|---|---|
| VM | QEMU/KVM，24 vCPUs，20 GiB RAM |
| OS | Debian 13（trixie） |
| Kernel | `6.12.113+deb13-amd64` |
| 保留記憶體 | `memmap=16385M$6G` |
| OS 可見記憶體 | 約 4 GiB（16 GiB 保留給 NVMeVirt） |
| NVMeVirt | commit `c93e88c` |

實驗使用的 kernel command line：

```text
memmap=16385M$6G isolcpus=2,3,4,5 iommu=off intremap=off quiet
```

`isolcpus` 將 NVMeVirt 的 dispatcher 與 I/O worker 使用的 CPU 隔離出來。`iommu=off` 與 `intremap=off` 是必要的，因為 NVMeVirt 的資料路徑假設 PRP 值是實體位址；若 IOMMU 啟用，nvme driver 會提供 IOVA，而不是實體位址。

VM 韌體的 Secure Boot 已關閉。`scripts/` 內仍保留本機 MOK key 與 `sign-file` 輔助腳本，供必須維持 Secure Boot 的環境使用。

### 2.2 雙 namespace 設定

原始 NVMeVirt 程式碼已經有兩個 namespace 的骨架（`NR_NAMESPACES`、`NS_SSD_TYPE_0/1`、`NS_CAPACITY_0/1`），但所有既有的 `BASE_SSD` model 都設定 `NR_NAMESPACES=1`。因此本實驗在 `ssd_config.h` 新增了 lab 專用的 `LAB_MIXED` target，並在 `Kbuild` 中選用。

| 屬性 | Namespace 1 | Namespace 2 |
|---|---|---|
| 類型 | Conventional FTL | ZNS |
| 裝置 | `/dev/nvme0n1` | `/dev/nvme0n2` |
| 容量 | 7.47 GiB logical（8 GiB physical） | 8 GiB |
| FTL | `conv_ftl.c`（4 partitions） | 無 |
| 定址方式 | LPN → mapping table → PPA | 以 zone／write pointer 為基礎 |
| Zone 大小 | — | 16 MiB |
| Zone 數量 | — | 512 |
| NAND | 8 channels × 8 LUNs，16 MiB blocks | 相同 |
| 每個 zone 的 die 數 | — | 1 |
| Write buffer | 8 MiB global | 共用 global buffer |

兩個 namespace 刻意使用相同的 NAND timing 與 geometry。這樣可以將「FTL vs. ZNS」的差異，與「兩個完全不同 SSD model」的差異隔離。設定 `DIES_PER_ZONE=1` 時，zone `z` 會對應到 channel `z % 8`，使 zone 與 channel 的關係可以直接從統計中觀察。

Conventional namespace 有 7% over-provisioning（`OP_AREA_PERCENT=0.07`），所以 8 GiB physical 會變成約 7.47 GiB logical。

### 2.3 已驗證的 Identify 資訊

完整輸出保存在 `results/identify.txt`。相關欄位如下：

| 欄位 | 值 |
|---|---|
| Controller `nn` | 2 |
| `nvme0n1` NSZE | `0xef4099`（約 7.47 GiB） |
| `nvme0n2` NSZE | `0x1000000`（8 GiB） |
| ZNS `zoc` | 0 |
| ZNS `ozcs` | 0 |
| ZNS max active resources `mar` | `0x1ff`（511，0-based） |
| ZNS max open resources `mor` | `0x1ff`（511，0-based） |
| ZNS zone size | 16 MiB |
| ZNS zone 數量 | 512 |
| ZNS zone type | sequential write required |

`nvme zns report-zones` 回報 512 個 zones，`Cap: 0x8000`（16 MiB），`Type: 0x2`。`blkzone report` 回報相同 geometry，且剛格式化完成的裝置為 `zcond: em`（empty）。

---

## 3. NVMeVirt 中的 Conventional FTL 與 ZNS

### 3.1 命令處理

| 主題 | Conventional FTL | ZNS |
|---|---|---|
| Namespace 初始化 | `conv_init_namespace()` | `zns_init_namespace()` |
| I/O dispatch | `conv_proc_nvme_io_cmd()` | `zns_proc_nvme_io_cmd()` |
| 資料命令 | NVM read/write | NVM read/write、zone append |
| 額外命令 | — | zone management send/receive、ZNS Identify、report zones |
| Identify CSI | `NVME_CSI_NVM` | `NVME_CSI_ZNS` |
| 完成處理 | `conv_write()` / `conv_read()` | `zns_write()` / `zns_read()` |

ZNS 增加了 Zoned Namespace command set：zone append、zone open/close/finish/reset/offline、report zones、ZNS Identify namespace/controller，以及 ZNS namespace descriptor。Conventional namespace 只暴露 NVM command set；主機看不到 zone management。

### 3.2 資料放置

Conventional FTL 完全隱藏資料放置：

- Logical pages 經由 page-level mapping table 對應到實體位置。
- Namespace 被切成 `SSD_PARTITIONS=4` 個 FTL partitions。
- Pages 會 stripe 到 partitions、channels、LUNs 與 planes。
- FTL 自行選擇實體 pages，並在內部執行 garbage collection。
- 主機可以在 logical address space 中任意位置發出 random write。

ZNS 則暴露資料放置：

- 主機必須在 zone 內循序寫入 LBAs。
- `__lpn_to_ppa()` 將 zone 與 offset 對應到 die/channel。
- 每個 zone 有明確的 write pointer 與狀態（`EMPTY`、`IMPLICIT_OPEN`、`EXPLICIT_OPEN`、`CLOSED`、`FULL`、`READ_ONLY`、`OFFLINE`）。
- 主機在重寫 zone 前必須先 reset。
- 沒有 FTL mapping table，也沒有 FTL garbage collection。

在本 lab 設定中，`DIES_PER_ZONE=1`，所以一個 zone 由一個 die 服務，也就是只使用一個 channel。對不同 zone 的並行寫入可以使用不同的 channels。

### 3.3 Zone management 與 hidden FTL 的差異

| 面向 | Conventional FTL | ZNS |
|---|---|---|
| 誰管理 erase/GC | FTL，主機看不到 | 主機／檔案系統透過 zone reset |
| 寫入順序 | 任意 | zone 內必須循序 |
| 位址轉換 | mapping table | zone + write pointer |
| Over-provisioning | FTL 管理的 OP area | 主機可見的 zone capacity／layout |
| 平行度來源 | FTL striping | 同時 active 的 zone 數量 |
| Random write | 支援 | 不允許 |

因此 conventional 路徑提供主機一個簡單的 block device，但對檔案系統隱藏了平行度：由 FTL 決定如何分散 I/O。ZNS 則把放置控制權交給檔案系統，但檔案系統必須真的同時開啟並寫入多個 zones，才能利用內部平行度。

### 3.4 Timing model

兩個 namespace 都使用相同的 `ssd.c` NAND/channel timing model。Namespace 之間唯一的差異是資料放置與命令處理。Conventional FTL 使用 global write buffer 與 FTL GC；ZNS namespace 使用共用的 write buffer（`ZONE_WB_SIZE=0`）與 zone descriptors。這使得本實驗主要是在比較架構，而不是比較兩個不同的裝置 model。

---

## 4. 平行度 instrumentation

### 4.1 量測指標

新增了 `/proc/nvmev/parallel` 介面，會回報：

- **ZNS 主機端並行度**：host I/O 總數與 bytes、目前／最大 active zones、目前／最大 active channels。
- **Per-zone counters**：每個收到 I/O 的 zone 的 I/O 次數與 bytes。
- **Per-channel counters**：host bytes、NAND operations、NAND bytes、NAND channel busy time，以及 outstanding host I/O 數量。
- **Conventional counters**：每個 partition、每個 channel 的 NAND operations、bytes 與 busy time。

一個 zone 從命令被接受開始，到 completion 被 post 為止，都算 active。這是主機可見的並行度。Channel busy time 量測的是 NAND channel transfer time，不是 NAND program time，因此會與 per-channel bytes 一起回報。

每隔 `stats_interval_ms`（預設 1000 ms）會輸出一行週期性 `dmesg`：

```text
NVMEVirt: ZNS parallel: az cur=0 avg=2 max=9 | ac cur=0 avg=1 max=8 ...
```

### 4.2 實作

| 檔案 | 角色 |
|---|---|
| `nvmev.h` | `complete_io_cmd` callback、`proc_parallel` |
| `main.c` | 平均分配容量、proc 介面、週期性 tick |
| `io.c` | completion callback hook |
| `zns_ftl.h/.c` | ZNS 統計、zone-to-channel mapping、dump/reset |
| `zns_read_write.c` | 成功的資料命令開始計數 |
| `ssd.h/.c` | atomic per-channel NAND counters |
| `conv_ftl.h/.c` | conventional channel 統計 |

Completion hook 很重要：zone 並行度必須在主機看到命令完成時下降，而不是在 NAND operation 被排程時下降。這就是為什麼 counter 是在 `io.c` 的 completion 時更新。

---

## 5. Benchmark 結果

Benchmark matrix 由 `scripts/run_experiments.sh` 執行；原始資料與 snapshot 保存在 `results/`。產生的表格是 `results/summary.md`。

### 5.1 Raw namespaces

| Workload | 1 job | 8 jobs | 32 jobs |
|---|---:|---:|---:|
| Raw FTL sequential write（MiB/s） | 1251.8 | 1085.6 | — |
| Raw FTL 4 KiB random write（MiB/s） | 569.5（145,798 IOPS） | — | — |
| Raw ZNS sequential write（MiB/s） | 62.0 | 151.1 | 449.1 |

FTL 可以處理 random 4 KiB writes，而且只要一個 job 就已經使用全部八個 channels。ZNS namespace 不接受 random writes；單一 sequential stream 會被限制在一個 zone、一個 channel，所以 throughput 從約 62 MiB/s 開始，並隨著同時寫入的 zones 數量增加而擴展。

### 5.2 ZNS 上的 ZoneFS 與 F2FS

| Workload | 1 job | 8 jobs | 32 jobs |
|---|---:|---:|---:|
| ZoneFS sequential write（MiB/s） | 62.0 | 234.4 | 543.5 |
| F2FS on ZNS sequential write（MiB/s） | 64.8 | 60.2 | 61.1 |
| F2FS on FTL sequential write（MiB/s） | 2133.3 | 638.4 | 607.0 |

![Throughput vs. concurrency](/results/throughput_vs_jobs.png)

ZoneFS 將每個 zone 暴露成一個檔案，每個 fio job 寫入自己的 zone file。因此增加 job 數量會增加 active zones 與 channels 的數量：

| Workload | max active zones（1/8/32） | max active channels（1/8/32） |
|---|---:|---:|
| Raw ZNS | 2 / 9 / 23 | 2 / 8 / 8 |
| ZoneFS | 2 / 7 / 14 | 2 / 7 / 8 |
| F2FS on ZNS | 2 / 2 / 2 | 2 / 2 / 2 |

![Active zones](/results/active_zones.png)

F2FS 在 ZNS 上沒有 scaling。即使有 32 個並行 fio jobs，主機可見的 active zones/channels 最大值仍然只有 2。F2FS 是 log-structured 檔案系統：它透過 active logs 寫入，並以大致循序的方式配置 sections/zones。因此即使多個使用者 thread 同時發出 I/O，檔案系統仍然把實際的 zone 寫入序列化。由於 F2FS 會隨時間輪替 zones/channels，時間平均下來的 per-channel byte 分佈仍然相當平均；真正關鍵的差異是缺少「同時」使用多個 channels。

### 5.3 Channel 使用率

![Channel share at 8 jobs](/results/channel_share_j8.png)

在 8 jobs 時：

| Workload | channel share CV | 解讀 |
|---|---:|---|
| Raw FTL（1 job） | 0.0012 | FTL 已將單一 stream stripe 到所有 channels |
| ZoneFS（8 jobs） | 0.0001 | 八個 jobs 對應到八個不同 channels |
| F2FS on ZNS（8 jobs） | 0.0415 | 時間平均下平衡，但一次只有 2 個 channels active |
| F2FS on FTL（8 jobs） | 0.1341 | 使用所有 channels，但有 FTL GC 造成的不平衡 |

ZoneFS 的低 CV 來自「一個 job 對應一個 zone」以及 `zone z -> channel z % 8`。F2FS on ZNS 的低 CV 來自時間平均的輪替，而不是並行度；max-active-channel counter 才是決定性的指標。

---

## 6. 討論

### 6.1 FTL 隱藏平行度；ZNS 暴露平行度

Conventional FTL 是一層 address-translation layer。主機看到平面的 block device，由 FTL 決定使用哪些 physical pages、channels 與 dies。因此單一 sequential stream 就可以佔用全部八個 channels；主機與檔案系統不需要知道內部 layout。代價是主機無法控制放置，而且 FTL 必須維護 mapping tables 與 GC。

ZNS 移除 FTL mapping table 並暴露 zones。主機必須在 zone 內循序寫入，但不同 zones 可以同時寫入。在這個 model 中，單一 zone 由一個 die/channel 服務，所以單一 stream 無法使用整個裝置。檔案系統／應用程式必須開啟多個 zones，才能利用內部平行度。這正是 ZoneFS 容易做到的事：一個 zone 一個檔案，一個檔案一個 writer。

### 6.2 ZoneFS 與 F2FS 的比較

ZoneFS 是 zones 到檔案的薄映射。它不隱藏 sequential write constraint，也不強加自己的配置策略。應用程式可以決定要同時寫入多少 zones。在 8 與 32 個並行 writers 時，ZoneFS 達到 7 與 8 個 active channels，throughput 擴展到 234/544 MiB/s。

F2FS 是完整的 POSIX 檔案系統，使用 log-structured allocator。它必須維護 metadata、checkpoints、NAT/SIT 與 GC。其 active-log 設計會把寫入集中在少數 sections/zones。在這個模擬中，32 個並行 fio jobs 仍然最多只有 2 個 active zones 與 2 個 active channels，throughput 維持在約 60 MiB/s。F2FS 並沒有「壞掉」；它是在最佳化檔案系統層級的循序性與 metadata 一致性，而不是暴露裝置層級的 zone 平行度。

### 6.3 F2FS on FTL baseline

F2FS 在 conventional namespace 上達到約 0.6–2.1 GiB/s，而且總是使用所有 channels。1-job 的結果特別高，因為 FTL write buffer 與 early completion 將主機完成與 NAND programming 解耦；在 8/32 jobs 時，write buffer contention 與 FTL GC 會降低觀察到的 throughput。這個 baseline 證實：提供 channel 平行度的是 FTL，而不是檔案系統。

### 6.4 為什麼 raw FTL 與 F2FS-on-FTL 的數字不同

Raw FTL 1-job sequential write 約為 1.25 GiB/s，而 F2FS-on-FTL 1-job 約為 2.13 GiB/s。差異來自 request size／queueing，以及 FTL write buffer／early-completion 行為；F2FS 發出大型 sequential requests，主機可以在所有 NAND programming 完成前就完成它們。因此絕對數字應該在同一個 workload family 內比較，而不是跨 raw 與檔案系統執行比較。

---

## 7. 限制

1. **Early completion。** `WRITE_EARLY_COMPLETION=1` 表示主機可見的 throughput 不等同於 NAND program throughput。平行度 counters 量測的是主機可見的 zone/channel 並行度與 channel transfer time，而不是完整的 NAND program occupancy。
2. **每個 zone 一個 die。** 這是為了讓 zone-to-channel mapping 可觀察。真實 ZNS 裝置通常會把一個 zone 分散到多個 dies；絕對 scaling 會不同。
3. **獨立的 namespace models。** 每個 namespace 有自己的模擬 channel/PCIe model，所以本實驗沒有模擬 shared controller 上跨 namespace 的 contention。
4. **F2FS metadata 放在 loop device。** Loop device 將 metadata I/O 與 NVMeVirt 隔離，但它不是真正的 NVMe conventional namespace。使用 `/dev/nvme0n1` 作為 F2FS metadata device 是可行的，而且會把 metadata I/O 納入統計。
5. **每個 case 只跑一次。** 結果具有代表性，但不是統計分佈；主機 scheduling 與 FTL GC 會造成一些變異，尤其是 F2FS-on-FTL。

---

## 8. 結論

NVMeVirt 可以在同一個虛擬 controller 上暴露 conventional FTL namespace 與 ZNS namespace。Conventional FTL 隱藏 address translation 與平行度：它自動使用所有 channels、支援 random writes，並在內部負擔 mapping/GC 的成本。ZNS 移除 FTL 並暴露 zones/write pointers；此時平行度取決於主機同時寫入多少 zones。

本 lab 加入的 instrumentation 直接展示了這一點。Raw ZNS 與 ZoneFS 會隨著並行 zone writers 數量增加，從一個 active channel 擴展到全部八個。F2FS on ZNS 維持在約 60 MiB/s，且最多只有兩個 active zones/channels，因為其 log-structured allocator 會把 zone 寫入序列化。F2FS 在 conventional FTL 上則快速且 channel 使用平衡，證實該處提供平行度的是 FTL。

---

## 9. 交付項目對照

| 配分 | 交付項目 | 證據 |
|---:|---|---|
| 30% | 兩個 namespaces + Identify | `results/identify.txt`、`lsblk`、`nvme id-ctrl/id-ns`、`nvme zns id-ns`、`report-zones` |
| 30% | 兩個 namespaces 的 benchmarks | `results/*.json`、`results/summary.md` |
| 20% | Code-path 比較 | 第 3、6 節，以及修改後的原始碼 |
| 10% | 平行度 logging | `/proc/nvmev/parallel`、`results/*.stats`、`results/*.dmesg` |
| 10% | ZoneFS 與 F2FS 比較 | 第 5.2–5.3 節與三張圖 |

---

## 10. 附錄 — 主要命令

```bash
# 編譯與載入
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

# 平行度 snapshot
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

# 執行完整 matrix
sudo bash scripts/run_experiments.sh
python3 scripts/summarize.py
python3 scripts/plot_results.py
```
