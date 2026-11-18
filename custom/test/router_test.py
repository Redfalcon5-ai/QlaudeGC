"""End-to-end test of tar_router_cli (same engine as the router built into QlaudeGC).

Usage: python router_test.py <path to tar_router_cli.exe>

Topology (all loopback):
  drone  (sys 1)   -> UDP server 24550
  gcs    (sys 255) <- UDP client to 24570
  other  (sys 77)  <- UDP client to 24580
  tcp    (sys 200) -> TCP server 25760
"""
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

CONF = """
[General]
TcpServerPort = 25760

[UdpEndpoint drone]
Mode = Server
Address = 127.0.0.1
Port = 24550

[UdpEndpoint gcs]
Mode = Normal
Address = 127.0.0.1
Port = 24570

[UdpEndpoint other]
Mode = Normal
Address = 127.0.0.1
Port = 24580
BlockMsgIdOut = 30
"""

CRC_EXTRA = {0: 50, 30: 39, 76: 152, 77: 143}


def x25(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        tmp = b ^ (crc & 0xFF)
        tmp = (tmp ^ (tmp << 4)) & 0xFF
        crc = ((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & 0xFFFF
    return crc


def frame_v2(msgid, payload, sysid, compid, seq=0, signed=False, corrupt=False):
    # MAVLink 2 trims trailing zeros from the payload
    trimmed = payload.rstrip(b"\x00") or payload[:1]
    incompat = 0x01 if signed else 0
    header = struct.pack("<BBBBBB", len(trimmed), incompat, 0, seq, sysid, compid) + struct.pack("<I", msgid)[:3]
    crc = x25(header + trimmed + bytes([CRC_EXTRA[msgid]]))
    if corrupt:
        crc ^= 0x5A5A
    out = b"\xFD" + header + trimmed + struct.pack("<H", crc)
    if signed:
        out += bytes(range(13))  # link id + timestamp + signature; router must pass it through untouched
    return out


def heartbeat(sysid, compid=1, **kw):
    return frame_v2(0, struct.pack("<IBBBBB", 0, 2, 12, 0, 4, 3), sysid, compid, **kw)


def attitude(sysid):
    return frame_v2(30, struct.pack("<Iffffff", 1000, 0.1, 0.2, 0.3, 0, 0, 0), sysid, 1)


def command_long(sysid, target_sys, target_comp=1):
    payload = struct.pack("<fffffffHBBB", 0, 0, 0, 0, 0, 0, 0, 400, target_sys, target_comp, 0)
    return frame_v2(76, payload, sysid, 190)


def udp(port=0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    s.settimeout(0.05)
    return s


def drain(sock, wait=0.4):
    """Return everything received within `wait` seconds as one bytes blob."""
    end = time.time() + wait
    data = b""
    while time.time() < end:
        try:
            if sock.type == socket.SOCK_DGRAM:
                data += sock.recvfrom(65535)[0]
            else:
                chunk = sock.recv(65535)
                if not chunk:
                    break
                data += chunk
        except (socket.timeout, BlockingIOError):
            pass
    return data


results = []


def check(name, cond):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name)


def main():
    exe = str(Path(sys.argv[1]).resolve())
    conf = Path(tempfile.mkdtemp()) / "main.conf"
    conf.write_text(CONF)

    gcs, other = udp(24570), udp(24580)
    drone = udp()
    # Router output goes to a file: an unread pipe fills after a few KB on Windows and would block the router.
    log_path = conf.with_name("router.log")
    log_file = open(log_path, "w")
    proc = subprocess.Popen([exe, str(conf), "--stats"], stdout=log_file, stderr=subprocess.STDOUT)
    time.sleep(1.0)
    tcp = socket.create_connection(("127.0.0.1", 25760))
    tcp.settimeout(0.05)
    time.sleep(0.3)

    try:
        # 1. vehicle heartbeat fans out to every other endpoint
        hb = heartbeat(1)
        drone.sendto(hb, ("127.0.0.1", 24550))
        g, o, t = drain(gcs), drain(other, 0.1), drain(tcp, 0.1)
        check("heartbeat from drone reaches gcs", hb in g)
        check("heartbeat from drone reaches other", hb in o)
        check("heartbeat from drone reaches tcp client", hb in t)
        check("heartbeat not echoed back to drone", drain(drone, 0.2) == b"")

        # gcs/other are UDP *clients* of the router: the router sends to 24570/24580 from an ephemeral
        # port, so learn that port from a received datagram and reply there (like a real GCS does).

        def router_port_for(sock):
            drone.sendto(heartbeat(1), ("127.0.0.1", 24550))
            sock.settimeout(1.0)
            _, addr = sock.recvfrom(65535)
            sock.settimeout(0.05)
            drain(sock, 0.2)
            return addr

        gcs_router = router_port_for(gcs)
        other_router = router_port_for(other)
        drain(tcp, 0.3)
        drain(drone, 0.3)
        gcs.sendto(heartbeat(255, 190), gcs_router)
        other.sendto(heartbeat(77), other_router)
        tcp.sendall(heartbeat(200))
        time.sleep(0.3)
        for s in (gcs, other, tcp, drone):
            drain(s, 0.2)

        # 2. targeted command goes only to the endpoint that has seen the target system
        cmd = command_long(255, 1)
        gcs.sendto(cmd, gcs_router)
        d, o, t = drain(drone), drain(other, 0.1), drain(tcp, 0.1)
        check("COMMAND_LONG to sys 1 reaches drone", cmd in d)
        check("COMMAND_LONG to sys 1 NOT sent to other", cmd not in o)
        check("COMMAND_LONG to sys 1 NOT sent to tcp", cmd not in t)

        # 3. command to a system nobody has seen is broadcast (first contact)
        cmd42 = command_long(255, 42)
        gcs.sendto(cmd42, gcs_router)
        d, o, t = drain(drone), drain(other, 0.1), drain(tcp, 0.1)
        check("COMMAND_LONG to unknown sys 42 broadcast to drone/other/tcp", cmd42 in d and cmd42 in o and cmd42 in t)

        # 4. bad CRC is dropped
        bad = heartbeat(1, corrupt=True)
        drone.sendto(bad, ("127.0.0.1", 24550))
        check("corrupted frame dropped", bad not in drain(gcs))

        # 5. TCP stream split byte-by-byte is reassembled
        cmd_tcp = command_long(200, 1)
        for b in cmd_tcp:
            tcp.sendall(bytes([b]))
            time.sleep(0.002)
        check("byte-split TCP frame reassembled and routed to drone", cmd_tcp in drain(drone))

        # 6. signed frame forwarded byte-identical
        signed = heartbeat(1, signed=True, seq=9)
        drone.sendto(signed, ("127.0.0.1", 24550))
        check("signed MAVLink2 frame forwarded intact", signed in drain(gcs))

        # 7. out filter: ATTITUDE (30) blocked towards 'other' only
        att = attitude(1)
        drone.sendto(att, ("127.0.0.1", 24550))
        g, o = drain(gcs), drain(other, 0.1)
        check("BlockMsgIdOut=30 blocks ATTITUDE to other", att not in o)
        check("ATTITUDE still reaches gcs", att in g)

        # 8. burst throughput: 2000 heartbeats in ~1 s, count arrivals at gcs
        gcs.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        n = 2000
        frames = [heartbeat(1, seq=i % 256) for i in range(n)]
        received = 0
        t0 = time.time()
        for i, f in enumerate(frames):
            drone.sendto(f, ("127.0.0.1", 24550))
            if i % 50 == 0:
                time.sleep(0.02)
        end = time.time() + 1.0
        gcs.settimeout(0.05)
        while time.time() < end:
            try:
                data, _ = gcs.recvfrom(65535)
                received += data.count(b"\xFD\x09")
            except socket.timeout:
                pass
        dt = time.time() - t0
        print(f"     burst: {received}/{n} frames delivered ({n / dt:.0f} msg/s offered)")
        check("burst of 2000 msgs delivered >= 99%", received >= 0.99 * n)
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        log_file.close()
        print("---- router output (tail) ----")
        print("\n".join(log_path.read_text().splitlines()[-6:]))

    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
