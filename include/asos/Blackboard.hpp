#pragma once

#include "L3KVG/Engine.hpp"
#include "schema.hpp"
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <shared_mutex>

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

    // Security & Multi-Tenancy Credentials
    void set_auth_mode(const std::string& mode);
    std::string get_auth_mode() const;
    bool register_token(const std::string& token, const std::string& user, const std::string& role);
    bool validate_token(const std::string& token, std::string& out_user, std::string& out_role);
    std::vector<std::pair<std::string, std::string>> get_registered_users() const;

    bool register_user_credentials(const std::string& username, const std::string& public_key);
    uint32_t get_user_uid(const std::string& username) const;

    // Knowledge Management (CPB)
    /**
     * @brief Commit a Knowledge Atom to the Commonplace Book.
     */
    bool commit_cpb_entry(const CpbEntry& entry, uint32_t principal_id = 0);

    /**
     * @brief Reconcile an incoming atom with the local graph.
     */
    bool semantic_merge(const CpbEntry& entry);

    /**
     * @brief Query incoming backlinks for a given note atom.
     */
    std::vector<std::pair<std::string, std::string>> get_backlinks(const std::string& note_uuid, uint32_t principal_id = 0);

    /**
     * @brief Query outgoing links from a given note atom.
     */
    std::vector<std::pair<std::string, std::string>> get_outbound_links(const std::string& note_uuid, uint32_t principal_id = 0);


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
    std::string auth_mode_{"trusted_network"};
    mutable std::shared_mutex auth_mutex_;
    std::vector<std::pair<std::string, std::string>> registered_users_;
};

} // namespace asos
