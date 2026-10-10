#pragma once

#include <agentic_blackboard/schema.hpp>
#include <algorithm>
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
 * - Findable (max 30 pts):
 *   - PID: 15 pts if !entry.pid.empty() && entry.pid != "urn:ab:artifact:"
 *   - Title: 15 pts if !entry.title.empty()
 * - Accessible (max 25 pts):
 *   - Content Hash or Primary Locator: 15 pts if !entry.content_hash.empty() || !entry.primary_locator.empty()
 *   - Logical Path: 10 pts if !entry.collection_path.empty() && !entry.logical_name.empty()
 * - Interoperable (max 20 pts):
 *   - MIME Type: 10 pts if !entry.mime_type.empty() && entry.mime_type != "application/octet-stream"
 *   - Explicit Version: 10 pts if !entry.version.empty()
 * - Reusable (max 25 pts):
 *   - License: 20 pts if !entry.license.empty()
 *   - Abstract: 5 pts if !entry.abstract.empty()
 */
class StandardFairScoreStrategy : public IFairScoreStrategy {
public:
    double calculate_score(const ArtifactEntry& entry) const override {
        double score = 0.0;

        // Findable (max 30): Has non-empty PID (+15), has non-empty Title (+15)
        if (!entry.pid.empty() && entry.pid != "urn:ab:artifact:") score += 15.0;
        if (!entry.title.empty()) score += 15.0;

        // Accessible (max 25): Has non-empty content_hash (+15) (or primary_locator if hash empty), has logical path (+10)
        if (!entry.content_hash.empty() || !entry.primary_locator.empty()) score += 15.0;
        if (!entry.collection_path.empty() && !entry.logical_name.empty()) score += 10.0;

        // Interoperable (max 20): Known MIME type (+10), version specified (+10)
        if (!entry.mime_type.empty() && entry.mime_type != "application/octet-stream") score += 10.0;
        if (!entry.version.empty()) score += 10.0;

        // Reusable (max 25): Has valid license (+20), has abstract or description (+5)
        if (!entry.license.empty()) score += 20.0;
        if (!entry.abstract.empty()) score += 5.0;

        return std::clamp(score, 0.0, 100.0);
    }
};

} // namespace agentic_blackboard

namespace blackboard = agentic_blackboard;
namespace ab = agentic_blackboard;
