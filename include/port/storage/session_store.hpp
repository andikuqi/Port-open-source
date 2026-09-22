#pragma once

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace port::storage {

struct SessionEntry {
    std::string timestamp;
    std::string tag;
    std::string content;
};

// Thread-safe append-only session log with structured entries.
// Replaces the raw ofstream append in ai_kernel.cpp.
class SessionStore {
public:
    explicit SessionStore(const std::string& path);
    ~SessionStore();

    void append(std::string_view tag, std::string_view content);
    [[nodiscard]] std::vector<SessionEntry> tail(std::size_t n) const;
    void flush();

private:
    static std::string now();

    mutable std::mutex mutex_;
    mutable std::ofstream file_;
    std::string path_;
};

} // namespace port::storage
