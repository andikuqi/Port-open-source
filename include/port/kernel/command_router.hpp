
 #pragma once

#include "port/kernel/command.hpp"

#include <string>
#include <string_view>

namespace port::kernel {

// Stateless text-to-Command parser.
class CommandRouter {
public:
    [[nodiscard]] Command parse(std::string_view input) const;

private:
    [[nodiscard]] static std::string_view trim(std::string_view s) noexcept;
};

} // namespace port::kernel
