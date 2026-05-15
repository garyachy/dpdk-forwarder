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
| 0 | Main | EAL init, port init, mbuf pool allocation, worker launch, RCU QSBR setup, stats export + flow expiry loop, SIGINT/SIGTERM handler, final export on shutdown |
| 1..N | Worker | RX burst, header parse, flow lookup/create, TX burst, per-interval perf logging, RCU quiescent-state reporting |

Stats export and flow expiry run on the main lcore, which was previously idle. Workers report quiescent states via `rte_rcu_qsbr_quiescent()` after each burst so the main lcore can safely delete expired entries between bursts. See §15 for details.

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

- Export is driven by the **main lcore** at every `--stats-interval` seconds.
- `export_tsc_interval` is pre-computed at startup: `stats_interval_s * rte_get_tsc_hz()`.
- All flows in the table are iterated via `rte_hash_iterate()` and written to CSV.
- Counters are cumulative — the same flow produces one row per export interval showing running totals.
- `fflush()` is called after each export pass.
- Flows inactive for longer than `flow_timeout` are collected during the CSV pass; after `rte_rcu_qsbr_synchronize()` they are deleted from the hash table (two-pass, no stop-the-world on workers).

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
3. Workers observe `force_quit`, call `rte_rcu_qsbr_thread_offline()`, close CSV files, return from lcore function.
4. Main calls `rte_eal_wait_lcore()` for each worker.
5. Main performs one final `stats_export_and_expire()` pass for every worker (all workers are offline so `rte_rcu_qsbr_synchronize()` returns instantly).
6. Main calls `rte_eth_dev_stop()` and `rte_eth_dev_close()` for both ports.
7. Main prints final port stats via `port_stats_print()`.
8. `rte_free(qsv)`, `rte_eal_cleanup()`, and `exit(0)`.

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
| tmux | any | Runtime image only |

### 12.2 Virtual PMD Selection

`net_virtio_user` is used for functional testing because it is the only DPDK virtual PMD with genuine RSS support (40-byte configurable Toeplitz key, 128-entry RETA). `net_ring` and `net_null` do not support RSS and must not be used for flow affinity verification.

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

The packet processing loop (`worker_run`) applies seven layered optimizations.

#### 1. No Double Lookup (1130 → 332 cycles/pkt)

Flow entry pointers are stored in a parallel `tx_flows[]` array during the RX processing loop. TX statistics (`tx_packets`, `tx_bytes`) are updated by dereferencing the stored pointer after `rte_eth_tx_burst()` returns — no re-parsing of the packet header and no second hash table lookup per forwarded packet.

#### 2. `__rte_always_inline` on `parse_key` (332 → 317 cycles/pkt)

Guarantees that `parse_key` is inlined at its single call site in the hot loop, eliminating function-call setup/teardown and giving the compiler full visibility to schedule the loads and branches optimally.

#### 3. `rte_hash_lookup_bulk`

The most impactful single change. The hot loop is restructured into three explicit phases per burst:

1. **Parse phase**: iterate `rx_pkts[0..nb_rx-1]`, build `keys[]` and `key_ptrs[]` arrays, free non-IPv4 packets early.
2. **Bulk lookup**: call `rte_hash_lookup_bulk(ht, key_ptrs, nb_ip, positions)` once for all IP packets in the burst. DPDK's implementation uses a 4-ahead software prefetch pipeline — while comparing the key for lookup `i`, it issues a prefetch for the hash bucket of lookup `i+4`. This hides the ~100-cycle hash→bucket memory-access latency that serialised the previous per-packet loop.
3. **Stats phase**: iterate the results; update counters for hits, call `rte_hash_add_key` for misses (rare, sequential cost is acceptable).

#### 4. Single `rte_rdtsc` per Active Burst (reduces TSC serialisation overhead)

Previously two `rte_rdtsc()` calls fired per active poll: `now_tsc` (before rx_burst) and `proc_start` (after). They are merged into one call sampled immediately after `rte_eth_rx_burst()` returns, reused for both the export-interval check and the `proc_start` reference. Each `rte_rdtsc()` is a serialising instruction (~20 cycles); eliminating one per poll saves ~20 cycles/burst amortised over burst size.

