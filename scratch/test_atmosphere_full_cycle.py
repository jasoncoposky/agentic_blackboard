#!/usr/bin/env python3
"""
End-to-End Atmosphere Integration & Full Cycle Verification Script.

Validates the complete end-to-end cycle for Project Nucleus Atmosphere agent tools:
  1. Agent searches for existing note (search_commonplace) - Pre-flight discovery
  2. Creates new note with citation and link (create_note)
  3. Pre-flight duplicate check rejects identical statement (check_duplicates=True)
  4. Creates catalog recipe entry (create_catalog_entry) with items, steps, metrics
  5. Inspects backlinks (get_node_links) and verifies inbound and outbound synapses
  6. Exports W3C RDF Turtle (export_graph_rdf) and asserts schema:Recipe, schema:citation, and rel predicates
  7. Reads skill resources via MCP (asos://skills/commonplace-curation, asos://skills/procedural-catalog)
  8. Verifies multi-tenancy ACL enforcement across search, links, and export
  9. Verifies both Python FastMCP and Node.js MCP tools against the live daemon!

Lifecycle:
  - Connects to existing ASOS daemon or starts build/asos_daemon on port 8085
  - Cleans up child processes upon exit
  - Exits with 0 on total success
"""

import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import httpx

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

# Import MCP tools and FastMCP instance
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
    DEFAULT_TIMEOUT,
    RDF_TIMEOUT,
)

started_process = None


async def ensure_backend():
    """Ensure the ASOS daemon is running and reachable."""
    global started_process
    try:
        async with httpx.AsyncClient() as client:
            resp = await client.get(f"{ASOS_API_URL}/schema", timeout=2.0)
            if resp.status_code == 200:
                print(f"[DAEMON] Connected to running ASOS daemon at {ASOS_API_URL}")
                return
    except Exception:
        pass

    daemon_bin = REPO_ROOT / "build" / "asos_daemon"
    if not daemon_bin.exists():
        raise RuntimeError(f"Daemon binary not found at {daemon_bin}. Run cmake build first!")

    print(f"[DAEMON] Starting ASOS daemon: {daemon_bin}")
    started_process = subprocess.Popen(
        [str(daemon_bin)],
        cwd=str(REPO_ROOT),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )

    # Wait up to 30 seconds for daemon to initialize
    for _ in range(150):
        if started_process and started_process.poll() is not None:
            raise RuntimeError(f"ASOS daemon exited prematurely with code {started_process.returncode}")
        await asyncio.sleep(0.2)
        try:
            async with httpx.AsyncClient() as client:
                resp = await client.get(f"{ASOS_API_URL}/schema", timeout=1.0)
                if resp.status_code == 200:
                    print(f"[DAEMON] ASOS daemon initialized and responsive at {ASOS_API_URL}")
                    return
        except Exception:
            pass

    raise RuntimeError("Timed out waiting for ASOS daemon to start on port 8085.")


