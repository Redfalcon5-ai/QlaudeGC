"""Multi-vehicle routing test for tar_router_cli (same engine as QlaudeGC).

Usage: python multi_vehicle_test.py <path to tar_router_cli.exe>

Two vehicles (sys 1 and 2) share one link (UDP server 25550, like QGC's 14550 link). Each has its own
MAVSDK port, filtered with AllowSrcSysOut, plus one unfiltered "all vehicles" port. Both MAVSDK instances
use the MAVSDK default system id 245, as they would in real use.
"""
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from router_test import command_long, frame_v2, heartbeat  # noqa: E402

CONF = """
[General]
TcpServerPort = 0

[UdpEndpoint vehicles]
Mode = Server
Address = 127.0.0.1
Port = 25550

[UdpEndpoint mavsdk_v1]
Mode = Normal
Address = 127.0.0.1
Port = 25541
AllowSrcSysOut = 1

[UdpEndpoint mavsdk_v2]
Mode = Normal
Address = 127.0.0.1
Port = 25542
AllowSrcSysOut = 2

[UdpEndpoint mavsdk_all]
Mode = Normal
Address = 127.0.0.1
Port = 25540
BlockSrcSysOut = 245
"""

BAD_CONF = """
[UdpEndpoint x]
Mode = Normal
Address = 127.0.0.1
Port = 1
AllowSrcSysOut = 300
"""

results = []


def check(name, cond):
    results.append(bool(cond))
    print(("PASS " if cond else "FAIL ") + name)


def sock(port=0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    s.settimeout(0.05)
    return s


def drain(s, wait=0.4):
    end, data, src = time.time() + wait, b"", None
    while time.time() < end:
        try:
            chunk, src = s.recvfrom(65535)
            data += chunk
        except socket.timeout:
            pass
    return data, src


def command_ack(sysid, target_sys, target_comp=190):
    # COMMAND_ACK (77): command u16, result u8, then extensions progress u8, result_param2 i32, target sys/comp
    payload = struct.pack("<HBBiBB", 400, 0, 0, 0, target_sys, target_comp)
    return frame_v2(77, payload, sysid, 1)


def main():
    exe = str(Path(sys.argv[1]).resolve())
    tmp = Path(tempfile.mkdtemp())
    conf, bad = tmp / "main.conf", tmp / "bad.conf"
    conf.write_text(CONF)
    bad.write_text(BAD_CONF)

    chk = subprocess.run([exe, "--check", str(bad)], capture_output=True, text=True)
    check("AllowSrcSysOut = 300 rejected (system ids are 1-255)", chk.returncode == 1 and "out of range" in chk.stdout)

    v1, v2 = sock(), sock()
    m1, m2, mall = sock(25541), sock(25542), sock(25540)
    log = open(tmp / "router.log", "w")
    proc = subprocess.Popen([exe, str(conf), "--stats"], stdout=log, stderr=subprocess.STDOUT)
    time.sleep(1.0)
    try:
        hb1, hb2 = heartbeat(1), heartbeat(2)
        v1.sendto(hb1, ("127.0.0.1", 25550))
        v2.sendto(hb2, ("127.0.0.1", 25550))
        d1, r1 = drain(m1)
        d2, r2 = drain(m2, 0.1)
        da, _ = drain(mall, 0.1)
        check("vehicle 1 heartbeat reaches mavsdk_v1 only", hb1 in d1 and hb1 not in d2)
        check("vehicle 2 heartbeat reaches mavsdk_v2 only", hb2 in d2 and hb2 not in d1)
        check("unfiltered mavsdk_all gets both vehicles", hb1 in da and hb2 in da)

        assert r1 is not None and r2 is not None, "router never sent to the MAVSDK ports"
        # Both MAVSDK instances announce themselves as sys 245 (MAVSDK default)
        m1.sendto(heartbeat(245, 190), r1)
        m2.sendto(heartbeat(245, 190), r2)
        time.sleep(0.2)
        for s in (v1, v2, m1, m2, mall):
            drain(s, 0.1)

        # MAVSDK on vehicle 2's port commands vehicle 2
        cmd = command_long(245, 2)
        m2.sendto(cmd, r2)
        got_v2, _ = drain(v2)
        check("mavsdk_v2 COMMAND_LONG(target 2) reaches vehicle 2", cmd in got_v2)
        check("one MAVSDK's traffic is not echoed to the other MAVSDK port", cmd not in drain(m1, 0.1)[0])
        check("MAVSDK traffic hidden from mavsdk_all (BlockSrcSysOut=245)", cmd not in drain(mall, 0.1)[0])

        # Vehicle 2 acks to sys 245: only mavsdk_v2 must get it, although both MAVSDKs are sys 245
        ack = command_ack(2, 245)
        v2.sendto(ack, ("127.0.0.1", 25550))
        a2, _ = drain(m2)
        a1, _ = drain(m1, 0.1)
        check("vehicle 2 COMMAND_ACK to sys 245 reaches mavsdk_v2", ack in a2)
        check("vehicle 2 COMMAND_ACK NOT delivered to mavsdk_v1", ack not in a1)
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        log.close()
        print("---- router stats (last) ----")
        print("\n".join((tmp / "router.log").read_text().splitlines()[-4:]))

    print(f"\n{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
