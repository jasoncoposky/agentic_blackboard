import requests
import json
import uuid
import time
import random

BASE_URL = "http://localhost:8085/api/v1"

def create_atom(atom_id, ka, statement, project="asos-v0.5", agent="librarian-alpha"):
    return {
        "header": {
            "uuid": atom_id,
            "origin": {
                "project_id": project,
                "agent_id": agent,
                "timestamp": int(time.time())
            }
        },
        "taxonomy": {
            "knowledge_area": ka,
            "confidence_score": 0.95
        },
        "payload": {
            "statement": statement,
            "encoding": "UTF8"
        },
        "anchors": []
    }

atoms = [
    # DESIGN Area
    create_atom("atom-raft-01", 1, "Raft consensus protocol for distributed WAL synchronization."),
    create_atom("atom-sharding-01", 1, "Consistent hashing for linear horizontal scaling across shards."),
    create_atom("atom-p2p-01", 1, "Gossip-based membership protocol for swarm discovery."),
    
    # RELIABILITY Area
    create_atom("atom-latency-01", 2, "Sync latency monitoring via atomic hop-latency probes."),
    create_atom("atom-fault-01", 2, "Automatic shard promotion during network partitions."),
    create_atom("atom-audit-01", 2, "Real-time orphan detection in the semantic substrate."),
    
    # SEMANTICS Area
    create_atom("atom-nlp-01", 4, "Zero-shot analogy detection using tiered similarity scores."),
    create_atom("atom-graph-01", 4, "Force-directed topology visualization for knowledge exploration."),
    create_atom("atom-synapse-01", 4, "Dynamic edge weight adjustment based on swarm traversal hits."),
    
    # PROTOCOL Area
    create_atom("atom-bson-01", 5, "Zero-copy BSON serialization for high-throughput RPC."),
    create_atom("atom-zmq-01", 5, "Asynchronous ZeroMQ pipeline for non-blocking substrate synchronization."),
    create_atom("atom-http-01", 5, "REST/BSON gateway for external dashboard connectivity."),
]

# Links (Edges)
# (src, label, weight, dst)
links = [
    ("atom-raft-01", "CPB_SIMILARITY", 1.0, "atom-zmq-01"),
    ("atom-sharding-01", "RELATED_TO", 0.7, "atom-raft-01"),
    ("atom-latency-01", "RELATED_TO", 0.8, "atom-audit-01"),
    ("atom-nlp-01", "CPB_SIMILARITY", 0.9, "atom-graph-01"),
    ("atom-nlp-01", "RELATED_TO", 0.5, "atom-synapse-01"),
    ("atom-bson-01", "CPB_SIMILARITY", 1.0, "atom-http-01"),
    ("atom-p2p-01", "RELATED_TO", 0.6, "atom-raft-01"),
    ("atom-audit-01", "RELATED_TO", 0.9, "atom-fault-01"),
    ("atom-graph-01", "RELATED_TO", 0.8, "atom-synapse-01"),
]

def main():
    print(f"[*] Seeding Master Swarm to {BASE_URL}...")
    
    # 1. Commit Atoms as a Bundle (Substrate requires anchors)
    bundle = {
        "project_id": "asos-v0.5",
        "agent_id": "librarian-alpha",
        "atoms": atoms
    }
    
    res = requests.post(f"{BASE_URL}/graph/bundle", json=bundle)
    if res.status_code == 200:
        print(f" [+] Committed Bundle of {len(atoms)} atoms")
    else:
        print(f" [-] Failed Bundle: {res.text}")

    # 2. Add Links

    for src, label, weight, dst in links:
        link_data = {
            "source": src,
            "target": dst,
            "label": label,
            "weight": weight
        }
        res = requests.post(f"{BASE_URL}/link", json=link_data)
        if res.status_code == 200:
            print(f" [+] Linked {src} --[{label}]--> {dst}")
        else:
            print(f" [-] Failed link {src}->{dst}: {res.text}")

if __name__ == "__main__":
    main()

