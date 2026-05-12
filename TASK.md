# Task: DPDK-Based Packet Forwarder with Flow Tracking

Build a high-performance C application using DPDK that captures packets on one network interface, forwards them to another, and tracks per-flow statistics.

---

## Requirements

### 1. Environment
- Language: **C**
- Framework: **DPDK** (high-performance poll-mode packet processing)
- Target OS: Linux with DPDK installed

### 2. Core Functionality
- Initialize DPDK and allocate mbuf memory pools
- Bind to two network interfaces (RX port and TX port)
- Receive packets on one interface, forward to the other

### 3. Packet Processing
- Parse Ethernet/IPv4 headers from each received packet
- **Drop** non-IPv4 packets silently
- Optionally rewrite the destination MAC address
- Forward valid packets to the TX interface

### 4. Performance
- Use multiple RX/TX queues for parallel per-core processing
- Batch processing via `rte_eth_rx/tx_burst()`
- Cache-friendly memory access patterns

### 5. Flow Tracking & Affinity
- Track flows by **5-tuple**: `(src_ip, dst_ip, src_port, dst_port, protocol)`
- **Flow affinity**: all packets of the same flow must be handled by the same worker core
- Use **RSS** (Receive Side Scaling) or a consistent hash to distribute flows across queues

### 6. Per-Flow Statistics
Collect for each flow, updated in real-time:

| Metric | Description |
|--------|-------------|
| `rx_bytes` | Inbound bytes |
| `tx_bytes` | Outbound bytes |
| `rx_packets` | Inbound packet count |
| `tx_packets` | Outbound packet count |

### 7. Periodic Statistics Export
- One CSV file per worker core: `flow_stats_core_<core_id>.csv`
- Append one row per active flow at each export interval
- Row format: `timestamp, src_ip, dst_ip, src_port, dst_port, protocol, rx_bytes, tx_bytes, rx_packets, tx_packets`
- A flow produces multiple rows over time (cumulative counters)
- Export interval is **configurable** via CLI

### 8. Flow Timeout & Cleanup
- Remove flows inactive longer than a configurable timeout
- Timeout is **configurable** via CLI

### 9. Logging
- Debug/info logging throughout for observability

---

## Deliverables

- [ ] C source implementing the forwarder with flow tracking
- [ ] `README.md` covering:
  - Build and run instructions
  - Dependency list
  - Flow tracking design and CSV export format
  - Expected output and verification steps
  - *(Optional)* Performance report at 64/256/512/1518-byte packet sizes

---

## Bonus (Optional)

1. Multiple cores with separate RX/TX threads for higher throughput
2. Configurable flow table capacity with graceful drop when full

---

## Evaluation Criteria

- Code correctness and stability
- Understanding of DPDK concepts
- Correct flow affinity implementation
- Accurate per-flow statistics
- Code efficiency and performance
- Best practices in low-level networking
- Documentation quality
