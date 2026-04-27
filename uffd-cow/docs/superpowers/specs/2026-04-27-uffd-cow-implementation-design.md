# UFFD + shmem + COW Demo 实现设计文档

## 1. 目标

完整实现设计文档中的功能，验证 UFFD + shmem + COW 机制的可行性。

**核心功能：**
1. 多个独立进程（非父子关系）共享同一份只读内存（shmem）
2. 根据缺页信息，从指定文件加载页数据
3. 写入时生成私有页（COW），保证原始共享页不被修改

## 2. 程序结构

```
uffd-cow/
├── main.c          # 主程序入口，命令行参数处理
├── shmem.c         # 共享内存创建和映射
├── uffd_handler.c  # UFFD 事件处理线程
├── page_meta.c     # 页元数据管理
├── test_data.c     # 测试文件生成和读取
├── Makefile        # 构建脚本
└── design.md       # 已有的设计文档
└── docs/superpowers/specs/  # 实现设计文档
```

## 3. 运行方式

**单程序多角色模式：**

```bash
# Creator 进程（创建 shmem 和测试文件，打印共享路径）
./uffd-cow --create

# Worker 进程（加入共享区域，处理 UFFD）
./uffd-cow --join /proc/<creator_pid>/fd/<fd>
```

Creator 进程职责：
1. 创建 memfd 和测试文件
2. 打印 `/proc/self/fd/<fd>` 路径供其他进程使用
3. mmap 共享区域并注册 UFFD
4. 启动 UFFD handler 线程
5. 等待用户输入退出

Worker 进程职责：
1. 通过路径打开已有 memfd
2. mmap 共享区域并注册 UFFD
3. 启动 UFFD handler 线程
4. 进行读取和写入操作演示 COW

## 4. 数据结构

```c
// 页状态
enum page_state {
    PAGE_UNLOADED,  // 未加载，需要从文件读取
    PAGE_LOADED,    // 已加载到 shmem，共享只读
    PAGE_PRIVATE    // 已 COW，进程私有可写
};

// 页元数据（共享区域）
struct page_meta {
    uint64_t file_offset;   // 文件偏移
    enum page_state state;  // 当前状态
};

// 全局配置
struct config {
    size_t region_size;     // 共享区域大小（16KB）
    size_t page_count;      // 页数量（4）
    char *shmem_path;       // shmem fd 路径（creator 为空，worker 从参数获取）
    int shmem_fd;           // memfd 文件描述符
    void *base_addr;        // mmap 基地址
    int uffd;               // UFFD 文件描述符
    struct page_meta *pages; // 页元数据数组指针
    char *test_file_path;   // 测试文件路径
};
```

**常量定义：**
```c
#define PAGE_SIZE       4096
#define PAGE_COUNT      4
#define REGION_SIZE     (PAGE_SIZE * PAGE_COUNT)
#define TEST_FILE_SIZE  REGION_SIZE
```

## 5. 共享区域布局

**双 mmap 区域方案（元数据独立）：**

```c
// 1. meta_region: 存放 page_meta 数组，PROT_READ | PROT_WRITE
// 2. data_region: 存放数据页，PROT_READ，触发 UFFD

// meta_fd: memfd_create("uffd-cow-meta", ...)
// data_fd: memfd_create("uffd-cow-data", ...)

// meta_region 大小：PAGE_SIZE（足够存放 PAGE_COUNT 个 page_meta）
// data_region 大小：REGION_SIZE
```

布局示意：
```
[meta_region - 4KB]
+-------------------+
| page_meta[0]      |
| page_meta[1]      |
| page_meta[2]      |
| page_meta[3]      |
+-------------------+

[data_region - 16KB]
+-------------------+
| 页 0 (数据)       |
| 页 1 (数据)       |
| 页 2 (数据)       |
| 页 3 (数据)       |
+-------------------+
```

## 6. 核心流程

### 6.1 Creator 进程流程

```
1. 创建 meta_fd = memfd_create("uffd-cow-meta", 0)
2. ftruncate(meta_fd, PAGE_SIZE)
3. 创建 data_fd = memfd_create("uffd-cow-data", 0)
4. ftruncate(data_fd, REGION_SIZE)
5. 创建测试文件 /tmp/uffd-cow-test.bin
6. mmap(meta_fd, PROT_READ|PROT_WRITE, MAP_SHARED) → 初始化 page_meta 数组
7. mmap(data_fd, PROT_READ, MAP_SHARED) → data_base_addr
8. 创建 UFFD，注册 data_region 的 MISSING | WP 事件
9. 启动 UFFD handler 线程
10. 打印共享路径: /proc/self/fd/<meta_fd> 和 /proc/self/fd/<data_fd>
11. 等待用户输入退出
12. 清理资源
```

