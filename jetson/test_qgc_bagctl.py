"""Unit tests for qgc-bagctl (no ROS or systemd needed; systemctl is faked).

Run on any Linux/macOS/WSL box:  python3 -m unittest jetson/test_qgc_bagctl.py -v
"""
import importlib.machinery
import importlib.util
import io
import json
import os
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))


def load_module(state, config):
    os.environ["QGC_BAGCTL_STATE"] = state
    os.environ["QGC_BAGCTL_CONFIG"] = config
    loader = importlib.machinery.SourceFileLoader("qgc_bagctl", os.path.join(HERE, "qgc-bagctl"))
    spec = importlib.util.spec_from_loader("qgc_bagctl", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


class FakeProc:
    def __init__(self, rc=0, out="", err=""):
        self.returncode, self.stdout, self.stderr = rc, out, err


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = self.tmp.name
        self.bags = os.path.join(t, "bags")
        os.makedirs(self.bags)
        cfgpath = os.path.join(t, "config.json")
        with open(cfgpath, "w") as f:
            json.dump({"bag_root": self.bags, "ros_setup": ["/nonexistent/setup.bash"], "min_free_bytes": 1}, f)
        self.m = load_module(os.path.join(t, "state"), cfgpath)
        self.cfg = self.m.load_config()
        self.live = False          # fake: is the systemd unit active?
        self.calls = []
        self.m.systemctl = self.fake_systemctl

    def tearDown(self):
        self.tmp.cleanup()

    def fake_systemctl(self, *args, check=False):
        self.calls.append(args)
        if args[0] == "show":
            return FakeProc(0, "ActiveState=%s\nSubState=x\nResult=success\nExecMainStatus=0\nLoadState=loaded\n" %
                            ("active" if self.live else "inactive"))
        if args[0] == "start":
            self.live = True
        if args[0] == "stop":
            self.live = False
            self.finish_bag()
        return FakeProc(0)

    def finish_bag(self):
        for m in self.m.all_sessions():
            bag = m["bag_path"]
            os.makedirs(bag, exist_ok=True)
            with open(os.path.join(bag, "x_0.db3"), "wb") as f:
                f.write(b"data")
            with open(os.path.join(bag, "metadata.yaml"), "w") as f:
                f.write("rosbag2_bagfile_information:\n  relative_file_paths:\n    - x_0.db3\n")

    def start(self, **opts):
        with mock.patch("sys.stdin", io.StringIO(json.dumps(opts))):
            return self.m.cmd_start(None)


class StartTests(Base):
    def test_start_returns_starting_and_creates_manifest(self):
        res, code = self.start(exclude_regex="/a/.*|/b", vehicle_label="drone1")
        self.assertEqual(code, 0)
        self.assertEqual(res["state"], "starting")
        self.assertTrue(res["bag_path"].startswith(self.bags + "/qgc-"))
        self.assertIn(("start", "--no-block", self.m.unit_name(res["session_id"])), self.calls)
        self.assertIn(("start", "--no-block", self.m.guard_unit_name(res["session_id"])), self.calls)

    def test_second_start_rejected(self):
        self.start()
        with self.assertRaises(self.m.CtlError) as cm:
            self.start()
        self.assertEqual(cm.exception.code, "already_recording")
        self.assertEqual(sum(1 for c in self.calls if c[:2] == ("start", "--no-block")
                             and c[2].startswith(self.m.UNIT_PREFIX)), 1)

    def test_bad_inputs(self):
        for opts, code in [({"exclude_regex": "a\nb"}, "bad_regex"), ({"vehicle_label": "../x"}, "bad_label"),
                           ({"exclude_regex": "x" * 3000}, "bad_regex")]:
            with self.assertRaises(self.m.CtlError) as cm:
                self.start(**opts)
            self.assertEqual(cm.exception.code, code)

    def test_disk_full(self):
        self.cfg["min_free_bytes"] = 1 << 60
        with mock.patch.object(self.m, "load_config", return_value=self.cfg):
            with self.assertRaises(self.m.CtlError) as cm:
                self.start()
        self.assertEqual(cm.exception.code, "disk_full")

    def test_session_id_validation(self):
        for bad in ["../etc", "a/b", "", "a b", "x" * 200]:
            with self.assertRaises(self.m.CtlError):
                self.m.check_session_id(bad)


class StateTests(Base):
    def test_disk_guard_stops_and_finalizes_recording(self):
        res, _ = self.start()
        os.makedirs(res["bag_path"])
        with mock.patch.object(self.m, "free_bytes", return_value=self.cfg["min_free_bytes"]):
            out, code = self.m.cmd_guard(mock.Mock(session=res["session_id"]))
        self.assertEqual(code, 0)
        self.assertEqual(out["state"], "finalized")
        self.assertIn(("stop", "--no-block", self.m.unit_name(res["session_id"])), self.calls)

    def test_concurrent_manifest_writes_use_distinct_temp_files(self):
        sid = "concurrent"
        barrier = threading.Barrier(2)
        errors = []
        original_replace = os.replace

        def replace_after_both_writers_finish(source, target):
            barrier.wait(timeout=5)
            original_replace(source, target)

        def write(value):
            try:
                self.m.write_manifest(sid, {"session_id": sid, "value": value})
            except Exception as e:
                errors.append(e)

        with mock.patch.object(self.m.os, "replace", side_effect=replace_after_both_writers_finish):
            threads = [threading.Thread(target=write, args=(value,)) for value in (1, 2)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=10)

        self.assertFalse(any(thread.is_alive() for thread in threads))
        self.assertEqual(errors, [])
        self.assertIn(self.m.read_manifest(sid)["value"], (1, 2))

    def test_starting_then_recording(self):
        res, _ = self.start()
        st, _ = self.m.cmd_status(None)
        self.assertEqual(st["state"], "starting")           # unit live but no bag dir yet
        os.makedirs(res["bag_path"])
        st, _ = self.m.cmd_status(None)
        self.assertEqual(st["state"], "recording")

    def test_stop_finalizes(self):
        res, _ = self.start()
        os.makedirs(res["bag_path"])
        args = mock.Mock(id=res["session_id"])
        out, _ = self.m.cmd_stop(args)
        self.assertEqual(out["state"], "finalized")
        out2, _ = self.m.cmd_stop(args)                        # idempotent
        self.assertEqual(out2["state"], "finalized")
        man, _ = self.m.cmd_manifest(mock.Mock(id=res["session_id"], sha256=True))
        self.assertTrue(any(f["path"] == "x_0.db3" for f in man["files"]))
        self.assertTrue(any(f["path"] == "metadata.yaml" for f in man["files"]))
        self.assertTrue(all("sha256" in f for f in man["files"]))

    def test_crash_is_failed_not_stopped(self):
        res, _ = self.start()
        os.makedirs(res["bag_path"])
        self.live = False                                       # unit died, nobody asked to stop
        st, _ = self.m.cmd_status(None)
        self.assertEqual(st["state"], "failed")
        self.assertIn("not running", st["last_error"])
        with self.assertRaises(self.m.CtlError) as cm:
            self.m.cmd_manifest(mock.Mock(id=res["session_id"], sha256=False))
        self.assertEqual(cm.exception.code, "not_finalized")

    def test_stop_without_metadata_is_incomplete(self):
        res, _ = self.start()
        os.makedirs(res["bag_path"])
        m = self.m.read_manifest(res["session_id"])
        m["stop_requested"] = True
        self.m.write_manifest(res["session_id"], m)
        self.live = False
        st, _ = self.m.cmd_status(None)
        self.assertEqual(st["state"], "failed")
        self.assertIn("incomplete", st["last_error"])

    def test_idle(self):
        st, _ = self.m.cmd_status(None)
        self.assertEqual(st["state"], "idle")


class RunTests(Base):
    def test_run_builds_argv_without_shell_and_omits_empty_regex(self):
        for regex, expect in [("/a|/b", ["-x", "/a|/b"]), ("", None)]:
            self.live = False
            for m in self.m.all_sessions():
                m["state"] = "failed"
                self.m.write_manifest(m["session_id"], m)
            res, _ = self.start(exclude_regex=regex)
            captured = {}
            with mock.patch.object(self.m, "build_env", return_value={"PATH": "/bin"}), \
                 mock.patch.object(self.m.shutil, "which", return_value="/bin/ros2"), \
                 mock.patch.object(self.m.os, "execve", side_effect=lambda e, a, env: captured.update(exe=e, argv=a)):
                self.m.cmd_run(mock.Mock(session=res["session_id"]))
            argv = captured["argv"]
            self.assertEqual(argv[:4], ["ros2", "bag", "record", "-a"])
            self.assertEqual(argv[-2:], ["-o", res["bag_path"]])
            if expect:
                self.assertEqual(argv[4:6], expect)
            else:
                self.assertNotIn("-x", argv)


if __name__ == "__main__":
    unittest.main()
