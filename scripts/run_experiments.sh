#!/bin/bash
# Run the Lab 1 benchmark matrix.  Must be run as root.
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p results

ZNS=/dev/nvme0n2
FTL=/dev/nvme0n1
ZONE_MNT=/mnt/zonefs
F2FS_MNT=/mnt/f2fs
F2FS_FTL_MNT=/mnt/f2fs-ftl
META_IMG=/var/tmp/f2fs-meta.img

json_bw() {
	python3 - "$1" <<'PY'
import json, sys
f = sys.argv[1]
d = json.load(open(f))
j = d['jobs'][0]
print(f"{f}: bw={j['write']['bw']/1024:.1f} MiB/s iops={j['write']['iops']:.0f} lat_us={j['write']['lat_ns']['mean']/1000:.1f}")
PY
}

run_case() {
	local name=$1
	shift
	echo "=== $name ==="
	echo reset > /proc/nvmev/parallel
	dmesg -c >/dev/null || true
	"$@"
	cat /proc/nvmev/parallel > "results/${name}.stats"
	dmesg > "results/${name}.dmesg" || true
	json_bw "results/${name}.json"
}

# ---------------- raw FTL ----------------
run_case ftl-seqw-j1 fio --name=ftl-seqw-j1 --filename=$FTL --rw=write --bs=128k \
	--direct=1 --ioengine=libaio --iodepth=32 --size=1G --group_reporting \
	--output-format=json --output=results/ftl-seqw-j1.json

run_case ftl-seqw-j8 fio --name=ftl-seqw-j8 --filename=$FTL --rw=write --bs=128k \
	--direct=1 --ioengine=libaio --iodepth=32 --numjobs=8 \
	--offset_increment=512M --size=512M --group_reporting \
	--output-format=json --output=results/ftl-seqw-j8.json

run_case ftl-randw-j1 fio --name=ftl-randw-j1 --filename=$FTL --rw=randwrite --bs=4k \
	--direct=1 --ioengine=libaio --iodepth=32 --size=512M --group_reporting \
	--output-format=json --output=results/ftl-randw-j1.json

# ---------------- raw ZNS ----------------
for jobs in 1 8 32; do
	name=zns-seqw-j${jobs}
	blkzone reset $ZNS
	run_case "$name" fio --name="$name" --filename=$ZNS --rw=write --bs=128k \
		--direct=1 --ioengine=psync --zonemode=zbd --numjobs=$jobs \
		--offset=0 --offset_increment=1z --size=1z --job_max_open_zones=1 \
		--group_reporting --output-format=json --output=results/${name}.json
done

# ---------------- ZoneFS on ZNS ----------------
umount $ZONE_MNT 2>/dev/null || true
mkzonefs -f $ZNS >/dev/null
mkdir -p $ZONE_MNT
mount -t zonefs $ZNS $ZONE_MNT
echo mq-deadline > /sys/block/$(basename $ZNS)/queue/scheduler || true

for jobs in 1 8 32; do
	for i in $(seq 0 $((jobs - 1))); do truncate -s 0 "$ZONE_MNT/seq/$i"; done
	name=zonefs-w-j${jobs}
	run_case "$name" fio --name="$name" --filename_format="$ZONE_MNT/seq/\$jobnum" \
		--rw=write --bs=128k --direct=1 --ioengine=psync --numjobs=$jobs \
		--size=16M --allow_file_create=0 --unlink=0 --file_append=1 \
		--group_reporting --output-format=json --output=results/${name}.json
done
umount $ZONE_MNT

# ---------------- F2FS on ZNS ----------------
truncate -s 2G $META_IMG
LOOP=$(losetup -f --show $META_IMG)
trap 'umount $F2FS_MNT 2>/dev/null || true; losetup -d $LOOP 2>/dev/null || true' EXIT

for jobs in 1 8 32; do
	umount $F2FS_MNT 2>/dev/null || true
	mkfs.f2fs -f -m -c $ZNS "$LOOP" >/dev/null
	mkdir -p $F2FS_MNT
	mount -t f2fs "$LOOP" $F2FS_MNT
	name=f2fs-zns-w-j${jobs}
	size=128M
	[ "$jobs" = 1 ] && size=256M
	[ "$jobs" = 32 ] && size=32M
	run_case "$name" fio --name="$name" --directory=$F2FS_MNT --rw=write --bs=128k \
		--direct=1 --numjobs=$jobs --size=$size --group_reporting \
		--output-format=json --output=results/${name}.json
done
umount $F2FS_MNT 2>/dev/null || true

# ---------------- F2FS on FTL ----------------
for jobs in 1 8 32; do
	umount $F2FS_FTL_MNT 2>/dev/null || true
	mkfs.f2fs -f $FTL >/dev/null
	mkdir -p $F2FS_FTL_MNT
	mount -t f2fs $FTL $F2FS_FTL_MNT
	name=f2fs-ftl-w-j${jobs}
	size=128M
	[ "$jobs" = 1 ] && size=256M
	[ "$jobs" = 32 ] && size=32M
	run_case "$name" fio --name="$name" --directory=$F2FS_FTL_MNT --rw=write --bs=128k \
		--direct=1 --numjobs=$jobs --size=$size --group_reporting \
		--output-format=json --output=results/${name}.json
done
umount $F2FS_FTL_MNT 2>/dev/null || true

echo "all done"
