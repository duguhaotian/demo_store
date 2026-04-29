use std::ffi::{CString, c_void};
use std::fs::File;
use std::mem::{self, MaybeUninit};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU8, AtomicU64, Ordering};
use std::thread;

const MFD_CLOEXEC: i32 = 0x0001;
const O_CLOEXEC: i32 = 0o2_000_000;
const O_NONBLOCK: i32 = 0o4_000;
const UFFD_USER_MODE_ONLY: i32 = 1;

const PROT_READ: i32 = 0x1;
const PROT_WRITE: i32 = 0x2;
const MAP_SHARED: i32 = 0x01;
const MAP_PRIVATE: i32 = 0x02;
const MAP_FIXED: i32 = 0x10;
const MAP_ANONYMOUS: i32 = 0x20;

const POLLIN: i16 = 0x0001;
const EINTR: i32 = 4;
const EAGAIN: i32 = 11;
const EBADF: i32 = 9;
const EEXIST: i32 = 17;

const SYS_MEMFD_CREATE: i64 = 319;
const SYS_USERFAULTFD: i64 = 323;

const UFFD_API: u64 = 0xAA;
const UFFDIO_API: u64 = 0xc018_aa3f;
const UFFDIO_REGISTER: u64 = 0xc020_aa00;
const UFFDIO_COPY: u64 = 0xc028_aa03;
const UFFDIO_WAKE: u64 = 0x8010_aa02;
const UFFDIO_WRITEPROTECT: u64 = 0xc018_aa06;

const UFFDIO_REGISTER_MODE_MISSING: u64 = 1 << 0;
const UFFDIO_REGISTER_MODE_WP: u64 = 1 << 1;
const UFFDIO_COPY_MODE_WP: u64 = 1 << 1;
const UFFDIO_WRITEPROTECT_MODE_DONTWAKE: u64 = 1 << 1;
const UFFD_EVENT_PAGEFAULT: u8 = 0x12;
const UFFD_PAGEFAULT_FLAG_WP: u64 = 1 << 1;
const UFFD_FEATURE_PAGEFAULT_FLAG_WP: u64 = 1 << 0;
const UFFD_FEATURE_MISSING_SHMEM: u64 = 1 << 5;
const UFFD_FEATURE_WP_HUGETLBFS_SHMEM: u64 = 1 << 12;

const _UFFDIO_COPY: u64 = 0x03;
const _UFFDIO_WAKE: u64 = 0x02;
const _UFFDIO_WRITEPROTECT: u64 = 0x06;

const STATE_UNLOADED: u8 = 0;
const STATE_LOADING: u8 = 1;
const STATE_SHARED_READY: u8 = 2;

pub type Result<T> = std::result::Result<T, Error>;

#[derive(Debug)]
pub enum Error {
    Io(std::io::Error),
    InvalidInput(String),
    Kernel(String),
    Thread(String),
}

impl From<std::io::Error> for Error {
    fn from(err: std::io::Error) -> Self {
        Self::Io(err)
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(err) => write!(f, "{err}"),
            Self::InvalidInput(msg) | Self::Kernel(msg) | Self::Thread(msg) => write!(f, "{msg}"),
        }
    }
}

impl std::error::Error for Error {}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TemplatePageState {
    Unloaded,
    Loading,
    SharedReady,
    Unknown(u8),
}

impl TemplatePageState {
    fn from_raw(raw: u8) -> Self {
        match raw {
            STATE_UNLOADED => Self::Unloaded,
            STATE_LOADING => Self::Loading,
            STATE_SHARED_READY => Self::SharedReady,
            other => Self::Unknown(other),
        }
    }
}

#[repr(C)]
struct TemplatePageMeta {
    state: AtomicU8,
    _pad: [u8; 7],
    backend_offset: AtomicU64,
}

#[derive(Clone, Copy)]
struct SharedMetaView {
    addr: usize,
    len: usize,
}

impl SharedMetaView {
    fn get(self, page_idx: usize) -> &'static TemplatePageMeta {
        assert!(page_idx < self.len);
        let ptr = self.addr as *const TemplatePageMeta;
        unsafe { &*ptr.add(page_idx) }
    }
}

struct SharedMetaMapping {
    view: SharedMetaView,
    bytes: usize,
}

