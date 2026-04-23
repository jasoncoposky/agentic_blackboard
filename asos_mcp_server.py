import asyncio
import httpx
from mcp.server.fastmcp import FastMCP

# Initialize FastMCP server
mcp = FastMCP("ASOS Substrate")

ASOS_API_URL = "http://localhost:8085/api/v1"

@mcp.tool()
async def ensure_node(type: str, id: str, description: str = "", status: str = "ACTIVE", content: str = "") -> str:
    """
    Idempotently ensure a node (PROJECT or IDENTITY) exists in the ASOS substrate.
    Returns the node's status (CREATED or EXISTS).
    """
    payload = {
        "type": type,
        "id": id,
        "metadata": {
            "description": description,
            "status": status,
            "content": content
        }
    }
    async with httpx.AsyncClient() as client:
        response = await client.post(f"{ASOS_API_URL}/graph/node", json=payload)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def commit_knowledge_bundle(project_id: str, agent_id: str, atoms: list[dict]) -> str:
    """
    Commit a bundle of knowledge atoms to the substrate.
    Each atom dict should have 'statement' and optionally 'content'.
    """
    payload = {
        "project_id": project_id,
        "agent_id": agent_id,
        "atoms": atoms
    }
    async with httpx.AsyncClient() as client:
        response = await client.post(f"{ASOS_API_URL}/graph/bundle", json=payload)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def link_nodes(source: str, target: str, label: str, weight: float = 1.0) -> str:
    """
    Create a labeled relationship (synapse) between two nodes.
    """
    payload = {
        "source": source,
        "target": target,
        "label": label,
        "weight": weight
    }
    async with httpx.AsyncClient() as client:
        response = await client.post(f"{ASOS_API_URL}/link", json=payload)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def query_substrate(match_alias: str = "n", where_eq: dict = None) -> str:
    """
    Query the substrate for nodes matching specific criteria.
    """
    payload = {
        "match": match_alias,
        "where_eq": where_eq or {}
    }
    async with httpx.AsyncClient() as client:
        response = await client.post(f"{ASOS_API_URL}/query", json=payload)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def spawn_widget(atom_id: str, behavior: str, label: str = "") -> str:
    """
    Propose a Nucleus spatial widget for an ASOS Knowledge Atom.
    """
    payload = {
        "atom_id": atom_id,
        "behavior": behavior,
        "label": label
    }
    async with httpx.AsyncClient() as client:
        response = await client.post(f"{ASOS_API_URL}/nucleus/materialize", json=payload)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def bind_anchor_to_atom(marker_id: str, atom_id: str, device_id: str = "default") -> str:
    """
    Bind a physical fiducial marker to an ASOS Knowledge Atom.
    """
    payload = {
        "marker_id": marker_id,
        "atom_id": atom_id,
        "device_id": device_id
    }
    async with httpx.AsyncClient() as client:
        # We'll use a link endpoint to establish the ANCHORED_TO / REPRESENTED_BY chain
        # For MVP, we'll assume a composite operation in the backend
        response = await client.post(f"{ASOS_API_URL}/nucleus/bind", json=payload)
        response.raise_for_status()
        return response.text

@mcp.resource("asos://schema")
async def get_schema() -> str:
    """
    Returns the ASOS Knowledge Schema, including valid node types, 
    Knowledge Areas (KA), and relationship labels.
    """
    async with httpx.AsyncClient() as client:
        response = await client.get(f"{ASOS_API_URL}/schema")
        response.raise_for_status()
        return response.text

@mcp.prompt("init_swarm")
def init_swarm(project_id: str, objective: str):
    """
    A template for initializing a new swarm project.
    """
    return f"""
You are an ASOS Agent tasked with initializing a new swarm project: '{project_id}'.
Objective: {objective}

Please follow these steps using the available tools:
1. Use 'ensure_node' to create a PROJECT anchor for '{project_id}'.
2. Decompose the objective into a detailed Work Breakdown Structure (WBS).
3. For each WBS element or major requirement, create a Knowledge Atom with a concise 'statement' and a detailed 'content' field (e.g., full markdown documentation).
4. Use 'commit_knowledge_bundle' to commit these atoms to the substrate, anchored to '{project_id}'.
5. Use 'link_nodes' to establish hierarchical or sequential relationships between the atoms.

Consult 'asos://schema' for valid Knowledge Area (KA) IDs and relationship labels.
"""

if __name__ == "__main__":
    mcp.run()
