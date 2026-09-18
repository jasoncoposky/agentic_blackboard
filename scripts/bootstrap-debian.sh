#!/usr/bin/env bash
set -euo pipefail

PACKAGE_ONLY=false
SKIP_BUILD=false
NO_START=false

print_usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Bootstrap and install Agentic Blackboard on Debian / Ubuntu systems.

Options:
  --package-only   Compile and build the .deb package without installing it
  --skip-build     Skip build step; install existing .deb package from build/
  --no-start       Install package but do not immediately start systemd service
  -h, --help       Show this help message and exit
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --package-only) PACKAGE_ONLY=true; shift ;;
        --skip-build) SKIP_BUILD=true; shift ;;
        --no-start) NO_START=true; shift ;;
        -h|--help) print_usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; print_usage; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 1. Pre-flight check: OS identification
if [ ! -f /etc/os-release ]; then
    echo "[ERROR] Cannot verify host OS: /etc/os-release missing." >&2
    exit 1
fi

. /etc/os-release
IS_DEBIAN_FAMILY=false
if [[ "${ID:-}" =~ ^(debian|ubuntu|linuxmint|pop)$ ]] || [[ "${ID_LIKE:-}" =~ (debian|ubuntu) ]]; then
    IS_DEBIAN_FAMILY=true
fi

if [ "$IS_DEBIAN_FAMILY" != "true" ]; then
    echo "[ERROR] This bootstrap script is intended for Debian / Ubuntu distributions (detected: ${NAME:-unknown})." >&2
    echo "For Enterprise Linux 9 / RHEL / Rocky, refer to packaging/rpm/agentic-blackboard.spec." >&2
    exit 1
fi

echo "=== Bootstrapping Agentic Blackboard on ${PRETTY_NAME} ==="

# Helper for elevated commands
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    elif [ "$PACKAGE_ONLY" = "true" ]; then
        SUDO=""
    else
        echo "[ERROR] Root privileges or sudo required." >&2
        exit 1
    fi
fi

# 2. Prerequisites
if [ "$SKIP_BUILD" != "true" ] && [ "$PACKAGE_ONLY" != "true" ]; then
    echo "[*] Ensuring build & runtime prerequisites are installed..."
    DEBIAN_FRONTEND=noninteractive $SUDO apt-get update -qq
    DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq \
        build-essential cmake libzmq3-dev libssl-dev dpkg-dev python3
fi

# 3. Build & Package
BUILD_DIR="${ROOT_DIR}/build"
if [ "$SKIP_BUILD" != "true" ]; then
    echo "[*] Configuring and building Agentic Blackboard..."
    mkdir -p "${BUILD_DIR}"
    cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" -DCMAKE_BUILD_TYPE=Release -DCPACK_GENERATOR="DEB"
    cmake --build "${BUILD_DIR}" -j"$(nproc)"

    echo "[*] Running engine verification test..."
    "${BUILD_DIR}/ab_verify"

    echo "[*] Generating Debian package via CPack..."
    cpack -G DEB -B "${BUILD_DIR}" --config "${BUILD_DIR}/CPackConfig.cmake"
fi

# Locate generated package
DEB_PACKAGE="$(find "${BUILD_DIR}" -maxdepth 1 -name "agentic-blackboard_*.deb" | head -n 1)"
if [ -z "${DEB_PACKAGE}" ] || [ ! -f "${DEB_PACKAGE}" ]; then
    echo "[ERROR] No agentic-blackboard_*.deb found in ${BUILD_DIR}." >&2
    exit 1
fi

echo "[+] Debian package ready: ${DEB_PACKAGE}"

if [ "$PACKAGE_ONLY" = "true" ]; then
    echo "[*] Package-only flag requested. Build complete."
    exit 0
fi

# 4. Installation
echo "[*] Installing package via apt-get..."
DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq "${DEB_PACKAGE}"

# 5. Service Lifecycle
if [ "$NO_START" != "true" ] && [ -d /run/systemd/system ]; then
    echo "[*] Starting agentic-blackboard.service..."
    $SUDO systemctl enable --now agentic-blackboard.service || true
fi

# 6. Status Output
echo ""
echo "================================================================="
echo "  Agentic Blackboard Bootstrapped Successfully!"
echo "================================================================="
if [ -f /var/lib/agentic-blackboard/admin.token ]; then
    echo "  Admin Token File: /var/lib/agentic-blackboard/admin.token"
    echo "  Daemon URL:       http://localhost:8085"
    echo ""
    echo "  To verify status:"
    echo "    ab-ctl status --token-file=/var/lib/agentic-blackboard/admin.token"
    echo ""
    echo "  To initialize a swarm workspace context:"
    echo "    ab-ctl swarm init --context project-alpha --name \"Alpha Project\""
fi
echo "================================================================="
