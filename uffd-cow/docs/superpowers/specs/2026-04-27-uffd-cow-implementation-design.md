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
./uffd-cow --join --meta /proc/<pid>/fd/<meta_fd> --data /proc/<pid>/fd/<data_fd>
```

Creator 进程职责：
1. 创建 memfd 和测试文件
2. 打印 meta 和 data fd 路径供其他进程使用
3. mmap 共享区域并注册 UFFD
4. 启动 UFFD handler 线程
5. 等待用户输入退出

Worker 进程职责：
1. 通过路径打开 meta_fd 和 data_fd
2. mmap 共享区域并注册 UFFD
3. 启动 UFFD handler 线程
4. 进行读取和写入操作演示 COW

## 4. 数据结构

```c
// 共享页状态（存储在共享 meta_region）
enum shared_page_state {
    PAGE_UNLOADED,  // 未加载，需要从文件读取
    PAGE_LOADED,    // 已加载到 shmem，共享只读
};

// 共享页元数据（存储在共享区域，所有进程可见）
struct shared_page_meta {
    uint64_t file_offset;           // 文件偏移
    enum shared_page_state state;   // 共享状态：UNLOADED 或 LOADED
};

// 进程本地私有页状态（每个进程独立维护）
struct local_page_state {
    bool is_private;    // 本进程是否已 COW 此页
    void *private_addr; // 如果已 COW，私有页的地址（可选）
};

