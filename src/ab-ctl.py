#!/usr/bin/env python3
"""
ab-ctl: Agentic Blackboard (ab) Controller
Provides administrative operations, credentials initialization, surface registration,
shared context management, and an integrated Model Context Protocol (MCP) server runner.
"""

from __future__ import annotations

import argparse
import asyncio
import configparser
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid as uuid_mod

DEFAULT_CONFIG_PATHS = [
    Path("/etc/agentic-blackboard/blackboard.conf"),
    Path("/etc/agentic-blackboard/blackboard.conf.default"),
]
DEFAULT_DATA_DIR = "/var/lib/agentic-blackboard"
DEFAULT_CONNECT_URL = "http://localhost:8085"
TOKEN_SALT = "ab_salt_token_v1:"


def hash_token(token: str) -> str:
    """Hash token with Agentic Blackboard salt using SHA-256."""
    return hashlib.sha256((TOKEN_SALT + token).encode("utf-8")).hexdigest()


def load_config(config_path: str | None = None) -> dict:
    """Load configuration from config file or default locations supporting multiple INI sections."""
    cfg = {
        "connect": DEFAULT_CONNECT_URL,
        "data_dir": DEFAULT_DATA_DIR,
        "token": "",
        "auth_mode": "token",
    }

    target_file = None
    if config_path:
        p = Path(config_path)
        if p.is_file():
            target_file = p
    else:
        for p in DEFAULT_CONFIG_PATHS:
            if p.is_file():
                target_file = p
                break

    if target_file and target_file.is_file():
        try:
            parser = configparser.ConfigParser()
            content = target_file.read_text(encoding="utf-8")
            if not content.strip().startswith("["):
                content = "[default]\n" + content
            parser.read_string(content)
            for section in parser.sections():
                for k, v in parser.items(section):
                    k_lower = k.lower().replace("-", "_")
                    if k_lower in ("connect", "url", "api_url"):
                        cfg["connect"] = v.strip()
                    elif k_lower in ("data_dir", "datadir"):
                        cfg["data_dir"] = v.strip()
                    elif k_lower in ("token", "admin_token"):
                        cfg["token"] = v.strip()
                    elif k_lower in ("mode", "auth_mode", "authmode"):
                        cfg["auth_mode"] = v.strip()
                    elif k_lower == "port":
                        cfg["connect"] = f"http://localhost:{v.strip()}"
        except Exception:
            pass

    # Environment variable overrides
    if "AB_URL" in os.environ:
        cfg["connect"] = os.environ["AB_URL"]
    if "AB_TOKEN" in os.environ:
        cfg["token"] = os.environ["AB_TOKEN"]

    return cfg


def init_credentials_db(db_path: Path):
    """Initialize SQLite credentials database tables."""
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS tokens (
            token_hash TEXT PRIMARY KEY,
            token_prefix TEXT,
            username TEXT NOT NULL,
            role TEXT NOT NULL,
            token_type TEXT NOT NULL,
            project_id TEXT,
            metadata TEXT,
            created_at INTEGER NOT NULL
        );
    """)
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS users (
            username TEXT PRIMARY KEY,
            role TEXT NOT NULL,
            created_at INTEGER NOT NULL
        );
    """)
    conn.commit()
    conn.close()


def insert_token_to_db(db_path: Path, token: str, username: str, role: str,
                        token_type: str, project_id: str | None = None, metadata: dict | None = None):
    """Record token and user into SQLite credentials DB."""
    try:
        init_credentials_db(db_path)
        token_h = hash_token(token)
        prefix = token[:7] if len(token) >= 7 else ""
        meta_str = json.dumps(metadata or {})
        now = int(time.time())

        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()
        cursor.execute("""
            INSERT OR REPLACE INTO tokens (token_hash, token_prefix, username, role, token_type, project_id, metadata, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """, (token_h, prefix, username, role, token_type, project_id, meta_str, now))
        cursor.execute("""
            INSERT OR REPLACE INTO users (username, role, created_at)
            VALUES (?, ?, ?)
        """, (username, role, now))
        conn.commit()
        conn.close()
    except Exception as e:
        print(f"[WARN] Failed to write to credentials.db: {e}", file=sys.stderr)


def get_auth_headers(token: str | None, active_user: str | None = None, active_agent: str | None = None) -> dict:
    """Generate HTTP headers for authentication with optional dual-identity."""
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
        headers["X-AB-Key"] = token
    if active_user:
        headers["X-Active-User"] = active_user
    if active_agent:
        headers["X-Active-Agent"] = active_agent
    return headers


# ----------------------------------------------------------------------
# CLI Subcommand Handlers
# ----------------------------------------------------------------------

def handle_init(args, cfg: dict):
    """Generate directory structure, initialize credentials DB, create bootstrap admin token."""
    data_dir_str = args.data_dir or cfg.get("data_dir", DEFAULT_DATA_DIR)
    data_dir = Path(data_dir_str).resolve()
    data_dir.mkdir(parents=True, exist_ok=True)

    creds_db = data_dir / "credentials.db"
    init_credentials_db(creds_db)

    # Generate bootstrap admin token
    admin_token = f"ab_adm_{secrets.token_hex(16)}"
    insert_token_to_db(
        creds_db,
        token=admin_token,
        username="admin",
        role="admin",
        token_type="admin",
        metadata={"bootstrap": True}
    )

    # Write admin token file
    token_file = data_dir / "admin.token"
    try:
        token_file.write_text(f"{admin_token}\n", encoding="utf-8")
        token_file.chmod(0o600)
    except Exception as e:
        print(f"[WARN] Could not write token file {token_file}: {e}", file=sys.stderr)

    print(f"Initialized Agentic Blackboard substrate at {data_dir}")
    print(f"Created credentials database: {creds_db}")
    print(f"Admin token: {admin_token}")
    print(f"Admin token file written to: {token_file}")
    return 0