async def run_full_cycle():
    run_ts = int(time.time() * 1000)
    tag_session = f"E2E_{run_ts}"

    print("\n" + "=" * 70)
    print("STARTING ATMOSPHERE AGENT COMMONPLACE FULL CYCLE INTEGRATION TEST")
    print(f"Session Tag: {tag_session}")
    print("=" * 70)

    # Setup Anchors
    print("\n--- [Setup] Anchoring Project and Agent Identities ---")
    p_res = await ensure_node(
        type="PROJECT",
        id="project:atmosphere_e2e",
        description="Atmosphere End-to-End Test Project",
        status="ACTIVE"
    )
    print("Project Anchor:", p_res)

    a_res = await ensure_node(
        type="IDENTITY",
        id="identity:atmosphere_e2e_agent",
        description="Atmosphere Autonomous Curating Agent",
        status="ACTIVE"
    )
    print("Agent Identity Anchor:", a_res)

    # =========================================================================
    # Checkpoint 1: Agent searches for existing note (search_commonplace)
    # =========================================================================
    print("\n" + "-" * 70)
    print("CHECKPOINT 1: Agent searches for existing note (search_commonplace)")
    print("-" * 70)
    search_term = "Byzantine Fault Tolerant Synapse Consensus"
    search_raw = await search_commonplace(query=search_term, limit=10)
    search_data = json.loads(search_raw)
    assert "matches" in search_data, f"search_commonplace output missing 'matches': {search_data}"
    assert "count" in search_data, f"search_commonplace output missing 'count': {search_data}"
    print(f"Pre-flight discovery search for '{search_term}': {search_data['count']} matches found.")
    print("✓ Checkpoint 1 PASSED: Agent pre-flight search executed cleanly.")

    # =========================================================================
    # Checkpoint 2: Creates new note with citation and link (create_note)
    # =========================================================================
    print("\n" + "-" * 70)
    print("CHECKPOINT 2: Creates new note with citation and link (create_note)")
    print("-" * 70)
    note_uuid = f"note-atmo-e2e-{run_ts}"
    note_statement = f"Bidirectional synapse validation establishes resilient dialectic topology [{run_ts}]."
    note_content = (
        "In distributed multi-agent substrates, creating typed dialectic links (SUPPORTS, EXTENDS, REFUTES) "
        "paired with bibliographic citations grounds assertions in verifiable evidence."
    )
    res_note = await create_note(
        project_id="project:atmosphere_e2e",
        agent_id="identity:atmosphere_e2e_agent",
        statement=note_statement,
        content=note_content,
        references=[{
            "title": "Verifiable Graph Topologies",
            "creator": "Atmosphere Working Group",
            "page_numbers": "pp. 42-49",
            "excerpt": "A synapse connects an atomic assertion to its grounding citation or dialectic peer.",
            "uuid": "urn:isbn:978-0-123456-78-9",
            "tags": ["TOPOLOGY", "PEER_REVIEWED"]
        }],
        note_links=[{
            "target_uuid": "identity:atmosphere_e2e_agent",
            "relation": "CREATED_BY",
            "context": "Agent provenance assertion"
        }],
        tags=["ATMOSPHERE", "TOPOLOGY", tag_session],
        ka=26,  # LITERATURE_READING
        uuid=note_uuid,
        check_duplicates=True
    )
    print("create_note response:", res_note)
    note_res_data = json.loads(res_note)
    assert note_res_data.get("status") == "COMMITTED", f"Expected COMMITTED, got {note_res_data}"
    assert note_res_data.get("uuid") == note_uuid, f"Expected {note_uuid}, got {note_res_data.get('uuid')}"

    # Verify retrieval via get_node
    node_raw = await get_node(uuid=note_uuid)
    node_data = json.loads(node_raw)
    assert node_data.get("statement") == note_statement, f"Hydrated statement mismatch: {node_data}"
    assert node_data.get("ka") == 26, f"Hydrated KA mismatch: {node_data}"
    assert tag_session in node_data.get("tags", []), f"Hydrated tags missing {tag_session}: {node_data}"
    print(f"Hydrated node retrieved: '{node_data.get('statement')}' (KA: {node_data.get('ka')})")
    print("✓ Checkpoint 2 PASSED: Note created with citations, links, and hydrated retrieval verified.")

    # =========================================================================
    # Checkpoint 3: Pre-flight duplicate check rejects identical statement
    # =========================================================================
    print("\n" + "-" * 70)
    print("CHECKPOINT 3: Pre-flight duplicate check rejects identical statement (check_duplicates=True)")
    print("-" * 70)
    dup_res = await create_note(
        project_id="project:atmosphere_e2e",
        agent_id="identity:atmosphere_e2e_agent",
        statement=note_statement,
        content="Attempting duplicate note submission.",
        check_duplicates=True
    )
    print("create_note duplicate response:", dup_res)
    dup_data = json.loads(dup_res)
    assert dup_data.get("status") == "ALREADY_EXISTS", f"Expected ALREADY_EXISTS, got {dup_data}"
    assert dup_data.get("uuid") == note_uuid, f"Expected existing UUID {note_uuid}, got {dup_data.get('uuid')}"
    assert "Duplicate statement detected" in dup_data.get("message", ""), "Expected duplicate warning message."

    # Test explicit bypass with check_duplicates=False
    forced_uuid = f"note-forced-e2e-{run_ts}"
    force_res = await create_note(
        project_id="project:atmosphere_e2e",
        agent_id="identity:atmosphere_e2e_agent",
        statement=note_statement,
        content="Forced intentional duplicate note.",
        uuid=forced_uuid,
        check_duplicates=False
    )
    force_data = json.loads(force_res)
    assert force_data.get("status") == "COMMITTED", f"Expected COMMITTED for forced create, got {force_data}"
    assert force_data.get("uuid") == forced_uuid
    print(f"Forced creation verified with check_duplicates=False: {forced_uuid}")
    print("✓ Checkpoint 3 PASSED: Duplicate detection and forced bypass validated.")

    # =========================================================================
    # Checkpoint 4: Creates catalog recipe entry (create_catalog_entry)
    # =========================================================================
    print("\n" + "-" * 70)
    print("CHECKPOINT 4: Creates catalog recipe entry (create_catalog_entry) with items, steps, metrics")
    print("-" * 70)
    recipe_uuid = f"recipe-e2e-{run_ts}"
    recipe_statement = f"Culinary Recipe: High-Hydration Rustic Sourdough Loaf [{run_ts}]"
    recipe_content = "Comprehensive procedural formula for baking artisanal naturally leavened sourdough bread."
    res_recipe = await create_catalog_entry(
        project_id="project:atmosphere_e2e",
        agent_id="identity:atmosphere_e2e_agent",
        statement=recipe_statement,
        content=recipe_content,
        items=[
            {"name": "Bread Flour", "quantity": 450.0, "unit": "g", "role": "flour", "notes": "12.7% protein"},
            {"name": "Whole Rye Flour", "quantity": 50.0, "unit": "g", "role": "flour", "notes": "Stone-ground"},
            {"name": "Water", "quantity": 375.0, "unit": "g", "role": "hydration", "notes": "75% hydration at 28C"},
            {"name": "Active Sourdough Starter", "quantity": 100.0, "unit": "g", "role": "leavening", "notes": "100% hydration"},
            {"name": "Fine Sea Salt", "quantity": 10.0, "unit": "g", "role": "seasoning", "notes": "2% salt"}
        ],
        steps=[
            {"step_number": 1, "instruction": "Autolyse flour and water for 45 minutes", "duration_seconds": 2700, "notes": "Resting phase"},
            {"step_number": 2, "instruction": "Incorporate ripe starter and salt thoroughly", "duration_seconds": 600, "notes": "Rubaud method"},
            {"step_number": 3, "instruction": "Bulk fermentation with 4 sets of stretch and folds", "duration_seconds": 14400, "notes": "Rest 30m between folds"},
            {"step_number": 4, "instruction": "Pre-shape, bench rest for 20 minutes, final shaping into banneton", "duration_seconds": 1800, "notes": "Build surface tension"},
            {"step_number": 5, "instruction": "Bake in preheated Dutch oven at 245C with lid on", "duration_seconds": 1200, "notes": "Steam generation"}
        ],
        metrics=[
            {"name": "total_hydration_percent", "value": 75.0, "unit": "%"},
            {"name": "bulk_fermentation_temp_c", "value": 26.0, "unit": "C"},
            {"name": "baking_temp_c", "value": 245.0, "unit": "C"},
            {"name": "yield_loaves", "value": 1.0, "unit": "loaf"}
        ],
        attributes={"cuisine": "Artisanal Baking", "difficulty": "Intermediate", "category": "Bread"},
        tags=["RECIPE", "CULINARY", "SOURDOUGH", tag_session],
        ka=27,  # CULINARY_RECIPES
        uuid=recipe_uuid,
        check_duplicates=True
    )
    print("create_catalog_entry response:", res_recipe)
    recipe_res_data = json.loads(res_recipe)
    assert recipe_res_data.get("status") == "COMMITTED", f"Expected COMMITTED, got {recipe_res_data}"
    assert recipe_res_data.get("uuid") == recipe_uuid, f"Expected {recipe_uuid}, got {recipe_res_data.get('uuid')}"

    # Verify duplicate rejection on catalog entry
    cat_dup_res = await create_catalog_entry(
        project_id="project:atmosphere_e2e",
        agent_id="identity:atmosphere_e2e_agent",
        statement=recipe_statement,
        check_duplicates=True
    )
    cat_dup_data = json.loads(cat_dup_res)
    assert cat_dup_data.get("status") == "ALREADY_EXISTS", f"Expected ALREADY_EXISTS, got {cat_dup_data}"
    assert cat_dup_data.get("uuid") == recipe_uuid
    print(f"Catalog duplicate detection verified: {cat_dup_data.get('status')}")
    print("✓ Checkpoint 4 PASSED: Catalog recipe entry created with items, steps, metrics, and deduplication.")

    # =========================================================================
    # Checkpoint 5: Inspects backlinks (get_node_links) and verifies inbound and outbound synapses
    # =========================================================================
    print("\n" + "-" * 70)
    print("CHECKPOINT 5: Inspects backlinks (get_node_links) and verifies inbound and outbound synapses")
    print("-" * 70)
    # Link note to recipe with PAIRS_WITH relation
    link_res = await link_nodes(
        source=note_uuid,
        target=recipe_uuid,
        label="PAIRS_WITH",
        weight=0.9
    )
    print("link_nodes response:", link_res)

    # Inbound backlinks inspection on recipe
    in_raw = await get_node_links(uuid=recipe_uuid, direction="inbound")
    in_data = json.loads(in_raw)
    inbound_list = in_data.get("inbound", [])
    inbound_sources = [link["source"] for link in inbound_list]
    assert note_uuid in inbound_sources, f"Expected {note_uuid} in inbound links of recipe: {in_data}"
    in_link = next(link for link in inbound_list if link["source"] == note_uuid)
    assert in_link["relation"] == "PAIRS_WITH", f"Expected PAIRS_WITH, got {in_link.get('relation')}"
    assert in_link.get("statement"), f"Expected populated statement in inbound link: {in_link}"
    print(f"Recipe inbound backlinks found: {inbound_sources}")

    # Outbound synapses inspection on note
    out_raw = await get_node_links(uuid=note_uuid, direction="outbound")
    out_data = json.loads(out_raw)
    outbound_list = out_data.get("outbound", [])
    outbound_targets = [link["target"] for link in outbound_list]
    assert recipe_uuid in outbound_targets, f"Expected {recipe_uuid} in outbound links of note: {out_data}"
    out_link = next(link for link in outbound_list if link["target"] == recipe_uuid)
    assert out_link["relation"] == "PAIRS_WITH", f"Expected PAIRS_WITH, got {out_link.get('relation')}"
    assert out_link.get("statement"), f"Expected populated statement in outbound link: {out_link}"
    print(f"Note outbound synapses found: {outbound_targets}")

    # Inspect bidirectional links
    both_raw = await get_node_links(uuid=note_uuid, direction="both")
    both_data = json.loads(both_raw)
    assert len(both_data.get("outbound", [])) >= 1, "Expected at least one outbound synapse"
    print(f"Bidirectional synapses for note verified ({len(both_data['outbound'])} outbound, {len(both_data['inbound'])} inbound).")
    print("✓ Checkpoint 5 PASSED: Bidirectional synapse links and backlinks verified.")

    # =========================================================================
    # Checkpoint 6: Exports W3C RDF Turtle (export_graph_rdf) and asserts schema:Recipe, schema:citation, and rel predicates
    # =========================================================================
    print("\n" + "-" * 70)
    print("CHECKPOINT 6: Exports W3C RDF Turtle (export_graph_rdf) with schema:Recipe, schema:citation, rel predicates")
    print("-" * 70)
    rdf_text = await export_graph_rdf()
    assert "@prefix schema: <http://schema.org/>" in rdf_text, "Missing @prefix schema in RDF"
    assert "@prefix asos: <http://asos.substrate.ai/schema#>" in rdf_text, "Missing @prefix asos in RDF"
    assert "schema:Recipe" in rdf_text, "Missing schema:Recipe in RDF export"
    assert "schema:citation" in rdf_text, "Missing schema:citation in RDF export"
    assert "schema:recipeIngredient" in rdf_text, "Missing schema:recipeIngredient in RDF export"
    assert "schema:recipeInstructions" in rdf_text, "Missing schema:recipeInstructions in RDF export"

    # Verify rel predicate mapping
    assert "asos:pairsWith" in rdf_text, "Missing asos:pairsWith in RDF export"
    print(f"RDF Turtle export validated ({len(rdf_text)} bytes). Verified schema:Recipe, schema:citation, and rel predicates.")
    print("✓ Checkpoint 6 PASSED: W3C RDF Turtle ontology export fully asserted.")

    # =========================================================================
    # Checkpoint 7: Reads skill resources via MCP (asos://skills/commonplace-curation, asos://skills/procedural-catalog)
    # =========================================================================
    print("\n" + "-" * 70)
    print("CHECKPOINT 7: Reads skill resources via MCP (commonplace-curation, procedural-catalog)")
    print("-" * 70)
    curation_res = await mcp.read_resource("asos://skills/commonplace-curation")
    assert len(curation_res) > 0, "No content for asos://skills/commonplace-curation"
    curation_text = curation_res[0].content
    assert "# Commonplace Curation Skill" in curation_text, "Missing title in commonplace-curation skill"
    assert "commonplace-curation" in curation_text, "Missing name in commonplace-curation skill"
    assert "Atomic Notes" in curation_text, "Missing Atomic Notes section in commonplace-curation"
    assert "Reference" in curation_text, "Missing Reference section in commonplace-curation"
    assert "NoteLink" in curation_text, "Missing NoteLink section in commonplace-curation"
    print(f"Read resource asos://skills/commonplace-curation: {len(curation_text)} bytes.")

    catalog_res = await mcp.read_resource("asos://skills/procedural-catalog")
    assert len(catalog_res) > 0, "No content for asos://skills/procedural-catalog"
    catalog_text = catalog_res[0].content
    assert "# Procedural Catalog Skill" in catalog_text, "Missing title in procedural-catalog skill"
    assert "procedural-catalog" in catalog_text, "Missing name in procedural-catalog skill"
    assert "CatalogItem" in catalog_text, "Missing CatalogItem section in procedural-catalog"
    assert "CatalogStep" in catalog_text, "Missing CatalogStep section in procedural-catalog"
    assert "CatalogMetric" in catalog_text, "Missing CatalogMetric section in procedural-catalog"
    print(f"Read resource asos://skills/procedural-catalog: {len(catalog_text)} bytes.")

    # Also verify knowledge-capture and schema resources
    kc_res = await mcp.read_resource("asos://skills/knowledge-capture")
    assert len(kc_res) > 0 and "# Knowledge Capture Skill" in kc_res[0].content
    schema_res = await mcp.read_resource("asos://schema")
    assert len(schema_res) > 0 and "knowledge_areas" in schema_res[0].content

    # Path traversal protection
    traversal_err = await get_skill("../../../etc/passwd")
    assert "not found" in traversal_err.lower(), f"Unexpected traversal response: {traversal_err}"
    print("✓ Checkpoint 7 PASSED: MCP skills and schema resources successfully read with traversal security.")

    # =========================================================================
    # Checkpoint 8: Verifies multi-tenancy ACL enforcement across search, links, and export
    # =========================================================================
    print("\n" + "-" * 70)
    print("CHECKPOINT 8: Verifies multi-tenancy ACL enforcement across search, links, and export")
    print("-" * 70)
    alice_ts = int(time.time() * 1000)
    alice_stmt = f"Confidential multi-tenant neural hyperparameter optimization [{alice_ts}]."
    alice_uuid = f"note-alice-priv-{alice_ts}"

    res_alice = await create_note(
        project_id="project:atmosphere_e2e",
        agent_id="identity:tenant-alice",
        statement=alice_stmt,
        content="Confidential tensor optimization specifications strictly scoped to tenant-alice.",
        tags=["CONFIDENTIAL", "TENANT_ALICE", tag_session],
        uuid=alice_uuid,
        active_user="tenant-alice",
        check_duplicates=True
    )
    alice_data = json.loads(res_alice)
    assert alice_data.get("status") == "COMMITTED", f"Failed creating Alice note: {alice_data}"
    print(f"Created Alice private note: {alice_uuid}")

    # 8a. Multi-tenant Search Isolation
    bob_search_raw = await search_commonplace(query=alice_stmt, active_user="tenant-bob")
    bob_search = json.loads(bob_search_raw)
    bob_matches = [m["uuid"] for m in bob_search.get("matches", [])]
    assert alice_uuid not in bob_matches, f"ACL Failure: Bob found Alice's private note in search! {bob_matches}"

    alice_search_raw = await search_commonplace(query=alice_stmt, active_user="tenant-alice")
    alice_search = json.loads(alice_search_raw)
    alice_matches = [m["uuid"] for m in alice_search.get("matches", [])]
    assert alice_uuid in alice_matches, f"Alice could not find her own note in search! {alice_matches}"
    print("✓ 8a. Search ACL isolation between Bob and Alice verified.")

    # 8b. Multi-tenant Links Isolation
    bob_links_raw = await get_node_links(uuid=alice_uuid, active_user="tenant-bob")
    bob_links = json.loads(bob_links_raw)
    assert bob_links.get("status") == "ERROR" and bob_links.get("code") == 404, (
        f"ACL Failure: Bob was able to query links for Alice's note: {bob_links}"
    )

    alice_links_raw = await get_node_links(uuid=alice_uuid, active_user="tenant-alice")
    alice_links = json.loads(alice_links_raw)
    assert "inbound" in alice_links and "outbound" in alice_links, f"Alice failed to query own links: {alice_links}"
    print("✓ 8b. Node links ACL isolation between Bob and Alice verified.")

    # 8c. Multi-tenant Export Isolation
    bob_rdf = await export_graph_rdf(active_user="tenant-bob")
    assert alice_uuid not in bob_rdf, f"ACL Failure: Alice's UUID {alice_uuid} leaked into Bob's RDF export!"
    assert alice_stmt not in bob_rdf, "ACL Failure: Alice's statement leaked into Bob's RDF export!"

    alice_rdf = await export_graph_rdf(active_user="tenant-alice")
    assert alice_uuid in alice_rdf, f"Alice's private note {alice_uuid} missing from Alice's RDF export!"
    print("✓ 8c. RDF Export ACL isolation between Bob and Alice verified.")
    print("✓ Checkpoint 8 PASSED: Multi-tenancy ACL enforcement verified across search, links, and export.")

    # =========================================================================
    # Checkpoint 9: Verifies both Python FastMCP and Node.js MCP tools against the live daemon!
    # =========================================================================
    print("\n" + "-" * 70)
    print("CHECKPOINT 9: Verifies both Python FastMCP and Node.js MCP tools against the live daemon!")
    print("-" * 70)

    # 9a. Python FastMCP Tool Invocation Protocol (mcp.call_tool)
    print("Verifying Python FastMCP protocol execution (mcp.call_tool)...")
    mcp_call_search = await mcp.call_tool("search_commonplace", {"query": f"Rustic Sourdough Loaf [{run_ts}]"})
    assert len(mcp_call_search[0]) > 0, "No content returned from mcp.call_tool('search_commonplace')"
    mcp_search_json = json.loads(mcp_call_search[0][0].text)
    assert mcp_search_json.get("count", 0) >= 1, f"Expected search match via FastMCP call_tool: {mcp_search_json}"
    print(f"Python FastMCP call_tool('search_commonplace') returned {mcp_search_json['count']} match(es).")

    # 9b. Node.js MCP Tools Verification against the same running daemon
    print("\nExecuting Node.js MCP verification suite (scratch/verify_mcp_node.js)...")
    node_script = REPO_ROOT / "scratch" / "verify_mcp_node.js"
    assert node_script.exists(), f"Node script not found at {node_script}"

    proc = await asyncio.create_subprocess_exec(
        "node",
        str(node_script),
        cwd=str(REPO_ROOT),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE
    )
    stdout, stderr = await asyncio.wait_for(proc.communicate(), timeout=60)
    out_str = stdout.decode("utf-8", errors="replace")
    err_str = stderr.decode("utf-8", errors="replace")

    print(out_str)
    if proc.returncode != 0:
        print("Node.js stderr:\n", err_str)
        raise RuntimeError(f"Node.js MCP verification failed with returncode {proc.returncode}")

    assert "ALL ASOS NODE.JS MCP VERIFICATION TESTS PASSED!" in out_str, "Node.js suite did not report full pass"
    print("✓ Checkpoint 9 PASSED: Dual Python FastMCP and Node.js MCP verified against the live daemon.")

    print("\n" + "=" * 70)
    print("ALL 9 ATMOSPHERE INTEGRATION CHECKPOINTS PASSED SUCCESSFULLY!")
    print("=" * 70 + "\n")


async def main():
    try:
        await ensure_backend()
        await run_full_cycle()
    finally:
        if started_process:
            print("[CLEANUP] Stopping ASOS daemon process...")
            started_process.terminate()
            try:
                started_process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                started_process.kill()
            print("[CLEANUP] Daemon stopped cleanly.")


if __name__ == "__main__":
    asyncio.run(main())
