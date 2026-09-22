#pragma once

#include <string>
#include <vector>

namespace port::kernel {

enum class DriverState { Missing, Installed };

struct FoundationDriver {
    std::string id;
    std::string name;
    std::string description;
    DriverState state{DriverState::Missing};
};

class FoundationDriverRegistry {
public:
    FoundationDriverRegistry();

    void installAll();
    [[nodiscard]] std::string summary() const;
    [[nodiscard]] std::string detailedReport() const;

private:
    std::vector<FoundationDriver> drivers_;
};

} // namespace port::kernel