#### 5. Counter-Gated Idle Export (eliminates TSC reads in empty-queue spin)

When no packets arrive, the worker spins calling `rte_eth_rx_burst()` and getting `nb_rx = 0`. Previously `rte_rdtsc()` was called on every iteration to check the export interval — wasting ~20 cycles per idle poll for a check that fires at most once per second. Now a counter increments each idle poll; `rte_rdtsc()` is called only every `IDLE_EXPORT_BATCH` (1,000,000) idle polls. This cuts idle-path CPU consumption by ~20 cycles/iteration while keeping export latency well within one second.

#### 6. jhash for 16-byte Keys

The flow table uses `rte_jhash` (not `rte_hash_crc`). For the 16-byte 5-tuple key, jhash's XOR/add/rotate mix operations can be partially scheduled by the out-of-order execution engine. Hardware CRC32 has a strict serial dependency chain (each `crc32` instruction reads the result of the previous), which prevents pipelining for small keys. CRC32 is faster for keys ≥ 32 bytes where 64-bit instructions provide more throughput.

#### 7. Packet Prefetch (correctness for cold-cache workloads)

For each packet `i`, `rte_prefetch0(rte_pktmbuf_mtod(rx_pkts[i+1]))` is issued one iteration ahead. With five hot flows and a small trace (L1/L2 cache resident), this has negligible effect. On a real NIC with a large flow table and many unique source IPs, packet headers arrive cold from DMA memory (~200 cycle latency); the prefetch hides this latency behind the processing of the previous packet.

#### 8. Flow Entry Prefetch After Bulk Lookup

After `rte_hash_lookup_bulk` returns `positions[]`, a prefetch loop issues `rte_prefetch0(&ft.entries[positions[j]])` for all `j` before `burst_flow_update` touches any entry:

```c
for (uint16_t j = 0; j < nb_ip; j++)
    if (likely(positions[j] >= 0))
        rte_prefetch0(&ft.entries[positions[j]]);
```

This separates the prefetch issue from the consumption by approximately `nb_ip × loop_body_cycles` ≈ 160 cycles for a 32-packet burst — enough to hide L2→L1 latency (~12 cycles) and partial L3→L1 latency (~40 cycles).

**When it helps**: flow tables larger than LLC (1M+ flows, 64 MB+ entries). Each entry is one 64-byte cache line; with many unique flows the hardware prefetcher cannot predict the scattered access pattern. A full-burst software prefetch issues all 32 loads simultaneously, allowing them to overlap with each other and with the subsequent processing loop.

**Overhead in virtualized environments**: when the entries array fits in the host's LLC (or the hypervisor provides fast virtual memory access), the prefetch loop adds ~1–2 cycles/pkt without benefit, as `test_entry_prefetch` confirms on Docker/KVM. The overhead is negligible relative to the benefit at production scale.

### 13.3 Measured Cycle Counts

`proc_cycles` excludes `rte_eth_rx_burst` and `rte_eth_tx_burst` I/O time. Measurements use `net_virtio_user` (vhost socket).

| Environment | Typical cycles/pkt | Notes |
|-------------|-------------------|-------|
| `net_virtio_user` (vhost socket, 100% poll eff.) | ~340–350 | 3 workers, 1 TCP flow; measured live with run_vdev.sh |
| Physical NIC (10G/25G), warm cache, small table | ~80–150 | Line-rate |
| Physical NIC, large table (1M+ flows) | ~120–200 | Entry prefetch hides most L3 latency |
| Physical NIC, large table, no entry prefetch | ~150–300 | L2/L3 misses on flow entries dominate |

**`test_entry_prefetch` benchmark** (Docker, 1M flows, 128 MB entries array, steady-state LLC-warm):

| Pass | cycles/access | Note |
|------|--------------|-------|
| No software prefetch | ~8 | Entries served from host LLC |
| With software prefetch | ~13 | Prefetch loop overhead dominates when data is already cached |

On bare metal where 128 MB exceeds LLC, the same test would show ~40–150 cycles/access without prefetch vs ~10–20 cycles/access with it.

