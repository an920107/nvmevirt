#!/usr/bin/env python3
"""Generate the figures used by report.md."""
import csv
import os
import re

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "results")


def read_summary():
    with open(os.path.join(RES, "summary.csv")) as f:
        return list(csv.DictReader(f))


def read_channels(name, kind):
    path = os.path.join(RES, name + ".stats")
    vals = [0] * 8
    if not os.path.exists(path):
        return vals
    for line in open(path):
        if kind == "zns" and line.startswith("zns_chan:"):
            m = re.search(r"ch=(\d+).*host_bytes=(\d+)", line)
            if m:
                vals[int(m.group(1))] = int(m.group(2))
        elif kind == "conv" and line.startswith("conv_chan:"):
            m = re.search(r"gch=(\d+) nand_ops=\d+ nand_bytes=(\d+)", line)
            if m:
                vals[int(m.group(1))] = int(m.group(2))
    total = sum(vals)
    return [v / total if total else 0 for v in vals]


def main():
    rows = read_summary()
    by_case = {r["case"]: r for r in rows}

    # 1. Throughput scaling for ZNS workloads and the FTL baselines.
    fig, ax = plt.subplots(figsize=(7, 4))
    series = [
        ("raw ZNS", ["zns-seqw-j1", "zns-seqw-j8", "zns-seqw-j32"]),
        ("ZoneFS on ZNS", ["zonefs-w-j1", "zonefs-w-j8", "zonefs-w-j32"]),
        ("F2FS on ZNS", ["f2fs-zns-w-j1", "f2fs-zns-w-j8", "f2fs-zns-w-j32"]),
        ("F2FS on FTL", ["f2fs-ftl-w-j1", "f2fs-ftl-w-j8", "f2fs-ftl-w-j32"]),
    ]
    jobs = [1, 8, 32]
    for label, cases in series:
        ys = [float(by_case[c]["bw_MiB_s"]) for c in cases]
        ax.plot(jobs, ys, marker="o", label=label)
    ax.set_xscale("log", base=2)
    ax.set_xticks(jobs)
    ax.set_xticklabels([str(j) for j in jobs])
    ax.set_xlabel("fio numjobs")
    ax.set_ylabel("Throughput (MiB/s)")
    ax.set_title("Sequential write throughput vs. concurrency")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(RES, "throughput_vs_jobs.png"), dpi=160)
    plt.close(fig)

    # 2. Channel byte distribution at 8 jobs.
    fig, ax = plt.subplots(figsize=(7, 4))
    width = 0.27
    xs = list(range(8))
    data = [
        ("ZoneFS", read_channels("zonefs-w-j8", "zns")),
        ("F2FS on ZNS", read_channels("f2fs-zns-w-j8", "zns")),
        ("F2FS on FTL", read_channels("f2fs-ftl-w-j8", "conv")),
    ]
    for i, (label, vals) in enumerate(data):
        ax.bar([x + (i - 1) * width for x in xs], vals, width=width, label=label)
    ax.set_xticks(xs)
    ax.set_xticklabels([f"ch{x}" for x in xs])
    ax.set_ylabel("Share of channel bytes")
    ax.set_title("Channel utilisation at 8 fio jobs")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(RES, "channel_share_j8.png"), dpi=160)
    plt.close(fig)

    # 3. Active zones / channels.
    fig, ax = plt.subplots(figsize=(7, 4))
    labels = ["raw ZNS", "ZoneFS", "F2FS on ZNS"]
    cases = [
        ["zns-seqw-j1", "zns-seqw-j8", "zns-seqw-j32"],
        ["zonefs-w-j1", "zonefs-w-j8", "zonefs-w-j32"],
        ["f2fs-zns-w-j1", "f2fs-zns-w-j8", "f2fs-zns-w-j32"],
    ]
    width = 0.25
    for i, (label, cs) in enumerate(zip(labels, cases)):
        ys = [int(by_case[c]["max_active_zones"]) for c in cs]
        ax.bar([x + (i - 1) * width for x in xs[:3]], ys, width=width, label=label + " (zones)")
    ax.set_xticks(xs[:3])
    ax.set_xticklabels(["1 job", "8 jobs", "32 jobs"])
    ax.set_ylabel("Max concurrently active zones")
    ax.set_title("Host-visible zone concurrency")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(RES, "active_zones.png"), dpi=160)
    plt.close(fig)

    print("wrote figures to", RES)


if __name__ == "__main__":
    main()
