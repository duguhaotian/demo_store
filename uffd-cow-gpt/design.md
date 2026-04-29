# 用户态 Template Memory System 设计

## 目标

本实现把 `prompt.txt` 中的模型落成一个可运行的 Rust 原型：多个进程共享同一份只读 template 页，本地进程负责 userfaultfd、写时复制和 dirty tracking。Template 侧只描述“页是否已经从 backend 加载到共享 memfd”，不记录任何进程私有状态。

代码组织上，核心能力放在 `src/lib.rs`，demo 编排放在 `src/main.rs`。后续集成 cloud-hypervisor 时，VMM 侧应依赖库 API，而不是依赖 demo binary。

## 组件边界

### TemplateService

- 创建并持有 `memfd`，作为所有 client 共享的 shmem page cache。
- 创建共享 `template_page_meta`，每页包含：
  - `state`: `UNLOADED / LOADING / SHARED_READY`
  - `backend_offset`
- 持有多层只读 backend overlay，并提供 `read_page(page_idx)`。
- 不注册 UFFD，不处理 COW，不记录 dirty。

### Client

- 映射 TemplateService 的 `memfd`。
- 在本进程注册 userfaultfd，模式为 `MISSING | WP`。
- 本地处理：
  - MISSING fault：通过 CAS 抢占 loader，读取 backend，填入 shmem。
  - WP fault：复制共享页到匿名私有页，并用 `MAP_FIXED` 替换当前页映射。
- 持有私有 `proc_page_meta`：
  - `is_private`
  - `dirty`

## Library API 边界

`src/lib.rs` 暴露的核心类型：

- `OverlayBackend`: 多层只读 backend，按 page bitmap 从顶层到底层定位真实页。
- `Layer`: backend 的一个只读层，包含文件和该层 page bitmap。
- `TemplateService`: 持有共享 `memfd`、共享 `template_page_meta` 和 backend。
- `TemplateMapping`: client 进程映射到的 template shmem 区域。
- `Client`: 本进程 UFFD handler、COW 和 per-process dirty tracking。
- `Checkpoint`: client 导出的 dirty pages、private bitmap 和 merged backend bitmap。

cloud-hypervisor 集成时的预期调用形态：

```rust
let backend = OverlayBackend::new(page_size, page_count, layers)?;
let template = TemplateService::new(guest_ram_size, backend)?;
let mapping = template.map_client_region()?;

let mut client = Client::new(&template, mapping.as_mut_ptr())?;
client.start_handler();

// VM 运行或 restore 期间按需触发 UFFD fault。

let ckpt = client.export_checkpoint();
client.stop_handler()?;
```

如果 cloud-hypervisor 已经通过 `GuestMemoryMmap` 管理 guest RAM，则 `Client::new()` 的 `base` 可以替换为对应 region 的 host address；`TemplateMapping` 只是本原型用于创建 shmem 映射的 RAII 包装。

## Backend Overlay

backend 是多层只读文件。每一层有一个页 bitmap，表示该层覆盖哪些 page。读取某页时从顶层到基底层查找第一个命中的 layer，然后按 `backend_offset` 定位真实文件偏移。

原型中每层文件使用相同 page index 到 file offset 的映射，即：

```text
file_offset = page_idx * page_size
```

但接口已经把定位逻辑封装在 overlay 内，后续可以替换为压缩块、稀疏块或外部索引。

## Fault 流程

### RO 缺页

```text
MISSING fault
  page_idx = (fault_addr - base) / page_size
  CAS template_meta[page_idx].state: UNLOADED -> LOADING
    success:
      backend.read_page(page_idx)
      UFFDIO_COPY(..., UFFDIO_COPY_MODE_WP)
      state = SHARED_READY
    fail:
      spin/yield 等待 SHARED_READY
      UFFDIO_WAKE
```

Rust 原型使用 `UFFDIO_COPY_MODE_WP` 直接把页填入 fault 地址并保持写保护。生产设计若要严格做到读路径 0 copy，可以把 backend 先填入 memfd page cache，再切到 `MINOR_SHMEM + UFFDIO_CONTINUE` 路径。

### 写时复制

