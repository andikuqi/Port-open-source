#pragma once

#include <string>

namespace fox {

enum class CommandType {
    Help,
    Status,
    FoundationInstall,
    Prompt,
    Exit,
    FsList,
    FsRead,
    FsWrite,
    FsDelete,
    FsMakeDir,
    FsRename,
    PlanProceed,
    PlanAbort,
    NetDownload,
    SysExecute,
    Unknown
};

struct Command {
    CommandType type;
    std::string rawText;
    std::string arg1;
    std::string arg2;

    Command() : type(CommandType::Unknown) {}
    Command(CommandType t, std::string r, std::string a1 = "", std::string a2 = "")
        : type(t), rawText(std::move(r)), arg1(std::move(a1)), arg2(std::move(a2)) {}
};

} // namespace fox