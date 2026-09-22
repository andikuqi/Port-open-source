#include "port/ai/prompt_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace port::ai {

namespace {

std::string normalizePlanLine(std::string line) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
        line.erase(line.begin());
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
        line.pop_back();
    if (line == "```" || line.rfind("```", 0) == 0) return {};
    if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0)
        line.erase(0, 2);
    std::size_t i = 0;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
    if (i > 0 && i < line.size() && (line[i] == '.' || line[i] == ')')) {
        ++i;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        line.erase(0, i);
    }
    return line;
}

// Offline fallback plan generator (Albanian + English keywords)
PlanResult offlinePlanFallback(const std::string& prompt) {
    std::string p = prompt;
    std::transform(p.begin(), p.end(), p.begin(), ::tolower);

    struct AppEntry {
        const char* keyword;
        const char* urlWin;
        const char* destWin;
    };
    static constexpr AppEntry kApps[] = {
        {"chrome",       "https://dl.google.com/chrome/install/chrome_installer.exe", "sandbox/Apps/chrome_installer.exe"},
        {"firefox",      "https://download.mozilla.org/?product=firefox-latest&os=win64&lang=en-US", "sandbox/Apps/firefox_installer.exe"},
        {"vlc",          "https://get.videolan.org/vlc/last/win64/vlc-latest-win64.exe", "sandbox/Apps/vlc_installer.exe"},
        {"7-zip",        "https://www.7-zip.org/a/7z2409-x64.exe", "sandbox/Apps/7zip_installer.exe"},
        {"7zip",         "https://www.7-zip.org/a/7z2409-x64.exe", "sandbox/Apps/7zip_installer.exe"},
        {"notepad++",    "https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v8.7/npp.8.7.Installer.x64.exe", "sandbox/Apps/notepadpp_installer.exe"},
        {"vscode",       "https://code.visualstudio.com/sha/download?build=stable&os=win32-x64-user", "sandbox/Apps/vscode_installer.exe"},
        {"visual studio code", "https://code.visualstudio.com/sha/download?build=stable&os=win32-x64-user", "sandbox/Apps/vscode_installer.exe"},
        {"discord",      "https://discord.com/api/downloads/distributions/app/installers/latest?channel=stable&platform=win&arch=x86", "sandbox/Apps/discord_installer.exe"},
        {"zoom",         "https://zoom.us/client/latest/ZoomInstallerFull.exe", "sandbox/Apps/zoom_installer.exe"},
        {nullptr, nullptr, nullptr}
    };

    bool isInstall = p.find("download") != std::string::npos
                  || p.find("install")  != std::string::npos
                  || p.find("shkarko")  != std::string::npos
                  || p.find("instalo")  != std::string::npos;

    if (isInstall) {
        for (int i = 0; kApps[i].keyword; ++i) {
            if (p.find(kApps[i].keyword) != std::string::npos) {
                PlanResult res;
                res.fromCache = false;
                res.rawResponse = std::string{"fs mkdir Apps\nnet download "}
                    + kApps[i].urlWin + " " + kApps[i].destWin
                    + "\nsys execute " + kApps[i].destWin;
                // Parse into steps
                std::istringstream ss{res.rawResponse};
                std::string ln;
                while (std::getline(ss, ln)) {
                    if (ln.empty()) continue;
                    bool destructive = ln.rfind("fs write", 0) == 0
                                    || ln.rfind("fs delete", 0) == 0
                                    || ln.rfind("sys execute", 0) == 0;
                    res.steps.push_back({ln, destructive});
                }
                return res;
            }
        }
    }

    // Generic fallback: just list files
    PlanResult res;
    res.fromCache = false;
    res.rawResponse = "fs list";
    res.steps.push_back({"fs list", false});
    return res;
}

} // namespace

// ---------- PromptRuntime ----------

PromptRuntime::PromptRuntime(std::shared_ptr<IAIProvider> provider,
                              std::shared_ptr<ContextEngine> context,
                              Config config)
    : provider_{std::move(provider)}
    , context_{std::move(context)}
    , config_{std::move(config)}
    , offlinePlanner_{offlinePlanFallback}
{}

void PromptRuntime::setOfflinePlan(std::function<PlanResult(const std::string&)> fn) {
    offlinePlanner_ = std::move(fn);
}

std::expected<PlanResult, AIError> PromptRuntime::submitSync(const std::string& userPrompt) {
    return execute(userPrompt);
}

PlanFuture PromptRuntime::submitAsync(std::string userPrompt) {
    return std::async(std::launch::async, [this, p = std::move(userPrompt)]() {
        return execute(p);
    });
}

std::expected<PlanResult, AIError> PromptRuntime::execute(const std::string& userPrompt) {
    // Build context-aware message list
    std::vector<Message> messages;
    if (context_) {
        messages = context_->addUserMessage(userPrompt);
    } else {
        messages.push_back({Message::Role::System, config_.systemInstruction});
        messages.push_back({Message::Role::User, userPrompt});
    }

    // Try online provider
    if (provider_ && provider_->isAvailable()) {
        PromptRequest req;
        req.messages = messages;
        req.systemInstruction = config_.systemInstruction;

        AIResult result = std::unexpected(
            AIError{AIError::Code::Offline, "Provider was not called"});
        const int attempts = std::max(1, config_.maxRetries + 1);
        for (int attempt = 0; attempt < attempts; ++attempt) {
            result = provider_->complete(req);
            if (result) break;
            if (result.error().code == AIError::Code::AuthFailure ||
                result.error().code == AIError::Code::QuotaExceeded) break;
        }
        if (result) {
            const std::string& raw = result->content;
            if (context_) context_->addAssistantMessage(raw);

            PlanResult plan;
            plan.rawResponse = raw;

            std::istringstream ss{raw};
            std::string line;
            while (std::getline(ss, line)) {
                line = normalizePlanLine(std::move(line));
                if (line.empty() || line == "unknown") continue;
                bool destructive = line.rfind("fs write",  0) == 0
                                || line.rfind("fs delete", 0) == 0
                                || line.rfind("sys execute", 0) == 0;
                plan.steps.push_back({line, destructive});
            }
            if (!plan.steps.empty()) return plan;
        }
        // Provider returned error — fall through to offline if enabled
        if (!config_.offlineFallback) {
            return std::unexpected(result.error());
        }
    }

    // Offline fallback
    if (offlinePlanner_) {
        return offlinePlanner_(userPrompt);
    }

    return std::unexpected(AIError{AIError::Code::Offline, "No provider and no offline planner"});
}

} // namespace port::ai
