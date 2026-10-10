#!/usr/bin/env python3
"""
Integration test for REST API Artifact Streaming & Metadata Endpoints.
Tests:
  1. Upload artifact with multipart form (file, path, license, avus)
  2. Retrieve metadata by UUID (GET /api/v1/artifacts/{uuid})
  3. Retrieve metadata by logical path (GET /api/v1/artifacts/{path})
  4. Retrieve full content (GET /api/v1/artifacts/{uuid}/content) + ETag check
  5. Retrieve partial content (Range: bytes=2-14) + HTTP 206 status & Content-Range
  6. Post metadata updates (POST /api/v1/artifacts/{uuid}/metadata) + verify updated AVUs
  7. Query artifacts by AVU (GET /api/v1/artifacts/query?attribute=...&value=...)
  8. Negative tests: 404 on unknown UUID and 400 on empty upload
"""

import sys
import os
import time
import json
import signal
import shutil
import subprocess
import requests

TEST_PORT = 18088
BASE_URL = f"http://127.0.0.1:{TEST_PORT}"
DB_PATH = "/tmp/ab_test_artifacts_py_db"
VAULT_PATH = f"{DB_PATH}_vault"
DAEMON_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "agentic-blackboardd"))

def cleanup():
    for p in [DB_PATH, VAULT_PATH, f"{DB_PATH}.wal", f"{DB_PATH}-shm", f"{DB_PATH}-wal"]:
        if os.path.exists(p):
            if os.path.isdir(p):
                shutil.rmtree(p, ignore_errors=True)
            else:
                try:
                    os.remove(p)
                except OSError:
                    pass

def wait_for_server(timeout=10.0):
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

