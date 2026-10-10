#pragma once
#include <cstddef>

namespace agentic_blackboard {
class Blackboard;

class GraphTopologyAuditor {
public:
    explicit GraphTopologyAuditor(Blackboard* blackboard = nullptr);
    void set_blackboard(Blackboard* blackboard) { blackboard_ = blackboard; }
    Blackboard* get_blackboard() const noexcept { return blackboard_; }
    size_t audit_orphans(Blackboard* blackboard = nullptr);

private:
    Blackboard* blackboard_{nullptr};
};
} // namespace agentic_blackboard

namespace ab = agentic_blackboard;
namespace blackboard = agentic_blackboard;
