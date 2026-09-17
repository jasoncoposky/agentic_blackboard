#!/usr/bin/env python3
"""
End-to-End Central Deployment & Canonical Containerization Verification Test.
Validates:
1. RPM packaging installation and file verification in container.
2. Systemd service syntax check with `systemd-analyze verify` in container.
3. Container build from `Dockerfile` using `docker build -t agentic-blackboard:latest .`.
4. Container startup with mounted volumes `/var/lib/agentic-blackboard` and `/etc/agentic-blackboard`, mapped port (e.g. -p 18095:8085).
5. Bootstrap token initialization on first boot (reading token from mounted volume).
6. Execution of CLI operations against container daemon:
   - ab-ctl status --connect http://localhost:18095 --token <token>
   - ab-ctl user create container-user --role curator --connect http://localhost:18095 --token <token>
   - ab-ctl surface register --name table-c1 --type tabletop --context ctx:container --connect http://localhost:18095 --token <user_token>
   - ab-ctl context focus ctx:container --selected note-1 --connect http://localhost:18095 --token <user_token>
   - ab-ctl mcp run --connect http://localhost:18095 --token <user_token> --smoke-test
7. Clean container shutdown (docker stop / docker rm).
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

WORKSPACE_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = WORKSPACE_ROOT / "build"
DOCKERFILE_PATH = WORKSPACE_ROOT / "Dockerfile"
AB_CTL_PY = WORKSPACE_ROOT / "src" / "ab-ctl.py"

CONTAINER_IMAGE = "agentic-blackboard:latest"
CONTAINER_PORT = int(os.environ.get("ASOS_CONTAINER_PORT", "18095"))
BASE_URL = f"http://localhost:{CONTAINER_PORT}"
CONTAINER_NAME = f"ab-e2e-test-{os.getpid()}"


def log_step(title: str):
    print(f"\n{'='*70}\n[STEP] {title}\n{'='*70}")


def run_cmd(cmd, check=True, capture=True, cwd=None, text=True, env=None):
    cmd_str = " ".join(str(c) for c in cmd)
    print(f"[*] Executing: {cmd_str}")
    proc = subprocess.run(
        cmd,
        cwd=cwd,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=text,
        env=env,
    )
    if check and proc.returncode != 0:
        if capture:
            print(f"[-] ERROR STDOUT:\n{proc.stdout}")
            print(f"[-] ERROR STDERR:\n{proc.stderr}")
        raise RuntimeError(f"Command failed with code {proc.returncode}: {cmd_str}")
    return proc


def find_rpm_package() -> Path:
    rpm_path = BUILD_DIR / "agentic-blackboard-0.4.0-1.el9.x86_64.rpm"
    if rpm_path.is_file():
        return rpm_path
    rpms = list(BUILD_DIR.glob("**/*.rpm"))
    if rpms:
        return rpms[0]
    raise FileNotFoundError(f"RPM package not found in {BUILD_DIR}")


def step1_verify_rpm_installation() -> bool:
    log_step("1. RPM Packaging Installation & File Verification in UBI 9 Minimal Container")
    rpm_file = find_rpm_package()
    print(f"[+] Using RPM: {rpm_file}")

    test_cmd = [
        "docker", "run", "--rm",
        "-v", f"{WORKSPACE_ROOT}:/workspace:ro",
        "registry.access.redhat.com/ubi9/ubi-minimal:latest",
        "bash", "-c",
        f"""set -e
rpm -Uvh https://dl.fedoraproject.org/pub/epel/epel-release-latest-9.noarch.rpm >/dev/null 2>&1
microdnf install -y shadow-utils systemd util-linux zeromq openssl python3 >/dev/null 2>&1
rpm -ivh /workspace/build/{rpm_file.name}

# Verify installed files
[ -x /usr/bin/agentic-blackboardd ]
[ -x /usr/bin/ab-ctl ]
[ -f /usr/lib/systemd/system/agentic-blackboard.service ]
[ -f /etc/agentic-blackboard/blackboard.conf ]
[ -f /etc/agentic-blackboard/blackboard.conf.default ]
[ -f /etc/security/limits.d/99-blackboard.conf ]
[ -d /usr/share/agentic-blackboard/skills ]
[ -d /var/lib/agentic-blackboard ]
[ -d /var/log/agentic-blackboard ]

id blackboard >/dev/null 2>&1
echo "RPM verification passed"
"""
    ]

    proc = run_cmd(test_cmd)
    if "RPM verification passed" not in proc.stdout:
        print("[-] FAIL: Container verification script did not complete successfully.")
        return False

    print("[+] PASS: RPM package installs cleanly and all files/directories verified.")
    return True


def step2_verify_systemd_service_syntax() -> bool:
    log_step("2. Systemd Service Syntax Validation via systemd-analyze verify")
    rpm_file = find_rpm_package()

    test_cmd = [
        "docker", "run", "--rm",
        "-v", f"{WORKSPACE_ROOT}:/workspace:ro",
        "registry.access.redhat.com/ubi9/ubi-minimal:latest",
        "bash", "-c",
        f"""set -e
