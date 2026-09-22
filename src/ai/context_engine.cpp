#include "port/ai/context_engine.hpp"

namespace port::ai {

ContextEngine::ContextEngine(Config config) : config_{std::move(config)} {}

std::vector<Message> ContextEngine::addUserMessage(const std::string& content) {
    std::lock_guard lock{mutex_};
    history_.push_back({Message::Role::User, content});
    trimToLimit();

    std::vector<Message> result;
    if (!config_.systemInstruction.empty()) {
        result.push_back({Message::Role::System, config_.systemInstruction});
    }
    for (const auto& m : history_) {
        result.push_back(m);
    }
    return result;
}

void ContextEngine::addAssistantMessage(const std::string& content) {
    std::lock_guard lock{mutex_};
    history_.push_back({Message::Role::Assistant, content});
    trimToLimit();
}

void ContextEngine::reset() {
    std::lock_guard lock{mutex_};
    history_.clear();
}

void ContextEngine::setSystemInstruction(const std::string& instruction) {
    std::lock_guard lock{mutex_};
    config_.systemInstruction = instruction;
}

std::vector<Message> ContextEngine::messages() const {
    std::lock_guard lock{mutex_};
    return {history_.begin(), history_.end()};
}

int ContextEngine::estimatedTokens() const {
    std::lock_guard lock{mutex_};
    return estimatedTokensUnlocked();
}

int ContextEngine::estimatedTokensUnlocked() const {
    int tokens = static_cast<int>(config_.systemInstruction.size() / 4);
    for (const auto& m : history_) {
        tokens += static_cast<int>(m.content.size() / 4);
    }
    return tokens;
}

void ContextEngine::trimToLimit() {
    while (history_.size() > config_.maxMessages) {
        history_.pop_front();
    }
    // Also trim by token estimate
    while (estimatedTokensUnlocked() > config_.maxContextTokens && !history_.empty()) {
        history_.pop_front();
    }
}

} // namespace port::ai