```text
WP fault
  snapshot = memcpy(shared_page)
  clear UFFD WP without wake
  mmap(MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS) 覆盖该页
  memcpy(snapshot -> private_page)
  mark proc_meta[page_idx].is_private = true
  mark proc_meta[page_idx].dirty = true
  UFFDIO_WAKE
```

COW 始终在 fault 所在进程执行，因此不会跨进程分配或安装页。

## Checkpoint

checkpoint 只扫描 `proc_page_meta`：

- `dirty == true` 的页被导出为 `(page_idx, page_data)`。
- 同时生成：
  - 当前进程私有 dirty bitmap。
  - backend overlay merged bitmap，用于恢复时快速定位底层页。

TemplateService 不参与 checkpoint 内容生成，只作为共享基底页来源。

## 与 cloud-hypervisor 的集成方向

### 现有 restore UFFD 路径

cloud-hypervisor 当前的 on-demand restore 在 `vmm/src/memory_manager.rs` 中完成：

1. `MemoryManager::new_from_snapshot()` 根据 `RestoreConfig.memory_restore_mode` 选择 copy 或 on-demand。
2. on-demand 模式进入 `restore_by_uffd()`。
3. `restore_by_uffd()` 打开 `memory-ranges` snapshot 文件，创建 `userfaultfd`，将每个 saved memory range 的 GPA 翻译成 host address。
4. 对每个 host range 调用 `UFFDIO_REGISTER(MISSING)`。
5. handler 线程 epoll `stop_event + uffd_fd`。
6. 收到 missing fault 后，按 `range.file_offset + (page_addr - range.host_addr)` 定位 snapshot 文件偏移。
7. 从 snapshot 文件读一个 page，再通过 `UFFDIO_COPY` 填回 fault address。

这个路径已经具备“client 本地 UFFD handler”的关键语义，但 page source、page metadata 和 fault handler 都嵌在 `MemoryManager` 里，不利于扩展 template backend overlay、跨进程共享 template metadata、COW 和 checkpoint dirty layer。

### 已完成的第一阶段集成

第一阶段把 restore client 逻辑抽成 `cloud-hypervisor/vmm/src/template_memory.rs`：

- `TemplateRange`: 描述一个 guest memory host range 到 backend offset 的映射。
- `TemplateRestoreBackend`: 持有只读 snapshot layer、range table 和 per-page `TemplatePageMeta`。
- `TemplatePageMeta`: 记录 `UNLOADED / LOADING / SHARED_READY` 和 `backend_offset`。
- `handler_loop()`: 独立 UFFD restore handler，负责 fault 地址定位、CAS page state、backend read、`UFFDIO_COPY` 和 `UFFDIO_WAKE`。

`MemoryManager::restore_by_uffd()` 现在只保留 cloud-hypervisor 相关的工作：

- GPA 到 host address 的转换。
- UFFD fd 创建和 range 注册。
- 基于 memory zone 推导 page size。
- 启动/停止 handler 线程。

这保持了当前 snapshot 格式和 on-demand restore 行为不变，但把“client 侧 template restore”变成独立模块。

### 后续演进

1. 将 `TemplateRestoreBackend` 从单层 `memory-ranges` 文件扩展为多层只读 overlay。
2. 将 `TemplatePageMeta` 放到共享 memfd 或可传递的共享 metadata 区域。
3. 将 WP/COW dirty page state 导出为 checkpoint dirty layer。
4. checkpoint 时扫描 dirty layer，并把 dirty layer 作为下一次 restore overlay 顶层。

当前 cloud-hypervisor 集成已经注册 `MISSING | WP`。missing fault 从 snapshot/template backend
读取页面，并通过 `UFFDIO_COPY_MODE_WP` 安装为写保护页；write-protect fault 会以 1MiB chunk
为粒度执行 COW：先主动补齐 chunk 内尚未加载的页，避免 handler 读取未加载页时自锁，再复制
chunk 内容，解除 UFFD 写保护，然后用 `mmap(MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS)` 在同一
host address 替换为私有匿名映射并标记 dirty。这样会牺牲最多 1MiB 粒度的复用率，但能显著
减少 4KiB 单页 COW 导致的 VMA 碎片。发生 WP/COW 后，`/proc/<pid>/smaps` 中 guest RAM VMA
可能按 1MiB 左右的粒度被拆分；如果仍只看到连续 VMA，优先检查 restore log 中是否已经出现
`kind=write-protect` 和 `template UFFD WP COW`。

