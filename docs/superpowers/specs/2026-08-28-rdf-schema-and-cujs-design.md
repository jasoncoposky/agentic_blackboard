# Design Spec: Agentic Blackboard RDF Schema Enhancements & Critical User Journeys (CUJs)
**Date**: 2026-08-28  
**Status**: APPROVED (Approach A)  
**Author**: Advanced Agentic Coding Pair  

---

## 1. Overview & Objectives

The **Agentic Blackboard** serves as the high-performance distributed blackboard for agentic swarm intelligence, spatial computing (Project Nucleus), and holistic life journaling. 

This specification establishes:
1. **First-Class Semantic Predicates (`ab::rel`)**: Formalizing task dependencies (`DEPENDS_ON`, `BLOCKS`, `SUBTASK_OF`), compliance auditing (`VALIDATED_BY`), and goal alignment (`CONTRIBUTES_TO`).
2. **Bitemporality**: Adding `event_timestamp` to `CpbEntry::Header` to clearly decouple physical occurrence time from substrate transaction/commit time.
3. **Long-Term Goal Hierarchy (`GoalNode`)**: Providing structured multi-period goal tracking linked to everyday knowledge atoms, wellness logs, and engineering tasks.
4. **W3C RDF Turtle Export (`RdfExporter`)**: Enabling seamless interoperability with Semantic Web, GraphDB, and external visualization tools via standard Turtle (`.ttl`) format and REST API.
5. **Formalization of 5 Core Critical User Journeys (CUJs)**: Mapping end-to-end multi-agent execution workflows.

---

## 2. Detailed Schema Enhancements

### 2.1. New Semantic Graph Relationships (`ab::rel`)

```cpp
namespace rel {
    // Baseline Provenance & Scoping
    const std::string CREATED_BY = "CREATED_BY";
    const std::string BELONGS_TO = "BELONGS_TO";
    const std::string MAINTAINS = "MAINTAINS";

    // Versioning & Analogy
    const std::string SUPERSEDED_BY = "SUPERSEDED_BY";
    const std::string CPB_SIMILARITY = "CPB_SIMILARITY";
    const std::string RELATED_TO = "RELATED_TO";

    // Spatial & Physical Materialization
    const std::string REPRESENTED_BY = "REPRESENTED_BY";
    const std::string ANCHORED_TO = "ANCHORED_TO";

    // Social & Geographic Grounding
    const std::string MENTIONS = "MENTIONS";
    const std::string OCCURRED_AT = "OCCURRED_AT";

    // --- NEW ENHANCEMENTS ---
    
    // Task Decomposition & Precedence (WBS / PDM DAG)
    const std::string DEPENDS_ON = "DEPENDS_ON";         // Node A -> Node B (A requires B to complete)
    const std::string BLOCKS = "BLOCKS";                 // Node B -> Node A (B blocks A)
    const std::string SUBTASK_OF = "SUBTASK_OF";         // Child Task -> Parent Epic/Task

    // Compliance & Auditing Provenance
    const std::string VALIDATED_BY = "VALIDATED_BY";     // Atom / EU -> IdentityNode (Auditor Agent)

    // Strategic Goal Alignment
    const std::string CONTRIBUTES_TO = "CONTRIBUTES_TO"; // Atom / Task / Journal -> GoalNode
}
```

---

### 2.2. Bitemporal Time Modeling (`CpbEntry::Header`)

To accommodate retrospective logging (e.g. logging a journal entry or post-mortem of an event that occurred earlier), `Header` is expanded:

```cpp
struct Header {
    std::string uuid;
    struct Origin {
        std::string agent_id;
        std::string project_id;
    } origin;
    int64_t timestamp = 0;       // Transaction Time (Substrate commit time in epoch ms)
    int64_t event_timestamp = 0; // Valid/Event Time (Physical occurrence time in epoch ms)
};
```

* **Serialization Rule**: `event_timestamp` is written to the BSON `"header"` object.
* **Deserialization & Backward Compatibility**: If `"event_timestamp"` key is absent, it defaults to `timestamp`.

---

### 2.3. Goal & Milestone Schema (`GoalNode`)

```cpp
struct GoalNode {
    std::string goal_id;          // e.g. "goal:marathon-2026", "goal:c-compiler"
    std::string title;
    std::string description;
    std::string category;         // "ENGINEERING", "HEALTH", "EDUCATION", "LIFE"
    int64_t target_date = 0;      // Target completion epoch ms
    double target_metric = 100.0; // Numerical target
    double current_progress = 0.0;// Numerical current progress (0.0 to 100.0 or units)
    std::string status = "ACTIVE";// "ACTIVE", "COMPLETED", "PAUSED", "ABANDONED"
    std::string content;

    void serialize(lite3cpp::Buffer& buf) const;
    static GoalNode deserialize(const lite3cpp::Buffer& buf);
};
```

* **Storage**: Stored as node `goal:<goal_id>` in `l3kvg`.
* **Auto-Anchoring**: Committing an atom with `rel::CONTRIBUTES_TO` auto-creates a stub goal node if it does not yet exist.

---

### 2.4. W3C RDF Turtle Serializer (`RdfExporter`) & REST API

