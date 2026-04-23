#pragma once

#include "buffer.hpp"

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <map>

namespace asos {

// Relationship Constants
namespace rel {
    const std::string CREATED_BY = "CREATED_BY";
    const std::string BELONGS_TO = "BELONGS_TO";
    const std::string MAINTAINS = "MAINTAINS";
    const std::string SUPERSEDED_BY = "SUPERSEDED_BY";
    const std::string CPB_SIMILARITY = "CPB_SIMILARITY";
    const std::string RELATED_TO = "RELATED_TO";
    const std::string REPRESENTED_BY = "REPRESENTED_BY"; // Atom -> Widget
    const std::string ANCHORED_TO = "ANCHORED_TO";      // Widget -> SpatialAnchor
}


/**
 * @brief SWEBOK Knowledge Areas (1-15)
 */
enum class KnowledgeArea : uint8_t {
    REQUIREMENTS = 1,
    DESIGN = 2,
    CONSTRUCTION = 3,
    TESTING = 4,
    MAINTENANCE = 5,
    CONFIG_MANAGEMENT = 6,
    ENGINEERING_MANAGEMENT = 7,
    ENGINEERING_PROCESS = 8,
    ENGINEERING_MODELS = 9,
    QUALITY = 10,
    PROFESSIONAL_PRACTICE = 11,
    ECONOMICS = 12,
    COMPUTING_FOUNDATIONS = 13,
    MATHEMATICAL_FOUNDATIONS = 14,
    ENGINEERING_FOUNDATIONS = 15,
    UNKNOWN = 0
};

/**
 * @brief Identity Node (Anchor)
 */
struct IdentityNode {
    std::string id;
    std::string display_name;
    std::string role;
    std::string public_key;
    std::string content;

    void serialize(lite3cpp::Buffer& buf) const {
        buf.init_object();
        size_t h_idx = buf.set_obj(0, "header");
        buf.set_str(h_idx, "type", "IDENTITY");
        buf.set_str(0, "id", id);
        buf.set_str(0, "display_name", display_name);
        buf.set_str(0, "role", role);
        buf.set_str(0, "public_key", public_key);
        buf.set_str(0, "content", content);
    }

    static IdentityNode deserialize(const lite3cpp::Buffer& buf) {
        IdentityNode in;
        in.id = buf.get_str(0, "id");
        in.display_name = buf.get_str(0, "display_name");
        in.role = buf.get_str(0, "role");
        in.public_key = buf.get_str(0, "public_key");
        in.content = buf.get_str(0, "content");
        return in;
    }
};

/**
 * @brief Project Node (Structural Anchor)
 */
struct ProjectNode {
    std::string project_id;
    std::string description;
    std::string lifecycle_status; // ACTIVE|ARCHIVED|STUB
    std::string content;

    void serialize(lite3cpp::Buffer& buf) const {
        buf.init_object();
        size_t h_idx = buf.set_obj(0, "header");
        buf.set_str(h_idx, "type", "PROJECT");
        buf.set_str(0, "project_id", project_id);
        buf.set_str(0, "description", description);
        buf.set_str(0, "lifecycle_status", lifecycle_status);
        buf.set_str(0, "content", content);
    }

    static ProjectNode deserialize(const lite3cpp::Buffer& buf) {
        ProjectNode pn;
        pn.project_id = buf.get_str(0, "project_id");
        pn.description = buf.get_str(0, "description");
        pn.lifecycle_status = buf.get_str(0, "lifecycle_status");
        pn.content = buf.get_str(0, "content");
        return pn;
    }
};

/**
 * @brief Swarm Health Summary (SRE Telemetry)
 */
struct SwarmHealthSummary {
    std::map<std::string, std::string> cluster_status; // location -> status
    
    struct Metrics {
        double knowledge_velocity;
        int64_t sync_latency_ms;
        double toil_ratio;
    } metrics;

    void serialize(lite3cpp::Buffer& buf) const {
        buf.init_object();
        size_t c_idx = buf.set_obj(0, "cluster_status");
        for (const auto& [loc, stat] : cluster_status) {
            buf.set_str(c_idx, loc, stat);
        }

        size_t m_idx = buf.set_obj(0, "metrics");
        buf.set_f64(m_idx, "knowledge_velocity", metrics.knowledge_velocity);
        buf.set_i64(m_idx, "sync_latency_ms", metrics.sync_latency_ms);
        buf.set_f64(m_idx, "toil_ratio", metrics.toil_ratio);
    }

