# Lab 1: NVMeVirt with ZNS vs. Traditional FTL (2026/10/22 )

## Goal

- Understand how NVMeVirt supports both traditional FTL and Zoned Namespace (ZNS).
- Compare their implementation differences at the simulator level.
- Extend NVMeVirt to monitor internal parallelism of ZNS devices.
- Demonstrate how file systems (F2FS vs. ZoneFS) interact with ZNS parallelism.

## Utilities / Tools They Will Use

- nvme-cli: identify namespaces, query ZNS features.
- fio (with ZNS/ZBD support): run controlled workloads.
- zonefs and f2fs: ZNS-aware filesystems.
- NVMeVirt source code: modified to expose logs/statistics.
- Basic Linux utilities: mkfs, mount, dmesg, etc.

## In this Lab

1. Set up NVMeVirt
  - Create and run two namespaces: one traditional (FTL) and one ZNS.
  - Verify both namespaces from the host OS.
2. Explore Implementation
  - Read NVMeVirt source to locate differences in handling conventional vs. ZNS namespaces.
  - Summarize:
    - API/command differences.
    - Data placement policies.
    - Zone management vs. hidden FTL.
3. Modify NVMeVirt
  - Instrument NVMeVirt to log/measure ZNS device parallelism (e.g., channels, zones accessed concurrently).
  - Run workloads on both ZoneFS and F2FS.

## Demonstration Deliverables

- **Show**: two namespaces running (conventional vs. ZNS) and their Identify info. (30%)
- **Show**: benchmark runs on both namespaces. (30%)
- **Present**: short write-up comparing NVMeVirt code paths. (20%)
- **Show**: modified NVMeVirt printing/logging parallelism stats. (10%)
- **Compare**: differences in parallelism utilization between ZoneFS and F2FS workloads (graphs/tables/logs). (10%)

## Hints:

If you encounter any issues, please refer to: <https://hackmd.io/@johnnychang924/NVM-Lab1>.
