"""End-to-end test of the router built into QlaudeGC (run while the app is open).

  fake vehicle (sys 1) --UDP--> QGC autoconnect :14550 --bridge--> router --UDP--> MAVSDK :14540
  MAVSDK COMMAND_LONG(target 1) --> router --> QGC link --> fake vehicle

Assumes the default router config (mavsdk endpoint -> 127.0.0.1:14540).
"""
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from router_test import command_long, heartbeat  # noqa: E402

PX4_AUTOPILOT = 12


def frames(blob):
    """Yield (sysid, msgid) for each MAVLink2 frame in blob."""
    i = 0
    while i < len(blob):
        if blob[i] != 0xFD or i + 10 > len(blob):
            i += 1
            continue
        plen = blob[i + 1]
        signed = blob[i + 2] & 1
        total = 10 + plen + 2 + (13 if signed else 0)
        yield blob[i + 5], blob[i + 7] | (blob[i + 8] << 8) | (blob[i + 9] << 16), blob[i:i + total]
        i += total


def main():
    vehicle = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    vehicle.bind(("127.0.0.1", 0))
    vehicle.settimeout(0.05)
    mavsdk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    mavsdk.bind(("127.0.0.1", 14540))
    mavsdk.settimeout(0.05)

    router_addr = None
    got_hb_at_mavsdk = 0
    got_cmd_at_vehicle = False
    qgc_msgs_at_vehicle = set()
    cmd = command_long(245, 1)
    t_end = time.time() + 25
    next_hb = 0
    sent_cmd = 0
    while time.time() < t_end:
        now = time.time()
        if now >= next_hb:
            vehicle.sendto(heartbeat(1, 1), ("127.0.0.1", 14550))
            next_hb = now + 0.25
        try:
            data, addr = mavsdk.recvfrom(65535)
            for sysid, msgid, _ in frames(data):
                if sysid == 1 and msgid == 0:
                    got_hb_at_mavsdk += 1
                    router_addr = addr
        except socket.timeout:
            pass
        if router_addr and got_hb_at_mavsdk >= 3 and now - sent_cmd > 1.0 and not got_cmd_at_vehicle:
            mavsdk.sendto(heartbeat(245, 190), router_addr)
            mavsdk.sendto(cmd, router_addr)
            sent_cmd = now
        try:
            data, _ = vehicle.recvfrom(65535)
            for sysid, msgid, raw in frames(data):
                if raw == cmd:
                    got_cmd_at_vehicle = True
                if sysid == 255:
                    qgc_msgs_at_vehicle.add(msgid)
        except socket.timeout:
            pass
        if got_cmd_at_vehicle and got_hb_at_mavsdk >= 5 and qgc_msgs_at_vehicle:
            break

    ok1 = got_hb_at_mavsdk >= 3
    ok2 = got_cmd_at_vehicle
    ok3 = len(qgc_msgs_at_vehicle) > 0
    print(("PASS " if ok1 else "FAIL ") + f"vehicle heartbeats reached MAVSDK port 14540 ({got_hb_at_mavsdk})")
    print(("PASS " if ok2 else "FAIL ") + "MAVSDK COMMAND_LONG reached the vehicle through QGC's 14550 link")
    print(("PASS " if ok3 else "FAIL ") + f"QGC itself still talks to the vehicle (msg ids from sys 255: {sorted(qgc_msgs_at_vehicle)[:8]})")
    sys.exit(0 if ok1 and ok2 and ok3 else 1)


if __name__ == "__main__":
    main()