    static SwarmHealthSummary deserialize(const lite3cpp::Buffer& buf) {
        SwarmHealthSummary sh;
        try {
            size_t c_idx = buf.get_obj(0, "cluster_status");
            size_t m_idx = buf.get_obj(0, "metrics");
            sh.metrics.knowledge_velocity = buf.get_f64(m_idx, "knowledge_velocity");
            sh.metrics.sync_latency_ms = buf.get_i64(m_idx, "sync_latency_ms");
            sh.metrics.toil_ratio = buf.get_f64(m_idx, "toil_ratio");
        } catch (...) {}
        return sh;
    }
};

/**
 * @brief Binary Delta Patch (L3_DELTA_PATCH)
 */
struct L3DeltaPatch {
    struct Header {
        std::string type = "L3_DELTA_PATCH";
        std::string base_uuid;
        int64_t base_timestamp;
    } header;

    struct Patch {
        int64_t offset;
        std::vector<uint8_t> binary_delta;
        uint64_t checksum; // XXH3_64bits
    } patch;

    void serialize(lite3cpp::Buffer& buf) const {
        buf.init_object();
        size_t h_idx = buf.set_obj(0, "header");
        buf.set_str(h_idx, "type", header.type);
        buf.set_str(h_idx, "base_uuid", header.base_uuid);
        buf.set_i64(h_idx, "base_timestamp", header.base_timestamp);

        size_t p_idx = buf.set_obj(0, "patch");
        buf.set_i64(p_idx, "offset", patch.offset);
        buf.set_i64(p_idx, "checksum", static_cast<int64_t>(patch.checksum));
        
        std::string delta_str(reinterpret_cast<const char*>(patch.binary_delta.data()), patch.binary_delta.size());
        buf.set_str(p_idx, "binary_delta", delta_str);
    }

    static L3DeltaPatch deserialize(const lite3cpp::Buffer& buf) {
        L3DeltaPatch p;
        size_t h_idx = buf.get_obj(0, "header");
        p.header.base_uuid = buf.get_str(h_idx, "base_uuid");
        p.header.base_timestamp = buf.get_i64(h_idx, "base_timestamp");

        size_t p_idx = buf.get_obj(0, "patch");
        p.patch.offset = buf.get_i64(p_idx, "offset");
        p.patch.checksum = static_cast<uint64_t>(buf.get_i64(p_idx, "checksum"));
        
        std::string_view delta_view = buf.get_str(p_idx, "binary_delta");
        p.patch.binary_delta.assign(delta_view.begin(), delta_view.end());
        return p;
    }
};

/**
 * @brief Validation Gate Signature
 */
struct ValidationSignature {
    bool verified = false;   // SWEBOK audit passed
    bool validated = false;  // Functional/Stress test passed
    std::string auditor_id;
    int64_t timestamp;

    void serialize(lite3cpp::Buffer& buf, size_t parent) const {
        size_t sig_idx = buf.set_obj(parent, "validation_signature");
        buf.set_bool(sig_idx, "verified", verified);
        buf.set_bool(sig_idx, "validated", validated);
        buf.set_str(sig_idx, "auditor_id", auditor_id);
        buf.set_i64(sig_idx, "timestamp", timestamp);
    }

    static ValidationSignature deserialize(const lite3cpp::Buffer& buf, size_t parent) {
        ValidationSignature sig;
        try {
            size_t sig_idx = buf.get_obj(parent, "validation_signature");
            sig.verified = buf.get_bool(sig_idx, "verified");
            sig.validated = buf.get_bool(sig_idx, "validated");
            sig.auditor_id = buf.get_str(sig_idx, "auditor_id");
            sig.timestamp = buf.get_i64(sig_idx, "timestamp");
        } catch (...) {
            return {false, false, "", 0};
        }
        return sig;
    }
};

/**
 * @brief Universal Atom Schema (CPB_ENTRY)
 */
struct CpbEntry {
    struct Header {
        std::string uuid;
        struct Origin {
            std::string agent_id;
            std::string project_id;
        } origin;
        int64_t timestamp;
    } header;

