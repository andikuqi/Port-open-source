#include "fox/command_router.hpp"

#include <algorithm>
#include <cctype>

namespace fox {

Command CommandRouter::parse(const std::string& input) const
{
    const std::string text = trim(input);

    if (text.empty()) {
        return {CommandType::Unknown, input};
    }

    if (text == "help") {
        return {CommandType::Help, text};
    }

    if (text == "status") {
        return {CommandType::Status, text};
    }

    if (text == "/port ans-install-fd" || text == "/fox ans-install-fd") {
        return {CommandType::FoundationInstall, text};
    }

    if (text.rfind("fs ", 0) == 0 || text == "fs") {
        std::string fsCmd = text.substr(2);
        fsCmd = trim(fsCmd);

        if (fsCmd.empty() || fsCmd == "list" || fsCmd == "ls") {
            return {CommandType::FsList, text, "", ""};
        }

        if (fsCmd.rfind("list ", 0) == 0 || fsCmd.rfind("ls ", 0) == 0) {
            std::string path = fsCmd.rfind("list ", 0) == 0 ? fsCmd.substr(5) : fsCmd.substr(3);
            return {CommandType::FsList, text, trim(path), ""};
        }

        if (fsCmd.rfind("read ", 0) == 0 || fsCmd.rfind("cat ", 0) == 0) {
            std::string path = fsCmd.rfind("read ", 0) == 0 ? fsCmd.substr(5) : fsCmd.substr(4);
            return {CommandType::FsRead, text, trim(path), ""};
        }

        if (fsCmd.rfind("write ", 0) == 0) {
            std::string rest = trim(fsCmd.substr(6));
            std::size_t spacePos = rest.find(' ');
            if (spacePos != std::string::npos) {
                std::string filename = rest.substr(0, spacePos);
                std::string content = rest.substr(spacePos + 1);
                if (content.size() >= 2 && content.front() == '"' && content.back() == '"') {
                    content = content.substr(1, content.size() - 2);
                }
                return {CommandType::FsWrite, text, trim(filename), content};
            } else {
                return {CommandType::FsWrite, text, rest, ""};
            }
        }

        if (fsCmd.rfind("delete ", 0) == 0 || fsCmd.rfind("rm ", 0) == 0) {
            std::string path = fsCmd.rfind("delete ", 0) == 0 ? fsCmd.substr(7) : fsCmd.substr(3);
            return {CommandType::FsDelete, text, trim(path), ""};
        }

        if (fsCmd.rfind("mkdir ", 0) == 0) {
            std::string path = trim(fsCmd.substr(6));
            return {CommandType::FsMakeDir, text, path, ""};
        }

        if (fsCmd.rfind("rename ", 0) == 0) {
            std::string rest = trim(fsCmd.substr(7));
            std::size_t spacePos = rest.find(' ');
            if (spacePos != std::string::npos) {
                std::string oldName = rest.substr(0, spacePos);
                std::string newName = trim(rest.substr(spacePos + 1));
                return {CommandType::FsRename, text, oldName, newName};
            }
            return {CommandType::FsRename, text, rest, ""};
        }
    }

    if (text == "plan proceed" || text == "plan yes" || text == "plan y") {
        return {CommandType::PlanProceed, text};
    }

    if (text == "plan abort" || text == "plan no" || text == "plan n") {
        return {CommandType::PlanAbort, text};
    }

    if (text.rfind("net download ", 0) == 0) {
        std::string rest = trim(text.substr(13));
        std::size_t spacePos = rest.find(' ');
        if (spacePos != std::string::npos) {
            std::string url = rest.substr(0, spacePos);
            std::string dest = trim(rest.substr(spacePos + 1));
            return {CommandType::NetDownload, text, url, dest};
        } else {
            return {CommandType::NetDownload, text, rest, ""};
        }
    }

    if (text.rfind("sys execute ", 0) == 0) {
        std::string rest = trim(text.substr(12));
        std::size_t spacePos = rest.find(' ');
        if (spacePos != std::string::npos) {
            std::string path = rest.substr(0, spacePos);
            std::string args = trim(rest.substr(spacePos + 1));
            return {CommandType::SysExecute, text, path, args};
        } else {
            return {CommandType::SysExecute, text, rest, ""};
        }
    }

    if (text == "exit" || text == "quit") {
        return {CommandType::Exit, text};
    }

    constexpr auto askPrefix = "ask ";
    if (text.rfind(askPrefix, 0) == 0) {
        return {CommandType::Prompt, trim(text.substr(4))};
    }

    return {CommandType::Prompt, text};
}

std::string CommandRouter::trim(std::string value)
{
    const auto notSpace = [](unsigned char ch) {
        return !std::isspace(ch);
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

} // namespace fox
