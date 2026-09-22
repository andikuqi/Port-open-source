#include "port/kernel/ai_kernel.hpp"
#include "port/kernel/command_router.hpp"
#include "port/ai/context_engine.hpp"
#include "port/ai/provider.hpp"
#include "port/kernel/runtime_ai_adapter.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <wininet.h>
#include <gdiplus.h>
#endif

#include <sstream>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <utility>

#ifdef _WIN32
namespace {

constexpr const char* PortAISystemInstruction =
    "You are the Port OS command planner. Return only a newline-separated plan "
    "using these commands: fs list [path], fs read <path>, fs write <path> <text>, "
    "fs mkdir <path>, fs rename <old> <new>, fs delete <path>, fs trash, "
    "fs empty-trash, net download <https-url> <sandbox-relative-destination>, "
    "or sys execute <sandbox-relative-program> [args]. Never use absolute paths, "
    "shell syntax, code fences, explanations, or commands outside this list. "
    "All file destinations must remain inside the Port sandbox.";

std::string trimCredential(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

// Reads an API key from an env var, falling back to a local key file.
// The file may contain "NAME=value" lines or the bare key; # is a comment.
std::string loadApiKey(const char* envName, const char* fileName) {
    if (const char* envKey = std::getenv(envName)) {
        auto key = trimCredential(envKey);
        if (!key.empty()) return key;
    }

    std::ifstream file{fileName};
    std::string line;
    const std::string prefix = std::string(envName) + "=";
    while (std::getline(file, line)) {
        line = trimCredential(line);
        if (line.empty() || line.front() == '#') continue;
        if (line.rfind(prefix, 0) == 0) line = line.substr(prefix.size());
        return trimCredential(line);
    }
    return {};
}

// The failover chain, kept global so drawTaskbar can show which engine
// actually served the last response.
std::shared_ptr<port::ai::ProviderManager> gAIManager;

bool configureAIProvider(port::kernel::AIKernel& kernel) {
    auto manager = std::make_shared<port::ai::ProviderManager>();

    // 1. OpenRouter — one key, hundreds of models
    if (const std::string key = loadApiKey("OPENROUTER_API_KEY", "openrouter_key.txt");
        !key.empty()) {
        port::ai::OpenAICompatConfig cfg;
        cfg.providerName = "OpenRouter";
        cfg.host         = "openrouter.ai";
        cfg.path         = "/api/v1/chat/completions";
        cfg.defaultModel = "openai/gpt-4o-mini";
        auto p = port::ai::makeOpenAICompatProvider(std::move(cfg));
        p->setCredential("api_key", key);
        if (const char* m = std::getenv("PORT_OPENROUTER_MODEL"))
            p->setCredential("model", trimCredential(m));
        manager->addProvider(std::move(p));
    }

    // 2. Groq — extremely fast inference
    if (const std::string key = loadApiKey("GROQ_API_KEY", "groq_key.txt");
        !key.empty()) {
        port::ai::OpenAICompatConfig cfg;
        cfg.providerName = "Groq";
        cfg.host         = "api.groq.com";
        cfg.path         = "/openai/v1/chat/completions";
        cfg.defaultModel = "llama-3.3-70b-versatile";
        auto p = port::ai::makeOpenAICompatProvider(std::move(cfg));
        p->setCredential("api_key", key);
        if (const char* m = std::getenv("PORT_GROQ_MODEL"))
            p->setCredential("model", trimCredential(m));
        manager->addProvider(std::move(p));
    }

    // 3. Gemini — existing provider
    if (const std::string key = loadApiKey("GEMINI_API_KEY", "gemini_key.txt");
        !key.empty()) {
        auto p = port::ai::makeGeminiProvider();
        p->setCredential("api_key", key);
        if (const char* m = std::getenv("PORT_AI_MODEL"))
            p->setCredential("model", trimCredential(m));
        manager->addProvider(std::move(p));
    }

    // 4. Ollama — local daemon, no key, works with no internet at all.
    //    Always in the chain: if it is not running the call fails fast
    //    and the kernel reports the failure honestly.
    {
        auto p = port::ai::makeOllamaProvider();
        if (const char* m = std::getenv("PORT_OLLAMA_MODEL"))
            p->setCredential("model", trimCredential(m));
        manager->addProvider(std::move(p));
    }

    gAIManager = manager;

    port::ai::ContextEngine::Config contextConfig;
    contextConfig.systemInstruction = PortAISystemInstruction;
    auto context = std::make_shared<port::ai::ContextEngine>(std::move(contextConfig));

    port::ai::PromptRuntime::Config runtimeConfig;
    runtimeConfig.systemInstruction = PortAISystemInstruction;
    // The kernel owns the fallback so it can tell the user when online AI
    // failed instead of silently pretending the provider produced the plan.
    runtimeConfig.offlineFallback = false;
    kernel.setAIAdapter(std::make_shared<port::kernel::RuntimeAIAdapter>(
        manager, std::move(context), std::move(runtimeConfig)));
    return manager->isAvailable();
}

Gdiplus::Image* desktopWallpaper = nullptr;
Gdiplus::Image* taskbarOrbImg = nullptr;
Gdiplus::Image* dockLogoImg = nullptr;   // transparent orb logo in the dock

// Real runtime metrics shown on the agent dashboard.
std::atomic<int> gLastLatencyMs{0};      // last provider round-trip, ms

struct SystemMemory { double usedGB; double totalGB; int percent; };
SystemMemory readSystemMemory()
{
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms) || ms.ullTotalPhys == 0) {
        return {0.0, 0.0, 0};
    }
    const double total = static_cast<double>(ms.ullTotalPhys) /
                         (1024.0 * 1024.0 * 1024.0);
    const double avail = static_cast<double>(ms.ullAvailPhys) /
                         (1024.0 * 1024.0 * 1024.0);
    const double used = total - avail;
    return {used, total, static_cast<int>((used / total) * 100.0 + 0.5)};
}

// "just now" / "3m ago" / "2h ago" / "5d ago"
std::string relativeTime(std::chrono::system_clock::time_point when)
{
    using namespace std::chrono;
    const auto secs = duration_cast<seconds>(
        system_clock::now() - when).count();
    if (secs < 45) return "just now";
    if (secs < 3600) return std::to_string(secs / 60) + "m ago";
    if (secs < 86400) return std::to_string(secs / 3600) + "h ago";
    return std::to_string(secs / 86400) + "d ago";
}
Gdiplus::Image* myPortIconImg = nullptr;
Gdiplus::Image* trashEmptyIconImg = nullptr;
Gdiplus::Image* trashFullIconImg = nullptr;
Gdiplus::Image* serverIconImg = nullptr;
Gdiplus::Image* folderIconImg = nullptr;
HBITMAP cachedBackground = nullptr;
void updateCachedBackground(HWND window, int width, int height);

constexpr int IdIconMyServer   = 1001;
constexpr int IdIconMyComputer = 1002;
constexpr int IdIconTrashBin   = 1003;
constexpr int IdTerminalOutput = 2001;
constexpr int IdTerminalInput = 2002;
constexpr int IdRunCommand = 2003;
constexpr int IdMyPortList      = 3001;
constexpr int IdMyPortRefresh   = 3002;
constexpr int IdTrashList       = 3003;
constexpr int IdTrashEmpty      = 3004;
constexpr int IdAIProgressLabel = 3005;
constexpr int IdAnimTimer       = 9001;
constexpr int IdFolderDragTimer = 9002;
constexpr UINT PortMessageAIComplete = WM_APP + 41;
// Posted to the DESKTOP window when an agent-app request finishes (the
// desktop always outlives the agent window, so the busy flag can't leak)
constexpr UINT PortMessageAgentAIComplete = WM_APP + 42;
constexpr int IdAgentInput = 7001;

// Folder window context menu IDs
constexpr int IdFolderCtxOpen    = 5001;
constexpr int IdFolderCtxNewFile = 5002;
constexpr int IdFolderCtxNewDir  = 5003;
constexpr int IdFolderCtxRename  = 5004;
constexpr int IdFolderCtxDelete  = 5005;
constexpr int IdFolderCtxRefresh = 5006;
// View modes inside a folder window
constexpr int IdFolderViewList   = 5010;
constexpr int IdFolderViewSmall  = 5011;
constexpr int IdFolderViewMedium = 5012;
constexpr int IdFolderViewLarge  = 5013;

constexpr COLORREF DesktopBackground = RGB(13, 18, 32);
constexpr COLORREF DesktopOrb = RGB(18, 38, 50);
constexpr COLORREF TaskbarBackground = RGB(15, 20, 35);
constexpr COLORREF TaskbarBorder = RGB(35, 45, 70);
constexpr COLORREF PortBlue = RGB(76, 118, 238);
constexpr COLORREF PortBlueLight = RGB(104, 139, 247);
constexpr COLORREF PortGreen = RGB(69, 210, 126);
constexpr COLORREF PortRed = RGB(239, 80, 72);
constexpr COLORREF PortText = RGB(220, 225, 238);
constexpr COLORREF PortMutedText = RGB(143, 153, 183);

// Bottom reserved area: dock bar flush to the bottom edge plus the
// floating search pill right above it.
constexpr int kDockH = 64;
constexpr int kSearchStripH = 56;   // 48px pill + 8px gap to the dock
constexpr int kTaskbarReserved = kDockH + kSearchStripH;

enum class IconSizeOption {
    Small,
    Medium,
    Large
};

struct IconLayoutInfo {
    int width;
    int height;
    int imageSize;
    int gapX;
    int gapY;
};

inline IconLayoutInfo getIconLayoutInfo(IconSizeOption sizeOpt) {
    switch (sizeOpt) {
    case IconSizeOption::Small:
        return {90, 100, 64, 104, 116};
    case IconSizeOption::Large:
        return {160, 170, 128, 184, 198};
    case IconSizeOption::Medium:
    default:
        return {122, 132, 96, 144, 158};
    }
}

struct ChatEntry {
    bool isUser;
    std::string text;
};

// Port agents — placeholder names for now, renamed later by the user.
struct AgentInfo {
    std::string name;
    std::string tagline;
    COLORREF accent;
    // Real per-agent system prompt used by the agent app windows
    std::string systemPrompt;
    // Extra specialist tool lines appended to the shared tool protocol
    std::string extraTools;
};

// Shared context appended to every agent's system prompt
constexpr const char* PortAgentSharedContext =
    "\n\nYou are running inside Port OS, an AI-native desktop environment. "
    "Be concise and practical; use plain text without markdown headings. "
    "Always answer in the same language the user writes in.";

// Tool protocol shared by all agents: read-only sandbox access. The model
// asks with a single "TOOL:" line, we execute and feed the result back.
constexpr const char* PortAgentToolProtocol =
    "\n\nTOOLS: You can read the user's Port OS sandbox files and the web. "
    "When the user's question requires such data, reply with exactly one "
    "line and nothing else:\n"
    "TOOL: fs list <folder>\n"
    "TOOL: fs read <file>\n"
    "TOOL: web search <query>\n"
    "TOOL: web fetch <url>\n"
    "(an empty <folder> lists the sandbox root; paths are relative, e.g. "
    "Desktop/notes.txt). You will then receive a TOOL RESULT message — "
    "use it to answer normally, citing source URLs for web results. Use "
    "tools only when needed; never invent file contents or web facts.";

struct DesktopState {
    port::kernel::AIKernel kernel;
    port::kernel::CommandRouter router;
    HWND output;
    HWND input;
    HWND aiStatusLabel;
    HWND chatPanel = nullptr;
    std::vector<ChatEntry> chatHistory;
    bool chatPanelVisible = false;
    // User-renameable labels for the system desktop icons
    std::string myServerLabel = "My Server";
    std::string trashLabel    = "Trash Bin";
    // Live drop-target feedback while dragging an icon:
    // blocked → red prohibition sign; hint → "Move to <target>" pill
    bool dragHoverBlocked = false;
    std::string dragHoverText;
    // Layered, semi-transparent drag image that follows the mouse while the
    // real icon window stays hidden — no backdrop compositing bugs possible.
    HWND dragGhost = nullptr;
    HFONT uiFont;
    HFONT labelFont;
    HFONT askFont;
    // Taskbar cached fonts (created once, reused every paint)
    HFONT tbOrbFont   = nullptr;
    HFONT tbMainFont  = nullptr;
    HFONT tbSnFont    = nullptr;
    HFONT tbClockFont = nullptr;
    HFONT tbDateFont  = nullptr;
    std::string terminalBuffer;
    HWND myPortWindow;
    HWND trashWindow;
    HWND myServerWindow;

    // Drag-select rubber band
    bool hasPrevSelRect = false;
    RECT prevSelRect = {};

    // Taskbar animation
    int  pulsePhase          = 0;
    bool isAIRunning         = false;
    std::string aiStatusMsg;
    int  statusClearCountdown = 0;

    // Icon size settings
    IconSizeOption iconSize;

    // Drag selection state
    bool isDragging;
    POINT dragStart;
    POINT dragEnd;

    // Desktop icon positions
    POINT myPortPos;
    POINT trashPos;
    POINT myServerPos;

    // Icon dragging state
    bool isDraggingIcon;
    bool iconDragMoved;
    ULONGLONG lastIconDragPaint;
    int draggedIconId;
    int selectedIconId;
    std::vector<int> dragSelectedIconIds;
    POINT iconDragOffset;
    POINT draggedIconOrigPos;
    // Multi-icon drag: when a rubber-band selection is dragged as a group,
    // every member moves rigidly by the same delta (like Windows).
    bool groupDrag = false;
    std::vector<std::pair<int, POINT>> groupOrig;

    // Dynamic desktop icons
    struct DynamicIcon {
        HWND hwnd;
        std::string name;
        bool isDir;
        POINT pos;
        int id;
    };
    std::vector<DynamicIcon> dynamicIcons;
    // Last known position of each Desktop entry, keyed by name, so a
    // rebuild (rename, new folder, delete) doesn't reset moved icons.
    std::map<std::string, POINT> dynamicIconSavedPos;

    // PORT AGENTS side panel
    std::vector<AgentInfo> agents;
    int activeAgent = 0;
    bool agentPanelVisible = true;
    HWND agentPanel = nullptr;   // the panel itself
    HWND agentHandle = nullptr;  // slim reopen arrow when panel is hidden
    int agentHotRow = -1;        // hovered row; -2 = close button
    int agentScroll = 0;         // vertical scroll offset of the agent list

    // "Agents" dock folder: popup grid with the agent medallions
    HWND agentGrid = nullptr;
    bool agentGridVisible = false;
    int agentGridHot = -1;
    int hotDockTile = -1;   // dock tile the mouse is hovering, or -1
    // Open agent app windows, keyed by agent index
    std::map<int, HWND> agentWindows;
    // Open folder browser windows, keyed by the desktop icon id that opened
    // them — lets the dock tile restore/minimize/preview the same window.
    std::map<int, HWND> folderWindows;

    // In-place icon rename (Windows-style edit box under the icon)
    HWND renameEdit = nullptr;
    int renameIconId = 0;
    // Wallpaper patch behind the edit, so its background is "transparent"
    HBRUSH renameBgBrush = nullptr;

    std::jthread aiWorker;
};

struct AsyncCommandResult {
    port::kernel::Command command;
    port::kernel::KernelResponse response;
};

void refreshDesktopDynamicIcons(HWND window, DesktopState* state,
                                bool fullRebuild = false);
std::string showInputDialog(HWND parent, const char* title, const char* prompt, const char* defaultVal);

// Port-themed popup menus (owner-drawn; defined after the GDI+ utilities)
struct PortMenuItem;
void themePortMenu(HMENU menu, std::vector<std::unique_ptr<PortMenuItem>>& store);
bool measurePortMenuItem(MEASUREITEMSTRUCT* mis);
bool drawPortMenuItem(const DRAWITEMSTRUCT* dis);

// GDI+ rounded-rect helper (defined with the other GDI+ utilities)
void addRoundRect(Gdiplus::GraphicsPath& path, int x, int y, int w, int h, int r);

// Port-themed properties popup — replaces the stock white MessageBox
// (defined after the GDI+ utilities)
struct PortPropRow { std::string label; std::string value; };
void showPortProperties(HWND parent, const std::string& title,
                        const std::vector<PortPropRow>& rows);
struct DirStats { std::uintmax_t bytes = 0; int files = 0; int folders = 0; };
DirStats computeDirStats(const std::filesystem::path& path);
std::string formatBytes(std::uintmax_t bytes);
std::string formatFileTime(const std::filesystem::path& path);

inline void snapIconsToGrid(DesktopState* state) {
    auto layoutInfo = getIconLayoutInfo(state->iconSize);

    int serverCol = (state->myServerPos.x - 28 + layoutInfo.gapX / 2) / layoutInfo.gapX;
    int serverRow = (state->myServerPos.y - 34 + layoutInfo.gapY / 2) / layoutInfo.gapY;
    if (serverCol < 0) serverCol = 0;
    if (serverRow < 0) serverRow = 0;

    int trashCol = (state->trashPos.x - 28 + layoutInfo.gapX / 2) / layoutInfo.gapX;
    int trashRow = (state->trashPos.y - 34 + layoutInfo.gapY / 2) / layoutInfo.gapY;
    if (trashCol < 0) trashCol = 0;
    if (trashRow < 0) trashRow = 0;
    if (trashCol == serverCol && trashRow == serverRow) {
        trashRow = serverRow + 1;
    }

    state->myServerPos.x = 28 + serverCol * layoutInfo.gapX;
    state->myServerPos.y = 34 + serverRow * layoutInfo.gapY;

    state->trashPos.x = 28 + trashCol * layoutInfo.gapX;
    state->trashPos.y = 34 + trashRow * layoutInfo.gapY;
}

struct DesktopState;
struct FolderWindowState {
    port::kernel::AIKernel* kernel;
    std::string currentDir;
    DesktopState* desktop = nullptr; // owner, to update the dock on destroy
    int dockIconId = -1;             // desktop icon id this window belongs to
    // Themed toolbar buttons, hit-tested in the window proc
    RECT tbUp{};
    RECT tbNewDir{};
    RECT tbRefresh{};
    int hotTb = -1;      // 0 Up, 1 New Folder, 2 Refresh
    // Dragging an item out of the list onto the desktop / Trash Bin
    bool maybeDrag = false;
    bool dragging = false;
    POINT dragStart{};
    POINT dragStartScreen{};
    std::string dragName;
    bool dragIsDir = false;
    HWND dragGhost = nullptr; // layered window following the cursor

    // View mode: 0 = List (listbox), 1 = Small, 2 = Medium, 3 = Large icons
    int viewMode = 2;
    // Items backing the icon grid (name, isDirectory); ".." first when nested
    std::vector<std::pair<std::string, bool>> items;
    int selected = -1;
    int hotItem = -1;
    int scrollY = 0;
    int contentH = 0;
};

constexpr int FolderHeaderH = 84; // header + breadcrumb strip height

// Icon-grid cell metrics per view mode
struct FolderCell { int w, h, icon; };
inline FolderCell folderCellFor(int viewMode) {
    switch (viewMode) {
    case 1:  return {78, 88, 32};
    case 3:  return {132, 146, 80};
    default: return {104, 116, 56};
    }
}

// Subclass a folder window's listbox so its items can be dragged out
void subclassFolderList(HWND listBox);

void refreshMyPortList(HWND listBox, port::security::Sandbox* sandbox, const std::string& currentDir)
{
    SendMessageA(listBox, LB_RESETCONTENT, 0, 0);

    if (!currentDir.empty()) {
        SendMessageA(listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(".."));
    }

    auto result = sandbox->list(currentDir);
    if (!result) return;
    for (const auto& entry : *result) {
        std::string displayName = entry.name + (entry.isDirectory ? "/" : "");
        SendMessageA(listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(displayName.c_str()));
    }
}

// Backing items for the icon-grid view (mirrors the listbox contents)
void refreshFolderItems(FolderWindowState* fs)
{
    if (!fs || !fs->kernel) return;
    fs->items.clear();
    fs->selected = -1;
    fs->hotItem = -1;
    if (!fs->currentDir.empty()) fs->items.push_back({"..", true});
    auto result = fs->kernel->getSandbox().list(fs->currentDir);
    if (!result) return;
    for (const auto& entry : *result) {
        fs->items.push_back({entry.name, entry.isDirectory});
    }
}

// Name of the currently selected item, for either view mode ("" if none)
std::string folderSelectedName(HWND folderWnd, FolderWindowState* fs,
                               bool* outIsDir = nullptr)
{
    if (!fs) return {};
    if (fs->viewMode == 0) {
        HWND lb = GetDlgItem(folderWnd, IdMyPortList);
        if (!lb) return {};
        const int idx = static_cast<int>(SendMessageA(lb, LB_GETCURSEL, 0, 0));
        if (idx == LB_ERR) return {};
        char buf[MAX_PATH] = {};
        SendMessageA(lb, LB_GETTEXT, idx, reinterpret_cast<LPARAM>(buf));
        std::string name(buf);
        const bool isDir = (!name.empty() && name.back() == '/');
        if (isDir) name.pop_back();
        if (outIsDir) *outIsDir = isDir || name == "..";
        return name;
    }
    if (fs->selected < 0 ||
        fs->selected >= static_cast<int>(fs->items.size())) {
        return {};
    }
    if (outIsDir) *outIsDir = fs->items[fs->selected].second;
    return fs->items[fs->selected].first;
}

void refreshTrashList(HWND listBox, port::security::Sandbox* sandbox)
{
    SendMessageA(listBox, LB_RESETCONTENT, 0, 0);
    auto result = sandbox->listTrash();
    if (!result || result->empty()) {
        // Item data 2 = placeholder row (no icon)
        int idx = static_cast<int>(SendMessageA(listBox, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>("Trash Bin is empty")));
        SendMessageA(listBox, LB_SETITEMDATA, idx, 2);
        return;
    }
    for (const auto& entry : *result) {
        // Item data: 1 = directory, 0 = file — drives the row icon
        int idx = static_cast<int>(SendMessageA(listBox, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(entry.name.c_str())));
        SendMessageA(listBox, LB_SETITEMDATA, idx, entry.isDirectory ? 1 : 0);
    }
}

std::string normalizeNewlines(const std::string& text)
{
    std::string normalized;
    normalized.reserve(text.size() + 16);

    for (char ch : text) {
        if (ch == '\n') {
            normalized += "\r\n";
        } else {
            normalized += ch;
        }
    }

    return normalized;
}

void appendTerminal(DesktopState* state, const std::string& text)
{
    if (!state || text.empty()) return;

    state->terminalBuffer += normalizeNewlines(text);
    state->terminalBuffer += "\r\n";
    if (state->output)
        SetWindowTextA(state->output, state->terminalBuffer.c_str());

    // Add AI response to chat history and show the panel
    state->chatHistory.push_back({false, text});
    state->chatPanelVisible = true;
    if (state->chatPanel && IsWindow(state->chatPanel)) {
        SetWindowPos(state->chatPanel, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        InvalidateRect(state->chatPanel, nullptr, TRUE);
    }
}

void finishTerminalCommand(DesktopState* state,
                           const port::kernel::Command& command,
                           const port::kernel::KernelResponse& response)
{
    if (!state) return;
    state->isAIRunning = false;
    if (state->input) EnableWindow(state->input, TRUE);
    if (!response.ok && response.message.rfind("CONFIRM_REQUIRED: ", 0) != 0) {
        state->aiStatusMsg = "Port Aborted. Check your connection.";
        state->statusClearCountdown = 38;
    } else {
        state->aiStatusMsg.clear();
    }

    if (!response.message.empty()) {
        if (response.message.rfind("CONFIRM_REQUIRED: ", 0) == 0) {
            std::string pendingCommand = response.message.substr(18);
            std::string promptMsg = "Allow Port OS to run this command?\n\n" + pendingCommand;
            int result = MessageBoxA(nullptr, promptMsg.c_str(), "Confirm Protected Action", MB_YESNO | MB_ICONWARNING);
            if (result == IDYES) {
                const auto confirmResponse = state->kernel.confirmPendingCommand();
                if (!confirmResponse.message.empty()) {
                    appendTerminal(state, confirmResponse.message);
                }
                if (!confirmResponse.ok) {
                    appendTerminal(state, "Command failed.");
                }
            } else {
                state->kernel.cancelPendingCommand();
                appendTerminal(state, "Command cancelled.");
            }
        } else {
            appendTerminal(state, response.message);
        }
    }

    // Refresh My Port window if it is open
    if (state->myPortWindow && IsWindow(state->myPortWindow)) {
        SendMessageA(state->myPortWindow, WM_COMMAND, IdMyPortRefresh, 0);
    }
    // Refresh dynamic desktop icons
    HWND parentWnd = GetParent(state->input);
    refreshDesktopDynamicIcons(parentWnd, state);

    // Refresh trash icon
    HWND trashBtn = GetDlgItem(parentWnd, IdIconTrashBin);
    if (trashBtn) InvalidateRect(trashBtn, nullptr, TRUE);
}

void runTerminalCommand(DesktopState* state, const std::string& text)
{
    if (!state || text.empty() || state->isAIRunning) return;

    // "/fox", "/nova hello ..." — switch the active agent by command.
    // Bare "/name" just switches; "/name <prompt>" switches and runs the
    // prompt as that agent.
    std::string effectiveText = text;
    if (text[0] == '/') {
        const std::size_t space = text.find(' ');
        std::string name = (space == std::string::npos)
                               ? text.substr(1)
                               : text.substr(1, space - 1);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        for (std::size_t i = 0; i < state->agents.size(); ++i) {
            // Compare lowercase and without spaces, so "/thegrid"
            // matches "The Grid".
            std::string agentName;
            for (const char ch : state->agents[i].name) {
                if (ch == ' ') continue;
                agentName += static_cast<char>(
                    std::tolower(static_cast<unsigned char>(ch)));
            }
            if (agentName != name) continue;

            state->activeAgent = static_cast<int>(i);
            state->aiStatusMsg = state->agents[i].name + " agent active";
            state->statusClearCountdown = 50;
            if (state->agentPanel && IsWindow(state->agentPanel)) {
                InvalidateRect(state->agentPanel, nullptr, FALSE);
            }
            HWND desktop = GetParent(state->input);
            if (desktop) {
                RECT tb;
                GetClientRect(desktop, &tb);
                tb.top = tb.bottom - kTaskbarReserved;
                InvalidateRect(desktop, &tb, FALSE);
            }

            std::string rest = (space == std::string::npos)
                                   ? std::string{}
                                   : text.substr(space + 1);
            const std::size_t firstChar = rest.find_first_not_of(' ');
            rest = (firstChar == std::string::npos) ? std::string{}
                                                    : rest.substr(firstChar);
            if (rest.empty()) return; // switch only, nothing to run
            effectiveText = std::move(rest);
            break;
        }
    }
    const std::string& commandText = effectiveText;

    // Add user message to chat history and show panel
    state->chatHistory.push_back({true, commandText});
    state->chatPanelVisible = true;
    if (state->chatPanel && IsWindow(state->chatPanel)) {
        SetWindowPos(state->chatPanel, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        InvalidateRect(state->chatPanel, nullptr, TRUE);
    }
    appendTerminal(state, "port> " + commandText);
    state->isAIRunning = true;
    state->aiStatusMsg.clear();
    state->statusClearCountdown = 0;

    HWND parent = GetParent(state->input);
    if (parent) {
        RECT r;
        GetClientRect(parent, &r);
        r.top = r.bottom - kTaskbarReserved;
        InvalidateRect(parent, &r, FALSE);
        UpdateWindow(parent);
    }

    auto command = state->router.parse(commandText);
    if (command.type == port::kernel::CommandType::Prompt) {
        // The active agent flavors the prompt; the chat/terminal display
        // above still shows only what the user typed.
        if (state->activeAgent >= 0 &&
            state->activeAgent < static_cast<int>(state->agents.size())) {
            const auto& agent = state->agents[state->activeAgent];
            command.rawText = "[You are " + agent.name +
                              ", the Port OS agent for " + agent.tagline +
                              ". Answer in that role.] " + command.rawText;
        }
        EnableWindow(state->input, FALSE);
        state->aiWorker = std::jthread(
            [state, parent, command](std::stop_token stopToken) {
                auto response = state->kernel.handleCommand(command);
                if (stopToken.stop_requested()) return;
                auto* result = new AsyncCommandResult{command, std::move(response)};
                if (!PostMessageA(parent, PortMessageAIComplete, 0,
                                  reinterpret_cast<LPARAM>(result))) {
                    delete result;
                }
            });
        return;
    }

    const auto response = state->kernel.handleCommand(command);
    finishTerminalCommand(state, command, response);
}

std::string getWindowText(HWND window)
{
    const int length = GetWindowTextLengthA(window);
    if (length <= 0) {
        return "";
    }

    std::string text(static_cast<std::size_t>(length), '\0');
    GetWindowTextA(window, &text[0], length + 1);
    return text;
}

// ── In-place icon rename, Windows-style ──────────────────────────────
// A small centered EDIT box appears over the icon's label. Enter or
// clicking elsewhere commits, Escape cancels.
WNDPROC gRenameEditOrigProc = nullptr;

void commitIconRename(HWND edit, bool apply)
{
    HWND desktop = GetParent(edit);
    DesktopState* state = reinterpret_cast<DesktopState*>(
        GetWindowLongPtrA(desktop, GWLP_USERDATA));
    if (!state || state->renameEdit != edit) return;
    // Clear first: DestroyWindow fires WM_KILLFOCUS, which calls back here.
    state->renameEdit = nullptr;
    const int iconId = state->renameIconId;
    state->renameIconId = 0;

    const std::string newName = getWindowText(edit);
    DestroyWindow(edit);
    if (state->renameBgBrush) {
        DeleteObject(state->renameBgBrush);
        state->renameBgBrush = nullptr;
    }
    // Bring the icon's own label back (it was hidden during the rename)
    if (HWND icon = GetDlgItem(desktop, iconId); icon && IsWindow(icon)) {
        InvalidateRect(icon, nullptr, TRUE);
    }
    if (!apply || newName.empty()) return;

    if (iconId == IdIconTrashBin) {
        if (newName != state->trashLabel) {
            state->trashLabel = newName;
            HWND icon = GetDlgItem(desktop, IdIconTrashBin);
            if (icon) InvalidateRect(icon, nullptr, TRUE);
        }
        return;
    }
    if (iconId == IdIconMyServer) {
        if (newName != state->myServerLabel) {
            state->myServerLabel = newName;
            HWND icon = GetDlgItem(desktop, IdIconMyServer);
            if (icon) InvalidateRect(icon, nullptr, TRUE);
        }
        return;
    }

    for (auto& di : state->dynamicIcons) {
        if (di.id != iconId) continue;
        if (newName != di.name) {
            // Keep the icon where it is under its new name.
            state->dynamicIconSavedPos[newName] = di.pos;
            state->dynamicIconSavedPos.erase(di.name);
            (void)state->kernel.getSandbox().rename(
                "Desktop/" + di.name, "Desktop/" + newName);
            refreshDesktopDynamicIcons(desktop, state);
        }
        break;
    }
}

LRESULT CALLBACK renameEditProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_PAINT: {
        // Let the edit paint its text, then draw the same rounded accent
        // border the selected icon tile uses.
        const LRESULT result = CallWindowProcA(gRenameEditOrigProc, hw, msg, wp, lp);
        HDC dc = GetDC(hw);
        RECT rc;
        GetClientRect(hw, &rc);
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath path;
            addRoundRect(path, 0, 0, rc.right - 1, rc.bottom - 1, 6);
            Gdiplus::Pen pen(Gdiplus::Color(210, 90, 160, 255), 1.6f);
            g.DrawPath(&pen, &path);
        }
        ReleaseDC(hw, dc);
        return result;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { commitIconRename(hw, true); return 0; }
        if (wp == VK_ESCAPE) { commitIconRename(hw, false); return 0; }
        goto redrawAfter;
    case WM_CHAR:
        // Swallow Enter/Escape chars so the edit control doesn't beep
        if (wp == VK_RETURN || wp == VK_ESCAPE) return 0;
        goto redrawAfter;
    // With a transparent background the edit never erases its old glyphs,
    // so anything that can change text/selection/caret must trigger a full
    // erase + repaint — otherwise letters pile up and look smudged.
    case WM_KEYUP:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEMOVE:
    redrawAfter: {
        const LRESULT result =
            CallWindowProcA(gRenameEditOrigProc, hw, msg, wp, lp);
        RedrawWindow(hw, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
        return result;
    }
    case WM_KILLFOCUS:
        commitIconRename(hw, true);
        break;
    }
    return CallWindowProcA(gRenameEditOrigProc, hw, msg, wp, lp);
}

void beginIconRename(HWND desktop, DesktopState* state, int iconId)
{
    if (!state) return;
    if (state->renameEdit && IsWindow(state->renameEdit)) {
        commitIconRename(state->renameEdit, true);
    }
    HWND icon = GetDlgItem(desktop, iconId);
    if (!icon || !IsWindow(icon)) return;

    std::string current;
    if (iconId == IdIconTrashBin) {
        current = state->trashLabel;
    } else if (iconId == IdIconMyServer) {
        current = state->myServerLabel;
    } else {
        for (const auto& di : state->dynamicIcons) {
            if (di.id == iconId) { current = di.name; break; }
        }
        if (current.empty()) return;
    }

    RECT r;
    GetWindowRect(icon, &r);
    POINT topLeft = {r.left, r.top};
    ScreenToClient(desktop, &topLeft);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;

    // Inset well inside the selection tile (the tile itself is inset ~7px
    // from the button edges) so the rename box never touches its corners.
    const int editX = topLeft.x + 14;
    const int editY = topLeft.y + h - 32;
    const int editW = w - 28;
    const int editH = 22;

    // ES_MULTILINE only so EM_SETRECT works — a single-line EDIT always
    // top-aligns its text; Enter is intercepted by the subclass below.
    HWND edit = CreateWindowExA(0, "EDIT", current.c_str(),
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_MULTILINE | ES_CENTER,
        editX, editY, editW, editH,
        desktop, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!edit) return;

    // "Transparent" background: a pattern brush holding the exact wallpaper
    // patch behind the edit, so only the text is visible.
    if (state->renameBgBrush) {
        DeleteObject(state->renameBgBrush);
        state->renameBgBrush = nullptr;
    }
    if (cachedBackground) {
        HDC screenDC = GetDC(desktop);
        HDC src = CreateCompatibleDC(screenDC);
        HGDIOBJ oldSrc = SelectObject(src, cachedBackground);
        HDC dst = CreateCompatibleDC(screenDC);
        HBITMAP patch = CreateCompatibleBitmap(screenDC, editW, editH);
        HGDIOBJ oldDst = SelectObject(dst, patch);
        BitBlt(dst, 0, 0, editW, editH, src, editX, editY, SRCCOPY);
        SelectObject(dst, oldDst);
        DeleteDC(dst);
        SelectObject(src, oldSrc);
        DeleteDC(src);
        ReleaseDC(desktop, screenDC);
        state->renameBgBrush = CreatePatternBrush(patch);
        DeleteObject(patch); // the brush keeps its own copy
    }

    if (state->labelFont) {
        SendMessageA(edit, WM_SETFONT,
                     reinterpret_cast<WPARAM>(state->labelFont), TRUE);
    }

    // Vertically center the text line inside the box
    {
        HDC editDc = GetDC(edit);
        HGDIOBJ oldFont = SelectObject(
            editDc, state->labelFont
                        ? state->labelFont
                        : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
        TEXTMETRICA tm = {};
        GetTextMetricsA(editDc, &tm);
        SelectObject(editDc, oldFont);
        ReleaseDC(edit, editDc);
        RECT fmt = {6, std::max(0L, (editH - tm.tmHeight) / 2),
                    editW - 6, editH};
        SendMessageA(edit, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&fmt));
    }

    gRenameEditOrigProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
        edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(renameEditProc)));
    state->renameEdit = edit;
    state->renameIconId = iconId;
    // Repaint the icon now so its own label disappears under the box
    RedrawWindow(icon, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    SendMessageA(edit, EM_SETSEL, 0, -1);
    SetWindowPos(edit, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetFocus(edit);
}

// Turn an icon window into a translucent, click-through-composited ghost
// while it is being dragged (WS_EX_LAYERED at ~55% alpha), so the icons it
// passes over remain visible behind it — the Windows drag-image look.
void setIconDragGhost(HWND icon, bool on)
{
    if (!icon || !IsWindow(icon)) return;
    const LONG_PTR ex = GetWindowLongPtrA(icon, GWL_EXSTYLE);
    if (on) {
        SetWindowLongPtrA(icon, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(icon, 0, 140, LWA_ALPHA);
    } else if (ex & WS_EX_LAYERED) {
        SetWindowLongPtrA(icon, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
        RedrawWindow(icon, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

// Pointer to an icon's stored position by control id — works for both the
// system icons (My Server, Trash Bin, My Port) and dynamic desktop icons.
POINT* iconPosPtr(DesktopState* state, int id)
{
    if (!state) return nullptr;
    if (id == IdIconMyComputer) return &state->myPortPos;
    if (id == IdIconTrashBin)   return &state->trashPos;
    if (id == IdIconMyServer)   return &state->myServerPos;
    for (auto& di : state->dynamicIcons) {
        if (di.id == id) return &di.pos;
    }
    return nullptr;
}

// Move every currently selected dynamic desktop icon to Trash in one pass,
// then refresh once. Covers both the single selection and a rubber-band
// multi-selection.
void deleteSelectedDesktopIcons(HWND window, DesktopState* state)
{
    if (!state) return;
    std::set<int> ids(state->dragSelectedIconIds.begin(),
                      state->dragSelectedIconIds.end());
    if (state->selectedIconId != 0) ids.insert(state->selectedIconId);

    std::vector<std::string> names;
    for (const int id : ids) {
        for (const auto& di : state->dynamicIcons) {
            if (di.id == id) { names.push_back(di.name); break; }
        }
    }
    if (names.empty()) return;

    for (const auto& name : names) {
        (void)state->kernel.getSandbox().moveToTrash("Desktop/" + name);
        state->dynamicIconSavedPos.erase(name);
    }
    state->selectedIconId = 0;
    state->dragSelectedIconIds.clear();
    refreshDesktopDynamicIcons(window, state);

    if (HWND trashBtn = GetDlgItem(window, IdIconTrashBin)) {
        RedrawWindow(trashBtn, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
    // Trash window, if open, updates live too
    if (state->trashWindow && IsWindow(state->trashWindow)) {
        if (HWND trashLb = GetDlgItem(state->trashWindow, IdTrashList)) {
            refreshTrashList(trashLb, &state->kernel.getSandbox());
        }
    }
    RECT dockRect;
    GetClientRect(window, &dockRect);
    dockRect.top = dockRect.bottom - kDockH;
    InvalidateRect(window, &dockRect, FALSE);
}

LRESULT CALLBACK iconButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND parent = GetParent(hwnd);
    DesktopState* state = reinterpret_cast<DesktopState*>(GetWindowLongPtrA(parent, GWLP_USERDATA));
    int controlId = GetDlgCtrlID(hwnd);
    switch (msg) {
    case WM_ERASEBKGND:
        // drawDesktopIcon paints every pixel from its own buffer; letting
        // the BUTTON class erase first caused a blank flash while dragging.
        return 1;
    case WM_KEYDOWN:
        if (wp == VK_DELETE && state && controlId >= 1100) {
            deleteSelectedDesktopIcons(parent, state);
            return 0;
        }
        break;
    case WM_LBUTTONDBLCLK:
        // Double-click opens the item (folders open a browser window,
        // files open their viewer) — like the Windows desktop.
        if (state) {
            state->isDraggingIcon = false;
            ReleaseCapture();
            if (controlId >= 1100) {
                SendMessageA(parent, WM_COMMAND,
                             MAKEWPARAM(controlId, 0), 0);
            } else {
                SendMessageA(parent, WM_COMMAND, controlId, 0);
            }
            return 0;
        }
        break;
    case WM_MEASUREITEM:
        // Owner-drawn context menu of this icon
        if (measurePortMenuItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp))) {
            return TRUE;
        }
        break;
    case WM_DRAWITEM:
        if (drawPortMenuItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lp))) {
            return TRUE;
        }
        break;
    case WM_RBUTTONUP: {
        if (state) {
            // The button window is slightly larger than the visible icon
            // tile; only react when the cursor is actually over the tile.
            POINT clickPt = {static_cast<short>(LOWORD(lp)),
                             static_cast<short>(HIWORD(lp))};
            RECT client;
            GetClientRect(hwnd, &client);
            const int imgSize = getIconLayoutInfo(state->iconSize).imageSize;
            const int boundsWidth = client.right - client.left;
            const int tileWidth = std::min(boundsWidth - 8, imgSize + 12);
            RECT tile;
            tile.left   = (boundsWidth - tileWidth) / 2;
            tile.top    = 6;
            tile.right  = tile.left + tileWidth;
            tile.bottom = client.bottom - 5;
            if (!PtInRect(&tile, clickPt)) {
                // Treat it as a right-click on the desktop behind the icon.
                POINT desktopPt = clickPt;
                MapWindowPoints(hwnd, parent, &desktopPt, 1);
                SendMessageA(parent, WM_RBUTTONUP, wp,
                             MAKELPARAM(desktopPt.x, desktopPt.y));
                return 0;
            }

            // If this icon is part of a rubber-band multi-selection, keep
            // that whole selection so the menu's Delete applies to all of
            // it. Otherwise right-click selects just this icon (like left).
            const bool inMultiSelection =
                std::find(state->dragSelectedIconIds.begin(),
                          state->dragSelectedIconIds.end(),
                          controlId) != state->dragSelectedIconIds.end();
            if (!inMultiSelection) {
                const std::vector<int> previousDragSelection =
                    state->dragSelectedIconIds;
                state->dragSelectedIconIds.clear();
                for (int selectedId : previousDragSelection) {
                    if (selectedId == controlId) continue;
                    HWND selectedIcon = GetDlgItem(parent, selectedId);
                    if (selectedIcon) {
                        RedrawWindow(selectedIcon, nullptr, nullptr,
                                     RDW_INVALIDATE | RDW_UPDATENOW);
                    }
                }
                const int previousSelectedId = state->selectedIconId;
                state->selectedIconId = controlId;
                if (previousSelectedId != 0 &&
                    previousSelectedId != controlId) {
                    HWND previousIcon = GetDlgItem(parent, previousSelectedId);
                    if (previousIcon) {
                        RedrawWindow(previousIcon, nullptr, nullptr,
                                     RDW_INVALIDATE | RDW_UPDATENOW);
                    }
                }
                RedrawWindow(hwnd, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_UPDATENOW);
            }

            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            
            if (controlId == IdIconTrashBin) {
                AppendMenuA(hMenu, MF_STRING, 6104, "Open Trash Bin");
                AppendMenuA(hMenu, MF_STRING, 6101, "Empty Trash Bin");
                AppendMenuA(hMenu, MF_STRING, 6102, "Rename");
                AppendMenuA(hMenu, MF_STRING, 6103, "Properties");
            } else if (controlId == IdIconMyServer) {
                AppendMenuA(hMenu, MF_STRING, 6401, "Open My Server");
                AppendMenuA(hMenu, MF_STRING, 6404, "Rename");
                AppendMenuA(hMenu, MF_STRING, 6403, "Properties");
            } else {
                // Dynamic desktop file/folder icons
                std::string openLabel = "Open";
                for (const auto& di : state->dynamicIcons) {
                    if (di.id == controlId) {
                        openLabel = "Open " + di.name;
                        break;
                    }
                }
                AppendMenuA(hMenu, MF_STRING, 6301, openLabel.c_str());
                AppendMenuA(hMenu, MF_STRING, 6302, "Delete");
                AppendMenuA(hMenu, MF_STRING, 6304, "Rename");
                AppendMenuA(hMenu, MF_STRING, 6303, "Properties");
            }
            
            std::vector<std::unique_ptr<PortMenuItem>> menuTheme;
            themePortMenu(hMenu, menuTheme);
            int cmd = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
            
            if (cmd == 6101) { // Empty Trash
                (void)state->kernel.getSandbox().emptyTrash();
                if (state->trashWindow && IsWindow(state->trashWindow)) {
                    HWND lb = GetDlgItem(state->trashWindow, IdTrashList);
                    if (lb) refreshTrashList(lb, &state->kernel.getSandbox());
                }
                InvalidateRect(hwnd, nullptr, TRUE);
            } else if (cmd == 6102) { // Rename Trash Bin (in place)
                beginIconRename(parent, state, IdIconTrashBin);
            } else if (cmd == 6104) { // Open Trash Bin
                SendMessageA(parent, WM_COMMAND, IdIconTrashBin, 0);
            } else if (cmd == 6103) { // Trash Properties
                auto trashResult3 = state->kernel.getSandbox().listTrash();
                int itemCount = 0;
                std::uintmax_t totalBytes = 0;
                if (trashResult3) {
                    for (const auto& entry : *trashResult3) {
                        ++itemCount;
                        totalBytes += entry.size;
                    }
                }
                std::vector<PortPropRow> rows = {
                    {"Type", "System Folder"},
                    {"Status", itemCount == 0 ? "Empty"
                                              : "Contains deleted items"},
                    {"Items", std::to_string(itemCount)},
                    {"Total size", formatBytes(totalBytes)},
                    {"Location", "sandbox/.trash/"},
                };
                showPortProperties(parent, state->trashLabel, rows);
            } else if (cmd == 6401) { // Open My Server
                SendMessageA(parent, WM_COMMAND, IdIconMyServer, 0);
            } else if (cmd == 6404) { // Rename My Server (in place)
                beginIconRename(parent, state, IdIconMyServer);
            } else if (cmd == 6403) { // My Server Properties
                char localAppData[MAX_PATH] = {};
                GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
                std::string dataPath = std::string(localAppData) + "\\PortOS";
                DirStats stats = computeDirStats(dataPath);
                std::vector<PortPropRow> rows = {
                    {"Type", "Local Data Storage"},
                    {"Privacy", "All data stored on this machine only"},
                    {"Contents", std::to_string(stats.files) + " files, " +
                                 std::to_string(stats.folders) + " folders"},
                    {"Size on disk", formatBytes(stats.bytes)},
                    {"Path", dataPath},
                };
                showPortProperties(parent, state->myServerLabel, rows);
            } else if (cmd == 6301) { // Open dynamic desktop folder
                SendMessageA(parent, WM_COMMAND, MAKEWPARAM(controlId, 0), 0);
            } else if (cmd == 6302) { // Delete — whole selection if multiple
                if (state->dragSelectedIconIds.size() > 1) {
                    deleteSelectedDesktopIcons(parent, state);
                } else {
                    SendMessageA(parent, WM_COMMAND,
                                 MAKEWPARAM(controlId, 1), 0);
                }
            } else if (cmd == 6303) { // Dynamic desktop folder properties
                SendMessageA(parent, WM_COMMAND, MAKEWPARAM(controlId, 2), 0);
            } else if (cmd == 6304) { // Rename dynamic file/folder (in place)
                beginIconRename(parent, state, controlId);
            }
            return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        if (state) {
            const std::vector<int> previousDragSelection = state->dragSelectedIconIds;
            // Clicking an icon that is already part of a multi-selection
            // keeps the whole group (so it can be dragged together);
            // clicking any other icon selects just that one.
            const bool clickedInGroup =
                previousDragSelection.size() > 1 &&
                std::find(previousDragSelection.begin(),
                          previousDragSelection.end(),
                          controlId) != previousDragSelection.end();

            state->groupDrag = false;
            state->groupOrig.clear();

            if (clickedInGroup) {
                state->groupDrag = true;
                for (int id : previousDragSelection) {
                    if (POINT* p = iconPosPtr(state, id)) {
                        state->groupOrig.push_back({id, *p});
                    }
                }
            } else {
                state->dragSelectedIconIds.clear();
                for (int selectedId : previousDragSelection) {
                    if (selectedId == controlId) continue;
                    HWND selectedIcon = GetDlgItem(parent, selectedId);
                    if (selectedIcon) {
                        RedrawWindow(selectedIcon, nullptr, nullptr,
                                     RDW_INVALIDATE | RDW_UPDATENOW);
                    }
                }
                const int previousSelectedId = state->selectedIconId;
                state->selectedIconId = controlId;
                if (previousSelectedId != 0 &&
                    previousSelectedId != controlId) {
                    HWND previousIcon = GetDlgItem(parent, previousSelectedId);
                    if (previousIcon) InvalidateRect(previousIcon, nullptr, TRUE);
                }
                InvalidateRect(hwnd, nullptr, TRUE);
            }

            state->isDraggingIcon = true;
            state->iconDragMoved = false;
            state->lastIconDragPaint = 0;
            state->draggedIconId = controlId;

            POINT* pPos = nullptr;
            if (controlId == IdIconMyComputer) pPos = &state->myPortPos;
            else if (controlId == IdIconTrashBin) pPos = &state->trashPos;
            else if (controlId == IdIconMyServer) pPos = &state->myServerPos;
            else {
                for (auto& di : state->dynamicIcons) {
                    if (di.id == controlId) {
                        pPos = &di.pos;
                        break;
                    }
                }
            }

            if (pPos) {
                state->draggedIconOrigPos = *pPos;
            } else {
                state->draggedIconOrigPos = {0, 0};
            }

            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(parent, &pt);

            RECT btnRect;
            GetWindowRect(hwnd, &btnRect);
            POINT btnTopLeft = {btnRect.left, btnRect.top};
            ScreenToClient(parent, &btnTopLeft);

            state->iconDragOffset.x = pt.x - btnTopLeft.x;
            state->iconDragOffset.y = pt.y - btnTopLeft.y;

            // Raise above the sibling icons so that while dragging over a
            // target (e.g. Trash Bin) this icon shows on top of it.
            SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

            // Make the dragged icon(s) translucent ghosts so icons behind
            // stay visible, like the Windows drag image.
            setIconDragGhost(hwnd, true);
            if (state->groupDrag) {
                for (const auto& [gid, gorig] : state->groupOrig) {
                    if (gid != controlId) {
                        setIconDragGhost(GetDlgItem(parent, gid), true);
                    }
                }
            }
            SetCapture(hwnd);
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (state && state->isDraggingIcon && state->draggedIconId == controlId) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(parent, &pt);

            int newX = pt.x - state->iconDragOffset.x;
            int newY = pt.y - state->iconDragOffset.y;

            // A normal click can produce tiny mouse movements. Do not turn
            // those into a desktop-wide icon drag/refresh.
            if (!state->iconDragMoved) {
                const int dx = newX - state->draggedIconOrigPos.x;
                const int dy = newY - state->draggedIconOrigPos.y;
                if (dx >= -3 && dx <= 3 && dy >= -3 && dy <= 3) {
                    break;
                }
                state->iconDragMoved = true;
            }

            auto layoutInfo = getIconLayoutInfo(state->iconSize);

            RECT clientRect;
            GetClientRect(parent, &clientRect);
            int maxX = (clientRect.right - clientRect.left) - layoutInfo.width;
            int maxY = (clientRect.bottom - clientRect.top) - kTaskbarReserved - layoutInfo.height;
            if (newX < 0) newX = 0;
            if (newY < 0) newY = 0;
            if (newX > maxX) newX = maxX;
            if (newY > maxY) newY = maxY;

            // Coalesce very dense WM_MOUSEMOVE bursts to a maximum of about
            // 120 visual updates per second.
            const ULONGLONG now = GetTickCount64();
            if (state->lastIconDragPaint != 0 &&
                now - state->lastIconDragPaint < 8) {
                return 0;
            }
            state->lastIconDragPaint = now;

            POINT* pPos = nullptr;
            if (controlId == IdIconMyComputer) pPos = &state->myPortPos;
            else if (controlId == IdIconTrashBin) pPos = &state->trashPos;
            else if (controlId == IdIconMyServer) pPos = &state->myServerPos;
            else {
                for (auto& di : state->dynamicIcons) {
                    if (di.id == controlId) {
                        pPos = &di.pos;
                        break;
                    }
                }
            }
            if (pPos) {
                *pPos = {newX, newY};
            }

            RECT oldScreenRect;
            GetWindowRect(hwnd, &oldScreenRect);
            POINT oldTopLeft = {oldScreenRect.left, oldScreenRect.top};
            POINT oldBottomRight = {oldScreenRect.right, oldScreenRect.bottom};
            ScreenToClient(parent, &oldTopLeft);
            ScreenToClient(parent, &oldBottomRight);
            RECT oldParentRect = {
                oldTopLeft.x, oldTopLeft.y,
                oldBottomRight.x, oldBottomRight.y
            };

            // SWP_NOCOPYBITS: without it the window drags its previously
            // painted pixels along — including the wallpaper tile baked in
            // for the OLD position, which looked like the icon carried a
            // photo of the background with it.
            SetWindowPos(hwnd, nullptr, newX, newY,
                         layoutInfo.width, layoutInfo.height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);

            if (!state->groupDrag) {
                // Single-icon drag: erase old spot, repaint icon at new spot.
                RedrawWindow(parent, &oldParentRect, nullptr,
                             RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                RedrawWindow(hwnd, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_UPDATENOW);
            } else {
                // Group drag: move every member (including the primary,
                // already positioned above) by the same delta, then repaint
                // the whole desktop once. A single full redraw avoids the
                // partial-rect trails that per-window repaints left behind.
                const int deltaX = newX - state->draggedIconOrigPos.x;
                const int deltaY = newY - state->draggedIconOrigPos.y;
                for (const auto& [gid, gorig] : state->groupOrig) {
                    if (gid == controlId) continue;
                    int gx = gorig.x + deltaX;
                    int gy = gorig.y + deltaY;
                    if (gx < 0) gx = 0;
                    if (gy < 0) gy = 0;
                    if (gx > maxX) gx = maxX;
                    if (gy > maxY) gy = maxY;
                    if (POINT* p = iconPosPtr(state, gid)) *p = {gx, gy};
                    if (HWND gwnd = GetDlgItem(parent, gid)) {
                        SetWindowPos(gwnd, nullptr, gx, gy,
                                     layoutInfo.width, layoutInfo.height,
                                     SWP_NOZORDER | SWP_NOACTIVATE |
                                     SWP_NOCOPYBITS);
                    }
                }
                RECT full;
                GetClientRect(parent, &full);
                full.bottom -= kTaskbarReserved;
                RedrawWindow(parent, &full, nullptr,
                             RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            }

            // ── Drop-target feedback under the cursor ─────────────
            {
                POINT cur;
                GetCursorPos(&cur);
                ScreenToClient(parent, &cur);

                const bool draggedSystem = (controlId == IdIconTrashBin ||
                                            controlId == IdIconMyServer);
                bool blocked = false;
                std::string hint;

                if (controlId != IdIconTrashBin) {
                    RECT trashR = {state->trashPos.x, state->trashPos.y,
                                   state->trashPos.x + layoutInfo.width,
                                   state->trashPos.y + layoutInfo.height};
                    if (PtInRect(&trashR, cur)) {
                        if (draggedSystem) blocked = true;
                        else hint = "Move to " + state->trashLabel;
                    }
                }
                if (!blocked && hint.empty()) {
                    for (const auto& di : state->dynamicIcons) {
                        if (di.id == controlId || !di.isDir) continue;
                        RECT fr = {di.pos.x, di.pos.y,
                                   di.pos.x + layoutInfo.width,
                                   di.pos.y + layoutInfo.height};
                        if (PtInRect(&fr, cur)) {
                            if (draggedSystem) blocked = true;
                            else hint = "Move to " + di.name;
                            break;
                        }
                    }
                }

                if (blocked != state->dragHoverBlocked ||
                    hint    != state->dragHoverText) {
                    state->dragHoverBlocked = blocked;
                    state->dragHoverText    = std::move(hint);
                    RedrawWindow(hwnd, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_UPDATENOW);
                }
            }
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (state && state->isDraggingIcon && state->draggedIconId == controlId) {
            state->isDraggingIcon = false;
            state->dragHoverBlocked = false;
            state->dragHoverText.clear();
            ReleaseCapture();

            // Restore the dragged icon(s) from ghost to solid before any
            // drop handling (some paths rebuild/destroy the windows).
            setIconDragGhost(hwnd, false);
            for (const auto& [gid, gorig] : state->groupOrig) {
                if (gid != controlId) {
                    setIconDragGhost(GetDlgItem(parent, gid), false);
                }
            }

            if (!state->iconDragMoved) {
                // Selection-only click: repaint just this icon. The original
                // button procedure still receives WM_LBUTTONUP below.
                state->groupDrag = false;
                state->groupOrig.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }

            // Group drag: snap every member to its own nearest grid cell,
            // then refresh once. Keeps the whole selection movable together.
            if (state->groupDrag) {
                auto layoutInfo = getIconLayoutInfo(state->iconSize);
                RECT deskRect;
                GetClientRect(parent, &deskRect);
                const int maxSnapX =
                    (deskRect.right - deskRect.left) - layoutInfo.width;
                const int maxSnapY =
                    (deskRect.bottom - deskRect.top) - kTaskbarReserved -
                    layoutInfo.height;
                for (const auto& [gid, gorig] : state->groupOrig) {
                    POINT* p = iconPosPtr(state, gid);
                    if (!p) continue;
                    int col = (p->x - 28 + layoutInfo.gapX / 2) /
                              layoutInfo.gapX;
                    int row = (p->y - 34 + layoutInfo.gapY / 2) /
                              layoutInfo.gapY;
                    if (col < 0) col = 0;
                    if (row < 0) row = 0;
                    int sx = 28 + col * layoutInfo.gapX;
                    int sy = 34 + row * layoutInfo.gapY;
                    if (sx > maxSnapX) sx = maxSnapX;
                    if (sy > maxSnapY) sy = maxSnapY;
                    *p = {sx, sy};
                    if (HWND gwnd = GetDlgItem(parent, gid)) {
                        SetWindowPos(gwnd, nullptr, sx, sy,
                                     layoutInfo.width, layoutInfo.height,
                                     SWP_NOZORDER | SWP_NOACTIVATE |
                                     SWP_NOCOPYBITS);
                    }
                }
                state->groupDrag = false;
                state->groupOrig.clear();
                RECT full;
                GetClientRect(parent, &full);
                full.bottom -= kTaskbarReserved;
                InvalidateRect(parent, &full, TRUE);
                UpdateWindow(parent);
                if (state->agentPanel && IsWindow(state->agentPanel)) {
                    SetWindowPos(state->agentPanel, HWND_TOP, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
                return 0;
            }

            int currentX = 0;
            int currentY = 0;
            POINT* pPos = nullptr;
            if (controlId == IdIconMyComputer) pPos = &state->myPortPos;
            else if (controlId == IdIconTrashBin) pPos = &state->trashPos;
            else if (controlId == IdIconMyServer) pPos = &state->myServerPos;
            else {
                for (auto& di : state->dynamicIcons) {
                    if (di.id == controlId) {
                        pPos = &di.pos;
                        break;
                    }
                }
            }
            if (pPos) {
                currentX = pPos->x;
                currentY = pPos->y;
            }

            auto layoutInfo = getIconLayoutInfo(state->iconSize);

            // Like Windows: the drop counts where the CURSOR is,
            // not where the icon's center happens to land.
            POINT cursor;
            GetCursorPos(&cursor);
            ScreenToClient(parent, &cursor);

            RECT trashRect = {state->trashPos.x, state->trashPos.y,
                              state->trashPos.x + layoutInfo.width,
                              state->trashPos.y + layoutInfo.height};

            // ── Files/folders: drop on Trash Bin or into a folder ────
            if (controlId >= 1100) {
                if (PtInRect(&trashRect, cursor)) {
                    // Straight to trash, no confirmation. Same path as the
                    // context-menu Delete — it moves the item to sandbox
                    // trash and rebuilds the desktop icons, which destroys
                    // THIS button window, so return without touching hwnd.
                    SendMessageA(parent, WM_COMMAND,
                                 MAKEWPARAM(controlId, 1), 0);
                    return 0;
                }

                // Dropping onto a folder moves the item inside it
                for (const auto& target : state->dynamicIcons) {
                    if (target.id == controlId || !target.isDir) continue;
                    RECT fr = {target.pos.x, target.pos.y,
                               target.pos.x + layoutInfo.width,
                               target.pos.y + layoutInfo.height};
                    if (!PtInRect(&fr, cursor)) continue;

                    std::string dragName;
                    for (const auto& di : state->dynamicIcons) {
                        if (di.id == controlId) { dragName = di.name; break; }
                    }
                    if (!dragName.empty()) {
                        (void)state->kernel.getSandbox().rename(
                            "Desktop/" + dragName,
                            "Desktop/" + target.name + "/" + dragName);
                        // Rebuild destroys this button window — return now.
                        refreshDesktopDynamicIcons(parent, state);
                        return 0;
                    }
                    break;
                }
            }

            // ── System icons can never enter the bin or a folder ─────
            bool dropBlocked = false;
            if (controlId == IdIconTrashBin || controlId == IdIconMyServer) {
                if (controlId != IdIconTrashBin &&
                    PtInRect(&trashRect, cursor)) {
                    dropBlocked = true;
                }
                if (!dropBlocked) {
                    for (const auto& di : state->dynamicIcons) {
                        if (!di.isDir) continue;
                        RECT fr = {di.pos.x, di.pos.y,
                                   di.pos.x + layoutInfo.width,
                                   di.pos.y + layoutInfo.height};
                        if (PtInRect(&fr, cursor)) { dropBlocked = true; break; }
                    }
                }
            }

            // Grid snapping logic (rows start at 34, cols start at 28)
            int col = (currentX - 28 + layoutInfo.gapX / 2) / layoutInfo.gapX;
            int row = (currentY - 34 + layoutInfo.gapY / 2) / layoutInfo.gapY;
            if (col < 0) col = 0;
            if (row < 0) row = 0;

            int snappedX = 28 + col * layoutInfo.gapX;
            int snappedY = 34 + row * layoutInfo.gapY;

            // The drag clamp keeps the icon above the taskbar, but rounding
            // to the nearest grid row can push the snap back down over it.
            // Clamp the snapped cell to the client area minus the taskbar so
            // an icon can sit at the bottom without covering the search bar.
            {
                RECT deskRect;
                GetClientRect(parent, &deskRect);
                const int maxSnapX =
                    (deskRect.right - deskRect.left) - layoutInfo.width;
                const int maxSnapY =
                    (deskRect.bottom - deskRect.top) - kTaskbarReserved -
                    layoutInfo.height;
                if (snappedX > maxSnapX) snappedX = maxSnapX;
                if (snappedY > maxSnapY) snappedY = maxSnapY;
            }

            // Prevent overlapping with other icons.
            // (No My Port check — the icon was removed, but its stale
            // position kept "occupying" the top grid cell and made it
            // impossible to drag another icon back there.)
            bool isOverlap = false;

            // Check Trash Bin
            if (!isOverlap && controlId != IdIconTrashBin) {
                int otherCol = (state->trashPos.x - 28 + layoutInfo.gapX / 2) / layoutInfo.gapX;
                int otherRow = (state->trashPos.y - 34 + layoutInfo.gapY / 2) / layoutInfo.gapY;
                if (col == otherCol && row == otherRow) isOverlap = true;
            }
            // Check My Server
            if (!isOverlap && controlId != IdIconMyServer) {
                int otherCol = (state->myServerPos.x - 28 + layoutInfo.gapX / 2) / layoutInfo.gapX;
                int otherRow = (state->myServerPos.y - 34 + layoutInfo.gapY / 2) / layoutInfo.gapY;
                if (col == otherCol && row == otherRow) isOverlap = true;
            }
            // Check dynamic desktop icons
            if (!isOverlap) {
                for (const auto& di : state->dynamicIcons) {
                    if (di.id != controlId) {
                        int otherCol = (di.pos.x - 28 + layoutInfo.gapX / 2) / layoutInfo.gapX;
                        int otherRow = (di.pos.y - 34 + layoutInfo.gapY / 2) / layoutInfo.gapY;
                        if (col == otherCol && row == otherRow) {
                            isOverlap = true;
                            break;
                        }
                    }
                }
            }

            if (isOverlap || dropBlocked) {
                snappedX = state->draggedIconOrigPos.x;
                snappedY = state->draggedIconOrigPos.y;
            }

            // Apply final coordinates
            if (pPos) {
                *pPos = {snappedX, snappedY};
            }

            // SWP_NOCOPYBITS + explicit redraw: MoveWindow would copy the
            // button's current pixels to the snapped position, carrying the
            // wallpaper patch baked for the pre-snap spot (visible as a
            // misaligned photo tile behind the icon).
            SetWindowPos(hwnd, nullptr, snappedX, snappedY,
                         layoutInfo.width, layoutInfo.height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW);

            RECT r;
            GetClientRect(parent, &r);
            r.bottom -= kTaskbarReserved;
            InvalidateRect(parent, &r, TRUE);
            UpdateWindow(parent);

            // Drag start raised this icon with HWND_TOP — put the agents
            // panel back above the icons.
            if (state->agentPanel && IsWindow(state->agentPanel)) {
                SetWindowPos(state->agentPanel, HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            if (state->agentHandle && IsWindow(state->agentHandle)) {
                SetWindowPos(state->agentHandle, HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        break;
    }
    }

    WNDPROC originalProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (originalProc) {
        return CallWindowProcA(originalProc, hwnd, msg, wp, lp);
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

LRESULT CALLBACK editSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        HWND parent = GetParent(hwnd);
        SendMessageA(parent, WM_COMMAND, IdRunCommand, 0);
        return 0; // Prevent default Windows beep on Enter in edit controls
    }
    WNDPROC originalProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (originalProc) {
        return CallWindowProcA(originalProc, hwnd, msg, wp, lp);
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

HWND createDesktopButton(HWND parent, int id, const char* text, int x, int y,
                         int width = 122, int height = 132)
{
    // Created hidden on purpose: a visible BUTTON paints its default white
    // background before our subclass takes over, which flashed as a white
    // square whenever an icon was (re)created. Showing it only after the
    // subclass is installed means the first paint is already owner-drawn.
    // WS_CLIPSIBLINGS: without it a repainting icon draws straight over
    // overlapping siblings (e.g. the agents panel) regardless of z-order.
    HWND hwnd = CreateWindowExA(
        0,
        "BUTTON",
        text,
        WS_CHILD | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleA(nullptr),
        nullptr
    );

    WNDPROC originalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(iconButtonProc)));
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(originalProc));
    ShowWindow(hwnd, SW_SHOWNA);

    return hwnd;
}

void refreshDesktopDynamicIcons(HWND window, DesktopState* state,
                                bool fullRebuild)
{
    if (!state) return;

    // Remember where every icon currently sits. A refresh (rename, new
    // folder, delete, drag-into-folder) must not send icons the user moved
    // back to their default grid slots.
    for (auto& icon : state->dynamicIcons) {
        state->dynamicIconSavedPos[icon.name] = icon.pos;
    }

    const std::vector<DesktopState::DynamicIcon> oldIcons =
        std::move(state->dynamicIcons);
    state->dynamicIcons.clear();

    if (fullRebuild) {
        // Explicit "Refresh": blink like the Windows desktop — everything
        // disappears for a moment, then comes back rebuilt from disk.
        for (const auto& icon : oldIcons) {
            if (icon.hwnd && IsWindow(icon.hwnd)) DestroyWindow(icon.hwnd);
        }
        HWND trashIcon  = GetDlgItem(window, IdIconTrashBin);
        HWND serverIcon = GetDlgItem(window, IdIconMyServer);
        if (trashIcon)  ShowWindow(trashIcon, SW_HIDE);
        if (serverIcon) ShowWindow(serverIcon, SW_HIDE);
        RECT blank;
        GetClientRect(window, &blank);
        blank.bottom -= kTaskbarReserved;
        RedrawWindow(window, &blank, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW);
        Sleep(200);
        if (trashIcon)  ShowWindow(trashIcon, SW_SHOWNA);
        if (serverIcon) ShowWindow(serverIcon, SW_SHOWNA);
    }

    (void)state->kernel.getSandbox().makeDir("Desktop");
    auto desktopResult = state->kernel.getSandbox().list("Desktop");

    auto layoutInfo = getIconLayoutInfo(state->iconSize);

    // How many icon rows fit above the taskbar, so new icons flow down the
    // first column and into the next once it fills — the whole desktop
    // height, not a hard-coded 3.
    RECT gridRc;
    GetClientRect(window, &gridRc);
    const int usableH = (gridRc.bottom - gridRc.top) - kTaskbarReserved;
    const int maxRow = std::max(0, (usableH - 34) / layoutInfo.gapY);

    // Grid cell as an actual (column, row) pair — no collision between
    // columns regardless of how many rows a column holds.
    auto cellOf = [&](POINT p) -> std::pair<int, int> {
        int col = (p.x - 28 + layoutInfo.gapX / 2) / layoutInfo.gapX;
        int row = (p.y - 34 + layoutInfo.gapY / 2) / layoutInfo.gapY;
        if (col < 0) col = 0;
        if (row < 0) row = 0;
        return {col, row};
    };

    std::set<std::pair<int, int>> occupied;
    occupied.insert(cellOf(state->trashPos));
    occupied.insert(cellOf(state->myServerPos));

    // First pass: reuse the existing window of an unchanged entry (so
    // creating/deleting one item never repaints its neighbors), and give
    // entries with a remembered position their spot back.
    struct Placement {
        std::string name;
        bool isDir;
        POINT pos;
        bool placed;
        HWND hwnd; // reused window, or nullptr to create one
        int id;
    };
    std::vector<Placement> placements;
    int nextId = 1100;
    for (const auto& icon : oldIcons) {
        if (icon.id >= nextId) nextId = icon.id + 1;
    }
    if (desktopResult) {
        for (const auto& entry : *desktopResult) {
            Placement p{entry.name, entry.isDirectory, {0, 0}, false,
                        nullptr, 0};
            if (!fullRebuild) {
                for (const auto& icon : oldIcons) {
                    if (icon.name == entry.name && icon.hwnd &&
                        IsWindow(icon.hwnd)) {
                        p.hwnd = icon.hwnd;
                        p.id = icon.id;
                        break;
                    }
                }
            }
            auto saved = state->dynamicIconSavedPos.find(entry.name);
            if (saved != state->dynamicIconSavedPos.end()) {
                const auto cell = cellOf(saved->second);
                // Only honor a remembered position if its cell is still
                // free. A stale saved position (e.g. from a deleted folder
                // of the same name) must not drop the new icon on top of a
                // system icon — it flows to a free cell instead.
                if (!occupied.count(cell)) {
                    p.pos = saved->second;
                    p.placed = true;
                    occupied.insert(cell);
                }
            }
            placements.push_back(std::move(p));
        }
    }

    // Destroy windows of entries that no longer exist on disk.
    for (const auto& icon : oldIcons) {
        bool reused = false;
        for (const auto& p : placements) {
            if (p.hwnd == icon.hwnd) { reused = true; break; }
        }
        if (!reused && icon.hwnd && IsWindow(icon.hwnd)) {
            DestroyWindow(icon.hwnd);
        }
    }

    // Second pass: new entries take the first free grid cell, scanning
    // column-major (down each column, then the next) — so a new folder
    // lands in the first empty slot and never on top of a system icon.
    for (auto& p : placements) {
        if (p.placed) continue;
        std::pair<int, int> cell = {0, 0};
        for (int col = 0; ; ++col) {
            bool found = false;
            for (int row = 0; row <= maxRow; ++row) {
                if (!occupied.count({col, row})) {
                    cell = {col, row};
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        p.pos = {28 + cell.first * layoutInfo.gapX,
                 34 + cell.second * layoutInfo.gapY};
        occupied.insert(cell);
    }

    for (auto& p : placements) {
        if (!p.hwnd) {
            p.id = nextId++;
            p.hwnd = createDesktopButton(window, p.id, p.name.c_str(),
                                         p.pos.x, p.pos.y,
                                         layoutInfo.width, layoutInfo.height);
        }

        DesktopState::DynamicIcon di;
        di.hwnd = p.hwnd;
        di.name = p.name;
        di.isDir = p.isDir;
        di.pos = p.pos;
        di.id = p.id;
        state->dynamicIcons.push_back(di);
    }

    // Newly created icon buttons land on top of the z-order — keep the
    // agents panel above them.
    if (state->agentPanel && IsWindow(state->agentPanel)) {
        SetWindowPos(state->agentPanel, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (state->agentHandle && IsWindow(state->agentHandle)) {
        SetWindowPos(state->agentHandle, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Repaint the desktop background; children keep their own paint state,
    // so untouched icons don't flash.
    RECT r_rect;
    GetClientRect(window, &r_rect);
    r_rect.bottom -= kTaskbarReserved; // exclude dock + search strip
    InvalidateRect(window, &r_rect, TRUE);

    // The dock mirrors the desktop folders — refresh it too.
    RECT dockRect;
    GetClientRect(window, &dockRect);
    dockRect.top = dockRect.bottom - kDockH;
    InvalidateRect(window, &dockRect, FALSE);
}

// GDI+ helper: build a rounded-rectangle GraphicsPath
inline void addRoundRect(Gdiplus::GraphicsPath& path, int x, int y, int w, int h, int r)
{
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    path.AddArc(x,         y,         r*2, r*2, 180, 90);
    path.AddArc(x + w-r*2, y,         r*2, r*2, 270, 90);
    path.AddArc(x + w-r*2, y + h-r*2, r*2, r*2,   0, 90);
    path.AddArc(x,         y + h-r*2, r*2, r*2,  90, 90);
    path.CloseFigure();
}

// Windows-style rubber-band selection: translucent blue fill with a solid
// border, in the same accent color as the icon selection highlight. Called
// from the desktop WM_PAINT and from drawDesktopIcon so the band is always
// composited inside a double-buffered pass (drawing it straight to the
// screen made it flicker on every repaint underneath).
void drawSelectionBand(Gdiplus::Graphics& g, const RECT& sel)
{
    Gdiplus::SolidBrush fill(Gdiplus::Color(45, 90, 160, 255));
    g.FillRectangle(&fill, static_cast<INT>(sel.left), sel.top,
                    sel.right - sel.left, sel.bottom - sel.top);
    Gdiplus::Pen border(Gdiplus::Color(220, 90, 160, 255), 1.0f);
    g.DrawRectangle(&border, static_cast<INT>(sel.left), sel.top,
                    sel.right - sel.left - 1, sel.bottom - sel.top - 1);
}

// ── Port-themed popup menus ──────────────────────────────────────────
// Every item is switched to MFT_OWNERDRAW so colors and padding are ours;
// command IDs and TrackPopupMenu behavior stay exactly as before.
struct PortMenuItem {
    std::string text;
    bool separator = false;
};

HFONT portMenuFont()
{
    static HFONT font = nullptr;
    if (!font) {
        NONCLIENTMETRICSA ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            font = CreateFontIndirectA(&ncm.lfMenuFont);
        }
        if (!font) font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    return font;
}

void themePortMenu(HMENU menu, std::vector<std::unique_ptr<PortMenuItem>>& store)
{
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        char text[256] = {};
        MENUITEMINFOA info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU;
        info.dwTypeData = text;
        info.cch = sizeof(text) - 1;
        if (!GetMenuItemInfoA(menu, static_cast<UINT>(i), TRUE, &info)) {
            continue;
        }
        if (info.hSubMenu) themePortMenu(info.hSubMenu, store);

        auto data = std::make_unique<PortMenuItem>();
        data->separator = (info.fType & MFT_SEPARATOR) != 0;
        data->text = text;

        MENUITEMINFOA mod{};
        mod.cbSize = sizeof(mod);
        mod.fMask = MIIM_FTYPE | MIIM_DATA;
        mod.fType = info.fType | MFT_OWNERDRAW;
        mod.dwItemData = reinterpret_cast<ULONG_PTR>(data.get());
        SetMenuItemInfoA(menu, static_cast<UINT>(i), TRUE, &mod);
        store.push_back(std::move(data));
    }

    // Dark strip behind and between the items (separator gaps included)
    static HBRUSH menuBackground = CreateSolidBrush(RGB(16, 22, 38));
    MENUINFO mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = menuBackground;
    SetMenuInfo(menu, &mi);
}

bool measurePortMenuItem(MEASUREITEMSTRUCT* mis)
{
    if (!mis || mis->CtlType != ODT_MENU || !mis->itemData) return false;
    const auto* data = reinterpret_cast<const PortMenuItem*>(mis->itemData);
    if (data->separator) {
        mis->itemWidth  = 90;
        mis->itemHeight = 9;
        return true;
    }
    HDC dc = GetDC(nullptr);
    HGDIOBJ oldFont = SelectObject(dc, portMenuFont());
    SIZE sz = {};
    GetTextExtentPoint32A(dc, data->text.c_str(),
                          static_cast<int>(data->text.size()), &sz);
    SelectObject(dc, oldFont);
    ReleaseDC(nullptr, dc);
    mis->itemWidth  = static_cast<UINT>(sz.cx) + 44; // roomier than stock
    mis->itemHeight = 30;
    return true;
}

bool drawPortMenuItem(const DRAWITEMSTRUCT* dis)
{
    if (!dis || dis->CtlType != ODT_MENU || !dis->itemData) return false;
    const auto* data = reinterpret_cast<const PortMenuItem*>(dis->itemData);
    HDC dc = dis->hDC;
    const RECT rc = dis->rcItem;

    HBRUSH bg = CreateSolidBrush(RGB(16, 22, 38));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    if (data->separator) {
        RECT line = {rc.left + 10, (rc.top + rc.bottom) / 2,
                     rc.right - 10, (rc.top + rc.bottom) / 2 + 1};
        HBRUSH lineBrush = CreateSolidBrush(RGB(48, 58, 86));
        FillRect(dc, &line, lineBrush);
        DeleteObject(lineBrush);
        return true;
    }

    const bool disabled = (dis->itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
    const bool hot = (dis->itemState & ODS_SELECTED) != 0 && !disabled;

    if (hot) {
        // Same accent as the icon selection and the rubber-band
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath path;
        addRoundRect(path, rc.left + 4, rc.top + 2,
                     (rc.right - rc.left) - 8, (rc.bottom - rc.top) - 4, 6);
        Gdiplus::SolidBrush fill(Gdiplus::Color(70, 90, 160, 255));
        g.FillPath(&fill, &path);
        Gdiplus::Pen border(Gdiplus::Color(150, 90, 160, 255), 1.0f);
        g.DrawPath(&border, &path);
    }

    if (dis->itemState & ODS_CHECKED) {
        // Accent dot marking the active choice (e.g. the current icon size)
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush dot(Gdiplus::Color(255, 90, 160, 255));
        const int cy = (rc.top + rc.bottom) / 2;
        g.FillEllipse(&dot, rc.left + 11, cy - 3, 6, 6);
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? RGB(110, 118, 140) : RGB(220, 225, 238));
    HGDIOBJ oldFont = SelectObject(dc, portMenuFont());
    RECT textRect = rc;
    textRect.left += 26;
    textRect.right -= 12;
    DrawTextA(dc, data->text.c_str(), -1, &textRect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_HIDEPREFIX);
    SelectObject(dc, oldFont);
    return true;
}

void fillRoundRect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border)
{
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH brush = CreateSolidBrush(fill);
    SelectObject(dc, pen);
    SelectObject(dc, brush);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    DeleteObject(pen);
    DeleteObject(brush);
}

// ── Asset loading ────────────────────────────────────────────────────
// Every bundled image lives under assets/. The exe points its CWD at the
// project root, and the extra prefixes keep the app working when it is
// started straight from a build directory.
Gdiplus::Image* loadAsset(const std::wstring& relativePath)
{
    static const wchar_t* const prefixes[] = { L"", L"../", L"../../" };
    for (const wchar_t* prefix : prefixes) {
        Gdiplus::Image* img = Gdiplus::Image::FromFile((prefix + relativePath).c_str());
        if (img && img->GetLastStatus() == Gdiplus::Ok) return img;
        delete img;
    }
    return nullptr;
}

// ── PORT AGENTS side panel ───────────────────────────────────────────
constexpr int AgentPanelWidth   = 264;
constexpr int AgentPanelHeaderH = 46;
constexpr int AgentRowH         = 74;

// Per-agent icon: "agent-<name>.png" (lowercase, spaces stripped) under
// assets/agents — e.g. agent-nova.png. Missing files fall back to the
// drawn disc with the agent's initial.
Gdiplus::Image* loadAgentImage(const std::string& name)
{
    static std::map<std::string, Gdiplus::Image*> cache;
    const auto found = cache.find(name);
    if (found != cache.end()) return found->second;

    std::string file = "agent-";
    for (const char ch : name) {
        if (ch == ' ') continue;
        file += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    file += ".png";

    const std::wstring wide(file.begin(), file.end());
    Gdiplus::Image* img = loadAsset(L"assets/agents/" + wide);
    cache[name] = img;
    return img;
}

LRESULT CALLBACK agentPanelProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopState* state =
        reinterpret_cast<DesktopState*>(GetWindowLongPtrA(hw, GWLP_USERDATA));

    static HFONT headerFont = CreateFontA(-12, 0, 0, 0, FW_SEMIBOLD, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    static HFONT nameFont = CreateFontA(-15, 0, 0, 0, FW_SEMIBOLD, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    static HFONT tagFont = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC screen = BeginPaint(hw, &ps);
        RECT rc;
        GetClientRect(hw, &rc);
        const int w = rc.right;
        const int h = rc.bottom;
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);

        HBRUSH bg = CreateSolidBrush(RGB(13, 17, 31));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        {
            Gdiplus::GraphicsPath borderPath;
            addRoundRect(borderPath, 0, 0, w - 1, h - 1, 12);
            Gdiplus::Pen borderPen(Gdiplus::Color(255, 44, 54, 84), 1.0f);
            g.DrawPath(&borderPen, &borderPath);
        }

        SetBkMode(dc, TRANSPARENT);

        // Header
        HGDIOBJ oldFont = SelectObject(dc, headerFont);
        SetTextColor(dc, RGB(140, 150, 175));
        RECT headerRect = {20, 0, w - 48, AgentPanelHeaderH};
        DrawTextA(dc, "P O R T   A G E N T S", -1, &headerRect,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

        // Close (✕)
        RECT closeRect = {w - 40, 11, w - 16, 35};
        if (state && state->agentHotRow == -2) {
            Gdiplus::GraphicsPath closeBg;
            addRoundRect(closeBg, closeRect.left, closeRect.top, 24, 24, 6);
            Gdiplus::SolidBrush closeFill(Gdiplus::Color(40, 255, 255, 255));
            g.FillPath(&closeFill, &closeBg);
        }
        {
            Gdiplus::Pen xPen(Gdiplus::Color(255, 168, 178, 202), 1.6f);
            g.DrawLine(&xPen, static_cast<INT>(closeRect.left + 7),
                       static_cast<INT>(closeRect.top + 7),
                       static_cast<INT>(closeRect.right - 7),
                       static_cast<INT>(closeRect.bottom - 7));
            g.DrawLine(&xPen, static_cast<INT>(closeRect.left + 7),
                       static_cast<INT>(closeRect.bottom - 7),
                       static_cast<INT>(closeRect.right - 7),
                       static_cast<INT>(closeRect.top + 7));
        }

        RECT headerLine = {0, AgentPanelHeaderH - 1, w, AgentPanelHeaderH};
        HBRUSH lineBrush = CreateSolidBrush(RGB(34, 42, 66));
        FillRect(dc, &headerLine, lineBrush);
        DeleteObject(lineBrush);

        // Agent rows — scrolled by agentScroll and clipped below the header
        if (state) {
            const int contentH =
                8 + static_cast<int>(state->agents.size()) * AgentRowH + 8;
            const int viewH = h - AgentPanelHeaderH;
            const int maxScroll = std::max(0, contentH - viewH);
            if (state->agentScroll > maxScroll) state->agentScroll = maxScroll;
            if (state->agentScroll < 0) state->agentScroll = 0;

            const int savedDc = SaveDC(dc);
            IntersectClipRect(dc, 0, AgentPanelHeaderH, w, h);
            g.SetClip(Gdiplus::Rect(0, AgentPanelHeaderH, w,
                                    h - AgentPanelHeaderH));

            int y = AgentPanelHeaderH + 8 - state->agentScroll;
            for (std::size_t i = 0; i < state->agents.size(); ++i) {
                const auto& agent = state->agents[i];
                const RECT row = {8, y, w - 8, y + AgentRowH - 6};
                const bool isActive = static_cast<int>(i) == state->activeAgent;
                const bool isHot = static_cast<int>(i) == state->agentHotRow;

                if (isActive || isHot) {
                    Gdiplus::GraphicsPath rowPath;
                    addRoundRect(rowPath, row.left, row.top,
                                 row.right - row.left, row.bottom - row.top, 10);
                    Gdiplus::SolidBrush rowFill(
                        isActive ? Gdiplus::Color(48, 90, 160, 255)
                                 : Gdiplus::Color(22, 255, 255, 255));
                    g.FillPath(&rowFill, &rowPath);
                    if (isActive) {
                        Gdiplus::Pen rowPen(Gdiplus::Color(110, 90, 160, 255), 1.0f);
                        g.DrawPath(&rowPen, &rowPath);
                    }
                }

                const BYTE ar = GetRValue(agent.accent);
                const BYTE ag2 = GetGValue(agent.accent);
                const BYTE ab = GetBValue(agent.accent);

                // Icon disc: the agent's own circular image if present,
                // otherwise dark fill + accent ring + accent initial
                const int discX = row.left + 10;
                const int discY = y + 15;
                const int discS = 38;
                Gdiplus::Image* agentImg = loadAgentImage(agent.name);
                if (agentImg) {
                    g.DrawImage(agentImg, discX, discY, discS, discS);
                    Gdiplus::Pen discRing(Gdiplus::Color(200, ar, ag2, ab), 1.6f);
                    g.DrawEllipse(&discRing, discX + 1, discY + 1,
                                  discS - 2, discS - 2);
                } else {
                    Gdiplus::SolidBrush discBg(Gdiplus::Color(255, 24, 30, 50));
                    g.FillEllipse(&discBg, discX, discY, discS, discS);
                    Gdiplus::Pen discRing(Gdiplus::Color(230, ar, ag2, ab), 2.0f);
                    g.DrawEllipse(&discRing, discX + 1, discY + 1,
                                  discS - 2, discS - 2);
                    SelectObject(dc, nameFont);
                    SetTextColor(dc, agent.accent);
                    const char initial[2] = {agent.name.empty() ? '?'
                                                                : agent.name[0], 0};
                    RECT initialRect = {discX, discY, discX + discS, discY + discS};
                    DrawTextA(dc, initial, -1, &initialRect,
                              DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                }

                // Name + tagline (two full lines fit below the name)
                SetTextColor(dc, RGB(228, 233, 245));
                RECT nameRect = {discX + discS + 12, y + 9, row.right - 26,
                                 y + 27};
                DrawTextA(dc, agent.name.c_str(), -1, &nameRect,
                          DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
                SelectObject(dc, tagFont);
                SetTextColor(dc, RGB(128, 138, 162));
                RECT tagRect = {discX + discS + 12, y + 28, row.right - 26,
                                y + 64};
                DrawTextA(dc, agent.tagline.c_str(), -1, &tagRect,
                          DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS |
                          DT_NOPREFIX);

                // Status dot
                Gdiplus::SolidBrush dot(Gdiplus::Color(255, ar, ag2, ab));
                g.FillEllipse(&dot, row.right - 20, y + 31, 8, 8);

                y += AgentRowH;
            }

            g.ResetClip();
            RestoreDC(dc, savedDc);

            // Thin scroll indicator on the right edge
            if (maxScroll > 0) {
                const int trackTop = AgentPanelHeaderH + 4;
                const int trackH = viewH - 8;
                const int thumbH =
                    std::max(24, trackH * viewH / contentH);
                const int thumbY = trackTop +
                    (trackH - thumbH) * state->agentScroll / maxScroll;
                Gdiplus::GraphicsPath thumb;
                addRoundRect(thumb, w - 6, thumbY, 3, thumbH, 1);
                Gdiplus::SolidBrush thumbFill(
                    Gdiplus::Color(140, 110, 130, 180));
                g.FillPath(&thumbFill, &thumb);
            }
        }

        SelectObject(dc, oldFont);
        BitBlt(screen, 0, 0, w, h, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!state) break;
        POINT pt = {static_cast<short>(LOWORD(lp)),
                    static_cast<short>(HIWORD(lp))};
        RECT rc;
        GetClientRect(hw, &rc);
        const RECT closeRect = {rc.right - 40, 11, rc.right - 16, 35};
        int hot = -1;
        if (PtInRect(&closeRect, pt)) {
            hot = -2;
        } else if (pt.y >= AgentPanelHeaderH && pt.x >= 8 &&
                   pt.x <= rc.right - 8) {
            const int idx = (pt.y - AgentPanelHeaderH - 8 +
                             state->agentScroll) / AgentRowH;
            if (idx >= 0 && idx < static_cast<int>(state->agents.size())) {
                hot = idx;
            }
        }
        if (hot != state->agentHotRow) {
            state->agentHotRow = hot;
            InvalidateRect(hw, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hw, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (state && state->agentHotRow != -1) {
            state->agentHotRow = -1;
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        if (!state) break;
        RECT rc;
        GetClientRect(hw, &rc);
        const int contentH =
            8 + static_cast<int>(state->agents.size()) * AgentRowH + 8;
        const int maxScroll =
            std::max(0, contentH - (static_cast<int>(rc.bottom) -
                                    AgentPanelHeaderH));
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int scroll = state->agentScroll - (delta / WHEEL_DELTA) * 40;
        if (scroll < 0) scroll = 0;
        if (scroll > maxScroll) scroll = maxScroll;
        if (scroll != state->agentScroll) {
            state->agentScroll = scroll;
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!state) break;
        POINT pt = {static_cast<short>(LOWORD(lp)),
                    static_cast<short>(HIWORD(lp))};
        RECT rc;
        GetClientRect(hw, &rc);
        const RECT closeRect = {rc.right - 40, 11, rc.right - 16, 35};
        if (PtInRect(&closeRect, pt)) {
            state->agentPanelVisible = false;
            ShowWindow(hw, SW_HIDE);
            if (state->agentHandle) ShowWindow(state->agentHandle, SW_SHOWNA);
            return 0;
        }
        if (pt.y >= AgentPanelHeaderH) {
            const int idx = (pt.y - AgentPanelHeaderH - 8 +
                             state->agentScroll) / AgentRowH;
            if (idx >= 0 && idx < static_cast<int>(state->agents.size()) &&
                idx != state->activeAgent) {
                state->activeAgent = idx;
                InvalidateRect(hw, nullptr, FALSE);
                // Refresh the agent badge in the taskbar
                HWND parent = GetParent(hw);
                if (parent) {
                    RECT pr;
                    GetClientRect(parent, &pr);
                    pr.top = pr.bottom - kTaskbarReserved;
                    InvalidateRect(parent, &pr, FALSE);
                }
            }
        }
        return 0;
    }
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

// ── "Agents" dock folder popup: grid of agent medallions ────────────
constexpr int AgentGridCols  = 5;
constexpr int AgentGridCellW = 88;
constexpr int AgentGridCellH = 104;
constexpr int AgentGridPad   = 14;

SIZE agentGridSize(const DesktopState* state)
{
    int count = state ? static_cast<int>(state->agents.size()) : 0;
    int rows = (count + AgentGridCols - 1) / AgentGridCols;
    if (rows < 1) rows = 1;
    return {AgentGridCols * AgentGridCellW + AgentGridPad * 2,
            rows * AgentGridCellH + AgentGridPad * 2};
}

void openAgentWindow(HWND desktopWnd, DesktopState* state, int agentIndex);

void hideAgentGrid(DesktopState* state)
{
    if (!state) return;
    state->agentGridVisible = false;
    state->agentGridHot = -1;
    if (state->agentGrid && IsWindow(state->agentGrid)) {
        ShowWindow(state->agentGrid, SW_HIDE);
    }
}

LRESULT CALLBACK agentGridProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopState* state =
        reinterpret_cast<DesktopState*>(GetWindowLongPtrA(hw, GWLP_USERDATA));

    static HFONT nameFont = CreateFontA(-12, 0, 0, 0, FW_SEMIBOLD, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    static HFONT bigFont = CreateFontA(-18, 0, 0, 0, FW_SEMIBOLD, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC screen = BeginPaint(hw, &ps);
        RECT rc;
        GetClientRect(hw, &rc);
        const int w = rc.right;
        const int h = rc.bottom;
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);

        HBRUSH bg = CreateSolidBrush(RGB(13, 17, 31));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        {
            Gdiplus::GraphicsPath borderPath;
            addRoundRect(borderPath, 0, 0, w - 1, h - 1, 14);
            Gdiplus::Pen borderPen(Gdiplus::Color(255, 44, 54, 84), 1.0f);
            g.DrawPath(&borderPen, &borderPath);
        }

        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(dc, nameFont);

        if (state) {
            for (std::size_t i = 0; i < state->agents.size(); ++i) {
                const auto& agent = state->agents[i];
                const int col = static_cast<int>(i) % AgentGridCols;
                const int row = static_cast<int>(i) / AgentGridCols;
                const int cellX = AgentGridPad + col * AgentGridCellW;
                const int cellY = AgentGridPad + row * AgentGridCellH;

                const bool isHot = static_cast<int>(i) == state->agentGridHot;
                const bool isActive =
                    static_cast<int>(i) == state->activeAgent;

                if (isHot) {
                    Gdiplus::GraphicsPath cellPath;
                    addRoundRect(cellPath, cellX + 2, cellY + 2,
                                 AgentGridCellW - 4, AgentGridCellH - 4, 10);
                    Gdiplus::SolidBrush cellFill(
                        Gdiplus::Color(26, 255, 255, 255));
                    g.FillPath(&cellFill, &cellPath);
                }

                const BYTE ar = GetRValue(agent.accent);
                const BYTE ag2 = GetGValue(agent.accent);
                const BYTE ab = GetBValue(agent.accent);

                const int imgS = 56;
                const int imgX = cellX + (AgentGridCellW - imgS) / 2;
                const int imgY = cellY + 8;
                Gdiplus::Image* img = loadAgentImage(agent.name);
                if (img) {
                    g.DrawImage(img, imgX, imgY, imgS, imgS);
                } else {
                    Gdiplus::SolidBrush discBg(Gdiplus::Color(255, 24, 30, 50));
                    g.FillEllipse(&discBg, imgX, imgY, imgS, imgS);
                    Gdiplus::Pen ring(Gdiplus::Color(230, ar, ag2, ab), 2.0f);
                    g.DrawEllipse(&ring, imgX + 1, imgY + 1,
                                  imgS - 2, imgS - 2);
                    SelectObject(dc, bigFont);
                    SetTextColor(dc, agent.accent);
                    const char initial[2] = {agent.name.empty() ? '?'
                                                                : agent.name[0],
                                             0};
                    RECT ir = {imgX, imgY, imgX + imgS, imgY + imgS};
                    DrawTextA(dc, initial, -1, &ir,
                              DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                }
                if (isActive) {
                    Gdiplus::Pen activeRing(Gdiplus::Color(220, ar, ag2, ab),
                                            2.0f);
                    g.DrawEllipse(&activeRing, imgX - 3, imgY - 3,
                                  imgS + 6, imgS + 6);
                }

                SelectObject(dc, nameFont);
                SetTextColor(dc, isActive ? agent.accent
                                          : RGB(215, 222, 238));
                RECT nameRect = {cellX + 2, imgY + imgS + 6,
                                 cellX + AgentGridCellW - 2,
                                 cellY + AgentGridCellH - 4};
                DrawTextA(dc, agent.name.c_str(), -1, &nameRect,
                          DT_SINGLELINE | DT_CENTER | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
            }
        }

        SelectObject(dc, oldFont);
        BitBlt(screen, 0, 0, w, h, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!state) break;
        POINT pt = {static_cast<short>(LOWORD(lp)),
                    static_cast<short>(HIWORD(lp))};
        int hot = -1;
        const int col = (pt.x - AgentGridPad) / AgentGridCellW;
        const int row = (pt.y - AgentGridPad) / AgentGridCellH;
        if (pt.x >= AgentGridPad && pt.y >= AgentGridPad &&
            col >= 0 && col < AgentGridCols && row >= 0) {
            const int idx = row * AgentGridCols + col;
            if (idx < static_cast<int>(state->agents.size())) hot = idx;
        }
        if (hot != state->agentGridHot) {
            state->agentGridHot = hot;
            InvalidateRect(hw, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hw, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (state && state->agentGridHot != -1) {
            state->agentGridHot = -1;
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        if (!state) break;
        if (state->agentGridHot >= 0 &&
            state->agentGridHot < static_cast<int>(state->agents.size())) {
            const int idx = state->agentGridHot;
            hideAgentGrid(state);
            openAgentWindow(GetParent(hw), state, idx);
        }
        return 0;
    }
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

// ── Agent app window: a per-agent chat session ──────────────────────
struct AgentSession {
    std::string title = "New chat";
    std::vector<ChatEntry> history;
    int scroll = 0;
    bool pinned = true;
    bool started = false; // a user message has been sent
    std::chrono::system_clock::time_point created =
        std::chrono::system_clock::now();
    std::chrono::system_clock::time_point lastActivity =
        std::chrono::system_clock::now();
};

struct AgentCardHit { RECT rect; std::string prompt; };

struct AgentWindowState {
    DesktopState* desktop = nullptr;
    int agentIndex = -1;
    std::vector<AgentSession> sessions;
    int activeSession = 0;
    int pendingSession = 0; // session the in-flight request belongs to
    int contentH = 0;       // measured chat content height (set in paint)
    bool waiting = false;   // this window has a request in flight
    HWND input = nullptr;

    // Agent Mode settings — actually change the AI request
    int mode = 0;      // 0 Balanced, 1 Precise, 2 Creative
    int reasoning = 1; // 0 Concise, 1 Standard, 2 Deep
    int style = 1;     // 0 Brief, 1 Detailed

    // Clickable regions, refreshed on every paint
    RECT newSessionRect{};
    std::vector<RECT> sessionRects;
    std::vector<AgentCardHit> cardRects;
    RECT modeChips[3]{}; // Mode / Reasoning / Style chips
    RECT wsRects[5]{};   // Workspace nav items
    int hotSession = -1;
    bool hotNewSession = false;
    int hotCard = -1;
    int hotWs = -1;

    AgentSession& active() { return sessions[activeSession]; }
};

// Dashboard column widths in the agent window
constexpr int AgentSidebarW = 248;
constexpr int AgentRightW   = 292;
constexpr int AgentMainHeaderH = 88;

struct AgentAsyncResult {
    int agentIndex;
    bool ok;
    bool final;   // false = intermediate tool-activity note
    std::string text;
};

// ── Web tools: plain HTTP fetch + Tavily search ─────────────────────

// Minimal WinInet request. GET when postJson is empty, POST otherwise.
std::string httpFetch(const std::string& url, const std::string& postJson = {})
{
    char host[256] = {};
    char path[2048] = {};
    URL_COMPONENTSA parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = sizeof(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = sizeof(path);
    if (!InternetCrackUrlA(url.c_str(), 0, 0, &parts)) return {};

    HINTERNET net = InternetOpenA("PortOS-Agent/1.0",
                                  INTERNET_OPEN_TYPE_PRECONFIG,
                                  nullptr, nullptr, 0);
    if (!net) return {};
    HINTERNET conn = InternetConnectA(net, host, parts.nPort, nullptr,
                                      nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) {
        InternetCloseHandle(net);
        return {};
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET req = HttpOpenRequestA(
        conn, postJson.empty() ? "GET" : "POST", path, nullptr, nullptr,
        nullptr,
        (secure ? INTERNET_FLAG_SECURE : 0) | INTERNET_FLAG_NO_CACHE_WRITE |
            INTERNET_FLAG_RELOAD,
        0);
    std::string data;
    if (req) {
        const char* headers = "Content-Type: application/json\r\n";
        const BOOL sent = postJson.empty()
            ? HttpSendRequestA(req, nullptr, 0, nullptr, 0)
            : HttpSendRequestA(req, headers,
                               static_cast<DWORD>(std::strlen(headers)),
                               const_cast<char*>(postJson.data()),
                               static_cast<DWORD>(postJson.size()));
        if (sent) {
            char buffer[8192];
            DWORD bytesRead = 0;
            while (InternetReadFile(req, buffer, sizeof(buffer), &bytesRead) &&
                   bytesRead > 0 && data.size() < 300'000) {
                data.append(buffer, bytesRead);
            }
        }
        InternetCloseHandle(req);
    }
    InternetCloseHandle(conn);
    InternetCloseHandle(net);
    return data;
}

// Crude but effective HTML → readable text: drops script/style blocks and
// tags, decodes common entities, collapses whitespace.
std::string htmlToText(const std::string& html)
{
    std::string lower = html;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string cleaned;
    cleaned.reserve(html.size());
    std::size_t i = 0;
    while (i < html.size()) {
        if (lower.compare(i, 7, "<script") == 0) {
            const std::size_t end = lower.find("</script>", i);
            i = (end == std::string::npos) ? html.size() : end + 9;
            continue;
        }
        if (lower.compare(i, 6, "<style") == 0) {
            const std::size_t end = lower.find("</style>", i);
            i = (end == std::string::npos) ? html.size() : end + 8;
            continue;
        }
        if (html[i] == '<') {
            const std::size_t end = html.find('>', i);
            if (end == std::string::npos) break;
            i = end + 1;
            cleaned += ' ';
            continue;
        }
        if (html[i] == '&') {
            struct Entity { const char* name; char ch; };
            static constexpr Entity entities[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
                {"&quot;", '"'}, {"&#39;", '\''}, {"&nbsp;", ' '},
            };
            bool matched = false;
            for (const auto& entity : entities) {
                const std::size_t len = std::strlen(entity.name);
                if (html.compare(i, len, entity.name) == 0) {
                    cleaned += entity.ch;
                    i += len;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }
        cleaned += html[i];
        ++i;
    }

    // Collapse whitespace runs
    std::string out;
    out.reserve(cleaned.size());
    bool inSpace = false;
    for (const char c : cleaned) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!inSpace && !out.empty()) out += ' ';
            inSpace = true;
        } else {
            out += c;
            inSpace = false;
        }
    }
    return out;
}

std::string jsonEscapeText(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) >= 0x20) out += c;
        }
    }
    return out;
}

// Pulls the string value of "key":"..." starting at from; handles \" escapes.
std::string extractJsonString(const std::string& src, const std::string& key,
                              std::size_t from = 0)
{
    const std::string needle = "\"" + key + "\":\"";
    const std::size_t start = src.find(needle, from);
    if (start == std::string::npos) return {};
    std::string out;
    for (std::size_t i = start + needle.size(); i < src.size(); ++i) {
        if (src[i] == '\\' && i + 1 < src.size()) {
            const char next = src[i + 1];
            if (next == 'n') out += ' ';
            else if (next == 't') out += ' ';
            else if (next != 'r' && next != 'u') out += next;
            ++i;
            continue;
        }
        if (src[i] == '"') break;
        out += src[i];
    }
    return out;
}

// Tavily search — free API key from tavily.com; read from env/key file.
std::string tavilySearch(const std::string& query)
{
    const std::string key = loadApiKey("TAVILY_API_KEY", "tavily_key.txt");
    if (key.empty()) {
        return "Error: web search is not configured. Get a free API key at "
               "tavily.com and save it in tavily_key.txt (or the "
               "TAVILY_API_KEY environment variable).";
    }
    const std::string body =
        "{\"api_key\":\"" + jsonEscapeText(key) + "\",\"query\":\"" +
        jsonEscapeText(query) +
        "\",\"max_results\":5,\"include_answer\":true}";
    const std::string response =
        httpFetch("https://api.tavily.com/search", body);
    if (response.empty()) return "Error: web search request failed.";

    std::ostringstream out;
    const std::string answer = extractJsonString(response, "answer");
    if (!answer.empty()) out << "Answer summary: " << answer << "\n\n";

    int count = 0;
    std::size_t pos = 0;
    while (count < 5) {
        const std::size_t hit = response.find("\"title\":\"", pos);
        if (hit == std::string::npos) break;
        const std::string title = extractJsonString(response, "title", hit);
        const std::string url = extractJsonString(response, "url", hit);
        const std::string content =
            extractJsonString(response, "content", hit);
        ++count;
        out << count << ". " << title << "\n   " << url << "\n   "
            << (content.size() > 300 ? content.substr(0, 300) + "..."
                                     : content)
            << "\n";
        pos = hit + 9;
    }
    if (count == 0 && answer.empty()) {
        return "Web search returned no results.";
    }
    return out.str();
}

std::string urlEncode(const std::string& text)
{
    std::string out;
    out.reserve(text.size() * 3);
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Content of the first <tag>...</tag> inside src, whitespace collapsed.
std::string extractXmlTag(const std::string& src, const char* tag)
{
    const std::string open = "<" + std::string(tag);
    const std::string close = "</" + std::string(tag) + ">";
    std::size_t start = src.find(open);
    if (start == std::string::npos) return {};
    start = src.find('>', start);
    if (start == std::string::npos) return {};
    const std::size_t end = src.find(close, start);
    if (end == std::string::npos) return {};

    std::string out;
    bool inSpace = false;
    for (std::size_t i = start + 1; i < end; ++i) {
        const char c = src[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!inSpace && !out.empty()) out += ' ';
            inSpace = true;
        } else {
            out += c;
            inSpace = false;
        }
    }
    return out;
}

// arXiv paper search (free Atom API, no key) — Aethro's specialist tool.
std::string arxivSearch(const std::string& query)
{
    const std::string url =
        "https://export.arxiv.org/api/query?search_query=all:" +
        urlEncode(query) + "&max_results=5&sortBy=relevance";
    const std::string xml = httpFetch(url);
    if (xml.empty()) return "Error: arXiv request failed.";

    std::ostringstream out;
    int count = 0;
    std::size_t pos = xml.find("<entry>");
    while (pos != std::string::npos && count < 5) {
        const std::size_t end = xml.find("</entry>", pos);
        if (end == std::string::npos) break;
        const std::string entry = xml.substr(pos, end - pos);

        const std::string title = extractXmlTag(entry, "title");
        const std::string link = extractXmlTag(entry, "id");
        std::string summary = extractXmlTag(entry, "summary");
        if (summary.size() > 300) summary = summary.substr(0, 300) + "...";

        ++count;
        out << count << ". " << title << "\n   " << link << "\n   "
            << summary << "\n";
        pos = xml.find("<entry>", end);
    }
    if (count == 0) return "arXiv returned no results for that query.";
    return out.str();
}

// NVD CVE keyword search (free, no key) — Zynx's specialist tool.
std::string cveSearch(const std::string& query)
{
    const std::string url =
        "https://services.nvd.nist.gov/rest/json/cves/2.0?keywordSearch=" +
        urlEncode(query) + "&resultsPerPage=5";
    const std::string json = httpFetch(url);
    if (json.empty()) return "Error: NVD request failed (rate limit is ~5 "
                             "requests per 30s without an API key).";

    std::ostringstream out;
    int count = 0;
    std::size_t pos = 0;
    while (count < 5) {
        const std::size_t hit = json.find("\"id\":\"CVE-", pos);
        if (hit == std::string::npos) break;
        const std::string id = extractJsonString(json, "id", hit);
        std::string description = extractJsonString(json, "value", hit);
        if (description.size() > 300) {
            description = description.substr(0, 300) + "...";
        }
        std::string score;
        const std::size_t scorePos = json.find("\"baseScore\":", hit);
        if (scorePos != std::string::npos) {
            std::size_t i = scorePos + 12;
            while (i < json.size() &&
                   (std::isdigit(static_cast<unsigned char>(json[i])) ||
                    json[i] == '.')) {
                score += json[i++];
            }
        }
        ++count;
        out << count << ". " << id;
        if (!score.empty()) out << " (CVSS " << score << ")";
        out << "\n   " << description << "\n";
        pos = hit + 10;
    }
    if (count == 0) return "NVD returned no CVEs for that keyword.";
    return out.str();
}

// Extract a "TOOL: fs ..." request from a model reply. Returns the tool
// command ("fs list ..." / "fs read ...") or empty if the reply is normal.
std::string parseAgentToolCall(const std::string& reply)
{
    std::istringstream stream{reply};
    std::string line;
    while (std::getline(stream, line)) {
        // Trim whitespace and stray backticks
        const auto notSpace = [](unsigned char c) {
            return !std::isspace(c) && c != '`';
        };
        while (!line.empty() && !notSpace(line.front())) line.erase(line.begin());
        while (!line.empty() && !notSpace(line.back())) line.pop_back();
        if (line.rfind("TOOL:", 0) != 0) continue;

        std::string cmd = line.substr(5);
        const std::size_t start = cmd.find_first_not_of(' ');
        if (start == std::string::npos) return {};
        cmd = cmd.substr(start);
        if (cmd.rfind("fs list", 0) == 0 || cmd.rfind("fs read", 0) == 0 ||
            cmd.rfind("web search", 0) == 0 ||
            cmd.rfind("web fetch", 0) == 0 ||
            cmd.rfind("arxiv search", 0) == 0 ||
            cmd.rfind("cve search", 0) == 0) {
            return cmd;
        }
        return {};
    }
    return {};
}

// Execute a read-only sandbox tool for an agent (runs on a worker thread;
// Sandbox::list/read are const filesystem reads).
std::string runAgentTool(port::security::Sandbox& sandbox,
                         const std::string& cmd)
{
    const auto argAfter = [&](std::size_t prefixLen) {
        std::string arg = cmd.size() > prefixLen ? cmd.substr(prefixLen)
                                                 : std::string{};
        const std::size_t start = arg.find_first_not_of(' ');
        if (start == std::string::npos) return std::string{};
        std::size_t end = arg.find_last_not_of(' ');
        return arg.substr(start, end - start + 1);
    };

    if (cmd.rfind("fs list", 0) == 0) {
        const std::string path = argAfter(7);
        auto res = sandbox.list(path);
        if (!res) return "Error: " + res.error().detail;
        std::ostringstream out;
        out << "sandbox/" << path << ":\n";
        for (const auto& entry : *res) {
            out << "  " << entry.name << (entry.isDirectory ? "/" : "")
                << '\n';
        }
        if (res->empty()) out << "  (empty)\n";
        return out.str();
    }
    if (cmd.rfind("fs read", 0) == 0) {
        const std::string path = argAfter(7);
        if (path.empty()) return "Error: fs read requires a file path.";
        auto res = sandbox.read(path);
        if (!res) return "Error: " + res.error().detail;
        constexpr std::size_t maxLen = 4000;
        if (res->size() > maxLen) {
            return res->substr(0, maxLen) + "\n...(truncated)";
        }
        return *res;
    }
    if (cmd.rfind("web search", 0) == 0) {
        const std::string query = argAfter(10);
        if (query.empty()) return "Error: web search requires a query.";
        return tavilySearch(query);
    }
    if (cmd.rfind("web fetch", 0) == 0) {
        const std::string url = argAfter(9);
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
            return "Error: web fetch requires an http(s) URL.";
        }
        const std::string html = httpFetch(url);
        if (html.empty()) return "Error: could not fetch " + url;
        std::string text = htmlToText(html);
        constexpr std::size_t maxLen = 4000;
        if (text.size() > maxLen) {
            text = text.substr(0, maxLen) + "\n...(truncated)";
        }
        return text;
    }
    if (cmd.rfind("arxiv search", 0) == 0) {
        const std::string query = argAfter(12);
        if (query.empty()) return "Error: arxiv search requires a query.";
        return arxivSearch(query);
    }
    if (cmd.rfind("cve search", 0) == 0) {
        const std::string query = argAfter(10);
        if (query.empty()) return "Error: cve search requires a keyword.";
        return cveSearch(query);
    }
    return "Error: unknown tool.";
}

constexpr int AgentHeaderH = 64;
constexpr int AgentInputH = 52;

LRESULT CALLBACK agentWindowProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* aw = reinterpret_cast<AgentWindowState*>(
        GetWindowLongPtrA(hw, GWLP_USERDATA));

    static HFONT heroFont = CreateFontA(-34, 0, 0, 0, FW_SEMIBOLD, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    static HFONT titleFont = CreateFontA(-24, 0, 0, 0, FW_SEMIBOLD, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    static HFONT headerFont = CreateFontA(-16, 0, 0, 0, FW_SEMIBOLD, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    static HFONT tagFont = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    static HFONT microFont = CreateFontA(-11, 0, 0, 0, FW_SEMIBOLD, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    static HFONT chatFont = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");

    // Move the input edit into the center column's bottom input box
    const auto layoutInput = [&](AgentWindowState* s) {
        if (!s || !s->input) return;
        RECT rc;
        GetClientRect(hw, &rc);
        const int mainL = AgentSidebarW;
        const int mainR = rc.right - AgentRightW;
        MoveWindow(s->input, mainL + 52, rc.bottom - 74,
                   (mainR - mainL) - 104, 24, TRUE);
    };

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        aw = reinterpret_cast<AgentWindowState*>(cs->lpCreateParams);
        SetWindowLongPtrA(hw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(aw));

        aw->sessions.emplace_back();

        aw->input = CreateWindowExA(0, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            20, 200, 200, 24, hw,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdAgentInput)),
            GetModuleHandleA(nullptr), nullptr);
        SendMessageA(aw->input, WM_SETFONT,
                     reinterpret_cast<WPARAM>(chatFont), TRUE);
        WNDPROC orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
            aw->input, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(editSubclassProc)));
        SetWindowLongPtrA(aw->input, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(orig));

        SetFocus(aw->input);
        // Refresh live metrics (RAM, latency, relative session times)
        SetTimer(hw, 1, 2000, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) InvalidateRect(hw, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = reinterpret_cast<HDC>(wp);
        SetTextColor(hdcEdit, PortText);
        SetBkColor(hdcEdit, RGB(18, 24, 42));
        static HBRUSH inputBrush = CreateSolidBrush(RGB(18, 24, 42));
        return reinterpret_cast<INT_PTR>(inputBrush);
    }
    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) {
            ShowWindow(hw, SW_HIDE);
            return 0;
        }
        layoutInput(aw);
        InvalidateRect(hw, nullptr, TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {1120, 640};
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC screen = BeginPaint(hw, &ps);
        RECT rc;
        GetClientRect(hw, &rc);
        const int w = rc.right;
        const int h = rc.bottom;
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);

        HBRUSH bg = CreateSolidBrush(RGB(10, 13, 24));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);

        const AgentInfo* agent = nullptr;
        if (aw && aw->desktop && aw->agentIndex >= 0 &&
            aw->agentIndex < static_cast<int>(aw->desktop->agents.size())) {
            agent = &aw->desktop->agents[aw->agentIndex];
        }
        if (!aw || !agent) {
            BitBlt(screen, 0, 0, w, h, dc, 0, 0, SRCCOPY);
            SelectObject(dc, oldBmp);
            DeleteObject(bmp);
            DeleteDC(dc);
            EndPaint(hw, &ps);
            return 0;
        }

        const BYTE ar = GetRValue(agent->accent);
        const BYTE ag = GetGValue(agent->accent);
        const BYTE ab = GetBValue(agent->accent);
        const Gdiplus::Color accent(255, ar, ag, ab);

        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        const int mainL = AgentSidebarW;
        const int mainR = w - AgentRightW;

        // Reusable rounded panel
        auto panel = [&](int x, int y, int pw, int ph, int rad,
                         Gdiplus::Color fill, Gdiplus::Color border) {
            Gdiplus::GraphicsPath path;
            addRoundRect(path, x, y, pw, ph, rad);
            Gdiplus::SolidBrush f(fill);
            g.FillPath(&f, &path);
            Gdiplus::Pen p(border, 1.0f);
            g.DrawPath(&p, &path);
        };

        // ══ Left sidebar ═════════════════════════════════════════
        {
            RECT side = {0, 0, AgentSidebarW, h};
            HBRUSH sideBg = CreateSolidBrush(RGB(15, 18, 30));
            FillRect(dc, &side, sideBg);
            DeleteObject(sideBg);
        }

        // Brand
        if (dockLogoImg) g.DrawImage(dockLogoImg, 18, 18, 26, 26);
        SelectObject(dc, headerFont);
        SetTextColor(dc, RGB(232, 236, 248));
        RECT brand = {52, 18, AgentSidebarW - 12, 44};
        DrawTextA(dc, "PORT OS", -1, &brand,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);

        // Agent identity panel
        panel(14, 58, AgentSidebarW - 28, 60, 12,
              Gdiplus::Color(255, 20, 24, 40), Gdiplus::Color(90, ar, ag, ab));
        Gdiplus::Image* img = loadAgentImage(agent->name);
        if (img) g.DrawImage(img, 26, 66, 44, 44);
        SelectObject(dc, headerFont);
        SetTextColor(dc, RGB(232, 236, 248));
        RECT idName = {80, 68, AgentSidebarW - 18, 90};
        DrawTextA(dc, agent->name.c_str(), -1, &idName,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
        SelectObject(dc, microFont);
        SetTextColor(dc, RGB(130, 140, 165));
        RECT idTag = {80, 90, AgentSidebarW - 18, 112};
        DrawTextA(dc, agent->tagline.c_str(), -1, &idTag,
                  DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);

        // New Session button
        aw->newSessionRect = {14, 130, AgentSidebarW - 14, 166};
        {
            Gdiplus::GraphicsPath path;
            addRoundRect(path, aw->newSessionRect.left, aw->newSessionRect.top,
                         aw->newSessionRect.right - aw->newSessionRect.left,
                         aw->newSessionRect.bottom - aw->newSessionRect.top, 9);
            Gdiplus::SolidBrush fill(Gdiplus::Color(
                aw->hotNewSession ? 255 : 225, ar, ag, ab));
            g.FillPath(&fill, &path);
        }
        SelectObject(dc, tagFont);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT nsText = {aw->newSessionRect.left + 16, aw->newSessionRect.top,
                       aw->newSessionRect.right, aw->newSessionRect.bottom};
        DrawTextA(dc, "+   New Session", -1, &nsText,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        // Ctrl N chip
        {
            RECT chip = {aw->newSessionRect.right - 52,
                         aw->newSessionRect.top + 8,
                         aw->newSessionRect.right - 12,
                         aw->newSessionRect.bottom - 8};
            Gdiplus::GraphicsPath cp;
            addRoundRect(cp, chip.left, chip.top, chip.right - chip.left,
                         chip.bottom - chip.top, 5);
            Gdiplus::SolidBrush cf(Gdiplus::Color(70, 255, 255, 255));
            g.FillPath(&cf, &cp);
            SelectObject(dc, microFont);
            DrawTextA(dc, "Ctrl N", -1, &chip,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }

        // Sessions
        SelectObject(dc, microFont);
        SetTextColor(dc, RGB(110, 120, 145));
        RECT sessLabel = {18, 180, AgentSidebarW - 12, 198};
        DrawTextA(dc, "SESSIONS", -1, &sessLabel,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
        aw->sessionRects.clear();
        int sy = 202;
        const int maxSessRows = 5;
        for (std::size_t i = 0;
             i < aw->sessions.size() && static_cast<int>(i) < maxSessRows;
             ++i) {
            const RECT row = {12, sy, AgentSidebarW - 12, sy + 34};
            aw->sessionRects.push_back(row);
            const bool activeRow = static_cast<int>(i) == aw->activeSession;
            const bool hotRow = static_cast<int>(i) == aw->hotSession;
            if (activeRow || hotRow) {
                Gdiplus::GraphicsPath path;
                addRoundRect(path, row.left, row.top, row.right - row.left,
                             row.bottom - row.top, 7);
                Gdiplus::SolidBrush fill(Gdiplus::Color(
                    activeRow ? 42 : 20, 255, 255, 255));
                g.FillPath(&fill, &path);
            }
            SelectObject(dc, tagFont);
            SetTextColor(dc, activeRow ? RGB(232, 236, 248)
                                       : RGB(165, 174, 198));
            RECT tr = {row.left + 12, row.top, row.right - 52, row.bottom};
            DrawTextA(dc, aw->sessions[i].title.c_str(), -1, &tr,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT |
                      DT_END_ELLIPSIS | DT_NOPREFIX);
            // Relative time, right-aligned
            if (aw->sessions[i].started) {
                SelectObject(dc, microFont);
                SetTextColor(dc, RGB(120, 130, 155));
                RECT tm = {row.right - 50, row.top, row.right - 8, row.bottom};
                DrawTextA(dc,
                          relativeTime(aw->sessions[i].lastActivity).c_str(),
                          -1, &tm,
                          DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
            }
            sy += 38;
        }

        // Workspace section
        SelectObject(dc, microFont);
        SetTextColor(dc, RGB(110, 120, 145));
        int wy = sy + 14;
        RECT wsLabel = {18, wy, AgentSidebarW - 12, wy + 18};
        DrawTextA(dc, "WORKSPACE", -1, &wsLabel,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
        wy += 26;
        // First two navigate to the real sandbox file browser; the rest
        // are honestly marked "Soon" until their backends exist.
        struct WsItem { const char* label; bool active; };
        const WsItem wsItems[] = {
            {"Projects", true}, {"Files", true},
            {"Knowledge Base", false}, {"Memory", false},
            {"Integrations", false}};
        SelectObject(dc, tagFont);
        for (int i = 0; i < 5; ++i) {
            const RECT wr = {12, wy - 4, AgentSidebarW - 12, wy + 24};
            aw->wsRects[i] = wsItems[i].active ? wr : RECT{};
            if (wsItems[i].active && aw->hotWs == i) {
                Gdiplus::GraphicsPath hp;
                addRoundRect(hp, wr.left, wr.top, wr.right - wr.left,
                             wr.bottom - wr.top, 7);
                Gdiplus::SolidBrush hf(Gdiplus::Color(22, 255, 255, 255));
                g.FillPath(&hf, &hp);
            }
            Gdiplus::SolidBrush dot(Gdiplus::Color(
                wsItems[i].active ? 150 : 60, ar, ag, ab));
            g.FillEllipse(&dot, 20, wy + 5, 8, 8);
            SetTextColor(dc, wsItems[i].active ? RGB(190, 198, 218)
                                               : RGB(110, 118, 140));
            RECT tr = {40, wy, AgentSidebarW - 12, wy + 20};
            DrawTextA(dc, wsItems[i].label, -1, &tr, DT_SINGLELINE | DT_LEFT);
            if (!wsItems[i].active) {
                SelectObject(dc, microFont);
                SetTextColor(dc, RGB(90, 98, 122));
                RECT sr = {40, wy, AgentSidebarW - 14, wy + 20};
                DrawTextA(dc, "Soon", -1, &sr, DT_SINGLELINE | DT_RIGHT);
                SelectObject(dc, tagFont);
            }
            wy += 28;
        }

        // Upgrade card + user, pinned to the bottom
        panel(14, h - 118, AgentSidebarW - 28, 46, 10,
              Gdiplus::Color(46, ar, ag, ab), Gdiplus::Color(110, ar, ag, ab));
        SelectObject(dc, tagFont);
        SetTextColor(dc, agent->accent);
        RECT upT = {24, h - 112, AgentSidebarW - 18, h - 94};
        DrawTextA(dc, "Upgrade Portal", -1, &upT, DT_SINGLELINE | DT_LEFT);
        SelectObject(dc, microFont);
        SetTextColor(dc, RGB(150, 158, 180));
        RECT upS = {24, h - 92, AgentSidebarW - 18, h - 78};
        DrawTextA(dc, "Unlock full potential", -1, &upS,
                  DT_SINGLELINE | DT_LEFT);
        {
            Gdiplus::SolidBrush av(Gdiplus::Color(255, 60, 70, 100));
            g.FillEllipse(&av, 18, h - 58, 32, 32);
            SelectObject(dc, tagFont);
            SetTextColor(dc, RGB(224, 230, 244));
            RECT un = {58, h - 58, AgentSidebarW - 12, h - 40};
            DrawTextA(dc, "Andi", -1, &un, DT_SINGLELINE | DT_LEFT);
            SelectObject(dc, microFont);
            SetTextColor(dc, RGB(130, 140, 165));
            RECT pl = {58, h - 40, AgentSidebarW - 12, h - 24};
            DrawTextA(dc, "Pro Plan", -1, &pl, DT_SINGLELINE | DT_LEFT);
        }

        // ══ Right sidebar ════════════════════════════════════════
        {
            RECT side = {mainR, 0, w, h};
            HBRUSH sideBg = CreateSolidBrush(RGB(15, 18, 30));
            FillRect(dc, &side, sideBg);
            DeleteObject(sideBg);
        }
        {
            const int rx = mainR + 16;
            const int rw = AgentRightW - 32;
            int ry = 20;
            const Gdiplus::Color cardBg(255, 20, 24, 40);
            const Gdiplus::Color cardBorder(255, 34, 40, 64);

            // Context card
            panel(rx, ry, rw, 168, 12, cardBg, cardBorder);
            SelectObject(dc, headerFont);
            SetTextColor(dc, RGB(226, 232, 246));
            RECT ct = {rx + 16, ry + 12, rx + rw - 12, ry + 34};
            DrawTextA(dc, "Context", -1, &ct, DT_SINGLELINE | DT_LEFT);
            struct CtxRow { const char* label; std::string value; };
            int deskCount = 0;
            if (auto r = aw->desktop->kernel.getSandbox().list("Desktop"))
                deskCount = static_cast<int>(r->size());
            int runningTasks = 0;
            for (const auto& kv : aw->desktop->agentWindows) {
                if (auto* other = reinterpret_cast<AgentWindowState*>(
                        GetWindowLongPtrA(kv.second, GWLP_USERDATA)))
                    if (other->waiting) ++runningTasks;
            }
            std::string workspaceName =
                aw->desktop->kernel.getSandbox().root().filename().string();
            if (workspaceName.empty()) workspaceName = "Port sandbox";
            CtxRow rows[] = {
                {"Current Workspace", workspaceName},
                {"Open Files", std::to_string(deskCount) + " files"},
                {"Running Tasks", std::to_string(runningTasks) + " active"},
                {"Connected Agents",
                 std::to_string(static_cast<int>(
                     aw->desktop->agentWindows.size())) + " online"},
            };
            int cy2 = ry + 44;
            for (const auto& row : rows) {
                SelectObject(dc, microFont);
                SetTextColor(dc, RGB(120, 130, 155));
                RECT lr = {rx + 16, cy2, rx + rw - 12, cy2 + 15};
                DrawTextA(dc, row.label, -1, &lr, DT_SINGLELINE | DT_LEFT);
                SelectObject(dc, tagFont);
                SetTextColor(dc, RGB(214, 222, 240));
                RECT vr = {rx + 16, cy2 + 15, rx + rw - 12, cy2 + 33};
                DrawTextA(dc, row.value.c_str(), -1, &vr,
                          DT_SINGLELINE | DT_LEFT);
                cy2 += 31;
            }
            ry += 184;

            // Memory card — real system RAM usage
            const SystemMemory memInfo = readSystemMemory();
            panel(rx, ry, rw, 150, 12, cardBg, cardBorder);
            SelectObject(dc, headerFont);
            SetTextColor(dc, RGB(226, 232, 246));
            RECT mt = {rx + 16, ry + 12, rx + rw - 12, ry + 34};
            DrawTextA(dc, "System Memory", -1, &mt, DT_SINGLELINE | DT_LEFT);
            // Donut with the real percentage
            {
                const int dcx = rx + 42, dcy = ry + 78, dr = 26;
                Gdiplus::Pen track(Gdiplus::Color(255, 40, 46, 70), 7.0f);
                g.DrawEllipse(&track, dcx - dr, dcy - dr, dr * 2, dr * 2);
                Gdiplus::Pen arc(accent, 7.0f);
                g.DrawArc(&arc, dcx - dr, dcy - dr, dr * 2, dr * 2,
                          -90.0f, 360.0f * (memInfo.percent / 100.0f));
                SelectObject(dc, tagFont);
                SetTextColor(dc, RGB(232, 236, 248));
                char pctText[8];
                std::snprintf(pctText, sizeof(pctText), "%d%%",
                              memInfo.percent);
                RECT pc = {dcx - dr, dcy - 10, dcx + dr, dcy + 10};
                DrawTextA(dc, pctText, -1, &pc,
                          DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            }
            SelectObject(dc, microFont);
            SetTextColor(dc, RGB(120, 130, 155));
            RECT mu = {rx + 86, ry + 50, rx + rw - 12, ry + 66};
            DrawTextA(dc, "RAM Usage", -1, &mu, DT_SINGLELINE | DT_LEFT);
            SelectObject(dc, tagFont);
            SetTextColor(dc, RGB(214, 222, 240));
            char ramText[48];
            std::snprintf(ramText, sizeof(ramText), "%.1f GB / %.1f GB",
                          memInfo.usedGB, memInfo.totalGB);
            RECT mv = {rx + 86, ry + 66, rx + rw - 12, ry + 84};
            DrawTextA(dc, ramText, -1, &mv, DT_SINGLELINE | DT_LEFT);
            {
                Gdiplus::GraphicsPath bp;
                addRoundRect(bp, rx + 86, ry + 90, rw - 100, 6, 3);
                Gdiplus::SolidBrush bt(Gdiplus::Color(255, 40, 46, 70));
                g.FillPath(&bt, &bp);
                Gdiplus::GraphicsPath fp;
                addRoundRect(fp, rx + 86, ry + 90,
                             static_cast<int>((rw - 100) *
                                              (memInfo.percent / 100.0f)),
                             6, 3);
                Gdiplus::SolidBrush ff(accent);
                g.FillPath(&ff, &fp);
            }
            SelectObject(dc, microFont);
            SetTextColor(dc, RGB(120, 130, 155));
            RECT li = {rx + 16, ry + 118, rx + rw - 80, ry + 134};
            DrawTextA(dc, "Available", -1, &li, DT_SINGLELINE | DT_LEFT);
            SetTextColor(dc, RGB(90, 210, 130));
            char availText[24];
            std::snprintf(availText, sizeof(availText), "%.1f GB free",
                          memInfo.totalGB - memInfo.usedGB);
            RECT la = {rx + 16, ry + 118, rx + rw - 12, ry + 134};
            DrawTextA(dc, availText, -1, &la, DT_SINGLELINE | DT_RIGHT);
            ry += 166;

            // Agent Mode card — the chips cycle through real settings
            panel(rx, ry, rw, 138, 12, cardBg, cardBorder);
            SelectObject(dc, headerFont);
            SetTextColor(dc, RGB(226, 232, 246));
            RECT amt = {rx + 16, ry + 12, rx + rw - 12, ry + 34};
            DrawTextA(dc, "Agent Mode", -1, &amt, DT_SINGLELINE | DT_LEFT);
            static const char* modeOpts[] = {"Balanced", "Precise",
                                             "Creative"};
            static const char* reasoningOpts[] = {"Concise", "Standard",
                                                  "Deep"};
            static const char* styleOpts[] = {"Brief", "Detailed"};
            const char* labels[] = {"Mode", "Reasoning Depth",
                                    "Response Style"};
            const char* values[] = {modeOpts[aw->mode],
                                    reasoningOpts[aw->reasoning],
                                    styleOpts[aw->style]};
            int my = ry + 44;
            for (int i = 0; i < 3; ++i) {
                SelectObject(dc, tagFont);
                SetTextColor(dc, RGB(150, 160, 182));
                RECT ml = {rx + 16, my, rx + rw / 2, my + 22};
                DrawTextA(dc, labels[i], -1, &ml,
                          DT_SINGLELINE | DT_LEFT | DT_VCENTER);
                RECT chip = {rx + rw - 104, my, rx + rw - 12, my + 24};
                aw->modeChips[i] = chip;
                Gdiplus::GraphicsPath cp;
                addRoundRect(cp, chip.left, chip.top, chip.right - chip.left,
                             chip.bottom - chip.top, 6);
                Gdiplus::SolidBrush cf(Gdiplus::Color(255, 28, 34, 56));
                g.FillPath(&cf, &cp);
                Gdiplus::Pen cpen(Gdiplus::Color(90, ar, ag, ab), 1.0f);
                g.DrawPath(&cpen, &cp);
                SetTextColor(dc, agent->accent);
                RECT cvr = {chip.left + 10, chip.top, chip.right - 18,
                            chip.bottom};
                DrawTextA(dc, values[i], -1, &cvr,
                          DT_SINGLELINE | DT_LEFT | DT_VCENTER);
                // small ▾ affordance
                SetTextColor(dc, RGB(150, 160, 182));
                RECT cvv = {chip.right - 16, chip.top, chip.right - 4,
                            chip.bottom};
                DrawTextA(dc, "v", -1, &cvv,
                          DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                my += 30;
            }
            ry += 154;

            // Engine card
            panel(rx, ry, rw, 66, 12,
                  Gdiplus::Color(60, ar, ag, ab),
                  Gdiplus::Color(120, ar, ag, ab));
            if (dockLogoImg) g.DrawImage(dockLogoImg, rx + 12, ry + 15, 36, 36);
            SelectObject(dc, tagFont);
            SetTextColor(dc, RGB(226, 232, 246));
            RECT et = {rx + 58, ry + 14, rx + rw - 12, ry + 34};
            DrawTextA(dc, "Port Agent Engine", -1, &et,
                      DT_SINGLELINE | DT_LEFT);
            SelectObject(dc, microFont);
            SetTextColor(dc, RGB(120, 130, 155));
            RECT es = {rx + 58, ry + 36, rx + rw - 70, ry + 52};
            DrawTextA(dc, "Engine Status", -1, &es, DT_SINGLELINE | DT_LEFT);
            SetTextColor(dc, RGB(90, 210, 130));
            RECT eo = {rx + 58, ry + 36, rx + rw - 12, ry + 52};
            DrawTextA(dc, "Optimal", -1, &eo, DT_SINGLELINE | DT_RIGHT);
        }

        // ══ Center column ════════════════════════════════════════
        // Header: name + provider engine
        SelectObject(dc, titleFont);
        SetTextColor(dc, RGB(236, 240, 250));
        RECT mName = {mainL + 32, 20, mainR - 360, 54};
        DrawTextA(dc, agent->name.c_str(), -1, &mName,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
        std::string engine = "Powered by Port Agent Engine";
        if (gAIManager) {
            const std::string p = gAIManager->activeProviderName();
            if (!p.empty()) engine = "Powered by " + p;
        }
        SelectObject(dc, microFont);
        SetTextColor(dc, agent->accent);
        RECT mEngine = {mainL + 32, 56, mainR - 240, 74};
        DrawTextA(dc, engine.c_str(), -1, &mEngine,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);

        // Status pills (Engine / Latency / Memory), top-right of center
        {
            const bool online = gAIManager && gAIManager->isAvailable();
            const int latency = gLastLatencyMs.load();
            std::string latencyText = latency > 0
                ? std::to_string(latency) + "ms" : "--";
            const SystemMemory pillMem = readSystemMemory();
            char memPill[8];
            std::snprintf(memPill, sizeof(memPill), "%d%%", pillMem.percent);
            struct Pill { const char* label; std::string value; bool dot;
                          COLORREF dotColor; };
            Pill pills[] = {
                {"Engine", online ? "Online" : "Offline", true,
                 online ? RGB(80, 220, 130) : RGB(220, 90, 90)},
                {"Latency", latencyText, false, agent->accent},
                {"Memory", memPill, false, agent->accent},
            };
            int px = mainR - 20;
            for (int i = 2; i >= 0; --i) {
                const int pw = 104;
                px -= pw + 8;
                panel(px, 20, pw, 44, 8, Gdiplus::Color(255, 20, 26, 44),
                      Gdiplus::Color(255, 36, 44, 70));
                SelectObject(dc, microFont);
                SetTextColor(dc, RGB(120, 130, 155));
                RECT lbl = {px + 12, 26, px + pw, 42};
                DrawTextA(dc, pills[i].label, -1, &lbl,
                          DT_SINGLELINE | DT_LEFT);
                if (pills[i].dot) {
                    Gdiplus::SolidBrush d(Gdiplus::Color(
                        255, GetRValue(pills[i].dotColor),
                        GetGValue(pills[i].dotColor),
                        GetBValue(pills[i].dotColor)));
                    g.FillEllipse(&d, px + 12, 46, 7, 7);
                    SetTextColor(dc, RGB(210, 218, 236));
                    RECT val = {px + 26, 42, px + pw, 60};
                    SelectObject(dc, tagFont);
                    DrawTextA(dc, pills[i].value.c_str(), -1, &val,
                              DT_SINGLELINE | DT_LEFT);
                } else {
                    SetTextColor(dc, RGB(210, 218, 236));
                    RECT val = {px + 12, 42, px + pw, 60};
                    SelectObject(dc, tagFont);
                    DrawTextA(dc, pills[i].value.c_str(), -1, &val,
                              DT_SINGLELINE | DT_LEFT);
                }
            }
        }

        aw->cardRects.clear();
        AgentSession& sess = aw->active();
        const int inputTop = h - 96;

        if (!sess.started) {
            // Hero greeting
            SelectObject(dc, heroFont);
            SetTextColor(dc, agent->accent);
            RECT hi = {mainL, 130, mainR, 178};
            DrawTextA(dc, "Hello Andi.", -1, &hi, DT_SINGLELINE | DT_CENTER);
            SelectObject(dc, headerFont);
            SetTextColor(dc, RGB(214, 222, 240));
            RECT sub = {mainL + 30, 184, mainR - 30, 236};
            const std::string subtitle =
                "I'm " + agent->name + ", " + agent->tagline +
                ".\nHow can I help you today?";
            DrawTextA(dc, subtitle.c_str(), -1, &sub,
                      DT_CENTER | DT_WORDBREAK);

            // Capability cards (6)
            struct Card { const char* title; const char* desc;
                          const char* prompt; };
            const Card cards[] = {
                {"Code", "Write, review and optimize code.",
                 "Help me write and review some code."},
                {"Documents", "Summarize and analyze files.",
                 "List the files on my Desktop, then summarize one of them."},
                {"Research", "Deep research with live data.",
                 "Search the web for "},
                {"Images", "Generate and analyze images.",
                 "Help me plan an image or analyze one."},
                {"Automation", "Automate tasks and workflows.",
                 "Suggest an automation for my workflow."},
                {"More", "Explore all capabilities.", ""},
            };
            const int n = 6;
            const int gap = 14;
            const int cardsW = (mainR - mainL) - 64;
            const int cw = (cardsW - gap * (n - 1)) / n;
            const int chH = 116;
            const int cyy = 250;
            int cx = mainL + 32;
            for (int i = 0; i < n; ++i) {
                RECT card = {cx, cyy, cx + cw, cyy + chH};
                aw->cardRects.push_back({card, cards[i].prompt});
                const bool hot = aw->hotCard == i;
                panel(card.left, card.top, cw, chH, 12,
                      Gdiplus::Color(hot ? 255 : 210, 18, 22, 40),
                      Gdiplus::Color(hot ? 210 : 70, ar, ag, ab));
                // Accent glyph badge
                Gdiplus::SolidBrush badge(Gdiplus::Color(60, ar, ag, ab));
                g.FillEllipse(&badge, card.left + 14, card.top + 14, 26, 26);
                SelectObject(dc, tagFont);
                SetTextColor(dc, RGB(230, 235, 248));
                RECT ct2 = {card.left + 12, card.top + 48, card.right - 10,
                            card.top + 68};
                DrawTextA(dc, cards[i].title, -1, &ct2,
                          DT_SINGLELINE | DT_LEFT);
                SelectObject(dc, microFont);
                SetTextColor(dc, RGB(135, 145, 168));
                RECT cd = {card.left + 12, card.top + 68, card.right - 10,
                           card.bottom - 8};
                DrawTextA(dc, cards[i].desc, -1, &cd, DT_LEFT | DT_WORDBREAK);
                cx += cw + gap;
            }

            // Cosmic hero graphic (planet arc + 4-point star)
            {
                const int cxc = (mainL + mainR) / 2;
                const int arcY = std::min(h - 150, cyy + chH + 190);
                Gdiplus::GraphicsPath glow;
                glow.AddEllipse(cxc - 260, arcY - 40, 520, 520);
                Gdiplus::PathGradientBrush gb(&glow);
                gb.SetCenterColor(Gdiplus::Color(70, ar, ag, ab));
                Gdiplus::Color surround(0, ar, ag, ab);
                int surroundCount = 1;
                gb.SetSurroundColors(&surround, &surroundCount);
                g.FillPath(&gb, &glow);
                Gdiplus::Pen arcPen(Gdiplus::Color(210, ar, ag, ab), 2.5f);
                g.DrawArc(&arcPen, cxc - 240, arcY, 480, 480, 180.0f, 180.0f);
                // 4-point star
                const int scx = cxc, scy = arcY + 8;
                Gdiplus::PointF star[8] = {
                    {(float)scx, (float)scy - 34}, {(float)scx + 9, (float)scy - 9},
                    {(float)scx + 34, (float)scy}, {(float)scx + 9, (float)scy + 9},
                    {(float)scx, (float)scy + 34}, {(float)scx - 9, (float)scy + 9},
                    {(float)scx - 34, (float)scy}, {(float)scx - 9, (float)scy - 9}};
                Gdiplus::SolidBrush starFill(Gdiplus::Color(255, ar, ag, ab));
                g.FillPolygon(&starFill, star, 8);
            }
        } else {
            // Chat transcript
            SelectObject(dc, chatFont);
            const int chatTop = AgentMainHeaderH + 12;
            const int chatBottom = inputTop - 12;
            const int viewH = chatBottom - chatTop;
            const int maxBubbleW = ((mainR - mainL) * 68) / 100;

            struct Laid { RECT box; bool isUser; };
            std::vector<Laid> laid;
            int y = 0;
            for (const auto& entry : sess.history) {
                RECT tr = {0, 0, maxBubbleW - 24, 0};
                DrawTextA(dc, entry.text.c_str(), -1, &tr,
                          DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                const int bw = (tr.right - tr.left) + 24;
                const int bh = (tr.bottom - tr.top) + 16;
                RECT box = entry.isUser
                    ? RECT{mainR - 24 - bw, y, mainR - 24, y + bh}
                    : RECT{mainL + 32, y, mainL + 32 + bw, y + bh};
                laid.push_back({box, entry.isUser});
                y += bh + 10;
            }
            aw->contentH = y + (aw->waiting ? 26 : 0);
            const int maxScroll = std::max(0, aw->contentH - viewH);
            if (sess.pinned) sess.scroll = maxScroll;
            if (sess.scroll > maxScroll) sess.scroll = maxScroll;
            if (sess.scroll < 0) sess.scroll = 0;

            const int savedDc = SaveDC(dc);
            IntersectClipRect(dc, mainL, chatTop, w, chatBottom);
            g.SetClip(Gdiplus::Rect(mainL, chatTop, w - mainL, viewH));
            for (std::size_t i = 0; i < laid.size(); ++i) {
                RECT box = laid[i].box;
                OffsetRect(&box, 0, chatTop - sess.scroll);
                if (box.bottom < chatTop || box.top > chatBottom) continue;
                Gdiplus::GraphicsPath path;
                addRoundRect(path, box.left, box.top, box.right - box.left,
                             box.bottom - box.top, 12);
                if (laid[i].isUser) {
                    Gdiplus::SolidBrush fill(Gdiplus::Color(52, ar, ag, ab));
                    g.FillPath(&fill, &path);
                    Gdiplus::Pen pen(Gdiplus::Color(130, ar, ag, ab), 1.0f);
                    g.DrawPath(&pen, &path);
                } else {
                    Gdiplus::SolidBrush fill(Gdiplus::Color(255, 20, 26, 44));
                    g.FillPath(&fill, &path);
                    Gdiplus::Pen pen(Gdiplus::Color(255, 36, 44, 70), 1.0f);
                    g.DrawPath(&pen, &path);
                }
                SetTextColor(dc, RGB(224, 230, 244));
                RECT tRect = {box.left + 12, box.top + 8, box.right - 12,
                              box.bottom - 8};
                DrawTextA(dc, sess.history[i].text.c_str(), -1, &tRect,
                          DT_WORDBREAK | DT_NOPREFIX);
            }
            if (aw->waiting) {
                SetTextColor(dc, agent->accent);
                RECT think = {mainL + 32,
                              chatTop - sess.scroll + aw->contentH - 22,
                              mainR - 24,
                              chatTop - sess.scroll + aw->contentH};
                DrawTextA(dc, "thinking...", -1, &think,
                          DT_SINGLELINE | DT_LEFT);
            }
            g.ResetClip();
            RestoreDC(dc, savedDc);
        }

        // Input box (bottom of center column) with a tool row + send button
        {
            const int boxX = mainL + 24;
            const int boxW = (mainR - mainL) - 48;
            const int boxTop = inputTop;
            const int boxH = 76;
            panel(boxX, boxTop, boxW, boxH, 14,
                  Gdiplus::Color(255, 18, 22, 38),
                  Gdiplus::Color(120, ar, ag, ab));

            // Placeholder when the edit is empty
            if (!sess.started && getWindowText(aw->input).empty()) {
                SelectObject(dc, tagFont);
                SetTextColor(dc, RGB(120, 130, 155));
                RECT ph = {boxX + 24, boxTop + 10, boxX + boxW - 60,
                           boxTop + 34};
                DrawTextA(dc, ("Ask " + agent->name + " anything...").c_str(),
                          -1, &ph, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
            }

            // Tool row
            SelectObject(dc, microFont);
            SetTextColor(dc, RGB(150, 160, 182));
            const char* tools[] = {"Attach", "Tools", "Voice", "Deep Think"};
            int tx = boxX + 24;
            for (const char* t : tools) {
                RECT tr = {tx, boxTop + boxH - 26, tx + 90, boxTop + boxH - 8};
                DrawTextA(dc, t, -1, &tr, DT_SINGLELINE | DT_LEFT);
                SIZE ts;
                GetTextExtentPoint32A(dc, t, static_cast<int>(strlen(t)), &ts);
                tx += ts.cx + 28;
            }
            // Send button
            {
                const int sr = 16;
                const int scx = boxX + boxW - 26;
                const int scy = boxTop + boxH - 24;
                Gdiplus::SolidBrush sb(accent);
                g.FillEllipse(&sb, scx - sr, scy - sr, sr * 2, sr * 2);
                Gdiplus::Pen ap(Gdiplus::Color(255, 255, 255, 255), 2.0f);
                g.DrawLine(&ap, scx, scy + 5, scx, scy - 5);
                g.DrawLine(&ap, scx - 4, scy - 1, scx, scy - 5);
                g.DrawLine(&ap, scx + 4, scy - 1, scx, scy - 5);
            }
        }

        // Disclaimer
        SelectObject(dc, microFont);
        SetTextColor(dc, RGB(95, 104, 128));
        RECT disc = {mainL, h - 18, mainR, h - 4};
        DrawTextA(dc, (agent->name +
                       " can make mistakes. Consider checking important "
                       "information.").c_str(),
                  -1, &disc, DT_SINGLELINE | DT_CENTER);

        BitBlt(screen, 0, 0, w, h, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!aw) break;
        POINT pt = {static_cast<short>(LOWORD(lp)),
                    static_cast<short>(HIWORD(lp))};
        bool changed = false;
        const bool hotNew = PtInRect(&aw->newSessionRect, pt) != 0;
        if (hotNew != aw->hotNewSession) { aw->hotNewSession = hotNew; changed = true; }
        int hotSess = -1;
        for (std::size_t i = 0; i < aw->sessionRects.size(); ++i) {
            if (PtInRect(&aw->sessionRects[i], pt)) { hotSess = static_cast<int>(i); break; }
        }
        if (hotSess != aw->hotSession) { aw->hotSession = hotSess; changed = true; }
        int hotCard = -1;
        for (std::size_t i = 0; i < aw->cardRects.size(); ++i) {
            if (PtInRect(&aw->cardRects[i].rect, pt)) { hotCard = static_cast<int>(i); break; }
        }
        if (hotCard != aw->hotCard) { aw->hotCard = hotCard; changed = true; }
        int hotWs = -1;
        for (int i = 0; i < 5; ++i) {
            if (!IsRectEmpty(&aw->wsRects[i]) &&
                PtInRect(&aw->wsRects[i], pt)) { hotWs = i; break; }
        }
        if (hotWs != aw->hotWs) { aw->hotWs = hotWs; changed = true; }
        if (changed) InvalidateRect(hw, nullptr, FALSE);
        TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hw, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (aw && (aw->hotNewSession || aw->hotSession != -1 ||
                   aw->hotCard != -1 || aw->hotWs != -1)) {
            aw->hotNewSession = false;
            aw->hotSession = -1;
            aw->hotCard = -1;
            aw->hotWs = -1;
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        if (!aw) break;
        POINT pt = {static_cast<short>(LOWORD(lp)),
                    static_cast<short>(HIWORD(lp))};
        if (PtInRect(&aw->newSessionRect, pt)) {
            aw->sessions.emplace_back();
            aw->activeSession = static_cast<int>(aw->sessions.size()) - 1;
            SetWindowTextA(aw->input, "");
            SetFocus(aw->input);
            layoutInput(aw);
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }
        for (std::size_t i = 0; i < aw->sessionRects.size(); ++i) {
            if (PtInRect(&aw->sessionRects[i], pt)) {
                aw->activeSession = static_cast<int>(i);
                layoutInput(aw);
                InvalidateRect(hw, nullptr, FALSE);
                return 0;
            }
        }
        for (const auto& card : aw->cardRects) {
            if (PtInRect(&card.rect, pt)) {
                if (!card.prompt.empty()) {
                    SetWindowTextA(aw->input, card.prompt.c_str());
                    SendMessageA(aw->input, EM_SETSEL,
                                 card.prompt.size(), card.prompt.size());
                }
                SetFocus(aw->input);
                return 0;
            }
        }
        // Workspace nav: Projects / Files open the sandbox file browser
        for (int i = 0; i < 2; ++i) {
            if (!IsRectEmpty(&aw->wsRects[i]) &&
                PtInRect(&aw->wsRects[i], pt)) {
                if (HWND desktopWnd = GetParent(hw)) {
                    PostMessageA(desktopWnd, WM_COMMAND, IdIconMyServer, 0);
                }
                return 0;
            }
        }
        // Agent Mode chips: cycle the setting
        if (PtInRect(&aw->modeChips[0], pt)) {
            aw->mode = (aw->mode + 1) % 3;
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&aw->modeChips[1], pt)) {
            aw->reasoning = (aw->reasoning + 1) % 3;
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&aw->modeChips[2], pt)) {
            aw->style = (aw->style + 1) % 2;
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (!aw) break;
        AgentSession& sess = aw->active();
        RECT rc;
        GetClientRect(hw, &rc);
        const int viewH = (rc.bottom - 96 - 12) - (AgentMainHeaderH + 12);
        const int maxScroll = std::max(0, aw->contentH - viewH);
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int scroll = sess.scroll - (delta / WHEEL_DELTA) * 48;
        if (scroll < 0) scroll = 0;
        if (scroll > maxScroll) scroll = maxScroll;
        if (scroll != sess.scroll) {
            sess.scroll = scroll;
            sess.pinned = (scroll >= maxScroll);
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IdRunCommand && aw && aw->desktop) {
            std::string text = getWindowText(aw->input);
            const std::size_t firstChar = text.find_first_not_of(' ');
            if (firstChar == std::string::npos) return 0;
            text = text.substr(firstChar);
            SetWindowTextA(aw->input, "");

            AgentSession& sess = aw->active();
            if (aw->waiting) {
                sess.history.push_back(
                    {false, "One moment - I'm still answering the previous "
                            "message."});
                InvalidateRect(hw, nullptr, FALSE);
                return 0;
            }

            const bool firstMessage = !sess.started;
            sess.history.push_back({true, text});
            sess.started = true;
            sess.pinned = true;
            sess.lastActivity = std::chrono::system_clock::now();
            aw->waiting = true;
            aw->pendingSession = aw->activeSession;
            if (firstMessage) {
                sess.title = text.size() > 32 ? text.substr(0, 32) + "..."
                                              : text;
                layoutInput(aw);
            }

            const auto& agent = aw->desktop->agents[aw->agentIndex];
            port::ai::PromptRequest request;
            request.systemInstruction = agent.systemPrompt +
                                        PortAgentSharedContext +
                                        PortAgentToolProtocol +
                                        agent.extraTools;

            // Apply the Agent Mode settings to the actual request.
            request.temperature = aw->mode == 1 ? 0.25f
                                : aw->mode == 2 ? 1.0f : 0.6f;
            request.maxTokens = aw->style == 0 ? 700 : 2048;
            std::string modeInstr;
            if (aw->reasoning == 0) {
                modeInstr += " Answer directly with minimal explanation.";
            } else if (aw->reasoning == 2) {
                modeInstr += " Think step by step and reason carefully "
                             "before giving the final answer.";
            }
            modeInstr += aw->style == 0
                ? " Keep the response brief and to the point."
                : " Give a thorough, well-structured response.";
            request.systemInstruction += modeInstr;

            const std::size_t maxHistory = 20;
            std::size_t start = sess.history.size() > maxHistory
                                    ? sess.history.size() - maxHistory
                                    : 0;
            while (start < sess.history.size() &&
                   !sess.history[start].isUser) {
                ++start;
            }
            for (std::size_t i = start; i < sess.history.size(); ++i) {
                if (!sess.history[i].isUser &&
                    sess.history[i].text.rfind("[tool] ", 0) == 0) {
                    continue;
                }
                const auto role = sess.history[i].isUser
                                      ? port::ai::Message::Role::User
                                      : port::ai::Message::Role::Assistant;
                if (!request.messages.empty() &&
                    request.messages.back().role == role) {
                    request.messages.back().content +=
                        "\n\n" + sess.history[i].text;
                } else {
                    request.messages.push_back({role, sess.history[i].text});
                }
            }

            HWND desktopWnd = GetParent(hw);
            const int agentIndex = aw->agentIndex;
            DesktopState* desktop = aw->desktop;
            std::thread([desktopWnd, agentIndex, desktop,
                         request = std::move(request)]() mutable {
                const auto post = [&](std::string text, bool ok, bool final) {
                    auto* result = new AgentAsyncResult{
                        agentIndex, ok, final, std::move(text)};
                    if (!PostMessageA(desktopWnd, PortMessageAgentAIComplete,
                                      0, reinterpret_cast<LPARAM>(result))) {
                        delete result;
                    }
                };

                if (!gAIManager || !gAIManager->isAvailable()) {
                    post("No AI provider is available. Configure an API key "
                         "(Gemini/Groq/OpenRouter) or start Ollama.",
                         false, true);
                    return;
                }

                // Tool loop: let the model request sandbox reads, feed the
                // results back, and stop at the final natural answer.
                constexpr int maxToolCalls = 3;
                for (int iteration = 0; ; ++iteration) {
                    const auto t0 = std::chrono::steady_clock::now();
                    auto ai = gAIManager->complete(request);
                    gLastLatencyMs = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count());
                    if (!ai) {
                        post("AI error: " + ai.error().detail, false, true);
                        return;
                    }
                    const std::string reply = ai->content;
                    const std::string toolCmd = parseAgentToolCall(reply);
                    if (toolCmd.empty() || iteration >= maxToolCalls) {
                        post(reply, true, true);
                        return;
                    }

                    post("[tool] " + toolCmd, true, false);
                    const std::string toolResult =
                        runAgentTool(desktop->kernel.getSandbox(), toolCmd);

                    request.messages.push_back(
                        {port::ai::Message::Role::Assistant, reply});
                    request.messages.push_back(
                        {port::ai::Message::Role::User,
                         "TOOL RESULT:\n" + toolResult +
                             "\n\nNow answer the user's question using this. "
                             "You may use one more TOOL line only if truly "
                             "necessary."});
                }
            }).detach();

            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hw);
        return 0;
    case WM_DESTROY:
        if (aw) {
            if (aw->desktop) {
                for (auto it = aw->desktop->agentWindows.begin();
                     it != aw->desktop->agentWindows.end(); ++it) {
                    if (it->second == hw) {
                        aw->desktop->agentWindows.erase(it);
                        break;
                    }
                }
            }
            delete aw;
            SetWindowLongPtrA(hw, GWLP_USERDATA, 0);
            // Remove this window's tile from the dock
            if (HWND desktopWnd = GetParent(hw)) {
                RECT dockRect;
                GetClientRect(desktopWnd, &dockRect);
                dockRect.top = dockRect.bottom - kDockH;
                InvalidateRect(desktopWnd, &dockRect, FALSE);
            }
        }
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

void openAgentWindow(HWND desktopWnd, DesktopState* state, int agentIndex)
{
    if (!state || agentIndex < 0 ||
        agentIndex >= static_cast<int>(state->agents.size())) {
        return;
    }
    const auto existing = state->agentWindows.find(agentIndex);
    if (existing != state->agentWindows.end() && IsWindow(existing->second)) {
        ShowWindow(existing->second, SW_SHOW);
        SetForegroundWindow(existing->second);
        return;
    }

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSA cls = {};
        cls.lpfnWndProc = agentWindowProc;
        cls.hInstance = GetModuleHandleA(nullptr);
        cls.lpszClassName = "PortAgentWindow";
        cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&cls);
        classRegistered = true;
    }

    auto* aw = new AgentWindowState();
    aw->desktop = state;
    aw->agentIndex = agentIndex;

    const auto& agent = state->agents[agentIndex];
    const std::string title = agent.name + " - Port Agent";

    // Centered over the desktop, with a slight cascade so several open
    // agents don't stack exactly on top of each other
    const int width = 1300;
    const int height = 800;
    RECT desktopRect;
    GetClientRect(desktopWnd, &desktopRect);
    const int cascade =
        (static_cast<int>(state->agentWindows.size()) % 5) * 24;
    const int x = std::max(0L, (desktopRect.right - width) / 2) + cascade;
    const int y = std::max(0L, (desktopRect.bottom - kTaskbarReserved -
                                height) / 2) + cascade;

    HWND wnd = CreateWindowExA(0, "PortAgentWindow", title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        x, y, width, height,
        desktopWnd, nullptr, GetModuleHandleA(nullptr), aw);
    if (!wnd) {
        delete aw;
        return;
    }
    state->agentWindows[agentIndex] = wnd;
    // Position the input control now that the window exists
    if (aw->input) {
        RECT rc;
        GetClientRect(wnd, &rc);
        MoveWindow(aw->input, AgentSidebarW + 40, rc.bottom - 44,
                   rc.right - AgentSidebarW - 96, 24, TRUE);
    }

    // The dock shows a tile per running agent — repaint it
    RECT dockRect;
    GetClientRect(desktopWnd, &dockRect);
    dockRect.top = dockRect.bottom - kDockH;
    InvalidateRect(desktopWnd, &dockRect, FALSE);
}

LRESULT CALLBACK agentHandleProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopState* state =
        reinterpret_cast<DesktopState*>(GetWindowLongPtrA(hw, GWLP_USERDATA));
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC screen = BeginPaint(hw, &ps);
        RECT rc;
        GetClientRect(hw, &rc);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);

        HBRUSH bg = CreateSolidBrush(RGB(13, 17, 31));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen borderPen(Gdiplus::Color(255, 44, 54, 84), 1.0f);
        g.DrawLine(&borderPen, rc.right - 1, 0, rc.right - 1, rc.bottom);

        // "❯" chevron pointing right
        Gdiplus::Pen chevron(Gdiplus::Color(255, 150, 175, 230), 2.0f);
        const int cy = rc.bottom / 2;
        g.DrawLine(&chevron, 7, cy - 7, 14, cy);
        g.DrawLine(&chevron, 14, cy, 7, cy + 7);

        BitBlt(screen, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_LBUTTONUP:
        if (state) {
            state->agentPanelVisible = true;
            ShowWindow(hw, SW_HIDE);
            if (state->agentPanel) {
                SetWindowPos(state->agentPanel, HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                             SWP_SHOWWINDOW);
            }
        }
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

void layoutDesktop(HWND window, DesktopState* state)
{
    RECT rect;
    GetClientRect(window, &rect);

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    if (IsIconic(window) || width < 200 || height < 200) {
        return;
    }
    const int taskbarHeight = kTaskbarReserved;

    auto layoutInfo = getIconLayoutInfo(state->iconSize);
    const int iconWidth = layoutInfo.width;
    const int iconHeight = layoutInfo.height;

    HWND computerIcon = GetDlgItem(window, IdIconMyComputer);
    HWND trashIcon = GetDlgItem(window, IdIconTrashBin);

    int maxX = width - iconWidth;
    int maxY = height - taskbarHeight - iconHeight;
    if (maxX < 0) maxX = 0;
    if (maxY < 0) maxY = 0;

    if (state->myPortPos.x > maxX) state->myPortPos.x = maxX;
    if (state->myPortPos.y > maxY) state->myPortPos.y = maxY;
    if (state->trashPos.x > maxX) state->trashPos.x = maxX;
    if (state->trashPos.y > maxY) state->trashPos.y = maxY;
    if (state->myServerPos.x > maxX) state->myServerPos.x = maxX;
    if (state->myServerPos.y > maxY) state->myServerPos.y = maxY;

    HWND serverIcon = GetDlgItem(window, IdIconMyServer);

    // Match the search pill layout in drawTaskbar:
    //   pillLeft=16, pillRight=width-16, pill floats above the dock,
    //   pillBottom = height - kDockH - 8, pillH = 48
    const int pillLeft  = 16;
    const int pillRight = width - 16;
    const int rSecX     = pillRight - 50;   // right section = magnifying glass only

    int editX      = pillLeft + 68;         // after orb + separator
    int editY      = height - kDockH - 8 - 48 + 13; // centered in the pill
    int editWidth  = rSecX - editX - 240;   // right strip = agent + engine badges
    int editHeight = 22;

    if (editWidth < 60) editWidth = 60;

    HDWP positions = BeginDeferWindowPos(6);
    // SWP_NOCOPYBITS: moving an icon by pixel-copy would drag along the
    // wallpaper patch painted for its old position.
    const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS;
    if (computerIcon) positions = DeferWindowPos(
        positions, computerIcon, nullptr, state->myPortPos.x, state->myPortPos.y,
        iconWidth, iconHeight, flags);
    if (trashIcon) positions = DeferWindowPos(
        positions, trashIcon, nullptr, state->trashPos.x, state->trashPos.y,
        iconWidth, iconHeight, flags);
    if (serverIcon) positions = DeferWindowPos(
        positions, serverIcon, nullptr, state->myServerPos.x, state->myServerPos.y,
        iconWidth, iconHeight, flags);
    if (state->input) positions = DeferWindowPos(
        positions, state->input, nullptr, editX, editY,
        editWidth, editHeight, flags);
    if (state->output) positions = DeferWindowPos(
        positions, state->output, nullptr, width + 100, height + 100,
        1, 1, flags);

    // aiStatusLabel is now drawn in drawTaskbar — keep it off-screen
    if (state->aiStatusLabel) positions = DeferWindowPos(
        positions, state->aiStatusLabel, nullptr, -2000, -2000,
        1, 1, flags);
    if (positions) EndDeferWindowPos(positions);

    // Chat panel: full width, 320px tall, above the Port taskbar
    if (state->chatPanel && IsWindow(state->chatPanel)) {
        const int panelH = 320;
        const int panelY = height - taskbarHeight - panelH;
        SetWindowPos(state->chatPanel, HWND_TOP, 0, panelY, width, panelH,
                     SWP_NOACTIVATE |
                     (state->chatPanelVisible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }

    // PORT AGENTS panel, pinned to the left edge above the taskbar
    if (state->agentPanel && IsWindow(state->agentPanel)) {
        const int panelH = height - taskbarHeight - 24;
        SetWindowPos(state->agentPanel, HWND_TOP, 12, 12,
                     AgentPanelWidth, panelH,
                     SWP_NOACTIVATE | SWP_NOCOPYBITS |
                     (state->agentPanelVisible ? SWP_SHOWWINDOW
                                               : SWP_HIDEWINDOW));
        SetWindowRgn(state->agentPanel,
                     CreateRoundRectRgn(0, 0, AgentPanelWidth + 1,
                                        panelH + 1, 24, 24), TRUE);
    }
    if (state->agentHandle && IsWindow(state->agentHandle)) {
        SetWindowPos(state->agentHandle, HWND_TOP, 0,
                     (height - taskbarHeight - 64) / 2, 22, 64,
                     SWP_NOACTIVATE | SWP_NOCOPYBITS |
                     (state->agentPanelVisible ? SWP_HIDEWINDOW
                                               : SWP_SHOWWINDOW));
    }

    // Agents popup grid — centered above the search pill
    if (state->agentGrid && IsWindow(state->agentGrid)) {
        const SIZE gridSize = agentGridSize(state);
        SetWindowPos(state->agentGrid, HWND_TOP,
                     (width - gridSize.cx) / 2,
                     height - taskbarHeight - gridSize.cy - 10,
                     gridSize.cx, gridSize.cy,
                     SWP_NOACTIVATE | SWP_NOCOPYBITS |
                     (state->agentGridVisible ? SWP_SHOWWINDOW
                                              : SWP_HIDEWINDOW));
        SetWindowRgn(state->agentGrid,
                     CreateRoundRectRgn(0, 0, gridSize.cx + 1,
                                        gridSize.cy + 1, 28, 28), TRUE);
    }
}

// View > Small/Medium/Large: remap every icon (system AND dynamic) from the
// old grid to the new one, then move/resize their windows. layoutDesktop
// only handles the system icons, so without this the folder icons kept
// their old size and position.
void setDesktopIconSize(HWND window, DesktopState* state, IconSizeOption newSize)
{
    if (!state || state->iconSize == newSize) return;

    const auto oldLayout = getIconLayoutInfo(state->iconSize);
    const auto newLayout = getIconLayoutInfo(newSize);

    auto remap = [&](POINT p) -> POINT {
        int col = (p.x - 28 + oldLayout.gapX / 2) / oldLayout.gapX;
        int row = (p.y - 34 + oldLayout.gapY / 2) / oldLayout.gapY;
        if (col < 0) col = 0;
        if (row < 0) row = 0;
        return {28 + col * newLayout.gapX, 34 + row * newLayout.gapY};
    };

    state->myPortPos   = remap(state->myPortPos);
    state->myServerPos = remap(state->myServerPos);
    state->trashPos    = remap(state->trashPos);
    for (auto& di : state->dynamicIcons) di.pos = remap(di.pos);
    for (auto& saved : state->dynamicIconSavedPos) saved.second = remap(saved.second);

    state->iconSize = newSize;
    layoutDesktop(window, state);
    for (auto& di : state->dynamicIcons) {
        if (di.hwnd && IsWindow(di.hwnd)) {
            SetWindowPos(di.hwnd, nullptr, di.pos.x, di.pos.y,
                         newLayout.width, newLayout.height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        }
    }
    InvalidateRect(window, nullptr, TRUE);
}

void drawCenteredLabel(HDC dc, const RECT& bounds, const char* text)
{
    RECT label = bounds;
    label.top = bounds.bottom - 34;

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, PortText);
    DrawTextA(dc, text, -1, &label, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
}

void drawPlaceholderTile(HDC dc, const RECT& bounds, COLORREF color)
{
    const int centerX = (bounds.left + bounds.right) / 2;
    const int top = bounds.top + 8;
    RECT tile = {centerX - 48, top, centerX + 48, top + 92};
    fillRoundRect(dc, tile, 28, color, color);
}

void drawSimpleDesktopGlyph(HDC dc, const RECT& bounds, int controlId)
{
    const int centerX = (bounds.left + bounds.right) / 2;
    const int top = bounds.top + 28;

    HPEN pen = CreatePen(PS_SOLID, 4, RGB(17, 24, 38));
    HBRUSH brush = CreateSolidBrush(RGB(210, 224, 255));
    HBRUSH emptyBrush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));

    SelectObject(dc, pen);
    SelectObject(dc, brush);

    if (false) {
        // (removed: IdIconFoxTerminal no longer exists)
    } else {
        SelectObject(dc, emptyBrush);
        RoundRect(dc, centerX - 28, top, centerX + 28, top + 40, 6, 6);
        MoveToEx(dc, centerX - 10, top + 52, nullptr);
        LineTo(dc, centerX + 10, top + 52);
        MoveToEx(dc, centerX, top + 42, nullptr);
        LineTo(dc, centerX, top + 52);
    }

    DeleteObject(pen);
    DeleteObject(brush);
}

// Draw a GDI+ image at the given rect, optionally faded to `alpha`.
void drawIconImage(Gdiplus::Graphics& g, Gdiplus::Image* img,
                   float x, float y, float w, float h, float alpha)
{
    if (!img) return;
    if (alpha >= 1.0f) {
        g.DrawImage(img, x, y, w, h);
        return;
    }
    Gdiplus::ColorMatrix cm = {};
    cm.m[0][0] = cm.m[1][1] = cm.m[2][2] = 1.0f;
    cm.m[3][3] = alpha;
    cm.m[4][4] = 1.0f;
    Gdiplus::ImageAttributes attr;
    attr.SetColorMatrix(&cm);
    const Gdiplus::Rect dst(static_cast<INT>(x), static_cast<INT>(y),
                            static_cast<INT>(w), static_cast<INT>(h));
    g.DrawImage(img, dst, 0, 0, img->GetWidth(), img->GetHeight(),
                Gdiplus::UnitPixel, &attr);
}

void drawDesktopIcon(const DRAWITEMSTRUCT* item)
{
    HDC screenDC = item->hDC;
    RECT bounds = item->rcItem;
    const int bufW = bounds.right - bounds.left;
    const int bufH = bounds.bottom - bounds.top;

    // Render the whole icon into an off-screen buffer and blit it once at
    // the end. Painting layer-by-layer straight to the screen let forced
    // repaints catch half-drawn frames, which read as flicker during drags.
    HDC dc = CreateCompatibleDC(screenDC);
    HBITMAP buffer = CreateCompatibleBitmap(screenDC, bufW, bufH);
    HGDIOBJ oldBuffer = SelectObject(dc, buffer);

    if (cachedBackground) {
        POINT btnTopLeft = {0, 0};
        ClientToScreen(item->hwndItem, &btnTopLeft);
        ScreenToClient(GetParent(item->hwndItem), &btnTopLeft);

        HDC memDC = CreateCompatibleDC(dc);
        HGDIOBJ oldBitmap = SelectObject(memDC, cachedBackground);
        BitBlt(dc, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, memDC, btnTopLeft.x, btnTopLeft.y, SRCCOPY);
        SelectObject(memDC, oldBitmap);
        DeleteDC(memDC);
    } else {
        HBRUSH background = CreateSolidBrush(DesktopBackground);
        FillRect(dc, &bounds, background);
        DeleteObject(background);
    }

    HWND parent = GetParent(item->hwndItem);
    DesktopState* state = reinterpret_cast<DesktopState*>(GetWindowLongPtrA(parent, GWLP_USERDATA));

    // Use only Port's explicit selection state. ODS_SELECTED is the temporary
    // Win32 push state and can make owner-drawn buttons look selected together.
    // While this icon is being renamed in place, its own label stays hidden
    // — otherwise it shows through under the rename box.
    const bool isRenaming = state && state->renameEdit &&
        state->renameIconId == static_cast<int>(item->CtlID);

    bool isSelected = state && state->selectedIconId == static_cast<int>(item->CtlID);
    bool isBeingDragged = (state && state->isDraggingIcon && static_cast<unsigned int>(state->draggedIconId) == item->CtlID);

    // Dragged icons fade via a layered window (see setIconDragGhost), which
    // also lets the icons behind show through — so the image itself is drawn
    // at full opacity here.
    const float dragAlpha = 1.0f;

    const bool isInSelectionRect = state &&
        std::find(state->dragSelectedIconIds.begin(),
                  state->dragSelectedIconIds.end(),
                  static_cast<int>(item->CtlID)) != state->dragSelectedIconIds.end();

    HFONT fontToUse = (state && state->labelFont) ? state->labelFont : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, fontToUse));

    int imgSize = 96;
    if (state) {
        auto layoutInfo = getIconLayoutInfo(state->iconSize);
        imgSize = layoutInfo.imageSize;
    }
    float imgW = static_cast<float>(imgSize);
    float imgH = imgW * 92.0f / 96.0f;

    // Selection is a background-only state. Drawing it before the image and
    // label keeps their size, color, and shape completely unchanged.
    if (isSelected || isBeingDragged || isInSelectionRect) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        const int boundsWidth = bounds.right - bounds.left;
        const int tileWidth = std::min(boundsWidth - 8, imgSize + 12);
        const int x = bounds.left + (boundsWidth - tileWidth) / 2;
        const int y = bounds.top + 6;
        const int w = tileWidth;
        const int h = (bounds.bottom - bounds.top) - 11;
        const int r = 14;

        Gdiplus::GraphicsPath path;
        path.AddArc(x, y, r, r, 180, 90);
        path.AddArc(x + w - r, y, r, r, 270, 90);
        path.AddArc(x + w - r, y + h - r, r, r, 0, 90);
        path.AddArc(x, y + h - r, r, r, 90, 90);
        path.CloseFigure();

        // Neutral gray at roughly 18% opacity keeps the wallpaper visible.
        // tinting the icon blue or changing the artwork itself.
        Gdiplus::SolidBrush selectedBackground(Gdiplus::Color(46, 92, 92, 92));
        graphics.FillPath(&selectedBackground, &path);

        // Accent border so the selection reads clearly against the wallpaper
        Gdiplus::Pen selectedBorder(Gdiplus::Color(210, 90, 160, 255), 1.6f);
        graphics.DrawPath(&selectedBorder, &path);
    }

    if (item->CtlID == IdIconMyServer) {
        if (serverIconImg) {
            const int centerX = (bounds.left + bounds.right) / 2;
            const int top = bounds.top + 8;
            Gdiplus::Graphics graphics(dc);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            drawIconImage(graphics, serverIconImg, centerX - imgW / 2.0f,
                          static_cast<float>(top), imgW, imgH, dragAlpha);
        } else {
            // Procedural 3D server icon fallback
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            const float cx  = static_cast<float>((bounds.left + bounds.right) / 2);
            const float top = static_cast<float>(bounds.top + 10);
            const float sc  = imgW / 96.0f;

            // Front face
            const float fw = 34.0f * sc, fh = 68.0f * sc;
            const float ox = 13.0f * sc, oy = -10.0f * sc;  // isometric depth

            // Right face
            Gdiplus::PointF right[4] = {
                {cx + fw,      top},
                {cx + fw + ox, top + oy},
                {cx + fw + ox, top + fh + oy},
                {cx + fw,      top + fh}
            };
            Gdiplus::SolidBrush rBrush(Gdiplus::Color(255, 26, 30, 42));
            g.FillPolygon(&rBrush, right, 4);

            // Top face
            Gdiplus::PointF topF[4] = {
                {cx - fw,      top},
                {cx - fw + ox, top + oy},
                {cx + fw + ox, top + oy},
                {cx + fw,      top}
            };
            Gdiplus::SolidBrush tBrush(Gdiplus::Color(255, 70, 76, 94));
            g.FillPolygon(&tBrush, topF, 4);

            // Front face
            Gdiplus::SolidBrush fBrush(Gdiplus::Color(255, 44, 48, 62));
            g.FillRectangle(&fBrush, cx - fw, top, fw * 2.0f, fh);

            // 3 rack units
            const float rackH  = 14.0f * sc;
            const float rackGap = 5.0f * sc;
            for (int ri = 0; ri < 3; ++ri) {
                float ry = top + 6.0f * sc + ri * (rackH + rackGap);
                Gdiplus::SolidBrush slotBrush(Gdiplus::Color(255, 18, 21, 30));
                g.FillRectangle(&slotBrush, cx - fw + 4.0f * sc, ry,
                                (fw * 2.0f) - 8.0f * sc, rackH);
                // Green activity LED
                float ledR = 3.0f * sc;
                float ledX = cx + fw - 12.0f * sc, ledY = ry + rackH / 2.0f;
                Gdiplus::SolidBrush gLed(Gdiplus::Color(255, 0, 220, 80));
                g.FillEllipse(&gLed, ledX - ledR, ledY - ledR, ledR * 2.0f, ledR * 2.0f);
                // Blue status LED
                float bLedX = ledX - 9.0f * sc;
                Gdiplus::SolidBrush bLed(Gdiplus::Color(255, 0, 150, 255));
                g.FillEllipse(&bLed, bLedX - ledR, ledY - ledR, ledR * 2.0f, ledR * 2.0f);
            }

            // Edge highlights
            Gdiplus::Pen ePen(Gdiplus::Color(180, 96, 102, 120), 1.2f);
            g.DrawLine(&ePen, cx - fw, top, cx + fw, top);
            g.DrawLine(&ePen, cx - fw, top, cx - fw, top + fh);
            Gdiplus::Pen tPen(Gdiplus::Color(140, 74, 80, 96), 1.0f);
            g.DrawLine(&tPen, cx - fw, top, cx - fw + ox, top + oy);
            g.DrawLine(&tPen, cx - fw + ox, top + oy, cx + fw + ox, top + oy);
        }
        if (!isRenaming) drawCenteredLabel(dc, bounds,
            state ? state->myServerLabel.c_str() : "My Server");
    } else if (item->CtlID == IdIconMyComputer) {
        if (myPortIconImg) {
            const int centerX = (bounds.left + bounds.right) / 2;
            const int top = bounds.top + 8;
            Gdiplus::Graphics graphics(dc);
            drawIconImage(graphics, myPortIconImg, centerX - imgW / 2.0f,
                          static_cast<float>(top), imgW, imgH, dragAlpha);
        } else {
            drawPlaceholderTile(dc, bounds, PortBlue);
            drawSimpleDesktopGlyph(dc, bounds, IdIconMyComputer);
        }
        drawCenteredLabel(dc, bounds, "My Port");
    } else if (item->CtlID == IdIconTrashBin) {
        Gdiplus::Image* imgToDraw = nullptr;
        if (state) {
            auto trashResult = state->kernel.getSandbox().listTrash();
            bool isEmpty = !trashResult || trashResult->empty();
            imgToDraw = isEmpty ? trashEmptyIconImg : trashFullIconImg;
        } else {
            imgToDraw = trashEmptyIconImg;
        }

        if (imgToDraw) {
            const int centerX = (bounds.left + bounds.right) / 2;
            const int top = bounds.top + 8;
            Gdiplus::Graphics graphics(dc);
            drawIconImage(graphics, imgToDraw, centerX - imgW / 2.0f,
                          static_cast<float>(top), imgW, imgH, dragAlpha);
        } else {
            drawPlaceholderTile(dc, bounds, PortRed);
            drawSimpleDesktopGlyph(dc, bounds, IdIconTrashBin);
        }
        if (!isRenaming) drawCenteredLabel(dc, bounds,
            state ? state->trashLabel.c_str() : "Trash Bin");
    } else if (item->CtlID >= 1100) {
        // Dynamic desktop icon!
        bool isDir = false;
        std::string displayName = "Folder";
        if (state) {
            for (const auto& di : state->dynamicIcons) {
                if (di.id == static_cast<int>(item->CtlID)) {
                    isDir = di.isDir;
                    displayName = di.name;
                    break;
                }
            }
        }

        if (isDir) {
            if (folderIconImg) {
                const int centerX = (bounds.left + bounds.right) / 2;
                const int top = bounds.top + 8;
                Gdiplus::Graphics graphics(dc);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                drawIconImage(graphics, folderIconImg, centerX - imgW / 2.0f,
                              static_cast<float>(top), imgW, imgH, dragAlpha);
            } else {
                drawPlaceholderTile(dc, bounds, PortBlue);
            }
        } else {
            // Draw a file tile
            drawPlaceholderTile(dc, bounds, RGB(144, 202, 249));
            const int centerX = (bounds.left + bounds.right) / 2;
            const int top = bounds.top + 28;
            HPEN pen = CreatePen(PS_SOLID, 3, RGB(17, 24, 38));
            HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
            SelectObject(dc, pen);
            SelectObject(dc, brush);
            RECT fileRect = {centerX - 16, top + 6, centerX + 16, top + 34};
            RoundRect(dc, fileRect.left, fileRect.top, fileRect.right, fileRect.bottom, 4, 4);
            DeleteObject(pen);
            DeleteObject(brush);
        }
        if (!isRenaming) drawCenteredLabel(dc, bounds, displayName.c_str());
    }

    // ── Drop-target overlay while this icon is being dragged ─────
    if (isBeingDragged && state) {
        Gdiplus::Graphics og(dc);
        og.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        if (state->dragHoverBlocked) {
            // Red prohibition sign: circle + diagonal bar ("not allowed")
            const int cx = (bounds.left + bounds.right) / 2;
            const int cy = bounds.top + 8 + imgSize / 2;
            const int r  = 18;
            Gdiplus::SolidBrush disc(Gdiplus::Color(120, 30, 8, 12));
            og.FillEllipse(&disc, cx - r, cy - r, r * 2, r * 2);
            Gdiplus::Pen rp(Gdiplus::Color(235, 225, 45, 60), 5.0f);
            og.DrawEllipse(&rp, cx - r, cy - r, r * 2, r * 2);
            const int d = static_cast<int>(r * 0.66f);
            og.DrawLine(&rp, cx - d, cy + d, cx + d, cy - d);
        } else if (!state->dragHoverText.empty()) {
            // Small "Move to <target>" pill under the icon image
            RECT tm = {0, 0, 0, 0};
            DrawTextA(dc, state->dragHoverText.c_str(), -1, &tm,
                      DT_CALCRECT | DT_SINGLELINE);
            int pw = std::min<int>(tm.right + 20,
                                   bounds.right - bounds.left - 6);
            const int ph = 22;
            const int px = bounds.left +
                           ((bounds.right - bounds.left) - pw) / 2;
            const int py = bounds.bottom - ph - 2;

            Gdiplus::GraphicsPath pill;
            addRoundRect(pill, px, py, pw, ph, ph / 2);
            Gdiplus::SolidBrush pf(Gdiplus::Color(235, 18, 26, 48));
            og.FillPath(&pf, &pill);
            Gdiplus::Pen pp(Gdiplus::Color(200, 90, 150, 255), 1.2f);
            og.DrawPath(&pp, &pill);

            SetTextColor(dc, RGB(210, 225, 255));
            RECT tr = {px + 4, py, px + pw - 4, py + ph};
            DrawTextA(dc, state->dragHoverText.c_str(), -1, &tr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    // Rubber-band selection passing over this icon: draw its portion into
    // the icon's own buffer so the band appears on top of the icon.
    if (state && state->isDragging && state->hasPrevSelRect) {
        POINT btnTopLeft = {0, 0};
        ClientToScreen(item->hwndItem, &btnTopLeft);
        ScreenToClient(parent, &btnTopLeft);
        RECT sel = state->prevSelRect;
        OffsetRect(&sel, -btnTopLeft.x, -btnTopLeft.y);
        Gdiplus::Graphics g(dc);
        drawSelectionBand(g, sel);
    }

    SelectObject(dc, oldFont);

    BitBlt(screenDC, bounds.left, bounds.top, bufW, bufH, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBuffer);
    DeleteObject(buffer);
    DeleteDC(dc);
}

void updateCachedBackground(HWND window, int width, int height)
{
    if (width <= 0 || height <= 0) return;

    if (cachedBackground) {
        DeleteObject(cachedBackground);
        cachedBackground = nullptr;
    }

    HDC screenDC = GetDC(window);
    HDC memDC = CreateCompatibleDC(screenDC);
    cachedBackground = CreateCompatibleBitmap(screenDC, width, height);
    HGDIOBJ oldBitmap = SelectObject(memDC, cachedBackground);

    RECT rect = {0, 0, width, height};

    if (desktopWallpaper) {
        Gdiplus::Graphics graphics(memDC);
        graphics.DrawImage(desktopWallpaper, 0.0f, 0.0f, static_cast<Gdiplus::REAL>(width), static_cast<Gdiplus::REAL>(height));
    } else {
        HBRUSH bg = CreateSolidBrush(DesktopBackground);
        FillRect(memDC, &rect, bg);
        DeleteObject(bg);

        HBRUSH orbBrush = CreateSolidBrush(DesktopOrb);
        HPEN orbPen = CreatePen(PS_SOLID, 1, DesktopOrb);
        SelectObject(memDC, orbBrush);
        SelectObject(memDC, orbPen);
        Ellipse(memDC, -340, rect.bottom - 360, 560, rect.bottom + 540);
        Ellipse(memDC, rect.right - 690, -420, rect.right + 140, 410);
        DeleteObject(orbBrush);
        DeleteObject(orbPen);

        HBRUSH blueBrush = CreateSolidBrush(RGB(93, 130, 244));
        HPEN bluePen = CreatePen(PS_SOLID, 1, RGB(93, 130, 244));
        SelectObject(memDC, blueBrush);
        SelectObject(memDC, bluePen);
        Ellipse(memDC, rect.right - 90, -210, rect.right + 170, 430);
        DeleteObject(blueBrush);
        DeleteObject(bluePen);
    }

    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(window, screenDC);
}

// Dock tiles: My Server plus every folder created on the desktop. Used by
// both drawTaskbar (rendering) and the desktop click handler (hit-testing)
// so the two always agree on geometry.
struct DockTile {
    RECT rect;
    int iconId;         // IdIconMyServer or a dynamic icon id
    bool isServer;
    std::string name;
    int agentIndex;     // >= 0: a running agent window's taskbar tile
};

std::vector<DockTile> computeDockTiles(DesktopState* state, int W, int H)
{
    std::vector<DockTile> tiles;
    if (!state) return tiles;

    // Leftmost: the "Agents" folder (iconId -1 opens the agent grid popup)
    tiles.push_back({{}, -1, false, "Agents", -1});
    tiles.push_back({{}, IdIconMyServer, true, state->myServerLabel, -1});
    for (const auto& di : state->dynamicIcons) {
        if (di.isDir) tiles.push_back({{}, di.id, false, di.name, -1});
    }
    // Running agent windows, like open apps on the Windows taskbar
    for (const auto& openWindow : state->agentWindows) {
        if (!IsWindow(openWindow.second)) continue;
        if (openWindow.first < 0 ||
            openWindow.first >= static_cast<int>(state->agents.size())) {
            continue;
        }
        tiles.push_back({{}, 0, false,
                         state->agents[openWindow.first].name,
                         openWindow.first});
    }

    constexpr int tileSize = 54;
    constexpr int tileGap = 14;
    // Keep the dock centered; drop trailing tiles if they'd collide with
    // the PORT OS label (left) or the clock (right).
    const int maxWidth = W - 2 * 200;
    while (tiles.size() > 1 &&
           static_cast<int>(tiles.size()) * (tileSize + tileGap) - tileGap >
               maxWidth) {
        tiles.pop_back();
    }

    const int totalW =
        static_cast<int>(tiles.size()) * (tileSize + tileGap) - tileGap;
    int x = (W - totalW) / 2;
    const int y = H - kDockH + (kDockH - tileSize) / 2;
    for (auto& tile : tiles) {
        tile.rect = {x, y, x + tileSize, y + tileSize};
        x += tileSize + tileGap;
    }
    return tiles;
}

void drawTaskbar(HWND window, HDC dc, DesktopState* state)
{
    RECT rect;
    GetClientRect(window, &rect);
    const int W = rect.right;

    SetBkMode(dc, TRANSPARENT);

    // ── Search pill — floats right above the dock bar ─────────────
    const int pillLeft   = 16;
    const int pillRight  = W - 16;
    const int pillBottom = rect.bottom - kDockH - 8;
    const int pillTop    = pillBottom - 48;
    const int pillH      = pillBottom - pillTop;
    const int pillW      = pillRight - pillLeft;
    const int pillR      = pillH / 2;  // fully rounded ends

    // Pulsing glow border when AI is running
    bool aiRunning = state && state->isAIRunning;
    int  phase     = state ? state->pulsePhase : 0;
    // triangle wave 0→1→0 over 30 ticks
    float t = (phase < 15) ? (phase / 15.0f) : ((30 - phase) / 15.0f);

    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // Outer glow (only when AI running)
        if (aiRunning) {
            int glowAlpha = static_cast<int>(30 + 90 * t);
            int glowExpand = static_cast<int>(2 + 5 * t);
            Gdiplus::GraphicsPath glowPath;
            addRoundRect(glowPath,
                pillLeft - glowExpand, pillTop - glowExpand,
                pillW + glowExpand * 2, pillH + glowExpand * 2,
                pillR + glowExpand);
            Gdiplus::SolidBrush glowBrush(Gdiplus::Color(
                static_cast<BYTE>(glowAlpha), 100, 160, 255));
            g.FillPath(&glowBrush, &glowPath);
        }

        // Pill fill — dark blue-charcoal
        Gdiplus::GraphicsPath pillPath;
        addRoundRect(pillPath, pillLeft, pillTop, pillW, pillH, pillR);
        Gdiplus::SolidBrush pillFill(Gdiplus::Color(255, 16, 22, 38));
        g.FillPath(&pillFill, &pillPath);

        // Pill border
        BYTE borderA = aiRunning ? static_cast<BYTE>(160 + static_cast<int>(80 * t)) : 70;
        BYTE borderR = aiRunning ? static_cast<BYTE>(80  + static_cast<int>(80 * t)) : 55;
        BYTE borderG = aiRunning ? static_cast<BYTE>(140 + static_cast<int>(60 * t)) : 65;
        BYTE borderB = aiRunning ? static_cast<BYTE>(255)                             : 110;
        Gdiplus::Pen pillPen(Gdiplus::Color(borderA, borderR, borderG, borderB), 1.5f);
        g.DrawPath(&pillPen, &pillPath);

        // ── Cosmic orb on the left (icon.png) ─────────────────────
        int orbX = pillLeft + pillR;
        int orbY = pillTop + pillH / 2;
        int orbR = 16;

        if (taskbarOrbImg) {
            // Soft glow behind the orb — brighter while AI runs
            BYTE haloA = aiRunning ? static_cast<BYTE>(60 + static_cast<int>(60 * t)) : 45;
            Gdiplus::GraphicsPath halo;
            addRoundRect(halo, orbX - orbR - 4, orbY - orbR - 4,
                         (orbR + 4) * 2, (orbR + 4) * 2, orbR + 4);
            Gdiplus::SolidBrush haloBrush(Gdiplus::Color(haloA, 110, 90, 255));
            g.FillPath(&haloBrush, &halo);

            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(taskbarOrbImg,
                        orbX - orbR, orbY - orbR, orbR * 2, orbR * 2);
        } else {
            // Fallback: drawn circle with "P"
            Gdiplus::GraphicsPath orbPath;
            addRoundRect(orbPath, orbX - orbR, orbY - orbR, orbR * 2, orbR * 2, orbR);
            Gdiplus::SolidBrush orbBg(Gdiplus::Color(255, 20, 30, 60));
            g.FillPath(&orbBg, &orbPath);
            Gdiplus::Pen orbRing(Gdiplus::Color(140, 90, 160, 255), 2.0f);
            g.DrawPath(&orbRing, &orbPath);
            HFONT oldFont = static_cast<HFONT>(
                SelectObject(dc, state && state->tbOrbFont ? state->tbOrbFont : GetStockObject(DEFAULT_GUI_FONT)));
            SetTextColor(dc, RGB(120, 180, 255));
            RECT pRect = {orbX - orbR, orbY - orbR, orbX + orbR, orbY + orbR};
            DrawTextA(dc, "P", -1, &pRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldFont);
        }

        // Thin vertical separator after the orb (like the reference design)
        Gdiplus::Pen orbSep(Gdiplus::Color(60, 130, 140, 200), 1.0f);
        g.DrawLine(&orbSep, orbX + orbR + 12, pillTop + 12,
                            orbX + orbR + 12, pillBottom - 12);
    }

    // ── Text inside pill ─────────────────────────────────────────
    const int textLeft  = pillLeft + pillR * 2 + 12;
    const int rSecW     = 50;    // just the magnifying glass
    const int rSecX     = pillRight - rSecW;
    const int textRight = rSecX - 8;

    HFONT mainFont = state && state->tbMainFont ? state->tbMainFont
                     : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SelectObject(dc, mainFont);

    const bool hasStatus = state && !state->aiStatusMsg.empty();
    const bool isAborted = hasStatus &&
        state->aiStatusMsg.rfind("Port Aborted", 0) == 0;

    RECT textRect = {textLeft, pillTop, textRight, pillBottom};

    if (aiRunning) {
        // "Super-Nova 2.0 inbound" with animated dots
        static const char* dotFrames[] = {"", ".", "..", "..."};
        int dotIdx = (phase / 7) % 4;
        char runText[64];
        wsprintfA(runText, "Super-Nova 1.0 inbound%s", dotFrames[dotIdx]);
        // Pulsing cyan-white color
        BYTE textB = static_cast<BYTE>(200 + static_cast<int>(55 * t));
        BYTE textG = static_cast<BYTE>(170 + static_cast<int>(55 * t));
        SetTextColor(dc, RGB(120, textG, textB));
        DrawTextA(dc, runText, -1, &textRect, DT_VCENTER | DT_LEFT | DT_SINGLELINE);
    } else if (isAborted) {
        SetTextColor(dc, RGB(255, 75, 75));
        DrawTextA(dc, state->aiStatusMsg.c_str(), -1, &textRect,
                  DT_VCENTER | DT_LEFT | DT_SINGLELINE);
    } else {
        // Placeholder is shown via EM_SETCUEBANNER on the edit control
    }

    // ── Active agent badge — set from the panel or via /name ─────
    if (state && state->activeAgent >= 0 &&
        state->activeAgent < static_cast<int>(state->agents.size())) {
        const auto& agent = state->agents[state->activeAgent];
        HFONT badgeFont = state->tbDateFont
                              ? state->tbDateFont
                              : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SelectObject(dc, badgeFont);
        SetTextColor(dc, agent.accent);
        const std::string label = agent.name + " active";
        RECT agentRect = {rSecX - 235, pillTop, rSecX - 140, pillBottom};
        DrawTextA(dc, label.c_str(), -1, &agentRect,
                  DT_VCENTER | DT_RIGHT | DT_SINGLELINE);
    }

    // ── Active engine badge — which provider answered last ───────
    if (gAIManager) {
        const std::string engine = gAIManager->activeProviderName();
        if (!engine.empty()) {
            HFONT badgeFont = state && state->tbDateFont ? state->tbDateFont
                              : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            SelectObject(dc, badgeFont);
            SetTextColor(dc, RGB(105, 200, 140));
            const std::string label = "engine: " + engine;
            // Sits between the input field and the magnifier — layoutDesktop
            // reserves this strip so the edit control never covers it.
            RECT badgeRect = {rSecX - 130, pillTop, rSecX - 10, pillBottom};
            DrawTextA(dc, label.c_str(), -1, &badgeRect,
                      DT_VCENTER | DT_RIGHT | DT_SINGLELINE);
        }
    }

    // ── Right section: magnifying glass ──────────────────────────
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        int mx = pillRight - pillR;          // center of the glass lens
        int my = pillTop + pillH / 2 - 2;
        int lensR = 7;

        BYTE mgA = aiRunning ? static_cast<BYTE>(180 + static_cast<int>(75 * t)) : 150;
        Gdiplus::Pen mgPen(Gdiplus::Color(mgA, 150, 160, 190), 2.0f);
        g.DrawEllipse(&mgPen, mx - lensR, my - lensR, lensR * 2, lensR * 2);
        // Handle at 45° down-right
        g.DrawLine(&mgPen, mx + lensR - 2, my + lensR - 2, mx + lensR + 5, my + lensR + 5);
    }

    // ── Dock bar — frosted glass over the wallpaper ─────────────────
    const int dockTop = rect.bottom - kDockH;

    // Blurred wallpaper strip: downscale 1:8 and stretch back with
    // HALFTONE — a cheap box blur, so the photo "fades" through the bar.
    if (cachedBackground) {
        HDC src = CreateCompatibleDC(dc);
        HGDIOBJ oldSrc = SelectObject(src, cachedBackground);
        const int smallW = std::max(1, W / 8);
        const int smallH = std::max(1, kDockH / 8);
        HDC tmp = CreateCompatibleDC(dc);
        HBITMAP tmpBmp = CreateCompatibleBitmap(dc, smallW, smallH);
        HGDIOBJ oldTmp = SelectObject(tmp, tmpBmp);
        SetStretchBltMode(tmp, HALFTONE);
        SetBrushOrgEx(tmp, 0, 0, nullptr);
        StretchBlt(tmp, 0, 0, smallW, smallH,
                   src, 0, dockTop, W, kDockH, SRCCOPY);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, 0, dockTop, W, kDockH,
                   tmp, 0, 0, smallW, smallH, SRCCOPY);
        SelectObject(tmp, oldTmp);
        DeleteObject(tmpBmp);
        DeleteDC(tmp);
        SelectObject(src, oldSrc);
        DeleteDC(src);
    } else {
        RECT dockRect = {0, dockTop, W, rect.bottom};
        HBRUSH dockBg = CreateSolidBrush(RGB(24, 32, 54));
        FillRect(dc, &dockRect, dockBg);
        DeleteObject(dockBg);
    }

    // Compute tiles up front; GDI+ work happens in one scoped block so the
    // Graphics object is gone before any GDI text call (a live Graphics on
    // the same DC swallows GDI text output).
    std::vector<DockTile> tiles;
    if (state) tiles = computeDockTiles(state, W, rect.bottom);

    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        // Light translucent tint over the blur + a soft top edge
        Gdiplus::SolidBrush tint(Gdiplus::Color(120, 30, 40, 66));
        g.FillRectangle(&tint, 0, dockTop, W, kDockH);
        Gdiplus::Pen topEdge(Gdiplus::Color(90, 130, 150, 205), 1.0f);
        g.DrawLine(&topEdge, 0, dockTop, W, dockTop);

        // Left: transparent orb logo (falls back to the old icon)
        const int orbSize = 36;
        Gdiplus::Image* logo = dockLogoImg ? dockLogoImg : taskbarOrbImg;
        if (logo) {
            g.DrawImage(logo, 14, dockTop + (kDockH - orbSize) / 2,
                        orbSize, orbSize);
        }

        // Center: tiles (translucent, lighter than before)
        for (std::size_t ti = 0; ti < tiles.size(); ++ti) {
            const auto& tile = tiles[ti];
            const int tw = tile.rect.right - tile.rect.left;
            const int th = tile.rect.bottom - tile.rect.top;

            // Hover highlight behind the icon, like the Windows taskbar
            if (state && static_cast<int>(ti) == state->hotDockTile) {
                Gdiplus::GraphicsPath hp;
                addRoundRect(hp, tile.rect.left - 3, tile.rect.top + 2,
                             tw + 6, th - 4, 8);
                Gdiplus::SolidBrush hf(Gdiplus::Color(38, 255, 255, 255));
                g.FillPath(&hf, &hp);
            }

            // Accent bar under a tile whose window is currently open
            auto runningBar = [&](COLORREF c) {
                Gdiplus::SolidBrush bar(Gdiplus::Color(
                    255, GetRValue(c), GetGValue(c), GetBValue(c)));
                const int barW = 16;
                g.FillRectangle(&bar, tile.rect.left + (tw - barW) / 2,
                                tile.rect.bottom - 5, barW, 3);
            };

            if (tile.agentIndex >= 0 && state) {
                // Running agent window: medallion + accent "running" bar
                const auto& agent = state->agents[tile.agentIndex];
                Gdiplus::Image* img = loadAgentImage(agent.name);
                if (img) {
                    const int imgSize = 42;
                    g.DrawImage(img,
                                tile.rect.left + (tw - imgSize) / 2,
                                tile.rect.top + (th - imgSize) / 2 - 2,
                                imgSize, imgSize);
                }
                runningBar(agent.accent);
                continue;
            }
            if (tile.iconId == -1) {
                // "Agents" folder: 2x2 dots in the first agents' accents
                const int cx0 = tile.rect.left + tw / 2;
                const int cy0 = tile.rect.top + th / 2;
                for (int d = 0; d < 4; ++d) {
                    COLORREF accent = RGB(120, 160, 230);
                    if (state && d < static_cast<int>(state->agents.size())) {
                        accent = state->agents[d].accent;
                    }
                    Gdiplus::SolidBrush dot(Gdiplus::Color(
                        255, GetRValue(accent), GetGValue(accent),
                        GetBValue(accent)));
                    const int dx = cx0 + ((d % 2) ? 3 : -11);
                    const int dy = cy0 + ((d / 2) ? 3 : -11);
                    g.FillEllipse(&dot, dx, dy, 8, 8);
                }
                continue;
            }
            Gdiplus::Image* img = tile.isServer ? serverIconImg : folderIconImg;
            if (img) {
                const int imgSize = 42;
                g.DrawImage(img,
                            tile.rect.left + (tw - imgSize) / 2,
                            tile.rect.top + (th - imgSize) / 2,
                            imgSize, imgSize);
            }
            // My Server: running bar when its window is open
            if (tile.isServer && state && state->myServerWindow &&
                IsWindow(state->myServerWindow)) {
                runningBar(RGB(120, 180, 255));
            }
            // Desktop folder: running bar when its browser window is open
            if (!tile.isServer && tile.iconId >= 1100 && state) {
                const auto fw = state->folderWindows.find(tile.iconId);
                if (fw != state->folderWindows.end() &&
                    IsWindow(fw->second)) {
                    runningBar(RGB(255, 200, 90));
                }
            }
        }

        // Right: network + volume glyphs, left of the clock
        const int cy = dockTop + kDockH / 2;
        Gdiplus::Pen glyphPen(Gdiplus::Color(230, 205, 215, 235), 1.8f);

        // Wi-Fi: three arcs opening upward + a dot
        {
            const int wx = W - 210;
            const int wy = cy + 7;
            for (int r = 4; r <= 12; r += 4) {
                g.DrawArc(&glyphPen, wx - r, wy - r, r * 2, r * 2,
                          -135.0f, 90.0f);
            }
            Gdiplus::SolidBrush dot(Gdiplus::Color(230, 205, 215, 235));
            g.FillEllipse(&dot, wx - 2, wy - 2, 4, 4);
        }

        // Volume: speaker body + sound arc
        {
            const int vx = W - 172;
            Gdiplus::PointF speaker[6] = {
                {static_cast<Gdiplus::REAL>(vx - 8), static_cast<Gdiplus::REAL>(cy - 3)},
                {static_cast<Gdiplus::REAL>(vx - 4), static_cast<Gdiplus::REAL>(cy - 3)},
                {static_cast<Gdiplus::REAL>(vx + 1), static_cast<Gdiplus::REAL>(cy - 8)},
                {static_cast<Gdiplus::REAL>(vx + 1), static_cast<Gdiplus::REAL>(cy + 8)},
                {static_cast<Gdiplus::REAL>(vx - 4), static_cast<Gdiplus::REAL>(cy + 3)},
                {static_cast<Gdiplus::REAL>(vx - 8), static_cast<Gdiplus::REAL>(cy + 3)},
            };
            Gdiplus::SolidBrush speakerFill(Gdiplus::Color(230, 205, 215, 235));
            g.FillPolygon(&speakerFill, speaker, 6);
            g.DrawArc(&glyphPen, vx + 2, cy - 7, 12, 14, -60.0f, 120.0f);
        }
    }

    // Brand text — drawn after the Graphics block so GDI text renders
    {
        HFONT brandFont = state && state->tbMainFont
                              ? state->tbMainFont
                              : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SelectObject(dc, brandFont);
        SetTextColor(dc, RGB(222, 230, 246));
        RECT brandRect = {14 + 36 + 10, dockTop, 220, rect.bottom};
        DrawTextA(dc, "PORT", -1, &brandRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // Clock + date, rightmost
    {
        SYSTEMTIME localTime;
        GetLocalTime(&localTime);
        char timeText[16], dateText[16];
        wsprintfA(timeText, "%02d:%02d", localTime.wHour, localTime.wMinute);
        wsprintfA(dateText, "%02d/%02d/%04d", localTime.wDay, localTime.wMonth, localTime.wYear);

        HFONT clockFont = state && state->tbClockFont ? state->tbClockFont
                          : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HFONT dateFont  = state && state->tbDateFont  ? state->tbDateFont
                          : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        SelectObject(dc, clockFont);
        SetTextColor(dc, RGB(228, 236, 255));
        RECT timeR = {W - 140, dockTop + 10, W - 16, dockTop + 34};
        DrawTextA(dc, timeText, -1, &timeR, DT_RIGHT | DT_TOP | DT_SINGLELINE);

        SelectObject(dc, dateFont);
        SetTextColor(dc, RGB(165, 175, 200));
        RECT dateR = {W - 140, dockTop + 36, W - 16, rect.bottom - 6};
        DrawTextA(dc, dateText, -1, &dateR, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<DesktopState*>(GetWindowLongPtrA(window, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* createdState = new DesktopState();
        createdState->myPortWindow        = nullptr;
        createdState->trashWindow         = nullptr;
        createdState->myServerWindow      = nullptr;
        createdState->aiStatusLabel       = nullptr;
        createdState->iconSize            = IconSizeOption::Medium;
        createdState->isDragging          = false;
        createdState->dragStart           = {0, 0};
        createdState->dragEnd             = {0, 0};
        createdState->isDraggingIcon      = false;
        createdState->iconDragMoved       = false;
        createdState->lastIconDragPaint   = 0;
        createdState->draggedIconId       = 0;
        createdState->selectedIconId      = 0;
        createdState->iconDragOffset      = {0, 0};
        createdState->hasPrevSelRect      = false;
        createdState->prevSelRect         = {};
        createdState->pulsePhase          = 0;
        createdState->isAIRunning         = false;
        createdState->statusClearCountdown = 0;

        // Initialize default icon positions.
        // "My Port" was removed — this is an app, not a full OS; the sandbox
        // is reachable through AI commands. Only Trash Bin + My Server remain.
        const auto initialLayout = getIconLayoutInfo(createdState->iconSize);
        createdState->myPortPos   = {28, 34};
        createdState->myServerPos = {28, 34};
        createdState->trashPos    = {28, 34 + initialLayout.gapY};

        SetWindowLongPtrA(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createdState));

        createDesktopButton(window, IdIconMyServer, "My Server",
                            createdState->myServerPos.x, createdState->myServerPos.y);
        createDesktopButton(window, IdIconTrashBin, "Trash Bin",
                            createdState->trashPos.x, createdState->trashPos.y);

        createdState->askFont = CreateFontA(
            20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI"
        );
        createdState->uiFont = CreateFontA(
            16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI"
        );
        createdState->labelFont = CreateFontA(
            15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI"
        );

        createdState->output = CreateWindowExA(
            WS_EX_CLIENTEDGE,
            "EDIT",
            "",
            WS_CHILD | ES_LEFT | ES_MULTILINE | ES_READONLY,
            -100,
            -100,
            1,
            1,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdTerminalOutput)),
            GetModuleHandleA(nullptr),
            nullptr
        );

        createdState->input = CreateWindowExA(
            0,
            "EDIT",
            "",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
            160,
            400,
            480,
            28,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdTerminalInput)),
            GetModuleHandleA(nullptr),
            nullptr
        );

        // Subclass the Edit control to capture Enter key presses
        WNDPROC originalEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(createdState->input, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(editSubclassProc)));
        SetWindowLongPtrA(createdState->input, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(originalEditProc));

        CreateWindowExA(
            0,
            "BUTTON",
            "Run",
            WS_CHILD | BS_DEFPUSHBUTTON,
            -100,
            -100,
            0,
            0,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdRunCommand)),
            GetModuleHandleA(nullptr),
            nullptr
        );

        SendMessageA(createdState->output, WM_SETFONT, reinterpret_cast<WPARAM>(createdState->uiFont), TRUE);
        SendMessageA(createdState->input, WM_SETFONT, reinterpret_cast<WPARAM>(createdState->askFont), TRUE);
        createdState->aiStatusLabel = CreateWindowExA(
            0, "STATIC", "",
            WS_CHILD | SS_CENTER,
            0, 0, 1, 1,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdAIProgressLabel)),
            GetModuleHandleA(nullptr),
            nullptr
        );
        SendMessageW(createdState->input, 0x1501, FALSE,
                     reinterpret_cast<LPARAM>(L"Search anything..."));

        // Create taskbar fonts once — reused every paint, never recreated
        createdState->tbOrbFont = CreateFontA(
            16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        createdState->tbMainFont = CreateFontA(
            16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        createdState->tbSnFont = CreateFontA(
            13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        createdState->tbClockFont = CreateFontA(
            18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        createdState->tbDateFont = CreateFontA(
            11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        // Chat panel — hidden child window, shown when AI responds
        createdState->chatPanel = CreateWindowExA(
            0, "PortChatPanel", "",
            WS_CHILD,   // hidden by default
            0, 0, 1, 1,
            window, nullptr, GetModuleHandleA(nullptr), createdState);

        SetTimer(window, IdAnimTimer, 80, nullptr);

        // Accept files dragged in from Windows Explorer onto the desktop.
        DragAcceptFiles(window, TRUE);

        // PORT AGENTS roster — name, tagline, accent, system prompt
        createdState->agents = {
            {"Nova",     "Your personal AI assistant",        RGB(170, 110, 255),
             "You are Nova, the general AI assistant of Port OS. You help with "
             "everyday questions, planning, writing and anything that doesn't "
             "belong to a specialist agent. Friendly, direct, no fluff.", ""},
            {"Fox",      "Documents, spreadsheets & presentations", RGB(255, 140,  60),
             "You are Fox, the document specialist of Port OS. Your expertise: "
             "Microsoft Word, Excel and PowerPoint — formulas, pivot tables, "
             "formatting, templates, document structure, mail merge, charts and "
             "office workflows. Give exact steps, formulas and shortcuts.",
             ""},
            {"Vector",   "Data analysis that drives decisions", RGB( 70, 200, 120),
             "You are Vector, the data analysis agent of Port OS. Your "
             "expertise: statistics, exploring datasets, SQL, pandas, data "
             "cleaning, visualization choices and interpreting results. Be "
             "rigorous about what the data does and does not support.", ""},
            {"Polar",    "3D art, game assets & interface design", RGB(235, 240, 248),
             "You are Polar, the design agent of Port OS. Your expertise: 3D "
             "modeling, character and prop design, texturing, game art "
             "pipelines, and UI/UX design — layout, typography, color and "
             "usability. Give concrete, critique-style feedback.", ""},
            {"Zynx",     "Security intelligence & defense",   RGB( 40,  85, 190),
             "You are Zynx, the cybersecurity agent of Port OS. Your "
             "expertise: defensive security, hardening, secure configuration, "
             "threat analysis, incident response and security best practices. "
             "You refuse to help with attacks on systems the user does not "
             "own or have authorization to test.",
             "\nTOOL: cve search <keyword>   (live NVD vulnerability lookup "
             "— use it whenever the user asks about specific software "
             "vulnerabilities)"},
            {"Vortex",   "Full-stack engineering & cloud",    RGB(236,  64, 122),
             "You are Vortex, the software engineering agent of Port OS. Your "
             "expertise: web development, software architecture, cloud "
             "deployment, APIs, databases and writing clean code in any "
             "language. Prefer working code examples over theory.", ""},
            {"Nexus",    "Network operations & diagnostics",  RGB( 90, 160, 255),
             "You are Nexus, the networking agent of Port OS. Your expertise: "
             "network protocols, routing, switching, DNS, firewalls, "
             "monitoring and troubleshooting infrastructure. Diagnose "
             "methodically, layer by layer.", ""},
            {"The Grid", "Low-level systems & automation",    RGB(255, 200,   0),
             "You are The Grid, the systems agent of Port OS. Your expertise: "
             "operating system internals, kernels, embedded systems, drivers, "
             "low-level programming and automation scripting. Precise and "
             "technical; assume a capable reader.", ""},
            {"Omni",     "Machine learning & AI engineering", RGB(240,  90, 150),
             "You are Omni, the AI and machine learning agent of Port OS. "
             "Your expertise: model architectures, training, fine-tuning, "
             "evaluation, MLOps and practical use of LLM APIs. Distinguish "
             "clearly between established results and speculation.", ""},
            {"Aethro",   "Deep research, grounded in sources", RGB(225,  70,  60),
             "You are Aethro, the deep research agent of Port OS. You handle "
             "hard scientific and technical questions: physics, mathematics, "
             "biology, engineering. Reason step by step, state your "
             "uncertainty honestly, and never invent citations.",
             "\nTOOL: arxiv search <query>   (live arXiv paper search — use "
             "it when the user asks about research or recent papers, and "
             "cite the returned links)"},
        };
        {
            WNDCLASSA panelClass = {};
            panelClass.lpfnWndProc = agentPanelProc;
            panelClass.hInstance = GetModuleHandleA(nullptr);
            panelClass.lpszClassName = "PortAgentPanel";
            panelClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassA(&panelClass);

            WNDCLASSA handleClass = {};
            handleClass.lpfnWndProc = agentHandleProc;
            handleClass.hInstance = GetModuleHandleA(nullptr);
            handleClass.lpszClassName = "PortAgentHandle";
            handleClass.hCursor = LoadCursor(nullptr, IDC_HAND);
            RegisterClassA(&handleClass);

            createdState->agentPanel = CreateWindowExA(0, "PortAgentPanel", "",
                WS_CHILD | WS_CLIPSIBLINGS, 12, 12, AgentPanelWidth, 400,
                window, nullptr, GetModuleHandleA(nullptr), nullptr);
            if (createdState->agentPanel) {
                SetWindowLongPtrA(createdState->agentPanel, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(createdState));
            }
            createdState->agentHandle = CreateWindowExA(0, "PortAgentHandle",
                "", WS_CHILD | WS_CLIPSIBLINGS, 0, 200, 22, 64, window,
                nullptr, GetModuleHandleA(nullptr), nullptr);
            if (createdState->agentHandle) {
                SetWindowLongPtrA(createdState->agentHandle, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(createdState));
            }

            WNDCLASSA gridClass = {};
            gridClass.lpfnWndProc = agentGridProc;
            gridClass.hInstance = GetModuleHandleA(nullptr);
            gridClass.lpszClassName = "PortAgentGrid";
            gridClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassA(&gridClass);

            const SIZE gridSize = agentGridSize(createdState);
            createdState->agentGrid = CreateWindowExA(0, "PortAgentGrid", "",
                WS_CHILD | WS_CLIPSIBLINGS, 0, 0, gridSize.cx, gridSize.cy,
                window, nullptr, GetModuleHandleA(nullptr), nullptr);
            if (createdState->agentGrid) {
                SetWindowLongPtrA(createdState->agentGrid, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(createdState));
            }
        }

        configureAIProvider(createdState->kernel);
        const auto boot = createdState->kernel.boot();
        layoutDesktop(window, createdState);
        refreshDesktopDynamicIcons(window, createdState);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        RECT clientRect;
        GetClientRect(window, &clientRect);
        RECT dirty = ps.rcPaint;
        if (IsRectEmpty(&dirty)) dirty = clientRect;

        const int width = dirty.right - dirty.left;
        const int height = dirty.bottom - dirty.top;

        // Double-buffer only the invalid region. Icon movement usually dirties
        // a small tile, so repainting the full-screen wallpaper here caused
        // unnecessary frame drops.
        HDC memDC = CreateCompatibleDC(dc);
        HBITMAP memBitmap = CreateCompatibleBitmap(dc, width, height);
        HGDIOBJ oldBitmap = SelectObject(memDC, memBitmap);

        if (cachedBackground) {
            HDC backgroundDC = CreateCompatibleDC(dc);
            HGDIOBJ oldBackground = SelectObject(backgroundDC, cachedBackground);
            BitBlt(memDC, 0, 0, width, height,
                   backgroundDC, dirty.left, dirty.top, SRCCOPY);
            SelectObject(backgroundDC, oldBackground);
            DeleteDC(backgroundDC);
        } else {
            RECT localRect = {0, 0, width, height};
            HBRUSH background = CreateSolidBrush(DesktopBackground);
            FillRect(memDC, &localRect, background);
            DeleteObject(background);
        }

        // Rubber-band selection, composited into the same buffered pass
        if (state && state->isDragging && state->hasPrevSelRect) {
            Gdiplus::Graphics g(memDC);
            g.TranslateTransform(static_cast<Gdiplus::REAL>(-dirty.left),
                                 static_cast<Gdiplus::REAL>(-dirty.top));
            drawSelectionBand(g, state->prevSelRect);
        }

        RECT taskbarRect = {clientRect.left,
                            clientRect.bottom - kTaskbarReserved,
                            clientRect.right, clientRect.bottom};
        RECT taskbarIntersection;
        if (IntersectRect(&taskbarIntersection, &dirty, &taskbarRect)) {
            POINT oldOrigin;
            SetViewportOrgEx(memDC, -dirty.left, -dirty.top, &oldOrigin);
            drawTaskbar(window, memDC, state);
            SetViewportOrgEx(memDC, oldOrigin.x, oldOrigin.y, nullptr);
        }

        BitBlt(dc, dirty.left, dirty.top, width, height, memDC, 0, 0, SRCCOPY);

        // Cleanup double-buffer resources
        SelectObject(memDC, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);

        EndPaint(window, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (state) {
            POINT pt;
            pt.x = static_cast<short>(LOWORD(lParam));
            pt.y = static_cast<short>(HIWORD(lParam));

            RECT clientRect;
            GetClientRect(window, &clientRect);
            if (pt.y >= clientRect.bottom - kTaskbarReserved) return 0;

            POINT screenPt = pt;
            ClientToScreen(window, &screenPt);
            if (WindowFromPoint(screenPt) != window) return 0;

            // A click on the empty desktop dismisses the agents popup
            if (state->agentGridVisible) hideAgentGrid(state);

            const int previousSelectedId = state->selectedIconId;
            const std::vector<int> previousDragSelection = state->dragSelectedIconIds;
            state->selectedIconId = 0;
            state->dragSelectedIconIds.clear();

            if (previousSelectedId != 0) {
                HWND selectedIcon = GetDlgItem(window, previousSelectedId);
                if (selectedIcon) {
                    RedrawWindow(selectedIcon, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_UPDATENOW);
                }
            }

            for (int selectedId : previousDragSelection) {
                if (selectedId == previousSelectedId) continue;
                HWND selectedIcon = GetDlgItem(window, selectedId);
                if (selectedIcon) {
                    RedrawWindow(selectedIcon, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_UPDATENOW);
                }
            }

            SetFocus(window);
            state->isDragging        = true;
            state->hasPrevSelRect    = false;
            state->dragStart.x       = pt.x;
            state->dragStart.y       = pt.y;
            state->dragEnd           = state->dragStart;
            SetCapture(window);
        }
        return 0;
    }
    case WM_KEYDOWN:
        // Delete every selected desktop icon (rubber-band selection too)
        if (wParam == VK_DELETE && state) {
            deleteSelectedDesktopIcons(window, state);
            return 0;
        }
        break;
    case WM_MOUSELEAVE:
        if (state && state->hotDockTile != -1) {
            state->hotDockTile = -1;
            RECT clientRect;
            GetClientRect(window, &clientRect);
            RECT dockR = {0, clientRect.bottom - kDockH,
                          clientRect.right, clientRect.bottom};
            InvalidateRect(window, &dockR, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE: {
        // Dock hover highlight (only when not rubber-band selecting)
        if (state && !state->isDragging && !state->isDraggingIcon) {
            POINT pt = {static_cast<short>(LOWORD(lParam)),
                        static_cast<short>(HIWORD(lParam))};
            RECT clientRect;
            GetClientRect(window, &clientRect);
            int hot = -1;
            if (pt.y >= clientRect.bottom - kDockH) {
                const auto tiles = computeDockTiles(state, clientRect.right,
                                                    clientRect.bottom);
                for (std::size_t i = 0; i < tiles.size(); ++i) {
                    if (PtInRect(&tiles[i].rect, pt)) {
                        hot = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (hot != state->hotDockTile) {
                state->hotDockTile = hot;
                RECT dockR = {0, clientRect.bottom - kDockH,
                              clientRect.right, clientRect.bottom};
                InvalidateRect(window, &dockR, FALSE);
                // Track when the cursor leaves the window to clear the hover
                TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE,
                                       window, 0};
                TrackMouseEvent(&tme);
            }
        }
        if (state && state->isDragging) {
            // Cap band updates to ~90/sec: without this a fast drag over
            // many folders queues far more repaints than the screen shows.
            static ULONGLONG lastBandTick = 0;
            const ULONGLONG nowTick = GetTickCount64();
            if (lastBandTick != 0 && nowTick - lastBandTick < 11) return 0;
            lastBandTick = nowTick;

            RECT clientRect;
            GetClientRect(window, &clientRect);
            int maxY = clientRect.bottom - kTaskbarReserved;
            int newX = static_cast<short>(LOWORD(lParam));
            int newY = static_cast<short>(HIWORD(lParam));
            if (newY > maxY) newY = maxY;

            state->dragEnd = {newX, newY};

            int x1 = state->dragStart.x < newX ? state->dragStart.x : newX;
            int y1 = state->dragStart.y < newY ? state->dragStart.y : newY;
            int x2 = state->dragStart.x < newX ? newX : state->dragStart.x;
            int y2 = state->dragStart.y < newY ? newY : state->dragStart.y;

            // Determine selection first, then repaint only icons whose state
            // actually changed. This avoids flashing all desktop icons on
            // every mouse movement.
            std::vector<int> nextSelectedIds;
            if (x2 - x1 > 3 && y2 - y1 > 3) {
                const RECT selectionRect = {x1, y1, x2, y2};
                auto includeIfIntersecting = [&](int id, HWND iconWindow) {
                    if (!iconWindow || !IsWindow(iconWindow)) return;
                    RECT screenRect;
                    GetWindowRect(iconWindow, &screenRect);
                    POINT topLeft = {screenRect.left, screenRect.top};
                    POINT bottomRight = {screenRect.right, screenRect.bottom};
                    ScreenToClient(window, &topLeft);
                    ScreenToClient(window, &bottomRight);
                    RECT iconRect = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
                    RECT intersection;
                    if (IntersectRect(&intersection, &selectionRect, &iconRect)) {
                        nextSelectedIds.push_back(id);
                    }
                };

                const int sysIds[] = {IdIconMyComputer, IdIconTrashBin, IdIconMyServer};
                for (int id : sysIds) includeIfIntersecting(id, GetDlgItem(window, id));
                for (const auto& di : state->dynamicIcons) {
                    includeIfIntersecting(di.id, di.hwnd);
                }
            }

            auto containsId = [](const std::vector<int>& ids, int id) {
                return std::find(ids.begin(), ids.end(), id) != ids.end();
            };
            std::vector<int> changedSelectionIds;
            for (int id : state->dragSelectedIconIds) {
                if (!containsId(nextSelectedIds, id)) {
                    changedSelectionIds.push_back(id);
                }
            }
            for (int id : nextSelectedIds) {
                if (!containsId(state->dragSelectedIconIds, id)) {
                    changedSelectionIds.push_back(id);
                }
            }
            state->dragSelectedIconIds = std::move(nextSelectedIds);

            // The band itself is rendered inside WM_PAINT / drawDesktopIcon,
            // both double-buffered. Here we only invalidate the union of the
            // old and new band areas and repaint synchronously — no
            // direct-to-screen drawing, so nothing flickers.
            const RECT newBand = {x1, y1, x2, y2};
            const bool haveNewBand = (x2 - x1 > 3 && y2 - y1 > 3);
            RECT dirtyBand = {};
            bool haveDirty = false;
            if (state->hasPrevSelRect) {
                dirtyBand = state->prevSelRect;
                haveDirty = true;
            }
            if (haveNewBand) {
                if (haveDirty) UnionRect(&dirtyBand, &dirtyBand, &newBand);
                else { dirtyBand = newBand; haveDirty = true; }
            }
            state->prevSelRect    = newBand;
            state->hasPrevSelRect = haveNewBand;

            // Collect every icon the band touches (old or new position) so
            // its band overlay repaints in the same synchronous pass — this
            // is what keeps the band on top of icons with no ghost trails.
            std::set<int> toRepaint(changedSelectionIds.begin(),
                                    changedSelectionIds.end());
            if (haveDirty) {
                auto addIfTouched = [&](int id, HWND iconWindow) {
                    if (!iconWindow || !IsWindow(iconWindow)) return;
                    RECT sr;
                    GetWindowRect(iconWindow, &sr);
                    POINT tl = {sr.left, sr.top};
                    POINT br = {sr.right, sr.bottom};
                    ScreenToClient(window, &tl);
                    ScreenToClient(window, &br);
                    RECT iconRect = {tl.x, tl.y, br.x, br.y};
                    RECT ix;
                    if (IntersectRect(&ix, &dirtyBand, &iconRect)) {
                        toRepaint.insert(id);
                    }
                };
                const int sysIds[] = {IdIconMyComputer, IdIconTrashBin,
                                      IdIconMyServer};
                for (int id : sysIds) addIfTouched(id, GetDlgItem(window, id));
                for (const auto& di : state->dynamicIcons) {
                    addIfTouched(di.id, di.hwnd);
                }
            }

            // Repaint the band region (parent background) and every touched
            // icon synchronously in one go — RDW_UPDATENOW forces the whole
            // set to render now, so the band never lags behind the icons or
            // leaves a trail from its previous position.
            if (haveDirty) {
                InflateRect(&dirtyBand, 2, 2);
                for (int id : toRepaint) {
                    if (HWND icon = GetDlgItem(window, id)) {
                        InvalidateRect(icon, nullptr, FALSE);
                    }
                }
                RedrawWindow(window, &dirtyBand, nullptr,
                             RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        // Dock tile click: open My Server or the clicked folder
        if (state && !state->isDragging) {
            POINT pt = {static_cast<short>(LOWORD(lParam)),
                        static_cast<short>(HIWORD(lParam))};
            RECT clientRect;
            GetClientRect(window, &clientRect);
            if (pt.y >= clientRect.bottom - kDockH) {
                const auto tiles = computeDockTiles(state, clientRect.right,
                                                    clientRect.bottom);
                for (const auto& tile : tiles) {
                    if (!PtInRect(&tile.rect, pt)) continue;
                    if (tile.agentIndex >= 0) {
                        // Running agent tile: restore / focus / minimize,
                        // like the Windows taskbar
                        hideAgentGrid(state);
                        const auto found =
                            state->agentWindows.find(tile.agentIndex);
                        if (found != state->agentWindows.end() &&
                            IsWindow(found->second)) {
                            HWND agentWnd = found->second;
                            if (IsIconic(agentWnd) ||
                                !IsWindowVisible(agentWnd)) {
                                ShowWindow(agentWnd, SW_RESTORE);
                                SetForegroundWindow(agentWnd);
                            } else if (GetForegroundWindow() == agentWnd) {
                                ShowWindow(agentWnd, SW_MINIMIZE);
                            } else {
                                SetForegroundWindow(agentWnd);
                            }
                        }
                        break;
                    }
                    if (tile.iconId == -1) {
                        // "Agents" folder: toggle the medallion grid popup
                        if (state->agentGridVisible) {
                            hideAgentGrid(state);
                        } else if (state->agentGrid &&
                                   IsWindow(state->agentGrid)) {
                            state->agentGridVisible = true;
                            state->agentGridHot = -1;
                            SetWindowPos(state->agentGrid, HWND_TOP, 0, 0,
                                         0, 0,
                                         SWP_NOMOVE | SWP_NOSIZE |
                                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
                            InvalidateRect(state->agentGrid, nullptr, TRUE);
                        }
                    } else {
                        hideAgentGrid(state);
                        // If this folder's window is already open, toggle it
                        // like the Windows taskbar; otherwise open it.
                        const auto fw =
                            state->folderWindows.find(tile.iconId);
                        if (fw != state->folderWindows.end() &&
                            IsWindow(fw->second)) {
                            HWND fWnd = fw->second;
                            if (IsIconic(fWnd) || !IsWindowVisible(fWnd)) {
                                ShowWindow(fWnd, SW_RESTORE);
                                SetForegroundWindow(fWnd);
                            } else if (GetForegroundWindow() == fWnd) {
                                ShowWindow(fWnd, SW_MINIMIZE);
                            } else {
                                SetForegroundWindow(fWnd);
                            }
                        } else {
                            SendMessageA(window, WM_COMMAND,
                                         MAKEWPARAM(tile.iconId, 0), 0);
                        }
                    }
                    break;
                }
                return 0;
            }
        }
        if (state && state->isDragging) {
            // Stop dragging first so the repaint below no longer draws the
            // band, then erase it by repainting the area it covered.
            state->isDragging = false;
            if (state->hasPrevSelRect) {
                RECT old = state->prevSelRect;
                InflateRect(&old, 2, 2);
                RedrawWindow(window, &old, nullptr,
                             RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                state->hasPrevSelRect = false;
            }
            ReleaseCapture();
            // Keep the icons selected after releasing the mouse.
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        if (state) {
            POINT pt;
            pt.x = static_cast<short>(LOWORD(lParam));
            pt.y = static_cast<short>(HIWORD(lParam));
            
            RECT rect;
            GetClientRect(window, &rect);
            rect.bottom -= kTaskbarReserved; // Exclude dock + search strip
            if (PtInRect(&rect, pt)) {
                ClientToScreen(window, &pt);
                HMENU hMenu = CreatePopupMenu();
                HMENU hViewMenu = CreatePopupMenu();
                
                AppendMenuA(hViewMenu, MF_STRING | (state->iconSize == IconSizeOption::Small ? MF_CHECKED : 0), 40001, "Small Icons");
                AppendMenuA(hViewMenu, MF_STRING | (state->iconSize == IconSizeOption::Medium ? MF_CHECKED : 0), 40002, "Medium Icons");
                AppendMenuA(hViewMenu, MF_STRING | (state->iconSize == IconSizeOption::Large ? MF_CHECKED : 0), 40003, "Large Icons");
                
                AppendMenuA(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hViewMenu), "View");
                AppendMenuA(hMenu, MF_STRING, 40005, "Refresh");
                AppendMenuA(hMenu, MF_STRING, 40004, "New Folder");
                AppendMenuA(hMenu, MF_STRING, 40006, "Settings");
                
                std::vector<std::unique_ptr<PortMenuItem>> menuTheme;
                themePortMenu(hMenu, menuTheme);
                TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
                DestroyMenu(hMenu);
            }
        }
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = reinterpret_cast<HDC>(wParam);
        HWND hwndEdit = reinterpret_cast<HWND>(lParam);
        if (state && hwndEdit == state->input) {
            SetTextColor(hdcEdit, RGB(220, 225, 238));
            SetBkColor(hdcEdit, RGB(16, 22, 38));
            static HBRUSH hBrush = CreateSolidBrush(RGB(16, 22, 38));
            return reinterpret_cast<INT_PTR>(hBrush);
        }
        if (state && hwndEdit == state->renameEdit) {
            // In-place rename: fully transparent — the background brush is
            // the wallpaper patch behind the edit, text matches icon labels.
            SetTextColor(hdcEdit, PortText);
            SetBkMode(hdcEdit, TRANSPARENT);
            if (state->renameBgBrush) {
                return reinterpret_cast<INT_PTR>(state->renameBgBrush);
            }
            return reinterpret_cast<INT_PTR>(GetStockObject(HOLLOW_BRUSH));
        }
        break;
    }
    case WM_SIZE:
        if (state) {
            RECT r;
            GetClientRect(window, &r);
            updateCachedBackground(window, r.right - r.left, r.bottom - r.top);
            layoutDesktop(window, state);
            InvalidateRect(window, nullptr, TRUE);
        }
        return 0;
    case WM_DROPFILES: {
        // Files dragged in from Windows Explorer are imported (copied) into
        // the sandbox Desktop, so agents can work with the user's real files.
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        if (!state) { DragFinish(drop); return 0; }
        int imported = 0;
        try {
            // Wide-char query so Unicode filenames (ë, ç, ...) survive intact.
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            const std::filesystem::path destDir =
                state->kernel.getSandbox().root() / "Desktop";
            std::error_code ec;
            std::filesystem::create_directories(destDir, ec);
            for (UINT i = 0; i < count; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                if (len == 0) continue;
                std::wstring wide(len, L'\0');
                DragQueryFileW(drop, i, &wide[0], len + 1);
                const std::filesystem::path src(wide);
                if (src.empty()) continue;
                const std::filesystem::path dest = destDir / src.filename();
                // Never copy a folder into itself or its own subtree.
                std::error_code cmp;
                if (std::filesystem::equivalent(src, destDir, cmp)) continue;
                if (std::filesystem::is_directory(src, ec)) {
                    std::filesystem::copy(
                        src, dest,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing,
                        ec);
                } else {
                    std::filesystem::copy_file(
                        src, dest,
                        std::filesystem::copy_options::overwrite_existing, ec);
                }
                if (!ec) ++imported;
            }
        } catch (...) {
            // Never let an import error take down the desktop.
        }
        DragFinish(drop);
        if (imported > 0) {
            refreshDesktopDynamicIcons(window, state);
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == IdAnimTimer && state) {
            // Only repaint the taskbar when something actually changes.
            // Repainting every tick re-ran the dock's frosted-glass blur
            // 12x/second forever, which got worse with each desktop folder.
            bool needPaint = false;
            if (state->isAIRunning) {
                state->pulsePhase = (state->pulsePhase + 1) % 30;
                needPaint = true; // the search-pill glow animates
            }
            if (state->statusClearCountdown > 0) {
                --state->statusClearCountdown;
                if (state->statusClearCountdown == 0) {
                    state->aiStatusMsg.clear();
                }
                needPaint = true;
            }
            static int lastMinute = -1;
            SYSTEMTIME now;
            GetLocalTime(&now);
            if (now.wMinute != lastMinute) {
                lastMinute = now.wMinute;
                needPaint = true; // clock advanced
            }
            if (needPaint) {
                RECT r; GetClientRect(window, &r);
                RECT tb = {r.left, r.bottom - kTaskbarReserved,
                           r.right, r.bottom};
                InvalidateRect(window, &tb, FALSE);
            }
        }
        return 0;

    case WM_COMMAND:
        if (!state) {
            return 0;
        }

        switch (LOWORD(wParam)) {
        case IdRunCommand: {
            const std::string text = getWindowText(state->input);
            runTerminalCommand(state, text);
            SetWindowTextA(state->input, "");
            return 0;
        }
        case IdIconMyComputer: {
            if (state->myPortWindow && IsWindow(state->myPortWindow)) {
                SetForegroundWindow(state->myPortWindow);
                return 0;
            }
            HWND mw = CreateWindowExA(
                0, "PortFolderWindow", "My Port - Sandbox Files",
                WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                200, 150, 560, 420,
                window, nullptr, GetModuleHandleA(nullptr), nullptr
            );
            state->myPortWindow = mw;
            if (mw) {
                RECT cr;
                GetClientRect(mw, &cr);
                int w = cr.right - cr.left;
                int h = cr.bottom - cr.top;

                // Themed listbox below the painted header/toolbar
                CreateWindowExA(0, "LISTBOX", nullptr,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                    12, FolderHeaderH, w - 24, h - FolderHeaderH - 12, mw,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdMyPortList)),
                    GetModuleHandleA(nullptr), nullptr);

                auto* folderState = new FolderWindowState{ &state->kernel, "" };
                folderState->desktop = state;
                SetWindowLongPtrA(mw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(folderState));
                HWND lb = GetDlgItem(mw, IdMyPortList);
                subclassFolderList(lb);
                refreshMyPortList(lb, &state->kernel.getSandbox(), "");
                refreshFolderItems(folderState);
                ShowWindow(lb, folderState->viewMode == 0 ? SW_SHOW : SW_HIDE);
                InvalidateRect(mw, nullptr, TRUE);
            }
            return 0;
        }
        case IdIconMyServer: {
            if (state->myServerWindow && IsWindow(state->myServerWindow)) {
                SetForegroundWindow(state->myServerWindow);
                return 0;
            }
            HWND sw = CreateWindowExA(
                0, "PortMyServerWindow", "My Server - Local Storage",
                WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                230, 160, 580, 430,
                window, nullptr, GetModuleHandleA(nullptr), nullptr
            );
            state->myServerWindow = sw;
            return 0;
        }
        case IdIconTrashBin: {
            if (state->trashWindow && IsWindow(state->trashWindow)) {
                SetForegroundWindow(state->trashWindow);
                return 0;
            }
            HWND tw = CreateWindowExA(
                0, "PortTrashWindow", "Trash Bin - Deleted Files",
                WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                220, 170, 520, 420,
                window, nullptr, GetModuleHandleA(nullptr), nullptr
            );
            state->trashWindow = tw;
            if (tw) {
                RECT tcr;
                GetClientRect(tw, &tcr);
                // Owner-drawn rows: file/folder glyphs like a real explorer
                HWND lb = CreateWindowExA(0, "LISTBOX", nullptr,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
                    LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                    12, 12, tcr.right - 24, tcr.bottom - 68, tw,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdTrashList)),
                    GetModuleHandleA(nullptr), nullptr);
                CreateWindowExA(0, "BUTTON", "Empty Trash Bin",
                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                    12, tcr.bottom - 44, 140, 32, tw,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdTrashEmpty)),
                    GetModuleHandleA(nullptr), nullptr);
                SetWindowLongPtrA(tw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state->kernel));
                refreshTrashList(lb, &state->kernel.getSandbox());
            }
            return 0;
        }
        case 40001: { // Small Icons
            setDesktopIconSize(window, state, IconSizeOption::Small);
            return 0;
        }
        case 40002: { // Medium Icons
            setDesktopIconSize(window, state, IconSizeOption::Medium);
            return 0;
        }
        case 40003: { // Large Icons
            setDesktopIconSize(window, state, IconSizeOption::Large);
            return 0;
        }
        case 40004: { // New Folder on Desktop
            std::string folderName = "New Folder";
            std::string fullPath;
            int suffix = 1;
            while (true) {
                std::string testName = (suffix == 1) ? folderName : (folderName + " (" + std::to_string(suffix) + ")");
                std::string path = "Desktop/" + testName;
                if (state->kernel.getSandbox().isSafePath(path, fullPath)) {
                    DWORD attr = GetFileAttributesA(fullPath.c_str());
                    if (attr == INVALID_FILE_ATTRIBUTES) {
                        folderName = testName;
                        break;
                    }
                } else {
                    break;
                }
                suffix++;
                if (suffix > 100) break;
            }
            std::string path = "Desktop/" + folderName;
            (void)state->kernel.getSandbox().makeDir(path);

            // Refresh desktop dynamic icons!
            refreshDesktopDynamicIcons(window, state);
            
            // Also refresh My Port window if it is open
            if (state->myPortWindow && IsWindow(state->myPortWindow)) {
                SendMessageA(state->myPortWindow, WM_COMMAND, IdMyPortRefresh, 0);
            }
            return 0;
        }
        case 40005: { // Refresh
            RECT r;
            GetClientRect(window, &r);
            updateCachedBackground(window, r.right - r.left, r.bottom - r.top);
            // Re-align icons to the grid and re-read the sandbox Desktop,
            // like a real desktop refresh
            snapIconsToGrid(state);
            layoutDesktop(window, state);
            refreshDesktopDynamicIcons(window, state, /*fullRebuild=*/true);
            if (state->myPortWindow && IsWindow(state->myPortWindow)) {
                SendMessageA(state->myPortWindow, WM_COMMAND, IdMyPortRefresh, 0);
            }
            // InvalidateRect on the parent alone leaves the icon buttons
            // stale — repaint everything synchronously, children included.
            RedrawWindow(window, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        }
        case 40006: { // Settings
            {
                std::vector<PortPropRow> rows = {
                    {"Version", "2.0"},
                    {"Codename", "Super-Nova"},
                    {"Engine", "AI Kernel Active"},
                };
                showPortProperties(window, "Port OS Settings", rows);
            }
            return 0;
        }
        default: {
            int controlId = LOWORD(wParam);
            if (controlId >= 1100 && state) {
                // Find the dynamic icon
                for (const auto& di : state->dynamicIcons) {
                    if (di.id == controlId) {
                        int notification = HIWORD(wParam);
                        if (notification == 0) { // Open
                            if (di.isDir) {
                                // Already open? Restore + focus, don't duplicate.
                                const auto existing =
                                    state->folderWindows.find(controlId);
                                if (existing != state->folderWindows.end() &&
                                    IsWindow(existing->second)) {
                                    ShowWindow(existing->second, SW_RESTORE);
                                    SetForegroundWindow(existing->second);
                                    break;
                                }
                                std::string path = "Desktop/" + di.name;
                                HWND mw = CreateWindowExA(
                                    0, "PortFolderWindow", ("Desktop - " + di.name).c_str(),
                                    WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                                    200, 150, 560, 420,
                                    window, nullptr, GetModuleHandleA(nullptr), nullptr
                                );
                                if (mw) {
                                    RECT cr;
                                    GetClientRect(mw, &cr);
                                    int w = cr.right - cr.left;
                                    int h = cr.bottom - cr.top;
                                    CreateWindowExA(0, "LISTBOX", nullptr,
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                        12, FolderHeaderH, w - 24, h - FolderHeaderH - 12, mw,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdMyPortList)),
                                        GetModuleHandleA(nullptr), nullptr);

                                    auto* folderState = new FolderWindowState{ &state->kernel, path };
                                    folderState->desktop = state;
                                    folderState->dockIconId = controlId;
                                    SetWindowLongPtrA(mw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(folderState));
                                    state->folderWindows[controlId] = mw;
                                    HWND lb = GetDlgItem(mw, IdMyPortList);
                                    subclassFolderList(lb);
                                    refreshMyPortList(lb, &state->kernel.getSandbox(), path);
                                    refreshFolderItems(folderState);
                                    ShowWindow(lb, folderState->viewMode == 0
                                                       ? SW_SHOW : SW_HIDE);
                                    InvalidateRect(mw, nullptr, TRUE);
                                    RECT dr; GetClientRect(window, &dr);
                                    dr.top = dr.bottom - kDockH;
                                    InvalidateRect(window, &dr, FALSE);
                                }
                            } else {
                                std::string path = "Desktop/" + di.name;
                                auto readResult = state->kernel.getSandbox().read(path);
                                if (readResult) {
                                    MessageBoxA(window, readResult->c_str(), di.name.c_str(), MB_OK | MB_ICONINFORMATION);
                                }
                            }
                        } else if (notification == 1) { // Delete (Move to Trash)
                            std::string path = "Desktop/" + di.name;
                            (void)state->kernel.getSandbox().moveToTrash(path);
                            refreshDesktopDynamicIcons(window, state);
                            // Refresh trash icon live
                            HWND trashBtn = GetDlgItem(window, IdIconTrashBin);
                            if (trashBtn) InvalidateRect(trashBtn, nullptr, TRUE);
                        } else if (notification == 2) { // Properties
                            const std::filesystem::path fullPath =
                                state->kernel.getSandbox().root() /
                                "Desktop" / di.name;
                            std::vector<PortPropRow> rows;
                            rows.push_back({"Name", di.name});
                            rows.push_back({"Type", di.isDir ? "Folder" : "File"});
                            rows.push_back({"Location",
                                            "sandbox/Desktop/" + di.name});
                            if (di.isDir) {
                                DirStats stats = computeDirStats(fullPath);
                                rows.push_back(
                                    {"Contains",
                                     std::to_string(stats.files) + " files, " +
                                     std::to_string(stats.folders) + " folders"});
                                rows.push_back({"Size", formatBytes(stats.bytes)});
                            } else {
                                std::error_code sizeError;
                                const auto fileSize =
                                    std::filesystem::file_size(fullPath, sizeError);
                                rows.push_back({"Size",
                                                sizeError ? std::string("-")
                                                          : formatBytes(fileSize)});
                            }
                            const std::string modified = formatFileTime(fullPath);
                            if (!modified.empty()) {
                                rows.push_back({"Modified", modified});
                            }
                            showPortProperties(window, di.name, rows);
                        }
                        break;
                    }
                }
            }
            return 0;
        }
        }
    case WM_MOUSEWHEEL:
        // Wheel messages land on the focused window; route them to the
        // agents panel when the cursor is over it so its list can scroll.
        if (state && state->agentPanelVisible && state->agentPanel &&
            IsWindow(state->agentPanel)) {
            POINT pt = {static_cast<short>(LOWORD(lParam)),
                        static_cast<short>(HIWORD(lParam))}; // screen coords
            RECT panelRect;
            GetWindowRect(state->agentPanel, &panelRect);
            if (PtInRect(&panelRect, pt)) {
                return SendMessageA(state->agentPanel, WM_MOUSEWHEEL,
                                    wParam, lParam);
            }
        }
        break;
    case WM_MEASUREITEM:
        if (measurePortMenuItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        break;
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (drawPortMenuItem(dis)) return TRUE;
        drawDesktopIcon(dis);
        return TRUE;
    }
    case PortMessageAIComplete: {
        std::unique_ptr<AsyncCommandResult> result{
            reinterpret_cast<AsyncCommandResult*>(lParam)};
        if (state && result) {
            finishTerminalCommand(state, result->command, result->response);
            if (state->input) SetFocus(state->input);
        }
        return 0;
    }
    case PortMessageAgentAIComplete: {
        std::unique_ptr<AgentAsyncResult> result{
            reinterpret_cast<AgentAsyncResult*>(lParam)};
        if (state && result) {
            const auto found = state->agentWindows.find(result->agentIndex);
            if (found != state->agentWindows.end() &&
                IsWindow(found->second)) {
                auto* aw = reinterpret_cast<AgentWindowState*>(
                    GetWindowLongPtrA(found->second, GWLP_USERDATA));
                if (aw && aw->pendingSession >= 0 &&
                    aw->pendingSession < static_cast<int>(aw->sessions.size())) {
                    AgentSession& sess = aw->sessions[aw->pendingSession];
                    sess.history.push_back(
                        {false, result->text.empty()
                                    ? std::string("(no response)")
                                    : result->text});
                    // Intermediate tool notes keep the "thinking" state
                    if (result->final) aw->waiting = false;
                    sess.pinned = true;
                    InvalidateRect(found->second, nullptr, FALSE);
                }
            }
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(window, IdAnimTimer);
        if (state) {
            state->aiWorker.request_stop();
            if (state->askFont)   DeleteObject(state->askFont);
            if (state->uiFont)    DeleteObject(state->uiFont);
            if (state->labelFont) DeleteObject(state->labelFont);
            if (state->tbOrbFont)   DeleteObject(state->tbOrbFont);
            if (state->tbMainFont)  DeleteObject(state->tbMainFont);
            if (state->tbSnFont)    DeleteObject(state->tbSnFont);
            if (state->tbClockFont) DeleteObject(state->tbClockFont);
            if (state->tbDateFont)  DeleteObject(state->tbDateFont);
            delete state;
        }
        SetWindowLongPtrA(window, GWLP_USERDATA, 0);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(window, message, wParam, lParam);
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

// ---- Icon-based folder window helpers ----

// Draw one file/folder tile in the folder list-view (owner-draw listbox)
void drawFolderItem(DRAWITEMSTRUCT* dis, bool isDir)
{
    HDC dc = dis->hDC;
    const RECT r = dis->rcItem;
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;

    // Row background — the listbox client color
    HBRUSH rowBg = CreateSolidBrush(RGB(16, 20, 34));
    FillRect(dc, &r, rowBg);
    DeleteObject(rowBg);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    // Rounded accent highlight for the selected row
    if (selected) {
        Gdiplus::GraphicsPath path;
        addRoundRect(path, r.left + 6, r.top + 2, (r.right - r.left) - 12,
                     (r.bottom - r.top) - 4, 8);
        Gdiplus::SolidBrush fill(Gdiplus::Color(52, 90, 150, 255));
        g.FillPath(&fill, &path);
        Gdiplus::Pen pen(Gdiplus::Color(150, 90, 160, 255), 1.0f);
        g.DrawPath(&pen, &path);
    }

    // Real folder / file icon
    const int iconSize = 22;
    const int iconX = r.left + 16;
    const int iconY = r.top + (r.bottom - r.top - iconSize) / 2;
    char buf[MAX_PATH] = {};
    SendMessageA(dis->hwndItem, LB_GETTEXT, dis->itemID,
                 reinterpret_cast<LPARAM>(buf));
    const std::string name(buf);
    const bool isUp = (name == "..");

    if (isUp) {
        Gdiplus::Pen up(Gdiplus::Color(255, 150, 175, 220), 2.0f);
        const int cx2 = iconX + iconSize / 2, cy2 = iconY + iconSize / 2;
        g.DrawLine(&up, cx2, cy2 + 5, cx2, cy2 - 5);
        g.DrawLine(&up, cx2 - 5, cy2, cx2, cy2 - 5);
        g.DrawLine(&up, cx2 + 5, cy2, cx2, cy2 - 5);
    } else if (isDir && folderIconImg) {
        g.DrawImage(folderIconImg, iconX, iconY, iconSize, iconSize);
    } else if (isDir) {
        Gdiplus::SolidBrush f(Gdiplus::Color(255, 90, 150, 240));
        g.FillRectangle(&f, iconX, iconY + 4, iconSize, iconSize - 6);
    } else {
        // File glyph: rounded page
        Gdiplus::GraphicsPath page;
        addRoundRect(page, iconX + 3, iconY + 1, iconSize - 8, iconSize - 2, 3);
        Gdiplus::SolidBrush pf(Gdiplus::Color(255, 210, 220, 245));
        g.FillPath(&pf, &page);
        Gdiplus::Pen pl(Gdiplus::Color(255, 120, 140, 180), 1.0f);
        g.DrawPath(&pl, &page);
    }

    // Item label (strip trailing '/') — regular Segoe UI, not the bold
    // system font the listbox would otherwise use.
    static HFONT itemFont = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    std::string label = name;
    if (!label.empty() && label.back() == '/') label.pop_back();
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, selected ? RGB(255, 255, 255) : RGB(220, 226, 240));
    HGDIOBJ oldFont = SelectObject(dc, itemFont);
    RECT textRect = {r.left + 48, r.top, r.right - 12, r.bottom};
    DrawTextA(dc, label.c_str(), -1, &textRect,
              DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

// Show a simple input dialog and return the entered string (empty = cancelled)
std::string formatBytes(std::uintmax_t bytes)
{
    char buf[64];
    if (bytes >= 1024ull * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.1f KB",
                      static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return buf;
}

DirStats computeDirStats(const std::filesystem::path& path)
{
    DirStats stats;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        path, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        if (it->is_directory(ec)) {
            ++stats.folders;
        } else {
            ++stats.files;
            const auto size = it->file_size(ec);
            if (!ec) stats.bytes += size;
        }
        it.increment(ec);
    }
    return stats;
}

std::string formatFileTime(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto fileTime = std::filesystem::last_write_time(path, ec);
    if (ec) return "";
    const auto sysTime =
        std::chrono::clock_cast<std::chrono::system_clock>(fileTime);
    const std::time_t t = std::chrono::system_clock::to_time_t(sysTime);
    const std::tm* tm = std::localtime(&t);
    if (!tm) return "";
    char buf[64];
    std::strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", tm);
    return buf;
}

void showPortProperties(HWND parent, const std::string& title,
                        const std::vector<PortPropRow>& rows)
{
    struct Ctx {
        std::string title;
        const std::vector<PortPropRow>* rows;
        HFONT titleFont;
        HFONT labelFont;
        HFONT valueFont;
        int titleH;
        bool hotClose = false;
        bool hotOk = false;
        RECT closeRect = {};
        RECT okRect = {};
    };

    Ctx ctx;
    ctx.title = title;
    ctx.rows = &rows;
    ctx.titleH = 48;
    ctx.titleFont = CreateFontA(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, "Segoe UI");
    ctx.labelFont = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, "Segoe UI");
    ctx.valueFont = CreateFontA(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, "Segoe UI");

    struct Dlg {
        static LRESULT CALLBACK proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
        {
            Ctx* c = reinterpret_cast<Ctx*>(GetWindowLongPtrA(hw, GWLP_USERDATA));
            switch (msg) {
            case WM_CREATE:
                SetWindowLongPtrA(hw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
                    reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams));
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                if (!c) break;
                PAINTSTRUCT ps;
                HDC screen = BeginPaint(hw, &ps);
                RECT rc;
                GetClientRect(hw, &rc);
                const int w = rc.right;
                const int h = rc.bottom;
                HDC dc = CreateCompatibleDC(screen);
                HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
                HGDIOBJ oldBmp = SelectObject(dc, bmp);

                HBRUSH bg = CreateSolidBrush(RGB(16, 22, 38));
                FillRect(dc, &rc, bg);
                DeleteObject(bg);

                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

                RECT headerLine = {0, c->titleH - 1, w, c->titleH};
                HBRUSH lineBrush = CreateSolidBrush(RGB(38, 48, 76));
                FillRect(dc, &headerLine, lineBrush);
                DeleteObject(lineBrush);

                SetBkMode(dc, TRANSPARENT);

                SetTextColor(dc, RGB(224, 230, 244));
                HGDIOBJ oldFont = SelectObject(dc, c->titleFont);
                RECT titleRect = {20, 0, w - 56, c->titleH};
                DrawTextA(dc, c->title.c_str(), -1, &titleRect,
                          DT_SINGLELINE | DT_VCENTER | DT_LEFT |
                          DT_END_ELLIPSIS | DT_NOPREFIX);

                // Close (✕) button
                c->closeRect = {w - 44, 10, w - 16, 38};
                if (c->hotClose) {
                    Gdiplus::GraphicsPath closeBg;
                    addRoundRect(closeBg, c->closeRect.left, c->closeRect.top,
                                 28, 28, 6);
                    Gdiplus::SolidBrush closeFill(Gdiplus::Color(60, 226, 76, 92));
                    g.FillPath(&closeFill, &closeBg);
                }
                {
                    Gdiplus::Pen xPen(c->hotClose
                                          ? Gdiplus::Color(255, 236, 120, 132)
                                          : Gdiplus::Color(255, 168, 178, 202),
                                      1.8f);
                    const int x1 = c->closeRect.left + 9;
                    const int y1 = c->closeRect.top + 9;
                    const int x2 = c->closeRect.right - 9;
                    const int y2 = c->closeRect.bottom - 9;
                    g.DrawLine(&xPen, x1, y1, x2, y2);
                    g.DrawLine(&xPen, x1, y2, x2, y1);
                }

                // Label/value rows
                int y = c->titleH + 12;
                for (const auto& row : *c->rows) {
                    SelectObject(dc, c->labelFont);
                    SetTextColor(dc, RGB(122, 132, 158));
                    RECT labelRect = {20, y, w - 20, y + 16};
                    DrawTextA(dc, row.label.c_str(), -1, &labelRect,
                              DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
                    SelectObject(dc, c->valueFont);
                    SetTextColor(dc, RGB(220, 225, 238));
                    RECT valueRect = {20, y + 17, w - 20, y + 40};
                    DrawTextA(dc, row.value.c_str(), -1, &valueRect,
                              DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS |
                              DT_NOPREFIX);
                    y += 46;
                }

                // OK button
                c->okRect = {w - 104, h - 48, w - 20, h - 16};
                {
                    Gdiplus::GraphicsPath okBg;
                    addRoundRect(okBg, c->okRect.left, c->okRect.top,
                                 c->okRect.right - c->okRect.left,
                                 c->okRect.bottom - c->okRect.top, 8);
                    Gdiplus::SolidBrush okFill(
                        c->hotOk ? Gdiplus::Color(90, 90, 160, 255)
                                 : Gdiplus::Color(40, 90, 160, 255));
                    g.FillPath(&okFill, &okBg);
                    Gdiplus::Pen okBorder(Gdiplus::Color(200, 90, 160, 255), 1.2f);
                    g.DrawPath(&okBorder, &okBg);
                }
                SelectObject(dc, c->valueFont);
                SetTextColor(dc, RGB(224, 230, 244));
                RECT okText = c->okRect;
                DrawTextA(dc, "OK", -1, &okText,
                          DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                // Outer border following the rounded window region
                {
                    Gdiplus::GraphicsPath borderPath;
                    addRoundRect(borderPath, 0, 0, w - 1, h - 1, 8);
                    Gdiplus::Pen borderPen(Gdiplus::Color(255, 58, 72, 110), 1.0f);
                    g.DrawPath(&borderPen, &borderPath);
                }

                SelectObject(dc, oldFont);
                BitBlt(screen, 0, 0, w, h, dc, 0, 0, SRCCOPY);
                SelectObject(dc, oldBmp);
                DeleteObject(bmp);
                DeleteDC(dc);
                EndPaint(hw, &ps);
                return 0;
            }
            case WM_MOUSEMOVE: {
                if (!c) break;
                POINT pt = {static_cast<short>(LOWORD(lp)),
                            static_cast<short>(HIWORD(lp))};
                const bool hotClose = PtInRect(&c->closeRect, pt) != 0;
                const bool hotOk = PtInRect(&c->okRect, pt) != 0;
                if (hotClose != c->hotClose || hotOk != c->hotOk) {
                    c->hotClose = hotClose;
                    c->hotOk = hotOk;
                    InvalidateRect(hw, nullptr, FALSE);
                }
                TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hw, 0};
                TrackMouseEvent(&tme);
                return 0;
            }
            case WM_MOUSELEAVE:
                if (c && (c->hotClose || c->hotOk)) {
                    c->hotClose = false;
                    c->hotOk = false;
                    InvalidateRect(hw, nullptr, FALSE);
                }
                return 0;
            case WM_LBUTTONUP: {
                if (!c) break;
                POINT pt = {static_cast<short>(LOWORD(lp)),
                            static_cast<short>(HIWORD(lp))};
                if (PtInRect(&c->closeRect, pt) || PtInRect(&c->okRect, pt)) {
                    DestroyWindow(hw);
                }
                return 0;
            }
            case WM_NCHITTEST: {
                const LRESULT hit = DefWindowProcA(hw, msg, wp, lp);
                if (hit == HTCLIENT && c) {
                    POINT pt = {static_cast<short>(LOWORD(lp)),
                                static_cast<short>(HIWORD(lp))};
                    ScreenToClient(hw, &pt);
                    if (pt.y < c->titleH && !PtInRect(&c->closeRect, pt)) {
                        return HTCAPTION; // drag the window by its header
                    }
                }
                return hit;
            }
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE || wp == VK_RETURN) DestroyWindow(hw);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcA(hw, msg, wp, lp);
        }
    };

    WNDCLASSA cls = {};
    cls.lpfnWndProc = Dlg::proc;
    cls.hInstance = GetModuleHandleA(nullptr);
    cls.lpszClassName = "PortPropsWindow";
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&cls);

    const int width = 400;
    const int height = ctx.titleH + 12 +
                       static_cast<int>(rows.size()) * 46 + 62;

    HWND wnd = CreateWindowExA(0, "PortPropsWindow", title.c_str(), WS_POPUP,
                               0, 0, width, height, parent, nullptr,
                               GetModuleHandleA(nullptr), &ctx);
    if (wnd) {
        SetWindowRgn(wnd, CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                             16, 16), TRUE);
        if (parent) {
            RECT pr;
            GetWindowRect(parent, &pr);
            const int x = pr.left + (pr.right - pr.left - width) / 2;
            const int y = pr.top + (pr.bottom - pr.top - height) / 2;
            SetWindowPos(wnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        ShowWindow(wnd, SW_SHOW);
        SetFocus(wnd);
        UpdateWindow(wnd);

        MSG msg;
        while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    UnregisterClassA("PortPropsWindow", GetModuleHandleA(nullptr));
    DeleteObject(ctx.titleFont);
    DeleteObject(ctx.labelFont);
    DeleteObject(ctx.valueFont);
}

// Port OS-themed modal input dialog: dark, rounded, accent OK button.
std::string showInputDialog(HWND parent, const char* title, const char* prompt, const char* defaultVal)
{
    struct ModalCtx {
        std::string title, prompt;
        std::string* result;
        bool* ok;
        HWND edit = nullptr;
        HFONT titleFont = nullptr, bodyFont = nullptr;
        RECT okRect{}, cancelRect{}, closeRect{};
        int hot = -1; // 0 OK, 1 Cancel, 2 Close
    };
    std::string result;
    bool okClicked = false;
    ModalCtx ctx{title ? title : "", prompt ? prompt : "", &result,
                 &okClicked};
    ctx.titleFont = CreateFontA(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    ctx.bodyFont = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");

    struct Dlg {
        static void commit(HWND hw, ModalCtx* c, bool ok) {
            if (ok && c->edit) {
                const int len = GetWindowTextLengthA(c->edit);
                if (len > 0) {
                    c->result->resize(static_cast<std::size_t>(len));
                    GetWindowTextA(c->edit, &(*c->result)[0], len + 1);
                }
                *c->ok = true;
            }
            DestroyWindow(hw);
        }
        static LRESULT CALLBACK editProc(HWND e, UINT m, WPARAM w, LPARAM l) {
            auto* orig = reinterpret_cast<WNDPROC>(
                GetWindowLongPtrA(e, GWLP_USERDATA));
            if (m == WM_KEYDOWN && w == VK_RETURN) {
                commit(GetParent(e),
                       reinterpret_cast<ModalCtx*>(
                           GetWindowLongPtrA(GetParent(e), GWLP_USERDATA)),
                       true);
                return 0;
            }
            if (m == WM_KEYDOWN && w == VK_ESCAPE) {
                DestroyWindow(GetParent(e));
                return 0;
            }
            if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;
            return CallWindowProcA(orig, e, m, w, l);
        }
        static LRESULT CALLBACK proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
            auto* c = reinterpret_cast<ModalCtx*>(
                GetWindowLongPtrA(hw, GWLP_USERDATA));
            switch (msg) {
            case WM_CREATE: {
                c = reinterpret_cast<ModalCtx*>(
                    reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);
                SetWindowLongPtrA(hw, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(c));
                RECT rc; GetClientRect(hw, &rc);
                const int eh = 34;
                // ES_MULTILINE only so EM_SETRECT can vertically center the
                // single line (a plain single-line EDIT top-aligns text);
                // Enter is intercepted by editProc, so no newline is added.
                c->edit = CreateWindowExA(0, "EDIT",
                    reinterpret_cast<CREATESTRUCT*>(lp)->lpszName,
                    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOHSCROLL,
                    24, 88, rc.right - 48, eh, hw,
                    reinterpret_cast<HMENU>(100), GetModuleHandleA(nullptr),
                    nullptr);
                SendMessageA(c->edit, WM_SETFONT,
                             reinterpret_cast<WPARAM>(c->bodyFont), TRUE);
                // Vertically center the text line inside the box
                {
                    HDC edc = GetDC(c->edit);
                    HGDIOBJ of = SelectObject(edc, c->bodyFont);
                    TEXTMETRICA tm{};
                    GetTextMetricsA(edc, &tm);
                    SelectObject(edc, of);
                    ReleaseDC(c->edit, edc);
                    RECT fmt = {12, std::max(0L, (eh - tm.tmHeight) / 2),
                                (rc.right - 48) - 12, eh};
                    SendMessageA(c->edit, EM_SETRECT, 0,
                                 reinterpret_cast<LPARAM>(&fmt));
                }
                WNDPROC orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
                    c->edit, GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(editProc)));
                SetWindowLongPtrA(c->edit, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(orig));
                SendMessageA(c->edit, EM_SETSEL, 0, -1);
                SetFocus(c->edit);
                return 0;
            }
            case WM_CTLCOLOREDIT: {
                // Same fill as the rounded input shape painted below, so the
                // edit's square client never shows as a lighter plate.
                HDC e = reinterpret_cast<HDC>(wp);
                SetTextColor(e, RGB(224, 230, 244));
                SetBkColor(e, RGB(20, 26, 44));
                static HBRUSH b = CreateSolidBrush(RGB(20, 26, 44));
                return reinterpret_cast<INT_PTR>(b);
            }
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                if (!c) break;
                PAINTSTRUCT ps;
                HDC screen = BeginPaint(hw, &ps);
                RECT rc; GetClientRect(hw, &rc);
                const int w = rc.right, h = rc.bottom;
                HDC dc = CreateCompatibleDC(screen);
                HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
                HGDIOBJ ob = SelectObject(dc, bmp);
                HBRUSH bg = CreateSolidBrush(RGB(16, 22, 38));
                FillRect(dc, &rc, bg); DeleteObject(bg);
                SetBkMode(dc, TRANSPARENT);
                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                const Gdiplus::Color accent(255, 90, 160, 255);

                // Title
                SelectObject(dc, c->titleFont);
                SetTextColor(dc, RGB(226, 232, 246));
                RECT tr = {24, 16, w - 48, 44};
                DrawTextA(dc, c->title.c_str(), -1, &tr,
                          DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS);
                // Close X
                c->closeRect = {w - 42, 14, w - 16, 40};
                {
                    Gdiplus::Pen xp(c->hot == 2
                        ? Gdiplus::Color(255, 236, 120, 132)
                        : Gdiplus::Color(255, 160, 170, 195), 1.6f);
                    g.DrawLine(&xp, static_cast<INT>(c->closeRect.left + 7),
                               static_cast<INT>(c->closeRect.top + 7),
                               static_cast<INT>(c->closeRect.right - 7),
                               static_cast<INT>(c->closeRect.bottom - 7));
                    g.DrawLine(&xp, static_cast<INT>(c->closeRect.left + 7),
                               static_cast<INT>(c->closeRect.bottom - 7),
                               static_cast<INT>(c->closeRect.right - 7),
                               static_cast<INT>(c->closeRect.top + 7));
                }
                // Prompt
                SelectObject(dc, c->bodyFont);
                SetTextColor(dc, RGB(150, 160, 182));
                RECT pr = {24, 62, w - 24, 84};
                DrawTextA(dc, c->prompt.c_str(), -1, &pr,
                          DT_SINGLELINE | DT_LEFT);
                // Rounded input field: fill matches the edit's own background
                // (WM_CTLCOLOREDIT), so only the rounded shape is visible.
                {
                    Gdiplus::GraphicsPath ep;
                    addRoundRect(ep, 22, 86, w - 44, 38, 8);
                    Gdiplus::SolidBrush ef(Gdiplus::Color(255, 20, 26, 44));
                    g.FillPath(&ef, &ep);
                    Gdiplus::Pen epn(Gdiplus::Color(170, 90, 160, 255), 1.3f);
                    g.DrawPath(&epn, &ep);
                }
                // Buttons
                c->cancelRect = {w - 200, h - 48, w - 108, h - 16};
                c->okRect = {w - 100, h - 48, w - 24, h - 16};
                auto btn = [&](RECT b, const char* label, bool primary,
                               bool hot) {
                    Gdiplus::GraphicsPath p;
                    addRoundRect(p, b.left, b.top, b.right - b.left,
                                 b.bottom - b.top, 8);
                    if (primary) {
                        Gdiplus::SolidBrush f(hot
                            ? Gdiplus::Color(255, 120, 180, 255) : accent);
                        g.FillPath(&f, &p);
                    } else {
                        Gdiplus::SolidBrush f(Gdiplus::Color(
                            hot ? 40 : 22, 255, 255, 255));
                        g.FillPath(&f, &p);
                        Gdiplus::Pen pn(Gdiplus::Color(60, 255, 255, 255),
                                        1.0f);
                        g.DrawPath(&pn, &p);
                    }
                    SelectObject(dc, c->bodyFont);
                    SetTextColor(dc, primary ? RGB(255, 255, 255)
                                             : RGB(200, 208, 228));
                    DrawTextA(dc, label, -1, &b,
                              DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                };
                btn(c->cancelRect, "Cancel", false, c->hot == 1);
                btn(c->okRect, "OK", true, c->hot == 0);
                // Outer border
                {
                    Gdiplus::GraphicsPath bp;
                    addRoundRect(bp, 0, 0, w - 1, h - 1, 12);
                    Gdiplus::Pen bpn(Gdiplus::Color(255, 58, 72, 110), 1.0f);
                    g.DrawPath(&bpn, &bp);
                }
                BitBlt(screen, 0, 0, w, h, dc, 0, 0, SRCCOPY);
                SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
                EndPaint(hw, &ps);
                return 0;
            }
            case WM_MOUSEMOVE: {
                if (!c) break;
                POINT p = {static_cast<short>(LOWORD(lp)),
                           static_cast<short>(HIWORD(lp))};
                int hot = PtInRect(&c->okRect, p) ? 0
                        : PtInRect(&c->cancelRect, p) ? 1
                        : PtInRect(&c->closeRect, p) ? 2 : -1;
                if (hot != c->hot) { c->hot = hot;
                    InvalidateRect(hw, nullptr, FALSE); }
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hw, 0};
                TrackMouseEvent(&tme);
                return 0;
            }
            case WM_MOUSELEAVE:
                if (c && c->hot != -1) { c->hot = -1;
                    InvalidateRect(hw, nullptr, FALSE); }
                return 0;
            case WM_LBUTTONUP: {
                if (!c) break;
                POINT p = {static_cast<short>(LOWORD(lp)),
                           static_cast<short>(HIWORD(lp))};
                if (PtInRect(&c->okRect, p)) commit(hw, c, true);
                else if (PtInRect(&c->cancelRect, p) ||
                         PtInRect(&c->closeRect, p)) DestroyWindow(hw);
                return 0;
            }
            case WM_NCHITTEST: {
                LRESULT hit = DefWindowProcA(hw, msg, wp, lp);
                if (hit == HTCLIENT && c) {
                    POINT p = {static_cast<short>(LOWORD(lp)),
                               static_cast<short>(HIWORD(lp))};
                    ScreenToClient(hw, &p);
                    if (p.y < 52 && !PtInRect(&c->closeRect, p))
                        return HTCAPTION;
                }
                return hit;
            }
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcA(hw, msg, wp, lp);
        }
    };

    static bool reg = false;
    if (!reg) {
        WNDCLASSA cls = {};
        cls.style = CS_DROPSHADOW; // lift the dialog off the background
        cls.lpfnWndProc = Dlg::proc;
        cls.hInstance = GetModuleHandleA(nullptr);
        cls.lpszClassName = "PortInputDialog";
        cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&cls);
        reg = true;
    }

    const int dw = 360, dh = 200;
    HWND dlg = CreateWindowExA(WS_EX_TOPMOST, "PortInputDialog",
        defaultVal ? defaultVal : "", WS_POPUP, 0, 0, dw, dh, parent,
        nullptr, GetModuleHandleA(nullptr), &ctx);
    if (dlg) {
        SetWindowRgn(dlg, CreateRoundRectRgn(0, 0, dw + 1, dh + 1, 16, 16),
                     TRUE);
        if (parent) {
            RECT pr; GetWindowRect(parent, &pr);
            SetWindowPos(dlg, nullptr,
                         pr.left + (pr.right - pr.left - dw) / 2,
                         pr.top + (pr.bottom - pr.top - dh) / 2, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
        }
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        MSG msg;
        while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    DeleteObject(ctx.titleFont);
    DeleteObject(ctx.bodyFont);
    return okClicked ? result : "";
}

// ── Drag ghost: a translucent icon+name that follows the cursor ──────
LRESULT CALLBACK dragGhostProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(hw, msg, wp, lp);
}

void destroyDragGhost(FolderWindowState* fs)
{
    if (fs && fs->dragGhost) {
        DestroyWindow(fs->dragGhost);
        fs->dragGhost = nullptr;
    }
}

void createDragGhost(FolderWindowState* fs)
{
    if (!fs || fs->dragGhost) return;

    static bool reg = false;
    if (!reg) {
        WNDCLASSA cls = {};
        cls.lpfnWndProc = dragGhostProc;
        cls.hInstance = GetModuleHandleA(nullptr);
        cls.lpszClassName = "PortDragGhost";
        RegisterClassA(&cls);
        reg = true;
    }

    const int gw = 190, gh = 56;
    HWND ghost = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        "PortDragGhost", "", WS_POPUP, 0, 0, gw, gh,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!ghost) return;
    fs->dragGhost = ghost;

    // Render the ghost into a 32-bit DIB and push it with UpdateLayeredWindow
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = gw;
    bi.bmiHeader.biHeight = -gh; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr,
                                   0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        Gdiplus::GraphicsPath pill;
        addRoundRect(pill, 0, 0, gw - 1, gh - 1, 10);
        Gdiplus::SolidBrush fill(Gdiplus::Color(190, 18, 24, 42));
        g.FillPath(&fill, &pill);
        Gdiplus::Pen pen(Gdiplus::Color(210, 90, 160, 255), 1.4f);
        g.DrawPath(&pen, &pill);

        if (fs->dragIsDir && folderIconImg) {
            g.DrawImage(folderIconImg, 10, 12, 32, 32);
        } else {
            Gdiplus::GraphicsPath page;
            addRoundRect(page, 16, 12, 22, 32, 4);
            Gdiplus::SolidBrush pf(Gdiplus::Color(230, 214, 224, 246));
            g.FillPath(&pf, &page);
        }

        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font font(&family, 11, Gdiplus::FontStyleRegular,
                           Gdiplus::UnitPixel);
        Gdiplus::SolidBrush text(Gdiplus::Color(235, 224, 232, 248));
        std::wstring wide(fs->dragName.begin(), fs->dragName.end());
        Gdiplus::RectF box(52.0f, 18.0f, static_cast<float>(gw - 62), 24.0f);
        Gdiplus::StringFormat sf;
        sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        g.DrawString(wide.c_str(), -1, &font, box, &sf, &text);
    }

    POINT src = {0, 0};
    SIZE size = {gw, gh};
    POINT pos;
    GetCursorPos(&pos);
    pos.x += 14;
    pos.y += 10;
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(ghost, screen, &pos, &size, dc, &src, 0, &blend,
                        ULW_ALPHA);
    ShowWindow(ghost, SW_SHOWNOACTIVATE);

    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

void moveDragGhost(FolderWindowState* fs)
{
    if (!fs || !fs->dragGhost) return;
    POINT pos;
    GetCursorPos(&pos);
    SetWindowPos(fs->dragGhost, HWND_TOPMOST, pos.x + 14, pos.y + 10, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Which grid cell is under a client point? -1 when none.
int folderGridHitTest(HWND folderWnd, FolderWindowState* fs, POINT pt)
{
    if (!fs || fs->viewMode == 0 || fs->items.empty()) return -1;
    RECT rc;
    GetClientRect(folderWnd, &rc);
    const FolderCell cell = folderCellFor(fs->viewMode);
    const int gridTop = FolderHeaderH + 8;
    const int gridLeft = 16;
    if (pt.y < gridTop) return -1;
    const int cols = std::max(1, static_cast<int>(rc.right - 32) / cell.w);
    const int col = static_cast<int>(pt.x - gridLeft) / cell.w;
    const int row = static_cast<int>(pt.y - gridTop + fs->scrollY) / cell.h;
    if (col < 0 || col >= cols || row < 0) return -1;
    const int idx = row * cols + col;
    if (idx < 0 || idx >= static_cast<int>(fs->items.size())) return -1;
    return idx;
}

// ── Dragging items out of a folder window ────────────────────────────
WNDPROC gFolderListOrigProc = nullptr;

// Resolve where the item was dropped and perform the move.
void dropFolderItem(HWND folderWnd, FolderWindowState* fs, POINT screenPt)
{
    if (!fs || fs->dragName.empty()) return;
    HWND desktopWnd = GetParent(folderWnd);
    auto* ds = reinterpret_cast<DesktopState*>(
        GetWindowLongPtrA(desktopWnd, GWLP_USERDATA));
    if (!ds) return;

    const std::string src = fs->currentDir.empty()
        ? fs->dragName : fs->currentDir + "/" + fs->dragName;

    // Deterministic drop target by screen rectangles — WindowFromPoint is
    // unreliable across overlapping/owned windows.
    RECT trashR{}, desktopR{}, folderR{};
    if (HWND trashBtn = GetDlgItem(desktopWnd, IdIconTrashBin)) {
        GetWindowRect(trashBtn, &trashR);
    }
    GetWindowRect(desktopWnd, &desktopR);
    GetWindowRect(folderWnd, &folderR);

    bool handled = false;
    if (PtInRect(&trashR, screenPt)) {
        auto r = ds->kernel.getSandbox().moveToTrash(src);
        if (!r) {
            MessageBoxA(folderWnd, r.error().detail.c_str(),
                        "Could not move item", MB_OK | MB_ICONWARNING);
            return;
        }
        handled = true;
    } else if (PtInRect(&desktopR, screenPt) &&
               !PtInRect(&folderR, screenPt)) {
        // Dropped on a visible part of the desktop (not on this window)
        const std::string dest = "Desktop/" + fs->dragName;
        if (dest != src) {
            auto r = ds->kernel.getSandbox().rename(src, dest);
            if (!r) {
                MessageBoxA(folderWnd, r.error().detail.c_str(),
                            "Could not move item", MB_OK | MB_ICONWARNING);
                return;
            }

            // Remember the requested drop cell before rebuilding the desktop
            // icon list, so the moved item appears where the user released it.
            POINT desktopPt = screenPt;
            ScreenToClient(desktopWnd, &desktopPt);
            const auto layout = getIconLayoutInfo(ds->iconSize);
            RECT desktopClient{};
            GetClientRect(desktopWnd, &desktopClient);
            const int maxX = std::max(
                28, static_cast<int>(desktopClient.right) - layout.width);
            const int maxY = std::max(
                34, static_cast<int>(desktopClient.bottom) -
                        kTaskbarReserved - layout.height);
            const int rawX = std::clamp(
                static_cast<int>(desktopPt.x) - layout.width / 2,
                                        28, maxX);
            const int rawY = std::clamp(
                static_cast<int>(desktopPt.y) - layout.height / 2,
                                        34, maxY);
            const int col = std::max(
                0, (rawX - 28 + layout.gapX / 2) / layout.gapX);
            const int row = std::max(
                0, (rawY - 34 + layout.gapY / 2) / layout.gapY);
            ds->dynamicIconSavedPos[fs->dragName] = {
                28 + col * layout.gapX, 34 + row * layout.gapY};
            handled = true;
        }
    }
    if (!handled) return;

    if (HWND lb = GetDlgItem(folderWnd, IdMyPortList)) {
        refreshMyPortList(lb, &ds->kernel.getSandbox(), fs->currentDir);
    }
    refreshFolderItems(fs);
    InvalidateRect(folderWnd, nullptr, FALSE);
    refreshDesktopDynamicIcons(desktopWnd, ds);
    if (ds->trashWindow && IsWindow(ds->trashWindow)) {
        if (HWND tlb = GetDlgItem(ds->trashWindow, IdTrashList)) {
            refreshTrashList(tlb, &ds->kernel.getSandbox());
        }
    }
    if (HWND tb = GetDlgItem(desktopWnd, IdIconTrashBin)) {
        RedrawWindow(tb, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
}

// Finish a folder-item drag from either the custom icon grid or its listbox.
// The short timer is a safety net for Windows capture changes: even if another
// child window steals mouse capture, releasing the button still performs the
// drop instead of silently cancelling it.
void finishFolderItemDrag(HWND eventWnd, HWND folderWnd,
                          FolderWindowState* fs, bool performDrop)
{
    if (!fs || !fs->dragging) return;
    POINT screenPt{};
    GetCursorPos(&screenPt);
    fs->dragging = false;
    fs->maybeDrag = false;
    KillTimer(eventWnd, IdFolderDragTimer);
    if (GetCapture() == eventWnd) ReleaseCapture();
    destroyDragGhost(fs);
    if (performDrop) dropFolderItem(folderWnd, fs, screenPt);
}

// Poll from mouse-down onward. The custom-painted grid can lose regular
// move/up routing when the cursor leaves its top-level window; polling keeps
// the drag deterministic without depending on those missing messages.
void pollFolderItemDrag(HWND eventWnd, HWND folderWnd, FolderWindowState* fs)
{
    if (!fs || (!fs->maybeDrag && !fs->dragging)) {
        KillTimer(eventWnd, IdFolderDragTimer);
        return;
    }

    const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    POINT screenPt{};
    GetCursorPos(&screenPt);

    if (!fs->dragging) {
        if (!leftDown) {
            fs->maybeDrag = false;
            KillTimer(eventWnd, IdFolderDragTimer);
            return;
        }
        if (std::abs(screenPt.x - fs->dragStartScreen.x) > 4 ||
            std::abs(screenPt.y - fs->dragStartScreen.y) > 4) {
            fs->dragging = true;
            createDragGhost(fs);
            SetCapture(eventWnd);
        }
    }

    if (fs->dragging) {
        moveDragGhost(fs);
        if (!leftDown) finishFolderItemDrag(eventWnd, folderWnd, fs, true);
    }
}

LRESULT CALLBACK folderListProc(HWND lb, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND folderWnd = GetParent(lb);
    auto* fs = reinterpret_cast<FolderWindowState*>(
        GetWindowLongPtrA(folderWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_LBUTTONDOWN:
        if (fs) {
            const POINT pt = {static_cast<short>(LOWORD(lp)),
                              static_cast<short>(HIWORD(lp))};
            const DWORD hit = static_cast<DWORD>(SendMessageA(
                lb, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y)));
            if (HIWORD(hit) == 0) {
                char buf[MAX_PATH] = {};
                SendMessageA(lb, LB_GETTEXT, LOWORD(hit),
                             reinterpret_cast<LPARAM>(buf));
                std::string name(buf);
                if (name != "..") {
                    fs->dragIsDir = (!name.empty() && name.back() == '/');
                    if (fs->dragIsDir) name.pop_back();
                    fs->dragName = name;
                    fs->maybeDrag = true;
                    fs->dragStart = pt;
                    GetCursorPos(&fs->dragStartScreen);
                    SetTimer(lb, IdFolderDragTimer, 16, nullptr);
                }
            }
        }
        break;
    case WM_MOUSEMOVE:
        if (fs && fs->maybeDrag && !fs->dragging && (wp & MK_LBUTTON)) {
            const POINT pt = {static_cast<short>(LOWORD(lp)),
                              static_cast<short>(HIWORD(lp))};
            if (std::abs(pt.x - fs->dragStart.x) > 4 ||
                std::abs(pt.y - fs->dragStart.y) > 4) {
                fs->dragging = true;
                createDragGhost(fs);
                SetCapture(lb);
            }
        }
        if (fs && fs->dragging) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            moveDragGhost(fs);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (fs && fs->dragging) {
            finishFolderItemDrag(lb, folderWnd, fs, true);
            return 0;
        }
        if (fs) {
            fs->maybeDrag = false;
            KillTimer(lb, IdFolderDragTimer);
        }
        break;
    case WM_TIMER:
        if (wp == IdFolderDragTimer && fs) {
            pollFolderItemDrag(lb, folderWnd, fs);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        // Do not cancel here. Some child/topmost windows can steal capture
        // during an otherwise valid drag; the timer completes it on release.
        break;
    case WM_CANCELMODE:
        if (fs && fs->dragging)
            finishFolderItemDrag(lb, folderWnd, fs, false);
        break;
    }
    return CallWindowProcA(gFolderListOrigProc, lb, msg, wp, lp);
}

void subclassFolderList(HWND listBox)
{
    if (!listBox) return;
    WNDPROC orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
        listBox, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(folderListProc)));
    if (!gFolderListOrigProc) gFolderListOrigProc = orig;
}

LRESULT CALLBACK folderWindowProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* folderState = reinterpret_cast<FolderWindowState*>(GetWindowLongPtrA(hw, GWLP_USERDATA));

    switch (msg) {
    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) {
            // Hide completely instead of the owner-window stub; the dock
            // tile is the way back.
            ShowWindow(hw, SW_HIDE);
            if (HWND desktopWnd = GetParent(hw)) {
                RECT dr; GetClientRect(desktopWnd, &dr);
                dr.top = dr.bottom - kDockH;
                InvalidateRect(desktopWnd, &dr, FALSE);
            }
            return 0;
        }
        HWND lb = GetDlgItem(hw, IdMyPortList);
        RECT r;
        GetClientRect(hw, &r);
        const int w = r.right - r.left;
        const int h = r.bottom - r.top;
        const bool listMode = !folderState || folderState->viewMode == 0;
        if (lb) {
            MoveWindow(lb, 12, FolderHeaderH, w - 24,
                       h - FolderHeaderH - 12, TRUE);
            ShowWindow(lb, listMode ? SW_SHOW : SW_HIDE);
        }
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC screen = BeginPaint(hw, &ps);
        RECT rc;
        GetClientRect(hw, &rc);
        const int w = rc.right;
        const int hFull = rc.bottom;
        const bool iconMode = folderState && folderState->viewMode != 0;
        const int paintH = iconMode ? hFull : FolderHeaderH;
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, paintH);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);

        RECT hdr = {0, 0, w, paintH};
        HBRUSH bg = CreateSolidBrush(RGB(14, 18, 30));
        FillRect(dc, &hdr, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);

        // The window is created visible, so the first WM_PAINT can arrive
        // before folderState is attached — just show the plain header.
        if (!folderState) {
            BitBlt(screen, 0, 0, w, paintH, dc, 0, 0, SRCCOPY);
            SelectObject(dc, oldBmp);
            DeleteObject(bmp);
            DeleteDC(dc);
            EndPaint(hw, &ps);
            return 0;
        }

        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        static HFONT fTitle = CreateFontA(-19, 0, 0, 0, FW_SEMIBOLD, FALSE,
            FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        static HFONT fCrumb = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE,
            FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");

        // Title = window caption; item count on the far right
        char title[128] = {};
        GetWindowTextA(hw, title, sizeof(title));
        SelectObject(dc, fTitle);
        SetTextColor(dc, RGB(232, 236, 248));
        RECT tr = {20, 14, w - 200, 40};
        DrawTextA(dc, title, -1, &tr,
                  DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);

        int itemCount = 0;
        if (HWND lb = GetDlgItem(hw, IdMyPortList)) {
            itemCount = static_cast<int>(SendMessageA(lb, LB_GETCOUNT, 0, 0));
            if (folderState && !folderState->currentDir.empty() &&
                itemCount > 0) {
                --itemCount; // don't count the ".." row
            }
        }

        // Toolbar buttons (right side of header): Up, New Folder, Refresh
        const int by = 12, bs = 30;
        int bx = w - 16 - bs;
        auto drawTbBtn = [&](RECT& out, int hotIndex, auto glyph) {
            RECT b = {bx, by, bx + bs, by + bs};
            out = b;
            Gdiplus::GraphicsPath path;
            addRoundRect(path, b.left, b.top, bs, bs, 8);
            Gdiplus::SolidBrush fill(Gdiplus::Color(
                folderState && folderState->hotTb == hotIndex ? 46 : 28,
                255, 255, 255));
            g.FillPath(&fill, &path);
            glyph(b);
            bx -= bs + 8;
        };
        Gdiplus::Pen glyphPen(Gdiplus::Color(255, 200, 210, 235), 1.8f);
        // Refresh (circular arrow)
        drawTbBtn(folderState->tbRefresh, 2, [&](RECT b) {
            const int cx2 = (b.left + b.right) / 2, cy2 = (b.top + b.bottom) / 2;
            g.DrawArc(&glyphPen, cx2 - 7, cy2 - 7, 14, 14, 30.0f, 280.0f);
            g.DrawLine(&glyphPen, cx2 + 6, cy2 - 7, cx2 + 8, cy2 - 2);
            g.DrawLine(&glyphPen, cx2 + 6, cy2 - 7, cx2 + 1, cy2 - 6);
        });
        // New Folder (folder + plus)
        drawTbBtn(folderState->tbNewDir, 1, [&](RECT b) {
            const int cx2 = (b.left + b.right) / 2, cy2 = (b.top + b.bottom) / 2;
            g.DrawLine(&glyphPen, cx2 - 6, cy2, cx2 + 6, cy2);
            g.DrawLine(&glyphPen, cx2, cy2 - 6, cx2, cy2 + 6);
        });
        // Up (parent folder)
        drawTbBtn(folderState->tbUp, 0, [&](RECT b) {
            const int cx2 = (b.left + b.right) / 2, cy2 = (b.top + b.bottom) / 2;
            g.DrawLine(&glyphPen, cx2, cy2 + 6, cx2, cy2 - 6);
            g.DrawLine(&glyphPen, cx2 - 5, cy2 - 1, cx2, cy2 - 6);
            g.DrawLine(&glyphPen, cx2 + 5, cy2 - 1, cx2, cy2 - 6);
        });

        // Breadcrumb + item count
        SelectObject(dc, fCrumb);
        SetTextColor(dc, RGB(130, 140, 165));
        std::string crumb = "sandbox";
        if (folderState && !folderState->currentDir.empty()) {
            crumb += " / " + folderState->currentDir;
        }
        RECT cr = {20, 48, w - 120, 70};
        DrawTextA(dc, crumb.c_str(), -1, &cr,
                  DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        char countText[32];
        std::snprintf(countText, sizeof(countText), "%d item%s", itemCount,
                      itemCount == 1 ? "" : "s");
        SetTextColor(dc, RGB(110, 120, 145));
        RECT ctr = {w - 140, 48, w - 16, 70};
        DrawTextA(dc, countText, -1, &ctr, DT_SINGLELINE | DT_RIGHT);

        // Divider under the header
        RECT line = {0, FolderHeaderH - 1, w, FolderHeaderH};
        HBRUSH lb2 = CreateSolidBrush(RGB(30, 38, 60));
        FillRect(dc, &line, lb2);
        DeleteObject(lb2);

        // ── Icon grid view ────────────────────────────────────────
        if (iconMode) {
            static HFONT fItem = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE,
                FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                "Segoe UI");
            const FolderCell cell = folderCellFor(folderState->viewMode);
            const int gridTop = FolderHeaderH + 8;
            const int gridLeft = 16;
            const int cols = std::max(1, (w - 32) / cell.w);
            const int rows = (static_cast<int>(folderState->items.size()) +
                              cols - 1) / cols;
            folderState->contentH = rows * cell.h + 16;
            const int viewH = hFull - gridTop - 8;
            const int maxScroll = std::max(0, folderState->contentH - viewH);
            if (folderState->scrollY > maxScroll)
                folderState->scrollY = maxScroll;
            if (folderState->scrollY < 0) folderState->scrollY = 0;

            const int saved = SaveDC(dc);
            IntersectClipRect(dc, 0, gridTop, w, hFull);
            g.SetClip(Gdiplus::Rect(0, gridTop, w, hFull - gridTop));

            for (std::size_t i = 0; i < folderState->items.size(); ++i) {
                const int col = static_cast<int>(i) % cols;
                const int row = static_cast<int>(i) / cols;
                RECT c = {gridLeft + col * cell.w,
                          gridTop + row * cell.h - folderState->scrollY,
                          gridLeft + col * cell.w + cell.w,
                          gridTop + row * cell.h + cell.h -
                              folderState->scrollY};
                if (c.bottom < gridTop || c.top > hFull) continue;

                const bool sel = static_cast<int>(i) == folderState->selected;
                const bool hotIt = static_cast<int>(i) == folderState->hotItem;
                if (sel || hotIt) {
                    Gdiplus::GraphicsPath sp;
                    addRoundRect(sp, c.left + 4, c.top + 4, cell.w - 8,
                                 cell.h - 8, 8);
                    Gdiplus::SolidBrush sf(sel
                        ? Gdiplus::Color(60, 90, 160, 255)
                        : Gdiplus::Color(24, 255, 255, 255));
                    g.FillPath(&sf, &sp);
                    if (sel) {
                        Gdiplus::Pen spn(Gdiplus::Color(170, 90, 160, 255),
                                         1.0f);
                        g.DrawPath(&spn, &sp);
                    }
                }

                const auto& item = folderState->items[i];
                const int ix = c.left + (cell.w - cell.icon) / 2;
                const int iy = c.top + 12;
                if (item.first == "..") {
                    Gdiplus::Pen up(Gdiplus::Color(255, 160, 180, 220), 2.4f);
                    const int ux = ix + cell.icon / 2;
                    const int uy = iy + cell.icon / 2;
                    const int s = cell.icon / 3;
                    g.DrawLine(&up, ux, uy + s, ux, uy - s);
                    g.DrawLine(&up, ux - s, uy, ux, uy - s);
                    g.DrawLine(&up, ux + s, uy, ux, uy - s);
                } else if (item.second && folderIconImg) {
                    g.DrawImage(folderIconImg, ix, iy, cell.icon, cell.icon);
                } else if (item.second) {
                    Gdiplus::SolidBrush f(Gdiplus::Color(255, 90, 150, 240));
                    g.FillRectangle(&f, ix, iy + cell.icon / 6, cell.icon,
                                    cell.icon - cell.icon / 4);
                } else {
                    Gdiplus::GraphicsPath page;
                    addRoundRect(page, ix + cell.icon / 6, iy,
                                 cell.icon - cell.icon / 3, cell.icon, 4);
                    Gdiplus::SolidBrush pf(
                        Gdiplus::Color(255, 214, 224, 246));
                    g.FillPath(&pf, &page);
                    Gdiplus::Pen pl(Gdiplus::Color(255, 120, 140, 180), 1.0f);
                    g.DrawPath(&pl, &page);
                }

                SelectObject(dc, fItem);
                SetTextColor(dc, sel ? RGB(255, 255, 255)
                                     : RGB(214, 222, 240));
                RECT tr2 = {c.left + 4, iy + cell.icon + 6, c.right - 4,
                            c.bottom - 4};
                DrawTextA(dc, item.first.c_str(), -1, &tr2,
                          DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
            }

            g.ResetClip();
            RestoreDC(dc, saved);
        }

        BitBlt(screen, 0, 0, w, paintH, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!folderState) break;
        POINT pt = {static_cast<short>(LOWORD(lp)),
                    static_cast<short>(HIWORD(lp))};

        // Icon-grid hover + drag-out
        if (folderState->viewMode != 0) {
            if (folderState->maybeDrag && !folderState->dragging &&
                (wp & MK_LBUTTON)) {
                if (std::abs(pt.x - folderState->dragStart.x) > 4 ||
                    std::abs(pt.y - folderState->dragStart.y) > 4) {
                    folderState->dragging = true;
                    // Create the ghost BEFORE grabbing capture: showing a new
                    // window can release an existing capture, which would send
                    // the drop's WM_LBUTTONUP to the wrong window.
                    createDragGhost(folderState);
                    SetCapture(hw);
                }
            }
            if (folderState->dragging) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                moveDragGhost(folderState);
                return 0;
            }
            const int hit = folderGridHitTest(hw, folderState, pt);
            if (hit != folderState->hotItem) {
                folderState->hotItem = hit;
                InvalidateRect(hw, nullptr, FALSE);
                TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE,
                                       hw, 0};
                TrackMouseEvent(&tme);
            }
        }

        int hot = -1;
        if (PtInRect(&folderState->tbUp, pt)) hot = 0;
        else if (PtInRect(&folderState->tbNewDir, pt)) hot = 1;
        else if (PtInRect(&folderState->tbRefresh, pt)) hot = 2;
        if (hot != folderState->hotTb) {
            folderState->hotTb = hot;
            RECT hdr = {0, 0, 0, FolderHeaderH};
            GetClientRect(hw, &hdr);
            hdr.bottom = FolderHeaderH;
            InvalidateRect(hw, &hdr, FALSE);
            TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hw, 0};
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (folderState && (folderState->hotTb != -1 ||
                            folderState->hotItem != -1)) {
            folderState->hotTb = -1;
            folderState->hotItem = -1;
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (folderState && folderState->viewMode != 0) {
            POINT pt = {static_cast<short>(LOWORD(lp)),
                        static_cast<short>(HIWORD(lp))};
            const int hit = folderGridHitTest(hw, folderState, pt);
            folderState->selected = hit;
            if (hit >= 0 && folderState->items[hit].first != "..") {
                folderState->dragName = folderState->items[hit].first;
                folderState->dragIsDir = folderState->items[hit].second;
                folderState->maybeDrag = true;
                folderState->dragStart = pt;
                GetCursorPos(&folderState->dragStartScreen);
                SetTimer(hw, IdFolderDragTimer, 16, nullptr);
            } else {
                folderState->maybeDrag = false;
                KillTimer(hw, IdFolderDragTimer);
            }
            SetFocus(hw);
            InvalidateRect(hw, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (folderState && folderState->viewMode != 0) {
            POINT pt = {static_cast<short>(LOWORD(lp)),
                        static_cast<short>(HIWORD(lp))};
            const int hit = folderGridHitTest(hw, folderState, pt);
            if (hit >= 0) {
                const auto& item = folderState->items[hit];
                if (item.first == "..") {
                    const std::size_t s =
                        folderState->currentDir.find_last_of("/\\");
                    folderState->currentDir = (s == std::string::npos)
                        ? "" : folderState->currentDir.substr(0, s);
                } else if (item.second) {
                    folderState->currentDir = folderState->currentDir.empty()
                        ? item.first
                        : folderState->currentDir + "/" + item.first;
                } else {
                    break; // files: nothing to open in the grid yet
                }
                folderState->scrollY = 0;
                if (HWND lb2 = GetDlgItem(hw, IdMyPortList)) {
                    refreshMyPortList(lb2,
                                      &folderState->kernel->getSandbox(),
                                      folderState->currentDir);
                }
                refreshFolderItems(folderState);
                InvalidateRect(hw, nullptr, FALSE);
            }
        }
        break;
    case WM_MOUSEWHEEL:
        if (folderState && folderState->viewMode != 0) {
            RECT rc2;
            GetClientRect(hw, &rc2);
            const int viewH = rc2.bottom - (FolderHeaderH + 8) - 8;
            const int maxScroll =
                std::max(0, folderState->contentH - viewH);
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int s = folderState->scrollY - (delta / WHEEL_DELTA) * 48;
            if (s < 0) s = 0;
            if (s > maxScroll) s = maxScroll;
            if (s != folderState->scrollY) {
                folderState->scrollY = s;
                InvalidateRect(hw, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP: {
        if (!folderState) break;
        POINT pt = {static_cast<short>(LOWORD(lp)),
                    static_cast<short>(HIWORD(lp))};
        // Finish an icon-grid drag-out
        if (folderState->viewMode != 0 && folderState->dragging) {
            finishFolderItemDrag(hw, hw, folderState, true);
            return 0;
        }
        folderState->maybeDrag = false;
        KillTimer(hw, IdFolderDragTimer);
        HWND lb = GetDlgItem(hw, IdMyPortList);
        if (PtInRect(&folderState->tbUp, pt)) {
            if (lb && !folderState->currentDir.empty()) {
                std::size_t s = folderState->currentDir.find_last_of("/\\");
                folderState->currentDir = (s == std::string::npos)
                    ? "" : folderState->currentDir.substr(0, s);
                refreshMyPortList(lb, &folderState->kernel->getSandbox(),
                                  folderState->currentDir);
                refreshFolderItems(folderState);
                folderState->scrollY = 0;
                InvalidateRect(hw, nullptr, TRUE);
            }
            return 0;
        }
        if (PtInRect(&folderState->tbNewDir, pt)) {
            SendMessageA(hw, WM_COMMAND, IdFolderCtxNewDir, 0);
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&folderState->tbRefresh, pt)) {
            SendMessageA(hw, WM_COMMAND, IdFolderCtxRefresh, 0);
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_COMMAND: {
        int controlId = LOWORD(wp);
        int notifCode = HIWORD(wp);

        if (controlId == IdMyPortList && notifCode == LBN_DBLCLK) {
            HWND lb = GetDlgItem(hw, IdMyPortList);
            int idx = SendMessageA(lb, LB_GETCURSEL, 0, 0);
            if (idx == LB_ERR) break;
            char buf[MAX_PATH];
            SendMessageA(lb, LB_GETTEXT, idx, reinterpret_cast<LPARAM>(buf));
            std::string selected(buf);

            if (selected == "..") {
                if (folderState) {
                    std::size_t s = folderState->currentDir.find_last_of("/\\");
                    folderState->currentDir = (s == std::string::npos) ? "" : folderState->currentDir.substr(0, s);
                    refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
                    refreshFolderItems(folderState);
                    folderState->scrollY = 0;
                    InvalidateRect(hw, nullptr, TRUE);
                }
            } else if (!selected.empty() && selected.back() == '/') {
                selected.pop_back();
                if (folderState) {
                    folderState->currentDir = folderState->currentDir.empty() ? selected : folderState->currentDir + "/" + selected;
                    refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
                    refreshFolderItems(folderState);
                    folderState->scrollY = 0;
                    InvalidateRect(hw, nullptr, TRUE);
                }
            }
        } else if (controlId == IdMyPortRefresh) {
            if (folderState) {
                HWND lb = GetDlgItem(hw, IdMyPortList);
                refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
            }
        } else if (controlId == IdFolderCtxNewFile) {
            if (!folderState) break;
            std::string name = showInputDialog(hw, "New File", "File name:", "NewFile.txt");
            if (!name.empty()) {
                std::string path = folderState->currentDir.empty() ? name : folderState->currentDir + "/" + name;
                (void)folderState->kernel->getSandbox().write(path, "");
                HWND lb = GetDlgItem(hw, IdMyPortList);
                refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
            }
        } else if (controlId == IdFolderCtxNewDir) {
            if (!folderState) break;
            std::string name = showInputDialog(hw, "New Folder", "Folder name:", "New Folder");
            if (!name.empty()) {
                std::string path = folderState->currentDir.empty() ? name : folderState->currentDir + "/" + name;
                (void)folderState->kernel->getSandbox().makeDir(path);
                HWND lb = GetDlgItem(hw, IdMyPortList);
                refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
            }
        } else if (controlId == IdFolderViewList ||
                   controlId == IdFolderViewSmall ||
                   controlId == IdFolderViewMedium ||
                   controlId == IdFolderViewLarge) {
            if (!folderState) break;
            folderState->viewMode =
                controlId == IdFolderViewList   ? 0 :
                controlId == IdFolderViewSmall  ? 1 :
                controlId == IdFolderViewMedium ? 2 : 3;
            folderState->scrollY = 0;
            if (HWND lb2 = GetDlgItem(hw, IdMyPortList)) {
                ShowWindow(lb2, folderState->viewMode == 0 ? SW_SHOW
                                                           : SW_HIDE);
            }
            refreshFolderItems(folderState);
            InvalidateRect(hw, nullptr, TRUE);
        } else if (controlId == IdFolderCtxRename) {
            if (!folderState) break;
            HWND lb = GetDlgItem(hw, IdMyPortList);
            std::string oldName = folderSelectedName(hw, folderState);
            if (oldName.empty() || oldName == "..") break;
            std::string newName = showInputDialog(hw, "Rename", "New name:", oldName.c_str());
            if (!newName.empty() && newName != oldName) {
                std::string oldPath = folderState->currentDir.empty() ? oldName : folderState->currentDir + "/" + oldName;
                std::string newPath = folderState->currentDir.empty() ? newName : folderState->currentDir + "/" + newName;
                (void)folderState->kernel->getSandbox().rename(oldPath, newPath);
                refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
            }
        } else if (controlId == IdFolderCtxDelete) {
            if (!folderState) break;
            HWND lb = GetDlgItem(hw, IdMyPortList);
            std::string name = folderSelectedName(hw, folderState);
            if (name.empty() || name == "..") break;
            std::string path = folderState->currentDir.empty() ? name : folderState->currentDir + "/" + name;
            std::string confirmMsg = "Move '" + name + "' to Trash?";
            if (MessageBoxA(hw, confirmMsg.c_str(), "Delete", MB_YESNO | MB_ICONWARNING) == IDYES) {
                (void)folderState->kernel->getSandbox().moveToTrash(path);
                refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
                
                HWND desktopWnd = GetParent(hw);
                DesktopState* desktopState = reinterpret_cast<DesktopState*>(GetWindowLongPtrA(desktopWnd, GWLP_USERDATA));
                if (desktopState) {
                    refreshDesktopDynamicIcons(desktopWnd, desktopState);
                    if (desktopState->trashWindow && IsWindow(desktopState->trashWindow)) {
                        HWND trashLb = GetDlgItem(desktopState->trashWindow, IdTrashList);
                        if (trashLb) {
                            refreshTrashList(trashLb, &folderState->kernel->getSandbox());
                            RedrawWindow(desktopState->trashWindow, nullptr,
                                         nullptr, RDW_INVALIDATE |
                                                  RDW_ALLCHILDREN |
                                                  RDW_UPDATENOW);
                        }
                    }
                }
                // Repaint the desktop Trash Bin icon LAST (after the icon
                // rebuild) so it switches to "full" right away, without
                // needing the folder window to be closed.
                if (HWND trashBtn = GetDlgItem(desktopWnd, IdIconTrashBin)) {
                    RedrawWindow(trashBtn, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
                }
            }
        } else if (controlId == IdFolderCtxRefresh) {
            if (folderState) {
                HWND lb = GetDlgItem(hw, IdMyPortList);
                refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
            }
        }
        // Keep the icon-grid items in sync with the listbox, then repaint
        if (folderState) refreshFolderItems(folderState);
        InvalidateRect(hw, nullptr, FALSE);
        break;
    }
    case WM_CONTEXTMENU: {
        // Right-click in the folder window
        if (!folderState) break;
        POINT pt;
        GetCursorPos(&pt);

        // Right-clicking a grid cell selects it first
        if (folderState->viewMode != 0) {
            POINT cp = pt;
            ScreenToClient(hw, &cp);
            const int hit = folderGridHitTest(hw, folderState, cp);
            if (hit != folderState->selected) {
                folderState->selected = hit;
                InvalidateRect(hw, nullptr, FALSE);
                UpdateWindow(hw);
            }
        }

        HMENU hMenu = CreatePopupMenu();
        // View submenu
        HMENU hView = CreatePopupMenu();
        const int vm = folderState->viewMode;
        AppendMenuA(hView, MF_STRING | (vm == 3 ? MF_CHECKED : 0),
                    IdFolderViewLarge,  "Large Icons");
        AppendMenuA(hView, MF_STRING | (vm == 2 ? MF_CHECKED : 0),
                    IdFolderViewMedium, "Medium Icons");
        AppendMenuA(hView, MF_STRING | (vm == 1 ? MF_CHECKED : 0),
                    IdFolderViewSmall,  "Small Icons");
        AppendMenuA(hView, MF_STRING | (vm == 0 ? MF_CHECKED : 0),
                    IdFolderViewList,   "List");
        AppendMenuA(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hView),
                    "View");
        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(hMenu, MF_STRING, IdFolderCtxNewFile,  "New File");
        AppendMenuA(hMenu, MF_STRING, IdFolderCtxNewDir,   "New Folder");

        bool haveSel = false;
        if (folderState->viewMode == 0) {
            HWND lb = GetDlgItem(hw, IdMyPortList);
            haveSel = lb && SendMessageA(lb, LB_GETCURSEL, 0, 0) != LB_ERR;
        } else {
            haveSel = folderState->selected >= 0 &&
                      folderState->selected <
                          static_cast<int>(folderState->items.size()) &&
                      folderState->items[folderState->selected].first != "..";
        }
        if (haveSel) {
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(hMenu, MF_STRING, IdFolderCtxRename, "Rename");
            AppendMenuA(hMenu, MF_STRING, IdFolderCtxDelete, "Move to Trash");
        }
        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(hMenu, MF_STRING, IdFolderCtxRefresh, "Refresh");
        std::vector<std::unique_ptr<PortMenuItem>> menuTheme;
        themePortMenu(hMenu, menuTheme);
        TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hw, nullptr);
        DestroyMenu(hMenu);
        break;
    }
    case WM_MEASUREITEM: {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (measurePortMenuItem(mis)) return TRUE;
        // Set item height for owner-draw listbox
        if (mis->CtlID == static_cast<UINT>(IdMyPortList)) {
            mis->itemHeight = 32;
        }
        return TRUE;
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (drawPortMenuItem(dis)) return TRUE;
        if (dis->CtlID == static_cast<UINT>(IdMyPortList) && dis->itemID != static_cast<UINT>(LB_ERR)) {
            char buf[MAX_PATH] = {};
            SendMessageA(dis->hwndItem, LB_GETTEXT, dis->itemID, reinterpret_cast<LPARAM>(buf));
            std::string name(buf);
            bool isDir = name == ".." || (!name.empty() && name.back() == '/');
            drawFolderItem(dis, isDir);
        }
        return TRUE;
    }
    case WM_CTLCOLORLISTBOX: {
        static HBRUSH lbBrush = CreateSolidBrush(RGB(16, 20, 34));
        return reinterpret_cast<INT_PTR>(lbBrush);
    }
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT r;
        GetClientRect(hw, &r);
        HBRUSH bg = CreateSolidBrush(RGB(14, 18, 30));
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        return 1;
    }
    case WM_TIMER:
        if (wp == IdFolderDragTimer && folderState) {
            pollFolderItemDrag(hw, hw, folderState);
            return 0;
        }
        break;
    case WM_CANCELMODE:
        if (folderState && folderState->dragging)
            finishFolderItemDrag(hw, hw, folderState, false);
        break;
    case WM_DESTROY:
        if (folderState) {
            destroyDragGhost(folderState);
            if (folderState->desktop && folderState->dockIconId != -1) {
                folderState->desktop->folderWindows.erase(
                    folderState->dockIconId);
                if (HWND desktopWnd = GetParent(hw)) {
                    RECT dr; GetClientRect(desktopWnd, &dr);
                    dr.top = dr.bottom - kDockH;
                    InvalidateRect(desktopWnd, &dr, FALSE);
                }
            }
            delete folderState;
            SetWindowLongPtrA(hw, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

LRESULT CALLBACK trashWindowProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IdTrashEmpty) {
            port::kernel::AIKernel* kernel = reinterpret_cast<port::kernel::AIKernel*>(GetWindowLongPtrA(hw, GWLP_USERDATA));
            if (kernel) {
                (void)kernel->getSandbox().emptyTrash();
                HWND lb = GetDlgItem(hw, IdTrashList);
                refreshTrashList(lb, &kernel->getSandbox());
                // Desktop trash icon switches back to "empty"
                HWND desktop = GetParent(hw);
                if (desktop) {
                    HWND trashBtn = GetDlgItem(desktop, IdIconTrashBin);
                    if (trashBtn) InvalidateRect(trashBtn, nullptr, TRUE);
                }
            }
        }
        return 0;

    case WM_MEASUREITEM: {
        auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (mi->CtlID == IdTrashList) mi->itemHeight = 32;
        return TRUE;
    }

    case WM_DRAWITEM: {
        auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);

        if (di->CtlID == IdTrashEmpty) {
            // Rounded dark-red action button
            Gdiplus::Graphics g(di->hDC);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const RECT rc = di->rcItem;
            HBRUSH bgFill = CreateSolidBrush(RGB(10, 14, 26));
            FillRect(di->hDC, &rc, bgFill);
            DeleteObject(bgFill);

            const bool pressed = (di->itemState & ODS_SELECTED) != 0;
            Gdiplus::GraphicsPath bp;
            addRoundRect(bp, rc.left, rc.top,
                         rc.right - rc.left, rc.bottom - rc.top, 8);
            Gdiplus::SolidBrush bf(pressed
                ? Gdiplus::Color(255, 120, 35, 45)
                : Gdiplus::Color(255, 82, 24, 34));
            g.FillPath(&bf, &bp);
            Gdiplus::Pen bpen(Gdiplus::Color(170, 235, 90, 105), 1.2f);
            g.DrawPath(&bpen, &bp);

            SetBkMode(di->hDC, TRANSPARENT);
            SetTextColor(di->hDC, RGB(255, 190, 195));
            RECT tr = rc;
            DrawTextA(di->hDC, "Empty Trash Bin", -1, &tr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        if (di->CtlID == IdTrashList) {
            if (di->itemID == static_cast<UINT>(-1)) return TRUE;

            char text[MAX_PATH] = {};
            SendMessageA(di->hwndItem, LB_GETTEXT, di->itemID,
                         reinterpret_cast<LPARAM>(text));
            const LRESULT kind = SendMessageA(di->hwndItem, LB_GETITEMDATA,
                                              di->itemID, 0);

            const RECT rc = di->rcItem;
            const bool selected = (di->itemState & ODS_SELECTED) != 0;
            HBRUSH rowBg = CreateSolidBrush(selected ? RGB(30, 45, 85)
                                                     : RGB(13, 18, 34));
            FillRect(di->hDC, &rc, rowBg);
            DeleteObject(rowBg);

            Gdiplus::Graphics g(di->hDC);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            SetBkMode(di->hDC, TRANSPARENT);

            if (kind == 2) {
                // Placeholder "empty" row — dim, centered, no glyph
                SetTextColor(di->hDC, RGB(100, 115, 150));
                RECT tr = rc;
                DrawTextA(di->hDC, text, -1, &tr,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }

            const int gx = rc.left + 12;
            const int gy = (rc.top + rc.bottom) / 2;
            if (kind == 1) {
                // Folder glyph (amber)
                Gdiplus::SolidBrush fb(Gdiplus::Color(255, 235, 180, 60));
                Gdiplus::GraphicsPath tab;
                addRoundRect(tab, gx, gy - 8, 9, 5, 2);
                g.FillPath(&fb, &tab);
                Gdiplus::GraphicsPath body;
                addRoundRect(body, gx, gy - 5, 18, 12, 3);
                g.FillPath(&fb, &body);
            } else {
                // File glyph (blue-gray document)
                Gdiplus::GraphicsPath doc;
                addRoundRect(doc, gx + 2, gy - 8, 13, 16, 3);
                Gdiplus::SolidBrush db(Gdiplus::Color(255, 150, 175, 220));
                g.FillPath(&db, &doc);
                Gdiplus::Pen ln(Gdiplus::Color(255, 60, 80, 120), 1.0f);
                g.DrawLine(&ln, gx + 5, gy - 3, gx + 12, gy - 3);
                g.DrawLine(&ln, gx + 5, gy,     gx + 12, gy);
                g.DrawLine(&ln, gx + 5, gy + 3, gx + 12, gy + 3);
            }

            SetTextColor(di->hDC, selected ? RGB(225, 235, 255)
                                           : RGB(200, 214, 242));
            RECT tr = {rc.left + 42, rc.top, rc.right - 8, rc.bottom};
            DrawTextA(di->hDC, text, -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            return TRUE;
        }
        return TRUE;
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetBkColor(hdc, RGB(13, 18, 34));
        static HBRUSH lbBrush = CreateSolidBrush(RGB(13, 18, 34));
        return reinterpret_cast<INT_PTR>(lbBrush);
    }

    case WM_SIZE: {
        const int w = LOWORD(lp), h = HIWORD(lp);
        HWND lb  = GetDlgItem(hw, IdTrashList);
        HWND btn = GetDlgItem(hw, IdTrashEmpty);
        if (lb)  MoveWindow(lb, 12, 12, w - 24, h - 68, TRUE);
        if (btn) MoveWindow(btn, 12, h - 44, 140, 32, TRUE);
        return 0;
    }

    case WM_DESTROY:
        SetWindowLongPtrA(hw, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

// ──────────────────────────────────────────────────────────────
// My Server window — local PortOS data store
// ──────────────────────────────────────────────────────────────
// Chat panel — shows AI conversation above the taskbar
// ──────────────────────────────────────────────────────────────
LRESULT CALLBACK chatPanelProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopState* state = reinterpret_cast<DesktopState*>(
        GetWindowLongPtrA(hw, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(lp);
        SetWindowLongPtrA(hw, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT cr;
        GetClientRect(hw, &cr);
        const int W = cr.right, H = cr.bottom;

        // ── Background ───────────────────────────────────────
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(245, 8, 12, 24));
        g.FillRectangle(&bgBrush, 0, 0, W, H);

        // Top border + title bar strip
        Gdiplus::SolidBrush titleBg(Gdiplus::Color(255, 13, 18, 36));
        g.FillRectangle(&titleBg, 0, 0, W, 36);
        Gdiplus::Pen topLine(Gdiplus::Color(120, 60, 110, 220), 1.0f);
        g.DrawLine(&topLine, 0, 0, W, 0);
        Gdiplus::Pen divLine(Gdiplus::Color(60, 60, 80, 140), 1.0f);
        g.DrawLine(&divLine, 0, 36, W, 36);

        // ⚡ Super-Nova 1.0 label in title bar
        SetBkMode(dc, TRANSPARENT);
        {
            HFONT titleFont = CreateFontA(
                14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            HFONT old = static_cast<HFONT>(SelectObject(dc, titleFont));
            SetTextColor(dc, RGB(140, 180, 255));
            RECT tr = {16, 0, W - 50, 36};
            // Unicode variant — the ⚡ glyph garbles through the ANSI API
            DrawTextW(dc, L"⚡ Super-Nova 1.0", -1, &tr,
                      DT_VCENTER | DT_LEFT | DT_SINGLELINE);
            SelectObject(dc, old);
            DeleteObject(titleFont);
        }

        // [X] close button
        {
            HFONT xFont = CreateFontA(
                15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            HFONT old = static_cast<HFONT>(SelectObject(dc, xFont));
            SetTextColor(dc, RGB(140, 150, 180));
            RECT xr = {W - 40, 0, W - 4, 36};
            DrawTextA(dc, "x", -1, &xr, DT_VCENTER | DT_CENTER | DT_SINGLELINE);
            SelectObject(dc, old);
            DeleteObject(xFont);
        }

        // ── Messages ─────────────────────────────────────────
        if (!state || state->chatHistory.empty()) {
            EndPaint(hw, &ps);
            return 0;
        }

        HFONT msgFont = CreateFontA(
            15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT oldFont = static_cast<HFONT>(SelectObject(dc, msgFont));

        const int pad    = 14;
        const int bubbleMaxW = static_cast<int>(W * 0.72f);
        int y = 44;

        // Show last N messages that fit in panel
        const int totalMsgs = static_cast<int>(state->chatHistory.size());
        // Calculate from bottom up to find which messages fit
        std::vector<std::pair<int,int>> msgRects; // {startY, height} per msg
        {
            int tempY = 44;
            for (int i = 0; i < totalMsgs; ++i) {
                const auto& e = state->chatHistory[i];
                RECT measure = {0, 0, bubbleMaxW - pad*2, 0};
                DrawTextA(dc, e.text.c_str(), -1, &measure,
                          DT_WORDBREAK | DT_CALCRECT);
                int bh = measure.bottom + pad * 2;
                msgRects.push_back({tempY, bh});
                tempY += bh + 8;
            }
        }

        // Scroll so latest message is visible
        int totalH = msgRects.empty() ? 0
                     : msgRects.back().first + msgRects.back().second - 44;
        int availH = H - 44 - 8;
        int scrollOff = (totalH > availH) ? (totalH - availH) : 0;
        y = 44 - scrollOff;

        for (int i = 0; i < totalMsgs; ++i) {
            const auto& entry = state->chatHistory[i];
            const bool isUser = entry.isUser;

            // Measure bubble text
            RECT measure = {0, 0, bubbleMaxW - pad*2, 0};
            DrawTextA(dc, entry.text.c_str(), -1, &measure,
                      DT_WORDBREAK | DT_CALCRECT);
            int textW = measure.right;
            int textH = measure.bottom;
            int bw = textW + pad * 2;
            int bh = textH + pad * 2;
            if (bw > bubbleMaxW) bw = bubbleMaxW;

            int bx = isUser ? (W - bw - 16) : 16;
            int by = y;

            if (by + bh > 36 && by < H - 8) {
                // Bubble background
                Gdiplus::GraphicsPath bubblePath;
                addRoundRect(bubblePath, bx, by, bw, bh, 10);
                if (isUser) {
                    Gdiplus::SolidBrush ub(Gdiplus::Color(220, 35, 75, 175));
                    g.FillPath(&ub, &bubblePath);
                } else {
                    Gdiplus::SolidBrush ab(Gdiplus::Color(240, 22, 30, 52));
                    g.FillPath(&ab, &bubblePath);
                    Gdiplus::Pen ap(Gdiplus::Color(60, 60, 90, 160), 1.0f);
                    g.DrawPath(&ap, &bubblePath);
                }

                // Bubble text
                SetTextColor(dc, isUser ? RGB(230, 238, 255) : RGB(200, 215, 245));
                RECT tr = {bx + pad, by + pad,
                           bx + bw - pad, by + bh - pad};
                DrawTextA(dc, entry.text.c_str(), -1, &tr, DT_WORDBREAK);
            }

            y += bh + 8;
        }

        SelectObject(dc, oldFont);
        DeleteObject(msgFont);
        EndPaint(hw, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        // Close button hit test
        RECT cr2;
        GetClientRect(hw, &cr2);
        int mx = LOWORD(lp), my2 = HIWORD(lp);
        if (mx >= cr2.right - 44 && mx <= cr2.right && my2 >= 0 && my2 <= 36) {
            if (state) state->chatPanelVisible = false;
            ShowWindow(hw, SW_HIDE);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (state) state->chatPanelVisible = false;
            ShowWindow(hw, SW_HIDE);
        }
        return 0;

    case WM_MOUSEWHEEL:
        InvalidateRect(hw, nullptr, TRUE);
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

// ──────────────────────────────────────────────────────────────
// My Server — custom-painted local storage dashboard.
// Fully GDI+ drawn (no child controls) so it scales cleanly to
// any window size, including maximized/fullscreen.
// ──────────────────────────────────────────────────────────────

struct ServerEntry {
    std::string name;
    bool isDir{false};
    unsigned long long size{0};
};

struct MyServerState {
    std::string dataPath;
    std::vector<ServerEntry> entries;
    unsigned long long totalBytes{0};
    int itemCount{0};
    int scrollOffset{0};       // in rows
    int hoverButton{-1};       // toolbar hover index
    int hoverRow{-1};
    // Hit zones rebuilt on every paint
    RECT btnRects[4]{};        // Open in Explorer / Projects / Sessions / Refresh
    RECT listRect{};
    int rowH{34};
};


void rescanServerData(MyServerState* ss) {
    ss->entries.clear();
    ss->totalBytes = 0;
    ss->itemCount = 0;

    namespace fs = std::filesystem;
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(ss->dataPath, ec)) {
        ServerEntry se;
        se.name  = entry.path().filename().string();
        se.isDir = entry.is_directory(ec);
        if (se.isDir) {
            for (const auto& sub : fs::recursive_directory_iterator(
                     entry.path(), fs::directory_options::skip_permission_denied, ec)) {
                if (sub.is_regular_file(ec)) {
                    se.size += sub.file_size(ec);
                    ++ss->itemCount;
                }
            }
        } else {
            se.size = entry.file_size(ec);
            ++ss->itemCount;
        }
        ss->totalBytes += se.size;
        ss->entries.push_back(std::move(se));
    }

    std::sort(ss->entries.begin(), ss->entries.end(),
              [](const ServerEntry& a, const ServerEntry& b) {
                  if (a.isDir != b.isDir) return a.isDir;
                  return a.name < b.name;
              });
}

LRESULT CALLBACK myServerWindowProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* ss = reinterpret_cast<MyServerState*>(GetWindowLongPtrA(hw, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        char localAppData[MAX_PATH] = {};
        GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
        std::string dataPath = std::string(localAppData) + "\\PortOS";

        CreateDirectoryA(dataPath.c_str(), nullptr);
        CreateDirectoryA((dataPath + "\\projects").c_str(), nullptr);
        CreateDirectoryA((dataPath + "\\sessions").c_str(), nullptr);
        CreateDirectoryA((dataPath + "\\cache").c_str(), nullptr);

        auto* state = new MyServerState;
        state->dataPath = dataPath;
        rescanServerData(state);
        SetWindowLongPtrA(hw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC screenDC = BeginPaint(hw, &ps);
        RECT cr;
        GetClientRect(hw, &cr);
        const int W = cr.right, H = cr.bottom;
        if (W <= 0 || H <= 0 || !ss) { EndPaint(hw, &ps); return 0; }

        // Full double buffer — resizing stays clean
        HDC dc = CreateCompatibleDC(screenDC);
        HBITMAP buf = CreateCompatibleBitmap(screenDC, W, H);
        HGDIOBJ oldBuf = SelectObject(dc, buf);

        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        SetBkMode(dc, TRANSPARENT);

        // ── Background: deep navy with a subtle top glow ─────────
        Gdiplus::SolidBrush bg(Gdiplus::Color(255, 9, 13, 26));
        g.FillRectangle(&bg, 0, 0, W, H);
        Gdiplus::SolidBrush glowBand(Gdiplus::Color(18, 90, 140, 255));
        g.FillRectangle(&glowBand, 0, 0, W, 120);

        HFONT titleFont = CreateFontA(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT subFont = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT cardValFont = CreateFontA(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT cardLblFont = CreateFontA(11, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT rowFont = CreateFontA(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT btnFont = CreateFontA(13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        const int margin = 20;

        // ── Header: title + ONLINE badge ─────────────────────────
        SelectObject(dc, titleFont);
        SetTextColor(dc, RGB(225, 235, 255));
        RECT titleR = {margin, 18, W - 140, 50};
        DrawTextA(dc, "My Server", -1, &titleR, DT_LEFT | DT_TOP | DT_SINGLELINE);

        SelectObject(dc, subFont);
        SetTextColor(dc, RGB(120, 135, 170));
        RECT subR = {margin, 48, W - 140, 68};
        DrawTextA(dc, ("Local Data Storage   -   " + ss->dataPath).c_str(), -1,
                  &subR, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        // ONLINE pill, top right
        {
            const int pw = 92, ph = 26;
            const int px = W - margin - pw, py = 22;
            Gdiplus::GraphicsPath pill;
            addRoundRect(pill, px, py, pw, ph, ph / 2);
            Gdiplus::SolidBrush pf(Gdiplus::Color(40, 0, 220, 110));
            g.FillPath(&pf, &pill);
            Gdiplus::Pen pp(Gdiplus::Color(160, 0, 210, 110), 1.2f);
            g.DrawPath(&pp, &pill);
            Gdiplus::SolidBrush dot(Gdiplus::Color(255, 0, 225, 110));
            g.FillEllipse(&dot, px + 12, py + ph / 2 - 4, 8, 8);
            SelectObject(dc, cardLblFont);
            SetTextColor(dc, RGB(120, 240, 170));
            RECT onr = {px + 26, py, px + pw, py + ph};
            DrawTextA(dc, "ONLINE", -1, &onr, DT_VCENTER | DT_LEFT | DT_SINGLELINE);
        }

        // ── Stat cards ───────────────────────────────────────────
        {
            const char* labels[3] = {"STORAGE USED", "FILES", "PRIVACY"};
            std::string values[3] = {
                formatBytes(ss->totalBytes),
                std::to_string(ss->itemCount),
                "100% Local"
            };
            const int cardTop = 78, cardH = 64;
            const int gap = 12;
            const int cardW = (W - margin * 2 - gap * 2) / 3;
            for (int i = 0; i < 3; ++i) {
                const int cx = margin + i * (cardW + gap);
                Gdiplus::GraphicsPath card;
                addRoundRect(card, cx, cardTop, cardW, cardH, 10);
                Gdiplus::SolidBrush cf(Gdiplus::Color(255, 16, 22, 40));
                g.FillPath(&cf, &card);
                Gdiplus::Pen cp(Gdiplus::Color(70, 70, 95, 160), 1.0f);
                g.DrawPath(&cp, &card);

                SelectObject(dc, cardLblFont);
                SetTextColor(dc, RGB(110, 125, 160));
                RECT lr = {cx + 14, cardTop + 10, cx + cardW - 14, cardTop + 26};
                DrawTextA(dc, labels[i], -1, &lr, DT_LEFT | DT_TOP | DT_SINGLELINE);

                SelectObject(dc, cardValFont);
                SetTextColor(dc, i == 2 ? RGB(120, 240, 170) : RGB(220, 232, 255));
                RECT vr = {cx + 14, cardTop + 28, cx + cardW - 14, cardTop + cardH - 6};
                DrawTextA(dc, values[i].c_str(), -1, &vr, DT_LEFT | DT_TOP | DT_SINGLELINE);
            }
        }

        // ── Toolbar buttons ──────────────────────────────────────
        {
            const char* btnText[4] = {"Open in Explorer", "Projects", "Sessions", "Refresh"};
            const int btnTop = 158, btnH = 30;
            int bx = margin;
            SelectObject(dc, btnFont);
            for (int i = 0; i < 4; ++i) {
                RECT tm = {0, 0, 0, 0};
                DrawTextA(dc, btnText[i], -1, &tm, DT_CALCRECT | DT_SINGLELINE);
                const int bw = tm.right + 32;
                ss->btnRects[i] = {bx, btnTop, bx + bw, btnTop + btnH};

                Gdiplus::GraphicsPath bp;
                addRoundRect(bp, bx, btnTop, bw, btnH, 8);
                const bool hov = ss->hoverButton == i;
                Gdiplus::SolidBrush bf(hov
                    ? Gdiplus::Color(255, 38, 62, 120)
                    : Gdiplus::Color(255, 22, 32, 58));
                g.FillPath(&bf, &bp);
                Gdiplus::Pen bpen(Gdiplus::Color(hov ? 200 : 90, 80, 130, 230), 1.2f);
                g.DrawPath(&bpen, &bp);

                SetTextColor(dc, hov ? RGB(200, 220, 255) : RGB(160, 180, 220));
                RECT btr = ss->btnRects[i];
                DrawTextA(dc, btnText[i], -1, &btr,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                bx += bw + 10;
            }
        }

        // ── File list panel ──────────────────────────────────────
        {
            const int listTop = 202;
            ss->listRect = {margin, listTop, W - margin, H - margin};
            const int lw = ss->listRect.right - ss->listRect.left;
            const int lh = ss->listRect.bottom - ss->listRect.top;

            Gdiplus::GraphicsPath panel;
            addRoundRect(panel, margin, listTop, lw, lh, 12);
            Gdiplus::SolidBrush pf(Gdiplus::Color(255, 13, 18, 34));
            g.FillPath(&pf, &panel);
            Gdiplus::Pen pp(Gdiplus::Color(70, 70, 95, 160), 1.0f);
            g.DrawPath(&pp, &panel);

            const int rowH = ss->rowH;
            const int visibleRows = (lh - 16) / rowH;
            const int total = static_cast<int>(ss->entries.size());
            if (ss->scrollOffset > total - visibleRows)
                ss->scrollOffset = std::max(0, total - visibleRows);

            SelectObject(dc, rowFont);
            if (total == 0) {
                SetTextColor(dc, RGB(100, 115, 150));
                RECT er = ss->listRect;
                DrawTextA(dc, "(empty - your projects will appear here)", -1,
                          &er, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            for (int vi = 0; vi < visibleRows; ++vi) {
                const int idx = ss->scrollOffset + vi;
                if (idx >= total) break;
                const auto& e = ss->entries[idx];
                const int ry = listTop + 8 + vi * rowH;

                if (idx == ss->hoverRow) {
                    Gdiplus::GraphicsPath hover;
                    addRoundRect(hover, margin + 6, ry, lw - 12, rowH - 2, 6);
                    Gdiplus::SolidBrush hb(Gdiplus::Color(40, 90, 140, 255));
                    g.FillPath(&hb, &hover);
                }

                // Glyph: folder (amber) or file (blue-gray)
                const int gx = margin + 18, gy = ry + rowH / 2;
                if (e.isDir) {
                    Gdiplus::SolidBrush fb(Gdiplus::Color(255, 235, 180, 60));
                    Gdiplus::GraphicsPath tab;
                    addRoundRect(tab, gx, gy - 8, 9, 5, 2);
                    g.FillPath(&fb, &tab);
                    Gdiplus::GraphicsPath body;
                    addRoundRect(body, gx, gy - 5, 18, 12, 3);
                    g.FillPath(&fb, &body);
                } else {
                    Gdiplus::GraphicsPath doc;
                    addRoundRect(doc, gx + 2, gy - 8, 13, 16, 3);
                    Gdiplus::SolidBrush db(Gdiplus::Color(255, 150, 175, 220));
                    g.FillPath(&db, &doc);
                    Gdiplus::Pen ln(Gdiplus::Color(255, 60, 80, 120), 1.0f);
                    g.DrawLine(&ln, gx + 5, gy - 3, gx + 12, gy - 3);
                    g.DrawLine(&ln, gx + 5, gy,     gx + 12, gy);
                    g.DrawLine(&ln, gx + 5, gy + 3, gx + 12, gy + 3);
                }

                SetTextColor(dc, RGB(205, 218, 245));
                RECT nr = {margin + 48, ry, ss->listRect.right - 110, ry + rowH};
                DrawTextA(dc, e.name.c_str(), -1, &nr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                SetTextColor(dc, RGB(110, 125, 160));
                RECT sr = {ss->listRect.right - 108, ry, ss->listRect.right - 16, ry + rowH};
                DrawTextA(dc, formatBytes(e.size).c_str(), -1, &sr,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }

            // Scroll indicator
            if (total > visibleRows) {
                const float frac  = static_cast<float>(visibleRows) / total;
                const float pos   = static_cast<float>(ss->scrollOffset) / total;
                const int trackH  = lh - 20;
                const int thumbH  = std::max(24, static_cast<int>(trackH * frac));
                const int thumbY  = listTop + 10 + static_cast<int>(trackH * pos);
                Gdiplus::SolidBrush thumb(Gdiplus::Color(90, 120, 150, 220));
                g.FillRectangle(&thumb, W - margin - 6, thumbY, 3, thumbH);
            }
        }

        DeleteObject(titleFont);  DeleteObject(subFont);
        DeleteObject(cardValFont); DeleteObject(cardLblFont);
        DeleteObject(rowFont);    DeleteObject(btnFont);

        BitBlt(screenDC, 0, 0, W, H, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBuf);
        DeleteObject(buf);
        DeleteDC(dc);
        EndPaint(hw, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!ss) return 0;
        const POINT pt = {static_cast<SHORT>(LOWORD(lp)), static_cast<SHORT>(HIWORD(lp))};
        int newBtn = -1;
        for (int i = 0; i < 4; ++i)
            if (PtInRect(&ss->btnRects[i], pt)) { newBtn = i; break; }
        int newRow = -1;
        if (PtInRect(&ss->listRect, pt)) {
            const int rel = pt.y - ss->listRect.top - 8;
            if (rel >= 0) {
                const int idx = ss->scrollOffset + rel / ss->rowH;
                if (idx < static_cast<int>(ss->entries.size())) newRow = idx;
            }
        }
        if (newBtn != ss->hoverButton || newRow != ss->hoverRow) {
            ss->hoverButton = newBtn;
            ss->hoverRow = newRow;
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!ss) return 0;
        if (ss->hoverButton == 0) {
            ShellExecuteA(nullptr, "explore", ss->dataPath.c_str(), nullptr, nullptr, SW_SHOW);
        } else if (ss->hoverButton == 1) {
            ShellExecuteA(nullptr, "explore", (ss->dataPath + "\\projects").c_str(), nullptr, nullptr, SW_SHOW);
        } else if (ss->hoverButton == 2) {
            ShellExecuteA(nullptr, "explore", (ss->dataPath + "\\sessions").c_str(), nullptr, nullptr, SW_SHOW);
        } else if (ss->hoverButton == 3) {
            rescanServerData(ss);
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        if (ss && ss->hoverRow >= 0 &&
            ss->hoverRow < static_cast<int>(ss->entries.size())) {
            const auto& e = ss->entries[ss->hoverRow];
            const std::string full = ss->dataPath + "\\" + e.name;
            ShellExecuteA(nullptr, e.isDir ? "explore" : "open",
                          full.c_str(), nullptr, nullptr, SW_SHOW);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (!ss) return 0;
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        ss->scrollOffset = std::max(0, ss->scrollOffset - delta / WHEEL_DELTA * 3);
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hw, nullptr, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {520, 380};
        return 0;
    }

    case WM_DESTROY:
        if (ss) {
            delete ss;
            SetWindowLongPtrA(hw, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

HICON createScaledIcon(Gdiplus::Image* srcImg, int size)
{
    if (!srcImg) return nullptr;
    Gdiplus::Bitmap targetBmp(size, size, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&targetBmp);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.DrawImage(srcImg, 0, 0, size, size);
    HICON hIcon = nullptr;
    targetBmp.GetHICON(&hIcon);
    return hIcon;
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand)
{
    // Without this, Windows display scaling (125%/150%) bitmap-stretches the
    // whole window and every piece of text renders blurry.
    SetProcessDPIAware();

    // Set CWD to the project root (one level above the exe in build-msys/)
    // so all relative asset paths (wallpaper, icons) resolve correctly
    // regardless of which terminal or shortcut launched the app.
    {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash) {
            *lastSlash = '\0';  // strip exe name → now points at build-msys/
            std::string root = std::string(exePath) + "\\..";
            SetCurrentDirectoryA(root.c_str());
        }
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    desktopWallpaper = loadAsset(L"assets/branding/wallpaper.png");

    myPortIconImg = loadAsset(L"assets/icons/myport.png");

    trashEmptyIconImg = loadAsset(L"assets/icons/bin-empty.png");

    trashFullIconImg = loadAsset(L"assets/icons/bin-full.png");

    serverIconImg = loadAsset(L"assets/icons/server.png");

    folderIconImg = loadAsset(L"assets/icons/folder.png");

    Gdiplus::Image* appIconImg = loadAsset(L"assets/branding/app-icon.png");

    Gdiplus::Image* iconSource = appIconImg ? appIconImg : desktopWallpaper;

    HICON hAppIcon = nullptr;
    HICON hAppIconSm = nullptr;
    if (iconSource) {
        hAppIcon = createScaledIcon(iconSource, 48);
        hAppIconSm = createScaledIcon(iconSource, 16);
    }

    // Keep the icon image alive — drawTaskbar draws it as the search-bar orb
    taskbarOrbImg = appIconImg;

    dockLogoImg = loadAsset(L"assets/branding/dock-logo.png");

    const char* className = "PortDesktopWindow";

    WNDCLASSEXA windowClass = {};
    windowClass.cbSize = sizeof(WNDCLASSEXA);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = CreateSolidBrush(DesktopBackground);
    if (hAppIcon) {
        windowClass.hIcon = hAppIcon;
    }
    if (hAppIconSm) {
        windowClass.hIconSm = hAppIconSm;
    }

    RegisterClassExA(&windowClass);

    WNDCLASSA folderClass = {};
    folderClass.lpfnWndProc = folderWindowProc;
    folderClass.hInstance = instance;
    folderClass.lpszClassName = "PortFolderWindow";
    folderClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    folderClass.hbrBackground = CreateSolidBrush(RGB(15, 20, 35));
    folderClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    RegisterClassA(&folderClass);

    WNDCLASSA trashClass = {};
    trashClass.lpfnWndProc = trashWindowProc;
    trashClass.hInstance = instance;
    trashClass.lpszClassName = "PortTrashWindow";
    trashClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    trashClass.hbrBackground = CreateSolidBrush(RGB(10, 14, 26));
    RegisterClassA(&trashClass);

    WNDCLASSA chatPanelClass = {};
    chatPanelClass.lpfnWndProc   = chatPanelProc;
    chatPanelClass.hInstance     = instance;
    chatPanelClass.lpszClassName = "PortChatPanel";
    chatPanelClass.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    chatPanelClass.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClassA(&chatPanelClass);

    WNDCLASSA serverClass = {};
    serverClass.lpfnWndProc = myServerWindowProc;
    serverClass.hInstance = instance;
    serverClass.lpszClassName = "PortMyServerWindow";
    serverClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    serverClass.hbrBackground = CreateSolidBrush(RGB(9, 13, 26));
    serverClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    RegisterClassA(&serverClass);

    HWND window = CreateWindowExA(
        0,
        className,
        "Port Desktop - Port OS Virtual Environment",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        920,
        560,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!window) {
        return 1;
    }

    ShowWindow(window, showCommand);
    
    // Initialize background cache
    RECT startRect;
    GetClientRect(window, &startRect);
    updateCachedBackground(window, startRect.right - startRect.left, startRect.bottom - startRect.top);

    UpdateWindow(window);

    MSG message = {};
    while (GetMessageA(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    if (hAppIcon) {
        DestroyIcon(hAppIcon);
    }
    if (hAppIconSm) {
        DestroyIcon(hAppIconSm);
    }
    if (myPortIconImg) {
        delete myPortIconImg;
        myPortIconImg = nullptr;
    }
    if (trashEmptyIconImg) {
        delete trashEmptyIconImg;
        trashEmptyIconImg = nullptr;
    }
    if (trashFullIconImg) {
        delete trashFullIconImg;
        trashFullIconImg = nullptr;
    }
    if (serverIconImg) {
        delete serverIconImg;
        serverIconImg = nullptr;
    }
    if (folderIconImg) {
        delete folderIconImg;
        folderIconImg = nullptr;
    }
    if (desktopWallpaper) {
        delete desktopWallpaper;
        desktopWallpaper = nullptr;
    }
    if (taskbarOrbImg) {
        delete taskbarOrbImg;
        taskbarOrbImg = nullptr;
    }
    if (dockLogoImg) {
        delete dockLogoImg;
        dockLogoImg = nullptr;
    }
    if (cachedBackground) {
        DeleteObject(cachedBackground);
        cachedBackground = nullptr;
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);

    return static_cast<int>(message.wParam);
}
#else
#include <iostream>
#include <string>
#include "port/kernel/ai_kernel.hpp"
#include "port/kernel/command_router.hpp"

int main()
{
    std::cout << "==================================================\n";
    std::cout << "  Port OS Virtual environment (Linux App CLI)\n";
    std::cout << "==================================================\n";
    std::cout << "  Folders ready: System, Apps, Users, Documents\n";
    std::cout << "  Ask Port is active.\n\n";

    port::kernel::AIKernel kernel;
    kernel.boot();

    std::string prompt;
    while (true) {
        std::cout << "Ask Port: ";
        if (!std::getline(std::cin, prompt)) {
            break;
        }
        if (prompt.empty()) continue;
        if (prompt == "exit" || prompt == "quit") {
            break;
        }

        port::kernel::CommandRouter router;
        auto command = router.parse(prompt);
        auto response = kernel.handleCommand(command);

        if (!response.message.empty()) {
            if (response.message.rfind("CONFIRM_REQUIRED: ", 0) == 0) {
                std::string pendingCommand = response.message.substr(18);
                std::cout << "Allow command '" << pendingCommand << "'? (yes/no): ";
                std::string confirm;
                if (std::getline(std::cin, confirm) && (confirm == "yes" || confirm == "y")) {
                    auto confirmResponse = kernel.confirmPendingCommand();
                    std::cout << confirmResponse.message << "\n";
                } else {
                    kernel.cancelPendingCommand();
                    std::cout << "Command cancelled.\n";
                }
            } else {
                std::cout << response.message << "\n";
            }
        }
    }

    kernel.shutdown();
    return 0;
}
#endif
