# QlaudeGC

QGroundControl v5.1.5 (stable) custom build with a **built-in MAVLink router**.

## What it adds

- **Router engine** compiled into QGC, running on its own thread so UI stalls never delay traffic.
  It reads mavlink-router's `main.conf` format (`[UdpEndpoint]` Server/Normal, `[TcpEndpoint]`,
  `[UartEndpoint]`, `[General] TcpServerPort`, `AllowMsgIdOut`/`BlockMsgIdOut`) and routes the same way:
  targeted messages go only to endpoints that have seen the target system, broadcasts go everywhere
  except back to the source, and frames are forwarded byte-for-byte (signed frames pass through).
- **QGC keeps its normal links** (drone on UDP 14550, radios, ...). Every QGC link is bridged into the
  router: what QGC receives is routed to the endpoints, and what the endpoints send to a vehicle goes out
  through that QGC link. Default config adds **MAVSDK on UDP 14540** (`udp://:14540`).
- **Graphical config editor**: router indicator -> Configure... edits main.conf (endpoint type, address,
  port, serial device/baud/flow control, message-id filters, TCP server port), validates and applies.
  **Import...** loads any main.conf (including existing mavlink-router configs), validates it and makes it
  active (kept verbatim, comments included); **Export active config...** saves the running config to a file.
- **Toolbar indicator** (network icon): green = vehicle traffic flowing, number = active endpoints,
  red = config error, "Off" = disabled. Click for per-endpoint status (rx/tx rates, systems seen,
  CRC errors), enable switch, Configure..., and (expanded) Edit File / Open Folder / Reload and the log.
