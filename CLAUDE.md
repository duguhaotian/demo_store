# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Purpose

This is a demo store repository for collecting and managing various demo projects and code examples.

## Directory Structure Convention

**IMPORTANT: When creating new demos, always create them in independent/separate directories.**

Each demo should have its own isolated directory with:
- A descriptive directory name (e.g., `rust-web-server/`, `python-data-pipeline/`, `cli-tool-demo/`)
- All demo-specific files contained within that directory
- Optional README.md within each demo directory to explain the demo's purpose

Example structure:
```
demo_store/
├── rust-http-server/
│   ├── Cargo.toml
│   ├── src/
│   └── README.md
├── python-cli-demo/
│   ├── main.py
│   └── requirements.txt
└── async-rust-demo/
│   ├── Cargo.toml
│   └── src/
```

Do not place demo files directly in the repository root. Always use dedicated subdirectories.

## Language Context

The .gitignore is configured for Rust projects, but demos in any language are acceptable. Adjust ignore patterns if adding demos in other languages.

## Workflow

- Create new demos in their own directories
- Each demo should be self-contained and runnable independently
- Commit demos individually with clear commit messages describing what the demo demonstrates