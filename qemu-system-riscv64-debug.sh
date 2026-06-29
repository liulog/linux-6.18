#!/usr/bin/env bash
# Boot the built RISC-V kernel with an initramfs, waiting for GDB.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL="${KERNEL:-${SCRIPT_DIR}/arch/riscv/boot/Image}"
INITRD="${INITRD:-${SCRIPT_DIR}/rootfs.cpio.gz}"
MEMORY="${MEMORY:-2G}"
SMP="${SMP:-2}"
LOG_DIR="${SCRIPT_DIR}/logs"
LOG_FILE="${LOG_DIR}/serial-$(date +%Y%m%d-%H%M%S).log"

# Ensure log directory exists
mkdir -p "$LOG_DIR"

for f in "$KERNEL" "$INITRD"; do
	if [[ ! -f "$f" ]]; then
		echo "error: missing file: $f" >&2
		exit 1
	fi
done

QEMU_CMD="qemu-system-riscv64 \
	-machine virt \
	-cpu rva23s64 \
	-smp $SMP \
	-m $MEMORY \
	-nographic \
	-kernel $KERNEL \
	-initrd $INITRD \
	-append \"console=ttyS0 earlycon=sbi rw root=/dev/ram\" \
	-s -S"

# Modify QEMU command to tee output to log file
QEMU_TEE_CMD="$QEMU_CMD 2>&1 | tee -a \"$LOG_FILE\""

GDB_CMD="gdb-multiarch -x $SCRIPT_DIR/debug.gdb"

echo "Serial output will be logged to: $LOG_FILE"

if command -v tmux &> /dev/null; then
    echo "tmux found, starting split screen session..."
    
    # Create a new tmux session in detached mode
    SESSION_NAME="riscv_gdb_debug_$$"
    tmux new-session -d -s "$SESSION_NAME" "bash -c '$QEMU_TEE_CMD'"
    
    # Split the window horizontally and run GDB
    tmux split-window -h "$GDB_CMD"
    
    # Attach to the session
    exec tmux attach-session -t "$SESSION_NAME"
else
    echo "tmux not found. Starting QEMU in foreground. Please start GDB manually in another terminal."
    echo "Waiting for GDB connection on localhost:1234..."
    eval exec "$QEMU_TEE_CMD"
fi
