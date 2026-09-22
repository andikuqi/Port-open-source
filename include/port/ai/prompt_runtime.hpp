#pragma once

#include "port/ai/context_engine.hpp"
#include "port/ai/provider.hpp"
#include "port/ai/tool_executor.hpp"

#include <expected>
#include <functional>
#include <future>
#include <string>
#include <vector>

namespace port::ai {

// Async result of a prompt invocation
struct PlanStep {
    std::string rawCommand;
    bool requiresApproval{false};
};

struct PlanResult {
    std::vector<PlanStep> steps;
    std::string rawResponse;
    bool fromCache{false};
};

using PlanFuture = std::future<std::expected<PlanResult, AIError>>;

// Orchestrates: context → provider → tool parsing → plan steps.
// All network I/O runs on a dedicated worker thread; the caller
// gets a future and is never blocked.
class PromptRuntime {
public:
    struct Config {
        std::string systemInstruction;
        bool offlineFallback;
        int maxRetries;
        Config() : offlineFallback(true), maxRetries(2) {}
    };

    explicit PromptRuntime(std::shared_ptr<IAIProvider> provider,
                           std::shared_ptr<ContextEngine> context,
                           Config config = Config{});

    // Submit a user prompt asynchronously
    [[nodiscard]] PlanFuture submitAsync(std::string userPrompt);

    // Synchronous version for terminal/test use
    [[nodiscard]] std::expected<PlanResult, AIError> submitSync(const std::string& userPrompt);

    void setOfflinePlan(std::function<PlanResult(const std::string&)> fn);

private:
    [[nodiscard]] std::expected<PlanResult, AIError>
        execute(const std::string& userPrompt);

    std::shared_ptr<IAIProvider> provider_;
    std::shared_ptr<ContextEngine> context_;
    Config config_;
    std::function<PlanResult(const std::string&)> offlinePlanner_;
};

} // namespace port::ai
