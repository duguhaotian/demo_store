use std::ffi::c_void;
use std::fs::File;
use std::io::Write;
use std::os::fd::RawFd;
use std::ptr;

use template_memory_demo::{
    Checkpoint, Client, Layer, OverlayBackend, Result, TemplatePageState, TemplateService,
    bitmap_from, bitmap_hex, page_size,
};

const REGION_SIZE: usize = 64 * 1024;
const PREVIEW_LEN: usize = 48;
const WRITE_PAGES: [usize; 3] = [6, 8, 11];
const STDOUT_FILENO: i32 = 1;
const EINTR: i32 = 4;
const EBADF: i32 = 9;

fn main() {
    if let Err(err) = run() {
        eprintln!("error: {err}");
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    let page_size = page_size()?;
    let page_count = REGION_SIZE / page_size;
    let backend = create_demo_backend(page_size, page_count)?;
    let service = TemplateService::new(REGION_SIZE, backend)?;
    let mapping = service.map_client_region()?;
    let base = mapping.as_mut_ptr();

    log_line(format_args!(
        "[parent pid={}] region={}KB pages={} page_size={} base={base:p}\n",
        std::process::id(),
        service.region_size() / 1024,
        service.page_count(),
        service.page_size()
    ));
    log_line(format_args!(
        "[parent pid={}] sparse write pages: {:?}\n",
        std::process::id(),
        WRITE_PAGES
    ));

    let mut notify = [0; 2];
    let mut control = [0; 2];
    pipe_checked(&mut notify)?;
    pipe_checked(&mut control)?;

    let child = unsafe { fork() };
    if child < 0 {
        return Err(last_io("fork"));
    }
    if child == 0 {
        let _ = close_fd(notify[0]);
        let _ = close_fd(control[1]);
        let status = match child_process(&service, base, notify[1], control[0]) {
            Ok(()) => 0,
            Err(err) => {
                eprintln!("child error: {err}");
                1
            }
        };
        let _ = close_fd(notify[1]);
        let _ = close_fd(control[0]);
        unsafe { _exit(status) };
    }

    close_fd(notify[1])?;
    close_fd(control[0])?;

    expect_token(notify[0], b'B', "before parent read preview")?;
    log_before_read_fault("parent", base, &service, 0)?;
    write_token(control[1], b'R')?;

    expect_token(notify[0], b'L', "after child read")?;
    log_mapping_page("parent-after-child-read", 0, base, service.page_size());
    write_token(control[1], b'W')?;

    expect_token(notify[0], b'D', "after child write")?;
    for page_idx in WRITE_PAGES {
        log_mapping_page(
            "parent-after-child-write",
            page_idx,
            base,
            service.page_size(),
        );
    }
    dump_region_layout(
        "parent",
        unsafe { getpid() },
        base as usize,
        service.region_size(),
    )?;
    dump_region_layout("child", child, base as usize, service.region_size())?;

    write_token(control[1], b'Q')?;
    let mut status = 0;
    if unsafe { waitpid(child, &mut status, 0) } < 0 {
        return Err(last_io("waitpid"));
    }
    if !wifexited(status) || wexitstatus(status) != 0 {
        return Err(
            std::io::Error::other(format!("child exited abnormally: status={status}")).into(),
        );
    }

    close_fd(notify[0])?;
    close_fd(control[1])?;
    log_line(format_args!(
        "[parent pid={}] demo completed successfully\n",
        std::process::id()
    ));
    Ok(())
}

fn child_process(
    service: &TemplateService,
    base: *mut u8,
    notify_fd: RawFd,
    control_fd: RawFd,
) -> Result<()> {
    let mut client = Client::new(service, base)?;
    client.start_handler();

    log_before_read_fault("child", base, service, 0)?;
    write_token(notify_fd, b'B')?;

    expect_token(control_fd, b'R', "before child read")?;
    log_line(format_args!(
        "[child pid={}] trigger read fault on page0\n",
        std::process::id()
    ));
    log_mapping_page("child-after-read", 0, base, service.page_size());
    write_token(notify_fd, b'L')?;

    expect_token(control_fd, b'W', "before child write")?;
    log_line(format_args!(
        "[child pid={}] prefault sparse write pages\n",
        std::process::id()
    ));
    for page_idx in WRITE_PAGES {
        let byte = unsafe { ptr::read_volatile(base.add(page_idx * service.page_size())) };
        let _ = byte;
        log_mapping_page("child-prefaulted", page_idx, base, service.page_size());
    }

    for (idx, page_idx) in WRITE_PAGES.iter().enumerate() {
        let value = b'X' + idx as u8;
        log_line(format_args!(
            "[child pid={}] trigger write fault on page{page_idx}[0] = '{}'\n",
            std::process::id(),
            value as char
        ));
        unsafe {
            ptr::write(base.add(page_idx * service.page_size()), value);
        }
    }

    for page_idx in WRITE_PAGES {
        log_mapping_page("child-after-write", page_idx, base, service.page_size());
    }

    let checkpoint = client.export_checkpoint();
    log_checkpoint(&checkpoint, service.page_size());
    write_token(notify_fd, b'D')?;

    expect_token(control_fd, b'Q', "before child exit")?;
    client.stop_handler()?;
    Ok(())
}

fn create_demo_backend(page_size: usize, page_count: usize) -> Result<OverlayBackend> {
    let base_path = "/tmp/template-memory-demo-base.bin";
    let delta_path = "/tmp/template-memory-demo-delta.bin";
    create_layer_file(base_path, page_size, page_count, "base", |_| true)?;
    create_layer_file(delta_path, page_size, page_count, "delta", |idx| {
        matches!(idx, 2 | 6 | 11)
    })?;

    OverlayBackend::new(
        page_size,
        page_count,
        vec![
            Layer::new(
                "base",
                File::open(base_path)?,
                bitmap_from(page_count, |_| true),
            ),
            Layer::new(
                "delta",
                File::open(delta_path)?,
                bitmap_from(page_count, |idx| matches!(idx, 2 | 6 | 11)),
            ),
        ],
    )
}

fn create_layer_file(
    path: &str,
    page_size: usize,
    page_count: usize,
    label: &str,
    present: impl Fn(usize) -> bool,
) -> Result<()> {
    let mut file = File::create(path)?;
    let mut page = vec![0; page_size];
    for page_idx in 0..page_count {
        page.fill(0);
        if present(page_idx) {
            fill_page_pattern(&mut page, page_idx, label);
        }
        file.write_all(&page)?;
    }
    file.sync_all()?;
    Ok(())
}

fn fill_page_pattern(page: &mut [u8], page_idx: usize, label: &str) {
    let fill = b'A' + (page_idx % 26) as u8;
    let header = format!(
        "page{page_idx:02}: {label} overlay payload ({})",
        fill as char
    );
    let header = header.as_bytes();
    let n = header.len().min(page.len());
    page[..n].copy_from_slice(&header[..n]);
    if n < page.len() {
        page[n] = 0;
        for byte in &mut page[n + 1..] {
            *byte = fill;
        }
    }
}

fn log_checkpoint(checkpoint: &Checkpoint, page_size: usize) {
    log_line(format_args!(
        "[checkpoint pid={}] dirty_pages={} private_bitmap={} merged_backend_bitmap={}\n",
        std::process::id(),
        checkpoint.dirty_pages.len(),
        bitmap_hex(&checkpoint.private_bitmap),
        bitmap_hex(&checkpoint.merged_backend_bitmap)
    ));
    for page in &checkpoint.dirty_pages {
        let preview = preview(&page.data[..page_size.min(PREVIEW_LEN)]);
        log_line(format_args!(
            "[checkpoint pid={}] page{} len={} preview=\"{}\"\n",
            std::process::id(),
            page.page_idx,
            page.data.len(),
            preview
        ));
    }
}

fn log_before_read_fault(
    who: &str,
    base: *const u8,
    service: &TemplateService,
    page_idx: usize,
) -> Result<()> {
    let state = service.page_state(page_idx);
    let mapping_view = if state == TemplatePageState::SharedReady {
        preview_from_mapping(base, service.page_size(), page_idx)
    } else {
        "<UNLOADED>".to_string()
    };
    let source_view = source_preview(service, page_idx)?;
    log_line(format_args!(
        "[{who} pid={}] before read fault: page{page_idx} mapped-view={mapping_view}; source-view=\"{source_view}\"\n",
        std::process::id()
    ));
    Ok(())
}

fn source_preview(service: &TemplateService, page_idx: usize) -> Result<String> {
    let mut page = vec![0; service.page_size()];
    service.backend().read_page(page_idx, &mut page)?;
    Ok(preview(&page[..PREVIEW_LEN.min(page.len())]))
}

fn log_mapping_page(who: &str, page_idx: usize, base: *const u8, page_size: usize) {
    let content = preview_from_mapping(base, page_size, page_idx);
    log_line(format_args!(
        "[{who} pid={}] page{page_idx} content=\"{content}\"\n",
        std::process::id()
    ));
}

fn preview_from_mapping(base: *const u8, page_size: usize, page_idx: usize) -> String {
    let len = PREVIEW_LEN.min(page_size);
    let ptr = unsafe { base.add(page_idx * page_size) };
    let bytes = unsafe { std::slice::from_raw_parts(ptr, len) };
    preview(bytes)
}

fn preview(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|&b| {
            if (32..=126).contains(&b) {
                b as char
            } else {
                '.'
            }
        })
        .collect()
}