impl SharedMetaMapping {
    fn new(page_count: usize, page_size: usize) -> Result<Self> {
        let bytes = page_count
            .checked_mul(mem::size_of::<TemplatePageMeta>())
            .ok_or_else(|| Error::InvalidInput("metadata mapping size overflow".to_string()))?;
        let ptr = unsafe {
            mmap(
                ptr::null_mut(),
                bytes,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS,
                -1,
                0,
            )
        };
        if is_map_failed(ptr) {
            return Err(last_os_error("mmap shared metadata"));
        }

        let meta_ptr = ptr.cast::<TemplatePageMeta>();
        for idx in 0..page_count {
            let meta = TemplatePageMeta {
                state: AtomicU8::new(STATE_UNLOADED),
                _pad: [0; 7],
                backend_offset: AtomicU64::new((idx * page_size) as u64),
            };
            unsafe { meta_ptr.add(idx).write(meta) };
        }

        Ok(Self {
            view: SharedMetaView {
                addr: meta_ptr as usize,
                len: page_count,
            },
            bytes,
        })
    }
}

impl Drop for SharedMetaMapping {
    fn drop(&mut self) {
        let _ = unsafe { munmap(self.view.addr as *mut c_void, self.bytes) };
    }
}

struct ProcPageMeta {
    is_private: AtomicBool,
    dirty: AtomicBool,
}

impl ProcPageMeta {
    fn new() -> Self {
        Self {
            is_private: AtomicBool::new(false),
            dirty: AtomicBool::new(false),
        }
    }
}

pub struct Layer {
    name: String,
    file: File,
    bitmap: Vec<u64>,
}

impl Layer {
    pub fn new(name: impl Into<String>, file: File, bitmap: Vec<u64>) -> Self {
        Self {
            name: name.into(),
            file,
            bitmap,
        }
    }
}

#[derive(Debug, Clone)]
pub struct PageLocation {
    pub layer_name: String,
    pub offset: u64,
}

pub struct OverlayBackend {
    page_size: usize,
    page_count: usize,
    layers: Vec<Layer>,
    merged_bitmap: Vec<u64>,
}

impl OverlayBackend {
    pub fn new(page_size: usize, page_count: usize, layers: Vec<Layer>) -> Result<Self> {
        if page_size == 0 || page_count == 0 {
            return Err(Error::InvalidInput(
                "page_size and page_count must be non-zero".to_string(),
            ));
        }
        if layers.is_empty() {
            return Err(Error::InvalidInput(
                "overlay backend requires at least one layer".to_string(),
            ));
        }

        let words = bitmap_words(page_count);
        for layer in &layers {
            if layer.bitmap.len() != words {
                return Err(Error::InvalidInput(format!(
                    "layer '{}' bitmap has {} words, expected {}",
                    layer.name,
                    layer.bitmap.len(),
                    words
                )));
            }
        }

        let mut backend = Self {
            page_size,
            page_count,
            layers,
            merged_bitmap: vec![0; words],
        };
        backend.rebuild_merged_bitmap();
        Ok(backend)
    }

    pub fn page_size(&self) -> usize {
        self.page_size
    }

    pub fn page_count(&self) -> usize {
        self.page_count
    }

    pub fn locate(&self, page_idx: usize) -> Result<PageLocation> {
        if page_idx >= self.page_count {
            return Err(Error::InvalidInput(format!(
                "page index out of range: {page_idx}"
            )));
        }

        for layer in self.layers.iter().rev() {
            if bitmap_get(&layer.bitmap, page_idx) {
                return Ok(PageLocation {
                    layer_name: layer.name.clone(),
                    offset: (page_idx * self.page_size) as u64,
                });
            }
        }

        Err(Error::InvalidInput(format!(
            "page {page_idx} has no backend layer"
        )))
    }

    pub fn read_page(&self, page_idx: usize, dst: &mut [u8]) -> Result<PageLocation> {
        if dst.len() != self.page_size {
            return Err(Error::InvalidInput(
                "backend read buffer must be one page".to_string(),
            ));
        }
        if page_idx >= self.page_count {
            return Err(Error::InvalidInput(format!(
                "page index out of range: {page_idx}"
            )));
        }

        for layer in self.layers.iter().rev() {
            if bitmap_get(&layer.bitmap, page_idx) {
                let offset = (page_idx * self.page_size) as u64;
                pread_exact(layer.file.as_raw_fd(), dst, offset)?;
                return Ok(PageLocation {
                    layer_name: layer.name.clone(),
                    offset,
                });
            }
        }

        Err(Error::InvalidInput(format!(
            "page {page_idx} has no backend layer"
        )))
    }

