#pragma once

#include "fox/command.hpp"

#include <string>

namespace fox {

class CommandRouter {
public:
    Command parse(const std::string& input) const;

private:
    static std::string trim(std::string value);
};

} // namespace fox

