# Agent安全沙箱内存快照方案设计（驱动版）

## 概述

基于内核驱动实现 microVM 安全沙箱物理内存共享，**完全不修改内核代码**，通过驱动 mmap + fault handler 实现跨 VM 物理页复用。

## 设计目标

| 目标 | 优先级 |
|------|--------|
| 内存效率 - 多沙箱共享物理内存 | 首要目标 |
| 零内核修改 - 仅使用驱动模块 | 约束条件 |
| 恢复速度 - 快照快速启动 | 辅助目标 |

## 适用场景

同构沙箱：从同一快照模板批量启动大量相同配置/相同应用的沙箱实例，内存高度相似，去重收益大。

## 系统架构

### 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                     Host Linux Server                        │
│  ┌───────────────────────────────────────────────────────┐  │
│  │                  Snapshot Driver                       │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐    │  │
│  │  │ Template A  │  │ Template B  │  │ Template C  │    │  │
│  │  │ Device File │  │ Device File │  │ Device File │    │  │
│  │  │ /dev/snap_A │  │ /dev/snap_B │  │ /dev/snap_C │    │  │
│  │  └─────────────┘  └─────────────┘  └─────────────┘    │  │
│  │        │                │                │            │  │
│  │  ┌─────┴─────┐    ┌─────┴─────┐    ┌─────┴─────┐      │  │
│  │  │ RO Page   │    │ RO Page   │    │ RO Page   │      │  │
│  │  │ Pool      │    │ Pool      │    │ Pool Pool │      │  │
│  │  └───────────┘    └───────────┘    └───────────┘      │  │
│  └───────────────────────────────────────────────────────┘  │
│         mmap                mmap                mmap         │
│    ┌─────────┐         ┌─────────┐         ┌─────────┐      │
│    │  VM 1   │         │  VM 2   │         │  VM 3   │      │
│    │ Cloud-  │         │ Cloud-  │         │ Cloud-  │      │
│    │Hypervisor│        │Hypervisor│        │Hypervisor│      │
│    └────┬────┘         └────┬────┘         └────┬────┘      │
│         │                   │                   │          │
│    ┌────┴────┐          ┌────┴────┐          ┌────┴────┐    │
│    │Guest Mem│          │Guest Mem│          │Guest Mem│    │
│    │(COW RO) │          │(COW RO) │          │(COW RO) │    │
│    └─────────┘          └─────────┘          └─────────┘    │
│                                                              │
│  ┌──────────────┐                                          │
│  │  Local Cache │                                          │
│  │  Files       │                                          │
│  └──────────────┘                                          │
└─────────────────────────────────────────────────────────────┘
```

### 核心组件

| 组件 | 职责 |
|------|------|
| **Snapshot Driver** | 内核驱动，管理模板设备文件、RO 页池、缺页处理 |
| **Template Device** | 每模板一个设备文件，mmap 提供 Guest 内存映射 |
| **RO Page Pool** | 驱动维护全局共享物理页池，按 hash_idx 组织 |
| **切分工具** | 用户态工具，将完整快照切分为 page_table.bin + pages.bin |
| **Cloud Hypervisor** | mmap 模板设备文件作为 Guest 内存后端 |

### 关键设计决策

| 决策 | 理由 |
|------|------|
| 每模板独立设备文件 | Cloud Hypervisor 直接使用文件路径，无需适配 |
| 驱动 `vm_ops.fault` 接管缺页 | 实现按需加载 + 物理页复用 |
| RO 页 + 内核标准 COW | 写入自动触发 COW，各 VM 独占修改页 |
| 驱动直接读取本地文件 | 简化实现，分布式存储由 conch 预下载到本地 |
| 延迟加载 page_table | 按需从文件读取，不预加载到内存，节省内存 |

## 快照制作流程

### 流程概览

```
┌─────────────┐    ┌───────────┐    ┌─────────┐
│Cloud-       │    │  切分工具  │    │ Driver  │
│Hypervisor   │───►│ (split)   │───►│ ioctl   │
└─────────────┘    └───────────┘    └─────────┘
       │                │               │
       │ 1.full snapshot│               │
       │───────────────►│               │
       │                │ 2.计算hash    │
       │                │  + 开放寻址   │
       │                │  写入稀疏文件 │
       │                │──────────────►│
       │                │               │ 3.create template
