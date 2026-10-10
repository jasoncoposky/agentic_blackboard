#include <agentic_blackboard/ArtifactIngestionService.hpp>
#include <agentic_blackboard/Blackboard.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>

namespace agentic_blackboard {

// ============================================================================
// PathNormalizationFilter
// ============================================================================

void PathNormalizationFilter::filter(IngestionContext& ctx) {
    std::string_view p_view = ctx.logical_path;
    while (p_view.size() > 1 && (p_view.back() == '/' || p_view.back() == '\\')) {
        p_view.remove_suffix(1);
    }
    std::filesystem::path p = std::filesystem::path(p_view).lexically_normal();
    std::string norm_path = p.generic_string();
    size_t start_idx = norm_path.find_first_not_of('/');
    std::string path_no_leading = (start_idx != std::string::npos) ? norm_path.substr(start_idx) : "";
    if (path_no_leading == "." || path_no_leading == ".." || path_no_leading.rfind("../", 0) == 0) {
        path_no_leading = "";
    }

    if (p.filename().empty() || p.filename() == "." || p.filename() == ".." || path_no_leading.empty()) {
        ctx.pid = "";
        ctx.collection_path = "";
        ctx.logical_name = "";
    } else {
        ctx.logical_name = p.filename().generic_string();
        std::string coll = p.parent_path().generic_string();
        if (coll.empty() || coll == ".") {
            coll = "/";
        } else if (coll[0] != '/') {
            coll = "/" + coll;
        }
        ctx.collection_path = coll;
        ctx.pid = "urn:ab:artifact:" + path_no_leading;
    }
}

// ============================================================================
// FrontmatterExtractionFilter
// ============================================================================

std::string FrontmatterExtractionFilter::infer_mime_type(std::string_view filename) {
    auto dot_pos = filename.rfind('.');
    if (dot_pos == std::string_view::npos) {
        return "application/octet-stream";
    }
    std::string ext(filename.substr(dot_pos));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
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

void FrontmatterExtractionFilter::filter(IngestionContext& ctx) {
    if (ctx.mime_type.empty()) {
        ctx.mime_type = infer_mime_type(ctx.logical_name);
    }

    auto dot_pos = ctx.logical_name.rfind('.');
    std::string ext;
    if (dot_pos != std::string::npos) {
        ext = ctx.logical_name.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    }

    bool is_code = (ext == ".py" || ext == ".sh" || ext == ".cpp" || ext == ".c" ||
                    ext == ".h" || ext == ".hpp" || ext == ".bash" || ext == ".zsh");
    bool supports_headings = !is_code;
    bool is_text = false;
    if (!ext.empty()) {
        if (ext == ".md" || ext == ".markdown" || ext == ".txt" || ext == ".text" ||
            ext == ".yaml" || ext == ".yml" || ext == ".json" || ext == ".csv" ||
            ext == ".tsv" || ext == ".xml" || ext == ".html" || ext == ".htm" ||
            ext == ".svg" || ext == ".toml" || ext == ".ini" || ext == ".cfg" ||
            ext == ".conf" || ext == ".rst" || ext == ".py" || ext == ".cpp" ||
            ext == ".h" || ext == ".c" || ext == ".hpp" || ext == ".sh") {
            is_text = true;
        }
    }

    if (!is_text || ctx.raw_content.empty()) {
        return;
    }

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

    size_t scan_limit = std::min(ctx.raw_content.size(), size_t(65536));
    std::string_view prefix = ctx.raw_content.substr(0, scan_limit);

    size_t pos = 0;
    auto get_next_line = [&](std::string_view& line) -> bool {
        if (pos >= prefix.size()) return false;
        size_t next_nl = prefix.find('\n', pos);
        if (next_nl == std::string_view::npos) {
            line = prefix.substr(pos);
            pos = prefix.size();
        } else {
            line = prefix.substr(pos, next_nl - pos);
            pos = next_nl + 1;
        }
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        return true;
    };

    bool found_fm_start = false;
    bool in_frontmatter = false;
    bool frontmatter_done = false;

    std::string_view line;
    while (get_next_line(line)) {
        std::string_view trimmed = trim_sv(line);

        if (!found_fm_start) {
            if (trimmed.empty()) {
                continue; // Skip leading blank lines
            }
            if (trimmed == "---") {
                found_fm_start = true;
                in_frontmatter = true;
                continue;
            } else {
                // No frontmatter present
                found_fm_start = true;
                frontmatter_done = true;
                // Check if first non-empty line is a heading
                if (supports_headings && trimmed.starts_with("# ")) {
                    std::string_view heading = trim_sv(trimmed.substr(2));
                    if (!heading.empty()) {
                        ctx.title = std::string(heading);
                        break; // Title heading found, early exit
                    }
                }
                if (is_code) {
                    break;
                }
                continue;
            }
        }

        if (in_frontmatter) {
            if (trimmed == "---" || trimmed == "...") {
                in_frontmatter = false;
                frontmatter_done = true;
                if (!ctx.title.empty() || is_code) {
                    break; // Title found in frontmatter, or code file, early exit
                }
                continue;
            }

            if (trimmed.empty() || trimmed.starts_with('#')) {
                continue;
            }

            size_t colon = trimmed.find(':');
            if (colon != std::string_view::npos) {
                std::string_view key_sv = trim_sv(trimmed.substr(0, colon));
                std::string_view val_sv = strip_comment(trimmed.substr(colon + 1));
                std::string val = unquote(val_sv);

                std::string key(key_sv);
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });

                if (key == "title") {
                    ctx.title = val;
                } else if (key == "license") {
                    ctx.license = val;
                } else if (key == "version") {
                    ctx.version = val;
                } else if (key == "abstract") {
                    ctx.abstract = val;
                } else if (key == "description") {
                    if (ctx.abstract.empty()) {
                        ctx.abstract = val;
                    }
                }
            }
        } else if (frontmatter_done) {
            if (ctx.title.empty() && supports_headings) {
                if (trimmed.starts_with("# ")) {
                    std::string_view heading = trim_sv(trimmed.substr(2));
                    if (!heading.empty()) {
                        ctx.title = std::string(heading);
                        break; // Heading found, early exit
                    }
                }
            } else {
                break;
            }
        }
    }
}