    pub fn merged_bitmap(&self) -> Vec<u64> {
        self.merged_bitmap.clone()
    }

    fn rebuild_merged_bitmap(&mut self) {
        for word in &mut self.merged_bitmap {
            *word = 0;
        }
        for layer in &self.layers {
            for (idx, word) in layer.bitmap.iter().enumerate() {
                self.merged_bitmap[idx] |= *word;
            }
        }
    }
}

pub struct TemplateService {
    page_size: usize,
    page_count: usize,
    region_size: usize,
    memfd: OwnedFd,
    meta: SharedMetaMapping,
    backend: Arc<OverlayBackend>,
}

impl TemplateService {
    pub fn new(region_size: usize, backend: OverlayBackend) -> Result<Self> {
        let page_size = page_size()?;
        if region_size % page_size != 0 {
            return Err(Error::InvalidInput(
                "region size must be page aligned".to_string(),
            ));
        }
        let page_count = region_size / page_size;
        if backend.page_size() != page_size || backend.page_count() != page_count {
            return Err(Error::InvalidInput(format!(
                "backend geometry mismatch: backend={}x{}, region={}x{}",
                backend.page_count(),
                backend.page_size(),
                page_count,
                page_size
            )));
        }

        let memfd = memfd_create("template-memory")?;
        ftruncate_fd(memfd.as_raw_fd(), region_size)?;
        let meta = SharedMetaMapping::new(page_count, page_size)?;

        Ok(Self {
            page_size,
            page_count,
            region_size,
            memfd,
            meta,
            backend: Arc::new(backend),
        })
    }

    pub fn page_size(&self) -> usize {
        self.page_size
    }

    pub fn page_count(&self) -> usize {
        self.page_count
    }

    pub fn region_size(&self) -> usize {
        self.region_size
    }

    pub fn backend(&self) -> &OverlayBackend {
        &self.backend
    }

    pub fn page_state(&self, page_idx: usize) -> TemplatePageState {
        let raw = self.meta.view.get(page_idx).state.load(Ordering::Acquire);
        TemplatePageState::from_raw(raw)
    }

    pub fn map_client_region(&self) -> Result<TemplateMapping> {
        let ptr = unsafe {
            mmap(
                ptr::null_mut(),
                self.region_size,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                self.memfd.as_raw_fd(),
                0,
            )
        };
        if is_map_failed(ptr) {
            return Err(last_os_error("mmap template memfd"));
        }

        Ok(TemplateMapping {
            ptr: ptr.cast::<u8>(),
            len: self.region_size,
        })
    }
}

pub struct TemplateMapping {
    ptr: *mut u8,
    len: usize,
}

impl TemplateMapping {
    pub fn as_ptr(&self) -> *const u8 {
        self.ptr
    }

