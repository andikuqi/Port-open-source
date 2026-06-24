#include "fox/foundation_driver.hpp"

#include <sstream>

namespace fox {

FoundationDriverRegistry::FoundationDriverRegistry()
{
    drivers_.push_back({
        "driver.registry",
        "System Driver Registry",
        "Tracks virtual foundation drivers used by the Port desktop runtime.",
        DriverState::Missing
    });
    drivers_.push_back({
        "toolchain.compiler",
        "Compiler Toolchain Detector",
        "Detects and validates the local compiler used to build Port OS modules.",
        DriverState::Missing
    });
    drivers_.push_back({
        "sandbox.filesystem",
        "Sandbox Filesystem Driver",
        "Controls isolated virtual filesystem access for AI tasks.",
        DriverState::Missing
    });
    drivers_.push_back({
        "network.controller",
        "Network Access Controller",
        "Provides controlled network access for search and downloads.",
        DriverState::Missing
    });
    drivers_.push_back({
        "ai.policy",
        "AI Execution Policy Engine",
        "Decides which AI actions are allowed, blocked, or require approval.",
        DriverState::Missing
    });
}

void FoundationDriverRegistry::installAll()
{
    for (auto& driver : drivers_) {
        driver.state = DriverState::Installed;
    }
}

std::string FoundationDriverRegistry::summary() const
{
    std::size_t installed = 0;
    for (const auto& driver : drivers_) {
        if (driver.state == DriverState::Installed) {
            ++installed;
        }
    }

    std::ostringstream output;
    output << installed << "/" << drivers_.size() << " foundation drivers installed";
    return output.str();
}

std::string FoundationDriverRegistry::detailedReport() const
{
    std::ostringstream output;

    for (const auto& driver : drivers_) {
        output << "  ["
               << (driver.state == DriverState::Installed ? "installed" : "missing")
               << "] " << driver.name << "\n";
        output << "      " << driver.description << "\n";
    }

    return output.str();
}

} // namespace fox
