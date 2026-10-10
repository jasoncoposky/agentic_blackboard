#pragma once
#include <cstddef>

namespace agentic_blackboard {
class Blackboard;

class GraphAnalogyEngine {
public:
    explicit GraphAnalogyEngine(Blackboard* blackboard = nullptr);
    void set_blackboard(Blackboard* blackboard) { blackboard_ = blackboard; }
    Blackboard* get_blackboard() const noexcept { return blackboard_; }
    size_t perform_analysis(Blackboard* blackboard = nullptr);

private:
    Blackboard* blackboard_{nullptr};
};
} // namespace agentic_blackboard

namespace ab = agentic_blackboard;
namespace blackboard = agentic_blackboard;
