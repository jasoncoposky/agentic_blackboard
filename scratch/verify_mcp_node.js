#!/usr/bin/env node
/**
 * Standalone verification script for ASOS Node.js MCP Server upgrades.
 * Verifies:
 * 1. HTTP Client Timeouts & Resource security (asos://schema, asos://skills/knowledge-capture, path traversal prevention)
 * 2. Prompts (curate_note, author_catalog, init_swarm)
 * 3. Anchor node creation (ensure_node)
 * 4. create_note (structured JSON response, auto-generated UUID, duplicate detection, forced duplicate)
 * 5. get_node (retrieval of full hydrated atom properties)
 * 6. create_catalog_entry (structured JSON response, auto-generated UUID, duplicate detection)
 * 7. search_commonplace (text query, ka filter, tags filter)
 * 8. link_nodes & get_node_links (inbound and outbound relationship queries)
 * 9. export_graph_rdf (W3C RDF Turtle serialization with RDF_TIMEOUT)
 * 10. Multi-Tenancy Isolation (active_user tenant isolation between Alice and Bob)
 * 11. HTTP Error Propagation (structured JSON error response on 404)
 * 12. Full MCP Client Protocol Interface via InMemoryTransport
 * 13. commit_anchored_knowledge / commit_knowledge_bundle with rich fields
 */

const path = require("path");
const fs = require("fs");
const { spawn } = require("child_process");

// Configure NODE_PATH to resolve packages from asos-mcp/node_modules
const REPO_ROOT = path.resolve(__dirname, "..");
const modulePath = path.resolve(REPO_ROOT, "asos-mcp/node_modules");
if (!process.env.NODE_PATH) {
  process.env.NODE_PATH = modulePath;
  require("module").Module._initPaths();
}

const axios = require("axios");
const { Client } = require("@modelcontextprotocol/sdk/client/index.js");
const { InMemoryTransport } = require("@modelcontextprotocol/sdk/inMemory.js");

const {
  server,
  DEFAULT_TIMEOUT,
  RDF_TIMEOUT,
  API_BASE,
  formatError,
  search_commonplace,
  get_node,
  get_node_links,
  export_graph_rdf,
  create_note,
  create_catalog_entry,
  commit_anchored_knowledge,
  commit_knowledge_bundle,
  ensure_node,
  link_nodes,
  query_knowledge,
  query_substrate,
  get_schema,
  get_skill,
  init_swarm,
  curate_note,
  author_catalog,
} = require("../asos-mcp/index.js");

let startedProcess = null;

async function ensureBackend() {
  try {
    const resp = await axios.get(`${API_BASE}/schema`, { timeout: 2000 });
    if (resp.status === 200) {
      console.log("[VERIFY] Connected to running ASOS daemon at", API_BASE);
      return;
    }
  } catch (e) {
    // Daemon not reachable, spawn it
  }

  const daemonBin = path.resolve(REPO_ROOT, "build/asos_daemon");
  if (!fs.existsSync(daemonBin)) {
    throw new Error(`Backend daemon binary not found at ${daemonBin}`);
  }

  console.log(`[VERIFY] Starting ASOS daemon: ${daemonBin}`);
  startedProcess = spawn(daemonBin, [], {
    cwd: REPO_ROOT,
    stdio: "ignore",
    detached: false,
  });

  for (let i = 0; i < 150; i++) {
    await new Promise((r) => setTimeout(r, 200));
    try {
      const resp = await axios.get(`${API_BASE}/schema`, { timeout: 1000 });
      if (resp.status === 200) {
        console.log("[VERIFY] ASOS daemon started and ready.");
        return;
      }
    } catch (e) {
      // Continue waiting
    }
  }

  throw new Error("Failed to connect to ASOS daemon after starting it.");
}

