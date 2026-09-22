#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace port::ai {

// Error type for AI operations
struct AIError {
    enum class Code { NetworkFailure, AuthFailure, QuotaExceeded, InvalidResponse, Offline };
    Code code;
    std::string detail;
};

// A single message in a conversation
struct Message {
    enum class Role { System, User, Assistant };
    Role role;
    std::string content;
};

// Request to an AI provider
struct PromptRequest {
    std::vector<Message> messages;
    std::string systemInstruction;
    int maxTokens{2048};
    float temperature{0.7f};
    std::string model; // empty = provider default
};

// Response from an AI provider
struct PromptResponse {
    std::string content;
    int inputTokens{0};
    int outputTokens{0};
    std::string modelUsed;
};

using AIResult = std::expected<PromptResponse, AIError>;

// Abstract provider — implement for Gemini, Claude, Ollama, etc.
struct IAIProvider {
    virtual ~IAIProvider() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual bool isAvailable() const = 0;

    // Synchronous call (run on worker thread, never UI thread)
    [[nodiscard]] virtual AIResult complete(const PromptRequest& request) = 0;

    // Set API credentials
    virtual void setCredential(const std::string& key, const std::string& value) = 0;
};

using ProviderPtr = std::shared_ptr<IAIProvider>;

// Provider registry
class ProviderRegistry {
public:
    static ProviderRegistry& instance();

    void registerProvider(ProviderPtr provider);
    [[nodiscard]] ProviderPtr get(const std::string& name) const;
    [[nodiscard]] ProviderPtr primary() const;
    void setPrimary(const std::string& name);

    [[nodiscard]] std::vector<std::string> availableNames() const;

private:
    ProviderRegistry() = default;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ProviderPtr> providers_;
    std::string primaryName_;
};

// Factory — creates a GeminiProvider and returns it as IAIProvider
// The provider must be configured via setCredential("api_key", key) before use.
[[nodiscard]] std::shared_ptr<IAIProvider> makeGeminiProvider();

// Config for any OpenAI-compatible chat endpoint (OpenRouter, Groq, ...)
struct OpenAICompatConfig {
    std::string providerName;  // shown in UI, e.g. "OpenRouter"
    std::string host;          // e.g. "openrouter.ai"
    std::string path;          // e.g. "/api/v1/chat/completions"
    std::string defaultModel;  // used when no "model" credential is set
};

// Factory — OpenRouter and Groq both speak this protocol; only the
// config differs. Configure via setCredential("api_key", key).
[[nodiscard]] std::shared_ptr<IAIProvider>
makeOpenAICompatProvider(OpenAICompatConfig config);

// Factory — local Ollama daemon on 127.0.0.1:11434. No API key needed;
// works fully offline. setCredential("model", ...) picks the local model.
[[nodiscard]] std::shared_ptr<IAIProvider> makeOllamaProvider();

// Failover chain: complete() tries providers in the order they were
// added and returns the first success. Implements IAIProvider itself so
// the kernel adapter can use it as a drop-in provider.
class ProviderManager final : public IAIProvider {
public:
    void addProvider(ProviderPtr provider);

    [[nodiscard]] std::string name() const override { return "provider-manager"; }
    [[nodiscard]] bool isAvailable() const override;
    [[nodiscard]] AIResult complete(const PromptRequest& request) override;
    void setCredential(const std::string& key, const std::string& value) override;

    // Name of the provider that served the most recent successful call
    // (empty until the first success) — used for the UI engine badge.
    [[nodiscard]] std::string activeProviderName() const;

    // Names of every provider in the chain, in failover order.
    [[nodiscard]] std::vector<std::string> chainNames() const;

private:
    mutable std::mutex mutex_;
    std::vector<ProviderPtr> chain_;
    std::string activeName_;
};

} // namespace port::ai
