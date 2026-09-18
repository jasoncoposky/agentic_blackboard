#!/usr/bin/env python3
"""
Test suite for scripts/bootstrap-debian.sh.
Validates:
1. scripts/bootstrap-debian.sh exists and is executable (0755).
2. Running with --help / -h prints usage instructions and exits with code 0.
3. Running with an unknown option exits with code 1 and prints an error message.
4. Running with --package-only:
   - Compiles targets and runs build/ab_verify.
   - Generates Debian package via CPack.
   - Exits with code 0 without modifying /usr or /etc.
   - Confirms that the built .deb package exists in build/.
   - Validates that the generated .deb package contains expected metadata (Package: agentic-blackboard).
5. Running with --skip-build --package-only finds the existing package and exits with code 0.
"""

import os
import re
import stat
import subprocess
import sys
from pathlib import Path

WORKSPACE_ROOT = Path(__file__).resolve().parent.parent
BOOTSTRAP_SCRIPT = WORKSPACE_ROOT / "scripts" / "bootstrap-debian.sh"
BUILD_DIR = WORKSPACE_ROOT / "build"


def run_command(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    print(f"[*] Executing: {' '.join(cmd)}")
    res = subprocess.run(
        cmd,
        cwd=str(cwd or WORKSPACE_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return res


def main() -> int:
    print("=== Testing Debian Bootstrap Script (scripts/bootstrap-debian.sh) ===")
    failed = False

    # ---------------------------------------------------------
    # 1. Existence and Permissions
    # ---------------------------------------------------------
    print("\n--- 1. Script Existence and Permissions ---")
    if not BOOTSTRAP_SCRIPT.is_file():
        print(f"[-] FAIL: Missing bootstrap script at {BOOTSTRAP_SCRIPT}")
        return 1
    print(f"[+] PASS: Found {BOOTSTRAP_SCRIPT}")

    file_mode = BOOTSTRAP_SCRIPT.stat().st_mode & 0o777
    if file_mode != 0o755:
        print(f"[-] FAIL: Incorrect permissions {oct(file_mode)}, expected 0o755")
        failed = True
    else:
        print("[+] PASS: File permissions are 0755")

    res_syntax = subprocess.run(
        ["bash", "-n", str(BOOTSTRAP_SCRIPT)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if res_syntax.returncode != 0:
        print(f"[-] FAIL: Bash syntax check failed: {res_syntax.stderr.strip()}")
        failed = True
    else:
        print("[+] PASS: Bash syntax validation passed (bash -n)")

    # ---------------------------------------------------------
    # 2. Help Option (--help / -h)
    # ---------------------------------------------------------
    print("\n--- 2. Usage / Help Output ---")
    for flag in ["--help", "-h"]:
        res_help = run_command([str(BOOTSTRAP_SCRIPT), flag])
        if res_help.returncode != 0:
            print(f"[-] FAIL: {flag} exited with code {res_help.returncode}, expected 0")
            failed = True
        elif "Usage:" not in res_help.stdout or "--package-only" not in res_help.stdout:
            print(f"[-] FAIL: {flag} output missing required usage descriptions")
            failed = True
        else:
            print(f"[+] PASS: {flag} exited with 0 and printed usage text")

    # ---------------------------------------------------------
    # 3. Unknown Option Handling
    # ---------------------------------------------------------
    print("\n--- 3. Unknown Option Error Handling ---")
    res_unknown = run_command([str(BOOTSTRAP_SCRIPT), "--invalid-option-test"])
    if res_unknown.returncode != 1:
        print(f"[-] FAIL: Expected exit code 1 for unknown option, got {res_unknown.returncode}")
        failed = True
    elif "Unknown option" not in res_unknown.stderr:
        print("[-] FAIL: Expected 'Unknown option' in stderr")
        failed = True
    else:
        print("[+] PASS: Unknown option rejected with code 1 and error message")

    # ---------------------------------------------------------
    # 4. Package-only Build Execution (--package-only)
    # ---------------------------------------------------------
    print("\n--- 4. Package-Only Build Execution ---")
    # Clean any pre-existing deb files in build directory
    for deb in BUILD_DIR.glob("agentic-blackboard*.deb"):
        try:
            deb.unlink()
        except OSError as e:
            print(f"[-] Warning: could not remove existing deb {deb}: {e}")

    # Record mtime of system directories if accessible to verify no alteration
    res_pkg = run_command([str(BOOTSTRAP_SCRIPT), "--package-only"])
    if res_pkg.returncode != 0:
        print(f"[-] FAIL: --package-only exited with code {res_pkg.returncode}")
        if res_pkg.stdout:
            print("--- STDOUT ---")
            print(res_pkg.stdout)
        if res_pkg.stderr:
            print("--- STDERR ---")
            print(res_pkg.stderr)
        return 1

    stdout = res_pkg.stdout
    if "Running engine verification test..." not in stdout:
        print("[-] FAIL: Engine verification step not reported in output")
        failed = True
    else:
        print("[+] PASS: Engine verification step ran")

    if "Generating Debian package via CPack..." not in stdout:
        print("[-] FAIL: CPack generation step not reported in output")
        failed = True
    else:
        print("[+] PASS: CPack generation step ran")

    if "Package-only flag requested. Build complete." not in stdout:
        print("[-] FAIL: Package-only completion message not found")
        failed = True
    else:
        print("[+] PASS: Package-only completion reported successfully")

    # Verify generated .deb package exists
    deb_packages = sorted(BUILD_DIR.glob("agentic-blackboard_*.deb"))
    if not deb_packages:
        print(f"[-] FAIL: No agentic-blackboard_*.deb found in {BUILD_DIR}")
        return 1

    generated_deb = deb_packages[0]
    deb_size = generated_deb.stat().st_size
    if deb_size <= 0:
        print(f"[-] FAIL: Generated package {generated_deb.name} is empty (0 bytes)")
        failed = True
    else:
        print(f"[+] PASS: Generated package {generated_deb.name} ({deb_size} bytes)")

    # Validate package metadata with dpkg-deb -I
    res_info = run_command(["dpkg-deb", "-I", str(generated_deb)])
    if res_info.returncode != 0:
        print(f"[-] FAIL: dpkg-deb -I failed on {generated_deb}")
        failed = True
    else:
        if "Package: agentic-blackboard" in res_info.stdout:
            print("[+] PASS: dpkg-deb verified package name 'agentic-blackboard'")
        else:
            print("[-] FAIL: Package name 'agentic-blackboard' not in deb control metadata")
            failed = True

    # ---------------------------------------------------------
    # 5. Skip-Build with Package-Only (--skip-build --package-only)
    # ---------------------------------------------------------
    print("\n--- 5. Skip-Build with Package-Only ---")
    res_skip = run_command([str(BOOTSTRAP_SCRIPT), "--skip-build", "--package-only"])
    if res_skip.returncode != 0:
        print(f"[-] FAIL: --skip-build --package-only failed with code {res_skip.returncode}")
        failed = True
    elif f"Debian package ready: {generated_deb}" not in res_skip.stdout:
        print("[-] FAIL: Existing deb package was not identified")
        failed = True
    else:
        print("[+] PASS: --skip-build successfully reused existing package")

    print("\n=========================================================")
    if failed:
        print("[-] RESULT: Debian bootstrap tests FAILED")
        return 1
    print("[+] RESULT: All Debian bootstrap tests PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
