#pragma once

#include "port/kernel/command.hpp"
#include "port/security/sandbox.hpp"
#include "port/security/audit_log.hpp"
#include "port/storage/session_store.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace port::kernel {

struct KernelResponse {
    bool ok{false};
    std::string message;
    bool requiresConfirmation{false};
};

struct TaskStep {
    Command command;
    bool approved{false};
};

struct TaskPlan {
    std::vector<TaskStep> steps;
    std::size_t currentStep{0};
    std::string prompt;
    bool active{false};
};

// Forward declarations for optional AI integration
class IAIAdapter;
class FoundationDriverRegistry;

// AI Kernel: central command dispatcher.
// Owns the sandbox, session store, and optional AI adapter.
// Thread-compatible: external callers must serialize access.
class AIKernel {
public:
    struct Config {
        std::string sandboxPath;
        std::string sessionLogPath;
        std::string auditLogPath;
        Config()
            : sandboxPath("sandbox")
            , sessionLogPath("session_history.txt")
            , auditLogPath("sandbox_operations.log") {}
    };

    explicit AIKernel(Config config = Config{});
    ~AIKernel();

    KernelResponse boot();
    KernelResponse shutdown();
    KernelResponse handleCommand(const Command& command);
    KernelResponse confirmPendingCommand();
    KernelResponse cancelPendingCommand();

    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] std::string status() const;

    // Direct sandbox access for desktop integration
    [[nodiscard]] port::security::Sandbox& getSandbox() noexcept;
    [[nodiscard]] const port::security::Sandbox& getSandbox() const noexcept;

    // Inject AI adapter (optional; falls back to offline plan if null)
    void setAIAdapter(std::shared_ptr<IAIAdapter> adapter);

private:
    KernelResponse executeNextStep();
    void buildPlan(const std::string& prompt, const std::string& planText);
    void log(std::string_view tag, std::string_view content);

    Config config_;
    bool running_{false};
    port::security::Sandbox sandbox_;
    std::unique_ptr<port::storage::SessionStore> sessionStore_;
    std::unique_ptr<FoundationDriverRegistry> drivers_;
    std::shared_ptr<IAIAdapter> aiAdapter_;
    TaskPlan activePlan_;
    std::optional<Command> pendingCommand_;
    bool approvalBypass_{false};
};

// Adapter interface: bridges kernel → AI layer
struct IAIAdapter {
    virtual ~IAIAdapter() = default;
    // Returns a newline-separated list of commands (the plan)
    [[nodiscard]] virtual std::string requestPlan(const std::string& prompt) = 0;
    [[nodiscard]] virtual std::string lastError() const { return {}; }
};

} // namespace port::kernel
