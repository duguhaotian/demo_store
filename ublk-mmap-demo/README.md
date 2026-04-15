# ublk mmap Demo

Demonstrates mmap on ublk block device with page fault triggering ublk IO chain.

## Architecture

```
Test Program mmap /dev/ublkb0
        ↓ Page Fault
    ublk IO Request
        ↓
    ublksrv
        ↓
    Sparse File backend.data
```

## Requirements

- Linux kernel 6.0+ (ublk support)
- liburing (io_uring library)
- Root privileges (ublk device creation)

## Build

```bash
make
```

## Run Demo

```bash
# Setup: create sparse file and start ublksrv
./scripts/setup.sh

# Run test program
./test/test_mmap

# Cleanup
./scripts/cleanup.sh
```

## What This Demonstrates

1. ublk device creation with sparse file backend
2. mmap on block device triggers page faults
3. Page fault → ublk IO → ublksrv → sparse file read chain
4. Data flows back to fill the page