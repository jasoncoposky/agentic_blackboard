#include <agentic_blackboard/GraphAnalogyEngine.hpp>
#include <agentic_blackboard/Blackboard.hpp>
#include <agentic_blackboard/schema.hpp>
#include "buffer.hpp"
#include "engine/store.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/Node.hpp"

#include <iostream>
#include <set>
#include <algorithm>
#include <vector>
#include <string>

namespace agentic_blackboard {

GraphAnalogyEngine::GraphAnalogyEngine(Blackboard* blackboard)
    : blackboard_(blackboard) {}

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

size_t GraphAnalogyEngine::perform_analysis(Blackboard* blackboard) {
    Blackboard* target_bb = blackboard ? blackboard : blackboard_;
    if (!target_bb) return 0;

    auto engine = target_bb->get_engine();
    if (!engine) return 0;
    auto store = engine->get_store();
    if (!store) return 0;

    std::cout << "[Librarian] Scanning graph for structural analogies..." << std::endl;

    // 1. Discover all Knowledge Atoms via parallel multi-shard prefix entries
    auto entries = store->get_prefix_entries_all_shards("n:{", "", 1000);
    
    std::vector<CpbEntry> atoms;
    for (const auto& [key, val] : entries) {
        // Skip sidecar history/loser keys if we only want primary analogies
        if (key.find(":los:") != std::string::npos || val.empty()) continue;

        try {
            lite3cpp::Buffer buf(reinterpret_cast<const uint8_t*>(val.data()), val.size());
            atoms.push_back(CpbEntry::deserialize(buf));
        } catch (...) {
            // Skip corrupted or incompatible nodes
        }
    }

    if (atoms.size() < 2) return 0;

    // 2. Perform Pairwise Similarity Analysis
    size_t synapses_created = 0;
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

    return synapses_created;
}

} // namespace agentic_blackboard
