#pragma once

#include <agentic_blackboard/schema.hpp>
#include <agentic_blackboard/FairScoreStrategy.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentic_blackboard {

class Blackboard;

/**
 * @brief Context passed through the ingestion filter pipeline.
 */
struct IngestionContext {
    std::string_view logical_path;
    std::string_view raw_content;
    std::string_view content_hash;
    std::string_view user_id;
    std::string_view agent_id;
    std::vector<AVUTriple> initial_avus;

    // Mutable pipeline state passed through filters
    std::string collection_path;
    std::string logical_name;
    std::string pid;
    std::string mime_type;
    std::string title;
    std::string abstract;
    std::string license;
    std::string version;
    std::vector<AVUTriple> avus;
};

/**
 * @brief Abstract filter interface for the ingestion pipeline.
 */
class IIngestionFilter {
public:
    virtual ~IIngestionFilter() = default;
    virtual void filter(IngestionContext& ctx) = 0;
};

/**
 * @brief Normalizes logical paths and generates persistent identifiers (PIDs).
 */
class PathNormalizationFilter : public IIngestionFilter {
public:
    PathNormalizationFilter() = default;
    void filter(IngestionContext& ctx) override;
};

/**
 * @brief Extracts YAML frontmatter and markdown headings, and infers MIME types.
 */
class FrontmatterExtractionFilter : public IIngestionFilter {
public:
    FrontmatterExtractionFilter() = default;
    void filter(IngestionContext& ctx) override;

    static std::string infer_mime_type(std::string_view filename);
};

/**
 * @brief Evaluates FAIR compliance score and auto-tags verified fair:score AVU.
 */
class FairScoringFilter : public IIngestionFilter {
public:
    explicit FairScoringFilter(std::shared_ptr<IFairScoreStrategy> strategy = nullptr);
    void filter(IngestionContext& ctx) override;

    void set_strategy(std::shared_ptr<IFairScoreStrategy> strategy);
    std::shared_ptr<IFairScoreStrategy> get_strategy() const noexcept;

private:
    std::shared_ptr<IFairScoreStrategy> strategy_;
};

/**
 * @brief Orchestrates artifact ingestion through a configurable Chain of Responsibility pipeline.
 */
class ArtifactIngestionService {
public:
    explicit ArtifactIngestionService(
        Blackboard* blackboard = nullptr,
        std::shared_ptr<IFairScoreStrategy> score_strategy = nullptr
    );

    void set_score_strategy(std::shared_ptr<IFairScoreStrategy> strategy);
    std::shared_ptr<IFairScoreStrategy> get_score_strategy() const noexcept;

    void add_filter(std::unique_ptr<IIngestionFilter> filter);
    const std::vector<std::unique_ptr<IIngestionFilter>>& filters() const noexcept;
    void clear_filters();

    void set_blackboard(Blackboard* blackboard) noexcept;
    Blackboard* get_blackboard() const noexcept;

    ArtifactEntry ingest(
        std::string_view logical_path,
        std::string_view raw_content,
        std::string_view content_hash,
        std::string_view user_id = "",
        std::string_view agent_id = "",
        const std::vector<AVUTriple>& initial_avus = {}
    );

    double calculate_fair_score(const ArtifactEntry& entry) const;

private:
    Blackboard* blackboard_{nullptr};
    std::shared_ptr<IFairScoreStrategy> score_strategy_;
    std::vector<std::unique_ptr<IIngestionFilter>> pipeline_;
};

} // namespace agentic_blackboard

namespace blackboard = agentic_blackboard;
namespace ab = agentic_blackboard;