### 6.2 Worker 进程流程

```
1. 从参数获取 meta_path 和 data_path
2. 打开 meta_path → mmap 获取 page_meta 数组指针
3. 打开 data_path → mmap 获取 data_base_addr
4. 创建 UFFD，注册 data_region 的 MISSING | WP 事件
5. 启动 UFFD handler 线程
6. 执行演示操作：
   a. 读取页 0 → 触发缺页
   b. 读取页 1 → 触发缺页
   c. 写入页 0 → 触发写保护 (COW)
   d. 再次读取页 0 → 显示修改后的值
7. 清理资源
```

### 6.3 UFFD Handler 流程

```c
while (running) {
    read(uffd, &event);  // 阻塞等待事件

    if (event.type == MISSING) {
        page_idx = (event.addr - data_base) / PAGE_SIZE;
        if (pages[page_idx].state == PAGE_UNLOADED) {
            从测试文件 offset 读取 PAGE_SIZE 数据到 buffer
            struct uffdio_copy copy = {
                .dst = page_addr,
                .src = buffer,
                .len = PAGE_SIZE,
                .mode = 0
            };
            ioctl(uffd, UFFDIO_COPY, &copy);
            pages[page_idx].state = PAGE_LOADED;
        }
    }

    if (event.type == WP) {
        page_idx = (event.addr - data_base) / PAGE_SIZE;
        分配匿名私有页 private_page = mmap(..., MAP_PRIVATE|MAP_ANONYMOUS, ...);
        memcpy(private_page, shared_page, PAGE_SIZE);
        mmap(page_addr, PAGE_SIZE, PROT_READ|PROT_WRITE,
             MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        pages[page_idx].state = PAGE_PRIVATE;
    }
}
```

## 7. 测试文件内容

每页填充不同的字节模式：
- 页 0：全 'A' (0x41)
- 页 1：全 'B' (0x42)
- 页 2：全 'C' (0x43)
- 页 3：全 'D' (0x44)

便于验证读取正确性，以及写入后区分修改值。

## 8. 错误处理

- `memfd_create` 失败 → perror 并退出
- `ftruncate` 失败 → perror 并退出
- `mmap` 失败 → perror 并退出
- `UFFDIO_REGISTER` 失败 → perror 并退出
- 测试文件读取失败 → perror，handler 返回错误
- COW mmap 失败 → perror，进程终止

## 9. 边界情况

- **重复缺页**：handler 检查状态，若已 LOADED 则忽略
- **COW 后再次写入**：页已是 PRIVATE，不再触发 WP
- **Worker 先于 Creator 启动**：打开路径失败，打印错误退出

## 10. 构建配置

```makefile
CC = gcc
CFLAGS = -Wall -Wextra -g -O0
LDFLAGS = -pthread

TARGET = uffd-cow
SRCS = main.c shmem.c uffd_handler.c page_meta.c test_data.c

all: $(TARGET)

$(TARGET): $(SRCS)
    $(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

clean:
    rm -f $(TARGET)
```

**内核要求：Linux 4.3+ 支持 UFFD，Linux 5.7+ 支持 WP**

## 11. 预期输出示例

```
# Creator
Created shmem:
  meta: /proc/self/fd/3
  data: /proc/self/fd/4
Test file: /tmp/uffd-cow-test.bin
Waiting for workers... Press Enter to exit

# Worker 1
[Worker 1] Read page 0: 'AAAAAAAA...' (size=4096)
[Worker 1] Read page 1: 'BBBBBBBB...' (size=4096)
[Worker 1] Write page 0: changing to 'XXXXXXXX...'
[Worker 1] Read page 0 after write: 'XXXXXXXX...' (COW page)

# Worker 2
[Worker 2] Read page 0: 'AAAAAAAA...' (original shared page)
```

这清晰展示了 COW 效果：Worker 1 写入后看到修改值，Worker 2 仍看到原始共享页。

## 12. 进程间协调

采用设计文档的简单策略：不做锁或 futex，允许短暂重复加载。

- 缺页时检查 page_meta 状态，若已 LOADED 则跳过加载
- page_meta 位于独立共享区域，各进程可直接读写
- 状态变更：UNLOADED → LOADED → PRIVATE（单向，无回退）