The `RdfExporter` class inspects nodes and edges in `l3kvg` and outputs standard W3C Turtle (`text/turtle`):

```turtle
@prefix ab: <http://agenticblackboard.ai/schema#> .
@prefix rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
@prefix prov: <http://www.w3.org/ns/prov#> .
@prefix geo: <http://www.w3.org/2003/01/geo/wgs84_pos#> .

<atom:101> a ab:KnowledgeAtom ;
    ab:statement "Visitor pattern for AST traversal in C++" ;
    ab:knowledgeArea 2 ;
    ab:applicability 85 ;
    prov:wasGeneratedBy <identity:agent-arch> ;
    ab:belongsTo <project:proj-compiler> ;
    ab:eventTime "2026-08-28T16:00:00Z"^^xsd:dateTime ;
    prov:generatedAtTime "2026-08-28T16:05:00Z"^^xsd:dateTime ;
    ab:validatedBy <identity:auditor-007> ;
    ab:contributesTo <goal:c-compiler> .
```

* **HTTP Endpoint**: `GET /api/v1/graph/export?format=turtle` in `ApiServer`.

---

## 3. Critical User Journeys (CUJs) Matrix

```mermaid
journey
    title Agentic Blackboard Swarm Critical User Journeys
    section CUJ 1: Multi-Agent SWE Execution
      Decompose WBS into DAG: 5: Architect Agent
      Claim Subtask & Execute: 5: Worker Agent
      Audit & Promote Principle: 5: Validator Agent
    section CUJ 2: Life Journaling & Goals
      Log Journal & Wellness: 5: Human / Agent
      Link Mentions & Location: 5: System
      Advance Long-Term Goal: 5: Goal Tracker
    section CUJ 3: Spatial Telemetry (Nucleus)
      Sense Physical Fiducial: 5: Spatial Sensor
      Materialize HUD Widget: 5: Nucleus Governor
    section CUJ 4: Disconnected Sync & Merge
      Enter Safe-Mode on Partition: 5: Isolated Node
      Binary XOR Patch Exchange: 5: Orchestrator / DeltaEngine
      CRDT Semantic Merge: 5: Blackboard
    section CUJ 5: Knowledge Discovery
      Mine Structural Analogies: 5: Librarian
      Export RDF Turtle Snapshot: 5: External Tool / Researcher
```

### Detailed CUJ Definitions:
1. **CUJ 1: Multi-Agent WBS Task Decomposition & Execution**
   * Architect agent creates project node and child work breakdown structure with `SUBTASK_OF` and `DEPENDS_ON`.
   * Worker subagents query open unblocked tasks, execute units, commit knowledge atoms with observations.
   * Validator agent runs SWEBOK validation, signs V&V signature, creates `VALIDATED_BY` edge, and triggers principle promotion.

2. **CUJ 2: Holistic Life Journaling & Goal Alignment**
   * Agent/User commits journal entry with `event_timestamp`, `Wellness` telemetry, and `Education` logs under private multi-tenant UID.
   * Entry creates semantic links to referenced contacts (`MENTIONS`), physical places (`OCCURRED_AT`), and long-term milestones (`CONTRIBUTES_TO`).
   * Multi-tenant ACLs prevent cross-user leakage.

3. **CUJ 3: Project Nucleus Spatial Grounding & Materialization**
   * Spatial camera/sensor detects fiducial tag $\to$ commits `SpatialAnchorNode`.
   * Blackboard binds knowledge atom $\to$ `NucleusWidgetNode` $\to$ `ANCHORED_TO` `SpatialAnchorNode`.
   * `Orchestrator` dispatches JSON control command over ZMQ port 5556 to spawn widget on Sovereign Desktop.

4. **CUJ 4: Swarm Partition Resilience & Semantic CRDT Merge**
   * Node loses network connection $\to$ detects heartbeat timeout $\to$ switches state to `ISOLATED` (Safe-Mode).
   * Local writes mark `uncertainty = true`.
   * On reconnection, `DeltaEngine` computes and applies binary XOR patches (`L3DeltaPatch`).
   * Semantic collisions evaluated via `better_than` logic; displaced records archived under `:los:<timestamp>` with `SUPERSEDED_BY` lineage.

5. **CUJ 5: Structural Analogy Discovery & Semantic Web Interoperability**
   * `Librarian` background governance agent audits graph for unanchored orphans.
   * Computes Jaccard keyword and tag similarity across disparate projects to generate `CPB_SIMILARITY` synapses.
   * External tools query `GET /api/v1/graph/export?format=turtle` to ingest full graph topology into Neo4j, Protege, or Obsidian.

---

## 4. Verification Plan

1. **Schema Unit Tests (`ab_verify`)**:
   - Verify `DEPENDS_ON`, `BLOCKS`, `SUBTASK_OF`, `VALIDATED_BY`, and `CONTRIBUTES_TO` edge traversal.
   - Verify `GoalNode` serialization, deserialization, and progress tracking.
   - Verify bitemporal sorting and filtering (`event_timestamp` vs `timestamp`).
   - Verify RDF Turtle generation outputs valid Turtle syntax and correct triple statements.
2. **Regression Testing**:
   - Ensure all 8 existing test cases continue to pass cleanly with clean database tear-down.
