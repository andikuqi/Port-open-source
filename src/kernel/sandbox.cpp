#define NOMINMAX
#include "fox/sandbox.hpp"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#endif

#include <algorithm>
#include <fstream>
#include <sstream>
#include <ctime>
#include <cstdlib>
#include <vector>

namespace fox {

Sandbox::Sandbox(const std::string& rootPath)
{
#ifdef _WIN32
    char resolved[MAX_PATH];
    DWORD len = GetFullPathNameA(rootPath.c_str(), MAX_PATH, resolved, nullptr);
    if (len > 0 && len < MAX_PATH) {
        rootPath_ = resolved;
    } else {
        rootPath_ = rootPath;
    }
    std::replace(rootPath_.begin(), rootPath_.end(), '\\', '/');
#else
    char resolved[4096];
    if (realpath(rootPath.c_str(), resolved) != nullptr) {
        rootPath_ = resolved;
    } else {
        // Directory may not exist yet; just use the given path as-is
        rootPath_ = rootPath;
    }
#endif
}

bool Sandbox::createSandboxDir() const
{
#ifdef _WIN32
    std::string nativeRoot = rootPath_;
    std::replace(nativeRoot.begin(), nativeRoot.end(), '/', '\\');

    DWORD attr = GetFileAttributesA(nativeRoot.c_str());
    bool created = false;
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        created = true;
    } else {
        created = (CreateDirectoryA(nativeRoot.c_str(), nullptr) != 0);
    }

    if (created) {
        CreateDirectoryA((rootPath_ + "/System").c_str(), nullptr);
        CreateDirectoryA((rootPath_ + "/Apps").c_str(), nullptr);
        CreateDirectoryA((rootPath_ + "/Users").c_str(), nullptr);
        CreateDirectoryA((rootPath_ + "/Documents").c_str(), nullptr);
        return true;
    }
    return false;
#else
    struct stat st;
    bool created = false;
    if (stat(rootPath_.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            created = true;
        }
    } else {
        created = (mkdir(rootPath_.c_str(), 0777) == 0);
    }

    if (created) {
        mkdir((rootPath_ + "/System").c_str(), 0777);
        mkdir((rootPath_ + "/Apps").c_str(), 0777);
        mkdir((rootPath_ + "/Users").c_str(), 0777);
        mkdir((rootPath_ + "/Documents").c_str(), 0777);
        return true;
    }
    return false;
#endif
}

bool Sandbox::isSafePath(const std::string& path, std::string& outFullPath) const
{
    if (!path.empty()) {
        if (path[0] == '/' || path[0] == '\\') {
            return false;
        }
        if (path.size() >= 2 && path[1] == ':' &&
            ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z'))) {
            return false;
        }
    }

    std::string combined = rootPath_ + "/" + path;

#ifdef _WIN32
    char resolved[MAX_PATH];
    DWORD len = GetFullPathNameA(combined.c_str(), MAX_PATH, resolved, nullptr);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }
    std::string fullPath(resolved);
    std::replace(fullPath.begin(), fullPath.end(), '\\', '/');
    outFullPath = resolved;
#else
    // On Linux, realpath requires the path to exist; use manual normalization
    std::string fullPath;
    // Manually resolve ".." and "." from the combined path
    std::vector<std::string> parts;
    std::istringstream ss(combined);
    std::string segment;
    while (std::getline(ss, segment, '/')) {
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(segment);
        }
    }
    fullPath = "/";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) fullPath += "/";
        fullPath += parts[i];
    }
    outFullPath = fullPath;
#endif

    std::string normalizedRoot = rootPath_;
    if (!normalizedRoot.empty() && normalizedRoot.back() == '/') {
        normalizedRoot.pop_back();
    }
    // Normalize root slashes
    std::replace(normalizedRoot.begin(), normalizedRoot.end(), '\\', '/');

    if (fullPath == normalizedRoot) {
        return true;
    }

    std::string prefix = normalizedRoot + "/";
    if (fullPath.rfind(prefix, 0) == 0) {
        return true;
    }

    return false;
}

std::string Sandbox::list(const std::string& dir, bool& ok)
{
    std::string fullDir;
    if (!isSafePath(dir, fullDir)) {
        logOperation("LIST", dir, false);
        ok = false;
        return "Security Violation: Attempted to escape sandbox.";
    }

#ifdef _WIN32
    std::string searchPath = fullDir;
    if (!searchPath.empty() && searchPath.back() != '\\' && searchPath.back() != '/') {
        searchPath += "\\*";
    } else {
        searchPath += "*";
    }

    WIN32_FIND_DATAA findData;
    HANDLE findHandle = FindFirstFileA(searchPath.c_str(), &findData);

    if (findHandle == INVALID_HANDLE_VALUE) {
        logOperation("LIST", dir, false);
        ok = false;
        return "Directory not found or cannot be opened.";
    }

    std::ostringstream result;
    result << "Contents of sandbox/" << dir << ":\n";

    bool hasFiles = false;
    do {
        std::string filename = findData.cFileName;
        if (filename == "." || filename == "..") {
            continue;
        }

        hasFiles = true;
        result << "  " << filename;
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            result << "/";
        }
        result << "\n";
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);

    if (!hasFiles) {
        result << "  (empty)\n";
    }

    logOperation("LIST", dir, true);
    ok = true;
    return result.str();
