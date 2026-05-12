# dpdk-forwarder

High-performance DPDK packet forwarder with per-flow statistics tracking, built entirely in Docker with DPDK v23.11 LTS as a git submodule.

## Architecture

```
testpmd (txonly) ──net_vhost──[Unix socket]──net_virtio_user── forwarder ──net_virtio_user──[Unix socket]──net_vhost── testpmd (sink)
```

- **RSS affinity**: Symmetric Toeplitz key ensures each 5-tuple lands on the same worker core every time.
- **Per-core flow tables**: Zero cross-core locking. Each worker owns its `rte_hash` instance.
- **Inline stats export**: No dedicated exporter lcore; triggered by TSC comparison inside the worker loop.

See [FUNC_SPEC.md](FUNC_SPEC.md) for the full functional specification.

## Dependencies

| Dependency         | Version     | Source                |
|--------------------|-------------|-----------------------|
| DPDK               | 23.11 (LTS) | `dpdk/` git submodule |
| GCC                | ≥ 11        | Ubuntu 22.04          |
| Meson              | ≥ 0.56      | Ubuntu 22.04          |
| Ninja              | ≥ 1.10      | Ubuntu 22.04          |
| python3-pyelftools | any         | Ubuntu 22.04          |
| libnuma-dev        | any         | Ubuntu 22.04          |
| libpcap-dev        | any         | Ubuntu 22.04          |
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

The multi-stage build compiles DPDK from the submodule and then the forwarder binary. Subsequent builds are fast due to layer caching.

### 4. Run unit tests

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

This opens a tmux session with:
- **Left pane**: forwarder process
- **Right pane**: testpmd traffic generator (txonly)
- **Bottom pane**: live tail of CSV output

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

## Building from Source (without Docker)

Requires DPDK 23.11 installed and `pkg-config` findable:

```bash
meson setup build
ninja -C build
meson test -C build
```
