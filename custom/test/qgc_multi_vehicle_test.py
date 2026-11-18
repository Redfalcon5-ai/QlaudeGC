"""Two vehicles on QGC's UDP 14550 link, per-vehicle MAVSDK ports (run while QlaudeGC is open with
endpoints mavsdk_v1 -> 14541 AllowSrcSysOut=1 and mavsdk_v2 -> 14542 AllowSrcSysOut=2).
"""
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from qgc_bridge_test import frames  # noqa: E402
from router_test import command_long, heartbeat  # noqa: E402


def sock(port=0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    s.settimeout(0.02)
    return s


def main():
    veh = {1: sock(), 2: sock()}
    mav = {1: sock(14541), 2: sock(14542)}
    seen = {1: set(), 2: set()}          # sysids received on each MAVSDK port
    router = {}
    cmd_seen = {1: False, 2: False}       # vehicle got the command addressed to it
    cmds = {1: command_long(245, 1), 2: command_long(245, 2)}
    end, next_hb, sent = time.time() + 20, 0, 0
    while time.time() < end:
        now = time.time()
        if now >= next_hb:
            for sysid, s in veh.items():
                s.sendto(heartbeat(sysid, 1), ("127.0.0.1", 14550))
            next_hb = now + 0.25
        for port_id, s in mav.items():
            try:
                data, addr = s.recvfrom(65535)
                router[port_id] = addr
                for sysid, _, _ in frames(data):
                    seen[port_id].add(sysid)
            except socket.timeout:
                pass
        if len(router) == 2 and now - sent > 1.0:
            for port_id in (1, 2):
                mav[port_id].sendto(heartbeat(245, 190), router[port_id])
                mav[port_id].sendto(cmds[port_id], router[port_id])
            sent = now
        for sysid, s in veh.items():
            try:
                data, _ = s.recvfrom(65535)
                for _, _, raw in frames(data):
                    if raw == cmds[sysid]:
                        cmd_seen[sysid] = True
            except socket.timeout:
                pass
        if all(cmd_seen.values()) and seen[1] and seen[2] and now > end - 15:
            break

    checks = [
        ("port 14541 receives vehicle 1 only", seen[1] == {1}),
        ("port 14542 receives vehicle 2 only", seen[2] == {2}),
        ("MAVSDK on 14541 commands vehicle 1 through QGC's link", cmd_seen[1]),
        ("MAVSDK on 14542 commands vehicle 2 through QGC's link", cmd_seen[2]),
    ]
    for name, ok in checks:
        print(("PASS " if ok else "FAIL ") + name)
    print(f"  (sys ids seen: 14541={sorted(seen[1])} 14542={sorted(seen[2])})")
    sys.exit(0 if all(ok for _, ok in checks) else 1)


if __name__ == "__main__":
    main()
