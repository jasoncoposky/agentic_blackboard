import asyncio
import json
from pathlib import Path
import httpx
from mcp.server.fastmcp import FastMCP

# Initialize FastMCP server
mcp = FastMCP("ASOS Substrate")

ASOS_API_URL = "http://localhost:8085/api/v1"
REPO_ROOT = Path(__file__).resolve().parent

@mcp.tool()
async def ensure_node(
    type: str,
    id: str,
    description: str = "",
    status: str = "ACTIVE",
    content: str = "",
    active_user: str = None
) -> str:
    """
    Idempotently ensure an anchor node (PROJECT or IDENTITY) exists in the ASOS substrate.
    Returns the node's status (CREATED or EXISTS).

    Args:
        type: Node type ('PROJECT' or 'IDENTITY').
        id: Unique identifier for the anchor (e.g. 'project:alpha', 'identity:agent_1').
        description: Human-readable summary of the anchor.
        status: Lifecycle status (e.g. 'ACTIVE', 'ARCHIVED'). Default is 'ACTIVE'.
        content: Detailed documentation or schema body for the anchor.
        active_user: Optional tenant / active user identifier (sets X-Active-User header).
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
    headers = {"X-Active-User": active_user} if active_user else {}
    async with httpx.AsyncClient() as client:
        response = await client.post(f"{ASOS_API_URL}/graph/node", json=payload, headers=headers)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def commit_knowledge_bundle(
    project_id: str,
    agent_id: str,
    atoms: list[dict],
    active_user: str = None
) -> str:
    """
    Commit a bundle of knowledge atoms to the substrate anchored to a project and agent.
    Each atom dict should have 'statement' and optionally 'content', 'ka', 'tags',
    'references', 'note_links', 'items', 'steps', 'metrics', 'attributes'.

    Args:
        project_id: Mandatory project anchor ID (e.g. 'project:research').
        agent_id: Mandatory author/agent identity anchor (e.g. 'identity:analyst').
        atoms: List of knowledge atom dictionaries to ingest.
        active_user: Optional tenant / active user identifier (sets X-Active-User header).
    """
    payload = {
        "project_id": project_id,
        "agent_id": agent_id,
        "atoms": atoms
    }
    headers = {"X-Active-User": active_user} if active_user else {}
    async with httpx.AsyncClient() as client:
        response = await client.post(f"{ASOS_API_URL}/graph/bundle", json=payload, headers=headers)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def search_commonplace(
    query: str,
    ka: int = None,
    tags: list[str] = None,
    limit: int = 10,
    active_user: str = None
) -> str:
    """
    Search the ASOS Commonplace Book for existing notes, concepts, and recipes.
    ALWAYS use this before creating a new note to prevent duplicate nodes.

    Args:
        query: Search string to match against statements, content, and tags.
        ka: Optional Knowledge Area integer ID filter (e.g. 31 for General Commonplace, 27 for Culinary/Recipes).
        tags: Optional list of tags to filter matches (e.g. ['ARCHITECTURE', 'SECURITY']).
        limit: Maximum number of matches to return (default: 10).
        active_user: Optional tenant / active user identifier (sets X-Active-User header).

    Returns:
        JSON string containing matching atoms and count.
    """
    params = {"q": query or "", "limit": limit}
    if ka is not None:
        params["ka"] = ka
    if tags:
        params["tags"] = ",".join(tags)
    headers = {"X-Active-User": active_user} if active_user else {}
    async with httpx.AsyncClient() as client:
        response = await client.get(f"{ASOS_API_URL}/search", params=params, headers=headers)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def get_node(uuid: str, active_user: str = None) -> str:
    """
    Retrieve full hydrated atom or node by UUID from the substrate.

    Args:
        uuid: The unique identifier of the node.
        active_user: Optional tenant / active user identifier (sets X-Active-User header).

    Returns:
        JSON string containing node properties, taxonomy, and payload.
    """
    headers = {"X-Active-User": active_user} if active_user else {}
    async with httpx.AsyncClient() as client:
        response = await client.get(f"{ASOS_API_URL}/node/{uuid}", headers=headers)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def get_node_links(uuid: str, direction: str = "both", active_user: str = None) -> str:
    """
    Query synapses (inbound and/or outbound links) connected to a node.

    Args:
        uuid: The unique identifier of the target node.
        direction: Link direction: 'both' (default), 'inbound' (backlinks), or 'outbound'.
        active_user: Optional tenant / active user identifier (sets X-Active-User header).

    Returns:
        JSON string containing inbound and outbound link lists with hydrated statements.
    """
    params = {"direction": direction}
    headers = {"X-Active-User": active_user} if active_user else {}
    async with httpx.AsyncClient() as client:
        response = await client.get(f"{ASOS_API_URL}/node/{uuid}/links", params=params, headers=headers)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def export_graph_rdf(active_user: str = None) -> str:
    """
    Export the substrate knowledge graph as W3C RDF Turtle text.
    Mapped to standard ontologies (schema:Recipe, schema:HowToStep, dcterms:references, schema:citation).

    Args:
        active_user: Optional tenant / active user identifier (sets X-Active-User header).

    Returns:
        W3C RDF Turtle serialization of the knowledge graph.
    """
    headers = {"X-Active-User": active_user} if active_user else {}
    async with httpx.AsyncClient() as client:
        response = await client.get(f"{ASOS_API_URL}/graph/export", headers=headers)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def create_note(
    project_id: str,
    agent_id: str,
    statement: str,
    content: str = "",
    references: list[dict] = None,
    note_links: list[dict] = None,
    tags: list[str] = None,
    ka: int = 31,
    uuid: str = None,
    check_duplicates: bool = True,
    active_user: str = None
) -> str:
    """
    Create a knowledge note atom in the ASOS substrate.
    Optionally checks for existing duplicate notes before creation.

    Args:
        project_id: Mandatory project anchor ID (e.g. 'project:research').
        agent_id: Mandatory author/agent identity anchor (e.g. 'identity:analyst').
        statement: Core assertion, insight, or thesis (1-2 sentences).
        content: Detailed explanation, markdown notes, or supporting argument.
        references: Citations list of dicts: [{'title': '...', 'creator': '...', 'page_numbers': '...', 'uuid': '...', 'tags': [...], 'excerpt': '...'}].
        note_links: Semantic links to prior notes: [{'target_uuid': '...', 'relation': 'SUPPORTS|REFUTES|EXTENDS|...', 'context': '...'}].
        tags: Taxonomy tags list (e.g. ['ARCHITECTURE', 'NETWORKING']).
        ka: Knowledge Area integer ID (default: 31 for General Commonplace).
        uuid: Optional explicit UUID for the atom.
        check_duplicates: If True (default), checks for duplicate statement before creating.
        active_user: Optional tenant / active user identifier (sets X-Active-User header).

    Returns:
        Success JSON string, or duplicate warning JSON if check_duplicates is True and match exists.
    """
    headers = {"X-Active-User": active_user} if active_user else {}
    async with httpx.AsyncClient() as client:
        if check_duplicates:
            search_res = await client.get(
                f"{ASOS_API_URL}/search",
                params={"q": statement},
                headers=headers
            )
            if search_res.status_code == 200:
                data = search_res.json()
                matches = data.get("matches", [])
                stmt_clean = statement.strip()
                for match in matches:
                    m_stmt = match.get("statement", "").strip()
                    if m_stmt == stmt_clean or m_stmt.lower() == stmt_clean.lower():
                        return json.dumps({
                            "status": "ALREADY_EXISTS",
                            "uuid": match.get("uuid", ""),
                            "message": "Duplicate statement detected. Link to this existing atom or set check_duplicates=False to force creation."
                        })

        atom = {
            "statement": statement,
            "content": content,
            "ka": ka,
            "tags": tags or [],
        }
        if uuid:
            atom["uuid"] = uuid
        if references:
            atom["references"] = references
        if note_links:
            atom["note_links"] = note_links

        payload = {
            "project_id": project_id,
            "agent_id": agent_id,
            "atoms": [atom]
        }
        response = await client.post(
            f"{ASOS_API_URL}/graph/bundle",
            json=payload,
            headers=headers
        )
        response.raise_for_status()
        return response.text

@mcp.tool()
async def create_catalog_entry(
    project_id: str,
    agent_id: str,
    statement: str,
    content: str = "",
    items: list[dict] = None,
    steps: list[dict] = None,
    metrics: list[dict] = None,
    attributes: dict = None,
    tags: list[str] = None,
    ka: int = 27,
    uuid: str = None,
    check_duplicates: bool = True,
    active_user: str = None
) -> str:
    """
    Create a structured catalog entry (recipe, protocol, runbook, or inventory) in the substrate.
    Optionally checks for duplicate catalog entries before creation.

    Args:
        project_id: Mandatory project anchor ID.
        agent_id: Mandatory author/agent identity anchor.
        statement: Title or core objective of the catalog entry.
        content: Detailed description, overview, or yield information.
        items: List of item/ingredient dicts: [{'name': 'Flour', 'quantity': 500, 'unit': 'g', 'role': 'dry_ingredient', 'notes': 'all-purpose'}].
        steps: List of step dicts: [{'step_number': 1, 'instruction': 'Mix ingredients', 'duration_seconds': 300, 'notes': 'whisk thoroughly'}].
        metrics: List of metric dicts: [{'name': 'prep_time', 'value': 15, 'unit': 'minutes'}].
        attributes: Metadata dict: {'servings': '4', 'cuisine': 'Italian', 'difficulty': 'medium'}.
        tags: Taxonomy tags list (e.g. ['RECIPE', 'BAKING']).
        ka: Knowledge Area integer ID (default: 27 for Culinary/Recipes).
        uuid: Optional explicit UUID for the catalog entry.
        check_duplicates: If True (default), checks for duplicate statement before creating.
        active_user: Optional tenant / active user identifier (sets X-Active-User header).

    Returns:
        Success JSON string, or duplicate warning JSON if check_duplicates is True and match exists.
    """
    headers = {"X-Active-User": active_user} if active_user else {}
    async with httpx.AsyncClient() as client:
        if check_duplicates:
            search_res = await client.get(
                f"{ASOS_API_URL}/search",
                params={"q": statement},
                headers=headers
            )
            if search_res.status_code == 200:
                data = search_res.json()
                matches = data.get("matches", [])
                stmt_clean = statement.strip()
                for match in matches:
                    m_stmt = match.get("statement", "").strip()
                    if m_stmt == stmt_clean or m_stmt.lower() == stmt_clean.lower():
                        return json.dumps({
                            "status": "ALREADY_EXISTS",
                            "uuid": match.get("uuid", ""),
                            "message": "Duplicate statement detected. Link to this existing atom or set check_duplicates=False to force creation."
                        })

        atom = {
            "statement": statement,
            "content": content,
            "ka": ka,
            "tags": tags or [],
        }
        if uuid:
            atom["uuid"] = uuid
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
        response = await client.post(
            f"{ASOS_API_URL}/graph/bundle",
            json=payload,
            headers=headers
        )
        response.raise_for_status()
        return response.text

@mcp.tool()
async def link_nodes(
    source: str,
    target: str,
    label: str,
    weight: float = 1.0,
    active_user: str = None
) -> str:
    """
    Create a labeled relationship (synapse) between two nodes in the substrate.
    Consult 'asos://schema' for valid relationship labels (e.g. REFERENCES, SUPPORTS, EXTENDS).

    Args:
        source: Source node UUID.
        target: Target node UUID.
        label: Semantic relationship label.
        weight: Connection weight/confidence (0.0 - 1.0). Default is 1.0.
        active_user: Optional tenant / active user identifier (sets X-Active-User header).
    """
    payload = {
        "source": source,
        "target": target,
        "label": label,
        "weight": weight
    }
    headers = {"X-Active-User": active_user} if active_user else {}
    async with httpx.AsyncClient() as client:
        response = await client.post(f"{ASOS_API_URL}/link", json=payload, headers=headers)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def query_substrate(
    match_alias: str = "n",
    where_eq: dict = None,
    active_user: str = None
) -> str:
    """
    Query the substrate for nodes matching specific criteria.
    """
    payload = {
        "match": match_alias,
        "where_eq": where_eq or {}
    }
    headers = {"X-Active-User": active_user} if active_user else {}
    async with httpx.AsyncClient() as client:
        response = await client.post(f"{ASOS_API_URL}/query", json=payload, headers=headers)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def spawn_widget(atom_id: str, behavior: str, label: str = "", active_user: str = None) -> str:
    """
    Propose a Nucleus spatial widget for an ASOS Knowledge Atom.
    """
    payload = {
        "atom_id": atom_id,
        "behavior": behavior,
        "label": label
    }
    headers = {"X-Active-User": active_user} if active_user else {}
    async with httpx.AsyncClient() as client:
        response = await client.post(f"{ASOS_API_URL}/nucleus/materialize", json=payload, headers=headers)
        response.raise_for_status()
        return response.text

@mcp.tool()
async def bind_anchor_to_atom(marker_id: str, atom_id: str, device_id: str = "default", active_user: str = None) -> str:
    """
    Bind a physical fiducial marker to an ASOS Knowledge Atom.
    """
    payload = {
        "marker_id": marker_id,
        "atom_id": atom_id,
        "device_id": device_id
    }
    headers = {"X-Active-User": active_user} if active_user else {}
    async with httpx.AsyncClient() as client:
        # We'll use a link endpoint to establish the ANCHORED_TO / REPRESENTED_BY chain
        # For MVP, we'll assume a composite operation in the backend
        response = await client.post(f"{ASOS_API_URL}/nucleus/bind", json=payload, headers=headers)
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

@mcp.resource("asos://skills/{name}")
async def get_skill(name: str) -> str:
    """
    Retrieve skill documentation by name from skills/{name}/SKILL.md.
    Path is resolved relative to the repository root.

    Args:
        name: Skill name (e.g. 'knowledge-capture', 'graph-integrity-audit').
    """
    clean_name = name.removesuffix("/SKILL.md").removesuffix(".md").strip("/")
    skill_path = (REPO_ROOT / "skills" / clean_name / "SKILL.md").resolve()
    skills_dir = (REPO_ROOT / "skills").resolve()

    try:
        skill_path.relative_to(skills_dir)
    except ValueError:
        return f"Error: Invalid skill path for '{name}'."

    if not skill_path.is_file():
        return f"Error: Skill '{name}' not found at {skill_path}."

    try:
        return skill_path.read_text(encoding="utf-8")
    except Exception as e:
        return f"Error reading skill '{name}': {e}"

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

@mcp.prompt("curate_note")
def curate_note(project_id: str, thesis: str) -> str:
    """
    Guiding prompt for curating a knowledge note in the ASOS Commonplace Book.
    Enforces pre-creation search, citation addition, and linking to prior notes.
    """
    return f"""
