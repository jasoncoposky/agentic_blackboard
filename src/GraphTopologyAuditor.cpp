#include <agentic_blackboard/GraphTopologyAuditor.hpp>
#include <agentic_blackboard/Blackboard.hpp>
#include <agentic_blackboard/schema.hpp>
#include "engine/store.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/Node.hpp"
#include <nlohmann/json.hpp>

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
    auto entries = store->get_prefix_entries_all_shards("n:{", "", 1000);
    for (const auto& [key, val] : entries) {
        if (key.find(":los:") != std::string::npos || val.empty()) continue;

        std::string_view uuid_view = key;
        if (uuid_view.starts_with("n:{")) {
            uuid_view.remove_prefix(3);
            if (uuid_view.ends_with("}")) uuid_view.remove_suffix(1);
        } else if (uuid_view.starts_with("n:")) {
            uuid_view.remove_prefix(2);
        }
        std::string uuid(uuid_view);

        // Verify that the node is actually a CPB atom.
        // Non-CPB nodes (such as PROJECT, IDENTITY, ARTIFACT, COLLECTION, AVU)
        // should NOT be flagged as orphans for lacking CREATED_BY / BELONGS_TO edges.
        bool is_cpb_atom = false;
        std::string atom_uuid = uuid;

        try {
            lite3cpp::Buffer buf(reinterpret_cast<const uint8_t*>(val.data()), val.size());
            if (buf.size() > 0) {
                bool has_non_atom_type = false;
                try {
                    size_t h_idx = buf.get_obj(0, "header");
                    std::string type = std::string(buf.get_str(h_idx, "type"));
                    if (type != "ATOM") {
                        has_non_atom_type = true;
                    }
                } catch (...) {}

                if (!has_non_atom_type) {
                    try {
                        auto entry = CpbEntry::deserialize(buf);
                        if (!entry.header.uuid.empty() || !entry.payload.statement.empty()) {
                            is_cpb_atom = true;
                            if (!entry.header.uuid.empty()) {
                                atom_uuid = entry.header.uuid;
                            }
                        }
                    } catch (...) {}
                }
            }
        } catch (...) {}

        if (!is_cpb_atom) {
            try {
                auto j = nlohmann::json::parse(val);
                if (j.is_object()) {
                    if (j.contains("header") && j["header"].is_object()) {
                        std::string htype = j["header"].value("type", "");
                        if (htype == "ATOM" || (htype.empty() && (j.contains("statement") || j.contains("payload")))) {
                            is_cpb_atom = true;
                        }
                    } else if (j.value("type", "") == "ATOM" || j.contains("statement")) {
                        is_cpb_atom = true;
                    }
                    if (is_cpb_atom) {
                        if (j.contains("uuid") && j["uuid"].is_string()) {
                            atom_uuid = j["uuid"].get<std::string>();
                        } else if (j.contains("header") && j["header"].contains("uuid") && j["header"]["uuid"].is_string()) {
                            atom_uuid = j["header"]["uuid"].get<std::string>();
                        }
                    }
                }
            } catch (...) {}
        }

        if (!is_cpb_atom) continue;

        auto node = engine->get_node(atom_uuid);
        if (!node) node = engine->get_node(uuid);
        if (!node) continue;

        bool has_author = !node->get_edges(rel::CREATED_BY).empty();
        bool has_project = !node->get_edges(rel::BELONGS_TO).empty();

        if (!has_author || !has_project) {
            std::cout << "[Librarian] ORPHAN DETECTED: " << atom_uuid 
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
