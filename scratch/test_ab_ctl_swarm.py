#!/usr/bin/env python3
"""
Integration test for Task 1: ab-ctl swarm CLI & Core Lease Management.
Tests all swarm subcommands (init, task create/list, lease claim/release,
review submit/verdict, accept) against a mock Agentic Blackboard daemon.
"""

from __future__ import annotations

import http.server
import json
import os
from pathlib import Path
import re
import socketserver
import subprocess
import sys
import threading
import time
import urllib.parse

REPO_ROOT = Path(__file__).resolve().parent.parent
AB_CTL_PY = REPO_ROOT / "src" / "ab-ctl.py"


class MockBlackboardHandler(http.server.BaseHTTPRequestHandler):
    """In-memory mock for Agentic Blackboard daemon REST endpoints."""

    def log_message(self, format, *args):
        # Silence default HTTP server logging
        pass

    def _send_json(self, status: int, data: dict | list):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_text(self, status: int, text: str):
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        self.server.request_history.append({
            "method": "GET",
            "path": self.path,
            "headers": dict(self.headers),
        })

        if path == "/api/v1/schema":
            self._send_json(200, {"status": "ok"})
            return

        if path.startswith("/api/v1/context/"):
            context_id = path.removeprefix("/api/v1/context/").split("/")[0]
            ctx = self.server.contexts.get(context_id, {
                "context_id": context_id,
                "active_surfaces": [],
                "focus": {"selected": []}
            })
            self._send_json(200, ctx)
            return

        node_links_match = re.match(r"^/api/v1/node/([^/]+)/links", path)
        if node_links_match:
            node_id = node_links_match.group(1)
            direction = query.get("direction", ["both"])[0]
            inbound = []
            outbound = []
            for link in self.server.links:
                if link["target"] == node_id and direction in ("both", "inbound"):
                    inbound.append({
                        "uuid": link["source"],
                        "source": link["source"],
                        "relation": link["label"],
                        "statement": ""
                    })
                if link["source"] == node_id and direction in ("both", "outbound"):
                    outbound.append({
                        "uuid": link["target"],
                        "target": link["target"],
                        "relation": link["label"],
                        "statement": ""
                    })
            self._send_json(200, {
                "uuid": node_id,
                "inbound": inbound,
                "outbound": outbound
            })
            return

        node_match = re.match(r"^/api/v1/node/([^/]+)", path)
        if node_match:
            node_id = node_match.group(1)
            if node_id in self.server.nodes:
                self._send_json(200, self.server.nodes[node_id])
            else:
                self._send_text(404, "Node not found")
            return

        if path == "/api/v1/search":
            q = query.get("q", [""])[0].lower()
            matches = []
            for n in self.server.nodes.values():
                name = str(n.get("metadata", {}).get("name", "")).lower()
                nid = str(n.get("id", "")).lower()
                stmt = str(n.get("statement", "")).lower()
                cid = str(n.get("metadata", {}).get("context_id", "")).lower()
                content = str(n.get("content", "")).lower()
                if not q or q in name or q in nid or q in stmt or q in cid or q in content:
                    matches.append(n)
            self._send_json(200, {"matches": matches, "count": len(matches)})
            return

        self._send_text(404, f"Unknown GET route: {path}")

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        content_len = int(self.headers.get("Content-Length", 0))
        post_data = self.rfile.read(content_len).decode("utf-8") if content_len > 0 else "{}"
        try:
            body = json.loads(post_data)
        except Exception:
            body = {}

        self.server.request_history.append({
            "method": "POST",
            "path": self.path,
            "headers": dict(self.headers),
            "body": body
        })

        if path == "/api/v1/context/register":
            cid = body.get("context_id", "default")
            self.server.contexts[cid] = body
            self._send_json(200, {"status": "REGISTERED", "context_id": cid})
            return

        if path == "/api/v1/graph/node":
            node_id = body.get("id", "")
            if not node_id:
                self._send_text(400, "Missing id")
                return
            # Store/update node
            self.server.nodes[node_id] = body
            self._send_json(201, {"status": "CREATED", "id": node_id})
            return

        if path == "/api/v1/link":
            src = body.get("source")
            dst = body.get("target")
            lbl = body.get("label", "RELATED")
            self.server.links.append({
                "source": src,
                "target": dst,
                "label": lbl,
                "weight": body.get("weight", 1.0)
            })
            self._send_json(200, {"status": "SYNCED"})
            return

        self._send_text(404, f"Unknown POST route: {path}")


class MockServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    def __init__(self, server_address, handler_class):
        super().__init__(server_address, handler_class)
        self.nodes = {}
        self.links = []
        self.contexts = {}
        self.request_history = []


def run_cli(args: list[str], check: bool = True) -> subprocess.CompletedProcess:
    cmd = [sys.executable, str(AB_CTL_PY)] + args
    print(f"[RUN CLI] {' '.join(cmd)}")
    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    if check and proc.returncode != 0:
        print(f"[STDOUT]\n{proc.stdout}")
        print(f"[STDERR]\n{proc.stderr}")
        raise RuntimeError(f"Command failed with code {proc.returncode}: {' '.join(cmd)}")
    return proc


def main():
    print("=== Testing ab-ctl swarm CLI & Core Lease Management ===")

    # Step 1: swarm --help output
    print("\n--- Test 1: ab-ctl swarm --help ---")
    proc = run_cli(["swarm", "--help"])
    assert proc.returncode == 0
    help_out = proc.stdout
    assert "init" in help_out, "Missing 'init' in swarm --help"
    assert "task" in help_out, "Missing 'task' in swarm --help"
    assert "lease" in help_out, "Missing 'lease' in swarm --help"
    assert "review" in help_out, "Missing 'review' in swarm --help"
    assert "accept" in help_out, "Missing 'accept' in swarm --help"
    print("[PASS] ab-ctl swarm --help verified.")

    # Start mock server
    server = MockServer(("127.0.0.1", 0), MockBlackboardHandler)
    port = server.server_address[1]
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    base_url = f"http://127.0.0.1:{port}"
    token = "ab_adm_mock_token_12345"

    try:
        # Step 2: ab-ctl swarm init
        print("\n--- Test 2: ab-ctl swarm init ---")
        proc = run_cli([
            "swarm", "init",
            "--context", "ctx-test-swarm",
            "--name", "Security Swarm",
            "--requirement", "Implement TLS 1.3 Handshake Verification",
            f"--connect={base_url}",
            f"--token={token}"
        ])
        assert proc.returncode == 0
        assert "ctx-test-swarm" in server.contexts
        assert any(n.get("type") == "requirement" for n in server.nodes.values())
        # Verify dual identity headers were transmitted
        init_reqs = [r for r in server.request_history if r["path"] == "/api/v1/context/register"]
        assert len(init_reqs) > 0
        assert "Authorization" in init_reqs[0]["headers"]
        assert "X-Active-User" in init_reqs[0]["headers"] or "X-Active-Agent" in init_reqs[0]["headers"]
        print("[PASS] swarm init registered context and created requirement atom.")

        # Step 3: ab-ctl swarm task create (Task-1 leaf)
        print("\n--- Test 3: ab-ctl swarm task create Task-1 ---")
        proc = run_cli([
            "swarm", "task", "create",
            "--context", "ctx-test-swarm",
            "--name", "Task-1",
            "--workflow", "feature",
            "--symbols", "tls_init,tls_handshake",
            "--agent", "cpg-architect",
            f"--connect={base_url}",
            f"--token={token}"
        ])
        assert proc.returncode == 0
        assert "Task-1" in server.nodes
        t1 = server.nodes["Task-1"]
        assert t1.get("type") == "task"
        meta1 = t1.get("metadata", {})
        assert meta1.get("status") == "READY"
        assert meta1.get("workflow") == "feature"
        assert "tls_init" in meta1.get("target_symbols", [])
        assert meta1.get("blast_radius_k") == 2
        print("[PASS] swarm task create created Task-1 in READY state with default blast-radius=2.")

        # Step 4: ab-ctl swarm task create (Task-2 depending on Task-1 with -k 3)
        print("\n--- Test 4: ab-ctl swarm task create Task-2 with depends-on Task-1 ---")
        proc = run_cli([
            "swarm", "task", "create",
            "--context", "ctx-test-swarm",
            "--name", "Task-2",
            "--workflow", "feature",
            "--symbols", "tls_client_connect",
            "--depends-on", "Task-1",
            "-k", "3",
            "--agent", "cpg-architect",
            f"--connect={base_url}",
            f"--token={token}"
        ])
        assert proc.returncode == 0
        assert "Task-2" in server.nodes
        meta2 = server.nodes["Task-2"].get("metadata", {})
        assert meta2.get("blast_radius_k") == 3
        # Verify DEPENDS_ON link was created
        dep_links = [l for l in server.links if l["label"] == "DEPENDS_ON"]
        assert len(dep_links) > 0
        assert any(l["source"] == "Task-2" and l["target"] == "Task-1" for l in dep_links)
        print("[PASS] swarm task create created Task-2 and linked DEPENDS_ON Task-1 with blast-radius=3.")

        # Step 5: ab-ctl swarm task list (table & json format)
        print("\n--- Test 5: ab-ctl swarm task list ---")
        proc_table = run_cli([
            "swarm", "task", "list",
            "--context", "ctx-test-swarm",
            f"--connect={base_url}",
            f"--token={token}"
        ])
        assert proc_table.returncode == 0
        assert "Task-1" in proc_table.stdout
        assert "Task-2" in proc_table.stdout
        assert "READY" in proc_table.stdout

        proc_json = run_cli([
            "swarm", "task", "list",
            "--context", "ctx-test-swarm",
            "--format", "json",
            f"--connect={base_url}",
            f"--token={token}"
        ])
        assert proc_json.returncode == 0
        parsed_json = json.loads(proc_json.stdout)
        assert isinstance(parsed_json, (list, dict))
        print("[PASS] swarm task list formats verified.")

        # Step 6: ab-ctl swarm lease claim Task-2 (MUST FAIL: Task-1 is unvalidated)
        print("\n--- Test 6: ab-ctl swarm lease claim Task-2 (should be BLOCKED) ---")
        proc_blocked = run_cli([
            "swarm", "lease", "claim", "Task-2",
            "--agent", "worker-beta",
            f"--connect={base_url}",
            f"--token={token}"
        ], check=False)
        assert proc_blocked.returncode != 0, "Expected lease claim to fail when prerequisite is not validated"
        assert "BLOCKED" in (proc_blocked.stderr + proc_blocked.stdout).upper()
        print("[PASS] swarm lease claim on blocked task rejected with non-zero exit.")

        # Step 7: ab-ctl swarm lease claim Task-1 (MUST SUCCEED)
        print("\n--- Test 7: ab-ctl swarm lease claim Task-1 ---")
        proc_claim = run_cli([
            "swarm", "lease", "claim", "Task-1",
            "--agent", "worker-alpha",
            "--ttl", "300",
            f"--connect={base_url}",
            f"--token={token}"
        ])
        assert proc_claim.returncode == 0
        t1_claimed = server.nodes["Task-1"]
        meta_claimed = t1_claimed.get("metadata", {})
        assert meta_claimed.get("status") == "IN_PROGRESS"
        assert meta_claimed.get("lease", {}).get("holder") == "worker-alpha"
        print("[PASS] swarm lease claim set status=IN_PROGRESS and holder=worker-alpha.")

        # Step 8: ab-ctl swarm lease release Task-1
        print("\n--- Test 8: ab-ctl swarm lease release Task-1 ---")
        proc_release = run_cli([
            "swarm", "lease", "release", "Task-1",
            "--agent", "worker-alpha",
            f"--connect={base_url}",
            f"--token={token}"
        ])
        assert proc_release.returncode == 0
        t1_rel = server.nodes["Task-1"]
        meta_rel = t1_rel.get("metadata", {})
        assert meta_rel.get("status") == "READY"
        assert meta_rel.get("lease", {}).get("holder") is None
        print("[PASS] swarm lease release reset status=READY and cleared lease holder.")

        # Step 9: Invariant checks & submit review
        print("\n--- Test 9: ab-ctl swarm review submit Task-1 (precondition checks & submit) ---")
        # Precondition check 1: submit review when task is READY (must fail)
        proc_premature_rev = run_cli([
            "swarm", "review", "submit", "Task-1",
            "--agent", "worker-alpha",
            "--patch", "Premature patch",
            f"--connect={base_url}",
            f"--token={token}"
        ], check=False)
        assert proc_premature_rev.returncode != 0
        assert "expected 'IN_PROGRESS'" in (proc_premature_rev.stderr + proc_premature_rev.stdout)

        # Claim Task-1 with worker-alpha
        run_cli([
            "swarm", "lease", "claim", "Task-1",
            "--agent", "worker-alpha",
            f"--connect={base_url}",
            f"--token={token}"
        ])

        # Precondition check 2: submit review by non-holder worker-gamma (must fail)
        proc_wrong_agent = run_cli([
            "swarm", "review", "submit", "Task-1",
            "--agent", "worker-gamma",
            "--patch", "Wrong agent patch",
            f"--connect={base_url}",
            f"--token={token}"
        ], check=False)
        assert proc_wrong_agent.returncode != 0
        assert "leased by 'worker-alpha', not 'worker-gamma'" in (proc_wrong_agent.stderr + proc_wrong_agent.stdout)

        # Submit review by worker-alpha (must succeed)
        proc_review = run_cli([
            "swarm", "review", "submit", "Task-1",
            "--agent", "worker-alpha",
            "--patch", "Introduce TLS 1.3 state machine in tls_handshake()",
            f"--connect={base_url}",
            f"--token={token}"
        ])
        assert proc_review.returncode == 0
        t1_rev = server.nodes["Task-1"]
        assert t1_rev.get("metadata", {}).get("status") == "REVIEW_PENDING"
        # Check HAS_SOLUTION link
        sol_links = [l for l in server.links if l["label"] == "HAS_SOLUTION"]
        assert len(sol_links) > 0
        assert any(l["source"] == "Task-1" for l in sol_links)
        print("[PASS] swarm review submit preconditions and submission verified.")

        # Step 10: Dialectic Verdict FAIL
        print("\n--- Test 10: ab-ctl swarm review verdict FAIL ---")
        proc_fail = run_cli([
            "swarm", "review", "verdict", "Task-1",
            "--verifier", "cpg-verifier-01",
            "--verdict", "FAIL",
            "--details", '{"trace": "Typestate leak on error path"}',
            f"--connect={base_url}",
            f"--token={token}"
        ])
        assert proc_fail.returncode == 0
        t1_failed = server.nodes["Task-1"]
        assert t1_failed.get("metadata", {}).get("status") == "READY"
        assert t1_failed.get("metadata", {}).get("lease", {}).get("holder") is None
        refute_links = [l for l in server.links if l["label"] == "REFUTES"]
        assert len(refute_links) > 0
        assert any(l["source"] == "Task-1" for l in refute_links)
        print("[PASS] swarm review verdict FAIL attached REFUTES and reset Task-1 to READY.")

        # Step 11: Re-claim, re-submit, Verdict PASS
        print("\n--- Test 11: ab-ctl swarm review verdict PASS ---")
        run_cli([
            "swarm", "lease", "claim", "Task-1",
            "--agent", "worker-alpha",
            f"--connect={base_url}",
            f"--token={token}"
        ])
        run_cli([
            "swarm", "review", "submit", "Task-1",
            "--agent", "worker-alpha",
            "--patch", "Fixed typestate leak on error path",
            f"--connect={base_url}",
            f"--token={token}"
        ])
        proc_pass = run_cli([
            "swarm", "review", "verdict", "Task-1",
            "--verifier", "cpg-verifier-01",
            "--verdict", "PASS",
            "--details", '{"taint_violations": 0, "smt_paths": 12}',
            f"--connect={base_url}",
            f"--token={token}"
        ])
        assert proc_pass.returncode == 0
        t1_passed = server.nodes["Task-1"]
        assert t1_passed.get("metadata", {}).get("status") == "VALIDATED"
        assert t1_passed.get("metadata", {}).get("lease", {}).get("holder") is None
        assert t1_passed.get("metadata", {}).get("lease", {}).get("expires_at") == 0
        val_links = [l for l in server.links if l["label"] == "VALIDATED_BY"]
        assert len(val_links) > 0
        assert any(l["source"] == "Task-1" for l in val_links)

        # Precondition check: Cannot claim lease on VALIDATED task
        proc_claim_val = run_cli([
            "swarm", "lease", "claim", "Task-1",
            "--agent", "worker-alpha",
            f"--connect={base_url}",
            f"--token={token}"
        ], check=False)
        assert proc_claim_val.returncode != 0
        assert "task is already VALIDATED" in (proc_claim_val.stderr + proc_claim_val.stdout)
        print("[PASS] swarm review verdict PASS cleared lease, set VALIDATED, and blocked re-claim.")

        # Step 12: Stakeholder acceptance
        print("\n--- Test 12: ab-ctl swarm accept Task-1 ---")
        # Precondition check: Cannot accept unvalidated task (Task-2 is READY)
        proc_premature_acc = run_cli([
            "swarm", "accept", "Task-2",
            "--stakeholder", "cpg-stakeholder-01",
            f"--connect={base_url}",
            f"--token={token}"
        ], check=False)
        assert proc_premature_acc.returncode != 0
        assert "status is 'READY', expected 'VALIDATED'" in (proc_premature_acc.stderr + proc_premature_acc.stdout)

        proc_acc = run_cli([
            "swarm", "accept", "Task-1",
            "--stakeholder", "cpg-stakeholder-01",
            "--notes", "Acceptance criteria met, handshake verified",
            f"--connect={base_url}",
            f"--token={token}"
        ])
        assert proc_acc.returncode == 0
        t1_acc = server.nodes["Task-1"]
        assert t1_acc.get("metadata", {}).get("status") == "COMPLETED"
        acc_links = [l for l in server.links if l["label"] == "ACCEPTS"]
        assert len(acc_links) > 0
        assert any(l["source"] == "Task-1" or l["target"] == "Task-1" for l in acc_links)

        # Precondition check: Cannot claim lease on COMPLETED task
        proc_claim_comp = run_cli([
            "swarm", "lease", "claim", "Task-1",
            "--agent", "worker-alpha",
            f"--connect={base_url}",
            f"--token={token}"
        ], check=False)
        assert proc_claim_comp.returncode != 0
        assert "task is already COMPLETED" in (proc_claim_comp.stderr + proc_claim_comp.stdout)
        print("[PASS] swarm accept marked status=COMPLETED and enforced validation invariants.")

        # Step 13: Claim Task-2 (MUST SUCCEED NOW that Task-1 is COMPLETED)
        print("\n--- Test 13: ab-ctl swarm lease claim Task-2 (unblocked now) ---")
        proc_t2_claim = run_cli([
            "swarm", "lease", "claim", "Task-2",
            "--agent", "worker-beta",
            "--ttl", "600",
            f"--connect={base_url}",
            f"--token={token}"
        ])
        assert proc_t2_claim.returncode == 0
        t2_claimed = server.nodes["Task-2"]
        assert t2_claimed.get("metadata", {}).get("status") == "IN_PROGRESS"
        assert t2_claimed.get("metadata", {}).get("lease", {}).get("holder") == "worker-beta"
        print("[PASS] swarm lease claim on Task-2 succeeded after prerequisite completion.")

        print("\n=== All ab-ctl Swarm CLI Tests Passed Successfully! ===")

    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
