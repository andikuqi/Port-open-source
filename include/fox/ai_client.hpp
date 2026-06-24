#pragma once

#include <string>

namespace fox {

class AIClient {
public:
    AIClient();
    std::string requestPlan(const std::string& prompt);

private:
    std::string getOfflinePlan(const std::string& prompt);
    std::string callGeminiAPI(const std::string& apiKey, const std::string& prompt);
    static std::string trim(std::string value);
};

} // namespace fox
