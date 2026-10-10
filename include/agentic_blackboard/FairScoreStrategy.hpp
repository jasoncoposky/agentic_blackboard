#pragma once

#include <agentic_blackboard/schema.hpp>
#include <memory>
#include <string>

namespace agentic_blackboard {

/**
 * @brief Interface for FAIR (Findable, Accessible, Interoperable, Reusable) scoring strategies.
 */
class IFairScoreStrategy {
public:
    virtual ~IFairScoreStrategy() = default;

    /**
     * @brief Calculate the FAIR compliance score for an artifact entry.
     * @param entry The artifact entry to evaluate.
     * @return Score between 0.0 and 100.0.
     */
    virtual double calculate_score(const ArtifactEntry& entry) const = 0;
};

/**
 * @brief Standard implementation of the 100-point FAIR scoring rubric.
 *
 * Scoring breakdown:
 * - Findable (35 pts):
 *   - PID: 15 pts if !entry.pid.empty() && entry.pid != "urn:ab:artifact:"
 *   - Collection + Logical Name: 10 pts if !entry.collection_path.empty() && !entry.logical_name.empty()
 *   - Title: 10 pts if !entry.title.empty()
 * - Accessible (25 pts):
 *   - Primary Locator: 15 pts if !entry.primary_locator.empty()
 *   - Content Hash: 10 pts if !entry.content_hash.empty()
 * - Interoperable (20 pts):
 *   - MIME Type: 10 pts if !entry.mime_type.empty() && entry.mime_type != "application/octet-stream"
 *   - Explicit Version: 10 pts if !entry.version.empty()
 * - Reusable (20 pts):
 *   - License: 15 pts if !entry.license.empty()
 *   - Abstract: 5 pts if !entry.abstract.empty()
 */
class StandardFairScoreStrategy : public IFairScoreStrategy {
public:
    double calculate_score(const ArtifactEntry& entry) const override {
        double score = 0.0;

        // Findable (35 pts)
        if (!entry.pid.empty() && entry.pid != "urn:ab:artifact:") {
            score += 15.0;
        }
        if (!entry.collection_path.empty() && !entry.logical_name.empty()) {
            score += 10.0;
        }
        if (!entry.title.empty()) {
            score += 10.0;
        }

        // Accessible (25 pts)
        if (!entry.primary_locator.empty()) {
            score += 15.0;
        }
        if (!entry.content_hash.empty()) {
            score += 10.0;
        }

        // Interoperable (20 pts)
        if (!entry.mime_type.empty() && entry.mime_type != "application/octet-stream") {
            score += 10.0;
        }
        if (!entry.version.empty()) {
            score += 10.0;
        }

        // Reusable (20 pts)
        if (!entry.license.empty()) {
            score += 15.0;
        }
        if (!entry.abstract.empty()) {
            score += 5.0;
        }

        if (score > 100.0) {
            score = 100.0;
        }
        return score;
    }
};

} // namespace agentic_blackboard

namespace blackboard = agentic_blackboard;
namespace ab = agentic_blackboard;