    pub fn as_mut_ptr(&self) -> *mut u8 {
        self.ptr
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

impl Drop for TemplateMapping {
    fn drop(&mut self) {
        let _ = unsafe { munmap(self.ptr.cast::<c_void>(), self.len) };
    }
}

pub struct Client {
    inner: Arc<ClientInner>,
    handler: Option<thread::JoinHandle<Result<()>>>,
}

impl Client {
    pub fn new(service: &TemplateService, base: *mut u8) -> Result<Self> {
        let uffd = create_userfaultfd()?;
        let ioctls = register_uffd(
            uffd.as_raw_fd(),
            base as u64,
            service.region_size as u64,
            UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP,
        )?;
        ensure_ioctl(ioctls, _UFFDIO_COPY, "UFFDIO_COPY")?;
        ensure_ioctl(ioctls, _UFFDIO_WAKE, "UFFDIO_WAKE")?;
        ensure_ioctl(ioctls, _UFFDIO_WRITEPROTECT, "UFFDIO_WRITEPROTECT")?;

        let proc_meta = (0..service.page_count)
            .map(|_| ProcPageMeta::new())
            .collect();
        let inner = Arc::new(ClientInner {
            uffd,
            base_addr: base as usize,
            region_size: service.region_size,
            page_size: service.page_size,
            page_count: service.page_count,
            meta: service.meta.view,
            backend: Arc::clone(&service.backend),
            proc_meta,
            stop: AtomicBool::new(false),
        });

        Ok(Self {
            inner,
            handler: None,
        })
    }

    pub fn start_handler(&mut self) {
        let inner = Arc::clone(&self.inner);
        self.handler = Some(thread::spawn(move || fault_handler_main(inner)));
    }

    pub fn stop_handler(&mut self) -> Result<()> {
        self.inner.stop.store(true, Ordering::Release);
        if let Some(handler) = self.handler.take() {
            match handler.join() {
                Ok(Ok(())) => {}
                Ok(Err(err)) => return Err(err),
                Err(_) => {
                    return Err(Error::Thread("fault handler thread panicked".to_string()));
                }
            }
        }
        Ok(())
    }

    pub fn export_checkpoint(&self) -> Checkpoint {
        Checkpoint::export(&self.inner)
    }

    pub fn dirty_bitmap(&self) -> Vec<u64> {
        let mut bitmap = vec![0; bitmap_words(self.inner.page_count)];
        for page_idx in 0..self.inner.page_count {
            if self.inner.proc_meta[page_idx].dirty.load(Ordering::Acquire) {
                bitmap_set(&mut bitmap, page_idx);
            }
        }
        bitmap
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        let _ = self.stop_handler();
    }
}

struct ClientInner {
    uffd: OwnedFd,
    base_addr: usize,
    region_size: usize,
    page_size: usize,
    page_count: usize,
    meta: SharedMetaView,
    backend: Arc<OverlayBackend>,
    proc_meta: Vec<ProcPageMeta>,
    stop: AtomicBool,
}

impl ClientInner {
    fn page_index_from_addr(&self, fault_addr: usize) -> Result<usize> {
        if fault_addr < self.base_addr || fault_addr >= self.base_addr + self.region_size {
            return Err(Error::InvalidInput(format!(
                "fault outside client range: {fault_addr:#x}"
            )));
        }
        Ok((fault_addr - self.base_addr) / self.page_size)
    }

    fn page_start(&self, addr: usize) -> usize {
        addr & !(self.page_size - 1)
    }

    fn mark_private_dirty(&self, page_idx: usize) {
        self.proc_meta[page_idx]
            .is_private
            .store(true, Ordering::Release);
        self.proc_meta[page_idx]
            .dirty
            .store(true, Ordering::Release);
    }
}

pub struct DirtyPage {
    pub page_idx: usize,
    pub data: Vec<u8>,
}

pub struct Checkpoint {
    pub dirty_pages: Vec<DirtyPage>,
    pub private_bitmap: Vec<u64>,
    pub merged_backend_bitmap: Vec<u64>,
}

impl Checkpoint {
    fn export(client: &ClientInner) -> Self {
        let mut dirty_pages = Vec::new();
        let mut private_bitmap = vec![0; bitmap_words(client.page_count)];

        for page_idx in 0..client.page_count {
            if !client.proc_meta[page_idx].dirty.load(Ordering::Acquire) {
                continue;
            }
            bitmap_set(&mut private_bitmap, page_idx);
            let page_addr = client.base_addr + page_idx * client.page_size;
            let mut data = vec![0; client.page_size];
            unsafe {
                ptr::copy_nonoverlapping(
                    page_addr as *const u8,
                    data.as_mut_ptr(),
                    client.page_size,
                );
            }
            dirty_pages.push(DirtyPage { page_idx, data });
        }

        Self {
            dirty_pages,
            private_bitmap,
            merged_backend_bitmap: client.backend.merged_bitmap(),
        }
    }
}

fn fault_handler_main(client: Arc<ClientInner>) -> Result<()> {
    loop {
        if client.stop.load(Ordering::Acquire) {
            return Ok(());
        }

        let mut pfd = PollFd {
            fd: client.uffd.as_raw_fd(),
            events: POLLIN,
            revents: 0,
        };
        let ready = unsafe { poll(&mut pfd, 1, 100) };
        if ready < 0 {
            let err = std::io::Error::last_os_error();
            if matches!(err.raw_os_error(), Some(EINTR)) {
                continue;
            }
            if matches!(err.raw_os_error(), Some(EBADF)) && client.stop.load(Ordering::Acquire) {
                return Ok(());
            }
            return Err(Error::Io(err));
        }
        if ready == 0 || (pfd.revents & POLLIN) == 0 {
            continue;
        }

        let mut msg = MaybeUninit::<UffdMsg>::zeroed();
        let nread = unsafe {
            read(
                client.uffd.as_raw_fd(),
                msg.as_mut_ptr().cast::<c_void>(),
                mem::size_of::<UffdMsg>(),
            )
        };
        if nread < 0 {
            let err = std::io::Error::last_os_error();
            if matches!(err.raw_os_error(), Some(EAGAIN | EINTR)) {
                continue;
            }
            if matches!(err.raw_os_error(), Some(EBADF)) && client.stop.load(Ordering::Acquire) {
                return Ok(());
            }
            return Err(Error::Io(err));
        }
        if nread as usize != mem::size_of::<UffdMsg>() {
            return Err(Error::Kernel(format!(
                "short read from userfaultfd: {nread}"
            )));
        }

        let msg = unsafe { msg.assume_init() };
        if msg.event != UFFD_EVENT_PAGEFAULT {
            continue;
        }
        if (msg.pf_flags & UFFD_PAGEFAULT_FLAG_WP) != 0 {
            handle_wp_fault(&client, msg.pf_address as usize)?;
        } else {
            handle_missing_fault(&client, msg.pf_address as usize)?;
        }
    }
}

fn handle_missing_fault(client: &ClientInner, fault_addr: usize) -> Result<()> {
    let page_idx = client.page_index_from_addr(fault_addr)?;
    let aligned = client.page_start(fault_addr);
    let page_meta = client.meta.get(page_idx);

    match page_meta.state.compare_exchange(
        STATE_UNLOADED,
        STATE_LOADING,
        Ordering::AcqRel,
        Ordering::Acquire,
    ) {
        Ok(_) => {
            let mut page = vec![0; client.page_size];
            client.backend.read_page(page_idx, &mut page)?;
            match uffdio_copy(
                client.uffd.as_raw_fd(),
                aligned as u64,
                page.as_ptr(),
                client.page_size as u64,
                UFFDIO_COPY_MODE_WP,
            ) {
                Ok(()) => {}
                Err(err) if matches!(err.raw_os_error(), Some(EEXIST)) => {
                    uffdio_wake(
                        client.uffd.as_raw_fd(),
                        aligned as u64,
                        client.page_size as u64,
                    )?;
                }
                Err(err) => return Err(Error::Io(err)),
            }
            page_meta.state.store(STATE_SHARED_READY, Ordering::Release);
        }
        Err(STATE_LOADING) => {
            while page_meta.state.load(Ordering::Acquire) != STATE_SHARED_READY {
                thread::yield_now();
            }
            uffdio_wake(
                client.uffd.as_raw_fd(),
                aligned as u64,
                client.page_size as u64,
            )?;
        }
        Err(STATE_SHARED_READY) => {
            uffdio_wake(
                client.uffd.as_raw_fd(),
                aligned as u64,
                client.page_size as u64,
            )?;
        }
        Err(other) => {
            return Err(Error::Kernel(format!(
                "unexpected template page state {other} for page {page_idx}"
            )));
        }
    }

    Ok(())
}

fn handle_wp_fault(client: &ClientInner, fault_addr: usize) -> Result<()> {
    let page_idx = client.page_index_from_addr(fault_addr)?;
    let aligned = client.page_start(fault_addr);
    let snapshot = unsafe {
        mmap(
            ptr::null_mut(),
            client.page_size,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0,
        )
    };
    if is_map_failed(snapshot) {
        return Err(last_os_error("mmap snapshot page"));
    }

    unsafe {
        ptr::copy_nonoverlapping(
            aligned as *const u8,
            snapshot.cast::<u8>(),
            client.page_size,
        );
    }

    uffdio_writeprotect(
        client.uffd.as_raw_fd(),
        aligned as u64,
        client.page_size as u64,
        UFFDIO_WRITEPROTECT_MODE_DONTWAKE,
    )?;

    let private_page = unsafe {
        mmap(
            aligned as *mut c_void,
            client.page_size,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
            -1,
            0,
        )
    };
    if is_map_failed(private_page) {
        let _ = unsafe { munmap(snapshot, client.page_size) };
        return Err(last_os_error("mmap private page over shared mapping"));
    }

    unsafe {
        ptr::copy_nonoverlapping(
            snapshot.cast::<u8>(),
            private_page.cast::<u8>(),
            client.page_size,
        );
        munmap(snapshot, client.page_size);
    }
    client.mark_private_dirty(page_idx);
    uffdio_wake(
        client.uffd.as_raw_fd(),
        aligned as u64,
        client.page_size as u64,
    )?;

    Ok(())
}

pub fn bitmap_words(bits: usize) -> usize {
    bits.div_ceil(64)
}

pub fn bitmap_from(page_count: usize, present: impl Fn(usize) -> bool) -> Vec<u64> {
    let mut bitmap = vec![0; bitmap_words(page_count)];
    for page_idx in 0..page_count {
        if present(page_idx) {
            bitmap_set(&mut bitmap, page_idx);
        }
    }
    bitmap
}

pub fn bitmap_hex(bitmap: &[u64]) -> String {
    bitmap
        .iter()
        .rev()
        .map(|word| format!("{word:016x}"))
        .collect::<Vec<_>>()
        .join("_")
}

fn bitmap_set(bitmap: &mut [u64], bit: usize) {
    bitmap[bit / 64] |= 1_u64 << (bit % 64);
}

fn bitmap_get(bitmap: &[u64], bit: usize) -> bool {
    (bitmap[bit / 64] & (1_u64 << (bit % 64))) != 0
}

fn create_userfaultfd() -> Result<OwnedFd> {
    let flags = O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY;
    let fd = unsafe { syscall(SYS_USERFAULTFD, flags) };
    if fd < 0 {
        return Err(last_os_error("userfaultfd"));
    }
    let fd = unsafe { OwnedFd::from_raw_fd(fd as RawFd) };
    let mut api = UffdioApi {
        api: UFFD_API,
        features: 0,
        ioctls: 0,
    };
    ioctl_checked(fd.as_raw_fd(), UFFDIO_API, &mut api, "UFFDIO_API")?;

    let required = UFFD_FEATURE_PAGEFAULT_FLAG_WP
        | UFFD_FEATURE_MISSING_SHMEM
        | UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
    if (api.features & required) != required {
        return Err(Error::Kernel(format!(
            "kernel lacks required userfaultfd features: have={:#x} required={:#x}",
            api.features, required
        )));
    }

    Ok(fd)
}

fn register_uffd(fd: RawFd, addr: u64, len: u64, mode: u64) -> Result<u64> {
    let mut reg = UffdioRegister {
        range: UffdioRange { start: addr, len },
        mode,
        ioctls: 0,
    };
    ioctl_checked(fd, UFFDIO_REGISTER, &mut reg, "UFFDIO_REGISTER")?;
    Ok(reg.ioctls)
}

fn ensure_ioctl(ioctls: u64, bit: u64, name: &str) -> Result<()> {
    if (ioctls & (1_u64 << bit)) == 0 {
        return Err(Error::Kernel(format!(
            "registered range does not support {name}"
        )));
    }
    Ok(())
}

fn uffdio_copy(fd: RawFd, dst: u64, src: *const u8, len: u64, mode: u64) -> std::io::Result<()> {
    let mut copy = UffdioCopy {
        dst,
        src: src as u64,
        len,
        mode,
        copy: 0,
    };
    ioctl_io(fd, UFFDIO_COPY, &mut copy)
}

fn uffdio_wake(fd: RawFd, start: u64, len: u64) -> std::io::Result<()> {
    let mut range = UffdioRange { start, len };
    ioctl_io(fd, UFFDIO_WAKE, &mut range)
}

fn uffdio_writeprotect(fd: RawFd, start: u64, len: u64, mode: u64) -> std::io::Result<()> {
    let mut wp = UffdioWriteProtect {
        range: UffdioRange { start, len },
        mode,
    };
    ioctl_io(fd, UFFDIO_WRITEPROTECT, &mut wp)
}

fn ioctl_checked<T>(fd: RawFd, request: u64, arg: &mut T, name: &str) -> Result<()> {
    ioctl_io(fd, request, arg).map_err(|err| Error::Kernel(format!("{name}: {err}")))
}

fn ioctl_io<T>(fd: RawFd, request: u64, arg: &mut T) -> std::io::Result<()> {
    let ret = unsafe { ioctl(fd, request, (arg as *mut T).cast::<c_void>()) };
    if ret < 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(())
}

pub fn page_size() -> Result<usize> {
    let value = unsafe { sysconf(30) };
    if value <= 0 {
        return Err(last_os_error("sysconf(_SC_PAGESIZE)"));
    }
    Ok(value as usize)
}

fn memfd_create(name: &str) -> Result<OwnedFd> {
    let name = CString::new(name)
        .map_err(|_| Error::InvalidInput("memfd name contains NUL".to_string()))?;
    let fd = unsafe { syscall(SYS_MEMFD_CREATE, name.as_ptr(), MFD_CLOEXEC) };
    if fd < 0 {
        return Err(last_os_error("memfd_create"));
    }
    Ok(unsafe { OwnedFd::from_raw_fd(fd as RawFd) })
}

fn ftruncate_fd(fd: RawFd, len: usize) -> Result<()> {
    if unsafe { ftruncate(fd, len as i64) } < 0 {
        return Err(last_os_error("ftruncate"));
    }
    Ok(())
}

fn pread_exact(fd: RawFd, mut dst: &mut [u8], mut offset: u64) -> Result<()> {
    while !dst.is_empty() {
        let nread = pread_some(fd, dst, offset)?;
        if nread == 0 {
            return Err(Error::Io(std::io::Error::new(
                std::io::ErrorKind::UnexpectedEof,
                "unexpected EOF while reading backend",
            )));
        }
        offset += nread as u64;
        dst = &mut dst[nread..];
    }
    Ok(())
}

fn pread_some(fd: RawFd, dst: &mut [u8], offset: u64) -> Result<usize> {
    loop {
        let nread = unsafe {
            pread(
                fd,
                dst.as_mut_ptr().cast::<c_void>(),
                dst.len(),
                offset as i64,
            )
        };
        if nread < 0 {
            let err = std::io::Error::last_os_error();
            if matches!(err.raw_os_error(), Some(EINTR)) {
                continue;
            }
            return Err(Error::Io(err));
        }
        return Ok(nread as usize);
    }
}

fn is_map_failed(ptr: *mut c_void) -> bool {
    ptr as isize == -1
}

fn last_os_error(what: &str) -> Error {
    Error::Kernel(format!("{what}: {}", std::io::Error::last_os_error()))
}

#[repr(C)]
struct UffdioApi {
    api: u64,
    features: u64,
    ioctls: u64,
}

#[repr(C)]
struct UffdioRange {
    start: u64,
    len: u64,
}

#[repr(C)]
struct UffdioRegister {
    range: UffdioRange,
    mode: u64,
    ioctls: u64,
}

#[repr(C)]
struct UffdioCopy {
    dst: u64,
    src: u64,
    len: u64,
    mode: u64,
    copy: i64,
}

#[repr(C)]
struct UffdioWriteProtect {
    range: UffdioRange,
    mode: u64,
}

#[repr(C)]
struct UffdMsg {
    event: u8,
    _reserved1: u8,
    _reserved2: u16,
    _reserved3: u32,
    pf_flags: u64,
    pf_address: u64,
    _pad: [u8; 8],
}

const _: () = assert!(mem::size_of::<UffdMsg>() == 32);

#[repr(C)]
struct PollFd {
    fd: RawFd,
    events: i16,
    revents: i16,
}

unsafe extern "C" {
    fn syscall(num: i64, ...) -> i64;
    fn sysconf(name: i32) -> i64;
    fn ftruncate(fd: RawFd, length: i64) -> i32;
    fn mmap(
        addr: *mut c_void,
        length: usize,
        prot: i32,
        flags: i32,
        fd: RawFd,
        offset: i64,
    ) -> *mut c_void;
    fn munmap(addr: *mut c_void, length: usize) -> i32;
    fn ioctl(fd: RawFd, request: u64, ...) -> i32;
    fn poll(fds: *mut PollFd, nfds: usize, timeout: i32) -> i32;
    fn read(fd: RawFd, buf: *mut c_void, count: usize) -> isize;
    fn pread(fd: RawFd, buf: *mut c_void, count: usize, offset: i64) -> isize;
}
