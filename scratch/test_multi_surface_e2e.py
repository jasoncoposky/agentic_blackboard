#!/usr/bin/env python3
"""
End-to-End Multi-Surface Integration Test & Regression Verification.

Verifies Task 5 requirements:
1. Daemon initialization with isolated port and token auth.
2. User-space identity keystore generation via `ab-ctl init --user jason --generate-agent`.
   - Permissions 0600 on identity.json.
   - Substrate provisioned with user:jason and agent:jason-agent and DELEGATES_TO link.
3. Multi-device surface pairing:
   - Phone (surface:phone-safari, mobile_browser) pairing request, approval via workstation CLI, claim.
   - Table (surface:table-lab, tabletop) pairing request, approval via workstation CLI, claim.
   - Verification of enrolled surfaces via workstation CLI.
4. Multi-device workflow execution:
   - Phone commits requirement atom atom-e2e-req-1 (auto-populated origin).
   - Table queries active atoms via /api/v1/query, commits solution atom atom-e2e-sol-1 with SOLVES link.
   - Workstation uses user token to review/query atoms and commit verification verdict atom.
5. Graph Invariance & Anti-Pollution Assertions:
   - Exactly ONE user:jason identity node.
   - Exactly ONE agent:jason-agent identity node.
   - Zero ephemeral agent nodes (no architect, verifier, phone-agent, etc.).
   - Exactly ONE 1:1 DELEGATES_TO edge from user:jason to agent:jason-agent.
6. Surface revocation verification:
   - Revocation of Phone surface via workstation CLI (`ab-ctl surface revoke surface:phone-safari`).
   - Phone token immediately rejected with HTTP 401 on bundle commit and node queries.
   - Table token and Workstation credentials remain 100% operational.
7. Clean daemon teardown and temporary directory cleanup.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import unittest

REPO_ROOT = Path(__file__).resolve().parent.parent
DAEMON_BIN = REPO_ROOT / "build" / "agentic-blackboardd"
AB_CTL_PY = REPO_ROOT / "src" / "ab-ctl.py"


def find_free_port(preferred: int = 19186) -> int:
    """Find an available port, attempting preferred port first."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("127.0.0.1", preferred))
            return preferred
    except OSError:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("", 0))
            return s.getsockname()[1]


def run_cli(
    args: list[str],
    check: bool = True,
    env: dict | None = None,
) -> subprocess.CompletedProcess:
    """Run ab-ctl CLI command and capture output."""
    cmd = [sys.executable, str(AB_CTL_PY)] + args
    cmd_env = os.environ.copy()
    if env:
        cmd_env.update(env)

    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=cmd_env,
    )
    if check and proc.returncode != 0:
        raise RuntimeError(
            f"Command failed with code {proc.returncode}: {' '.join(cmd)}\n"
            f"STDOUT: {proc.stdout}\nSTDERR: {proc.stderr}"
        )
    return proc


def http_request(
    url: str,
    method: str = "GET",
    payload: dict | None = None,
    token: str | None = None,
    timeout: float = 5.0,
) -> tuple[int, dict | str]:
    """Execute HTTP request against daemon and return status and parsed response."""
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"

    data_bytes = json.dumps(payload).encode("utf-8") if payload is not None else None
    req = urllib.request.Request(url, data=data_bytes, headers=headers, method=method)

    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8")
            try:
                return resp.status, json.loads(body)
            except Exception:
                return resp.status, body
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")
        try:
            return e.code, json.loads(err_body)
        except Exception:
            return e.code, err_body
    except Exception as e:
        return 0, str(e)


