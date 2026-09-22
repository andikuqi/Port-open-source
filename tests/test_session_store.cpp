#include "test_runner.hpp"
#include "port/storage/session_store.hpp"

#include <filesystem>

int main() {
    const std::string path = "session_store_test.log";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    test::add("SessionStore — tail returns newest structured entries", [&] {
        port::storage::SessionStore store{path};
        store.append("ONE", "first");
        store.append("TWO", "second");
        store.append("THREE", "third");
        const auto entries = store.tail(2);
        test::check("tail has two entries", entries.size() == 2);
        if (entries.size() == 2) {
            test::checkEq<std::string, std::string>("oldest retained tag", entries[0].tag, "TWO");
            test::checkEq<std::string, std::string>("newest retained content", entries[1].content, "third");
        }
    });

    const int result = test::run();
    std::filesystem::remove(path, ec);
    return result;
}
