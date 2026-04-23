# ASOS: Agentic Sovereign Orchestration Substrate

ASOS is a high-performance, distributed **Agentic Blackboard** designed for the decentralized governance and coordination of AI swarm intelligence. It serves as the primary "shared brain" for agentic swarms, enabling real-time semantic consensus, conflict resolution, and spatial materialization within the **Project Nucleus** ecosystem.

![ASOS Dashboard](https://img.shields.io/badge/Substrate-v0.4--Alpha-cyan)
![ZMQ](https://img.shields.io/badge/Network-ZeroMQ-purple)
![SWEBOK](https://img.shields.io/badge/Compliance-SWEBOK--15-blue)

## 🌌 Core Architecture

ASOS is built on a "Substrate-First" philosophy, where knowledge is not just stored, but actively managed by a fleet of dedicated engines:

*   **Blackboard (The Core)**: A high-frequency, thread-safe knowledge hub that stores "Atoms" (CpbEntry). It implements a **BetterThan** logic for semantic merge, ensuring that the most accurate and high-priority knowledge always prevails.
*   **DeltaEngine**: A specialized graph processor that maintains the relationships (Synapses) between Atoms, Identities, and Projects.
*   **Orchestrator (ZMQ Mirror)**: A ZeroMQ-native distribution engine that mirrors the blackboard state across the swarm on port **8090**. It provides sub-15ms synchronization latency between distributed nodes.
*   **Librarian**: An autonomous governance agent that audits the graph for orphans, resolves uncertainty, and maintains SWEBOK compliance.

## 🌉 Project Nucleus Integration

ASOS is deeply integrated with **Project Nucleus**, bridging semantic reasoning with spatial reality:

*   **Spatial Anchors**: Atoms can be "anchored" to physical or virtual fiducials in the Nucleus environment.
*   **Tactical Materialization**: The substrate can dispatch ZeroMQ commands to the **Nucleus Governor** to spawn widgets or HUD elements directly onto the Sovereign Desktop.
*   **MCP Bridge**: A Model Context Protocol (MCP) server allows any LLM-based agent to interact with the blackboard using standardized tools.

## 🛠️ Components

### 1. ASOS Daemon (`asos_daemon`)
The C++ high-performance storage and orchestration engine.
*   **API Port**: `8085` (REST)
*   **Mirror Port**: `8090` (ZMQ Pub/Sub)
*   **Control Port**: `5556` (Nucleus MCP Bridge)

### 2. Librarian Knowledge Studio
A Next.js-based visualization and governance dashboard.
*   **Map View**: Real-time force-directed graph of the substrate topology.
*   **Studio View**: List view of Knowledge Atoms with "Promote" and "Materialize" actions.
*   **Atmospheric Aesthetic**: A drifting nebular UI that reflects the real-time health of the swarm.

### 3. Agent Skills Suite
Standardized Markdown-based procedural guides that instruct agents on how to leverage the substrate:
*   `knowledge-capture`: Strategic ingestion of research.
*   `task-orchestration`: Goal decomposition and WBS mapping.
*   `spatial-materialization`: Projecting knowledge into physical space.

## 🚀 Quick Start

### Build the Substrate (C++)
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target asos_daemon
./Release/asos_daemon
```

### Launch the Dashboard (Next.js)
```bash
cd asos-dashboard
npm install
npm run dev
```

### Connect an Agent (Python/MCP)
```bash
python asos_mcp_server.py
```

## 📖 API Reference

| Endpoint | Method | Description |
| :--- | :--- | :--- |
| `/api/v1/graph/bundle` | `POST` | Commit a batch of anchored Knowledge Atoms. |
| `/api/v1/cpb/promote` | `POST` | Elevate an atom to `PRINCIPLE` status. |
| `/api/v1/nucleus/materialize` | `POST` | Spawn a spatial widget in Project Nucleus. |
| `/api/v1/graph/snapshot` | `GET` | Retrieve the full substrate topology. |

---

**Developed for the Advanced Agentic Coding Swarm.**
*"Knowledge is the only substrate that grows when shared."*