You are curating a Knowledge Note in the ASOS Substrate for project '{project_id}'.
Thesis / Insight: {thesis}

Follow this workflow strictly:
1. Search Commonplace First: Call 'search_commonplace' with terms from the thesis to ensure no duplicate or overlapping note exists.
2. Link or Create:
   - If an existing note covers this thesis, retrieve it with 'get_node' and create a link with 'link_nodes' rather than duplicating.
   - If novel, use 'create_note' with:
     - statement: Concise, atomic summary of '{thesis}'.
     - content: Detailed markdown explanation, analysis, or proof.
     - references: Add bibliographic or source citations (title, author, url/uuid).
     - note_links: Explicit semantic links to related prior notes or concepts.
     - tags and appropriate Knowledge Area (ka, e.g., 31 for Commonplace Note).
3. Connect Synapses: Use 'get_node_links' on related nodes to inspect the knowledge neighborhood and ensure bidirectional coherence.
"""

@mcp.prompt("author_catalog")
def author_catalog(project_id: str, catalog_type: str, title: str) -> str:
    """
    Guiding prompt for authoring a structured catalog entry (recipe, protocol, runbook, or inventory).
    Enforces structuring items, steps, and metrics.
    """
    return f"""
You are authoring a structured catalog entry ({catalog_type}) titled '{title}' for project '{project_id}'.

Follow this workflow strictly:
1. Search Commonplace: Call 'search_commonplace' to verify if a recipe or protocol with this title already exists.
2. Structure the Catalog Entry:
   - statement: Clear title and purpose for '{title}'.
   - content: Overview and high-level description of this {catalog_type}.
   - items: List of required tools, materials, ingredients, or inputs (with name, quantity, unit, role).
   - steps: Sequential instructions (step_number, instruction, duration_seconds/minutes, notes, required_tools, prerequisites).
   - metrics: Quantifiable performance metrics or targets (name/key, value, unit).
   - attributes: Metadata dictionary describing taxonomy or properties.
   - tags and appropriate Knowledge Area (ka, default 27 for Catalog Entry).
3. Commit Catalog: Use 'create_catalog_entry' to commit the atom bundle to the substrate.
"""

if __name__ == "__main__":
    mcp.run()
