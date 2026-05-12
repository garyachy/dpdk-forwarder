# DPDK Packet Forwarder — Functional Specification

## 1. Overview

The DPDK Packet Forwarder is a high-performance, multi-core C application that receives IPv4 packets on one network port, tracks per-flow statistics, and forwards packets to a second port. It is designed for line-rate processing using DPDK's lockless, poll-mode I/O model.

The application runs entirely inside Docker. DPDK is included as a git submodule and built from source — no host DPDK installation is required.

### 1.1 Scope

- Ingress on one DPDK port; egress on a second DPDK port.
- IPv4 only. Non-IPv4 packets are dropped silently.
- Per-flow tracking keyed on the 5-tuple: `(src_ip, dst_ip, src_port, dst_port, protocol)`.
- Periodic per-flow statistics export to per-core CSV files.
- Flow expiry for inactive flows.
- Tested with `net_virtio_user` virtual PMD (genuine RSS support) inside Docker using tmux.

### 1.2 Non-Goals

- No IPv6 support.
- No GRE, VLAN, MPLS, or tunnel decapsulation.
- No IP reassembly or TCP state tracking.
- No control plane, REST API, or runtime reconfiguration.
- No encryption or authentication.
- No NAT or packet modification beyond optional destination MAC rewrite.
- No persistence of flow state across restarts.

---

## 2. System Architecture

```
                    ┌─────────────────────────────────────────────┐
                    │              Docker Container               │
                    │                                             │
  ┌──────────┐      │  ┌─────────┐    ┌──────────────────────┐  │
  │ testpmd  │      │  │  main   │    │  Worker lcore 1      │  │
  │ (txonly) │◄────►│  │ lcore 0 │    │  RX queue 0          │  │
  │ net_vhost│      │  │         │    │  Flow table (core 1) │  │
  └──────────┘      │  │ init    │    │  CSV: core_1.csv     │  │
  Unix socket        │  │ launch  │    └──────────────────────┘  │
  (fwd_rx.sock)     │  │ SIGINT  │    ┌──────────────────────┐  │
                    │  └─────────┘    │  Worker lcore 2      │  │
  ┌──────────┐      │                 │  RX queue 1          │  │
  │ testpmd  │      │                 │  Flow table (core 2) │  │
  │ (sink)   │◄────►│                 │  CSV: core_2.csv     │  │
  │ net_vhost│      │                 └──────────────────────┘  │
  └──────────┘      │                         ...               │
  Unix socket        └─────────────────────────────────────────────┘
  (fwd_tx.sock)
```

### 2.1 Lcore Roles

| Lcore | Role | Responsibilities |
|-------|------|-----------------|
| 0 | Main | EAL init, port init, mbuf pool allocation, worker launch, SIGINT/SIGTERM handler |
| 1..N | Worker | RX burst, header parse, flow lookup/create, TX burst, inline stats export |

No dedicated stats-exporter lcore. Export is triggered inline within each worker loop, gated by a TSC timestamp comparison. This avoids any cross-core synchronization.

### 2.2 Per-Core Data Ownership

Each worker lcore owns exclusively:
- Its RX queue (queue index = lcore index 0..N-1)
- Its TX queue (same index)
- Its flow hash table instance (`rte_hash`, named `flow_core_<lcore_id>`)
- Its flow entry array (flat, cache-line-aligned)
- Its CSV output file handle

No shared mutable state between worker lcores. The only shared state is `volatile bool force_quit` (written by main, read by workers) — a single-word write with no ABA concern.

---

## 3. Flow Lifecycle

```
                   packet arrives
                        │
                        ▼
              ┌─────────────────┐
              │   LOOKUP in     │
              │   flow table    │
              └────────┬────────┘
                       │
           ┌───────────┴────────────┐
           │ HIT                    │ MISS
           ▼                        ▼
   ┌───────────────┐      ┌──────────────────┐
   │  ACTIVE flow  │      │ table full?       │
   │  update stats │      └────┬─────────────┘
   │  last_seen_tsc│           │ NO        │ YES
   └───────┬───────┘           ▼           ▼
           │          ┌──────────────┐  drop pkt
           │          │ CREATE flow  │  log WARN
           │          │ init stats   │  (once per interval)
           │          └──────┬───────┘
           │                 │
           └────────┬────────┘
                    ▼
           forward packet (TX)
                    │
                    ▼
         ┌──────────────────────┐
         │  export interval?    │◄── TSC check each loop iteration
         └──────────┬───────────┘
                    │ YES
                    ▼
         ┌──────────────────────┐
         │  iterate all flows   │
         │  write CSV row       │
         │  check last_seen_tsc │
         │  → expire if stale   │
         └──────────────────────┘
```