microdnf install -y shadow-utils systemd python3 >/dev/null 2>&1
rpm -ivh /workspace/build/{rpm_file.name} >/dev/null 2>&1
systemd-analyze verify /usr/lib/systemd/system/agentic-blackboard.service
echo "SYSTEMD_VERIFY_OK"
"""
    ]

    proc = run_cmd(test_cmd)
    if "SYSTEMD_VERIFY_OK" not in proc.stdout:
        print(f"[-] FAIL: systemd-analyze verify failed:\n{proc.stderr}\n{proc.stdout}")
        return False

    print("[+] PASS: systemd-analyze verify passed with exit code 0.")
    return True


def step3_build_container_image() -> bool:
    log_step("3. Container Build from Dockerfile (docker build -t agentic-blackboard:latest .)")
    if not DOCKERFILE_PATH.is_file():
        print(f"[-] FAIL: Dockerfile missing at: {DOCKERFILE_PATH}")
        return False

    build_cmd = [
        "docker", "build",
        "-t", CONTAINER_IMAGE,
        "."
    ]
    proc = run_cmd(build_cmd, cwd=WORKSPACE_ROOT)
    if proc.returncode != 0:
        print(f"[-] FAIL: docker build failed:\n{proc.stderr}")
        return False

    print(f"[+] PASS: Container image {CONTAINER_IMAGE} built successfully.")
    return True


def wait_for_daemon(url: str, timeout_secs: float = 20.0) -> bool:
    start = time.time()
    while time.time() - start < timeout_secs:
        try:
            req = urllib.request.Request(f"{url}/api/v1/schema")
            with urllib.request.urlopen(req, timeout=1.5) as resp:
                if resp.status == 200:
                    return True
        except Exception:
            time.sleep(0.3)
    return False


def run_ab_ctl(args: list[str], check: bool = True) -> subprocess.CompletedProcess:
    full_cmd = [sys.executable, str(AB_CTL_PY)] + args
    return run_cmd(full_cmd, check=check)


def step4_to_7_container_lifecycle_and_e2e() -> bool:
    log_step("4-7. Container Startup, Bootstrap Token Init, CLI Operations & Shutdown")

    host_data_dir = Path(tempfile.mkdtemp(prefix="bb_e2e_data_"))
    host_conf_dir = Path(tempfile.mkdtemp(prefix="bb_e2e_conf_"))
    host_data_dir.chmod(0o777)
    host_conf_dir.chmod(0o777)

    # Pre-copy blackboard.conf into conf directory with open permissions
    default_conf = WORKSPACE_ROOT / "packaging" / "config" / "blackboard.conf"
    if default_conf.is_file():
        dest_conf = host_conf_dir / "blackboard.conf"
        shutil.copyfile(default_conf, dest_conf)
        dest_conf.chmod(0o666)

    container_started = False

    try:
        # Step 4: Container Startup with mounted volumes and mapped port
        print(f"[*] Starting container '{CONTAINER_NAME}' on port {CONTAINER_PORT}...")
        run_container_cmd = [
            "docker", "run", "-d",
            "--name", CONTAINER_NAME,
            "-p", f"{CONTAINER_PORT}:8085",
            "-v", f"{host_data_dir}:/var/lib/agentic-blackboard:rw",
            "-v", f"{host_conf_dir}:/etc/agentic-blackboard:rw",
            CONTAINER_IMAGE
        ]
        run_cmd(run_container_cmd)
        container_started = True

        # Wait for daemon readiness
        print(f"[*] Waiting for daemon on {BASE_URL}...")
        if not wait_for_daemon(BASE_URL, timeout_secs=25.0):
            # Print logs for diagnostics
            log_proc = subprocess.run(["docker", "logs", CONTAINER_NAME], capture_output=True, text=True)
            print(f"[-] Container logs:\n{log_proc.stdout}\n{log_proc.stderr}")
            raise RuntimeError("Daemon failed to become healthy within timeout.")

        print(f"[+] PASS: Container started and daemon is healthy at {BASE_URL}.")

        # Step 5: Bootstrap token initialization on first boot
        print("[*] Checking bootstrap token in mounted volume or container...")
        admin_token = None
        for candidate in [
            host_conf_dir / "admin.token",
            host_data_dir / "admin.token",
        ]:
            if candidate.is_file():
                try:
                    tok = candidate.read_text(encoding="utf-8").strip()
                    if tok.startswith("ab_adm_"):
                        admin_token = tok
                        print(f"[+] PASS: Found bootstrap admin token in mounted volume {candidate}: {tok[:16]}...")
                        break
                except PermissionError:
                    pass

        if not admin_token:
            # Check via docker exec
            exec_res = subprocess.run(
                ["docker", "exec", CONTAINER_NAME, "cat", "/var/lib/agentic-blackboard/admin.token"],
                capture_output=True, text=True
            )
            if exec_res.returncode == 0 and exec_res.stdout.strip().startswith("ab_adm_"):
                admin_token = exec_res.stdout.strip()
                print(f"[+] PASS: Found bootstrap admin token via container filesystem: {admin_token[:16]}...")

        if not admin_token:
            raise AssertionError("Bootstrap admin token was not initialized on first boot.")

        # Step 6: CLI Operations
        print("\n[*] Executing CLI verification operations against containerized daemon...")

        # 6a. ab-ctl status --connect http://localhost:18095 --token <token>
        print("\n--- 6a. ab-ctl status ---")
        status_res = run_ab_ctl(["status", f"--connect={BASE_URL}", f"--token={admin_token}"])
        assert status_res.returncode == 0
        assert "OPERATIONAL" in status_res.stdout
        print(f"[+] PASS: Status returned OPERATIONAL: {status_res.stdout.strip()[:80]}")

        # 6b. ab-ctl user create container-user --role curator --connect http://localhost:18095 --token <token>
        print("\n--- 6b. ab-ctl user create ---")
        user_create_res = run_ab_ctl([
            "user", "create", "container-user",
            "--role", "curator",
            f"--connect={BASE_URL}",
            f"--token={admin_token}"
        ])
        assert user_create_res.returncode == 0
        user_tok_match = re.search(r"ab_usr_[0-9a-fA-F]{32,64}", user_create_res.stdout)
        if not user_tok_match:
            raise AssertionError(f"User token not found in output: {user_create_res.stdout}")
        user_token = user_tok_match.group(0)
        print(f"[+] PASS: Created user 'container-user' with token: {user_token[:16]}...")

        # 6c. ab-ctl surface register --name table-c1 --type tabletop --context ctx:container --connect http://localhost:18095 --token <user_token>
        print("\n--- 6c. ab-ctl surface register ---")
        surf_res = run_ab_ctl([
            "surface", "register",
            "--name", "table-c1",
            "--type", "tabletop",
            "--context", "ctx:container",
            f"--connect={BASE_URL}",
            f"--token={user_token}"
        ])
        assert surf_res.returncode == 0
        print(f"[+] PASS: Surface 'table-c1' registered successfully: {surf_res.stdout.strip()}")

        # 6d. ab-ctl context focus ctx:container --selected note-1 --connect http://localhost:18095 --token <user_token>
        print("\n--- 6d. ab-ctl context focus ---")
        focus_res = run_ab_ctl([
            "context", "focus", "ctx:container",
            "--selected", "note-1",
            f"--connect={BASE_URL}",
            f"--token={user_token}"
        ])
        assert focus_res.returncode == 0
        print(f"[+] PASS: Context focus updated successfully: {focus_res.stdout.strip()}")

        # 6e. ab-ctl mcp run --connect http://localhost:18095 --token <user_token> --smoke-test
        print("\n--- 6e. ab-ctl mcp run --smoke-test ---")
        mcp_res = run_ab_ctl([
            "mcp", "run",
            f"--connect={BASE_URL}",
            f"--token={user_token}",
            "--smoke-test"
        ])
        assert mcp_res.returncode == 0
        assert "Smoke test passed successfully" in mcp_res.stdout
        print(f"[+] PASS: MCP smoke test passed: {mcp_res.stdout.strip()[:100]}...")

        return True

    finally:
        # Step 7: Clean container shutdown (docker stop / docker rm)
        if container_started:
            log_step("7. Clean Container Shutdown (docker stop / docker rm)")
            print(f"[*] Stopping container {CONTAINER_NAME}...")
            subprocess.run(["docker", "stop", CONTAINER_NAME], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            print(f"[*] Removing container {CONTAINER_NAME}...")
            subprocess.run(["docker", "rm", "-f", CONTAINER_NAME], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            print("[+] PASS: Container stopped and removed cleanly.")

        shutil.rmtree(host_data_dir, ignore_errors=True)
        shutil.rmtree(host_conf_dir, ignore_errors=True)


def main():
    print("=== ASOS Task 6: Canonical Containerization & E2E Deployment Verification ===")
    
    # 1. RPM packaging installation and file verification in container
    if not step1_verify_rpm_installation():
        sys.exit(1)

    # 2. Systemd service syntax check with systemd-analyze verify
    if not step2_verify_systemd_service_syntax():
        sys.exit(1)

    # 3. Container build from Dockerfile
    if not step3_build_container_image():
        sys.exit(1)

    # 4-7. Container startup, token init, CLI verification, shutdown
    if not step4_to_7_container_lifecycle_and_e2e():
        sys.exit(1)

    print("\n=========================================================================")
    print(">>> ALL CHECKS PASSED: Canonical Containerization & E2E Verified! <<<")
    print("=========================================================================")
    sys.exit(0)


if __name__ == "__main__":
    main()
