import httpx
import uuid
import time

ASOS_API_URL = "http://localhost:8085/api/v1"

def ensure_project(project_id, description):
    payload = {
        "type": "PROJECT",
        "id": project_id,
        "metadata": {
            "description": description,
            "status": "ACTIVE"
        }
    }
    print(f"Ensuring Project: {project_id}")
    res = httpx.post(f"{ASOS_API_URL}/graph/node", json=payload)
    print(res.text)

def commit_bundle(project_id, agent_id, atoms):
    payload = {
        "project_id": project_id,
        "agent_id": agent_id,
        "atoms": atoms
    }
    print(f"Committing Bundle for {project_id}")
    res = httpx.post(f"{ASOS_API_URL}/graph/bundle", json=payload)
    print(res.text)
    # Return UUIDs of created atoms
    import json
    try:
        data = res.json()
        return data.get("committed_uuids", [])
    except:
        return []

def link_nodes(src, dst, label):
    payload = {
        "source": src,
        "target": dst,
        "label": label,
        "weight": 1.0
    }
    print(f"Linking {src} -> {dst} ({label})")
    res = httpx.post(f"{ASOS_API_URL}/link", json=payload)
    print(res.text)

def seed():
    # 1. Project Aether
    ensure_project("PROJECT_AETHER", "Deep Sea Exploration Swarm - Underwater Mesh Network")
    
    aether_atoms = [
        {"statement": "Acoustic modem handshake protocol established.", "content": "Uses 12kHz carrier with phase-shift keying.", "ka": 2}, # Design
        {"statement": "Pressure hull stress test passed at 4000m.", "content": "Titanium-6Al-4V sphere shows 0.02% deformation.", "ka": 4},   # Testing
        {"statement": "Swarm localization requires 3 fixed beacons.", "content": "Static anchors at GPS-verified coordinates.", "ka": 1} # Requirements
    ]
    
    aether_uuids = commit_bundle("PROJECT_AETHER", "SURVEYOR_01", aether_atoms)
    
    if len(aether_uuids) >= 3:
        link_nodes(aether_uuids[0], "PROJECT_AETHER", "BELONGS_TO")
        link_nodes(aether_uuids[1], "PROJECT_AETHER", "BELONGS_TO")
        link_nodes(aether_uuids[2], "PROJECT_AETHER", "BELONGS_TO")
        link_nodes(aether_uuids[0], aether_uuids[2], "IMPLEMENTS")

    # 2. Project Helios
    ensure_project("PROJECT_HELIOS", "Solar Flare Monitoring Swarm - L1 Lagrange Point")
    
    helios_atoms = [
        {"statement": "X-ray sensor saturation threshold set to 10e6 counts.", "content": "Protects CCD from permanent damage.", "ka": 6}, # Config Mgmt
        {"statement": "Communication window: 8 hours per day.", "content": "Synchronized with Deep Space Network (DSN) scheduling.", "ka": 1}, # Requirements
        {"statement": "Thermal shield integrity validated for 2000K peaks.", "content": "Multi-layer insulation (MLI) with carbon-carbon coating.", "ka": 10} # Quality
    ]
    
    helios_uuids = commit_bundle("PROJECT_HELIOS", "OBSERVER_ALPHA", helios_atoms)
    
    if len(helios_uuids) >= 3:
        link_nodes(helios_uuids[0], "PROJECT_HELIOS", "BELONGS_TO")
        link_nodes(helios_uuids[1], "PROJECT_HELIOS", "BELONGS_TO")
        link_nodes(helios_uuids[2], "PROJECT_HELIOS", "BELONGS_TO")
        link_nodes(helios_uuids[0], helios_uuids[1], "DEPENDS_ON")

if __name__ == "__main__":
    seed()
