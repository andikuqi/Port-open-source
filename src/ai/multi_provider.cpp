// Multi-provider AI stack:
//   - OpenAICompatProvider — OpenRouter, Groq (same wire protocol)
//   - OllamaProvider       — local daemon, offline-capable, no API key
//   - ProviderManager      — failover chain, first success wins

#include "port/ai/provider.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wininet.h>
#endif

#include <mutex>
#include <sstream>
#include <string>

namespace port::ai {

namespace {

// ---------- JSON helpers (same conventions as gemini_provider) ----------

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

// Finds the first non-empty string value for the given key, e.g.
// extractJsonString(body, "content") on a chat/completions response.
std::string extractJsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t searchFrom = 0;
    while (true) {
        const auto keyPos = json.find(needle, searchFrom);
        if (keyPos == std::string::npos) return {};
        const auto colon = json.find(':', keyPos + needle.size());
        if (colon == std::string::npos) return {};
        std::size_t i = colon + 1;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' ||
                                   json[i] == '\r' || json[i] == '\n')) ++i;
        if (i >= json.size() || json[i] != '"') {
            searchFrom = colon + 1;   // value was null/number/object — keep looking
            continue;
        }
        std::string decoded;
        bool escape = false;
        for (++i; i < json.size(); ++i) {
            const char c = json[i];
            if (escape) {
                switch (c) {
                case 'n':  decoded += '\n'; break;
                case 'r':  decoded += '\r'; break;
                case 't':  decoded += '\t'; break;
                case '"':  decoded += '"';  break;
                case '\\': decoded += '\\'; break;
                default:   decoded += c;    break;
                }
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                break;
            } else {
                decoded += c;
            }
        }
        if (!decoded.empty()) return decoded;
        searchFrom = i + 1;
    }
}

#ifdef _WIN32

struct HttpResponse {
    DWORD status{0};
    std::string body;
};

