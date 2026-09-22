#include "port/kernel/command_router.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace port::kernel {

namespace {

std::string_view trimSV(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.remove_suffix(1);
    return s;
}

// Split "rest" into (arg1, arg2) at first unquoted space
std::pair<std::string, std::string> splitArgs(std::string_view rest) {
    std::string a1, a2;
    auto r = trimSV(rest);
    if (r.empty()) return {a1, a2};

    // arg1 may be quoted
    if (!r.empty() && r.front() == '"') {
        r.remove_prefix(1);
        auto end = r.find('"');
        if (end != std::string_view::npos) {
            a1 = std::string{r.substr(0, end)};
            r.remove_prefix(end + 1);
        } else {
            a1 = std::string{r};
            return {a1, a2};
        }
    } else {
        auto sp = r.find(' ');
        if (sp == std::string_view::npos) {
            a1 = std::string{r};
            return {a1, a2};
        }
        a1 = std::string{r.substr(0, sp)};
        r.remove_prefix(sp);
    }

    // arg2 is everything after the space (trim leading whitespace; strip surrounding quotes)
    r = trimSV(r);
    if (!r.empty() && r.front() == '"' && r.back() == '"') {
        r.remove_prefix(1);
        r.remove_suffix(1);
    }
    a2 = std::string{r};
    return {a1, a2};
}

} // namespace

std::string_view CommandRouter::trim(std::string_view s) noexcept {
    return trimSV(s);
}

Command CommandRouter::parse(std::string_view input) const {
    const auto text = std::string{trim(input)};
    if (text.empty()) return {CommandType::Unknown, text};

    // ---- Exact keywords ----
    if (text == "help")               return {CommandType::Help,   text};
    if (text == "status")             return {CommandType::Status, text};
    if (text == "exit" || text == "quit") return {CommandType::Exit, text};
    if (text == "plan proceed" || text == "plan yes" || text == "plan y")
        return {CommandType::PlanProceed, text};
    if (text == "plan abort" || text == "plan no" || text == "plan n")
        return {CommandType::PlanAbort, text};
    if (text == "/port ans-install-fd" || text == "/fox ans-install-fd")
        return {CommandType::FoundationInstall, text};
    if (text == "fs trash" || text == "fs list-trash")
        return {CommandType::FsTrash, text};
    if (text == "fs empty-trash")
        return {CommandType::FsEmptyTrash, text};

    // ---- fs <subcommand> ----
    if (text.rfind("fs ", 0) == 0 || text == "fs") {
        std::string_view rest = trimSV(std::string_view{text}.substr(2));

        if (rest.empty() || rest == "list" || rest == "ls")
            return {CommandType::FsList, text, "", ""};

        if (rest.rfind("list ", 0) == 0)
            return {CommandType::FsList, text, std::string{trimSV(rest.substr(5))}, ""};
        if (rest.rfind("ls ", 0) == 0)
            return {CommandType::FsList, text, std::string{trimSV(rest.substr(3))}, ""};

        if (rest.rfind("read ", 0) == 0)
            return {CommandType::FsRead, text, std::string{trimSV(rest.substr(5))}, ""};
        if (rest.rfind("cat ", 0) == 0)
            return {CommandType::FsRead, text, std::string{trimSV(rest.substr(4))}, ""};

        if (rest.rfind("write ", 0) == 0) {
            auto [a1, a2] = splitArgs(rest.substr(6));
            return {CommandType::FsWrite, text, a1, a2};
        }

        if (rest.rfind("delete ", 0) == 0)
            return {CommandType::FsDelete, text, std::string{trimSV(rest.substr(7))}, ""};
        if (rest.rfind("rm ", 0) == 0)
            return {CommandType::FsDelete, text, std::string{trimSV(rest.substr(3))}, ""};

        if (rest.rfind("mkdir ", 0) == 0)
            return {CommandType::FsMakeDir, text, std::string{trimSV(rest.substr(6))}, ""};

        if (rest.rfind("rename ", 0) == 0) {
            auto [a1, a2] = splitArgs(rest.substr(7));
            return {CommandType::FsRename, text, a1, a2};
        }

        return {CommandType::Unknown, text};
    }

    // ---- net download <url> <dest> ----
    if (text.rfind("net download ", 0) == 0) {
        auto [url, dest] = splitArgs(std::string_view{text}.substr(13));
        return {CommandType::NetDownload, text, url, dest};
    }

    // ---- sys execute <path> [args] ----
    if (text.rfind("sys execute ", 0) == 0) {
        auto [path, args] = splitArgs(std::string_view{text}.substr(12));
        return {CommandType::SysExecute, text, path, args};
    }

    // ---- ask <prompt> (explicit prefix) ----
    if (text.rfind("ask ", 0) == 0) {
        return {CommandType::Prompt, std::string{trim(std::string_view{text}.substr(4))}};
    }

    // ---- Natural language → Prompt ----
    return {CommandType::Prompt, text};
}

} // namespace port::kernel
