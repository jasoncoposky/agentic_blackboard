#!/usr/bin/env python3
"""
Integration test for Task 3: Shared Workspace Context & Multi-Surface SSE Event Sync
Connects Simulated Table and Simulated Tablet to SSE event streams,
tests surface registration, context state query, focus broadcasting (<50ms latency),
context-anchored atom commit notifications, multi-surface presence, and edge cases.
"""

import http.client
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

SERVER_PORT = int(os.environ.get("ASOS_PORT", 8085))
BASE_URL = f"http://127.0.0.1:{SERVER_PORT}"
CONTEXT_ID = "ctx:lab-42"


class SSEClient:
    def __init__(self, host: str, port: int, path: str, headers: dict = None):
        self.host = host
        self.port = port
        self.path = path
        self.headers = headers or {}
        self.events = queue.Queue()
        self.running = True
        self.connected_event = threading.Event()
        self.conn = None
        self.thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.thread.start()

    def _reader_loop(self):
        try:
            self.conn = http.client.HTTPConnection(self.host, self.port, timeout=10)
            req_headers = {"Accept": "text/event-stream"}
            req_headers.update(self.headers)
            self.conn.request("GET", self.path, headers=req_headers)
            res = self.conn.getresponse()
            if res.status != 200:
                self.events.put({"error": f"HTTP {res.status}", "status": res.status})
                self.connected_event.set()
                return

            self.connected_event.set()
            cur_event = "message"
            cur_data = ""

            while self.running:
                line = res.readline()
                if not line:
                    break
                line_str = line.decode("utf-8", errors="replace").rstrip("\r\n")
                if not line_str:
                    # Empty line dispatches event
                    if cur_data:
                        try:
                            parsed_data = json.loads(cur_data)
                        except Exception:
                            parsed_data = cur_data
                        self.events.put({
                            "event": cur_event,
                            "data": parsed_data,
                            "raw": cur_data,
                            "timestamp": time.perf_counter()
                        })
                        cur_event = "message"
                        cur_data = ""
                    continue

                if line_str.startswith(":"):
                    # SSE comment / ping
                    continue
                if line_str.startswith("event:"):
                    cur_event = line_str[6:].strip()
                elif line_str.startswith("data:"):
                    d = line_str[5:].strip()
                    if cur_data:
                        cur_data += "\n" + d
                    else:
                        cur_data = d
        except Exception as e:
            if self.running:
                self.events.put({"error": str(e)})
        finally:
            self.connected_event.set()

    def get_event(self, timeout: float = 3.0):
        return self.events.get(timeout=timeout)

    def close(self):
        self.running = False
        if self.conn:
            try:
                self.conn.close()
            except Exception:
                pass


