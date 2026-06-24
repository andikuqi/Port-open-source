#include "fox/ai_kernel.hpp"
#include "fox/command_router.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace fox {

namespace {

// Split a multi-line plan string into individual command lines.
std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    // Trim
    while (!line.empty() &&
           std::isspace(static_cast<unsigned char>(line.front())))
      line.erase(line.begin());
    while (!line.empty() &&
           std::isspace(static_cast<unsigned char>(line.back())))
      line.pop_back();
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return lines;
}

bool isDestructive(CommandType type) {
  return type == CommandType::FsWrite || type == CommandType::FsDelete;
}

} // namespace

AIKernel::AIKernel()
    : running_(false), sandbox_("sandbox"), activePlan_{{}, 0, "", false} {}

KernelResponse AIKernel::boot() {
  if (running_) {
    return {true, "AI Kernel is already running."};
  }

  running_ = true;
  sandbox_.createSandboxDir();
  logSession("[BOOT] AI Kernel booted.");
  return {true, "AI Kernel booted successfully."};
}

KernelResponse AIKernel::shutdown() {
  if (!running_) {
    return {true, "AI Kernel is already stopped."};
  }

  running_ = false;
  logSession("[SHUTDOWN] AI Kernel stopped.");
  return {true, "AI Kernel stopped."};
}

void AIKernel::logSession(const std::string &logText) const {
  std::ofstream logFile("session_history.txt", std::ios::app);
  if (!logFile)
    return;

  time_t now = time(nullptr);
  char timeStr[64];
  struct tm *timeInfo = localtime(&now);
  if (timeInfo) {
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeInfo);
    logFile << "[" << timeStr << "] " << logText << "\n";
  }
}

KernelResponse AIKernel::executeStep() {
  if (!activePlan_.active ||
      activePlan_.currentStep >= activePlan_.steps.size()) {
    activePlan_.active = false;
    return {true, "Plan completed."};
  }

  TaskStep &step = activePlan_.steps[activePlan_.currentStep];
  KernelResponse response = handleCommand(step.command);
  logSession("[PLAN STEP " + std::to_string(activePlan_.currentStep + 1) + "/" +
             std::to_string(activePlan_.steps.size()) + "] " +
             step.command.rawText + " -> " + response.message);

  activePlan_.currentStep++;

  if (activePlan_.currentStep >= activePlan_.steps.size()) {
    activePlan_.active = false;
    return {response.ok,
            response.message + "\n\nPlan complete. All steps executed."};
  }

  // Check if the next step needs confirmation.
  TaskStep &nextStep = activePlan_.steps[activePlan_.currentStep];
  if (isDestructive(nextStep.command.type)) {
    std::ostringstream msg;
    msg << response.message << "\n\n";
    msg << "Step " << (activePlan_.currentStep + 1) << "/"
        << activePlan_.steps.size() << ":\n";
    msg << "  PLAN_CONFIRM_REQUIRED: " << nextStep.command.rawText << "\n";
    msg << "Type 'plan proceed' to continue or 'plan abort' to cancel.";
    return {true, msg.str()};
  }

  // Auto-execute next non-destructive step.
  return executeStep();
}

