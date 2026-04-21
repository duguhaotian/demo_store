#!/bin/bash
# Generate test snapshot files

OUTPUT_DIR=${1:-"test_snapshot"}
SIZE_MB=${2:-4}

mkdir -p "$OUTPUT_DIR"

# Generate 4MB test snapshot with repeating patterns
total_pages=$((SIZE_MB * 256))
page_size=4096

# Create snapshot.mem with repeating patterns (for dedup test)
dd if=/dev/zero bs=$page_size count=$total_pages of="$OUTPUT_DIR/snapshot.mem" 2>/dev/null

# Add some unique pages
for i in 1 2 3 4; do
    offset=$((i * page_size))
    dd if=/dev/urandom bs=$page_size count=1 of="$OUTPUT_DIR/snapshot.mem" \
       seek=$((offset / page_size)) conv=notrunc 2>/dev/null
done

# Run split tool
split_snapshot "$OUTPUT_DIR/snapshot.mem" "$OUTPUT_DIR"

echo "Generated test snapshot in $OUTPUT_DIR"
ls -la "$OUTPUT_DIR"