def http_get(path: str, headers: dict = None):
    url = f"{BASE_URL}{path}"
    req = urllib.request.Request(url, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            body = resp.read().decode("utf-8")
            return resp.status, json.loads(body) if body else {}
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8")
        try:
            return e.code, json.loads(body) if body else {}
        except Exception:
            return e.code, body


def http_post(path: str, body: dict, headers: dict = None):
    url = f"{BASE_URL}{path}"
    data = json.dumps(body).encode("utf-8")
    req_headers = {"Content-Type": "application/json"}
    if headers:
        req_headers.update(headers)
    req = urllib.request.Request(url, data=data, headers=req_headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            resp_body = resp.read().decode("utf-8")
            try:
                return resp.status, json.loads(resp_body) if resp_body else {}
            except Exception:
                return resp.status, resp_body
    except urllib.error.HTTPError as e:
        resp_body = e.read().decode("utf-8")
        try:
            return e.code, json.loads(resp_body) if resp_body else {}
        except Exception:
            return e.code, resp_body


def start_server_if_needed():
    try:
        status, _ = http_get("/api/v1/schema")
        if status == 200:
            print(f"[Test] Found running ASOS server on port {SERVER_PORT}")
            return None, None
    except Exception:
        pass

    print(f"[Test] Starting new ASOS daemon on port {SERVER_PORT}...")
    temp_dir = tempfile.mkdtemp(prefix="asos_sync_test_")
    db_path = os.path.join(temp_dir, "db")

    # Seed anchors
    seed_cmd = ["./build/asos_daemon", "--seed", f"--db={db_path}"]
    subprocess.run(seed_cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Launch daemon
    daemon_proc = subprocess.Popen(
        ["./build/asos_daemon", "1", f"--db={db_path}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )

    # Wait for ready
    ready = False
    for _ in range(30):
        time.sleep(0.2)
        try:
            status, _ = http_get("/api/v1/schema")
            if status == 200:
                ready = True
                break
        except Exception:
            pass

    if not ready:
        daemon_proc.terminate()
        shutil.rmtree(temp_dir, ignore_errors=True)
        raise RuntimeError("Failed to start ASOS daemon within timeout.")

    print(f"[Test] ASOS daemon started (pid: {daemon_proc.pid})")
    return daemon_proc, temp_dir


def main():
    daemon_proc, temp_dir = None, None
    table_sse = None
    tablet_sse = None
    token_sse = None
    try:
        daemon_proc, temp_dir = start_server_if_needed()

        print("\n=== Phase 1: Connect Simulated Table & Tablet to SSE stream ===")
        events_path = f"/api/v1/events?context={urllib.parse.quote(CONTEXT_ID)}"
        table_sse = SSEClient("127.0.0.1", SERVER_PORT, events_path, headers={"X-Active-User": "admin"})
        tablet_sse = SSEClient("127.0.0.1", SERVER_PORT, events_path, headers={"X-Active-User": "admin"})

        assert table_sse.connected_event.wait(3.0), "Table SSE client failed to connect"
        assert tablet_sse.connected_event.wait(3.0), "Tablet SSE client failed to connect"

        # Check initial connected handshake
        evt_table_init = table_sse.get_event(timeout=2.0)
        assert "error" not in evt_table_init, f"Table SSE error: {evt_table_init}"
        assert evt_table_init["event"] == "connected", f"Expected connected event, got {evt_table_init}"
        assert evt_table_init["data"].get("context_id") == CONTEXT_ID

        evt_tablet_init = tablet_sse.get_event(timeout=2.0)
        assert "error" not in evt_tablet_init, f"Tablet SSE error: {evt_tablet_init}"
        assert evt_tablet_init["event"] == "connected", f"Expected connected event, got {evt_tablet_init}"
        assert evt_tablet_init["data"].get("context_id") == CONTEXT_ID
        print("  [PASS] Both Table and Tablet connected and received initial SSE handshake.")

        print("\n=== Phase 2: Surface Registration & Context State ===")
        reg_payload = {
            "context_id": CONTEXT_ID,
            "surface_id": "surface:table-01",
            "client_app": "spatial-canvas",
            "capabilities": {
                "touch": True,
                "display": "4k_interactive_table"
            }
        }
        status, reg_resp = http_post("/api/v1/context/register", reg_payload, headers={"X-Active-User": "admin"})
        assert status == 200, f"Register failed with status {status}: {reg_resp}"
        assert reg_resp.get("status") == "REGISTERED"
        assert reg_resp.get("context_id") == CONTEXT_ID
        assert reg_resp.get("surface_id") == "surface:table-01"

        # Both subscribers should receive surface_joined event
        evt_join = tablet_sse.get_event(timeout=2.0)
        assert evt_join["event"] == "surface_joined", f"Expected surface_joined, got {evt_join}"
        assert evt_join["data"].get("surface_id") == "surface:table-01"
        assert evt_join["data"].get("context_id") == CONTEXT_ID
        # Flush table_sse event as well
        table_sse.get_event(timeout=2.0)
        print("  [PASS] Surface registration returned 200 and broadcast 'surface_joined' event.")

        # Register a second surface (Tablet)
        reg_tablet = {
            "context_id": CONTEXT_ID,
            "surface_id": "surface:tablet-01",
            "client_app": "spatial-notes",
            "capabilities": {
                "stylus": True,
                "display": "retina_11in"
            }
        }
        status, reg_t_resp = http_post("/api/v1/context/register", reg_tablet, headers={"X-Active-User": "admin"})
        assert status == 200
        # Drain surface_joined events
        tablet_sse.get_event(timeout=2.0)
        table_sse.get_event(timeout=2.0)

        # Query context state
        status, ctx_state = http_get(f"/api/v1/context/{urllib.parse.quote(CONTEXT_ID)}", headers={"X-Active-User": "admin"})
        assert status == 200, f"Get context failed with status {status}: {ctx_state}"
        assert ctx_state.get("context_id") == CONTEXT_ID
        surfaces = ctx_state.get("active_surfaces", [])
        assert any(s.get("surface_id") == "surface:table-01" for s in surfaces), f"Surface table-01 not in {surfaces}"
        assert any(s.get("surface_id") == "surface:tablet-01" for s in surfaces), f"Surface tablet-01 not in {surfaces}"
        print("  [PASS] GET /api/v1/context/:id returns all active surfaces.")

        print("\n=== Phase 3: Real-Time Focus Update (<50ms Latency) ===")
        focus_payload = {
            "surface_id": "table-1",
            "selected": ["atom-1", "atom-2"],
            "telemetry": {
                "viewport": [0, 0, 1920, 1080],
                "zoom": 1.5
            }
        }

        t_send = time.perf_counter()
        status, focus_resp = http_post(f"/api/v1/context/{urllib.parse.quote(CONTEXT_ID)}/focus", focus_payload, headers={"X-Active-User": "admin"})
        assert status == 200, f"Focus update failed with status {status}: {focus_resp}"

        focus_evt = tablet_sse.get_event(timeout=2.0)
        t_recv = focus_evt["timestamp"]
        latency_ms = (t_recv - t_send) * 1000.0

        assert focus_evt["event"] == "focus_update", f"Expected focus_update event, got {focus_evt}"
        assert focus_evt["data"].get("selected") == ["atom-1", "atom-2"], f"Selected mismatch: {focus_evt['data']}"
        assert focus_evt["data"].get("surface_id") == "table-1", f"Surface mismatch: {focus_evt['data']}"
        print(f"  [PASS] Tablet received focus_update in {latency_ms:.2f}ms (threshold < 50ms).")
        assert latency_ms < 50.0, f"Focus update latency {latency_ms:.2f}ms exceeded 50ms threshold!"

        # Verify context state now has this focus
        status, ctx_state = http_get(f"/api/v1/context/{urllib.parse.quote(CONTEXT_ID)}", headers={"X-Active-User": "admin"})
        assert status == 200
        assert ctx_state.get("focus", {}).get("selected") == ["atom-1", "atom-2"]
        print("  [PASS] Context state reflects updated focus selection.")

        print("\n=== Phase 4: Atom Commit Context Broadcast ===")
        bundle_payload = {
            "project_id": "ALPHA_SWARM",
            "agent_id": "Nexus_Agent_7",
            "atoms": [
                {
                    "uuid": "atom-sync-ctx-test",
                    "statement": "Context-anchored atomic proposition for sync verification",
                    "context_id": CONTEXT_ID
                }
            ]
        }
        status, bundle_resp = http_post("/api/v1/graph/bundle", bundle_payload, headers={"X-Active-User": "admin"})
        assert status == 200, f"Bundle commit failed: {bundle_resp}"

        commit_evt = tablet_sse.get_event(timeout=2.0)
        assert commit_evt["event"] == "atom_committed", f"Expected atom_committed event, got {commit_evt}"
        assert commit_evt["data"].get("context_id") == CONTEXT_ID
        assert commit_evt["data"].get("uuid") == "atom-sync-ctx-test"
        print("  [PASS] Tablet received 'atom_committed' SSE event for atom committed to context.")

        print("\n=== Phase 5: Query Param Token & Validation Edge Cases ===")
        # Missing context param on /api/v1/events must return 400
        status_no_ctx, resp_no_ctx = http_get("/api/v1/events")
        assert status_no_ctx == 400, f"Expected 400 for missing context param, got {status_no_ctx}"
        print("  [PASS] GET /api/v1/events without context parameter returned 400 Bad Request.")

        # Non-existent context must return 404
        status_404, resp_404 = http_get("/api/v1/context/ctx:nonexistent_unknown_context")
        assert status_404 == 404, f"Expected 404 for unknown context, got {status_404}"
        print("  [PASS] GET /api/v1/context/:id for non-existent context returned 404 Not Found.")

        # Connect SSE with query token parameter
        token_path = f"/api/v1/events?context={urllib.parse.quote(CONTEXT_ID)}&token=test_query_token"
        token_sse = SSEClient("127.0.0.1", SERVER_PORT, token_path)
        assert token_sse.connected_event.wait(3.0)
        evt_tok_init = token_sse.get_event(timeout=2.0)
        assert evt_tok_init["event"] == "connected"
        print("  [PASS] SSE connection authenticated via '?token=...' query parameter.")

        print("\n[SUCCESS] Multi-Surface SSE Event Sync integration tests all passed!")

    finally:
        if table_sse:
            table_sse.close()
        if tablet_sse:
            tablet_sse.close()
        if token_sse:
            token_sse.close()
        if daemon_proc:
            print("[Test] Stopping test daemon...")
            daemon_proc.terminate()
            try:
                daemon_proc.wait(timeout=3)
            except Exception:
                daemon_proc.kill()
        if temp_dir:
            shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
