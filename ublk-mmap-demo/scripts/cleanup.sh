#!/bin/bash
# ublk-mmap-demo/scripts/cleanup.sh
# Cleanup: stop ublksrv, remove sparse file

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_DIR="$(dirname "$SCRIPT_DIR")"
PID_FILE="${DEMO_DIR}/.ublksrv_pid"
BACKEND_FILE="${DEMO_DIR}/backend.data"

echo "=== ublk mmap demo cleanup ==="

# Stop ublksrv
if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE")
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        echo "Stopping ublksrv (PID: $PID)..."
        kill "$PID"
        sleep 1
        # Force kill if still running
        if kill -0 "$PID" 2>/dev/null; then
            echo "Force killing..."
            kill -9 "$PID"
        fi
        rm -f "$PID_FILE"
    else
        echo "ublksrv not running or PID file stale"
        rm -f "$PID_FILE"
    fi
else
    echo "No PID file found, ublksrv may not be running"
fi

# Check if ublk device still exists
if [ -b "/dev/ublkb0" ]; then
    echo "Warning: /dev/ublkb0 still exists"
    echo "This should be cleaned up by ublksrv on exit"
fi

# Remove sparse file
if [ -f "$BACKEND_FILE" ]; then
    echo "Removing sparse file: $BACKEND_FILE"
    rm -f "$BACKEND_FILE"
fi

echo "Cleanup complete"