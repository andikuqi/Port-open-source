#include "port/core/service_manager.hpp"
#include <algorithm>
#include <stdexcept>

namespace port::core {

ServiceManager& ServiceManager::instance() {
    static ServiceManager inst;
    return inst;
}

void ServiceManager::registerService(std::shared_ptr<IService> service) {
    std::lock_guard lock{mutex_};
    const auto name = service->name();
    services_[name] = std::move(service);
    order_.push_back(name);
}

void ServiceManager::unregisterService(const std::string& name) {
    std::lock_guard lock{mutex_};
    services_.erase(name);
    std::erase(order_, name);
}

bool ServiceManager::startAll() {
    std::lock_guard lock{mutex_};
    bool ok = true;
    for (const auto& name : order_) {
        if (auto it = services_.find(name); it != services_.end()) {
            if (!it->second->start()) ok = false;
        }
    }
    return ok;
}

void ServiceManager::stopAll() {
    std::lock_guard lock{mutex_};
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        if (auto sit = services_.find(*it); sit != services_.end()) {
            sit->second->stop();
        }
    }
}

bool ServiceManager::startService(const std::string& name) {
    std::lock_guard lock{mutex_};
    auto it = services_.find(name);
    if (it == services_.end()) return false;
    return it->second->start();
}

void ServiceManager::stopService(const std::string& name) {
    std::lock_guard lock{mutex_};
    auto it = services_.find(name);
    if (it != services_.end()) it->second->stop();
}

ServiceState ServiceManager::stateOf(const std::string& name) const {
    std::lock_guard lock{mutex_};
    auto it = services_.find(name);
    if (it == services_.end()) return ServiceState::Stopped;
    return it->second->state();
}

std::shared_ptr<IService> ServiceManager::get(const std::string& name) const {
    std::lock_guard lock{mutex_};
    auto it = services_.find(name);
    return (it != services_.end()) ? it->second : nullptr;
}

} // namespace port::core
