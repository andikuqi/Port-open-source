#include "test_runner.hpp"
#include "port/core/event_bus.hpp"

#include <atomic>
#include <string>

using port::core::EventBus;

struct PingEvent { int value; };
struct TextEvent  { std::string msg; };

static void registerTests() {
    test::add("EventBus — subscribe and publish", [] {
        EventBus bus;
        int received = 0;

        bus.subscribe<PingEvent>([&](const PingEvent& e) { received += e.value; });
        bus.publish(PingEvent{10});
        bus.publish(PingEvent{5});

        test::checkEq<int, int>("received sum = 15", received, 15);
    });

    test::add("EventBus — multiple subscribers", [] {
        EventBus bus;
        int a = 0, b = 0;

        bus.subscribe<PingEvent>([&](const PingEvent& e) { a = e.value; });
        bus.subscribe<PingEvent>([&](const PingEvent& e) { b = e.value * 2; });
        bus.publish(PingEvent{7});

        test::checkEq<int, int>("subscriber A received 7", a, 7);
        test::checkEq<int, int>("subscriber B received 14", b, 14);
    });

    test::add("EventBus — unsubscribe", [] {
        EventBus bus;
        int count = 0;

        const auto id = bus.subscribe<PingEvent>([&](const PingEvent&) { ++count; });
        bus.publish(PingEvent{1});
        bus.unsubscribe(id);
        bus.publish(PingEvent{1});

        test::checkEq<int, int>("only 1 call after unsubscribe", count, 1);
    });

    test::add("EventBus — different event types isolated", [] {
        EventBus bus;
        int pings = 0;
        int texts = 0;

        bus.subscribe<PingEvent>([&](const PingEvent&) { ++pings; });
        bus.subscribe<TextEvent>([&](const TextEvent&) { ++texts; });

        bus.publish(PingEvent{1});
        bus.publish(PingEvent{2});
        bus.publish(TextEvent{"hello"});

        test::checkEq<int, int>("2 pings received", pings, 2);
        test::checkEq<int, int>("1 text received", texts, 1);
    });

    test::add("EventBus — clear removes all subscribers", [] {
        EventBus bus;
        int count = 0;
        bus.subscribe<PingEvent>([&](const PingEvent&) { ++count; });
        bus.clear();
        bus.publish(PingEvent{1});
        test::checkEq<int, int>("no calls after clear", count, 0);
    });

    test::add("EventBus — globalBus is a singleton", [] {
        auto& b1 = port::core::globalBus();
        auto& b2 = port::core::globalBus();
        test::check("same instance", &b1 == &b2);
    });
}

int main() {
    registerTests();
    return test::run();
}