    struct Taxonomy {
        KnowledgeArea knowledge_area;
        std::vector<std::string> tags;
        int64_t applicability; // 0-100
        bool uncertainty = false;
        bool is_principle = false; // Promoted state
    } taxonomy;

    struct Payload {
        std::string content_type;
        std::string statement;
        std::string content;
        std::vector<std::string> artifact_refs;
    } payload;

    std::optional<ValidationSignature> signature;

    void serialize(lite3cpp::Buffer& buf) const {
        buf.init_object();
        
        size_t h_idx = buf.set_obj(0, "header");
        buf.set_str(h_idx, "uuid", header.uuid);
        buf.set_i64(h_idx, "timestamp", header.timestamp);
        size_t o_idx = buf.set_obj(h_idx, "origin");
        buf.set_str(o_idx, "agent_id", header.origin.agent_id);
        buf.set_str(o_idx, "project_id", header.origin.project_id);

        size_t t_idx = buf.set_obj(0, "taxonomy");
        buf.set_i64(t_idx, "knowledge_area", static_cast<int64_t>(taxonomy.knowledge_area));
        buf.set_i64(t_idx, "applicability", taxonomy.applicability);
        buf.set_bool(t_idx, "uncertainty", taxonomy.uncertainty);
        buf.set_bool(t_idx, "is_principle", taxonomy.is_principle);
        size_t tags_idx = buf.set_arr(t_idx, "tags");
        for (const auto& tag : taxonomy.tags) buf.arr_append_str(tags_idx, tag);

        size_t p_idx = buf.set_obj(0, "payload");
        buf.set_str(p_idx, "content_type", payload.content_type);
        buf.set_str(p_idx, "statement", payload.statement);
        buf.set_str(p_idx, "content", payload.content);
        size_t refs_idx = buf.set_arr(p_idx, "artifact_refs");
        for (const auto& ref : payload.artifact_refs) buf.arr_append_str(refs_idx, ref);

        if (signature) {
            signature->serialize(buf, 0);
        }
    }

    static CpbEntry deserialize(const lite3cpp::Buffer& buf) {
        CpbEntry entry;
        size_t h_idx = buf.get_obj(0, "header");
        entry.header.uuid = buf.get_str(h_idx, "uuid");
        entry.header.timestamp = buf.get_i64(h_idx, "timestamp");
        size_t o_idx = buf.get_obj(h_idx, "origin");
        entry.header.origin.agent_id = buf.get_str(o_idx, "agent_id");
        entry.header.origin.project_id = buf.get_str(o_idx, "project_id");

        size_t t_idx = buf.get_obj(0, "taxonomy");
        entry.taxonomy.knowledge_area = static_cast<KnowledgeArea>(buf.get_i64(t_idx, "knowledge_area"));
        entry.taxonomy.applicability = buf.get_i64(t_idx, "applicability");
        entry.taxonomy.uncertainty = buf.get_bool(t_idx, "uncertainty");
        entry.taxonomy.is_principle = buf.get_bool(t_idx, "is_principle");
        
        size_t tags_idx = buf.get_arr(t_idx, "tags");
        lite3cpp::NodeView tags_nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data() + tags_idx));
        for (uint32_t i = 0; i < tags_nv.size(); ++i) {
            entry.taxonomy.tags.push_back(std::string(buf.arr_get_str(tags_idx, i)));
        }

        size_t p_idx = buf.get_obj(0, "payload");
        entry.payload.content_type = buf.get_str(p_idx, "content_type");
        entry.payload.statement = buf.get_str(p_idx, "statement");
        entry.payload.content = buf.get_str(p_idx, "content");
        
        size_t refs_idx = buf.get_arr(p_idx, "artifact_refs");
        lite3cpp::NodeView refs_nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data() + refs_idx));
        for (uint32_t i = 0; i < refs_nv.size(); ++i) {
            entry.payload.artifact_refs.push_back(std::string(buf.arr_get_str(refs_idx, i)));
        }

        try {
            if (buf.get_type(0, "validation_signature") == lite3cpp::Type::Object) {
                entry.signature = ValidationSignature::deserialize(buf, 0);
            }
        } catch (...) {}
        
        return entry;
    }

    bool better_than(const CpbEntry& other) const {
        if (taxonomy.is_principle != other.taxonomy.is_principle) {
            return taxonomy.is_principle;
        }
        if (taxonomy.applicability != other.taxonomy.applicability) {
            return taxonomy.applicability > other.taxonomy.applicability;
        }
        return header.timestamp > other.header.timestamp;
    }
};

