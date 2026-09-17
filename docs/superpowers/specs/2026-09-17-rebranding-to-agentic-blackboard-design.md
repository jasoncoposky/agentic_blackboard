# Rebranding from ASOS to Agentic Blackboard (AB) Design Specification

**Date:** 2026-09-17  
**Status:** Approved  
**Topic:** Rebranding ASOS to `agentic_blackboard` / `ab`  

---

## 1. Overview & Objective

The project is officially branded as **Agentic Blackboard** (abbreviated **AB** or `ab`). All previous references to `asos` across the codebase, build system, directories, packaging, tools, tests, documentation, and skills are to be comprehensively eradicated and replaced with `agentic_blackboard` or `ab`.

---

## 2. Core Architecture & C++ Restructuring

### 2.1 Header Directory Structure
The header directory is moved from `include/asos/` to `include/agentic_blackboard/`, with a companion convenience alias directory `include/ab/`:

```
include/
├── agentic_blackboard/
│   ├── ApiServer.hpp
│   ├── Blackboard.hpp
│   ├── DeltaEngine.hpp
│   ├── Librarian.hpp
│   ├── Monitor.hpp
│   ├── Orchestrator.hpp
│   ├── RdfExporter.hpp
│   ├── schema.hpp
│   └── Validator.hpp
└── ab/
    ├── ApiServer.hpp       (forwarding to <agentic_blackboard/ApiServer.hpp>)
    ├── Blackboard.hpp      (forwarding to <agentic_blackboard/Blackboard.hpp>)
    ├── DeltaEngine.hpp     (forwarding to <agentic_blackboard/DeltaEngine.hpp>)
    ├── Librarian.hpp       (forwarding to <agentic_blackboard/Librarian.hpp>)
    ├── Monitor.hpp         (forwarding to <agentic_blackboard/Monitor.hpp>)
    ├── Orchestrator.hpp    (forwarding to <agentic_blackboard/Orchestrator.hpp>)
    ├── RdfExporter.hpp     (forwarding to <agentic_blackboard/RdfExporter.hpp>)
    ├── schema.hpp          (forwarding to <agentic_blackboard/schema.hpp>)
    └── Validator.hpp       (forwarding to <agentic_blackboard/Validator.hpp>)
```

### 2.2 Namespaces
All C++ classes, structs, enums, and functions are placed under `namespace agentic_blackboard`.
The shorthand namespace alias is defined at the root:
```cpp
namespace agentic_blackboard {
// Core engine, schema, and API definitions
}

namespace ab = agentic_blackboard;
```

All source implementations (`src/*.cpp`) and test harnesses will migrate from `namespace asos` to `namespace agentic_blackboard` / `namespace ab`.

### 2.3 Header Guards & Logging Prefixes
- Header guards updated to `AGENTIC_BLACKBOARD_<NAME>_HPP`.
- Runtime logging identifiers updated from `[ASOS]`, `[ASOS Server]` to `[AgenticBlackboard]` and `[AB]`.
- Salt token prefix updated from `asos_salt_token_v1:` to `ab_salt_token_v1:` in both `src/Blackboard.cpp` and `src/ab-ctl.py`.

---

## 3. Build System, Executables & Packaging

### 3.1 CMake Configuration (`CMakeLists.txt`)
- **Core Library Target:**
  - Rename target `asos_engine` to `ab_engine`.
  - Provide CMake alias `agentic_blackboard::engine`.
  - Produce static library `libab_engine.a`.
- **Daemon Executables:**
  - Canonical binary remains `agentic-blackboardd` (installed to `/usr/bin/agentic-blackboardd`).
  - Provide developer alias target `ab_daemon` linking to the same entrypoint.
- **Verification Target:**
  - Rename test executable target `asos_verify` to `ab_verify` (produces `build/ab_verify`).
- **Include Paths:**
  - `${CMAKE_CURRENT_SOURCE_DIR}/include` exposing both `agentic_blackboard/` and `ab/`.

### 3.2 Packaging, Limits & Systemd
- **RPM Spec (`packaging/rpm/agentic-blackboard.spec`):**
  - Scrub any remaining references to `asos` in comments, descriptions, and scriptlets.
  - Maintain canonical package `agentic-blackboard-0.4.0-1.el9.x86_64.rpm`.