fn pipe_checked(fds: &mut [RawFd; 2]) -> Result<()> {
    if unsafe { pipe(fds.as_mut_ptr()) } < 0 {
        return Err(last_io("pipe"));
    }
    Ok(())
}

fn write_token(fd: RawFd, token: u8) -> Result<()> {
    write_all_fd(fd, &[token])
}

fn expect_token(fd: RawFd, expected: u8, context: &str) -> Result<()> {
    let mut token = [0];
    read_exact_fd(fd, &mut token)?;
    if token[0] != expected {
        return Err(std::io::Error::other(format!(
            "unexpected token {:#x} {context}, expected {:#x}",
            token[0], expected
        ))
        .into());
    }
    Ok(())
}

fn write_all_fd(fd: RawFd, mut buf: &[u8]) -> Result<()> {
    while !buf.is_empty() {
        let nwritten = unsafe { write(fd, buf.as_ptr().cast::<c_void>(), buf.len()) };
        if nwritten < 0 {
            let err = std::io::Error::last_os_error();
            if matches!(err.raw_os_error(), Some(EINTR)) {
                continue;
            }
            return Err(err.into());
        }
        buf = &buf[nwritten as usize..];
    }
    Ok(())
}

fn read_exact_fd(fd: RawFd, mut buf: &mut [u8]) -> Result<()> {
    while !buf.is_empty() {
        let nread = unsafe { read(fd, buf.as_mut_ptr().cast::<c_void>(), buf.len()) };
        if nread < 0 {
            let err = std::io::Error::last_os_error();
            if matches!(err.raw_os_error(), Some(EINTR)) {
                continue;
            }
            return Err(err.into());
        }
        if nread == 0 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::UnexpectedEof,
                "unexpected EOF while reading pipe",
            )
            .into());
        }
        let nread = nread as usize;
        buf = &mut buf[nread..];
    }
    Ok(())
}

