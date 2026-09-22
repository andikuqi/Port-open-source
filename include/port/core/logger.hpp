#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace port::core {

enum class LogLevel : std::uint8_t {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

[[nodiscard]] constexpr std::string_view levelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

struct LogRecord {
    LogLevel level;
    std::string component;
    std::string message;
    std::string timestamp;
};

using LogSink = std::function<void(const LogRecord&)>;

// Thread-safe structured logger with pluggable sinks.
class Logger {
public:
    explicit Logger(std::string component, LogLevel minLevel = LogLevel::Info)
        : component_{std::move(component)}, minLevel_{minLevel} {}

    void addSink(LogSink sink) {
        std::lock_guard lock{mutex_};
        sinks_.push_back(std::move(sink));
    }

    void setLevel(LogLevel level) noexcept { minLevel_.store(level); }

    void log(LogLevel level, std::string_view message) {
        if (level < minLevel_.load()) return;
        LogRecord rec{level, component_, std::string{message}, currentTimestamp()};
        std::lock_guard lock{mutex_};
        for (const auto& sink : sinks_) {
            sink(rec);
        }
    }

    void trace(std::string_view msg) { log(LogLevel::Trace, msg); }
    void debug(std::string_view msg) { log(LogLevel::Debug, msg); }
    void info(std::string_view msg)  { log(LogLevel::Info,  msg); }
    void warn(std::string_view msg)  { log(LogLevel::Warn,  msg); }
    void error(std::string_view msg) { log(LogLevel::Error, msg); }
    void fatal(std::string_view msg) { log(LogLevel::Fatal, msg); }

    // Convenience: build message from stream
    template <typename F>
    void log(LogLevel level, F&& builder) {
        if (level < minLevel_.load()) return;
        std::ostringstream oss;
        builder(oss);
        log(level, oss.str());
    }

private:
    static std::string currentTimestamp();

    std::string component_;
    std::atomic<LogLevel> minLevel_;
    std::vector<LogSink> sinks_;
    std::mutex mutex_;
};

// Pre-built sinks
LogSink makeConsoleSink();
LogSink makeFileSink(const std::string& path);

// Factory for component loggers sharing a common set of sinks
class LoggerFactory {
public:
    static LoggerFactory& instance();

    void addSink(LogSink sink);
    void setGlobalLevel(LogLevel level);
    [[nodiscard]] std::unique_ptr<Logger> make(std::string component);

private:
    std::mutex mutex_;
    std::vector<LogSink> sinks_;
    LogLevel globalLevel_{LogLevel::Info};
};

} // namespace port::core
