#!/usr/bin/env python3
"""
Test for Task 1: User-space identity keystore and auto-discovery.
Verifies:
1. ab-ctl init --user <name> --generate-agent creates ~/.config/agentic-blackboard/identity.json with 0600 permissions.
2. Keystore contains user and agent tokens and default surface metadata.
3. Substrate graph contains user identity node, agent identity node, and DELEGATES_TO edge.
4. load_config() auto-discovers identity.json without CLI flags.
5. resolve_swarm_headers() automatically uses discovered user and agent IDs.
"""

import http.server
import importlib.util
import json
import os
import sqlite3
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
AB_CTL_PY = REPO_ROOT / "src" / "ab-ctl.py"


def load_ab_ctl_module():
    spec = importlib.util.spec_from_file_location("ab_ctl", AB_CTL_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestIdentityKeystore(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.fake_home = Path(self.temp_dir.name)
        self.config_dir = self.fake_home / ".config" / "agentic-blackboard"

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_keystore_creation_and_discovery(self):
        env = os.environ.copy()
        env["HOME"] = str(self.fake_home)
        env["XDG_CONFIG_HOME"] = str(self.fake_home / ".config")

        data_dir = self.fake_home / "data"

        # 1. Run init --user jason --generate-agent (dry-run / offline credentials file creation)
        cmd = [
            sys.executable, str(AB_CTL_PY), "init",
            "--user", "jason",
            "--generate-agent",
            "--data-dir", str(data_dir)
        ]
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, f"init failed: {proc.stderr}\nStdout: {proc.stdout}")

        # 2. Check identity.json existence and permissions
        identity_file = self.config_dir / "identity.json"
        self.assertTrue(identity_file.is_file(), f"identity.json not found at {identity_file}")
        mode = identity_file.stat().st_mode & 0o777
        self.assertEqual(mode, 0o600, f"identity.json has unsafe permissions: {oct(mode)}")

        # 3. Check identity.json structure
        with open(identity_file, "r", encoding="utf-8") as f:
            data = json.load(f)

        self.assertEqual(data["user"]["name"], "jason")
        self.assertEqual(data["user"]["id"], "user:jason")
        self.assertTrue(data["user"]["token"].startswith("ab_usr_"))
        self.assertEqual(data["agent"]["name"], "jason-agent")
        self.assertEqual(data["agent"]["id"], "agent:jason-agent")
        self.assertTrue(data["agent"]["token"].startswith("ab_agt_"))
        self.assertEqual(data["default_surface"]["type"], "workstation")
        self.assertEqual(data["default_surface"]["id"], "surface:workstation")

        # 4. Check SQLite credentials.db recorded user and agent tokens
        creds_db = data_dir / "credentials.db"
        self.assertTrue(creds_db.is_file(), f"credentials.db not found at {creds_db}")
        conn = sqlite3.connect(creds_db)
        cursor = conn.cursor()
        cursor.execute("SELECT username, role, token_type FROM tokens")
        tokens = cursor.fetchall()
        cursor.execute("SELECT username, role FROM users")
        users = cursor.fetchall()
        conn.close()

        token_map = {t[0]: (t[1], t[2]) for t in tokens}
        self.assertIn("admin", token_map)
        self.assertIn("jason", token_map)
        self.assertIn("jason-agent", token_map)
        self.assertEqual(token_map["jason"][1], "user")
        self.assertEqual(token_map["jason-agent"][1], "agent")

        user_names = {u[0] for u in users}
        self.assertIn("jason", user_names)
        self.assertIn("jason-agent", user_names)

    def test_load_config_and_resolve_headers_autodiscovery(self):
        env = os.environ.copy()
        env["HOME"] = str(self.fake_home)
        env["XDG_CONFIG_HOME"] = str(self.fake_home / ".config")

        # Initialize identity
        cmd = [
            sys.executable, str(AB_CTL_PY), "init",
            "--user", "jason",
            "--generate-agent",
            "--data-dir", str(self.fake_home / "data")
        ]
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, f"init failed: {proc.stderr}")

        # In-process test with patched environment
        orig_home = os.environ.get("HOME")
        orig_xdg = os.environ.get("XDG_CONFIG_HOME")
        try:
            os.environ["HOME"] = str(self.fake_home)
            os.environ["XDG_CONFIG_HOME"] = str(self.fake_home / ".config")
            os.environ.pop("AB_TOKEN", None)
            os.environ.pop("AB_USER", None)
            os.environ.pop("AB_AGENT", None)
            os.environ.pop("AB_ACTIVE_USER", None)
            os.environ.pop("AB_ACTIVE_AGENT", None)

            ab_ctl = load_ab_ctl_module()
            cfg = ab_ctl.load_config()

            self.assertEqual(cfg.get("user_id"), "user:jason")
            self.assertEqual(cfg.get("agent_id"), "agent:jason-agent")
            self.assertTrue(cfg.get("user_token", "").startswith("ab_usr_"))
            self.assertTrue(cfg.get("agent_token", "").startswith("ab_agt_"))
            self.assertEqual(cfg.get("token"), cfg.get("user_token"))
            self.assertEqual(cfg.get("surface_type"), "workstation")
            self.assertEqual(cfg.get("surface_id"), "surface:workstation")

            # Verify resolve_swarm_headers uses discovered user and agent IDs
            class DummyArgs:
                token = None
                token_file = None
                active_user = None
                user = None
                active_agent = None
                agent = None

            headers = ab_ctl.resolve_swarm_headers(DummyArgs(), cfg)
            self.assertEqual(headers.get("X-Active-User"), "user:jason")
            self.assertEqual(headers.get("X-Active-Agent"), "agent:jason-agent")
            self.assertEqual(headers.get("Authorization"), f"Bearer {cfg['user_token']}")
        finally:
            if orig_home is not None:
                os.environ["HOME"] = orig_home
            if orig_xdg is not None:
                os.environ["XDG_CONFIG_HOME"] = orig_xdg

    def test_custom_identity_file_flag(self):
        custom_identity = self.fake_home / "custom_dir" / "my_identity.json"
        cmd = [
            sys.executable, str(AB_CTL_PY), "init",
            "--user", "alice",
            "--generate-agent",
            "--identity-file", str(custom_identity)
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, f"init failed: {proc.stderr}")
        self.assertTrue(custom_identity.is_file())
        mode = custom_identity.stat().st_mode & 0o777
        self.assertEqual(mode, 0o600)

        with open(custom_identity, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.assertEqual(data["user"]["id"], "user:alice")
        self.assertEqual(data["agent"]["id"], "agent:alice-agent")

    def test_substrate_graph_provisioning_with_daemon(self):
        """Test that init provisions user node, agent node, and DELEGATES_TO edge when daemon responds."""
        received_requests = []

        class MockDaemonHandler(http.server.BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass

            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length).decode("utf-8")
                data = json.loads(body) if body else {}
                received_requests.append((self.path, data))

                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"status": "OK"}).encode("utf-8"))

        server = http.server.HTTPServer(("127.0.0.1", 0), MockDaemonHandler)
        port = server.server_address[1]
        server_thread = threading.Thread(target=server.serve_forever)
        server_thread.daemon = True
        server_thread.start()

        try:
            connect_url = f"http://127.0.0.1:{port}"
            custom_identity = self.fake_home / "daemon_identity.json"
            cmd = [
                sys.executable, str(AB_CTL_PY), "init",
                "--user", "bob",
                "--generate-agent",
                "--identity-file", str(custom_identity),
                "--connect", connect_url
            ]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, f"init failed: {proc.stderr}\nStdout: {proc.stdout}")

            # Verify that user node, agent node, and DELEGATES_TO edge were committed
            paths = [r[0] for r in received_requests]
            self.assertIn("/api/v1/graph/node", paths)
            self.assertIn("/api/v1/link", paths)

            nodes = [r[1] for r in received_requests if r[0] == "/api/v1/graph/node"]
            node_ids = {n.get("id") for n in nodes}
            self.assertIn("user:bob", node_ids)
            self.assertIn("agent:bob-agent", node_ids)

            links = [r[1] for r in received_requests if r[0] == "/api/v1/link"]
            delegation_links = [l for l in links if l.get("label") == "DELEGATES_TO"]
            self.assertTrue(len(delegation_links) >= 1)
            self.assertEqual(delegation_links[0]["source"], "user:bob")
            self.assertEqual(delegation_links[0]["target"], "agent:bob-agent")
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    unittest.main()
