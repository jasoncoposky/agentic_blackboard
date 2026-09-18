#!/usr/bin/env python3
"""
Test for Task 3: Surface Authentication & Origin Provenance Enforcement.
Verifies:
1. Legitimate atom commit with matching surface origin succeeds (200/201) via POST /api/v1/graph/bundle.
2. Auto-population of omitted origin fields (user_id, agent_id, surface_id, surface_type, context_id).
3. User spoofing prevention: origin.user_id = "user:alice" with phone-jason surface token yields 403.
4. Surface spoofing prevention: origin.surface_id = "surface:another-surface" yields 403.
5. Agent spoofing prevention: origin.agent_id = "agent:other-agent" yields 403.
6. Revoked surface token immediately yields HTTP 401 Unauthorized on subsequent requests.
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
TEST_PORT = 19184
BASE_URL = f"http://127.0.0.1:{TEST_PORT}"


class TestSurfaceAuthEnforcement(unittest.TestCase):
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
        cls.admin_token = "ab_adm_surface_auth_test_1234567890abcdef"
        cls.proc = None
        cls.start_daemon()

        # Pair surface phone-jason bound to user:jason, agent:jason-agent, context:ctx-notes
        req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/request",
            data=json.dumps({"surface_type": "mobile_browser", "suggested_id": "phone-jason"}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(req) as resp:
            pair = json.loads(resp.read().decode("utf-8"))
            cls.pairing_id = pair["pairing_id"]
            cls.pin = pair["pin"]

        approve_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/approve",
            data=json.dumps({
                "pairing_id": cls.pairing_id,
                "pin": cls.pin,
                "user_id": "user:jason",
                "agent_id": "agent:jason-agent",
                "context_id": "ctx-notes",
                "surface_id": "surface:phone-jason"
            }).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {cls.admin_token}"
            }
        )
        with urllib.request.urlopen(approve_req) as resp:
            assert resp.status == 200

        claim_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/claim",
            data=json.dumps({"pairing_id": cls.pairing_id}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(claim_req) as resp:
            claim_data = json.loads(resp.read().decode("utf-8"))
            cls.surface_token = claim_data["surface_token"]
            cls.surface_id = claim_data["surface_id"]

        # Register non-admin user alice
        cls.alice_token = "ab_usr_alice_token_1234567890abcdef"
        reg_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/admin/users",
            data=json.dumps({
                "username": "alice",
                "role": "user",
                "token": cls.alice_token
            }).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {cls.admin_token}"
            }
        )
        with urllib.request.urlopen(reg_req) as resp:
            assert resp.status == 200

    @classmethod
    def tearDownClass(cls):
        cls.stop_daemon()
        cls.temp_dir.cleanup()

    def test_1_legitimate_atom_commit(self):
        """Test 1: Commit atom with matching origin succeeds (200/201)."""
        atom_payload = {
            "id": "atom-surface-note-1",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-surface-note-1",
                "origin": {
                    "user_id": "user:jason",
                    "agent_id": "agent:jason-agent",
                    "surface_id": "surface:phone-jason",
                    "surface_type": "mobile_browser",
                    "context_id": "ctx-notes",
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Captured on phone"}
        }
        post_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atom": atom_payload}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.surface_token}"}
        )
        with urllib.request.urlopen(post_req) as resp:
            self.assertIn(resp.status, (200, 201))

        # Verify atom is stored and retrievable
        get_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/node/atom-surface-note-1",
            headers={"Authorization": f"Bearer {self.surface_token}"}
        )
        with urllib.request.urlopen(get_req) as resp:
            self.assertEqual(resp.status, 200)
            data = json.loads(resp.read().decode("utf-8"))
            self.assertEqual(data.get("uuid"), "atom-surface-note-1")
            self.assertEqual(data.get("statement"), "Captured on phone")

    def test_2_auto_populate_omitted_origin_fields(self):
        """Test 2: Commit atom with empty origin fields auto-populates from surface token metadata."""
        atom_payload = {
            "id": "atom-surface-auto-1",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-surface-auto-1",
                "origin": {
                    "user_id": "",
                    "agent_id": "",
                    "surface_id": "",
                    "surface_type": "",
                    "context_id": "",
                    "project_id": ""
                }
            },
            "payload": {"statement": "Omitted origin note"}
        }
        post_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atoms": [atom_payload]}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.surface_token}"}
        )
        with urllib.request.urlopen(post_req) as resp:
            self.assertIn(resp.status, (200, 201))

        # Verify auto-populated origin fields via GET /api/v1/node/:id
        get_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/node/atom-surface-auto-1",
            headers={"Authorization": f"Bearer {self.surface_token}"}
        )
        with urllib.request.urlopen(get_req) as resp:
            self.assertEqual(resp.status, 200)
            data = json.loads(resp.read().decode("utf-8"))
            origin = data.get("origin", {})
            user_id = data.get("user_id") or origin.get("user_id")
            agent_id = data.get("agent_id") or origin.get("agent_id")
            surface_id = data.get("surface_id") or origin.get("surface_id")
            surface_type = data.get("surface_type") or origin.get("surface_type")
            context_id = data.get("context_id") or origin.get("context_id")

            self.assertIn(user_id, ["user:jason", "jason"])
            self.assertIn(agent_id, ["agent:jason-agent", "jason-agent"])
            self.assertIn(surface_id, ["surface:phone-jason", "phone-jason"])
            self.assertEqual(surface_type, "mobile_browser")
            self.assertEqual(context_id, "ctx-notes")

    def test_3_user_spoofing_prevention(self):
        """Test 3: User spoofing (origin.user_id = 'user:alice') returns HTTP 403."""
        atom_payload = {
            "id": "atom-spoof-user",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-spoof-user",
                "origin": {
                    "user_id": "user:alice",
                    "agent_id": "agent:jason-agent",
                    "surface_id": "surface:phone-jason",
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Spoofed user note"}
        }
        bad_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atom": atom_payload}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.surface_token}"}
        )
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(bad_req)
        self.assertEqual(cm.exception.code, 403)

    def test_4_surface_spoofing_prevention(self):
        """Test 4: Surface spoofing (origin.surface_id = 'surface:another-surface') returns HTTP 403."""
        atom_payload = {
            "id": "atom-spoof-surface",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-spoof-surface",
                "origin": {
                    "user_id": "user:jason",
                    "agent_id": "agent:jason-agent",
                    "surface_id": "surface:another-surface",
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Spoofed surface note"}
        }
        bad_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atom": atom_payload}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.surface_token}"}
        )
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(bad_req)
        self.assertEqual(cm.exception.code, 403)

    def test_5_agent_spoofing_prevention(self):
        """Test 5: Agent spoofing (origin.agent_id = 'agent:other-agent') returns HTTP 403."""
        atom_payload = {
            "id": "atom-spoof-agent",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-spoof-agent",
                "origin": {
                    "user_id": "user:jason",
                    "agent_id": "agent:other-agent",
                    "surface_id": "surface:phone-jason",
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Spoofed agent note"}
        }
        bad_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atom": atom_payload}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.surface_token}"}
        )
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(bad_req)
        self.assertEqual(cm.exception.code, 403)

    def test_6_revoked_surface_token_immediately_yields_401(self):
        """Test 6: Revoking surface makes previous surface_token return HTTP 401."""
        # Pair an ephemeral surface specifically for the revocation test
        req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/request",
            data=json.dumps({"surface_type": "mobile_browser", "suggested_id": "phone-ephemeral-revoke"}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(req) as resp:
            pair = json.loads(resp.read().decode("utf-8"))
            eph_pairing_id = pair["pairing_id"]
            eph_pin = pair["pin"]

        approve_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/approve",
            data=json.dumps({
                "pairing_id": eph_pairing_id,
                "pin": eph_pin,
                "user_id": "user:jason",
                "agent_id": "agent:jason-agent",
                "context_id": "ctx-notes",
                "surface_id": "surface:phone-ephemeral-revoke"
            }).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.admin_token}"
            }
        )
        with urllib.request.urlopen(approve_req) as resp:
            self.assertEqual(resp.status, 200)

        claim_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/pair/claim",
            data=json.dumps({"pairing_id": eph_pairing_id}).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(claim_req) as resp:
            claim_data = json.loads(resp.read().decode("utf-8"))
            eph_surface_token = claim_data["surface_token"]
            eph_surface_id = claim_data["surface_id"]

        del_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/surface/{eph_surface_id}",
            method="DELETE",
            headers={"Authorization": f"Bearer {self.admin_token}"}
        )
        with urllib.request.urlopen(del_req) as resp:
            self.assertEqual(resp.status, 200)

        # Attempt to commit with revoked surface token
        atom_payload = {
            "id": "atom-revoked-test",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-revoked-test",
                "origin": {
                    "user_id": "user:jason",
                    "agent_id": "agent:jason-agent",
                    "surface_id": eph_surface_id,
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Should be rejected with 401"}
        }
        post_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atom": atom_payload}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {eph_surface_token}"}
        )
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(post_req)
        self.assertEqual(cm.exception.code, 401)

    def test_user_token_spoof_prevention(self):
        """Test non-admin user token spoof prevention across user_id, agent_id, and surface_id."""
        # 1. Attempt commit with origin.agent_id = "agent:bob-agent" -> assert HTTP 403
        bad_agent_atom = {
            "id": "atom-user-spoof-agent",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-user-spoof-agent",
                "origin": {
                    "user_id": "user:alice",
                    "agent_id": "agent:bob-agent",
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Spoofed agent by user"}
        }
        bad_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atom": bad_agent_atom}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.alice_token}"}
        )
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(bad_req)
        self.assertEqual(cm.exception.code, 403)

        # 2. Attempt commit with origin.surface_id belonging to jason -> assert HTTP 403
        bad_surface_atom = {
            "id": "atom-user-spoof-surface",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-user-spoof-surface",
                "origin": {
                    "user_id": "user:alice",
                    "agent_id": "agent:alice-agent",
                    "surface_id": "surface:phone-jason",
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Spoofed surface by user"}
        }
        bad_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atom": bad_surface_atom}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.alice_token}"}
        )
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(bad_req)
        self.assertEqual(cm.exception.code, 403)

        # 3. Attempt commit with origin.user_id = "user:bob" -> assert HTTP 403
        bad_user_atom = {
            "id": "atom-user-spoof-user",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-user-spoof-user",
                "origin": {
                    "user_id": "user:bob",
                    "agent_id": "agent:alice-agent",
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Spoofed user by user"}
        }
        bad_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atom": bad_user_atom}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.alice_token}"}
        )
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(bad_req)
        self.assertEqual(cm.exception.code, 403)

        # 4. Commit with matching origin -> assert HTTP 200/201
        legit_atom = {
            "id": "atom-user-legit-matching",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-user-legit-matching",
                "origin": {
                    "user_id": "user:alice",
                    "agent_id": "agent:alice-agent",
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Legitimate atom matching origin"}
        }
        good_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atom": legit_atom}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.alice_token}"}
        )
        with urllib.request.urlopen(good_req) as resp:
            self.assertIn(resp.status, (200, 201))

        # 5. Commit with empty origin -> assert HTTP 200/201 (auto-populated)
        empty_origin_atom = {
            "id": "atom-user-legit-empty",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-user-legit-empty",
                "origin": {
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Legitimate atom empty origin"}
        }
        good_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atom": empty_origin_atom}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.alice_token}"}
        )
        with urllib.request.urlopen(good_req) as resp:
            self.assertIn(resp.status, (200, 201))

    def test_node_creation_spoof_prevention(self):
        """Test non-admin user cannot create IDENTITY node for another user."""
        # Attempt to create bob's IDENTITY node using alice's token -> HTTP 403
        bad_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/node",
            data=json.dumps({
                "type": "IDENTITY",
                "id": "n:IDENTITY:user:bob",
                "metadata": {"name": "Bob", "role": "USER"}
            }).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.alice_token}"}
        )
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(bad_req)
        self.assertEqual(cm.exception.code, 403)

        # Attempt to create alice's IDENTITY node using alice's token -> HTTP 201
        good_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/node",
            data=json.dumps({
                "type": "IDENTITY",
                "id": "n:IDENTITY:user:alice",
                "metadata": {"name": "Alice", "role": "USER"}
            }).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.alice_token}"}
        )
        with urllib.request.urlopen(good_req) as resp:
            self.assertEqual(resp.status, 201)

    def test_bundle_atomicity_and_defensive_origin(self):
        """Test two-pass bundle processing: failure in any atom rejects entire bundle without committing."""
        atom_valid = {
            "id": "atom-bundle-atomic-valid",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-bundle-atomic-valid",
                "origin": {
                    "user_id": "user:alice",
                    "agent_id": "agent:alice-agent",
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Atomic valid atom"}
        }
        atom_spoofed = {
            "id": "atom-bundle-atomic-spoofed",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-bundle-atomic-spoofed",
                "origin": {
                    "user_id": "user:bob",  # Spoofed user
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Atomic spoofed atom"}
        }

        # Submit bundle containing 1 valid and 1 invalid atom -> must fail with 403
        bundle_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atoms": [atom_valid, atom_spoofed]}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.alice_token}"}
        )
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(bundle_req)
        self.assertEqual(cm.exception.code, 403)

        # Verify atom_valid was NOT committed
        get_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/node/atom-bundle-atomic-valid",
            headers={"Authorization": f"Bearer {self.alice_token}"}
        )
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(get_req)
        self.assertEqual(cm.exception.code, 404)

        # Test defensive get_origin_field: null / integer origin values do not throw type_error
        atom_defensive = {
            "id": "atom-bundle-defensive-origin",
            "type": "REQUIREMENT",
            "header": {
                "uuid": "atom-bundle-defensive-origin",
                "origin": {
                    "user_id": None,
                    "agent_id": 12345,
                    "surface_id": ["invalid"],
                    "project_id": "proj-default"
                }
            },
            "payload": {"statement": "Defensive origin types atom"}
        }
        defensive_req = urllib.request.Request(
            f"{BASE_URL}/api/v1/graph/bundle",
            data=json.dumps({"atom": atom_defensive}).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.alice_token}"}
        )
        with urllib.request.urlopen(defensive_req) as resp:
            self.assertIn(resp.status, (200, 201))


if __name__ == "__main__":
    unittest.main()