---

## 14. Reference: Key Concepts

### Toeplitz Hash (Symmetric RSS)

Toeplitz is the hash function used by NICs for Receive Side Scaling. It works by sliding a secret key over the input bits and XOR-ing windows into an accumulator:

```
hash = 0
for each bit b in input (src_ip, dst_ip, src_port, dst_port):
    if b == 1:
        hash ^= (current 32-bit window of the key)
    shift key window left by 1 bit
```

The `0x6D5A` repeating key used in `port.c` has the symmetric property: `hash(A→B) == hash(B→A)`. This guarantees that both directions of a TCP flow (SYN and ACK) land on the **same RX queue and therefore the same worker core**. Without symmetry, forward and return packets would split across two cores, causing incomplete per-flow counters without cross-core locking.

### False Sharing

False sharing occurs when two CPU cores write to **different variables that happen to occupy the same 64-byte cache line**:

```
Core 0 writes flow_entry[0]  ──┐
                                ├── same cache line → coherency protocol serializes both cores
Core 1 writes flow_entry[1]  ──┘
```

Even though the cores access distinct addresses, the MESI cache coherency protocol forces each write to invalidate the other core's copy of the line, stalling both pipelines. The cost is typically 100–200 cycles per false-sharing miss — comparable to the entire per-packet processing budget in this forwarder.

The fix is `__rte_cache_aligned` on `struct flow_entry` (see `flow.h`), which pads each entry to a full cache line. Each worker core then writes exclusively to its own lines with no cross-core interference.

The dominant remaining cost at small flow counts is the jhash computation (~50 cycles) and the hash-table bucket access chain. At large flow counts, cache misses on flow entries become the bottleneck — the prefetch and bulk-lookup pipeline hide most of that latency.

### RETA (RSS Redirection Table)

RETA is a lookup table inside the NIC that maps RSS hash buckets to RX queue numbers. After computing the Toeplitz hash of a packet's 5-tuple, the NIC takes the low N bits of that hash as an index into the RETA and steers the packet to whichever queue is stored there:

```
packet → Toeplitz hash → low 7 bits → RETA[0..127] → queue index → worker core
```

Typical size is 128 entries. Without explicitly programming the RETA, the NIC uses its power-on default which may not distribute evenly across the actual queue count. `port.c` fills it round-robin across the configured number of queues:

```
RETA[0]=0, RETA[1]=1, RETA[2]=2, RETA[3]=0, ...
```

`rte_eth_dev_rss_reta_update` writes this table directly into NIC registers. On virtual PMDs (e.g. `net_virtio_user`) the call may return `-ENOTSUP` and is treated as non-fatal — the PMD handles queue steering internally.

### RCU (Read-Copy-Update)

RCU is a synchronization mechanism built on one insight: **readers never block, writers wait for readers to finish naturally**.

The problem it solves: a dedicated export lcore wants to delete expired flow entries from the hash table while worker lcores are actively doing `rte_hash_lookup_bulk` on the same table. A lock would stall the workers. Atomics on individual counters don't help — a worker could be mid-lookup on an entry the exporter just deleted.

RCU avoids this by defining a **quiescent state** — a point where a thread is guaranteed to hold no references into the shared data structure. In DPDK (`rte_rcu_qsbr`), the natural quiescent state is the end of a packet burst. Workers call `rte_rcu_qsbr_quiescent()` once per burst; the exporter calls `rte_rcu_qsbr_synchronize()` before deleting an entry, which blocks until every worker has passed through at least one quiescent point:

```
Workers (readers)                    Exporter (writer)
─────────────────                    ─────────────────
process burst...                     wants to delete flow entry X
rte_rcu_qsbr_quiescent()  ──────►
process burst...                     rte_rcu_qsbr_synchronize()
rte_rcu_qsbr_quiescent()  ──────►     blocks until ALL workers have
process burst...                       called quiescent at least once
rte_rcu_qsbr_quiescent()  ──────►    ◄─ returns: no worker holds a
                                        reference to X
                                     rte_hash_del_key(X)  ← safe
```

