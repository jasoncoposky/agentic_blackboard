const { Server } = require("@modelcontextprotocol/sdk/server/index.js");
const { StdioServerTransport } = require("@modelcontextprotocol/sdk/server/stdio.js");
const {
  CallToolRequestSchema,
  ListToolsRequestSchema,
  ListResourcesRequestSchema,
  ReadResourceRequestSchema,
  ListResourceTemplatesRequestSchema,
  ListPromptsRequestSchema,
  GetPromptRequestSchema,
} = require("@modelcontextprotocol/sdk/types.js");
const axios = require("axios");
const crypto = require("crypto");
const path = require("path");
const fs = require("fs");

const API_BASE = process.env.AB_API_URL || "http://localhost:8085/api/v1";
const REPO_ROOT = path.resolve(__dirname, "..");
const SKILLS_DIR = path.resolve(REPO_ROOT, "skills");

const DEFAULT_TIMEOUT = 10000; // 10s
const RDF_TIMEOUT = 30000;     // 30s

/**
 * Format error into structured JSON string matching FastMCP server convention.
 */
function formatError(error) {
  const status = error.response ? error.response.status : 500;
  let message = "";
  if (error.response && error.response.data !== undefined) {
    if (typeof error.response.data === "string") {
      message = error.response.data;
    } else {
      message = JSON.stringify(error.response.data);
    }
  } else {
    message = error.message || "Unknown error";
  }
  return JSON.stringify({
    status: "ERROR",
    code: status,
    message: message,
  });
}

/**
 * Retrieve skill documentation with strict path traversal protection.
 */
async function get_skill(name) {
  try {
    if (!name || typeof name !== "string") {
      return `Error: Skill '${name}' not found.`;
    }
    const cleanName = name
      .replace(/\/(SKILL\.md|\.md)$/, "")
      .replace(/\.md$/, "")
      .replace(/^\/+|\/+$/g, "");
    const skillPath = path.resolve(SKILLS_DIR, cleanName, "SKILL.md");

    // Path traversal check: must resolve within SKILLS_DIR
    const rel = path.relative(SKILLS_DIR, skillPath);
    if (rel.startsWith("..") || path.isAbsolute(rel) || !skillPath.startsWith(SKILLS_DIR + path.sep)) {
      return `Error: Skill '${name}' not found.`;
    }

    return await fs.promises.readFile(skillPath, "utf-8");
  } catch (error) {
    return `Error: Skill '${name}' not found.`;
  }
}

/**
 * Retrieve Agentic Blackboard schema.
 */
async function get_schema() {
  try {
    const response = await axios.get(`${API_BASE}/schema`, {
      timeout: DEFAULT_TIMEOUT,
    });
    return typeof response.data === "string" ? response.data : JSON.stringify(response.data, null, 2);
  } catch (error) {
    return formatError(error);
  }
}

/**
 * Search the Agentic Blackboard Commonplace Book.
 */
async function search_commonplace(queryOrArgs, kaParam, tagsParam, limitParam, activeUserParam) {
  let query, ka, tags, limit, active_user;
  if (typeof queryOrArgs === "object" && queryOrArgs !== null && !Array.isArray(queryOrArgs)) {
    ({ query, ka, tags, limit, active_user } = queryOrArgs);
  } else {
    query = queryOrArgs;
    ka = kaParam;
    tags = tagsParam;
    limit = limitParam;
    active_user = activeUserParam;
  }

  const params = {
    q: query || "",
    limit: limit !== undefined ? limit : 10,
  };
  if (ka !== undefined && ka !== null) {
    params.ka = ka;
  }
  if (tags && (Array.isArray(tags) ? tags.length > 0 : Boolean(tags))) {
    params.tags = Array.isArray(tags) ? tags.join(",") : tags;
  }
  const headers = active_user ? { "X-Active-User": active_user } : {};

  try {
    const response = await axios.get(`${API_BASE}/search`, {
      params,
      headers,
      timeout: DEFAULT_TIMEOUT,
    });
    return typeof response.data === "string" ? response.data : JSON.stringify(response.data);
  } catch (error) {
    return formatError(error);
  }
}

/**
 * Retrieve full hydrated atom or node by UUID.
 */
