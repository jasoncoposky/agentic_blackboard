import httpx
import json
import uuid
import time

ASOS_API_URL = "http://127.0.0.1:8085"

def verify_skill_knowledge_capture():
    print("[Verification] Starting 'knowledge-capture' skill execution...")
    
    # 1. Synthesize Statement
    statement = "Nucleus Atmosphere uses a ZeroMQ PUB/SUB pattern with CONFLATE=1 for fresh state broadcasting."
    
    # 2. Prepare Bundle
    atom_id = f"nucleus-atmos-{uuid.uuid4().hex[:8]}"
    # The API expects a bundle format
    bundle = {
        "project_id": "PROJECT_NUCLEUS",
        "agent_id": "Antigravity",
        "atoms": [
            {
                "uuid": atom_id,
                "taxonomy": {
                    "ka": 13, # COMPUTING_FOUNDATIONS
                    "tags": ["ATMOSPHERE", "ZEROMQ", "DISCOVERY"],
                    "applicability": 100
                },
                "payload": {
                    "statement": statement,
                    "content": "Protocol uses port 5555 for state broadcast and 5556 for MCP control."
                }
            }
        ]
    }
    
    print(f"[Verification] Committing Atom: {atom_id}")
    
    try:
        response = httpx.post(f"{ASOS_API_URL}/api/v1/graph/bundle", json=bundle)
        response.raise_for_status()
        print(f"[Verification] Success! Server Response: {response.text}")
        
        # 3. Verify Materialization
        print("[Verification] Querying health to confirm ingestion...")
        health = httpx.get(f"{ASOS_API_URL}/api/v1/health").json()
        print(f"[Verification] Swarm Health: {health}")
        
    except Exception as e:
        print(f"[Verification] FAILED: {e}")

if __name__ == "__main__":
    verify_skill_knowledge_capture()