// ============================================================================
// FairScoringFilter
// ============================================================================

FairScoringFilter::FairScoringFilter(std::shared_ptr<IFairScoreStrategy> strategy)
    : strategy_(strategy ? std::move(strategy) : std::make_shared<StandardFairScoreStrategy>()) {}

void FairScoringFilter::set_strategy(std::shared_ptr<IFairScoreStrategy> strategy) {
    strategy_ = strategy ? std::move(strategy) : std::make_shared<StandardFairScoreStrategy>();
}

std::shared_ptr<IFairScoreStrategy> FairScoringFilter::get_strategy() const noexcept {
    return strategy_;
}

void FairScoringFilter::filter(IngestionContext& ctx) {
    std::vector<AVUTriple> filtered_avus;
    filtered_avus.reserve(ctx.avus.size() + ctx.initial_avus.size());
    for (const auto& avu : ctx.avus) {
        if (avu.attribute != "fair:score") {
            filtered_avus.push_back(avu);
        }
    }
    for (const auto& avu : ctx.initial_avus) {
        if (avu.attribute != "fair:score") {
            bool exists = false;
            for (const auto& existing : filtered_avus) {
                if (existing.attribute == avu.attribute &&
                    existing.value == avu.value &&
                    existing.units == avu.units) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                filtered_avus.push_back(avu);
            }
        }
    }
    ctx.avus = std::move(filtered_avus);

    ArtifactEntry entry;
    entry.pid = ctx.pid;
    entry.collection_path = ctx.collection_path;
    entry.logical_name = ctx.logical_name;
    entry.content_hash = std::string(ctx.content_hash);
    entry.mime_type = ctx.mime_type;
    entry.byte_size = ctx.raw_content.size();
    entry.title = ctx.title;
    entry.abstract = ctx.abstract;
    entry.license = ctx.license;
    entry.version = ctx.version;
    entry.avus = ctx.avus;

    double score = strategy_ ? strategy_->calculate_score(entry) : 0.0;
    ctx.avus.push_back(AVUTriple{"fair:score", std::to_string(static_cast<int>(score)), "points"});
}

// ============================================================================
// ArtifactIngestionService
// ============================================================================

