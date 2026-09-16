#!/usr/bin/env python3
"""
Standalone verification script for ASOS FastMCP Atmosphere & Commonplace Server upgrades.
Verifies:
1. search_commonplace (text query, ka filter, tags filter)
2. get_node (retrieval of full hydrated atom properties)
3. get_node_links (inbound and outbound relationship queries)
4. create_note (creation and duplicate detection on second attempt)
5. create_catalog_entry (creation and duplicate detection)
6. export_graph_rdf (W3C RDF Turtle serialization)
7. Reading asos://skills/knowledge-capture resource
8. Curate note & author catalog prompts
"""

import asyncio
import json
from pathlib import Path
import subprocess
import sys
import httpx

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

# Import from asos_mcp_server
from asos_mcp_server import (
    mcp,
    ensure_node,
    commit_knowledge_bundle,
    search_commonplace,
    get_node,
    get_node_links,
    export_graph_rdf,
    create_note,
    create_catalog_entry,
    link_nodes,
    get_skill,
    curate_note,
    author_catalog,
    ASOS_API_URL,
)

started_process = None

async def ensure_backend():
    """Ensure the ASOS backend server is reachable at ASOS_API_URL."""
    global started_process
    try:
        async with httpx.AsyncClient() as client:
            resp = await client.get(f"{ASOS_API_URL}/schema", timeout=2.0)
            if resp.status_code == 200:
                print("[VERIFY] Connected to running ASOS daemon at", ASOS_API_URL)
                return
    except Exception:
        pass

    daemon_bin = REPO_ROOT / "build" / "asos_daemon"
    if not daemon_bin.exists():
        raise RuntimeError(f"Backend daemon binary not found at {daemon_bin}")

    print(f"[VERIFY] Starting ASOS daemon: {daemon_bin}")
    started_process = subprocess.Popen(
        [str(daemon_bin)],
        cwd=str(REPO_ROOT),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )

    for _ in range(30):
        await asyncio.sleep(0.2)
        try:
            async with httpx.AsyncClient() as client:
                resp = await client.get(f"{ASOS_API_URL}/schema", timeout=1.0)
                if resp.status_code == 200:
                    print("[VERIFY] ASOS daemon started and ready.")
                    return
        except Exception:
            pass

    raise RuntimeError("Failed to connect to ASOS daemon after starting it.")