fn close_fd(fd: RawFd) -> Result<()> {
    if unsafe { close(fd) } < 0 {
        let err = std::io::Error::last_os_error();
        if matches!(err.raw_os_error(), Some(EBADF)) {
            return Ok(());
        }
        return Err(err.into());
    }
    Ok(())
}

fn dump_region_layout(who: &str, pid: i32, region_start: usize, region_len: usize) -> Result<()> {
    let maps = std::fs::read_to_string(format!("/proc/{pid}/maps"))?;
    let region_end = region_start + region_len;
    let mut shared_bytes = 0usize;
    let mut private_bytes = 0usize;
    log_line(format_args!(
        "[{who} pid={pid}] region=[{region_start:#x}-{region_end:#x}) VMA layout:\n"
    ));

    for line in maps.lines() {
        let mut fields = line.split_whitespace();
        let Some(range) = fields.next() else {
            continue;
        };
        let Some(perms) = fields.next() else {
            continue;
        };
        let Some((start, end)) = parse_maps_range(range) else {
            continue;
        };
        if end <= region_start || start >= region_end {
            continue;
        }
        let overlap_start = start.max(region_start);
        let overlap_end = end.min(region_end);
        let overlap_len = overlap_end - overlap_start;
        match perms.as_bytes().get(3).copied() {
            Some(b's') => shared_bytes += overlap_len,
            Some(b'p') => private_bytes += overlap_len,
            _ => {}
        }
        log_line(format_args!("[{who} pid={pid}]   {line}\n"));
    }
    log_line(format_args!(
        "[{who} pid={pid}] shared={}KB private={}KB\n",
        shared_bytes / 1024,
        private_bytes / 1024
    ));
    Ok(())
}

fn parse_maps_range(range: &str) -> Option<(usize, usize)> {
    let (start, end) = range.split_once('-')?;
    let start = usize::from_str_radix(start, 16).ok()?;
    let end = usize::from_str_radix(end, 16).ok()?;
    Some((start, end))
}

fn wifexited(status: i32) -> bool {
    status & 0x7f == 0
}

fn wexitstatus(status: i32) -> i32 {
    (status >> 8) & 0xff
}

fn log_line(args: std::fmt::Arguments<'_>) {
    let line = args.to_string();
    let _ = write_all_fd(STDOUT_FILENO, line.as_bytes());
}

fn last_io(what: &str) -> template_memory_demo::Error {
    template_memory_demo::Error::Io(std::io::Error::other(format!(
        "{what}: {}",
        std::io::Error::last_os_error()
    )))
}

unsafe extern "C" {
    fn pipe(fds: *mut RawFd) -> i32;
    fn close(fd: RawFd) -> i32;
    fn fork() -> i32;
    fn waitpid(pid: i32, status: *mut i32, options: i32) -> i32;
    fn getpid() -> i32;
    fn _exit(status: i32) -> !;
    fn read(fd: RawFd, buf: *mut c_void, count: usize) -> isize;
    fn write(fd: RawFd, buf: *const c_void, count: usize) -> isize;
}