async function get_node(uuidOrArgs, activeUserParam) {
  let uuid, active_user;
  if (typeof uuidOrArgs === "object" && uuidOrArgs !== null) {
    ({ uuid, active_user } = uuidOrArgs);
  } else {
    uuid = uuidOrArgs;
    active_user = activeUserParam;
  }

  const headers = active_user ? { "X-Active-User": active_user } : {};
  try {
    const response = await axios.get(`${API_BASE}/node/${encodeURIComponent(uuid)}`, {
      headers,
      timeout: DEFAULT_TIMEOUT,
    });
    return typeof response.data === "string" ? response.data : JSON.stringify(response.data);
  } catch (error) {
    return formatError(error);
  }
}

/**
 * Query synapses connected to a node.
 */
async function get_node_links(uuidOrArgs, directionParam, activeUserParam) {
  let uuid, direction, active_user;
  if (typeof uuidOrArgs === "object" && uuidOrArgs !== null) {
    ({ uuid, direction, active_user } = uuidOrArgs);
  } else {
    uuid = uuidOrArgs;
    direction = directionParam;
    active_user = activeUserParam;
  }

  const params = { direction: direction || "both" };
  const headers = active_user ? { "X-Active-User": active_user } : {};
  try {
    const response = await axios.get(`${API_BASE}/node/${encodeURIComponent(uuid)}/links`, {
      params,
      headers,
      timeout: DEFAULT_TIMEOUT,
    });
    return typeof response.data === "string" ? response.data : JSON.stringify(response.data);
  } catch (error) {
    return formatError(error);
  }
}

/**
 * Export the substrate knowledge graph as W3C RDF Turtle text.
 */
async function export_graph_rdf(activeUserOrArgs) {
  let active_user;
  if (typeof activeUserOrArgs === "object" && activeUserOrArgs !== null) {
    active_user = activeUserOrArgs.active_user;
  } else {
    active_user = activeUserOrArgs;
  }

  const headers = active_user ? { "X-Active-User": active_user } : {};
  try {
    const response = await axios.get(`${API_BASE}/graph/export`, {
      headers,
      timeout: RDF_TIMEOUT,
    });
    return typeof response.data === "string" ? response.data : JSON.stringify(response.data);
  } catch (error) {
    return formatError(error);
  }
}

/**
 * Create a knowledge note atom in the Agentic Blackboard substrate with optional duplicate prevention.
 */
async function create_note(
  projectIdOrArgs,
  agentIdParam,
  statementParam,
  contentParam,
  referencesParam,
  noteLinksParam,
  tagsParam,
  kaParam,
  uuidParam,
  checkDuplicatesParam,
  activeUserParam
) {
  let project_id, agent_id, statement, content, references, note_links, tags, ka, uuid, check_duplicates, active_user;
  if (typeof projectIdOrArgs === "object" && projectIdOrArgs !== null) {
    ({
      project_id,
      agent_id,
      statement,
      content = "",
      references,
      note_links,
      tags,
      ka = 31,
      uuid,
      check_duplicates = true,
      active_user,
    } = projectIdOrArgs);
  } else {
    project_id = projectIdOrArgs;
    agent_id = agentIdParam;
    statement = statementParam;
    content = contentParam || "";
    references = referencesParam;
    note_links = noteLinksParam;
    tags = tagsParam;
    ka = kaParam !== undefined ? kaParam : 31;
    uuid = uuidParam;
    check_duplicates = checkDuplicatesParam !== undefined ? checkDuplicatesParam : true;
    active_user = activeUserParam;
  }

  const headers = active_user ? { "X-Active-User": active_user } : {};

  // Pre-flight duplicate check
  if (check_duplicates !== false) {
    try {
      const searchRes = await axios.get(`${API_BASE}/search`, {
        params: { q: statement, limit: 50 },
        headers,
        timeout: DEFAULT_TIMEOUT,
        validateStatus: () => true,
      });
      if (searchRes.status === 200 && searchRes.data && Array.isArray(searchRes.data.matches)) {
        const stmtClean = (statement || "").trim().toLowerCase();
        for (const match of searchRes.data.matches) {
          const mStmt = (match.statement || "").trim().toLowerCase();
          if (mStmt === stmtClean) {
            return JSON.stringify({
              status: "ALREADY_EXISTS",
              uuid: match.uuid || "",
              message: "Duplicate statement detected. Link to this existing atom or set check_duplicates=False to force creation.",
            });
          }
        }
      }
    } catch (err) {
      // Ignore preflight search failure
    }
  }

  const node_uuid = uuid || `note-${crypto.randomBytes(4).toString("hex")}`;
  const atom = {
    statement,
    content: content || "",
    ka: ka !== undefined && ka !== null ? ka : 31,
    tags: tags || [],
    uuid: node_uuid,
  };
  if (references) atom.references = references;
  if (note_links) atom.note_links = note_links;

  const payload = {
    project_id,
    agent_id,
    atoms: [atom],
  };

  try {
    const response = await axios.post(`${API_BASE}/graph/bundle`, payload, {
      headers,
      timeout: DEFAULT_TIMEOUT,
    });
    return JSON.stringify({
      status: "COMMITTED",
      uuid: node_uuid,
      message: typeof response.data === "string" ? response.data : JSON.stringify(response.data),
    });
  } catch (error) {
    return formatError(error);
  }
}

