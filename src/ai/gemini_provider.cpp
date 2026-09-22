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
#include <unordered_map>

namespace port::ai {

// ---------- JSON helpers (no external library dependency) ----------

namespace {

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

// Minimal JSON text extraction — finds first "text": "..." value
std::string extractJsonText(const std::string& json) {
    std::string lastDecoded;
    std::size_t searchFrom = 0;
    while (true) {
        const auto key = json.find("\"text\"", searchFrom);
        if (key == std::string::npos) break;
        const auto colon = json.find(':', key + 6);
        const auto quote = colon == std::string::npos
            ? std::string::npos : json.find('"', colon + 1);
        if (quote == std::string::npos) break;

        std::string decoded;
        bool escape = false;
        std::size_t i = quote + 1;
        for (; i < json.size(); ++i) {
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
        if (!decoded.empty()) lastDecoded = std::move(decoded);
        searchFrom = i < json.size() ? i + 1 : json.size();
    }
    return lastDecoded;
}

} // namespace

// ---------- GeminiProvider ----------

class GeminiProvider final : public IAIProvider {
public:
    [[nodiscard]] std::string name() const override { return "gemini"; }

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
        if (apiKey_.empty()) {
            return std::unexpected(AIError{AIError::Code::AuthFailure, "No API key configured"});
        }
#ifdef _WIN32
        return callWinInet(request);
#else
        return std::unexpected(AIError{AIError::Code::Offline,
                                       "Network calls not implemented on this platform"});
#endif
    }

private:
#ifdef _WIN32
    AIResult callWinInet(const PromptRequest& request) {
        // Build request body
        std::string systemText;
        for (const auto& msg : request.messages) {
            if (msg.role == Message::Role::System) {
                systemText = msg.content;
                break;
            }
        }
        if (systemText.empty()) systemText = request.systemInstruction;

        std::ostringstream contents;
        bool firstMessage = true;
        for (const auto& message : request.messages) {
            if (message.role == Message::Role::System) continue;
            if (!firstMessage) contents << ',';
            firstMessage = false;
            contents << "{\"role\":\""
                     << (message.role == Message::Role::Assistant ? "model" : "user")
                     << "\",\"parts\":[{\"text\":\""
                     << jsonEscape(message.content) << "\"}]}";
        }
        if (firstMessage) {
            return std::unexpected(AIError{AIError::Code::InvalidResponse,
                                           "Prompt contains no user message"});
        }

        std::ostringstream bodyStream;
        bodyStream << "{\"systemInstruction\":{\"parts\":[{\"text\":\""
                   << jsonEscape(systemText)
                   << "\"}]},\"contents\":[" << contents.str()
                   << "],\"generationConfig\":{\"temperature\":"
                   << request.temperature << ",\"maxOutputTokens\":"
                   << request.maxTokens << "}}";
        const std::string body = bodyStream.str();

        const std::string modelName = model_.empty() ? "gemini-3.5-flash" : model_;
        const std::string path = "/v1beta/models/" + modelName + ":generateContent";

        HINTERNET hNet = InternetOpenA("PortOS/2.0",
                                        INTERNET_OPEN_TYPE_DIRECT,
                                        nullptr, nullptr, 0);
        if (!hNet) return std::unexpected(AIError{AIError::Code::NetworkFailure, "InternetOpen failed"});

        DWORD timeoutMs = 15000;
        InternetSetOptionA(hNet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        InternetSetOptionA(hNet, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        InternetSetOptionA(hNet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

        HINTERNET hConn = InternetConnectA(hNet,
                                            "generativelanguage.googleapis.com",
                                            INTERNET_DEFAULT_HTTPS_PORT,
                                            nullptr, nullptr,
                                            INTERNET_SERVICE_HTTP, 0, 0);
        if (!hConn) {
            InternetCloseHandle(hNet);
            return std::unexpected(AIError{AIError::Code::NetworkFailure, "InternetConnect failed"});
        }

        HINTERNET hReq = HttpOpenRequestA(hConn, "POST", path.c_str(),
                                           nullptr, nullptr, nullptr,
                                           INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0);
        if (!hReq) {
            InternetCloseHandle(hConn);
            InternetCloseHandle(hNet);
            return std::unexpected(AIError{AIError::Code::NetworkFailure, "HttpOpenRequest failed"});
        }

        const std::string headers = "Content-Type: application/json\r\n"
                                    "x-goog-api-key: " + apiKey_ + "\r\n";
        BOOL sent = HttpSendRequestA(hReq,
                                      headers.c_str(),
                                      static_cast<DWORD>(headers.size()),
                                      const_cast<char*>(body.c_str()),
                                      static_cast<DWORD>(body.size()));
        if (!sent) {
            InternetCloseHandle(hReq);
            InternetCloseHandle(hConn);
            InternetCloseHandle(hNet);
            return std::unexpected(AIError{AIError::Code::NetworkFailure, "HttpSendRequest failed"});
        }

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &statusCode, &statusSize, nullptr);

        std::string response;
        char buf[4096];
        DWORD read = 0;
        while (InternetReadFile(hReq, buf, sizeof(buf) - 1, &read) && read > 0) {
            buf[read] = '\0';
            response.append(buf, read);
        }

        InternetCloseHandle(hReq);
        InternetCloseHandle(hConn);
        InternetCloseHandle(hNet);

        if (statusCode < 200 || statusCode >= 300) {
            AIError::Code code = AIError::Code::NetworkFailure;
            if (statusCode == 401 || statusCode == 403) code = AIError::Code::AuthFailure;
            if (statusCode == 429) code = AIError::Code::QuotaExceeded;
            return std::unexpected(AIError{code,
                "Gemini HTTP " + std::to_string(statusCode) + ": " + response.substr(0, 200)});
        }

        const std::string text = extractJsonText(response);
        if (text.empty()) {
            return std::unexpected(AIError{AIError::Code::InvalidResponse,
                                           "Could not parse response: " + response.substr(0, 200)});
        }

        PromptResponse pr;
        pr.content   = text;
        pr.modelUsed = modelName;
        return pr;
    }
#endif

    mutable std::mutex mutex_;
    std::string apiKey_;
    std::string model_{"gemini-3.5-flash"};
};

// ---------- ProviderRegistry ----------

ProviderRegistry& ProviderRegistry::instance() {
    static ProviderRegistry reg;
    return reg;
}

void ProviderRegistry::registerProvider(ProviderPtr provider) {
    std::lock_guard lock{mutex_};
    const auto nm = provider->name();
    providers_[nm] = std::move(provider);
    if (primaryName_.empty()) primaryName_ = nm;
}

ProviderPtr ProviderRegistry::get(const std::string& name) const {
    std::lock_guard lock{mutex_};
    auto it = providers_.find(name);
    return (it != providers_.end()) ? it->second : nullptr;
}

ProviderPtr ProviderRegistry::primary() const {
    std::lock_guard lock{mutex_};
    auto it = providers_.find(primaryName_);
    return (it != providers_.end()) ? it->second : nullptr;
}

void ProviderRegistry::setPrimary(const std::string& name) {
    std::lock_guard lock{mutex_};
    primaryName_ = name;
}

std::vector<std::string> ProviderRegistry::availableNames() const {
    std::lock_guard lock{mutex_};
    std::vector<std::string> names;
    for (const auto& [k, v] : providers_) {
        if (v->isAvailable()) names.push_back(k);
    }
    return names;
}

// ---------- Registration helper ----------

std::shared_ptr<IAIProvider> makeGeminiProvider() {
    return std::make_shared<GeminiProvider>();
}

} // namespace port::ai
