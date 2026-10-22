# NVM-Lab1 Notes

- NVMeVirt is not compatible with kernels newer than 6.15. Avoid using it on Arch or other distros with newer kernels.
- Reserved memory must be 1 MB larger than the requested size (e.g., 16 GB → 16385 MB).
- In Zoned mode, F2FS requires a conventional block device for its superblock. You can create a file on the host and use it as a fake block device.
- If a ZNS device has fewer than 10 zones, F2FS will fail with “Device size is not sufficient for F2FS volume.” Reduce the zone size to increase the zone count.
- Versions after commit c93e88c may be broken.

