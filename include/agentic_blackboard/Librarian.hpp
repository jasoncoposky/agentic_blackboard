#pragma once

#include <agentic_blackboard/Blackboard.hpp>
#include <agentic_blackboard/schema.hpp>
#include <thread>
#include <atomic>
#include <string_view>
#include <vector>

namespace agentic_blackboard {

/**
 * @brief Singleton analyzer that creates "Synapses" (Edges) between CPB atoms
 *        and enforces data ingestion policies and FAIR metric scoring.
 */
class Librarian {
public:
    static Librarian& instance() {
        static Librarian inst;
        return inst;
    }

    explicit Librarian(Blackboard* blackboard = nullptr);
    ~Librarian();

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
     * @brief Ingest an artifact: parses frontmatter, extracts headings, mints PID,
     * calculates FAIR score, and populates AVUs.
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
    
    std::atomic<bool> running_;
    std::thread thread_;
    Blackboard* blackboard_;
};

} // namespace agentic_blackboard

namespace ab = agentic_blackboard;
namespace blackboard = agentic_blackboard;
