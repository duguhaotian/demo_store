#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CH_DIR="${CH_DIR:-$ROOT_DIR/cloud-hypervisor}"
WORKDIR="${WORKDIR:-$(mktemp -d /tmp/template-restore-e2e.XXXXXX)}"
MEMORY_SIZE="${MEMORY_SIZE:-512M}"
CMDLINE="${CMDLINE:-console=hvc0 root=/dev/vda1 rw}"
SECCOMP="${SECCOMP:-false}"
SKIP_UFFD_PREFLIGHT="${SKIP_UFFD_PREFLIGHT:-0}"
CH_VERBOSE="${CH_VERBOSE:--v}"
WAIT_FOR_CONFIRM="${WAIT_FOR_CONFIRM:-1}"
METRICS_SETTLE_SECONDS="${METRICS_SETTLE_SECONDS:-2}"

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
  SECCOMP=$SECCOMP
  SKIP_UFFD_PREFLIGHT=$SKIP_UFFD_PREFLIGHT
  CH_VERBOSE=$CH_VERBOSE
  WAIT_FOR_CONFIRM=$WAIT_FOR_CONFIRM
  TEMPLATE_METRICS_LOG=${TEMPLATE_METRICS_LOG:-<workdir>/template-metrics.log}
  METRICS_SETTLE_SECONDS=$METRICS_SETTLE_SECONDS
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