### 3.1 Flow States

| State | Condition |
|-------|-----------|
| Active | `last_seen_tsc` within `flow_timeout` of current TSC |
| Expired | `now_tsc - last_seen_tsc > timeout_tsc`; deleted during next export pass |

There is no explicit "closed" state. Flows are removed lazily during the export/expire pass.

---

## 4. RSS Flow Affinity Contract

The application configures `net_virtio_user` with a **symmetric Toeplitz RSS key** (52 bytes). This guarantees:

1. All packets with the same 5-tuple are delivered to the same RX queue.
2. The owner worker lcore is determined by the NIC hardware (vhost RSS negotiation) — not by software.
3. A given lcore processes only the flows whose RSS hash maps to its queue index.
4. Each flow appears in exactly one per-core CSV file.

RSS hash types enabled: `RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_NONFRAG_IPV4_UDP | RTE_ETH_RSS_NONFRAG_IPV4_OTHER`.

RETA size: 128 entries, mapped round-robin to queues `0..nb_workers-1`.

**Symmetric key property**: `RSS(src=A, dst=B) == RSS(src=B, dst=A)`. Forward and return traffic of the same session land on the same queue without a reverse-flow lookup.

---

## 5. CLI Interface

```
dpdk-forwarder [EAL options] -- [application options]
```

EAL options are passed directly to `rte_eal_init()`. Application options follow `--`:

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--rx-port N` | uint16 | 0 | DPDK port index for ingress |
| `--tx-port N` | uint16 | 1 | DPDK port index for egress |
| `--workers N` | uint16 | auto | Number of worker lcores (auto = EAL lcore count - 1) |
| `--burst N` | uint32 | 32 | RX/TX burst size (1–512) |
| `--pool-size N` | uint32 | 8192 | Mbuf pool entries (must be power of 2 minus 1) |
| `--max-flows N` | uint32 | 65536 | Flow table capacity per core |
| `--stats-interval N` | uint32 | 5 | Stats export interval in seconds |
| `--flow-timeout N` | uint32 | 60 | Flow inactivity timeout in seconds |
| `--output-dir PATH` | string | `.` | Directory for CSV output files |
| `--dst-mac XX:XX:XX:XX:XX:XX` | MAC | (none) | Rewrite destination MAC to this address |
| `--log-level N` | int | 2 | 0=ERR, 1=WARN, 2=INFO, 3=DEBUG |

### 5.1 Validation Rules

- `rx-port` and `tx-port` must differ.
- Both ports must be available (`rte_eth_dev_count_avail() > max(rx, tx)`).
- `workers` must be ≤ number of available worker lcores (EAL `-l` mask minus main lcore).
- `burst` must be in [1, 512].
- `pool-size` must satisfy: `pool_size >= workers * burst * 4` (otherwise log WARNING and continue).
- `stats-interval` must be > 0.
- `flow-timeout` must be > 0 and >= `stats-interval`.
- `output-dir` must be writable.

Validation failures on critical parameters (port availability, zero workers) cause `rte_exit()`. Warnings on non-critical parameters (pool size) are logged but do not abort startup.

---

## 6. Packet Processing

### 6.1 Header Parsing

For each received mbuf, the following headers are parsed in-place (zero-copy pointer arithmetic):

```
Ethernet header  (14 bytes)
  └── ether_type == 0x0800 (IPv4)?  → continue; else drop
IPv4 header      (≥ 20 bytes)
  └── next_proto_id:
        TCP  (6)  → extract src_port, dst_port from TCP header
        UDP (17)  → extract src_port, dst_port from UDP header
        other     → src_port = 0, dst_port = 0
