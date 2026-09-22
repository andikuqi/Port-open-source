#include "port/ai/tool_executor.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>

namespace port::ai {

// ---------- ToolRegistry ----------

ToolRegistry& ToolRegistry::instance() {
    static ToolRegistry reg;
    return reg;
}

void ToolRegistry::registerTool(ToolDefinition def) {
    std::lock_guard lock{mutex_};
    tools_[def.name] = std::move(def);
}

void ToolRegistry::unregisterTool(const std::string& name) {
    std::lock_guard lock{mutex_};
    tools_.erase(name);
}

bool ToolRegistry::hasTool(const std::string& name) const {
    std::lock_guard lock{mutex_};
    return tools_.count(name) > 0;
}

ToolResult ToolRegistry::invoke(const std::string& name,
                                 const std::vector<ToolArg>& args) const {
    std::lock_guard lock{mutex_};
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return std::unexpected("Unknown tool: " + name);
    }
    return it->second.handler(args);
}

std::string ToolRegistry::describeAll() const {
    std::lock_guard lock{mutex_};
    std::ostringstream oss;
    for (const auto& [nm, def] : tools_) {
        oss << nm << ": " << def.description << '\n';
    }
    return oss.str();
}

std::vector<std::string> ToolRegistry::names() const {
    std::lock_guard lock{mutex_};
    std::vector<std::string> out;
    for (const auto& [k, v] : tools_) out.push_back(k);
    return out;
}

// ---------- parseToolCalls ----------

std::vector<ParsedTool> parseToolCalls(const std::string& aiOutput) {
    std::vector<ParsedTool> results;
    std::istringstream stream{aiOutput};
    std::string line;

    while (std::getline(stream, line)) {
        // Trim
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
            line.erase(line.begin());
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (line.empty() || line == "unknown") continue;

        // Split into tokens (respecting quoted strings)
        ParsedTool pt;
        std::istringstream tokenStream{line};
        std::string token;
        bool firstToken = true;
        while (tokenStream >> std::ws) {
            if (tokenStream.peek() == '"') {
                tokenStream.get(); // consume opening quote
                std::getline(tokenStream, token, '"');
            } else {
                tokenStream >> token;
            }
            if (firstToken) {
                pt.name = token;
                firstToken = false;
            } else {
                pt.args.push_back(token);
            }
        }
        if (!pt.name.empty()) {
            results.push_back(std::move(pt));
        }
    }
    return results;
}

} // namespace port::ai
