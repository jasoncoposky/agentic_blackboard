#!/usr/bin/env python3
"""
End-to-End Swarm Integration Test: 4-Agent Dialectic Swarm Lifecycle.

Simulates the complete dialectic swarm coordination lifecycle against a live C++
Agentic Blackboard daemon (build/agentic-blackboardd):
  - Phase 0: Isolated Daemon Spawning, Bootstrap Auth & Multi-Agent Token Generation
             (admin, stakeholder user:jason, agent:cpg-architect-01,
              agent:cpg-worker-01, agent:cpg-worker-02, agent:cpg-verifier-01)
  - Phase 1: Stakeholder Agent (Requirements & Context Initiation)
  - Phase 2: Architect Agent (Task DAG & Dependency Construction)
  - Phase 3: Worker 2 Premature Claim (Dependency Barrier Verification)
  - Phase 4: Worker 1 Claim & Review Submission
  - Phase 5: Verifier Agent Refutation & Auto-Increment
  - Phase 6: Worker 1 Fix & Verifier Validation
  - Phase 7: Stakeholder Acceptance (Scope Completion & Claim Prevention)
  - Phase 8: Worker 2 Unblocked Claim (DAG Progression)
  - Phase 9: FastMCP Integration Test against Live Daemon
  - Phase 10: Clean Daemon Shutdown & Temp Substrate Cleanup
"""

from __future__ import annotations

import asyncio
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

REPO_ROOT = Path(__file__).resolve().parent.parent
DAEMON_BIN = REPO_ROOT / "build" / "agentic-blackboardd"
AB_CTL_PY = REPO_ROOT / "src" / "ab-ctl.py"

# Dynamically import ab-ctl module for FastMCP testing
spec = importlib.util.spec_from_file_location("ab_ctl", AB_CTL_PY)
ab_ctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ab_ctl)


