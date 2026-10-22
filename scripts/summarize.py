#!/usr/bin/env python3
"""Summarize fio JSON + /proc/nvmev/parallel snapshots for Lab 1."""
import csv
import glob
import json
import os
import re
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "results")

CASES = [
    ("ftl-seqw-j1", "raw FTL", "FTL", 1),
    ("ftl-seqw-j8", "raw FTL", "FTL", 8),
    ("ftl-randw-j1", "raw FTL rand", "FTL", 1),
    ("zns-seqw-j1", "raw ZNS", "ZNS", 1),
    ("zns-seqw-j8", "raw ZNS", "ZNS", 8),
    ("zns-seqw-j32", "raw ZNS", "ZNS", 32),
    ("zonefs-w-j1", "ZoneFS", "ZNS", 1),
    ("zonefs-w-j8", "ZoneFS", "ZNS", 8),
    ("zonefs-w-j32", "ZoneFS", "ZNS", 32),
    ("f2fs-zns-w-j1", "F2FS on ZNS", "ZNS", 1),
    ("f2fs-zns-w-j8", "F2FS on ZNS", "ZNS", 8),
    ("f2fs-zns-w-j32", "F2FS on ZNS", "ZNS", 32),
    ("f2fs-ftl-w-j1", "F2FS on FTL", "FTL", 1),
    ("f2fs-ftl-w-j8", "F2FS on FTL", "FTL", 8),
    ("f2fs-ftl-w-j32", "F2FS on FTL", "FTL", 32),
]


def parse_fio(name):
    path = os.path.join(RES, name + ".json")
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return None
    d = json.load(open(path))
    j = d["jobs"][0]
    key = "write" if "write" in j else "read"
    return {
        "bw_mib": j[key]["bw"] / 1024.0,
        "iops": j[key]["iops"],
        "lat_us": j[key]["lat_ns"].get("mean", 0) / 1000.0,
    }


def parse_stats(name, target):
    path = os.path.join(RES, name + ".stats")
    out = {
        "max_az": 0,
        "max_ac": 0,
        "channels": [],
        "total_bytes": 0,
        "cv": 0.0,
        "avg_az": 0.0,
        "avg_ac": 0.0,
    }
    if not os.path.exists(path):
        return out
    for line in open(path):
        if line.startswith("zns:") and target == "ZNS":
            m = re.search(r"total_bytes=(\d+).*active_zones=(\d+) max_active_zones=(\d+) "
                          r"active_channels=(\d+) max_active_channels=(\d+)", line)
            if m:
                out["total_bytes"] = int(m.group(1))
                out["max_az"] = int(m.group(3))
                out["max_ac"] = int(m.group(5))
        elif line.startswith("zns_chan:") and target == "ZNS":
            m = re.search(r"ch=(\d+).*host_bytes=(\d+)", line)
            if m:
                out["channels"].append(int(m.group(2)))
        elif line.startswith("conv_chan:") and target == "FTL":
            m = re.search(r"gch=(\d+) nand_ops=\d+ nand_bytes=(\d+)", line)
            if m:
                out["channels"].append(int(m.group(2)))
    if target == "FTL":
        out["total_bytes"] = sum(out["channels"])
    if out["channels"] and out["total_bytes"]:
        mean = statistics.mean(out["channels"])
        if mean:
            out["cv"] = statistics.pstdev(out["channels"]) / mean
    # dmesg interval averages (only for runs longer than the 1s tick)
    dpath = os.path.join(RES, name + ".dmesg")
    az_avgs, ac_avgs = [], []
    if os.path.exists(dpath):
        for line in open(dpath):
            m = re.search(r"az cur=\d+ avg=(\d+) max=(\d+) \| ac cur=\d+ avg=(\d+) max=(\d+)", line)
            if m:
                az_avgs.append(int(m.group(1)))
                ac_avgs.append(int(m.group(3)))
    if az_avgs:
        out["avg_az"] = statistics.mean(az_avgs)
        out["avg_ac"] = statistics.mean(ac_avgs)
    return out


def main():
    rows = []
    for name, label, target, jobs in CASES:
        fio = parse_fio(name)
        st = parse_stats(name, target)
        if fio is None:
            continue
        rows.append({
            "case": name,
            "workload": label,
            "target": target,
            "jobs": jobs,
            "bw_MiB_s": round(fio["bw_mib"], 1),
            "iops": round(fio["iops"], 0),
            "lat_us": round(fio["lat_us"], 1),
            "total_MiB": round(st["total_bytes"] / (1024 * 1024), 1),
            "max_active_zones": st["max_az"],
            "max_active_channels": st["max_ac"],
            "avg_active_zones": round(st["avg_az"], 2),
            "avg_active_channels": round(st["avg_ac"], 2),
            "channel_cv": round(st["cv"], 4),
        })
    fields = list(rows[0].keys()) if rows else []
    with open(os.path.join(RES, "summary.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)

    with open(os.path.join(RES, "summary.md"), "w") as f:
        f.write("| case | workload | target | jobs | BW (MiB/s) | IOPS | lat (us) | data (MiB) | max AZ | max AC | avg AZ | avg AC | channel CV |\n")
        f.write("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for r in rows:
            f.write("| {case} | {workload} | {target} | {jobs} | {bw_MiB_s} | {iops:.0f} | {lat_us} | {total_MiB} | {max_active_zones} | {max_active_channels} | {avg_active_zones} | {avg_active_channels} | {channel_cv} |\n".format(**r))
    print(open(os.path.join(RES, "summary.md")).read())


if __name__ == "__main__":
    main()
