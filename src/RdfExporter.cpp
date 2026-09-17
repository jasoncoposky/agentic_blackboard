#include <agentic_blackboard/RdfExporter.hpp>
#include <agentic_blackboard/schema.hpp>
#include "engine/store.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/Node.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>
#include <set>
#include <algorithm>
#include <vector>
#include <string>
#include <cmath>
#include <limits>

namespace agentic_blackboard {

namespace {

std::string sanitize_iri(const std::string& str) {
    std::string res;
    res.reserve(str.size() + 8);
    for (char c : str) {
        if (c == ' ') {
            res += "%20";
        } else if (c == '"' || c == '\\' || c == '<' || c == '>' || c == '^' || c == '`' || c == '{' || c == '}' || c == '|') {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            res += buf;
        } else if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) == 127) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            res += buf;
        } else {
            res += c;
        }
    }
    return res;
}

std::string sanitize_str(const std::string& str) {
    std::string res;
    res.reserve(str.size() + 16);
    for (char c : str) {
        if (c == '\\') res += "\\\\";
        else if (c == '"') res += "\\\"";
        else if (c == '\n') res += "\\n";
        else if (c == '\r') res += "\\r";
        else if (c == '\t') res += "\\t";
        else res += c;
    }
    return res;
}

std::string format_quantity(double q) {
    if (std::isnan(q) || std::isinf(q)) {
        return "0";
    }
    if (std::abs(q) <= static_cast<double>(std::numeric_limits<int64_t>::max()) && q == static_cast<int64_t>(q)) {
        return std::to_string(static_cast<int64_t>(q));
    }
    std::ostringstream ss;
    ss << q;
    return ss.str();
}

std::string make_identity_iri(const std::string& id) {
    std::string clean = id;
    if (clean.starts_with("identity:")) clean = clean.substr(9);
    if (clean.starts_with("urn:")) return "<" + sanitize_iri(clean) + ">";
    return "<urn:asos:identity:" + sanitize_iri(clean) + ">";
}

std::string make_project_iri(const std::string& id) {
    std::string clean = id;
    if (clean.starts_with("project:")) clean = clean.substr(8);
    if (clean.starts_with("urn:")) return "<" + sanitize_iri(clean) + ">";
    return "<urn:asos:project:" + sanitize_iri(clean) + ">";
}

std::string make_atom_iri(const std::string& id) {
    std::string clean = id;
    if (clean.starts_with("atom:")) clean = clean.substr(5);
    if (clean.starts_with("urn:")) return "<" + sanitize_iri(clean) + ">";
    return "<urn:asos:atom:" + sanitize_iri(clean) + ">";
}

std::string format_target_uri(const std::string& target) {
    if (target.empty()) return "<urn:asos:atom:unknown>";
    if (target.starts_with("urn:")) return "<" + sanitize_iri(target) + ">";
    if (target.starts_with("identity:")) return make_identity_iri(target.substr(9));
    if (target.starts_with("project:")) return make_project_iri(target.substr(8));
    if (target.starts_with("atom:")) return make_atom_iri(target.substr(5));
    return make_atom_iri(target);
}

