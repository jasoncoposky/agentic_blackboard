#pragma once

#include "asos/Blackboard.hpp"
#include <string>

namespace asos {

class RdfExporter {
public:
    static std::string export_turtle(Blackboard* blackboard, uint32_t principal_id = 0);
};

} // namespace asos
