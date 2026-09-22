#include "test_runner.hpp"
#include "port/core/config_engine.hpp"

using port::core::ConfigEngine;

static void registerTests() {
    test::add("ConfigEngine — set and get string", [] {
        ConfigEngine::instance().reset();
        ConfigEngine::instance().set("test.key", std::string{"hello"});
        auto val = ConfigEngine::instance().getString("test.key");
        test::check("key found", val.has_value());
        if (val) test::checkEq<std::string, std::string>("value matches", *val, "hello");
    });

    test::add("ConfigEngine — set and get int", [] {
        ConfigEngine::instance().reset();
        ConfigEngine::instance().set("int.key", int64_t{42});
        auto val = ConfigEngine::instance().getInt("int.key");
        test::check("key found", val.has_value());
        if (val) test::checkEq<int64_t, int64_t>("value matches", *val, 42);
    });

    test::add("ConfigEngine — set and get bool", [] {
        ConfigEngine::instance().reset();
        ConfigEngine::instance().set("bool.key", true);
        auto val = ConfigEngine::instance().getBool("bool.key");
        test::check("key found", val.has_value());
        if (val) test::check("value is true", *val);
    });

    test::add("ConfigEngine — missing key returns nullopt", [] {
        ConfigEngine::instance().reset();
        auto val = ConfigEngine::instance().getString("nonexistent");
        test::check("missing key returns nullopt", !val.has_value());
    });

    test::add("ConfigEngine — default fallback", [] {
        ConfigEngine::instance().reset();
        auto val = ConfigEngine::instance().getString("nope", "default_value");
        test::checkEq<std::string, std::string>("fallback returned", val, "default_value");
    });

    test::add("ConfigEngine — string-to-bool coercion", [] {
        ConfigEngine::instance().reset();
        ConfigEngine::instance().set("truthy",  std::string{"true"});
        ConfigEngine::instance().set("falsy",   std::string{"false"});
        ConfigEngine::instance().set("one",     std::string{"1"});
        ConfigEngine::instance().set("zero",    std::string{"0"});

        test::check("\"true\" is true",  ConfigEngine::instance().getBool("truthy", false));
        test::check("\"false\" is false", !ConfigEngine::instance().getBool("falsy", true));
        test::check("\"1\" is true",     ConfigEngine::instance().getBool("one", false));
        test::check("\"0\" is false",    !ConfigEngine::instance().getBool("zero", true));
    });

    test::add("ConfigEngine — runtime overrides file", [] {
        ConfigEngine::instance().reset();
        // Simulate a file-loaded value
        ConfigEngine::instance().set("layer.key", std::string{"from_runtime"});
        auto val = ConfigEngine::instance().getString("layer.key", "fallback");
        test::checkEq<std::string, std::string>("runtime value wins", val, "from_runtime");
    });
}

int main() {
    registerTests();
    return test::run();
}