std::string predicate_for_relation(const std::string& rel) {
    if (rel == rel::SEE_ALSO || rel == "SEE_ALSO" || rel == "seeAlso" || rel == "rdfs:seeAlso") {
        return "rdfs:seeAlso";
    }
    if (rel == rel::CITES || rel == "CITES" || rel == "cites" || rel == "schema:citation") {
        return "schema:citation";
    }
    if (rel == rel::SUPPORTS || rel == "SUPPORTS" || rel == "supports" || rel == "asos:supports") {
        return "asos:supports";
    }
    if (rel == rel::REFUTES || rel == "REFUTES" || rel == "refutes" || rel == "asos:refutes") {
        return "asos:refutes";
    }
    if (rel == rel::EXTENDS || rel == "EXTENDS" || rel == "extends" || rel == "asos:extends") {
        return "asos:extends";
    }
    if (rel == rel::PAIRS_WITH || rel == "PAIRS_WITH" || rel == "pairsWith" || rel == "asos:pairsWith") {
        return "asos:pairsWith";
    }
    if (rel == rel::VARIATION_OF || rel == "VARIATION_OF" || rel == "variationOf" || rel == "asos:variationOf") {
        return "asos:variationOf";
    }
    if (rel == rel::USES_INGREDIENT || rel == "USES_INGREDIENT" || rel == "usesIngredient" || rel == "asos:usesIngredient") {
        return "asos:usesIngredient";
    }
    if (rel == rel::DEPENDS_ON || rel == "DEPENDS_ON" || rel == "dependsOn") {
        return "asos:dependsOn";
    }
    if (rel == rel::BLOCKS || rel == "BLOCKS" || rel == "blocks") {
        return "asos:blocks";
    }
    if (rel == rel::SUBTASK_OF || rel == "SUBTASK_OF" || rel == "subtaskOf") {
        return "asos:subtaskOf";
    }
    if (rel == rel::VALIDATED_BY || rel == "VALIDATED_BY" || rel == "validatedBy") {
        return "asos:validatedBy";
    }
    if (rel == rel::CONTRIBUTES_TO || rel == "CONTRIBUTES_TO" || rel == "contributesTo") {
        return "asos:contributesTo";
    }
    if (rel == rel::SYNTHESIS_OF || rel == "SYNTHESIS_OF" || rel == "synthesisOf") {
        return "asos:synthesisOf";
    }
    if (rel == rel::QUESTION_RAISED_BY || rel == "QUESTION_RAISED_BY" || rel == "questionRaisedBy") {
        return "asos:questionRaisedBy";
    }
    if (rel == rel::ANALOGY_TO || rel == "ANALOGY_TO" || rel == "analogyTo") {
        return "asos:analogyTo";
    }

    if (rel.starts_with("asos:") || rel.starts_with("rdfs:") || rel.starts_with("schema:") || rel.starts_with("prov:") || rel.starts_with("dc:") || rel.starts_with("geo:")) {
        return rel;
    }
    return "asos:" + rel;
}

} // anonymous namespace

