#pragma once

#include <string>
#include <vector>

namespace fox {

enum class DriverState {
    Missing,
    Installed
};

struct FoundationDriver {
    std::string id;
    std::string name;
    std::string description;
    DriverState state;
};

class FoundationDriverRegistry {
public:
    FoundationDriverRegistry();

    void installAll();

    std::string summary() const;
    std::string detailedReport() const;

private:
    std::vector<FoundationDriver> drivers_;
};

} // namespace fox

