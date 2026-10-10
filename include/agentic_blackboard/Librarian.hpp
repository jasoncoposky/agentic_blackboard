#pragma once

#include <agentic_blackboard/Blackboard.hpp>
#include <agentic_blackboard/schema.hpp>
#include <agentic_blackboard/ArtifactIngestionService.hpp>
#include <agentic_blackboard/GraphAnalogyEngine.hpp>
#include <agentic_blackboard/GraphTopologyAuditor.hpp>
#include <thread>
#include <atomic>
#include <memory>
#include <string_view>
#include <vector>

namespace agentic_blackboard {

/**
 * @brief Facade orchestrating knowledge graph analysis, orphan auditing,
 *        and artifact ingestion services.
 */
class Librarian {
public:
    static Librarian& instance();

    explicit Librarian(
        Blackboard* blackboard = nullptr,
        std::unique_ptr<ArtifactIngestionService> ingestion_svc = nullptr,
        std::unique_ptr<GraphAnalogyEngine> analogy_engine = nullptr,
        std::unique_ptr<GraphTopologyAuditor> topology_auditor = nullptr
    );
    ~Librarian();

    Librarian(const Librarian&) = delete;
    Librarian& operator=(const Librarian&) = delete;
    Librarian(Librarian&&) = delete;
    Librarian& operator=(Librarian&&) = delete;

    ArtifactIngestionService& ingestion_service() { return *ingestion_service_; }
    const ArtifactIngestionService& ingestion_service() const { return *ingestion_service_; }
    GraphAnalogyEngine& analogy_engine() { return *analogy_engine_; }
    const GraphAnalogyEngine& analogy_engine() const { return *analogy_engine_; }
    GraphTopologyAuditor& topology_auditor() { return *topology_auditor_; }
    const GraphTopologyAuditor& topology_auditor() const { return *topology_auditor_; }

    /**
     * @brief Start the background analysis thread.
     */
    void start(Blackboard* blackboard);
    void stop();

    /**
     * @brief Force a manual analysis of the current graph.
     */
    void perform_analysis();
    void audit_orphans();

    /**
     * @brief Ingest an artifact: delegates to ArtifactIngestionService.
     */
    ArtifactEntry process_ingest_artifact(
        std::string_view logical_path,
        std::string_view raw_content,
        std::string_view content_hash,
        std::string_view user_id = "",
        std::string_view agent_id = "",
        const std::vector<AVUTriple>& initial_avus = {}
    );

    /**
     * @brief Calculate FAIR rubric score (0.0 to 100.0) based on findability,
     * accessibility, interoperability, and reusability.
     */
    double calculate_fair_score(const ArtifactEntry& entry) const;

private:
    void analysis_loop();
    
    std::atomic<bool> running_{false};
    std::thread thread_;
    Blackboard* blackboard_{nullptr};

    std::unique_ptr<ArtifactIngestionService> ingestion_service_;
    std::unique_ptr<GraphAnalogyEngine> analogy_engine_;
    std::unique_ptr<GraphTopologyAuditor> topology_auditor_;
};

} // namespace agentic_blackboard

namespace ab = agentic_blackboard;
namespace blackboard = agentic_blackboard;
