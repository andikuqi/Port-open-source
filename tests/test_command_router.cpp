#include "test_runner.hpp"
#include "port/kernel/command_router.hpp"

using port::kernel::CommandRouter;
using port::kernel::CommandType;

static void registerTests() {
    test::add("CommandRouter — basic keywords", [] {
        CommandRouter r;
        test::check("help", r.parse("help").type == CommandType::Help);
        test::check("status", r.parse("status").type == CommandType::Status);
        test::check("exit", r.parse("exit").type == CommandType::Exit);
        test::check("quit", r.parse("quit").type == CommandType::Exit);
        test::check("whitespace trimmed", r.parse("  help  ").type == CommandType::Help);
    });

    test::add("CommandRouter — plan commands", [] {
        CommandRouter r;
        test::check("plan proceed",  r.parse("plan proceed").type == CommandType::PlanProceed);
        test::check("plan yes",      r.parse("plan yes").type == CommandType::PlanProceed);
        test::check("plan y",        r.parse("plan y").type == CommandType::PlanProceed);
        test::check("plan abort",    r.parse("plan abort").type == CommandType::PlanAbort);
        test::check("plan no",       r.parse("plan no").type == CommandType::PlanAbort);
    });

    test::add("CommandRouter — foundation install", [] {
        CommandRouter r;
        test::check("/port ans-install-fd", r.parse("/port ans-install-fd").type == CommandType::FoundationInstall);
        test::check("/fox ans-install-fd",  r.parse("/fox ans-install-fd").type == CommandType::FoundationInstall);
    });

    test::add("CommandRouter — fs list", [] {
        CommandRouter r;
        test::check("fs list", r.parse("fs list").type == CommandType::FsList);
        test::check("fs ls",   r.parse("fs ls").type == CommandType::FsList);
        test::check("fs",      r.parse("fs").type == CommandType::FsList);

        auto cmd = r.parse("fs list docs");
        test::check("fs list docs → FsList", cmd.type == CommandType::FsList);
        test::checkEq<std::string, std::string>("fs list docs → arg1 = docs", cmd.arg1, "docs");
    });

    test::add("CommandRouter — fs read", [] {
        CommandRouter r;
        auto cmd = r.parse("fs read notes.txt");
        test::check("fs read → FsRead", cmd.type == CommandType::FsRead);
        test::checkEq<std::string, std::string>("fs read arg1", cmd.arg1, "notes.txt");

        auto cmd2 = r.parse("fs cat readme.md");
        test::check("fs cat → FsRead", cmd2.type == CommandType::FsRead);
        test::checkEq<std::string, std::string>("fs cat arg1", cmd2.arg1, "readme.md");
    });

    test::add("CommandRouter — fs write", [] {
        CommandRouter r;
        auto cmd = r.parse("fs write hello.txt \"Hello World\"");
        test::check("fs write → FsWrite", cmd.type == CommandType::FsWrite);
        test::checkEq<std::string, std::string>("fs write arg1", cmd.arg1, "hello.txt");
        test::checkEq<std::string, std::string>("fs write arg2", cmd.arg2, "Hello World");
    });

    test::add("CommandRouter — fs delete", [] {
        CommandRouter r;
        auto cmd = r.parse("fs delete old.txt");
        test::check("fs delete → FsDelete", cmd.type == CommandType::FsDelete);
        test::checkEq<std::string, std::string>("fs delete arg1", cmd.arg1, "old.txt");

        auto cmd2 = r.parse("fs rm old.txt");
        test::check("fs rm → FsDelete", cmd2.type == CommandType::FsDelete);
    });

    test::add("CommandRouter — fs mkdir", [] {
        CommandRouter r;
        auto cmd = r.parse("fs mkdir Documents");
        test::check("fs mkdir → FsMakeDir", cmd.type == CommandType::FsMakeDir);
        test::checkEq<std::string, std::string>("fs mkdir arg1", cmd.arg1, "Documents");
    });

    test::add("CommandRouter — fs rename", [] {
        CommandRouter r;
        auto cmd = r.parse("fs rename old.txt new.txt");
        test::check("fs rename → FsRename", cmd.type == CommandType::FsRename);
        test::checkEq<std::string, std::string>("fs rename arg1", cmd.arg1, "old.txt");
        test::checkEq<std::string, std::string>("fs rename arg2", cmd.arg2, "new.txt");
    });

    test::add("CommandRouter — fs trash / empty-trash", [] {
        CommandRouter r;
        test::check("fs trash", r.parse("fs trash").type == CommandType::FsTrash);
        test::check("fs list-trash", r.parse("fs list-trash").type == CommandType::FsTrash);
        test::check("fs empty-trash", r.parse("fs empty-trash").type == CommandType::FsEmptyTrash);
    });

    test::add("CommandRouter — net download", [] {
        CommandRouter r;
        auto cmd = r.parse("net download https://example.com/file.zip sandbox/file.zip");
        test::check("net download → NetDownload", cmd.type == CommandType::NetDownload);
        test::checkEq<std::string, std::string>("net download url", cmd.arg1, "https://example.com/file.zip");
        test::checkEq<std::string, std::string>("net download dest", cmd.arg2, "sandbox/file.zip");
    });

    test::add("CommandRouter — sys execute", [] {
        CommandRouter r;
        auto cmd = r.parse("sys execute sandbox/app.exe --arg1");
        test::check("sys execute → SysExecute", cmd.type == CommandType::SysExecute);
        test::checkEq<std::string, std::string>("sys execute path", cmd.arg1, "sandbox/app.exe");
        test::checkEq<std::string, std::string>("sys execute args", cmd.arg2, "--arg1");
    });

    test::add("CommandRouter — natural language → Prompt", [] {
        CommandRouter r;
        test::check("free text → Prompt", r.parse("Install Chrome please").type == CommandType::Prompt);
        test::check("ask prefix → Prompt", r.parse("ask what can you do").type == CommandType::Prompt);
    });
}

int main() {
    registerTests();
    return test::run();
}
