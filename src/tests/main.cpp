#include "fox/command_router.hpp"
#include "fox/ai_kernel.hpp"

#include <iostream>
#include <string>
#include <cassert>

namespace {

int passed = 0;
int failed = 0;

void checkTrue(const std::string& testName, bool condition)
{
    if (condition) {
        std::cout << "  [PASS] " << testName << "\n";
        ++passed;
    } else {
        std::cout << "  [FAIL] " << testName << "\n";
        ++failed;
    }
}

void checkEqual(const std::string& testName, const std::string& a, const std::string& b)
{
    if (a == b) {
        std::cout << "  [PASS] " << testName << "\n";
        ++passed;
    } else {
        std::cout << "  [FAIL] " << testName << "\n";
        std::cout << "         Expected: \"" << b << "\"\n";
        std::cout << "         Got:      \"" << a << "\"\n";
        ++failed;
    }
}

void testCommandRouterParsing()
{
    std::cout << "\n[TEST GROUP] CommandRouter parsing\n";
    fox::CommandRouter router;

    auto cmd = router.parse("help");
    checkTrue("'help' -> Help", cmd.type == fox::CommandType::Help);

    cmd = router.parse("status");
    checkTrue("'status' -> Status", cmd.type == fox::CommandType::Status);

    cmd = router.parse("/port ans-install-fd");
    checkTrue("'/port ans-install-fd' -> FoundationInstall", cmd.type == fox::CommandType::FoundationInstall);

    cmd = router.parse("/fox ans-install-fd");
    checkTrue("'/fox ans-install-fd' -> FoundationInstall", cmd.type == fox::CommandType::FoundationInstall);

    cmd = router.parse("exit");
    checkTrue("'exit' -> Exit", cmd.type == fox::CommandType::Exit);

    cmd = router.parse("quit");
    checkTrue("'quit' -> Exit", cmd.type == fox::CommandType::Exit);

    cmd = router.parse("fs list");
    checkTrue("'fs list' -> FsList", cmd.type == fox::CommandType::FsList);

    cmd = router.parse("fs ls");
    checkTrue("'fs ls' -> FsList", cmd.type == fox::CommandType::FsList);

    cmd = router.parse("fs read test.txt");
    checkTrue("'fs read test.txt' -> FsRead", cmd.type == fox::CommandType::FsRead);
    checkEqual("'fs read test.txt' arg1 = test.txt", cmd.arg1, "test.txt");

    cmd = router.parse("fs cat notes.txt");
    checkTrue("'fs cat notes.txt' -> FsRead", cmd.type == fox::CommandType::FsRead);
    checkEqual("'fs cat notes.txt' arg1 = notes.txt", cmd.arg1, "notes.txt");

    cmd = router.parse("fs write hello.txt \"Hello World\"");
    checkTrue("'fs write hello.txt ...' -> FsWrite", cmd.type == fox::CommandType::FsWrite);
    checkEqual("FsWrite arg1 = hello.txt", cmd.arg1, "hello.txt");
    checkEqual("FsWrite arg2 = Hello World", cmd.arg2, "Hello World");

    cmd = router.parse("fs delete old.txt");
    checkTrue("'fs delete old.txt' -> FsDelete", cmd.type == fox::CommandType::FsDelete);
    checkEqual("FsDelete arg1 = old.txt", cmd.arg1, "old.txt");

    cmd = router.parse("fs rm old.txt");
    checkTrue("'fs rm old.txt' -> FsDelete", cmd.type == fox::CommandType::FsDelete);

    cmd = router.parse("plan proceed");
    checkTrue("'plan proceed' -> PlanProceed", cmd.type == fox::CommandType::PlanProceed);

    cmd = router.parse("plan abort");
    checkTrue("'plan abort' -> PlanAbort", cmd.type == fox::CommandType::PlanAbort);

    cmd = router.parse("  help  ");
    checkTrue("Whitespace trimmed -> Help", cmd.type == fox::CommandType::Help);
}

void testSandboxSafety()
{
    std::cout << "\n[TEST GROUP] Sandbox path safety\n";
    fox::Sandbox sandbox("sandbox");
    sandbox.createSandboxDir();

    std::string out;
    checkTrue("Normal file is safe", sandbox.isSafePath("test.txt", out));
    checkTrue("Subdir file is safe", sandbox.isSafePath("subdir/file.txt", out));
    checkTrue("Empty string (root) is safe", sandbox.isSafePath("", out));
    checkTrue("Traversal ../ is blocked", !sandbox.isSafePath("../CMakeLists.txt", out));
    checkTrue("Absolute path is blocked", !sandbox.isSafePath("C:/Windows/System32/cmd.exe", out));
    checkTrue("Double traversal blocked", !sandbox.isSafePath("../../etc/passwd", out));
}

void testSandboxFileOps()
{
    std::cout << "\n[TEST GROUP] Sandbox file operations\n";
    fox::Sandbox sandbox("sandbox");
    sandbox.createSandboxDir();

    bool ok = sandbox.write("unit_test.txt", "Port OS unit test");
    checkTrue("Write file succeeds", ok);

    bool readOk = false;
    std::string content = sandbox.read("unit_test.txt", readOk);
    checkTrue("Read file succeeds", readOk);
    checkEqual("Read content matches written content", content, "Port OS unit test");

    bool listOk = false;
    std::string listing = sandbox.list("", listOk);
    checkTrue("List sandbox succeeds", listOk);
    checkTrue("Listing contains unit_test.txt", listing.find("unit_test.txt") != std::string::npos);

    bool trash = sandbox.moveToTrash("unit_test.txt");
    checkTrue("Move to trash succeeds", trash);

    bool trashOk = false;
    std::string trashList = sandbox.listTrash(trashOk);
    checkTrue("List trash succeeds", trashOk);
    checkTrue("Trash listing contains unit_test.txt", trashList.find("unit_test.txt") != std::string::npos);

    bool empty = sandbox.emptyTrash();
    checkTrue("Empty trash succeeds", empty);
}

void testAIKernelCommands()
{
    std::cout << "\n[TEST GROUP] AIKernel command execution\n";
    fox::AIKernel kernel;

    auto bootResp = kernel.boot();
    checkTrue("Boot succeeds", bootResp.ok);
    checkTrue("Kernel is running after boot", kernel.isRunning());

    fox::CommandRouter router;

    auto helpResp = kernel.handleCommand(router.parse("help"));
    checkTrue("help command returns ok", helpResp.ok);
    checkTrue("help response contains 'fs write'", helpResp.message.find("fs write") != std::string::npos);

    auto statusResp = kernel.handleCommand(router.parse("status"));
    checkTrue("status command returns ok", statusResp.ok);
    checkTrue("status includes 'AI Kernel: running'", statusResp.message.find("AI Kernel: running") != std::string::npos);

    auto writeResp = kernel.handleCommand(router.parse("fs write kern_test.txt \"kernel test\""));
    checkTrue("fs write via kernel ok", writeResp.ok);

    auto readResp = kernel.handleCommand(router.parse("fs read kern_test.txt"));
    checkTrue("fs read via kernel ok", readResp.ok);
    checkEqual("fs read content", readResp.message, "kernel test");

    auto deleteResp = kernel.handleCommand(router.parse("fs delete kern_test.txt"));
    checkTrue("fs delete returns CONFIRM_REQUIRED", deleteResp.ok);
    checkTrue("fs delete response is CONFIRM_REQUIRED", deleteResp.message.rfind("CONFIRM_REQUIRED:", 0) == 0);

    fox::Command confirmCmd = router.parse("fs delete kern_test.txt");
    confirmCmd.arg2 = "yes";
    auto confirmResp = kernel.handleCommand(confirmCmd);
    checkTrue("Confirmed fs delete ok", confirmResp.ok);

    auto shutResp = kernel.handleCommand(router.parse("exit"));
    checkTrue("Exit shuts down kernel", shutResp.ok);
    checkTrue("Kernel is stopped after exit", !kernel.isRunning());
}

} // namespace

int main()
{
    std::cout << "Port OS - Automated Unit Tests\n";
    std::cout << "=================================\n";

    testCommandRouterParsing();
    testSandboxSafety();
    testSandboxFileOps();
    testAIKernelCommands();

    std::cout << "\n=================================\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed.\n";

    return failed > 0 ? 1 : 0;
}
