#!/usr/bin/env python3
"""
Verification test for Task 4: ab-ctl CLI Implementation
Tests admin token generation, user/agent creation, surface registration,
context inspection/focus, and integrated MCP runner with --smoke-test.
"""

import hashlib
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
AB_CTL_PY = REPO_ROOT / "src" / "ab-ctl.py"
DAEMON_BIN = REPO_ROOT / "build" / "agentic-blackboardd"
TEST_PORT = int(os.environ.get("AB_TEST_PORT", 18088))
BASE_URL = f"http://127.0.0.1:{TEST_PORT}"


def run_cli(args, env=None, check=True):
    """Run ab-ctl CLI command."""
    if not AB_CTL_PY.is_file():
        raise FileNotFoundError(f"CLI script not found: {AB_CTL_PY}")

    full_cmd = [sys.executable, str(AB_CTL_PY)] + args
    print(f"[CLI CMD] {' '.join(full_cmd)}")
    cmd_env = os.environ.copy()
    if env:
        cmd_env.update(env)
    proc = subprocess.run(
        full_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=cmd_env
    )
    if check and proc.returncode != 0:
        print(f"[CLI ERROR STDOUT]\n{proc.stdout}")
        print(f"[CLI ERROR STDERR]\n{proc.stderr}")
        raise RuntimeError(f"Command failed with code {proc.returncode}: {' '.join(full_cmd)}")
    return proc


def wait_for_server(url, timeout_secs=10):
    """Poll endpoint until server is ready."""
    start = time.time()
    while time.time() - start < timeout_secs:
        try:
            req = urllib.request.Request(f"{url}/api/v1/schema")
            with urllib.request.urlopen(req, timeout=1.0) as resp:
                if resp.status == 200:
                    return True
        except Exception:
            time.sleep(0.2)
    return False