/**
 * @brief Engineering Unit (EU) - Work Order Contract
 */
struct EngineeringUnit {
    struct Header {
        std::string type = "EU";
        std::string uuid;
        int64_t timestamp;
    } header;

    std::string functional_spec;
    std::string test_plan;
    double error_budget_impact; // SRE Impact

    enum class Status : uint8_t {
        PROPOSED = 0,
        IN_PROGRESS = 1,
        COMPLETED = 2,
        VALIDATED = 3
    } status = Status::PROPOSED;

    void serialize(lite3cpp::Buffer& buf) const {
        buf.init_object();
        size_t h_idx = buf.set_obj(0, "header");
        buf.set_str(h_idx, "type", header.type);
        buf.set_str(h_idx, "uuid", header.uuid);
        buf.set_i64(h_idx, "timestamp", header.timestamp);
        
        buf.set_str(0, "functional_spec", functional_spec);
        buf.set_str(0, "test_plan", test_plan);
        buf.set_f64(0, "error_budget_impact", error_budget_impact);
        buf.set_i64(0, "status", static_cast<int64_t>(status));
    }

    static EngineeringUnit deserialize(const lite3cpp::Buffer& buf) {
        EngineeringUnit eu;
        size_t h_idx = buf.get_obj(0, "header");
        eu.header.uuid = buf.get_str(h_idx, "uuid");
        eu.header.timestamp = buf.get_i64(h_idx, "timestamp");
        
        eu.functional_spec = buf.get_str(0, "functional_spec");
        eu.test_plan = buf.get_str(0, "test_plan");
        eu.error_budget_impact = buf.get_f64(0, "error_budget_impact");
        eu.status = static_cast<Status>(buf.get_i64(0, "status"));
        return eu;
    }
};

/**
 * @brief WBS Node (Task Decomposition)
 */
struct WbsNode {
    struct Header {
        std::string type = "WBS_NODE";
        std::string id;
    } header;

    std::string task;
    std::string content;
    
    struct PdmLogic {
        std::vector<std::string> dependencies;
        std::string type = "FS"; // Finish-to-Start
    } pdm_logic;

    std::vector<std::string> cpb_alignment; // atom_ids

    void serialize(lite3cpp::Buffer& buf) const {
        buf.init_object();
        size_t h_idx = buf.set_obj(0, "header");
        buf.set_str(h_idx, "type", header.type);
        buf.set_str(h_idx, "id", header.id);
        
        buf.set_str(0, "task", task);
        buf.set_str(0, "content", content);
        
        size_t pdm_idx = buf.set_obj(0, "pdm_logic");
        buf.set_str(pdm_idx, "type", pdm_logic.type);
        size_t dep_idx = buf.set_arr(pdm_idx, "dependencies");
        for (const auto& dep : pdm_logic.dependencies) buf.arr_append_str(dep_idx, dep);

        size_t align_idx = buf.set_arr(0, "cpb_alignment");
        for (const auto& atom : cpb_alignment) buf.arr_append_str(align_idx, atom);
    }
};

/**
 * @brief Node Heartbeat
 */
struct NodeHeartbeat {
    std::string location_id;
    std::string connectivity_state; // CONNECTED|DEGRADED|ISOLATED
    std::vector<std::string> local_wbs_claims;
    int64_t sync_epoch;

    void serialize(lite3cpp::Buffer& buf) const {
        buf.init_object();
        buf.set_str(0, "location_id", location_id);
        buf.set_str(0, "connectivity_state", connectivity_state);
        buf.set_i64(0, "sync_epoch", sync_epoch);
        size_t claim_idx = buf.set_arr(0, "local_wbs_claims");
        for (const auto& claim : local_wbs_claims) buf.arr_append_str(claim_idx, claim);
    }

