SHELL := /usr/bin/env bash

CH_DIR := cloud-hypervisor
CH_FEATURES ?= kvm

.PHONY: help
help:
	@printf '%s\n' \
		'Targets:' \
		'  make fmt                 Format root helper and cloud-hypervisor workspace' \
		'  make check               Check root helper and cloud-hypervisor vmm' \
		'  make clippy              Run clippy for cloud-hypervisor vmm' \
		'  make build               Build root helper plus cloud-hypervisor/ch-remote' \
		'  make demo                Run the standalone Rust UFFD/COW demo' \
		'  make template-smoke      Smoke-test snapshot -> template conversion' \
		'  make e2e                 Run template restore E2E; requires KERNEL=... DISK=...' \
		'' \
		'Variables:' \
		'  CH_FEATURES=kvm          cloud-hypervisor feature set' \
		'  KERNEL=/path/to/vmlinux  required by make e2e' \
		'  DISK=/path/to/rootfs     required by make e2e' \
		'  SECCOMP=false            seccomp mode for e2e cloud-hypervisor processes' \
		'  SKIP_UFFD_PREFLIGHT=0    skip e2e userfaultfd permission preflight when set to 1' \
		'  CH_VERBOSE=-v            cloud-hypervisor verbosity for e2e logs' \
		'  WAIT_FOR_CONFIRM=1       wait for Enter before e2e cleanup when interactive' \
		'  TEMPLATE_METRICS_LOG=... optional template service metrics log path' \
		'  METRICS_SETTLE_SECONDS=2 wait time before collecting template metrics' \
		'  RESTORE_PAUSE_BEFORE_SUMMARY=1 pause restored VM before metrics summary'

.PHONY: fmt
fmt:
	cargo fmt
	cd $(CH_DIR) && cargo fmt

.PHONY: fmt-check
fmt-check:
	cargo fmt --check
	cd $(CH_DIR) && cargo fmt --check

.PHONY: check
check:
	cargo check
	cd $(CH_DIR) && cargo check -p vmm --features $(CH_FEATURES)

.PHONY: clippy
clippy:
	cd $(CH_DIR) && cargo clippy -p vmm --features $(CH_FEATURES)

.PHONY: build
build:
	cargo build --bin template-memory-demo
	cd $(CH_DIR) && cargo build --features $(CH_FEATURES) --bin cloud-hypervisor --bin ch-remote

.PHONY: demo
demo:
	cargo run -- demo

.PHONY: template-smoke
template-smoke:
	@tmp="$$(mktemp -d /tmp/template-convert-smoke.XXXXXX)"; \
	mkdir -p "$$tmp/snapshot"; \
	printf '0123456789abcdef' > "$$tmp/snapshot/memory-ranges"; \
	cargo run --quiet -- convert --snapshot-dir "$$tmp/snapshot" --template-dir "$$tmp/template"; \
	test -f "$$tmp/template/template.manifest"; \
	test -f "$$tmp/template/memory-ranges"; \
	echo "template smoke passed: $$tmp/template"

.PHONY: test
test: fmt-check check clippy template-smoke
	cd $(CH_DIR) && cargo test -p vmm --features $(CH_FEATURES) test_restore_parsing --no-run

.PHONY: e2e
e2e: build
	@if [[ -z "$${KERNEL:-}" || -z "$${DISK:-}" ]]; then \
		echo 'KERNEL and DISK are required. Example:' >&2; \
		echo '  KERNEL=/path/to/vmlinux DISK=/path/to/rootfs.img make e2e' >&2; \
		exit 2; \
	fi
	KERNEL="$${KERNEL}" DISK="$${DISK}" SECCOMP="$${SECCOMP:-false}" SKIP_UFFD_PREFLIGHT="$${SKIP_UFFD_PREFLIGHT:-0}" CH_VERBOSE="$${CH_VERBOSE:--v}" WAIT_FOR_CONFIRM="$${WAIT_FOR_CONFIRM:-1}" METRICS_SETTLE_SECONDS="$${METRICS_SETTLE_SECONDS:-2}" RESTORE_PAUSE_BEFORE_SUMMARY="$${RESTORE_PAUSE_BEFORE_SUMMARY:-1}" ./scripts/template_restore_e2e.sh