/**
 * Create a structured catalog entry (recipe, protocol, runbook, inventory) with optional duplicate prevention.
 */
async function create_catalog_entry(
  projectIdOrArgs,
  agentIdParam,
  statementParam,
  contentParam,
  itemsParam,
  stepsParam,
  metricsParam,
  attributesParam,
  tagsParam,
  kaParam,
  uuidParam,
  checkDuplicatesParam,
  activeUserParam
) {
  let project_id, agent_id, statement, content, items, steps, metrics, attributes, tags, ka, uuid, check_duplicates, active_user;
  if (typeof projectIdOrArgs === "object" && projectIdOrArgs !== null) {
    ({
      project_id,
      agent_id,
      statement,
      content = "",
      items,
      steps,
      metrics,
      attributes,
      tags,
      ka = 27,
      uuid,
      check_duplicates = true,
      active_user,
    } = projectIdOrArgs);
  } else {
    project_id = projectIdOrArgs;
    agent_id = agentIdParam;
    statement = statementParam;
    content = contentParam || "";
    items = itemsParam;
    steps = stepsParam;
    metrics = metricsParam;
    attributes = attributesParam;
    tags = tagsParam;
    ka = kaParam !== undefined ? kaParam : 27;
    uuid = uuidParam;
    check_duplicates = checkDuplicatesParam !== undefined ? checkDuplicatesParam : true;
    active_user = activeUserParam;
  }

  const headers = active_user ? { "X-Active-User": active_user } : {};

  // Pre-flight duplicate check
  if (check_duplicates !== false) {
    try {
      const searchRes = await axios.get(`${API_BASE}/search`, {
        params: { q: statement, limit: 50 },
        headers,
        timeout: DEFAULT_TIMEOUT,
        validateStatus: () => true,
      });
      if (searchRes.status === 200 && searchRes.data && Array.isArray(searchRes.data.matches)) {
        const stmtClean = (statement || "").trim().toLowerCase();
        for (const match of searchRes.data.matches) {
          const mStmt = (match.statement || "").trim().toLowerCase();
          if (mStmt === stmtClean) {
            return JSON.stringify({
              status: "ALREADY_EXISTS",
              uuid: match.uuid || "",
              message: "Duplicate statement detected. Link to this existing atom or set check_duplicates=False to force creation.",
            });
          }
        }
      }
    } catch (err) {
      // Ignore preflight search failure
    }
  }

  const node_uuid = uuid || `catalog-${crypto.randomBytes(4).toString("hex")}`;
  const atom = {
    statement,
    content: content || "",
    ka: ka !== undefined && ka !== null ? ka : 27,
    tags: tags || [],
    uuid: node_uuid,
  };
  if (items) atom.items = items;
  if (steps) atom.steps = steps;
  if (metrics) atom.metrics = metrics;
  if (attributes) atom.attributes = attributes;

  const payload = {
    project_id,
    agent_id,
    atoms: [atom],
  };

  try {
    const response = await axios.post(`${API_BASE}/graph/bundle`, payload, {
      headers,
      timeout: DEFAULT_TIMEOUT,
    });
    return JSON.stringify({
      status: "COMMITTED",
      uuid: node_uuid,
      message: typeof response.data === "string" ? response.data : JSON.stringify(response.data),
    });
  } catch (error) {
    return formatError(error);
  }
}

/**
 * Atomically commit a subgraph of knowledge atoms.
 */
async function commit_anchored_knowledge(projectIdOrArgs, agentIdParam, atomsParam, activeUserParam) {
  let project_id, agent_id, atoms, active_user;
  if (typeof projectIdOrArgs === "object" && projectIdOrArgs !== null) {
    ({ project_id, agent_id, atoms, active_user } = projectIdOrArgs);
  } else {
    project_id = projectIdOrArgs;
    agent_id = agentIdParam;
    atoms = atomsParam;
    active_user = activeUserParam;
  }

  const payload = { project_id, agent_id, atoms };
  const headers = active_user ? { "X-Active-User": active_user } : {};
  try {
    const response = await axios.post(`${API_BASE}/graph/bundle`, payload, {
      headers,
      timeout: DEFAULT_TIMEOUT,
    });
    return typeof response.data === "string" ? response.data : JSON.stringify(response.data);
  } catch (error) {
    return formatError(error);
  }
}

