# nvmetal design

## Decision log

| # | Decision | Why |
|---|----------|-----|
| 1 | Cite the NVM Express Base Specification **revision 1.4c** for every register and command. | 1.4c is the last revision with the NVM command set inside the base document, so one document covers registers, admin and I/O commands. Section titles are given next to numbers so 2.x readers can find them. |
| 2 | C11 + CMake/Ninja, one flag set (`-Wall -Wextra -Werror -O2 -g`) for every build type, sanitizers via `-DNVMETAL_SANITIZE=`. | Sanitizer builds run the same optimised code paths as release builds. |
| 3 | Own ~60-line test harness (`tests/test.h`) instead of a framework. | No third-party code; tests need only libc. |
| 4 | CI pins clang-format 21.1.8 from PyPI. | Ubuntu 24.04 ships clang-format 18, which formats differently from the 21.x used for development. |
| 5 | Guest kernel command line adds `iommu=pt` to the brief's starting point. | With `caching-mode=on`, every DMA map/unmap the kernel nvme driver does per I/O becomes an IOTLB invalidation that QEMU must handle, which would make the kernel baseline pay a cost that bare-metal `iommu=pt` systems do not. `iommu=pt` gives kernel drivers an identity domain; VFIO still gets its own translated domain, so nvmetal runs through the vIOMMU. nvmetal maps its DMA pool once at start-up, so it has no per-I/O IOMMU cost either way. |
| 6 | `nvme.poll_queues=4` instead of 2. | The benchmark matrix runs io_uring polled I/O with up to 4 threads; with 2 poll queues the 4-thread runs would share queues. |
| 7 | Code reaches the guest by rsync over ssh (`scripts/vm/sync.sh`), not virtio-9p. | Builds run on the guest's own disk at native speed with no uid mapping issues. |
| 8 | Controllers get fixed PCI slots: NVMDATA `0000:00:04.0`, NVMNULL `0000:00:05.0`; scripts find the kernel block device through `/sys/bus/pci/devices/<bdf>/nvme/`. | `nvmeX` numbering changes every time a controller is rebound between drivers. |
| 9 | Root filesystem label verified as `cloudimg-rootfs` by reading the ext4 superblock of partition 1 in the converted image. | The brief asked to verify it. |