```

### 详细步骤

| 步骤 | 执行者 | 操作 |
|------|--------|------|
| 1 | Cloud Hypervisor | 生成完整内存快照文件（如 `snapshot.mem`） |
| 2 | 切分工具 | 读取完整快照，按 4KB 切分，计算 hash_idx，写入稀疏文件 |
| 3 | conch | ioctl 调用驱动创建模板设备文件 `/dev/snapshot_{id}` |

### 切分工具详细逻辑

| 步骤 | 操作 |
|------|------|
| 1 | 读取完整快照 `snapshot.mem` |
| 2 | 逐页计算 hash_idx（SHA256 前 8 字节） |
| 3 | 计算文件索引：`file_index = hash_idx % N`（N = 2^30） |
| 4 | 冲突处理：线性探测（最多 64 次），找到空槽或匹配槽 |
| 5 | 写入稀疏文件：`pages.bin` 的对应偏移 |
| 6 | 生成 `page_table.bin`：VA → hash_idx 映射 |
| 7 | 生成 `metadata.json`：模板配置信息 |

### 文件定位算法

```c
// 文件索引计算（开放寻址）
uint64_t file_index = hash_idx % N;  // N = 2^30
uint32_t probe_count = 0;
while (file[file_index].hash_idx != hash_idx && 
       file[file_index].occupied && 
       probe_count < MAX_PROBE_COUNT) {
    file_index = (file_index + 1) % N;
    probe_count++;
}
file_offset = file_index * (PAGE_SIZE + 8);  // 每槽位 4KB + 8B header
```

### 输出文件结构

```
/cache/snap_001/
  ├── metadata.json      # 模板配置
  ├── page_table.bin     # VA → hash_idx 映射
  └── pages.bin          # 稀疏文件，每槽位 4KB + 8B header
```

**page_table_entry 结构（16 bytes）**：
```c
struct page_table_entry {
  uint64_t va_offset;    // 虚拟地址偏移（8 bytes）
  uint64_t hash_idx;     // 页内容哈希（8 bytes）
};
```
条目按 va_offset 顺序存储，驱动可通过 `offset = (va / PAGE_SIZE) * 16` 直接定位。

**metadata.json 示例**：
```json
{
  "template_id": "snap_001",
  "total_size": 134217728,
  "page_count": 32768,
  "hash_modulus": 1073741824
}
```

**pages.bin 槽位结构**：
```
每槽位（4KB + 8B）:
  ┌─────────────────────────────────┐
  │ hash_idx (8 bytes)              │  header：存储实际 hash_idx
  │ page_data (4096 bytes)          │  页内容
  └─────────────────────────────────┘
```

## 快照恢复流程

### 流程概览

```
┌─────────┐    ┌──────────────────┐    ┌─────────┐    ┌─────────┐
│  conch  │───►│ Cloud Hypervisor │───►│ Driver  │───►│ KVM     │
│         │    │                  │    │         │    │         │
└─────────┘    └──────────────────┘    └─────────┘    └─────────┘
     │                   │                  │            │
     │ 1.准备本地缓存    │                  │            │
     │─────────────────►│                  │            │
     │                   │                  │            │
     │ 2.ioctl create    │                  │            │
     │────────────────────────────────────►│            │
     │                   │                  │            │
     │                   │ 3.open设备文件   │            │
     │                   │ 4.mmap          │            │
     │                   │────────────────►│            │
     │                   │                  │            │
     │                   │ 6.配置Guest内存  │            │
     │                   │─────────────────────────────►│
     │                   │                  │            │
     │                   │ 7.vCPU启动       │            │
     │                   │─────────────────────────────►│
     │                   │                  │            │
     │                   │                  │ 8.缺页处理│
     │                   │                  │ (fault)   │
