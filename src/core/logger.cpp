#include "port/core/logger.hpp"

#include <chrono>
#include <ctime>
#include <iostream>

namespace port::core {

// ---------- Logger ----------

std::string Logger::currentTimestamp() {
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

// ---------- Sinks ----------

LogSink makeConsoleSink() {
    return [](const LogRecord& rec) {
        std::clog << '[' << rec.timestamp << "] ["
                  << levelName(rec.level) << "] ["
                  << rec.component << "] "
                  << rec.message << '\n';
    };
}

LogSink makeFileSink(const std::string& path) {
    auto file = std::make_shared<std::ofstream>(path, std::ios::app);
    return [file](const LogRecord& rec) {
        if (file->is_open()) {
            *file << '[' << rec.timestamp << "] ["
                  << levelName(rec.level) << "] ["
                  << rec.component << "] "
                  << rec.message << '\n';
            file->flush();
        }
    };
}

// ---------- LoggerFactory ----------

LoggerFactory& LoggerFactory::instance() {
    static LoggerFactory inst;
    return inst;
}

void LoggerFactory::addSink(LogSink sink) {
    std::lock_guard lock{mutex_};
    sinks_.push_back(std::move(sink));
}

void LoggerFactory::setGlobalLevel(LogLevel level) {
    std::lock_guard lock{mutex_};
    globalLevel_ = level;
}

std::unique_ptr<Logger> LoggerFactory::make(std::string component) {
    std::lock_guard lock{mutex_};
    auto log = std::make_unique<Logger>(std::move(component), globalLevel_);
    for (const auto& s : sinks_) {
        log->addSink(s);
    }
    return log;
}

} // namespace port::core
