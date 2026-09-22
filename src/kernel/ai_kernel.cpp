#include "port/kernel/ai_kernel.hpp"
#include "port/kernel/command_router.hpp"
#include "port/kernel/foundation_driver.hpp"
#include "port/security/audit_log.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <urlmon.h>
#endif

#include <algorithm>
#include <cctype>
#include <sstream>

namespace port::kernel {

namespace {

std::filesystem::path sandboxRelativePath(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.rfind("sandbox/", 0) == 0) value.erase(0, 8);
    if (value == "sandbox") value.clear();
    return std::filesystem::path{value};
}

bool isHttpsUrl(const std::string& value) {
    return value.rfind("https://", 0) == 0 &&
           value.find_first_of("\r\n") == std::string::npos;
}

} // namespace

// ---------- AIKernel ----------

AIKernel::AIKernel(Config config)
    : config_{std::move(config)}
    , sandbox_{config_.sandboxPath}
    , sessionStore_{std::make_unique<port::storage::SessionStore>(config_.sessionLogPath)}
    , drivers_{std::make_unique<FoundationDriverRegistry>()}
{
    port::security::AuditLog::instance().open(config_.auditLogPath);
}

AIKernel::~AIKernel() {
    if (running_) shutdown();
}

void AIKernel::setAIAdapter(std::shared_ptr<IAIAdapter> adapter) {
    aiAdapter_ = std::move(adapter);
}

void AIKernel::log(std::string_view tag, std::string_view content) {
    sessionStore_->append(tag, content);
}

KernelResponse AIKernel::boot() {
    if (running_) return {true, "AI Kernel is already running."};
    running_ = true;
    sandbox_.initialize();
    log("BOOT", "AI Kernel booted.");
    port::security::AuditLog::instance().record(
        port::security::AuditAction::KernelBoot, "port-os", true);
    return {true, "AI Kernel booted successfully."};
}

KernelResponse AIKernel::shutdown() {
    if (!running_) return {true, "AI Kernel is already stopped."};
    running_ = false;
    log("SHUTDOWN", "AI Kernel stopped.");
    port::security::AuditLog::instance().record(
        port::security::AuditAction::KernelShutdown, "port-os", true);
    return {true, "AI Kernel stopped."};
}

bool AIKernel::isRunning() const noexcept { return running_; }

port::security::Sandbox& AIKernel::getSandbox() noexcept { return sandbox_; }
const port::security::Sandbox& AIKernel::getSandbox() const noexcept { return sandbox_; }

std::string AIKernel::status() const {
    std::ostringstream oss;
    oss << "Port OS - Super-Nova 2.0\n"
        << "  AI Kernel  : " << (running_ ? "running" : "stopped") << '\n'
        << "  Foundation : " << drivers_->summary() << '\n'
        << "  Sandbox    : " << sandbox_.root().string() << '\n'
        << "  AI Planner : "
        << (activePlan_.active
               ? "active (" + std::to_string(activePlan_.steps.size()) + " steps)"
               : "idle")
        << '\n'
        << "  Engine     : Super-Nova 2.0\n"
        << "  AI Provider: " << (aiAdapter_ ? "connected" : "offline fallback");
    return oss.str();
}

void AIKernel::buildPlan(const std::string& prompt, const std::string& planText) {
    activePlan_ = {};
    activePlan_.prompt = prompt;
    activePlan_.active = true;

    CommandRouter router;
    std::istringstream ss{planText};
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
            line.erase(line.begin());
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (line.empty() || line == "unknown") continue;
        TaskStep step;
        step.command  = router.parse(line);
        step.approved = !isDestructive(step.command.type);
        activePlan_.steps.push_back(std::move(step));
    }
}

