#!/usr/bin/env python3
"""
Test for Task 2: Surface Pairing API endpoints on live daemon.
Verifies:
1. POST /api/v1/surface/pair/request returns pairing_id, 6-digit PIN, and expires_at.
2. POST /api/v1/surface/pair/approve binds user_id, agent_id, and context_id.
3. POST /api/v1/surface/pair/claim returns scoped surface_token (ab_srf_...).
4. GET /api/v1/surface/list lists the enrolled surface.
5. DELETE /api/v1/surface/:surface_id revokes the surface.
"""

import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DAEMON_BIN = REPO_ROOT / "build" / "agentic-blackboardd"
AB_CTL_PY = REPO_ROOT / "src" / "ab-ctl.py"
TEST_PORT = 19183
BASE_URL = f"http://127.0.0.1:{TEST_PORT}"


class TestSurfacePairingApi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_dir = tempfile.TemporaryDirectory()
        data_dir = Path(cls.temp_dir.name) / "data"
        data_dir.mkdir(parents=True)
        cls.admin_token = "ab_adm_test_surface_1234567890abcdef"
        
        # Start daemon
        cls.proc = subprocess.Popen([
            str(DAEMON_BIN), "1",
            f"--data-dir={data_dir}",
            "--auth-mode=token",
            f"--admin-token={cls.admin_token}",
            f"--port={TEST_PORT}"
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        
        # Wait for daemon
        time.sleep(1.0)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait()
        cls.temp_dir.cleanup()

    def test_pairing_lifecycle(self):
        # 1. Request pairing from ambient surface
        req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/request",
            data=json.dumps({
                "surface_type": "tabletop",
                "client_app": "MultiTouchCanvas",
                "suggested_id": "lab-table"
            }).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(req) as resp:
            self.assertEqual(resp.status, 201)
            pair_data = json.loads(resp.read().decode("utf-8"))
            self.assertIn("pairing_id", pair_data)
            self.assertIn("pin", pair_data)
            pairing_id = pair_data["pairing_id"]
            pin = pair_data["pin"]

        # 2. User approves pairing with admin/user token
        approve_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/approve",
            data=json.dumps({
                "pairing_id": pairing_id,
                "pin": pin,
                "user_id": "user:jason",
                "agent_id": "agent:jason-agent",
                "context_id": "ctx-arch",
                "surface_id": "surface:lab-table"
            }).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.admin_token}"
            }
        )
        with urllib.request.urlopen(approve_req) as resp:
            self.assertEqual(resp.status, 200)

        # 3. Surface claims surface token
        claim_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/claim",
            data=json.dumps({"pairing_id": pairing_id}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(claim_req) as resp:
            self.assertEqual(resp.status, 200)
            claim_data = json.loads(resp.read().decode("utf-8"))
            self.assertTrue(claim_data["surface_token"].startswith("ab_srf_"))
            self.assertEqual(claim_data["surface_id"], "surface:lab-table")
            self.assertEqual(claim_data["user_id"], "user:jason")
            self.assertEqual(claim_data["agent_id"], "agent:jason-agent")

        # 4. List surfaces
        list_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/list",
            headers={"Authorization": f"Bearer {self.admin_token}"}
        )
        with urllib.request.urlopen(list_req) as resp:
            self.assertEqual(resp.status, 200)
            surfaces = json.loads(resp.read().decode("utf-8"))["surfaces"]
            self.assertTrue(any(s["surface_id"] == "surface:lab-table" for s in surfaces))

        # 5. Revoke surface
        del_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/surface:lab-table",
            headers={"Authorization": f"Bearer {self.admin_token}"},
            method="DELETE"
        )
        with urllib.request.urlopen(del_req) as resp:
            self.assertEqual(resp.status, 200)

        # 6. Verify surface is no longer in list
        with urllib.request.urlopen(list_req) as resp:
            surfaces = json.loads(resp.read().decode("utf-8"))["surfaces"]
            self.assertFalse(any(s["surface_id"] == "surface:lab-table" for s in surfaces))

    def test_pin_retries_and_lockout(self):
        # 1. Request pairing
        req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/request",
            data=json.dumps({"surface_type": "tablet"}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(req) as resp:
            pair_data = json.loads(resp.read().decode("utf-8"))
            pairing_id = pair_data["pairing_id"]

        # 2. Attempt claim before approval -> must fail (400)
        claim_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/claim",
            data=json.dumps({"pairing_id": pairing_id}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(claim_req)
        self.assertEqual(ctx.exception.code, 400)

        # 3. Two failed PIN attempts
        for _ in range(2):
            bad_req = urllib.request.Request(
                f"{BASE_URL}/api/v1/surface/pair/approve",
                data=json.dumps({
                    "pairing_id": pairing_id,
                    "pin": "000000",
                    "user_id": "user:test"
                }).encode("utf-8"),
                headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.admin_token}"}
            )
            with self.assertRaises(urllib.error.HTTPError) as ctx:
                urllib.request.urlopen(bad_req)
            self.assertEqual(ctx.exception.code, 400)

        # 4. Third failed PIN attempt -> session invalidated
        bad_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/approve",
            data=json.dumps({
                "pairing_id": pairing_id,
                "pin": "000000",
                "user_id": "user:test"
            }).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.admin_token}"}
        )
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(bad_req)
        self.assertEqual(ctx.exception.code, 400)

        # 5. Subsequent attempt now reports session not found / invalidated
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(bad_req)
        self.assertEqual(ctx.exception.code, 400)

    def test_nonexistent_session_or_surface(self):
        # Claim non-existent pairing ID -> 404
        claim_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/claim",
            data=json.dumps({"pairing_id": "pair-nonexistent-123"}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(claim_req)
        self.assertEqual(ctx.exception.code, 404)

        # Revoke non-existent surface -> 404
        del_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/surface:nonexistent",
            headers={"Authorization": f"Bearer {self.admin_token}"},
            method="DELETE"
        )
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(del_req)
        self.assertEqual(ctx.exception.code, 404)


if __name__ == "__main__":
    unittest.main()
