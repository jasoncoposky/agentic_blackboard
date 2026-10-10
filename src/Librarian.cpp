#include <agentic_blackboard/Librarian.hpp>
#include <chrono>
#include <iostream>

namespace agentic_blackboard {

Librarian& Librarian::instance() {
    static Librarian inst;
    return inst;
}

Librarian::Librarian(
    Blackboard* blackboard,
    std::unique_ptr<ArtifactIngestionService> ingestion_svc,
    std::unique_ptr<GraphAnalogyEngine> analogy_engine,
    std::unique_ptr<GraphTopologyAuditor> topology_auditor
)
    : running_(false),
      blackboard_(blackboard),
      ingestion_service_(std::move(ingestion_svc)),
      analogy_engine_(std::move(analogy_engine)),
      topology_auditor_(std::move(topology_auditor))
{
    if (!ingestion_service_) {
        ingestion_service_ = std::make_unique<ArtifactIngestionService>(blackboard_);
    }
    if (!analogy_engine_) {
        analogy_engine_ = std::make_unique<GraphAnalogyEngine>(blackboard_);
    }
    if (!topology_auditor_) {
        topology_auditor_ = std::make_unique<GraphTopologyAuditor>(blackboard_);
    }

    if (blackboard_) {
        if (ingestion_service_ && !ingestion_service_->get_blackboard()) {
            ingestion_service_->set_blackboard(blackboard_);
        }
        if (analogy_engine_ && !analogy_engine_->get_blackboard()) {
            analogy_engine_->set_blackboard(blackboard_);
        }
        if (topology_auditor_ && !topology_auditor_->get_blackboard()) {
            topology_auditor_->set_blackboard(blackboard_);
        }
    }
}

Librarian::~Librarian() {
    stop();
}

void Librarian::set_blackboard(Blackboard* bb) {
    blackboard_ = bb;
    if (ingestion_service_) ingestion_service_->set_blackboard(bb);
    if (analogy_engine_) analogy_engine_->set_blackboard(bb);
    if (topology_auditor_) topology_auditor_->set_blackboard(bb);
}

void Librarian::start(Blackboard* blackboard) {
    if (running_ || thread_.joinable()) stop();
    set_blackboard(blackboard);
    running_ = true;
    thread_ = std::thread(&Librarian::analysis_loop, this);
}

void Librarian::stop() {
    running_ = false;
    if (thread_.joinable())
        thread_.join();
}

void Librarian::perform_analysis() {
    if (analogy_engine_) {
        analogy_engine_->perform_analysis(blackboard_);
    }
}

void Librarian::audit_orphans() {
    if (topology_auditor_) {
        topology_auditor_->audit_orphans(blackboard_);
    }
}

ArtifactEntry Librarian::process_ingest_artifact(
    std::string_view logical_path,
    std::string_view raw_content,
    std::string_view content_hash,
    std::string_view user_id,
    std::string_view agent_id,
    const std::vector<AVUTriple>& initial_avus
) {
    if (ingestion_service_) {
        return ingestion_service_->ingest(logical_path, raw_content, content_hash, user_id, agent_id, initial_avus);
    }
    return ArtifactEntry{};
}

double Librarian::calculate_fair_score(const ArtifactEntry& entry) const {
    if (ingestion_service_) {
        return ingestion_service_->calculate_fair_score(entry);
    }
    return 0.0;
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
