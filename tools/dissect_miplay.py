#!/usr/bin/env python3
"""
零依赖纯 Python 小米妙播 (MiPlay) PCAP 协议分析器
支持解析：
1. mDNS (UDP 5353) 服务发现 PTR / SRV / TXT 记录
2. TCP 8899 二进制控制通道流重组与帧头/负载分析
3. 反向 WFD RTSP (含 MULTI_MIRROR / wfd_audio_codecs_v2 / TimeOffset) 明文交互提取
4. UDP/TCP 高频流与 TimerService 校时候选端口统计
"""

import os
import re
import socket
import struct
import sys
from collections import Counter, defaultdict


def read_pcap(path):
    with open(path, "rb") as f:
        ghdr = f.read(24)
        if len(ghdr) < 24:
            raise ValueError("Invalid pcap header")
        magic = struct.unpack("<I", ghdr[:4])[0]
        if magic == 0xA1B2C3D4:
            endian = "<"
            nano = False
        elif magic == 0xD4C3B2A1:
            endian = ">"
            nano = False
        elif magic == 0xA1B23C4D:
            endian = "<"
            nano = True
        else:
            raise ValueError(f"Unsupported pcap magic: {hex(magic)}")
        _, _, _, _, _, _, linktype = struct.unpack(endian + "IHHiIII", ghdr)

        packets = []
        while True:
            rhdr = f.read(16)
            if len(rhdr) < 16:
                break
            ts_sec, ts_sub, incl_len, orig_len = struct.unpack(endian + "IIII", rhdr)
            data = f.read(incl_len)
            if len(data) < incl_len:
                break
            ts = ts_sec + (ts_sub / 1e9 if nano else ts_sub / 1e6)
            packets.append((ts, data, linktype))
        return packets


def parse_ipv4_packet(data, linktype):
    # Ethernet (1) or Linux cooked SLL (113) or Raw IP (101)
    if linktype == 1:
        if len(data) < 14:
            return None
        eth_type = struct.unpack("!H", data[12:14])[0]
        offset = 14
        if eth_type == 0x8100:  # VLAN
            if len(data) < 18:
                return None
            eth_type = struct.unpack("!H", data[16:18])[0]
            offset = 18
        if eth_type != 0x0800:
            return None
    elif linktype == 113:
        if len(data) < 16:
            return None
        eth_type = struct.unpack("!H", data[14:16])[0]
        if eth_type != 0x0800:
            return None
        offset = 16
    elif linktype == 101:
        offset = 0
    else:
        return None

    ip = data[offset:]
    if len(ip) < 20:
        return None
    ver_ihl = ip[0]
    if (ver_ihl >> 4) != 4:
        return None
    ihl = (ver_ihl & 0x0F) * 4
    total_len = struct.unpack("!H", ip[2:4])[0]
    proto = ip[9]
    src_ip = socket.inet_ntoa(ip[12:16])
    dst_ip = socket.inet_ntoa(ip[16:20])
    payload = ip[ihl:total_len]

    if proto == 6 and len(payload) >= 20:  # TCP
        sport, dport, seq, ack, off_flags = struct.unpack("!HHIIH", payload[:14])
        doff = ((off_flags >> 12) & 0x0F) * 4
        flags = off_flags & 0x3F
        app_data = payload[doff:]
        return ("TCP", src_ip, sport, dst_ip, dport, seq, flags, app_data)
    elif proto == 17 and len(payload) >= 8:  # UDP
        sport, dport, ulen, _ = struct.unpack("!HHHH", payload[:8])
        app_data = payload[8:ulen]
        return ("UDP", src_ip, sport, dst_ip, dport, 0, 0, app_data)
    return None


def decode_dns_name(buf, offset, depth=0):
    if depth > 10:
        return ("<loop>", offset)
    labels = []
    orig_offset = offset
    jumped = False
    while offset < len(buf):
        length = buf[offset]
        if length == 0:
            offset += 1
            break
        if (length & 0xC0) == 0xC0:
            if offset + 1 >= len(buf):
                break
            ptr = ((length & 0x3F) << 8) | buf[offset + 1]
            if not jumped:
                orig_offset = offset + 2
                jumped = True
            name_part, _ = decode_dns_name(buf, ptr, depth + 1)
            labels.append(name_part)
            break
        else:
            offset += 1
            label = buf[offset:offset + length].decode("utf-8", errors="replace")
            labels.append(label)
            offset += length
    return (".".join(labels), orig_offset if jumped else offset)


def parse_mdns(buf):
    if len(buf) < 12:
        return None
    txid, flags, qdcount, ancount, nscount, arcount = struct.unpack("!HHHHHH", buf[:12])
    offset = 12
    queries = []
    records = []

    for _ in range(qdcount):
        if offset >= len(buf):
            break
        name, offset = decode_dns_name(buf, offset)
        if offset + 4 > len(buf):
            break
        qtype, qclass = struct.unpack("!HH", buf[offset:offset + 4])
        offset += 4
        queries.append((name, qtype))

    for _ in range(ancount + nscount + arcount):
        if offset >= len(buf):
            break
        name, offset = decode_dns_name(buf, offset)
        if offset + 10 > len(buf):
            break
        rtype, rclass, ttl, rdlen = struct.unpack("!HHIH", buf[offset:offset + 10])
        offset += 10
        rdata = buf[offset:offset + rdlen]
        rdata_start = offset
        offset += rdlen

        val = ""
        if rtype == 12:  # PTR
            val, _ = decode_dns_name(buf, rdata_start)
        elif rtype == 16:  # TXT
            txts = []
            i = 0
            while i < len(rdata):
                ln = rdata[i]
                i += 1
                txts.append(rdata[i:i + ln].decode("utf-8", errors="replace"))
                i += ln
            val = "; ".join(txts)
        elif rtype == 33 and len(rdata) >= 6:  # SRV
            prio, weight, port = struct.unpack("!HHH", rdata[:6])
            target, _ = decode_dns_name(buf, rdata_start + 6)
            val = f"{target}:{port} (prio={prio},w={weight})"
        elif rtype == 1 and len(rdata) == 4:  # A
            val = socket.inet_ntoa(rdata)
        else:
            val = rdata.hex()[:40]
        records.append((name, rtype, ttl, val))
    return (flags, queries, records)


