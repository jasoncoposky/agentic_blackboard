#include "asos/ApiServer.hpp"
#include "asos/Orchestrator.hpp"
#include "httplib.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <vector>
#include <string>
#include "asos/schema.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "engine/store.hpp"



using json = nlohmann::json;

namespace asos {

ApiServer::~ApiServer() {
    stop();
}

void ApiServer::start(Blackboard* blackboard, int port) {
    blackboard_ = blackboard;
    port_ = port;
    running_ = true;
    thread_ = std::thread(&ApiServer::listen_loop, this);
}

void ApiServer::stop() {
    running_ = false;
    // In a real implementation, we'd trigger a dummy request to break the accept()
    // For this prototype, we'll assume the thread terminates on its next check.
    if (thread_.joinable())
        thread_.join();
}

void ApiServer::listen_loop() {
    httplib::Server svr;

    // Enable CORS for Dashboard
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"}
    });

    svr.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // 1. Schema Discovery Endpoint

    svr.Get("/api/v1/schema", [this](const httplib::Request&, httplib::Response& res) {
        std::cout << "[API] GET /api/v1/schema" << std::endl;
        json schema = {

            {"system", "ASOS v0.4-α"},
            {"types", {
                {"IDENTITY", {{"fields", {"id", "display_name", "role", "public_key", "content"}}}},
                {"PROJECT", {{"fields", {"project_id", "description", "lifecycle_status", "content"}}}},
                {"CPB_ENTRY", {{"fields", {"uuid", "agent_id", "project_id", "statement", "content", "ka", "applicability"}}}}
            }},
            {"relationships", {rel::CREATED_BY, rel::BELONGS_TO, rel::MAINTAINS, rel::CPB_SIMILARITY, rel::RELATED_TO}}
        };

        res.set_content(schema.dump(2), "application/json");
    });

    // 2. Atomic Graph Bundle Commit (Orphan Prevention)
    // Expects JSON: { "atoms": [...], "project_id": "...", "agent_id": "..." }
    svr.Post("/api/v1/graph/bundle", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            std::string project_id = j.value("project_id", "");
            std::string agent_id = j.value("agent_id", "");

            if (project_id.empty() || agent_id.empty()) {
                res.status = 400;
                res.set_content("Error: Missing mandatory Project or Identity anchor.", "text/plain");
                return;
            }

            int count = 0;
            for (auto& item : j["atoms"]) {
                CpbEntry e;
                // Support both flat and nested header structures
                if (item.contains("header")) {
                    e.header.uuid = item["header"].value("uuid", "");
                } else {
                    e.header.uuid = item.value("uuid", "");
                }

                e.header.origin.agent_id = agent_id;
                e.header.origin.project_id = project_id;

                if (item.contains("payload")) {
                    e.payload.statement = item["payload"].value("statement", "");
                    e.payload.content = item["payload"].value("content", "");
                } else {
                    e.payload.statement = item.value("statement", "");
                    e.payload.content = item.value("content", "");
                }

                if (item.contains("taxonomy")) {
                    e.taxonomy.knowledge_area = static_cast<KnowledgeArea>(item["taxonomy"].value("ka", 0));
                    e.taxonomy.applicability = item["taxonomy"].value("applicability", 50);
                    if (item["taxonomy"].contains("tags")) {
                        for (auto& tag : item["taxonomy"]["tags"]) e.taxonomy.tags.push_back(tag.get<std::string>());
                    }
                } else {
                    e.taxonomy.knowledge_area = static_cast<KnowledgeArea>(item.value("ka", 0));
                    e.taxonomy.applicability = item.value("applicability", 50);
                    if (item.contains("tags")) {
                        for (auto& tag : item["tags"]) e.taxonomy.tags.push_back(tag.get<std::string>());
                    }
                }

                
                std::cout << "[API] Processing Atom Statement: " << e.payload.statement << std::endl;
                if (blackboard_->commit_cpb_entry(e)) {
                    count++;
                }
            }
            
            res.set_content("Successfully committed " + std::to_string(count) + " anchored nodes.", "text/plain");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 3. Robust Querying API
    // Expects Query DSL: { "match": "...", "where_eq": {"key": "val"} }
    svr.Post("/api/v1/query", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            auto engine = blackboard_->get_engine();
            auto q = engine->query();

            std::string alias = "n";
            if (j.contains("match")) {
                alias = j["match"];
                q.match(alias);
            }

            if (j.contains("where_eq")) {
                for (auto& [key, val] : j["where_eq"].items()) {
                    q.where_eq(alias, key, val.get<std::string>());
                }
            }

            auto results = q.execute();
            json response = json::array();
            for (auto& row : results) {
                response.push_back(row.fields);
            }
            res.set_content(response.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 2b. Manual Synapse Creation (Link)
    // { "source": "...", "target": "...", "label": "...", "weight": 1.0 }
    svr.Post("/api/v1/link", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            std::string src = j.at("source");
            std::string dst = j.at("target");
            std::string label = j.at("label");
            double weight = j.value("weight", 1.0);

            auto engine = blackboard_->get_engine();
            engine->add_edge(src, label, weight, dst);

            res.set_content("{\"status\":\"SYNCED\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(e.what(), "text/plain");
        }
    });
 
 
    // 2c. Atom Promotion (Move to PRINCIPLE status)
    svr.Post("/api/v1/cpb/promote", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            std::string uuid = j.at("uuid");
            
            auto engine = blackboard_->get_engine();
            auto store = engine->get_store();
            
            auto buf = store->get("n:" + uuid);
            if (buf.size() == 0) {
                res.status = 404;
                res.set_content("Atom not found", "text/plain");
                return;
            }
            
            CpbEntry e = CpbEntry::deserialize(buf);
            e.taxonomy.is_principle = true; // Promoted state
            e.taxonomy.uncertainty = false; // Promotion implies validation
            
            if (blackboard_->commit_cpb_entry(e)) {
                res.set_content("{\"status\":\"PROMOTED\"}", "application/json");
            } else {
                res.status = 500;
                res.set_content("Failed to update status", "text/plain");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 5. Nucleus Integration Endpoints
    svr.Post("/api/v1/nucleus/materialize", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            std::string atom_id = j.at("atom_id");
            std::string behavior = j.at("behavior");
            std::string label = j.value("label", "");

            // Build Nucleus MCP Request
            json nucleus_req = {
                {"method", "spawn_fragment"},
                {"params", {
                    {"atom_id", atom_id},
                    {"behavior", behavior},
                    {"label", label}
                }}
            };

            asos::Orchestrator::instance().dispatch_nucleus_command(nucleus_req.dump());
            res.set_content("{\"status\":\"DISPATCHED\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(e.what(), "text/plain");
        }
    });

    svr.Post("/api/v1/nucleus/bind", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            std::string marker_id = j.at("marker_id");
            std::string atom_id = j.at("atom_id");

            json nucleus_req = {
                {"method", "kv_set"},
                {"params", {
                    {"key", "anchor:" + marker_id},
                    {"value", atom_id}
                }}
            };

            asos::Orchestrator::instance().dispatch_nucleus_command(nucleus_req.dump());
            res.set_content("{\"status\":\"BOUND\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(e.what(), "text/plain");
        }
    });


    // 4. Graph Topology Snapshot
    svr.Get("/api/v1/graph/snapshot", [this](const httplib::Request&, httplib::Response& res) {
        std::cout << "[API] GET /api/v1/graph/snapshot" << std::endl;
        try {
            auto engine = blackboard_->get_engine();
            auto store = engine->get_store();
            
            json response;
            response["nodes"] = json::array();
            response["edges"] = json::array();

            // 1. Fetch ALL Nodes (Atoms, Identities, Projects)
            std::set<std::string> unique_keys;
            auto keys1 = store->get_prefix_keys_all_shards("n:", "n:", 1000);
            auto keys2 = store->get_prefix_keys_all_shards("n:{", "n:{", 1000);
            unique_keys.insert(keys1.begin(), keys1.end());
            unique_keys.insert(keys2.begin(), keys2.end());

            std::cout << "[API] Snapshot Scan: Found " << unique_keys.size() << " potential node keys." << std::endl;

            std::unordered_set<std::string> materialized_ids;

            for(const auto& key : unique_keys) {
                auto buf = store->get(key);
                if (buf.size() > 0) {
                    try {
                        size_t h_idx = buf.get_obj(0, "header");
                        bool has_type = false;
                        std::string type = "ATOM";

                        try {
                            type = std::string(buf.get_str(h_idx, "type"));
                            has_type = true;
                        } catch (...) {}

                        if (has_type && type == "IDENTITY") {
                            auto iden = IdentityNode::deserialize(buf);
                            response["nodes"].push_back({
                                {"id", iden.id},
                                {"type", "IDENTITY"},
                                {"label", iden.display_name},
                                {"role", iden.role},
                                {"content", iden.content}
                            });
                            materialized_ids.insert(iden.id);
                            std::cout << "[API] Materialized IDENTITY: " << iden.id << std::endl;
                        } else if (has_type && type == "PROJECT") {
                            auto proj = ProjectNode::deserialize(buf);
                            response["nodes"].push_back({
                                {"id", proj.project_id},
                                {"type", "PROJECT"},
                                {"label", proj.project_id},
                                {"status", proj.lifecycle_status},
                                {"content", proj.content}
                            });
                            materialized_ids.insert(proj.project_id);
                            std::cout << "[API] Materialized PROJECT: " << proj.project_id << std::endl;
                        } else {
                            // Default: Knowledge Atom
                            auto entry = CpbEntry::deserialize(buf);
                            response["nodes"].push_back({
                                {"id", entry.header.uuid},
                                {"type", "ATOM"},
                                {"ka", static_cast<int>(entry.taxonomy.knowledge_area)},
                                {"statement", entry.payload.statement},
                                {"content", entry.payload.content},
                                {"tags", entry.taxonomy.tags},
                                {"project", entry.header.origin.project_id},
                                {"author", entry.header.origin.agent_id},
                                {"status", entry.taxonomy.is_principle ? "PRINCIPLE" : (entry.taxonomy.uncertainty ? "UNCERTAIN" : "VALIDATED")},
                                {"label", entry.payload.statement.substr(0, 30) + (entry.payload.statement.size() > 30 ? "..." : "")}
                            });
                            materialized_ids.insert(entry.header.uuid);
                        }
                    } catch (...) {}
                }
            }


            // 2. Fetch Edges
            auto edge_keys = store->get_prefix_keys_all_shards("e:out:{", "e:out:{", 2000);
            std::cout << "[API] Snapshot Scan: Found " << edge_keys.size() << " potential synapses." << std::endl;

            for(const auto& key : edge_keys) {
                // Key format: e:out:{src}:{label}:{weight}:{dst}
                try {
                    size_t src_start = key.find("{") + 1;
                    size_t src_end = key.find("}", src_start);
                    std::string src = key.substr(src_start, src_end - src_start);

                    size_t label_start = src_end + 2;
                    size_t label_end = key.find(":", label_start);
                    std::string label = key.substr(label_start, label_end - label_start);

                    size_t weight_start = label_end + 1;
                    size_t weight_end = key.find(":", weight_start);
                    double weight = std::stod(key.substr(weight_start, weight_end - weight_start));

                    // Destination is everything after the weight_end + 1 (the colon)
                    std::string dst = key.substr(weight_end + 1);

                    // Fallback: If destination node is not in materialized_ids, try to fetch it directly
                    if (materialized_ids.find(dst) == materialized_ids.end()) {
                        std::string dst_node_key = std::string(l3kvg::KeyBuilder::node_key(dst));
                        auto dst_buf = store->get(dst_node_key);
                        if (dst_buf.size() > 0) {
                            std::cout << "[API] Fallback Retrieval for Target: " << dst << std::endl;
                            try {
                                size_t h_idx = dst_buf.get_obj(0, "header");
                                std::string t = std::string(dst_buf.get_str(h_idx, "type"));
                                if (t == "IDENTITY") {
                                    auto iden = IdentityNode::deserialize(dst_buf);
                                    response["nodes"].push_back({{"id", iden.id}, {"type", "IDENTITY"}, {"label", iden.display_name}, {"role", iden.role}});
                                } else if (t == "PROJECT") {
                                    auto proj = ProjectNode::deserialize(dst_buf);
                                    response["nodes"].push_back({{"id", proj.project_id}, {"type", "PROJECT"}, {"label", proj.project_id}, {"status", proj.lifecycle_status}});
                                }
                                materialized_ids.insert(dst);
                            } catch (...) {}
                        }
                    }

                    response["edges"].push_back({
                        {"source", src},
                        {"target", dst},
                        {"label", label},
                        {"weight", weight}
                    });
                } catch (...) {}
            }


            res.set_content(response.dump(2), "application/json");
        } catch (const std::exception& e) {
            std::cerr << "[API] Error generating graph snapshot: " << e.what() << std::endl;
            json err = {{"error", e.what()}, {"nodes", json::array()}, {"edges", json::array()}};
            res.status = 500;
            res.set_content(err.dump(2), "application/json");
        }
    });

    // 5. Idempotent Node Creation (For MCP)
    svr.Post("/api/v1/graph/node", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            std::string type = j.at("type");
            std::string id = j.at("id");
            json metadata = j.value("metadata", json::object());

            auto engine = blackboard_->get_engine();
            auto store = engine->get_store();
            std::string key = std::string(l3kvg::KeyBuilder::node_key(id));
            auto buf = store->get(key);

            if (buf.size() > 0) {
                res.status = 200;
                res.set_content("{\"status\":\"EXISTS\", \"id\":\"" + id + "\"}", "application/json");
                return;
            }

            bool success = false;
            if (type == "PROJECT") {
                ProjectNode p = {id, metadata.value("description", ""), metadata.value("status", "ACTIVE"), metadata.value("content", "")};
                success = blackboard_->commit_project_node(p);
            } else if (type == "IDENTITY") {
                IdentityNode i = {id, metadata.value("name", id), metadata.value("role", "AGENT"), metadata.value("pubkey", ""), metadata.value("content", "")};
                success = blackboard_->commit_identity_node(i);
            }

            if (success) {
                res.status = 201;
                res.set_content("{\"status\":\"CREATED\", \"id\":\"" + id + "\"}", "application/json");
            } else {
                res.status = 500;
                res.set_content("{\"error\":\"Failed to commit node\"}", "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 6. Get Node Details
    svr.Get(R"(/api/v1/node/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string uuid = req.matches[1];
            auto engine = blackboard_->get_engine();
            auto store = engine->get_store();
            std::string key = std::string(l3kvg::KeyBuilder::node_key(uuid));
            auto buf = store->get(key);

            if (buf.size() == 0) {
                res.status = 404;
                res.set_content("Node not found", "text/plain");
                return;
            }

            res.set_content("{\"id\":\"" + uuid + "\", \"found\":true}", "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 7. SRE Health Metrics
    svr.Get("/api/v1/health", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto engine = blackboard_->get_engine();
            auto store = engine->get_store();
            auto buf = store->get("n:{governance:swarm_health}");
            
            if (buf.size() == 0) {
                res.status = 404;
                res.set_content("{\"error\":\"Health substrate not initialized\"}", "application/json");
                return;
            }

            auto health = SwarmHealthSummary::deserialize(buf);
            json j = {
                {"status", "OPERATIONAL"},
                {"metrics", {
                    {"knowledge_velocity", health.metrics.knowledge_velocity},
                    {"sync_latency_ms", health.metrics.sync_latency_ms},
                    {"toil_ratio", health.metrics.toil_ratio}
                }},
                {"cluster", health.cluster_status}
            };
            res.set_content(j.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    std::cout << "[API] ASOS Server starting on 127.0.0.1:" << port_ << "..." << std::endl;

    if (!svr.listen("127.0.0.1", port_)) {
        std::cerr << "[API] FAILED to start server on port " << port_ << std::endl;
    }
}


} // namespace asos