// Blocking HTTPS/HTTP POST via WinINet. Runs on the kernel worker thread.
std::expected<HttpResponse, AIError>
httpPost(const std::string& host, INTERNET_PORT port, bool secure,
         const std::string& path, const std::string& extraHeaders,
         const std::string& body, DWORD timeoutMs)
{
    HINTERNET hNet = InternetOpenA("PortOS/2.0", INTERNET_OPEN_TYPE_DIRECT,
                                   nullptr, nullptr, 0);
    if (!hNet)
        return std::unexpected(AIError{AIError::Code::NetworkFailure, "InternetOpen failed"});

    InternetSetOptionA(hNet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionA(hNet, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionA(hNet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

    HINTERNET hConn = InternetConnectA(hNet, host.c_str(), port,
                                       nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) {
        InternetCloseHandle(hNet);
        return std::unexpected(AIError{AIError::Code::NetworkFailure,
                                       "Cannot reach " + host});
    }

    DWORD flags = INTERNET_FLAG_RELOAD;
    if (secure) flags |= INTERNET_FLAG_SECURE;
    HINTERNET hReq = HttpOpenRequestA(hConn, "POST", path.c_str(),
                                      nullptr, nullptr, nullptr, flags, 0);
    if (!hReq) {
        InternetCloseHandle(hConn);
        InternetCloseHandle(hNet);
        return std::unexpected(AIError{AIError::Code::NetworkFailure, "HttpOpenRequest failed"});
    }

    const std::string headers = "Content-Type: application/json\r\n" + extraHeaders;
    const BOOL sent = HttpSendRequestA(hReq, headers.c_str(),
                                       static_cast<DWORD>(headers.size()),
                                       const_cast<char*>(body.c_str()),
                                       static_cast<DWORD>(body.size()));
    if (!sent) {
        InternetCloseHandle(hReq);
        InternetCloseHandle(hConn);
        InternetCloseHandle(hNet);
        return std::unexpected(AIError{AIError::Code::NetworkFailure,
                                       host + " did not answer"});
    }

    HttpResponse out;
    DWORD statusSize = sizeof(out.status);
    HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &out.status, &statusSize, nullptr);

    char buf[4096];
    DWORD read = 0;
    while (InternetReadFile(hReq, buf, sizeof(buf) - 1, &read) && read > 0) {
        out.body.append(buf, read);
    }

    InternetCloseHandle(hReq);
    InternetCloseHandle(hConn);
    InternetCloseHandle(hNet);
    return out;
}

#endif // _WIN32

AIError::Code errorCodeForStatus(DWORD status) {
    if (status == 401 || status == 403) return AIError::Code::AuthFailure;
    if (status == 429)                  return AIError::Code::QuotaExceeded;
    return AIError::Code::NetworkFailure;
}

// Builds the OpenAI-style {"messages":[...]} array shared by the
// chat/completions and Ollama /api/chat formats.
std::string buildChatMessages(const PromptRequest& request) {
    std::string systemText;
    for (const auto& msg : request.messages) {
        if (msg.role == Message::Role::System) { systemText = msg.content; break; }
    }
    if (systemText.empty()) systemText = request.systemInstruction;

    std::ostringstream messages;
    bool first = true;
    if (!systemText.empty()) {
        messages << "{\"role\":\"system\",\"content\":\""
                 << jsonEscape(systemText) << "\"}";
        first = false;
    }
    for (const auto& msg : request.messages) {
        if (msg.role == Message::Role::System) continue;
        if (!first) messages << ',';
        first = false;
        messages << "{\"role\":\""
                 << (msg.role == Message::Role::Assistant ? "assistant" : "user")
                 << "\",\"content\":\"" << jsonEscape(msg.content) << "\"}";
    }
    return messages.str();
}

// ---------- OpenAI-compatible provider (OpenRouter, Groq, ...) ----------

class OpenAICompatProvider final : public IAIProvider {
public:
    explicit OpenAICompatProvider(OpenAICompatConfig config)
        : config_{std::move(config)} {}

    [[nodiscard]] std::string name() const override { return config_.providerName; }

    [[nodiscard]] bool isAvailable() const override {
        std::lock_guard lock{mutex_};
        return !apiKey_.empty();
    }

    void setCredential(const std::string& key, const std::string& value) override {
        std::lock_guard lock{mutex_};
        if (key == "api_key") apiKey_ = value;
        if (key == "model")   model_  = value;
    }

    [[nodiscard]] AIResult complete(const PromptRequest& request) override {
        std::lock_guard lock{mutex_};
        if (apiKey_.empty())
            return std::unexpected(AIError{AIError::Code::AuthFailure, "No API key configured"});
#ifndef _WIN32
        return std::unexpected(AIError{AIError::Code::Offline,
                                       "Network calls not implemented on this platform"});
#else
        const std::string model =
            !request.model.empty() ? request.model
            : !model_.empty()      ? model_
                                   : config_.defaultModel;

        std::ostringstream bodyStream;
        bodyStream << "{\"model\":\"" << jsonEscape(model)
                   << "\",\"messages\":[" << buildChatMessages(request)
                   << "],\"temperature\":" << request.temperature
                   << ",\"max_tokens\":" << request.maxTokens << "}";

        const std::string authHeader = "Authorization: Bearer " + apiKey_ + "\r\n";
        auto response = httpPost(config_.host, INTERNET_DEFAULT_HTTPS_PORT, true,
                                 config_.path, authHeader, bodyStream.str(), 20000);
        if (!response) return std::unexpected(response.error());

        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(AIError{errorCodeForStatus(response->status),
                config_.providerName + " HTTP " + std::to_string(response->status) +
                ": " + response->body.substr(0, 200)});
        }

        const std::string text = extractJsonString(response->body, "content");
        if (text.empty()) {
            return std::unexpected(AIError{AIError::Code::InvalidResponse,
                config_.providerName + ": could not parse response: " +
                response->body.substr(0, 200)});
        }

        PromptResponse pr;
        pr.content   = text;
        pr.modelUsed = model;
        return pr;
#endif
    }

private:
    OpenAICompatConfig config_;
    mutable std::mutex mutex_;
    std::string apiKey_;
    std::string model_;
};

// ---------- Ollama provider (local, offline-capable) ----------

class OllamaProvider final : public IAIProvider {
public:
    [[nodiscard]] std::string name() const override { return "Ollama"; }

    // Availability is decided by the actual call: the daemon may start or
    // stop at any time, so probing here would only give a stale answer.
    [[nodiscard]] bool isAvailable() const override { return true; }

    void setCredential(const std::string& key, const std::string& value) override {
        std::lock_guard lock{mutex_};
        if (key == "model") model_ = value;
        if (key == "host")  host_  = value;
    }

    [[nodiscard]] AIResult complete(const PromptRequest& request) override {
        std::lock_guard lock{mutex_};
#ifndef _WIN32
        return std::unexpected(AIError{AIError::Code::Offline,
                                       "Network calls not implemented on this platform"});
#else
        const std::string model =
            !request.model.empty() ? request.model : model_;

        std::ostringstream bodyStream;
        bodyStream << "{\"model\":\"" << jsonEscape(model)
                   << "\",\"messages\":[" << buildChatMessages(request)
                   << "],\"stream\":false,\"options\":{\"temperature\":"
                   << request.temperature << ",\"num_predict\":"
                   << request.maxTokens << "}}";

        auto response = httpPost(host_, 11434, false, "/api/chat",
                                 "", bodyStream.str(), 60000);
        if (!response) return std::unexpected(response.error());

        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(AIError{errorCodeForStatus(response->status),
                "Ollama HTTP " + std::to_string(response->status) +
                ": " + response->body.substr(0, 200)});
        }

        const std::string text = extractJsonString(response->body, "content");
        if (text.empty()) {
            return std::unexpected(AIError{AIError::Code::InvalidResponse,
                "Ollama: could not parse response: " + response->body.substr(0, 200)});
        }

        PromptResponse pr;
        pr.content   = text;
        pr.modelUsed = model;
        return pr;
#endif
    }

private:
    mutable std::mutex mutex_;
    std::string model_{"llama3.2"};
    std::string host_{"127.0.0.1"};
};

} // namespace

