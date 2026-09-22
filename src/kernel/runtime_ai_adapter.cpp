#include "port/kernel/runtime_ai_adapter.hpp"
#include "port/kernel/command_router.hpp"

#include <sstream>

namespace port::kernel {

RuntimeAIAdapter::RuntimeAIAdapter(
    std::shared_ptr<port::ai::IAIProvider> provider,
    std::shared_ptr<port::ai::ContextEngine> context,
    port::ai::PromptRuntime::Config config)
    : runtime_{std::move(provider), std::move(context), std::move(config)}
{}

std::string RuntimeAIAdapter::requestPlan(const std::string& prompt) {
    auto result = runtime_.submitSync(prompt);
    if (!result) {
        std::lock_guard lock{errorMutex_};
        lastError_ = result.error().detail;
        return {};
    }
    {
        std::lock_guard lock{errorMutex_};
        lastError_.clear();
    }

    std::ostringstream plan;
    CommandRouter router;
    for (const auto& step : result->steps) {
        if (step.rawCommand.empty()) continue;
        const auto command = router.parse(step.rawCommand);
        switch (command.type) {
        case CommandType::FsList:
        case CommandType::FsRead:
        case CommandType::FsWrite:
        case CommandType::FsDelete:
        case CommandType::FsMakeDir:
        case CommandType::FsRename:
        case CommandType::FsTrash:
        case CommandType::FsEmptyTrash:
        case CommandType::NetDownload:
        case CommandType::SysExecute:
            plan << command.rawText << '\n';
            break;
        default:
            break;
        }
    }
    return plan.str();
}

std::string RuntimeAIAdapter::lastError() const {
    std::lock_guard lock{errorMutex_};
    return lastError_;
}

} // namespace port::kernel
