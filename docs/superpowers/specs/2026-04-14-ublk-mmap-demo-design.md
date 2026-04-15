---
name: ublk-mmap-demo-design
description: Design for ublk mmap demo demonstrating page fault handling with userspace data fetching via file-backed ublk device
type: project
---

# ublk mmap Demo 设计文档

**日期：** 2026-04-15
**目标：** 演示 ublk 以普通文件作为后端，测试程序 mmap ublk 上文件系统的文件，缺页触发 ublk IO 链路的完整流程

## 1. 总体架构

### 核心流程

```
┌─────────────────┐
│   后端文件 A     │  (普通文件，作为 ublk 虚拟磁盘存储)
│  backend.data   │
└─────────────────┘
        ↑
        │ ublksrv 读取数据
        │
┌─────────────────┐
│   ublksrv       │  (用户态 ublk server)
│                 │  - 创建 ublk 块设备
│                 │  - 处理 IO 请求
│                 │  - 从后端文件读取数据
└─────────────────┘
        ↑
        │ ublk IO 请求
        │
┌─────────────────┐
│  ublk 块设备     │  /dev/ublkb0
│                 │  (用户态块设备)
└─────────────────┘
        ↑
        │ 文件系统 IO
        │
┌─────────────────┐
│   文件系统       │  (ext4/fat 等，挂载在 ublk 上)
│                 │
│   文件 A         │  /mnt/ublk/test.data
└─────────────────┘
        ↑
        │ mmap 缺页触发
        │
┌─────────────────┐
│   测试程序       │  mmap 文件 A，访问触发缺页
│                 │
└─────────────────┘
```

### 目录结构

```
ublk-mmap-demo/
├── README.md               # Demo 说明文档
├── Makefile                # 编译脚本
├── ublksrv/                # 用户态 ublk server
│   ├── ublk_loop_srv.c     # ublk server（文件后端）
│   └── Makefile
├── scripts/                # 辅助脚本
│   ├── setup.sh            # 创建后端文件、挂载文件系统
│   ├── cleanup.sh          # 清理环境
│   └── run_demo.sh         # 运行完整 demo
└── test/                   # 测试程序
│   ├── test_mmap.c         # mmap 测试程序
│   └── Makefile
```

### 三大组件

1. **ublksrv（用户态 ublk server）**
   - 创建 ublk 块设备 `/dev/ublkb0`
   - 以普通文件 `backend.data` 作为后端存储
   - 处理 IO 请求：读取请求 → 从后端文件读取对应数据 → 返回

2. **文件系统层**
   - 在 ublk 块设备上挂载文件系统（ext4）
   - 创建测试文件 `/mnt/ublk/test.data`
   - 预填充测试数据

3. **测试程序**
   - mmap 文件 `/mnt/ublk/test.data`
   - 访问地址触发缺页
   - 打印获取的数据验证正确性

## 2. ublksrv 设计

### 核心功能

**初始化流程：**
1. 打开 `/dev/ublk-control`
2. 打开后端文件 `backend.data`（作为虚拟磁盘存储）
3. 通过 ioctl 配置 ublk 设备参数
4. mmap ublk 共享内存区域（用于 IO 数据传输）
5. 启动 io_uring 循环处理 IO 请求

**ublk 设备配置：**
```c
struct ublk_params params = {
    .dev_id = 0,
    .block_size = 512,        // 扇区大小
    .nr_hw_queues = 1,
    .queue_depth = 32,
    .dev_size = file_size,    // 后端文件大小
};
```

**IO 处理循环：**
```c
while (running) {
    // 1. 等待 io_uring 完成事件
    io_uring_wait_cqe(&ring, &cqe);
    
    // 2. 解析 IO 请求
    struct ublk_io *io = (struct ublk_io *)cqe->user_data;
    
    // 3. 处理读请求
    if (io->op == UBLK_IO_OP_READ) {
        // 计算后端文件偏移
        off_t offset = io->sector * 512;
        
        // 从后端文件读取数据
        pread backend_fd, io->addr, io->nr_sectors * 512, offset);
        
        // 提交完成结果
        ublk_complete_io(io);
    }
    
    // 4. 处理写请求（可选）
    if (io->op == UBLK_IO_OP_WRITE) {
        pwrite backend_fd, io->addr, io->nr_sectors * 512, io->sector * 512);
        ublk_complete_io(io);
    }
}
```

### 后端文件布局

后端文件 `backend.data` 作为虚拟磁盘的原始存储：
- 文件大小：例如 64MB
- 内容：原始磁盘数据（包含文件系统结构）
- ublksrv 按 sector 偏移直接读取/写入

## 3. 文件系统层设计

### 设置流程（setup.sh）

