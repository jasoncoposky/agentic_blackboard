#include <agentic_blackboard/GraphTopologyAuditor.hpp>
#include <agentic_blackboard/Blackboard.hpp>
#include <agentic_blackboard/schema.hpp>
#include "engine/store.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/Node.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace agentic_blackboard {

GraphTopologyAuditor::GraphTopologyAuditor(Blackboard* blackboard)
    : blackboard_(blackboard) {}

size_t GraphTopologyAuditor::audit_orphans(Blackboard* blackboard) {
    Blackboard* target_bb = blackboard ? blackboard : blackboard_;
    if (!target_bb) return 0;

    auto* engine = target_bb->get_engine();
    if (!engine) return 0;
    auto* store = engine->get_store();
    if (!store) return 0;
    
    std::cout << "[Librarian] Auditing graph for unanchored nodes..." << std::endl;
    size_t orphan_count = 0;

    // Scan the 'n:' subspace for knowledge atoms
    auto keys = store->get_prefix_keys_all_shards("n:{", "", 1000);
    for (const auto& key : keys) {
        if (key.find(":los:") != std::string::npos) continue;

        std::string_view uuid_view = key;
        if (uuid_view.starts_with("n:{")) {
            uuid_view.remove_prefix(3);
            if (uuid_view.ends_with("}")) uuid_view.remove_suffix(1);
        } else if (uuid_view.starts_with("n:")) {
            uuid_view.remove_prefix(2);
        }
        std::string uuid(uuid_view);

        auto node = engine->get_node(uuid);
        if (!node) continue;

        bool has_author = !node->get_edges(rel::CREATED_BY).empty();
        bool has_project = !node->get_edges(rel::BELONGS_TO).empty();

        if (!has_author || !has_project) {
            std::cout << "[Librarian] ORPHAN DETECTED: " << uuid 
                      << " (Author: " << (has_author ? "OK" : "MISSING") 
                      << " | Project: " << (has_project ? "OK" : "MISSING") << ")" << std::endl;
            orphan_count++;
        }
    }

    if (orphan_count > 0) {
        std::cout << "[Librarian] Orphan Audit Complete. Found " << orphan_count << " orphans." << std::endl;
    } else {
        std::cout << "[Librarian] Orphan Audit Complete. All nodes anchored." << std::endl;
    }

    return orphan_count;
}

} // namespace agentic_blackboard