const commit_knowledge_bundle = commit_anchored_knowledge;

/**
 * Idempotently ensure an anchor node (PROJECT or IDENTITY) exists in the Agentic Blackboard substrate.
 */
async function ensure_node(typeOrArgs, idParam, descriptionParam, statusParam, contentParam, activeUserParam) {
  let type, id, description, status, content, active_user;
  if (typeof typeOrArgs === "object" && typeOrArgs !== null) {
    ({ type, id, description = "", status = "ACTIVE", content = "", active_user } = typeOrArgs);
  } else {
    type = typeOrArgs;
    id = idParam;
    description = descriptionParam || "";
    status = statusParam || "ACTIVE";
    content = contentParam || "";
    active_user = activeUserParam;
  }

  const payload = {
    type,
    id,
    metadata: {
      description,
      status,
      content,
    },
  };
  const headers = active_user ? { "X-Active-User": active_user } : {};
  try {
    const response = await axios.post(`${API_BASE}/graph/node`, payload, {
      headers,
      timeout: DEFAULT_TIMEOUT,
    });
    return typeof response.data === "string" ? response.data : JSON.stringify(response.data);
  } catch (error) {
    return formatError(error);
  }
}

/**
 * Create a labeled relationship (synapse) between two nodes.
 */
async function link_nodes(sourceOrArgs, targetParam, labelParam, weightParam, activeUserParam) {
  let source, target, label, weight, active_user;
  if (typeof sourceOrArgs === "object" && sourceOrArgs !== null) {
    ({ source, target, label, weight = 1.0, active_user } = sourceOrArgs);
  } else {
    source = sourceOrArgs;
    target = targetParam;
    label = labelParam;
    weight = weightParam !== undefined ? weightParam : 1.0;
    active_user = activeUserParam;
  }

  const payload = {
    source,
    target,
    label,
    weight: weight !== undefined ? weight : 1.0,
  };
  const headers = active_user ? { "X-Active-User": active_user } : {};
  try {
    const response = await axios.post(`${API_BASE}/link`, payload, {
      headers,
      timeout: DEFAULT_TIMEOUT,
    });
    return typeof response.data === "string" ? response.data : JSON.stringify(response.data);
  } catch (error) {
    return formatError(error);
  }
}

/**
 * Execute a graph query against the Agentic Blackboard knowledge substrate.
 */
async function query_knowledge(whereEqOrArgs, matchParam, activeUserParam) {
  let where_eq = {};
  let match = "n";
  let active_user;

  if (typeof whereEqOrArgs === "object" && whereEqOrArgs !== null) {
    if ("where_eq" in whereEqOrArgs || "match" in whereEqOrArgs || "match_alias" in whereEqOrArgs || "active_user" in whereEqOrArgs) {
      where_eq = whereEqOrArgs.where_eq || {};
      match = whereEqOrArgs.match || whereEqOrArgs.match_alias || matchParam || "n";
      active_user = whereEqOrArgs.active_user || activeUserParam;
    } else {
      where_eq = whereEqOrArgs;
      match = matchParam || "n";
      active_user = activeUserParam;
    }
  } else {
    match = matchParam || "n";
    active_user = activeUserParam;
  }

  const payload = { match, where_eq };
  const headers = active_user ? { "X-Active-User": active_user } : {};
  try {
    const response = await axios.post(`${API_BASE}/query`, payload, {
      headers,
      timeout: DEFAULT_TIMEOUT,
    });
    return typeof response.data === "string" ? response.data : JSON.stringify(response.data, null, 2);
  } catch (error) {
    return formatError(error);
  }
}

const query_substrate = query_knowledge;

// Prompt templates matching FastMCP server
function init_swarm(project_id, objective) {
  return `You are an Agentic Blackboard Agent tasked with initializing a new swarm project: '${project_id}'.
Objective: ${objective}

Please follow these steps using the available tools:
1. Use 'ensure_node' to create a PROJECT anchor for '${project_id}'.
2. Decompose the objective into a detailed Work Breakdown Structure (WBS).
3. For each WBS element or major requirement, create a Knowledge Atom with a concise 'statement' and a detailed 'content' field (e.g., full markdown documentation).
4. Use 'commit_knowledge_bundle' to commit these atoms to the substrate, anchored to '${project_id}'.
5. Use 'link_nodes' to establish hierarchical or sequential relationships between the atoms.

Consult 'ab://schema' for valid Knowledge Area (KA) IDs and relationship labels.`;
}

