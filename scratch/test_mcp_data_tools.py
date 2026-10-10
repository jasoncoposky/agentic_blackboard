#!/usr/bin/env python3
"""
Unit and integration tests for Task 6: FastMCP Agent Tools & ab-ctl data CLI.
Verifies:
  1. FastMCP publish_artifact tool function (returns uuid, content_hash, pid, etc.)
  2. FastMCP read_artifact tool function (verifies content, HTTP range, metadata)
  3. FastMCP annotate_artifact tool function (verifies status == 'success', avus)
  4. FastMCP query_artifacts tool function (verifies artifact found by AVU triple)
  5. FastMCP verify_artifact_fair tool function (verifies FAIR score rubric & passed flag)
  6. CLI subprocess calls:
     - ab-ctl data put
     - ab-ctl data ls
     - ab-ctl data get (full, file output, and byte range)
     - ab-ctl data meta set
     - ab-ctl data meta get
     - ab-ctl data fair-check
"""

import asyncio
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
import pytest
import requests

# Ensure repository root is on sys.path
REPO_ROOT = Path(__file__).resolve().parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import ab_mcp_server

TEST_PORT = 18090
BASE_URL = f"http://127.0.0.1:{TEST_PORT}"
DB_PATH = "/tmp/ab_test_mcp_data_db"
VAULT_PATH = f"{DB_PATH}_vault"
DAEMON_PATH = REPO_ROOT / "build" / "agentic-blackboardd"
AB_CTL_PY = REPO_ROOT / "src" / "ab-ctl.py"

_ab_ctl_spec = importlib.util.spec_from_file_location("ab_ctl", str(AB_CTL_PY))
_ab_ctl = importlib.util.module_from_spec(_ab_ctl_spec)
_ab_ctl_spec.loader.exec_module(_ab_ctl)
build_mcp_server = _ab_ctl.build_mcp_server


def _cleanup_db():
    for p in [DB_PATH, VAULT_PATH, f"{DB_PATH}.wal", f"{DB_PATH}-shm", f"{DB_PATH}-wal"]:
        if os.path.exists(p):
            if os.path.isdir(p):
                shutil.rmtree(p, ignore_errors=True)
            else:
                try:
                    os.remove(p)
                except OSError:
                    pass


def _wait_for_server(timeout=10.0):
    start = time.time()
    while time.time() - start < timeout:
        try:
            r = requests.get(f"{BASE_URL}/api/v1/schema", timeout=1.0)
            if r.status_code == 200:
                return True
        except Exception:
            pass
        time.sleep(0.1)
    return False


