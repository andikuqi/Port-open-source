#define NOMINMAX
#include "fox/ai_client.hpp"

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#endif

#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace fox {

AIClient::AIClient() {}

std::string AIClient::requestPlan(const std::string& prompt)
{
    std::string apiKey;

    // 1. Try reading from gemini_key.txt
    {
        std::ifstream keyFile("gemini_key.txt");
        if (keyFile.is_open()) {
            std::string key;
            if (std::getline(keyFile, key)) {
                apiKey = trim(key);
            }
        }
    }

    // 2. If empty, try reading from .env
    if (apiKey.empty()) {
        std::ifstream envFile(".env");
        if (envFile.is_open()) {
            std::string line;
            while (std::getline(envFile, line)) {
                std::size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string varName = trim(line.substr(0, eq));
                    if (varName == "GEMINI_API_KEY") {
                        apiKey = trim(line.substr(eq + 1));
                        break;
                    }
                }
            }
        }
    }

    // 3. If empty, try environment variable
    if (apiKey.empty()) {
        char* apiKeyEnv = std::getenv("GEMINI_API_KEY");
        if (apiKeyEnv && apiKeyEnv[0] != '\0') {
            apiKey = trim(std::string(apiKeyEnv));
        }
    }

    if (!apiKey.empty()) {
        std::string onlinePlan = callGeminiAPI(apiKey, prompt);
        if (!onlinePlan.empty() && onlinePlan.rfind("ERROR:", 0) != 0) {
            return onlinePlan;
        }
    }
    return getOfflinePlan(prompt);
}

