// Copyright © 2026 Cloud Hypervisor Authors
//
// SPDX-License-Identifier: Apache-2.0

//! Template-memory restore client support.
//!
//! This module is the integration boundary for the user-space template memory
//! model. The first step keeps Cloud Hypervisor's existing on-demand restore
//! semantics: page faults are resolved from a read-only snapshot layer with
//! `UFFDIO_COPY`. The code is structured around per-page template metadata and
//! a backend page locator so the same client can grow into multi-layer overlay
//! restore and per-process COW/dirty tracking without keeping the logic inside
//! `MemoryManager`.

use std::fs::File;
use std::io::{self, Read as _, Seek, SeekFrom, Write as _};
use std::os::fd::{AsFd, AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::mpsc::SyncSender;
use std::thread;

use log::{info, warn};
use vmm_sys_util::eventfd::EventFd;

use crate::uffd;

const STATE_UNLOADED: u8 = 0;
const STATE_LOADING: u8 = 1;
const STATE_SHARED_READY: u8 = 2;

pub(crate) struct TemplateRange {
    pub host_addr: u64,
    pub length: u64,
    pub file_offset: u64,
    pub page_size: u64,
    first_page_index: u64,
}

impl TemplateRange {
    pub(crate) fn new(host_addr: u64, length: u64, file_offset: u64, page_size: u64) -> Self {
        Self {
            host_addr,
            length,
            file_offset,
            page_size,
            first_page_index: 0,
        }
    }

    fn page_count(&self) -> u64 {
        self.length.div_ceil(self.page_size)
    }

    fn locate(&self, fault_addr: u64) -> Option<TemplateFault> {
        let page_addr = fault_addr & !(self.page_size - 1);
        if page_addr < self.host_addr || page_addr >= self.host_addr + self.length {
            return None;
        }

        let offset_in_range = page_addr - self.host_addr;
        Some(TemplateFault {
            page_addr,
            page_size: self.page_size,
            page_index: self.first_page_index + offset_in_range / self.page_size,
            backend_offset: self.file_offset + offset_in_range,
        })
    }
}

struct TemplateFault {
    page_addr: u64,
    page_size: u64,
    page_index: u64,
    backend_offset: u64,
}

struct TemplatePageMeta {
    state: AtomicU8,
    backend_offset: u64,
}

impl TemplatePageMeta {
    fn new(backend_offset: u64) -> Self {
        Self {
            state: AtomicU8::new(STATE_UNLOADED),
            backend_offset,
        }
    }
}

pub(crate) struct TemplateRestoreBackend {
    source: TemplatePageSource,
    ranges: Vec<TemplateRange>,
    pages: Vec<TemplatePageMeta>,
}

enum TemplatePageSource {
    File(File),
    Service(TemplateServiceClient),
}

impl TemplateRestoreBackend {
    pub(crate) fn from_file(snapshot_file: File, ranges: Vec<TemplateRange>) -> Self {
        Self::new(TemplatePageSource::File(snapshot_file), ranges)
    }

    pub(crate) fn from_service(socket_path: &Path, ranges: Vec<TemplateRange>) -> io::Result<Self> {
        Ok(Self::new(
            TemplatePageSource::Service(TemplateServiceClient::connect(socket_path)?),
            ranges,
        ))
    }

    fn new(source: TemplatePageSource, mut ranges: Vec<TemplateRange>) -> Self {
        let mut first_page_index = 0;
        let mut pages = Vec::new();

        for range in &mut ranges {
            range.first_page_index = first_page_index;
            for page in 0..range.page_count() {
                pages.push(TemplatePageMeta::new(
                    range.file_offset + page * range.page_size,
                ));
            }
            first_page_index += range.page_count();
        }

        Self {
            source,
            ranges,
            pages,
        }
    }

    pub(crate) fn total_pages(&self) -> u64 {
        self.pages.len() as u64
    }

    pub(crate) fn max_page_size(&self, default_page_size: u64) -> u64 {
        self.ranges
            .iter()
            .map(|r| r.page_size)
            .max()
            .unwrap_or(default_page_size)
    }

    fn locate_fault(&self, fault_addr: u64) -> Option<TemplateFault> {
        self.ranges
            .iter()
            .find_map(|range| range.locate(fault_addr))
    }

    fn read_page(&mut self, fault: &TemplateFault, page_buf: &mut [u8]) -> io::Result<()> {
        let meta = &self.pages[fault.page_index as usize];
        debug_assert_eq!(meta.backend_offset, fault.backend_offset);

        match &mut self.source {
            TemplatePageSource::File(snapshot_file) => {
                snapshot_file.seek(SeekFrom::Start(meta.backend_offset))?;
                snapshot_file.read_exact(&mut page_buf[..fault.page_size as usize])
            }
            TemplatePageSource::Service(client) => client.read_page(
                meta.backend_offset,
                &mut page_buf[..fault.page_size as usize],
            ),
        }
    }
}

struct TemplateServiceClient {
    stream: UnixStream,
}

impl TemplateServiceClient {
    fn connect(socket_path: &Path) -> io::Result<Self> {
        Ok(Self {
            stream: UnixStream::connect(socket_path)?,
        })
    }

    fn read_page(&mut self, offset: u64, dst: &mut [u8]) -> io::Result<()> {
        let len = u32::try_from(dst.len()).map_err(|_| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                "template page request exceeds u32::MAX",
            )
        })?;
        self.stream.write_all(&offset.to_le_bytes())?;
        self.stream.write_all(&len.to_le_bytes())?;

        let mut header = [0u8; 8];
        self.stream.read_exact(&mut header)?;
        let status = u32::from_le_bytes(header[0..4].try_into().unwrap());
        let response_len = u32::from_le_bytes(header[4..8].try_into().unwrap()) as usize;
        if status != 0 {
            return Err(io::Error::other(format!(
                "template service read failed for offset {offset:#x}"
            )));
        }
        if response_len != dst.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "template service returned {response_len} bytes, expected {}",
                    dst.len()
                ),
            ));
        }
        self.stream.read_exact(dst)
    }
}

