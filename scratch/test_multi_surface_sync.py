#!/usr/bin/env python3
"""
Integration test for Task 3: Shared Workspace Context & Multi-Surface SSE Event Sync
Connects Simulated Table and Simulated Tablet to SSE event streams,
tests surface registration, context state query, focus broadcasting (<50ms latency),
context-anchored atom commit notifications, multi-surface presence,
token authentication mode validation, and edge cases.
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

SERVER_PORT = int(os.environ.get("AB_PORT", 8085))
BASE_URL = f"http://127.0.0.1:{SERVER_PORT}"
CONTEXT_ID = "ctx:lab-42"
ADMIN_TOKEN = "ab_adm_0123456789abcdef0123456789abcdef"
CURATOR_TOKEN = "ab_usr_fedcba9876543210fedcba9876543210"


class SSEClient:
    def __init__(self, host: str, port: int, path: str, headers: dict = None):
        self.host = host
        self.port = port
        self.path = path
        self.headers = headers or {}
        self.events = queue.Queue()
        self.running = True
        self.connected_event = threading.Event()
        self.http_status = None
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
            self.http_status = res.status
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


def start_server_if_needed(auth_mode: str = "trusted_network"):
    try:
        status, _ = http_get("/api/v1/schema")
        if status == 200 and auth_mode == "trusted_network":
            print(f"[Test] Found running Agentic Blackboard server on port {SERVER_PORT}")
            return None, None
    except Exception:
        pass

    print(f"[Test] Starting new Agentic Blackboard daemon on port {SERVER_PORT} (auth_mode: {auth_mode})...")
    temp_dir = tempfile.mkdtemp(prefix="ab_sync_test_")
    db_path = os.path.join(temp_dir, "db")

    # Seed anchors & tokens
    seed_cmd = ["./build/agentic-blackboardd", "--seed", f"--db={db_path}"]
    subprocess.run(seed_cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Launch daemon
    daemon_proc = subprocess.Popen(
        ["./build/agentic-blackboardd", "1", f"--db={db_path}", f"--auth-mode={auth_mode}"],
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
        raise RuntimeError("Failed to start Agentic Blackboard daemon within timeout.")

    print(f"[Test] Agentic Blackboard daemon started (pid: {daemon_proc.pid})")
    return daemon_proc, temp_dir


def stop_server(daemon_proc, temp_dir):
    if daemon_proc:
        print("[Test] Stopping test daemon...")
        daemon_proc.terminate()
        try:
            daemon_proc.wait(timeout=3)
        except Exception:
            daemon_proc.kill()
    if temp_dir:
        shutil.rmtree(temp_dir, ignore_errors=True)


def test_standard_sync():
    daemon_proc, temp_dir = None, None
    table_sse = None
    tablet_sse = None
    token_sse = None
    try:
        daemon_proc, temp_dir = start_server_if_needed("trusted_network")

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
        assert focus_evt["data"].get("context_id") == CONTEXT_ID, f"Context mismatch: {focus_evt['data']}"
        print(f"  [PASS] Tablet received focus_update in {latency_ms:.2f}ms (threshold < 50ms).")
        assert latency_ms < 50.0, f"Focus update latency {latency_ms:.2f}ms exceeded 50ms threshold!"

        # Verify context state now has this focus with normalized fields
        status, ctx_state = http_get(f"/api/v1/context/{urllib.parse.quote(CONTEXT_ID)}", headers={"X-Active-User": "admin"})
        assert status == 200
        assert ctx_state.get("focus", {}).get("selected") == ["atom-1", "atom-2"]
        assert ctx_state.get("focus", {}).get("context_id") == CONTEXT_ID
        print("  [PASS] Context state reflects updated, normalized focus selection.")

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

    finally:
        if table_sse:
            table_sse.close()
        if tablet_sse:
            tablet_sse.close()
        if token_sse:
            token_sse.close()
        stop_server(daemon_proc, temp_dir)


def test_token_mode_auth():
    print("\n=== Phase 6: Token Auth Mode Enforcement & UUID Fallback ===")
    daemon_proc, temp_dir = start_server_if_needed(auth_mode="token")
    client_unauth = None
    client_bad_tok = None
    client_valid_tok = None
    try:
        ctx_tok = "ctx:token-workspace-99"

        # 1. Unauthenticated SSE connection must be rejected with 401
        path_unauth = f"/api/v1/events?context={urllib.parse.quote(ctx_tok)}"
        client_unauth = SSEClient("127.0.0.1", SERVER_PORT, path_unauth)
        assert client_unauth.connected_event.wait(3.0)
        assert client_unauth.http_status == 401, f"Expected 401 unauth, got {client_unauth.http_status}"
        print("  [PASS] SSE connection without token in token mode rejected with 401 Unauthorized.")

        # 2. Invalid token must be rejected with 401
        path_bad = f"/api/v1/events?context={urllib.parse.quote(ctx_tok)}&token=ab_usr_invalid_token"
        client_bad_tok = SSEClient("127.0.0.1", SERVER_PORT, path_bad)
        assert client_bad_tok.connected_event.wait(3.0)
        assert client_bad_tok.http_status == 401, f"Expected 401 bad token, got {client_bad_tok.http_status}"
        print("  [PASS] SSE connection with invalid ?token=... rejected with 401 Unauthorized.")

        # 3. Valid token query parameter must succeed (200) and establish SSE stream
        path_valid = f"/api/v1/events?context={urllib.parse.quote(ctx_tok)}&token={ADMIN_TOKEN}"
        client_valid_tok = SSEClient("127.0.0.1", SERVER_PORT, path_valid)
        assert client_valid_tok.connected_event.wait(3.0)
        assert client_valid_tok.http_status == 200, f"Expected 200 with valid token, got {client_valid_tok.http_status}"
        evt_init = client_valid_tok.get_event(timeout=2.0)
        assert evt_init["event"] == "connected"
        assert evt_init["data"].get("context_id") == ctx_tok
        print("  [PASS] SSE connection with valid ?token=... connected (200) and received handshake.")

        # 4. Context query before any surface registers must return 404 (phantom context not seeded)
        status, ctx_res = http_get(f"/api/v1/context/{urllib.parse.quote(ctx_tok)}?token={ADMIN_TOKEN}")
        assert status == 404, f"Expected 404 for un-registered context, got {status}: {ctx_res}"
        print("  [PASS] GET /api/v1/context/:id returned 404 before registration (no phantom context).")

        # 5. Surface registration with valid token
        reg_payload = {
            "context_id": ctx_tok,
            "surface_id": "surface:secure-wall",
            "client_app": "secure-canvas",
            "capabilities": {"security_level": "top_secret"}
        }
        status, reg_res = http_post(f"/api/v1/context/register?token={ADMIN_TOKEN}", reg_payload)
        assert status == 200
        join_evt = client_valid_tok.get_event(timeout=2.0)
        assert join_evt["event"] == "surface_joined"
        assert join_evt["data"]["surface_id"] == "surface:secure-wall"
        print("  [PASS] Registered surface with ?token=...; SSE received 'surface_joined'.")

        # 6. Context query now returns active surface
        status, ctx_res = http_get(f"/api/v1/context/{urllib.parse.quote(ctx_tok)}?token={ADMIN_TOKEN}")
        assert status == 200
        assert any(s["surface_id"] == "surface:secure-wall" for s in ctx_res["active_surfaces"])
        print("  [PASS] GET /api/v1/context/:id with token returned active surfaces.")

        # 7. Focus update with normalized payload
        focus_body = {
            "surface_id": "surface:secure-wall",
            "selected": ["atom-secure-1", "atom-secure-2"]
        }
        status, _ = http_post(f"/api/v1/context/{urllib.parse.quote(ctx_tok)}/focus?token={ADMIN_TOKEN}", focus_body)
        assert status == 200
        f_evt = client_valid_tok.get_event(timeout=2.0)
        assert f_evt["event"] == "focus_update"
        assert f_evt["data"]["selected"] == ["atom-secure-1", "atom-secure-2"]
        assert f_evt["data"]["context_id"] == ctx_tok
        assert f_evt["data"]["surface_id"] == "surface:secure-wall"
        print("  [PASS] Focus updated and broadcast with consistent normalized payload.")

        # 8. Atom commit without explicit UUID - verifies UUID auto-generation before broadcast
        anon_atom_bundle = {
            "project_id": "ALPHA_SWARM",
            "agent_id": "Nexus_Agent_7",
            "atoms": [
                {
                    "statement": "Anonymously identified atom with auto-generated UUID",
                    "context_id": ctx_tok
                }
            ]
        }
        status, bundle_res = http_post(f"/api/v1/graph/bundle?token={ADMIN_TOKEN}", anon_atom_bundle)
        assert status == 200, f"Bundle commit failed: {bundle_res}"
        atom_evt = client_valid_tok.get_event(timeout=2.0)
        assert atom_evt["event"] == "atom_committed"
        assert atom_evt["data"]["context_id"] == ctx_tok
        gen_uuid = atom_evt["data"].get("uuid", "")
        assert gen_uuid.startswith("atom-"), f"Expected auto-generated UUID starting with 'atom-', got: {gen_uuid}"
        print(f"  [PASS] Auto-generated UUID verified on commit broadcast: '{gen_uuid}'.")

        print("\n[SUCCESS] Token authentication mode and review polish tests passed!")

    finally:
        if client_unauth:
            client_unauth.close()
        if client_bad_tok:
            client_bad_tok.close()
        if client_valid_tok:
            client_valid_tok.close()
        stop_server(daemon_proc, temp_dir)


def main():
    test_standard_sync()
    test_token_mode_auth()
    print("\n[FINAL SUCCESS] All Task 3 integration and security tests passed!")


if __name__ == "__main__":
    main()
