#pragma once

#include "L3KVG/Engine.hpp"
#include "schema.hpp"
#include <memory>
#include <string>

namespace asos {

/**
 * @brief Main entry point for the ASOS Blackboard System.
 * Wraps L3KVG to provide knowledge-aware graph operations.
 */
class Blackboard {
public:
    /**
     * @param db_path Path to the persistent L3KV storage.
     * @param node_id Local node identity.
     */
    Blackboard(const std::string& db_path, uint32_t node_id);
    ~Blackboard();

    // Knowledge Management (CPB)
    /**
     * @brief Commit a Knowledge Atom to the Commonplace Book.
     */
    bool commit_cpb_entry(const CpbEntry& entry);

    /**
     * @brief Reconcile an incoming atom with the local graph.
     */
    bool semantic_merge(const CpbEntry& entry);


    // Task Management (WBS)
    /**
     * @brief Add a task decomposition node to the Work Breakdown Structure.
     */
    bool add_wbs_node(const WbsNode& node);

    /**
     * @brief Apply a binary delta patch to an existing atom.
     */
    bool apply_delta_patch(const L3DeltaPatch& patch);

    /**
     * @brief Commit an Engineering Unit (Work Order) to the system.

     */
    bool commit_engineering_unit(const EngineeringUnit& eu);

    // Identity and Structural Anchors
    bool commit_identity_node(const IdentityNode& identity);
    bool commit_project_node(const ProjectNode& project);


    // Graph Access

    l3kvg::Engine* get_engine() { return engine_.get(); }


private:
    std::unique_ptr<l3kvg::Engine> engine_;
};

} // namespace asos
