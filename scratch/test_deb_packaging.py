#!/usr/bin/env python3
"""
Integration test for CPack DEB package generation and Debian package verification.
Validates:
1. Runs `cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCPACK_GENERATOR=DEB` and builds target `agentic-blackboardd`.
2. Runs `cpack -G DEB --config build/CPackConfig.cmake` and verifies it generates a `.deb` package matching `agentic-blackboard_0.4.0-1_*.deb` in `build/`.
3. Queries package info via `dpkg-deb -I <file>.deb`:
   - Package: agentic-blackboard
   - Version: 0.4.0-1 (or Version: 0.4.0)
   - Maintainer: Agentic Blackboard Team <team@agentic-blackboard.org>
   - Depends: contains systemd, python3, openssl (libssl3/libssl3t64), and resolved shared libraries (libc6, libzmq5, etc.).
4. Queries file list via `dpkg-deb -c <file>.deb`:
   - /usr/bin/agentic-blackboardd
   - /usr/bin/ab-ctl
   - /usr/lib/systemd/system/agentic-blackboard.service
   - /etc/agentic-blackboard/blackboard.conf
   - /usr/share/agentic-blackboard/skills/
   - /usr/include/agentic_blackboard
   - /usr/include/ab
   - Negative audit: no files under /usr/local
5. Extracts control archive via `dpkg-deb -e <file>.deb <temp_dir>` and checks:
   - postinst, prerm, and postrm exist and have executable permissions (0755).
   - postinst, prerm, and postrm pass POSIX syntax validation (`sh -n`).
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

WORKSPACE_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = WORKSPACE_ROOT / "build"


def run_command(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    print(f"[*] Running: {' '.join(cmd)}")
    res = subprocess.run(
        cmd,
        cwd=str(cwd or WORKSPACE_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if res.returncode != 0:
        print(f"[-] Command failed with exit code {res.returncode}")
        if res.stdout:
            print("--- STDOUT ---")
            print(res.stdout)
        if res.stderr:
            print("--- STDERR ---")
            print(res.stderr)
    return res


def main() -> int:
    print("=== Testing Debian Package Generation & Structure ===")
    failed = False

    # Clean up any existing .deb files in build/ to avoid false positives
    for deb_path in BUILD_DIR.glob("agentic-blackboard*.deb"):
        try:
            deb_path.unlink()
            print(f"[*] Cleaned existing package: {deb_path}")
        except Exception as e:
            print(f"[-] Warning: could not remove {deb_path}: {e}")

    # 1. CMake configure & build target
    print("\n--- 1. CMake Configure & Build ---")
    config_cmd = [
        "cmake",
        "-B",
        str(BUILD_DIR),
        "-S",
        str(WORKSPACE_ROOT),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCPACK_GENERATOR=DEB",
    ]
    res_config = run_command(config_cmd)
    if res_config.returncode != 0:
        print("[-] FAIL: CMake configuration failed")
        return 1
    print("[+] PASS: CMake configuration succeeded")

    build_cmd = [
        "cmake",
        "--build",
        str(BUILD_DIR),
        "--target",
        "agentic-blackboardd",
        "-j",
    ]
    res_build = run_command(build_cmd)
    if res_build.returncode != 0:
        print("[-] FAIL: Building target agentic-blackboardd failed")
        return 1
    print("[+] PASS: Built target agentic-blackboardd")

    # 2. CPack DEB generation
    print("\n--- 2. CPack DEB Package Generation ---")
    cpack_cmd = [
        "cpack",
        "-G",
        "DEB",
        "-B",
        str(BUILD_DIR),
        "--config",
        str(BUILD_DIR / "CPackConfig.cmake"),
    ]
    res_cpack = run_command(cpack_cmd, cwd=BUILD_DIR)
    if res_cpack.returncode != 0:
        print("[-] FAIL: CPack DEB generation failed")
        return 1

    deb_candidates = sorted(BUILD_DIR.glob("agentic-blackboard_0.4.0*.deb"))
    if not deb_candidates:
        print(f"[-] FAIL: No agentic-blackboard_0.4.0*.deb package found in {BUILD_DIR}")
        return 1

    deb_package = deb_candidates[0]
    print(f"[+] PASS: Generated Debian package: {deb_package.name}")

    # 3. Package Info / Control Metadata via dpkg-deb -I
    print("\n--- 3. Control Metadata Checks (dpkg-deb -I) ---")
    res_info = run_command(["dpkg-deb", "-I", str(deb_package)])
    if res_info.returncode != 0:
        print("[-] FAIL: dpkg-deb -I failed")
        return 1

    info_out = res_info.stdout

    # Check Package name
    if re.search(r"^ Package:\s*agentic-blackboard\b", info_out, re.MULTILINE):
        print("[+] PASS: Package is 'agentic-blackboard'")
    else:
        print("[-] FAIL: Package name 'agentic-blackboard' not found in control info")
        failed = True

    # Check Version
    if re.search(r"^ Version:\s*0\.4\.0(-1)?\b", info_out, re.MULTILINE):
        print("[+] PASS: Version is 0.4.0 / 0.4.0-1")
    else:
        print("[-] FAIL: Version '0.4.0' or '0.4.0-1' not found in control info")
        failed = True

    # Check Maintainer
    maintainer_pattern = r"^ Maintainer:\s*Agentic Blackboard Team <team@agentic-blackboard\.org>"
    if re.search(maintainer_pattern, info_out, re.MULTILINE):
        print("[+] PASS: Maintainer matches 'Agentic Blackboard Team <team@agentic-blackboard.org>'")
    else:
        print("[-] FAIL: Expected Maintainer not found in control info")
        failed = True

    # Check Depends
    depends_match = re.search(r"^ Depends:\s*(.+)$", info_out, re.MULTILINE)
    if not depends_match:
        print("[-] FAIL: 'Depends:' field not found in control info")
        failed = True
    else:
        depends_val = depends_match.group(1)
        # Handle multiline continuation if any
        for required_dep in ["systemd", "python3", "libc6", "libzmq5"]:
            if required_dep in depends_val or re.search(rf"\b{required_dep}\b", depends_val):
                print(f"[+] PASS: Dependency '{required_dep}' present in Depends")
            else:
                print(f"[-] FAIL: Required dependency '{required_dep}' missing from Depends: {depends_val}")
                failed = True

        # OpenSSL dependency check
        if re.search(r"libssl3(t64)?", depends_val):
            print("[+] PASS: OpenSSL dependency matching 'libssl3(t64)?' present in Depends")
        else:
            print(f"[-] FAIL: Required dependency matching 'libssl3(t64)?' missing from Depends: {depends_val}")
            failed = True

    # 4. Manifest / File List Checks via dpkg-deb -c
    print("\n--- 4. Manifest Checks (dpkg-deb -c) ---")
    res_contents = run_command(["dpkg-deb", "-c", str(deb_package)])
    if res_contents.returncode != 0:
        print("[-] FAIL: dpkg-deb -c failed")
        return 1

    installed_files = set()
    for line in res_contents.stdout.splitlines():
        parts = line.strip().split()
        if not parts:
            continue
        raw_path = parts[-1]
        # Normalize ./usr/bin/foo -> /usr/bin/foo
        norm_path = raw_path[1:] if raw_path.startswith(".") else raw_path
        installed_files.add(norm_path)
        # Also add without trailing slash if present
        if norm_path.endswith("/") and len(norm_path) > 1:
            installed_files.add(norm_path.rstrip("/"))

    # Hygiene / Negative audit: Assert that NO paths start with /usr/local
    usr_local_paths = [p for p in installed_files if p.startswith("/usr/local")]
    if usr_local_paths:
        print(f"[-] FAIL: Found {len(usr_local_paths)} paths starting with '/usr/local' in package manifest:")
        for p in usr_local_paths[:10]:
            print(f"    {p}")
        if len(usr_local_paths) > 10:
            print(f"    ... and {len(usr_local_paths) - 10} more")
        failed = True
    else:
        print("[+] PASS: Hygiene check: NO paths in package manifest start with '/usr/local'")

    required_entries = [
        "/usr/bin/agentic-blackboardd",
        "/usr/bin/ab-ctl",
        "/usr/lib/systemd/system/agentic-blackboard.service",
        "/etc/agentic-blackboard/blackboard.conf",
        "/usr/share/agentic-blackboard/skills",
        "/usr/include/agentic_blackboard",
        "/usr/include/ab",
    ]

    for entry in required_entries:
        if entry in installed_files:
            print(f"[+] PASS: Found file/dir '{entry}' in package")
        else:
            print(f"[-] FAIL: Missing required entry '{entry}' in package manifest")
            failed = True

    # 5. Maintainer Scripts in Control Archive via dpkg-deb -e
    print("\n--- 5. Maintainer Scripts in Control Archive (dpkg-deb -e) ---")
    with tempfile.TemporaryDirectory() as tmp_dir:
        res_extract = run_command(["dpkg-deb", "-e", str(deb_package), tmp_dir])
        if res_extract.returncode != 0:
            print("[-] FAIL: dpkg-deb -e failed to extract control archive")
            return 1

        extracted_dir = Path(tmp_dir)
        for script_name in ["postinst", "prerm", "postrm"]:
            script_path = extracted_dir / script_name
            if not script_path.is_file():
                print(f"[-] FAIL: Missing '{script_name}' in control archive")
                failed = True
            else:
                mode = script_path.stat().st_mode & 0o777
                if mode != 0o755:
                    print(f"[-] FAIL: '{script_name}' permissions are {oct(mode)}, expected 0o755")
                    failed = True
                else:
                    print(f"[+] PASS: '{script_name}' found in control archive with permissions 0755")

                # POSIX syntax check via sh -n
                res_syntax = run_command(["sh", "-n", str(script_path)])
                if res_syntax.returncode != 0:
                    print(f"[-] FAIL: '{script_name}' failed POSIX syntax check (sh -n): {res_syntax.stderr.strip()}")
                    failed = True
                else:
                    print(f"[+] PASS: '{script_name}' passed POSIX syntax check (sh -n)")

    if failed:
        print("\n[-] Verification failed: One or more package checks failed.")
        return 1

    print("\n[+] SUCCESS: Debian packaging test passed!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
