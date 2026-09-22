#include "port/security/sandbox.hpp"
#include "port/security/audit_log.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <sstream>

namespace port::security {

namespace {

SandboxError makeError(SandboxError::Code code, const std::string& detail) {
    return {code, detail};
}

SandboxResult<void> voidOk() { return {}; }

bool realPathOfExisting(const std::filesystem::path& path,
                        std::filesystem::path& output,
                        std::error_code& ec) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    CloseHandle(handle);
    if (length == 0 || length >= buffer.size()) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    buffer.resize(length);
    output = std::filesystem::path{buffer}.lexically_normal();
    ec.clear();
    return true;
#else
    output = std::filesystem::canonical(path, ec);
    return !ec;
#endif
}

bool realPathIsWithin(const std::filesystem::path& root,
                      const std::filesystem::path& candidate) {
#ifdef _WIN32
    std::wstring rootText = root.native();
    std::wstring candidateText = candidate.native();
    std::transform(rootText.begin(), rootText.end(), rootText.begin(), ::towlower);
    std::transform(candidateText.begin(), candidateText.end(), candidateText.begin(), ::towlower);
#else
    std::string rootText = root.native();
    std::string candidateText = candidate.native();
#endif
    if (candidateText == rootText) return true;
    if (!rootText.empty() && rootText.back() != std::filesystem::path::preferred_separator)
        rootText.push_back(std::filesystem::path::preferred_separator);
    return candidateText.rfind(rootText, 0) == 0;
}

} // namespace

Sandbox::Sandbox(std::filesystem::path rootPath)
    : root_{[&rootPath] {
          // The sandbox root is allowed not to exist yet. Some standard
          // library implementations throw from weakly_canonical in that
          // situation, which made repeated boots/tests fail after cleanup.
          std::error_code ec;
          auto absoluteRoot = std::filesystem::absolute(rootPath, ec);
          return (ec ? rootPath : absoluteRoot).lexically_normal();
      }()}
    , trash_{root_ / ".trash"}
{}

bool Sandbox::initialize() {
    std::error_code ec;
    std::filesystem::create_directories(root_ / "System",    ec);
    std::filesystem::create_directories(root_ / "Apps",      ec);
    std::filesystem::create_directories(root_ / "Users",     ec);
    std::filesystem::create_directories(root_ / "Documents", ec);
    std::filesystem::create_directories(root_ / "Desktop",   ec);
    std::filesystem::create_directories(trash_,              ec);
    AuditLog::instance().record(AuditAction::KernelBoot, root_.string(), !ec);
    return !ec;
}

SandboxResult<std::filesystem::path>
Sandbox::resolvePath(const std::filesystem::path& relative) const {
    // Reject absolute paths
    if (relative.is_absolute()) {
        return std::unexpected(makeError(SandboxError::Code::PathTraversal,
                                         "Absolute path rejected: " + relative.string()));
    }

    // Lexically resolve to catch "../" traversal without requiring the path to exist
    const auto combined = (root_ / relative).lexically_normal();

    // Check that combined begins with root_
    const auto rootNorm = root_.lexically_normal();
    auto [rootEnd, combinedEnd] = std::mismatch(
        rootNorm.begin(), rootNorm.end(),
        combined.begin(), combined.end());

    if (rootEnd != rootNorm.end()) {
        return std::unexpected(makeError(SandboxError::Code::PathTraversal,
                                         "Path escapes sandbox: " + relative.string()));
    }

    // Lexical checks alone can be bypassed through a symlink/reparse point
    // located inside the sandbox. Canonicalize the nearest existing ancestor
    // and verify that its real location still belongs to the real root.
    std::error_code ec;
    std::filesystem::path canonicalRoot;
    if (!realPathOfExisting(root_, canonicalRoot, ec)) {
        return std::unexpected(makeError(SandboxError::Code::IOError,
                                         "Cannot resolve sandbox root: " + ec.message()));
    }

    auto existingAncestor = combined;
    while (!existingAncestor.empty() &&
           !std::filesystem::exists(existingAncestor, ec)) {
        ec.clear();
        const auto parent = existingAncestor.parent_path();
        if (parent == existingAncestor) break;
        existingAncestor = parent;
    }
    std::filesystem::path canonicalAncestor;
    if (!realPathOfExisting(existingAncestor, canonicalAncestor, ec)) {
        return std::unexpected(makeError(SandboxError::Code::IOError,
                                         "Cannot resolve path: " + ec.message()));
    }
    if (!realPathIsWithin(canonicalRoot, canonicalAncestor)) {
        return std::unexpected(makeError(SandboxError::Code::PathTraversal,
                                         "Path resolves outside sandbox: " + relative.string()));
    }

    return combined;
}

SandboxResult<std::vector<DirEntry>>
Sandbox::list(const std::filesystem::path& dir) const {
    auto resolved = resolvePath(dir);
    if (!resolved) {
        AuditLog::instance().record(AuditAction::FsRead, dir.string(), false);
        return std::unexpected(resolved.error());
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(*resolved, ec)) {
        AuditLog::instance().record(AuditAction::FsRead, dir.string(), false);
        return std::unexpected(makeError(SandboxError::Code::NotADirectory,
                                         resolved->string() + " is not a directory"));
    }

    std::vector<DirEntry> entries;
    for (const auto& entry : std::filesystem::directory_iterator{*resolved, ec}) {
        if (ec) break;
        DirEntry de;
        de.name        = entry.path().filename().string();
        de.isDirectory = entry.is_directory(ec);
        de.size        = entry.is_regular_file(ec) ? entry.file_size(ec) : 0;
        if (de.name != ".trash") {
            entries.push_back(std::move(de));
        }
    }

    // Sort: directories first, then files, both alphabetically
    std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
        return a.name < b.name;
    });

    AuditLog::instance().record(AuditAction::FsRead, dir.string(), true);
    return entries;
}

