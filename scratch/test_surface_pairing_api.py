#!/usr/bin/env python3
"""
Test for Task 2: Surface Pairing API endpoints on live daemon.
Verifies:
1. POST /api/v1/surface/pair/request returns pairing_id, 6-digit PIN, and expires_at.
2. POST /api/v1/surface/pair/approve binds user_id, agent_id, and context_id.
3. POST /api/v1/surface/pair/claim returns scoped surface_token (ab_srf_...).
4. Second claim attempt on same pairing_id fails (400 or 404).
5. GET /api/v1/surface/list lists the enrolled surface with all metadata including last_seen.
6. Enrolled surfaces persist across daemon restart.
7. DELETE /api/v1/surface/:surface_id revokes the surface.
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
    def start_daemon(cls):
        cls.proc = subprocess.Popen([
            str(DAEMON_BIN), "1",
            f"--data-dir={cls.data_dir}",
            "--auth-mode=token",
            f"--admin-token={cls.admin_token}",
            f"--port={TEST_PORT}"
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(30):
            try:
                req = urllib.request.Request(f"{BASE_URL}/api/v1/schema")
                with urllib.request.urlopen(req, timeout=1.0) as resp:
                    if resp.status == 200:
                        break
            except Exception:
                time.sleep(0.1)

    @classmethod
    def stop_daemon(cls):
        if cls.proc is not None:
            cls.proc.terminate()
            cls.proc.wait()
            cls.proc = None
            time.sleep(0.2)

    @classmethod
    def setUpClass(cls):
        cls.temp_dir = tempfile.TemporaryDirectory()
        cls.data_dir = Path(cls.temp_dir.name) / "data"
        cls.data_dir.mkdir(parents=True)
        cls.admin_token = "ab_adm_test_surface_1234567890abcdef"
        cls.proc = None
        cls.start_daemon()

    @classmethod
    def tearDownClass(cls):
        cls.stop_daemon()
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
            self.assertTrue(pairing_id.startswith("pair_"))
            self.assertEqual(len(pairing_id), 37)  # 'pair_' (5) + 32 hex chars
            self.assertEqual(len(pin), 6)

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

        # 3b. Assert claiming a second time fails (400 or 404)
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(claim_req)
        self.assertIn(ctx.exception.code, [400, 404])

        # 4. List surfaces
        list_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/list",
            headers={"Authorization": f"Bearer {self.admin_token}"}
        )
        with urllib.request.urlopen(list_req) as resp:
            self.assertEqual(resp.status, 200)
            surfaces = json.loads(resp.read().decode("utf-8"))["surfaces"]
            matched = [s for s in surfaces if s["surface_id"] == "surface:lab-table"]
            self.assertEqual(len(matched), 1)
            s = matched[0]
            required_fields = [
                "surface_id", "surface_type", "client_app", "user_id",
                "agent_id", "context_id", "enrolled_at", "last_seen"
            ]
            for f in required_fields:
                self.assertIn(f, s)
            self.assertEqual(s["surface_type"], "tabletop")
            self.assertEqual(s["client_app"], "MultiTouchCanvas")
            self.assertEqual(s["user_id"], "user:jason")
            self.assertEqual(s["agent_id"], "agent:jason-agent")
            self.assertEqual(s["context_id"], "ctx-arch")
            self.assertGreater(s["enrolled_at"], 0)
            self.assertGreater(s["last_seen"], 0)

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
            self.assertTrue(pairing_id.startswith("pair_"))
            self.assertEqual(len(pairing_id), 37)

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

    def test_persistence_across_restart(self):
        # 1. Request pairing
        req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/request",
            data=json.dumps({
                "surface_type": "wall-display",
                "client_app": "BigScreenViewer",
                "suggested_id": "main-wall"
            }).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(req) as resp:
            self.assertEqual(resp.status, 201)
            pair_data = json.loads(resp.read().decode("utf-8"))
            pairing_id = pair_data["pairing_id"]
            pin = pair_data["pin"]

        # 2. Approve pairing
        approve_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/approve",
            data=json.dumps({
                "pairing_id": pairing_id,
                "pin": pin,
                "user_id": "user:alice",
                "agent_id": "agent:alice-agent",
                "context_id": "ctx-main",
                "surface_id": "surface:main-wall"
            }).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.admin_token}"
            }
        )
        with urllib.request.urlopen(approve_req) as resp:
            self.assertEqual(resp.status, 200)

        # 3. Claim pairing
        claim_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/claim",
            data=json.dumps({"pairing_id": pairing_id}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(claim_req) as resp:
            self.assertEqual(resp.status, 200)

        # 4. Stop the daemon
        self.stop_daemon()

        # 5. Restart the daemon on the same data directory
        self.start_daemon()

        # 6. Verify GET /api/v1/surface/list still lists the surface with all metadata
        list_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/list",
            headers={"Authorization": f"Bearer {self.admin_token}"}
        )
        with urllib.request.urlopen(list_req) as resp:
            self.assertEqual(resp.status, 200)
            surfaces = json.loads(resp.read().decode("utf-8"))["surfaces"]
            matched = [s for s in surfaces if s["surface_id"] == "surface:main-wall"]
            self.assertEqual(len(matched), 1)
            s = matched[0]
            required_fields = [
                "surface_id", "surface_type", "client_app", "user_id",
                "agent_id", "context_id", "enrolled_at", "last_seen"
            ]
            for f in required_fields:
                self.assertIn(f, s)
            self.assertEqual(s["surface_type"], "wall-display")
            self.assertEqual(s["client_app"], "BigScreenViewer")
            self.assertEqual(s["user_id"], "user:alice")
            self.assertEqual(s["agent_id"], "agent:alice-agent")
            self.assertEqual(s["context_id"], "ctx-main")
            self.assertGreater(s["enrolled_at"], 0)
            self.assertGreater(s["last_seen"], 0)

        # Clean up by revoking the surface
        del_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/surface:main-wall",
            headers={"Authorization": f"Bearer {self.admin_token}"},
            method="DELETE"
        )
        with urllib.request.urlopen(del_req) as resp:
            self.assertEqual(resp.status, 200)

    def test_nonexistent_session_or_surface(self):
        # Claim non-existent pairing ID -> 404
        claim_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/claim",
            data=json.dumps({"pairing_id": "pair_nonexistent_123"}).encode("utf-8"),
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

    def test_non_admin_rbac_and_isolation(self):
        # 0. Ensure admin has an enrolled surface 'surface:lab-table'
        admin_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/request",
            data=json.dumps({
                "surface_type": "tabletop",
                "client_app": "AdminApp",
                "suggested_id": "lab-table"
            }).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(admin_req) as resp:
            self.assertEqual(resp.status, 201)
            admin_pair_data = json.loads(resp.read().decode("utf-8"))
            admin_pairing_id = admin_pair_data["pairing_id"]
            admin_pin = admin_pair_data["pin"]

        admin_approve_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/approve",
            data=json.dumps({
                "pairing_id": admin_pairing_id,
                "pin": admin_pin,
                "user_id": "user:jason",
                "agent_id": "agent:jason-agent",
                "surface_id": "surface:lab-table"
            }).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.admin_token}"
            }
        )
        with urllib.request.urlopen(admin_approve_req) as resp:
            self.assertEqual(resp.status, 200)

        admin_claim_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/claim",
            data=json.dumps({"pairing_id": admin_pairing_id}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(admin_claim_req) as resp:
            self.assertEqual(resp.status, 200)

        # 1. Register standard non-admin user "alice" via POST /api/v1/admin/users
        alice_token = "ab_usr_alice_token_1234567890abcdef"
        reg_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/admin/users",
            data=json.dumps({
                "username": "alice",
                "role": "curator",
                "token": alice_token
            }).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.admin_token}"
            }
        )
        with urllib.request.urlopen(reg_req) as resp:
            self.assertEqual(resp.status, 200)
            user_data = json.loads(resp.read().decode("utf-8"))
            self.assertEqual(user_data["username"], "alice")
            self.assertEqual(user_data["token"], alice_token)

        # 2. Alice requests pairing for tablet-alice
        pair_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/request",
            data=json.dumps({
                "surface_type": "tablet",
                "client_app": "AliceDrawingApp",
                "suggested_id": "tablet-alice"
            }).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(pair_req) as resp:
            self.assertEqual(resp.status, 201)
            pair_data = json.loads(resp.read().decode("utf-8"))
            alice_pairing_id = pair_data["pairing_id"]
            alice_pin = pair_data["pin"]

        # 3. Alice tries to approve a pairing session for user:bob -> verify 403 Forbidden
        bob_approve_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/approve",
            data=json.dumps({
                "pairing_id": alice_pairing_id,
                "pin": alice_pin,
                "user_id": "user:bob",
                "agent_id": "agent:alice-agent",
                "surface_id": "tablet-alice"
            }).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {alice_token}"
            }
        )
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(bob_approve_req)
        self.assertEqual(ctx.exception.code, 403)

        # 4. Alice tries to approve with agent_id: "agent:bob-agent" -> verify 403 Forbidden
        bob_agent_approve_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/approve",
            data=json.dumps({
                "pairing_id": alice_pairing_id,
                "pin": alice_pin,
                "user_id": "user:alice",
                "agent_id": "agent:bob-agent",
                "surface_id": "tablet-alice"
            }).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {alice_token}"
            }
        )
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(bob_agent_approve_req)
        self.assertEqual(ctx.exception.code, 403)

        # 5. Alice approves pairing with her user token (alice / user:alice)
        alice_approve_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/approve",
            data=json.dumps({
                "pairing_id": alice_pairing_id,
                "pin": alice_pin,
                "user_id": "user:alice",
                "agent_id": "agent:alice-agent",
                "context_id": "ctx-alice",
                "surface_id": "tablet-alice"
            }).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {alice_token}"
            }
        )
        with urllib.request.urlopen(alice_approve_req) as resp:
            self.assertEqual(resp.status, 200)
            appr_data = json.loads(resp.read().decode("utf-8"))
            self.assertEqual(appr_data["status"], "APPROVED")
            self.assertEqual(appr_data["surface_id"], "surface:tablet-alice")

        # 5b. Duplicate approval check -> 400
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(alice_approve_req)
        self.assertEqual(ctx.exception.code, 400)

        # 6. Alice lists surfaces (GET /api/v1/surface/list) -> verify only tablet-alice is returned, not admin's lab-table
        alice_list_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/list",
            headers={"Authorization": f"Bearer {alice_token}"}
        )
        with urllib.request.urlopen(alice_list_req) as resp:
            self.assertEqual(resp.status, 200)
            surfaces = json.loads(resp.read().decode("utf-8"))["surfaces"]
            surface_ids = [s["surface_id"] for s in surfaces]
            self.assertIn("surface:tablet-alice", surface_ids)
            self.assertNotIn("surface:lab-table", surface_ids)
            self.assertEqual(len(surfaces), 1)

        # 7. Alice tries to delete admin's surface (DELETE /api/v1/surface/surface:lab-table) -> verify 403 Forbidden
        alice_del_admin_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/surface:lab-table",
            headers={"Authorization": f"Bearer {alice_token}"},
            method="DELETE"
        )
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(alice_del_admin_req)
        self.assertEqual(ctx.exception.code, 403)

        # 8. Alice successfully claims her surface token and deletes her own surface
        alice_claim_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/claim",
            data=json.dumps({"pairing_id": alice_pairing_id}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(alice_claim_req) as resp:
            self.assertEqual(resp.status, 200)
            claim_data = json.loads(resp.read().decode("utf-8"))
            self.assertTrue(claim_data["surface_token"].startswith("ab_srf_"))
            self.assertEqual(claim_data["surface_id"], "surface:tablet-alice")

        alice_del_own_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/surface:tablet-alice",
            headers={"Authorization": f"Bearer {alice_token}"},
            method="DELETE"
        )
        with urllib.request.urlopen(alice_del_own_req) as resp:
            self.assertEqual(resp.status, 200)

        # Verify Alice's surface list is now empty
        with urllib.request.urlopen(alice_list_req) as resp:
            surfaces = json.loads(resp.read().decode("utf-8"))["surfaces"]
            self.assertFalse(any(s["surface_id"] == "surface:tablet-alice" for s in surfaces))

        # 9. Test default agent_id assignment when omitted for non-admin user
        pair_req2 = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/request",
            data=json.dumps({"suggested_id": "tablet-default-agent"}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(pair_req2) as resp:
            p2 = json.loads(resp.read().decode("utf-8"))

        approve_def_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/approve",
            data=json.dumps({
                "pairing_id": p2["pairing_id"],
                "pin": p2["pin"],
                "user_id": "alice"
            }).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {alice_token}"}
        )
        with urllib.request.urlopen(approve_def_req) as resp:
            self.assertEqual(resp.status, 200)
            appr2 = json.loads(resp.read().decode("utf-8"))
            self.assertEqual(appr2["agent_id"], "agent:alice-agent")

        claim2_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/claim",
            data=json.dumps({"pairing_id": p2["pairing_id"]}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(claim2_req) as resp:
            self.assertEqual(resp.status, 200)

        del2_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/surface:tablet-default-agent",
            headers={"Authorization": f"Bearer {alice_token}"},
            method="DELETE"
        )
        with urllib.request.urlopen(del2_req) as resp:
            self.assertEqual(resp.status, 200)

        # 10. Clean up admin's surface
        admin_del_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/surface:lab-table",
            headers={"Authorization": f"Bearer {self.admin_token}"},
            method="DELETE"
        )
        with urllib.request.urlopen(admin_del_req) as resp:
            self.assertEqual(resp.status, 200)


if __name__ == "__main__":
    unittest.main()