std::string RdfExporter::export_turtle(Blackboard* blackboard, uint32_t principal_id) {
    if (!blackboard) return "";
    auto engine = blackboard->get_engine();
    if (!engine) return "";
    auto store = engine->get_store();
    if (!store) return "";

    std::ostringstream ss;
    ss << "@prefix asos: <http://asos.substrate.ai/schema#> .\n";
    ss << "@prefix rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n";
    ss << "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n";
    ss << "@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .\n";
    ss << "@prefix prov: <http://www.w3.org/ns/prov#> .\n";
    ss << "@prefix schema: <http://schema.org/> .\n";
    ss << "@prefix dc: <http://purl.org/dc/terms/> .\n";
    ss << "@prefix geo: <http://www.w3.org/2003/01/geo/wgs84_pos#> .\n\n";

    std::set<std::string> unique_keys;
    auto all_keys = store->get_prefix_keys_all_shards("n:", "n:", 10000);
    unique_keys.insert(all_keys.begin(), all_keys.end());

    for (const auto& key : unique_keys) {
        if (key.ends_with(":meta")) continue;

        auto buf = store->get(key, principal_id);
        if (buf.size() == 0) continue;

        size_t start_brace = key.find('{');
        size_t end_brace = key.find('}');
        if (start_brace == std::string::npos || end_brace == std::string::npos || end_brace <= start_brace) {
            continue;
        }

        std::string hex_id = key.substr(start_brace + 1, end_brace - start_brace - 1);

        bool has_type = false;
        std::string type = "";
        try {
            if (buf.get_type(0, "header") == lite3cpp::Type::Object) {
                size_t h_idx = buf.get_obj(0, "header");
                if (buf.get_type(h_idx, "type") == lite3cpp::Type::String) {
                    type = std::string(buf.get_str(h_idx, "type"));
                    has_type = true;
                }
            }
        } catch (...) {}

        if (has_type && type == "IDENTITY") {
            IdentityNode id_node = IdentityNode::deserialize(buf);
            std::string id_str = id_node.id.empty() ? hex_id : id_node.id;
            std::string subject_uri = make_identity_iri(id_str);

            std::vector<std::pair<std::string, std::string>> props;
            std::string label = id_node.display_name.empty() ? id_str : id_node.display_name;
            props.push_back({"rdfs:label", "\"" + sanitize_str(label) + "\""});
            props.push_back({"dc:title", "\"" + sanitize_str(label) + "\""});
            if (!id_node.role.empty()) {
                props.push_back({"asos:role", "\"" + sanitize_str(id_node.role) + "\""});
            }

            auto outbound = blackboard->get_outbound_links(id_str, principal_id);
            for (const auto& [dst_uuid, rel_label] : outbound) {
                std::string pred = predicate_for_relation(rel_label);
                std::string target_uri = format_target_uri(dst_uuid);
                props.push_back({pred, target_uri});
            }

            ss << subject_uri << " a asos:Identity";
            for (const auto& [pred, val] : props) {
                ss << " ;\n    " << pred << " " << val;
            }
            ss << " .\n\n";
        } else if (has_type && type == "PROJECT") {
            ProjectNode p_node = ProjectNode::deserialize(buf);
            std::string proj_str = p_node.project_id.empty() ? hex_id : p_node.project_id;
            std::string subject_uri = make_project_iri(proj_str);

            std::vector<std::pair<std::string, std::string>> props;
            if (!p_node.description.empty()) {
                props.push_back({"rdfs:comment", "\"" + sanitize_str(p_node.description) + "\""});
                props.push_back({"dc:description", "\"" + sanitize_str(p_node.description) + "\""});
            }
            if (!p_node.lifecycle_status.empty()) {
                props.push_back({"asos:lifecycleStatus", "\"" + sanitize_str(p_node.lifecycle_status) + "\""});
            }

            auto outbound = blackboard->get_outbound_links(proj_str, principal_id);
            for (const auto& [dst_uuid, rel_label] : outbound) {
                std::string pred = predicate_for_relation(rel_label);
                std::string target_uri = format_target_uri(dst_uuid);
                props.push_back({pred, target_uri});
            }

            ss << subject_uri << " a asos:Project";
            for (const auto& [pred, val] : props) {
                ss << " ;\n    " << pred << " " << val;
            }
            ss << " .\n\n";
        } else {
            // Default: CPB_ENTRY / Knowledge Atom / Note / Recipe
            try {
                CpbEntry entry = CpbEntry::deserialize(buf);
                std::string atom_id = entry.header.uuid.empty() ? hex_id : entry.header.uuid;
                std::string subject_uri = make_atom_iri(atom_id);

                bool is_recipe = (!entry.items.empty() || !entry.steps.empty() ||
                                  entry.taxonomy.knowledge_area == KnowledgeArea::CULINARY_RECIPES);
                bool is_creative_work = (!entry.payload.references.empty() ||
                                         entry.taxonomy.knowledge_area == KnowledgeArea::LITERATURE_READING ||
                                         (!is_recipe && !entry.payload.note_links.empty()));

                std::vector<std::string> types = {"asos:KnowledgeAtom"};
                if (is_recipe) {
                    types.push_back("schema:Recipe");
                }
                if (is_creative_work) {
                    types.push_back("schema:CreativeWork");
                }

                std::string type_str;
                for (size_t i = 0; i < types.size(); ++i) {
                    if (i > 0) type_str += ", ";
                    type_str += types[i];
                }

                std::vector<std::pair<std::string, std::string>> props;

                if (!entry.payload.statement.empty()) {
                    props.push_back({"asos:statement", "\"" + sanitize_str(entry.payload.statement) + "\""});
                    props.push_back({"schema:headline", "\"" + sanitize_str(entry.payload.statement) + "\""});
                    props.push_back({"dc:title", "\"" + sanitize_str(entry.payload.statement) + "\""});
                    if (is_recipe) {
                        props.push_back({"schema:name", "\"" + sanitize_str(entry.payload.statement) + "\""});
                    }
                }

                if (!entry.payload.content.empty()) {
                    props.push_back({"schema:text", "\"" + sanitize_str(entry.payload.content) + "\""});
                    props.push_back({"dc:description", "\"" + sanitize_str(entry.payload.content) + "\""});
                    if (is_recipe) {
                        props.push_back({"schema:description", "\"" + sanitize_str(entry.payload.content) + "\""});
                    }
                }

                if (!entry.header.origin.agent_id.empty()) {
                    std::string agent_iri = make_identity_iri(entry.header.origin.agent_id);
                    props.push_back({"prov:wasGeneratedBy", agent_iri});
                    props.push_back({"dc:creator", agent_iri});
                }
                if (!entry.header.origin.project_id.empty()) {
                    std::string proj_iri = make_project_iri(entry.header.origin.project_id);
                    props.push_back({"asos:belongsTo", proj_iri});
                }

                if (entry.header.timestamp > 0) {
                    props.push_back({"prov:generatedAtTime", std::to_string(entry.header.timestamp)});
                }
                if (entry.header.event_timestamp > 0) {
                    props.push_back({"asos:eventTimestamp", std::to_string(entry.header.event_timestamp)});
                }

                // Citations
                for (const auto& ref : entry.payload.references) {
                    std::ostringstream bnode;
                    bnode << "[\n        a schema:CreativeWork";
                    if (!ref.uuid.empty()) {
                        bnode << " ;\n        schema:identifier \"" << sanitize_str(ref.uuid) << "\"";
                        bnode << " ;\n        schema:sameAs " << format_target_uri(ref.uuid);
                    }
                    if (!ref.title.empty()) {
                        bnode << " ;\n        schema:name \"" << sanitize_str(ref.title) << "\"";
                        bnode << " ;\n        dc:title \"" << sanitize_str(ref.title) << "\"";
                    }
                    if (!ref.page_numbers.empty()) {
                        bnode << " ;\n        schema:pagination \"" << sanitize_str(ref.page_numbers) << "\"";
                    }
                    if (!ref.creator.empty()) {
                        bnode << " ;\n        schema:author \"" << sanitize_str(ref.creator) << "\"";
                        bnode << " ;\n        dc:creator \"" << sanitize_str(ref.creator) << "\"";
                    }
                    if (!ref.tags.empty()) {
                        bnode << " ;\n        schema:keywords ";
                        for (size_t ti = 0; ti < ref.tags.size(); ++ti) {
                            if (ti > 0) bnode << ", ";
                            bnode << "\"" << sanitize_str(ref.tags[ti]) << "\"";
                        }
                    }
                    if (!ref.excerpt.empty()) {
                        bnode << " ;\n        schema:text \"" << sanitize_str(ref.excerpt) << "\"";
                    }
                    bnode << "\n    ]";
                    props.push_back({"schema:citation", bnode.str()});
                }

                // Ingredients/Items
                for (const auto& item : entry.items) {
                    std::string ing_str = item.name;
                    if (item.quantity > 0) {
                        ing_str += " (" + format_quantity(item.quantity);
                        if (!item.unit.empty()) {
                            ing_str += " " + item.unit;
                        }
                        ing_str += ")";
                    }
                    props.push_back({"schema:recipeIngredient", "\"" + sanitize_str(ing_str) + "\""});
                }

                // Steps
                for (const auto& step : entry.steps) {
                    std::string step_text = step.instruction;
                    std::string step_pfx = std::to_string(step.step_number) + ".";
                    if (step.step_number > 0 && !step_text.starts_with(step_pfx)) {
                        step_text = step_pfx + " " + step_text;
                    }
                    props.push_back({"schema:recipeInstructions", "\"" + sanitize_str(step_text) + "\""});
                }

                // Metrics
                for (const auto& metric : entry.metrics) {
                    std::ostringstream bnode;
                    bnode << "[\n        asos:metricKey \"" << sanitize_str(metric.key) << "\" ;\n";
                    bnode << "        asos:metricValue " << format_quantity(metric.value);
                    if (!metric.unit.empty()) {
                        bnode << " ;\n        asos:metricUnit \"" << sanitize_str(metric.unit) << "\"";
                    }
                    bnode << "\n    ]";
                    props.push_back({"asos:metric", bnode.str()});
                }

                // Attributes
                for (const auto& [k, v] : entry.attributes) {
                    if (k == "keywords" || k == "tags") {
                        props.push_back({"schema:keywords", "\"" + sanitize_str(v) + "\""});
                    } else {
                        std::ostringstream bnode;
                        bnode << "[\n        rdfs:label \"" << sanitize_str(k) << "\" ;\n";
                        bnode << "        rdf:value \"" << sanitize_str(v) << "\"\n    ]";
                        props.push_back({"asos:attribute", bnode.str()});
                    }
                }

                // Tags
                if (!entry.taxonomy.tags.empty()) {
                    std::string tag_str;
                    for (size_t ti = 0; ti < entry.taxonomy.tags.size(); ++ti) {
                        if (ti > 0) tag_str += ", ";
                        tag_str += "\"" + sanitize_str(entry.taxonomy.tags[ti]) + "\"";
                    }
                    props.push_back({"schema:keywords", tag_str});
                }

                // Outgoing edges
                auto outbound = blackboard->get_outbound_links(atom_id, principal_id);

                // If dst_uuid in outbound is an unresolved 16-hex hash, resolve via note_links or references
                for (auto& [dst_uuid, rel_label] : outbound) {
                    if (dst_uuid.size() == 16) {
                        for (const auto& nl : entry.payload.note_links) {
                            if (!nl.target_uuid.empty()) {
                                char hbuf[17];
                                std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                                              static_cast<unsigned long long>(engine->get_resolver().parse_uuid(nl.target_uuid)));
                                if (dst_uuid == hbuf) {
                                    dst_uuid = nl.target_uuid;
                                    break;
                                }
                            }
                        }
                        for (const auto& r : entry.payload.references) {
                            if (!r.uuid.empty()) {
                                char hbuf[17];
                                std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                                              static_cast<unsigned long long>(engine->get_resolver().parse_uuid(r.uuid)));
                                if (dst_uuid == hbuf) {
                                    dst_uuid = r.uuid;
                                    break;
                                }
                            }
                        }
                    }
                }

                // Supplement with note_links from payload if not present
                for (const auto& nl : entry.payload.note_links) {
                    if (!nl.target_uuid.empty()) {
                        std::string rel_label = nl.relation.empty() ? rel::SEE_ALSO : nl.relation;
                        bool exists = false;
                        for (const auto& [dst, l] : outbound) {
                            if (dst == nl.target_uuid && l == rel_label) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            outbound.push_back({nl.target_uuid, rel_label});
                        }
                    }
                }

                // Deduplicate outbound links
                std::vector<std::pair<std::string, std::string>> deduped_outbound;
                for (const auto& item : outbound) {
                    if (std::find(deduped_outbound.begin(), deduped_outbound.end(), item) == deduped_outbound.end()) {
                        deduped_outbound.push_back(item);
                    }
                }
                outbound = std::move(deduped_outbound);

                for (const auto& [dst_uuid, rel_label] : outbound) {
                    bool is_inline_ref = false;
                    for (const auto& r : entry.payload.references) {
                        if (!r.uuid.empty() && r.uuid == dst_uuid) {
                            is_inline_ref = true;
                            break;
                        }
                    }
                    if (is_inline_ref && (rel_label == rel::CITES || rel_label == "CITES")) {
                        continue;
                    }

                    std::string pred = predicate_for_relation(rel_label);
                    std::string target_uri = format_target_uri(dst_uuid);
                    props.push_back({pred, target_uri});
                }

                ss << subject_uri << " a " << type_str;
                for (const auto& [pred, val] : props) {
                    ss << " ;\n    " << pred << " " << val;
                }
                ss << " .\n\n";
            } catch (...) {}
        }
    }

    return ss.str();
}

} // namespace agentic_blackboard
