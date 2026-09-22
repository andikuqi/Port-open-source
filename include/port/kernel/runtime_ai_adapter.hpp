#pragma once

#include "port/ai/prompt_runtime.hpp"
#include "port/kernel/ai_kernel.hpp"

#include <memory>
#include <mutex>

namespace port::kernel {

// Bridges the provider/context runtime to the kernel's plan interface.
class RuntimeAIAdapter final : public IAIAdapter {
public:
    RuntimeAIAdapter(std::shared_ptr<port::ai::IAIProvider> provider,
                     std::shared_ptr<port::ai::ContextEngine> context,
                     port::ai::PromptRuntime::Config config);

    [[nodiscard]] std::string requestPlan(const std::string& prompt) override;
    [[nodiscard]] std::string lastError() const override;

private:
    port::ai::PromptRuntime runtime_;
    mutable std::mutex errorMutex_;
    std::string lastError_;
};

} // namespace port::kernel