/// Poll the UFFD fd and serve page faults from the template backend.
pub(crate) fn handler_loop(
    uffd_fd: &OwnedFd,
    stop_event: &EventFd,
    mut backend: TemplateRestoreBackend,
    page_size: u64,
    ready_tx: &SyncSender<()>,
) -> io::Result<()> {
    let uffd_raw_fd = uffd_fd.as_raw_fd();
    let mut page_buf = vec![0u8; page_size as usize];
    let total_pages = backend.total_pages();
    let mut pages_served: u64 = 0;

    const EVENT_STOP: u64 = 0;
    const EVENT_UFFD: u64 = 1;

    let epoll_fd = epoll::create(true).map_err(io::Error::other)?;
    // SAFETY: epoll_fd is valid and owned by this scope.
    let _epoll_file = unsafe { File::from_raw_fd(epoll_fd) };

    epoll::ctl(
        epoll_fd,
        epoll::ControlOptions::EPOLL_CTL_ADD,
        stop_event.as_raw_fd(),
        epoll::Event::new(epoll::Events::EPOLLIN, EVENT_STOP),
    )
    .map_err(io::Error::other)?;

    epoll::ctl(
        epoll_fd,
        epoll::ControlOptions::EPOLL_CTL_ADD,
        uffd_raw_fd,
        epoll::Event::new(epoll::Events::EPOLLIN | epoll::Events::EPOLLHUP, EVENT_UFFD),
    )
    .map_err(io::Error::other)?;

    ready_tx.send(()).ok();

    let mut events = vec![epoll::Event::new(epoll::Events::empty(), 0); 2];
    loop {
        let num_events = match epoll::wait(epoll_fd, -1, &mut events) {
            Ok(n) => n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        };

        let mut got_uffd_data = false;
        for event in events.iter().take(num_events) {
            let token = event.data;
            let evt_flags = event.events;

            if token == EVENT_STOP {
                stop_event.read().ok();
                info!("template UFFD handler: received stop event, exiting");
                return Ok(());
            }

            if token == EVENT_UFFD
                && (evt_flags & epoll::Events::EPOLLHUP.bits()) != 0
                && (evt_flags & epoll::Events::EPOLLIN.bits()) == 0
            {
                info!("template UFFD handler: fd closed (EPOLLHUP), exiting");
                return Ok(());
            }

            if token == EVENT_UFFD && (evt_flags & epoll::Events::EPOLLIN.bits()) != 0 {
                got_uffd_data = true;
            }
        }

        if !got_uffd_data {
            continue;
        }

        // SAFETY: UffdMsg is a plain repr(C) struct, safe to zero-init.
        let mut msg: uffd::UffdMsg = unsafe { std::mem::zeroed() };
        // SAFETY: reading a uffd_msg-sized struct from the valid uffd fd.
        let n = unsafe {
            libc::read(
                uffd_raw_fd,
                &mut msg as *mut uffd::UffdMsg as *mut libc::c_void,
                std::mem::size_of::<uffd::UffdMsg>(),
            )
        };
        if n < 0 {
            let err = io::Error::last_os_error();
            if err.kind() == io::ErrorKind::WouldBlock {
                continue;
            }
            return Err(err);
        }
        if n == 0 {
            info!("template UFFD handler: EOF on fd, exiting");
            return Ok(());
        }
        if n as usize != std::mem::size_of::<uffd::UffdMsg>() {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "Short read from userfaultfd",
            ));
        }

        if msg.event != crate::userfaultfd::UFFD_EVENT_PAGEFAULT {
            continue;
        }

        let fault_addr = msg.pf_address;
        let fault = backend.locate_fault(fault_addr).ok_or_else(|| {
            io::Error::other(format!(
                "template UFFD handler: fault at {fault_addr:#x} does not belong to any registered range",
            ))
        })?;
        let page_index = fault.page_index as usize;
        info!(
            "template UFFD fault: addr={:#x} page_addr={:#x} page_index={} backend_offset={} backend_offset_hex={:#x} page_size={}",
            fault_addr,
            fault.page_addr,
            fault.page_index,
            fault.backend_offset,
            fault.backend_offset,
            fault.page_size
        );

        match backend.pages[page_index].state.compare_exchange(
            STATE_UNLOADED,
            STATE_LOADING,
            Ordering::AcqRel,
            Ordering::Acquire,
        ) {
            Ok(_) => {
                backend.read_page(&fault, &mut page_buf)?;
                loop {
                    match uffd::copy(
                        uffd_fd.as_fd(),
                        fault.page_addr,
                        page_buf.as_ptr(),
                        fault.page_size,
                    ) {
                        Ok(()) => {
                            backend.pages[page_index]
                                .state
                                .store(STATE_SHARED_READY, Ordering::Release);
                            pages_served += 1;
                            break;
                        }
                        Err(e) if e.raw_os_error() == Some(libc::EEXIST) => {
                            backend.pages[page_index]
                                .state
                                .store(STATE_SHARED_READY, Ordering::Release);
                            if let Err(e) =
                                uffd::wake(uffd_fd.as_fd(), fault.page_addr, fault.page_size)
                            {
                                warn!("UFFDIO_WAKE failed at {:#x}: {e}", fault.page_addr);
                            }
                            pages_served += 1;
                            break;
                        }
                        Err(e) if e.raw_os_error() == Some(libc::EAGAIN) => {
                            // The kernel can report a transient EAGAIN while the fault
                            // is being resolved; yield and retry instead of aborting restore.
                            thread::yield_now();
                        }
                        Err(e) => return Err(e),
                    }
                }
            }
            Err(STATE_LOADING) => {
                while backend.pages[page_index].state.load(Ordering::Acquire) != STATE_SHARED_READY
                {
                    thread::yield_now();
                }
                if let Err(e) = uffd::wake(uffd_fd.as_fd(), fault.page_addr, fault.page_size) {
                    warn!("UFFDIO_WAKE failed at {:#x}: {e}", fault.page_addr);
                }
            }
            Err(STATE_SHARED_READY) => {
                if let Err(e) = uffd::wake(uffd_fd.as_fd(), fault.page_addr, fault.page_size) {
                    warn!("UFFDIO_WAKE failed at {:#x}: {e}", fault.page_addr);
                }
            }
            Err(other) => {
                return Err(io::Error::other(format!(
                    "template UFFD handler: unexpected state {other} for page {}",
                    fault.page_index
                )));
            }
        }

        if pages_served == total_pages {
            info!("template UFFD handler: all {pages_served} pages served, exiting");
            return Ok(());
        }
    }
}
