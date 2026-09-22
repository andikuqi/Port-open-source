#include "port/core/event_bus.hpp"

namespace port::core {

EventBus& globalBus() {
    static EventBus bus;
    return bus;
}

} // namespace port::core
