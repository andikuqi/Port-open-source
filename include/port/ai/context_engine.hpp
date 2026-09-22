#pragma once

#include "port/ai/provider.hpp"

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace port::ai {

// Manages conversation context for a single session.
// Keeps a rolling window of messages within the token budget.
class ContextEngine {
public:
    struct Config {
        std::size_t maxMessages;
        int maxContextTokens;
        std::string systemInstruction;
        Config() : maxMessages(32), maxContextTokens(4096) {}
    };

    explicit ContextEngine(Config config = Config{});

    // Add a user message; returns the full message list for the provider
    [[nodiscard]] std::vector<Message> addUserMessage(const std::string& content);

    // Record an assistant response
    void addAssistantMessage(const std::string& content);

    // Clear the conversation
    void reset();

    // Override the system instruction
    void setSystemInstruction(const std::string& instruction);

    // Export current context
    [[nodiscard]] std::vector<Message> messages() const;

    // Rough token estimate (characters / 4)
    [[nodiscard]] int estimatedTokens() const;

private:
    void trimToLimit();
    [[nodiscard]] int estimatedTokensUnlocked() const;

    Config config_;
    std::deque<Message> history_;
    mutable std::mutex mutex_;
};

} // namespace port::ai
