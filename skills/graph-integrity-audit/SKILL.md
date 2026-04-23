# Graph Integrity Audit Skill

## name: graph-integrity-audit
## description: Perform periodic SWEBOK compliance and structural health checks on the ASOS Knowledge Graph.

---

## Overview
A blackboard is only as useful as its structural integrity. This skill provides a workflow for agents to "garbage collect" stale data, resolve uncertainty, and ensure that the "Principle of No Orphans" is maintained.

## When to Use
- Before a major system migration.
- After a bulk ingestion of data (e.g., after `knowledge-capture` sessions).
- When the `Monitor` reports high "Toil Ratio" or "Friction".

## Process

1. **Topology Scan**:
   - Query for all nodes without an `IDENTITY` or `PROJECT` anchor.
   - For every orphan, identify its "Origin Agent" from the header.

2. **Stub Resolution**:
   - For every identified Origin Agent that is missing an `IDENTITY` node, create a stub.
   - Re-anchor the orphans to the new stubs.

3. **Uncertainty Triage**:
   - Filter atoms with `uncertainty: true`.
   - Assign a task (`task-orchestration`) to verify or disprove the atom's statement.

4. **SWEBOK Balancing**:
   - Check the distribution of Knowledge Areas.
   - If a project is 90% "TESTING" and 0% "REQUIREMENTS", flag it as "Strategically Blind".

5. **Garbage Collection**:
   - Identify atoms that have been `SUPERSEDED_BY` newer versions.
   - Archive stale nodes to the `loser_key` path in the substrate.

## Rationalizations
| Excuse | Rebuttal |
| :--- | :--- |
| "The graph works even with orphans." | Orphans degrade search relevance and break the "Collective Reasoning" path. |
| "I'll clean it up later." | Technical debt in a knowledge graph compounds exponentially. |

## Red Flags
- Deleting atoms without establishing a `SUPERSEDED_BY` relationship.
- Ignoring uncertainty flags during an audit.
- High counts of "STUB" nodes that haven't been hydrated in > 24 hours.

## Verification
- `Librarian` reports 0 orphans in the target project cluster.
- `Monitor` API shows a decrease in "Toil Ratio" after cleanup.
