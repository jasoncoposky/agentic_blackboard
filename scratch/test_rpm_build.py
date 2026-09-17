#!/usr/bin/env python3
"""
Verification script for RPM packaging and filesystem layout specs.
Validates:
1. Packaging files exist and have valid syntax.
2. CMake configuration and build of agentic-blackboardd binary.
3. Execution of RPM packaging build (via containerized rpmbuild / cpack).
4. Inspection of RPM contents via rpm -qlp.
"""

import configparser
import os
import subprocess
import sys
from pathlib import Path

WORKSPACE_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = WORKSPACE_ROOT / "build"
PACKAGING_DIR = WORKSPACE_ROOT / "packaging"


def check_file_exists(path: Path, description: str) -> bool:
    if not path.is_file():
        print(f"[-] FAIL: Missing {description}: {path}")
        return False
    print(f"[+] PASS: Found {description}: {path}")
    return True


def check_dir_exists(path: Path, description: str) -> bool:
    if not path.is_dir():
        print(f"[-] FAIL: Missing {description}: {path}")
        return False
    print(f"[+] PASS: Found {description}: {path}")
    return True


def verify_packaging_files() -> bool:
    print("\n=== 1. Checking Packaging Files Existence & Syntax ===")
    spec_path = PACKAGING_DIR / "rpm" / "agentic-blackboard.spec"
    service_path = PACKAGING_DIR / "systemd" / "agentic-blackboard.service"
    conf_path = PACKAGING_DIR / "config" / "blackboard.conf"
    conf_def_path = PACKAGING_DIR / "config" / "blackboard.conf.default"
    limits_path = PACKAGING_DIR / "limits" / "99-blackboard.conf"

    files_ok = True
    for p, desc in [
        (spec_path, "RPM spec file"),
        (service_path, "Systemd service file"),
        (conf_path, "Configuration file"),
        (conf_def_path, "Default configuration file"),
        (limits_path, "Limits configuration file"),
    ]:
        if not check_file_exists(p, desc):
            files_ok = False

    if not files_ok:
        return False

    # Check spec content
    spec_content = spec_path.read_text(encoding="utf-8")
    for keyword in ["Name: agentic-blackboard", "Version: 0.4.0", "Release: 1.el9", "License: Apache-2.0"]:
        # Normalize whitespace
        k_name, k_val = keyword.split(":")
        k_name = k_name.strip()
        k_val = k_val.strip()
        matched = False
        for line in spec_content.splitlines():
            if line.strip().startswith(k_name + ":") and k_val in line:
                matched = True
                break
        if not matched:
            print(f"[-] FAIL: Spec file missing or incorrect '{keyword}'")
            return False
    for scriptlet in ["%pre", "%post", "%preun", "%postun", "%files"]:
        if scriptlet not in spec_content:
            print(f"[-] FAIL: Spec file missing scriptlet or section '{scriptlet}'")
            return False
    if "blackboard" not in spec_content:
        print("[-] FAIL: Spec %pre scriptlet must create 'blackboard' user/group")
        return False
    if "ab-ctl init --bootstrap" not in spec_content:
        print("[-] FAIL: Spec %post scriptlet must include bootstrap init via ab-ctl")
        return False
    print("[+] PASS: RPM spec file syntax and scriptlets validated.")

    # Check service content
    service_content = service_path.read_text(encoding="utf-8")
    for req in [
        "Type=simple",
        "User=blackboard",
        "Group=blackboard",
        "ExecStart=/usr/bin/agentic-blackboardd --config=/etc/agentic-blackboard/blackboard.conf",
        "Restart=always",
        "LimitNOFILE=65536",
    ]:
        if req not in service_content:
            print(f"[-] FAIL: Systemd service missing requirement '{req}'")
            return False
    print("[+] PASS: Systemd service configuration validated.")

    # Check blackboard.conf & blackboard.conf.default
    for cp, c_name in [(conf_path, "blackboard.conf"), (conf_def_path, "blackboard.conf.default")]:
        cp_parser = configparser.ConfigParser()
        try:
            cp_parser.read_string(cp.read_text(encoding="utf-8"))
        except Exception as e:
            print(f"[-] FAIL: Failed to parse {c_name} as INI: {e}")
            return False

        if not cp_parser.has_section("server") or not cp_parser.has_section("auth") or not cp_parser.has_section("storage"):
            print(f"[-] FAIL: {c_name} missing required sections [server], [auth], [storage]")
            return False

        if cp_parser.get("server", "port", fallback=None) != "8085":
            print(f"[-] FAIL: {c_name} [server] port != 8085")
            return False
        if cp_parser.get("server", "host", fallback=None) != "0.0.0.0":
            print(f"[-] FAIL: {c_name} [server] host != 0.0.0.0")
            return False
        if cp_parser.get("auth", "mode", fallback=None) != "token":
            print(f"[-] FAIL: {c_name} [auth] mode != token")
            return False
        if cp_parser.get("storage", "data_dir", fallback=None) != "/var/lib/agentic-blackboard":
            print(f"[-] FAIL: {c_name} [storage] data_dir != /var/lib/agentic-blackboard")
            return False
        print(f"[+] PASS: {c_name} INI sections and key-values validated.")

    # Check limits
    limits_content = limits_path.read_text(encoding="utf-8")
    if "blackboard soft nofile 65536" not in limits_content or "blackboard hard nofile 65536" not in limits_content:
        print("[-] FAIL: 99-blackboard.conf missing soft/hard nofile 65536 limits")
        return False
    print("[+] PASS: 99-blackboard.conf limits validated.")

    return True