def find_free_port() -> int:
    """Allocate an unused ephemeral TCP port."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("", 0))
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        return s.getsockname()[1]


def run_cli(args: list[str], check: bool = True, env: dict | None = None) -> subprocess.CompletedProcess:
    """Run ab-ctl CLI command and capture output."""
    cmd = [sys.executable, str(AB_CTL_PY)] + args
    cmd_str = " ".join(cmd)
    print(f"[CLI] {cmd_str}")
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
        print(f"[ERROR STDOUT]\n{proc.stdout}")
        print(f"[ERROR STDERR]\n{proc.stderr}")
        raise RuntimeError(f"Command failed with code {proc.returncode}: {cmd_str}")
    return proc


def wait_for_daemon(base_url: str, timeout_secs: float = 15.0) -> bool:
    """Poll daemon schema discovery endpoint until responsive."""
    start = time.time()
    while time.time() - start < timeout_secs:
        try:
            req = urllib.request.Request(f"{base_url}/api/v1/schema")
            with urllib.request.urlopen(req, timeout=1.0) as resp:
                if resp.status == 200:
                    return True
        except Exception:
            time.sleep(0.2)
    return False


def http_get_json(url: str, token: str) -> dict:
    """Perform authenticated HTTP GET request returning parsed JSON."""
    headers = {"Authorization": f"Bearer {token}"}
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=5.0) as resp:
        return json.loads(resp.read().decode("utf-8"))


async def run_fastmcp_suite(mcp, context_id: str):
    """Execute and verify FastMCP swarm tools against the live daemon."""
    print("\n" + "=" * 70)
    print("[Phase 9] FastMCP Tool Integration Test against Live Daemon")
    print("=" * 70)

    async def call_mcp(name: str, args: dict) -> dict:
        raw_res = await mcp.call_tool(name, args)
        if isinstance(raw_res, (list, tuple)) and len(raw_res) > 0:
            first = raw_res[0]
            if isinstance(first, (list, tuple)) and len(first) > 0:
                text = getattr(first[0], "text", str(first[0]))
            elif hasattr(first, "text"):
                text = first.text
            else:
                text = str(first)
        elif hasattr(raw_res, "content") and len(raw_res.content) > 0:
            text = getattr(raw_res.content[0], "text", str(raw_res.content[0]))
        else:
            text = str(raw_res)

        try:
            return json.loads(text)
        except Exception as e:
            raise AssertionError(f"FastMCP tool '{name}' did not return valid JSON: {text!r}") from e

    # 1. swarm_list_tasks
    list_res = await call_mcp("swarm_list_tasks", {"context_id": context_id})
    assert list_res.get("status") == "SUCCESS", f"swarm_list_tasks failed: {list_res}"
    tasks_map = {t["id"]: t for t in list_res.get("tasks", [])}
    assert "task-leaf-buffer" in tasks_map, f"task-leaf-buffer not listed: {tasks_map}"
    assert "task-consumer-mgr" in tasks_map, f"task-consumer-mgr not listed: {tasks_map}"
    assert tasks_map["task-leaf-buffer"]["status"] == "COMPLETED"
    assert tasks_map["task-consumer-mgr"]["status"] == "IN_PROGRESS"
    print(f"[PASS] FastMCP swarm_list_tasks verified ({len(tasks_map)} tasks).")

    # 2. swarm_release_lease on task-consumer-mgr
    rel_res = await call_mcp("swarm_release_lease", {
        "task_id": "task-consumer-mgr",
        "agent_id": "cpg-worker-02"
    })
    assert rel_res.get("status") == "SUCCESS", f"swarm_release_lease failed: {rel_res}"
    print("[PASS] FastMCP swarm_release_lease reset task to READY.")

    # 3. swarm_claim_lease on task-consumer-mgr
    claim_res = await call_mcp("swarm_claim_lease", {
        "task_id": "task-consumer-mgr",
        "agent_id": "cpg-worker-02",
        "ttl_sec": 300
    })
    assert claim_res.get("status") == "SUCCESS", f"swarm_claim_lease failed: {claim_res}"
    print("[PASS] FastMCP swarm_claim_lease re-claimed task-consumer-mgr.")

    # 4. swarm_submit_review on task-consumer-mgr
    sub_res = await call_mcp("swarm_submit_review", {
        "task_id": "task-consumer-mgr",
        "agent_id": "cpg-worker-02",
        "patch_summary": "Implementation of consumer pool management"
    })
    assert sub_res.get("status") == "SUCCESS", f"swarm_submit_review failed: {sub_res}"
    print("[PASS] FastMCP swarm_submit_review set task status to REVIEW_PENDING.")

    # 5. swarm_record_verdict PASS on task-consumer-mgr
    verd_res = await call_mcp("swarm_record_verdict", {
        "task_id": "task-consumer-mgr",
        "verifier_id": "cpg-verifier-01",
        "verdict": "PASS",
        "details": {"assertions": 24, "data_races": 0}
    })
    assert verd_res.get("status") == "SUCCESS", f"swarm_record_verdict failed: {verd_res}"
    print("[PASS] FastMCP swarm_record_verdict validated task-consumer-mgr.")

    # 6. swarm_accept_task on task-consumer-mgr
    acc_res = await call_mcp("swarm_accept_task", {
        "task_id": "task-consumer-mgr",
        "stakeholder_id": "jason",
        "notes": "Consumer pool accepted into architecture baseline"
    })
    assert acc_res.get("status") == "SUCCESS", f"swarm_accept_task failed: {acc_res}"
    print("[PASS] FastMCP swarm_accept_task accepted task into COMPLETED.")

    # 7. Final task list check
    final_list = await call_mcp("swarm_list_tasks", {"context_id": context_id})
    final_tasks = {t["id"]: t for t in final_list.get("tasks", [])}
    assert final_tasks["task-leaf-buffer"]["status"] == "COMPLETED"
    assert final_tasks["task-consumer-mgr"]["status"] == "COMPLETED"
    print("[PASS] FastMCP verified both tasks reached COMPLETED terminal status.")


def main():
    print("=" * 80)
    print("4-AGENT DIALECTIC SWARM END-TO-END INTEGRATION TEST")
    print("Live C++ Daemon (agentic-blackboardd) Verification Suite")
    print("=" * 80)

    if not DAEMON_BIN.is_file():
        raise FileNotFoundError(f"Daemon binary not found at: {DAEMON_BIN}. Run cmake build first!")

    temp_dir = tempfile.mkdtemp(prefix="bb_swarm_e2e_")
    data_dir = os.path.join(temp_dir, "substrate")
    test_port = int(os.environ.get("AB_TEST_PORT", 0)) or find_free_port()
    base_url = f"http://127.0.0.1:{test_port}"
    daemon_proc = None

    try:
        # =====================================================================
        # Phase 0: Daemon Spawning & Auth Setup
        # =====================================================================
        print("\n" + "=" * 70)
        print(f"[Phase 0] Daemon Spawning & Auth Setup on port {test_port}")
        print("=" * 70)

        # Bootstrap admin token and substrate directory
        init_res = run_cli(["init", "--bootstrap", f"--data-dir={data_dir}"])
        admin_tok_file = os.path.join(data_dir, "admin.token")
        assert os.path.isfile(admin_tok_file), f"Expected admin.token at {admin_tok_file}"
        with open(admin_tok_file, "r") as f:
            admin_token = f.read().strip()
        assert admin_token.startswith("ab_adm_")
        print(f"[PASS] Bootstrap generated admin token: {admin_token[:16]}...")

        # Spawn live C++ daemon with --data-dir and --admin-token
        daemon_cmd = [
            str(DAEMON_BIN),
            "1",
            f"--data-dir={data_dir}",
            "--auth-mode=token",
            f"--admin-token={admin_token}",
            f"--port={test_port}"
        ]
        print(f"[DAEMON] Spawning: {' '.join(daemon_cmd)}")
        daemon_proc = subprocess.Popen(
            daemon_cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        # Wait for daemon readiness
        if not wait_for_daemon(base_url, timeout_secs=15.0):
            raise RuntimeError(f"Daemon failed to initialize on {base_url}")
        print(f"[PASS] Live daemon operational on {base_url} (pid: {daemon_proc.pid})")

        # Generate tokens for Stakeholder user and Agents
        print("\n--- Generating & Testing Identity Tokens ---")
        # 1. Stakeholder: user:jason
        u_res = run_cli([
            "user", "create", "jason",
            "--role", "curator",
            f"--connect={base_url}",
            f"--token={admin_token}",
            f"--data-dir={data_dir}"
        ])
        tok_m = re.search(r"ab_usr_[0-9a-fA-F]{32,64}", u_res.stdout)
        assert tok_m, f"User token not found in output: {u_res.stdout}"
        user_jason_token = tok_m.group(0)
        print(f"[PASS] Created Stakeholder user:jason ({user_jason_token[:16]}...)")

        # 2. Architect: agent:cpg-architect-01
        arch_res = run_cli([
            "agent", "create", "cpg-architect-01",
            "--user", "jason",
            "--project", "ctx-cpg-e2e",
            f"--connect={base_url}",
            f"--token={admin_token}",
            f"--data-dir={data_dir}"
        ])
        tok_m = re.search(r"ab_agt_[0-9a-fA-F]{32,64}", arch_res.stdout)
        assert tok_m, f"Agent token not found in output: {arch_res.stdout}"
        architect_token = tok_m.group(0)
        print(f"[PASS] Created Architect agent:cpg-architect-01 ({architect_token[:16]}...)")

        # 3. Worker 1: agent:cpg-worker-01
        w1_res = run_cli([
            "agent", "create", "cpg-worker-01",
            "--user", "jason",
            "--project", "ctx-cpg-e2e",
            f"--connect={base_url}",
            f"--token={admin_token}",
            f"--data-dir={data_dir}"
        ])
        tok_m = re.search(r"ab_agt_[0-9a-fA-F]{32,64}", w1_res.stdout)
        assert tok_m, f"Agent token not found in output: {w1_res.stdout}"
        worker1_token = tok_m.group(0)
        print(f"[PASS] Created Worker 1 agent:cpg-worker-01 ({worker1_token[:16]}...)")

        # 4. Worker 2: agent:cpg-worker-02
        w2_res = run_cli([
            "agent", "create", "cpg-worker-02",
            "--user", "jason",
            "--project", "ctx-cpg-e2e",
            f"--connect={base_url}",
            f"--token={admin_token}",
            f"--data-dir={data_dir}"
        ])
        tok_m = re.search(r"ab_agt_[0-9a-fA-F]{32,64}", w2_res.stdout)
        assert tok_m, f"Agent token not found in output: {w2_res.stdout}"
        worker2_token = tok_m.group(0)
        print(f"[PASS] Created Worker 2 agent:cpg-worker-02 ({worker2_token[:16]}...)")

        # 5. Verifier: agent:cpg-verifier-01
        v_res = run_cli([
            "agent", "create", "cpg-verifier-01",
            "--user", "jason",
            "--project", "ctx-cpg-e2e",
            f"--connect={base_url}",
            f"--token={admin_token}",
            f"--data-dir={data_dir}"
        ])
        tok_m = re.search(r"ab_agt_[0-9a-fA-F]{32,64}", v_res.stdout)
        assert tok_m, f"Agent token not found in output: {v_res.stdout}"
        verifier_token = tok_m.group(0)
        print(f"[PASS] Created Verifier agent:cpg-verifier-01 ({verifier_token[:16]}...)")

        # Validate token authentication with daemon
        user_status = run_cli(["status", f"--connect={base_url}", f"--token={user_jason_token}"])
        assert "OPERATIONAL" in user_status.stdout
        print("[PASS] Stakeholder user token successfully authenticated against live daemon.")

        # =====================================================================
        # Phase 1: Stakeholder Agent (Requirements & Context Initiation)
        # =====================================================================
        print("\n" + "=" * 70)
        print("[Phase 1] Stakeholder Agent (Requirements & Context Initiation)")
        print("=" * 70)

        context_id = "ctx-cpg-e2e"
        requirement_desc = "req-buffer-pool: Zero-copy dialectic buffer pool with bounded fragmentation"

        init_proc = run_cli([
            "swarm", "init",
            "--context", context_id,
            "--name", "CPG Buffer Swarm",
            "--requirement", requirement_desc,
            f"--connect={base_url}",
            f"--token={admin_token}"
        ])
        assert init_proc.returncode == 0
        assert context_id in init_proc.stdout

        # Assert context registered on daemon
        ctx_data = http_get_json(f"{base_url}/api/v1/context/{context_id}", admin_token)
        assert ctx_data.get("context_id") == context_id
        surfaces = ctx_data.get("active_surfaces", [])
        assert any(s.get("surface_id") == "swarm-coordinator" for s in surfaces)
        print(f"[PASS] Context '{context_id}' successfully registered with swarm-coordinator surface.")

        # Assert requirement knowledge atom exists
        req_node_id = f"req-{context_id}"
        req_data = http_get_json(f"{base_url}/api/v1/node/{req_node_id}", admin_token)
        assert req_data.get("id") == req_node_id
        req_content = json.loads(req_data.get("content", "{}"))
        assert req_content.get("status") == "PROPOSED"
        assert req_content.get("context_id") == context_id
        print(f"[PASS] Requirement atom '{req_node_id}' committed in PROPOSED state.")

        # =====================================================================
        # Phase 2: Architect Agent (Task DAG & Dependency Construction)
        # =====================================================================
        print("\n" + "=" * 70)
        print("[Phase 2] Architect Agent (Task DAG & Dependency Construction)")
        print("=" * 70)

        # 1. Create task-leaf-buffer (status: READY, -k 2, workflow: feature)
        t1_proc = run_cli([
            "swarm", "task", "create",
            "--context", context_id,
            "--name", "task-leaf-buffer",
            "--id", "task-leaf-buffer",
            "--workflow", "feature",
            "--symbols", "buf_pool_alloc,buf_pool_free",
            "-k", "2",
            "--agent", "cpg-architect-01",
            f"--connect={base_url}",
            f"--token={admin_token}"
        ])
        assert t1_proc.returncode == 0
        assert "task-leaf-buffer" in t1_proc.stdout

        # 2. Create task-consumer-mgr with --depends-on task-leaf-buffer
        t2_proc = run_cli([
            "swarm", "task", "create",
            "--context", context_id,
            "--name", "task-consumer-mgr",
            "--id", "task-consumer-mgr",
            "--workflow", "feature",
            "--symbols", "consumer_mgr_dispatch",
            "--depends-on", "task-leaf-buffer",
            "-k", "3",
            "--agent", "cpg-architect-01",
            f"--connect={base_url}",
            f"--token={admin_token}"
        ])
        assert t2_proc.returncode == 0
        assert "task-consumer-mgr" in t2_proc.stdout

        # Assert both tasks exist on daemon
        leaf_node = http_get_json(f"{base_url}/api/v1/node/task-leaf-buffer", admin_token)
        leaf_meta = json.loads(leaf_node.get("content", "{}"))
        assert leaf_meta.get("status") == "READY"
        assert leaf_meta.get("blast_radius_k") == 2
        assert leaf_meta.get("workflow") == "feature"

        consumer_node = http_get_json(f"{base_url}/api/v1/node/task-consumer-mgr", admin_token)
        consumer_meta = json.loads(consumer_node.get("content", "{}"))
        assert consumer_meta.get("status") == "READY"
        assert "task-leaf-buffer" in consumer_meta.get("depends_on", [])

        # Assert outbound DEPENDS_ON link connects consumer to leaf
        consumer_links = http_get_json(f"{base_url}/api/v1/node/task-consumer-mgr/links?direction=outbound", admin_token)
        outbound = consumer_links.get("outbound", [])
        dep_link = next((l for l in outbound if l.get("relation") == "DEPENDS_ON" and l.get("target") == "task-leaf-buffer"), None)
        assert dep_link is not None, f"Expected DEPENDS_ON link from consumer to leaf: {outbound}"
        print("[PASS] Task DAG created: task-consumer-mgr DEPENDS_ON task-leaf-buffer (k=2).")

        # =====================================================================
        # Phase 3: Worker 2 Premature Claim (Dependency Barrier)
        # =====================================================================
        print("\n" + "=" * 70)
        print("[Phase 3] Worker 2 Premature Claim (Dependency Barrier)")
        print("=" * 70)

        premature_proc = run_cli([
            "swarm", "lease", "claim", "task-consumer-mgr",
            "--agent", "cpg-worker-02",
            f"--connect={base_url}",
            f"--token={admin_token}"
        ], check=False)
        assert premature_proc.returncode != 0, "Expected premature lease claim to strictly fail"
        err_combined = (premature_proc.stderr + premature_proc.stdout).upper()
        assert "BLOCKED" in err_combined, f"Expected [BLOCKED] message in output: {premature_proc.stderr}"
        print(f"[PASS] Premature claim strictly rejected with exit code {premature_proc.returncode} and [BLOCKED] barrier.")

        # =====================================================================
        # Phase 4: Worker 1 Claim & Review Submission
        # =====================================================================
        print("\n" + "=" * 70)
        print("[Phase 4] Worker 1 Claim & Review Submission")
        print("=" * 70)

        # Worker 1 claims lease on task-leaf-buffer
        w1_claim_proc = run_cli([
            "swarm", "lease", "claim", "task-leaf-buffer",
            "--agent", "cpg-worker-01",
            "--ttl", "600",
            f"--connect={base_url}",
            f"--token={admin_token}"
        ])
        assert w1_claim_proc.returncode == 0

        # Assert status is IN_PROGRESS and lease holder is cpg-worker-01
        leaf_claimed = http_get_json(f"{base_url}/api/v1/node/task-leaf-buffer", admin_token)
        leaf_claimed_meta = json.loads(leaf_claimed.get("content", "{}"))
        assert leaf_claimed_meta.get("status") == "IN_PROGRESS"
        assert leaf_claimed_meta.get("lease", {}).get("holder") == "cpg-worker-01"
        print("[PASS] Worker 1 acquired exclusive lease: status=IN_PROGRESS, holder=cpg-worker-01.")

        # Worker 1 submits review with solution patch
        w1_sub_proc = run_cli([
            "swarm", "review", "submit", "task-leaf-buffer",
            "--agent", "cpg-worker-01",
            "--patch", "Implement zero-copy slab allocator for buffer pool with bounded freelists",
            f"--connect={base_url}",
            f"--token={admin_token}"
        ])
        assert w1_sub_proc.returncode == 0

        # Assert status is REVIEW_PENDING and HAS_SOLUTION link is attached
        leaf_pending = http_get_json(f"{base_url}/api/v1/node/task-leaf-buffer", admin_token)
        leaf_pending_meta = json.loads(leaf_pending.get("content", "{}"))
        assert leaf_pending_meta.get("status") == "REVIEW_PENDING"

        leaf_links = http_get_json(f"{base_url}/api/v1/node/task-leaf-buffer/links?direction=outbound", admin_token)
        sol_link = next((l for l in leaf_links.get("outbound", []) if l.get("relation") == "HAS_SOLUTION"), None)
        assert sol_link is not None, f"Expected HAS_SOLUTION link: {leaf_links}"
        print(f"[PASS] Review submitted: status=REVIEW_PENDING, HAS_SOLUTION -> {sol_link.get('target')}.")

        # =====================================================================
        # Phase 5: Verifier Agent Refutation & Auto-Increment
        # =====================================================================
        print("\n" + "=" * 70)
        print("[Phase 5] Verifier Agent Refutation & Auto-Increment")
        print("=" * 70)

        counterexample_trace = json.dumps({
            "error_path": "buf_pool_alloc -> out-of-memory -> leak in unmapped chunk header",
            "taint_sink": "buf_pool_free",
            "cpg_violation": "CWE-401"
        })
        fail_proc = run_cli([
            "swarm", "review", "verdict", "task-leaf-buffer",
            "--verifier", "cpg-verifier-01",
            "--verdict", "FAIL",
            "--details", counterexample_trace,
            f"--connect={base_url}",
            f"--token={admin_token}"
        ])
        assert fail_proc.returncode == 0

        # Assert status reverted to READY, lease cleared, REFUTES attached, refutation_count == 1
        leaf_failed = http_get_json(f"{base_url}/api/v1/node/task-leaf-buffer", admin_token)
        leaf_failed_meta = json.loads(leaf_failed.get("content", "{}"))
        assert leaf_failed_meta.get("status") == "READY"
        assert leaf_failed_meta.get("lease", {}).get("holder") is None
        assert leaf_failed_meta.get("refutation_count") == 1

        fail_links = http_get_json(f"{base_url}/api/v1/node/task-leaf-buffer/links?direction=outbound", admin_token)
        refute_link = next((l for l in fail_links.get("outbound", []) if l.get("relation") == "REFUTES"), None)
        assert refute_link is not None, f"Expected REFUTES link: {fail_links}"
        print(f"[PASS] Refutation recorded: status=READY, refutation_count=1, REFUTES -> {refute_link.get('target')}.")

        # =====================================================================
        # Phase 6: Worker 1 Fix & Verifier Validation
        # =====================================================================
        print("\n" + "=" * 70)
        print("[Phase 6] Worker 1 Fix & Verifier Validation")
        print("=" * 70)

        # Worker 1 re-claims lease on task-leaf-buffer
        run_cli([
            "swarm", "lease", "claim", "task-leaf-buffer",
            "--agent", "cpg-worker-01",
            f"--connect={base_url}",
            f"--token={admin_token}"
        ])

        # Worker 1 re-submits clean review
        run_cli([
            "swarm", "review", "submit", "task-leaf-buffer",
            "--agent", "cpg-worker-01",
            "--patch", "Fixed unmapped chunk leak with RAII memory guard in buf_pool_alloc",
            f"--connect={base_url}",
            f"--token={admin_token}"
        ])

        # Verifier records PASS verdict with formal proof
        proof_details = json.dumps({
            "symbolic_execution_paths": 18,
            "violations_detected": 0,
            "cpg_invariants_hold": True
        })
        pass_proc = run_cli([
            "swarm", "review", "verdict", "task-leaf-buffer",
            "--verifier", "cpg-verifier-01",
            "--verdict", "PASS",
            "--details", proof_details,
            f"--connect={base_url}",
            f"--token={admin_token}"
        ])
        assert pass_proc.returncode == 0

        # Assert status is VALIDATED, lease cleared, and VALIDATED_BY link attached
        leaf_val = http_get_json(f"{base_url}/api/v1/node/task-leaf-buffer", admin_token)
        leaf_val_meta = json.loads(leaf_val.get("content", "{}"))
        assert leaf_val_meta.get("status") == "VALIDATED"
        assert leaf_val_meta.get("lease", {}).get("holder") is None
        assert leaf_val_meta.get("lease", {}).get("expires_at") == 0

        val_links = http_get_json(f"{base_url}/api/v1/node/task-leaf-buffer/links?direction=outbound", admin_token)
        val_link = next((l for l in val_links.get("outbound", []) if l.get("relation") == "VALIDATED_BY"), None)
        assert val_link is not None, f"Expected VALIDATED_BY link: {val_links}"
        print(f"[PASS] Proof verified: status=VALIDATED, lease cleared, VALIDATED_BY -> {val_link.get('target')}.")

        # =====================================================================
        # Phase 7: Stakeholder Acceptance (Scope Completion)
        # =====================================================================
        print("\n" + "=" * 70)
        print("[Phase 7] Stakeholder Acceptance (Scope Completion)")
        print("=" * 70)

        # Stakeholder tries to claim lease -> must be rejected (task is already VALIDATED)
        stakeholder_claim_proc = run_cli([
            "swarm", "lease", "claim", "task-leaf-buffer",
            "--agent", "jason",
            f"--connect={base_url}",
            f"--token={admin_token}"
        ], check=False)
        assert stakeholder_claim_proc.returncode != 0
        assert "VALIDATED" in (stakeholder_claim_proc.stderr + stakeholder_claim_proc.stdout).upper()
        print("[PASS] Lease claim on already VALIDATED task strictly rejected.")

        # Stakeholder accepts task
        accept_proc = run_cli([
            "swarm", "accept", "task-leaf-buffer",
            "--stakeholder", "jason",
            "--notes", "Formally proved buffer pool satisfies zero-copy specification",
            f"--connect={base_url}",
            f"--token={admin_token}"
        ])
        assert accept_proc.returncode == 0

        # Assert status is COMPLETED and ACCEPTS link attached
        leaf_comp = http_get_json(f"{base_url}/api/v1/node/task-leaf-buffer", admin_token)
        leaf_comp_meta = json.loads(leaf_comp.get("content", "{}"))
        assert leaf_comp_meta.get("status") == "COMPLETED"

        comp_links = http_get_json(f"{base_url}/api/v1/node/task-leaf-buffer/links?direction=both", admin_token)
        all_links = comp_links.get("outbound", []) + comp_links.get("inbound", [])
        acc_link = next((l for l in all_links if l.get("relation") == "ACCEPTS"), None)
        assert acc_link is not None, f"Expected ACCEPTS link: {comp_links}"
        print(f"[PASS] Task accepted into baseline: status=COMPLETED, ACCEPTS link verified.")

        # =====================================================================
        # Phase 8: Worker 2 Unblocked Claim
        # =====================================================================
        print("\n" + "=" * 70)
        print("[Phase 8] Worker 2 Unblocked Claim")
        print("=" * 70)

        unblock_proc = run_cli([
            "swarm", "lease", "claim", "task-consumer-mgr",
            "--agent", "cpg-worker-02",
            "--ttl", "600",
            f"--connect={base_url}",
            f"--token={admin_token}"
        ])
        assert unblock_proc.returncode == 0

        consumer_claimed = http_get_json(f"{base_url}/api/v1/node/task-consumer-mgr", admin_token)
        consumer_claimed_meta = json.loads(consumer_claimed.get("content", "{}"))
        assert consumer_claimed_meta.get("status") == "IN_PROGRESS"
        assert consumer_claimed_meta.get("lease", {}).get("holder") == "cpg-worker-02"
        print("[PASS] Worker 2 claimed task-consumer-mgr now that prerequisite is COMPLETED.")

        # =====================================================================
        # Phase 9: FastMCP Integration Test against Live Daemon
        # =====================================================================
        mcp = ab_ctl.build_mcp_server(base_url, admin_token)
        assert mcp is not None and mcp != 1
        asyncio.run(run_fastmcp_suite(mcp, context_id))

        # =====================================================================
        # Phase 10: Clean Daemon Shutdown & Final Verification
        # =====================================================================
        print("\n" + "=" * 70)
        print("[Phase 10] Clean Daemon Shutdown & Assertion")
        print("=" * 70)

        daemon_proc.terminate()
        try:
            daemon_proc.wait(timeout=3)
        except Exception:
            daemon_proc.kill()
        daemon_proc = None
        print("[PASS] Live daemon process terminated cleanly.")

        print("\n" + "=" * 80)
        print("[PASS] Complete 4-Agent Dialectic Swarm Lifecycle Verified!")
        print("=" * 80)

    finally:
        if daemon_proc:
            print("[CLEANUP] Terminating residual daemon process...")
            daemon_proc.terminate()
            try:
                daemon_proc.wait(timeout=2)
            except Exception:
                daemon_proc.kill()
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