- **Systemd Unit (`packaging/systemd/agentic-blackboard.service`):**
  - Description: `Agentic Blackboard Daemon (ab)`.
- **Configuration Templates:**
  - Clean comments in `packaging/config/blackboard.conf` and `blackboard.conf.default`.
- **Git Ignore (`.gitignore`):**
  - Replace `asos_db` and `test_asos_db` with `ab_db`, `ab_db-*`, and `blackboard_db`.

---

## 4. Tooling, MCP, Dashboard & Asset Migrations

### 4.1 Relocated Directories and Files
- `asos-dashboard/` $\rightarrow$ `ab-dashboard/`
- `asos-mcp/` $\rightarrow$ `ab-mcp/`
- `asos_mcp_server.py` $\rightarrow$ `ab_mcp_server.py`
- `asos_logo.png` $\rightarrow$ `ab_logo.png`
- `asos_screenshot.png` $\rightarrow$ `ab_screenshot.png`
- `scratch/populate_asos_project.py` $\rightarrow$ `scratch/populate_ab_project.py`

### 4.2 Dashboard & Node MCP Updates
- `ab-dashboard/package.json`: package name `"ab-dashboard"`.
- `ab-dashboard/src/app/layout.js`, `page.js`, `Sidebar.js`: change UI titles and brand headings to "Agentic Blackboard".
- `ab-mcp/package.json`: package name `"ab-mcp"`.
- `ab-mcp/index.js`: update server banner and tool registration logging.

### 4.3 `ab-ctl` Tooling & FastMCP Server
- Ensure all command descriptions, help banners, and default database paths use `agentic-blackboard` and `ab`.
- Reference `ab_mcp_server` and embedded FastMCP bridge tools.
- Use `ab_salt_token_v1:` for bootstrap token generation and validation.

---

## 5. Documentation & Skills Rebranding

### 5.1 Root Guides & Documentation
- Rename `ASOS Schema & Code Reference Guide.md` $\rightarrow$ `Agentic Blackboard Schema & Code Reference Guide.md`.
- Rename `ASOS Architecture & Master Schema TRD v0.2.md` $\rightarrow$ `Agentic Blackboard Architecture & Master Schema TRD v0.2.md`.
- In `README.md`, thoroughly update project branding, architecture diagrams, build targets (`ab_verify`, `agentic-blackboardd`), and CLI instructions (`ab-ctl`).

### 5.2 Built-in Skills (`skills/`)
Update references to ASOS in:
- `skills/commonplace-curation/SKILL.md`
- `skills/graph-integrity-audit/SKILL.md`
- `skills/knowledge-capture/SKILL.md`
- `skills/procedural-catalog/SKILL.md`
- `skills/spatial-materialization/SKILL.md`
- `skills/task-orchestration/SKILL.md`

### 5.3 Historical Superpower Specs & Plans (`docs/superpowers/`)
Update historical design specifications and implementation plans across `docs/superpowers/specs/` and `docs/superpowers/plans/` to consistently use the `Agentic Blackboard` and `ab` terminology.

---

## 6. Verification & Acceptance Criteria

1. **Build Integrity:**
   - `cmake --build build --target ab_engine ab_verify agentic-blackboardd` builds cleanly with zero errors or warnings.
2. **Core Verification:**
   - `./build/ab_verify` runs and passes 100% of all unit, graph, ACL, provenance, token auth, and restart resilience tests.
3. **Multi-Surface & CLI Integration:**
   - `python3 scratch/test_multi_surface_sync.py` passes all 6 phases.
   - `python3 scratch/test_ab_ctl.py` passes all 7 CLI and FastMCP smoke steps.
4. **RPM Packaging & Container Deployment:**
   - `python3 scratch/test_rpm_build.py` produces and validates the canonical RPM package.
   - `python3 scratch/test_e2e_central_deployment.py` completes clean container build, startup, volume auto-bootstrap, and CLI interaction.
5. **No Lingering References:**
   - Case-insensitive search `git grep -i "asos"` returns 0 results across the repository.