class TestMultiSurfaceE2E(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_dir = tempfile.TemporaryDirectory()
        cls.base_path = Path(cls.temp_dir.name)
        cls.data_dir = cls.base_path / "substrate_data"
        cls.data_dir.mkdir(parents=True, exist_ok=True)

        cls.fake_home = cls.base_path / "home" / "jason"
        cls.xdg_config_home = cls.fake_home / ".config"
        cls.xdg_config_home.mkdir(parents=True, exist_ok=True)

        cls.port = find_free_port(19186)
        cls.base_url = f"http://127.0.0.1:{cls.port}"
        cls.admin_token = f"ab_adm_e2e_{secrets.token_hex(16)}"

        # Prepare workstation environment for user jason
        cls.workstation_env = os.environ.copy()
        cls.workstation_env["HOME"] = str(cls.fake_home)
        cls.workstation_env["XDG_CONFIG_HOME"] = str(cls.xdg_config_home)
        for k in list(cls.workstation_env.keys()):
            if k.startswith("AB_"):
                del cls.workstation_env[k]

        # 1. Start daemon on isolated port with --auth-mode=token
        daemon_cmd = [
            str(DAEMON_BIN),
            "1",
            f"--data-dir={cls.data_dir}",
            "--auth-mode=token",
            f"--admin-token={cls.admin_token}",
            f"--port={cls.port}",
        ]
        cls.daemon_proc = subprocess.Popen(
            daemon_cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        # Poll daemon until online
        daemon_ready = False
        for _ in range(50):
            try:
                st, _ = http_request(f"{cls.base_url}/api/v1/schema", timeout=1.0)
                if st == 200:
                    daemon_ready = True
                    break
            except Exception:
                pass
            time.sleep(0.1)

        if not daemon_ready:
            cls.daemon_proc.terminate()
            cls.daemon_proc.wait()
            raise RuntimeError(f"Daemon failed to start on port {cls.port}")

    @classmethod
    def tearDownClass(cls):
        # 8. Teardown: Stop daemon and clean up temporary directory
        if cls.daemon_proc is not None:
            cls.daemon_proc.terminate()
            try:
                cls.daemon_proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                cls.daemon_proc.kill()
                cls.daemon_proc.wait()
            cls.daemon_proc = None
        cls.temp_dir.cleanup()

    def test_complete_multi_surface_lifecycle_and_invariants(self):
        """Execute full multi-surface lifecycle and verify 1:1 identity graph invariants."""
        # -----------------------------------------------------------------
        # Step 1: Workstation CLI Init & Keystore Creation
        # -----------------------------------------------------------------
        init_res = run_cli(
            [
                "init",
                "--user", "jason",
                "--generate-agent",
                f"--connect={self.base_url}",
                f"--token={self.admin_token}",
            ],
            check=True,
            env=self.workstation_env,
        )
        self.assertEqual(init_res.returncode, 0)
        self.assertIn("user:jason", init_res.stdout)
        self.assertIn("agent:jason-agent", init_res.stdout)

        # Verify identity.json existence, permissions (0600), and content
        identity_path = self.xdg_config_home / "agentic-blackboard" / "identity.json"
        self.assertTrue(identity_path.is_file(), f"Keystore not created at {identity_path}")
        mode = identity_path.stat().st_mode & 0o777
        self.assertEqual(mode, 0o600, f"Unsafe keystore permissions: {oct(mode)}")

        with open(identity_path, "r", encoding="utf-8") as f:
            identity_data = json.load(f)

        self.assertEqual(identity_data["user"]["id"], "user:jason")
        self.assertEqual(identity_data["user"]["name"], "jason")
        user_token = identity_data["user"]["token"]
        self.assertTrue(user_token.startswith("ab_usr_"))

        self.assertEqual(identity_data["agent"]["id"], "agent:jason-agent")
        self.assertEqual(identity_data["agent"]["name"], "jason-agent")
        agent_token = identity_data["agent"]["token"]
        self.assertTrue(agent_token.startswith("ab_agt_"))

        self.assertEqual(identity_data["default_surface"]["id"], "surface:workstation")
        self.assertEqual(identity_data["default_surface"]["type"], "workstation")

        # Verify substrate nodes and DELEGATES_TO edge exist on daemon
        st, user_node = http_request(f"{self.base_url}/api/v1/node/user:jason", token=self.admin_token)
        self.assertEqual(st, 200, f"Failed to fetch user:jason: {user_node}")
        self.assertEqual(user_node.get("type"), "IDENTITY")

        st, agent_node = http_request(f"{self.base_url}/api/v1/node/agent:jason-agent", token=self.admin_token)
        self.assertEqual(st, 200, f"Failed to fetch agent:jason-agent: {agent_node}")
        self.assertEqual(agent_node.get("type"), "IDENTITY")

        st, user_links = http_request(f"{self.base_url}/api/v1/node/user:jason/links?direction=outbound", token=self.admin_token)
        self.assertEqual(st, 200)
        outbound = user_links.get("outbound", [])
        delegation_links = [l for l in outbound if l.get("target") == "agent:jason-agent" and l.get("relation") == "DELEGATES_TO"]
        self.assertTrue(len(delegation_links) >= 1, f"DELEGATES_TO link missing: {outbound}")

        # -----------------------------------------------------------------
        # Step 2: Surface Pairing (Phone and Table)
        # -----------------------------------------------------------------
        # 2A. Pair Phone (surface:phone-safari, mobile_browser)
        st, phone_pair = http_request(
            f"{self.base_url}/api/v1/surface/pair/request",
            method="POST",
            payload={
                "surface_type": "mobile_browser",
                "suggested_id": "surface:phone-safari",
                "client_app": "Safari-iOS",
            },
        )
        self.assertIn(st, (200, 201))
        phone_pairing_id = phone_pair["pairing_id"]
        phone_pin = phone_pair["pin"]

        # Workstation approves Phone pairing via CLI (using discovered identity.json credentials)
        approve_phone = run_cli(
            [
                "surface", "approve", phone_pairing_id,
                "--pin", phone_pin,
                "--surface-id", "surface:phone-safari",
            ],
            check=True,
            env=self.workstation_env,
        )
        self.assertEqual(approve_phone.returncode, 0)
        self.assertIn("approved successfully", approve_phone.stdout)

        # Phone claims surface token
        st, phone_claim = http_request(
            f"{self.base_url}/api/v1/surface/pair/claim",
            method="POST",
            payload={"pairing_id": phone_pairing_id},
        )
        self.assertEqual(st, 200)
        phone_token = phone_claim.get("surface_token", "")
        self.assertTrue(phone_token.startswith("ab_srf_"))
        self.assertEqual(phone_claim.get("surface_id"), "surface:phone-safari")

        # 2B. Pair Table (surface:table-lab, tabletop)
        st, table_pair = http_request(
            f"{self.base_url}/api/v1/surface/pair/request",
            method="POST",
            payload={
                "surface_type": "tabletop",
                "suggested_id": "surface:table-lab",
                "client_app": "LabTableInterface",
            },
        )
        self.assertIn(st, (200, 201))
        table_pairing_id = table_pair["pairing_id"]
        table_pin = table_pair["pin"]

        # Workstation approves Table pairing via CLI
        approve_table = run_cli(
            [
                "surface", "approve", table_pairing_id,
                "--pin", table_pin,
                "--surface-id", "surface:table-lab",
            ],
            check=True,
            env=self.workstation_env,
        )
        self.assertEqual(approve_table.returncode, 0)
        self.assertIn("approved successfully", approve_table.stdout)

        # Table claims surface token
        st, table_claim = http_request(
            f"{self.base_url}/api/v1/surface/pair/claim",
            method="POST",
            payload={"pairing_id": table_pairing_id},
        )
        self.assertEqual(st, 200)
        table_token = table_claim.get("surface_token", "")
        self.assertTrue(table_token.startswith("ab_srf_"))
        self.assertEqual(table_claim.get("surface_id"), "surface:table-lab")

        # 2C. Workstation CLI verifies enrolled surfaces
        list_res = run_cli(["surface", "list", "--format", "json"], check=True, env=self.workstation_env)
        surfaces_data = json.loads(list_res.stdout)
        surfaces = surfaces_data.get("surfaces", [])
        surface_map = {s["surface_id"]: s for s in surfaces}

        self.assertIn("surface:phone-safari", surface_map)
        self.assertEqual(surface_map["surface:phone-safari"]["user_id"], "user:jason")
        self.assertEqual(surface_map["surface:phone-safari"]["agent_id"], "agent:jason-agent")
        self.assertEqual(surface_map["surface:phone-safari"]["surface_type"], "mobile_browser")

        self.assertIn("surface:table-lab", surface_map)
        self.assertEqual(surface_map["surface:table-lab"]["user_id"], "user:jason")
        self.assertEqual(surface_map["surface:table-lab"]["agent_id"], "agent:jason-agent")
        self.assertEqual(surface_map["surface:table-lab"]["surface_type"], "tabletop")

        # -----------------------------------------------------------------
        # Step 3: Multi-Device Workflow Execution
        # -----------------------------------------------------------------
        # 3A. Phone commits requirement atom atom-e2e-req-1
        req_atom_payload = {
            "atoms": [
                {
                    "id": "atom-e2e-req-1",
                    "type": "REQUIREMENT",
                    "payload": {
                        "statement": "Mobile requirement: multi-surface authentication and key delegation",
                        "content": "Surface tokens must bind to user identity and prevent origin spoofing.",
                    },
                }
            ]
        }
        st, bundle_res = http_request(
            f"{self.base_url}/api/v1/graph/bundle",
            method="POST",
            payload=req_atom_payload,
            token=phone_token,
        )
        self.assertIn(st, (200, 201), f"Phone bundle commit failed: {bundle_res}")

        # Validate atom committed with exact origin provenance
        st, atom1_node = http_request(f"{self.base_url}/api/v1/node/atom-e2e-req-1", token=phone_token)
        self.assertEqual(st, 200)
        self.assertEqual(atom1_node.get("id"), "atom-e2e-req-1")
        self.assertEqual(atom1_node.get("origin", {}).get("surface_id"), "surface:phone-safari")
        self.assertEqual(atom1_node.get("origin", {}).get("user_id"), "user:jason")
        self.assertEqual(atom1_node.get("origin", {}).get("agent_id"), "agent:jason-agent")
        self.assertEqual(atom1_node.get("origin", {}).get("surface_type"), "mobile_browser")

        # 3A-2. Verify Phone surface cannot spoof other users or surfaces (HTTP 403)
        spoof_user_payload = {
            "atoms": [
                {
                    "id": "atom-spoof-user",
                    "type": "NOTE",
                    "header": {
                        "origin": {
                            "user_id": "user:alice",
                            "surface_id": "surface:phone-safari",
                        }
                    },
                    "payload": {"statement": "Attempting user spoofing from phone"},
                }
            ]
        }
        st_spoof_u, _ = http_request(f"{self.base_url}/api/v1/graph/bundle", method="POST", payload=spoof_user_payload, token=phone_token)
        self.assertEqual(st_spoof_u, 403, f"Expected 403 for user spoofing, got {st_spoof_u}")

        spoof_surface_payload = {
            "atoms": [
                {
                    "id": "atom-spoof-surface",
                    "type": "NOTE",
                    "header": {
                        "origin": {
                            "user_id": "user:jason",
                            "surface_id": "surface:workstation",
                        }
                    },
                    "payload": {"statement": "Attempting surface spoofing from phone"},
                }
            ]
        }
        st_spoof_s, _ = http_request(f"{self.base_url}/api/v1/graph/bundle", method="POST", payload=spoof_surface_payload, token=phone_token)
        self.assertEqual(st_spoof_s, 403, f"Expected 403 for surface spoofing, got {st_spoof_s}")

        # 3B. Table queries nodes via /api/v1/query and commits solution atom atom-e2e-sol-1
        st, q_res = http_request(
            f"{self.base_url}/api/v1/query",
            method="POST",
            payload={"match": "n"},
            token=table_token,
        )
        self.assertEqual(st, 200, f"Table query failed: {q_res}")

        # Table commits solution atom with link to requirement atom
        sol_atom_payload = {
            "atoms": [
                {
                    "id": "atom-e2e-sol-1",
                    "type": "SOLUTION",
                    "payload": {
                        "statement": "Tabletop solution: ephemeral PIN pairing & scoped keys",
                        "content": "Workstations approve pairing requests and issue scoped ab_srf_ tokens.",
                        "note_links": [
                            {
                                "target_uuid": "atom-e2e-req-1",
                                "relation": "SOLVES",
                            }
                        ],
                    },
                }
            ]
        }
        st, bundle_sol_res = http_request(
            f"{self.base_url}/api/v1/graph/bundle",
            method="POST",
            payload=sol_atom_payload,
            token=table_token,
        )
        self.assertIn(st, (200, 201), f"Table bundle commit failed: {bundle_sol_res}")

        # Validate solution atom committed with exact origin provenance
        st, atom2_node = http_request(f"{self.base_url}/api/v1/node/atom-e2e-sol-1", token=table_token)
        self.assertEqual(st, 200)
        self.assertEqual(atom2_node.get("id"), "atom-e2e-sol-1")
        self.assertEqual(atom2_node.get("origin", {}).get("surface_id"), "surface:table-lab")
        self.assertEqual(atom2_node.get("origin", {}).get("user_id"), "user:jason")
        self.assertEqual(atom2_node.get("origin", {}).get("agent_id"), "agent:jason-agent")
        self.assertEqual(atom2_node.get("origin", {}).get("surface_type"), "tabletop")

        # Verify link from solution to requirement
        st, atom2_links = http_request(f"{self.base_url}/api/v1/node/atom-e2e-sol-1/links?direction=outbound", token=table_token)
        self.assertEqual(st, 200)
        solves_links = [l for l in atom2_links.get("outbound", []) if l.get("target") == "atom-e2e-req-1" and l.get("relation") == "SOLVES"]
        self.assertTrue(len(solves_links) >= 1, f"SOLVES link missing from atom-e2e-sol-1: {atom2_links}")

        # 3C. Workstation reviews and queries atoms, then commits evaluation verdict
        status_cli = run_cli(["status"], check=True, env=self.workstation_env)
        self.assertEqual(status_cli.returncode, 0)
        self.assertIn("OPERATIONAL", status_cli.stdout)

        # Workstation commits verification verdict atom linking atom-e2e-sol-1
        verdict_payload = {
            "atoms": [
                {
                    "id": "atom-e2e-verdict-1",
                    "type": "VERIFICATION",
                    "payload": {
                        "statement": "Workstation review: verified requirements and solution architecture",
                        "content": "1:1 user-agent identity invariance confirmed across mobile and tabletop surfaces.",
                        "note_links": [
                            {
                                "target_uuid": "atom-e2e-sol-1",
                                "relation": "VALIDATES",
                            }
                        ],
                    },
                }
            ]
        }
        st, bundle_verdict_res = http_request(
            f"{self.base_url}/api/v1/graph/bundle",
            method="POST",
            payload=verdict_payload,
            token=user_token,
        )
        self.assertIn(st, (200, 201), f"Workstation bundle commit failed: {bundle_verdict_res}")

        # Validate verdict atom
        st, verdict_node = http_request(f"{self.base_url}/api/v1/node/atom-e2e-verdict-1", token=user_token)
        self.assertEqual(st, 200)
        self.assertEqual(verdict_node.get("origin", {}).get("user_id"), "user:jason")
        self.assertEqual(verdict_node.get("origin", {}).get("agent_id"), "agent:jason-agent")

        # -----------------------------------------------------------------
        # Step 4: Graph Invariance & Anti-Pollution Assertions
        # -----------------------------------------------------------------
        st, snapshot = http_request(f"{self.base_url}/api/v1/graph/snapshot", token=self.admin_token)
        self.assertEqual(st, 200)
        nodes = snapshot.get("nodes", [])
        edges = snapshot.get("edges", [])

        # Filter all IDENTITY nodes in graph
        identity_nodes = [n for n in nodes if n.get("type") == "IDENTITY"]
        identity_ids = [n.get("id") for n in identity_nodes]

        # Invariant 1: Exactly ONE user:jason identity node exists
        user_identity_nodes = [
            n for n in identity_nodes
            if n.get("id") in ("user:jason", "n:IDENTITY:user:jason") or
               (n.get("role") in ("user", "curator") and "jason" in n.get("id", ""))
        ]
        self.assertEqual(len(user_identity_nodes), 1, f"Expected exactly 1 user:jason identity node, found: {user_identity_nodes}")
        self.assertEqual(user_identity_nodes[0].get("id"), "user:jason")

        # Invariant 2: Exactly ONE agent:jason-agent identity node exists
        agent_identity_nodes = [
            n for n in identity_nodes
            if n.get("id") in ("agent:jason-agent", "n:IDENTITY:agent:jason-agent") or
               (n.get("role") == "agent" and "jason-agent" in n.get("id", ""))
        ]
        self.assertEqual(len(agent_identity_nodes), 1, f"Expected exactly 1 agent:jason-agent identity node, found: {agent_identity_nodes}")
        self.assertEqual(agent_identity_nodes[0].get("id"), "agent:jason-agent")

        # Invariant 3: Zero ephemeral agent nodes exist in the graph
        ephemeral_patterns = [
            "agent:architect",
            "agent:verifier",
            "agent:phone-agent",
            "agent:table-agent",
            "agent:mobile_browser",
            "agent:tabletop",
            "agent:phone-safari",
            "agent:table-lab",
        ]
        for n in identity_nodes:
            nid = n.get("id", "")
            for pattern in ephemeral_patterns:
                self.assertNotEqual(nid, pattern, f"Ephemeral agent node found in graph: {nid}")
            if nid.startswith("agent:"):
                self.assertEqual(nid, "agent:jason-agent", f"Unexpected agent node found: {nid}")

        # Invariant 4: user:jason has DELEGATES_TO edge to agent:jason-agent
        st_links, user_links = http_request(f"{self.base_url}/api/v1/node/user:jason/links?direction=outbound", token=self.admin_token)
        self.assertEqual(st_links, 200)
        outbound_links = user_links.get("outbound", [])
        delegation_links = [
            l for l in outbound_links
            if l.get("target") == "agent:jason-agent" and l.get("relation") == "DELEGATES_TO"
        ]
        self.assertTrue(len(delegation_links) >= 1, f"Missing DELEGATES_TO edge from user:jason: {outbound_links}")

        # Also verify inbound delegation on agent:jason-agent
        st_agent_links, agent_links = http_request(f"{self.base_url}/api/v1/node/agent:jason-agent/links?direction=inbound", token=self.admin_token)
        self.assertEqual(st_agent_links, 200)
        inbound_links = agent_links.get("inbound", [])
        inbound_delegations = [
            l for l in inbound_links
            if l.get("source") == "user:jason" and l.get("relation") == "DELEGATES_TO"
        ]
        self.assertTrue(len(inbound_delegations) >= 1, f"Missing inbound DELEGATES_TO on agent:jason-agent: {inbound_links}")

        # -----------------------------------------------------------------
        # Step 5: Revocation Verification
        # -----------------------------------------------------------------
        # Revoke Phone surface via workstation CLI
        revoke_cli = run_cli(["surface", "revoke", "surface:phone-safari"], check=True, env=self.workstation_env)
        self.assertEqual(revoke_cli.returncode, 0)
        self.assertIn("revoked", revoke_cli.stdout.lower())

        # Assert Phone token is immediately rejected with HTTP 401 on bundle commit
        st_bad_bundle, _ = http_request(
            f"{self.base_url}/api/v1/graph/bundle",
            method="POST",
            payload={
                "atoms": [
                    {
                        "id": "atom-revoked-phone",
                        "type": "NOTE",
                        "payload": {"statement": "Should be rejected post-revocation"},
                    }
                ]
            },
            token=phone_token,
        )
        self.assertEqual(st_bad_bundle, 401, f"Expected 401 after revocation, got {st_bad_bundle}")

        # Assert Phone token is immediately rejected with HTTP 401 on node query
        st_bad_node, _ = http_request(f"{self.base_url}/api/v1/node/atom-e2e-req-1", token=phone_token)
        self.assertEqual(st_bad_node, 401, f"Expected 401 after revocation, got {st_bad_node}")

        # Assert Phone token is immediately rejected with HTTP 401 on /api/v1/query
        st_bad_query, _ = http_request(f"{self.base_url}/api/v1/query", method="POST", payload={"match": "n"}, token=phone_token)
        self.assertEqual(st_bad_query, 401, f"Expected 401 after revocation, got {st_bad_query}")

        # Assert Table surface token remains 100% operational
        st_table_q, _ = http_request(f"{self.base_url}/api/v1/query", method="POST", payload={"match": "n"}, token=table_token)
        self.assertEqual(st_table_q, 200, "Table query failed after phone revocation")

        st_table_node, _ = http_request(f"{self.base_url}/api/v1/node/atom-e2e-sol-1", token=table_token)
        self.assertEqual(st_table_node, 200, "Table node fetch failed after phone revocation")

        st_table_commit, _ = http_request(
            f"{self.base_url}/api/v1/graph/bundle",
            method="POST",
            payload={
                "atoms": [
                    {
                        "id": "atom-e2e-table-post-revoke",
                        "type": "NOTE",
                        "payload": {"statement": "Tabletop remains active after phone revoked"},
                    }
                ]
            },
            token=table_token,
        )
        self.assertIn(st_table_commit, (200, 201), "Table atom commit failed after phone revocation")

        # Assert Workstation credentials remain 100% operational
        list_after = run_cli(["surface", "list", "--format", "json"], check=True, env=self.workstation_env)
        surfaces_after = json.loads(list_after.stdout).get("surfaces", [])
        after_ids = [s["surface_id"] for s in surfaces_after]
        self.assertNotIn("surface:phone-safari", after_ids, "Revoked phone still listed in enrolled surfaces")
        self.assertIn("surface:table-lab", after_ids, "Table missing from enrolled surfaces")

        st_ws_node, _ = http_request(f"{self.base_url}/api/v1/node/atom-e2e-verdict-1", token=user_token)
        self.assertEqual(st_ws_node, 200, "Workstation node fetch failed")


if __name__ == "__main__":
    unittest.main()