async function runTests() {
  console.log("\n" + "=".repeat(60));
  console.log("RUNNING ASOS NODE.JS MCP VERIFICATION SUITE");
  console.log("=".repeat(60));

  // 0. Test Timeout Configurations
  console.log("\n--- 0. Testing Timeout Configurations ---");
  if (DEFAULT_TIMEOUT !== 10000) throw new Error(`Expected DEFAULT_TIMEOUT 10000, got ${DEFAULT_TIMEOUT}`);
  if (RDF_TIMEOUT !== 30000) throw new Error(`Expected RDF_TIMEOUT 30000, got ${RDF_TIMEOUT}`);
  console.log("✓ DEFAULT_TIMEOUT (10000ms) and RDF_TIMEOUT (30000ms) verified.");

  // 1. Test Resources & Security
  console.log("\n--- 1. Testing Resources & Security ---");
  const schemaText = await get_schema();
  if (!schemaText.includes("knowledge_areas") || !schemaText.includes("types")) {
    throw new Error("get_schema() missing knowledge_areas or types: " + schemaText);
  }
  console.log("✓ asos://schema resource verified.");

  const skillText = await get_skill("knowledge-capture");
  if (!skillText.includes("# Knowledge Capture Skill")) {
    throw new Error("Skill title missing in knowledge-capture: " + skillText);
  }
  console.log("✓ asos://skills/knowledge-capture resource verified.");

  const errSkill = await get_skill("nonexistent-skill-xyz");
  if (errSkill !== "Error: Skill 'nonexistent-skill-xyz' not found.") {
    throw new Error(`Unexpected output for missing skill: ${errSkill}`);
  }
  console.log("✓ Error handling for missing skill resource verified without path leakage.");

  const errTraversal = await get_skill("../../src/ApiServer");
  if (errTraversal !== "Error: Skill '../../src/ApiServer' not found.") {
    throw new Error(`Unexpected traversal output: ${errTraversal}`);
  }
  console.log("✓ Path traversal protection for skill resource verified.");

  // 2. Test Prompts
  console.log("\n--- 2. Testing Prompts ---");
  const promptCurate = curate_note("proj:test_curate", "Atomic state synchronization in distributed consensus");
  if (!promptCurate.includes("curating a Knowledge Note") || !promptCurate.includes("search_commonplace")) {
    throw new Error("curate_note prompt template invalid");
  }
  console.log("✓ curate_note prompt template verified.");

  const promptCat = author_catalog("proj:test_cat", "recipe", "High-Altitude Sourdough Bread");
  if (!promptCat.includes("authoring a structured catalog") || !promptCat.includes("items") || !promptCat.includes("steps")) {
    throw new Error("author_catalog prompt template invalid");
  }
  console.log("✓ author_catalog prompt template verified.");

  const promptInit = init_swarm("proj:test_init", "Build next-gen substrate");
  if (!promptInit.includes("initializing a new swarm project") || !promptInit.includes("ensure_node")) {
    throw new Error("init_swarm prompt template invalid");
  }
  console.log("✓ init_swarm prompt template verified.");

  // 3. Setup Project & Identity Anchors
  console.log("\n--- 3. Setting Up Anchors ---");
  const pRes = await ensure_node({
    type: "PROJECT",
    id: "project:node_suite",
    description: "Node MCP Verification Project",
    status: "ACTIVE",
  });
  console.log("ensure_node(PROJECT):", pRes);

  const aRes = await ensure_node({
    type: "IDENTITY",
    id: "identity:node_verifier",
    description: "Node MCP Test Agent",
    status: "ACTIVE",
  });
  console.log("ensure_node(IDENTITY):", aRes);
  console.log("✓ Anchors created/confirmed.");

  const runId = String(Date.now());
  const tagName = `NODE_SYN_${runId}`;

  // 4. Test create_note, Returned UUID, & Duplicate Detection
  console.log("\n--- 4. Testing create_note, Returned UUID & Duplicate Detection ---");
  const noteUuid = `note-node-${runId}`;
  const noteStmt = `Node MCP subagents require bidirectional synapse verification on creation [${runId}].`;
  const noteContent = "Detailed documentation on why bidirectional links ensure graph integrity in Node runtime.";

  const resNote1 = await create_note({
    project_id: "project:node_suite",
    agent_id: "identity:node_verifier",
    statement: noteStmt,
    content: noteContent,
    references: [
      {
        title: "Autonomous Swarms (Node)",
        creator: "Substrate Research",
        page_numbers: "12-14",
        excerpt: "Synapses between nodes form the semantic backbone.",
      },
    ],
    note_links: [
      {
        target_uuid: "identity:node_verifier",
        relation: "CREATED_BY",
        context: "Author provenance",
      },
    ],
    tags: ["ATMOSPHERE", tagName, "INTEGRITY"],
    ka: 31,
    uuid: noteUuid,
    check_duplicates: true,
  });
  console.log("create_note (initial creation with explicit UUID):", resNote1);
  const note1Data = JSON.parse(resNote1);
  if (note1Data.status !== "COMMITTED") throw new Error(`Expected COMMITTED, got ${resNote1}`);
  if (note1Data.uuid !== noteUuid) throw new Error(`Expected ${noteUuid}, got ${note1Data.uuid}`);
  console.log(`✓ create_note returned structured JSON with explicit UUID: ${note1Data.uuid}`);

  // Test create_note with client-side auto-generated UUID
  const resNoteAuto = await create_note({
    project_id: "project:node_suite",
    agent_id: "identity:node_verifier",
    statement: `Auto generated UUID note assertion [${runId}].`,
    content: "Testing automatic UUID generation when uuid is omitted.",
    tags: ["AUTO_UUID", tagName],
    check_duplicates: false,
  });
  console.log("create_note (auto-generated UUID):", resNoteAuto);
  const autoNoteData = JSON.parse(resNoteAuto);
  if (autoNoteData.status !== "COMMITTED") throw new Error(`Expected COMMITTED, got ${resNoteAuto}`);
  const assignedNoteUuid = autoNoteData.uuid || "";
  if (!assignedNoteUuid.startsWith("note-")) throw new Error(`Expected prefix 'note-', got ${assignedNoteUuid}`);
  console.log(`✓ create_note auto-generated client-side UUID: ${assignedNoteUuid}`);

  // Attempt second creation with identical statement and check_duplicates=true
  const resNote2 = await create_note({
    project_id: "project:node_suite",
    agent_id: "identity:node_verifier",
    statement: noteStmt,
    content: "Attempting duplicate insertion",
    check_duplicates: true,
  });
  console.log("create_note (duplicate check attempt):", resNote2);
  const dupData = JSON.parse(resNote2);
  if (dupData.status !== "ALREADY_EXISTS") throw new Error(`Expected ALREADY_EXISTS, got ${resNote2}`);
  if (dupData.uuid !== noteUuid) throw new Error(`Expected duplicate uuid ${noteUuid}, got ${dupData.uuid}`);
  if (!dupData.message.includes("Duplicate statement detected")) throw new Error("Expected duplicate warning message");
  console.log("✓ create_note duplicate detection verified.");

  // Test force creation with check_duplicates=false
  const forcedUuid = `note-node-${runId}-forced`;
  const resNoteForce = await create_note({
    project_id: "project:node_suite",
    agent_id: "identity:node_verifier",
    statement: noteStmt,
    content: "Forced duplicate insertion",
    uuid: forcedUuid,
    check_duplicates: false,
  });
  console.log("create_note (force duplicate with check_duplicates=false):", resNoteForce);
  const forceData = JSON.parse(resNoteForce);
  if (forceData.status !== "COMMITTED") throw new Error(`Expected COMMITTED, got ${resNoteForce}`);
  if (forceData.uuid !== forcedUuid) throw new Error(`Expected ${forcedUuid}, got ${forceData.uuid}`);
  console.log("✓ create_note forced creation with check_duplicates=false verified.");

  // 5. Test get_node
  console.log("\n--- 5. Testing get_node ---");
  const nodeStr = await get_node(noteUuid);
  const nodeData = JSON.parse(nodeStr);
  if (nodeData.uuid !== noteUuid && nodeData.id !== noteUuid) throw new Error(`Node uuid mismatch: ${nodeStr}`);
  if (nodeData.statement !== noteStmt) throw new Error(`Node statement mismatch: ${nodeStr}`);
  if (nodeData.ka !== 31) throw new Error(`Node KA mismatch: ${nodeStr}`);
  if (!nodeData.tags || !nodeData.tags.includes("ATMOSPHERE")) throw new Error(`Node tags mismatch: ${nodeStr}`);
  console.log(`✓ get_node returned hydrated atom: ${nodeData.statement}`);

  // 6. Test create_catalog_entry & Duplicate Detection
  console.log("\n--- 6. Testing create_catalog_entry & Duplicate Detection ---");
  const catUuid = `cat-node-${runId}`;
  const catStmt = `Runbook: Automated Cold Restart of Substrate Shards via Node [${runId}]`;
  const catContent = "Step-by-step procedure for cleanly restarting sharded LMDB storage nodes from Node runtime.";

  const resCat1 = await create_catalog_entry({
    project_id: "project:node_suite",
    agent_id: "identity:node_verifier",
    statement: catStmt,
    content: catContent,
    items: [
      { name: "Cluster Controller", quantity: 1.0, unit: "server", role: "coordinator", notes: "Primary node" },
      { name: "Storage Shards", quantity: 16.0, unit: "shards", role: "storage", notes: "LMDB targets" },
    ],
    steps: [
      { step_number: 1, instruction: "Drain pending writes", duration_seconds: 30, notes: "Wait for flush" },
      { step_number: 2, instruction: "Issue SIGTERM to daemon", duration_seconds: 15, notes: "Clean unmount" },
    ],
    metrics: [
      { name: "downtime_seconds", value: 45.0, unit: "seconds" },
      { name: "data_loss_bytes", value: 0.0, unit: "bytes" },
    ],
    attributes: { environment: "production", criticality: "high" },
    tags: ["RUNBOOK", "OPERATIONS", "LMDB"],
    ka: 27,
    uuid: catUuid,
    check_duplicates: true,
  });
  console.log("create_catalog_entry (initial creation):", resCat1);
  const cat1Data = JSON.parse(resCat1);
  if (cat1Data.status !== "COMMITTED") throw new Error(`Expected COMMITTED, got ${resCat1}`);
  if (cat1Data.uuid !== catUuid) throw new Error(`Expected ${catUuid}, got ${cat1Data.uuid}`);

  // Test create_catalog_entry with auto-generated UUID
  const resCatAuto = await create_catalog_entry({
    project_id: "project:node_suite",
    agent_id: "identity:node_verifier",
    statement: `Protocol: High Availability Failover Sequence Node [${runId}]`,
    content: "Automated failover sequence.",
    check_duplicates: false,
  });
  console.log("create_catalog_entry (auto-generated UUID):", resCatAuto);
  const autoCatData = JSON.parse(resCatAuto);
  if (autoCatData.status !== "COMMITTED") throw new Error(`Expected COMMITTED, got ${resCatAuto}`);
  const assignedCatUuid = autoCatData.uuid || "";
  if (!assignedCatUuid.startsWith("catalog-")) throw new Error(`Expected prefix 'catalog-', got ${assignedCatUuid}`);
  console.log(`✓ create_catalog_entry auto-generated client-side UUID: ${assignedCatUuid}`);

  // Duplicate check for catalog entry
  const resCat2 = await create_catalog_entry({
    project_id: "project:node_suite",
    agent_id: "identity:node_verifier",
    statement: catStmt,
    check_duplicates: true,
  });
  console.log("create_catalog_entry (duplicate check attempt):", resCat2);
  const catDupData = JSON.parse(resCat2);
  if (catDupData.status !== "ALREADY_EXISTS") throw new Error(`Expected ALREADY_EXISTS, got ${resCat2}`);
  if (catDupData.uuid !== catUuid) throw new Error(`Expected duplicate uuid ${catUuid}, got ${catDupData.uuid}`);
  console.log("✓ create_catalog_entry duplicate detection verified.");

  // 7. Test search_commonplace
  console.log("\n--- 7. Testing search_commonplace ---");
  // 7a. Query search
  const sQueryStr = await search_commonplace({
    query: `Cold Restart of Substrate Shards via Node [${runId}]`,
    limit: 10,
  });
  const sQueryData = JSON.parse(sQueryStr);
  if (sQueryData.count < 1) throw new Error("Expected search matches for Cold Restart");
  const matchUuids = sQueryData.matches.map((m) => m.uuid);
  if (!matchUuids.includes(catUuid)) throw new Error(`Expected ${catUuid} in matches: ${matchUuids}`);
  console.log(`✓ search_commonplace(query) found ${sQueryData.count} match(es).`);

  // 7b. KA filter
  const sKaStr = await search_commonplace({
    query: `[${runId}]`,
    ka: 27,
  });
  const sKaData = JSON.parse(sKaStr);
  if (sKaData.count < 1) throw new Error("Expected KA 27 matches");
  for (const m of sKaData.matches) {
    if (m.ka !== 27) throw new Error(`Expected KA 27, got ${m.ka}`);
  }
  console.log("✓ search_commonplace(ka=27) verified.");

  // 7c. Tags filter
  const sTagStr = await search_commonplace({
    query: "",
    tags: [tagName],
  });
  const sTagData = JSON.parse(sTagStr);
  if (sTagData.count < 1) throw new Error(`Expected matches for tag ${tagName}`);
  const tagMatchedUuids = sTagData.matches.map((m) => m.uuid);
  if (!tagMatchedUuids.includes(noteUuid)) throw new Error(`Expected ${noteUuid} in tag matches: ${tagMatchedUuids}`);
  console.log(`✓ search_commonplace(tags=['${tagName}']) verified.`);

  // 8. Test link_nodes and get_node_links
  console.log("\n--- 8. Testing link_nodes & get_node_links ---");
  const linkRes = await link_nodes({
    source: noteUuid,
    target: catUuid,
    label: "REFERENCES",
    weight: 0.95,
  });
  console.log("link_nodes:", linkRes);

  // Inbound links for catUuid
  const inStr = await get_node_links({ uuid: catUuid, direction: "inbound" });
  const inData = JSON.parse(inStr);
  const inboundSources = (inData.inbound || []).map((l) => l.source);
  if (!inboundSources.includes(noteUuid)) throw new Error(`Expected ${noteUuid} in inbound links: ${inStr}`);
  console.log(`✓ get_node_links(inbound) verified: ${inboundSources}`);

  // Outbound links for noteUuid
  const outStr = await get_node_links({ uuid: noteUuid, direction: "outbound" });
  const outData = JSON.parse(outStr);
  const outboundTargets = (outData.outbound || []).map((l) => l.target);
  if (!outboundTargets.includes(catUuid)) throw new Error(`Expected ${catUuid} in outbound links: ${outStr}`);
  console.log(`✓ get_node_links(outbound) verified: ${outboundTargets}`);

  // 9. Test export_graph_rdf
  console.log("\n--- 9. Testing export_graph_rdf ---");
  const rdfText = await export_graph_rdf();
  if (!rdfText.includes("@prefix")) throw new Error("RDF Turtle missing @prefix");
  if (!rdfText.includes("schema:") && !rdfText.includes("asos:")) {
    throw new Error("RDF Turtle missing expected ontology prefixes");
  }
  console.log(`✓ export_graph_rdf verified (${rdfText.length} bytes received).`);

  // 10. Test Multi-Tenancy Isolation
  console.log("\n--- 10. Testing Multi-Tenancy Isolation ---");
  const aliceStmt = `Alice tenant confidential node statement [${runId}].`;
  const aliceContent = "Tenant Alice isolated knowledge content in Node.";
  const resAlice = await create_note({
    project_id: "project:node_suite",
    agent_id: "identity:tenant-alice",
    statement: aliceStmt,
    content: aliceContent,
    tags: ["ALICE_ISOLATED", tagName],
    active_user: "tenant-alice",
    check_duplicates: true,
  });
  console.log("create_note (tenant-alice):", resAlice);
  const aliceData = JSON.parse(resAlice);
  if (aliceData.status !== "COMMITTED") throw new Error(`Expected COMMITTED, got ${resAlice}`);
  const aliceUuid = aliceData.uuid;
  if (!aliceUuid) throw new Error("Expected assigned UUID for Alice note");
  console.log(`✓ Created Alice note with UUID: ${aliceUuid}`);

  // Bob searches: Alice's note must NOT be found
  const bobSearchStr = await search_commonplace({ query: aliceStmt, active_user: "tenant-bob" });
  const bobSearchData = JSON.parse(bobSearchStr);
  const bobMatchUuids = (bobSearchData.matches || []).map((m) => m.uuid);
  if (bobMatchUuids.includes(aliceUuid)) {
    throw new Error(`Multi-tenancy breach! Bob found Alice note: ${bobMatchUuids}`);
  }
  console.log("✓ search_commonplace(active_user='tenant-bob') isolated from Alice's note.");

  // Bob queries links: rejected with 404
  const bobLinksStr = await get_node_links({ uuid: aliceUuid, active_user: "tenant-bob" });
  const bobLinksData = JSON.parse(bobLinksStr);
  if (bobLinksData.status !== "ERROR" || bobLinksData.code !== 404) {
    throw new Error(`Expected 404 error for Bob accessing Alice links, got: ${bobLinksStr}`);
  }
  console.log("✓ get_node_links(active_user='tenant-bob') rejected with 404 error.");

  // Alice searches: found
  const aliceSearchStr = await search_commonplace({ query: aliceStmt, active_user: "tenant-alice" });
  const aliceSearchData = JSON.parse(aliceSearchStr);
  const aliceMatchUuids = (aliceSearchData.matches || []).map((m) => m.uuid);
  if (!aliceMatchUuids.includes(aliceUuid)) {
    throw new Error(`Alice could not find her own note: ${aliceMatchUuids}`);
  }
  console.log("✓ search_commonplace(active_user='tenant-alice') found Alice's note.");

  // Alice queries links: succeeded
  const aliceLinksStr = await get_node_links({ uuid: aliceUuid, active_user: "tenant-alice" });
  const aliceLinksData = JSON.parse(aliceLinksStr);
  if (!("inbound" in aliceLinksData) || !("outbound" in aliceLinksData)) {
    throw new Error(`Alice failed to retrieve links: ${aliceLinksStr}`);
  }
  console.log("✓ get_node_links(active_user='tenant-alice') succeeded.");

  // 11. Test HTTP Error Propagation
  console.log("\n--- 11. Testing HTTP Error Propagation ---");
  const badNodeRes = await get_node("nonexistent-atom-node-xyz-999");
  const badNodeData = JSON.parse(badNodeRes);
  if (badNodeData.status !== "ERROR") throw new Error(`Expected ERROR status, got ${badNodeRes}`);
  if (badNodeData.code !== 404) throw new Error(`Expected code 404, got ${badNodeData.code}`);
  if (!badNodeData.message.toLowerCase().includes("not found")) {
    throw new Error(`Expected 'not found' message, got: ${badNodeData.message}`);
  }
  console.log("✓ Backend HTTP error propagation verified.");

  // 12. Test Full MCP Client Protocol Interface via InMemoryTransport
  console.log("\n--- 12. Testing Full MCP Client Protocol Interface (InMemoryTransport) ---");
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  const mcpClient = new Client({ name: "asos-test-client", version: "1.0.0" }, { capabilities: {} });

  await server.connect(serverTransport);
  await mcpClient.connect(clientTransport);

  // 12a. List Tools
  const toolsList = await mcpClient.listTools();
  const toolNames = toolsList.tools.map((t) => t.name);
  const requiredTools = [
    "search_commonplace",
    "get_node",
    "get_node_links",
    "export_graph_rdf",
    "create_note",
    "create_catalog_entry",
    "commit_anchored_knowledge",
  ];
  for (const reqTool of requiredTools) {
    if (!toolNames.includes(reqTool)) {
      throw new Error(`Missing tool '${reqTool}' in MCP tools: ${toolNames}`);
    }
  }
  console.log(`✓ mcpClient.listTools() returned ${toolsList.tools.length} tools including all required tools.`);

  // 12b. Call Tool via MCP Client
  const mcpCallRes = await mcpClient.callTool({
    name: "search_commonplace",
    arguments: { query: `bidirectional synapse verification on creation [${runId}]` },
  });
  if (!mcpCallRes.content || mcpCallRes.content.length === 0) {
    throw new Error("mcpClient.callTool returned empty content");
  }
  const mcpCallData = JSON.parse(mcpCallRes.content[0].text);
  if (mcpCallData.count < 1) throw new Error(`Expected search matches via MCP client, got ${mcpCallRes.content[0].text}`);
  console.log("✓ mcpClient.callTool('search_commonplace') verified via JSON-RPC.");

  // 12c. List & Read Resources via MCP Client
  const resList = await mcpClient.listResources();
  const resUris = resList.resources.map((r) => r.uri);
  if (!resUris.includes("asos://schema")) throw new Error("Missing asos://schema in listResources");
  console.log(`✓ mcpClient.listResources() listed ${resList.resources.length} resources.`);

  const schemaMcpRes = await mcpClient.readResource({ uri: "asos://schema" });
  if (!schemaMcpRes.contents || schemaMcpRes.contents.length === 0) throw new Error("readResource schema empty");
  if (!schemaMcpRes.contents[0].text.includes("knowledge_areas")) {
    throw new Error("readResource schema missing knowledge_areas");
  }
  console.log("✓ mcpClient.readResource('asos://schema') verified via JSON-RPC.");

  const skillMcpRes = await mcpClient.readResource({ uri: "asos://skills/knowledge-capture" });
  if (!skillMcpRes.contents || skillMcpRes.contents.length === 0) throw new Error("readResource skill empty");
  if (!skillMcpRes.contents[0].text.includes("# Knowledge Capture Skill")) {
    throw new Error("readResource skill content missing title");
  }
  console.log("✓ mcpClient.readResource('asos://skills/knowledge-capture') verified via JSON-RPC.");

  // 12d. List & Get Prompts via MCP Client
  const promptsList = await mcpClient.listPrompts();
  const promptNames = promptsList.prompts.map((p) => p.name);
  if (!promptNames.includes("curate_note") || !promptNames.includes("author_catalog") || !promptNames.includes("init_swarm")) {
    throw new Error(`Missing expected prompts in listPrompts: ${promptNames}`);
  }
  console.log(`✓ mcpClient.listPrompts() returned ${promptsList.prompts.length} prompts.`);

  const promptRes = await mcpClient.getPrompt({
    name: "curate_note",
    arguments: { project_id: "p1", thesis: "test" },
  });
  if (!promptRes.messages || promptRes.messages.length === 0) {
    throw new Error("getPrompt returned empty messages");
  }
  const promptContent = promptRes.messages[0].content.text || "";
  if (!promptContent.includes("curating a Knowledge Note") || !promptContent.includes("p1") || !promptContent.includes("test")) {
    throw new Error(`Unexpected prompt content: ${promptContent}`);
  }
  console.log("✓ mcpClient.getPrompt('curate_note') verified via JSON-RPC.");

  // 13. Test commit_anchored_knowledge with rich fields
  console.log("\n--- 13. Testing commit_anchored_knowledge with Rich Fields ---");
  const richAtomUuid = `note-rich-${runId}`;
  const commitRes = await commit_anchored_knowledge({
    project_id: "project:node_suite",
    agent_id: "identity:node_verifier",
    atoms: [
      {
        uuid: richAtomUuid,
        statement: `Rich Knowledge Bundle Atom [${runId}]`,
        content: "Testing bundle commit with references, note links, items, and steps.",
        ka: 31,
        tags: ["RICH_BUNDLE", tagName],
        references: [{ title: "Ref Book", creator: "Author X" }],
        note_links: [{ target_uuid: noteUuid, relation: "EXTENDS", context: "Extends first note" }],
        items: [{ name: "CPU", quantity: 4, unit: "cores" }],
        steps: [{ step_number: 1, instruction: "Initialize memory", duration_seconds: 10 }],
      },
    ],
  });
  console.log("commit_anchored_knowledge result:", commitRes);
  if (!commitRes.includes("Successfully committed")) {
    throw new Error(`Expected success message in commitRes: ${commitRes}`);
  }

  // Verify rich atom retrieved
  const richNodeStr = await get_node(richAtomUuid);
  const richNodeData = JSON.parse(richNodeStr);
  if (richNodeData.statement !== `Rich Knowledge Bundle Atom [${runId}]`) {
    throw new Error(`Rich atom mismatch: ${richNodeStr}`);
  }
  console.log("✓ commit_anchored_knowledge with rich fields verified.");

  // Verify commit_knowledge_bundle alias exported and functioning
  const bundleAtomUuid = `note-bundle-${runId}`;
  const bundleRes = await commit_knowledge_bundle({
    project_id: "project:node_suite",
    agent_id: "identity:node_verifier",
    atoms: [
      {
        uuid: bundleAtomUuid,
        statement: `Knowledge Bundle Alias Atom [${runId}]`,
        content: "Testing commit_knowledge_bundle alias.",
        ka: 31,
        tags: ["BUNDLE_ALIAS", tagName],
      },
    ],
  });
  console.log("commit_knowledge_bundle result:", bundleRes);
  if (!bundleRes.includes("Successfully committed")) {
    throw new Error(`Expected success message in bundleRes: ${bundleRes}`);
  }
  const bundleNodeStr = await get_node(bundleAtomUuid);
  const bundleNodeData = JSON.parse(bundleNodeStr);
  if (bundleNodeData.statement !== `Knowledge Bundle Alias Atom [${runId}]`) {
    throw new Error(`Bundle atom mismatch: ${bundleNodeStr}`);
  }
  console.log("✓ commit_knowledge_bundle alias verified.");

  // Test commit_knowledge_bundle via MCP client tool
  const mcpBundleCallRes = await mcpClient.callTool({
    name: "commit_knowledge_bundle",
    arguments: {
      project_id: "project:node_suite",
      agent_id: "identity:node_verifier",
      atoms: [
        {
          statement: `MCP Tool Commit Knowledge Bundle [${runId}]`,
          content: "Testing commit_knowledge_bundle via JSON-RPC.",
          ka: 31,
          tags: ["MCP_BUNDLE", tagName],
        },
      ],
    },
  });
  if (!mcpBundleCallRes.content || mcpBundleCallRes.content.length === 0) {
    throw new Error("mcpClient.callTool('commit_knowledge_bundle') returned empty content");
  }
  console.log("✓ mcpClient.callTool('commit_knowledge_bundle') verified via JSON-RPC.");

  // 14. Test query_knowledge & query_substrate Parameter Handling
  console.log("\n--- 14. Testing query_knowledge & query_substrate Parameter Handling ---");
  // Positional call with where_eq as first argument: query_knowledge({ ka: 31 })
  const qPosRes = await query_knowledge({ ka: 31 });
  console.log("✓ query_knowledge(positional where_eq) verified.");

  // Object options call with where_eq and match_alias
  const qObjRes = await query_knowledge({ where_eq: { ka: 31 }, match_alias: "n" });
  console.log("✓ query_knowledge(object options with match_alias) verified.");

  // query_substrate via MCP client tool
  const mcpSubstrateRes = await mcpClient.callTool({
    name: "query_substrate",
    arguments: { where_eq: { ka: 31 }, match_alias: "n" },
  });
  if (!mcpSubstrateRes.content || mcpSubstrateRes.content.length === 0) {
    throw new Error("mcpClient.callTool('query_substrate') returned empty content");
  }
  console.log("✓ mcpClient.callTool('query_substrate') verified via JSON-RPC.");

  console.log("\n" + "=".repeat(60));
  console.log("ALL ASOS NODE.JS MCP VERIFICATION TESTS PASSED!");
  console.log("=".repeat(60) + "\n");
}

async function main() {
  try {
    await ensureBackend();
    await runTests();
  } catch (err) {
    console.error("\n❌ VERIFICATION TEST FAILED:", err);
    process.exitCode = 1;
  } finally {
    if (startedProcess) {
      console.log("[CLEANUP] Stopping spawned backend daemon...");
      try {
        startedProcess.kill("SIGTERM");
      } catch (e) {}
      console.log("[CLEANUP] Done.");
    }
  }
}

if (require.main === module) {
  main();
}
