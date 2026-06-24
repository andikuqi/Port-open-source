#pragma once

#include <string>

namespace fox {

class Sandbox {
public:
    explicit Sandbox(const std::string& rootPath);

    bool createSandboxDir() const;
    bool isSafePath(const std::string& path, std::string& outFullPath) const;

    std::string list(const std::string& dir, bool& ok);
    std::string read(const std::string& filename, bool& ok);
    bool write(const std::string& filename, const std::string& content);
    bool deleteFile(const std::string& filename);
    bool makeDir(const std::string& dirPath);
    bool renameEntry(const std::string& oldPath, const std::string& newPath);
    bool moveToTrash(const std::string& filename);
    std::string listTrash(bool& ok);
    bool emptyTrash();

    void logOperation(const std::string& op, const std::string& details, bool success) const;

private:
    std::string rootPath_;
};

} // namespace fox