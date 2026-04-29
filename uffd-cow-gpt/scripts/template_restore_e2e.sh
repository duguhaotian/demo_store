#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CH_DIR="${CH_DIR:-$ROOT_DIR/cloud-hypervisor}"
WORKDIR="${WORKDIR:-$(mktemp -d /tmp/template-restore-e2e.XXXXXX)}"
MEMORY_SIZE="${MEMORY_SIZE:-512M}"
CMDLINE="${CMDLINE:-console=hvc0 root=/dev/vda1 rw}"

CH_BIN="${CH_BIN:-$CH_DIR/target/debug/cloud-hypervisor}"
CH_REMOTE="${CH_REMOTE:-$CH_DIR/target/debug/ch-remote}"
TEMPLATE_BIN="${TEMPLATE_BIN:-$ROOT_DIR/target/debug/template-memory-demo}"

KERNEL="${KERNEL:-}"
DISK="${DISK:-}"

usage() {
    cat <<EOF
Usage:
  KERNEL=/path/to/vmlinux DISK=/path/to/rootfs.img $0

Optional environment:
  WORKDIR=$WORKDIR
  MEMORY_SIZE=$MEMORY_SIZE
  CMDLINE="$CMDLINE"
  CH_BIN=$CH_BIN
  CH_REMOTE=$CH_REMOTE
  TEMPLATE_BIN=$TEMPLATE_BIN
EOF
}

require_file() {
    local name="$1"
    local path="$2"
    if [[ -z "$path" || ! -f "$path" ]]; then
        echo "missing $name: $path" >&2
        usage >&2
        exit 2
    fi
}

wait_for_api() {
    local socket="$1"
    for _ in $(seq 1 120); do
        if [[ -S "$socket" ]] && "$CH_REMOTE" --api-socket "$socket" info >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    echo "timed out waiting for API socket $socket" >&2
    return 1
}

cleanup() {
    set +e
    [[ -n "${SRC_PID:-}" ]] && kill "$SRC_PID" >/dev/null 2>&1
    [[ -n "${RESTORE_PID:-}" ]] && kill "$RESTORE_PID" >/dev/null 2>&1
    [[ -n "${SERVICE_PID:-}" ]] && kill "$SERVICE_PID" >/dev/null 2>&1
}
trap cleanup EXIT

require_file KERNEL "$KERNEL"
require_file DISK "$DISK"

mkdir -p "$WORKDIR"

echo "[1/7] building template helper"
(cd "$ROOT_DIR" && cargo build --bin template-memory-demo)

if [[ ! -x "$CH_BIN" || ! -x "$CH_REMOTE" ]]; then
    echo "[2/7] building cloud-hypervisor binaries"
    (cd "$CH_DIR" && cargo build --features kvm --bin cloud-hypervisor --bin ch-remote)
else
    echo "[2/7] using existing cloud-hypervisor binaries"
fi

SRC_API="$WORKDIR/source.sock"
RESTORE_API="$WORKDIR/restore.sock"
SNAPSHOT_DIR="$WORKDIR/snapshot"
TEMPLATE_DIR="$WORKDIR/template"
TEMPLATE_SOCKET="$WORKDIR/template.sock"
SRC_LOG="$WORKDIR/source.log"
RESTORE_LOG="$WORKDIR/restore.log"
SERVICE_LOG="$WORKDIR/template-service.log"

rm -rf "$SNAPSHOT_DIR" "$TEMPLATE_DIR" "$SRC_API" "$RESTORE_API" "$TEMPLATE_SOCKET"
mkdir -p "$SNAPSHOT_DIR"

echo "[3/7] starting source sandbox"
"$CH_BIN" \
    --api-socket "$SRC_API" \
    --kernel "$KERNEL" \
    --disk "path=$DISK" \
    --memory "size=$MEMORY_SIZE" \
    --cmdline "$CMDLINE" \
    >"$SRC_LOG" 2>&1 &
SRC_PID=$!
wait_for_api "$SRC_API"

echo "[4/7] pausing and snapshotting source sandbox"
"$CH_REMOTE" --api-socket "$SRC_API" pause
"$CH_REMOTE" --api-socket "$SRC_API" snapshot "file://$SNAPSHOT_DIR"
test -f "$SNAPSHOT_DIR/memory-ranges"
test -f "$SNAPSHOT_DIR/state.json"

echo "[5/7] converting memory snapshot to template format"
"$TEMPLATE_BIN" convert --snapshot-dir "$SNAPSHOT_DIR" --template-dir "$TEMPLATE_DIR"
test -f "$TEMPLATE_DIR/template.manifest"
test -f "$TEMPLATE_DIR/memory-ranges"

echo "[6/7] starting template service"
"$TEMPLATE_BIN" serve --template-dir "$TEMPLATE_DIR" --socket "$TEMPLATE_SOCKET" \
    >"$SERVICE_LOG" 2>&1 &
SERVICE_PID=$!
for _ in $(seq 1 40); do
    [[ -S "$TEMPLATE_SOCKET" ]] && break
    sleep 0.25
done
test -S "$TEMPLATE_SOCKET"

echo "[7/7] restoring sandbox through template service"
"$CH_BIN" \
    --api-socket "$RESTORE_API" \
    --restore "source_url=file://$SNAPSHOT_DIR,memory_restore_mode=ondemand,template_socket=$TEMPLATE_SOCKET,resume=true" \
    >"$RESTORE_LOG" 2>&1 &
RESTORE_PID=$!
wait_for_api "$RESTORE_API"

if ! grep -q "template UFFD restore: using template service socket" "$RESTORE_LOG"; then
    echo "restore completed, but restore log does not show template service usage" >&2
    echo "restore log: $RESTORE_LOG" >&2
    exit 1
fi

echo "template restore e2e passed"
echo "workdir: $WORKDIR"
