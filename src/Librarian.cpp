#include "asos/Librarian.hpp"
#include "buffer.hpp"
#include "engine/store.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "asos/schema.hpp"
#include "L3KVG/Node.hpp"


#include <chrono>
#include <iostream>
#include <set>
#include <algorithm>
#include <vector>

namespace asos {

// Helper: Calculate Jaccard Similarity between two sets of tags
static double calculate_jaccard(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.empty() && b.empty()) return 1.0;
    if (a.empty() || b.empty()) return 0.0;

    std::set<std::string> set_a(a.begin(), a.end());
    std::set<std::string> set_b(b.begin(), b.end());

    std::vector<std::string> intersection;
    std::set_intersection(set_a.begin(), set_a.end(),
                          set_b.begin(), set_b.end(),
                          std::back_inserter(intersection));

    std::vector<std::string> union_set;
    std::set_union(set_a.begin(), set_a.end(),
                   set_b.begin(), set_b.end(),
                   std::back_inserter(union_set));

    return static_cast<double>(intersection.size()) / static_cast<double>(union_set.size());
}

Librarian::~Librarian() {
    stop();
}

void Librarian::start(Blackboard* blackboard) {
    blackboard_ = blackboard;
    running_ = true;
    thread_ = std::thread(&Librarian::analysis_loop, this);
}

void Librarian::stop() {
    running_ = false;
    if (thread_.joinable())
        thread_.join();
}

void Librarian::perform_analysis() {
    if (!blackboard_) return;

    auto engine = blackboard_->get_engine();
    auto store = engine->get_store();

    std::cout << "[Librarian] Scanning graph for structural analogies..." << std::endl;

    // 1. Discover all Knowledge Atoms
    // Nodes are stored with "n:{" prefix in KeyBuilder::node_key
    auto keys = store->get_prefix_keys_all_shards("n:{", "", 1000);
    
    std::vector<CpbEntry> atoms;
    for (const auto& key : keys) {
        // Skip sidecar history/loser keys if we only want primary analogies
        if (key.find(":los:") != std::string::npos) continue;

        auto buf = store->get(key);
        if (buf.size() > 0) {
            try {
                atoms.push_back(CpbEntry::deserialize(buf));
            } catch (...) {
                // Skip corrupted or incompatible nodes
            }
        }
    }

    if (atoms.size() < 2) return;

    // 2. Perform Pairwise Similarity Analysis
    int synapses_created = 0;
    for (size_t i = 0; i < atoms.size(); ++i) {
        for (size_t j = i + 1; j < atoms.size(); ++j) {
            const auto& a = atoms[i];
            const auto& b = atoms[j];

            double similarity = calculate_jaccard(a.taxonomy.tags, b.taxonomy.tags);
            
            // Apply boost for same Knowledge Area
            if (a.taxonomy.knowledge_area == b.taxonomy.knowledge_area && a.taxonomy.knowledge_area != KnowledgeArea::UNKNOWN) {
                similarity += 0.4;
            }

            if (similarity > 1.0) similarity = 1.0;

            // 3. Create Synapse (Links) based on confidence tiers
            if (similarity > 0.7) {
                engine->add_edge(a.header.uuid, rel::CPB_SIMILARITY, similarity, b.header.uuid);
                synapses_created++;
            } else if (similarity > 0.3) {
                engine->add_edge(a.header.uuid, rel::RELATED_TO, similarity, b.header.uuid);
                synapses_created++;
            }

        }
    }

    if (synapses_created > 0) {
        std::cout << "[Librarian] Synapse analysis complete. Created " << synapses_created << " edges." << std::endl;
    }
}

void Librarian::audit_orphans() {
    if (!blackboard_) return;
    auto* engine = blackboard_->get_engine();
    auto* store = engine->get_store();
    
    std::cout << "[Librarian] Auditing graph for unanchored nodes..." << std::endl;
    int orphan_count = 0;

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
}


void Librarian::analysis_loop() {
    while (running_) {
        perform_analysis();
        audit_orphans();
        
        // Throttle to avoid high CPU usage in prototype

        for (int i = 0; i < 10 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

} // namespace asos
