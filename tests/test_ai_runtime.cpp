#include "test_runner.hpp"
#include "port/ai/context_engine.hpp"
#include "port/kernel/runtime_ai_adapter.hpp"

class FakeProvider final : public port::ai::IAIProvider {
public:
    std::string name() const override { return "fake"; }
    bool isAvailable() const override { return true; }
    void setCredential(const std::string&, const std::string&) override {}
    port::ai::AIResult complete(const port::ai::PromptRequest& request) override {
        sawContext = !request.messages.empty();
        return port::ai::PromptResponse{
            "fs mkdir AIProject\nfs list AIProject", 10, 8, "fake-model"};
    }
    bool sawContext{false};
};

int main() {
    test::add("RuntimeAIAdapter — provider response becomes validated plan", [] {
        auto provider = std::make_shared<FakeProvider>();
        auto context = std::make_shared<port::ai::ContextEngine>();
        port::ai::PromptRuntime::Config config;
        config.systemInstruction = "Return Port OS commands only.";
        port::kernel::RuntimeAIAdapter adapter{provider, context, config};

        const auto plan = adapter.requestPlan("create an AI project");
        test::check("provider received context", provider->sawContext);
        test::check("mkdir command retained", plan.find("fs mkdir AIProject") != std::string::npos);
        test::check("list command retained", plan.find("fs list AIProject") != std::string::npos);
    });
    return test::run();
}