function curate_note(project_id, thesis) {
  return `You are curating a Knowledge Note in the Agentic Blackboard Substrate for project '${project_id}'.
Thesis / Insight: ${thesis}

Follow this workflow strictly:
1. Search Commonplace First: Call 'search_commonplace' with terms from the thesis to ensure no duplicate or overlapping note exists.
2. Link or Create:
   - If an existing note covers this thesis, retrieve it with 'get_node' and create a link with 'link_nodes' rather than duplicating.
   - If novel, use 'create_note' with:
     - statement: Concise, atomic summary of '${thesis}'.
     - content: Detailed markdown explanation, analysis, or proof.
     - references: Add bibliographic or source citations (title, author, url/uuid).
     - note_links: Explicit semantic links to related prior notes or concepts.
     - tags and appropriate Knowledge Area (ka, e.g., 31 for Commonplace Note).
3. Connect Synapses: Use 'get_node_links' on related nodes to inspect the knowledge neighborhood and ensure bidirectional coherence.`;
}

function author_catalog(project_id, catalog_type, title) {
  return `You are authoring a structured catalog entry (${catalog_type}) titled '${title}' for project '${project_id}'.

Follow this workflow strictly:
1. Search Commonplace: Call 'search_commonplace' to verify if a recipe or protocol with this title already exists.
2. Structure the Catalog Entry:
   - statement: Clear title and purpose for '${title}'.
   - content: Overview and high-level description of this ${catalog_type}.
   - items: List of required tools, materials, ingredients, or inputs (with name, quantity, unit, role).
   - steps: Sequential instructions (step_number, instruction, duration_seconds/minutes, notes, required_tools, prerequisites).
   - metrics: Quantifiable performance metrics or targets (name/key, value, unit).
   - attributes: Metadata dictionary describing taxonomy or properties.
   - tags and appropriate Knowledge Area (ka, default 27 for Catalog Entry).
3. Commit Catalog: Use 'create_catalog_entry' to commit the atom bundle to the substrate.`;
}

const server = new Server(
  {
    name: "agentic-blackboard",
    version: "0.4.0",
  },
  {
    capabilities: {
      tools: {},
      resources: {},
      prompts: {},
    },
  }
);

const bundleInputProperties = {
  project_id: { type: "string", description: "Mandatory project anchor ID" },
  agent_id: { type: "string", description: "Mandatory author/agent identity anchor" },
  active_user: { type: "string", description: "Optional tenant / active user identifier" },
  atoms: {
    type: "array",
    items: {
      type: "object",
      properties: {
        uuid: { type: "string" },
        statement: { type: "string" },
        content: { type: "string" },
        ka: { type: "number", description: "Knowledge Area integer ID" },
        tags: { type: "array", items: { type: "string" } },
        references: { type: "array", items: { type: "object" } },
        note_links: { type: "array", items: { type: "object" } },
        items: { type: "array", items: { type: "object" } },
        steps: { type: "array", items: { type: "object" } },
        metrics: { type: "array", items: { type: "object" } },
        attributes: { type: "object" },
      },
      required: ["statement"],
    },
  },
};

