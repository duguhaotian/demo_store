#!/bin/bash
# UFFD + COW Demo 自动测试脚本
# 使用方法: sudo ./test.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UFFD_COW="$SCRIPT_DIR/uffd-cow"

echo "=== UFFD + COW Demo Test Script ==="

# 检查二进制文件
if [ ! -f "$UFFD_COW" ]; then
    echo "Error: $UFFD_COW not found. Run 'make' first."
    exit 1
fi

# 检查内核版本
KERNEL_VER=$(uname -r | cut -d. -f1-2)
echo "Kernel version: $(uname -r)"
if [ "$KERNEL_VER" \< "5.7" ]; then
    echo "Warning: Kernel < 5.7, WP feature may not be supported"
fi

# 检查/设置 unprivileged_userfaultfd
UFFD_SYSCTL="/proc/sys/vm/unprivileged_userfaultfd"
if [ -f "$UFFD_SYSCTL" ]; then
    VAL=$(cat "$UFFD_SYSCTL")
    echo "unprivileged_userfaultfd = $VAL"
    if [ "$VAL" != "1" ]; then
        echo "Enabling unprivileged_userfaultfd..."
        sysctl -w vm.unprivileged_userfaultfd=1 || {
            echo "Failed to enable. Run with sudo."
            exit 1
        }
    fi
fi

# 清理旧测试文件
rm -f /tmp/uffd-cow-test.bin

# 启动 creator（后台运行）
echo ""
echo "=== Starting Creator ==="
$UFFD_COW --create &
CREATOR_PID=$!

# 等待 creator 输出路径信息
sleep 1

# 获取 creator 的 fd 路径
if [ ! -d "/proc/$CREATOR_PID/fd" ]; then
    echo "Error: Creator process not found"
    exit 1
fi

# 查找 meta 和 data fd
META_FD=$(ls -la /proc/$CREATOR_PID/fd | grep "uffd-cow-meta" | awk '{print $NF}' | head -1)
DATA_FD=$(ls -la /proc/$CREATOR_PID/fd | grep "uffd-cow-data" | awk '{print $NF}' | head -1)

if [ -z "$META_FD" ] || [ -z "$DATA_FD" ]; then
    # 如果找不到命名 memfd，使用数值 fd
    FD_LIST=$(ls /proc/$CREATOR_PID/fd | sort -n)
    META_FD=$(echo "$FD_LIST" | head -3 | tail -1)
    DATA_FD=$(echo "$FD_LIST" | head -4 | tail -1)
fi

META_PATH="/proc/$CREATOR_PID/fd/$META_FD"
DATA_PATH="/proc/$CREATOR_PID/fd/$DATA_FD"

echo "Creator PID: $CREATOR_PID"
echo "Meta path: $META_PATH"
echo "Data path: $DATA_PATH"

# 检查测试文件
sleep 0.5
if [ -f "/tmp/uffd-cow-test.bin" ]; then
    SIZE=$(stat -c%s /tmp/uffd-cow-test.bin)
    echo "Test file created: $SIZE bytes"
else
    echo "Error: Test file not created"
    kill $CREATOR_PID 2>/dev/null || true
    exit 1
fi

# 启动 Worker 1
echo ""
echo "=== Starting Worker 1 ==="
$UFFD_COW --join --meta "$META_PATH" --data "$DATA_PATH" 2>&1 | sed 's/^/[Worker1] /'
WORKER1_OUTPUT=$(mktemp)
$UFFD_COW --join --meta "$META_PATH" --data "$DATA_PATH" > "$WORKER1_OUTPUT" 2>&1

# 检查 Worker1 输出是否包含 COW 标记
if grep -q "'XXXX'..." "$WORKER1_OUTPUT"; then
    echo "[Test] Worker1 COW write: PASS"
else
    echo "[Test] Worker1 COW write: FAIL"
fi

# 启动 Worker 2（验证 COW 隔离）
echo ""
echo "=== Starting Worker 2 ==="
WORKER2_OUTPUT=$(mktemp)
$UFFD_COW --join --meta "$META_PATH" --data "$DATA_PATH" > "$WORKER2_OUTPUT" 2>&1
cat "$WORKER2_OUTPUT" | sed 's/^/[Worker2] /'

# 验证 Worker2 看到的是原始值（不是 Worker1 的修改）
if grep -q "'AAAA'..." "$WORKER2_OUTPUT"; then
    echo "[Test] Worker2 sees original shared page: PASS (COW isolation verified)"
else
    echo "[Test] Worker2 sees original shared page: FAIL"
fi

# 清理
echo ""
echo "=== Cleanup ==="
kill $CREATOR_PID 2>/dev/null || true
rm -f "$WORKER1_OUTPUT" "$WORKER2_OUTPUT"
rm -f /tmp/uffd-cow-test.bin

echo ""
echo "=== Test Complete ==="