def verify_cmake_and_build() -> bool:
    print("\n=== 2. Checking CMake Target & Building agentic-blackboardd ===")
    cmakelists = WORKSPACE_ROOT / "CMakeLists.txt"
    cmake_content = cmakelists.read_text(encoding="utf-8")
    if "agentic-blackboardd" not in cmake_content:
        print("[-] FAIL: CMakeLists.txt does not contain agentic-blackboardd target")
        return False

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    # Configure CMake
    cmd_cfg = ["cmake", "-B", str(BUILD_DIR), "-S", str(WORKSPACE_ROOT)]
    res = subprocess.run(cmd_cfg, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] FAIL: cmake configure failed:\n{res.stderr}")
        return False

    # Build agentic-blackboardd
    cmd_build = ["cmake", "--build", str(BUILD_DIR), "--target", "agentic-blackboardd", "-j"]
    res = subprocess.run(cmd_build, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] FAIL: cmake build target agentic-blackboardd failed:\n{res.stderr}")
        return False

    bin_path = BUILD_DIR / "agentic-blackboardd"
    if not bin_path.is_file() or not os.access(bin_path, os.X_OK):
        print(f"[-] FAIL: Built binary {bin_path} is missing or not executable")
        return False

    print(f"[+] PASS: agentic-blackboardd binary built successfully: {bin_path}")
    return True


