#!/usr/bin/env python3
"""
Unit and integration test for Task 2: FastMCP Swarm Tool Expansion in ab-ctl.py.
Verifies FastMCP server instantiates, registers all 8 swarm tools with expected schemas,
and executes all swarm lifecycle tools against a mock Agentic Blackboard daemon.
"""

from __future__ import annotations

import asyncio
import http.server
import importlib.util
import json
import os
from pathlib import Path
import re
import socketserver
import sys
import threading
import time
import urllib.parse

REPO_ROOT = Path(__file__).resolve().parent.parent
AB_CTL_PY = REPO_ROOT / "src" / "ab-ctl.py"

# Dynamically import ab-ctl.py as module
spec = importlib.util.spec_from_file_location("ab_ctl", AB_CTL_PY)
ab_ctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ab_ctl)


class MockBlackboardHandler(http.server.BaseHTTPRequestHandler):
    """In-memory mock for Agentic Blackboard daemon REST endpoints."""

    def log_message(self, format, *args):
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
            self.server.nodes[node_id] = body
            self._send_json(201, {"status": "CREATED", "id": node_id})
            return

        if path == "/api/v1/graph/bundle":
            atoms = body.get("atoms", [])
            for atom in atoms:
                aid = atom.get("uuid") or atom.get("id")
                if aid:
                    self.server.nodes[aid] = atom
            self._send_json(201, {"status": "COMMITTED", "count": len(atoms)})
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


EXPECTED_SWARM_TOOLS = [
    "swarm_init_context",
    "swarm_create_task",
    "swarm_list_tasks",
    "swarm_claim_lease",
    "swarm_release_lease",
    "swarm_submit_review",
    "swarm_record_verdict",
    "swarm_accept_task",
]


