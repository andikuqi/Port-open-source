#include "port/security/audit_log.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>

namespace port::security {

AuditLog& AuditLog::instance() {
    static AuditLog log;
    return log;
}

void AuditLog::open(const std::string& path) {
    std::lock_guard lock{mutex_};
    file_.open(path, std::ios::app);
    if (file_.is_open()) {
        file_ << "timestamp,action,actor,detail,outcome\n";
        file_.flush();
    }
}

void AuditLog::close() {
    std::lock_guard lock{mutex_};
    file_.close();
}

std::string AuditLog::timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
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

void AuditLog::record(AuditAction action,
                       std::string_view detail,
                       bool success,
                       std::string_view actor) {
    std::lock_guard lock{mutex_};
    if (!file_.is_open()) return;
    file_ << timestamp() << ','
          << actionName(action) << ','
          << actor << ','
          << detail << ','
          << (success ? "OK" : "FAIL")
          << '\n';
    file_.flush();
}

} // namespace port::security