def hexdump_preview(data, limit=96):
    chunk = data[:limit]
    hex_str = " ".join(f"{b:02x}" for b in chunk)
    ascii_str = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
    suffix = f" ... (+{len(data) - limit}B)" if len(data) > limit else ""
    return f"{hex_str}{suffix}\n      ASCII: {ascii_str}"


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <file.pcap>")
        sys.exit(1)

    pcap_path = sys.argv[1]
    packets = read_pcap(pcap_path)
    print(f"=== Loaded {len(packets)} packets from {pcap_path} ===\n")
    if not packets:
        return

    t0 = packets[0][0]
    mdns_queries = set()
    mdns_records = {}
    cmd8899_events = []
    rtsp_events = []
    udp_flows = Counter()
    udp_sizes = defaultdict(list)
    tcp_flows = Counter()

    rtsp_pattern = re.compile(
        rb"(RTSP/1\.0|OPTIONS |GET_PARAMETER |SET_PARAMETER |SETUP |PLAY |PAUSE |TEARDOWN |MULTI_MIRROR |MIRROR_CONTROLS |FIRST_RENDER )"
    )

    for ts, raw, linktype in packets:
        parsed = parse_ipv4_packet(raw, linktype)
        if not parsed:
            continue
        proto, sip, sport, dip, dport, seq, flags, data = parsed
        rel_t = ts - t0

        if proto == "UDP":
            udp_flows[(sip, sport, dip, dport)] += 1
            if len(udp_sizes[(sip, sport, dip, dport)]) < 10:
                udp_sizes[(sip, sport, dip, dport)].append((rel_t, len(data), data[:32]))
            if sport == 5353 or dport == 5353:
                mdns = parse_mdns(data)
                if mdns:
                    mflags, qs, recs = mdns
                    for qname, qtype in qs:
                        mdns_queries.add((sip, qname, qtype))
                    for rname, rtype, ttl, val in recs:
                        mdns_records[(rname, rtype, val)] = sip
        elif proto == "TCP":
            tcp_flows[(sip, sport, dip, dport)] += 1
            if not data:
                continue
            if sport == 8899 or dport == 8899:
                cmd8899_events.append((rel_t, sip, sport, dip, dport, data))
            elif rtsp_pattern.search(data):
                rtsp_events.append((rel_t, sip, sport, dip, dport, data))

    print("================ 1. mDNS QUERIES & RECORDS ================")
    for sip, qname, qtype in sorted(mdns_queries):
        print(f"  [Q] {sip} -> {qname} (type={qtype})")
    for (rname, rtype, val), sip in sorted(mdns_records.items()):
        tname = {1: "A", 12: "PTR", 16: "TXT", 33: "SRV", 28: "AAAA"}.get(rtype, str(rtype))
        if any(k in (rname + val).lower() for k in ("mi", "play", "link", "8899", "audio", "xiaomi", "sound", "cast", "raop", "airplay", "local")):
            print(f"  [R:{tname}] ({sip}) {rname} => {val}")

    print(f"\n================ 2. TCP 8899 CONTROL FRAMES ({len(cmd8899_events)} segs) ================")
    for idx, (rt, sip, sp, dip, dp, data) in enumerate(cmd8899_events[:60]):
        print(f"  [{idx:02d}] +{rt:7.3f}s {sip}:{sp} -> {dip}:{dp} len={len(data)}")
        print(f"      HEX  : {hexdump_preview(data, 96)}")

    print(f"\n================ 3. WFD RTSP / MULTI_MIRROR ({len(rtsp_events)} msgs) ================")
    for idx, (rt, sip, sp, dip, dp, data) in enumerate(rtsp_events[:80]):
        text = data.decode("utf-8", errors="replace").strip()
        print(f"  --- [{idx:02d}] +{rt:7.3f}s {sip}:{sp} -> {dip}:{dp} (len={len(data)}) ---")
        for line in text.splitlines():
            print(f"      {line}")

    print("\n================ 4. TOP UDP FLOWS (RTP / MPT / TimerService 候选) ================")
    for (sip, sp, dip, dp), cnt in udp_flows.most_common(25):
        if sp == 5353 or dp == 5353:
            continue
        samples = udp_sizes[(sip, sp, dip, dp)]
        sz_list = [s[1] for s in samples[:5]]
        first_hex = samples[0][2].hex() if samples else ""
        print(f"  {sip}:{sp} -> {dip}:{dp}  pkts={cnt}  sample_lens={sz_list}  first32B={first_hex}")

    print("\n================ 5. TOP TCP FLOWS ================")
    for (sip, sp, dip, dp), cnt in tcp_flows.most_common(20):
        print(f"  {sip}:{sp} -> {dip}:{dp}  pkts={cnt}")


if __name__ == "__main__":
    main()
