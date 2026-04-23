# Knowledge Capture Skill

## name: knowledge-capture
## description: Strategically ingest technical insights, "gotchas", and architectural decisions into the ASOS Blackboard.

---

## Overview
This skill ensures that as an agent performs research or code analysis, it does not let valuable insights disappear. It provides a structured workflow for committing "Knowledge Atoms" to the ASOS Substrate, ensuring they are categorized by SWEBOK and properly anchored to Identities and Projects.

## When to Use
- After completing a research task that yielded a non-trivial conclusion.
- When discovering a "gotcha" or workaround in the codebase that others should know.
- After a design discussion where a decision was reached.
- When a "Strategic" insight is needed to guide "Tactical" agents.

## Process

1. **Synthesize the Statement**:
   - Condense the insight into a single, punchy "Statement" (1-2 sentences).
   - *Example*: "Raft consensus in Nucleus requires a strict timeout of 15ms to avoid heartbeat collision."

2. **SWEBOK Alignment**:
   - Assign the correct `KnowledgeArea` (1-15).
   - Use `CONFIG_MANAGEMENT` for build/CI issues, `DESIGN` for architecture, `COMPUTING_FOUNDATIONS` for algorithms.

3. **Identify Anchors**:
   - Identify which **Project** this belongs to (e.g., `ALPHA_SWARM`).
   - Identify the **Identity** of the author/contributor.

4. **Prepare the Bundle**:
   - Create a JSON Knowledge Bundle containing the header, taxonomy (tags), and payload (content).
   - Include `uncertainty` flags if the insight is experimental.

5. **Commit to Substrate**:
   - Use the `commit_knowledge_bundle` tool via the MCP server.

6. **Verify Materialization**:
   - Check the ASOS Dashboard or query the `/api/v1/health` endpoint to ensure the Atom count increased.

## Rationalizations (Excuses to avoid this)
| Excuse | Rebuttal |
| :--- | :--- |
| "It's just a small detail." | Small details are the primary source of toil. Capture it now. |
| "I'll add it at the end of the day." | Context is lost rapidly. Commit the Atom immediately after discovery. |
| "The user already knows this." | The Blackboard is for the *entire swarm*, not just the current user. |

## Red Flags
- Committing an atom without a Project anchor (creates orphans).
- Using "UNKNOWN" knowledge area when a more specific SWEBOK area fits.
- Statement is too long or contains multiple unrelated points (Split into multiple Atoms).

## Verification
- MCP tool returns a success status with a unique UUID.
- Librarian Audit log shows the node is successfully anchored.
