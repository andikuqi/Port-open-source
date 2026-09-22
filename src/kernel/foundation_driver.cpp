#include "port/kernel/foundation_driver.hpp"

#include <sstream>

namespace port::kernel {

FoundationDriverRegistry::FoundationDriverRegistry() {
    drivers_ = {
        {"driver.registry",   "System Driver Registry",         "Tracks virtual foundation drivers.",                              DriverState::Missing},
        {"toolchain.compiler","Compiler Toolchain Detector",    "Detects and validates the local compiler.",                       DriverState::Missing},
        {"sandbox.filesystem","Sandbox Filesystem Driver",      "Controls isolated virtual filesystem access for AI tasks.",       DriverState::Missing},
        {"network.controller","Network Access Controller",      "Provides controlled network access for search and downloads.",    DriverState::Missing},
        {"ai.policy",         "AI Execution Policy Engine",     "Decides which AI actions are allowed, blocked, or need approval.",DriverState::Missing},
        {"event.bus",         "Event Bus Driver",               "Routes typed events between Port OS subsystems.",                 DriverState::Missing},
        {"metrics.collector", "Metrics Collector",              "Collects performance and usage telemetry.",                       DriverState::Missing},
    };
}

void FoundationDriverRegistry::installAll() {
    for (auto& d : drivers_) {
        d.state = DriverState::Installed;
    }
}

std::string FoundationDriverRegistry::summary() const {
    std::size_t installed = 0;
    for (const auto& d : drivers_) {
        if (d.state == DriverState::Installed) ++installed;
    }
    std::ostringstream oss;
    oss << installed << '/' << drivers_.size() << " foundation drivers installed";
    return oss.str();
}

std::string FoundationDriverRegistry::detailedReport() const {
    std::ostringstream oss;
    for (const auto& d : drivers_) {
        oss << "  [" << (d.state == DriverState::Installed ? "installed" : "missing ")
            << "] " << d.name << "\n"
            << "        " << d.description << "\n";
    }
    return oss.str();
}

} // namespace port::kernel
