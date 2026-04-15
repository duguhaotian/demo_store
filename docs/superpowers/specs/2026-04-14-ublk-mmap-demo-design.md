---
name: ublk-mmap-demo-design
description: Design for ublk mmap demo - test program directly mmap ublk block device, page fault triggers ublk IO to sparse file backend
type: project
---

# ublk mmap Demo 设计文档

**日期：** 2026-04-15
**目标：** 演示测试程序直接 mmap ublk 块设备，缺页触发 ublk IO，ublksrv 从后端稀疏文件获取数据

## 1. 总体架构

### 核心流程

```
┌─────────────────┐
│  后端稀疏文件    │  backend.data (sparse file)
│                 │  大小与 ublkbX 一一对应
└─────────────────┘
        ↑ 读写
        │
┌─────────────────┐
│   ublksrv       │  用户态 ublk server
│                 │  - 创建 ublk 设备
│                 │  - 处理 IO 请求
│                 │  - 直接读写稀疏文件
└─────────────────┘
        ↑ ublk IO
        │
┌─────────────────┐
│  ublk 块设备     │  /dev/ublkb0
│                 │
└─────────────────┘
        ↑ mmap + 缺页
        │
┌─────────────────┐
│   测试程序       │  mmap /dev/ublkb0
│                 │  访问触发缺页
└─────────────────┘
```

### 目录结构

```
ublk-mmap-demo/
├── README.md               # Demo 说明文档
├── Makefile                # 编译脚本
├── ublksrv/                # 用户态 ublk server
│   ├── ublk_loop_srv.c     # ublk server（稀疏文件后端）
│   └── Makefile
├── scripts/                # 辅助脚本
│   ├── setup.sh            # 创建稀疏文件、启动 ublksrv
│   └── cleanup.sh          # 清理环境
└── test/                   # 测试程序
│   ├── test_mmap.c         # mmap ublk 设备测试
│   └── Makefile
```

### 三大组件

1. **后端稀疏文件 `backend.data`**
   - 作为 ublk 设备的虚拟存储
   - 稀疏文件，大小与 ublk 设备容量一致
   - ublksrv 按 sector 偏移直接读写

2. **ublksrv（用户态 ublk server）**
   - 创建 ublk 块设备 `/dev/ublkb0`
   - 处理 IO 请求：按 sector 偏移读写稀疏文件

3. **测试程序**
   - 直接 mmap `/dev/ublkb0`
   - 访问地址触发缺页
   - 打印获取的数据验证正确性

## 2. ublksrv 设计

### 核心功能

**初始化流程：**
1. 打开 `/dev/ublk-control`
2. 打开后端稀疏文件 `backend.data`
3. 配置 ublk 设备参数（设备大小 = 文件大小）
4. mmap ublk 共享内存区域
5. 启动 io_uring 循环处理 IO 请求

**ublk 设备配置：**
```c
struct ublk_params params = {
    .dev_id = 0,
    .block_size = 512,           // 扇区大小
    .nr_hw_queues = 1,
    .queue_depth = 32,
    .dev_size = sparse_file_size, // 与稀疏文件大小一致
};
```

**IO 处理：**
```c
// 读请求
if (io->op == UBLK_IO_OP_READ) {
    off_t offset = io->sector * 512;
    size_t size = io->nr_sectors * 512;
    
    // 直接从稀疏文件读取
    pread(backend_fd, io->addr, size, offset);
    
    ublk_complete_io(io);
}

// 写请求
if (io->op == UBLK_IO_OP_WRITE) {
    off_t offset = io->sector * 512;
    size_t size = io->nr_sectors * 512;
    
    // 直接写入稀疏文件
    pwrite(backend_fd, io->addr, size, offset);
    
    ublk_complete_io(io);
}
```

## 3. 后端稀疏文件设计

### 文件属性

- **类型：** 稀疏文件（sparse file）
- **大小：** 例如 64MB（与 ublk 设备容量一致）
- **特点：** 未写入区域为 hole，读取返回 0

### 创建方式

