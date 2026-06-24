#pragma once

#include "fox/command.hpp"
#include "fox/foundation_driver.hpp"
#include "fox/sandbox.hpp"

#include <string>
#include <vector>
#include "fox/ai_client.hpp"

namespace fox {

struct KernelResponse {
    bool ok;
    std::string message;
};

struct TaskStep {
    Command command;
    bool approved;
};

struct TaskPlan {
    std::vector<TaskStep> steps;
    std::size_t currentStep;
    std::string prompt;
    bool active;
};

class AIKernel {
public:
    AIKernel();

    KernelResponse boot();
    KernelResponse shutdown();
    KernelResponse handleCommand(const Command& command);

    bool isRunning() const;
    std::string status() const;
    fox::Sandbox& getSandbox();

private:
    bool running_;
    FoundationDriverRegistry foundationDrivers_;
    Sandbox sandbox_;
    AIClient aiClient_;
    TaskPlan activePlan_;

    void logSession(const std::string& logText) const;
    KernelResponse executeStep();
};

} // namespace fox