```bash
# 1. 创建后端文件（虚拟磁盘）
dd if=/dev/zero of=backend.data bs=1M count=64

# 2. 启动 ublksrv
./ublksrv &

# 3. 等待 ublk 设备创建
sleep 1

# 4. 在 ublk 设备上创建文件系统
mkfs.ext4 /dev/ublkb0

# 5. 挂载文件系统
mkdir -p /mnt/ublk
mount /dev/ublkb0 /mnt/ublk

# 6. 创建测试文件并填充数据
echo "Hello from ublk mmap demo - Block 0" > /mnt/ublk/test.data
dd if=/dev/urandom of=/mnt/ublk/test.data bs=4K count=16 seek=1 conv=notrunc
```

### 测试文件布局

`/mnt/ublk/test.data`：
- 大小：64KB + 开头文字
- 内容：测试数据，用于 mmap 验证

## 4. 测试程序设计

### 核心流程

```c
int main() {
    const char *file_path = "/mnt/ublk/test.data";
    size_t file_size = 64 * 1024;
    
    // 1. 打开文件
    int fd = open(file_path, O_RDONLY);
    
    // 2. mmap 文件
    void *map = mmap(NULL, file_size,
                     PROT_READ,
                     MAP_PRIVATE, fd, 0);
    
    // 3. 触发缺页：访问不同偏移
    printf("Offset 0: %s\n", (char *)map);           // 第一个块
    printf("Offset 4K: %.20s...\n", (char *)map + 4096);  // 第二个块
    
    // 4. 打印缺页触发信息（配合内核日志）
    printf("Check dmesg for page fault handling logs\n");
    
    // 5. 清理
    munmap(map, file_size);
    close(fd);
    
    return 0;
}
```

## 5. 完整数据流

```
测试程序 mmap /mnt/ublk/test.data
            ↓
        访问 map[offset] 触发缺页
            ↓
        文件系统 ext4 处理缺页
            ↓
        ext4 向 ublk 块设备发起读请求
            ↓
        ublk 框架将请求传递给 ublksrv
            ↓
        ublksrv 解析请求（sector, nr_sectors）
            ↓
        ublksrv 从 backend.data 读取对应数据
            ↓
        数据写入 ublk 共享内存
            ↓
        ublksrv 提交完成结果
            ↓
        ublk 框架返回数据给文件系统
            ↓
        文件系统填充页面
            ↓
        缺页解决，测试程序继续执行
```

### 缺页到 IO 的映射关系

```
文件偏移 0~4K    → 文件系统块 0    → ublk sector 0~7     → backend.data offset 0~4K
文件偏移 4K~8K   → 文件系统块 1    → ublk sector 8~15    → backend.data offset 4K~8K
...
```

（具体映射取决于文件系统布局和文件在磁盘上的位置）

## 6. 错误处理

### ublksrv

- ublk-control 打开失败 → 检查权限和内核版本（ublk 需要 6.0+）
- 后端文件打开失败 → 检查文件路径和权限
- io_uring 初始化失败 → 使用 fallback poll 模式
- IO 处理失败 → 记录错误，返回 IO 错误状态

### 测试程序

- 文件不存在 → 提示运行 setup.sh
- mmap 失败 → 检查文件系统和挂载状态
- 数据验证失败 → 打印期望值与实际值对比

## 7. 测试计划

### 基础功能测试

1. 运行 `setup.sh` → 验证后端文件、ublk 设备、文件系统创建成功
2. 运行测试程序 → 验证 mmap 和数据访问正常
3. 检查 `dmesg` → 验证缺页触发和 IO 处理流程
4. 检查 ublksrv 日志 → 验证 IO 请求处理正确

### 边界测试

- 访问文件不同偏移 → 验证多个缺页处理
- 大文件 mmap → 验证大量缺页处理
- 并发 mmap（多进程） → 验证并发安全性

### 清理测试

- 运行 `cleanup.sh` → 验证环境清理正确
- 检查 ublk 设备删除 → 验证 ublksrv 正常退出

## 8. 系统要求

- **Linux 内核：** 6.0+ （ublk 框架支持）
- **权限：** root（创建 ublk 设备、挂载文件系统需要）
- **依赖：** liburing（io_uring 库）

## 9. 实现要点

**Why:** 这个 demo 展示了 ublk 以文件为后端创建块设备，测试程序 mmap 文件触发缺页的完整 IO 链路。核心价值在于演示：
- ublk 作为用户态块设备框架的工作机制
- 文件系统缺页如何触发底层块设备 IO
- ublksrv 如何处理 IO 并从后端存储获取数据

**How to apply:** 实现时注意以下关键点：
- ublksrv 需正确解析 ublk IO 请求（sector 编号、扇区数）
- 后端文件偏移计算：`offset = sector * 512`
- 文件系统块到 ublk sector 的映射由文件系统内部处理
- 测试程序 mmap 文件触发缺页，观察完整数据流