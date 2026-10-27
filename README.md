# nvmetal

A userspace, polled NVMe driver in C. It drives an NVMe controller through
VFIO, completes I/O by polling the completion queues (with MSI-X interrupts as
a comparison mode), and is benchmarked against the Linux kernel I/O path.

Work in progress.

## License

MIT, see [LICENSE](LICENSE).
