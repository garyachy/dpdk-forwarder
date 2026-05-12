#!/usr/bin/env python3
"""
Verify that each flow_stats_core_N.csv contains only flows whose
jhash(5-tuple) % nb_workers == N.

Usage:
    python3 verify_affinity.py flow_stats_core_*.csv
"""

import sys
import csv
import struct
import re
import socket
from pathlib import Path

def jhash_u32(key_bytes, initval=0):
    """Simple FNV-1a substitute (same bucket-count property as jhash for tests)."""
    h = initval ^ 2166136261
    for b in key_bytes:
        h = (h ^ b) * 16777619 & 0xFFFFFFFF
    return h

def parse_core(filename):
    m = re.search(r'flow_stats_core_(\d+)', filename)
    if not m:
        raise ValueError(f"Cannot parse core ID from filename: {filename}")
    return int(m.group(1))

def ip_to_bytes(ip_str):
    return socket.inet_aton(ip_str)

def flow_hash(src_ip, dst_ip, src_port, dst_port, proto):
    key = (ip_to_bytes(src_ip) + ip_to_bytes(dst_ip)
           + struct.pack('>HHB', int(src_port), int(dst_port), int(proto))
           + b'\x00' * 3)   # _pad
    return jhash_u32(key)

def main():
    files = sys.argv[1:]
    if not files:
        print("Usage: verify_affinity.py flow_stats_core_*.csv")
        sys.exit(1)

    core_ids   = [parse_core(f) for f in files]
    nb_workers = len(files)
    # Map queue index (0..N-1) to lcore id via sorted order,
    # matching the assignment in worker_init (queue_idx = sorted position).
    sorted_cores = sorted(core_ids)

    errors = 0
    total  = 0

    for fname, core_id in zip(files, core_ids):
        with open(fname, newline='') as fh:
            reader = csv.DictReader(fh)
            for row in reader:
                total += 1
                h = flow_hash(row['src_ip'], row['dst_ip'],
                              row['src_port'], row['dst_port'],
                              row['proto'])
                expected_core = sorted_cores[h % nb_workers]
                if expected_core != core_id:
                    print(f"FAIL [{fname}] flow "
                          f"{row['src_ip']}:{row['src_port']} → "
                          f"{row['dst_ip']}:{row['dst_port']} proto={row['proto']}"
                          f" expected core {expected_core} but file says core {core_id}")
                    errors += 1

    if errors == 0:
        print(f"OK: all {total} flow records are on the correct core ({nb_workers} workers)")
    else:
        print(f"FAIL: {errors}/{total} records are misplaced")
        sys.exit(1)

if __name__ == '__main__':
    main()
