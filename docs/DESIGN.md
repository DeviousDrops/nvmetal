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
| 10 | Legacy VFIO container/group API (`/dev/vfio/vfio` + `/dev/vfio/N`), type1v2 IOMMU, not iommufd cdev. | Works on every kernel since 3.6 and in no-IOMMU mode with the same code; the guest kernel (6.8) supports both. |
| 11 | IOVA window chosen from `VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE`, starting at 4 GiB. | Stays clear of the x86 MSI window at `0xfee00000` that the IOMMU reserves; falls back to [4 GiB, 512 GiB) when the kernel does not report ranges. |
| 12 | The DMA pool is mapped once at start-up, one `VFIO_IOMMU_MAP_DMA` per 2 MiB hugepage, at consecutive IOVAs. | No IOMMU work on the I/O path; virt→IOVA is one table lookup indexed by `(va - base) >> 21`. |
| 13 | IOVA source behind `struct nvm_iova_ops` (IOMMU / physical via `/proc/self/pagemap` / identity for tests). | The no-IOMMU fallback and host tests share the allocator. With physical addresses only a 2 MiB hugepage is known to be contiguous, so allocations may not cross a chunk boundary in that mode. |
| 14 | 64-bit registers (CAP, ASQ, ACQ) accessed as two 32-bit MMIO operations, low half first. | Permitted by §3.1 and does not depend on the platform issuing 8-byte MMIO transactions. |
| 15 | The admin queue reuses the I/O queue pair code (QID 0), driven synchronously under a mutex. | One ring implementation to test; admin commands are rare and never on the I/O path. |
| 16 | Commands are capped at `depth - 1` outstanding per queue pair (= usable CIDs). | §4.1.2 leaves one SQ slot empty; with equal SQ/CQ sizes the same cap guarantees the CQ cannot overflow, so the driver never has to check CQ space. |
| 17 | Timeouts use `CLOCK_MONOTONIC_COARSE` stamped at submit and are scanned every 4096 empty polls. A timed-out command is reported to its caller with `NVM_STATUS_TIMEOUT`, but its CID stays reserved until the device's late completion arrives. | A coarse vDSO clock read costs a few ns, cheap enough per I/O, and timeouts are seconds-scale. Reusing the CID early could match a stale completion to a new command. |
| 18 | `max_xfer = min(MDTS, 2 MiB)`. | 2 MiB is 512 pages: PRP1 plus a 511-entry list fits one list page, so each request needs exactly one preallocated list page (chaining is still implemented and unit tested). QEMU reports MDTS = 512 KiB. |
| 19 | Normal shutdown waits for `CSTS.SHST = 10b` for max(CAP.TO, 10 s). | RTD3E would be the precise bound, but CAP.TO is available without Identify and is ample. |
| 20 | Data buffers must come from `nvm_dma_alloc` and start 4 KiB aligned; the driver rejects anything else with `-EINVAL` before touching the queue. | Every PRP entry is then a whole page (§4.3), translation is one table lookup, and no bounce buffers are ever needed. |
| 21 | Submissions never ring the doorbell; `nvm_qpair_kick()` or `nvm_qpair_poll()` does, once per batch. | One MMIO write (one VM exit under KVM) per batch instead of per command. Verified in the guest: 32 queued writes → 1 doorbell write. |