- **Multiple vehicles**: give every vehicle a unique `MAV_SYS_ID`. Per-endpoint vehicle filters
  (`AllowSrcSysOut` / `BlockSrcSysOut`, mavlink-router names) restrict an endpoint to chosen system ids;
  replies addressed to a GCS id (e.g. MAVSDK's 245) only reach endpoints allowed to see that vehicle, so
  several MAVSDK instances can share sys id 245. Editor button **MAVSDK per vehicle** adds one endpoint
  per connected vehicle at UDP `14540 + sys id` (vehicle 1 -> `udp://:14541`, ...). The indicator shows
  "Heard from" / "Forwarding from" system ids per endpoint. Commands leaving through a shared QGC UDP
  link go to every vehicle on it (QGC's UDP link behaviour); vehicles ignore ones not addressed to them.
- **TCP server** on 5760 by default so MAVSDK, mavproxy, a second QGC, etc. can share the link.
- `tar_router_cli.exe`: the same router as a console program (`tar_router_cli main.conf --stats`);
  `tar_router_cli --check file.conf` validates a config with the same rules as Import.

Config file: `%LOCALAPPDATA%\TAR\QlaudeGC\router\main.conf` (exact path shown in the indicator;
created from `res/router/main.conf` on first run).

## Layout

```
custom/
  CMakeLists.txt, cmake/CustomOverrides.cmake   build wiring, app name
  custom.qrc, res/qml, res/router/main.conf     indicator, config editor dialog, default config
  src/TarPlugin.*                               QGCCorePlugin subclass (CUSTOMCLASS)
  src/Router/MavlinkFrameParser.*               v1/v2 framer, CRC check, target extraction
  src/Router/RouterConfig.*                     main.conf parser
  src/Router/RouterEndpoint.*                   UDP server/client, TCP client/accepted, serial
  src/Router/RouterCore.*                       routing table + stats (router thread)
  src/Router/TarRouter.*                        QML facade, QGC link bridge, config editor API
  src/Router/tar_router_cli.cc                  standalone router
  test/router_test.py                           end-to-end routing test against the CLI
  test/multi_vehicle_test.py                    per-vehicle filters / shared GCS id (CLI)
  test/qgc_bridge_test.py, qgc_multi_vehicle_test.py   same, through the running app
```

## Build (Windows)

Needs VS 2022 Build Tools (C++), Qt 6.11.1 msvc2022_64 at `C:\Qt` (installed with aqtinstall), and
pip `cmake` + `ninja`. From the repo root:

```
tar-build.bat            configure + build  (RelWithDebInfo, no LTO)
tar-build.bat build      incremental build
```

Output: `build\RelWithDebInfo\QlaudeGC.exe` (see the build log for the exact path) and
`build\tar_router_cli.exe`. Test the router: `python custom\test\router_test.py build\tar_router_cli.exe`.

## ROS 2 bag recording (Jetson)

Toolbar button **Start recording bag** starts `ros2 bag record -a [-x <regex>]` on the Jetson; it then
becomes **Stop recording bag** (with a confirmation). Click the status text for the details drawer:
Stop, Stop and download, Download bag, settings (host, user, key, regex, download folder) and a Jetson check.

- **Design.** QGC runs `ssh.exe` (key auth, `StrictHostKeyChecking=yes`, `BatchMode=yes`) with a fixed remote command
  (`qgc-bagctl`). The recorder runs as a `systemd --user` service on the Jetson, so closing QGC, SSH or NoMachine
  does not stop it. QGC rediscovers a running recording on launch and after network drops.
- **State.** "Recording" is shown only when the Jetson reports it. SSH failures show "unknown", never "stopped".
- **Download.** After stop and finalization, `scp -r` copies the bag into `.incomplete-<id>`, checks it against a
  server-side file list (names and sizes, optional SHA-256), then renames it to `qgc-<id>`. Nothing is deleted on the Jetson.
- **Setup.** Install `jetson/qgc-bagctl` on the Jetson first: see `jetson/SETUP.md`. Settings live under the
  `TarBagRecorder` QSettings group.
- **Troubleshooting.** Use "Run Jetson check" in the drawer, or `ssh -T -o BatchMode=yes <user>@<host> '~/.local/bin/qgc-bagctl doctor --json'`.
  Host-key and auth errors are shown verbatim in the drawer; fix them with a one-time manual `ssh` login.
- **Tests.** `python3 -m unittest jetson/test_qgc_bagctl.py -v` (Jetson side, no ROS needed).

## Updating QGC

The custom code only touches public QGC APIs (`QGCCorePlugin`, `LinkManager`, `UDPConfiguration`), so
moving to a newer QGC tag is: fetch the tag, rebase the `tar-custom` branch, rebuild.

## Claude chat sidebar

Click the chat-bubble icon in the toolbar to open a Claude chat panel on the right side of the window. Drag its left edge to resize it.

- **How it works.**
  - Each message runs the locally installed Claude Code CLI (`claude -p`, stream-json) in the background, and the next message resumes the same session.
  - The conversation is saved to `%APPDATA%\QGroundControl\QlaudeGC Daily\assistant\chat.json`.
  - **New** starts a fresh session.
- **Live context.** Each message carries a snapshot of QGC's state at the moment you press Send:
  - every vehicle's mode, armed and flying state, prearm error, GPS, batteries, altitude, position and recent vehicle messages;
  - the router's endpoints and rates, any config errors and the log tail.

  The snapshot is not shown in the chat. Use **Settings > Preview context** to see exactly what is sent, or turn the snapshot off there.
- **Vehicle parameters.** QGC hosts a small MCP server named `qgc` on `127.0.0.1`.
  - It listens on a random port and requires a per-launch token. Its config is written to `assistant\mcp.json`.
  - Tools:
    - `list_vehicles`;
    - `search_parameters` (by name or description);
    - `get_parameters` (value, units, min/max, default, options, bitmask bits, whether a reboot is required);
    - `set_parameters`.
  - **Approval.** `set_parameters` first checks each value against the parameter's type and range. It then shows an approval card in the chat listing old → new values, the reason, and warnings (vehicle armed, reboot needed). Nothing is sent until you click **Approve**. The tool waits for the vehicle to acknowledge the writes and reports back.
  - **Reject, stop or timeout.** Reject, Stop, or no answer within 10 minutes all leave the vehicle unchanged.
  - Arming, flight modes and other vehicle commands are not exposed.
- **QGC app settings.** `search_app_settings`, `get_app_settings` and `set_app_settings` cover every visible QGC setting group (video, units, maps, fly/plan view, auto-connect, MAVLink, RTK, ADS-B, joystick, logs...).
  - Settings are named `<group>.<name>`, e.g. `video.videoSource`, `video.rtspUrl`. Enum settings accept the option label.
  - Changes use the same approval card and are applied and saved as soon as you approve.
- **Comm Links.** `list_comm_links`, `set_comm_link` (create or edit by name, optionally connect; UDP `localPort`/`hostList`, TCP `host`/`port`, serial `portName`/`baud`/..., `autoConnect`, `highLatency`) and `comm_link_action` (connect, disconnect, delete). Every change needs approval.
- **Everything else.** `qgc_inspect`, `qgc_set_property` and `qgc_call` reflect QGC's live object tree, the same Q_PROPERTYs and Q_INVOKABLEs the UI uses.
  - Roots: `vehicle` (active), `vehicle<sysid>`, `multiVehicleManager`, `linkManager`, `settingsManager`, `videoManager`, `joystickManager`, `adsbVehicleManager`, `positionManager`, `corePlugin`, `QGroundControl` (QML singleton), `window`. Paths use dots and `[index]`; Vehicle also exposes `missionManager`, `geoFenceManager`, `rallyPointManager`.
  - Getter-style calls (`get*`, `is*`, `has*`, `find*`...) run directly. Every property write and other call shows an approval card, with warnings when it targets an armed or flying vehicle.
  - Objects returned by calls become handles `$N` that later paths and arguments can use.
- **Input history.** Up/Down in the message box recall messages you sent (when the cursor is on the first/last line), like a terminal.
- **Slash commands.** Claude Code commands that work in print mode can be typed in the sidebar, e.g. `/goal <condition>`, `/cost`, `/config key=value`. Commands that only reply (like `/cost`) show their output as a normal message. `/goal` keeps Claude working until the condition is met; press Stop to end it early.
- **File permissions.** These apply to files only; parameter changes always need approval.
  - **Read only** (the default): Read, Glob, Grep, WebSearch and WebFetch.
  - **Can edit files**: also Edit and Write.
  - Shell tools are always blocked, and Claude cannot command vehicles.
  - The router config folder is added with `--add-dir`, so Claude can read the config, and edit it in edit mode. Press Reload afterwards.
- **Requirements.** Claude Code must be installed and signed in (run `claude` once in a terminal). It is found on PATH or in `~/.local/bin`.
  - If it isn't signed in, the chat shows an error with a **Sign in to Claude Code...** button that opens Claude Code in a terminal. Sign in there, then resend; the conversation is kept.
  - If a saved conversation can't be resumed, the sidebar starts a new one and resends the message automatically.
  - The working folder defaults to `Desktop\TAR` and can be changed in Settings, along with the model.
- **Code.**
  - `src/Assistant/TarAssistant.{h,cc}`: runs the CLI, parses the stream-json output, builds the context snapshot and holds the chat model.
  - `src/Assistant/TarMcpServer.{h,cc}`: the MCP server (HTTP and JSON-RPC) and the parameter tools.
  - Tests: `test/fake_px4_params.py`, a fake PX4 with parameters, and `test/qgc_param_tools_test.py`, which runs 12 checks against the running app.
  - `res/qml/AssistantPanel.qml` and `AssistantIndicator.qml`: the panel and its toolbar button.
  - `TarPlugin::createRootWindow` adds the panel to the main window.