## 自动化测试流程

新增 `scripts/template_restore_e2e.sh` 用于验证最小闭环：

1. 构建 root crate 的 template helper 和 cloud-hypervisor / ch-remote。
2. 使用给定 `KERNEL`、`DISK` 启动 source sandbox。
3. 通过 `ch-remote pause` 和 `ch-remote snapshot file://...` 生成快照。
4. 关闭 source cloud-hypervisor 进程，释放 disk image 的写锁。
5. 运行 `template-memory-demo convert --snapshot-dir ... --template-dir ...`，把 `memory-ranges` 转换为 template 目录：
   - `template.manifest`
   - `memory-ranges`
6. 运行 `template-memory-demo serve --template-dir ... --socket ...` 启动外部 template service。
   service 同时写入 `template-metrics.log`，持续记录连接数、page request 数、请求错误数、
   唯一请求页数、重复请求数、累计服务字节数和每次请求的 backend offset。真正的 UFFD
   fault 仍由 cloud-hypervisor handler 记录在 restore log 中。
7. 使用修改后的 cloud-hypervisor restore：

```text
--restore source_url=file://<snapshot>,memory_restore_mode=ondemand,template_socket=<socket>,resume=true
```

8. 等待 restored VM API 可用，并检查 restore log 包含：

```text
template UFFD restore: using template service socket
```

9. 读取 cloud-hypervisor UFFD fault 日志和 template metrics，并输出复用性总结：
   - `template_read_ratio`: restore 期间实际从 template 读取的唯一字节数 / template backend 总字节数。
   - `deferred_reuse_ratio`: restore 后仍未被 fault 触碰的 template 字节比例。
   - `duplicate_request_ratio`: 重复 page request / 总 page request。
   - `access_read/access_write`: cloud-hypervisor handler 观测到的 UFFD fault 访问类型。
   - `kind_missing/kind_write_protect/kind_minor`: UFFD fault 类型；当前预期主要是 `kind_missing`。
   - `wp_cow_chunks/wp_cow_pages`: WP COW 的 chunk 数，以及因 chunk COW 被标记 dirty/private 的页数。
   - 同时输出重复最多的 CH fault backend offset 和 template service request offset。
   - 默认不暂停 restored VM，便于继续观察运行中的 WP fault、VMA 拆分和 metrics 变化；如需稳定截面，
     可设置 `RESTORE_PAUSE_BEFORE_SUMMARY=1`。

运行方式：

```bash
KERNEL=/path/to/vmlinux \
DISK=/path/to/rootfs.img \
./scripts/template_restore_e2e.sh
```

宿主机需要允许创建可处理 KVM/kernel-originated fault 的 `userfaultfd`。如果 restore 日志中出现
`Failed to create userfaultfd: Operation not permitted`，通常是当前进程缺少 `CAP_SYS_PTRACE` 且
`vm.unprivileged_userfaultfd=0`。测试脚本会在启动前检查这个条件；可通过以下方式修复：

```bash
sudo sysctl -w vm.unprivileged_userfaultfd=1
```

当前最小闭环满足四个测试目标，但语义仍是“单层 snapshot 作为 template backend”：

- 已支持通过 cloud-hypervisor 启动 sandbox 并生成快照。
- 已支持把 `memory-ranges` 转换为 template helper 可识别的目录格式。
- 已支持启动独立 template service 进程。
- 已支持 cloud-hypervisor restore 通过 `template_socket` 从 template service 拉取 fault page。

尚未覆盖完整产品语义：

- template service 目前只服务单层 `memory-ranges`，还不是多层 overlay。
- checkpoint dirty layer 输出还没有接入 cloud-hypervisor snapshot 格式。

## 当前原型限制

- 为了保持 demo 独立，没有依赖 cloud-hypervisor 内部 crate。
- 使用 Linux x86_64 syscall/ioctl 常量。
- COW 使用 `MAP_FIXED`，会产生 VMA 碎片，正好暴露后续需要优化的问题。
- 如果宿主内核禁用 unprivileged userfaultfd 且进程没有 `CAP_SYS_PTRACE`，on-demand restore
  会失败，但编译仍可验证。