async def run_tests():
    print("\n" + "=" * 60)
    print("RUNNING ASOS FASTMCP ATMOSPHERE VERIFICATION SUITE")
    print("=" * 60)

    # 1. Test Resources
    print("\n--- 1. Testing Resources ---")
    schema_res = await mcp.read_resource("asos://schema")
    assert len(schema_res) > 0, "No content returned from asos://schema"
    schema_text = schema_res[0].content
    assert "knowledge_areas" in schema_text, "asos://schema missing knowledge_areas"
    assert "types" in schema_text, "asos://schema missing types"
    print("✓ asos://schema resource verified.")

    skill_res = await mcp.read_resource("asos://skills/knowledge-capture")
    assert len(skill_res) > 0, "No content returned from asos://skills/knowledge-capture"
    skill_text = skill_res[0].content
    assert "# Knowledge Capture Skill" in skill_text, "Skill title missing"
    assert "knowledge-capture" in skill_text, "Skill name missing"
    print("✓ asos://skills/knowledge-capture resource verified.")

    # Test get_skill directly with non-existent skill
    err_skill = await get_skill("nonexistent-skill-xyz")
    assert "not found" in err_skill.lower(), "Expected not found error for nonexistent skill"
    print("✓ Error handling for missing skill resource verified.")

    # 2. Test Prompts
    print("\n--- 2. Testing Prompts ---")
    prompt_curate = curate_note("proj:test_curate", "Atomic state synchronization in distributed consensus")
    assert "curating a Knowledge Note" in prompt_curate, "curate_note missing intro"
    assert "search_commonplace" in prompt_curate, "curate_note missing search_commonplace"
    assert "create_note" in prompt_curate, "curate_note missing create_note"
    print("✓ curate_note prompt template verified.")

    prompt_cat = author_catalog("proj:test_cat", "recipe", "High-Altitude Sourdough Bread")
    assert "authoring a structured catalog" in prompt_cat, "author_catalog missing intro"
    assert "create_catalog_entry" in prompt_cat, "author_catalog missing create_catalog_entry"
    assert "items" in prompt_cat and "steps" in prompt_cat and "metrics" in prompt_cat, "author_catalog missing schema fields"
    print("✓ author_catalog prompt template verified.")

    # 3. Setup Project & Identity Anchors
    print("\n--- 3. Setting Up Anchors ---")
    p_res = await ensure_node(
        type="PROJECT",
        id="project:atmo_suite",
        description="Atmosphere MCP Verification Project",
        status="ACTIVE"
    )
    print("ensure_node(PROJECT):", p_res)

    a_res = await ensure_node(
        type="IDENTITY",
        id="identity:atmo_verifier",
        description="Atmosphere MCP Test Agent",
        status="ACTIVE"
    )
    print("ensure_node(IDENTITY):", a_res)
    print("✓ Anchors created/confirmed.")

    import time
    run_id = str(int(time.time() * 1000))
    tag_name = f"SYN_{run_id}"

    # 4. Test create_note & Duplicate Detection
    print("\n--- 4. Testing create_note & Duplicate Detection ---")
    note_uuid = f"note-atmo-{run_id}"
    note_stmt = f"Atmosphere subagents require bidirectional synapse verification on creation [{run_id}]."
    note_content = "Detailed documentation on why bidirectional links ensure graph integrity."

    res_note1 = await create_note(
        project_id="project:atmo_suite",
        agent_id="identity:atmo_verifier",
        statement=note_stmt,
        content=note_content,
        references=[{
            "title": "Autonomous Swarms",
            "creator": "Substrate Research",
            "page_numbers": "12-14",
            "excerpt": "Synapses between nodes form the semantic backbone."
        }],
        note_links=[{
            "target_uuid": "identity:atmo_verifier",
            "relation": "CREATED_BY",
            "context": "Author provenance"
        }],
        tags=["ATMOSPHERE", tag_name, "INTEGRITY"],
        ka=31,
        uuid=note_uuid,
        check_duplicates=True
    )
    print("create_note (initial creation):", res_note1)
    assert "Successfully committed" in res_note1 or "COMMITTED" in res_note1, f"Unexpected create_note response: {res_note1}"

    # Attempt second creation with identical statement and check_duplicates=True
    res_note2 = await create_note(
        project_id="project:atmo_suite",
        agent_id="identity:atmo_verifier",
        statement=note_stmt,
        content="Attempting duplicate insertion",
        check_duplicates=True
    )
    print("create_note (duplicate check attempt):", res_note2)
    dup_data = json.loads(res_note2)
    assert dup_data.get("status") == "ALREADY_EXISTS", f"Expected ALREADY_EXISTS, got {dup_data}"
    assert dup_data.get("uuid") == note_uuid, f"Expected duplicate uuid {note_uuid}, got {dup_data.get('uuid')}"
    assert "Duplicate statement detected" in dup_data.get("message", ""), "Expected duplicate warning message"
    print("✓ create_note duplicate detection verified.")

    # Test force creation with check_duplicates=False
    forced_uuid = f"note-atmo-{run_id}-forced"
    res_note_force = await create_note(
        project_id="project:atmo_suite",
        agent_id="identity:atmo_verifier",
        statement=note_stmt,
        content="Forced duplicate insertion",
        uuid=forced_uuid,
        check_duplicates=False
    )
    print("create_note (force duplicate with check_duplicates=False):", res_note_force)
    assert "Successfully committed" in res_note_force or "COMMITTED" in res_note_force
    print("✓ create_note forced creation with check_duplicates=False verified.")

    # 5. Test get_node
    print("\n--- 5. Testing get_node ---")
    node_str = await get_node(uuid=note_uuid)
    node_data = json.loads(node_str)
    assert node_data.get("uuid") == note_uuid or node_data.get("id") == note_uuid, f"Node uuid mismatch: {node_data}"
    assert node_data.get("statement") == note_stmt, f"Node statement mismatch: {node_data}"
    assert node_data.get("ka") == 31, f"Node KA mismatch: {node_data}"
    assert "ATMOSPHERE" in node_data.get("tags", []), f"Node tags mismatch: {node_data}"
    print(f"✓ get_node returned hydrated atom: {node_data.get('statement')}")

    # 6. Test create_catalog_entry & Duplicate Detection
    print("\n--- 6. Testing create_catalog_entry & Duplicate Detection ---")
    cat_uuid = f"cat-atmo-{run_id}"
    cat_stmt = f"Runbook: Automated Cold Restart of Substrate Shards [{run_id}]"
    cat_content = "Step-by-step procedure for cleanly restarting sharded LMDB storage nodes."

    res_cat1 = await create_catalog_entry(
        project_id="project:atmo_suite",
        agent_id="identity:atmo_verifier",
        statement=cat_stmt,
        content=cat_content,
        items=[
            {"name": "Cluster Controller", "quantity": 1.0, "unit": "server", "role": "coordinator", "notes": "Primary node"},
            {"name": "Storage Shards", "quantity": 16.0, "unit": "shards", "role": "storage", "notes": "LMDB targets"}
        ],
        steps=[
            {"step_number": 1, "instruction": "Drain pending writes", "duration_seconds": 30, "notes": "Wait for flush"},
            {"step_number": 2, "instruction": "Issue SIGTERM to daemon", "duration_seconds": 15, "notes": "Clean unmount"}
        ],
        metrics=[
            {"name": "downtime_seconds", "value": 45.0, "unit": "seconds"},
            {"name": "data_loss_bytes", "value": 0.0, "unit": "bytes"}
        ],
        attributes={"environment": "production", "criticality": "high"},
        tags=["RUNBOOK", "OPERATIONS", "LMDB"],
        ka=27,
        uuid=cat_uuid,
        check_duplicates=True
    )
    print("create_catalog_entry (initial creation):", res_cat1)
    assert "Successfully committed" in res_cat1 or "COMMITTED" in res_cat1

    # Duplicate check for catalog entry
    res_cat2 = await create_catalog_entry(
        project_id="project:atmo_suite",
        agent_id="identity:atmo_verifier",
        statement=cat_stmt,
        check_duplicates=True
    )
    print("create_catalog_entry (duplicate check attempt):", res_cat2)
    cat_dup_data = json.loads(res_cat2)
    assert cat_dup_data.get("status") == "ALREADY_EXISTS", f"Expected ALREADY_EXISTS, got {cat_dup_data}"
    assert cat_dup_data.get("uuid") == cat_uuid
    print("✓ create_catalog_entry duplicate detection verified.")

    # 7. Test search_commonplace
    print("\n--- 7. Testing search_commonplace ---")
    # 7a. Query search
    s_query_str = await search_commonplace(query=f"Cold Restart of Substrate Shards [{run_id}]", limit=10)
    s_query_data = json.loads(s_query_str)
    assert s_query_data["count"] >= 1, "Expected search matches for Cold Restart"
    uuids = [m["uuid"] for m in s_query_data["matches"]]
    assert cat_uuid in uuids, f"Expected {cat_uuid} in search matches: {uuids}"
    print(f"✓ search_commonplace(query) found {s_query_data['count']} match(es).")

    # 7b. KA filter
    s_ka_str = await search_commonplace(query=f"[{run_id}]", ka=27)
    s_ka_data = json.loads(s_ka_str)
    assert s_ka_data["count"] >= 1, "Expected KA 27 matches"
    for m in s_ka_data["matches"]:
        assert m["ka"] == 27, f"Expected KA 27, got {m['ka']}"
    print(f"✓ search_commonplace(ka=27) verified.")

    # 7c. Tags filter
    s_tag_str = await search_commonplace(query="", tags=[tag_name])
    s_tag_data = json.loads(s_tag_str)
    assert s_tag_data["count"] >= 1, f"Expected matches for tag {tag_name}"
    tag_uuids = [m["uuid"] for m in s_tag_data["matches"]]
    assert note_uuid in tag_uuids, f"Expected {note_uuid} in tag matches: {tag_uuids}"
    print(f"✓ search_commonplace(tags=['{tag_name}']) verified.")

    # 8. Test link_nodes and get_node_links
    print("\n--- 8. Testing link_nodes & get_node_links ---")
    l_res = await link_nodes(
        source=note_uuid,
        target=cat_uuid,
        label="REFERENCES",
        weight=0.95
    )
    print("link_nodes:", l_res)

    # Inbound links for cat_uuid
    in_str = await get_node_links(uuid=cat_uuid, direction="inbound")
    in_data = json.loads(in_str)
    inbound_sources = [link["source"] for link in in_data.get("inbound", [])]
    assert note_uuid in inbound_sources, f"Expected {note_uuid} in inbound links: {in_data}"
    print(f"✓ get_node_links(inbound) verified: {inbound_sources}")

    # Outbound links for note_uuid
    out_str = await get_node_links(uuid=note_uuid, direction="outbound")
    out_data = json.loads(out_str)
    outbound_targets = [link["target"] for link in out_data.get("outbound", [])]
    assert cat_uuid in outbound_targets, f"Expected {cat_uuid} in outbound links: {out_data}"
    print(f"✓ get_node_links(outbound) verified: {outbound_targets}")

    # 9. Test export_graph_rdf
    print("\n--- 9. Testing export_graph_rdf ---")
    rdf_text = await export_graph_rdf()
    assert "@prefix" in rdf_text, "RDF Turtle missing @prefix"
    assert "schema:" in rdf_text or "asos:" in rdf_text, "RDF Turtle missing expected ontology prefixes"
    print(f"✓ export_graph_rdf verified ({len(rdf_text)} bytes received).")

    # 10. Test via FastMCP call_tool interface
    print("\n--- 10. Testing FastMCP call_tool interface ---")
    mcp_call_res = await mcp.call_tool("search_commonplace", {"query": f"bidirectional synapse verification on creation [{run_id}]"})
    assert len(mcp_call_res[0]) > 0
    mcp_text = mcp_call_res[0][0].text
    mcp_data = json.loads(mcp_text)
    assert mcp_data["count"] >= 1
    print("✓ FastMCP mcp.call_tool interface verified.")

    print("\n" + "=" * 60)
    print("ALL ASOS FASTMCP ATMOSPHERE VERIFICATION TESTS PASSED!")
    print("=" * 60 + "\n")


async def main():
    try:
        await ensure_backend()
        await run_tests()
    finally:
        if started_process:
            print("[CLEANUP] Stopping spawned backend daemon...")
            started_process.terminate()
            started_process.wait()
            print("[CLEANUP] Done.")


if __name__ == "__main__":
    asyncio.run(main())