// ---------- ProviderManager ----------

void ProviderManager::addProvider(ProviderPtr provider) {
    std::lock_guard lock{mutex_};
    chain_.push_back(std::move(provider));
}

bool ProviderManager::isAvailable() const {
    std::lock_guard lock{mutex_};
    for (const auto& p : chain_) {
        if (p->isAvailable()) return true;
    }
    return false;
}

void ProviderManager::setCredential(const std::string&, const std::string&) {
    // Credentials belong to the individual providers in the chain.
}

std::string ProviderManager::activeProviderName() const {
    std::lock_guard lock{mutex_};
    return activeName_;
}

std::vector<std::string> ProviderManager::chainNames() const {
    std::lock_guard lock{mutex_};
    std::vector<std::string> names;
    names.reserve(chain_.size());
    for (const auto& p : chain_) names.push_back(p->name());
    return names;
}

AIResult ProviderManager::complete(const PromptRequest& request) {
    // Snapshot the chain so the (long) network calls run unlocked.
    std::vector<ProviderPtr> chain;
    {
        std::lock_guard lock{mutex_};
        chain = chain_;
    }

    std::string failures;
    for (const auto& provider : chain) {
        if (!provider->isAvailable()) continue;
        auto result = provider->complete(request);
        if (result) {
            std::lock_guard lock{mutex_};
            activeName_ = provider->name();
            return result;
        }
        if (!failures.empty()) failures += " | ";
        failures += provider->name() + ": " + result.error().detail;
    }

    if (failures.empty()) {
        return std::unexpected(AIError{AIError::Code::Offline,
                                       "No AI providers configured"});
    }
    return std::unexpected(AIError{AIError::Code::NetworkFailure,
                                   "All providers failed — " + failures});
}

// ---------- Factories ----------

std::shared_ptr<IAIProvider> makeOpenAICompatProvider(OpenAICompatConfig config) {
    return std::make_shared<OpenAICompatProvider>(std::move(config));
}

std::shared_ptr<IAIProvider> makeOllamaProvider() {
    return std::make_shared<OllamaProvider>();
}

} // namespace port::ai