def run_tests():
    cleanup()

    if not os.path.exists(DAEMON_PATH):
        raise FileNotFoundError(f"Daemon binary not found at: {DAEMON_PATH}")

    print(f"[TEST] Starting daemon: {DAEMON_PATH} on port {TEST_PORT}...")
    proc = subprocess.Popen(
        [DAEMON_PATH, f"--port={TEST_PORT}", f"--db={DB_PATH}", "--auth-mode=trusted_network"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    try:
        if not wait_for_server():
            stdout, stderr = proc.communicate(timeout=2)
            print(f"[FAIL] Server failed to start within timeout.\nStdout:\n{stdout}\nStderr:\n{stderr}")
            sys.exit(1)
        print("[TEST] Server is ready.")

        # --- Test 1: Upload artifact with multipart form ---
        print("\n--- Test 1: Upload Artifact ---")
        sample_content = "time,temp,pressure\n0,21.5,101.3\n1,22.0,101.2\n2,22.4,101.4\n"
        files = {
            "file": ("experiment_run.csv", sample_content, "text/csv")
        }
        initial_avus = [
            {"attribute": "policy:tier", "value": "hot", "units": ""},
            {"attribute": "dataset:domain", "value": "thermodynamics", "units": ""}
        ]
        data = {
            "path": "/lab/physics/experiment_run.csv",
            "license": "CC-BY-4.0",
            "avus": json.dumps(initial_avus)
        }
        res = requests.post(f"{BASE_URL}/api/v1/artifacts/upload", files=files, data=data)
        assert res.status_code == 200, f"Upload failed: {res.status_code} {res.text}"
        art = res.json()
        uuid = art["uuid"]
        pid = art["pid"]
        content_hash = art["content_hash"]
        assert uuid.startswith("art-"), f"Unexpected UUID: {uuid}"
        assert pid == "urn:ab:artifact:lab/physics/experiment_run.csv", f"Unexpected PID: {pid}"
        assert art["logical_name"] == "experiment_run.csv"
        assert art["collection_path"] == "/lab/physics"
        assert art["license"] == "CC-BY-4.0"
        assert art["byte_size"] == len(sample_content)
        assert art["content_hash"].startswith("sha256:")
        print(f"[PASS] 1. Uploaded artifact UUID={uuid}, PID={pid}, Hash={content_hash}")

        # --- Test 2: Retrieve metadata by UUID ---
        print("\n--- Test 2: Retrieve Metadata by UUID ---")
        res = requests.get(f"{BASE_URL}/api/v1/artifacts/{uuid}")
        assert res.status_code == 200, f"Get by UUID failed: {res.status_code} {res.text}"
        meta = res.json()
        assert meta["uuid"] == uuid
        assert meta["pid"] == pid
        assert meta["byte_size"] == len(sample_content)
        print("[PASS] 2. Metadata matches by UUID")

        # --- Test 3: Retrieve metadata by logical path ---
        print("\n--- Test 3: Retrieve Metadata by Logical Path ---")
        res = requests.get(f"{BASE_URL}/api/v1/artifacts/lab/physics/experiment_run.csv")
        assert res.status_code == 200, f"Get by relative path failed: {res.status_code} {res.text}"
        assert res.json()["uuid"] == uuid

        res = requests.get(f"{BASE_URL}/api/v1/artifacts//lab/physics/experiment_run.csv")
        assert res.status_code == 200, f"Get by absolute path failed: {res.status_code} {res.text}"
        assert res.json()["uuid"] == uuid

        res = requests.get(f"{BASE_URL}/api/v1/artifacts/experiment_run.csv")
        assert res.status_code == 200, f"Get by filename failed: {res.status_code} {res.text}"
        assert res.json()["uuid"] == uuid
        print("[PASS] 3. Metadata resolved by relative path, absolute path, and filename")

        # --- Test 4: Retrieve full content ---
        print("\n--- Test 4: Retrieve Full Content ---")
        res = requests.get(f"{BASE_URL}/api/v1/artifacts/{uuid}/content")
        assert res.status_code == 200, f"Content get failed: {res.status_code}"
        assert res.text == sample_content
        assert res.headers.get("ETag") == f'"{content_hash}"'
        assert res.headers.get("Accept-Ranges") == "bytes"
        assert "experiment_run.csv" in res.headers.get("Content-Disposition", "")
        print("[PASS] 4. Full content retrieved; ETag and headers verified")

        # --- Test 5: Retrieve partial content (HTTP Range) ---
        print("\n--- Test 5: Partial Content (Range: bytes=2-14) ---")
        headers = {"Range": "bytes=2-14"}
        res = requests.get(f"{BASE_URL}/api/v1/artifacts/{uuid}/content", headers=headers)
        assert res.status_code == 206, f"Expected 206 Partial Content, got {res.status_code}"
        expected_slice = sample_content[2:15] # bytes 2..14 inclusive (13 bytes)
        assert res.text == expected_slice, f"Slice mismatch: expected {expected_slice!r}, got {res.text!r}"
        assert res.headers.get("Content-Range") == f"bytes 2-14/{len(sample_content)}"
        assert res.headers.get("Accept-Ranges") == "bytes"
        assert res.headers.get("ETag") == f'"{content_hash}"'
        print("[PASS] 5. HTTP Range request verified: 206 Partial Content with exact slice")

        # --- Test 5b: Suffix Range Exceeding File Size (Range: bytes=-1000) ---
        print("\n--- Test 5b: Suffix Range Exceeding File Size (Range: bytes=-1000) ---")
        headers = {"Range": "bytes=-1000"}
        res = requests.get(f"{BASE_URL}/api/v1/artifacts/{uuid}/content", headers=headers)
        assert res.status_code == 206, f"Expected 206, got {res.status_code}"
        assert res.text == sample_content, f"Expected full content, got {res.text!r}"
        assert res.headers.get("Content-Range") == f"bytes 0-{len(sample_content)-1}/{len(sample_content)}"
        print("[PASS] 5b. Suffix range request verified (HTTP 206 with full content)")

        # --- Test 5c: Out-of-bounds Range (Range: bytes=9999-) ---
        print("\n--- Test 5c: Out-of-bounds Range (Range: bytes=9999-) ---")
        headers = {"Range": "bytes=9999-"}
        res = requests.get(f"{BASE_URL}/api/v1/artifacts/{uuid}/content", headers=headers)
        assert res.status_code == 416, f"Expected 416 Range Not Satisfiable, got {res.status_code}"
        assert res.headers.get("Content-Range") == f"bytes */{len(sample_content)}"
        print("[PASS] 5c. Out-of-bounds range request verified (HTTP 416)")

        # --- Test 6: Metadata endpoints GET & POST ---
        print("\n--- Test 6: Metadata GET & POST Updates ---")
        res = requests.get(f"{BASE_URL}/api/v1/artifacts/{uuid}/metadata")
        assert res.status_code == 200
        initial_meta = res.json()
        assert initial_meta["uuid"] == uuid
        assert isinstance(initial_meta["avus"], list)

        # Update AVUs via POST
        updated_avus_payload = {
            "avus": [
                {"attribute": "policy:tier", "value": "hot", "units": ""},
                {"attribute": "sensor:calibrated", "value": "true", "units": ""}
            ]
        }
        res = requests.post(f"{BASE_URL}/api/v1/artifacts/{uuid}/metadata", json=updated_avus_payload)
        assert res.status_code == 200, f"Metadata update failed: {res.status_code} {res.text}"
        post_resp = res.json()
        assert post_resp["status"] == "OK"

        # Verify update persisted
        res = requests.get(f"{BASE_URL}/api/v1/artifacts/{uuid}/metadata")
        assert res.status_code == 200
        cur_meta = res.json()
        avu_map = {a["attribute"]: a["value"] for a in cur_meta["avus"]}
        assert avu_map.get("policy:tier") == "hot"
        assert avu_map.get("sensor:calibrated") == "true"
        print("[PASS] 6. POST /api/v1/artifacts/{uuid}/metadata updated and persisted AVUs")

        # --- Test 7: Query artifacts by AVU ---
        print("\n--- Test 7: Query Artifacts by AVU ---")
        res = requests.get(f"{BASE_URL}/api/v1/artifacts/query?attribute=policy:tier&value=hot")
        assert res.status_code == 200, f"Query failed: {res.status_code} {res.text}"
        query_results = res.json()
        assert any(item["uuid"] == uuid for item in query_results), f"UUID {uuid} not in query results: {query_results}"

        res = requests.get(f"{BASE_URL}/api/v1/artifacts/query?attribute=sensor:calibrated&value=true")
        assert res.status_code == 200
        query_results = res.json()
        assert any(item["uuid"] == uuid for item in query_results)

        res = requests.get(f"{BASE_URL}/api/v1/artifacts/query?attribute=policy:tier&value=nonexistent_tier")
        assert res.status_code == 200
        assert len(res.json()) == 0
        print("[PASS] 7. Query artifacts by AVU verified")

        # --- Test 8: Negative tests ---
        print("\n--- Test 8: Negative Tests ---")
        res = requests.get(f"{BASE_URL}/api/v1/artifacts/unknown-artifact-uuid")
        assert res.status_code == 404, f"Expected 404, got {res.status_code}"

        res = requests.get(f"{BASE_URL}/api/v1/artifacts/unknown-artifact-uuid/content")
        assert res.status_code == 404, f"Expected 404, got {res.status_code}"

        res = requests.get(f"{BASE_URL}/api/v1/artifacts/unknown-artifact-uuid/metadata")
        assert res.status_code == 404, f"Expected 404, got {res.status_code}"

        res = requests.post(f"{BASE_URL}/api/v1/artifacts/unknown-artifact-uuid/metadata", json={"avus": []})
        assert res.status_code == 404, f"Expected 404, got {res.status_code}"

        # Upload with neither file nor body
        res = requests.post(f"{BASE_URL}/api/v1/artifacts/upload")
        assert res.status_code == 400, f"Expected 400, got {res.status_code}"
        print("[PASS] 8. Negative tests verified (404 and 400)")

        # --- Test 8b: Query non-artifact UUID (CPB atom) -> verify 404 ---
        print("\n--- Test 8b: Non-artifact Node Query (CPB atom) ---")
        bundle = {
            "atoms": [{
                "uuid": "atom-not-artifact-001",
                "project_id": "proj-default",
                "statement": "Statement of test atom"
            }]
        }
        res = requests.post(f"{BASE_URL}/api/v1/graph/bundle", json=bundle)
        assert res.status_code == 200, f"Bundle commit failed: {res.status_code} {res.text}"

        res = requests.get(f"{BASE_URL}/api/v1/artifacts/atom-not-artifact-001")
        assert res.status_code == 404, f"Expected 404 for CPB atom node, got {res.status_code}"
        res = requests.get(f"{BASE_URL}/api/v1/artifacts/atom-not-artifact-001/content")
        assert res.status_code == 404, f"Expected 404 for CPB atom content, got {res.status_code}"
        res = requests.get(f"{BASE_URL}/api/v1/artifacts/atom-not-artifact-001/metadata")
        assert res.status_code == 404, f"Expected 404 for CPB atom metadata, got {res.status_code}"
        print("[PASS] 8b. Query non-artifact UUID (CPB atom) verified as 404")

        # --- Test 9: Upload with form license: SPDX:Apache-2.0 ---
        print("\n--- Test 9: Upload with License & FAIR Score Verification ---")
        lic_files = {
            "target_path": (None, "models/resnet.pt"),
            "license": (None, "SPDX:Apache-2.0"),
            "file": ("resnet.pt", b"binary weights data", "application/octet-stream")
        }
        res = requests.post(f"{BASE_URL}/api/v1/artifacts/upload", files=lic_files)
        assert res.status_code == 200, f"Upload with license failed: {res.status_code} {res.text}"
        lic_json = res.json()
        assert lic_json["license"] == "SPDX:Apache-2.0"
        fair_score = None
        for a in lic_json["avus"]:
            if a["attribute"] == "fair:score":
                fair_score = int(a["value"])
        # Score: PID (+15) + content_hash (+15) + path/logical (+10) + license (+20) = 60
        assert fair_score == 60, f"Expected FAIR score 60 (+20 for license), got {fair_score}"
        print("[PASS] 9. Upload with license: SPDX:Apache-2.0 verified with +20 license points (score=60)")

        print("\n[SUCCESS] All Python REST API artifact integration tests passed successfully!")

    finally:
        print("\n[TEST] Stopping daemon gracefully...")
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
            print("[TEST] Daemon stopped cleanly.")
        except subprocess.TimeoutExpired:
            print("[WARN] Daemon did not stop within timeout, killing...")
            proc.kill()
            proc.wait()
        cleanup()

if __name__ == "__main__":
    run_tests()
