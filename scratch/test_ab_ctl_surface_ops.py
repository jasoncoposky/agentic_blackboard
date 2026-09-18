#!/usr/bin/env python3
"""
Test for Task 4: ab-ctl surface CLI subcommands.
Verifies:
1. ab-ctl surface pair --type tabletop --name living-table produces PIN and QR info.
2. ab-ctl surface approve <pairing_id> --pin <pin> succeeds.
3. ab-ctl surface list displays enrolled surfaces in formatted table/JSON.
4. ab-ctl surface revoke <surface_id> revokes access.
"""

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DAEMON_BIN = REPO_ROOT / "build" / "agentic-blackboardd"
AB_CTL_PY = REPO_ROOT / "src" / "ab-ctl.py"
TEST_PORT = 19185
BASE_URL = f"http://127.0.0.1:{TEST_PORT}"


class TestAbCtlSurfaceOps(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_dir = tempfile.TemporaryDirectory()
        data_dir = Path(cls.temp_dir.name) / "data"
        data_dir.mkdir(parents=True)
        cls.admin_token = "ab_adm_surface_cli_test_12345"
        
        cls.proc = subprocess.Popen([
            str(DAEMON_BIN), "1",
            f"--data-dir={data_dir}",
            "--auth-mode=token",
            f"--admin-token={cls.admin_token}",
            f"--port={TEST_PORT}"
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        try:
            cls.proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            cls.proc.kill()
            cls.proc.wait()
        cls.temp_dir.cleanup()

    def run_cli(self, args):
        cmd = [sys.executable, str(AB_CTL_PY)] + args + [
            f"--connect={BASE_URL}",
            f"--token={self.admin_token}"
        ]
        return subprocess.run(cmd, capture_output=True, text=True)

    def test_cli_surface_workflow(self):
        # 1. pair request
        res = self.run_cli(["surface", "pair", "--type", "tabletop", "--name", "living-table"])
        self.assertEqual(res.returncode, 0, f"pair failed: {res.stderr}")
        self.assertIn("PIN:", res.stdout)
        self.assertIn("Pairing ID:", res.stdout)
        
        # Extract pairing_id and pin
        lines = res.stdout.splitlines()
        pairing_id = [l.split("Pairing ID:")[1].strip() for l in lines if "Pairing ID:" in l][0]
        pin = [l.split("PIN:")[1].strip() for l in lines if "PIN:" in l][0]

        # 2. approve
        res_app = self.run_cli(["surface", "approve", pairing_id, "--pin", pin, "--surface-id", "surface:living-table"])
        self.assertEqual(res_app.returncode, 0, f"approve failed: {res_app.stderr}")

        # 3. list JSON
        res_list = self.run_cli(["surface", "list", "--format", "json"])
        self.assertEqual(res_list.returncode, 0, f"list failed: {res_list.stderr}")
        data = json.loads(res_list.stdout)
        self.assertTrue(any(s["surface_id"] == "surface:living-table" for s in data["surfaces"]))

        # 4. list table (default format)
        res_table = self.run_cli(["surface", "list"])
        self.assertEqual(res_table.returncode, 0, f"table list failed: {res_table.stderr}")
        self.assertIn("surface:living-table", res_table.stdout)

        # 5. revoke
        res_rev = self.run_cli(["surface", "revoke", "surface:living-table"])
        self.assertEqual(res_rev.returncode, 0, f"revoke failed: {res_rev.stderr}")
        self.assertIn("revoked", res_rev.stdout.lower())

        # 6. list again to verify surface is gone
        res_list_after = self.run_cli(["surface", "list", "--format", "json"])
        data_after = json.loads(res_list_after.stdout)
        self.assertFalse(any(s["surface_id"] == "surface:living-table" for s in data_after["surfaces"]))

    def test_cli_approve_invalid_pin_fails(self):
        # Start a pairing session
        res = self.run_cli(["surface", "pair", "--type", "tablet", "--name", "bad-pin-test"])
        self.assertEqual(res.returncode, 0, f"pair request failed: {res.stderr}")
        lines = res.stdout.splitlines()
        pairing_id = [l.split("Pairing ID:")[1].strip() for l in lines if "Pairing ID:" in l][0]

        # Attempt to approve with invalid PIN "000000"
        res_bad = self.run_cli(["surface", "approve", pairing_id, "--pin", "000000", "--surface-id", "surface:bad-pin-test"])
        self.assertNotEqual(res_bad.returncode, 0)
        self.assertIn("Error approving surface pairing", res_bad.stderr)

    def test_cli_revoke_nonexistent_surface_fails(self):
        # Attempt to revoke nonexistent surface
        res = self.run_cli(["surface", "revoke", "surface:nonexistent-unknown-12345"])
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("Error revoking surface", res.stderr)


if __name__ == "__main__":
    unittest.main()
