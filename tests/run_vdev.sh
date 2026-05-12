#!/bin/bash
# Manual functional test — run inside the Docker container.
# Starts a tmux session with 3 panes:
#   pane 0: dpdk-forwarder (net_virtio_user, server mode)
#   pane 1: dpdk-testpmd  (net_vhost, client, txonly traffic generator)
#   pane 2: live tail of CSV output files

set -euo pipefail

SOCK_RX=/tmp/fwd_rx.sock
SOCK_TX=/tmp/fwd_tx.sock
OUTPUT_DIR=/output
SESSION=fwd

# Clean up stale sockets
rm -f "$SOCK_RX" "$SOCK_TX"
mkdir -p "$OUTPUT_DIR"

# Require tmux
if ! command -v tmux &>/dev/null; then
    echo "tmux not found; install it in the container first" >&2
    exit 1
fi

# Kill existing session if any
tmux kill-session -t "$SESSION" 2>/dev/null || true

tmux new-session -d -s "$SESSION" -x 240 -y 55

# ── Pane 0: forwarder (server=1 creates the sockets) ──────────────────────
tmux send-keys -t "$SESSION":0.0 \
    "dpdk-forwarder \
      -l 0-3 -n 4 --socket-mem 512 --file-prefix fwd \
      --vdev 'net_virtio_user0,path=${SOCK_RX},queues=4,queue_size=1024,server=1' \
      --vdev 'net_virtio_user1,path=${SOCK_TX},queues=4,queue_size=1024,server=1' \
      -- \
      --rx-port 0 --tx-port 1 --workers 3 \
      --stats-interval 5 --flow-timeout 30 \
      --output-dir ${OUTPUT_DIR}" \
    Enter

# Wait for sockets to appear
echo "Waiting for forwarder to create sockets..."
for i in $(seq 1 15); do
    [ -S "$SOCK_RX" ] && [ -S "$SOCK_TX" ] && break
    sleep 1
done
if [ ! -S "$SOCK_RX" ]; then
    echo "ERROR: forwarder did not create $SOCK_RX in time" >&2
    exit 1
fi

# ── Pane 1: testpmd traffic generator (client=1) ──────────────────────────
tmux split-window -h -t "$SESSION":0
tmux send-keys -t "$SESSION":0.1 \
    "dpdk-testpmd \
      -l 4-7 -n 4 --socket-mem 512 --file-prefix gen \
      --vdev 'net_vhost0,iface=${SOCK_RX},queues=4,client=1' \
      --vdev 'net_vhost1,iface=${SOCK_TX},queues=4,client=1' \
      -- --rxq=4 --txq=4 --nb-cores=3 \
         --forward-mode=txonly --auto-start" \
    Enter

# ── Pane 2: CSV monitor ────────────────────────────────────────────────────
tmux split-window -v -t "$SESSION":0.0
tmux send-keys -t "$SESSION":0.2 \
    "watch -n2 'tail -5 ${OUTPUT_DIR}/flow_stats_core_*.csv 2>/dev/null || echo \"no CSV yet\"'" \
    Enter

tmux attach -t "$SESSION"
