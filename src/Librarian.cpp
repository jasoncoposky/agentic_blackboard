#include <agentic_blackboard/Librarian.hpp>
#include "buffer.hpp"
#include "engine/store.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include <agentic_blackboard/schema.hpp>
#include "L3KVG/Node.hpp"


#include <chrono>
#include <iostream>
#include <set>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <cctype>
#include <cstdio>

namespace agentic_blackboard {

Librarian::Librarian(Blackboard* blackboard)
    : running_(false), blackboard_(blackboard) {}

static std::string infer_mime_type(std::string_view filename) {
    auto dot_pos = filename.rfind('.');
    if (dot_pos == std::string_view::npos) {
        return "application/octet-stream";
    }
    std::string ext(filename.substr(dot_pos));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == ".md" || ext == ".markdown") return "text/markdown";
    if (ext == ".json") return "application/json";
    if (ext == ".txt") return "text/plain";
    if (ext == ".yaml" || ext == ".yml") return "application/yaml";
    if (ext == ".csv") return "text/csv";
    if (ext == ".xml") return "application/xml";
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg") return "image/svg+xml";
    return "application/octet-stream";
}

double Librarian::calculate_fair_score(const ArtifactEntry& entry) const {
    double score = 0.0;

    // Findable (max 30): Has non-empty PID (+15), has non-empty Title (+15)
    if (!entry.pid.empty() && entry.pid != "urn:ab:artifact:") score += 15.0;
    if (!entry.title.empty()) score += 15.0;

    // Accessible (max 25): Has non-empty content_hash (+15), has logical path (+10)
    if (!entry.content_hash.empty()) score += 15.0;
    if (!entry.collection_path.empty() && !entry.logical_name.empty()) score += 10.0;

    // Interoperable (max 20): Known MIME type (+10), version specified (+10)
    if (!entry.mime_type.empty() && entry.mime_type != "application/octet-stream") score += 10.0;
    if (!entry.version.empty()) score += 10.0;

    // Reusable (max 25): Has valid license e.g. starts with SPDX: or is non-empty (+20), has abstract or description (+5)
    if (!entry.license.empty()) score += 20.0;
    if (!entry.abstract.empty()) score += 5.0;

    if (score > 100.0) score = 100.0;
    return score;
}

