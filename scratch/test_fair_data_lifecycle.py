#!/usr/bin/env python3
"""
Full-Cycle FAIR Data Integration & Packaging Verification.

Hermetically tests the entire lifecycle of Content-Addressable Storage (CAS)
and FAIR Data Management in Agentic Blackboard:
  1. FastMCP Publish with YAML Frontmatter
  2. 100% Physical CAS Vault Deduplication
  3. L3KVG Graph Relationship Query (CONTAINS, ANNOTATED_WITH, SPECIFIES, STORED_AS)
  4. W3C RDF Export (Turtle serialization of DigitalDocument and Collection)
  5. HTTP 206 Partial Content Streaming
  6. AVU Mutation & Verification (annotate_artifact & query_artifacts)
  7. Clean Teardown
"""

import asyncio
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import httpx

REPO_ROOT = Path(__file__).resolve().parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import ab_mcp_server
from ab_mcp_server import (
    publish_artifact,
    read_artifact,
    annotate_artifact,
    query_artifacts,
    verify_artifact_fair,
    get_node,
    get_node_links,
    export_graph_rdf,
)


def find_free_port() -> int:
    """Allocate an unused ephemeral TCP port."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("", 0))
        return s.getsockname()[1]


def find_vault_dir(root_dir: Path) -> Path:
    """Find the physical CAS vault directory created by the daemon."""
    for p in root_dir.glob("**/vault"):
        if p.is_dir():
            return p
    for p in root_dir.glob("**/*_vault"):
        if p.is_dir():
            return p
    return root_dir / "vault"


started_process = None
daemon_log_file = None


async def ensure_backend(test_port: int, data_dir: str, api_url: str):
    """Ensure the Agentic Blackboard daemon is running and reachable."""
    global started_process, daemon_log_file

    daemon_bin = REPO_ROOT / "build" / "agentic-blackboardd"
    if not daemon_bin.exists():
        raise RuntimeError(f"Daemon binary not found at {daemon_bin}. Run cmake build first!")

    daemon_cmd = [
        str(daemon_bin),
        "1",
        f"--data-dir={data_dir}",
        "--auth-mode=trusted_network",
        f"--port={test_port}"
    ]
    print(f"[DAEMON] Starting Agentic Blackboard daemon on port {test_port}: {' '.join(daemon_cmd)}")
    daemon_log_path = Path(data_dir) / "daemon.log"
    daemon_log_file = open(daemon_log_path, "w")
    started_process = subprocess.Popen(
        daemon_cmd,
        cwd=str(REPO_ROOT),
        stdout=daemon_log_file,
        stderr=subprocess.STDOUT
    )

    # Wait up to 30 seconds for daemon to initialize
    for _ in range(150):
        if started_process and started_process.poll() is not None:
            log_content = ""
            if daemon_log_path.exists():
                log_content = daemon_log_path.read_text(errors="replace")
            raise RuntimeError(f"Daemon exited prematurely with code {started_process.returncode}:\n{log_content}")
        await asyncio.sleep(0.2)
        try:
            async with httpx.AsyncClient() as client:
                resp = await client.get(f"{api_url}/schema", timeout=1.0)
                if resp.status_code == 200:
                    print(f"[DAEMON] Daemon initialized and responsive at {api_url}")
                    return
        except Exception:
            pass

    log_content = ""
    if daemon_log_path.exists():
        log_content = daemon_log_path.read_text(errors="replace")
    raise RuntimeError(f"Timed out waiting for daemon to start on port {test_port}.\n{log_content}")


async def run_lifecycle(temp_dir: Path, api_url: str):
    print("\n" + "=" * 70)
    print("STARTING FULL-CYCLE FAIR DATA INTEGRATION VERIFICATION")
    print("=" * 70)

    # =========================================================================
    # Step 1: FastMCP Publish with YAML Frontmatter
    # =========================================================================
    print("\n" + "-" * 70)
    print("STEP 1: FastMCP Publish with YAML Frontmatter")
    print("-" * 70)

    doc_content = (
        "---\n"
        "title: Decoupled CAS & FAIR Storage Specification\n"
        "license: SPDX:Apache-2.0\n"
        "version: 1.0.0\n"
        "abstract: Architectural design for Content-Addressable Storage with zero binary blobs in L3KVG.\n"
        "---\n"
        "# Decoupled CAS & FAIR Storage Specification\n"
        "Detailed architecture description.\n"
    )
    target_path = "/nucleus/specs/storage_cas.md"

    pub_res = publish_artifact(path=target_path, content=doc_content)
    assert isinstance(pub_res, dict), f"Publish failed: {pub_res}"
    assert "error" not in pub_res, f"Publish error: {pub_res.get('error')}"

    art_uuid = pub_res.get("uuid")
    content_hash = pub_res.get("content_hash")
    pid = pub_res.get("pid")

    print(f"Artifact Published: UUID={art_uuid}, PID={pid}, Content Hash={content_hash}")

    assert art_uuid and art_uuid.startswith("art-"), f"Invalid uuid: {art_uuid}"
    assert pid == "urn:ab:artifact:nucleus/specs/storage_cas.md", f"Invalid pid: {pid}"
    assert content_hash and (content_hash.startswith("sha256:") or content_hash.startswith("blake3:")), f"Invalid content_hash: {content_hash}"
    assert pub_res.get("title") == "Decoupled CAS & FAIR Storage Specification", f"Title mismatch: {pub_res.get('title')}"
    assert pub_res.get("license") == "SPDX:Apache-2.0", f"License mismatch: {pub_res.get('license')}"
    assert pub_res.get("version") == "1.0.0", f"Version mismatch: {pub_res.get('version')}"
    assert "Content-Addressable Storage" in pub_res.get("abstract", ""), f"Abstract mismatch: {pub_res.get('abstract')}"

    # Evaluate FAIR compliance score
    fair_res = verify_artifact_fair(target_path)
    assert isinstance(fair_res, dict), f"FAIR evaluation failed: {fair_res}"
    assert fair_res.get("passed") is True, f"FAIR check did not pass: {fair_res}"
    fair_score = fair_res.get("score", 0.0)
    assert fair_score >= 80.0, f"FAIR score too low: {fair_score} (expected >= 80)"
    print(f"FAIR Score: {fair_score}/100.0 (Passed: {fair_res.get('passed')})")

    # Verify GET /api/v1/node/:id works for artifact nodes via get_node MCP tool
    node_raw = await get_node(uuid=art_uuid)
    node_data = json.loads(node_raw)
    assert node_data.get("uuid") == art_uuid or node_data.get("id") == art_uuid, f"get_node failed: {node_data}"
    assert node_data.get("type") == "ARTIFACT", f"Expected type ARTIFACT, got: {node_data.get('type')}"
    assert node_data.get("title") == "Decoupled CAS & FAIR Storage Specification", f"Title mismatch in get_node: {node_data}"
    assert node_data.get("content_hash") == content_hash, f"Hash mismatch in get_node: {node_data}"
    print(f"✓ get_node({art_uuid}) verified via MCP: type={node_data.get('type')}, title={node_data.get('title')}")

    print("✓ Step 1 PASSED: FastMCP Publish with YAML Frontmatter verified.")

    # =========================================================================
    # Step 2: 100% Physical CAS Vault Deduplication
    # =========================================================================
    print("\n" + "-" * 70)
    print("STEP 2: 100% Physical CAS Vault Deduplication")
    print("-" * 70)

    backup_path = "/archive/specs/storage_cas_backup.md"
    pub_res2 = publish_artifact(path=backup_path, content=doc_content)
    assert isinstance(pub_res2, dict), f"Second publish failed: {pub_res2}"

    art_uuid2 = pub_res2.get("uuid")
    content_hash2 = pub_res2.get("content_hash")
    pid2 = pub_res2.get("pid")

    print(f"Second Artifact Published: UUID={art_uuid2}, PID={pid2}, Content Hash={content_hash2}")

    assert art_uuid2 != art_uuid, "Expected different UUIDs for separate logical artifacts"
    assert pid2 == "urn:ab:artifact:archive/specs/storage_cas_backup.md", f"Invalid backup pid: {pid2}"
    assert content_hash2 == content_hash, f"Content hash mismatch: {content_hash2} vs {content_hash}"

    # Inspect physical CAS vault on disk
    vault_dir = find_vault_dir(temp_dir)
    print(f"Inspecting physical CAS vault at: {vault_dir}")
    assert vault_dir.is_dir(), f"Vault directory does not exist: {vault_dir}"

    raw_hex = content_hash.split(":")[-1]
    expected_cas_file = vault_dir / raw_hex[:2] / raw_hex[2:4] / raw_hex
    assert expected_cas_file.is_file(), f"Expected physical CAS file not found: {expected_cas_file}"

    # Count how many files in the entire vault match this digest / exist
    all_vault_files = [f for f in vault_dir.rglob("*") if f.is_file() and not f.name.endswith(".tmp")]
    cas_files_matching_hash = [f for f in all_vault_files if f.name == raw_hex]
    print(f"Underlying CAS files in vault: total={len(all_vault_files)}, matching hash={len(cas_files_matching_hash)}")

    assert len(cas_files_matching_hash) == 1, (
        f"Deduplication failure! Expected exactly 1 CAS file for hash {raw_hex}, found {len(cas_files_matching_hash)}"
    )
    # File content matches doc_content
    with open(expected_cas_file, "rb") as f:
        disk_bytes = f.read()
    assert disk_bytes == doc_content.encode("utf-8"), "CAS file content on disk does not match published payload"
    print("✓ Step 2 PASSED: 100% Physical CAS Vault Deduplication verified.")

    # =========================================================================
    # Step 3: L3KVG Graph Relationship Query
    # =========================================================================
    print("\n" + "-" * 70)
    print("STEP 3: L3KVG Graph Relationship Query")
    print("-" * 70)

    links_raw = await get_node_links(uuid=art_uuid, direction="both")
    links_data = json.loads(links_raw)
    assert "inbound" in links_data and "outbound" in links_data, f"Invalid links response: {links_data}"

    inbound = links_data.get("inbound", [])
    outbound = links_data.get("outbound", [])

    print(f"Artifact {art_uuid} Inbound Links ({len(inbound)}):")
    for link in inbound:
        print(f"  <- [{link.get('relation')}] from {link.get('source')}")

    print(f"Artifact {art_uuid} Outbound Links ({len(outbound)}):")
    for link in outbound:
        print(f"  -> [{link.get('relation')}] to {link.get('target')}")

    # 1. Collection /nucleus/specs links to artifact via CONTAINS
    has_contains = any(
        link.get("relation") == "CONTAINS" and link.get("source") == "/nucleus/specs"
        for link in inbound
    )
    assert has_contains, f"Collection /nucleus/specs must link to artifact via CONTAINS. Inbound: {inbound}"
    print("✓ Collection /nucleus/specs -[CONTAINS]-> Artifact confirmed.")

    # 2. Artifact links to AVU nodes via ANNOTATED_WITH
    avu_links = [link for link in outbound if link.get("relation") == "ANNOTATED_WITH"]
    assert len(avu_links) >= 1, f"Artifact must link to AVU nodes via ANNOTATED_WITH. Outbound: {outbound}"
    print(f"✓ Artifact -[ANNOTATED_WITH]-> AVU nodes confirmed ({len(avu_links)} AVU edges).")

    # 3. Artifact links to PID via SPECIFIES
    specifies_links = [
        link for link in outbound
        if link.get("relation") == "SPECIFIES" and link.get("target") == pid
    ]
    assert len(specifies_links) >= 1, f"Artifact must link to PID {pid} via SPECIFIES. Outbound: {outbound}"
    print(f"✓ Artifact -[SPECIFIES]-> {pid} confirmed.")

    # 4. Artifact links to locator via STORED_AS
    stored_as_links = [link for link in outbound if link.get("relation") == "STORED_AS"]
    assert len(stored_as_links) >= 1, f"Artifact must link to storage locator via STORED_AS. Outbound: {outbound}"
    print(f"✓ Artifact -[STORED_AS]-> {stored_as_links[0].get('target')} confirmed.")
    print("✓ Step 3 PASSED: All 4 L3KVG Graph Relationships verified.")

    # =========================================================================
    # Step 4: W3C RDF Export
    # =========================================================================
    print("\n" + "-" * 70)
    print("STEP 4: W3C RDF Export")
    print("-" * 70)

    rdf_text = await export_graph_rdf()
    assert isinstance(rdf_text, str) and len(rdf_text) > 0, "RDF export returned empty string"

    print(f"RDF Turtle export received ({len(rdf_text)} bytes).")

    assert "@prefix schema: <http://schema.org/>" in rdf_text, "Missing @prefix schema in RDF"
    assert "@prefix ab: <http://agenticblackboard.ai/schema#>" in rdf_text, "Missing @prefix ab in RDF"
    assert "@prefix dc: <http://purl.org/dc/terms/>" in rdf_text, "Missing @prefix dc in RDF"

    # Verify Artifact representation
    assert "schema:DigitalDocument" in rdf_text, "Missing schema:DigitalDocument in RDF export"
    assert "Decoupled CAS & FAIR Storage Specification" in rdf_text, "Missing artifact title in RDF export"
    assert "SPDX:Apache-2.0" in rdf_text, "Missing artifact license in RDF export"

    # Verify Collection representation
    assert "schema:Collection" in rdf_text, "Missing schema:Collection in RDF export"
    assert "nucleus/specs" in rdf_text, "Missing collection path in RDF export"

    # Verify Storage Locator representation
    assert "urn:ab:locator:" in rdf_text, "Missing urn:ab:locator in RDF export"

    print("✓ Step 4 PASSED: W3C RDF Turtle Export with DigitalDocument and Collection verified.")

    # =========================================================================
    # Step 5: HTTP 206 Partial Content Streaming
    # =========================================================================
    print("\n" + "-" * 70)
    print("STEP 5: HTTP 206 Partial Content Streaming")
    print("-" * 70)

    range_spec = "bytes=0-40"
    range_res = read_artifact(id_or_path=target_path, range=range_spec)
    assert isinstance(range_res, dict), f"read_artifact failed: {range_res}"
    assert range_res.get("status_code") == 206, f"Expected HTTP 206, got: {range_res.get('status_code')}"

    expected_bytes = doc_content.encode("utf-8")[:41]
    actual_content = range_res.get("content", "")
    assert actual_content.encode("utf-8") == expected_bytes, (
        f"Byte slice mismatch! Expected {len(expected_bytes)} bytes, got {len(actual_content.encode('utf-8'))} bytes."
    )
    assert len(expected_bytes) == 41, "Range bytes=0-40 should return 41 bytes"
    print(f"Received partial content ({len(expected_bytes)} bytes): {repr(actual_content)}")
    print("✓ Step 5 PASSED: HTTP 206 Partial Content Streaming verified.")

    # =========================================================================
    # Step 6: AVU Mutation & Verification
    # =========================================================================
    print("\n" + "-" * 70)
    print("STEP 6: AVU Mutation & Verification")
    print("-" * 70)

    anno_res = annotate_artifact(
        id_or_path=art_uuid,
        attribute="curation:review",
        value="approved",
        units=""
    )
    assert isinstance(anno_res, dict), f"annotate_artifact failed: {anno_res}"
    assert anno_res.get("status") == "success", f"Expected success status, got: {anno_res}"

    # Query back via query_artifacts
    query_res = query_artifacts(attribute="curation:review", value="approved")
    assert isinstance(query_res, list), f"Expected list response from query_artifacts, got: {type(query_res)}"
    matching_uuids = [art.get("uuid") for art in query_res]
    print(f"Query for curation:review=approved returned: {matching_uuids}")

    assert art_uuid in matching_uuids, (
        f"Artifact {art_uuid} not found in query results for curation:review=approved! Got: {matching_uuids}"
    )
    print("✓ Step 6 PASSED: AVU Mutation & Verification verified.")


async def main():
    test_port = int(os.environ.get("AB_TEST_PORT", 0)) or find_free_port()
    temp_dir = tempfile.mkdtemp(prefix="bb_fair_lifecycle_")
    data_dir = os.path.join(temp_dir, "substrate")
    os.makedirs(data_dir, exist_ok=True)
    base_url = f"http://127.0.0.1:{test_port}"
    api_url = f"{base_url}/api/v1"
    os.environ["AB_API_URL"] = api_url
    os.environ["AB_TEST_PORT"] = str(test_port)
    ab_mcp_server.AB_API_URL = api_url

    try:
        await ensure_backend(test_port, data_dir, api_url)
        await run_lifecycle(Path(temp_dir), api_url)
    finally:
        global started_process, daemon_log_file
        if started_process:
            print("\n[CLEANUP] Stopping Agentic Blackboard daemon process...")
            started_process.terminate()
            try:
                started_process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                started_process.kill()
            print("[CLEANUP] Daemon stopped cleanly.")
        if daemon_log_file:
            try:
                daemon_log_file.close()
            except Exception:
                pass
            print("[CLEANUP] Daemon log file closed.")
        shutil.rmtree(temp_dir, ignore_errors=True)
        print("[CLEANUP] Temporary test directory removed.")

    print("\n" + "=" * 70)
    print("[SUCCESS] Complete FAIR Data Management Lifecycle Verified!")
    print("=" * 70 + "\n")


if __name__ == "__main__":
    asyncio.run(main())