```

### 详细步骤

| 步骤 | 执行者 | 操作 |
|------|--------|------|
| 1 | conch | 检查本地缓存，若无则从快照服务器下载模板文件 |
| 2 | conch | ioctl 调用驱动创建模板设备文件 `/dev/snapshot_{id}` |
| 3 | Cloud Hypervisor | `open("/dev/snapshot_{id}")` 获取文件描述符 |
| 4 | Cloud Hypervisor | `mmap(fd, size)` 获取 Guest 内存映射地址 |
| 5 | 驱动 | mmap 时注册 `vm_ops.fault` 处理函数 |
| 6 | Cloud Hypervisor | 配置 KVM 内存槽：`userspace_addr = mmap_addr` |
| 7 | Cloud Hypervisor | 启动 vCPU，Guest 开始执行 |
| 8 | 驱动 | 缺页时 fault handler 处理，映射 RO 页 |

### 驱动缺页处理详细流程

```
fault_handler(vmf):
    1. 获取缺页的 va_offset
    2. 计算 page_table.bin 文件偏移:
       pt_offset = (va_offset / PAGE_SIZE) * sizeof(page_table_entry)
    3. 从 page_table_path 读取 page_table_entry，获取 hash_idx
    
    4. 从 global_page_pool 查询: hash_idx → phys_page
       
       IF phys_page 存在:
           // 命中：复用已有物理页
           映射 va → phys_page (RO)
           增加 phys_page 引用计数
           记录映射到 vma_data->va_to_pa_map
           返回 VM_FAULT_NOPAGE
       
       ELSE:
           // 未命中：从文件加载
           计算 file_index = hash_idx % N
           线性探测（最多64次）找到匹配槽位
           file_offset = file_index * (PAGE_SIZE + 8)
           
           从 pages_path 的 file_offset 位置：
               - 读取 8B header，验证 hash_idx 匹配
               - 读取 4KB 页数据
           分配新物理页，填充数据
           加入 global_page_pool: hash_idx → phys_page
           映射 va → phys_page (RO)
           记录映射到 vma_data->va_to_pa_map
           返回 VM_FAULT_NOPAGE
```

### 映射复制机制

当后续 VM mmap 同一模板时，可复制首个 VMA 的已有映射：

```
snapshot_vma_open(vma):
    1. vma_data = vma->vm_private_data
    2. template = vma_data->template
    
    3. IF template->first_vma_data 存在:
           // 复制已有映射到本 VMA
           遍历 template->first_vma_data->va_to_pa_map
           对于每个 (va, phys_page):
               vmf_insert_pfn(vma, va_offset, phys_page, RO)
               增加 phys_page->ref_count
           复制映射到 vma_data->va_to_pa_map
    4. ELSE:
           // 首次创建，初始化空映射
           template->first_vma_data = vma_data
```

### RO 页 + COW 机制

- 驱动映射页时使用 `vmf_insert_pfn(vmf, pfn, RO)`
- 页表项标记为只读
- Guest 写入时触发内核标准 COW：
  1. 内核分配新物理页
  2. 复制内容到新页
  3. 更新页表映射到新页
  4. 新页标记为 RW
  5. 该 VM 独占此修改页

## 驱动数据结构

### 驱动全局状态

```c
struct snapshot_driver_state {
  struct list_head template_list;      // 所有模板链表
  spinlock_t template_lock;
  
  struct global_page_pool page_pool;   // 全局共享物理页池
  
  // 配置参数
  uint64_t hash_modulus;               // N = 2^30
  uint32_t max_probe_count;            // 最大探测次数 = 64
};
```

### 全局物理页池

```c
struct global_page_pool {
  struct hlist_head *buckets;    // hash_idx → phys_page_entry 哈希表
  spinlock_t lock;
  uint64_t total_pages;          // 已加载页总数
};

struct phys_page_entry {
  uint64_t hash_idx;
  struct page *page;             // 内核物理页指针
  atomic_t ref_count;            // 引用计数
  struct hlist_node node;
};
```

### 模板结构

```c
struct snapshot_template {
  char template_id[64];
  uint64_t total_size;
  
  char page_table_path[256];   // page_table.bin 文件路径
  char pages_path[256];        // pages.bin 文件路径
  
  struct vma_snapshot_data *first_vma_data;  // 首个 VMA 数据（映射源头）
  
  struct miscdevice mdev;              // 设备文件
  atomic_t ref_count;                  // mmap 引用计数
  struct list_head list;               // 模板链表
};
```

### VMA 级别数据

```c
// VMA 级别数据（通过 vma->vm_private_data 存储）
struct vma_snapshot_data {
  struct snapshot_template *template;
  struct rb_root va_to_pa_map;      // 本 VMA 的 VA → PA 映射
  bool is_first_vma;                // 是否是此模板的第一个 VMA
};
```

### vm_operations_struct

```c
static vm_fault_t snapshot_fault(struct vm_fault *vmf);
static void snapshot_vma_open(struct vm_area_struct *vma);
static void snapshot_vma_close(struct vm_area_struct *vma);