ArtifactEntry Librarian::process_ingest_artifact(
    std::string_view logical_path,
    std::string_view raw_content,
    std::string_view content_hash,
    std::string_view user_id,
    std::string_view agent_id,
    const std::vector<AVUTriple>& initial_avus
) {
    ArtifactEntry entry;

    // 1. Path normalization
    std::filesystem::path p = std::filesystem::path(logical_path).lexically_normal();
    std::string norm_path = p.string();
    size_t start_idx = norm_path.find_first_not_of('/');
    std::string path_no_leading = (start_idx != std::string::npos) ? norm_path.substr(start_idx) : "";
    if (path_no_leading == "." || path_no_leading == ".." || path_no_leading.rfind("../", 0) == 0) {
        path_no_leading = "";
    }

    if (path_no_leading.empty()) {
        entry.pid = "";
        entry.collection_path = "";
        entry.logical_name = "";
    } else {
        entry.logical_name = p.filename().string();
        std::string coll = p.parent_path().string();
        if (coll.empty()) {
            coll = "/";
        } else if (coll[0] != '/') {
            coll = "/" + coll;
        }
        entry.collection_path = coll;
        entry.pid = "urn:ab:artifact:" + path_no_leading;
    }

    // 2. Frontmatter parser & Heading fallback
    auto trim_sv = [](std::string_view s) -> std::string_view {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
            s.remove_suffix(1);
        }
        return s;
    };

    auto strip_comment = [&trim_sv](std::string_view s) -> std::string_view {
        bool in_single = false;
        bool in_double = false;
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '\\' && in_double && i + 1 < s.size()) {
                ++i;
                continue;
            }
            if (c == '\'' && !in_double) {
                in_single = !in_single;
            } else if (c == '"' && !in_single) {
                in_double = !in_double;
            } else if (c == '#' && !in_single && !in_double && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
                return trim_sv(s.substr(0, i));
            }
        }
        return trim_sv(s);
    };

    auto unquote = [](std::string_view s) -> std::string {
        if (s.size() >= 2) {
            if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')) {
                return std::string(s.substr(1, s.size() - 2));
            }
        }
        return std::string(s);
    };

    // Split raw_content into lines
    std::vector<std::string_view> lines;
    size_t pos = 0;
    while (pos < raw_content.size()) {
        size_t next_nl = raw_content.find('\n', pos);
        if (next_nl == std::string_view::npos) {
            std::string_view line = raw_content.substr(pos);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            lines.push_back(line);
            break;
        }
        std::string_view line = raw_content.substr(pos, next_nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        pos = next_nl + 1;
    }

    size_t fm_start = 0;
    while (fm_start < lines.size() && trim_sv(lines[fm_start]).empty()) {
        fm_start++;
    }

    bool has_fm = false;
    size_t fm_end = 0;
    if (fm_start < lines.size() && trim_sv(lines[fm_start]) == "---") {
        for (size_t i = fm_start + 1; i < lines.size(); ++i) {
            if (trim_sv(lines[i]) == "---" || trim_sv(lines[i]) == "...") {
                has_fm = true;
                fm_end = i;
                break;
            }
        }
    }

    if (has_fm) {
        for (size_t i = fm_start + 1; i < fm_end; ++i) {
            std::string_view line = trim_sv(lines[i]);
            if (line.empty() || line.starts_with('#')) continue;
            size_t colon = line.find(':');
            if (colon == std::string_view::npos) continue;

            std::string_view key_sv = trim_sv(line.substr(0, colon));
            std::string_view val_sv = strip_comment(line.substr(colon + 1));
            std::string val = unquote(val_sv);

            std::string key(key_sv);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });

            if (key == "title") {
                entry.title = val;
            } else if (key == "license") {
                entry.license = val;
            } else if (key == "version") {
                entry.version = val;
            } else if (key == "abstract") {
                entry.abstract = val;
            } else if (key == "description") {
                if (entry.abstract.empty()) {
                    entry.abstract = val;
                }
            }
        }
    }

    // Heading fallback: If no frontmatter or title is empty, scan for the first line starting with '# '
    if (entry.title.empty()) {
        size_t scan_start = has_fm ? (fm_end + 1) : 0;
        for (size_t i = scan_start; i < lines.size(); ++i) {
            std::string_view trimmed = trim_sv(lines[i]);
            if (trimmed.starts_with("# ")) {
                std::string_view heading = trim_sv(trimmed.substr(2));
                if (!heading.empty()) {
                    entry.title = std::string(heading);
                    break;
                }
            }
        }
    }

    // 3. Set content_hash, byte_size, mime_type, created_at_ms
    entry.content_hash = std::string(content_hash);
    entry.byte_size = raw_content.size();
    entry.mime_type = infer_mime_type(entry.logical_name);
    entry.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // 4. Generate UUID if empty
    if (entry.uuid.empty()) {
        static std::atomic<uint64_t> s_seq{0};
        std::string seed = std::to_string(entry.created_at_ms) + ":" +
                           std::to_string(++s_seq) + ":" +
                           entry.logical_name + ":" +
                           entry.content_hash + ":" +
                           entry.title + ":" +
                           entry.pid;
        uint32_t h = std::hash<std::string>{}(seed);
        char hex[9];
        std::snprintf(hex, sizeof(hex), "%08x", h);
        entry.uuid = "art-" + std::string(hex);
    }

    // 5. Populate AVUs: add initial_avus (excluding any pre-existing fair:score) and auto-tag verified fair:score
    entry.avus.clear();
    for (const auto& avu : initial_avus) {
        if (avu.attribute != "fair:score") {
            entry.avus.push_back(avu);
        }
    }
    double score = calculate_fair_score(entry);
    entry.avus.push_back(AVUTriple{"fair:score", std::to_string(static_cast<int>(score)), "points"});

    // 6. Commit to Blackboard if available
    if (blackboard_) {
        if (!blackboard_->commit_artifact(entry, user_id, agent_id)) {
            std::cerr << "[Librarian] Warning: Failed to commit artifact to blackboard: " << entry.uuid << std::endl;
        }
    }

    return entry;
}

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
    if (running_ || thread_.joinable()) stop();
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

} // namespace agentic_blackboard
