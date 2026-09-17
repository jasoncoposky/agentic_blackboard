#pragma once

#include <agentic_blackboard/Blackboard.hpp>
#include <string>

namespace agentic_blackboard {

class RdfExporter {
public:
    static std::string export_turtle(Blackboard* blackboard, uint32_t principal_id = 0);
};

} // namespace agentic_blackboard

namespace ab = agentic_blackboard;