// 1. List Available Tools
server.setRequestHandler(ListToolsRequestSchema, async () => {
  return {
    tools: [
      {
        name: "discover_schema",
        description: "Fetch the current Agentic Blackboard graph schema, types, and allowed relationships.",
        inputSchema: { type: "object", properties: {} },
      },
      {
        name: "query_knowledge",
        description: "Execute a graph query against the Agentic Blackboard knowledge swarm (e.g., search by KA, tags, or UUID).",
        inputSchema: {
          type: "object",
          properties: {
            where_eq: { type: "object", description: "Filtering criteria (e.g., { 'ka': '2' })" },
            match: { type: "string", description: "Node alias to match" },
            active_user: { type: "string", description: "Optional tenant / active user identifier" },
          },
        },
      },
      {
        name: "query_substrate",
        description: "Query the substrate for nodes matching specific criteria.",
        inputSchema: {
          type: "object",
          properties: {
            where_eq: { type: "object", description: "Filtering criteria" },
            match_alias: { type: "string", description: "Node alias to match (default 'n')" },
            active_user: { type: "string", description: "Optional tenant / active user identifier" },
          },
        },
      },
      {
        name: "ensure_node",
        description: "Idempotently ensure an anchor node (PROJECT or IDENTITY) exists in the Agentic Blackboard substrate.",
        inputSchema: {
          type: "object",
          properties: {
            type: { type: "string", description: "Node type ('PROJECT' or 'IDENTITY')" },
            id: { type: "string", description: "Unique identifier for the anchor (e.g. 'project:alpha')" },
            description: { type: "string", description: "Human-readable summary of the anchor" },
            status: { type: "string", description: "Lifecycle status (default 'ACTIVE')" },
            content: { type: "string", description: "Detailed documentation or schema body" },
            active_user: { type: "string", description: "Optional tenant / active user identifier" },
          },
          required: ["type", "id"],
        },
      },
      {
        name: "link_nodes",
        description: "Create a labeled relationship (synapse) between two nodes in the substrate.",
        inputSchema: {
          type: "object",
          properties: {
            source: { type: "string", description: "Source node UUID" },
            target: { type: "string", description: "Target node UUID" },
            label: { type: "string", description: "Semantic relationship label" },
            weight: { type: "number", description: "Connection weight/confidence (0.0 - 1.0, default 1.0)" },
            active_user: { type: "string", description: "Optional tenant / active user identifier" },
          },
          required: ["source", "target", "label"],
        },
      },
      {
        name: "commit_anchored_knowledge",
        description: "Atomically commit a subgraph of knowledge atoms, ensuring they are anchored to a Project and Identity.",
        inputSchema: {
          type: "object",
          properties: bundleInputProperties,
          required: ["project_id", "agent_id", "atoms"],
        },
      },
      {
        name: "commit_knowledge_bundle",
        description: "Commit a bundle of knowledge atoms to the substrate anchored to a project and agent.",
        inputSchema: {
          type: "object",
          properties: bundleInputProperties,
          required: ["project_id", "agent_id", "atoms"],
        },
      },
      {
        name: "search_commonplace",
        description: "Search the Agentic Blackboard Commonplace Book for existing notes, concepts, and recipes. ALWAYS use this before creating a new note to prevent duplicate nodes.",
        inputSchema: {
          type: "object",
          properties: {
            query: { type: "string", description: "Search string to match against statements, content, and tags." },
            ka: { type: "number", description: "Optional Knowledge Area integer ID filter (e.g. 31 for General Commonplace, 27 for Culinary/Recipes)." },
            tags: { type: "array", items: { type: "string" }, description: "Optional list of tags to filter matches." },
            limit: { type: "number", description: "Maximum number of matches to return (default: 10)." },
            active_user: { type: "string", description: "Optional tenant / active user identifier (sets X-Active-User header)." },
          },
        },
      },
      {
        name: "get_node",
        description: "Retrieve full hydrated atom or node by UUID from the substrate.",
        inputSchema: {
          type: "object",
          properties: {
            uuid: { type: "string", description: "The unique identifier of the node." },
            active_user: { type: "string", description: "Optional tenant / active user identifier (sets X-Active-User header)." },
          },
          required: ["uuid"],
        },
      },
      {
        name: "get_node_links",
        description: "Query synapses (inbound and/or outbound links) connected to a node.",
        inputSchema: {
          type: "object",
          properties: {
            uuid: { type: "string", description: "The unique identifier of the target node." },
            direction: { type: "string", description: "Link direction: 'both' (default), 'inbound' (backlinks), or 'outbound'.", enum: ["both", "inbound", "outbound"] },
            active_user: { type: "string", description: "Optional tenant / active user identifier (sets X-Active-User header)." },
          },
          required: ["uuid"],
        },
      },
      {
        name: "export_graph_rdf",
        description: "Export the substrate knowledge graph as W3C RDF Turtle text. Mapped to standard ontologies (schema:Recipe, schema:HowToStep, dcterms:references, schema:citation).",
        inputSchema: {
          type: "object",
          properties: {
            active_user: { type: "string", description: "Optional tenant / active user identifier (sets X-Active-User header)." },
          },
        },
      },
      {
        name: "create_note",
        description: "Create a knowledge note atom in the Agentic Blackboard substrate. Optionally checks for existing duplicate notes before creation.",
        inputSchema: {
          type: "object",
          properties: {
            project_id: { type: "string", description: "Mandatory project anchor ID (e.g. 'project:research')." },
            agent_id: { type: "string", description: "Mandatory author/agent identity anchor (e.g. 'identity:analyst')." },
            statement: { type: "string", description: "Core assertion, insight, or thesis (1-2 sentences)." },
            content: { type: "string", description: "Detailed explanation, markdown notes, or supporting argument." },
            references: {
              type: "array",
              items: { type: "object" },
              description: "Citations list: [{'title': '...', 'creator': '...', 'page_numbers': '...', 'excerpt': '...'}].",
            },
            note_links: {
              type: "array",
              items: { type: "object" },
              description: "Semantic links to prior notes: [{'target_uuid': '...', 'relation': 'SUPPORTS|REFUTES|EXTENDS', 'context': '...'}].",
            },
            tags: { type: "array", items: { type: "string" }, description: "Taxonomy tags list (e.g. ['ARCHITECTURE', 'NETWORKING'])." },
            ka: { type: "number", description: "Knowledge Area integer ID (default: 31 for General Commonplace)." },
            uuid: { type: "string", description: "Optional explicit UUID for the atom." },
            check_duplicates: { type: "boolean", description: "If true (default), checks for duplicate statement before creating." },
            active_user: { type: "string", description: "Optional tenant / active user identifier (sets X-Active-User header)." },
          },
          required: ["project_id", "agent_id", "statement"],
        },
      },
      {
        name: "create_catalog_entry",
        description: "Create a structured catalog entry (recipe, protocol, runbook, or inventory) in the substrate. Optionally checks for duplicate catalog entries before creation.",
        inputSchema: {
          type: "object",
          properties: {
            project_id: { type: "string", description: "Mandatory project anchor ID." },
            agent_id: { type: "string", description: "Mandatory author/agent identity anchor." },
            statement: { type: "string", description: "Title or core objective of the catalog entry." },
            content: { type: "string", description: "Detailed description, overview, or yield information." },
            items: {
              type: "array",
              items: { type: "object" },
              description: "List of item/ingredient dicts: [{'name': '...', 'quantity': 1, 'unit': 'g', 'role': '...', 'notes': '...'}].",
            },
            steps: {
              type: "array",
              items: { type: "object" },
              description: "List of step dicts: [{'step_number': 1, 'instruction': '...', 'duration_seconds': 300, 'notes': '...'}].",
            },
            metrics: {
              type: "array",
              items: { type: "object" },
              description: "List of metric dicts: [{'name': 'prep_time', 'value': 15, 'unit': 'minutes'}].",
            },
            attributes: { type: "object", description: "Metadata dict: {'servings': '4', 'difficulty': 'medium'}." },
            tags: { type: "array", items: { type: "string" }, description: "Taxonomy tags list (e.g. ['RECIPE', 'BAKING'])." },
            ka: { type: "number", description: "Knowledge Area integer ID (default: 27 for Culinary/Recipes)." },
            uuid: { type: "string", description: "Optional explicit UUID for the catalog entry." },
            check_duplicates: { type: "boolean", description: "If true (default), checks for duplicate statement before creating." },
            active_user: { type: "string", description: "Optional tenant / active user identifier (sets X-Active-User header)." },
          },
          required: ["project_id", "agent_id", "statement"],
        },
      },
    ],
  };
});

