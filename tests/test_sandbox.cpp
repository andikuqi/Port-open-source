#include "test_runner.hpp"
#include "port/security/sandbox.hpp"
#include "port/security/audit_log.hpp"

#include <filesystem>
#include <string>

using port::security::Sandbox;

static const std::string kTestRoot = "sandbox_test_tmp";

static void cleanup() {
    std::error_code ec;
    std::filesystem::remove_all(kTestRoot, ec);
}

static void registerTests() {
    test::add("Sandbox — initialization", [] {
        cleanup();
        Sandbox sb{kTestRoot};
        test::check("initialize() returns true", sb.initialize());
        test::check("System dir created",    std::filesystem::is_directory(kTestRoot + "/System"));
        test::check("Apps dir created",      std::filesystem::is_directory(kTestRoot + "/Apps"));
        test::check("Documents dir created", std::filesystem::is_directory(kTestRoot + "/Documents"));
        test::check("Desktop dir created",   std::filesystem::is_directory(kTestRoot + "/Desktop"));
    });

    test::add("Sandbox — safe path resolution", [] {
        Sandbox sb{kTestRoot};
        sb.initialize();

        std::string out;
        test::check("normal file is safe",    sb.isSafePath("file.txt", out));
        test::check("subdir file is safe",    sb.isSafePath("docs/file.txt", out));
        test::check("empty path is safe",     sb.isSafePath("", out));
        test::check("traversal ../ blocked",  !sb.isSafePath("../CMakeLists.txt", out));
        test::check("double traversal blocked", !sb.isSafePath("../../etc/passwd", out));
        test::check("absolute path blocked",  !sb.isSafePath("C:/Windows/System32", out));
    });

    test::add("Sandbox — write and read", [] {
        Sandbox sb{kTestRoot};
        sb.initialize();

        auto wr = sb.write("test_file.txt", "Hello Port OS");
        test::check("write succeeds", wr.has_value());

        auto rd = sb.read("test_file.txt");
        test::check("read succeeds", rd.has_value());
        if (rd) {
            test::checkEq<std::string, std::string>("content matches", *rd, "Hello Port OS");
        }
    });

    test::add("Sandbox — list directory", [] {
        Sandbox sb{kTestRoot};
        sb.initialize();
        sb.write("list_test.txt", "data");

        auto listing = sb.list("");
        test::check("list succeeds", listing.has_value());
        if (listing) {
            bool found = false;
            for (const auto& e : *listing) {
                if (e.name == "list_test.txt") { found = true; break; }
            }
            test::check("list contains written file", found);
        }
    });

    test::add("Sandbox — mkdir", [] {
        Sandbox sb{kTestRoot};
        sb.initialize();

        auto mk = sb.makeDir("MyNewDir");
        test::check("mkdir succeeds", mk.has_value());
        test::check("directory exists", std::filesystem::is_directory(kTestRoot + "/MyNewDir"));
    });

    test::add("Sandbox — rename", [] {
        Sandbox sb{kTestRoot};
        sb.initialize();
        sb.write("rename_src.txt", "content");

        auto rn = sb.rename("rename_src.txt", "rename_dst.txt");
        test::check("rename succeeds", rn.has_value());
        test::check("dst exists",  std::filesystem::exists(kTestRoot + "/rename_dst.txt"));
        test::check("src removed", !std::filesystem::exists(kTestRoot + "/rename_src.txt"));
    });

    test::add("Sandbox — trash lifecycle", [] {
        Sandbox sb{kTestRoot};
        sb.initialize();
        sb.write("trash_me.txt", "delete me");

        auto mv = sb.moveToTrash("trash_me.txt");
        test::check("move to trash succeeds", mv.has_value());
        test::check("original gone",  !std::filesystem::exists(kTestRoot + "/trash_me.txt"));
        test::check("in .trash",       std::filesystem::exists(kTestRoot + "/.trash/trash_me.txt"));

        auto trashList = sb.listTrash();
        test::check("listTrash succeeds", trashList.has_value());
        if (trashList) {
            bool found = false;
            for (const auto& e : *trashList) {
                if (e.name == "trash_me.txt") { found = true; break; }
            }
            test::check("trash list contains file", found);
        }

        auto emp = sb.emptyTrash();
        test::check("emptyTrash succeeds", emp.has_value());
        auto afterEmpty = sb.listTrash();
        test::check("trash is empty after emptyTrash",
                    afterEmpty.has_value() && afterEmpty->empty());
    });

    test::add("Sandbox — path traversal blocked", [] {
        Sandbox sb{kTestRoot};
        sb.initialize();

        auto res = sb.read("../CMakeLists.txt");
        test::check("read outside sandbox blocked", !res.has_value());

        auto wr = sb.write("../../evil.txt", "evil");
        test::check("write outside sandbox blocked", !wr.has_value());
    });

    // Cleanup after all tests
    test::add("Sandbox — cleanup", [] {
        cleanup();
        test::check("test root removed", !std::filesystem::exists(kTestRoot));
    });
}

int main() {
    // Suppress audit log to a temp file
    port::security::AuditLog::instance().open("sandbox_test_audit.log");
    registerTests();
    int result = test::run();
    port::security::AuditLog::instance().close();
    std::filesystem::remove("sandbox_test_audit.log");
    return result;
}