#else
    DIR* dp = opendir(fullDir.c_str());
    if (!dp) {
        logOperation("LIST", dir, false);
        ok = false;
        return "Directory not found or cannot be opened.";
    }

    std::ostringstream result;
    result << "Contents of sandbox/" << dir << ":\n";

    bool hasFiles = false;
    struct dirent* ep;
    while ((ep = readdir(dp))) {
        std::string filename = ep->d_name;
        if (filename == "." || filename == "..") {
            continue;
        }

        hasFiles = true;
        result << "  " << filename;
        
        std::string fullPath = fullDir + "/" + filename;
        struct stat st;
        if (stat(fullPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            result << "/";
        }
        result << "\n";
    }
    closedir(dp);

    if (!hasFiles) {
        result << "  (empty)\n";
    }

    logOperation("LIST", dir, true);
    ok = true;
    return result.str();
#endif
}

std::string Sandbox::read(const std::string& filename, bool& ok)
{
    std::string fullPath;
    if (!isSafePath(filename, fullPath)) {
        logOperation("READ", filename, false);
        ok = false;
        return "Security Violation: Attempted to escape sandbox.";
    }

    std::ifstream file(fullPath, std::ios::binary);
    if (!file) {
        logOperation("READ", filename, false);
        ok = false;
        return "File not found or cannot be opened.";
    }

    std::ostringstream content;
    content << file.rdbuf();

    logOperation("READ", filename, true);
    ok = true;
    return content.str();
}

bool Sandbox::write(const std::string& filename, const std::string& content)
{
    std::string fullPath;
    if (!isSafePath(filename, fullPath)) {
        logOperation("WRITE", filename, false);
        return false;
    }

    std::ofstream file(fullPath, std::ios::binary);
    if (!file) {
        logOperation("WRITE", filename, false);
        return false;
    }

    file << content;
    logOperation("WRITE", filename, true);
    return true;
}

bool Sandbox::deleteFile(const std::string& filename)
{
    return moveToTrash(filename);
}

bool Sandbox::makeDir(const std::string& dirPath)
{
    std::string fullPath;
    if (!isSafePath(dirPath, fullPath)) {
        logOperation("MKDIR", dirPath, false);
        return false;
    }
#ifdef _WIN32
    std::string nativePath = fullPath;
    std::replace(nativePath.begin(), nativePath.end(), '/', '\\');
    BOOL ok = CreateDirectoryA(nativePath.c_str(), nullptr);
    logOperation("MKDIR", dirPath, ok != 0);
    return ok != 0;
#else
    int ret = mkdir(fullPath.c_str(), 0777);
    logOperation("MKDIR", dirPath, ret == 0);
    return ret == 0;
#endif
}

bool Sandbox::renameEntry(const std::string& oldPath, const std::string& newPath)
{
    std::string fullOld, fullNew;
    if (!isSafePath(oldPath, fullOld) || !isSafePath(newPath, fullNew)) {
        logOperation("RENAME", oldPath + " -> " + newPath, false);
        return false;
    }
#ifdef _WIN32
    std::string nativeOld = fullOld, nativeNew = fullNew;
    std::replace(nativeOld.begin(), nativeOld.end(), '/', '\\');
    std::replace(nativeNew.begin(), nativeNew.end(), '/', '\\');
    BOOL ok = MoveFileExA(nativeOld.c_str(), nativeNew.c_str(), MOVEFILE_REPLACE_EXISTING);
    logOperation("RENAME", oldPath + " -> " + newPath, ok != 0);
    return ok != 0;
#else
    int ret = std::rename(fullOld.c_str(), fullNew.c_str());
    logOperation("RENAME", oldPath + " -> " + newPath, ret == 0);
    return ret == 0;
#endif
}

