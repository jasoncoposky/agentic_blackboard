#!/usr/bin/env python3
"""
Verification script for Debian maintainer scripts (postinst, prerm, postrm).
Validates:
1. All three files exist (packaging/debian/postinst, prerm, postrm).
2. All three files have executable permissions (0755).
3. All three files pass syntax checking (sh -n <file>).
4. postinst contains addgroup / groupadd, adduser / useradd, chown -R blackboard:blackboard, ab-ctl init --bootstrap, and permission modes (0750, 0640).
5. prerm stops agentic-blackboard.service.
6. postrm reloads systemd on remove/purge.
7. All three scripts exit with status 1 on unknown argument.
"""

import os
import stat
import subprocess
import sys
from pathlib import Path

WORKSPACE_ROOT = Path(__file__).resolve().parent.parent
DEBIAN_DIR = WORKSPACE_ROOT / "packaging" / "debian"


def main() -> int:
    print("=== Auditing Debian Maintainer Scripts ===")
    failed = False

    scripts = {
        "postinst": DEBIAN_DIR / "postinst",
        "prerm": DEBIAN_DIR / "prerm",
        "postrm": DEBIAN_DIR / "postrm",
    }

    # 1. Existence check
    print("\n--- 1. File Existence ---")
    for name, path in scripts.items():
        if not path.is_file():
            print(f"[-] FAIL: Missing {name} at {path}")
            failed = True
        else:
            print(f"[+] PASS: Found {name} at {path}")

    if failed:
        print("\n[-] Pre-condition failed: Missing maintainer script files.")
        return 1

    # 2. Executable permission check (0755)
    print("\n--- 2. Executable Permissions (0755) ---")
    for name, path in scripts.items():
        mode = path.stat().st_mode & 0o777
        if mode != 0o755:
            print(f"[-] FAIL: {name} permissions are {oct(mode)}, expected 0o755")
            failed = True
        else:
            print(f"[+] PASS: {name} has permissions 0755")

    # 3. Shell syntax check (sh -n)
    print("\n--- 3. POSIX Shell Syntax (sh -n) ---")
    for name, path in scripts.items():
        res = subprocess.run(
            ["sh", "-n", str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if res.returncode != 0:
            print(f"[-] FAIL: {name} failed syntax check: {res.stderr.strip()}")
            failed = True
        else:
            print(f"[+] PASS: {name} passed 'sh -n' syntax check")

    # 4. Content checks: postinst
    print("\n--- 4. postinst Content Checks ---")
    postinst_content = scripts["postinst"].read_text()
    if ("addgroup" not in postinst_content) and ("groupadd" not in postinst_content):
        print("[-] FAIL: postinst missing addgroup / groupadd")
        failed = True
    else:
        print("[+] PASS: postinst contains addgroup / groupadd")

    if ("adduser" not in postinst_content) and ("useradd" not in postinst_content):
        print("[-] FAIL: postinst missing adduser / useradd")
        failed = True
    else:
        print("[+] PASS: postinst contains adduser / useradd")

    if "chown -R blackboard:blackboard" not in postinst_content:
        print("[-] FAIL: postinst missing 'chown -R blackboard:blackboard'")
        failed = True
    else:
        print("[+] PASS: postinst contains 'chown -R blackboard:blackboard'")

    if "ab-ctl init --bootstrap" not in postinst_content:
        print("[-] FAIL: postinst missing 'ab-ctl init --bootstrap'")
        failed = True
    else:
        print("[+] PASS: postinst contains 'ab-ctl init --bootstrap'")

    if "0750" not in postinst_content:
        print("[-] FAIL: postinst missing '0750'")
        failed = True
    else:
        print("[+] PASS: postinst contains '0750'")

    if "0640" not in postinst_content:
        print("[-] FAIL: postinst missing '0640'")
        failed = True
    else:
        print("[+] PASS: postinst contains '0640'")

    if "chown blackboard:blackboard /var/lib/agentic-blackboard" not in postinst_content:
        print("[-] FAIL: postinst missing top-level non-recursive chown")
        failed = True
    else:
        print("[+] PASS: postinst contains top-level non-recursive chown")

    # 5. Content checks: prerm
    print("\n--- 5. prerm Content Checks ---")
    prerm_content = scripts["prerm"].read_text()
    if "agentic-blackboard.service" not in prerm_content or "systemctl stop" not in prerm_content:
        print("[-] FAIL: prerm does not stop agentic-blackboard.service")
        failed = True
    else:
        print("[+] PASS: prerm stops agentic-blackboard.service")

    # 6. Content checks: postrm
    print("\n--- 6. postrm Content Checks ---")
    postrm_content = scripts["postrm"].read_text()
    if "daemon-reload" not in postrm_content or "systemctl" not in postrm_content:
        print("[-] FAIL: postrm missing systemctl daemon-reload")
        failed = True
    elif "remove" not in postrm_content or "purge" not in postrm_content:
        print("[-] FAIL: postrm does not handle remove/purge")
        failed = True
    else:
        print("[+] PASS: postrm reloads systemd on remove/purge")

    if "multi-user.target.wants" not in postrm_content or "rm -f" not in postrm_content:
        print("[-] FAIL: postrm does not clean systemd enablement symlinks on purge")
        failed = True
    else:
        print("[+] PASS: postrm cleans systemd enablement symlinks on purge")

    # 7. Unknown argument handling check (exit code 1)
    print("\n--- 7. Unknown Argument Handling (exit 1) ---")
    for name, path in scripts.items():
        res = subprocess.run(
            ["sh", str(path), "invalid_arg"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if res.returncode != 1:
            print(f"[-] FAIL: {name} with 'invalid_arg' returned code {res.returncode}, expected 1")
            failed = True
        else:
            print(f"[+] PASS: {name} correctly exited with code 1 on unknown argument")

    if failed:
        print("\n[-] Verification failed: One or more checks failed.")
        return 1

    print("\n[+] SUCCESS: All maintainer script audits passed!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
