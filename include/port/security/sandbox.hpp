#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace port::security {

struct SandboxError {
    enum class Code { PathTraversal, NotFound, PermissionDenied, IOError, NotADirectory };
    Code code;
    std::string detail;
};

template <typename T>
using SandboxResult = std::expected<T, SandboxError>;

struct DirEntry {
    std::string name;
    bool isDirectory{false};
    std::uintmax_t size{0};
};

// Filesystem sandbox: all operations are restricted to rootPath_.
// Uses std::filesystem for cross-platform path resolution.
class Sandbox {
public:
    explicit Sandbox(std::filesystem::path rootPath);

    // Create the sandbox directory tree (System, Apps, Users, Documents, Desktop)
    bool initialize();

    // Safe path resolution: returns absolute path only if within sandbox
    [[nodiscard]] SandboxResult<std::filesystem::path>
        resolvePath(const std::filesystem::path& relative) const;

    // Filesystem operations — all paths are relative to sandbox root
    [[nodiscard]] SandboxResult<std::vector<DirEntry>>
        list(const std::filesystem::path& dir = "") const;

    [[nodiscard]] SandboxResult<std::string>
        read(const std::filesystem::path& file) const;

    [[nodiscard]] SandboxResult<void>
        write(const std::filesystem::path& file, std::string_view content);

    [[nodiscard]] SandboxResult<void>
        makeDir(const std::filesystem::path& dir);

    [[nodiscard]] SandboxResult<void>
        remove(const std::filesystem::path& path);

    [[nodiscard]] SandboxResult<void>
        rename(const std::filesystem::path& from, const std::filesystem::path& to);

    [[nodiscard]] SandboxResult<void>
        moveToTrash(const std::filesystem::path& path);

    [[nodiscard]] SandboxResult<std::vector<DirEntry>> listTrash() const;
    [[nodiscard]] SandboxResult<void> emptyTrash();

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    // Legacy compatibility: convert relative string → native absolute string
    [[nodiscard]] bool isSafePath(const std::string& relative, std::string& outFull) const;

private:
    std::filesystem::path root_;
    std::filesystem::path trash_;
};

} // namespace port::security