#ifdef _WIN32
std::string AIClient::callGeminiAPI(const std::string& apiKey, const std::string& prompt)
{
    HINTERNET hInternet = InternetOpenA("PortOS/1.0", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
    if (!hInternet) {
        return "ERROR: InternetOpen failed.";
    }

    HINTERNET hConnect = InternetConnectA(hInternet, "generativelanguage.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect) {
        InternetCloseHandle(hInternet);
        return "ERROR: InternetConnect failed.";
    }

    std::string path = "/v1beta/models/gemini-2.5-flash:generateContent?key=" + apiKey;
    HINTERNET hRequest = HttpOpenRequestA(hConnect, "POST", path.c_str(), nullptr, nullptr, nullptr, INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0);
    if (!hRequest) {
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return "ERROR: HttpOpenRequest failed.";
    }

    std::string headers = "Content-Type: application/json\r\n";

    std::string escapedPrompt;
    for (char ch : prompt) {
        if (ch == '"') escapedPrompt += "\\\"";
        else if (ch == '\\') escapedPrompt += "\\\\";
        else if (ch == '\n') escapedPrompt += "\\n";
        else if (ch == '\r') escapedPrompt += "\\r";
        else if (ch == '\t') escapedPrompt += "\\t";
        else escapedPrompt += ch;
    }

    std::string systemInstruction = 
        "You are Super-Nova 1.0, the AI engine of Port OS — an AI-native operating environment. "
        "The user speaks in English or Albanian. Understand their intent naturally. "
        "Output a task plan as a list of commands, one per line. No explanations, no markdown. "
        "Available commands: "
        "fs list [path] "
        "fs read <file> "
        "fs write <file> <content> "
        "fs delete <file> "
        "fs mkdir <dir> "
        "fs rename <old> <new> "
        "net download <url> <dest> "
        "sys execute <path> [args] "
        "For downloads: always use net download to fetch the installer/package, then sys execute to run it. "
        "For Chrome on Windows: net download https://dl.google.com/chrome/install/chrome_installer.exe sandbox/Apps/chrome_installer.exe then sys execute sandbox/Apps/chrome_installer.exe "
        "For Chrome on Linux: net download https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb sandbox/Apps/google-chrome.deb then sys execute sudo dpkg -i sandbox/Apps/google-chrome.deb "
        "For VLC on Windows: net download https://get.videolan.org/vlc/last/win64/vlc-latest-win64.exe sandbox/Apps/vlc_installer.exe then sys execute sandbox/Apps/vlc_installer.exe "
        "For Firefox on Windows: net download https://download.mozilla.org/?product=firefox-latest&os=win64&lang=en-US sandbox/Apps/firefox_installer.exe then sys execute sandbox/Apps/firefox_installer.exe "
        "For 7-Zip on Windows: net download https://www.7-zip.org/a/7z2407-x64.exe sandbox/Apps/7zip_installer.exe then sys execute sandbox/Apps/7zip_installer.exe "
        "If the request is unclear or cannot be done with these commands, output: unknown";

    std::string requestBody = 
        "{\n"
        "  \"systemInstruction\": {\n"
        "    \"parts\": [{\"text\": \"" + systemInstruction + "\"}]\n"
        "  },\n"
        "  \"contents\": [\n"
        "    {\n"
        "      \"parts\": [{\"text\": \"" + escapedPrompt + "\"}]\n"
        "    }\n"
        "  ]\n"
        "}";

    BOOL sent = HttpSendRequestA(hRequest, headers.c_str(), static_cast<DWORD>(headers.length()), 
                                 const_cast<char*>(requestBody.c_str()), static_cast<DWORD>(requestBody.length()));
    if (!sent) {
        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return "ERROR: HttpSendRequest failed.";
    }

    std::string responseData;
    char buffer[4096];
    DWORD bytesRead = 0;
    while (InternetReadFile(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        responseData.append(buffer, bytesRead);
    }

    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);

    std::size_t textPos = responseData.find("\"text\": \"");
    if (textPos == std::string::npos) {
        return "ERROR: Invalid JSON response or empty plan.";
    }

    textPos += 9;
    std::size_t endPos = responseData.find("\"", textPos);
    if (endPos == std::string::npos) {
        return "ERROR: Invalid JSON structure.";
    }

    std::string plan = responseData.substr(textPos, endPos - textPos);

    std::string decodedPlan;
    for (std::size_t i = 0; i < plan.length(); ++i) {
        if (plan[i] == '\\' && i + 1 < plan.length()) {
            char next = plan[i + 1];
            if (next == 'n') decodedPlan += '\n';
            else if (next == 'r') decodedPlan += '\r';
            else if (next == 't') decodedPlan += '\t';
            else if (next == '"') decodedPlan += '"';
            else if (next == '\\') decodedPlan += '\\';
            else decodedPlan += next;
            ++i;
        } else {
            decodedPlan += plan[i];
        }
    }

    return trim(decodedPlan);
}
#else
std::string AIClient::callGeminiAPI(const std::string& apiKey, const std::string& prompt)
{
    (void)apiKey;
    (void)prompt;
    return "ERROR: Online API not implemented on this platform.";
}
#endif

std::string AIClient::getOfflinePlan(const std::string& prompt)
{
    std::string p = prompt;
    std::transform(p.begin(), p.end(), p.begin(), ::tolower);

    struct AppEntry {
        const char* keyword;
        const char* urlWin;
        const char* destWin;
        const char* urlLinux;
        const char* destLinux;
        const char* linuxInstallCmd;
    };
    static const AppEntry apps[] = {
        { "chrome",
          "https://dl.google.com/chrome/install/chrome_installer.exe",
          "sandbox/Apps/chrome_installer.exe",
          "https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb",
          "sandbox/Apps/google-chrome.deb",
          "dpkg -i sandbox/Apps/google-chrome.deb" },
        { "firefox",
          "https://download.mozilla.org/?product=firefox-latest&os=win64&lang=en-US",
          "sandbox/Apps/firefox_installer.exe",
          "https://download.mozilla.org/?product=firefox-latest&os=linux64&lang=en-US",
          "sandbox/Apps/firefox.tar.bz2",
          "tar -xjf sandbox/Apps/firefox.tar.bz2 -C sandbox/Apps/" },
        { "vlc",
          "https://get.videolan.org/vlc/last/win64/vlc-latest-win64.exe",
          "sandbox/Apps/vlc_installer.exe",
          "https://get.videolan.org/vlc/last/win64/vlc-latest-win64.exe",
          "sandbox/Apps/vlc_installer.exe",
          "" },
        { "7-zip",
          "https://www.7-zip.org/a/7z2407-x64.exe",
          "sandbox/Apps/7zip_installer.exe",
          "https://www.7-zip.org/a/7z2407-x64.exe",
          "sandbox/Apps/7zip_installer.exe",
          "" },
        { "7zip",
          "https://www.7-zip.org/a/7z2407-x64.exe",
          "sandbox/Apps/7zip_installer.exe",
          "https://www.7-zip.org/a/7z2407-x64.exe",
          "sandbox/Apps/7zip_installer.exe",
          "" },
        { "notepad++",
          "https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v8.6.8/npp.8.6.8.Installer.x64.exe",
          "sandbox/Apps/notepadpp_installer.exe",
          "https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v8.6.8/npp.8.6.8.Installer.x64.exe",
          "sandbox/Apps/notepadpp_installer.exe",
          "" },
        { "vscode",
          "https://code.visualstudio.com/sha/download?build=stable&os=win32-x64-user",
          "sandbox/Apps/vscode_installer.exe",
          "https://code.visualstudio.com/sha/download?build=stable&os=linux-deb-x64",
          "sandbox/Apps/vscode.deb",
          "dpkg -i sandbox/Apps/vscode.deb" },
        { "visual studio code",
          "https://code.visualstudio.com/sha/download?build=stable&os=win32-x64-user",
          "sandbox/Apps/vscode_installer.exe",
          "https://code.visualstudio.com/sha/download?build=stable&os=linux-deb-x64",
          "sandbox/Apps/vscode.deb",
          "dpkg -i sandbox/Apps/vscode.deb" },
        { "winrar",
          "https://www.rarlab.com/rar/winrar-x64-701.exe",
          "sandbox/Apps/winrar_installer.exe",
          "https://www.rarlab.com/rar/winrar-x64-701.exe",
          "sandbox/Apps/winrar_installer.exe",
          "" },
        { "zoom",
          "https://zoom.us/client/latest/ZoomInstallerFull.exe",
          "sandbox/Apps/zoom_installer.exe",
          "https://zoom.us/client/latest/zoom_amd64.deb",
          "sandbox/Apps/zoom.deb",
          "dpkg -i sandbox/Apps/zoom.deb" },
        { "discord",
          "https://discord.com/api/downloads/distributions/app/installers/latest?channel=stable&platform=win&arch=x86",
          "sandbox/Apps/discord_installer.exe",
          "https://discord.com/api/downloads/distributions/app/installers/latest?channel=stable&platform=linux&arch=x86_64",
          "sandbox/Apps/discord.deb",
          "dpkg -i sandbox/Apps/discord.deb" },
        { "obs",
          "https://github.com/obsproject/obs-studio/releases/download/30.1.2/OBS-Studio-30.1.2-Windows.exe",
          "sandbox/Apps/obs_installer.exe",
          "https://github.com/obsproject/obs-studio/releases/download/30.1.2/OBS-Studio-30.1.2-Windows.exe",
          "sandbox/Apps/obs_installer.exe",
          "" },
        { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }
    };

    bool isDownload = p.find("download") != std::string::npos ||
                      p.find("install") != std::string::npos ||
                      p.find("shkarko") != std::string::npos ||
                      p.find("instalo") != std::string::npos;

    if (isDownload) {
        for (int i = 0; apps[i].keyword != nullptr; ++i) {
            if (p.find(apps[i].keyword) != std::string::npos) {
#ifdef _WIN32
                std::string plan = "fs mkdir Apps\n";
                plan += "net download ";
                plan += apps[i].urlWin;
                plan += " ";
                plan += apps[i].destWin;
                plan += "\nsys execute ";
                plan += apps[i].destWin;
                return plan;
#else
                std::string plan = "fs mkdir Apps\n";
                plan += "net download ";
                plan += apps[i].urlLinux;
                plan += " ";
                plan += apps[i].destLinux;
                if (apps[i].linuxInstallCmd && apps[i].linuxInstallCmd[0] != '\0') {
                    plan += "\nsys execute sudo ";
                    plan += apps[i].linuxInstallCmd;
                }
                return plan;
#endif
            }
        }
    }

    bool isWrite = p.find("krijo") != std::string::npos ||
                   p.find("shkruaj") != std::string::npos ||
                   p.find("create") != std::string::npos ||
                   p.find("write") != std::string::npos;

    bool isDelete = p.find("fshij") != std::string::npos ||
                    p.find("delete") != std::string::npos ||
                    p.find("remove") != std::string::npos ||
                    p.find("rm ") != std::string::npos;

    bool isList = p.find("list") != std::string::npos ||
                  p.find("ls") != std::string::npos ||
                  p.find("trego") != std::string::npos ||
                  p.find("shfaq") != std::string::npos;

    bool isRead = p.find("lexo") != std::string::npos ||
                  p.find("read") != std::string::npos ||
                  p.find("cat ") != std::string::npos;

    bool isMkdir = (p.find("folder") != std::string::npos ||
                    p.find("directory") != std::string::npos ||
                    p.find("dosje") != std::string::npos) &&
                   (p.find("new") != std::string::npos ||
                    p.find("make") != std::string::npos ||
                    p.find("krijo") != std::string::npos ||
                    isWrite);

    if (isMkdir) {
        return "fs mkdir sandbox/Documents/NewFolder\nfs list";
    }

    if (isWrite) {
        std::size_t txtPos = p.find(".txt");
        std::string filename = "file.txt";
        if (txtPos != std::string::npos) {
            std::size_t start = txtPos;
            while (start > 0 && !std::isspace(static_cast<unsigned char>(p[start - 1]))) --start;
            filename = prompt.substr(start, txtPos + 4 - start);
        }
        std::string content = "Hello from Port OS";
        std::size_t quoteStart = prompt.find('"');
        if (quoteStart != std::string::npos) {
            std::size_t quoteEnd = prompt.find('"', quoteStart + 1);
            if (quoteEnd != std::string::npos)
                content = prompt.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        }
        return "fs write " + filename + " \"" + content + "\"\nfs list";
    }

    if (isDelete) {
        std::size_t txtPos = p.find(".txt");
        std::string filename = "file.txt";
        if (txtPos != std::string::npos) {
            std::size_t start = txtPos;
            while (start > 0 && !std::isspace(static_cast<unsigned char>(p[start - 1]))) --start;
            filename = prompt.substr(start, txtPos + 4 - start);
        }
        return "fs delete " + filename + "\nfs list";
    }

    if (isRead) {
        std::size_t txtPos = p.find(".txt");
        std::string filename = "file.txt";
        if (txtPos != std::string::npos) {
            std::size_t start = txtPos;
            while (start > 0 && !std::isspace(static_cast<unsigned char>(p[start - 1]))) --start;
            filename = prompt.substr(start, txtPos + 4 - start);
        }
        return "fs read " + filename;
    }

    if (isList) {
        return "fs list";
    }

    return "fs list";
}

std::string AIClient::trim(std::string value)
{
    const auto notSpace = [](unsigned char ch) {
        return !std::isspace(ch);
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

} // namespace fox