bool Sandbox::moveToTrash(const std::string& filename)
{
    std::string fullPath;
    if (!isSafePath(filename, fullPath)) {
        logOperation("TRASH_MOVE", filename, false);
        return false;
    }

    if (filename == ".trash" || filename.rfind(".trash/", 0) == 0) {
        logOperation("TRASH_MOVE", filename, false);
        return false;
    }

    std::string trashDir = rootPath_ + "/.trash";
#ifdef _WIN32
    std::string nativeTrashDir = trashDir;
    std::replace(nativeTrashDir.begin(), nativeTrashDir.end(), '/', '\\');
    CreateDirectoryA(nativeTrashDir.c_str(), nullptr);
#else
    mkdir(trashDir.c_str(), 0777);
#endif

    std::size_t lastSlash = filename.find_last_of("/\\");
    std::string rawName = (lastSlash == std::string::npos) ? filename : filename.substr(lastSlash + 1);

    std::string destPath = trashDir + "/" + rawName;

#ifdef _WIN32
    std::string nativeDestPath = destPath;
    std::replace(nativeDestPath.begin(), nativeDestPath.end(), '/', '\\');

    std::string nativeSrcPath = fullPath;
    std::replace(nativeSrcPath.begin(), nativeSrcPath.end(), '/', '\\');

    BOOL success = MoveFileExA(nativeSrcPath.c_str(), nativeDestPath.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    bool success = (std::rename(fullPath.c_str(), destPath.c_str()) == 0);
#endif

    logOperation("TRASH_MOVE", filename + " -> .trash/" + rawName, success != 0);
    return success != 0;
}

std::string Sandbox::listTrash(bool& ok)
{
    std::string trashDir = rootPath_ + "/.trash";
#ifdef _WIN32
    std::string nativeTrashDir = trashDir;
    std::replace(nativeTrashDir.begin(), nativeTrashDir.end(), '/', '\\');

    std::string searchPath = nativeTrashDir + "\\*";

    WIN32_FIND_DATAA findData;
    HANDLE findHandle = FindFirstFileA(searchPath.c_str(), &findData);

    if (findHandle == INVALID_HANDLE_VALUE) {
        logOperation("LIST_TRASH", "", true);
        ok = true;
        return "Trash Bin is empty.\n";
    }

    std::ostringstream result;
    result << "Contents of virtual Trash Bin:\n";

    bool hasFiles = false;
    do {
        std::string filename = findData.cFileName;
        if (filename == "." || filename == "..") {
            continue;
        }

        hasFiles = true;
        result << "  " << filename;
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            result << "/";
        }
        result << "\n";
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);

    if (!hasFiles) {
        result.str("");
        result << "Trash Bin is empty.\n";
    }

    logOperation("LIST_TRASH", "", true);
    ok = true;
    return result.str();
#else
    DIR* dp = opendir(trashDir.c_str());
    if (!dp) {
        logOperation("LIST_TRASH", "", true);
        ok = true;
        return "Trash Bin is empty.\n";
    }

    std::ostringstream result;
    result << "Contents of virtual Trash Bin:\n";

    bool hasFiles = false;
    struct dirent* ep;
    while ((ep = readdir(dp))) {
        std::string filename = ep->d_name;
        if (filename == "." || filename == "..") {
            continue;
        }

        hasFiles = true;
        result << "  " << filename;
        
        std::string fullPath = trashDir + "/" + filename;
        struct stat st;
        if (stat(fullPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            result << "/";
        }
        result << "\n";
    }
    closedir(dp);

    if (!hasFiles) {
        result.str("");
        result << "Trash Bin is empty.\n";
    }

    logOperation("LIST_TRASH", "", true);
    ok = true;
    return result.str();
#endif
}

bool Sandbox::emptyTrash()
{
    std::string trashDir = rootPath_ + "/.trash";
#ifdef _WIN32
    std::string nativeTrashDir = trashDir;
    std::replace(nativeTrashDir.begin(), nativeTrashDir.end(), '/', '\\');

    std::string searchPath = nativeTrashDir + "\\*";

    WIN32_FIND_DATAA findData;
    HANDLE findHandle = FindFirstFileA(searchPath.c_str(), &findData);

    if (findHandle == INVALID_HANDLE_VALUE) {
        logOperation("EMPTY_TRASH", "", true);
        return true;
    }

    bool success = true;
    do {
        std::string filename = findData.cFileName;
        if (filename == "." || filename == "..") {
            continue;
        }

        std::string fullPath = trashDir + "/" + filename;
        std::string nativePath = fullPath;
        std::replace(nativePath.begin(), nativePath.end(), '/', '\\');

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            RemoveDirectoryA(nativePath.c_str());
        } else {
            if (!DeleteFileA(nativePath.c_str())) {
                success = false;
            }
        }
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);

    logOperation("EMPTY_TRASH", "", success);
    return success;
#else
    DIR* dp = opendir(trashDir.c_str());
    if (!dp) {
        logOperation("EMPTY_TRASH", "", true);
        return true;
    }

    bool success = true;
    struct dirent* ep;
    while ((ep = readdir(dp))) {
        std::string filename = ep->d_name;
        if (filename == "." || filename == "..") {
            continue;
        }

        std::string fullPath = trashDir + "/" + filename;
        struct stat st;
        if (stat(fullPath.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (rmdir(fullPath.c_str()) != 0) {
                    success = false;
                }
            } else {
                if (unlink(fullPath.c_str()) != 0) {
                    success = false;
                }
            }
        }
    }
    closedir(dp);

    logOperation("EMPTY_TRASH", "", success);
    return success;
#endif
}

void Sandbox::logOperation(const std::string& op, const std::string& details, bool success) const
{
    std::ofstream logFile("sandbox_operations.log", std::ios::app);
    if (!logFile) return;

    time_t now = time(nullptr);
    char timeStr[64];
    struct tm* timeInfo = localtime(&now);
    if (timeInfo) {
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeInfo);
        logFile << "[" << timeStr << "] [" << op << "] " << details 
                << " - " << (success ? "SUCCESS" : "FAILED/BLOCKED") << "\n";
    }
}

} // namespace fox
