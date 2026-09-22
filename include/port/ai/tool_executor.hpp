#pragma once

#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace port::ai {

// Parameter and return value for tools
using ToolArg = std::string;
using ToolResult = std::expected<std::string, std::string>;

// A registered tool callable by the AI
struct ToolDefinition {
    std::string name;
    std::string description;
    std::vector<std::string> paramNames;
    std::function<ToolResult(const std::vector<ToolArg>&)> handler;
};

// Registry of callable tools
class ToolRegistry {
public:
    static ToolRegistry& instance();

    void registerTool(ToolDefinition def);
    void unregisterTool(const std::string& name);

    [[nodiscard]] bool hasTool(const std::string& name) const;
    [[nodiscard]] ToolResult invoke(const std::string& name,
                                    const std::vector<ToolArg>& args) const;

    [[nodiscard]] std::string describeAll() const;
    [[nodiscard]] std::vector<std::string> names() const;

private:
    ToolRegistry() = default;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ToolDefinition> tools_;
};

// Parses AI-generated command text into (tool-name, args) pairs.
// Format expected: "tool_name arg1 arg2" one per line.
struct ParsedTool {
    std::string name;
    std::vector<std::string> args;
};

[[nodiscard]] std::vector<ParsedTool> parseToolCalls(const std::string& aiOutput);

} // namespace port::ai