SandboxResult<std::string>
Sandbox::read(const std::filesystem::path& file) const {
    auto resolved = resolvePath(file);
    if (!resolved) {
        AuditLog::instance().record(AuditAction::FsRead, file.string(), false);
        return std::unexpected(resolved.error());
    }

    std::ifstream f{*resolved, std::ios::binary};
    if (!f) {
        AuditLog::instance().record(AuditAction::FsRead, file.string(), false);
        return std::unexpected(makeError(SandboxError::Code::NotFound,
                                         "Cannot open: " + resolved->string()));
    }

    std::ostringstream oss;
    oss << f.rdbuf();
    AuditLog::instance().record(AuditAction::FsRead, file.string(), true);
    return oss.str();
}

SandboxResult<void>
Sandbox::write(const std::filesystem::path& file, std::string_view content) {
    auto resolved = resolvePath(file);
    if (!resolved) {
        AuditLog::instance().record(AuditAction::FsWrite, file.string(), false);
        return std::unexpected(resolved.error());
    }

    // Auto-create parent directories
    std::error_code ec;
    std::filesystem::create_directories(resolved->parent_path(), ec);

    std::ofstream f{*resolved, std::ios::binary | std::ios::trunc};
    if (!f) {
        AuditLog::instance().record(AuditAction::FsWrite, file.string(), false);
        return std::unexpected(makeError(SandboxError::Code::PermissionDenied,
                                         "Cannot write: " + resolved->string()));
    }
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    AuditLog::instance().record(AuditAction::FsWrite, file.string(), true);
    return voidOk();
}

SandboxResult<void>
Sandbox::makeDir(const std::filesystem::path& dir) {
    auto resolved = resolvePath(dir);
    if (!resolved) {
        AuditLog::instance().record(AuditAction::FsMkDir, dir.string(), false);
        return std::unexpected(resolved.error());
    }

    std::error_code ec;
    std::filesystem::create_directories(*resolved, ec);
    AuditLog::instance().record(AuditAction::FsMkDir, dir.string(), !ec);
    if (ec) {
        return std::unexpected(makeError(SandboxError::Code::IOError, ec.message()));
    }
    return voidOk();
}

SandboxResult<void>
Sandbox::remove(const std::filesystem::path& path) {
    return moveToTrash(path);
}

SandboxResult<void>
Sandbox::rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    auto rFrom = resolvePath(from);
    auto rTo   = resolvePath(to);
    if (!rFrom) {
        AuditLog::instance().record(AuditAction::FsRename, from.string() + " -> " + to.string(), false);
        return std::unexpected(rFrom.error());
    }
    if (!rTo) {
        AuditLog::instance().record(AuditAction::FsRename, from.string() + " -> " + to.string(), false);
        return std::unexpected(rTo.error());
    }

    std::error_code ec;
    std::filesystem::rename(*rFrom, *rTo, ec);
    AuditLog::instance().record(AuditAction::FsRename,
                                from.string() + " -> " + to.string(), !ec);
    if (ec) return std::unexpected(makeError(SandboxError::Code::IOError, ec.message()));
    return voidOk();
}

SandboxResult<void>
Sandbox::moveToTrash(const std::filesystem::path& path) {
    // Prevent trashing the trash itself
    if (path == ".trash" || path.string().rfind(".trash/", 0) == 0) {
        return std::unexpected(makeError(SandboxError::Code::PermissionDenied,
                                         "Cannot delete .trash"));
    }

    auto resolved = resolvePath(path);
    if (!resolved) {
        AuditLog::instance().record(AuditAction::FsTrash, path.string(), false);
        return std::unexpected(resolved.error());
    }

    std::error_code ec;
    std::filesystem::create_directories(trash_, ec);

    const auto dest = trash_ / resolved->filename();
    std::filesystem::rename(*resolved, dest, ec);
    AuditLog::instance().record(AuditAction::FsTrash,
                                path.string() + " -> .trash/" + resolved->filename().string(),
                                !ec);
    if (ec) return std::unexpected(makeError(SandboxError::Code::IOError, ec.message()));
    return voidOk();
}

SandboxResult<std::vector<DirEntry>> Sandbox::listTrash() const {
    std::error_code ec;
    if (!std::filesystem::is_directory(trash_, ec)) {
        return std::vector<DirEntry>{};
    }

    std::vector<DirEntry> entries;
    for (const auto& entry : std::filesystem::directory_iterator{trash_, ec}) {
        if (ec) break;
        DirEntry de;
        de.name        = entry.path().filename().string();
        de.isDirectory = entry.is_directory(ec);
        de.size        = entry.is_regular_file(ec) ? entry.file_size(ec) : 0;
        entries.push_back(std::move(de));
    }
    return entries;
}

SandboxResult<void> Sandbox::emptyTrash() {
    std::error_code ec;
    std::filesystem::remove_all(trash_, ec);
    std::filesystem::create_directories(trash_, ec);
    AuditLog::instance().record(AuditAction::FsTrash, ".trash (empty)", !ec);
    if (ec) return std::unexpected(makeError(SandboxError::Code::IOError, ec.message()));
    return voidOk();
}

// Legacy compat
bool Sandbox::isSafePath(const std::string& relative, std::string& outFull) const {
    auto res = resolvePath(relative);
    if (!res) return false;
    outFull = res->string();
    return true;
}

} // namespace port::security
