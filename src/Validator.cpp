#include "asos/Validator.hpp"
#include "asos/schema.hpp"
#include "engine/store.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include <chrono>

namespace asos {

bool Validator::verify_atom(const std::string& uuid, const std::string& auditor_id) {
    auto* store = blackboard_->get_engine()->get_store();
    std::string key(l3kvg::KeyBuilder::node_key(uuid));
    
    auto buf = store->get(key);
    if (buf.size() == 0) return false;

    CpbEntry entry = CpbEntry::deserialize(buf);
    
    if (!entry.signature) {
        entry.signature = ValidationSignature();
    }
    
    entry.signature->verified = true;
    entry.signature->auditor_id = auditor_id;
    entry.signature->timestamp = std::chrono::system_clock::now().time_since_epoch().count();

    blackboard_->commit_cpb_entry(entry);
    blackboard_->get_engine()->get_store()->wait_all_shards();
    return true;
}


bool Validator::validate_atom(const std::string& uuid, const std::string& auditor_id) {
    auto* store = blackboard_->get_engine()->get_store();
    std::string key(l3kvg::KeyBuilder::node_key(uuid));
    
    auto buf = store->get(key);
    if (buf.size() == 0) return false;

    CpbEntry entry = CpbEntry::deserialize(buf);
    
    if (!entry.signature) {
        entry.signature = ValidationSignature();
    }
    
    entry.signature->validated = true;
    entry.signature->auditor_id = auditor_id;
    entry.signature->timestamp = std::chrono::system_clock::now().time_since_epoch().count();

    blackboard_->commit_cpb_entry(entry);
    return true;
}

bool Validator::try_promote(const std::string& uuid) {
    auto* store = blackboard_->get_engine()->get_store();
    std::string key(l3kvg::KeyBuilder::node_key(uuid));
    
    auto buf = store->get(key);
    if (buf.size() == 0) return false;

    CpbEntry entry = CpbEntry::deserialize(buf);
    
    if (entry.signature && entry.signature->verified && entry.signature->validated) {
        entry.taxonomy.is_principle = true;
        blackboard_->commit_cpb_entry(entry);
        blackboard_->get_engine()->get_store()->wait_all_shards();
        return true;
    }

    
    return false;
}

} // namespace asos
