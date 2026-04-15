#!/bin/bash
# ublk-mmap-demo/scripts/setup.sh
# Setup: create sparse file, build, start ublksrv

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_DIR="$(dirname "$SCRIPT_DIR")"
BACKEND_FILE="${DEMO_DIR}/backend.data"
UBLKSRV="${DEMO_DIR}/ublksrv/ublk_loop_srv"
SIZE_MB=64

echo "=== ublk mmap demo setup ==="

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Error: This script requires root privileges for ublk device creation"
    echo "Run with: sudo $0"
    exit 1
fi

# Check kernel version (need 6.0+ for ublk)
KERNEL_VER=$(uname -r | cut -d. -f1-2)
if [ "$KERNEL_VER" \< "6.0" ]; then
    echo "Error: Linux kernel 6.0+ required for ublk support"
    echo "Current kernel: $(uname -r)"
    exit 1
fi

# Check ublk-control exists
if [ ! -e "/dev/ublk-control" ]; then
    echo "Error: /dev/ublk-control not found"
    echo "Make sure ublk kernel module is loaded: modprobe ublk_drv"
    exit 1
fi

# Build if needed
if [ ! -x "$UBLKSRV" ]; then
    echo "Building..."
    make -C "$DEMO_DIR"
fi

# Create sparse file
echo "Creating sparse file: $BACKEND_FILE (${SIZE_MB}MB)"
truncate -s "${SIZE_MB}M" "$BACKEND_FILE"

# Write test data at specific offsets
echo "Writing test data..."
echo "Block 0: Hello from ublk mmap demo offset 0!" | \
    dd of="$BACKEND_FILE" bs=512 seek=0 conv=notrunc 2>/dev/null
echo "Block 8 (4KB): Data at 4KB offset test" | \
    dd of="$BACKEND_FILE" bs=512 seek=8 conv=notrunc 2>/dev/null

# Show file info
ls -lh "$BACKEND_FILE"
du -h "$BACKEND_FILE"

# Start ublksrv in background
echo "Starting ublksrv..."
cd "$DEMO_DIR"
"$UBLKSRV" "$BACKEND_FILE" &
UBLKSRV_PID=$!

# Wait for device to appear
sleep 2

# Check if device was created
if [ -b "/dev/ublkb0" ]; then
    echo "Success! ublk device created: /dev/ublkb0"
    ls -l /dev/ublkb0
    echo "ublksrv PID: $UBLKSRV_PID"
    echo ""
    echo "Now run: sudo ./test/test_mmap"
else
    echo "Error: ublk device not created"
    kill $UBLKSRV_PID 2>/dev/null || true
    exit 1
fi

# Save PID for cleanup
echo "$UBLKSRV_PID" > "${DEMO_DIR}/.ublksrv_pid"