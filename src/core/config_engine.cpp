#include "port/core/config_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace port::core {

namespace {

std::string trim(const std::string& s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    auto b = std::find_if(s.begin(), s.end(), notSpace);
    auto e = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    return (b < e) ? std::string{b, e} : std::string{};
}

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

} // namespace

ConfigEngine& ConfigEngine::instance() {
    static ConfigEngine inst;
    return inst;
}

void ConfigEngine::loadFile(const std::string& path) {
    std::ifstream f{path};
    if (!f.is_open()) return;

    std::lock_guard lock{mutex_};
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (!key.empty()) {
            file_[key] = ConfigValue{val};
        }
    }
}

void ConfigEngine::loadEnv() {
    // Port OS env vars are prefixed PORT_
    // We probe the well-known keys used by the codebase
    const char* keys[] = {
        "GEMINI_API_KEY", "PORT_LOG_LEVEL", "PORT_SANDBOX_PATH",
        "PORT_SESSION_LOG", "PORT_AUDIT_LOG", nullptr
    };

    std::lock_guard lock{mutex_};
    for (int i = 0; keys[i]; ++i) {
        const char* val = std::getenv(keys[i]);
        if (val && val[0] != '\0') {
            env_[keys[i]] = ConfigValue{std::string{val}};
        }
    }
}

void ConfigEngine::set(const std::string& key, ConfigValue value) {
    std::lock_guard lock{mutex_};
    runtime_[key] = std::move(value);
}

std::optional<ConfigValue> ConfigEngine::lookup(const std::string& key) const {
    if (auto it = runtime_.find(key); it != runtime_.end()) return it->second;
    if (auto it = env_.find(key);     it != env_.end())     return it->second;
    if (auto it = file_.find(key);    it != file_.end())    return it->second;
    return std::nullopt;
}

std::optional<std::string> ConfigEngine::getString(const std::string& key) const {
    std::lock_guard lock{mutex_};
    auto v = lookup(key);
    if (!v) return std::nullopt;
    if (auto* s = std::get_if<std::string>(&*v)) return *s;
    return std::nullopt;
}

std::optional<int64_t> ConfigEngine::getInt(const std::string& key) const {
    std::lock_guard lock{mutex_};
    auto v = lookup(key);
    if (!v) return std::nullopt;
    if (auto* n = std::get_if<int64_t>(&*v)) return *n;
    if (auto* s = std::get_if<std::string>(&*v)) {
        try { return static_cast<int64_t>(std::stoll(*s)); } catch (...) {}
    }
    return std::nullopt;
}

std::optional<double> ConfigEngine::getDouble(const std::string& key) const {
    std::lock_guard lock{mutex_};
    auto v = lookup(key);
    if (!v) return std::nullopt;
    if (auto* d = std::get_if<double>(&*v)) return *d;
    if (auto* s = std::get_if<std::string>(&*v)) {
        try { return std::stod(*s); } catch (...) {}
    }
    return std::nullopt;
}

std::optional<bool> ConfigEngine::getBool(const std::string& key) const {
    std::lock_guard lock{mutex_};
    auto v = lookup(key);
    if (!v) return std::nullopt;
    if (auto* b = std::get_if<bool>(&*v)) return *b;
    if (auto* s = std::get_if<std::string>(&*v)) {
        const auto lo = [&] {
            std::string t = *s;
            std::transform(t.begin(), t.end(), t.begin(), ::tolower);
            return t;
        }();
        if (lo == "true" || lo == "1" || lo == "yes") return true;
        if (lo == "false"|| lo == "0" || lo == "no")  return false;
    }
    return std::nullopt;
}

std::string ConfigEngine::getString(const std::string& key, std::string def) const {
    return getString(key).value_or(std::move(def));
}
int64_t ConfigEngine::getInt(const std::string& key, int64_t def) const {
    return getInt(key).value_or(def);
}
double ConfigEngine::getDouble(const std::string& key, double def) const {
    return getDouble(key).value_or(def);
}
bool ConfigEngine::getBool(const std::string& key, bool def) const {
    return getBool(key).value_or(def);
}

void ConfigEngine::reset() {
    std::lock_guard lock{mutex_};
    runtime_.clear();
    file_.clear();
    env_.clear();
}

} // namespace port::core