def handle_status(args, cfg: dict):
    """Query daemon health and operational status."""
    connect_url = (args.connect or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
    token = args.token or cfg.get("token")
    headers = get_auth_headers(token)

    # First attempt /api/v1/health
    health_url = f"{connect_url}/api/v1/health"
    try:
        req = urllib.request.Request(health_url, headers=headers)
        with urllib.request.urlopen(req, timeout=5.0) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            print(f"Status: {data.get('status', 'OPERATIONAL')}")
            if "cluster" in data:
                print(f"Cluster: {data['cluster']}")
            if "metrics" in data:
                m = data["metrics"]
                print(f"Metrics: Velocity={m.get('knowledge_velocity')}, Latency={m.get('sync_latency_ms')}ms, Toil={m.get('toil_ratio')}")
            return 0
    except urllib.error.HTTPError as e:
        if e.code == 404:
            # Substrate running but health node unseeded; probe /api/v1/schema
            schema_url = f"{connect_url}/api/v1/schema"
            try:
                s_req = urllib.request.Request(schema_url, headers=headers)
                with urllib.request.urlopen(s_req, timeout=5.0) as s_resp:
                    if s_resp.status == 200:
                        print("Status: OPERATIONAL (substrate active, health metrics uninitialized)")
                        return 0
            except Exception:
                pass
        err_body = e.read().decode("utf-8", errors="replace")
        print(f"Error checking status: HTTP {e.code} {e.reason}: {err_body}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"Status: UNREACHABLE (Failed to connect to {connect_url}: {e})", file=sys.stderr)
        return 1


def handle_user(args, cfg: dict):
    """User management subcommands (create, list)."""
    if args.user_action == "create":
        username = args.username
        role = args.role or "curator"
        user_token = f"ab_usr_{secrets.token_hex(16)}"
        connect_url = (args.connect or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
        admin_token = args.token or cfg.get("token")

        # Register token via REST API if daemon connect URL provided
        if connect_url:
            req_url = f"{connect_url}/api/v1/admin/users"
            payload = {
                "username": username,
                "role": role,
                "token": user_token
            }
            headers = get_auth_headers(admin_token)
            data_bytes = json.dumps(payload).encode("utf-8")
            try:
                req = urllib.request.Request(req_url, data=data_bytes, headers=headers, method="POST")
                with urllib.request.urlopen(req, timeout=5.0) as resp:
                    if resp.status not in (200, 201):
                        print(f"Error registering user on daemon: HTTP {resp.status}", file=sys.stderr)
                        return 1
            except urllib.error.HTTPError as e:
                err_body = e.read().decode("utf-8", errors="replace")
                print(f"Error registering user: HTTP {e.code} {e.reason}: {err_body}", file=sys.stderr)
                return 1
            except Exception as e:
                print(f"Error connecting to daemon at {req_url}: {e}", file=sys.stderr)
                return 1

        # Also store locally in credentials.db if available
        data_dir_str = args.data_dir or cfg.get("data_dir")
        if data_dir_str:
            db_path = Path(data_dir_str) / "credentials.db"
            if db_path.parent.exists():
                insert_token_to_db(db_path, user_token, username, role, "user")

        print(f"User created: {username} (Role: {role})")
        print(f"User token: {user_token}")
        return 0

    elif args.user_action == "list":
        # If --data-dir was NOT passed on CLI, query daemon API
        if not args.data_dir:
            connect_url = (args.connect or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
            admin_token = args.token or cfg.get("token")
            headers = get_auth_headers(admin_token)
            req_url = f"{connect_url}/api/v1/admin/users"
            try:
                req = urllib.request.Request(req_url, headers=headers)
                with urllib.request.urlopen(req, timeout=5.0) as resp:
                    data = json.loads(resp.read().decode("utf-8"))
                    print("Registered Users:")
                    for u in data.get("users", []):
                        print(f" - {u.get('username')} (Role: {u.get('role')})")
                    return 0
            except urllib.error.HTTPError as e:
                err_body = e.read().decode("utf-8", errors="replace")
                print(f"Error listing users from daemon: HTTP {e.code} {e.reason}: {err_body}", file=sys.stderr)
                return 1
            except Exception as e:
                print(f"Error connecting to daemon at {req_url}: {e}", file=sys.stderr)
                return 1

        # Otherwise read from credentials.db in provided data_dir
        data_dir_str = args.data_dir
        db_path = Path(data_dir_str) / "credentials.db"
        if db_path.is_file():
            conn = sqlite3.connect(db_path)
            cursor = conn.cursor()
            cursor.execute("SELECT username, role, created_at FROM users")
            rows = cursor.fetchall()
            conn.close()
            print("Registered Users:")
            for r in rows:
                print(f" - {r[0]} (Role: {r[1]}, Created: {r[2]})")
            return 0
        else:
            print(f"No credentials database found at {db_path}", file=sys.stderr)
            return 1

    return 0


def handle_agent(args, cfg: dict):
    """Agent proxy token creation and node registration."""
    if args.agent_action == "create":
        agent_name = args.agent_name
        username = args.user
        project_id = args.project
        agent_token = f"ab_agt_{secrets.token_hex(16)}"
        connect_url = (args.connect or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
        admin_token = args.token or cfg.get("token")

        if connect_url:
            # 1. Register agent token with daemon
            req_url = f"{connect_url}/api/v1/admin/users"
            payload = {
                "username": agent_name,
                "role": "agent",
                "token": agent_token,
                "user": username,
                "project": project_id
            }
            headers = get_auth_headers(admin_token)
            try:
                req = urllib.request.Request(req_url, data=json.dumps(payload).encode("utf-8"), headers=headers, method="POST")
                with urllib.request.urlopen(req, timeout=5.0) as resp:
                    if resp.status not in (200, 201):
                        print(f"Error registering agent token on daemon: HTTP {resp.status}", file=sys.stderr)
                        return 1
            except urllib.error.HTTPError as e:
                err_body = e.read().decode("utf-8", errors="replace")
                print(f"Error registering agent: HTTP {e.code} {e.reason}: {err_body}", file=sys.stderr)
                return 1
            except Exception as e:
                print(f"Error connecting to daemon at {req_url}: {e}", file=sys.stderr)
                return 1

            # 2. Ensure IDENTITY node in substrate graph
            node_url = f"{connect_url}/api/v1/graph/node"
            node_payload = {
                "type": "IDENTITY",
                "id": agent_name,
                "metadata": {
                    "description": f"Delegated Agent {agent_name} for user {username} on project {project_id}",
                    "status": "ACTIVE"
                }
            }
            try:
                req = urllib.request.Request(node_url, data=json.dumps(node_payload).encode("utf-8"), headers=headers, method="POST")
                with urllib.request.urlopen(req, timeout=5.0) as resp:
                    pass
            except Exception:
                pass

        # Also store locally in credentials.db if available
        data_dir_str = args.data_dir or cfg.get("data_dir")
        if data_dir_str:
            db_path = Path(data_dir_str) / "credentials.db"
            if db_path.parent.exists():
                insert_token_to_db(
                    db_path,
                    token=agent_token,
                    username=agent_name,
                    role="agent",
                    token_type="agent",
                    project_id=project_id,
                    metadata={"user": username, "project": project_id}
                )

        print(f"Agent created: {agent_name} (User: {username}, Project: {project_id})")
        print(f"Agent token: {agent_token}")
        return 0

    return 0


def handle_surface(args, cfg: dict):
    """Surface registration subcommand."""
    if args.surface_action == "register":
        name = args.name
        stype = args.type
        context_id = args.context
        connect_url = (args.connect or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
        token = args.token or cfg.get("token")

        cap = {"surface_type": stype}
        if getattr(args, "capabilities", None):
            try:
                cap.update(json.loads(args.capabilities))
            except Exception:
                pass

        payload = {
            "context_id": context_id,
            "surface_id": name,
            "client_app": getattr(args, "client_app", None) or name,
            "capabilities": cap
        }

        headers = get_auth_headers(token)
        req_url = f"{connect_url}/api/v1/context/register"
        data_bytes = json.dumps(payload).encode("utf-8")
        try:
            req = urllib.request.Request(req_url, data=data_bytes, headers=headers, method="POST")
            with urllib.request.urlopen(req, timeout=5.0) as resp:
                body = resp.read().decode("utf-8")
                print(f"Surface registered: {name} (Context: {context_id}, Status: {resp.status} REGISTERED)")
                print(body)
                return 0
        except urllib.error.HTTPError as e:
            err_body = e.read().decode("utf-8", errors="replace")
            print(f"Error registering surface: HTTP {e.code} {e.reason}: {err_body}", file=sys.stderr)
            return 1
        except Exception as e:
            print(f"Error connecting to {req_url}: {e}", file=sys.stderr)
            return 1

    return 0


def handle_context(args, cfg: dict):
    """Context state inspection and focus subcommands."""
    connect_url = (args.connect or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
    token = args.token or cfg.get("token")
    headers = get_auth_headers(token)

    if args.context_action == "show":
        context_id = args.context_id
        req_url = f"{connect_url}/api/v1/context/{urllib.parse.quote(context_id)}"
        try:
            req = urllib.request.Request(req_url, headers=headers)
            with urllib.request.urlopen(req, timeout=5.0) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                print(json.dumps(data, indent=2))
                return 0
        except urllib.error.HTTPError as e:
            err_body = e.read().decode("utf-8", errors="replace")
            print(f"Error querying context: HTTP {e.code} {e.reason}: {err_body}", file=sys.stderr)
            return 1
        except Exception as e:
            print(f"Error connecting to {req_url}: {e}", file=sys.stderr)
            return 1

    elif args.context_action == "focus":
        context_id = args.context_id
        selected = args.selected or []
        surface_id = getattr(args, "surface_id", None) or "ab-ctl"
        payload = {
            "selected": selected,
            "surface_id": surface_id
        }
        req_url = f"{connect_url}/api/v1/context/{urllib.parse.quote(context_id)}/focus"
        data_bytes = json.dumps(payload).encode("utf-8")
        try:
            req = urllib.request.Request(req_url, data=data_bytes, headers=headers, method="POST")
            with urllib.request.urlopen(req, timeout=5.0) as resp:
                body = resp.read().decode("utf-8")
                print(f"Focus updated for context {context_id}")
                print(body)
                return 0
        except urllib.error.HTTPError as e:
            err_body = e.read().decode("utf-8", errors="replace")
            print(f"Error updating focus: HTTP {e.code} {e.reason}: {err_body}", file=sys.stderr)
            return 1
        except Exception as e:
            print(f"Error connecting to {req_url}: {e}", file=sys.stderr)
            return 1

    return 0


# ----------------------------------------------------------------------
# Swarm Coordination & Core Lease Management Helpers and Handlers
# ----------------------------------------------------------------------

def http_request_json(url: str, method: str = "GET", payload: dict | list | None = None,
                      headers: dict | None = None, timeout: float = 10.0) -> tuple[int, dict | list | str]:
    """Execute HTTP request and return (status_code, parsed_body_or_raw_str)."""
    h = dict(headers or {})
    data_bytes = None
    if payload is not None:
        data_bytes = json.dumps(payload).encode("utf-8")
        if "Content-Type" not in h:
            h["Content-Type"] = "application/json"

    req = urllib.request.Request(url, data=data_bytes, headers=h, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_str = resp.read().decode("utf-8", errors="replace")
            try:
                return resp.status, json.loads(body_str)
            except Exception:
                return resp.status, body_str
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")
        try:
            return e.code, json.loads(err_body)
        except Exception:
            return e.code, err_body
    except Exception as e:
        return 0, str(e)


def resolve_swarm_headers(args, cfg: dict, active_user: str | None = None, active_agent: str | None = None) -> dict:
    """Resolve authentication and dual-identity headers."""
    token = getattr(args, "token", None) or cfg.get("token")
    user = (getattr(args, "active_user", None) or
            getattr(args, "user", None) or
            active_user or
            os.environ.get("AB_ACTIVE_USER") or
            os.environ.get("AB_USER") or
            "cpg-swarm-user")
    agent = (getattr(args, "active_agent", None) or
             getattr(args, "agent", None) or
             active_agent or
             os.environ.get("AB_ACTIVE_AGENT") or
             os.environ.get("AB_AGENT") or
             "cpg-swarm-agent")
    return get_auth_headers(token, active_user=user, active_agent=agent)


def commit_graph_node(connect_url: str, headers: dict, node_type: str, node_id: str, metadata: dict) -> tuple[bool, str]:
    """Commit or update a node in the blackboard substrate with bundle fallback."""
    node_url = f"{connect_url}/api/v1/graph/node"
    payload = {
        "type": node_type,
        "id": node_id,
        "metadata": metadata
    }
    status, res = http_request_json(node_url, method="POST", payload=payload, headers=headers)
    if status in (200, 201):
        if not (isinstance(res, dict) and res.get("status") == "EXISTS"):
            return True, ""

    # Fallback to /api/v1/graph/bundle for daemons requiring atomic bundle format or updating existing atoms
    bundle_url = f"{connect_url}/api/v1/graph/bundle"
    project_id = metadata.get("context_id") or "default-swarm"
    agent_id = metadata.get("agent_id") or headers.get("X-Active-Agent") or "cpg-swarm-agent"
    user_id = headers.get("X-Active-User") or "cpg-swarm-user"

    if "type" not in metadata:
        metadata = dict(metadata)
        metadata["type"] = node_type

    atom = {
        "uuid": node_id,
        "header": {
            "uuid": node_id,
            "origin": {
                "project_id": project_id,
                "agent_id": agent_id,
                "user_id": user_id,
                "context_id": metadata.get("context_id", ""),
            }
        },
        "payload": {
            "statement": metadata.get("name") or metadata.get("statement") or node_id,
            "content": json.dumps(metadata)
        },
        "attributes": {k: (v if isinstance(v, str) else json.dumps(v)) for k, v in metadata.items()}
    }
    b_payload = {
        "project_id": project_id,
        "agent_id": agent_id,
        "atoms": [atom]
    }
    b_status, b_res = http_request_json(bundle_url, method="POST", payload=b_payload, headers=headers)
    if b_status in (200, 201):
        return True, ""

    return False, str(res)


def fetch_graph_node(connect_url: str, headers: dict, node_id: str) -> tuple[int, dict | None]:
    """Fetch node details by ID."""
    url = f"{connect_url}/api/v1/node/{urllib.parse.quote(node_id)}"
    status, res = http_request_json(url, method="GET", headers=headers)
    if status == 200 and isinstance(res, dict):
        return 200, res
    return status, None


def extract_node_metadata(node_data: dict) -> dict:
    """Normalize metadata from node object."""
    if not node_data:
        return {}
    meta = node_data.get("metadata")
    if isinstance(meta, dict) and meta:
        meta_copy = dict(meta)
        if "status" not in meta_copy and "status" in node_data:
            meta_copy["status"] = node_data["status"]
        if "lease" not in meta_copy and "lease" in node_data:
            meta_copy["lease"] = node_data["lease"]
        if "context_id" not in meta_copy:
            meta_copy["context_id"] = node_data.get("context_id") or node_data.get("project", "")
        return meta_copy

    attributes = node_data.get("attributes", {})
    if isinstance(attributes, dict) and attributes:
        constructed = {}
        for k, v in attributes.items():
            try:
                constructed[k] = json.loads(v)
            except Exception:
                constructed[k] = v
        if "status" in node_data and "status" not in constructed:
            constructed["status"] = node_data["status"]
        if "lease" not in constructed and "lease" in node_data:
            constructed["lease"] = node_data["lease"]
        if "context_id" not in constructed:
            constructed["context_id"] = node_data.get("context_id") or node_data.get("project", "")
        if "status" in constructed:
            return constructed

    content = node_data.get("content", "")
    if isinstance(content, str) and content.strip().startswith("{"):
        try:
            parsed = json.loads(content)
            if isinstance(parsed, dict) and parsed:
                if "status" not in parsed and "status" in node_data:
                    parsed["status"] = node_data["status"]
                if "lease" not in parsed and "lease" in node_data:
                    parsed["lease"] = node_data["lease"]
                if "context_id" not in parsed:
                    parsed["context_id"] = node_data.get("context_id") or node_data.get("project", "")
                return parsed
        except Exception:
            pass

    return {
        "status": node_data.get("status", "READY"),
        "workflow": node_data.get("workflow", "feature"),
        "target_symbols": node_data.get("target_symbols", []),
        "lease": node_data.get("lease", {}),
        "depends_on": node_data.get("depends_on", []),
        "name": node_data.get("label") or node_data.get("statement") or node_data.get("id"),
        "context_id": node_data.get("context_id") or node_data.get("project", ""),
    }


def create_graph_link(connect_url: str, headers: dict, source: str, target: str, label: str, weight: float = 1.0) -> bool:
    """Create relationship edge between two nodes in substrate."""
    link_url = f"{connect_url}/api/v1/link"
    payload = {
        "source": source,
        "target": target,
        "label": label,
        "weight": weight
    }
    status, _ = http_request_json(link_url, method="POST", payload=payload, headers=headers)
    return status in (200, 201)


def fetch_node_links(connect_url: str, headers: dict, node_id: str, direction: str = "both") -> dict:
    """Fetch inbound and outbound links for a node."""
    links_url = f"{connect_url}/api/v1/node/{urllib.parse.quote(node_id)}/links?direction={direction}"
    status, res = http_request_json(links_url, method="GET", headers=headers)
    if status == 200 and isinstance(res, dict):
        return res
    return {"inbound": [], "outbound": []}


def handle_swarm_init(args, cfg: dict) -> int:
    """Register context and create initial requirement knowledge atom."""
    connect_url = (getattr(args, "connect", None) or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
    context_id = args.context
    name = args.name
    requirement = args.requirement
    headers = resolve_swarm_headers(args, cfg)

    # 1. Register context ID
    reg_url = f"{connect_url}/api/v1/context/register"
    reg_payload = {
        "context_id": context_id,
        "surface_id": "swarm-coordinator",
        "client_app": "ab-ctl-swarm",
        "capabilities": {
            "swarm_name": name,
            "surface_type": "orchestrator"
        }
    }
    st, res = http_request_json(reg_url, method="POST", payload=reg_payload, headers=headers)
    if st not in (200, 201):
        print(f"Error registering swarm context: HTTP {st}: {res}", file=sys.stderr)
        return 1

    # 2. Create initial requirement knowledge atom
    req_id = f"req-{context_id}"
    req_meta = {
        "name": name,
        "statement": requirement or f"Requirement for {name}",
        "description": requirement or f"Requirement for {name}",
        "context_id": context_id,
        "status": "PROPOSED",
        "created_at": int(time.time())
    }
    ok, err = commit_graph_node(connect_url, headers, "requirement", req_id, req_meta)
    if not ok:
        print(f"Error creating requirement atom: {err}", file=sys.stderr)
        return 1

    print(f"Swarm context initialized: {context_id} (Name: {name})")
    print(f"Requirement atom created: {req_id}")
    return 0


def handle_swarm_task_create(args, cfg: dict) -> int:
    """Create task atom and establish DEPENDS_ON links."""
    connect_url = (getattr(args, "connect", None) or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
    agent_id = getattr(args, "agent", None) or "cpg-architect"
    headers = resolve_swarm_headers(args, cfg, active_agent=agent_id)

    context_id = args.context
    name = args.name
    workflow = getattr(args, "workflow", "feature") or "feature"
    symbols_raw = getattr(args, "symbols", "") or ""
    depends_raw = getattr(args, "depends_on", "") or ""
    blast_radius = getattr(args, "blast_radius", 2)
    if blast_radius is None:
        blast_radius = 2

    symbols = [s.strip() for s in symbols_raw.split(",") if s.strip()]
    depends_on = [d.strip() for d in depends_raw.split(",") if d.strip()]

    # Determine task_id
    task_id = getattr(args, "id", None)
    if not task_id:
        if re.match(r"^[a-zA-Z0-9_\-]+$", name):
            task_id = name
        else:
            task_id = f"task-{secrets.token_hex(4)}"

    task_meta = {
        "type": "task",
        "name": name,
        "workflow": workflow,
        "target_symbols": symbols,
        "status": "READY",
        "context_id": context_id,
        "agent_id": agent_id,
        "blast_radius_k": blast_radius,
        "lease": {
            "holder": None,
            "expires_at": 0
        },
        "depends_on": depends_on,
        "created_at": int(time.time())
    }

    ok, err = commit_graph_node(connect_url, headers, "task", task_id, task_meta)
    if not ok:
        print(f"Error creating task node: {err}", file=sys.stderr)
        return 1

    # Create DEPENDS_ON edges to prerequisite tasks
    for dep_id in depends_on:
        if not create_graph_link(connect_url, headers, task_id, dep_id, "DEPENDS_ON"):
            print(f"Warning: Failed to create DEPENDS_ON link from {task_id} to {dep_id}", file=sys.stderr)

    print(f"Task created: {task_id} (Name: {name}, Status: READY)")
    if depends_on:
        print(f"Dependencies: {', '.join(depends_on)}")
    return 0


def handle_swarm_task_list(args, cfg: dict) -> int:
    """List tasks in a swarm context as table or JSON."""
    connect_url = (getattr(args, "connect", None) or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
    headers = resolve_swarm_headers(args, cfg)
    context_id = args.context
    output_format = getattr(args, "format", "table") or "table"

    tasks_map = {}

    def _is_task_node(item_id: str, item_data: dict, meta: dict) -> bool:
        if str(item_id).startswith(("req-", "sol-", "proof-", "counter-", "acc-")):
            return False
        node_type = item_data.get("type") or meta.get("type")
        if node_type == "requirement":
            return False
        return True

    def _extract_search_items(search_resp: dict) -> list:
        if not isinstance(search_resp, dict):
            return []
        items = search_resp.get("matches")
        if items is None:
            items = search_resp.get("results")
        if isinstance(items, list):
            return items
        return []

    # Query context endpoint
    ctx_url = f"{connect_url}/api/v1/context/{urllib.parse.quote(context_id)}"
    st, ctx_data = http_request_json(ctx_url, method="GET", headers=headers)
    if st == 200 and isinstance(ctx_data, dict):
        for item in ctx_data.get("tasks", []):
            tid = item.get("id") or item.get("uuid")
            if tid:
                meta = extract_node_metadata(item)
                if _is_task_node(tid, item, meta):
                    tasks_map[tid] = item
        for item in ctx_data.get("nodes", []):
            tid = item.get("id") or item.get("uuid")
            if tid:
                meta = extract_node_metadata(item)
                if _is_task_node(tid, item, meta) and (item.get("type") == "task" or "task" in tid.lower()):
                    tasks_map[tid] = item

    # Query search endpoint by context
    search_url = f"{connect_url}/api/v1/search?q={urllib.parse.quote(context_id)}"
    st, search_data = http_request_json(search_url, method="GET", headers=headers)
    if st == 200 and isinstance(search_data, dict):
        for item in _extract_search_items(search_data):
            tid = item.get("id") or item.get("uuid")
            if tid:
                meta = extract_node_metadata(item)
                item_ctx = meta.get("context_id") or item.get("context_id") or item.get("project")
                if (not item_ctx or item_ctx == context_id) and _is_task_node(tid, item, meta):
                    tasks_map[tid] = item

    # General search fallback if empty
    if not tasks_map:
        all_url = f"{connect_url}/api/v1/search?q="
        st, all_data = http_request_json(all_url, method="GET", headers=headers)
        if st == 200 and isinstance(all_data, dict):
            for item in _extract_search_items(all_data):
                tid = item.get("id") or item.get("uuid")
                if tid:
                    meta = extract_node_metadata(item)
                    item_ctx = meta.get("context_id") or item.get("context_id") or item.get("project")
                    if (not item_ctx or item_ctx == context_id) and _is_task_node(tid, item, meta):
                        tasks_map[tid] = item

    task_rows = []
    for tid, node in tasks_map.items():
        meta = extract_node_metadata(node)
        name = meta.get("name") or node.get("label") or node.get("statement") or tid
        status = meta.get("status", "READY")
        lease = meta.get("lease", {})
        holder = lease.get("holder") if isinstance(lease, dict) else None

        # Fetch links for dependencies and verdicts
        links_data = fetch_node_links(connect_url, headers, tid, direction="both")
        dep_names = list(meta.get("depends_on", []))
        verdicts = []
        for l in links_data.get("outbound", []):
            rel = l.get("relation", "")
            target = l.get("target") or l.get("uuid")
            if rel == "DEPENDS_ON" and target and target not in dep_names:
                dep_names.append(target)
            elif rel in ("VALIDATED_BY", "REFUTES", "ACCEPTS", "HAS_SOLUTION"):
                verdicts.append(f"{rel}:{target}")
        for l in links_data.get("inbound", []):
            rel = l.get("relation", "")
            source = l.get("source") or l.get("uuid")
            if rel in ("ACCEPTS",):
                verdicts.append(f"{rel}:{source}")

        dep_str = ", ".join(dep_names) if dep_names else ""
        verdict_str = ", ".join(verdicts) if verdicts else ""
        extra = []
        if dep_str:
            extra.append(f"depends: {dep_str}")
        if verdict_str:
            extra.append(verdict_str)
        extra_str = "; ".join(extra) if extra else "-"

        task_rows.append({
            "id": tid,
            "name": name,
            "status": status,
            "lease_holder": holder or "-",
            "dependencies": dep_names,
            "verdicts": verdicts,
            "details": extra_str,
            "metadata": meta
        })

    if output_format == "json":
        print(json.dumps(task_rows, indent=2))
        return 0

    if not task_rows:
        print(f"No tasks found for context: {context_id}")
        return 0

    col_id = max(max(len(r["id"]) for r in task_rows), 16)
    col_name = max(max(len(r["name"]) for r in task_rows), 20)
    col_status = max(max(len(r["status"]) for r in task_rows), 14)
    col_holder = max(max(len(r["lease_holder"]) for r in task_rows), 14)
    col_details = max(max(len(r["details"]) for r in task_rows), 25)

    header = f"{'ID':<{col_id}}  {'Name':<{col_name}}  {'Status':<{col_status}}  {'Lease Holder':<{col_holder}}  {'Dependencies / Verdicts':<{col_details}}"
    print(header)
    print("-" * len(header))
    for r in task_rows:
        print(f"{r['id']:<{col_id}}  {r['name']:<{col_name}}  {r['status']:<{col_status}}  {r['lease_holder']:<{col_holder}}  {r['details']:<{col_details}}")
    return 0


def handle_swarm_lease_claim(args, cfg: dict) -> int:
    """Verify prerequisites and claim an exclusive lease on a task."""
    connect_url = (getattr(args, "connect", None) or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
    headers = resolve_swarm_headers(args, cfg, active_agent=args.agent)
    task_id = args.task_id
    ttl = getattr(args, "ttl", None) or 600

    # 1. Fetch task node
    status, node_data = fetch_graph_node(connect_url, headers, task_id)
    if status != 200 or not node_data:
        print(f"Error: Task '{task_id}' not found (HTTP {status})", file=sys.stderr)
        return 1

    metadata = extract_node_metadata(node_data)

    current_status = metadata.get("status", "").upper()
    if current_status in ("COMPLETED", "VALIDATED"):
        print(f"[BLOCKED] Cannot claim task {task_id}: task is already {current_status}", file=sys.stderr)
        return 1

    # 2. Fetch links to find prerequisites
    links_data = fetch_node_links(connect_url, headers, task_id, direction="both")
    prereq_ids = set(metadata.get("depends_on", []))
    for l in links_data.get("outbound", []):
        if l.get("relation", "").upper() == "DEPENDS_ON":
            dep = l.get("target") or l.get("uuid")
            if dep and dep != task_id:
                prereq_ids.add(dep)

    # 3. Verify all prerequisites have COMPLETED or VALIDATED status
    for dep_id in prereq_ids:
        dep_st, dep_node = fetch_graph_node(connect_url, headers, dep_id)
        if dep_st != 200 or not dep_node:
            print(f"[BLOCKED] Prerequisite {dep_id} not found in blackboard", file=sys.stderr)
            return 1
        dep_meta = extract_node_metadata(dep_node)
        dep_status = dep_meta.get("status", "UNKNOWN").upper()
        if dep_status not in ("COMPLETED", "VALIDATED"):
            print(f"[BLOCKED] Prerequisite {dep_id} not validated (Current status: {dep_status})", file=sys.stderr)
            return 1

    # 4. Check lease status
    lease = metadata.get("lease") or {}
    holder = lease.get("holder")
    expires_at = lease.get("expires_at") or 0
    now = time.time()

    if holder and holder != args.agent and expires_at > now:
        print(f"[BLOCKED] Task {task_id} is already leased by agent '{holder}' until {int(expires_at)}", file=sys.stderr)
        return 1

    # 5. Unblocked: claim lease
    metadata["status"] = "IN_PROGRESS"
    metadata["lease"] = {
        "holder": args.agent,
        "expires_at": int(now + ttl)
    }

    ok, err = commit_graph_node(connect_url, headers, "task", task_id, metadata)
    if not ok:
        print(f"Error updating task lease: {err}", file=sys.stderr)
        return 1

    print(f"Lease claimed for task {task_id} by agent {args.agent} (TTL: {ttl}s, Status: IN_PROGRESS)")
    return 0


def handle_swarm_lease_release(args, cfg: dict) -> int:
    """Release active lease on a task and return it to READY state."""
    connect_url = (getattr(args, "connect", None) or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
    headers = resolve_swarm_headers(args, cfg, active_agent=args.agent)
    task_id = args.task_id

    status, node_data = fetch_graph_node(connect_url, headers, task_id)
    if status != 200 or not node_data:
        print(f"Error: Task '{task_id}' not found (HTTP {status})", file=sys.stderr)
        return 1

    metadata = extract_node_metadata(node_data)
    lease = metadata.get("lease") or {}
    holder = lease.get("holder")
    expires_at = lease.get("expires_at") or 0
    now = time.time()

    if holder and holder != args.agent and expires_at > now:
        print(f"[BLOCKED] Cannot release lease: Task {task_id} is leased by different agent '{holder}'", file=sys.stderr)
        return 1

    metadata["status"] = "READY"
    metadata["lease"] = {
        "holder": None,
        "expires_at": 0
    }

    ok, err = commit_graph_node(connect_url, headers, "task", task_id, metadata)
    if not ok:
        print(f"Error releasing lease: {err}", file=sys.stderr)
        return 1

    print(f"Lease released for task {task_id} by agent {args.agent} (Status: READY)")
    return 0


def handle_swarm_review_submit(args, cfg: dict) -> int:
    """Create solution atom, link via HAS_SOLUTION, and mark task REVIEW_PENDING."""
    connect_url = (getattr(args, "connect", None) or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
    headers = resolve_swarm_headers(args, cfg, active_agent=args.agent)
    task_id = args.task_id
    patch_summary = args.patch or "Solution patch submitted"

    status, node_data = fetch_graph_node(connect_url, headers, task_id)
    if status != 200 or not node_data:
        print(f"Error: Task '{task_id}' not found (HTTP {status})", file=sys.stderr)
        return 1

    task_meta = extract_node_metadata(node_data)
    context_id = task_meta.get("context_id", "")

    current_status = task_meta.get("status")
    if current_status != "IN_PROGRESS":
        print(f"[BLOCKED] Cannot submit review for task {task_id}: status is '{current_status}', expected 'IN_PROGRESS'", file=sys.stderr)
        return 1

    lease_holder = (task_meta.get("lease") or {}).get("holder")
    if lease_holder and lease_holder != args.agent:
        print(f"[BLOCKED] Cannot submit review: Task is leased by '{lease_holder}', not '{args.agent}'", file=sys.stderr)
        return 1

    # 1. Create solution atom
    solution_id = f"sol-{task_id}-{secrets.token_hex(4)}"
    sol_meta = {
        "task_id": task_id,
        "agent_id": args.agent,
        "patch": patch_summary,
        "summary": patch_summary,
        "created_at": int(time.time()),
        "context_id": context_id
    }
    ok, err = commit_graph_node(connect_url, headers, "solution", solution_id, sol_meta)
    if not ok:
        print(f"Error creating solution atom: {err}", file=sys.stderr)
        return 1

    # 2. Link task -> solution via HAS_SOLUTION
    if not create_graph_link(connect_url, headers, task_id, solution_id, "HAS_SOLUTION"):
        print(f"Warning: Failed to create HAS_SOLUTION link from {task_id} to {solution_id}", file=sys.stderr)

    # 3. Update task status = REVIEW_PENDING
    task_meta["status"] = "REVIEW_PENDING"
    ok, err = commit_graph_node(connect_url, headers, "task", task_id, task_meta)
    if not ok:
        print(f"Error updating task status: {err}", file=sys.stderr)
        return 1

    print(f"Review submitted for task {task_id} by agent {args.agent} (Solution: {solution_id}, Status: REVIEW_PENDING)")
    return 0


def handle_swarm_review_verdict(args, cfg: dict) -> int:
    """Record verification proof or counterexample trace, linking VALIDATED_BY or REFUTES."""
    connect_url = (getattr(args, "connect", None) or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
    headers = resolve_swarm_headers(args, cfg, active_agent=args.verifier)
    task_id = args.task_id
    verdict = args.verdict.upper()

    status, node_data = fetch_graph_node(connect_url, headers, task_id)
    if status != 200 or not node_data:
        print(f"Error: Task '{task_id}' not found (HTTP {status})", file=sys.stderr)
        return 1

    details = args.details
    details_val = {}
    if details:
        try:
            details_val = json.loads(details)
        except Exception:
            details_val = {"raw": details}

    task_meta = extract_node_metadata(node_data)
    current_status = task_meta.get("status")
    if current_status != "REVIEW_PENDING":
        print(f"[BLOCKED] Cannot record verdict for task {task_id}: status is '{current_status}', expected 'REVIEW_PENDING'", file=sys.stderr)
        return 1
    context_id = task_meta.get("context_id", "")

    if verdict == "PASS":
        proof_id = f"proof-{task_id}-{secrets.token_hex(4)}"
        proof_meta = {
            "task_id": task_id,
            "verifier_agent": args.verifier,
            "verdict": "PASS",
            "details": details_val,
            "context_id": context_id,
            "created_at": int(time.time())
        }
        ok, err = commit_graph_node(connect_url, headers, "verification_proof", proof_id, proof_meta)
        if not ok:
            print(f"Error creating verification proof atom: {err}", file=sys.stderr)
            return 1

        if not create_graph_link(connect_url, headers, task_id, proof_id, "VALIDATED_BY"):
            print(f"Warning: Failed to create VALIDATED_BY link from {task_id} to {proof_id}", file=sys.stderr)

        task_meta["status"] = "VALIDATED"
        if "lease" in task_meta and isinstance(task_meta["lease"], dict):
            task_meta["lease"]["holder"] = None
            task_meta["lease"]["expires_at"] = 0
        else:
            task_meta["lease"] = {"holder": None, "expires_at": 0}

        ok, err = commit_graph_node(connect_url, headers, "task", task_id, task_meta)
        if not ok:
            print(f"Error updating task status: {err}", file=sys.stderr)
            return 1

        print(f"Verdict recorded for task {task_id}: PASS by {args.verifier} (Proof: {proof_id}, Status: VALIDATED)")
        return 0

    elif verdict == "FAIL":
        counter_id = f"counter-{task_id}-{secrets.token_hex(4)}"
        counter_meta = {
            "task_id": task_id,
            "verifier_agent": args.verifier,
            "verdict": "FAIL",
            "details": details_val,
            "context_id": context_id,
            "created_at": int(time.time())
        }
        ok, err = commit_graph_node(connect_url, headers, "counterexample_trace", counter_id, counter_meta)
        if not ok:
            print(f"Error creating counterexample trace atom: {err}", file=sys.stderr)
            return 1

        if not create_graph_link(connect_url, headers, task_id, counter_id, "REFUTES"):
            print(f"Warning: Failed to create REFUTES link from {task_id} to {counter_id}", file=sys.stderr)

        task_meta["status"] = "READY"
        if "lease" in task_meta and isinstance(task_meta["lease"], dict):
            task_meta["lease"]["holder"] = None
            task_meta["lease"]["expires_at"] = 0
        else:
            task_meta["lease"] = {"holder": None, "expires_at": 0}

        ok, err = commit_graph_node(connect_url, headers, "task", task_id, task_meta)
        if not ok:
            print(f"Error updating task status: {err}", file=sys.stderr)
            return 1

        print(f"Verdict recorded for task {task_id}: FAIL by {args.verifier} (Refutation: {counter_id}, Status: READY)")
        return 0

    else:
        print(f"Error: Invalid verdict '{args.verdict}'. Must be PASS or FAIL.", file=sys.stderr)
        return 1


def handle_swarm_accept(args, cfg: dict) -> int:
    """Record stakeholder acceptance and mark task COMPLETED."""
    connect_url = (getattr(args, "connect", None) or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
    headers = resolve_swarm_headers(args, cfg, active_user=args.stakeholder)
    task_id = args.task_id

    status, node_data = fetch_graph_node(connect_url, headers, task_id)
    if status != 200 or not node_data:
        print(f"Error: Task '{task_id}' not found (HTTP {status})", file=sys.stderr)
        return 1

    task_meta = extract_node_metadata(node_data)
    task_status = task_meta.get("status")
    if task_status != "VALIDATED":
        print(f"[BLOCKED] Cannot accept task {task_id}: status is '{task_status}', expected 'VALIDATED'", file=sys.stderr)
        return 1

    context_id = task_meta.get("context_id", "")

    acc_id = f"acc-{task_id}-{secrets.token_hex(4)}"
    acc_meta = {
        "task_id": task_id,
        "stakeholder": args.stakeholder,
        "notes": args.notes or "Accepted by stakeholder",
        "context_id": context_id,
        "created_at": int(time.time())
    }
    ok, err = commit_graph_node(connect_url, headers, "acceptance", acc_id, acc_meta)
    if not ok:
        print(f"Error creating acceptance record: {err}", file=sys.stderr)
        return 1

    if not create_graph_link(connect_url, headers, task_id, acc_id, "ACCEPTS"):
        print(f"Warning: Failed to create ACCEPTS link from {task_id} to {acc_id}", file=sys.stderr)

    task_meta["status"] = "COMPLETED"
    ok, err = commit_graph_node(connect_url, headers, "task", task_id, task_meta)
    if not ok:
        print(f"Error updating task status: {err}", file=sys.stderr)
        return 1

    print(f"Task {task_id} accepted by {args.stakeholder} (Acceptance: {acc_id}, Status: COMPLETED)")
    return 0


def handle_swarm(args, cfg: dict) -> int:
    """Dispatcher for swarm subcommands."""
    action = getattr(args, "swarm_action", None)
    if action == "init":
        return handle_swarm_init(args, cfg)
    elif action == "task":
        task_action = getattr(args, "task_action", None)
        if task_action == "create":
            return handle_swarm_task_create(args, cfg)
        elif task_action == "list":
            return handle_swarm_task_list(args, cfg)
    elif action == "lease":
        lease_action = getattr(args, "lease_action", None)
        if lease_action == "claim":
            return handle_swarm_lease_claim(args, cfg)
        elif lease_action == "release":
            return handle_swarm_lease_release(args, cfg)
    elif action == "review":
        review_action = getattr(args, "review_action", None)
        if review_action == "submit":
            return handle_swarm_review_submit(args, cfg)
        elif review_action == "verdict":
            return handle_swarm_review_verdict(args, cfg)
    elif action == "accept":
        return handle_swarm_accept(args, cfg)

    print(f"Unknown swarm action: {action}", file=sys.stderr)
    return 1


# ----------------------------------------------------------------------
# Integrated FastMCP Server Runner (with deferred imports)
# ----------------------------------------------------------------------

def build_mcp_server(connect_url: str, token: str | None):
    """Instantiate and configure FastMCP server forwarding tool calls to Agentic Blackboard REST API."""
    try:
        import httpx
        from mcp.server.fastmcp import FastMCP
    except ImportError:
        print("[ERROR] MCP dependencies missing. Install via: pip install httpx mcp", file=sys.stderr)
        return 1

    try:
        import ab_mcp_server
    except ImportError:
        pass

    mcp = FastMCP("agentic-blackboard")
    api_url = f"{connect_url.rstrip('/')}/api/v1"
    headers = {}
    if token:
        headers["Authorization"] = f"Bearer {token}"
        headers["X-AB-Key"] = token

    client_timeout = httpx.Timeout(10.0, connect=3.0)

    @mcp.tool()
    async def ensure_node(
        type: str,
        id: str,
        description: str = "",
        status: str = "ACTIVE",
        content: str = "",
        active_user: str | None = None
    ) -> str:
        """Idempotently ensure an anchor node (PROJECT or IDENTITY) exists in the Agentic Blackboard substrate."""
        payload = {
            "type": type,
            "id": id,
            "metadata": {
                "description": description,
                "status": status,
                "content": content
            }
        }
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.post(f"{api_url}/graph/node", json=payload, headers=req_h)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return resp.text

    @mcp.tool()
    async def commit_knowledge_bundle(
        project_id: str,
        agent_id: str,
        atoms: list[dict],
        active_user: str | None = None
    ) -> str:
        """Commit a bundle of knowledge atoms to the substrate anchored to a project and agent."""
        payload = {
            "project_id": project_id,
            "agent_id": agent_id,
            "atoms": atoms
        }
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.post(f"{api_url}/graph/bundle", json=payload, headers=req_h)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return resp.text

    @mcp.tool()
    async def search_commonplace(
        query: str,
        ka: int | None = None,
        tags: list[str] | None = None,
        limit: int = 10,
        active_user: str | None = None
    ) -> str:
        """Search the Agentic Blackboard Commonplace Book for notes and concepts."""
        params = {"q": query, "limit": limit}
        if ka is not None:
            params["ka"] = ka
        if tags:
            params["tags"] = ",".join(tags)
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.get(f"{api_url}/search", params=params, headers=req_h)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return resp.text

    @mcp.tool()
    async def get_node(uuid: str, active_user: str | None = None) -> str:
        """Retrieve full hydrated atom or node by UUID from the substrate."""
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.get(f"{api_url}/node/{uuid}", headers=req_h)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return resp.text

    @mcp.tool()
    async def get_node_links(uuid: str, direction: str = "both", active_user: str | None = None) -> str:
        """Query synapses (inbound and/or outbound links) connected to a node."""
        params = {"direction": direction}
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.get(f"{api_url}/node/{uuid}/links", params=params, headers=req_h)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return resp.text

    @mcp.tool()
    async def create_note(
        project_id: str,
        agent_id: str,
        statement: str,
        content: str = "",
        references: list[dict] | None = None,
        note_links: list[dict] | None = None,
        tags: list[str] | None = None,
        ka: int | None = 31,
        uuid: str | None = None,
        active_user: str | None = None
    ) -> str:
        """Create a knowledge note atom in the Agentic Blackboard substrate."""
        node_uuid = uuid if uuid else f"note-{uuid_mod.uuid4().hex[:8]}"
        atom = {
            "statement": statement,
            "content": content,
            "ka": ka if ka is not None else 31,
            "tags": tags or [],
            "uuid": node_uuid,
        }
        if references:
            atom["references"] = references
        if note_links:
            atom["note_links"] = note_links

        payload = {
            "project_id": project_id,
            "agent_id": agent_id,
            "atoms": [atom]
        }
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.post(f"{api_url}/graph/bundle", json=payload, headers=req_h)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return json.dumps({"status": "COMMITTED", "uuid": node_uuid, "message": resp.text})

    @mcp.tool()
    async def create_catalog_entry(
        project_id: str,
        agent_id: str,
        statement: str,
        content: str = "",
        items: list[dict] | None = None,
        steps: list[dict] | None = None,
        metrics: list[dict] | None = None,
        attributes: dict | None = None,
        tags: list[str] | None = None,
        ka: int | None = 27,
        uuid: str | None = None,
        active_user: str | None = None
    ) -> str:
        """Create a structured catalog entry (recipe, protocol, runbook, or inventory) in the substrate."""
        node_uuid = uuid if uuid else f"catalog-{uuid_mod.uuid4().hex[:8]}"
        atom = {
            "statement": statement,
            "content": content,
            "ka": ka if ka is not None else 27,
            "tags": tags or [],
            "uuid": node_uuid,
        }
        if items:
            atom["items"] = items
        if steps:
            atom["steps"] = steps
        if metrics:
            atom["metrics"] = metrics
        if attributes:
            atom["attributes"] = attributes

        payload = {
            "project_id": project_id,
            "agent_id": agent_id,
            "atoms": [atom]
        }
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.post(f"{api_url}/graph/bundle", json=payload, headers=req_h)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return json.dumps({"status": "COMMITTED", "uuid": node_uuid, "message": resp.text})

    @mcp.tool()
    async def link_nodes(
        source: str,
        target: str,
        label: str,
        weight: float = 1.0,
        active_user: str | None = None
    ) -> str:
        """Create a labeled relationship (synapse) between two nodes."""
        payload = {
            "source": source,
            "target": target,
            "label": label,
            "weight": weight
        }
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.post(f"{api_url}/link", json=payload, headers=req_h)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return resp.text

    @mcp.tool()
    async def query_substrate(
        match_alias: str = "n",
        where_eq: dict | None = None,
        active_user: str | None = None
    ) -> str:
        """Query substrate for nodes matching specific criteria."""
        payload = {
            "match": match_alias,
            "where_eq": where_eq or {}
        }
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.post(f"{api_url}/query", json=payload, headers=req_h)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return resp.text

    @mcp.tool()
    async def spawn_widget(
        atom_id: str,
        behavior: str,
        label: str = "",
        active_user: str | None = None
    ) -> str:
        """Propose a Nucleus spatial widget for an Agentic Blackboard Knowledge Atom."""
        payload = {
            "atom_id": atom_id,
            "behavior": behavior,
            "label": label
        }
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.post(f"{api_url}/nucleus/materialize", json=payload, headers=req_h)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return resp.text

    @mcp.tool()
    async def bind_anchor_to_atom(
        marker_id: str,
        atom_id: str,
        device_id: str = "default",
        active_user: str | None = None
    ) -> str:
        """Bind a physical fiducial marker to an Agentic Blackboard Knowledge Atom."""
        payload = {
            "marker_id": marker_id,
            "atom_id": atom_id,
            "device_id": device_id
        }
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.post(f"{api_url}/nucleus/bind", json=payload, headers=req_h)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return resp.text

    @mcp.tool()
    async def export_graph_rdf(active_user: str | None = None) -> str:
        """Export the substrate knowledge graph as W3C RDF Turtle text."""
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=httpx.Timeout(30.0)) as client:
            resp = await client.get(f"{api_url}/graph/export", headers=req_h)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return resp.text

    @mcp.tool()
    async def inspect_blackboard_health(active_user: str | None = None) -> str:
        """Query operational health, sync latency, and toil metrics of the blackboard substrate."""
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.get(f"{api_url}/health", headers=req_h)
            if resp.is_error:
                s_resp = await client.get(f"{api_url}/schema", headers=req_h)
                if not s_resp.is_error:
                    return json.dumps({"status": "OPERATIONAL", "message": "Substrate online; health metrics pending initialization."})
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return resp.text

    @mcp.tool()
    async def verify_atom_integrity(uuid: str, active_user: str | None = None) -> str:
        """Verify knowledge atom existence, structural integrity, and schema compliance."""
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.get(f"{api_url}/node/{uuid}", headers=req_h)
            if resp.status_code == 404:
                return json.dumps({"status": "NOT_FOUND", "uuid": uuid})
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            data = resp.json()
            has_origin = "origin" in data or "metadata" in data or "header" in data
            return json.dumps({
                "status": "VALID" if has_origin else "MALFORMED",
                "uuid": uuid,
                "data": data
            })

    @mcp.tool()
    async def fetch_identity_provenance(atom_id: str, active_user: str | None = None) -> str:
        """Fetch multi-surface origin provenance, author identity, and creation lineage for an atom."""
        req_h = dict(headers)
        if active_user:
            req_h["X-Active-User"] = active_user
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            node_resp = await client.get(f"{api_url}/node/{atom_id}", headers=req_h)
            links_resp = await client.get(f"{api_url}/node/{atom_id}/links?direction=both", headers=req_h)
            node_data = node_resp.json() if node_resp.status_code == 200 else {}
            links_data = links_resp.json() if links_resp.status_code == 200 else {}
            return json.dumps({
                "atom_id": atom_id,
                "node": node_data,
                "links": links_data
            })

    def _make_headers(active_user: str | None = None, active_agent: str | None = None) -> dict:
        h = dict(headers)
        if active_user:
            h["X-Active-User"] = active_user
        if active_agent:
            h["X-Active-Agent"] = active_agent
        return h

    async def _async_commit_graph_node(client: httpx.AsyncClient, node_type: str, node_id: str, metadata: dict, req_headers: dict) -> tuple[bool, str]:
        node_url = f"{api_url}/graph/node"
        payload = {
            "type": node_type,
            "id": node_id,
            "metadata": metadata
        }
        try:
            resp = await client.post(node_url, json=payload, headers=req_headers)
            if resp.status_code in (200, 201):
                try:
                    res_json = resp.json()
                    if not (isinstance(res_json, dict) and res_json.get("status") == "EXISTS"):
                        return True, ""
                except Exception:
                    return True, ""

            bundle_url = f"{api_url}/graph/bundle"
            project_id = metadata.get("context_id") or "default-swarm"
            agent_id = metadata.get("agent_id") or req_headers.get("X-Active-Agent") or "cpg-swarm-agent"
            user_id = req_headers.get("X-Active-User") or "cpg-swarm-user"
            meta_copy = dict(metadata)
            if "type" not in meta_copy:
                meta_copy["type"] = node_type
            atom = {
                "uuid": node_id,
                "header": {
                    "uuid": node_id,
                    "origin": {
                        "project_id": project_id,
                        "agent_id": agent_id,
                        "user_id": user_id,
                        "context_id": metadata.get("context_id", ""),
                    }
                },
                "payload": {
                    "statement": metadata.get("name") or metadata.get("statement") or node_id,
                    "content": json.dumps(meta_copy)
                },
                "attributes": {k: (v if isinstance(v, str) else json.dumps(v)) for k, v in meta_copy.items()}
            }
            b_payload = {
                "project_id": project_id,
                "agent_id": agent_id,
                "atoms": [atom]
            }
            b_resp = await client.post(bundle_url, json=b_payload, headers=req_headers)
            if b_resp.status_code in (200, 201):
                return True, ""
            return False, resp.text
        except Exception as e:
            return False, str(e)

    async def _async_fetch_graph_node(client: httpx.AsyncClient, node_id: str, req_headers: dict) -> tuple[int, dict | None]:
        url = f"{api_url}/node/{urllib.parse.quote(node_id)}"
        try:
            resp = await client.get(url, headers=req_headers)
            if resp.status_code == 200:
                try:
                    return 200, resp.json()
                except Exception:
                    return 200, None
            return resp.status_code, None
        except Exception:
            return 500, None

    async def _async_create_graph_link(client: httpx.AsyncClient, source: str, target: str, label: str, req_headers: dict, weight: float = 1.0) -> bool:
        link_url = f"{api_url}/link"
        payload = {
            "source": source,
            "target": target,
            "label": label,
            "weight": weight
        }
        try:
            resp = await client.post(link_url, json=payload, headers=req_headers)
            return resp.status_code in (200, 201)
        except Exception:
            return False

    async def _async_fetch_node_links(client: httpx.AsyncClient, node_id: str, req_headers: dict, direction: str = "both") -> dict:
        url = f"{api_url}/node/{urllib.parse.quote(node_id)}/links?direction={direction}"
        try:
            resp = await client.get(url, headers=req_headers)
            if resp.status_code == 200:
                try:
                    return resp.json()
                except Exception:
                    return {"inbound": [], "outbound": []}
            return {"inbound": [], "outbound": []}
        except Exception:
            return {"inbound": [], "outbound": []}

    # ------------------------------------------------------------------
    # Swarm Coordination Tools
    # ------------------------------------------------------------------

    @mcp.tool()
    async def swarm_init_context(
        context_id: str,
        name: str,
        user_id: str = "human"
    ) -> str:
        """Initialize a new swarm execution context and propose the initial requirement atom."""
        req_h = _make_headers(active_user=user_id, active_agent="swarm-coordinator")
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            # 1. Register context ID
            reg_url = f"{api_url}/context/register"
            reg_payload = {
                "context_id": context_id,
                "surface_id": "swarm-coordinator",
                "client_app": "ab-ctl-swarm",
                "capabilities": {
                    "swarm_name": name,
                    "surface_type": "orchestrator"
                }
            }
            try:
                resp = await client.post(reg_url, json=reg_payload, headers=req_h)
                if resp.status_code not in (200, 201):
                    return json.dumps({"status": "ERROR", "message": f"Error registering swarm context: HTTP {resp.status_code}: {resp.text}"})
            except Exception as e:
                return json.dumps({"status": "ERROR", "message": f"Error registering swarm context: {e}"})

            # 2. Create initial requirement knowledge atom
            req_id = f"req-{context_id}"
            req_meta = {
                "name": name,
                "statement": f"Requirement for {name}",
                "description": f"Requirement for {name}",
                "context_id": context_id,
                "status": "PROPOSED",
                "created_at": int(time.time())
            }
            ok, err = await _async_commit_graph_node(client, "requirement", req_id, req_meta, req_h)
            if not ok:
                return json.dumps({"status": "ERROR", "message": f"Error creating requirement atom: {err}"})

            return json.dumps({
                "status": "SUCCESS",
                "context_id": context_id,
                "requirement_id": req_id,
                "name": name
            })

    @mcp.tool()
    async def swarm_create_task(
        context_id: str,
        name: str,
        workflow: str = "feature",
        target_symbols: list[str] = [],
        depends_on: list[str] = [],
        blast_radius_k: int = 2,
        agent_id: str = "cpg-architect"
    ) -> str:
        """Create a swarm task atom with target CPG symbols, workflow, and DEPENDS_ON links."""
        req_h = _make_headers(active_user="human", active_agent=agent_id)
        if isinstance(target_symbols, str):
            symbols = [s.strip() for s in target_symbols.split(",") if s.strip()]
        elif target_symbols is None:
            symbols = []
        else:
            symbols = list(target_symbols)

        if isinstance(depends_on, str):
            deps = [d.strip() for d in depends_on.split(",") if d.strip()]
        elif depends_on is None:
            deps = []
        else:
            deps = list(depends_on)

        if re.match(r"^[a-zA-Z0-9_\-]+$", name):
            task_id = name
        else:
            task_id = f"task-{secrets.token_hex(4)}"

        task_meta = {
            "type": "task",
            "name": name,
            "workflow": workflow or "feature",
            "target_symbols": symbols,
            "status": "READY",
            "context_id": context_id,
            "agent_id": agent_id,
            "blast_radius_k": blast_radius_k if blast_radius_k is not None else 2,
            "lease": {
                "holder": None,
                "expires_at": 0
            },
            "depends_on": deps,
            "created_at": int(time.time())
        }

        async with httpx.AsyncClient(timeout=client_timeout) as client:
            ok, err = await _async_commit_graph_node(client, "task", task_id, task_meta, req_h)
            if not ok:
                return json.dumps({"status": "ERROR", "message": f"Error creating task node: {err}"})

            for dep_id in deps:
                await _async_create_graph_link(client, task_id, dep_id, "DEPENDS_ON", req_h)

            return json.dumps({
                "status": "SUCCESS",
                "task_id": task_id,
                "name": name,
                "workflow": workflow,
                "target_symbols": symbols,
                "depends_on": deps,
                "blast_radius_k": blast_radius_k
            })

    @mcp.tool()
    async def swarm_list_tasks(context_id: str) -> str:
        """List all tasks in a swarm context with status, lease holder, dependencies, and verdicts."""
        req_h = _make_headers(active_user="human", active_agent="cpg-swarm-agent")
        tasks_map = {}

        def _is_task_node(item_id: str, item_data: dict, meta: dict) -> bool:
            if str(item_id).startswith(("req-", "sol-", "proof-", "counter-", "acc-")):
                return False
            node_type = item_data.get("type") or meta.get("type")
            if node_type == "requirement":
                return False
            return True

        def _extract_search_items(search_resp: dict) -> list:
            if not isinstance(search_resp, dict):
                return []
            items = search_resp.get("matches")
            if items is None:
                items = search_resp.get("results")
            if isinstance(items, list):
                return items
            return []

        async with httpx.AsyncClient(timeout=client_timeout) as client:
            # Query context endpoint
            ctx_url = f"{api_url}/context/{urllib.parse.quote(context_id)}"
            try:
                c_resp = await client.get(ctx_url, headers=req_h)
                if c_resp.status_code == 200:
                    ctx_data = c_resp.json()
                    if isinstance(ctx_data, dict):
                        for item in ctx_data.get("tasks", []):
                            tid = item.get("id") or item.get("uuid")
                            if tid:
                                meta = extract_node_metadata(item)
                                if _is_task_node(tid, item, meta):
                                    tasks_map[tid] = item
                        for item in ctx_data.get("nodes", []):
                            tid = item.get("id") or item.get("uuid")
                            if tid:
                                meta = extract_node_metadata(item)
                                if _is_task_node(tid, item, meta) and (item.get("type") == "task" or "task" in tid.lower()):
                                    tasks_map[tid] = item
            except Exception:
                pass

            # Query search endpoint by context
            search_url = f"{api_url}/search?q={urllib.parse.quote(context_id)}"
            try:
                s_resp = await client.get(search_url, headers=req_h)
                if s_resp.status_code == 200:
                    search_data = s_resp.json()
                    if isinstance(search_data, dict):
                        for item in _extract_search_items(search_data):
                            tid = item.get("id") or item.get("uuid")
                            if tid:
                                meta = extract_node_metadata(item)
                                item_ctx = meta.get("context_id") or item.get("context_id") or item.get("project")
                                if (not item_ctx or item_ctx == context_id) and _is_task_node(tid, item, meta):
                                    tasks_map[tid] = item
            except Exception:
                pass

            # General search fallback if empty
            if not tasks_map:
                try:
                    all_resp = await client.get(f"{api_url}/search?q=", headers=req_h)
                    if all_resp.status_code == 200:
                        all_data = all_resp.json()
                        if isinstance(all_data, dict):
                            for item in _extract_search_items(all_data):
                                tid = item.get("id") or item.get("uuid")
                                if tid:
                                    meta = extract_node_metadata(item)
                                    item_ctx = meta.get("context_id") or item.get("context_id") or item.get("project")
                                    if (not item_ctx or item_ctx == context_id) and _is_task_node(tid, item, meta):
                                        tasks_map[tid] = item
                except Exception:
                    pass

            task_rows = []
            for tid, node in tasks_map.items():
                meta = extract_node_metadata(node)
                name = meta.get("name") or node.get("label") or node.get("statement") or tid
                status = meta.get("status", "READY")
                lease = meta.get("lease", {})
                holder = lease.get("holder") if isinstance(lease, dict) else None

                links_data = await _async_fetch_node_links(client, tid, req_h, direction="both")
                dep_names = list(meta.get("depends_on", []))
                verdicts = []
                for l in links_data.get("outbound", []):
                    rel = l.get("relation", "")
                    target = l.get("target") or l.get("uuid")
                    if rel == "DEPENDS_ON" and target and target not in dep_names:
                        dep_names.append(target)
                    elif rel in ("VALIDATED_BY", "REFUTES", "ACCEPTS", "HAS_SOLUTION"):
                        verdicts.append(f"{rel}:{target}")
                for l in links_data.get("inbound", []):
                    rel = l.get("relation", "")
                    source = l.get("source") or l.get("uuid")
                    if rel in ("ACCEPTS",):
                        verdicts.append(f"{rel}:{source}")

                task_rows.append({
                    "id": tid,
                    "name": name,
                    "status": status,
                    "lease_holder": holder,
                    "dependencies": dep_names,
                    "verdicts": verdicts,
                    "metadata": meta
                })

            return json.dumps({"status": "SUCCESS", "context_id": context_id, "tasks": task_rows})

    @mcp.tool()
    async def swarm_claim_lease(
        task_id: str,
        agent_id: str,
        ttl_sec: int = 600
    ) -> str:
        """Claims an exclusive lease on a task whose dependencies are satisfied and not already completed/validated."""
        req_h = _make_headers(active_user="human", active_agent=agent_id)
        ttl = ttl_sec if ttl_sec is not None else 600
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            status, node_data = await _async_fetch_graph_node(client, task_id, req_h)
            if status != 200 or not node_data:
                return json.dumps({"status": "ERROR", "message": f"Task '{task_id}' not found (HTTP {status})"})

            metadata = extract_node_metadata(node_data)
            current_status = metadata.get("status", "").upper()
            if current_status in ("COMPLETED", "VALIDATED"):
                return json.dumps({"status": "ERROR", "message": f"[BLOCKED] Cannot claim task {task_id}: task is already {current_status}"})

            links_data = await _async_fetch_node_links(client, task_id, req_h, direction="both")
            prereq_ids = set(metadata.get("depends_on", []))
            for l in links_data.get("outbound", []):
                if l.get("relation", "").upper() == "DEPENDS_ON":
                    dep = l.get("target") or l.get("uuid")
                    if dep and dep != task_id:
                        prereq_ids.add(dep)

            for dep_id in prereq_ids:
                dep_st, dep_node = await _async_fetch_graph_node(client, dep_id, req_h)
                if dep_st != 200 or not dep_node:
                    return json.dumps({"status": "ERROR", "message": f"[BLOCKED] Prerequisite {dep_id} not found in blackboard"})
                dep_meta = extract_node_metadata(dep_node)
                dep_status = dep_meta.get("status", "UNKNOWN").upper()
                if dep_status not in ("COMPLETED", "VALIDATED"):
                    return json.dumps({"status": "ERROR", "message": f"[BLOCKED] Prerequisite {dep_id} not validated (Current status: {dep_status})"})

            lease = metadata.get("lease") or {}
            holder = lease.get("holder")
            expires_at = lease.get("expires_at") or 0
            now = time.time()

            if holder and holder != agent_id and expires_at > now:
                return json.dumps({"status": "ERROR", "message": f"[BLOCKED] Task {task_id} is already leased by agent '{holder}' until {int(expires_at)}"})

            metadata["status"] = "IN_PROGRESS"
            metadata["lease"] = {
                "holder": agent_id,
                "expires_at": int(now + ttl)
            }

            ok, err = await _async_commit_graph_node(client, "task", task_id, metadata, req_h)
            if not ok:
                return json.dumps({"status": "ERROR", "message": f"Error updating task lease: {err}"})

            return json.dumps({
                "status": "SUCCESS",
                "task_id": task_id,
                "holder": agent_id,
                "status_value": "IN_PROGRESS",
                "expires_at": int(now + ttl),
                "ttl_sec": ttl
            })

    @mcp.tool()
    async def swarm_release_lease(
        task_id: str,
        agent_id: str
    ) -> str:
        """Release active lease on a task and return it to READY state."""
        req_h = _make_headers(active_user="human", active_agent=agent_id)
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            status, node_data = await _async_fetch_graph_node(client, task_id, req_h)
            if status != 200 or not node_data:
                return json.dumps({"status": "ERROR", "message": f"Task '{task_id}' not found (HTTP {status})"})

            metadata = extract_node_metadata(node_data)
            lease = metadata.get("lease") or {}
            holder = lease.get("holder")
            expires_at = lease.get("expires_at") or 0
            now = time.time()

            if holder and holder != agent_id and expires_at > now:
                return json.dumps({"status": "ERROR", "message": f"[BLOCKED] Cannot release lease: Task {task_id} is leased by different agent '{holder}'"})

            metadata["status"] = "READY"
            metadata["lease"] = {
                "holder": None,
                "expires_at": 0
            }

            ok, err = await _async_commit_graph_node(client, "task", task_id, metadata, req_h)
            if not ok:
                return json.dumps({"status": "ERROR", "message": f"Error releasing lease: {err}"})

            return json.dumps({
                "status": "SUCCESS",
                "task_id": task_id,
                "agent_id": agent_id,
                "status_value": "READY"
            })

    @mcp.tool()
    async def swarm_submit_review(
        task_id: str,
        agent_id: str,
        patch_summary: str = ""
    ) -> str:
        """Create solution atom, link via HAS_SOLUTION, and mark task REVIEW_PENDING."""
        req_h = _make_headers(active_user="human", active_agent=agent_id)
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            status, node_data = await _async_fetch_graph_node(client, task_id, req_h)
            if status != 200 or not node_data:
                return json.dumps({"status": "ERROR", "message": f"Task '{task_id}' not found (HTTP {status})"})

            task_meta = extract_node_metadata(node_data)
            context_id = task_meta.get("context_id", "")
            current_status = task_meta.get("status")

            if current_status != "IN_PROGRESS":
                return json.dumps({"status": "ERROR", "message": f"[BLOCKED] Cannot submit review for task {task_id}: status is '{current_status}', expected 'IN_PROGRESS'"})

            lease_holder = (task_meta.get("lease") or {}).get("holder")
            if lease_holder and lease_holder != agent_id:
                return json.dumps({"status": "ERROR", "message": f"[BLOCKED] Cannot submit review: Task is leased by '{lease_holder}', not '{agent_id}'"})

            patch = patch_summary or "Solution patch submitted"
            solution_id = f"sol-{task_id}-{secrets.token_hex(4)}"
            sol_meta = {
                "task_id": task_id,
                "agent_id": agent_id,
                "patch": patch,
                "summary": patch,
                "created_at": int(time.time()),
                "context_id": context_id
            }

            ok, err = await _async_commit_graph_node(client, "solution", solution_id, sol_meta, req_h)
            if not ok:
                return json.dumps({"status": "ERROR", "message": f"Error creating solution atom: {err}"})

            await _async_create_graph_link(client, task_id, solution_id, "HAS_SOLUTION", req_h)

            task_meta["status"] = "REVIEW_PENDING"
            ok, err = await _async_commit_graph_node(client, "task", task_id, task_meta, req_h)
            if not ok:
                return json.dumps({"status": "ERROR", "message": f"Error updating task status: {err}"})

            return json.dumps({
                "status": "SUCCESS",
                "task_id": task_id,
                "solution_id": solution_id,
                "agent_id": agent_id,
                "status_value": "REVIEW_PENDING"
            })

    @mcp.tool()
    async def swarm_record_verdict(
        task_id: str,
        verifier_id: str,
        verdict: str,
        details: str = ""
    ) -> str:
        """Record dialectic verification verdict (PASS/FAIL), linking proof or counterexample trace."""
        req_h = _make_headers(active_user="human", active_agent=verifier_id)
        v_upper = (verdict or "").upper()
        if v_upper not in ("PASS", "FAIL"):
            return json.dumps({"status": "ERROR", "message": f"Invalid verdict '{verdict}'. Must be PASS or FAIL."})

        async with httpx.AsyncClient(timeout=client_timeout) as client:
            status, node_data = await _async_fetch_graph_node(client, task_id, req_h)
            if status != 200 or not node_data:
                return json.dumps({"status": "ERROR", "message": f"Task '{task_id}' not found (HTTP {status})"})

            task_meta = extract_node_metadata(node_data)
            current_status = task_meta.get("status")
            if current_status != "REVIEW_PENDING":
                return json.dumps({"status": "ERROR", "message": f"[BLOCKED] Cannot record verdict for task {task_id}: status is '{current_status}', expected 'REVIEW_PENDING'"})

            context_id = task_meta.get("context_id", "")
            if isinstance(details, dict):
                details_val = details
            elif details and isinstance(details, str):
                try:
                    details_val = json.loads(details)
                except Exception:
                    details_val = {"raw": details}
            else:
                details_val = {}

            if v_upper == "PASS":
                proof_id = f"proof-{task_id}-{secrets.token_hex(4)}"
                proof_meta = {
                    "task_id": task_id,
                    "verifier_agent": verifier_id,
                    "verdict": "PASS",
                    "details": details_val,
                    "context_id": context_id,
                    "created_at": int(time.time())
                }
                ok, err = await _async_commit_graph_node(client, "verification_proof", proof_id, proof_meta, req_h)
                if not ok:
                    return json.dumps({"status": "ERROR", "message": f"Error creating verification proof atom: {err}"})

                await _async_create_graph_link(client, task_id, proof_id, "VALIDATED_BY", req_h)

                task_meta["status"] = "VALIDATED"
                if "lease" in task_meta and isinstance(task_meta["lease"], dict):
                    task_meta["lease"]["holder"] = None
                    task_meta["lease"]["expires_at"] = 0
                else:
                    task_meta["lease"] = {"holder": None, "expires_at": 0}

                ok, err = await _async_commit_graph_node(client, "task", task_id, task_meta, req_h)
                if not ok:
                    return json.dumps({"status": "ERROR", "message": f"Error updating task status: {err}"})

                return json.dumps({
                    "status": "SUCCESS",
                    "task_id": task_id,
                    "verdict": "PASS",
                    "proof_id": proof_id,
                    "verifier_id": verifier_id,
                    "status_value": "VALIDATED"
                })

            else:  # FAIL
                counter_id = f"counter-{task_id}-{secrets.token_hex(4)}"
                counter_meta = {
                    "task_id": task_id,
                    "verifier_agent": verifier_id,
                    "verdict": "FAIL",
                    "details": details_val,
                    "context_id": context_id,
                    "created_at": int(time.time())
                }
                ok, err = await _async_commit_graph_node(client, "counterexample_trace", counter_id, counter_meta, req_h)
                if not ok:
                    return json.dumps({"status": "ERROR", "message": f"Error creating counterexample trace atom: {err}"})

                await _async_create_graph_link(client, task_id, counter_id, "REFUTES", req_h)

                task_meta["status"] = "READY"
                if "lease" in task_meta and isinstance(task_meta["lease"], dict):
                    task_meta["lease"]["holder"] = None
                    task_meta["lease"]["expires_at"] = 0
                else:
                    task_meta["lease"] = {"holder": None, "expires_at": 0}

                ok, err = await _async_commit_graph_node(client, "task", task_id, task_meta, req_h)
                if not ok:
                    return json.dumps({"status": "ERROR", "message": f"Error updating task status: {err}"})

                return json.dumps({
                    "status": "SUCCESS",
                    "task_id": task_id,
                    "verdict": "FAIL",
                    "counter_id": counter_id,
                    "verifier_id": verifier_id,
                    "status_value": "READY"
                })

    @mcp.tool()
    async def swarm_accept_task(
        task_id: str,
        stakeholder_id: str,
        notes: str = ""
    ) -> str:
        """Record stakeholder acceptance and advance task from VALIDATED to COMPLETED."""
        req_h = _make_headers(active_user=stakeholder_id, active_agent="cpg-swarm-agent")
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            status, node_data = await _async_fetch_graph_node(client, task_id, req_h)
            if status != 200 or not node_data:
                return json.dumps({"status": "ERROR", "message": f"Task '{task_id}' not found (HTTP {status})"})

            task_meta = extract_node_metadata(node_data)
            task_status = task_meta.get("status")
            if task_status != "VALIDATED":
                return json.dumps({"status": "ERROR", "message": f"[BLOCKED] Cannot accept task {task_id}: status is '{task_status}', expected 'VALIDATED'"})

            context_id = task_meta.get("context_id", "")
            acc_id = f"acc-{task_id}-{secrets.token_hex(4)}"
            acc_meta = {
                "task_id": task_id,
                "stakeholder": stakeholder_id,
                "notes": notes or "Accepted by stakeholder",
                "context_id": context_id,
                "created_at": int(time.time())
            }

            ok, err = await _async_commit_graph_node(client, "acceptance", acc_id, acc_meta, req_h)
            if not ok:
                return json.dumps({"status": "ERROR", "message": f"Error creating acceptance record: {err}"})

            await _async_create_graph_link(client, task_id, acc_id, "ACCEPTS", req_h)

            task_meta["status"] = "COMPLETED"
            ok, err = await _async_commit_graph_node(client, "task", task_id, task_meta, req_h)
            if not ok:
                return json.dumps({"status": "ERROR", "message": f"Error updating task status: {err}"})

            return json.dumps({
                "status": "SUCCESS",
                "task_id": task_id,
                "acceptance_id": acc_id,
                "stakeholder": stakeholder_id,
                "status_value": "COMPLETED"
            })

    @mcp.resource("ab://schema")
    async def get_schema() -> str:
        """Retrieve Agentic Blackboard Knowledge Schema."""
        async with httpx.AsyncClient(timeout=client_timeout) as client:
            resp = await client.get(f"{api_url}/schema", headers=headers)
            if resp.is_error:
                return json.dumps({"status": "ERROR", "code": resp.status_code, "message": resp.text})
            return resp.text

    @mcp.resource("ab://skills/{name}")
    async def get_skill(name: str) -> str:
        """Retrieve skill documentation by name from skills/{name}/SKILL.md."""
        try:
            clean_name = name.removesuffix("/SKILL.md").removesuffix(".md").strip("/")
            repo_root = Path(__file__).resolve().parent.parent
            skills_dir = (repo_root / "skills").resolve()
            skill_path = (skills_dir / clean_name / "SKILL.md").resolve()
            skill_path.relative_to(skills_dir)
            if skill_path.is_file():
                return skill_path.read_text(encoding="utf-8")
            system_path = Path(f"/usr/share/agentic-blackboard/skills/{clean_name}/SKILL.md")
            if system_path.is_file():
                return system_path.read_text(encoding="utf-8")
            return f"Error: Skill '{name}' not found."
        except Exception:
            return f"Error: Skill '{name}' not found."

    return mcp


def handle_mcp(args, cfg: dict):
    """Run FastMCP server facade or execute smoke test."""
    connect_url = (args.connect or cfg.get("connect", DEFAULT_CONNECT_URL)).rstrip("/")
    token = args.token or cfg.get("token")

    mcp = build_mcp_server(connect_url, token)
    if mcp is None or mcp == 1:
        return 1

    if args.smoke_test:
        print("[MCP] Running self-test and smoke verification...")
        tools = asyncio.run(mcp.list_tools())
        assert len(tools) > 0, "No MCP tools registered on FastMCP server"
        print(f"[MCP] Registered {len(tools)} tools: {', '.join(t.name for t in tools)}")

        # Verify connectivity to blackboard REST API
        schema_url = f"{connect_url}/api/v1/schema"
        h = get_auth_headers(token)
        try:
            req = urllib.request.Request(schema_url, headers=h)
            with urllib.request.urlopen(req, timeout=5.0) as resp:
                assert resp.status == 200, f"Schema returned status {resp.status}"
                body = resp.read().decode("utf-8")
                assert len(body) > 0, "Empty schema body"
        except Exception as e:
            print(f"[MCP ERROR] Smoke test failed connecting to {schema_url}: {e}", file=sys.stderr)
            return 1

        print(f"[MCP] Successfully connected to blackboard at {connect_url}")
        print("[MCP] Smoke test passed successfully.")
        return 0

    # Normal MCP execution over stdio
    mcp.run()
    return 0


# ----------------------------------------------------------------------
# Main Argument Parser
# ----------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        prog="ab-ctl",
        description="Agentic Blackboard (ab) Controller"
    )
    parser.add_argument("--config", help="Path to blackboard.conf configuration file")
    parser.add_argument("--connect", help="Blackboard daemon URL (default: http://localhost:8085)")
    parser.add_argument("--token", help="Bearer authorization token")
    parser.add_argument("--data-dir", help="Substrate data directory (default: /var/lib/agentic-blackboard)")

    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    # 1. init
    init_p = subparsers.add_parser("init", help="Initialize substrate directories and credentials database")
    init_p.add_argument("--bootstrap", action="store_true", help="Create bootstrap cluster administrator credentials")
    init_p.add_argument("--data-dir", help="Data directory path")

    # 2. status
    status_p = subparsers.add_parser("status", help="Query daemon operational status and metrics")
    status_p.add_argument("--connect", help="Daemon connect URL")
    status_p.add_argument("--token", help="Authentication token")

    # 3. user
    user_p = subparsers.add_parser("user", help="User and token credential operations")
    user_sub = user_p.add_subparsers(dest="user_action", required=True)
    user_create = user_sub.add_parser("create", help="Create user and generate user token")
    user_create.add_argument("username", help="Username")
    user_create.add_argument("--role", default="curator", help="User role (default: curator)")
    user_create.add_argument("--token", help="Admin token for authorization")
    user_create.add_argument("--connect", help="Daemon connect URL")
    user_create.add_argument("--data-dir", help="Data directory path")

    user_list = user_sub.add_parser("list", help="List registered users")
    user_list.add_argument("--token", help="Admin token")
    user_list.add_argument("--connect", help="Daemon connect URL")
    user_list.add_argument("--data-dir", help="Data directory path")

    # 4. agent
    agent_p = subparsers.add_parser("agent", help="Agent proxy token operations")
    agent_sub = agent_p.add_subparsers(dest="agent_action", required=True)
    agent_create = agent_sub.add_parser("create", help="Issue delegated agent proxy token")
    agent_create.add_argument("agent_name", help="Agent identity name")
    agent_create.add_argument("--user", required=True, help="Sovereign user identifier")
    agent_create.add_argument("--project", required=True, help="Anchor project identifier")
    agent_create.add_argument("--token", help="Admin or authorizing user token")
    agent_create.add_argument("--connect", help="Daemon connect URL")
    agent_create.add_argument("--data-dir", help="Data directory path")

    # 5. surface
    surface_p = subparsers.add_parser("surface", help="Surface device operations")
    surface_sub = surface_p.add_subparsers(dest="surface_action", required=True)
    surface_reg = surface_sub.add_parser("register", help="Register ambient surface with workspace context")
    surface_reg.add_argument("--name", required=True, help="Surface identifier")
    surface_reg.add_argument("--type", required=True, help="Surface type (e.g. tabletop, tablet, wall)")
    surface_reg.add_argument("--context", required=True, help="Workspace context ID")
    surface_reg.add_argument("--capabilities", help="Optional JSON string of surface capabilities")
    surface_reg.add_argument("--client-app", help="Client application name")
    surface_reg.add_argument("--token", help="Bearer authorization token")
    surface_reg.add_argument("--connect", help="Daemon connect URL")

    # 6. context
    context_p = subparsers.add_parser("context", help="Workspace context broker operations")
    context_sub = context_p.add_subparsers(dest="context_action", required=True)
    context_show = context_sub.add_parser("show", help="Display active surfaces and focus in context")
    context_show.add_argument("context_id", help="Workspace context ID")
    context_show.add_argument("--token", help="Bearer authorization token")
    context_show.add_argument("--connect", help="Daemon connect URL")

    context_focus = context_sub.add_parser("focus", help="Update and broadcast focus selection in context")
    context_focus.add_argument("context_id", help="Workspace context ID")
    context_focus.add_argument("--selected", nargs="+", required=True, help="Selected atom IDs")
    context_focus.add_argument("--surface-id", help="Surface ID broadcasting focus")
    context_focus.add_argument("--token", help="Bearer authorization token")
    context_focus.add_argument("--connect", help="Daemon connect URL")

    # 7. mcp
    mcp_p = subparsers.add_parser("mcp", help="Integrated MCP Server operations")
    mcp_sub = mcp_p.add_subparsers(dest="mcp_action", required=True)
    mcp_run = mcp_sub.add_parser("run", help="Run FastMCP server bridge forwarding stdio to blackboard")
    mcp_run.add_argument("--connect", help="Blackboard daemon URL")
    mcp_run.add_argument("--token", help="Bearer authorization token")
    mcp_run.add_argument("--smoke-test", action="store_true", help="Run self-test of MCP tools and exit 0")

    def add_net_args(p: argparse.ArgumentParser):
        p.add_argument("--connect", help="Daemon connect URL")
        p.add_argument("--token", help="Bearer authorization token")
        p.add_argument("--active-user", help="X-Active-User header identity")
        p.add_argument("--active-agent", help="X-Active-Agent header identity")

    # 8. swarm
    swarm_p = subparsers.add_parser("swarm", help="Swarm coordination, task DAG, and lease management")
    swarm_sub = swarm_p.add_subparsers(dest="swarm_action", required=True)

    # 8a. swarm init
    init_swarm_p = swarm_sub.add_parser("init", help="Initialize swarm workspace context and requirement atom")
    init_swarm_p.add_argument("--context", required=True, help="Workspace context ID")
    init_swarm_p.add_argument("--name", required=True, help="Swarm / initiative name")
    init_swarm_p.add_argument("--requirement", help="Initial requirement statement or description")
    add_net_args(init_swarm_p)

    # 8b. swarm task
    task_p = swarm_sub.add_parser("task", help="Swarm task lifecycle management")
    task_sub = task_p.add_subparsers(dest="task_action", required=True)

    # task create
    task_create_p = task_sub.add_parser("create", help="Create a new task in the swarm DAG")
    task_create_p.add_argument("--context", required=True, help="Workspace context ID")
    task_create_p.add_argument("--name", required=True, help="Task name")
    task_create_p.add_argument("--id", help="Optional explicit task identifier (defaults to name or generated ID)")
    task_create_p.add_argument("--workflow", default="feature", help="Workflow type (feature, bugfix, refactoring)")
    task_create_p.add_argument("--symbols", default="", help="Comma-separated target symbols")
    task_create_p.add_argument("--depends-on", default="", help="Comma-separated prerequisite task IDs")
    task_create_p.add_argument("--blast-radius", "-k", type=int, default=2, help="CPG blast radius k-hop distance")
    task_create_p.add_argument("--agent", default="cpg-architect", help="Author agent ID (default: cpg-architect)")
    add_net_args(task_create_p)

    # task list
    task_list_p = task_sub.add_parser("list", help="List tasks for a swarm context")
    task_list_p.add_argument("--context", required=True, help="Workspace context ID")
    task_list_p.add_argument("--format", choices=["table", "json"], default="table", help="Output format (table or json)")
    add_net_args(task_list_p)

    # 8c. swarm lease
    lease_p = swarm_sub.add_parser("lease", help="Task exclusive lease operations")
    lease_sub = lease_p.add_subparsers(dest="lease_action", required=True)

    # lease claim
    lease_claim_p = lease_sub.add_parser("claim", help="Claim an exclusive lease on a task")
    lease_claim_p.add_argument("task_id", help="Task ID to claim")
    lease_claim_p.add_argument("--agent", required=True, help="Agent claiming the lease")
    lease_claim_p.add_argument("--ttl", type=int, default=600, help="Lease time-to-live in seconds (default: 600)")
    add_net_args(lease_claim_p)

    # lease release
    lease_release_p = lease_sub.add_parser("release", help="Release lease on a task")
    lease_release_p.add_argument("task_id", help="Task ID to release")
    lease_release_p.add_argument("--agent", required=True, help="Agent releasing the lease")
    add_net_args(lease_release_p)

    # 8d. swarm review
    review_p = swarm_sub.add_parser("review", help="Dialectic review and verification operations")
    review_sub = review_p.add_subparsers(dest="review_action", required=True)

    # review submit
    review_submit_p = review_sub.add_parser("submit", help="Submit task solution for review")
    review_submit_p.add_argument("task_id", help="Task ID to submit")
    review_submit_p.add_argument("--agent", required=True, help="Agent submitting the review")
    review_submit_p.add_argument("--patch", help="Summary or description of the patch/solution")
    add_net_args(review_submit_p)

    # review verdict
    review_verdict_p = review_sub.add_parser("verdict", help="Record verification verdict on a task")
    review_verdict_p.add_argument("task_id", help="Task ID under review")
    review_verdict_p.add_argument("--verifier", required=True, help="Verifier agent ID")
    review_verdict_p.add_argument("--verdict", required=True, choices=["PASS", "FAIL", "pass", "fail"], help="Verdict: PASS or FAIL")
    review_verdict_p.add_argument("--details", help="Optional JSON or text details of proof/refutation")
    add_net_args(review_verdict_p)

    # 8e. swarm accept
    accept_p = swarm_sub.add_parser("accept", help="Stakeholder acceptance of a validated task")
    accept_p.add_argument("task_id", help="Task ID to accept")
    accept_p.add_argument("--stakeholder", required=True, help="Stakeholder user or agent ID")
    accept_p.add_argument("--notes", help="Optional acceptance notes or comments")
    add_net_args(accept_p)

    parsed_args = parser.parse_args()
    config = load_config(parsed_args.config)

    # Subcommand routing
    if parsed_args.subcommand == "init":
        sys.exit(handle_init(parsed_args, config))
    elif parsed_args.subcommand == "status":
        sys.exit(handle_status(parsed_args, config))
    elif parsed_args.subcommand == "user":
        sys.exit(handle_user(parsed_args, config))
    elif parsed_args.subcommand == "agent":
        sys.exit(handle_agent(parsed_args, config))
    elif parsed_args.subcommand == "surface":
        sys.exit(handle_surface(parsed_args, config))
    elif parsed_args.subcommand == "context":
        sys.exit(handle_context(parsed_args, config))
    elif parsed_args.subcommand == "mcp":
        sys.exit(handle_mcp(parsed_args, config))
    elif parsed_args.subcommand == "swarm":
        sys.exit(handle_swarm(parsed_args, config))
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
