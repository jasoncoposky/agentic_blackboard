# Design Spec: Debian & Ubuntu Distribution Support and Automated Bootstrap

**Date**: 2026-09-17  
**Status**: APPROVED  
**Product**: `agentic-blackboard` (`agentic-blackboardd`, `ab-ctl`)  
**Authors**: Jason Coposky & Antigravity  

---

## 1. Executive Summary & Problem Statement

Agentic Blackboard was initially configured with an Enterprise Linux 9 (EL9) packaging target (`agentic-blackboard-*.el9.x86_64.rpm`) and a Red Hat UBI 9 Minimal container image (`ubi9-minimal`). However, primary development workstations, CI runners, and extensive production infrastructure operate on **Ubuntu 24.04 LTS (Noble Numbat)** and Debian-family distributions.

On Debian-family hosts, installing via RPM is unsuitable without foreign converters like `alien`. To establish first-class parity across the Linux ecosystem, Agentic Blackboard requires:
1. **Native Debian Packaging (`.deb`)**: Produced via CMake's built-in `CPack DEB` generator alongside the existing RPM target.
2. **Debian Package Lifecycle Scripts**: Canonical `postinst`, `prerm`, and `postrm` maintainer scripts that manage the `blackboard` service user/group, directories, permissions, and systemd service state.
3. **Automated Host Bootstrap Script**: An idempotent `scripts/bootstrap-debian.sh` script providing one-command installation, compilation, packaging, and service startup for Debian 12 and Ubuntu 24.04/22.04 LTS.

---

## 2. Packaging Architecture & CMake / CPack Integration

### 2.1 Unified Multi-Generator Packaging Pipeline
CMake's CPack subsystem is configured to support both `DEB` and `RPM` generators. The default generator on Debian/Ubuntu hosts defaults to `DEB` (or both via `-DCPACK_GENERATOR="DEB;RPM"`).

```
┌────────────────────────────────────────────────────────────────────────┐
│                        CMake / CPack Pipeline                          │
│                                                                        │
│   Targets: agentic-blackboardd (ELF C++20) + ab-ctl (Python 3)         │
│   Assets:  systemd service, blackboard.conf, limits, skills catalog    │
│                                │                                       │
│                ┌───────────────┴───────────────┐                       │
│                ▼                               ▼                       │
│      CPack RPM Generator             CPack DEB Generator               │
│  agentic-blackboard-*.el9.rpm    agentic-blackboard_*.deb              │
│  (RHEL/Rocky/AlmaLinux/UBI9)     (Ubuntu 24.04/22.04, Debian 12)       │
└────────────────────────────────────────────────────────────────────────┘
```

