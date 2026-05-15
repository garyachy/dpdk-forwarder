# dpdk-forwarder

High-performance DPDK packet forwarder with per-flow statistics tracking, built entirely in Docker with DPDK v23.11 LTS as a git submodule.

## Architecture

```
testpmd (txonly) ──net_vhost──[Unix socket]──net_virtio_user── forwarder ──net_virtio_user──[Unix socket]──net_vhost── testpmd (sink)
```

- **RSS affinity**: Symmetric Toeplitz key ensures each 5-tuple always lands on the same worker core.
- **Per-core flow tables**: Zero cross-core locking. Each worker owns its `rte_hash` instance.
- **Main-lcore stats export**: Workers call `rte_rcu_qsbr_quiescent()` each burst; the main lcore calls `rte_rcu_qsbr_synchronize()` before expiring flows and flushing CSV, keeping the fast path uninterrupted.

See [FUNC_SPEC.md](FUNC_SPEC.md) for the full functional specification.

## Features

- Multiple worker lcores with separate RX/TX queues — no cross-core locks anywhere. Worker count set with `--workers N`.
- Configurable per-core flow table capacity (`--max-flows N`). When full, new flows are silently dropped with a single logged warning; existing flows continue to be forwarded normally.

## Dependencies

| Dependency         | Version     | Source                |
|--------------------|-------------|-----------------------|
| DPDK               | 23.11 (LTS) | `dpdk/` git submodule |
| GCC                | ≥ 11        | Ubuntu 22.04          |
| Meson              | ≥ 0.56      | Ubuntu 22.04          |
| Ninja              | ≥ 1.10      | Ubuntu 22.04          |
| python3-pyelftools | any         | Ubuntu 22.04          |
| libnuma-dev        | any         | Ubuntu 22.04          |
| Docker             | ≥ 20.10     | host                  |

## Quick Start

### 1. Clone with submodule

```bash
git clone --recurse-submodules https://github.com/garyachy/dpdk-forwarder.git
cd dpdk-forwarder
# or if already cloned:
git submodule update --init
```

### 2. Allocate hugepages (host, once)

```bash
echo 256 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

### 3. Build Docker image

```bash
docker build -t dpdk-forwarder .
```

The multi-stage build compiles DPDK from the submodule, builds the forwarder, and runs the unit test suite. A clean build takes ~10 minutes; subsequent builds are fast due to layer caching.

### 4. Run unit tests manually

Tests run automatically during `docker build`. To run them in isolation:

```bash
docker build --target app-builder -t dpdk-fwd-test .
docker run --rm dpdk-fwd-test meson test -C build -v
```

### 5. Manual functional test (tmux)

```bash
mkdir -p output

docker run --rm -it --privileged \
  -v /dev/hugepages:/dev/hugepages \
  -v $(pwd)/output:/output \
  dpdk-forwarder \
  bash /tests/run_vdev.sh
```

This opens a tmux session with four panes:
- **Top-left**: forwarder process
- **Top-right**: testpmd traffic generator (txonly)
- **Bottom-left**: testpmd traffic sink (rxonly)
- **Bottom-right**: live tail of CSV output

Press `Ctrl-B D` to detach, `Ctrl-C` to stop the forwarder.

### 6. Verify flow affinity

```bash
python3 tests/verify_affinity.py output/flow_stats_core_*.csv
```

### 7. docker compose (alternative)

```bash
docker compose up --build
```

## CLI Reference

```
dpdk-forwarder [EAL opts] -- [app opts]

  --rx-port N          Ingress port index      (default 0)
  --tx-port N          Egress port index       (default 1)
  --workers N          Worker lcore count      (default: all available)
  --burst N            RX/TX burst size        (default 32)
  --pool-size N        Mbuf pool entries       (default 8191)
  --max-flows N        Flow table cap per core (default 65536)
  --stats-interval N   Export interval (s)     (default 5)
  --flow-timeout N     Inactivity timeout (s)  (default 60)
  --output-dir PATH    CSV output directory    (default .)
  --dst-mac MAC        Rewrite destination MAC (optional)
  --log-level N        0=ERR...3=DEBUG         (default 2)
```

## Output Format

Per-core CSV files at `<output-dir>/flow_stats_core_<lcore_id>.csv`:

```
timestamp,src_ip,dst_ip,src_port,dst_port,proto,rx_bytes,tx_bytes,rx_packets,tx_packets
2026-05-12T14:23:05Z,192.168.1.10,10.0.0.1,54321,80,6,4096,4096,10,10
```

Counters are cumulative. Each export interval appends one row per active flow.

## Flow Tracking

- **Key**: 5-tuple `(src_ip, dst_ip, src_port, dst_port, protocol)`
- **Table**: `rte_hash` (cuckoo hashing) with one instance per worker core
- **Affinity**: Symmetric RSS Toeplitz key guarantees same-core delivery for all packets of a given flow
- **Expiry**: Flows inactive for `--flow-timeout` seconds are removed during the next export pass

## Expected Output

After the functional test starts, the forwarder logs to stdout and writes CSV files to `output/`.

**Startup (top-left pane):**
```
INFO: worker lcore 1: queue 0, csv=/output/flow_stats_core_1.csv
INFO: worker lcore 2: queue 1, csv=/output/flow_stats_core_2.csv
INFO: worker lcore 3: queue 2, csv=/output/flow_stats_core_3.csv
INFO: worker lcore 1 started
INFO: worker lcore 2 started
INFO: worker lcore 3 started
```

**Every stats interval (5 s by default):**
```
INFO: [perf] core 1 | 121 cycles/pkt | 1.843 Mpps | rx=9215000 tx=9215000 | poll_eff=97.3%
INFO: [perf] core 2 | 118 cycles/pkt | 1.791 Mpps | rx=8955000 tx=8955000 | poll_eff=96.8%
```

**Live CSV tail (bottom-right pane):**
```
==> /output/flow_stats_core_1.csv <==
timestamp,src_ip,dst_ip,src_port,dst_port,proto,rx_bytes,tx_bytes,rx_packets,tx_packets
2026-05-12T14:23:05Z,10.0.0.1,10.0.1.1,10001,80,6,589824,589824,9216,9216
2026-05-12T14:23:05Z,10.0.0.2,10.0.1.1,10002,80,6,601088,601088,9392,9392
```

**Affinity verification:**
```
$ python3 tests/verify_affinity.py output/flow_stats_core_*.csv
core 1: 2 distinct flows
core 2: 2 distinct flows
core 3: 1 distinct flows
OK: no flow appears on more than one core
```

## Performance

Measurements use `net_virtio_user` (vhost socket). `proc_cycles` covers application logic only — `rte_eth_rx_burst` and `rte_eth_tx_burst` I/O time is excluded.

| Environment | Typical cycles/pkt | Notes |
|---|---|---|
| `net_virtio_user` (vhost socket) | ~340–350 | 3 workers, measured live with run_vdev.sh |
| Physical NIC (10G/25G), warm cache, small table | ~80–150 | Line-rate |
| Physical NIC, large table (1M+ flows) | ~120–200 | Entry prefetch hides most L3 latency |

At larger frame sizes (256/512/1518 bytes) the cycles/pkt figure is similar — the bottleneck is hash-table lookup and memory bandwidth, not header parsing — but Mpps throughput drops proportionally as PMD I/O time increases.

See [FUNC_SPEC.md §13](FUNC_SPEC.md) for the full methodology and hot-path details.

## Building from Source (without Docker)

Requires DPDK 23.11 installed and `pkg-config` findable:

```bash
meson setup build
ninja -C build
meson test -C build
```