Once `synchronize` returns, it is guaranteed no worker is mid-lookup on X, so deletion is safe with no locking on the read path.

**Read-side cost:** `rte_rcu_qsbr_quiescent()` is a single relaxed atomic store — one write per burst. No mutex, no CAS loop, no cache line contention between cores.

**Why "read-copy-update":** the full pattern for *updates* (not just deletions) is: keep the old entry live, make a modified copy, atomically swap the pointer so new readers see the new version, wait for quiescent, then free the old copy. This forwarder only needs the deletion half of that pattern.

**Relevance to this codebase:** the main lcore was previously spinning in `rte_delay_ms(100)` doing nothing. The RCU implementation described in §15 moves export and expiry there, eliminating the worker stop-the-world pause at large flow counts at zero extra CPU cost.

---

## 15. Large Flow Table: Stop-the-World Measurement and RCU Fix

### 15.1 The Problem

When `stats_export_and_expire` ran on the worker lcore (original design), it paused forwarding for the entire duration of the flow table walk + expiry deletion pass. At the default of 65 536 flows this pause is imperceptible. At 1 M flows it becomes a measurable drop window.

### 15.2 Stress Test Results

`test_scale_export` (see `tests/test_flow.c`) inserts 1 000 000 flows into a 2 097 152-slot table (47% load factor), then measures:

| Phase | Time (Docker, stdlib hash) |
|---|---|
| Insert 1M flows | 283.9 ms (one-time setup) |
| Walk — read every live entry | **6.88 ms** |
| Expire — delete every entry | **8.87 ms** |
| **Worker pause total** | **15.75 ms** |

At 1.8 Mpps per core, a 15.75 ms pause drops **~28 000 packets** and causes NIC RX descriptor ring overflow (`imissed` counter). The production `rte_hash` (hugepage-backed, NUMA-aware) is faster than the stdlib shim used in the test, but the same O(N) walk applies — measured stop-the-world will be in the same order of magnitude.

### 15.3 RCU Solution

The fix moves `stats_export_and_expire` to the main lcore, which was previously idle. Workers are never paused.

**What changed:**

| Component | Change |
|---|---|
| `flow.c` | Added `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY` so workers and main lcore can access the same hash table concurrently |
| `worker.c` | Removed export call; added `rte_rcu_qsbr_quiescent()` after each active burst and on idle polls |
| `stats.c` | Collect expired keys during walk, call `rte_rcu_qsbr_synchronize()` once, then delete — never pausing workers |
| `main.c` | Create shared `rte_rcu_qsbr` variable; register workers; drive export loop at `--stats-interval` cadence |

**What RCU protects and what it does not:**

| Concurrent operation | Protection |
|---|---|
| Worker `lookup_bulk` + main `del_key` | **RCU** — `synchronize()` ensures worker finished lookup before delete proceeds |
| Worker `add_key` + main `iterate` | **`RW_CONCURRENCY`** flag — internal rwlock in `rte_hash` |
| Worker counter increment + main counter read | x86 aligned 64-bit stores are atomic; slightly stale CSV rows are acceptable for monitoring |

**Worker hot-path overhead added:** one relaxed atomic store (`rte_rcu_qsbr_quiescent`) per burst. On x86 this is ~1–2 cycles — below measurement noise.

**Main lcore export sequence (per interval):**

```
main lcore                               worker lcores
──────────────────────────────           ─────────────────────────
rte_hash_iterate → write CSV rows        rx_burst → lookup_bulk → tx_burst
collect expired_keys[]                   rte_rcu_qsbr_quiescent()  ◄──────────┐
                                         rx_burst → lookup_bulk → tx_burst    │
rte_rcu_qsbr_synchronize()  ────────────────────────────────────────────────► │
  (blocks ≤ one burst latency ~10 µs)    rte_rcu_qsbr_quiescent()  ────────► ─┘
rte_hash_del_key(expired[0..N])          (continues forwarding uninterrupted)
```

Workers forward continuously throughout. The `synchronize()` call on the main lcore blocks for at most one burst latency (~10 µs at 1.8 Mpps with burst=32) — negligible compared to the 5-second export interval.
