#pragma once

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace test {

struct Case {
    std::string name;
    std::function<void()> fn;
};

inline int passed = 0;
inline int failed = 0;
inline std::vector<Case> cases;

inline void add(std::string name, std::function<void()> fn) {
    cases.push_back({std::move(name), std::move(fn)});
}

inline void check(const std::string& name, bool condition) {
    if (condition) {
        std::cout << "  [PASS] " << name << '\n';
        ++passed;
    } else {
        std::cout << "  [FAIL] " << name << '\n';
        ++failed;
    }
}

template <typename A, typename B>
inline void checkEq(const std::string& name, const A& actual, const B& expected) {
    if (actual == expected) {
        std::cout << "  [PASS] " << name << '\n';
        ++passed;
    } else {
        std::cout << "  [FAIL] " << name << '\n';
        std::cout << "         expected: " << expected << '\n';
        std::cout << "         actual  : " << actual   << '\n';
        ++failed;
    }
}

inline int run() {
    for (const auto& c : cases) {
        std::cout << "\n[TEST] " << c.name << '\n';
        try {
            c.fn();
        } catch (const std::exception& ex) {
            std::cout << "  [EXCEPTION] " << ex.what() << '\n';
            ++failed;
        }
    }
    std::cout << "\n==============================\n"
              << "  Passed: " << passed << '\n'
              << "  Failed: " << failed << '\n'
              << "==============================\n";
    return failed > 0 ? 1 : 0;
}

} // namespace test