// 2. Handle Tool Calls
server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args = {} } = request.params;

  try {
    let result;
    switch (name) {
      case "discover_schema":
        result = await get_schema();
        break;
      case "query_knowledge":
      case "query_substrate":
        result = await query_knowledge(args);
        break;
      case "ensure_node":
        result = await ensure_node(args);
        break;
      case "link_nodes":
        result = await link_nodes(args);
        break;
      case "commit_anchored_knowledge":
      case "commit_knowledge_bundle":
        result = await commit_anchored_knowledge(args);
        break;
      case "search_commonplace":
        result = await search_commonplace(args);
        break;
      case "get_node":
        result = await get_node(args);
        break;
      case "get_node_links":
        result = await get_node_links(args);
        break;
      case "export_graph_rdf":
        result = await export_graph_rdf(args);
        break;
      case "create_note":
        result = await create_note(args);
        break;
      case "create_catalog_entry":
        result = await create_catalog_entry(args);
        break;
      default:
        return {
          content: [
            {
              type: "text",
              text: JSON.stringify({ status: "ERROR", code: 400, message: `Unknown tool: ${name}` }),
            },
          ],
          isError: true,
        };
    }

    return {
      content: [{ type: "text", text: typeof result === "string" ? result : JSON.stringify(result) }],
    };
  } catch (error) {
    return {
      content: [{ type: "text", text: formatError(error) }],
      isError: true,
    };
  }
});