```

Non-IPv4 packets are freed immediately with no stat update.

### 6.2 MAC Rewrite

If `--dst-mac` is provided, `rte_ether_addr_copy()` overwrites the Ethernet destination address in-place before TX. This is optional and disabled by default.

### 6.3 TX Overflow Handling

`rte_eth_tx_burst()` may send fewer packets than requested. Unsent packets (indices `nb_sent..nb_tx-1`) are freed immediately via `rte_pktmbuf_free()`. TX stats are updated only for successfully sent packets.

---

## 7. Flow Table

### 7.1 Key

```c
struct flow_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  _pad[3];   /* 12 bytes total, 4-byte aligned */
};
```

`src_port` and `dst_port` are 0 for non-TCP/UDP protocols.

### 7.2 Entry

```c
struct flow_entry {
    struct flow_key key;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t last_seen_tsc;
    uint64_t created_tsc;
};
```

All counters are cumulative since flow creation (not reset on export).

### 7.3 Hash Function

The flow table uses **jhash** (`rte_jhash`) as its hash function. For a 16-byte key, jhash performs a series of XOR/add/rotate mix operations that the compiler can partially pipeline via out-of-order execution. Hardware CRC32 (`rte_hash_crc`) is faster for large keys (≥ 32 bytes) but has a strict serial dependency chain for small keys (each `crc32` instruction feeds the next), making jhash the better choice for our 16-byte 5-tuple.

### 7.4 Capacity and Drop Policy

When `flow_lookup_or_create()` is called and the table is at `max_flows` capacity:
- The packet is dropped (freed).
- A warning is logged at most once per export interval: `"flow table full on core %u, dropping packet"`.
- No LRU eviction. Flows age out only via the timeout mechanism.

---

## 8. Statistics Export

### 8.1 File Naming

```
<output-dir>/flow_stats_core_<lcore_id>.csv
```

One file per worker lcore, opened at startup in append mode (`"a"`). If the file is new (size = 0), a header line is written first.

### 8.2 CSV Format

```
timestamp,src_ip,dst_ip,src_port,dst_port,proto,rx_bytes,tx_bytes,rx_packets,tx_packets
```

| Column | Type | Format | Example |
|--------|------|--------|---------|
| `timestamp` | string | ISO-8601 UTC, second precision | `2026-05-12T14:23:05Z` |
| `src_ip` | string | dotted-decimal | `192.168.1.10` |
| `dst_ip` | string | dotted-decimal | `10.0.0.1` |
| `src_port` | uint16 | decimal | `54321` |
| `dst_port` | uint16 | decimal | `80` |
| `proto` | uint8 | decimal (6=TCP, 17=UDP) | `6` |
| `rx_bytes` | uint64 | decimal | `4096` |
| `tx_bytes` | uint64 | decimal | `4096` |
| `rx_packets` | uint64 | decimal | `10` |
| `tx_packets` | uint64 | decimal | `10` |

Example row:
```
2026-05-12T14:23:05Z,192.168.1.10,10.0.0.1,54321,80,6,4096,4096,10,10
```

### 8.3 Export Semantics

- Export is triggered when `rte_rdtsc() - last_export_tsc > export_tsc_interval`.
- `export_tsc_interval` is pre-computed at startup: `stats_interval_s * rte_get_tsc_hz()`.
- All flows in the table are iterated via `rte_hash_iterate()` and written to CSV.
- Counters are cumulative — the same flow produces one row per export interval showing running totals.
- `fflush()` is called after each export pass.
- Flows inactive for longer than `flow_timeout` are deleted during the same pass (no separate timer).

---

## 9. Logging

All log output goes to stderr via `rte_log()` with logtype `RTE_LOGTYPE_USER1`.

| Level | When Used |
|-------|-----------|
| ERR | Fatal errors before `rte_exit()` |
| WARNING | Flow table full (throttled), unsent packet burst, port config issues |
| INFO | Startup banner, port info, worker lcore assignment, shutdown summary |
| DEBUG | Per-packet trace (disabled in production; use `--log-level 3`) |

Log format: `[function:line] message\n`

---

## 10. Error Handling Contract

| Condition | Behavior |
|-----------|----------|
| `rte_eal_init()` fails | `rte_exit(EXIT_FAILURE, ...)` |
| Port not available | `rte_exit(EXIT_FAILURE, ...)` |
| `rte_pktmbuf_pool_create()` fails | `rte_exit(EXIT_FAILURE, ...)` |
| `port_init()` fails | `rte_exit(EXIT_FAILURE, ...)` |
| `rte_hash_create()` fails | `rte_exit(EXIT_FAILURE, ...)` |
| CSV file open fails | `rte_exit(EXIT_FAILURE, ...)` |
| `rte_hash_add_key()` fails (table full) | drop packet, log WARNING (throttled) |
| `rte_eth_tx_burst()` partial send | free unsent mbufs, continue |
| `fwrite`/`fprintf` to CSV fails | log WARNING, continue (data loss acceptable) |
| SIGINT / SIGTERM | set `force_quit = true`; workers drain and exit; main calls `rte_eth_dev_stop()` |

---

## 11. Shutdown Sequence

1. SIGINT/SIGTERM received by main lcore.
2. `force_quit = true` written (volatile, visible to all lcores).
3. Workers observe `force_quit`, perform one final export pass, close CSV files, return from lcore function.
4. Main calls `rte_eal_wait_lcore()` for each worker.
5. Main calls `rte_eth_dev_stop()` and `rte_eth_dev_close()` for both ports.
6. Main prints final port stats via `port_stats_print()`.
7. `rte_eal_cleanup()` and `exit(0)`.

---

## 12. Build and Test Environment

### 12.1 Dependencies

| Dependency | Version | Source |
|------------|---------|--------|
| DPDK | 23.11 (LTS) | Git submodule (`dpdk/`) |
| GCC | ≥ 11 | Ubuntu 22.04 package |
| Meson | ≥ 0.56 | Ubuntu 22.04 package |
| Ninja | ≥ 1.10 | Ubuntu 22.04 package |
| python3-pyelftools | any | Ubuntu 22.04 package |
| libnuma-dev | any | Ubuntu 22.04 package |
| libpcap-dev | any | Ubuntu 22.04 package (for pcap PMD) |
| tmux | any | Runtime image only |

### 12.2 Virtual PMD Selection

`net_virtio_user` is used for functional testing because it is the only DPDK virtual PMD with genuine RSS support (40-byte configurable Toeplitz key, 128-entry RETA). `net_ring`, `net_null`, and `net_pcap` do not support RSS and must not be used for flow affinity verification.

Traffic generation uses `dpdk-testpmd` with `net_vhost` (paired with `net_virtio_user` via Unix domain sockets).

### 12.3 Hugepage Requirement

The Docker container must be run with `--privileged` and `/dev/hugepages` mounted. Minimum 256 × 2 MB hugepages must be pre-allocated on the host:

```bash
echo 256 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

