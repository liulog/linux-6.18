#!/usr/bin/env bash
# Boot the built RISC-V kernel with an initramfs (no extra block devices).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL="/home/jingyu/workspace/linux-6.18/Image"
INITRD="${INITRD:-${SCRIPT_DIR}/rootfs.cpio.gz}"
MEMORY="${MEMORY:-2G}"
SMP="${SMP:-2}"

for f in "$KERNEL" "$INITRD"; do
	if [[ ! -f "$f" ]]; then
		echo "error: missing file: $f" >&2
		exit 1
	fi
done

exec qemu-system-riscv64 \
	-machine virt \
	-cpu rva23s64 \
	-smp "$SMP" \
	-m "$MEMORY" \
	-nographic \
	-kernel "$KERNEL" \
	-initrd "$INITRD" \
	-append "console=ttyS0 earlycon=sbi rw root=/dev/ram"