// 3. Resources
server.setRequestHandler(ListResourcesRequestSchema, async () => {
  const resources = [
    {
      uri: "ab://schema",
      name: "Agentic Blackboard Knowledge Schema",
      description: "Agentic Blackboard graph schema, node types, knowledge areas, and relationship predicates.",
      mimeType: "application/json",
    },
    {
      uri: "ab://swarm/health",
      name: "Swarm SRE Health Metrics",
      description: "Real-time telemetry for Knowledge Velocity, Toil, and Latency.",
      mimeType: "application/json",
    },
  ];

  try {
    const entries = await fs.promises.readdir(SKILLS_DIR, { withFileTypes: true });
    for (const entry of entries) {
      if (entry.isDirectory()) {
        resources.push({
          uri: `ab://skills/${entry.name}`,
          name: `Skill: ${entry.name}`,
          description: `Skill documentation for ${entry.name}`,
          mimeType: "text/markdown",
        });
      }
    }
  } catch (err) {
    // Skills directory may not be present in some environments
  }

  return { resources };
});

server.setRequestHandler(ListResourceTemplatesRequestSchema, async () => {
  return {
    resourceTemplates: [
      {
        uriTemplate: "ab://skills/{name}",
        name: "Agentic Blackboard Skill Documentation",
        description: "Retrieve on-demand skill documentation markdown files.",
        mimeType: "text/markdown",
      },
    ],
  };
});

server.setRequestHandler(ReadResourceRequestSchema, async (request) => {
  const uri = request.params.uri;

  if (uri === "ab://schema") {
    const text = await get_schema();
    return {
      contents: [
        {
          uri: "ab://schema",
          mimeType: "application/json",
          text,
        },
      ],
    };
  }

  if (uri.startsWith("ab://skills/")) {
    const skillName = uri.replace(/^ab:\/\/skills\//, "");
    const content = await get_skill(skillName);
    return {
      contents: [
        {
          uri,
          mimeType: "text/markdown",
          text: content,
        },
      ],
    };
  }

  if (uri === "ab://swarm/health") {
    return {
      contents: [
        {
          uri: "ab://swarm/health",
          mimeType: "application/json",
          text: JSON.stringify({ status: "OK", velocity: 1.0, toil: 0.0 }),
        },
      ],
    };
  }

  throw new Error(`Resource not found: ${uri}`);
});

// 4. Prompts
server.setRequestHandler(ListPromptsRequestSchema, async () => {
  return {
    prompts: [
      {
        name: "init_swarm",
        description: "A template for initializing a new swarm project.",
        arguments: [
          { name: "project_id", description: "Project ID", required: true },
          { name: "objective", description: "Project objective", required: true },
        ],
      },
      {
        name: "curate_note",
        description: "Guiding prompt for curating a knowledge note in the Agentic Blackboard Commonplace Book.",
        arguments: [
          { name: "project_id", description: "Project ID", required: true },
          { name: "thesis", description: "Thesis / Insight", required: true },
        ],
      },
      {
        name: "author_catalog",
        description: "Guiding prompt for authoring a structured catalog entry (recipe, protocol, runbook, or inventory).",
        arguments: [
          { name: "project_id", description: "Project ID", required: true },
          { name: "catalog_type", description: "Catalog entry type (e.g. recipe, protocol, runbook)", required: true },
          { name: "title", description: "Catalog entry title", required: true },
        ],
      },
    ],
  };
});

server.setRequestHandler(GetPromptRequestSchema, async (request) => {
  const { name, arguments: args = {} } = request.params;
  let text = "";
  if (name === "init_swarm") {
    text = init_swarm(args.project_id || "", args.objective || "");
  } else if (name === "curate_note") {
    text = curate_note(args.project_id || "", args.thesis || "");
  } else if (name === "author_catalog") {
    text = author_catalog(args.project_id || "", args.catalog_type || "", args.title || "");
  } else {
    throw new Error(`Prompt not found: ${name}`);
  }

  return {
    messages: [
      {
        role: "user",
        content: { type: "text", text },
      },
    ],
  };
});

async function run() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error("Agentic Blackboard MCP Server running on stdio");
}

if (require.main === module) {
  run().catch((error) => {
    console.error("Fatal error running server:", error);
    process.exit(1);
  });
}

module.exports = {
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
};