KernelResponse AIKernel::executeNextStep() {
    if (!activePlan_.active || activePlan_.currentStep >= activePlan_.steps.size()) {
        activePlan_.active = false;
        return {true, "Plan completed."};
    }

    auto& step = activePlan_.steps[activePlan_.currentStep];
    if (isDestructive(step.command.type) && !step.approved) {
        std::ostringstream msg;
        msg << "PLAN_CONFIRM_REQUIRED: " << step.command.rawText << '\n'
            << "Type 'plan proceed' to continue or 'plan abort' to cancel.";
        return {true, msg.str(), true};
    }

    const bool previousBypass = approvalBypass_;
    approvalBypass_ = step.approved;
    if (step.command.type == CommandType::FsDelete && step.approved) {
        step.command.arg2 = "yes";
    }
    KernelResponse resp = handleCommand(step.command);
    approvalBypass_ = previousBypass;

    log("PLAN STEP " + std::to_string(activePlan_.currentStep + 1) + '/' +
        std::to_string(activePlan_.steps.size()),
        step.command.rawText + " -> " + resp.message);

    ++activePlan_.currentStep;

    if (activePlan_.currentStep >= activePlan_.steps.size()) {
        activePlan_.active = false;
        resp.message += "\n\nPlan complete. All steps executed.";
        return resp;
    }

    // If the next step is destructive, pause and request confirmation
    const auto& next = activePlan_.steps[activePlan_.currentStep];
    if (isDestructive(next.command.type)) {
        std::ostringstream msg;
        msg << resp.message << "\n\nStep " << (activePlan_.currentStep + 1)
            << '/' << activePlan_.steps.size() << ":\n"
            << "  PLAN_CONFIRM_REQUIRED: " << next.command.rawText << '\n'
            << "Type 'plan proceed' to continue or 'plan abort' to cancel.";
        resp.message = msg.str();
        resp.requiresConfirmation = true;
        return resp;
    }

    return executeNextStep();
}