def main():
    print("=== Starting ab-ctl Verification Test Suite ===")

    # Ensure ab-ctl.py exists
    if not AB_CTL_PY.is_file():
        print(f"[FAIL] {AB_CTL_PY} does not exist.")
        sys.exit(1)

    temp_dir = tempfile.mkdtemp(prefix="ab_ctl_test_")
    daemon_proc = None

    try:
        data_dir = os.path.join(temp_dir, "substrate")
        db_path = os.path.join(temp_dir, "db")

        # -------------------------------------------------------------
        # Step 1: ab-ctl init --bootstrap --data-dir=<dir>
        # -------------------------------------------------------------
        print("\n--- Step 1: ab-ctl init ---")
        proc = run_cli(["init", "--bootstrap", f"--data-dir={data_dir}"])
        assert proc.returncode == 0, "ab-ctl init failed"
        assert os.path.isdir(data_dir), f"Directory {data_dir} was not created"

        # Check credentials.db created
        creds_db = os.path.join(data_dir, "credentials.db")
        assert os.path.isfile(creds_db), f"Credentials DB {creds_db} not found"

        # Assert admin token output or written
        admin_token_match = re.search(r"ab_adm_[0-9a-fA-F]{32,64}", proc.stdout)
        token_file = os.path.join(data_dir, "admin.token")
        if not admin_token_match and os.path.isfile(token_file):
            with open(token_file, "r") as f:
                admin_token = f.read().strip()
        elif admin_token_match:
            admin_token = admin_token_match.group(0)
        else:
            raise AssertionError(f"Admin token not found in output or token file: {proc.stdout}")

        assert admin_token.startswith("ab_adm_"), f"Unexpected admin token format: {admin_token}"
        print(f"[PASS] Step 1: init created substrate and admin token: {admin_token[:15]}...")

        # Verify credentials.db contains salted hash
        salt = "ab_salt_token_v1:"
        expected_hash = hashlib.sha256((salt + admin_token).encode("utf-8")).hexdigest()
        conn = sqlite3.connect(creds_db)
        cursor = conn.cursor()
        cursor.execute("SELECT token_hash, username, role FROM tokens WHERE token_hash=?", (expected_hash,))
        row = cursor.fetchone()
        assert row is not None, f"Expected token hash {expected_hash} not found in credentials.db"
        assert row[1] == "admin" and row[2] == "admin", f"Unexpected row: {row}"
        conn.close()
        print("[PASS] Step 1: credentials.db verified with salted SHA-256 hash.")

        # -------------------------------------------------------------
        # Step 2: Spawn daemon or test server with generated admin token
        # -------------------------------------------------------------
        print("\n--- Step 2: Spawning Agentic Blackboard daemon ---")
        daemon_cmd = [
            str(DAEMON_BIN),
            "1",
            f"--db={db_path}",
            "--auth-mode=token",
            f"--admin-token={admin_token}",
            f"--port={TEST_PORT}"
        ]
        print(f"[DAEMON CMD] {' '.join(daemon_cmd)}")
        daemon_proc = subprocess.Popen(
            daemon_cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )

        ready = wait_for_server(BASE_URL, timeout_secs=10)
        if not ready:
            raise RuntimeError(f"Daemon failed to start on {BASE_URL}")
        print(f"[PASS] Step 2: Agentic Blackboard daemon started (pid: {daemon_proc.pid}) on {BASE_URL}")

        # Test status subcommand
        status_proc = run_cli(["status", f"--connect={BASE_URL}", f"--token={admin_token}"])
        assert status_proc.returncode == 0
        print(f"[PASS] Step 2b: ab-ctl status succeeded: {status_proc.stdout.strip()[:80]}...")

        # -------------------------------------------------------------
        # Step 3: ab-ctl user create testuser --role curator
        # -------------------------------------------------------------
        print("\n--- Step 3: ab-ctl user create ---")
        user_proc = run_cli([
            "user", "create", "testuser",
            "--role", "curator",
            f"--token={admin_token}",
            f"--connect={BASE_URL}",
            f"--data-dir={data_dir}"
        ])
        assert user_proc.returncode == 0
        user_token_match = re.search(r"ab_usr_[0-9a-fA-F]{32,64}", user_proc.stdout)
        assert user_token_match is not None, f"User token not found in output: {user_proc.stdout}"
        user_token = user_token_match.group(0)
        print(f"[PASS] Step 3: user create succeeded with token: {user_token[:15]}...")

        # Verify user list subcommand with --data-dir
        user_list_proc = run_cli([
            "user", "list",
            f"--token={admin_token}",
            f"--connect={BASE_URL}",
            f"--data-dir={data_dir}"
        ])
        assert user_list_proc.returncode == 0
        assert "testuser" in user_list_proc.stdout
        print("[PASS] Step 3b: user list with --data-dir displays created user.")

        # Verify user list subcommand WITHOUT --data-dir (queries daemon API)
        user_list_api_proc = run_cli([
            "user", "list",
            f"--token={admin_token}",
            f"--connect={BASE_URL}"
        ])
        assert user_list_api_proc.returncode == 0
        assert "testuser" in user_list_api_proc.stdout, f"User list via API missing testuser: {user_list_api_proc.stdout}"
        print("[PASS] Step 3c: user list without --data-dir queries daemon API and displays created user.")

        # Verify strict error handling on unauthorized / failed user create
        failed_proc = run_cli([
            "user", "create", "unauthorized_user",
            "--role", "curator",
            "--token=ab_usr_invalid_token_1234567890",
            f"--connect={BASE_URL}"
        ], check=False)
        assert failed_proc.returncode != 0, "Expected non-zero exit code when server rejects user create"
        print("[PASS] Step 3d: user create with invalid token strictly fails with non-zero exit code.")

        # -------------------------------------------------------------
        # Step 4: ab-ctl agent create testagent --user testuser --project proj-test
        # -------------------------------------------------------------
        print("\n--- Step 4: ab-ctl agent create ---")
        agent_proc = run_cli([
            "agent", "create", "testagent",
            "--user", "testuser",
            "--project", "proj-test",
            f"--token={admin_token}",
            f"--connect={BASE_URL}",
            f"--data-dir={data_dir}"
        ])
        assert agent_proc.returncode == 0
        agent_token_match = re.search(r"ab_agt_[0-9a-fA-F]{32,64}", agent_proc.stdout)
        assert agent_token_match is not None, f"Agent token not found in output: {agent_proc.stdout}"
        agent_token = agent_token_match.group(0)
        print(f"[PASS] Step 4: agent create succeeded with token: {agent_token[:15]}...")

        # -------------------------------------------------------------
        # Step 5: ab-ctl surface register --name table-1 --type tabletop --context ctx-test
        # -------------------------------------------------------------
        print("\n--- Step 5: ab-ctl surface register ---")
        surf_proc = run_cli([
            "surface", "register",
            "--name", "table-1",
            "--type", "tabletop",
            "--context", "ctx-test",
            f"--token={user_token}",
            f"--connect={BASE_URL}"
        ])
        assert surf_proc.returncode == 0
        assert "REGISTERED" in surf_proc.stdout.upper() or "200" in surf_proc.stdout
        print(f"[PASS] Step 5: surface register returned 200 REGISTERED")

        # -------------------------------------------------------------
        # Step 6: ab-ctl context show ctx-test
        # -------------------------------------------------------------
        print("\n--- Step 6: ab-ctl context show ---")
        ctx_proc = run_cli([
            "context", "show", "ctx-test",
            f"--token={user_token}",
            f"--connect={BASE_URL}"
        ])
        assert ctx_proc.returncode == 0
        assert "table-1" in ctx_proc.stdout, f"Surface 'table-1' not present in context: {ctx_proc.stdout}"
        print(f"[PASS] Step 6: context show confirms surface presence: {ctx_proc.stdout.strip()}")

        # Test context focus subcommand
        focus_proc = run_cli([
            "context", "focus", "ctx-test",
            "--selected", "atom-alpha", "atom-beta",
            "--surface-id", "table-1",
            f"--token={user_token}",
            f"--connect={BASE_URL}"
        ])
        assert focus_proc.returncode == 0
        print("[PASS] Step 6b: context focus succeeded.")

        # -------------------------------------------------------------
        # Step 7: ab-ctl mcp run --connect <url> --token <token> --smoke-test
        # -------------------------------------------------------------
        print("\n--- Step 7: ab-ctl mcp run --smoke-test ---")
        mcp_proc = run_cli([
            "mcp", "run",
            f"--connect={BASE_URL}",
            f"--token={user_token}",
            "--smoke-test"
        ])
        assert mcp_proc.returncode == 0, f"mcp run --smoke-test exited with {mcp_proc.returncode}"
        print("[PASS] Step 7: mcp run --smoke-test passed with exit code 0.")

        # -------------------------------------------------------------
        # Step 8: Test --token-file support with status and swarm
        # -------------------------------------------------------------
        print("\n--- Step 8: ab-ctl --token-file support ---")
        status_tf_proc = run_cli(["status", f"--connect={BASE_URL}", f"--token-file={token_file}"])
        assert status_tf_proc.returncode == 0, f"status --token-file failed: {status_tf_proc.stderr}"
        print("[PASS] Step 8a: status with --token-file succeeded.")

        status_tf_global = run_cli([f"--token-file={token_file}", "status", f"--connect={BASE_URL}"])
        assert status_tf_global.returncode == 0, f"--token-file status failed: {status_tf_global.stderr}"
        print("[PASS] Step 8b: global --token-file before subcommand succeeded.")

        swarm_init_proc = run_cli([
            "swarm", "init",
            "--context", "ctx-token-file-test",
            "--name", "Token File Swarm",
            f"--connect={BASE_URL}",
            f"--token-file={token_file}"
        ])
        assert swarm_init_proc.returncode == 0, f"swarm init --token-file failed: {swarm_init_proc.stderr}"
        print("[PASS] Step 8c: swarm init with --token-file succeeded.")

        swarm_tasks_proc = run_cli([
            "swarm", "task", "list",
            "--context", "ctx-token-file-test",
            f"--connect={BASE_URL}",
            f"--token-file={token_file}"
        ])
        assert swarm_tasks_proc.returncode == 0, f"swarm task list --token-file failed: {swarm_tasks_proc.stderr}"
        print("[PASS] Step 8d: swarm task list with --token-file succeeded.")

        print("\n=== All ab-ctl CLI Tests Passed Successfully! ===")

    finally:
        if daemon_proc:
            print("[CLEANUP] Terminating daemon process...")
            daemon_proc.terminate()
            try:
                daemon_proc.wait(timeout=3)
            except Exception:
                daemon_proc.kill()
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
