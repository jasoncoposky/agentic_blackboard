# Project Insight openCypher Query Recipes

This catalog provides concrete, production-tested openCypher query templates for querying the Canonical Code Property Graph (CPG) stored in Project Insight (L3KVG). These queries are executed either via the MCP tool `insight_query_cypher` or via the CLI subcommand `insight query --format json "<query>"`.

---

## Table of Contents
1. [CPG Schema Overview & Conventions](#cpg-schema-overview--conventions)
2. [Execution Interfaces: MCP vs. CLI](#execution-interfaces-mcp-vs-cli)
3. [Recipe 1: Call Graph Traversal](#recipe-1-call-graph-traversal)
   - 1.1 Direct Upstream Callers
   - 1.2 Transitive Caller Tree (Reverse Blast Radius)
   - 1.3 Downstream Callee Subtree
   - 1.4 Callsite-Context Call Graph
4. [Recipe 2: State Affiliation & ModRef Analysis](#recipe-2-state-affiliation--modref-analysis)
   - 2.1 Direct Read/Write Access to Global Variables
   - 2.2 Transitive ModRef Reachability
   - 2.3 Shared State Interference (Concurrency Hazard)
5. [Recipe 3: Dataflow Taint Tracking](#recipe-3-dataflow-taint-tracking)
   - 3.1 Untrusted Input to Sensitive Sink Taint Paths
   - 3.2 Inter-Procedural Taint via Parameter Binding & Return Flows
   - 3.3 Sanitized vs. Unsanitized Taint Paths
6. [Recipe 4: Class Hierarchy & Virtual Dispatch](#recipe-4-class-hierarchy--virtual-dispatch)
   - 4.1 Class Inheritance Hierarchy
   - 4.2 Virtual Method Overrides
   - 4.3 Virtual Dispatch Resolution (Dynamic Target Discovery)
7. [Recipe 5: MemorySSA Def-Use Chains](#recipe-5-memoryssa-def-use-chains)
   - 5.1 Memory Def-Use and Clobber Edges
   - 5.2 Memory Phi Nodes and Incoming Versions
   - 5.3 Points-To and Aliasing Verification
8. [Best Practices for Swarm Agents](#best-practices-for-swarm-agents)

---

## CPG Schema Overview & Conventions

Project Insight models source code as a dual graph (Code Property Graph + Typed Memory Hierarchy) containing:
- **Node Categories (`NodeCategory`)**:
  - `Symbol`: Function, Method, GlobalVar, Class, Struct, MemberField, Constructor, Destructor, Variable, Parameter, LocalVar.
  - `Control`: FunctionEntry, BasicBlock, Branch, LoopHeader, ScopeExit, FunctionExit, BranchCondition.
  - `Data`: Variable, Parameter, Constant, MemoryPhi, Expression, CallSite, LoadExpr, StoreExpr, PhiNode, AllocationSite.
  - `Type`: Class, Struct, PrimitiveType, PointerType.
- **Edge Kinds (`EdgeKind`)**:
  - **Structural / Call**: `CALLS`, `CONTAINS`, `INVOKE_CALL`, `RETURN_FROM`, `MEMBER_OF`, `INHERITS`, `OVERRIDES`.
  - **Control Flow**: `CFG`, `CONTROLS`, `BRANCH_TRUE`, `BRANCH_FALSE`.
  - **Dataflow & SSA**: `DFG`, `DEFINES`, `USES`, `USE_DEF`, `TAINT_FLOWS_TO`, `PARAM_BINDING`, `RET_FLOW`.
  - **Memory & Pointer**: `POINTS_TO`, `ALIASED_TO`, `MEM_LOAD`, `MEM_STORE`, `MEM_DEF_USE`, `MEM_CLOBBER`, `MEM_PHI_INCOMING`.
  - **Typestate**: `TYPESTATE_TRANSITION`.

---

## Execution Interfaces: MCP vs. CLI

Swarm agents interact with Project Insight using two primary mechanisms:

### Option A: Via FastMCP Tool `insight_query_cypher`
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "insight_query_cypher",
    "arguments": {
      "query": "MATCH (caller:Function)-[:CALLS]->(target:Function {name: 'tls_handshake'}) RETURN caller.name AS caller, target.name AS target"
    }
  }
}
```

**MCP Response Structure:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "{\n  \"count\": 2,\n  \"node_count\": 2,\n  \"edge_count\": 0,\n  \"rows\": [\n    {\"caller\": \"tls_client_connect\", \"target\": \"tls_handshake\"},\n    {\"caller\": \"tls_server_accept\", \"target\": \"tls_handshake\"}\n  ]\n}"
      }
    ],
    "count": 2,
    "query": "MATCH (caller:Function)-[:CALLS]->(target:Function {name: 'tls_handshake'}) RETURN caller.name AS caller, target.name AS target",
    "nodes": [{"name": "tls_client_connect"}, {"name": "tls_server_accept"}],
    "edges": []
  }
}
```

### Option B: Via CLI Command
```bash
insight query --format json "MATCH (caller:Function)-[:CALLS]->(target:Function {name: 'tls_handshake'}) RETURN caller.name AS caller"
```

---

## Recipe 1: Call Graph Traversal

### 1.1 Direct Upstream Callers
Find all immediate callers of a target function. Used by the Architect Agent to identify affected integration points and determine the blast radius.

**openCypher Query:**
```cypher
MATCH (caller:Function)-[:CALLS]->(target:Function {name: "handle_packet"})
RETURN caller.name AS caller,
       caller.file AS source_file,
       caller.start_line AS source_line,
       target.name AS target
ORDER BY caller.name ASC
```

**Expected JSON Output:**
```json
{
  "count": 2,
  "rows": [
    {
      "caller": "network_dispatch_loop",
      "source_file": "src/net/dispatcher.cpp",
      "source_line": "142",
      "target": "handle_packet"
    },
    {
      "caller": "replay_engine_feed",
      "source_file": "src/replay/engine.cpp",
      "source_line": "89",
      "target": "handle_packet"
    }
  ]
}
```

---

### 1.2 Transitive Caller Tree (Reverse Blast Radius)
Discover all functions transitively upstream of a symbol up to $k$ hops (e.g., $k=3$). Essential for validating whether a modification can affect outer API entrypoints.

**openCypher Query:**
```cypher
MATCH path = (root:Function)-[:CALLS*1..3]->(target:Function {name: "allocate_buffer"})
RETURN root.name AS entrypoint,
       length(path) AS depth,
       [n IN nodes(path) | n.name] AS call_chain
ORDER BY depth ASC, root.name ASC
```

**Expected JSON Output:**
```json
{
  "count": 3,
  "rows": [
    {
      "entrypoint": "packet_init",
      "depth": 1,
      "call_chain": ["packet_init", "allocate_buffer"]
    },
    {
      "entrypoint": "handle_incoming_connection",
      "depth": 2,
      "call_chain": ["handle_incoming_connection", "packet_init", "allocate_buffer"]
    },
    {
      "entrypoint": "main_event_loop",
      "depth": 3,
      "call_chain": ["main_event_loop", "handle_incoming_connection", "packet_init", "allocate_buffer"]
    }
  ]
}
```

---

### 1.3 Downstream Callee Subtree
List all functions invoked directly or indirectly by a component under refactoring. Used by the Implementer Agent to ensure no required subsystem dependencies are broken.

**openCypher Query:**
```cypher
MATCH path = (root:Function {name: "parse_request"})-[:CALLS*1..2]->(callee:Function)
RETURN callee.name AS callee,
       length(path) AS depth,
       callee.file AS callee_file
ORDER BY depth ASC, callee.name ASC
```

**Expected JSON Output:**
```json
{
  "count": 3,
  "rows": [
    {
      "callee": "tokenize_header",
      "depth": 1,
      "callee_file": "src/http/parser.cpp"
    },
    {
      "callee": "validate_content_length",
      "depth": 1,
      "callee_file": "src/http/parser.cpp"
    },
    {
      "callee": "ascii_to_uint64",
      "depth": 2,
      "callee_file": "src/util/conv.cpp"
    }
  ]
}
```

---

### 1.4 Callsite-Context Call Graph
Inspect call sites within a function body, including invocation lines and arguments.

**openCypher Query:**
```cypher
MATCH (fn:Function {name: "tls_handshake"})-[:CONTAINS]->(cs:CallSite)-[:INVOKE_CALL]->(callee:Function)
RETURN cs.start_line AS line,
       cs.name AS callsite_expr,
       callee.name AS callee_name
ORDER BY cs.start_line ASC
```

**Expected JSON Output:**
```json
{
  "count": 2,
  "rows": [
    {
      "line": "210",
      "callsite_expr": "derive_secrets(ctx, session_key)",
      "callee_name": "derive_secrets"
    },
    {
      "line": "245",
      "callsite_expr": "emit_server_finished(ctx)",
      "callee_name": "emit_server_finished"
    }
  ]
}
```

---

## Recipe 2: State Affiliation & ModRef Analysis

### 2.1 Direct Read/Write Access to Global Variables
Identify every function that directly reads (`MEM_LOAD` / `READS` / `USES`) or mutates (`MEM_STORE` / `WRITES` / `DEFINES`) global or shared static state.

**openCypher Query:**
```cypher
MATCH (f:Function)-[access:MEM_STORE|MEM_LOAD|WRITES|READS]->(v:Variable)
WHERE v.is_global = "true" OR v.storage_class = "static"
RETURN f.name AS function_name,
       type(access) AS access_type,
       v.name AS variable_name,
       v.type AS variable_type
ORDER BY v.name ASC, f.name ASC
```

**Expected JSON Output:**
```json
{
  "count": 3,
  "rows": [
    {
      "function_name": "reset_session_counter",
      "access_type": "MEM_STORE",
      "variable_name": "g_active_sessions",
      "variable_type": "std::atomic<uint32_t>"
    },
    {
      "function_name": "get_session_count",
      "access_type": "MEM_LOAD",
      "variable_name": "g_active_sessions",
      "variable_type": "std::atomic<uint32_t>"
    },
    {
      "function_name": "init_tls_subsystem",
      "access_type": "MEM_STORE",
      "variable_name": "g_ssl_ctx",
      "variable_type": "SSL_CTX*"
    }
  ]
}
```

---

### 2.2 Transitive ModRef Reachability
Determine all global state variables that can be modified along any execution path originating from a given API entrypoint.

**openCypher Query:**
```cypher
MATCH (entry:Function {name: "process_transaction"})-[:CALLS*0..3]->(f:Function)-[:MEM_STORE]->(v:Variable)
WHERE v.is_global = "true"
RETURN DISTINCT entry.name AS entrypoint,
                f.name AS mutator_function,
                v.name AS mutated_global
```

**Expected JSON Output:**
```json
{
  "count": 2,
  "rows": [
    {
      "entrypoint": "process_transaction",
      "mutator_function": "record_ledger_entry",
      "mutated_global": "g_ledger_head"
    },
    {
      "entrypoint": "process_transaction",
      "mutator_function": "increment_tx_seq",
      "mutated_global": "g_transaction_sequence"
    }
  ]
}
```

---

### 2.3 Shared State Interference (Concurrency Hazard)
Detect concurrent functions that access the same global memory locations where at least one function performs a write (`MEM_STORE`).

**openCypher Query:**
```cypher
MATCH (writer:Function)-[:MEM_STORE]->(v:Variable)<-[:MEM_LOAD|MEM_STORE]-(reader:Function)
WHERE v.is_global = "true" AND writer.name <> reader.name
RETURN v.name AS shared_variable,
       writer.name AS writing_function,
       reader.name AS concurrent_function
ORDER BY v.name ASC
```

**Expected JSON Output:**
```json
{
  "count": 1,
  "rows": [
    {
      "shared_variable": "g_active_sessions",
      "writing_function": "terminate_session",
      "concurrent_function": "render_dashboard_metrics"
    }
  ]
}
```

---

## Recipe 3: Dataflow Taint Tracking

### 3.1 Untrusted Input to Sensitive Sink Taint Paths
Query end-to-end dataflow paths from unvalidated source nodes (e.g., HTTP request body, socket recv) to critical security sinks (e.g., `system`, `memcpy`, `SQL_EXEC`).

**openCypher Query:**
```cypher
MATCH path = (source:Node)-[:TAINT_FLOWS_TO*1..5]->(sink:Node)
WHERE source.is_taint_source = "true" AND sink.is_taint_sink = "true"
RETURN [n IN nodes(path) | n.name] AS taint_nodes,
       [n IN nodes(path) | n.location] AS locations,
       length(path) AS hops
```

**Expected JSON Output:**
```json
{
  "count": 1,
  "rows": [
    {
      "taint_nodes": ["user_input_buffer", "payload_ptr", "command_string", "system_call_arg"],
      "locations": ["src/http/recv.cpp:45", "src/http/recv.cpp:52", "src/exec/runner.cpp:112", "src/exec/runner.cpp:120"],
      "hops": 3
    }
  ]
}
```

---

### 3.2 Inter-Procedural Taint via Parameter Binding & Return Flows
Trace taint propagating across function boundaries via caller-to-callee argument binding (`PARAM_BINDING`) and return value flow (`RET_FLOW`).

**openCypher Query:**
```cypher
MATCH path = (src:Variable {name: "untrusted_token"})-[:PARAM_BINDING|RET_FLOW|DFG*1..4]->(target:Variable)
WHERE target.scope = "db_query_constructor"
RETURN [n IN nodes(path) | n.name] AS variable_flow,
       [r IN relationships(path) | type(r)] AS edge_sequence
```

**Expected JSON Output:**
```json
{
  "count": 1,
  "rows": [
    {
      "variable_flow": ["untrusted_token", "param_raw_id", "sanitized_candidate", "query_param"],
      "edge_sequence": ["PARAM_BINDING", "DFG", "RET_FLOW"]
    }
  ]
}
```

---

### 3.3 Sanitized vs. Unsanitized Taint Paths
Verify whether all dataflow paths pass through a designated sanitizer node.

**openCypher Query:**
```cypher
MATCH path = (src:Node {is_taint_source: "true"})-[:TAINT_FLOWS_TO*1..6]->(sink:Node {is_taint_sink: "true"})
WHERE NONE(n IN nodes(path) WHERE n.is_sanitizer = "true")
RETURN path
```

**Expected JSON Output:**
- If empty (`count: 0`), all paths are properly sanitized.
- If non-empty, represents a confirmed vulnerability trace for Verifier Agent refutation.

---

## Recipe 4: Class Hierarchy & Virtual Dispatch

### 4.1 Class Inheritance Hierarchy
Discover all derived implementations inheriting from a base interface or class.

**openCypher Query:**
```cypher
MATCH (derived:Class)-[:INHERITS*1..3]->(base:Class {name: "IStorageEngine"})
RETURN derived.name AS derived_class,
       derived.file AS defined_in,
       base.name AS base_class
ORDER BY derived.name ASC
```

**Expected JSON Output:**
```json
{
  "count": 3,
  "rows": [
    {
      "derived_class": "L3KVGStorageEngine",
      "defined_in": "include/storage/l3kvg.hpp",
      "base_class": "IStorageEngine"
    },
    {
      "derived_class": "RocksDBStorageEngine",
      "defined_in": "include/storage/rocksdb.hpp",
      "base_class": "IStorageEngine"
    },
    {
      "derived_class": "MemoryStorageEngine",
      "defined_in": "include/storage/memory.hpp",
      "base_class": "IStorageEngine"
    }
  ]
}
```

---

### 4.2 Virtual Method Overrides
Identify all override implementations of a virtual interface method across derived classes.

**openCypher Query:**
```cypher
MATCH (override:Function)-[:OVERRIDES]->(base_fn:Function {name: "commit_transaction"})
MATCH (override)-[:MEMBER_OF]->(cls:Class)
RETURN cls.name AS implementing_class,
       override.name AS method_name,
       override.file AS source_file,
       override.start_line AS line_number
ORDER BY cls.name ASC
```

**Expected JSON Output:**
```json
{
  "count": 2,
  "rows": [
    {
      "implementing_class": "L3KVGStorageEngine",
      "method_name": "commit_transaction",
      "source_file": "src/storage/l3kvg.cpp",
      "line_number": "340"
    },
    {
      "implementing_class": "MemoryStorageEngine",
      "method_name": "commit_transaction",
      "source_file": "src/storage/memory.cpp",
      "line_number": "115"
    }
  ]
}
```

---

### 4.3 Virtual Dispatch Resolution (Dynamic Target Discovery)
Resolve the set of concrete functions that could be invoked by a dynamic polymorphic callsite.

**openCypher Query:**
```cypher
MATCH (cs:CallSite {name: "engine->commit_transaction()"})-[:INVOKE_CALL]->(base_fn:Function)
OPTIONAL MATCH (override:Function)-[:OVERRIDES]->(base_fn)
RETURN base_fn.name AS abstract_target,
       collect(DISTINCT coalesce(override.name, base_fn.name)) AS possible_runtime_targets
```

**Expected JSON Output:**
```json
{
  "count": 1,
  "rows": [
    {
      "abstract_target": "commit_transaction",
      "possible_runtime_targets": ["L3KVGStorageEngine::commit_transaction", "MemoryStorageEngine::commit_transaction"]
    }
  ]
}
```

---

## Recipe 5: MemorySSA Def-Use Chains

### 5.1 Memory Def-Use and Clobber Edges
Analyze low-level memory definitions, clobbers, and uses generated by LLVM SSA memory lowering.

**openCypher Query:**
```cypher
MATCH (use_node:Node)-[rel:MEM_DEF_USE|MEM_CLOBBER]->(def_node:Node)
WHERE def_node.function = "process_packet"
RETURN use_node.name AS consumer,
       type(rel) AS relation_type,
       def_node.name AS producer,
       def_node.start_line AS def_line
ORDER BY def_node.start_line ASC
```

**Expected JSON Output:**
```json
{
  "count": 2,
  "rows": [
    {
      "consumer": "load_packet_len",
      "relation_type": "MEM_DEF_USE",
      "producer": "store_packet_len",
      "def_line": "56"
    },
    {
      "consumer": "store_packet_checksum",
      "relation_type": "MEM_CLOBBER",
      "producer": "store_packet_header",
      "def_line": "62"
    }
  ]
}
```

---

### 5.2 Memory Phi Nodes and Incoming Versions
Track confluent memory states across control-flow join points (branches, loops).

**openCypher Query:**
```cypher
MATCH (phi:Node {subkind: "MemoryPhi"})-[rel:MEM_PHI_INCOMING]->(incoming:Node)
WHERE phi.function = "evaluate_policy"
RETURN phi.name AS phi_node,
       incoming.name AS incoming_memory_version,
       incoming.start_line AS version_line
```

**Expected JSON Output:**
```json
{
  "count": 2,
  "rows": [
    {
      "phi_node": "mem_phi_join_block_3",
      "incoming_memory_version": "mem_store_branch_true",
      "version_line": "78"
    },
    {
      "phi_node": "mem_phi_join_block_3",
      "incoming_memory_version": "mem_store_branch_false",
      "version_line": "84"
    }
  ]
}
```

---

### 5.3 Points-To and Aliasing Verification
Verify pointer target sets to eliminate spurious aliasing warnings or ensure isolated memory regions.

**openCypher Query:**
```cypher
MATCH (ptr:Node)-[:POINTS_TO]->(target:Node)
WHERE ptr.function = "crypto_decrypt"
RETURN ptr.name AS pointer_variable,
       target.name AS memory_location,
       target.category AS location_type
```

**Expected JSON Output:**
```json
{
  "count": 1,
  "rows": [
    {
      "pointer_variable": "out_plain_buf",
      "memory_location": "heap_allocated_plaintext_slot",
      "location_type": "AllocationSite"
    }
  ]
}
```

---

## Best Practices for Swarm Agents

1. **Parameterize Query Constants**: Always filter on indexed properties (`name`, `category`, `subkind`, `file`) in `MATCH` or `WHERE` clauses to optimize L3KVG graph traversal.
2. **Limit Traversal Depth**: Always specify an upper bound on variable-length relationships (e.g., `[:CALLS*1..3]`, `[:TAINT_FLOWS_TO*1..5]`) to prevent combinatorial blowups.
3. **Use Reverse Blast Radius First**: When modifying an existing function, the Architect Agent should run `(root)-[:CALLS*1..k]->(target)` to discover all callers before partitioning tasks.
4. **Attach Evidence to Blackboards**: When the Verifier Agent refutes a task, copy the relevant `rows` from the openCypher taint/ModRef query directly into the `CounterexampleTrace` atom's `details.execution_trace`.
