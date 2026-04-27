# 最小功能实现设计文档（UFFD + shmem + COW）

## 1. 目标
1. 多个进程共享同一份只读内存（shmem）
2. 根据缺页信息，从指定文件加载页数据
3. 写入时生成私有页（COW），保证原始共享页不被修改

## 2. 系统组件
| 组件                        | 功能                                                           |
| ------------------------- | ------------------------------------------------------------ |
| **Shared Memory** (shmem) | 用于存放共享只读基页，所有进程 `mmap` 映射                                    |
| **Userfaultfd (UFFD)**    | 捕获缺页（missing）和写保护（WP）fault                                   |
| **Fault Handler**         | 用户态处理 UFFD 事件，负责：<br>- 缺页时读取文件填充 shmem<br>- 写保护 fault 时生成私有页 |
| **Metadata Table**        | 记录每页映射到哪个文件/offset，及当前状态（未加载/已加载）                            |

## 3. 内存布局

```
[共享 shmem 区域]
+-------------------+
| 页 0              |
| 页 1              |
| ...               |
+-------------------+

每个进程私有页（COW）：
+-------------------+
| 页 i (私有 RW)    |
+-------------------+
```

- 初始状态：shmem 区域全只读，写保护开启
- 缺页：根据 metadata 从文件加载数据到 shmem 页
- 写入：替换 fault 地址映射为新私有页

## 4. 数据结构

```c
struct page_meta {
    uint64_t file_id;     // 对应文件
    uint64_t file_offset; // 文件偏移
    enum { UNLOADED, LOADED } state;
};
```

- 每页一个 page_meta
- 可以用数组索引虚拟地址对应页

## 5. 初始化流程

1. 创建共享内存：
```c
int fd = memfd_create("shared", 0);
ftruncate(fd, REGION_SIZE);
```
2. 所有进程 mmap：
```c
void* base = mmap(NULL, REGION_SIZE, PROT_READ, MAP_SHARED, fd, 0);
```
3. 注册 UFFD：
```c
int uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
ioctl(uffd, UFFDIO_API, ...);
ioctl(uffd, UFFDIO_REGISTER, {base, REGION_SIZE, MISSING | WP});
```
4. 启用写保护：
```c
ioctl(uffd, UFFDIO_WRITEPROTECT, {base, REGION_SIZE, WP});
```

## 6. Fault 处理
### 6.1 缺页 (Missing)

1. 计算页索引 page_idx = (addr - base)/PAGE_SIZE
2. 根据 metadata 获取文件和 offset
3. 从文件读取 PAGE_SIZE 数据到 shmem 页
4. 使用 UFFDIO_COPY 填充页
5. 更新 metadata 状态为 LOADED

### 6.2 写保护 (COW)
1. 分配新的匿名页：mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
2. memcpy 原 shmem 页内容到新页
3. mmap(MAP_FIXED | MAP_PRIVATE) 覆盖 fault 地址为新页
4. 取消写保护，页可写

## 7. 简单状态机
```
UNLOADED --(missing fault)--> LOADED (shmem, RO)
LOADED --(write fault)--> PRIVATE (per-process, RW)
```

8. 多进程同步

- 缺页时多个进程可能同时触发
- 简单策略（最小功能）：
```
if (state == UNLOADED) {
    load file -> shmem
    state = LOADED;
} else {
    // 等待或直接返回，shmem 已经加载
}
```
- 不做锁或 futex，允许短暂重复加载（功能可用，性能低）

## 9. 限制
- 性能低：缺页时每个进程可能重复加载相同页
- 线程不安全：如果多个线程共享同一进程 uffd，需额外同步
- 写入 COW 时总会 memcpy：无法零拷贝
