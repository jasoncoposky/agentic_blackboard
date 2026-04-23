import httpx
import json
import time

ASOS_API_URL = "http://localhost:8085/api/v1"

def shakedown():
    print("[SHAKEDOWN] Starting MARS_ROVER project initialization...")
    
    # 1. Ensure Project Node
    print("[SHAKEDOWN] Ensuring PROJECT: MARS_ROVER")
    p_payload = {
        "type": "PROJECT",
        "id": "MARS_ROVER",
        "metadata": {"description": "Autonomous Mars Exploration Swarm"}
    }
    httpx.post(f"{ASOS_API_URL}/graph/node", json=p_payload).raise_for_status()

    # 2. Commit WBS Atoms
    print("[SHAKEDOWN] Committing Knowledge Bundle (WBS)")
    b_payload = {
        "project_id": "MARS_ROVER",
        "agent_id": "Nexus_Agent_7",
        "atoms": [
            {"statement": "WBS 1.1: Thermal Shield Design", "ka": 1},
            {"statement": "WBS 1.2: Radiation Hardened Compute", "ka": 2},
            {"statement": "WBS 1.3: Collaborative Pathfinding", "ka": 0}
        ]
    }
    res = httpx.post(f"{ASOS_API_URL}/graph/bundle", json=b_payload)
    res.raise_for_status()
    print(res.text)

    # 3. Establish Synapses (Relationships)
    # We need the IDs. In our current implementation, we'll just query for them.
    print("[SHAKEDOWN] Linking atoms to form dependency chain...")
    # For now, we'll just link the known project ID to some atoms if we had their UUIDs.
    # Since they are generated, let's just query all atoms for MARS_ROVER.
    
    q_payload = {"match": "n", "where_eq": {"project_id": "MARS_ROVER"}}
    nodes = httpx.post(f"{ASOS_API_URL}/query", json=q_payload).json()
    
    atom_ids = [n["id"] for n in nodes if n.get("type") == "ATOM" or "statement" in n]
    
    if len(atom_ids) >= 2:
        l_payload = {
            "source": atom_ids[0],
            "target": atom_ids[1],
            "label": "RELATED_TO",
            "weight": 0.8
        }
        httpx.post(f"{ASOS_API_URL}/link", json=l_payload).raise_for_status()
        print(f"[SHAKEDOWN] Linked {atom_ids[0]} -> {atom_ids[1]}")

    print("[SHAKEDOWN] Complete. Check dashboard.")

if __name__ == "__main__":
    shakedown()
