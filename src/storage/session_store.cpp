#include "port/storage/session_store.hpp"

#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <sstream>

namespace port::storage {

SessionStore::SessionStore(const std::string& path) : path_{path} {
    file_.open(path, std::ios::app);
}

SessionStore::~SessionStore() {
    flush();
}

std::string SessionStore::now() {
    using namespace std::chrono;
    const auto t = system_clock::to_time_t(system_clock::now());
    char buf[32]{};
#ifdef _WIN32
    struct tm tminfo{};
    localtime_s(&tminfo, &t);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tminfo);
#else
    struct tm tminfo{};
    localtime_r(&t, &tminfo);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tminfo);
#endif
    return buf;
}

void SessionStore::append(std::string_view tag, std::string_view content) {
    std::lock_guard lock{mutex_};
    if (!file_.is_open()) return;
    file_ << '[' << now() << "] [" << tag << "] " << content << '\n';
}

void SessionStore::flush() {
    std::lock_guard lock{mutex_};
    if (file_.is_open()) file_.flush();
}

std::vector<SessionEntry> SessionStore::tail(std::size_t n) const {
    std::lock_guard lock{mutex_};
    if (n == 0) return {};
    if (file_.is_open()) file_.flush();

    std::ifstream input{path_};
    if (!input) return {};

    std::deque<SessionEntry> recent;
    std::string line;
    while (std::getline(input, line)) {
        // Format: [timestamp] [tag] content
        if (line.empty() || line.front() != '[') continue;
        const auto timestampEnd = line.find(']');
        const auto tagStart = line.find('[', timestampEnd == std::string::npos ? 0 : timestampEnd + 1);
        const auto tagEnd = line.find(']', tagStart == std::string::npos ? 0 : tagStart + 1);
        if (timestampEnd == std::string::npos ||
            tagStart == std::string::npos || tagEnd == std::string::npos) continue;

        SessionEntry entry;
        entry.timestamp = line.substr(1, timestampEnd - 1);
        entry.tag = line.substr(tagStart + 1, tagEnd - tagStart - 1);
        std::size_t contentStart = tagEnd + 1;
        while (contentStart < line.size() && line[contentStart] == ' ') ++contentStart;
        entry.content = line.substr(contentStart);
        recent.push_back(std::move(entry));
        if (recent.size() > n) recent.pop_front();
    }
    return {recent.begin(), recent.end()};
}

} // namespace port::storage
