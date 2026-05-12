#!/usr/bin/env python3
"""Generate a minimal test.pcap with 5 flows, 20 packets each."""

import struct
import socket
import sys

def checksum(data):
    if len(data) % 2:
        data += b'\x00'
    s = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    s = (s >> 16) + (s & 0xffff)
    s += (s >> 16)
    return ~s & 0xffff

def make_eth_ipv4_tcp(src_ip, dst_ip, src_port, dst_port, payload=b'HELLO'):
    src_mac = b'\x02\x00\x00\x00\x00\x01'
    dst_mac = b'\x02\x00\x00\x00\x00\x02'
    eth = dst_mac + src_mac + b'\x08\x00'

    ip_len = 20 + 20 + len(payload)
    ip_hdr = struct.pack('!BBHHHBBH4s4s',
        0x45, 0, ip_len, 0x1234, 0x4000, 64, 6, 0,
        socket.inet_aton(src_ip), socket.inet_aton(dst_ip))
    ip_csum = checksum(ip_hdr)
    ip_hdr = ip_hdr[:10] + struct.pack('!H', ip_csum) + ip_hdr[12:]

    tcp_hdr = struct.pack('!HHIIBBHHH',
        src_port, dst_port, 0, 0, 0x50, 0x02, 8192, 0, 0)
    # TCP pseudo-header checksum
    pseudo = socket.inet_aton(src_ip) + socket.inet_aton(dst_ip) + \
             struct.pack('!BBH', 0, 6, len(tcp_hdr) + len(payload))
    tcp_csum = checksum(pseudo + tcp_hdr + payload)
    tcp_hdr = tcp_hdr[:16] + struct.pack('!H', tcp_csum) + tcp_hdr[18:]

    return eth + ip_hdr + tcp_hdr + payload

FLOWS = [
    ('10.0.0.1', '10.0.1.1', 10001, 80),
    ('10.0.0.2', '10.0.1.1', 10002, 80),
    ('10.0.0.3', '10.0.1.2', 10003, 443),
    ('10.0.0.4', '10.0.1.2', 10004, 443),
    ('10.0.0.5', '10.0.1.3', 10005, 8080),
]

out = sys.argv[1] if len(sys.argv) > 1 else '/tmp/test.pcap'

with open(out, 'wb') as f:
    # PCAP global header
    f.write(struct.pack('<IHHiIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
    ts = 1715000000
    for i in range(20):
        for src_ip, dst_ip, sp, dp in FLOWS:
            pkt = make_eth_ipv4_tcp(src_ip, dst_ip, sp, dp)
            f.write(struct.pack('<IIII', ts, i * 1000, len(pkt), len(pkt)))
            f.write(pkt)
        ts += 1

print(f'Written {20 * len(FLOWS)} packets to {out}')