async def run_swarm_mcp_tests(mcp, server, base_url: str, token: str):
    # Step 1 & 2: Verify tool registration
    print("\n--- Verifying FastMCP Tool Registration ---")
    tools = await mcp.list_tools()
    tool_names = {t.name for t in tools}
    print(f"Registered tools ({len(tools)}): {', '.join(sorted(tool_names))}")

    missing_tools = [t for t in EXPECTED_SWARM_TOOLS if t not in tool_names]
    if missing_tools:
        raise AssertionError(f"FastMCP server missing swarm tools: {missing_tools}")

    print("[PASS] All 8 FastMCP swarm tools are registered.")

    # Helper to call tool and parse JSON response
    async def call_tool(name: str, args: dict) -> dict:
        raw_res = await mcp.call_tool(name, args)
        assert len(raw_res) > 0 and len(raw_res[0]) > 0
        text = raw_res[0][0].text
        try:
            return json.loads(text)
        except Exception as e:
            raise AssertionError(f"Tool {name} did not return valid JSON: {text!r}") from e

    # Test 1: swarm_init_context
    print("\n--- Test 1: swarm_init_context ---")
    res = await call_tool("swarm_init_context", {
        "context_id": "ctx-mcp-swarm",
        "name": "MCP Swarm Coordination",
        "user_id": "lead-architect"
    })
    assert res.get("status") == "SUCCESS", f"swarm_init_context failed: {res}"
    assert "ctx-mcp-swarm" in server.contexts
    assert any(n.get("type") == "requirement" for n in server.nodes.values())

    # Check dual-identity headers
    init_reqs = [r for r in server.request_history if r["path"] == "/api/v1/context/register"]
    assert len(init_reqs) > 0
    assert init_reqs[0]["headers"].get("X-Active-User") == "lead-architect"
    assert "X-Active-Agent" in init_reqs[0]["headers"]
    print("[PASS] swarm_init_context registered context and created requirement atom.")

    # Test 2: swarm_create_task Task-A (leaf)
    print("\n--- Test 2: swarm_create_task Task-A ---")
    res = await call_tool("swarm_create_task", {
        "context_id": "ctx-mcp-swarm",
        "name": "Task-A",
        "workflow": "feature",
        "target_symbols": ["symbol_init", "symbol_load"],
        "depends_on": [],
        "blast_radius_k": 2,
        "agent_id": "cpg-architect"
    })
    assert res.get("status") == "SUCCESS", f"swarm_create_task Task-A failed: {res}"
    assert "Task-A" in server.nodes
    t_a = server.nodes["Task-A"]
    assert t_a.get("metadata", {}).get("status") == "READY"
    print("[PASS] swarm_create_task Task-A created.")

    # Test 3: swarm_create_task Task-B depending on Task-A
    print("\n--- Test 3: swarm_create_task Task-B (depends on Task-A) ---")
    res = await call_tool("swarm_create_task", {
        "context_id": "ctx-mcp-swarm",
        "name": "Task-B",
        "workflow": "feature",
        "target_symbols": ["symbol_dispatch"],
        "depends_on": ["Task-A"],
        "blast_radius_k": 3,
        "agent_id": "cpg-architect"
    })
    assert res.get("status") == "SUCCESS", f"swarm_create_task Task-B failed: {res}"
    assert "Task-B" in server.nodes
    dep_links = [l for l in server.links if l["label"] == "DEPENDS_ON"]
    assert any(l["source"] == "Task-B" and l["target"] == "Task-A" for l in dep_links)
    print("[PASS] swarm_create_task Task-B created with DEPENDS_ON link to Task-A.")

    # Test 3b: swarm_create_task with default None lists
    print("\n--- Test 3b: swarm_create_task Task-Defaults ---")
    res = await call_tool("swarm_create_task", {
        "context_id": "ctx-mcp-swarm",
        "name": "Task-Defaults"
    })
    assert res.get("status") == "SUCCESS", f"swarm_create_task Task-Defaults failed: {res}"
    assert "Task-Defaults" in server.nodes
    t_def = server.nodes["Task-Defaults"]
    assert t_def.get("metadata", {}).get("target_symbols") == []
    assert t_def.get("metadata", {}).get("depends_on") == []
    print("[PASS] swarm_create_task Task-Defaults created with default empty lists.")

    # Test 4: swarm_list_tasks
    print("\n--- Test 4: swarm_list_tasks ---")
    res = await call_tool("swarm_list_tasks", {"context_id": "ctx-mcp-swarm"})
    assert res.get("status") == "SUCCESS", f"swarm_list_tasks failed: {res}"
    tasks = res.get("tasks", [])
    task_ids = [t["id"] for t in tasks]
    assert "Task-A" in task_ids, f"Task-A not found in swarm_list_tasks: {tasks}"
    assert "Task-B" in task_ids, f"Task-B not found in swarm_list_tasks: {tasks}"
    print(f"[PASS] swarm_list_tasks listed {len(tasks)} tasks.")

    # Test 5: swarm_claim_lease Task-B (MUST FAIL: prerequisite Task-A is unvalidated)
    print("\n--- Test 5: swarm_claim_lease Task-B (blocked prerequisite) ---")
    res = await call_tool("swarm_claim_lease", {
        "task_id": "Task-B",
        "agent_id": "worker-beta",
        "ttl_sec": 600
    })
    assert res.get("status") == "ERROR", f"Expected error claiming blocked task: {res}"
    assert "blocked" in res.get("message", "").lower() or "not validated" in res.get("message", "").lower()
    print("[PASS] swarm_claim_lease Task-B correctly rejected because Task-A is not validated.")

    # Test 6: swarm_claim_lease Task-A (MUST SUCCEED)
    print("\n--- Test 6: swarm_claim_lease Task-A ---")
    res = await call_tool("swarm_claim_lease", {
        "task_id": "Task-A",
        "agent_id": "worker-alpha",
        "ttl_sec": 300
    })
    assert res.get("status") == "SUCCESS", f"swarm_claim_lease Task-A failed: {res}"
    assert server.nodes["Task-A"].get("metadata", {}).get("status") == "IN_PROGRESS"
    assert server.nodes["Task-A"].get("metadata", {}).get("lease", {}).get("holder") == "worker-alpha"
    print("[PASS] swarm_claim_lease Task-A claimed by worker-alpha.")

    # Test 7: swarm_release_lease Task-A
    print("\n--- Test 7: swarm_release_lease Task-A ---")
    res = await call_tool("swarm_release_lease", {
        "task_id": "Task-A",
        "agent_id": "worker-alpha"
    })
    assert res.get("status") == "SUCCESS", f"swarm_release_lease Task-A failed: {res}"
    assert server.nodes["Task-A"].get("metadata", {}).get("status") == "READY"
    assert server.nodes["Task-A"].get("metadata", {}).get("lease", {}).get("holder") is None
    print("[PASS] swarm_release_lease Task-A returned task to READY.")

    # Test 8: Invariant check - cannot submit review when task is READY
    print("\n--- Test 8: swarm_submit_review invariant (READY task) ---")
    res = await call_tool("swarm_submit_review", {
        "task_id": "Task-A",
        "agent_id": "worker-alpha",
        "patch_summary": "Premature patch"
    })
    assert res.get("status") == "ERROR", f"Expected error submitting review on READY task: {res}"
    assert "expected 'in_progress'" in res.get("message", "").lower()

    # Re-claim with worker-alpha
    await call_tool("swarm_claim_lease", {
        "task_id": "Task-A",
        "agent_id": "worker-alpha",
        "ttl_sec": 300
    })

    # Invariant check - non-holder cannot submit review
    res = await call_tool("swarm_submit_review", {
        "task_id": "Task-A",
        "agent_id": "worker-gamma",
        "patch_summary": "Wrong agent patch"
    })
    assert res.get("status") == "ERROR", f"Expected error when non-holder submits review: {res}"

    # Submit review by worker-alpha
    res = await call_tool("swarm_submit_review", {
        "task_id": "Task-A",
        "agent_id": "worker-alpha",
        "patch_summary": "Implementation of symbol_init and symbol_load"
    })
    assert res.get("status") == "SUCCESS", f"swarm_submit_review failed: {res}"
    assert server.nodes["Task-A"].get("metadata", {}).get("status") == "REVIEW_PENDING"
    sol_links = [l for l in server.links if l["label"] == "HAS_SOLUTION"]
    assert any(l["source"] == "Task-A" for l in sol_links)

    # Invariant: cannot claim lease on REVIEW_PENDING task
    res = await call_tool("swarm_claim_lease", {"task_id": "Task-A", "agent_id": "worker-alpha"})
    assert res.get("status") == "ERROR", f"Expected error claiming lease on REVIEW_PENDING task: {res}"
    assert "review_pending" in res.get("message", "").lower()

    # Invariant: cannot release lease on REVIEW_PENDING task
    res = await call_tool("swarm_release_lease", {"task_id": "Task-A", "agent_id": "worker-alpha"})
    assert res.get("status") == "ERROR", f"Expected error releasing lease on REVIEW_PENDING task: {res}"
    assert "review_pending" in res.get("message", "").lower()

    print("[PASS] swarm_submit_review verified and marked REVIEW_PENDING.")

    # Test 9: Invariant check - cannot record verdict when task is not REVIEW_PENDING
    print("\n--- Test 9: swarm_record_verdict invariant (not REVIEW_PENDING) ---")
    res = await call_tool("swarm_record_verdict", {
        "task_id": "Task-B",
        "verifier_id": "cpg-verifier-01",
        "verdict": "PASS",
        "details": "Premature verdict"
    })
    assert res.get("status") == "ERROR", f"Expected error recording verdict on READY task: {res}"
    assert "review_pending" in res.get("message", "").lower()

    # Test 10: swarm_record_verdict FAIL on Task-A
    print("\n--- Test 10: swarm_record_verdict FAIL ---")
    res = await call_tool("swarm_record_verdict", {
        "task_id": "Task-A",
        "verifier_id": "cpg-verifier-01",
        "verdict": "FAIL",
        "details": '{"trace": "Null pointer on empty input"}'
    })
    assert res.get("status") == "SUCCESS", f"swarm_record_verdict FAIL failed: {res}"
    assert server.nodes["Task-A"].get("metadata", {}).get("status") == "READY"
    assert server.nodes["Task-A"].get("metadata", {}).get("lease", {}).get("holder") is None
    refute_links = [l for l in server.links if l["label"] == "REFUTES"]
    assert any(l["source"] == "Task-A" for l in refute_links)
    print("[PASS] swarm_record_verdict FAIL reset task to READY and created REFUTES link.")

    # Test 11: Re-claim, re-submit, swarm_record_verdict PASS
    print("\n--- Test 11: swarm_record_verdict PASS ---")
    await call_tool("swarm_claim_lease", {"task_id": "Task-A", "agent_id": "worker-alpha"})
    await call_tool("swarm_submit_review", {
        "task_id": "Task-A",
        "agent_id": "worker-alpha",
        "patch_summary": "Fixed null pointer"
    })
    res = await call_tool("swarm_record_verdict", {
        "task_id": "Task-A",
        "verifier_id": "cpg-verifier-01",
        "verdict": "PASS",
        "details": {"violations": 0}
    })
    assert res.get("status") == "SUCCESS", f"swarm_record_verdict PASS failed: {res}"
    assert server.nodes["Task-A"].get("metadata", {}).get("status") == "VALIDATED"
    assert server.nodes["Task-A"].get("metadata", {}).get("lease", {}).get("holder") is None
    val_links = [l for l in server.links if l["label"] == "VALIDATED_BY"]
    assert any(l["source"] == "Task-A" for l in val_links)
    proof_nodes = [n for n in server.nodes.values() if n.get("type") == "verification_proof"]
    assert any(n.get("metadata", {}).get("details") == {"violations": 0} for n in proof_nodes)

    # Invariant: cannot claim lease on VALIDATED task
    res = await call_tool("swarm_claim_lease", {"task_id": "Task-A", "agent_id": "worker-alpha"})
    assert res.get("status") == "ERROR", f"Expected error claiming lease on VALIDATED task: {res}"
    assert "validated" in res.get("message", "").lower()

    # Invariant: cannot release lease on VALIDATED task
    res = await call_tool("swarm_release_lease", {"task_id": "Task-A", "agent_id": "worker-alpha"})
    assert res.get("status") == "ERROR", f"Expected error releasing lease on VALIDATED task: {res}"
    assert "validated" in res.get("message", "").lower()
    print("[PASS] swarm_record_verdict PASS validated task and prevented re-claim and release.")

    # Test 12: swarm_accept_task
    print("\n--- Test 12: swarm_accept_task ---")
    # Invariant: cannot accept unvalidated task Task-B
    res = await call_tool("swarm_accept_task", {
        "task_id": "Task-B",
        "stakeholder_id": "stakeholder-01",
        "notes": "Premature acceptance"
    })
    assert res.get("status") == "ERROR", f"Expected error accepting unvalidated task: {res}"
    assert "validated" in res.get("message", "").lower()

    # Accept Task-A
    res = await call_tool("swarm_accept_task", {
        "task_id": "Task-A",
        "stakeholder_id": "stakeholder-01",
        "notes": "Feature accepted into baseline"
    })
    assert res.get("status") == "SUCCESS", f"swarm_accept_task failed: {res}"
    assert server.nodes["Task-A"].get("metadata", {}).get("status") == "COMPLETED"
    acc_links = [l for l in server.links if l["label"] == "ACCEPTS"]
    assert any(l["source"] == "Task-A" or l["target"] == "Task-A" for l in acc_links)

    # Invariant: cannot claim lease on COMPLETED task
    res = await call_tool("swarm_claim_lease", {"task_id": "Task-A", "agent_id": "worker-alpha"})
    assert res.get("status") == "ERROR", f"Expected error claiming lease on COMPLETED task: {res}"
    assert "completed" in res.get("message", "").lower()

    # Invariant: cannot release lease on COMPLETED task
    res = await call_tool("swarm_release_lease", {"task_id": "Task-A", "agent_id": "worker-alpha"})
    assert res.get("status") == "ERROR", f"Expected error releasing lease on COMPLETED task: {res}"
    assert "completed" in res.get("message", "").lower()
    print("[PASS] swarm_accept_task accepted Task-A into COMPLETED.")

    # Test 13: Claim Task-B (now unblocked!)
    print("\n--- Test 13: Claim Task-B (unblocked now) ---")
    res = await call_tool("swarm_claim_lease", {
        "task_id": "Task-B",
        "agent_id": "worker-beta",
        "ttl_sec": 600
    })
    assert res.get("status") == "SUCCESS", f"Expected Task-B claim to succeed: {res}"
    assert server.nodes["Task-B"].get("metadata", {}).get("status") == "IN_PROGRESS"
    assert server.nodes["Task-B"].get("metadata", {}).get("lease", {}).get("holder") == "worker-beta"
    print("[PASS] Task-B lease claimed successfully after prerequisite completed.")


def main():
    print("=== Testing ab-ctl FastMCP Swarm Tool Expansion ===")

    server = MockServer(("127.0.0.1", 0), MockBlackboardHandler)
    port = server.server_address[1]
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    base_url = f"http://127.0.0.1:{port}"
    token = "ab_adm_mock_token_12345"

    try:
        mcp = ab_ctl.build_mcp_server(base_url, token)
        assert mcp is not None and mcp != 1, "build_mcp_server() failed to initialize"
        print("[PASS] build_mcp_server() instantiated successfully.")

        asyncio.run(run_swarm_mcp_tests(mcp, server, base_url, token))
        print("\n=== All FastMCP Swarm Tool Tests Passed Successfully! ===")

    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
