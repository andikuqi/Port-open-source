#pragma once

#include <algorithm>
#include <any>
#include <functional>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace port::core {

// Type-safe synchronous event bus.
// Publishers call publish<EventType>(event) and subscribers register via
// subscribe<EventType>(handler). Thread-safe: handlers are called under
// no lock, but the subscription list is protected by a mutex.
class EventBus {
public:
    using HandlerId = std::size_t;

    template <typename Event>
    [[nodiscard]] HandlerId subscribe(std::function<void(const Event&)> handler) {
        std::lock_guard lock{mutex_};
        const auto key = std::type_index{typeid(Event)};
        const HandlerId id = nextId_++;
        subscribers_[key].push_back({id, [h = std::move(handler)](const std::any& ev) {
            h(std::any_cast<const Event&>(ev));
        }});
        return id;
    }

    void unsubscribe(HandlerId id) {
        std::lock_guard lock{mutex_};
        for (auto& [key, vec] : subscribers_) {
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                          [id](const Entry& e) { return e.id == id; }),
                      vec.end());
        }
    }

    template <typename Event>
    void publish(Event&& event) {
        const auto key = std::type_index{typeid(Event)};
        std::vector<Entry> snap;
        {
            std::lock_guard lock{mutex_};
            auto it = subscribers_.find(key);
            if (it != subscribers_.end()) {
                snap = it->second;
            }
        }
        const std::any boxed{std::forward<Event>(event)};
        for (const auto& entry : snap) {
            entry.handler(boxed);
        }
    }

    void clear() {
        std::lock_guard lock{mutex_};
        subscribers_.clear();
    }

private:
    struct Entry {
        HandlerId id;
        std::function<void(const std::any&)> handler;
    };

    std::mutex mutex_;
    std::unordered_map<std::type_index, std::vector<Entry>> subscribers_;
    HandlerId nextId_{0};
};

// Global singleton accessor — inject in production code instead.
EventBus& globalBus();

} // namespace port::core
