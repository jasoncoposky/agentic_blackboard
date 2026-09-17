#pragma once

#include <agentic_blackboard/Blackboard.hpp>
#include <string>

namespace agentic_blackboard {

/**
 * @brief Manages the V&V lifecycle of Knowledge Atoms.
 */
class Validator {
public:
    explicit Validator(Blackboard* blackboard) : blackboard_(blackboard) {}

    /**
     * @brief Perform SWEBOK Verification (Static Audit).
     */
    bool verify_atom(const std::string& uuid, const std::string& auditor_id);

    /**
     * @brief Perform Functional Validation (Dynamic Test).
     */
    bool validate_atom(const std::string& uuid, const std::string& auditor_id);

    /**
     * @brief Promote an atom to Universal Principle if V&V gates are passed.
     */
    bool try_promote(const std::string& uuid);

private:
    Blackboard* blackboard_;
};

} // namespace agentic_blackboard

namespace ab = agentic_blackboard;