def build_and_verify_rpm() -> bool:
    print("\n=== 3. Building & Verifying RPM Package ===")
    rpm_output_path = BUILD_DIR / "agentic-blackboard-0.4.0-1.el9.x86_64.rpm"

    # Run rpmbuild in ubi9-minimal container
    docker_cmd = [
        "docker", "run", "--rm",
        "-v", f"{WORKSPACE_ROOT}:/workspace:rw",
        "registry.access.redhat.com/ubi9/ubi-minimal:latest",
        "bash", "-c",
        """set -e
microdnf install -y rpm-build >/dev/null 2>&1
mkdir -p /workspace/build/rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
rpmbuild -bb \
    --define '_topdir /workspace/build/rpmbuild' \
    --define '_sourcedir /workspace' \
    --define '_rpmdir /workspace/build' \
    /workspace/packaging/rpm/agentic-blackboard.spec >/dev/null 2>&1
if [ -f /workspace/build/x86_64/agentic-blackboard-0.4.0-1.el9.x86_64.rpm ]; then
    cp -f /workspace/build/x86_64/agentic-blackboard-0.4.0-1.el9.x86_64.rpm /workspace/build/agentic-blackboard-0.4.0-1.el9.x86_64.rpm
fi
"""
    ]

    print("[*] Running containerized rpmbuild...")
    res = subprocess.run(docker_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] FAIL: containerized rpmbuild failed:\n{res.stderr}\n{res.stdout}")
        return False

    if not rpm_output_path.is_file():
        print(f"[-] FAIL: Expected RPM file not found at: {rpm_output_path}")
        return False

    print(f"[+] PASS: RPM package generated at {rpm_output_path} ({rpm_output_path.stat().st_size} bytes)")

    # Query contents via rpm -qlp in ubi-minimal container
    query_cmd = [
        "docker", "run", "--rm",
        "-v", f"{WORKSPACE_ROOT}:/workspace:ro",
        "registry.access.redhat.com/ubi9/ubi-minimal:latest",
        "rpm", "-qlp", "/workspace/build/agentic-blackboard-0.4.0-1.el9.x86_64.rpm"
    ]
    res_q = subprocess.run(query_cmd, capture_output=True, text=True)
    if res_q.returncode != 0:
        print(f"[-] FAIL: rpm -qlp failed:\n{res_q.stderr}")
        return False

    rpm_files = res_q.stdout.splitlines()
    print("[*] RPM file manifest:")
    for f in sorted(rpm_files):
        print(f"    {f}")

    required_paths = [
        "/usr/bin/agentic-blackboardd",
        "/usr/bin/ab-ctl",
        "/usr/lib/systemd/system/agentic-blackboard.service",
        "/etc/agentic-blackboard/blackboard.conf",
        "/usr/share/agentic-blackboard/skills",
    ]

    missing_paths = []
    for rp in required_paths:
        if not any(f.startswith(rp) or f == rp for f in rpm_files):
            missing_paths.append(rp)

    if missing_paths:
        print(f"[-] FAIL: RPM missing required paths: {missing_paths}")
        return False

    print("[+] PASS: All required filesystem paths are present in RPM manifest.")

    # Query metadata via rpm -qip
    info_cmd = [
        "docker", "run", "--rm",
        "-v", f"{WORKSPACE_ROOT}:/workspace:ro",
        "registry.access.redhat.com/ubi9/ubi-minimal:latest",
        "rpm", "-qip", "/workspace/build/agentic-blackboard-0.4.0-1.el9.x86_64.rpm"
    ]
    res_info = subprocess.run(info_cmd, capture_output=True, text=True)
    if res_info.returncode != 0 or "agentic-blackboard" not in res_info.stdout:
        print("[-] FAIL: rpm -qip query failed or unexpected metadata")
        return False
    print("[+] PASS: RPM package metadata verified.")

    # Query scriptlets via rpm -qp --scripts
    scripts_cmd = [
        "docker", "run", "--rm",
        "-v", f"{WORKSPACE_ROOT}:/workspace:ro",
        "registry.access.redhat.com/ubi9/ubi-minimal:latest",
        "rpm", "-qp", "--scripts", "/workspace/build/agentic-blackboard-0.4.0-1.el9.x86_64.rpm"
    ]
    res_scripts = subprocess.run(scripts_cmd, capture_output=True, text=True)
    if res_scripts.returncode != 0:
        print("[-] FAIL: rpm -qp --scripts failed")
        return False
    scripts_out = res_scripts.stdout
    if "groupadd" not in scripts_out or "ab-ctl init --bootstrap" not in scripts_out:
        print("[-] FAIL: RPM package scriptlets missing user creation or bootstrap init")
        return False
    print("[+] PASS: RPM package scriptlets (%pre, %post, %preun, %postun) verified.")

    return True


def verify_rpm_install() -> bool:
    print("\n=== 4. Testing RPM Installation & Runtime Lifecycle in Clean Container ===")
    install_test_cmd = [
        "docker", "run", "--rm",
        "-v", f"{WORKSPACE_ROOT}:/workspace:ro",
        "registry.access.redhat.com/ubi9/ubi-minimal:latest",
        "bash", "-c",
        """set -e
microdnf install -y shadow-utils python3 >/dev/null 2>&1
rpm -ivh /workspace/build/agentic-blackboard-0.4.0-1.el9.x86_64.rpm
id blackboard >/dev/null
[ -f /var/lib/agentic-blackboard/credentials.db ]
[ -f /var/lib/agentic-blackboard/admin.token ]
[ -f /usr/bin/agentic-blackboardd ]
[ -x /usr/bin/ab-ctl ]
/usr/bin/ab-ctl --help >/dev/null
rpm -e agentic-blackboard
[ ! -f /usr/bin/agentic-blackboardd ]
[ ! -f /usr/bin/ab-ctl ]
"""
    ]

    res = subprocess.run(install_test_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] FAIL: Container installation lifecycle test failed:\n{res.stderr}\n{res.stdout}")
        return False

    print("[+] PASS: RPM installed, initialized credentials, verified CLI, and uninstalled cleanly.")
    return True


def main():
    print("=== Task 5 Verification: RPM Packaging & Filesystem Layout Specs ===")
    if not verify_packaging_files():
        print("\n[-] STEP 1 (Packaging Files) FAILED.")
        sys.exit(1)

    if not verify_cmake_and_build():
        print("\n[-] STEP 2 (CMake Target & Build) FAILED.")
        sys.exit(1)

    if not build_and_verify_rpm():
        print("\n[-] STEP 3 (RPM Generation & Validation) FAILED.")
        sys.exit(1)

    if not verify_rpm_install():
        print("\n[-] STEP 4 (RPM Installation Lifecycle) FAILED.")
        sys.exit(1)

    print("\n=======================================================")
    print(">>> ALL CHECKS PASSED: RPM Packaging Successfully Verified! <<<")
    print("=======================================================")
    sys.exit(0)


if __name__ == "__main__":
    main()
