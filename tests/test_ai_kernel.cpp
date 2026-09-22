#include "test_runner.hpp"
#include "port/kernel/ai_kernel.hpp"
#include "port/kernel/command_router.hpp"
#include "port/security/audit_log.hpp"

#include <filesystem>

using port::kernel::AIKernel;
using port::kernel::CommandRouter;

namespace {
class DestructivePlanAdapter final : public port::kernel::IAIAdapter {
public:
    std::string requestPlan(const std::string&) override {
        return "fs write planned.txt planned-content";
    }
};
}
using port::kernel::CommandType;

static const std::string kTestSandbox = "kernel_test_sandbox";

static void cleanup() {
    std::error_code ec;
    std::filesystem::remove_all(kTestSandbox, ec);
    std::filesystem::remove("kernel_test_session.txt", ec);
}

static void registerTests() {
    test::add("AIKernel — boot and shutdown", [] {
        cleanup();
        AIKernel::Config cfg;
        cfg.sandboxPath    = kTestSandbox;
        cfg.sessionLogPath = "kernel_test_session.txt";
        cfg.auditLogPath   = "kernel_test_audit.log";

        AIKernel kernel{std::move(cfg)};
        test::check("not running before boot", !kernel.isRunning());

        auto boot = kernel.boot();
        test::check("boot succeeds", boot.ok);
        test::check("running after boot", kernel.isRunning());

        auto boot2 = kernel.boot();
        test::check("second boot is no-op", boot2.ok);

        auto shut = kernel.shutdown();
        test::check("shutdown succeeds", shut.ok);
        test::check("not running after shutdown", !kernel.isRunning());
    });

    test::add("AIKernel — help command", [] {
        AIKernel::Config cfg;
        cfg.sandboxPath = kTestSandbox;
        cfg.sessionLogPath = "kernel_test_session.txt";
        cfg.auditLogPath   = "kernel_test_audit.log";

        AIKernel kernel{std::move(cfg)};
        kernel.boot();

        CommandRouter router;
        auto resp = kernel.handleCommand(router.parse("help"));
        test::check("help returns ok", resp.ok);
        test::check("help mentions fs write",
                    resp.message.find("fs write") != std::string::npos);
    });

    test::add("AIKernel — status command", [] {
        AIKernel::Config cfg;
        cfg.sandboxPath = kTestSandbox;
        cfg.sessionLogPath = "kernel_test_session.txt";
        cfg.auditLogPath   = "kernel_test_audit.log";

        AIKernel kernel{std::move(cfg)};
        kernel.boot();

        CommandRouter router;
        auto resp = kernel.handleCommand(router.parse("status"));
        test::check("status returns ok", resp.ok);
        test::check("status mentions 'running'",
                    resp.message.find("running") != std::string::npos);
    });

    test::add("AIKernel — fs write + read + delete", [] {
        cleanup();
        AIKernel::Config cfg;
        cfg.sandboxPath = kTestSandbox;
        cfg.sessionLogPath = "kernel_test_session.txt";
        cfg.auditLogPath   = "kernel_test_audit.log";

        AIKernel kernel{std::move(cfg)};
        kernel.boot();
        CommandRouter router;

        auto wr = kernel.handleCommand(router.parse("fs write kern_test.txt \"Port OS kernel test\""));
        test::check("fs write requires confirmation", wr.requiresConfirmation);
        wr = kernel.confirmPendingCommand();
        test::check("confirmed fs write ok", wr.ok);

        auto rd = kernel.handleCommand(router.parse("fs read kern_test.txt"));
        test::check("fs read ok", rd.ok);
        test::checkEq<std::string, std::string>("fs read content", rd.message, "Port OS kernel test");

        auto del = kernel.handleCommand(router.parse("fs delete kern_test.txt"));
        test::check("fs delete returns CONFIRM_REQUIRED", del.ok);
        test::check("response is CONFIRM_REQUIRED",
                    del.message.rfind("CONFIRM_REQUIRED:", 0) == 0);

        auto confirmed = kernel.confirmPendingCommand();
        test::check("confirmed delete ok", confirmed.ok);
    });

    test::add("AIKernel — fs list", [] {
        cleanup();
        AIKernel::Config cfg;
        cfg.sandboxPath = kTestSandbox;
        cfg.sessionLogPath = "kernel_test_session.txt";
        cfg.auditLogPath   = "kernel_test_audit.log";

        AIKernel kernel{std::move(cfg)};
        kernel.boot();
        CommandRouter router;

        kernel.handleCommand(router.parse("fs write list_test.txt \"data\""));
        kernel.confirmPendingCommand();
        auto ls = kernel.handleCommand(router.parse("fs list"));
        test::check("fs list ok", ls.ok);
        test::check("listing contains written file",
                    ls.message.find("list_test.txt") != std::string::npos);
    });

    test::add("AIKernel — protected commands require approval", [] {
        cleanup();
        AIKernel::Config cfg;
        cfg.sandboxPath = kTestSandbox;
        cfg.sessionLogPath = "kernel_test_session.txt";
        cfg.auditLogPath = "kernel_test_audit.log";
        AIKernel kernel{std::move(cfg)};
        kernel.boot();
        CommandRouter router;

        auto download = kernel.handleCommand(
            router.parse("net download https://example.com/file.exe sandbox/Apps/file.exe"));
        test::check("download requires confirmation", download.requiresConfirmation);
        auto cancelled = kernel.cancelPendingCommand();
        test::check("download can be cancelled", cancelled.ok);

        auto execute = kernel.handleCommand(
            router.parse("sys execute sandbox/Apps/file.exe"));
        test::check("execute requires confirmation", execute.requiresConfirmation);
        kernel.cancelPendingCommand();
    });

    test::add("AIKernel — AI plan pauses before protected step", [] {
        cleanup();
        AIKernel::Config cfg;
        cfg.sandboxPath = kTestSandbox;
        cfg.sessionLogPath = "kernel_test_session.txt";
        cfg.auditLogPath = "kernel_test_audit.log";
        AIKernel kernel{std::move(cfg)};
        kernel.setAIAdapter(std::make_shared<DestructivePlanAdapter>());
        kernel.boot();
        CommandRouter router;

        auto planned = kernel.handleCommand(router.parse("create a planned file"));
        test::check("plan requests approval", planned.requiresConfirmation);
        test::check("file absent before approval",
                    !std::filesystem::exists(kTestSandbox + "/planned.txt"));
        auto approved = kernel.handleCommand(router.parse("plan proceed"));
        test::check("approved plan executes", approved.ok);
        test::check("file exists after approval",
                    std::filesystem::exists(kTestSandbox + "/planned.txt"));
    });

    test::add("AIKernel — fs mkdir", [] {
        AIKernel::Config cfg;
        cfg.sandboxPath = kTestSandbox;
        cfg.sessionLogPath = "kernel_test_session.txt";
        cfg.auditLogPath   = "kernel_test_audit.log";

        AIKernel kernel{std::move(cfg)};
        kernel.boot();
        CommandRouter router;

        auto mk = kernel.handleCommand(router.parse("fs mkdir TestDir"));
        test::check("fs mkdir ok", mk.ok);
        test::check("directory exists",
                    std::filesystem::is_directory(kTestSandbox + "/TestDir"));
    });

    test::add("AIKernel — foundation install", [] {
        AIKernel::Config cfg;
        cfg.sandboxPath = kTestSandbox;
        cfg.sessionLogPath = "kernel_test_session.txt";
        cfg.auditLogPath   = "kernel_test_audit.log";

        AIKernel kernel{std::move(cfg)};
        kernel.boot();
        CommandRouter router;

        auto resp = kernel.handleCommand(router.parse("/port ans-install-fd"));
        test::check("foundation install ok", resp.ok);
        test::check("response mentions installed",
                    resp.message.find("installed") != std::string::npos);
    });

    test::add("AIKernel — exit command", [] {
        AIKernel::Config cfg;
        cfg.sandboxPath = kTestSandbox;
        cfg.sessionLogPath = "kernel_test_session.txt";
        cfg.auditLogPath   = "kernel_test_audit.log";

        AIKernel kernel{std::move(cfg)};
        kernel.boot();
        CommandRouter router;

        auto resp = kernel.handleCommand(router.parse("exit"));
        test::check("exit ok", resp.ok);
        test::check("not running after exit", !kernel.isRunning());
    });

    test::add("Cleanup test artifacts", [] {
        cleanup();
        // Close the audit log before deleting it (Windows locks open files)
        port::security::AuditLog::instance().close();
        std::error_code ec;
        std::filesystem::remove("kernel_test_audit.log", ec);
        test::check("sandbox removed", !std::filesystem::exists(kTestSandbox));
    });
}

int main() {
    port::security::AuditLog::instance().open("kernel_test_audit.log");
    registerTests();
    int result = test::run();
    port::security::AuditLog::instance().close();
    return result;
}