@pytest.fixture(scope="module")
def daemon():
    """Start daemon for the duration of the module tests."""
    _cleanup_db()
    if not DAEMON_PATH.is_file():
        pytest.skip(f"Daemon binary not found at {DAEMON_PATH}")

    os.environ["AB_API_URL"] = f"{BASE_URL}/api/v1"

    proc = subprocess.Popen(
        [str(DAEMON_PATH), f"--port={TEST_PORT}", f"--db={DB_PATH}", "--auth-mode=trusted_network"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    ready = _wait_for_server()
    if not ready:
        stdout, stderr = proc.communicate(timeout=2)
        proc.kill()
        _cleanup_db()
        raise RuntimeError(f"Daemon failed to start on port {TEST_PORT}:\nStdout: {stdout}\nStderr: {stderr}")

    try:
        yield BASE_URL
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        _cleanup_db()


# ----------------------------------------------------------------------
# 1. FastMCP Tool Function Tests
# ----------------------------------------------------------------------

def test_fastmcp_publish_artifact(daemon):
    """Test publish_artifact via direct FastMCP function call."""
    content = "# Experiment Protocol\nStep 1: Calibrate sensors.\nStep 2: Collect data.\n"
    res = ab_mcp_server.publish_artifact(
        path="/protocols/experiment.md",
        content=content,
        metadata={
            "license": "SPDX:Apache-2.0",
            "avus": [{"attribute": "policy:tier", "value": "hot", "units": ""}],
            "domain": "biophysics"
        }
    )

    assert isinstance(res, dict), f"Expected dict response, got {type(res)}: {res}"
    assert "uuid" in res, f"Response missing uuid: {res}"
    assert res["uuid"].startswith("art-"), f"Invalid uuid: {res['uuid']}"
    assert "content_hash" in res, f"Response missing content_hash: {res}"
    assert res["content_hash"].startswith("sha256:"), f"Invalid hash: {res['content_hash']}"
    assert res["pid"] == "urn:ab:artifact:protocols/experiment.md"
    assert res["logical_name"] == "experiment.md"
    assert res["collection_path"] == "/protocols"
    assert res["license"] == "SPDX:Apache-2.0"
    assert res["byte_size"] == len(content)


def test_fastmcp_read_artifact(daemon):
    """Test read_artifact via direct FastMCP function call (full and range)."""
    # 1. Read by UUID
    res = ab_mcp_server.read_artifact("urn:ab:artifact:protocols/experiment.md")
    assert res["status_code"] == 200
    assert "Experiment Protocol" in res["content"]
    assert res["uuid"].startswith("art-")
    assert res["content_hash"].startswith("sha256:")
    assert isinstance(res["metadata"], dict)

    # 2. Read by byte range
    range_res = ab_mcp_server.read_artifact("protocols/experiment.md", range="bytes=0-20")
    assert range_res["status_code"] == 206
    assert range_res["content"] == "# Experiment Protocol"


def test_fastmcp_annotate_artifact(daemon):
    """Test annotate_artifact via direct FastMCP function call."""
    res = ab_mcp_server.annotate_artifact(
        id_or_path="protocols/experiment.md",
        attribute="benchmark:latency",
        value="12.5",
        units="ms"
    )
    assert res["status"] == "success", f"Expected success status, got: {res}"
    assert "avus" in res
    avu_map = {a["attribute"]: a["value"] for a in res["avus"]}
    assert avu_map.get("benchmark:latency") == "12.5"


def test_fastmcp_query_artifacts(daemon):
    """Test query_artifacts via direct FastMCP function call."""
    # Query matching attribute and value
    results = ab_mcp_server.query_artifacts(attribute="policy:tier", value="hot")
    assert isinstance(results, list)
    assert len(results) >= 1
    pids = [r.get("pid") for r in results]
    assert "urn:ab:artifact:protocols/experiment.md" in pids

    # Query matching attribute only
    results_attr = ab_mcp_server.query_artifacts(attribute="benchmark:latency")
    assert len(results_attr) >= 1

    # Query nonexistent attribute
    empty_results = ab_mcp_server.query_artifacts(attribute="nonexistent:attribute:never")
    assert empty_results == []


def test_fastmcp_verify_artifact_fair(daemon):
    """Test verify_artifact_fair via direct FastMCP function call."""
    res = ab_mcp_server.verify_artifact_fair("protocols/experiment.md")
    assert isinstance(res, dict)
    assert res["uuid"].startswith("art-")
    assert res["pid"] == "urn:ab:artifact:protocols/experiment.md"
    assert isinstance(res["score"], (int, float))
    assert res["score"] >= 60.0
    assert "rubric" in res
    rubric = res["rubric"]
    assert "findable" in rubric
    assert "accessible" in rubric
    assert "interoperable" in rubric
    assert "reusable" in rubric
    assert isinstance(res["passed"], bool)

    # Publish an artifact with 100% FAIR compliance rubric
    # (has version, abstract, valid SPDX license, markdown MIME, PID, title, content_hash, path)
    compliant_content = """---
version: "1.0.0"
abstract: "A fully compliant FAIR dataset specification."
---
# Fully Compliant Protocol
Description of high-level protocol.
"""
    comp_art = ab_mcp_server.publish_artifact(
        path="/standards/fair_spec.md",
        content=compliant_content,
        metadata={"license": "SPDX:Apache-2.0"}
    )
    comp_eval = ab_mcp_server.verify_artifact_fair(comp_art["uuid"])
    assert comp_eval["score"] == 100.0
    assert comp_eval["passed"] is True
    assert comp_eval["rubric"]["findable"]["score"] == 30.0
    assert comp_eval["rubric"]["accessible"]["score"] == 25.0
    assert comp_eval["rubric"]["interoperable"]["score"] == 20.0
    assert comp_eval["rubric"]["reusable"]["score"] == 25.0


# ----------------------------------------------------------------------
# 2. CLI Subprocess Calls (ab-ctl data ...)
# ----------------------------------------------------------------------

def run_ab_ctl(args_list: list[str]) -> subprocess.CompletedProcess:
    """Helper to run ab-ctl in a subprocess."""
    cmd = [sys.executable, str(AB_CTL_PY)] + args_list
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def test_cli_data_put_and_ls(daemon, tmp_path):
    """Test 'ab-ctl data put' and 'ab-ctl data ls'."""
    sample_file = tmp_path / "climate_telemetry.csv"
    sample_content = "timestamp,temp_c,humidity\n1700000000,23.4,45.2\n1700000060,23.6,45.1\n"
    sample_file.write_text(sample_content, encoding="utf-8")

    # 1. ab-ctl data put
    proc = run_ab_ctl([
        "--connect", daemon,
        "data", "put",
        str(sample_file),
        "/sensors/meteorology/climate_telemetry.csv",
        "--license", "SPDX:MIT",
        "--avus", "policy:tier=hot,station=weather-alpha"
    ])
    assert proc.returncode == 0, f"data put failed: {proc.stderr}\n{proc.stdout}"
    assert "Artifact published successfully" in proc.stdout
    assert "UUID:" in proc.stdout
    assert "/sensors/meteorology/climate_telemetry.csv" in proc.stdout

    # Extract UUID
    match = re.search(r"UUID:\s+(art-[0-9a-f]+)", proc.stdout)
    assert match, f"Could not parse UUID from put output: {proc.stdout}"
    uuid = match.group(1)

    # 2. ab-ctl data ls with collection filter
    ls_proc = run_ab_ctl([
        "--connect", daemon,
        "data", "ls",
        "/sensors/meteorology"
    ])
    assert ls_proc.returncode == 0, f"data ls failed: {ls_proc.stderr}\n{ls_proc.stdout}"
    assert uuid in ls_proc.stdout
    assert "climate_telemetry.csv" in ls_proc.stdout

    # 3. ab-ctl data ls with AVU query
    ls_query_proc = run_ab_ctl([
        "--connect", daemon,
        "data", "ls",
        "--attr", "station",
        "--val", "weather-alpha"
    ])
    assert ls_query_proc.returncode == 0
    assert uuid in ls_query_proc.stdout


def test_cli_data_get(daemon, tmp_path):
    """Test 'ab-ctl data get' with stdout, file destination, and range."""
    # 1. Get stdout
    proc = run_ab_ctl([
        "--connect", daemon,
        "data", "get",
        "/sensors/meteorology/climate_telemetry.csv"
    ])
    assert proc.returncode == 0, f"data get failed: {proc.stderr}"
    assert "timestamp,temp_c,humidity" in proc.stdout

    # 2. Get to output file
    dest_file = tmp_path / "downloaded.csv"
    proc_file = run_ab_ctl([
        "--connect", daemon,
        "data", "get",
        "/sensors/meteorology/climate_telemetry.csv",
        "-o", str(dest_file)
    ])
    assert proc_file.returncode == 0
    assert dest_file.is_file()
    assert dest_file.read_text(encoding="utf-8") == "timestamp,temp_c,humidity\n1700000000,23.4,45.2\n1700000060,23.6,45.1\n"

    # 3. Get with range
    proc_range = run_ab_ctl([
        "--connect", daemon,
        "data", "get",
        "/sensors/meteorology/climate_telemetry.csv",
        "--range", "bytes=0-8"
    ])
    assert proc_range.returncode == 0
    assert proc_range.stdout == "timestamp"


def test_cli_data_meta_set_and_get(daemon):
    """Test 'ab-ctl data meta set' and 'ab-ctl data meta get'."""
    # 1. Set AVU
    set_proc = run_ab_ctl([
        "--connect", daemon,
        "data", "meta", "set",
        "/sensors/meteorology/climate_telemetry.csv",
        "qa:validated", "true"
    ])
    assert set_proc.returncode == 0, f"data meta set failed: {set_proc.stderr}\n{set_proc.stdout}"
    assert "AVU attached successfully" in set_proc.stdout
    assert "qa:validated" in set_proc.stdout

    # 2. Get AVUs
    get_proc = run_ab_ctl([
        "--connect", daemon,
        "data", "meta", "get",
        "/sensors/meteorology/climate_telemetry.csv"
    ])
    assert get_proc.returncode == 0, f"data meta get failed: {get_proc.stderr}\n{get_proc.stdout}"
    assert "qa:validated" in get_proc.stdout
    assert "true" in get_proc.stdout


def test_cli_data_fair_check(daemon):
    """Test 'ab-ctl data fair-check'."""
    proc = run_ab_ctl([
        "--connect", daemon,
        "data", "fair-check",
        "/sensors/meteorology/climate_telemetry.csv"
    ])
    assert proc.returncode == 0, f"data fair-check failed: {proc.stderr}\n{proc.stdout}"
    assert "FAIR Compliance Report" in proc.stdout
    assert "PID:" in proc.stdout
    assert "Findable" in proc.stdout
    assert "Accessible" in proc.stdout
    assert "Interoperable" in proc.stdout
    assert "Reusable" in proc.stdout


def test_fastmcp_publish_mime_interoperability(daemon):
    """Test publish_artifact infers correct MIME type for .yaml, earning full interoperability points."""
    yaml_content = "apiVersion: v1\nkind: ConfigMap\nmetadata:\n  name: test-cfg\n"
    res = ab_mcp_server.publish_artifact(
        path="/configs/app_config.yaml",
        content=yaml_content,
        metadata={"license": "SPDX:Apache-2.0"}
    )
    assert res["mime_type"] == "application/yaml"
    fair = ab_mcp_server.verify_artifact_fair(res["uuid"])
    assert fair["rubric"]["interoperable"]["mime_type"] is True
    assert fair["rubric"]["interoperable"]["score"] >= 10.0


def test_negative_read_artifact_nonexistent(daemon):
    """Test read_artifact with a non-existent artifact returns 404 and error info."""
    res = ab_mcp_server.read_artifact("nonexistent/artifact/does_not_exist.bin")
    assert res["status_code"] == 404
    assert res["uuid"] is None
    assert "error" in res
    assert res["error"] != ""


def test_negative_read_artifact_invalid_range(daemon):
    """Test read_artifact with invalid byte range returns HTTP 416 and error info."""
    res = ab_mcp_server.read_artifact("protocols/experiment.md", range="bytes=99999-999999")
    assert res["status_code"] == 416
    assert res["content"] == ""
    assert "error" in res


def test_negative_cli_data_put_nonexistent_file(daemon):
    """Test 'ab-ctl data put' with non-existent local file exits with error."""
    proc = run_ab_ctl([
        "--connect", daemon,
        "data", "put",
        "/tmp/definitely_nonexistent_file_xyz_123.txt",
        "/test/nonexistent.txt"
    ])
    assert proc.returncode != 0
    assert "not found" in proc.stderr.lower()


def test_build_mcp_server_data_tools(daemon):
    """Test that build_mcp_server() registers and provides functional data tools."""
    server_mcp = build_mcp_server(daemon, None)
    assert server_mcp is not None and server_mcp != 1

    async def _verify_all():
        tools = await server_mcp.list_tools()
        tool_names = {t.name for t in tools}
        expected_tools = [
            "publish_artifact",
            "read_artifact",
            "annotate_artifact",
            "query_artifacts",
            "verify_artifact_fair"
        ]
        for t in expected_tools:
            assert t in tool_names, f"Tool '{t}' not registered in build_mcp_server()"

        # 1. publish_artifact
        pub_fn = server_mcp._tool_manager.get_tool("publish_artifact").fn
        pub_res = pub_fn(
            path="/built_mcp/manifest.json",
            content='{"name": "mcp_test", "version": "1.0.0"}',
            metadata={"license": "SPDX:MIT"}
        )
        assert isinstance(pub_res, dict)
        assert "uuid" in pub_res
        art_uuid = pub_res["uuid"]
        assert pub_res["logical_name"] == "manifest.json"

        # 2. read_artifact
        read_fn = server_mcp._tool_manager.get_tool("read_artifact").fn
        read_res = read_fn(art_uuid)
        assert read_res["status_code"] == 200
        assert "mcp_test" in read_res["content"]
        assert read_res["uuid"] == art_uuid

        # 2b. read_artifact negative tests (invalid range & nonexistent)
        range_err_res = read_fn(art_uuid, range="bytes=99999-999999")
        assert range_err_res["status_code"] == 416
        assert range_err_res["content"] == ""
        assert "error" in range_err_res

        not_found_res = read_fn("nonexistent/artifact/not_here.bin")
        assert not_found_res["status_code"] == 404
        assert not_found_res["uuid"] is None
        assert "error" in not_found_res

        # 3. annotate_artifact
        ann_fn = server_mcp._tool_manager.get_tool("annotate_artifact").fn
        ann_res = ann_fn(art_uuid, attribute="test:engine", value="build_mcp_server")
        assert ann_res.get("status") == "success"

        # 4. query_artifacts
        query_fn = server_mcp._tool_manager.get_tool("query_artifacts").fn
        query_res = query_fn(attribute="test:engine", value="build_mcp_server")
        assert isinstance(query_res, list)
        assert any(q.get("uuid") == art_uuid for q in query_res)

        # 5. verify_artifact_fair
        fair_fn = server_mcp._tool_manager.get_tool("verify_artifact_fair").fn
        fair_res = fair_fn(art_uuid)
        assert isinstance(fair_res, dict)
        assert "score" in fair_res
        assert "rubric" in fair_res
        assert fair_res["rubric"]["interoperable"]["mime_type"] is True

    asyncio.run(_verify_all())


if __name__ == "__main__":
    sys.exit(pytest.main(["-v", __file__]))
