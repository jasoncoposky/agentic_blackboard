# Knowledge Capture Skill

## name: knowledge-capture
## description: Strategically ingest technical insights, "gotchas", and architectural decisions into the ASOS Blackboard with pre-flight discovery, citations, and dialectic links.

---

## Overview
This skill ensures that as an agent performs research, debugging, or architectural analysis, valuable insights are systematically captured rather than lost. It enforces the **"Discover Before You Create"** discipline, guiding agents to query existing knowledge via `search_commonplace`, synthesize atomic statements, ground claims with bibliographic citations (`Reference`), connect ideas with dialectic inter-note synapses (`NoteLink`), and commit nodes to the ASOS Substrate using `create_note`.

## When to Use
- After completing research or code analysis yielding a non-trivial conclusion.
- When discovering a "gotcha", performance bottleneck, or workaround in the codebase.
- After an architectural or design decision is finalized.
- When a strategic or dialectic insight must be captured to guide subsequent swarm agents.
- When reading technical documentation, papers, or RFCs that establish foundational domain knowledge.

## Core Process

### Step 0: Pre-Flight Discovery ("Discover Before You Create")
**MANDATORY RULE**: Never create a Knowledge Atom blindly. Always invoke `search_commonplace` before drafting a new node.
1. Formulate a search query targeting the core terminology of the insight:
   ```json
   {
     "query": "raft heartbeat collision nucleus",
     "limit": 10
   }
   ```
2. Evaluate returned matches:
   - **Identical statement found**: Do NOT duplicate. Retrieve the existing node via `get_node(uuid)` and attach supplementary evidence, or link your current task to it.
   - **Related or overlapping statement found**: Use `get_node_links(uuid)` to inspect the existing node's neighborhood. Plan a dialectic synapse (`EXTENDS`, `SUPPORTS`, `REFUTES`, or `SEE_ALSO`) connecting your new note to the existing one.
   - **No matches found**: Proceed to author the novel atom.

### Step 1: Synthesize the Atomic Statement
- Condense the core insight into a single, punchy, declarative assertion (1-2 sentences).
- The statement serves as the indexing key and must be understandable without surrounding prose.
- *Good Example*: "Raft consensus in Nucleus requires a strict heartbeat timeout of 15ms to avoid election collisions under network jitter."
- *Bad Example*: "Notes on Raft consensus and various networking experiments we ran yesterday afternoon."

### Step 2: SWEBOK & Domain Classification
Assign the appropriate integer `KnowledgeArea` (KA):
- **SWEBOK Engineering (1–15)**:
  - `1`: `REQUIREMENTS` — Specifications, user stories, constraints.
  - `2`: `DESIGN` — Architecture, system patterns, component topology.
  - `3`: `CONSTRUCTION` — Code implementation, language idioms, algorithms.
  - `4`: `TESTING` — Verification suites, test fixtures, fuzzing.
  - `6`: `CONFIG_MANAGEMENT` — Build systems, CMake, CI/CD, dependency management.
  - `13`: `COMPUTING_FOUNDATIONS` — Distributed consensus, memory models, runtime internals.
- **Commonplace & Research Domains (26–31)**:
  - `26`: `LITERATURE_READING` — Paper summaries, book excerpts, external publications.
  - `31`: `GENERAL_COMMONPLACE` — General dialectic notes, philosophical axioms, cross-cutting insights.

### Step 3: Identify Structural Anchors
Every atom must be anchored to avoid graph orphanhood:
- `project_id`: Target project anchor (e.g., `ALPHA_SWARM`, `project:nucleus`).
- `agent_id`: Identity anchor representing the author agent (e.g., `identity:analyst`, `identity:architect`).
- Ensure anchors exist using `ensure_node` if working in a newly initialized project context.

### Step 4: Attach Bibliographic References and Dialectic Synapses
Ground the atom with verifiable sources and inter-note connectivity:
1. **Bibliographic Citations (`references`)**:
   Add structured source attributions for external literature, RFCs, or documentation:
   ```json
   [
     {
       "title": "In Search of an Understandable Consensus Algorithm (Extended Version)",
       "creator": "Diego Ongaro and John Ousterhout",
       "page_numbers": "pp. 4-6",
       "excerpt": "Heartbeat timeouts must be significantly smaller than election timeouts to prevent unnecessary candidate transitions.",
       "tags": ["DISTRIBUTED_SYSTEMS", "CONSENSUS"]
     }
   ]
   ```
