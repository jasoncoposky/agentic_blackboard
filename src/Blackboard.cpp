#include "asos/Blackboard.hpp"
#include "asos/Orchestrator.hpp"
#include "asos/DeltaEngine.hpp"
#include "asos/Monitor.hpp"
#include "buffer.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/Node.hpp"
#include "engine/store.hpp"




namespace asos {

Blackboard::Blackboard(const std::string& db_path, uint32_t node_id) {
    engine_ = std::make_unique<l3kvg::Engine>(db_path, node_id);
}

Blackboard::~Blackboard() = default;

bool Blackboard::commit_cpb_entry(const CpbEntry& entry) {
    CpbEntry adjusted = entry;
    std::cout << "[Blackboard] commit_cpb_entry called for statement: " << adjusted.payload.statement << std::endl;

    // ORPHAN PREVENTION: Every atom must be anchored to both an Agent and a Project
    if (adjusted.header.origin.agent_id.empty() || adjusted.header.origin.project_id.empty()) {
        std::cerr << "[Blackboard] Rejecting Atom " << (adjusted.header.uuid.empty() ? "UNNAMED" : adjusted.header.uuid)
                  << ": Missing mandatory anchors (Agent AND Project required)." << std::endl;
        return false;
    }

    // UUID GENERATION: Ensure we have a unique key for the substrate
    if (adjusted.header.uuid.empty()) {
        // Deterministic hash-based ID for idempotency within the same session/payload
        uint32_t h = std::hash<std::string>{}(adjusted.payload.statement);
        char hex[9];
        sprintf(hex, "%08x", h);
        adjusted.header.uuid = "atom-" + std::string(hex);
    }
    
    // SAFE-MODE LOGIC: If isolated, mark as uncertain
    if (Orchestrator::instance().current_state() == Orchestrator::State::ISOLATED) {
        adjusted.taxonomy.uncertainty = true;
    }

    lite3cpp::Buffer buf;
    adjusted.serialize(buf);
    
    // FORCED LOCAL STORAGE: Ensure all atoms are stored on local substrate for shake-down
    std::cout << "[Blackboard] Committing Atom: " << adjusted.header.uuid << " (Project: " << adjusted.header.origin.project_id << ")" << std::endl;
    
    // Use the engine's put_node to ensure proper distributed routing, caching, and indexing
    std::cout << "[Blackboard] Committing Atom: " << adjusted.header.uuid << " (Project: " << adjusted.header.origin.project_id << ")" << std::endl;
    engine_->put_node(adjusted.header.uuid, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));

    
    // IDENTITY-CENTRIC ENFORCEMENT: Ensure Links (No Orphans)
    auto author_id = adjusted.header.origin.agent_id;
    auto project_id = adjusted.header.origin.project_id;

    // 1. Link Author (Identity)
    if (!author_id.empty()) {
        auto author_node = engine_->get_node(author_id);
        if (!author_node->has_attribute("header")) {
            IdentityNode stub = {author_id, "Unknown Agent (" + author_id + ")", "STUB", ""};
            commit_identity_node(stub);
        }
        engine_->add_edge(adjusted.header.uuid, rel::CREATED_BY, 1.0, author_id);
    }

    // 2. Link Project
    if (!project_id.empty()) {
        auto project_node = engine_->get_node(project_id);
        if (!project_node->has_attribute("header")) {
            ProjectNode stub = {project_id, "Auto-created stub for " + project_id, "STUB"};
            commit_project_node(stub);
        }
        engine_->add_edge(adjusted.header.uuid, rel::BELONGS_TO, 1.0, project_id);
    }


    // Telemetry
    Monitor::instance().report_atom_commit();

    // Distributed Mirroring (ZeroMQ)
    Orchestrator::instance().broadcast_atom(adjusted);

    return true;
}



bool Blackboard::semantic_merge(const CpbEntry& incoming) {
    auto* store = engine_->get_store();
    std::string key(l3kvg::KeyBuilder::node_key(incoming.header.uuid));
    
    // Check if we have a local version
    lite3cpp::Buffer local_buf = store->get(key);
    if (local_buf.size() == 0) {
        // No local version, just commit
        Monitor::instance().report_merge_event(false); // Success, no conflict
        return commit_cpb_entry(incoming);
    }


    // Deserialize local version
    CpbEntry local = CpbEntry::deserialize(local_buf);

    if (incoming.better_than(local)) {
        // Incoming WINS. Displace local.
        std::string loser_key = incoming.header.uuid + ":los:" + std::to_string(local.header.timestamp);
        
        // Move local to loser key
        engine_->put_node(loser_key, std::string(reinterpret_cast<const char*>(local_buf.data()), local_buf.size()));
        
        // Register incoming winner
        commit_cpb_entry(incoming);
        
        // Link loser to winner
        engine_->add_edge(loser_key, "SUPERSEDED_BY", 1.0, incoming.header.uuid);
        
        // Telemetry: Merge Conflict (Displacement)
        Monitor::instance().report_merge_event(true);
        return true;
    } else {
        // Local WINS. Store incoming as a loser.
        std::string loser_key = incoming.header.uuid + ":los:" + std::to_string(incoming.header.timestamp);
        
        lite3cpp::Buffer inc_buf;
        incoming.serialize(inc_buf);
        engine_->put_node(loser_key, std::string(reinterpret_cast<const char*>(inc_buf.data()), inc_buf.size()));
        
        // Link incoming loser to local winner
        engine_->add_edge(loser_key, "SUPERSEDED_BY", 1.0, local.header.uuid);
        
        // Telemetry: Merge Conflict (Displacement)
        Monitor::instance().report_merge_event(true);
        return false; // Incoming did not win
    }
}


bool Blackboard::add_wbs_node(const WbsNode& node) {
    lite3cpp::Buffer buf;
    node.serialize(buf);
    
    // Using a separate subspace for WBS nodes
    std::string key = "wbs:" + node.header.id;
    engine_->put_node(key, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
    return true;
}

bool Blackboard::apply_delta_patch(const L3DeltaPatch& patch) {
    auto* store = engine_->get_store();
    std::string key(l3kvg::KeyBuilder::node_key(patch.header.base_uuid));
    
    // Retrieve Base
    lite3cpp::Buffer base_buf = store->get(key);
    if (base_buf.size() == 0) {
        std::cerr << "[Blackboard] Cannot apply delta: base node " << patch.header.base_uuid << " not found" << std::endl;
        return false;
    }

    // Apply Patch
    auto reconstructed = DeltaEngine::apply_xor_patch(base_buf, patch);
    if (!reconstructed) return false;

    // Direct put to store (bypass semantic merge as patches are usually during catchup/sync)
    engine_->put_node(patch.header.base_uuid, std::string(reinterpret_cast<const char*>(reconstructed->data()), reconstructed->size()));
    return true;
}

bool Blackboard::commit_identity_node(const IdentityNode& identity) {
    lite3cpp::Buffer buf;
    identity.serialize(buf);
    engine_->put_node(identity.id, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
    return true;
}

bool Blackboard::commit_project_node(const ProjectNode& project) {
    lite3cpp::Buffer buf;
    project.serialize(buf);
    engine_->put_node(project.project_id, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
    return true;
}

bool Blackboard::commit_engineering_unit(const EngineeringUnit& eu) {

    lite3cpp::Buffer buf;
    eu.serialize(buf);
    
    // EUs are stored in their own space
    std::string key = "eu:" + eu.header.uuid;
    engine_->put_node(key, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
    return true;
}

} // namespace asos