static const struct vm_operations_struct snapshot_vm_ops = {
  .fault = snapshot_fault,
  .open = snapshot_vma_open,
  .close = snapshot_vma_close,
};
```

## ioctl 接口定义

```c
// 创建模板
struct ioctl_create_template {
  char template_id[64];
  uint64_t total_size;
  char page_table_path[256];
  char pages_path[256];
};
#define IOCTL_CREATE_TEMPLATE _IOW('S', 1, struct ioctl_create_template)

// 删除模板
struct ioctl_delete_template {
  char template_id[64];
};
#define IOCTL_DELETE_TEMPLATE _IOW('S', 2, struct ioctl_delete_template)

// 获取模板状态
struct ioctl_template_status {
  char template_id[64];
  uint64_t mmap_count;         // 当前 mmap 数量
  uint64_t loaded_pages;       // 已加载物理页数
};
#define IOCTL_TEMPLATE_STATUS _IOWR('S', 3, struct ioctl_template_status)
```

## 错误处理

### 错误处理场景

| 场景 | 处理方式 |
|------|----------|
| ioctl 创建模板失败（路径无效） | 返回 `-EINVAL`，conch 检查路径后重试 |
| ioctl 创建模板失败（模板 ID 已存在） | 返回 `-EEXIST`，conch 使用新 ID |
| mmap 超出模板大小 | 返回 `-EINVAL`，VMA 创建失败 |
| 缺页时读取 page_table.bin 失败 | 返回 `VM_FAULT_SIGBUS`，VM 可能异常 |
| 缺页时 pages.bin 探测超过最大次数 | 返回 `VM_FAULT_SIGBUS`，页未找到 |
| 缺页时分配物理页失败（内存不足） | 返回 `VM_FAULT_OOM`，触发内核 OOM |

## 生命周期管理

### 模板生命周期

| 操作 | 引用计数变化 |
|------|--------------|
| ioctl_create_template | template 初始化，ref_count = 0 |
| mmap (vma_open) | ref_count++ |
| munmap (vma_close) | ref_count-- |
| ioctl_delete_template | 仅 ref_count == 0 时允许删除 |

### 物理页生命周期

| 操作 | phys_page 引用计数变化 |
|------|------------------------|
| 缺页处理（新加载） | ref_count = 1（首个 VM） |
| 复制映射到新 VMA | ref_count++（每增加一个 VM） |
| vma_close | ref_count--（每减少一个 VM） |
| ref_count == 0 | 释放物理页，从 global_page_pool 移除 |

## 性能与安全考量

### 性能优化点

| 优化项 | 说明 |
|--------|------|
| **延迟加载 page_table** | 按需从文件读取，不预加载到内存 |
| **映射复制** | 后续 VM 直接复制首个 VMA 的映射，避免重复缺页 |
| **稀疏文件** | pages.bin 仅实际槽位占用磁盘，节省空间 |
| **全局物理页池** | hash_idx 相同的页复用同一物理页 |
| **RO + COW** | 内核标准机制，无需驱动额外处理 |

### 预期性能对比

| 场景 | 第一个 VM | 后续 VM |
|------|-----------|---------|
| 128MB 模板 | ~3000 次缺页处理 | ~0 次缺页（直接复制映射） |
| 启动耗时 | 较长（按需加载） | 极短（映射复制） |
| 内存占用 | 加载所有物理页 | 共享相同物理页 |

### 安全考量

| 安全项 | 处理方式 |
|--------|----------|
| **设备文件权限** | `/dev/snapshot_{id}` 设置权限，仅授权进程可访问 |
| **模板隔离** | 每模板独立设备文件，不同模板不共享 |
| **路径验证** | ioctl 时验证 page_table_path 和 pages_path 存在且可读 |
| **VMA 验证** | mmap 时验证大小不超过 template->total_size |
| **并发安全** | spinlock 保护模板列表，atomic 引用计数 |

## 设计约束

| 约束项 | 说明 |
|--------|------|
| 去重粒度 | 4KB 页（标准 Linux 页大小） |
| 内存页属性 | 全部只读，写入触发 COW |
| 内核改动 | 无，仅新增内核驱动模块 |
| hash_modulus | N = 2^30 |
| 最大探测次数 | 64 次 |
| pages.bin 存储方式 | 稀疏文件 |