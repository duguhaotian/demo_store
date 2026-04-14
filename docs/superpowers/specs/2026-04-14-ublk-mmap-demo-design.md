---
name: ublk-mmap-demo-design
description: Design for ublk mmap demo demonstrating page fault handling with userspace data fetching
type: project
---

# ublk mmap Demo 设计文档

**日期：** 2026-04-14
**目标：** 演示 ublk 设备通过 mmap 映射、缺页触发、用户态 server 处理数据获取的核心机制

## 1. 总体架构

### 目录结构

```
ublk-mmap-demo/
├── README.md               # Demo 说明文档
├── Makefile                # 编译脚本
├── kernel/                 # 内核模块
│   ├── ublk_char_dev.c     # 字符设备驱动 + mmap + fault handler
│   ├── ublk_char_dev.h     # 头文件
│   └── Makefile            # 内核模块编译
├── ublk-server/            # 用户态 ublk server
│   ├── ublk_server.c       # ublk 设备创建和 IO 处理
│   └── Makefile
└── test/                   # 测试程序
│   ├── test_mmap.c         # mmap 测试程序
│   └── Makefile
```

### 三大组件

1. **内核模块 `ublk_char_dev`**
   - 创建字符设备 `/dev/ublk_char_demo`
   - 实现 mmap 操作
   - 实现 fault handler + workqueue 数据获取

2. **用户态 ublk server**
   - 通过 `/dev/ublk-control` 创建 ublk 块设备
   - 处理来自内核模块的 IO 请求（读操作）
   - 提供虚拟数据（固定 pattern）

3. **测试程序**
   - mmap 字符设备
   - 直接访问地址触发缺页
   - 打印获取的数据验证正确性

## 2. 内核模块设计

### 字符设备核心功能

**初始化流程：**
1. 注册字符设备（`cdev_add`），设备名 `ublk_char_demo`
2. 创建设备类和设备节点 `/dev/ublk_char_demo`
3. 注册 ublk 模块参数（设备号、队列深度等）

**mmap 实现：**
- `ublk_char_mmap()` 分配虚拟内存区域
- 使用 `vm_operations_struct` 注册 fault handler
- 不立即映射物理内存，等待缺页时动态分配

**vm_operations_struct：**
```c
static const struct vm_operations_struct ublk_char_vm_ops = {
    .open = ublk_char_vma_open,
    .close = ublk_char_vma_close,
    .fault = ublk_char_fault,  // 关键：缺页处理
};
```

**fault handler `ublk_char_fault()`：**
- 获取缺页的虚拟地址和偏移
- 创建 workqueue 任务，传入缺页信息
- 返回 `VM_FAULT_NOPAGE`（暂不填充，等待 workqueue 完成）
- workqueue 完成后调用 `vm_insert_page` 填充页面

**workqueue 处理 `ublk_fetch_work()`：**
1. 根据 fault 偏移计算 ublk 块号和块内偏移
2. 通过内核 io_uring 或直接向 ublk 发送读请求
3. 等待 ublk server 返回数据
4. 分配新页面，填充数据
5. 映射页面到缺页地址
6. 唤醒等待的进程

## 3. 用户态 ublk Server 设计

### ublk Server 核心功能

**初始化流程：**
1. 打开 `/dev/ublk-control`
2. 通过 ioctl 配置 ublk 设备参数（设备 ID、块大小、容量）
3. 通过 mmap 映射 ublk 的共享内存区域（用于 IO 数据传输）
4. 启动 io_uring 循环处理 IO 请求

**ublk 设备配置：**
```c
struct ublk_params params = {
    .dev_id = 0,              // 设备 ID
    .max_sectors = 256,       // 最大扇区数
    .block_size = 4096,       // 4KB 块大小
    .nr_hw_queues = 1,        // 1 个硬件队列
    .queue_depth = 32,        // 队列深度
    .dev_size = 16 * 4096,    // 64KB 总容量
};
```

**io_uring IO 处理循环：**
1. 等待 io_uring 完成事件（ublk IO 请求）
2. 解析 IO 操作类型（读/写）和目标位置
3. 对于读请求：生成虚拟数据（按块号填充 pattern）
4. 将数据写入 ublk 共享内存区域
5. 通过 io_uring 提交完成结果

**数据生成策略：**
- 每个 4KB 块填充固定 pattern：`"Block N: data..."`
- 简单但能验证数据获取正确性

## 4. 数据流设计

### 完整数据流

```
测试程序 mmap 字符设备 → 访问地址触发缺页
                          ↓
                  字符设备 fault handler
                          ↓
                  workqueue 提交任务
                          ↓
                  向 ublk 发送读请求
                          ↓
                  ublk server 处理请求
                          ↓
                  数据写入共享内存
                          ↓
                  完成结果返回
                          ↓
                  页面填充完成
                          ↓
                  测试程序继续执行
```

### 测试程序流程

```c
int main() {
    // 1. 打开字符设备
    fd = open("/dev/ublk_char_demo", O_RDWR);

    // 2. mmap 映射
    void *map = mmap(NULL, 64 * 1024,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);

    // 3. 触发缺页：访问不同块
    printf("Block 0: %s\n", (char *)map + 0);
    printf("Block 5: %s\n", (char *)map + 5*4096);

    // 4. 验证数据
    // 期望输出: "Block 0: AAAA..." "Block 5: EEEE..."

    // 5. 清理
    munmap(map, 64 * 1024);
    close(fd);
}
```

## 5. 错误处理

### 内核模块

- 设备注册失败 → 清理并返回错误码
- fault handler 内存分配失败 → 返回 `VM_FAULT_OOM`
- ublk IO 请求超时 → 重试 3 次，失败后返回 `VM_FAULT_SIGBUS`
- workqueue 调度失败 → 记录日志，返回错误

### ublk server

- ublk-control 打开失败 → 检查权限和内核版本（ublk 需要 6.0+）
- io_uring 初始化失败 → 使用 fallback poll 模式
- IO 处理失败 → 记录错误，继续处理其他请求

### 测试程序

- 字符设备不存在 → 提示加载内核模块
- mmap 失败 → 检查设备权限
- 数据验证失败 → 打印期望值与实际值对比

## 6. 测试计划

### 基础功能测试

1. 加载内核模块 → 验证 `/dev/ublk_char_demo` 创建成功
2. 启动 ublk server → 醯证 `/dev/ublkb0` 创建成功
3. 运行测试程序 → 验证 mmap 和数据访问正常
4. 检查日志输出 → 验证缺页触发和数据获取流程

### 边界测试

- 访问超出设备容量的地址 → 应返回错误或截断
- 连续访问多个块 → 验证缓存和重复缺页处理
- 并发访问（多线程） → 验证并发安全性

### 清理测试

- 卸载内核模块 → 验证设备清理
- 停止 ublk server → 验证 ublk 设备删除

## 7. 系统要求

- **Linux 内核：** 6.0+ （ublk 框架支持）
- **权限：** root（创建 ublk 设备需要）
- **依赖：** liburing（io_uring 库）或内核原生 io_uring 支持

## 8. 实现要点

**Why:** 这个 demo 展示了用户态块设备 (ublk) 与内存映射 (mmap) 结合的完整流程，核心价值在于演示缺页处理时如何从用户态 server 动态获取数据。

**How to apply:** 实现时注意以下关键点：
- fault handler 不能阻塞，必须使用 workqueue 等异步机制
- ublk server 使用 io_uring 与内核通信，需要正确处理共享内存布局
- 测试程序访问不同偏移验证缺页触发和数据获取的正确性