has_cap_sys_ptrace() {
    local cap_eff_hex
    cap_eff_hex="$(awk '/^CapEff:/ { print $2 }' /proc/self/status 2>/dev/null || true)"
    [[ -n "$cap_eff_hex" ]] || return 1

    local cap_eff=$((16#$cap_eff_hex))
    (( (cap_eff & (1 << 19)) != 0 ))
}

check_uffd_permissions() {
    [[ "$SKIP_UFFD_PREFLIGHT" == "1" ]] && return 0

    local sysctl_path="/proc/sys/vm/unprivileged_userfaultfd"
    [[ -r "$sysctl_path" ]] || return 0

    local unprivileged
    read -r unprivileged <"$sysctl_path" || unprivileged=""
    if [[ "$unprivileged" == "1" ]] || has_cap_sys_ptrace; then
        return 0
    fi

    cat >&2 <<EOF
on-demand restore needs userfaultfd permissions that can handle KVM/kernel-originated faults.
Current host has vm.unprivileged_userfaultfd=$unprivileged and this process lacks CAP_SYS_PTRACE,
so cloud-hypervisor restore would fail with:
  Failed to create userfaultfd: Operation not permitted

Fix one of:
  sudo sysctl -w vm.unprivileged_userfaultfd=1
  run make e2e as a user/process with CAP_SYS_PTRACE

Set SKIP_UFFD_PREFLIGHT=1 to bypass this check.
EOF
    exit 2
}

wait_for_api() {
    local socket="$1"
    local log_path="${2:-}"
    local pid="${3:-}"
    for _ in $(seq 1 120); do
        if [[ -S "$socket" ]] && "$CH_REMOTE" --api-socket "$socket" info >/dev/null 2>&1; then
            return 0
        fi
        if [[ -n "$pid" ]] && ! kill -0 "$pid" >/dev/null 2>&1; then
            echo "cloud-hypervisor exited before API socket became ready: $socket" >&2
            if [[ -n "$log_path" && -f "$log_path" ]]; then
                echo "last 120 lines from $log_path:" >&2
                tail -n 120 "$log_path" >&2
            fi
            return 1
        fi
        sleep 0.5
    done
    echo "timed out waiting for API socket $socket" >&2
    if [[ -n "$log_path" && -f "$log_path" ]]; then
        echo "last 120 lines from $log_path:" >&2
        tail -n 120 "$log_path" >&2
    fi
    return 1
}

stop_source_sandbox() {
    [[ -n "${SRC_PID:-}" ]] || return 0

    echo "[4/7] stopping source sandbox to release disk lock"
    "$CH_REMOTE" --api-socket "$SRC_API" shutdown-vmm >/dev/null 2>&1 || true
    sleep 1

    if kill -0 "$SRC_PID" >/dev/null 2>&1; then
        kill "$SRC_PID" >/dev/null 2>&1 || true
        sleep 1
    fi

    if kill -0 "$SRC_PID" >/dev/null 2>&1; then
        echo "source sandbox did not exit after shutdown-vmm; forcing it down" >&2
        kill -KILL "$SRC_PID" >/dev/null 2>&1 || true
    fi

    wait "$SRC_PID" >/dev/null 2>&1 || true
    SRC_PID=""
}

cleanup() {
    set +e
    [[ -n "${SRC_PID:-}" ]] && kill "$SRC_PID" >/dev/null 2>&1
    [[ -n "${RESTORE_PID:-}" ]] && kill "$RESTORE_PID" >/dev/null 2>&1
    [[ -n "${SERVICE_PID:-}" ]] && kill "$SERVICE_PID" >/dev/null 2>&1
}
trap cleanup EXIT

wait_for_user_confirmation() {
    [[ "$WAIT_FOR_CONFIRM" == "1" ]] || return 0
    [[ -t 0 ]] || return 0

    echo
    echo "template restore e2e finished; press Enter to stop background processes and exit."
    read -r _
}

metric_value() {
    local line="$1"
    local key="$2"
    for field in $line; do
        if [[ "$field" == "$key="* ]]; then
            printf '%s\n' "${field#*=}"
            return 0
        fi
    done
    printf '0\n'
}

pct() {
    local numerator="$1"
    local denominator="$2"
    awk -v n="$numerator" -v d="$denominator" 'BEGIN { if (d == 0) printf "0.00"; else printf "%.2f", (n * 100.0 / d) }'
}

summarize_template_metrics() {
    if [[ ! -f "$METRICS_LOG" ]]; then
        echo "template metrics log missing: $METRICS_LOG" >&2
        return 1
    fi

    local metrics_line
    metrics_line="$(grep '^metrics ' "$METRICS_LOG" | tail -n 1 || true)"
    if [[ -z "$metrics_line" ]]; then
        echo "template metrics summary missing in $METRICS_LOG" >&2
        return 1
    fi

    local backend_bytes page_size read_faults write_faults read_errors bytes_read unique_pages unique_bytes duplicate_reads connections
    backend_bytes="$(metric_value "$metrics_line" backend_bytes)"
    page_size="$(metric_value "$metrics_line" page_size)"
    read_faults="$(metric_value "$metrics_line" read_faults)"
    write_faults="$(metric_value "$metrics_line" write_faults)"
    read_errors="$(metric_value "$metrics_line" read_errors)"
    bytes_read="$(metric_value "$metrics_line" bytes_read)"
    unique_pages="$(metric_value "$metrics_line" unique_pages)"
    unique_bytes="$(metric_value "$metrics_line" unique_bytes)"
    duplicate_reads="$(metric_value "$metrics_line" duplicate_reads)"
    connections="$(metric_value "$metrics_line" connections)"

    local deferred_bytes
    if (( backend_bytes > unique_bytes )); then
        deferred_bytes=$((backend_bytes - unique_bytes))
    else
        deferred_bytes=0
    fi

    echo
    echo "template memory reuse summary:"
    echo "  metrics_log=$METRICS_LOG"
    echo "  backend_bytes=$backend_bytes page_size=$page_size connections=$connections"
    echo "  read_faults=$read_faults write_faults=$write_faults read_errors=$read_errors duplicate_reads=$duplicate_reads"
    echo "  bytes_read=$bytes_read unique_pages=$unique_pages unique_bytes=$unique_bytes"
    echo "  template_read_ratio=$(pct "$unique_bytes" "$backend_bytes")%"
    echo "  deferred_reuse_ratio=$(pct "$deferred_bytes" "$backend_bytes")%"
    echo "  duplicate_request_ratio=$(pct "$duplicate_reads" "$read_faults")%"
}

require_file KERNEL "$KERNEL"
require_file DISK "$DISK"
check_uffd_permissions

mkdir -p "$WORKDIR"

echo "[1/7] building template helper"
(cd "$ROOT_DIR" && cargo build --bin template-memory-demo)

if [[ ! -x "$CH_BIN" || ! -x "$CH_REMOTE" ]]; then
    echo "[2/7] building cloud-hypervisor binaries"
    (cd "$CH_DIR" && cargo build --features kvm --bin cloud-hypervisor --bin ch-remote)
else
    echo "[2/7] using existing cloud-hypervisor binaries"
fi

CH_LOG_ARGS=()
if [[ -n "$CH_VERBOSE" ]]; then
    CH_LOG_ARGS+=("$CH_VERBOSE")
fi

SRC_API="$WORKDIR/source.sock"
RESTORE_API="$WORKDIR/restore.sock"
SNAPSHOT_DIR="$WORKDIR/snapshot"
TEMPLATE_DIR="$WORKDIR/template"
TEMPLATE_SOCKET="$WORKDIR/template.sock"
SRC_LOG="$WORKDIR/source.log"
RESTORE_LOG="$WORKDIR/restore.log"
SERVICE_LOG="$WORKDIR/template-service.log"
METRICS_LOG="${TEMPLATE_METRICS_LOG:-$WORKDIR/template-metrics.log}"

rm -rf "$SNAPSHOT_DIR" "$TEMPLATE_DIR" "$SRC_API" "$RESTORE_API" "$TEMPLATE_SOCKET" "$METRICS_LOG"
mkdir -p "$SNAPSHOT_DIR"

echo "[3/7] starting source sandbox"
"$CH_BIN" \
    "${CH_LOG_ARGS[@]}" \
    --api-socket "$SRC_API" \
    --seccomp "$SECCOMP" \
    --kernel "$KERNEL" \
    --disk "path=$DISK" \
    --memory "size=$MEMORY_SIZE" \
    --cmdline "$CMDLINE" \
    >"$SRC_LOG" 2>&1 &
SRC_PID=$!
wait_for_api "$SRC_API" "$SRC_LOG" "$SRC_PID"

echo "[4/7] pausing and snapshotting source sandbox"
"$CH_REMOTE" --api-socket "$SRC_API" pause
"$CH_REMOTE" --api-socket "$SRC_API" snapshot "file://$SNAPSHOT_DIR"
test -f "$SNAPSHOT_DIR/memory-ranges"
test -f "$SNAPSHOT_DIR/state.json"
stop_source_sandbox

echo "[5/7] converting memory snapshot to template format"
"$TEMPLATE_BIN" convert --snapshot-dir "$SNAPSHOT_DIR" --template-dir "$TEMPLATE_DIR"
test -f "$TEMPLATE_DIR/template.manifest"
test -f "$TEMPLATE_DIR/memory-ranges"

echo "[6/7] starting template service"
"$TEMPLATE_BIN" serve --template-dir "$TEMPLATE_DIR" --socket "$TEMPLATE_SOCKET" --metrics-log "$METRICS_LOG" \
    >"$SERVICE_LOG" 2>&1 &
SERVICE_PID=$!
for _ in $(seq 1 40); do
    [[ -S "$TEMPLATE_SOCKET" ]] && break
    sleep 0.25
done
test -S "$TEMPLATE_SOCKET"

echo "[7/7] restoring sandbox through template service"
"$CH_BIN" \
    "${CH_LOG_ARGS[@]}" \
    --api-socket "$RESTORE_API" \
    --seccomp "$SECCOMP" \
    --restore "source_url=file://$SNAPSHOT_DIR,memory_restore_mode=ondemand,template_socket=$TEMPLATE_SOCKET,resume=true" \
    >"$RESTORE_LOG" 2>&1 &
RESTORE_PID=$!
wait_for_api "$RESTORE_API" "$RESTORE_LOG" "$RESTORE_PID"

if ! grep -q "template UFFD restore: using template service socket" "$RESTORE_LOG"; then
    echo "restore completed, but restore log does not show template service usage" >&2
    echo "restore log: $RESTORE_LOG" >&2
    echo "last 120 lines from $RESTORE_LOG:" >&2
    tail -n 120 "$RESTORE_LOG" >&2
    exit 1
fi

sleep "$METRICS_SETTLE_SECONDS"
summarize_template_metrics
echo "template restore e2e passed"
echo "workdir: $WORKDIR"
wait_for_user_confirmation
