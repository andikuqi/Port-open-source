#pragma once

#include <string>
#include <string_view>

namespace port::kernel {

enum class CommandType : int {
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
    FsTrash,
    FsEmptyTrash,
    PlanProceed,
    PlanAbort,
    NetDownload,
    SysExecute,
    Unknown
};

[[nodiscard]] constexpr std::string_view commandTypeName(CommandType t) noexcept {
    switch (t) {
    case CommandType::Help:           return "Help";
    case CommandType::Status:         return "Status";
    case CommandType::FoundationInstall: return "FoundationInstall";
    case CommandType::Prompt:         return "Prompt";
    case CommandType::Exit:           return "Exit";
    case CommandType::FsList:         return "FsList";
    case CommandType::FsRead:         return "FsRead";
    case CommandType::FsWrite:        return "FsWrite";
    case CommandType::FsDelete:       return "FsDelete";
    case CommandType::FsMakeDir:      return "FsMakeDir";
    case CommandType::FsRename:       return "FsRename";
    case CommandType::FsTrash:        return "FsTrash";
    case CommandType::FsEmptyTrash:   return "FsEmptyTrash";
    case CommandType::PlanProceed:    return "PlanProceed";
    case CommandType::PlanAbort:      return "PlanAbort";
    case CommandType::NetDownload:    return "NetDownload";
    case CommandType::SysExecute:     return "SysExecute";
    case CommandType::Unknown:        return "Unknown";
    }
    return "?";
}

// Whether the command is destructive and needs user confirmation
[[nodiscard]] constexpr bool isDestructive(CommandType t) noexcept {
    return t == CommandType::FsWrite
        || t == CommandType::FsDelete
        || t == CommandType::FsEmptyTrash
        || t == CommandType::NetDownload
        || t == CommandType::SysExecute;
}

struct Command {
    CommandType type{CommandType::Unknown};
    std::string rawText;
    std::string arg1;
    std::string arg2;

    Command() = default;
    Command(CommandType t, std::string raw, std::string a1 = {}, std::string a2 = {})
        : type{t}, rawText{std::move(raw)}, arg1{std::move(a1)}, arg2{std::move(a2)} {}
};

} // namespace port::kernel