2. **Dialectic Synapses (`note_links`)**:
   Connect the atom to prior notes using typed semantic relations:
   ```json
   [
     {
       "target_uuid": "note-8a7b3c2d",
       "relation": "EXTENDS",
       "context": "Specializes general Raft heartbeat parameters for high-frequency Nucleus desktop IPC."
     }
   ]
   ```

### Step 5: Commit to Substrate (`create_note`)
Submit the atom using the `create_note` MCP tool with automated duplicate checking enabled:
```json
{
  "project_id": "project:nucleus",
  "agent_id": "identity:architect",
  "statement": "Raft consensus in Nucleus requires a strict heartbeat timeout of 15ms to avoid election collisions under network jitter.",
  "content": "Detailed architectural rationale:\n- Nucleus desktop IPC runs over low-latency shared memory channels.\n- Setting election timeout to 150ms and heartbeat to 15ms gives a 10x safety margin.\n- Benchmarked under 20% simulated packet jitter.",
  "ka": 13,
  "tags": ["RAFT", "CONSENSUS", "NETWORKING", "NUCLEUS"],
  "references": [
    {
      "title": "In Search of an Understandable Consensus Algorithm",
      "creator": "Ongaro & Ousterhout",
      "page_numbers": "Section 5.2",
      "excerpt": "Broadcast time << Election timeout << MTBF"
    }
  ],
  "note_links": [
    {
      "target_uuid": "note-8a7b3c2d",
      "relation": "EXTENDS",
      "context": "Adapts general timing bounds to local IPC."
    }
  ],
  "check_duplicates": true
}
```
- If `create_note` returns `status: "COMMITTED"`, record the returned `uuid`.
- If `create_note` returns `status: "ALREADY_EXISTS"`, inspect the returned `uuid`. Link to that existing node or update its context rather than forcing duplicate creation.

### Step 6: Verify Materialization & Graph Connectivity
1. Verify that `create_note` succeeded with status `COMMITTED` and a unique UUID.
2. Query `get_node_links(uuid, direction="both")` to ensure citations and inter-note synapses are materialized in the blackboard topology.
3. If necessary, use `link_nodes` to add additional cross-project or cross-identity relationships.

---

## Anti-Patterns & Rationalizations

| Excuse / Rationalization | Engineering Rebuttal |
| :--- | :--- |
| "I don't need to search first; my insight is completely novel." | Unchecked creation causes duplicate node fragmentation. Always run `search_commonplace` first. |
| "Adding references and links takes too much time." | Unanchored assertions without citations or links become isolated noise. Dialectic synapses create compounding swarm intelligence. |
| "It's just a minor implementation detail." | Small undocumented quirks cause compounding hours of debugging toil for other agents. Capture it immediately. |
| "I'll batch all my notes at the end of the session." | Context and nuances degrade rapidly. Commit atoms synchronously upon discovery. |
| "The user already knows this from chat." | Chat history is transient. The ASOS Blackboard persists collective memory across swarms, sessions, and lifetimes. |

---

## Red Flags
- Calling `create_note` or `commit_knowledge_bundle` without prior `search_commonplace` execution.
- Blindly setting `check_duplicates: false` to bypass `ALREADY_EXISTS` warnings.
- Statement exceeds 2 sentences or bundles multiple distinct assertions into one blob.
- Setting `ka: 0` (`UNKNOWN`) when a specific SWEBOK or Commonplace area applies.
- Committing notes without a valid `project_id` or `agent_id` (creates graph orphans).
- Leaving `references` or `note_links` empty when external documents or prior notes are referenced in `content`.

---

## Verification Checklist
- [ ] `search_commonplace` executed prior to atom authoring.
- [ ] Statement is atomic, declarative, and bounded to 1–2 sentences.
- [ ] SWEBOK or Commonplace `KnowledgeArea` integer accurately assigned.
- [ ] External sources captured in structured `references` array with locators.
- [ ] Related prior atoms linked via typed `note_links` (`EXTENDS`, `SUPPORTS`, etc.).
- [ ] `create_note` returned `status: "COMMITTED"` with a valid UUID.
- [ ] `get_node_links` confirms edges projected into the substrate graph.