// 全局配置
struct config {
    size_t region_size;     // 共享区域大小（16KB）
    size_t page_count;      // 页数量（4）
    char *meta_path;        // meta fd 路径（creator 为空，worker 从参数获取）
    char *data_path;        // data fd 路径
    int meta_fd;            // meta memfd 文件描述符
    int data_fd;            // data memfd 文件描述符
    void *meta_base;        // meta_region mmap 基地址
    void *data_base;        // data_region mmap 基地址
    int uffd;               // UFFD 文件描述符
    struct shared_page_meta *shared_pages;  // 共享页元数据数组指针（指向 meta_region）
    struct local_page_state *local_pages;   // 本进程私有页状态数组（进程本地 malloc）
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

**状态转换表（明确进程归属）：**

| 状态 | 存储位置 | 作用域 | 含义 |
|------|---------|--------|------|
| UNLOADED | shared_pages (meta_region) | 所有进程 | 页未从文件加载 |
| LOADED | shared_pages (meta_region) | 所有进程 | 页已加载到共享 shmem |
| PRIVATE | local_pages (进程本地) | 单个进程 | 本进程已对此页执行 COW |

状态转换：
- UNLOADED → LOADED：缺页处理时，共享状态变更
- 任何 → PRIVATE：本进程执行 COW，仅影响 local_pages

## 5. 共享区域布局

**双 mmap 区域方案（元数据独立）：**

```c
// 1. meta_region: 存放 shared_page_meta 数组，PROT_READ | PROT_WRITE
// 2. data_region: 存放数据页，PROT_READ | PROT_WRITE，用 UFFDIO_WRITEPROTECT 开启写保护

// meta_fd: memfd_create("uffd-cow-meta", ...)
// data_fd: memfd_create("uffd-cow-data", ...)

// meta_region 大小：PAGE_SIZE（足够存放 PAGE_COUNT 个 shared_page_meta）
// data_region 大小：REGION_SIZE
```

布局示意：
```
[meta_region - 4KB，无 UFFD 监控]
+-------------------+
| shared_page_meta[0] (file_offset, state=UNLOADED/LOADED)
| shared_page_meta[1]
| shared_page_meta[2]
| shared_page_meta[3]
+-------------------+

[data_region - 16KB，UFFD 监控 MISSING | WP]
+-------------------+
| 页 0 (数据)       | ← 写保护开启，写入触发 WP fault
| 页 1 (数据)       |
| 页 2 (数据)       |
| 页 3 (数据)       |
+-------------------+
```

## 6. 核心流程

### 6.1 Runtime Capability Checks

在执行任何 UFFD 操作前，必须检查内核特性支持：

```c
// 1. 创建 UFFD fd
int uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
if (uffd < 0) {
    perror("userfaultfd syscall failed");
    exit(1);  // 内核不支持 UFFD 或权限不足
}

// 2. API 协商
struct uffdio_api api = {
    .api = UFFD_API,
    .features = UFFD_FEATURE_PAGEFAULT_FLAG_WP  // 请求 WP 特性
};
if (ioctl(uffd, UFFDIO_API, &api) < 0) {
    perror("UFFDIO_API failed");
    exit(1);  // 内核不支持 WP 特性
}

// 3. 检查返回特性
if (!(api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP)) {
    fprintf(stderr, "Kernel does not support UFFD write-protect\n");
    exit(1);  // 需要 Linux 5.7+ 才支持 WP
}
```

**内核版本要求：**
- Linux 4.3+：基础 UFFD 支持
- Linux 5.7+：UFFD WP (write-protect) 特性支持

### 6.2 Creator 进程流程

```
1. 创建 meta_fd = memfd_create("uffd-cow-meta", 0)
2. ftruncate(meta_fd, PAGE_SIZE)
3. 创建 data_fd = memfd_create("uffd-cow-data", 0)
4. ftruncate(data_fd, REGION_SIZE)
5. 创建测试文件 /tmp/uffd-cow-test.bin

6. mmap(meta_fd, PROT_READ|PROT_WRITE, MAP_SHARED) → meta_base
7. 初始化 shared_pages 数组（全部设为 UNLOADED）

8. mmap(data_fd, PROT_READ|PROT_WRITE, MAP_SHARED) → data_base
   // 注意：必须用 PROT_READ|PROT_WRITE，否则 WP 无法正常触发

9. 创建 UFFD fd，执行 UFFDIO_API 协商（请求 WP 特性）

10. 注册 UFFD：
    struct uffdio_register reg = {
        .range = { .start = data_base, .len = REGION_SIZE },
        .mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP
    };
    ioctl(uffd, UFFDIO_REGISTER, &reg);

11. 开启写保护：
    struct uffdio_writeprotect wp = {
        .range = { .start = data_base, .len = REGION_SIZE },
        .mode = UFFDIO_WRITEPROTECT_MODE_WP  // 开启 WP
    };
    ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);

12. 初始化 local_pages 数组（malloc，全部设为 is_private=false）

13. 启动 UFFD handler 线程

14. 打印共享路径:
    printf("meta: /proc/self/fd/%d\n", meta_fd);
    printf("data: /proc/self/fd/%d\n", data_fd);

15. 等待用户输入退出

16. 清理资源（munmap, close fd, 删除测试文件）
```

### 6.3 Worker 进程流程

```
1. 从参数获取 meta_path 和 data_path

2. 打开 meta_path → meta_fd
   mmap(meta_fd, PROT_READ|PROT_WRITE, MAP_SHARED) → meta_base
   shared_pages = (struct shared_page_meta *)meta_base

3. 打开 data_path → data_fd
   mmap(data_fd, PROT_READ|PROT_WRITE, MAP_SHARED) → data_base

4. 创建 UFFD fd，执行 UFFDIO_API 协商（请求 WP 特性）

5. 注册 UFFD（同 Creator）：
   UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP

6. 开启写保护（同 Creator）：
   UFFDIO_WRITEPROTECT_MODE_WP

7. 初始化 local_pages 数组（malloc，全部设为 is_private=false）

8. 启动 UFFD handler 线程

9. 执行演示操作：
   a. 读取页 0 → 触发 MISSING fault → handler 加载
   b. 读取页 1 → 触发 MISSING fault → handler 加载
   c. 写入页 0 → 触发 WP fault → handler 执行 COW
   d. 再次读取页 0 → 显示修改后的值（私有页）

10. 清理资源
```

### 6.4 UFFD Handler 流程（详细 syscall 级）

```c
void *uffd_handler_thread(void *arg) {
    struct config *cfg = arg;

    while (running) {
        struct uffd_msg event;
        int n = read(cfg->uffd, &event, sizeof(event));
        if (n <= 0) continue;

        if (event.type == UFFD_MSG_PAGEFAULT) {
            uint64_t fault_addr = event.arg.pagefault.address;
            uint64_t flags = event.arg.pagefault.flags;
            int page_idx = (fault_addr - cfg->data_base) / PAGE_SIZE;
            uint64_t page_addr = cfg->data_base + page_idx * PAGE_SIZE;

            // === MISSING fault 处理 ===
            if (flags & UFFD_PAGEFAULT_FLAG_WRITE) {
                // 这是写入触发的缺页（页未加载）
                // 需要先加载，然后执行 COW
            }

            if (!(flags & UFFD_PAGEFAULT_FLAG_WP)) {
                // 纯 MISSING fault（页未加载）
                // 检查共享状态
                if (cfg->shared_pages[page_idx].state == PAGE_UNLOADED) {
                    // 从文件读取
                    char buffer[PAGE_SIZE];
                    read_from_test_file(cfg->test_file_path,
                                        cfg->shared_pages[page_idx].file_offset,
                                        buffer, PAGE_SIZE);

                    // UFFDIO_COPY 填充页并唤醒
                    struct uffdio_copy copy = {
                        .dst = page_addr,
                        .src = (uint64_t)buffer,
                        .len = PAGE_SIZE,
                        .mode = 0  // 填充后页变为可写（对于写入 fault）
                                   // 或保持写保护（对于读取 fault）
                    };

                    // 如果是写入触发的 MISSING，需要先 COPY 再 WP-unprotect
                    if (flags & UFFD_PAGEFAULT_FLAG_WRITE) {
                        copy.mode = UFFDIO_COPY_MODE_WP;  // 保持写保护
                        ioctl(cfg->uffd, UFFDIO_COPY, &copy);

                        // 然后执行 COW（见下方 WP 处理）
                        goto do_cow;
                    } else {
                        ioctl(cfg->uffd, UFFDIO_COPY, &copy);
                        // 对于读取触发的 MISSING，填充后页保持写保护
                    }

                    // 更新共享状态（允许竞争，重复加载可接受）
                    cfg->shared_pages[page_idx].state = PAGE_LOADED;
                }
                // 如果已是 LOADED，说明其他进程已加载，本进程无需操作
                // faulting thread 会自动重试并命中已加载页
            }

            // === WP fault 处理（COW）===
            if (flags & UFFD_PAGEFAULT_FLAG_WP) {
                // 写保护触发的 fault：本进程尝试写入已加载的共享页
do_cow:
                if (cfg->local_pages[page_idx].is_private) {
                    // 已是私有页，不应再触发 WP
                    // 解除写保护即可
                    struct uffdio_writeprotect wp = {
                        .range = { .start = page_addr, .len = PAGE_SIZE },
                        .mode = 0  // WP = 0 表示解除写保护
                    };
                    ioctl(cfg->uffd, UFFDIO_WRITEPROTECT, &wp);
                } else {
                    // 执行 COW：创建私有页替换共享映射

                    // 方法 A：mmap 新匿名页覆盖 fault 地址
                    void *private_page = mmap(page_addr, PAGE_SIZE,
                                              PROT_READ | PROT_WRITE,
                                              MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                                              -1, 0);
                    if (private_page == MAP_FAILED) {
                        perror("COW mmap failed");
                        // 发送错误响应
                        struct uffdio_copy err_copy = {
                            .dst = page_addr,
                            .src = 0,
                            .len = PAGE_SIZE,
                            .mode = UFFDIO_COPY_MODE_DONTWAKE
                        };
                        ioctl(cfg->uffd, UFFDIO_COPY, &err_copy);
                        continue;
                    }

                    // 复制原共享页内容到私有页
                    // 注意：此时共享页内容已在 shmem 中
                    memcpy(private_page, (void *)page_addr, PAGE_SIZE);

                    // 标记本进程此页为私有
                    cfg->local_pages[page_idx].is_private = true;
                    cfg->local_pages[page_idx].private_addr = private_page;

                    // mmap(MAP_FIXED) 已自动解决了 fault
                    // 无需额外的 wake 操作
                }
            }
        }
    }
    return NULL;
}
```

**关键细节说明：**

1. **MISSING fault + WRITE flag**：页未加载时写入，需要先加载再 COW
2. **WP fault**：页已加载但写保护触发，执行 COW
3. **COW 实现**：使用 `mmap(MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS)` 覆盖 fault 地址，新页自动继承 fault 地址
4. **mmap vs UFFDIO_COPY**：对于 COW，直接 mmap 覆盖比 UFFDIO_COPY 更简单，因为新页是进程私有的

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
- `syscall(__NR_userfaultfd)` 失败 → perror 并退出（内核不支持）
- `UFFDIO_API` 失败或特性不支持 → 打印错误并退出
- `UFFDIO_REGISTER` 失败 → perror 并退出
- `UFFDIO_WRITEPROTECT` 失败 → perror 并退出
- 测试文件读取失败 → perror，handler 发送错误响应
- COW mmap 失败 → perror，handler 发送错误响应并继续

## 9. 边界情况

- **重复缺页**：检查 shared_pages 状态，若已 LOADED 则无需加载，允许竞争导致重复加载（可接受）
- **COW 后再次写入**：local_pages 已标记 PRIVATE，解除写保护即可
- **Worker 先于 Creator 启动**：打开路径失败，打印错误退出
- **Creator 退出**：Worker 的 fd 可能失效，后续操作失败退出
- **并发同页 fault**：多个进程同时触发同一页缺页，允许多次加载，最终状态一致

**负面测试矩阵：**

| 场景 | 预期行为 |
|------|---------|
| 内核不支持 WP | UFFDIO_API 检查失败，打印错误退出 |
| Worker 先启动 | 打开路径失败，退出 |
| Creator 退出后 Worker 操作 | fd 失效，后续 mmap/fault 失败 |
| 同页并发 fault | 允许重复加载，最终一致 |

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

## 11. 预期输出示例

```
# Creator
Created shmem:
  meta: /proc/self/fd/3
  data: /proc/self/fd/4
Test file: /tmp/uffd-cow-test.bin
Waiting for workers... Press Enter to exit

# Worker 1
[Worker 1] Starting...
[Worker 1] Read page 0: 'AAAAAAAA...' (size=4096) [MISSING fault handled]
[Worker 1] Read page 1: 'BBBBBBBB...' (size=4096) [MISSING fault handled]
[Worker 1] Write page 0: changing to 'XXXXXXXX...' [WP fault -> COW]
[Worker 1] Read page 0 after write: 'XXXXXXXX...' (private COW page)

# Worker 2
[Worker 2] Starting...
[Worker 2] Read page 0: 'AAAAAAAA...' (original shared page, loaded by Worker 1)
[Worker 2] Read page 1: 'BBBBBBBB...' (shared page)
[Worker 2] Write page 1: changing to 'YYYYYYYY...' [WP fault -> COW]
[Worker 2] Read page 1 after write: 'YYYYYYYY...' (private COW page)
```

这清晰展示了 COW 效果：
- Worker 1 写入页 0 后看到修改值（私有页）
- Worker 2 读取页 0 仍看到原始值（共享页未变）
- 各进程的 COW 状态独立（local_pages）

## 12. 进程间协调

采用简单策略：不做锁或原子操作，允许短暂重复加载。

- **shared_pages 状态**：UNLOADED → LOADED 转换可能存在竞争，允许多进程同时加载同一页
- **local_pages 状态**：每个进程独立维护，无竞争
- **原子性说明**：本 demo 为功能验证，允许数据竞争；生产环境需使用原子操作或锁

```
共享状态竞争示例：
进程 A: 缺页 → 检查 UNLOADED → 开始加载
进程 B: 缺页 → 检查 UNLOADED → 开始加载
进程 A: UFFDIO_COPY → 设为 LOADED
进程 B: UFFDIO_COPY → 设为 LOADED（重复，可接受）

最终结果：页已加载，状态为 LOADED
```