#!/bin/bash
# Manual functional test — run inside the Docker container.
#
# Layout (4 tmux panes):
#   pane 0 (top-left)  : dpdk-forwarder   — net_virtio_user server=1 (MASTER)
#   pane 1 (top-right) : testpmd-A txonly — net_vhost client=1 (SLAVE, RX socket)
#   pane 2 (bot-left)  : testpmd-B rxonly — net_vhost client=1 (SLAVE, TX socket)
#   pane 3 (bot-right) : live tail of CSV output
#
# Two testpmd instances are used to avoid the sequential-accept ordering problem:
#   net_virtio_user server=1 blocks in accept() per-port during EAL probe, so
#   the TX socket only exists AFTER something connects to the RX socket first.
#   testpmd-A connects to RX (unblocks the forwarder), then we wait for the TX
#   socket before starting testpmd-B.

set -euo pipefail

SOCK_RX=/tmp/fwd_rx.sock
SOCK_TX=/tmp/fwd_tx.sock
OUTPUT_DIR=/output
SESSION=fwd

rm -f "$SOCK_RX" "$SOCK_TX"
mkdir -p "$OUTPUT_DIR"

if ! command -v tmux &>/dev/null; then
    echo "tmux not found; install it in the container first" >&2
    exit 1
fi

tmux kill-session -t "$SESSION" 2>/dev/null || true
tmux new-session -d -s "$SESSION" -x 240 -y 55

# ── Pane 0: forwarder (MASTER, creates both sockets in server=1 mode) ──────
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

# Wait for RX socket (forwarder's EAL probe is blocking on first accept())
echo "Waiting for RX socket..."
for i in $(seq 1 30); do
    [ -S "$SOCK_RX" ] && break
    sleep 0.5
done
[ -S "$SOCK_RX" ] || { echo "ERROR: forwarder did not create $SOCK_RX" >&2; exit 1; }

# ── Pane 1: testpmd-A — traffic generator (SLAVE, connects to RX socket) ───
# Using net_vhost client=1 (SLAVE) to speak to the forwarder's MASTER socket.
# This connection unblocks the forwarder so it can proceed to create the TX socket.
tmux split-window -h -t "$SESSION":0
tmux send-keys -t "$SESSION":0.1 \
    "dpdk-testpmd \
      -l 4-5 -n 4 --socket-mem 256 --file-prefix genA \
      --vdev 'net_vhost0,iface=${SOCK_RX},queues=4,client=1' \
      -- --rxq=4 --txq=4 --nb-cores=1 \
         --forward-mode=txonly --auto-start" \
    Enter

# Wait for TX socket (created by forwarder after its first accept() returns)
echo "Waiting for TX socket..."
for i in $(seq 1 30); do
    [ -S "$SOCK_TX" ] && break
    sleep 0.5
done
[ -S "$SOCK_TX" ] || { echo "ERROR: forwarder did not create $SOCK_TX" >&2; exit 1; }

# ── Pane 2: testpmd-B — traffic sink (SLAVE, connects to TX socket) ────────
tmux split-window -v -t "$SESSION":0.0
tmux send-keys -t "$SESSION":0.2 \
    "dpdk-testpmd \
      -l 6-7 -n 4 --socket-mem 256 --file-prefix genB \
      --vdev 'net_vhost0,iface=${SOCK_TX},queues=4,client=1' \
      -- --rxq=4 --txq=4 --nb-cores=1 \
         --forward-mode=rxonly --auto-start" \
    Enter

# ── Pane 3: CSV monitor ────────────────────────────────────────────────────
tmux split-window -v -t "$SESSION":0.1
tmux send-keys -t "$SESSION":0.3 \
    "watch -n2 'tail -5 ${OUTPUT_DIR}/flow_stats_core_*.csv 2>/dev/null || echo \"no CSV yet\"'" \
    Enter

tmux attach -t "$SESSION"
