#!/bin/bash
# Host-side launcher for the tmux functional test lab.
# Builds the Docker image if missing, allocates hugepages, then drops
# into the container running tests/run_vdev.sh (tmux, 3 panes).

set -euo pipefail

IMAGE=dpdk-forwarder
HUGEPAGES=256
OUTPUT_DIR="$(pwd)/output"

# ── Hugepages ──────────────────────────────────────────────────────────────
HP_FILE=/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
current_hp=$(cat "$HP_FILE" 2>/dev/null || echo 0)
if [ "$current_hp" -lt "$HUGEPAGES" ]; then
    echo "Allocating $HUGEPAGES hugepages (current: $current_hp)…"
    echo "$HUGEPAGES" | sudo tee "$HP_FILE" >/dev/null
fi

# ── Docker image ───────────────────────────────────────────────────────────
if ! docker image inspect "$IMAGE" &>/dev/null; then
    echo "Image '$IMAGE' not found — building…"
    docker build -t "$IMAGE" .
fi

# ── Output directory ───────────────────────────────────────────────────────
mkdir -p "$OUTPUT_DIR"

# ── Launch ─────────────────────────────────────────────────────────────────
echo "Starting tmux test lab inside container…"
exec docker run --rm -it --privileged \
    -v /dev/hugepages:/dev/hugepages \
    -v "$OUTPUT_DIR":/output \
    "$IMAGE" \
    bash /tests/run_vdev.sh