KernelResponse AIKernel::handleCommand(const Command& cmd) {
    if (!running_ && cmd.type != CommandType::Exit) {
        return {false, "AI Kernel is not running."};
    }

    if (isDestructive(cmd.type) && !approvalBypass_) {
        pendingCommand_ = cmd;
        return {true, "CONFIRM_REQUIRED: " + cmd.rawText, true};
    }

    switch (cmd.type) {

    case CommandType::Help:
        return {true,
            "Port OS - Super-Nova 2.0 AI Kernel\n"
            "Commands:\n"
            "  help                     Show this help\n"
            "  status                   Kernel status\n"
            "  fs list [path]           List files (alias: ls)\n"
            "  fs read <file>           Read a file (alias: cat)\n"
            "  fs write <file> <text>   Write to a file\n"
            "  fs delete <file>         Delete (moves to Trash)\n"
            "  fs mkdir <dir>           Create directory\n"
            "  fs rename <old> <new>    Rename\n"
            "  fs trash                 List Trash Bin\n"
            "  fs empty-trash           Empty Trash Bin\n"
            "  net download <url> <dst> Download a file\n"
            "  sys execute <path> [arg] Execute a program\n"
            "  plan proceed             Approve next plan step\n"
            "  plan abort               Cancel current plan\n"
            "  exit                     Shut down Port OS\n"
            "\nOr just type naturally — Super-Nova 2.0 will understand."};

    case CommandType::Status:
        return {true, status()};

    case CommandType::FoundationInstall:
        drivers_->installAll();
        log("CMD", "/port ans-install-fd");
        return {true,
            "Foundation drivers installed.\n\n" + drivers_->detailedReport() +
            "\nThese are Port virtual drivers."};

    case CommandType::Prompt: {
        const std::string& prompt = cmd.rawText;
        log("ASK", prompt);

        std::string planText;
        std::string providerWarning;
        if (aiAdapter_) {
            planText = aiAdapter_->requestPlan(prompt);
            if (planText.empty()) providerWarning = aiAdapter_->lastError();
        }

        // Offline fallback if no adapter or empty result
        if (planText.empty() || planText == "unknown") {
            // Simple keyword-based offline planner
            std::string p = prompt;
            std::transform(p.begin(), p.end(), p.begin(), ::tolower);
            if (p.find("list") != std::string::npos || p.find("ls") != std::string::npos)
                planText = "fs list";
            else
                planText = "fs list";
        }

        log("AI PLAN", planText);
        buildPlan(prompt, planText);

        if (activePlan_.steps.empty()) {
            activePlan_.active = false;
            return {false, "AI returned an empty plan."};
        }

        std::ostringstream msg;
        msg << "Plan with " << activePlan_.steps.size() << " step(s):\n";
        for (std::size_t i = 0; i < activePlan_.steps.size(); ++i) {
            msg << "  " << (i + 1) << ". " << activePlan_.steps[i].command.rawText << '\n';
        }
        if (!providerWarning.empty()) {
            msg << "\nOnline AI unavailable; using offline fallback: "
                << providerWarning << '\n';
        }

        const auto& first = activePlan_.steps[0];
        if (isDestructive(first.command.type)) {
            msg << "\nStep 1/" << activePlan_.steps.size() << ":\n"
                << "  PLAN_CONFIRM_REQUIRED: " << first.command.rawText << '\n'
                << "Type 'plan proceed' to continue or 'plan abort' to cancel.";
            return {true, msg.str(), true};
        }

        auto stepResp = executeNextStep();
        msg << '\n' << stepResp.message;
        return {stepResp.ok, msg.str(), stepResp.requiresConfirmation};
    }

    case CommandType::PlanProceed:
        if (!activePlan_.active) return {false, "No active plan to proceed with."};
        log("PLAN", "proceed");
        activePlan_.steps[activePlan_.currentStep].approved = true;
        return executeNextStep();

    case CommandType::PlanAbort:
        if (!activePlan_.active) return {false, "No active plan to abort."};
        activePlan_.active = false;
        log("PLAN", "aborted");
        return {true, "Plan aborted."};

    case CommandType::FsList: {
        auto res = sandbox_.list(cmd.arg1);
        if (!res) return {false, "Error: " + res.error().detail};
        std::ostringstream oss;
        oss << "sandbox/" << cmd.arg1 << ":\n";
        for (const auto& e : *res) {
            oss << "  " << e.name;
            if (e.isDirectory) oss << '/';
            oss << '\n';
        }
        if (res->empty()) oss << "  (empty)\n";
        port::security::AuditLog::instance().record(
            port::security::AuditAction::FsRead, cmd.arg1, true);
        return {true, oss.str()};
    }

    case CommandType::FsRead: {
        if (cmd.arg1.empty()) return {false, "fs read requires a filename."};
        auto res = sandbox_.read(cmd.arg1);
        if (!res) return {false, "Error: " + res.error().detail};
        port::security::AuditLog::instance().record(
            port::security::AuditAction::FsRead, cmd.arg1, true);
        return {true, *res};
    }

    case CommandType::FsWrite: {
        if (cmd.arg1.empty()) return {false, "fs write requires a filename."};
        auto res = sandbox_.write(cmd.arg1, cmd.arg2);
        if (!res) return {false, "Error: " + res.error().detail};
        port::security::AuditLog::instance().record(
            port::security::AuditAction::FsWrite, cmd.arg1, true);
        return {true, "File '" + cmd.arg1 + "' written."};
    }

    case CommandType::FsDelete: {
        if (cmd.arg1.empty()) return {false, "fs delete requires a filename."};
        if (cmd.arg2 == "yes") {
            auto res = sandbox_.moveToTrash(cmd.arg1);
            if (!res) return {false, "Error: " + res.error().detail};
            port::security::AuditLog::instance().record(
                port::security::AuditAction::FsTrash, cmd.arg1, true);
            return {true, "'" + cmd.arg1 + "' moved to Trash."};
        }
        return {true, "CONFIRM_REQUIRED: " + cmd.arg1, true};
    }

    case CommandType::FsMakeDir: {
        if (cmd.arg1.empty()) return {false, "fs mkdir requires a directory name."};
        auto res = sandbox_.makeDir(cmd.arg1);
        if (!res) return {false, "Error: " + res.error().detail};
        return {true, "Directory '" + cmd.arg1 + "' created."};
    }

    case CommandType::FsRename: {
        if (cmd.arg1.empty() || cmd.arg2.empty())
            return {false, "fs rename requires old and new names."};
        auto res = sandbox_.rename(cmd.arg1, cmd.arg2);
        if (!res) return {false, "Error: " + res.error().detail};
        return {true, "Renamed '" + cmd.arg1 + "' → '" + cmd.arg2 + "'."};
    }

    case CommandType::FsTrash: {
        auto res = sandbox_.listTrash();
        if (!res) return {false, "Error: " + res.error().detail};
        if (res->empty()) return {true, "Trash Bin is empty."};
        std::ostringstream oss;
        oss << "Trash Bin contents:\n";
        for (const auto& e : *res) {
            oss << "  " << e.name;
            if (e.isDirectory) oss << '/';
            oss << '\n';
        }
        return {true, oss.str()};
    }

    case CommandType::FsEmptyTrash: {
        auto res = sandbox_.emptyTrash();
        if (!res) return {false, "Error: " + res.error().detail};
        return {true, "Trash Bin emptied."};
    }

    case CommandType::NetDownload: {
        if (cmd.arg1.empty() || cmd.arg2.empty())
            return {false, "net download requires URL and destination."};
        if (!isHttpsUrl(cmd.arg1))
            return {false, "Only HTTPS downloads are allowed."};

        const auto relativeDestination = sandboxRelativePath(cmd.arg2);
        if (relativeDestination.empty())
            return {false, "Download destination must be a sandbox file."};
        if (const auto parent = relativeDestination.parent_path(); !parent.empty()) {
            auto made = sandbox_.makeDir(parent);
            if (!made) return {false, "Error: " + made.error().detail};
        }
        auto destination = sandbox_.resolvePath(relativeDestination);
        if (!destination) return {false, "Error: " + destination.error().detail};

        log("NET_DOWNLOAD", cmd.arg1 + " -> " + cmd.arg2);

#ifdef _WIN32
        const HRESULT hr = URLDownloadToFileA(
            nullptr, cmd.arg1.c_str(), destination->string().c_str(), 0, nullptr);
        port::security::AuditLog::instance().record(
            port::security::AuditAction::NetDownload,
            cmd.arg1 + " -> " + relativeDestination.string(), hr == S_OK);
        if (hr == S_OK)
            return {true, "Downloaded into sandbox/" + relativeDestination.string()};
        return {false, "Download failed (HRESULT " + std::to_string(hr) + ")"};
#else
        return {false, "Secure downloads are not implemented on this platform."};
#endif
    }

    case CommandType::SysExecute: {
        if (cmd.arg1.empty()) return {false, "sys execute requires a program path."};

        const auto relativeProgram = sandboxRelativePath(cmd.arg1);
        auto program = sandbox_.resolvePath(relativeProgram);
        if (!program) return {false, "Error: " + program.error().detail};
        std::error_code fileError;
        if (!std::filesystem::is_regular_file(*program, fileError))
            return {false, "Program does not exist inside the sandbox."};
        std::string extension = program->extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
#ifdef _WIN32
        if (extension != ".exe")
            return {false, "Only .exe programs inside the sandbox may be executed."};
#endif
        if (cmd.arg2.find_first_of("\r\n") != std::string::npos)
            return {false, "Invalid program arguments."};

        log("SYS_EXECUTE", cmd.arg1 + ' ' + cmd.arg2);

#ifdef _WIN32
        HINSTANCE hInst = ShellExecuteA(
            nullptr, "open",
            program->string().c_str(),
            cmd.arg2.empty() ? nullptr : cmd.arg2.c_str(),
            sandbox_.root().string().c_str(), SW_SHOWNORMAL);
        const bool success = reinterpret_cast<INT_PTR>(hInst) > 32;
        port::security::AuditLog::instance().record(
            port::security::AuditAction::SysExecute,
            relativeProgram.string() + ' ' + cmd.arg2, success);
        if (success)
            return {true, "Launched: sandbox/" + relativeProgram.string()};
        return {false, "ShellExecute failed (code " +
                std::to_string(reinterpret_cast<INT_PTR>(hInst)) + ")"};
#else
        return {false, "Secure program execution is not implemented on this platform."};
#endif
    }

    case CommandType::Exit:
        return shutdown();

    case CommandType::Unknown:
    default:
        return {false, "Unknown command. Type 'help' for available commands."};
    }
}

KernelResponse AIKernel::confirmPendingCommand() {
    if (!pendingCommand_) return {false, "No command is waiting for confirmation."};
    Command command = std::move(*pendingCommand_);
    pendingCommand_.reset();
    if (command.type == CommandType::FsDelete) command.arg2 = "yes";
    const bool previousBypass = approvalBypass_;
    approvalBypass_ = true;
    auto response = handleCommand(command);
    approvalBypass_ = previousBypass;
    return response;
}

KernelResponse AIKernel::cancelPendingCommand() {
    if (!pendingCommand_) return {false, "No command is waiting for confirmation."};
    pendingCommand_.reset();
    return {true, "Command cancelled."};
}

} // namespace port::kernel
 