---

## 13. Performance Measurement and Optimizations

### 13.1 Per-Packet Cycle Counter

Each worker logs a performance summary line at every export interval:

```
[perf] core N | <cycles>/pkt | <Mpps> Mpps | rx=<n> tx=<n> | poll_eff=<P>%
```

| Field | Description |
|-------|-------------|
| `cycles/pkt` | TSC cycles spent on application logic per received packet |
| `Mpps` | Million packets per second received during this interval |
| `rx` / `tx` | Packets received / forwarded in this interval |
| `poll_eff` | Fraction of `rte_eth_rx_burst()` calls that returned ≥ 1 packet |

**What is measured**: application logic only — header parse, flow lookup/create, counter update, MAC rewrite, and TX stats update. `rte_eth_rx_burst()` and `rte_eth_tx_burst()` I/O time is excluded; those costs are PMD-dependent and not application code.

All counters reset to zero after each export interval (per-interval deltas, not cumulative).

### 13.2 Hot-Path Optimizations

The packet processing loop (`worker_run`) applies three layered optimizations to minimize cycles per packet:

#### jhash for 16-byte Keys

The flow table uses `rte_jhash`. For the 16-byte 5-tuple key, jhash's XOR/add/rotate mix operations can be partially scheduled by the out-of-order execution engine, whereas hardware CRC32 has a strict serial dependency chain (each `crc32` feeds the next) that prevents pipelining for small keys. Hardware CRC32 is faster for keys ≥ 32 bytes.

#### Single Hash Computation

`rte_hash_hash()` computes the hash signature once. Both the lookup (`rte_hash_lookup_with_hash`) and the create-on-miss (`rte_hash_add_key_with_hash`) reuse the pre-computed signature, eliminating a redundant hash call on the first-packet-of-flow path.

#### No Double Lookup

Flow entry pointers are stored alongside the TX packet array (`tx_flows[]`) during the RX processing loop. TX statistics (tx_packets, tx_bytes) are updated by dereferencing the stored pointer after `rte_eth_tx_burst()` returns — no re-parsing of the packet header and no second hash table lookup.

#### Dual Prefetch

For each packet `i` in the burst, two prefetch instructions are issued for packet `i+1`:
- `rte_prefetch0(rx_pkts[i+1])` — the mbuf struct (holds `pkt_len`, `data_off`, etc.)
- `rte_prefetch0(rte_pktmbuf_mtod(rx_pkts[i+1]))` — the packet data (Ethernet/IP/TCP headers)

These are at different memory addresses and occupy separate cache lines. Issuing both prefetches one iteration ahead hides the memory access latency of both.

### 13.3 Expected Cycle Counts

| Environment | Typical cycles/pkt | Notes |
|-------------|-------------------|-------|
| `net_pcap` (regression test) | ~200–350 | Includes pcap overhead; file I/O excluded from measurement |
| `net_virtio_user` (vhost socket) | ~150–250 | Virtio descriptor ring overhead |
| Physical NIC (1G/10G) | ~100–200 | Approaching full pipeline efficiency |

The dominant remaining costs are: L1/L2 cache miss on flow entries (cold flows), instruction latency of the CRC32 → hash-table dependency chain, and TSC serialization overhead per burst.
