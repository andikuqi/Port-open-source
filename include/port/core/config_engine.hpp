#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <mutex>
#include <variant>
#include <vector>

namespace port::core {

using ConfigValue = std::variant<std::string, int64_t, double, bool>;

// Hierarchical config engine with layered override precedence:
//   1. Explicit set() call (runtime override)
//   2. Environment variable (PORT_*)
//   3. Config file (.env / port.conf)
//   4. Default value
class ConfigEngine {
public:
    static ConfigEngine& instance();

    // Load key=value pairs from a file
    void loadFile(const std::string& path);

    // Reads PORT_<UPPER_KEY> from environment
    void loadEnv();

    // Runtime override (highest priority)
    void set(const std::string& key, ConfigValue value);

    // Typed accessors — return nullopt if key not found / wrong type
    [[nodiscard]] std::optional<std::string> getString(const std::string& key) const;
    [[nodiscard]] std::optional<int64_t>     getInt(const std::string& key) const;
    [[nodiscard]] std::optional<double>      getDouble(const std::string& key) const;
    [[nodiscard]] std::optional<bool>        getBool(const std::string& key) const;

    // Typed accessors with default fallback
    [[nodiscard]] std::string getString(const std::string& key, std::string def) const;
    [[nodiscard]] int64_t     getInt(const std::string& key, int64_t def) const;
    [[nodiscard]] double      getDouble(const std::string& key, double def) const;
    [[nodiscard]] bool        getBool(const std::string& key, bool def) const;

    void reset();

private:
    ConfigEngine() = default;

    [[nodiscard]] std::optional<ConfigValue> lookup(const std::string& key) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ConfigValue> runtime_;
    std::unordered_map<std::string, ConfigValue> file_;
    std::unordered_map<std::string, ConfigValue> env_;
};

} // namespace port::core