```bash
# 创建 64MB 稀疏文件（不实际占用磁盘空间）
truncate -s 64M backend.data

# 或用 dd 创建
dd if=/dev/zero of=backend.data bs=1M count=0 seek=64
```

### 预填充测试数据（可选）

```bash
# 在特定偏移写入测试数据
echo "Block 0 test data" | dd of=backend.data bs=512 seek=0 conv=notrunc
echo "Block 1024 test data" | dd of=backend.data bs=512 seek=1024 conv=notrunc
```

## 4. 测试程序设计

### 核心流程

```c
int main() {
    const char *dev_path = "/dev/ublkb0";
    size_t dev_size = 64 * 1024 * 1024;  // 64MB
    
    // 1. 打开 ublk 块设备
    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        perror("open ublk device");
        return 1;
    }
    
    // 2. mmap ublk 设备
    void *map = mmap(NULL, dev_size,
                     PROT_READ,
                     MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    
    // 3. 触发缺页：访问不同偏移
    printf("Offset 0: %s\n", (char *)map);
    printf("Offset 512K: %.30s...\n", (char *)map + 512 * 1024);
    
    // 4. 验证数据（检查是否从稀疏文件读取）
    printf("Access triggered page faults, check ublksrv logs\n");
    
    // 5. 清理
    munmap(map, dev_size);
    close(fd);
    
    return 0;
}
```

## 5. 完整数据流

```
测试程序 mmap /dev/ublkb0
            ↓
        访问 map[offset] 触发缺页
            ↓
        内核块层向 ublk 发起读请求
            ↓
        ublk 框架将请求传递给 ublksrv
            ↓
        ublksrv 解析请求（sector, nr_sectors）
            ↓
        ublksrv 从 backend.data 读取对应偏移数据
            ↓
        数据写入 ublk 共享内存
            ↓
        ublksrv 提交完成结果
            ↓
        ublk 框架返回数据
            ↓
        页面填充完成，缺页解决
            ↓
        测试程序继续执行
```

### 偏移映射关系

```
mmap 偏移 0~512     → sector 0     → backend.data offset 0~512
mmap 偏移 512~1024  → sector 1     → backend.data offset 512~1024
mmap 偏移 N         → sector N/512 → backend.data offset N
```

直接一一对应，无文件系统层转换。

## 6. 错误处理

### ublksrv

- ublk-control 打开失败 → 检查权限和内核版本（6.0+）
- 稀疏文件打开失败 → 检查文件路径和权限
- IO 处理失败 → 返回 IO 错误状态

### 测试程序

- ublk 设备不存在 → 提示启动 ublksrv
- mmap 失败 → 检查设备大小和权限
- 数据验证失败 → 打印期望值与实际值

## 7. 测试计划

### 基础功能测试

1. 创建稀疏文件 → 验证文件大小
2. 启动 ublksrv → 验证 `/dev/ublkb0` 创建成功
3. 运行测试程序 → 验证 mmap 和数据访问
4. 检查 ublksrv 日志 → 验证 IO 请求处理

### 边界测试

- 访问不同偏移 → 验证多个缺页处理
- 访问稀疏文件 hole 区域 → 应返回 0
- 访问预填充数据区域 → 验证数据正确

### 清理测试

- 停止 ublksrv → 验证 ublk 设备删除
- 删除稀疏文件 → 清理环境

## 8. 系统要求

- **Linux 内核：** 6.0+ （ublk 框架支持）
- **权限：** root（创建 ublk 设备需要）
- **依赖：** liburing（io_uring 库）

## 9. 实现要点

**Why:** 演示最简化的 ublk mmap 流程：测试程序直接 mmap ublk 块设备，缺页触发 IO，ublksrv 从稀疏文件获取数据。无中间文件系统层。

**How to apply:**
- 稀疏文件大小必须与 ublk 设备容量一致
- sector 到文件偏移直接映射：`offset = sector * 512`
- mmap 块设备触发缺页时，内核直接向块设备发起 IO
- ublksrv 处理 IO 并读写稀疏文件对应偏移