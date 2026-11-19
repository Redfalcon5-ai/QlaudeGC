# Jetson setup for QlaudeGC bag recording

Replace every `<PLACEHOLDER>`. Nothing here is guessed: find the values on the Jetson in a terminal where
`ros2 bag record -a` works today (`echo $ROS_DOMAIN_ID $RMW_IMPLEMENTATION`, `ls /opt/ros`, `which ros2`).

## 1. Install (on the Jetson, as the normal user, not root)

From the PC (PowerShell, in the repo folder):

```powershell
scp jetson\qgc-bagctl "jetson\qgc-rosbag@.service" "jetson\qgc-bag-guard@.service" jetson\config.example.json <USER>@<JETSON_HOST>:/tmp/
```

On the Jetson:

```bash
mkdir -p ~/.local/bin ~/.config/systemd/user ~/.config/qgc-bagctl ~/.local/state/qgc-bagctl ~/rosbags
install -m 755 /tmp/qgc-bagctl ~/.local/bin/qgc-bagctl
install -m 644 "/tmp/qgc-rosbag@.service" ~/.config/systemd/user/
install -m 644 "/tmp/qgc-bag-guard@.service" ~/.config/systemd/user/
cp -n /tmp/config.example.json ~/.config/qgc-bagctl/config.json
nano ~/.config/qgc-bagctl/config.json      # set ros_setup (distro, workspace), env (ROS_DOMAIN_ID, RMW), bag_root
systemctl --user daemon-reload
sudo loginctl enable-linger "$USER"        # lets the recorder survive logout; verify below
loginctl show-user "$USER" -p Linger       # Linger=yes
~/.local/bin/qgc-bagctl doctor --json
```

`doctor` must report `"ok": true`. If `systemctl --user` fails over SSH ("Failed to connect to bus"),
linger is not on or `XDG_RUNTIME_DIR` is unset; enable linger and log in again.
If lingering is not allowed on your Jetson, use a system unit instead (`/etc/systemd/system/qgc-rosbag@.service`
with `User=<USER>`, and change `systemctl --user` to `sudo systemctl` plus a sudoers rule) - not implemented here.

Optional: pin the storage format, e.g. `"extra_record_args": ["--storage", "mcap"]`. Empty means rosbag2 defaults.
Confirm flags with `ros2 bag record --help` on your distro.

## 2. Test on the Jetson before using QGC

```bash
echo '{"exclude_regex":"<YOUR_REGEX_OR_EMPTY>","vehicle_label":"drone1"}' | qgc-bagctl start --json
qgc-bagctl status --json        # starting -> recording (only once the bag directory exists)
qgc-bagctl start --json </dev/null   # must fail with already_recording
qgc-bagctl stop --id <SESSION_ID> --json
qgc-bagctl status --json        # stopping -> finalized
ros2 bag info ~/rosbags/qgc-<SESSION_ID>
```

Then close the terminal / NoMachine and confirm recording continues (`status` from a new session).

## 3. SSH from the Windows PC (key only, no passwords)

```powershell
ssh-keygen -t ed25519 -f $env:USERPROFILE\.ssh\qgc_jetson     # if you do not already have a key
type $env:USERPROFILE\.ssh\qgc_jetson.pub | ssh <USER>@<JETSON_HOST> "mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys"
ssh <USER>@<JETSON_HOST> exit        # ONCE, interactively: verify the host key fingerprint and accept it
ssh -T -o BatchMode=yes <USER>@<JETSON_HOST> '~/.local/bin/qgc-bagctl doctor --json'
```

QlaudeGC runs ssh with `StrictHostKeyChecking=yes` and `BatchMode=yes`, so an unknown host key or a
missing key fails with a clear error rather than prompting or auto-accepting. Windows needs the
"OpenSSH Client" optional feature (`ssh.exe`, `scp.exe`). The host can be a Tailscale name/IP or the Jetson's LAN IP;
port 22 must be reachable from the PC (test: `Test-NetConnection <JETSON_HOST> -Port 22`).

## 4. Safety notes

- No network port is opened on the Jetson; everything goes through sshd.
- Bags are never deleted by this tooling. Clean up `~/rosbags` yourself after you verified the download.
- A separate user service stops each recording when free space reaches `min_free_bytes` plus 1 GiB of headroom.
  It sends the recorder the same graceful stop request as the UI, then the bag can be downloaded normally.
- A Jetson reboot or power loss ends the recording; status then shows `failed` (recorder not running) and,
  for sqlite3 bags, the partial `.db3` can often be recovered with `ros2 bag reindex`.
- Not covered by the unit tests (need hardware): real `ros2 bag record` signal handling, linger, DDS discovery.
