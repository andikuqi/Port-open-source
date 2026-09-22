#pragma once

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace port::security {

enum class AuditAction {
    FsRead, FsWrite, FsDelete, FsMkDir, FsRename, FsTrash,
    NetDownload, SysExecute,
    AIRequest, AIResponse,
    KernelBoot, KernelShutdown,
    PermissionDenied
};

[[nodiscard]] constexpr std::string_view actionName(AuditAction a) noexcept {
    switch (a) {
    case AuditAction::FsRead:          return "FS_READ";
    case AuditAction::FsWrite:         return "FS_WRITE";
    case AuditAction::FsDelete:        return "FS_DELETE";
    case AuditAction::FsMkDir:         return "FS_MKDIR";
    case AuditAction::FsRename:        return "FS_RENAME";
    case AuditAction::FsTrash:         return "FS_TRASH";
    case AuditAction::NetDownload:     return "NET_DOWNLOAD";
    case AuditAction::SysExecute:      return "SYS_EXECUTE";
    case AuditAction::AIRequest:       return "AI_REQUEST";
    case AuditAction::AIResponse:      return "AI_RESPONSE";
    case AuditAction::KernelBoot:      return "KERNEL_BOOT";
    case AuditAction::KernelShutdown:  return "KERNEL_SHUTDOWN";
    case AuditAction::PermissionDenied:return "PERMISSION_DENIED";
    }
    return "UNKNOWN";
}

// Append-only structured audit log.
// Each record is written as a single CSV line: timestamp,action,detail,outcome
class AuditLog {
public:
    static AuditLog& instance();

    void open(const std::string& path);
    void close();

    void record(AuditAction action,
                std::string_view detail,
                bool success,
                std::string_view actor = "kernel");

private:
    AuditLog() = default;
    static std::string timestamp();

    std::mutex mutex_;
    std::ofstream file_;
};

} // namespace port::security