    static NodeHeartbeat deserialize(const lite3cpp::Buffer& buf) {
        NodeHeartbeat hb;
        hb.location_id = buf.get_str(0, "location_id");
        hb.connectivity_state = buf.get_str(0, "connectivity_state");
        hb.sync_epoch = buf.get_i64(0, "sync_epoch");
        
        size_t claim_idx = buf.get_arr(0, "local_wbs_claims");
        lite3cpp::NodeView nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data() + claim_idx));
        for (uint32_t i = 0; i < nv.size(); ++i) {
            hb.local_wbs_claims.push_back(std::string(buf.arr_get_str(claim_idx, i)));
        }
        return hb;
    }
};

/**
 * @brief Spatial Anchor Node (Nucleus Integration)
 */
struct SpatialAnchorNode {
    std::string anchor_id;      // e.g. "marker_42" or "fiducial_A"
    std::string device_id;      // Device that owns this sensing context
    double x, y, z;             // Millimeters (Metric Sovereignty)
    double rotation[3];         // Euler angles
    int64_t last_seen_ns;

    void serialize(lite3cpp::Buffer& buf) const {
        buf.init_object();
        size_t h_idx = buf.set_obj(0, "header");
        buf.set_str(h_idx, "type", "SPATIAL_ANCHOR");
        buf.set_str(0, "anchor_id", anchor_id);
        buf.set_str(0, "device_id", device_id);
        buf.set_f64(0, "x", x);
        buf.set_f64(0, "y", y);
        buf.set_f64(0, "z", z);
        size_t r_idx = buf.set_arr(0, "rotation");
        buf.arr_append_f64(r_idx, rotation[0]);
        buf.arr_append_f64(r_idx, rotation[1]);
        buf.arr_append_f64(r_idx, rotation[2]);
        buf.set_i64(0, "last_seen_ns", last_seen_ns);
    }

    static SpatialAnchorNode deserialize(const lite3cpp::Buffer& buf) {
        SpatialAnchorNode sa;
        sa.anchor_id = buf.get_str(0, "anchor_id");
        sa.device_id = buf.get_str(0, "device_id");
        sa.x = buf.get_f64(0, "x");
        sa.y = buf.get_f64(0, "y");
        sa.z = buf.get_f64(0, "z");
        size_t r_idx = buf.get_arr(0, "rotation");
        sa.rotation[0] = buf.arr_get_f64(r_idx, 0);
        sa.rotation[1] = buf.arr_get_f64(r_idx, 1);
        sa.rotation[2] = buf.arr_get_f64(r_idx, 2);
        sa.last_seen_ns = buf.get_i64(0, "last_seen_ns");
        return sa;
    }
};

/**
 * @brief Nucleus Widget Node (A2UI Representation)
 */
struct NucleusWidgetNode {
    std::string widget_id;      // UUID linked to SHM Fragment if active
    uint32_t fragment_id;       // index 0-127 in SHM Blackboard
    uint32_t behavior;          // WidgetBehavior enum
    uint32_t status_flags;      // STATE_ENABLED, STATE_HOVER, etc.
    std::string label;
    float utility_score;

    void serialize(lite3cpp::Buffer& buf) const {
        buf.init_object();
        size_t h_idx = buf.set_obj(0, "header");
        buf.set_str(h_idx, "type", "NUCLEUS_WIDGET");
        buf.set_str(0, "widget_id", widget_id);
        buf.set_i64(0, "fragment_id", fragment_id);
        buf.set_i64(0, "behavior", behavior);
        buf.set_i64(0, "status_flags", status_flags);
        buf.set_str(0, "label", label);
        buf.set_f64(0, "utility_score", utility_score);
    }

    static NucleusWidgetNode deserialize(const lite3cpp::Buffer& buf) {
        NucleusWidgetNode nw;
        nw.widget_id = buf.get_str(0, "widget_id");
        nw.fragment_id = static_cast<uint32_t>(buf.get_i64(0, "fragment_id"));
        nw.behavior = static_cast<uint32_t>(buf.get_i64(0, "behavior"));
        nw.status_flags = static_cast<uint32_t>(buf.get_i64(0, "status_flags"));
        nw.label = buf.get_str(0, "label");
        nw.utility_score = static_cast<float>(buf.get_f64(0, "utility_score"));
        return nw;
    }
};

} // namespace asos