### 2.2 CPack DEB Directives in `CMakeLists.txt`
The following CPack DEB variables will be added to [`CMakeLists.txt`](file:///home/darkfell/dev/agentic_blackboard/CMakeLists.txt):

```cmake
# Debian Packaging Directives
set(CPACK_DEBIAN_PACKAGE_NAME "${CPACK_PACKAGE_NAME}")
set(CPACK_DEBIAN_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Agentic Blackboard Team <team@agentic-blackboard.org>")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "Agentic Blackboard Server & Ambient Substrate Daemon")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/jasoncoposky/agentic_blackboard")

# Dynamic Shared Library Dependency Resolution
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# Additional Runtime Dependencies
set(CPACK_DEBIAN_PACKAGE_DEPENDS "systemd, python3 (>= 3.10), adduser | passwd")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "python3-httpx")

# Maintainer Control Scripts
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm"
)
```

`CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON` executes `dpkg-shlibdeps` on the compiled `agentic-blackboardd` ELF binary at package time, auto-detecting and generating exact dependency statements for `libc6`, `libstdc++6`, `libzmq5`, and `libssl3` (including 64-bit `t64` variants on Ubuntu 24.04).

---

## 3. Filesystem Hierarchy & Permissions

The `.deb` package installs to the standard Linux Filesystem Hierarchy Standard (FHS):

| Target Path | Permissions | Owner:Group | Purpose |
|:---|:---|:---|:---|
| `/usr/bin/agentic-blackboardd` | `0755` | `root:root` | C++20 Substrate Daemon |
| `/usr/bin/ab-ctl` | `0755` | `root:root` | CLI & FastMCP Server Runner |
| `/usr/lib/systemd/system/agentic-blackboard.service` | `0644` | `root:root` | Systemd service definition |
| `/etc/agentic-blackboard/blackboard.conf` | `0640` | `root:blackboard` | Default configuration |
| `/etc/agentic-blackboard/blackboard.conf.default` | `0644` | `root:root` | Reference configuration template |
| `/etc/security/limits.d/99-blackboard.conf` | `0644` | `root:root` | File descriptor resource limits |
| `/usr/share/agentic-blackboard/skills/` | `0755 / 0644` | `root:root` | Swarm skills and references catalog |
| `/var/lib/agentic-blackboard` | `0750` | `blackboard:blackboard` | Database and persistent state |
| `/var/log/agentic-blackboard` | `0750` | `blackboard:blackboard` | Log file directory |

---

## 4. Debian Package Maintainer Scripts

### 4.1 `packaging/debian/postinst`
Executed after files are unpacked:
1. **User & Group Provisioning**:
   Checks if group/user `blackboard` exists; creates system group and user with `/usr/sbin/nologin` shell and `/var/lib/agentic-blackboard` home if missing.
2. **Directory Perms**:
   Ensures `/var/lib/agentic-blackboard`, `/var/log/agentic-blackboard`, and `/etc/agentic-blackboard` exist with `0750` / `0640` permissions.
3. **Idempotent Bootstrap**:
   If `/var/lib/agentic-blackboard/initialized` does not exist:
   - Executes `/usr/bin/ab-ctl init --bootstrap --data-dir=/var/lib/agentic-blackboard`.
   - Creates stamp file `/var/lib/agentic-blackboard/initialized`.
   - Ensures correct ownership (`chown -R blackboard:blackboard /var/lib/agentic-blackboard`).
4. **Systemd Reload**:
   Checks if systemd is active (`[ -d /run/systemd/system ]`); if so, executes `systemctl --system daemon-reload` and enables the unit.

### 4.2 `packaging/debian/prerm`
Executed before package removal:
1. If systemd is active, stops `agentic-blackboard.service`.

### 4.3 `packaging/debian/postrm`
Executed after package removal:
1. If systemd is active, executes `systemctl --system daemon-reload`.
2. On `purge`, leaves persistent knowledge data intact or notifies the operator, avoiding accidental data loss.

---

## 5. Automated Bootstrap Script (`scripts/bootstrap-debian.sh`)

An idempotent, single-command utility for host initialization:

### 5.1 CLI Interface
```bash
./scripts/bootstrap-debian.sh [options]

Options:
  --package-only     Build and generate the .deb package without installing it
  --skip-build       Skip compilation and install existing package from build/
  --no-start         Install package but do not immediately start the service
  --help             Display usage guide
```

### 5.2 Execution Flow
1. **OS Pre-flight**: Verifies Debian or Ubuntu host via `/etc/os-release` (`ID` or `ID_LIKE`).
2. **Prerequisites Check & Install**: Installs `build-essential`, `cmake`, `libzmq3-dev`, `libssl-dev`, `dpkg-dev`, `python3` via `apt-get`.
3. **Build & Verification**: Runs `cmake -B build -S . -DCMAKE_BUILD_TYPE=Release`, compiles targets, and runs `./build/ab_verify`.
4. **CPack Packaging**: Executes `cpack -G DEB --config build/CPackConfig.cmake`.
5. **Installation**: Executes `apt-get install -y ./build/agentic-blackboard_*.deb` (or `dpkg -i`).
6. **Health Verification & Output**: Verifies `ab-ctl status` and outputs the bootstrap admin token path and connection instructions.

---

## 6. Verification & Test Plan

### 6.1 Packaging Test Suite (`scratch/test_deb_packaging.py`)
A dedicated Python test suite verifies:
1. **Package File Creation**: Confirms `agentic-blackboard_*.deb` exists in `build/`.
2. **Control Header Audit (`dpkg-deb -I`)**: Confirms `Package: agentic-blackboard`, `Version: 0.4.0`, and correct architecture (`amd64`).
3. **Dependency Graph Inspection**: Confirms `Depends:` contains `libc6`, `libzmq5`, `libssl3` (or `libssl3t64`), `python3`, `systemd`.
4. **Maintainer Script Audit (`dpkg-deb -e`)**: Extracts and verifies `postinst`, `prerm`, and `postrm` exist, have `0755` permissions, and pass `sh -n` syntax checks.
5. **Payload File Audit (`dpkg-deb -c`)**: Verifies all required binaries, config files, systemd units, and skills catalog are present.

### 6.2 Existing Suite Non-Regression
1. `./build/ab_verify`: 100% PASS.
2. `python3 scratch/test_ab_ctl.py`: 100% PASS.
3. `python3 scratch/test_ab_ctl_swarm.py`: 100% PASS.
4. `python3 scratch/test_mcp_swarm.py`: 100% PASS.
5. `python3 scratch/test_e2e_cpg_swarm.py`: 100% PASS.
