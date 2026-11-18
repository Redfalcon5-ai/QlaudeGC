"""Test the QGC MCP server (parameter tools) of a running QlaudeGC.

Run fake_px4_params.py first and wait for QGC to load its parameters. The last check raises an approval
card in the Claude sidebar: click Approve within 2 minutes (or pass --no-approval to skip it).
"""
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

CONF = Path(os.environ["APPDATA"]) / "QGroundControl" / "QlaudeGC Daily" / "assistant" / "mcp.json"
results = []


def check(name, cond, extra: object = ""):
    results.append(bool(cond))
    print(("PASS " if cond else "FAIL ") + name + (f"  [{extra}]" if extra and not cond else ""))


def rpc(url, token, method, params=None, rid=1, timeout=30):
    body = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}}).encode()
    req = urllib.request.Request(url, body, {"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                                             "Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def call(url, token, tool, args, timeout=30):
    res = rpc(url, token, "tools/call", {"name": tool, "arguments": args}, timeout=timeout)["result"]
    return res["content"][0]["text"], res.get("isError", False)


def main():
    approvals = "--no-approval" not in sys.argv
    cfg = json.loads(CONF.read_text())["mcpServers"]["qgc"]
    url, token = cfg["url"], cfg["headers"]["Authorization"].split(" ", 1)[1]

    try:
        rpc(url, "wrong", "initialize")
        check("wrong token rejected", False)
    except urllib.error.HTTPError as e:
        check("wrong token rejected (401)", e.code == 401)

    init = rpc(url, token, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "0"}})
    check("initialize", init["result"]["serverInfo"]["name"] == "qgc")
    tools = {t["name"] for t in rpc(url, token, "tools/list")["result"]["tools"]}
    expected = {"list_vehicles", "search_parameters", "get_parameters", "set_parameters",
                "search_app_settings", "get_app_settings", "set_app_settings",
                "list_comm_links", "set_comm_link", "comm_link_action",
                "qgc_inspect", "qgc_set_property", "qgc_call"}
    check(f"tools/list has the {len(expected)} expected tools", tools == expected, tools ^ expected)

    text, _ = call(url, token, "list_vehicles", {})
    vehicles = json.loads(text)
    check("list_vehicles: vehicle 1 with parameters loaded", any(v["vehicle_id"] == 1 and v["parameters_loaded"] for v in vehicles), text)

    text, err = call(url, token, "search_parameters", {"query": "takeoff"})
    check("search 'takeoff' finds MIS_TAKEOFF_ALT", not err and "MIS_TAKEOFF_ALT" in text, text[:300])

    text, err = call(url, token, "get_parameters", {"names": ["MPC_XY_VEL_MAX", "NOPE_PARAM"]})
    got = json.loads(text)
    p = got["parameters"][0]
    check("get_parameters returns value + metadata", p["name"] == "MPC_XY_VEL_MAX" and "max" in p and "description" in p, p)
    check("get_parameters reports unknown names", got.get("not_found") == ["NOPE_PARAM"])
    old_vel = float(p["value"])

    text, err = call(url, token, "set_parameters", {"changes": [{"name": "MPC_XY_VEL_MAX", "value": 99999}]})
    check("out-of-range value rejected before approval", err and "Nothing was changed" in text, text)
    text, err = call(url, token, "set_parameters", {"changes": [{"name": "NOPE_PARAM", "value": 1}]})
    check("unknown parameter rejected", err and "no such parameter" in text, text)
    text, err = call(url, token, "set_parameters", {"changes": [{"name": "MPC_XY_VEL_MAX", "value": old_vel}]})
    check("no-op change needs no approval", not err and "already have" in text, text)

    if approvals:
        new_vel = 8.0 if old_vel != 8.0 else 9.0
        text, err = call(url, token, "set_parameters",
                         {"changes": [{"name": "MPC_XY_VEL_MAX", "value": new_vel}, {"name": "MIS_TAKEOFF_ALT", "value": 4}],
                          "reason": "test"}, timeout=130)
        check("approved change applied and confirmed by vehicle", not err and "confirmed by the vehicle" in text, text)
        text, _ = call(url, token, "get_parameters", {"names": ["MPC_XY_VEL_MAX", "MIS_TAKEOFF_ALT"]})
        vals = {q["name"]: float(q["value"]) for q in json.loads(text)["parameters"]}
        check("QGC shows the new values", vals == {"MPC_XY_VEL_MAX": new_vel, "MIS_TAKEOFF_ALT": 4.0}, vals)

    print(f"\n{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
