"""Fake PX4 vehicle with a small parameter set, for testing parameter tools against QlaudeGC.

Usage: python fake_px4_params.py [seconds] [sysid]

Sends heartbeats to QGC on UDP 14550, answers PARAM_REQUEST_LIST / PARAM_REQUEST_READ and acknowledges
PARAM_SET with PARAM_VALUE like the autopilot does. Prints every PARAM_SET it receives.
"""
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import router_test  # noqa: E402
from router_test import frame_v2, heartbeat  # noqa: E402

router_test.CRC_EXTRA.update({20: 214, 21: 159, 22: 220, 23: 168})

PARAMS = {  # all REAL32 so encoding questions (bytewise vs cast) don't matter
    "MPC_XY_VEL_MAX": 12.0,
    "MIS_TAKEOFF_ALT": 2.5,
    "COM_RC_LOSS_T": 0.5,
    "MPC_Z_VEL_MAX_UP": 3.0,
    "RTL_RETURN_ALT": 60.0,
}
NAMES = list(PARAMS)


def param_value(sysid, name):
    payload = struct.pack("<fHH16sB", PARAMS[name], len(NAMES), NAMES.index(name), name.encode(), 9)
    return frame_v2(22, payload, sysid, 1)


def frames(blob):
    i = 0
    while i < len(blob):
        if blob[i] != 0xFD or i + 10 > len(blob):
            i += 1
            continue
        plen = blob[i + 1]
        signed = blob[i + 2] & 1
        msgid = blob[i + 7] | (blob[i + 8] << 8) | (blob[i + 9] << 16)
        yield msgid, blob[i + 10:i + 10 + plen]
        i += 10 + plen + 2 + (13 if signed else 0)


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 300
    sysid = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(0.05)
    qgc = ("127.0.0.1", 14550)
    end, next_hb = time.time() + seconds, 0
    while time.time() < end:
        now = time.time()
        if now >= next_hb:
            s.sendto(heartbeat(sysid, 1), qgc)
            next_hb = now + 0.5
        try:
            data, addr = s.recvfrom(65535)
        except (socket.timeout, ConnectionResetError):  # Windows: ICMP unreachable before QGC listens
            continue
        qgc = addr
        for msgid, raw in frames(data):
            if msgid == 21:  # PARAM_REQUEST_LIST
                for n in NAMES:
                    s.sendto(param_value(sysid, n), qgc)
            elif msgid == 20:  # PARAM_REQUEST_READ
                p = raw.ljust(20, b"\0")
                index, = struct.unpack_from("<h", p, 0)
                name = p[4:20].rstrip(b"\0").decode()
                if index >= 0 and index < len(NAMES):
                    name = NAMES[index]
                if name in PARAMS:
                    s.sendto(param_value(sysid, name), qgc)
            elif msgid == 23:  # PARAM_SET
                p = raw.ljust(23, b"\0")
                value, target = struct.unpack_from("<fB", p, 0)
                name = p[6:22].rstrip(b"\0").decode()
                if target == sysid and name in PARAMS:
                    PARAMS[name] = value
                    print(f"PARAM_SET {name} = {value:g}", flush=True)
                    s.sendto(param_value(sysid, name), qgc)


if __name__ == "__main__":
    main()
