import httpx
import time
import threading
import random
import hashlib

ASOS_API_URL = "http://localhost:8085/api/v1"

def commit_atom(agent_id, project_id, statement, timestamp=None):
    payload = {
        "project_id": project_id,
        "agent_id": agent_id,
        "atoms": [
            {
                "statement": statement,
                "timestamp": timestamp or int(time.time() * 1000),
                "ka": random.randint(1, 15),
                "applicability": random.randint(0, 100),
                "tags": random.sample(["STRESS", "CONCURRENCY", "L3KVG", "SCALABILITY", "DISTRIBUTED"], random.randint(1, 3))
            }
        ]
    }
    try:
        r = httpx.post(f"{ASOS_API_URL}/graph/bundle", json=payload, timeout=5.0)
        return r.status_code == 200
    except Exception as e:
        print(f"Error: {e}")
        return False

def stress_worker(worker_id, stop_event):
    print(f"Worker {worker_id} started.")
    count = 0
    while not stop_event.is_set():
        # Conflicting updates for the same "Global Knowledge" atom
        statement = f"Global Principle {random.randint(1, 5)}: Concurrency is key."
        agent_id = f"agent_{worker_id}"
        # We use a fixed UUID by hashing the statement (Blackboard does this if missing)
        # But we want to test REPLACING the same UUID with a "better" one.
        
        # Simulating a race for the same UUID
        if commit_atom(agent_id, "project:governance", statement):
            count += 1
        
        time.sleep(0.1) # Rapid but not insane
    print(f"Worker {worker_id} finished. Committed {count} atoms.")

def monitor_health():
    print("\nMonitoring Swarm Health...")
    for _ in range(10):
        try:
            # We fetch via query to see if it shows up in snapshots
            r = httpx.get(f"{ASOS_API_URL}/graph/snapshot")
            data = r.json()
            
            # Find the health node in the snapshot
            health = next((n for n in data["nodes"] if n["id"] == "governance:swarm_health"), None)
            if health:
                # The snapshot doesn't include the metrics directly because they are in the buffer
                # But we can see if it exists.
                print(f"Health Node exists: {health['id']}")
            
            # Count atoms
            atoms = [n for n in data["nodes"] if n["type"] == "ATOM"]
            print(f"Total Atoms in Graph: {len(atoms)}")
            
        except Exception as e:
            print(f"Monitor error: {e}")
        time.sleep(2)

def main():
    print("Starting ASOS Sync Stress Test...")
    stop_event = threading.Event()
    threads = []
    
    # Start 5 workers
    for i in range(5):
        t = threading.Thread(target=stress_worker, args=(i, stop_event))
        t.start()
        threads.append(t)
    
    try:
        monitor_health()
    except KeyboardInterrupt:
        pass
    
    print("\nStopping workers...")
    stop_event.set()
    for t in threads:
        t.join()
    print("Stress test complete.")

if __name__ == "__main__":
    main()
