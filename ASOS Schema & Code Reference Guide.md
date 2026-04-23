# **ASOS Schema Reference & Implementation Guide**

## **1\. Core Graph Schemas**

### **1.1 CPB\_ENTRY (The Knowledge Atom)**

`{`  
  `"header": {`  
    `"uuid": "sha256_hash",`  
    `"origin": { "agent_id": "string", "project_id": "string" },`  
    `"timestamp": "int64"`  
  `},`  
  `"taxonomy": {`  
    `"knowledge_area": "SWEBOK_KA_ENUM",`  
    `"tags": ["string"],`  
    `"applicability": "int64"`  
  `},`  
  `"payload": {`  
    `"content_type": "string",`  
    `"statement": "string",`  
    `"artifact_refs": ["uri_string"]`  
  `}`  
`}`

### **1.2 WBS\_NODE (Task Decomposition)**

`{`  
  `"header": { "type": "WBS_NODE", "id": "string" },`  
  `"task": "string",`  
  `"pdm_logic": { "dependencies": ["wbs_id"], "type": "FS" },`  
  `"cpb_alignment": ["atom_id"]`  
`}`

## **2\. Distributed Logic Schemas**

### **2.1 NODE\_HEARTBEAT**

`{`  
  `"location_id": "string",`  
  `"connectivity_state": "CONNECTED|DEGRADED|ISOLATED",`  
  `"local_wbs_claims": ["string"],`  
  `"sync_epoch": "int64"`  
`}`

### **2.2 L3\_DELTA\_PATCH**

`{`  
  `"header": { "type": "L3_DELTA_PATCH", "base_epoch": "int64" },`  
  `"patch": {`  
    `"offset": "int64",`  
    `"binary_delta": "hex_bytes",`  
    `"checksum": "sha256"`  
  `}`  
`}`

## **3\. SRE & Governance**

### **3.1 SWARM\_HEALTH\_SUMMARY**

`{`  
  `"cluster_status": { "apex": "string", "pitt": "string" },`  
  `"metrics": {`  
    `"knowledge_velocity": "float64",`  
    `"sync_latency_ms": "int64",`  
    `"toil_ratio": "float64"`  
  `}`  
`}`  
