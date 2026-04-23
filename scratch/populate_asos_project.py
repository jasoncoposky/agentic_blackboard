import httpx
import time

ASOS_API_URL = "http://localhost:8085/api/v1"

def ensure_node(type, id, description, content=""):
    payload = {
        "type": type,
        "id": id,
        "metadata": {
            "description": description,
            "content": content
        }
    }
    httpx.post(f"{ASOS_API_URL}/graph/node", json=payload).raise_for_status()

def commit_bundle(project_id, agent_id, atoms):
    payload = {
        "project_id": project_id,
        "agent_id": agent_id,
        "atoms": atoms
    }
    httpx.post(f"{ASOS_API_URL}/graph/bundle", json=payload).raise_for_status()

def link_nodes(src, dst, label):
    payload = {
        "source": src,
        "target": dst,
        "label": label,
        "weight": 1.0
    }
    httpx.post(f"{ASOS_API_URL}/link", json=payload).raise_for_status()

def main():
    print("Populating ASOS Project Graph...")

    # 1. Identity and Project
    ensure_node("IDENTITY", "jason_coposky", "Jason Coposky", 
                "### Identity: Jason Coposky\nRole: Lead Architect\n\nExpert in distributed graph systems and agentic swarms.")
    
    ensure_node("PROJECT", "asos_substrate", "ASOS Knowledge Substrate and Agentic Blackboard",
                "# ASOS Substrate\n\nThe foundational layer for autonomous agentic swarms. Implements a high-performance L3KVG distributed graph engine.")

    # 2. Define Atoms with explicit UUIDs so we can link them
    atoms = [
        # Documents
        {"uuid": "doc-reqs", 
         "statement": "Requirements Document: Provide a hardened, scalable knowledge graph for autonomous agent swarms.",
         "content": "## Requirements Document\n\n1. **Orphan Prevention**: All atoms must be anchored.\n2. **UUID Integrity**: Deterministic hashing.\n3. **Interactive Visualization**: Real-time D3-force layout.\n4. **MCP Support**: Seamless AI integration."},
        {"uuid": "doc-schema", "statement": "Schema Document: Defines ATOM, PROJECT, IDENTITY nodes, Knowledge Areas (KA), and structural relationships (BELONGS_TO, CREATED_BY)."},
        
        # Milestone 1
        {"uuid": "m1", "statement": "Milestone 1: Hardening the Knowledge Graph", "tags": ["MILESTONE"]},
        {"uuid": "m1-wbs-hl", "statement": "M1 WBS (High-Level): Implement strict data integrity and orphan prevention rules."},
        {"uuid": "m1-wbs-det", "statement": "M1 WBS (Detail): Add mandatory anchor validation (Agent & Project) in Blackboard::commit_cpb_entry and generate deterministic hash-based UUIDs for atoms missing IDs."},
        {"uuid": "m1-ac", "statement": "M1 Acceptance Criteria: Unanchored commits are rejected, and DB remains clean across restarts."},

        # Milestone 2
        {"uuid": "m2", "statement": "Milestone 2: Librarian Dashboard & Visualization", "tags": ["MILESTONE"]},
        {"uuid": "m2-wbs-hl", "statement": "M2 WBS (High-Level): Build an interactive real-time map for exploring the substrate."},
        {"uuid": "m2-wbs-det", "statement": "M2 WBS (Detail): Implement drag-and-drop node physics in GraphMap.js and fix Studio View type errors."},
        {"uuid": "m2-ac", "statement": "M2 Acceptance Criteria: Dashboard renders mixed node types without crashing and allows physics-based re-organization."},

        # Milestone 3
        {"uuid": "m3", "statement": "Milestone 3: Model Context Protocol (MCP) Integration", "tags": ["MILESTONE"]},
        {"uuid": "m3-wbs-hl", "statement": "M3 WBS (High-Level): Expose the substrate to external AI agents via standard MCP tools."},
        {"uuid": "m3-wbs-det", "statement": "M3 WBS (Detail): Create Python FastMCP bridge (asos_mcp_server.py) providing ensure_node, commit_knowledge_bundle, and structural query endpoints."},
        {"uuid": "m3-ac", "statement": "M3 Acceptance Criteria: Agent can query schema and autonomously execute the init_swarm playbook."}
    ]

    commit_bundle("asos_substrate", "jason_coposky", atoms)

    # 3. Establish structural links
    links = [
        # Docs related to project
        ("doc-reqs", "asos_substrate", "DEFINES"),
        ("doc-schema", "asos_substrate", "DEFINES"),
        
        # M1 structure
        ("m1", "asos_substrate", "MILESTONE"),
        ("m1-wbs-hl", "m1", "BREAKDOWN"),
        ("m1-wbs-det", "m1-wbs-hl", "DETAIL"),
        ("m1-ac", "m1", "CRITERIA"),

        # M2 structure
        ("m2", "asos_substrate", "MILESTONE"),
        ("m2-wbs-hl", "m2", "BREAKDOWN"),
        ("m2-wbs-det", "m2-wbs-hl", "DETAIL"),
        ("m2-ac", "m2", "CRITERIA"),

        # M3 structure
        ("m3", "asos_substrate", "MILESTONE"),
        ("m3-wbs-hl", "m3", "BREAKDOWN"),
        ("m3-wbs-det", "m3-wbs-hl", "DETAIL"),
        ("m3-ac", "m3", "CRITERIA")
    ]

    for src, dst, label in links:
        link_nodes(src, dst, label)

    print("Population complete.")

if __name__ == "__main__":
    main()
