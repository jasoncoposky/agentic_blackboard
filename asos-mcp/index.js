const { Server } = require("@modelcontextprotocol/sdk/server/index.js");
const { StdioServerTransport } = require("@modelcontextprotocol/sdk/server/stdio.js");
const {
  CallToolRequestSchema,
  ListToolsRequestSchema,
  ListResourcesRequestSchema,
  ReadResourceRequestSchema,
} = require("@modelcontextprotocol/sdk/types.js");
const axios = require("axios");

const API_BASE = "http://localhost:8085/api/v1";


const server = new Server(
  {
    name: "asos-swarm-mcp",
    version: "0.4.0",
  },
  {
    capabilities: {
      tools: {},
      resources: {},
    },
  }
);

// 1. List Available Tools
server.setRequestHandler(ListToolsRequestSchema, async () => {
  return {
    tools: [
      {
        name: "discover_schema",
        description: "Fetch the current ASOS graph schema, types, and allowed relationships.",
        inputSchema: { type: "object", properties: {} },
      },
      {
        name: "query_knowledge",
        description: "Execute a graph query against the ASOS knowledge swarm (e.g., search by KA, tags, or UUID).",
        inputSchema: {
          type: "object",
          properties: {
            where_eq: { type: "object", description: "Filtering criteria (e.g., { 'ka': '2' })" },
            match: { type: "string", description: "Node alias to match" },
          },
        },
      },
      {
        name: "commit_anchored_knowledge",
        description: "Atomically commit a subgraph of knowledge atoms, ensuring they are anchored to a Project and Identity.",
        inputSchema: {
          type: "object",
          properties: {
            project_id: { type: "string" },
            agent_id: { type: "string" },
            atoms: {
              type: "array",
              items: {
                type: "object",
                properties: {
                  uuid: { type: "string" },
                  statement: { type: "string" },
                  ka: { type: "number", description: "SWEBOK KA index (1-15)" },
                },
                required: ["statement"],
              },
            },
          },
          required: ["project_id", "agent_id", "atoms"],
        },
      },
    ],
  };
});

// 2. Handle Tool Calls
server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;

  try {
    switch (name) {
      case "discover_schema": {
        const response = await axios.get(`${API_BASE}/schema`);
        return { content: [{ type: "text", text: JSON.stringify(response.data, null, 2) }] };
      }
      case "query_knowledge": {
        const response = await axios.post(`${API_BASE}/query`, args);
        return { content: [{ type: "text", text: JSON.stringify(response.data, null, 2) }] };
      }
      case "commit_anchored_knowledge": {
        const response = await axios.post(`${API_BASE}/graph/bundle`, args);
        return { content: [{ type: "text", text: response.data }] };
      }
      default:
        throw new Error(`Unknown tool: ${name}`);
    }
  } catch (error) {
    return {
      content: [{ type: "text", text: `Error: ${error.response?.data || error.message}` }],
      isError: true,
    };
  }
});

// 3. Resources (Optional - Swarm Health)
server.setRequestHandler(ListResourcesRequestSchema, async () => {
  return {
    resources: [
      {
        uri: "asos://swarm/health",
        name: "Swarm SRE Health Metrics",
        description: "Real-time telemetry for Knowledge Velocity, Toil, and Latency.",
        mimeType: "application/json",
      },
    ],
  };
});

async function run() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error("ASOS MCP Server running on stdio");
}

run().catch((error) => {
  console.error("Fatal error running server:", error);
  process.exit(1);
});