KernelResponse AIKernel::handleCommand(const Command &command) {
  if (!running_ && command.type != CommandType::Exit) {
    return {false, "AI Kernel is not running."};
  }

  switch (command.type) {
  case CommandType::Help:
    return {
        true,
        "Port OS - Super-Nova 1.0 AI Kernel\n"
        "Available commands:\n"
        "  help                     Show this help message\n"
        "  status                   Show kernel status\n"
        "  fs list [path]           List files in sandbox (alias: ls)\n"
        "  fs read <file>           Read a file (alias: cat)\n"
        "  fs write <file> <text>   Write content to a file\n"
        "  fs delete <file>         Delete a file (moves to Trash)\n"
        "  fs mkdir <dir>           Create a new directory\n"
        "  fs rename <old> <new>    Rename a file or folder\n"
        "  fs trash                 List files in the Trash Bin\n"
        "  fs empty-trash           Permanently empty the Trash Bin\n"
        "  net download <url> <dst> Download a file from the internet\n"
        "  sys execute <path> [arg] Execute a program\n"
        "  plan proceed             Approve the next plan step\n"
        "  plan abort               Cancel the current AI plan\n"
        "  exit                     Shut down Port OS\n"
        "\nOr just type anything naturally — Super-Nova 1.0 will understand."};

  case CommandType::Status:
    return {true, status()};

  case CommandType::FoundationInstall:
    foundationDrivers_.installAll();
    logSession("[CMD] /port ans-install-fd");
    return {
        true,
        "Port foundation installer completed.\n"
        "\n"
        "Installed virtual foundation drivers:\n" +
            foundationDrivers_.detailedReport() +
            "\n"
            "These are Port virtual drivers, not host Windows kernel drivers.\n"
            "Next milestone: bind each driver to real sandbox checks and local "
            "tool detection."};

  case CommandType::Prompt: {
    const std::string &prompt = command.rawText;
    logSession("[ASK] " + prompt);

    std::string plan = aiClient_.requestPlan(prompt);
    logSession("[AI PLAN]\n" + plan);

    if (plan.empty() || plan == "unknown") {
      return {false, "AI could not generate a plan for: " + prompt};
    }

    std::vector<std::string> lines = splitLines(plan);
    if (lines.empty()) {
      return {false, "AI returned an empty plan."};
    }

    // Build new active plan from parsed steps.
    activePlan_.steps.clear();
    activePlan_.currentStep = 0;
    activePlan_.prompt = prompt;
    activePlan_.active = true;

    CommandRouter router;
    for (const auto &line : lines) {
      TaskStep step;
      step.command = router.parse(line);
      step.approved = !isDestructive(step.command.type);
      activePlan_.steps.push_back(step);
    }

    std::ostringstream msg;
    msg << "AI generated a plan with " << activePlan_.steps.size()
        << " step(s):\n";
    for (std::size_t i = 0; i < activePlan_.steps.size(); ++i) {
      msg << "  " << (i + 1) << ". " << activePlan_.steps[i].command.rawText
          << "\n";
    }

    // Execute until the first destructive step or end.
    TaskStep &first = activePlan_.steps[0];
    if (isDestructive(first.command.type)) {
      msg << "\nStep 1/" << activePlan_.steps.size() << ":\n";
      msg << "  PLAN_CONFIRM_REQUIRED: " << first.command.rawText << "\n";
      msg << "Type 'plan proceed' to continue or 'plan abort' to cancel.";
      return {true, msg.str()};
    }

    KernelResponse stepResult = executeStep();
    msg << "\n" << stepResult.message;
    return {stepResult.ok, msg.str()};
  }

  case CommandType::PlanProceed:
    if (!activePlan_.active) {
      return {false, "No active plan to proceed with."};
    }
    logSession("[PLAN PROCEED]");
    return executeStep();

  case CommandType::PlanAbort:
    if (!activePlan_.active) {
      return {false, "No active plan to abort."};
    }
    activePlan_.active = false;
    logSession("[PLAN ABORTED]");
    return {true, "Plan aborted."};

  case CommandType::FsList: {
    bool ok = false;
    std::string listMsg = sandbox_.list(command.arg1, ok);
    return {ok, listMsg};
  }
  case CommandType::FsRead: {
    bool ok = false;
    std::string content = sandbox_.read(command.arg1, ok);
    return {ok, content};
  }
  case CommandType::FsWrite: {
    if (command.arg1.empty()) {
      return {false, "Error: Write command requires a filename."};
    }
    bool ok = sandbox_.write(command.arg1, command.arg2);
    if (ok) {
      return {true, "File '" + command.arg1 + "' written successfully."};
    } else {
      return {false, "Error: Failed to write to file '" + command.arg1 + "'."};
    }
  }
  case CommandType::FsDelete: {
    if (command.arg1.empty()) {
      return {false, "Error: Delete command requires a filename."};
    }
    if (command.arg2 == "yes") {
      bool success = sandbox_.deleteFile(command.arg1);
      if (success) {
        return {true, "File '" + command.arg1 + "' moved to Trash Bin."};
      } else {
        return {false, "Error: Failed to delete file '" + command.arg1 + "'."};
      }
    }
    return {true, "CONFIRM_REQUIRED: " + command.arg1};
  }
  case CommandType::FsMakeDir: {
    if (command.arg1.empty()) {
      return {false, "Error: mkdir requires a directory name."};
    }
    bool ok = sandbox_.makeDir(command.arg1);
    if (ok) {
      return {true, "Directory '" + command.arg1 + "' created."};
    } else {
      return {false, "Error: Failed to create directory '" + command.arg1 + "'."};
    }
  }
  case CommandType::FsRename: {
    if (command.arg1.empty() || command.arg2.empty()) {
      return {false, "Error: rename requires old and new names."};
    }
    bool ok = sandbox_.renameEntry(command.arg1, command.arg2);
    if (ok) {
      return {true, "Renamed '" + command.arg1 + "' to '" + command.arg2 + "'."};
    } else {
      return {false, "Error: Failed to rename '" + command.arg1 + "'."};
    }
  }
  case CommandType::NetDownload: {
    if (command.arg1.empty() || command.arg2.empty()) {
      return {false, "Error: net download requires URL and destination path."};
    }

    // Auto-create destination directory if it doesn't exist to prevent URLDownloadToFile failures
    std::string dest = command.arg2;
    std::size_t lastSlash = dest.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        std::string dirPath = dest.substr(0, lastSlash);
        // Strip sandbox prefix if present since sandbox_.makeDir prepends it
        if (dirPath.rfind("sandbox/", 0) == 0) {
            dirPath = dirPath.substr(8);
        } else if (dirPath.rfind("sandbox\\", 0) == 0) {
            dirPath = dirPath.substr(8);
        } else if (dirPath == "sandbox") {
            dirPath = "";
        }
        if (!dirPath.empty()) {
            sandbox_.makeDir(dirPath);
        }
    }
#ifdef _WIN32
    HMODULE hUrlmon = LoadLibraryA("urlmon.dll");
    if (hUrlmon) {
      typedef HRESULT(WINAPI* URLDownloadToFileA_t)(LPUNKNOWN, LPCSTR, LPCSTR, DWORD, LPVOID);
      URLDownloadToFileA_t fnURLDownloadToFileA = reinterpret_cast<URLDownloadToFileA_t>(GetProcAddress(hUrlmon, "URLDownloadToFileA"));
      if (fnURLDownloadToFileA) {
        HRESULT hr = fnURLDownloadToFileA(nullptr, command.arg1.c_str(), command.arg2.c_str(), 0, nullptr);
        FreeLibrary(hUrlmon);
        if (hr == S_OK) {
          return {true, "Successfully downloaded " + command.arg1 + " to " + command.arg2};
        } else {
          return {false, "Error: Download failed with HRESULT " + std::to_string(hr)};
        }
      }
      FreeLibrary(hUrlmon);
    }
    return {false, "Error: Failed to load urlmon.dll or locate URLDownloadToFileA"};
#else
    std::string sysCmd = "curl -L -o \"" + command.arg2 + "\" \"" + command.arg1 + "\"";
    int ret = std::system(sysCmd.c_str());
    if (ret == 0) {
      return {true, "Successfully downloaded " + command.arg1 + " to " + command.arg2};
    } else {
      return {false, "Error: curl download failed with exit code " + std::to_string(ret)};
    }
#endif
  }
  case CommandType::SysExecute: {
    if (command.arg1.empty()) {
      return {false, "Error: sys execute requires program path."};
    }
#ifdef _WIN32
    HINSTANCE hInst = ShellExecuteA(nullptr, "open", command.arg1.c_str(), command.arg2.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hInst) > 32) {
      return {true, "Successfully executed " + command.arg1 + " " + command.arg2};
    } else {
      return {false, "Error: ShellExecute failed with code " + std::to_string(reinterpret_cast<INT_PTR>(hInst))};
    }
#else
    std::string chmodCmd = "chmod +x \"" + command.arg1 + "\"";
    std::system(chmodCmd.c_str());
    std::string sysCmd = "\"" + command.arg1 + "\" " + command.arg2;
    int ret = std::system(sysCmd.c_str());
    if (ret == 0) {
      return {true, "Successfully executed " + command.arg1 + " " + command.arg2};
    } else {
      return {false, "Error: execution failed with exit code " + std::to_string(ret)};
    }
#endif
  }
  case CommandType::Exit:
    return shutdown();
  case CommandType::Unknown: {
    // Check for inline trash commands.
    std::string raw = command.rawText;
    std::transform(raw.begin(), raw.end(), raw.begin(), ::tolower);
    if (raw == "fs trash" || raw == "fs list-trash") {
      bool ok = false;
      return {ok, sandbox_.listTrash(ok)};
    }
    if (raw == "fs empty-trash") {
      bool ok = sandbox_.emptyTrash();
      return {ok, ok ? "Trash Bin emptied successfully."
                     : "Failed to empty Trash Bin."};
    }
    return {false, "Unknown command. Type 'help' to see available commands."};
  }
  }

  return {false, "Unhandled command."};
}

bool AIKernel::isRunning() const { return running_; }

fox::Sandbox &AIKernel::getSandbox() { return sandbox_; }

std::string AIKernel::status() const {
  std::ostringstream output;
  output << "Port OS - Super-Nova 1.0\n";
  output << "  AI Kernel: " << (running_ ? "running" : "stopped") << '\n';
  output << "  Foundation: " << foundationDrivers_.summary() << '\n';
  output << "  Sandbox: sandbox directory active\n";
  output << "  AI Planner: "
         << (activePlan_.active
                 ? "plan active (" + std::to_string(activePlan_.steps.size()) +
                       " steps)"
                 : "idle")
         << '\n';
  output << "  Engine: Super-Nova 1.0 (Gemini-backed)\n";
  output << "  Network: ready";
  return output.str();
}

} // namespace fox
