#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace port::core {

enum class ServiceState { Stopped, Starting, Running, Stopping, Failed };

struct IService {
    virtual ~IService() = default;
    virtual std::string name() const = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual ServiceState state() const = 0;
};

// Registry + lifecycle manager for named services.
// Services are started in registration order, stopped in reverse.
class ServiceManager {
public:
    static ServiceManager& instance();

    void registerService(std::shared_ptr<IService> service);
    void unregisterService(const std::string& name);

    bool startAll();
    void stopAll();

    bool startService(const std::string& name);
    void stopService(const std::string& name);

    [[nodiscard]] ServiceState stateOf(const std::string& name) const;
    [[nodiscard]] std::shared_ptr<IService> get(const std::string& name) const;

private:
    ServiceManager() = default;

    mutable std::mutex mutex_;
    std::vector<std::string> order_;
    std::unordered_map<std::string, std::shared_ptr<IService>> services_;
};

} // namespace port::core