ArtifactIngestionService::ArtifactIngestionService(
    Blackboard* blackboard,
    std::shared_ptr<IFairScoreStrategy> score_strategy
)
    : blackboard_(blackboard),
      score_strategy_(score_strategy ? std::move(score_strategy) : std::make_shared<StandardFairScoreStrategy>()) {
    pipeline_.push_back(std::make_unique<PathNormalizationFilter>());
    pipeline_.push_back(std::make_unique<FrontmatterExtractionFilter>());
    pipeline_.push_back(std::make_unique<FairScoringFilter>(score_strategy_));
}

void ArtifactIngestionService::set_score_strategy(std::shared_ptr<IFairScoreStrategy> strategy) {
    score_strategy_ = strategy ? std::move(strategy) : std::make_shared<StandardFairScoreStrategy>();
    for (auto& f : pipeline_) {
        if (auto* fs = dynamic_cast<FairScoringFilter*>(f.get())) {
            fs->set_strategy(score_strategy_);
        }
    }
}

std::shared_ptr<IFairScoreStrategy> ArtifactIngestionService::get_score_strategy() const noexcept {
    return score_strategy_;
}

void ArtifactIngestionService::add_filter(std::unique_ptr<IIngestionFilter> filter) {
    if (filter) {
        pipeline_.push_back(std::move(filter));
    }
}

void ArtifactIngestionService::insert_filter(size_t index, std::unique_ptr<IIngestionFilter> filter) {
    if (!filter) {
        return;
    }
    if (index >= pipeline_.size()) {
        pipeline_.push_back(std::move(filter));
    } else {
        pipeline_.insert(pipeline_.begin() + index, std::move(filter));
    }
}

const std::vector<std::unique_ptr<IIngestionFilter>>& ArtifactIngestionService::filters() const noexcept {
    return pipeline_;
}

void ArtifactIngestionService::clear_filters() {
    pipeline_.clear();
}

void ArtifactIngestionService::set_blackboard(Blackboard* blackboard) noexcept {
    blackboard_ = blackboard;
}

Blackboard* ArtifactIngestionService::get_blackboard() const noexcept {
    return blackboard_;
}

ArtifactEntry ArtifactIngestionService::ingest(
    std::string_view logical_path,
    std::string_view raw_content,
    std::string_view content_hash,
    std::string_view user_id,
    std::string_view agent_id,
    const std::vector<AVUTriple>& initial_avus
) {
    IngestionContext ctx;
    ctx.logical_path = logical_path;
    ctx.raw_content = raw_content;
    ctx.content_hash = content_hash;
    ctx.user_id = user_id;
    ctx.agent_id = agent_id;
    ctx.initial_avus = initial_avus;
    ctx.avus = initial_avus;

    for (auto& filter : pipeline_) {
        if (ctx.aborted) {
            break;
        }
        if (filter) {
            filter->filter(ctx);
        }
        if (ctx.aborted) {
            break;
        }
    }

    ArtifactBuilder builder;
    builder.with_path(ctx.collection_path, ctx.logical_name)
           .with_content_hash(ctx.content_hash)
           .with_mime_type(ctx.mime_type)
           .with_byte_size(ctx.raw_content.size())
           .with_title(ctx.title)
           .with_abstract(ctx.abstract)
           .with_license(ctx.license)
           .with_version(ctx.version);

    if (!ctx.pid.empty()) {
        builder.with_pid(ctx.pid);
    }

    for (const auto& avu : ctx.avus) {
        builder.add_avu(avu);
    }

    ArtifactEntry entry = builder.build();
    if (ctx.pid.empty()) {
        entry.pid.clear();
    }

    if (ctx.logical_name.empty() || ctx.pid.empty() || ctx.aborted) {
        return entry;
    }

    if (blackboard_) {
        if (!blackboard_->commit_artifact(entry, user_id, agent_id)) {
            std::cerr << "[ArtifactIngestionService] Warning: Failed to commit artifact to blackboard: "
                      << entry.uuid << std::endl;
        }
    }

    return entry;
}

double ArtifactIngestionService::calculate_fair_score(const ArtifactEntry& entry) const {
    if (score_strategy_) {
        return score_strategy_->calculate_score(entry);
    }
    return 0.0;
}

} // namespace agentic_blackboard
