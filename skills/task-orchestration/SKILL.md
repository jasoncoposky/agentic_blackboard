# Task Orchestration Skill

## name: task-orchestration
## description: Decompose complex goals into a Work Breakdown Structure (WBS) on the ASOS Blackboard.

---

## Overview
This skill guides agents in breaking down a "Mission" into actionable "Tasks" (WBS Nodes) that are semantically linked to existing Knowledge Atoms. This ensures that the execution path is informed by the system's collective intelligence.

## When to Use
- When starting a new feature implementation or refactor.
- When a `/plan` is approved and needs to be tracked on the Blackboard.
- When a task is discovered to have hidden dependencies.

## Process

1. **Mission Analysis**:
   - Query the Blackboard for existing atoms related to the mission keyword.
   - Identify the "Identity Anchor" responsible for the mission.

2. **Node Decomposition**:
   - Define a series of WBS Nodes (`id`, `task`, `content`).
   - Assign a PDM Logic (e.g., "FS" for Finish-to-Start).

3. **Knowledge Alignment**:
   - For each WBS Node, identify relevant `CPB_ENTRY` atoms.
   - Include these atom IDs in the `cpb_alignment` field of the WBS Node.

4. **Commit to Substrate**:
   - Use the `ensure_node` or a dedicated `commit_wbs_node` tool via MCP.

5. **Dependency Linking**:
   - Establish `DEPENDS_ON` edges between nodes to create the critical path.

## Rationalizations
| Excuse | Rebuttal |
| :--- | :--- |
| "A local task list is enough." | Local lists aren't visible to the Governor or other swarm members. |
| "It's a linear task, no need for WBS." | Explicit dependencies prevent race conditions in multi-agent environments. |

## Red Flags
- WBS nodes with no `cpb_alignment` (implies the task is being done "in the dark").
- Circular dependencies in the task graph.
- "Identity Bloat": Assigning too many tasks to a single identity without considering swarm distribution.

## Verification
- Graph visualization shows a clear hierarchy from Project -> WBS Node -> Knowledge Atom.
- `Librarian` reports 0% orphan rate for the new task cluster.
