# Spatial Materialization Skill

## name: spatial-materialization
## description: Project semantic Knowledge Atoms into the Nucleus Sovereign Desktop via widgets and spatial anchors.

---

## Overview
This skill enables agents to "Manifest" knowledge in the user's physical/virtual environment. By mapping Atoms to Nucleus Widgets and binding them to Spatial Anchors (fiducial markers), agents can create "Liquid UI" experiences that are spatially aware.

## When to Use
- When a "Critical Atom" (e.g., a high-uncertainty principle) needs user attention.
- When the user asks for a "visualization" of a project.
- When a physical object (marked with a fiducial) needs to be semantically identified.

## Process

1. **Atom Selection**:
   - Query the Blackboard for the target Atom ID.
   - Evaluate the `utility_score` (if available) to determine materialization priority.

2. **UI Mapping**:
   - Select a `WidgetBehavior` (e.g., "status_ring" for health, "tree_list" for WBS).
   - Define a `label` for the widget.

3. **Anchor Discovery**:
   - List available `SPATIAL_ANCHOR` nodes in the Blackboard.
   - If no anchors exist, default to "Float Mode" (origin 0,0,0).

4. **Execution**:
   - Call `spawn_widget(atom_id, behavior, label)` via MCP.
   - Call `bind_anchor_to_atom(marker_id, atom_id)` to lock the widget to a physical point.

5. **Interlock Sync**:
   - Verify that the `NUCLEUS_WIDGET` node is created in ASOS with the correct `fragment_id`.

## Rationalizations
| Excuse | Rebuttal |
| :--- | :--- |
| "I'll just print the data in chat." | Chat is ephemeral; Nucleus widgets provide persistent, glanceable awareness. |
| "Spatial anchoring is overkill." | Millimeter-level precision is the "Sovereignty" requirement of Project Nucleus. |

## Red Flags
- Spawning a widget without linking it back to an Atom (creates a "Ghost Widget").
- Overlapping widgets at the same spatial coordinates.
- Using a high-latency widget (e.g., "bar_graph" with 100 values) for a low-latency requirement.

## Verification
- Governor telemetry shows a successful `BlackboardOp` commit in Nucleus.
- `Librarian` visualization shows the `REPRESENTED_BY` edge is active.
