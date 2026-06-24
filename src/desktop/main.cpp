#include "fox/ai_kernel.hpp"
#include "fox/command_router.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#endif

#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
namespace {

Gdiplus::Image* desktopWallpaper = nullptr;
Gdiplus::Image* myPortIconImg = nullptr;
Gdiplus::Image* trashEmptyIconImg = nullptr;
Gdiplus::Image* trashFullIconImg = nullptr;
HBITMAP cachedBackground = nullptr;
void updateCachedBackground(HWND window, int width, int height);

constexpr int IdIconFoxTerminal = 1001;
constexpr int IdIconMyComputer = 1002;
constexpr int IdIconTrashBin = 1003;
constexpr int IdTerminalOutput = 2001;
constexpr int IdTerminalInput = 2002;
constexpr int IdRunCommand = 2003;
constexpr int IdMyPortList = 3001;
constexpr int IdMyPortRefresh = 3002;
constexpr int IdTrashList = 3003;
constexpr int IdTrashEmpty = 3004;
constexpr int IdAIProgressLabel = 3005;

// Folder window context menu IDs
constexpr int IdFolderCtxOpen    = 5001;
constexpr int IdFolderCtxNewFile = 5002;
constexpr int IdFolderCtxNewDir  = 5003;
constexpr int IdFolderCtxRename  = 5004;
constexpr int IdFolderCtxDelete  = 5005;
constexpr int IdFolderCtxRefresh = 5006;

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
        return {90, 100, 64, 100, 110};
    case IconSizeOption::Large:
        return {160, 170, 128, 180, 190};
    case IconSizeOption::Medium:
    default:
        return {122, 132, 96, 140, 150};
    }
}

struct DesktopState {
    fox::AIKernel kernel;
    fox::CommandRouter router;
    HWND output;
    HWND input;
    HWND aiStatusLabel;
    HFONT uiFont;
    HFONT labelFont;
    HFONT askFont;
    std::string terminalBuffer;
    HWND myPortWindow;
    HWND trashWindow;

    // Icon size settings
    IconSizeOption iconSize;

    // Drag selection state
    bool isDragging;
    POINT dragStart;
    POINT dragEnd;

    // Desktop icon positions
    POINT myPortPos;
    POINT trashPos;

    // Icon dragging state
    bool isDraggingIcon;
    int draggedIconId;
    POINT iconDragOffset;
    POINT draggedIconOrigPos;

    // Dynamic desktop icons
    struct DynamicIcon {
        HWND hwnd;
        std::string name;
        bool isDir;
        POINT pos;
        int id;
    };
    std::vector<DynamicIcon> dynamicIcons;
};

void refreshDesktopDynamicIcons(HWND window, DesktopState* state);

inline void snapIconsToGrid(DesktopState* state) {
    auto layoutInfo = getIconLayoutInfo(state->iconSize);
    
    int myPortCol = (state->myPortPos.x - 28 + 70) / 140;
    int myPortRow = (state->myPortPos.y - 34 + 75) / 150;
    
    int trashCol = (state->trashPos.x - 28 + 70) / 140;
    int trashRow = (state->trashPos.y - 34 + 75) / 150;
    
    if (myPortCol < 0) myPortCol = 0;
    if (myPortRow < 0) myPortRow = 0;
    if (trashCol < 0) trashCol = 0;
    if (trashRow < 0) trashRow = 0;
    
    if (myPortCol == trashCol && myPortRow == trashRow) {
        trashRow = myPortRow + 1;
    }
    
    state->myPortPos.x = 28 + myPortCol * layoutInfo.gapX;
    state->myPortPos.y = 34 + myPortRow * layoutInfo.gapY;
    
    state->trashPos.x = 28 + trashCol * layoutInfo.gapX;
    state->trashPos.y = 34 + trashRow * layoutInfo.gapY;
}

struct FolderWindowState {
    fox::AIKernel* kernel;
    std::string currentDir;
};

void refreshMyPortList(HWND listBox, fox::Sandbox* sandbox, const std::string& currentDir)
{
    SendMessageA(listBox, LB_RESETCONTENT, 0, 0);
    
    if (!currentDir.empty()) {
        SendMessageA(listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(".."));
    }

    bool ok = false;
    std::string listing = sandbox->list(currentDir, ok);
    if (!ok || listing.empty()) {
        return;
    }
    std::istringstream stream(listing);
    std::string line;
    bool first = true;
    while (std::getline(stream, line)) {
        if (first) { first = false; continue; } // skip header line
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
        if (!line.empty()) {
            SendMessageA(listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        }
    }
}

void refreshTrashList(HWND listBox, fox::Sandbox* sandbox)
{
    SendMessageA(listBox, LB_RESETCONTENT, 0, 0);
    bool ok = false;
    std::string listing = sandbox->listTrash(ok);
    if (!ok || listing.find("empty") != std::string::npos) {
        SendMessageA(listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>("(Trash Bin is empty)"));
        return;
    }
    std::istringstream stream(listing);
    std::string line;
    bool first = true;
    while (std::getline(stream, line)) {
        if (first) { first = false; continue; }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
        if (!line.empty()) {
            SendMessageA(listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        }
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
    if (!state) {
        return;
    }

    state->terminalBuffer += normalizeNewlines(text);
    state->terminalBuffer += "\r\n";

    if (state->output) {
        SetWindowTextA(state->output, state->terminalBuffer.c_str());
    }
}

void runTerminalCommand(DesktopState* state, const std::string& text)
{
    if (!state || text.empty()) {
        return;
    }

    appendTerminal(state, "fox> " + text);

    // Show "Super-Nova thinking..." in the input area as placeholder
    if (state->aiStatusLabel) {
        SetWindowTextA(state->aiStatusLabel, "Super-Nova 1.0 is thinking...");
        ShowWindow(state->aiStatusLabel, SW_SHOW);
        // Force repaint so user sees it before the (potentially slow) AI call
        HWND parent = GetParent(state->aiStatusLabel);
        if (parent) {
            RECT r;
            GetClientRect(parent, &r);
            r.top = r.bottom - 76;
            InvalidateRect(parent, &r, TRUE);
            UpdateWindow(parent);
        }
    }

    const auto command = state->router.parse(text);
    const auto response = state->kernel.handleCommand(command);

    // Hide status
    if (state->aiStatusLabel) {
        SetWindowTextA(state->aiStatusLabel, "");
        ShowWindow(state->aiStatusLabel, SW_HIDE);
    }

    if (!response.message.empty()) {
        if (response.message.rfind("CONFIRM_REQUIRED: ", 0) == 0) {
            std::string filename = response.message.substr(18);
            std::string promptMsg = "Move '" + filename + "' to Trash Bin?";
            int result = MessageBoxA(nullptr, promptMsg.c_str(), "Confirm Deletion", MB_YESNO | MB_ICONWARNING);
            if (result == IDYES) {
                fox::Command confirmCommand = command;
                confirmCommand.arg2 = "yes";
                const auto confirmResponse = state->kernel.handleCommand(confirmCommand);
                if (!confirmResponse.message.empty()) {
                    appendTerminal(state, confirmResponse.message);
                }
                if (!confirmResponse.ok) {
                    appendTerminal(state, "Command failed.");
                }
            } else {
                appendTerminal(state, "Deletion cancelled.");
            }
        } else {
            appendTerminal(state, response.message);
        }
    }

    if (!response.ok && response.message.rfind("CONFIRM_REQUIRED: ", 0) != 0) {
        appendTerminal(state, "Command failed.");
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

LRESULT CALLBACK iconButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND parent = GetParent(hwnd);
    DesktopState* state = reinterpret_cast<DesktopState*>(GetWindowLongPtrA(parent, GWLP_USERDATA));
    int controlId = GetDlgCtrlID(hwnd);
    switch (msg) {
    case WM_RBUTTONUP: {
        if (state) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            
            if (controlId == IdIconTrashBin) {
                AppendMenuA(hMenu, MF_STRING, 6101, "Empty Trash");
                AppendMenuA(hMenu, MF_STRING, 6102, "Rename");
                AppendMenuA(hMenu, MF_STRING, 6103, "Properties");
            } else if (controlId == IdIconMyComputer) {
                AppendMenuA(hMenu, MF_STRING, 6201, "Open");
                AppendMenuA(hMenu, MF_STRING, 6202, "Properties");
            } else {
                // For dynamic desktop folder icons (implemented later)
                AppendMenuA(hMenu, MF_STRING, 6301, "Open");
                AppendMenuA(hMenu, MF_STRING, 6302, "Delete");
                AppendMenuA(hMenu, MF_STRING, 6303, "Properties");
            }
            
            int cmd = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
            
            if (cmd == 6101) { // Empty Trash
                state->kernel.getSandbox().emptyTrash();
                if (state->trashWindow && IsWindow(state->trashWindow)) {
                    HWND lb = GetDlgItem(state->trashWindow, IdTrashList);
                    if (lb) refreshTrashList(lb, &state->kernel.getSandbox());
                }
                InvalidateRect(hwnd, nullptr, TRUE);
            } else if (cmd == 6102) { // Rename Trash
                MessageBoxA(parent, "The Trash Bin cannot be renamed.", "System Message", MB_OK | MB_ICONINFORMATION);
            } else if (cmd == 6103) { // Trash Properties
                bool ok = false;
                std::string contents = state->kernel.getSandbox().listTrash(ok);
                bool isEmpty = (contents.find("empty") != std::string::npos || contents.find("Empty") != std::string::npos);
                std::string msg = "Trash Bin Properties\n\nType: System Folder\nStatus: " + 
                                  std::string(isEmpty ? "Empty" : "Contains deleted files") + 
                                  "\nLocation: sandbox/.trash/";
                MessageBoxA(parent, msg.c_str(), "Trash Bin Properties", MB_OK | MB_ICONINFORMATION);
            } else if (cmd == 6201) { // Open My Port
                SendMessageA(parent, WM_COMMAND, IdIconMyComputer, 0);
            } else if (cmd == 6202) { // My Port Properties
                MessageBoxA(parent, "My Port Properties\n\nType: Virtual OS Root Folder\nLocation: sandbox/", "My Port Properties", MB_OK | MB_ICONINFORMATION);
            } else if (cmd == 6301) { // Open dynamic desktop folder
                SendMessageA(parent, WM_COMMAND, MAKEWPARAM(controlId, 0), 0);
            } else if (cmd == 6302) { // Delete dynamic desktop folder
                SendMessageA(parent, WM_COMMAND, MAKEWPARAM(controlId, 1), 0);
            } else if (cmd == 6303) { // Dynamic desktop folder properties
                SendMessageA(parent, WM_COMMAND, MAKEWPARAM(controlId, 2), 0);
            }
            return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        if (state) {
            state->isDraggingIcon = true;
            state->draggedIconId = controlId;

            POINT* pPos = nullptr;
            if (controlId == IdIconMyComputer) pPos = &state->myPortPos;
            else if (controlId == IdIconTrashBin) pPos = &state->trashPos;
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

            auto layoutInfo = getIconLayoutInfo(state->iconSize);

            RECT clientRect;
            GetClientRect(parent, &clientRect);
            int maxX = (clientRect.right - clientRect.left) - layoutInfo.width;
            int maxY = (clientRect.bottom - clientRect.top) - 76 - layoutInfo.height;
            if (newX < 0) newX = 0;
            if (newY < 0) newY = 0;
            if (newX > maxX) newX = maxX;
            if (newY > maxY) newY = maxY;

            POINT* pPos = nullptr;
            if (controlId == IdIconMyComputer) pPos = &state->myPortPos;
            else if (controlId == IdIconTrashBin) pPos = &state->trashPos;
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

            RECT oldRect;
            GetWindowRect(hwnd, &oldRect);
            POINT oldTopLeft = {oldRect.left, oldRect.top};
            POINT oldBottomRight = {oldRect.right, oldRect.bottom};
            ScreenToClient(parent, &oldTopLeft);
            ScreenToClient(parent, &oldBottomRight);
            RECT oldParentRect = {oldTopLeft.x, oldTopLeft.y, oldBottomRight.x, oldBottomRight.y};

            MoveWindow(hwnd, newX, newY, layoutInfo.width, layoutInfo.height, TRUE);

            InvalidateRect(parent, &oldParentRect, TRUE);
            RECT newParentRect = {newX, newY, newX + layoutInfo.width, newY + layoutInfo.height};
            InvalidateRect(parent, &newParentRect, TRUE);
            UpdateWindow(parent);
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (state && state->isDraggingIcon && state->draggedIconId == controlId) {
            state->isDraggingIcon = false;
            ReleaseCapture();

            int currentX = 0;
            int currentY = 0;
            POINT* pPos = nullptr;
            if (controlId == IdIconMyComputer) pPos = &state->myPortPos;
            else if (controlId == IdIconTrashBin) pPos = &state->trashPos;
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

            // Grid snapping logic (rows start at 34, cols start at 28)
            int col = (currentX - 28 + layoutInfo.gapX / 2) / layoutInfo.gapX;
            int row = (currentY - 34 + layoutInfo.gapY / 2) / layoutInfo.gapY;
            if (col < 0) col = 0;
            if (row < 0) row = 0;

            int snappedX = 28 + col * layoutInfo.gapX;
            int snappedY = 34 + row * layoutInfo.gapY;

            // Prevent overlapping with other icons
            bool isOverlap = false;

            // Check My Port
            if (controlId != IdIconMyComputer) {
                int otherCol = (state->myPortPos.x - 28 + layoutInfo.gapX / 2) / layoutInfo.gapX;
                int otherRow = (state->myPortPos.y - 34 + layoutInfo.gapY / 2) / layoutInfo.gapY;
                if (col == otherCol && row == otherRow) isOverlap = true;
            }
            // Check Trash Bin
            if (!isOverlap && controlId != IdIconTrashBin) {
                int otherCol = (state->trashPos.x - 28 + layoutInfo.gapX / 2) / layoutInfo.gapX;
                int otherRow = (state->trashPos.y - 34 + layoutInfo.gapY / 2) / layoutInfo.gapY;
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

            if (isOverlap) {
                snappedX = state->draggedIconOrigPos.x;
                snappedY = state->draggedIconOrigPos.y;
            }

            // Apply final coordinates
            if (pPos) {
                *pPos = {snappedX, snappedY};
            }

            MoveWindow(hwnd, snappedX, snappedY, layoutInfo.width, layoutInfo.height, TRUE);

            RECT r;
            GetClientRect(parent, &r);
            r.bottom -= 76;
            InvalidateRect(parent, &r, TRUE);
            UpdateWindow(parent);
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

HWND createDesktopButton(HWND parent, int id, const char* text, int x, int y)
{
    HWND hwnd = CreateWindowExA(
        0,
        "BUTTON",
        text,
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        x,
        y,
        122,
        132,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleA(nullptr),
        nullptr
    );

    WNDPROC originalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(iconButtonProc)));
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(originalProc));

    return hwnd;
}

void refreshDesktopDynamicIcons(HWND window, DesktopState* state)
{
    if (!state) return;

    // 1. Destroy all existing dynamic icon windows
    for (auto& icon : state->dynamicIcons) {
        if (icon.hwnd && IsWindow(icon.hwnd)) {
            DestroyWindow(icon.hwnd);
        }
    }
    state->dynamicIcons.clear();

    // 2. Ensure sandbox/Desktop directory exists
    bool ok = false;
    state->kernel.getSandbox().makeDir("Desktop");

    // 3. List the contents of Desktop
    std::string listing = state->kernel.getSandbox().list("Desktop", ok);
    if (!ok || listing.empty()) {
        return;
    }

    std::istringstream stream(listing);
    std::string line;
    bool first = true;
    int idx = 0;

    auto layoutInfo = getIconLayoutInfo(state->iconSize);

    while (std::getline(stream, line)) {
        if (first) { first = false; continue; } // skip header line
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.erase(line.begin());
        }
        if (line.empty()) continue;

        bool isDir = (line.back() == '/');
        std::string displayName = line;
        if (isDir) {
            displayName.pop_back(); // remove trailing '/'
        }

        // Place dynamic icons on a grid starting at Col 0, Row 2
        // Column 0 has Row 0 (My Port), Row 1 (Trash Bin)
        int gridIndex = idx + 2;
        int col = gridIndex / 3;
        int r = gridIndex % 3;

        int x = 28 + col * layoutInfo.gapX;
        int y = 34 + r * layoutInfo.gapY;

        int id = 1100 + idx;

        // Create the button
        HWND hwnd = createDesktopButton(window, id, displayName.c_str(), x, y);

        // Store it
        DesktopState::DynamicIcon di;
        di.hwnd = hwnd;
        di.name = displayName;
        di.isDir = isDir;
        di.pos = {x, y};
        di.id = id;
        state->dynamicIcons.push_back(di);

        idx++;
    }

    // Force repaint of the desktop background
    RECT r_rect;
    GetClientRect(window, &r_rect);
    r_rect.bottom -= 76; // exclude taskbar
    InvalidateRect(window, &r_rect, TRUE);
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

void layoutDesktop(HWND window, DesktopState* state)
{
    RECT rect;
    GetClientRect(window, &rect);

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    if (IsIconic(window) || width < 200 || height < 200) {
        return;
    }
    const int taskbarHeight = 76;

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

    MoveWindow(computerIcon, state->myPortPos.x, state->myPortPos.y, iconWidth, iconHeight, TRUE);
    MoveWindow(trashIcon, state->trashPos.x, state->trashPos.y, iconWidth, iconHeight, TRUE);

    const int inputLeft = 40;
    const int inputRightPadding = 260;
    
    int askBoxLeft = inputLeft;
    int askBoxRight = width - inputRightPadding;
    int askBoxTop = height - taskbarHeight + 18;
    
    int editX = askBoxLeft + 45;
    int editY = askBoxTop + 9;
    int editWidth = (askBoxRight - 195) - editX;
    int editHeight = 22;

    MoveWindow(state->input, editX, editY, editWidth, editHeight, TRUE);
    MoveWindow(state->output, width + 100, height + 100, 1, 1, TRUE);

    // Position the AI status label above the input bar
    if (state->aiStatusLabel) {
        int statusY = height - taskbarHeight + 2;
        MoveWindow(state->aiStatusLabel, askBoxLeft + 45, statusY, editWidth, 14, TRUE);
    }
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

    if (controlId == IdIconFoxTerminal) {
        SelectObject(dc, CreateSolidBrush(RGB(9, 28, 18)));
        RoundRect(dc, centerX - 36, top, centerX + 36, top + 48, 10, 10);
        HPEN greenPen = CreatePen(PS_SOLID, 3, RGB(71, 227, 133));
        SelectObject(dc, greenPen);
        SelectObject(dc, emptyBrush);
        MoveToEx(dc, centerX - 20, top + 18, nullptr);
        LineTo(dc, centerX - 10, top + 24);
        LineTo(dc, centerX - 20, top + 30);
        MoveToEx(dc, centerX - 4, top + 34, nullptr);
        LineTo(dc, centerX + 14, top + 34);
        DeleteObject(greenPen);
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

void drawDesktopIcon(const DRAWITEMSTRUCT* item)
{
    HDC dc = item->hDC;
    RECT bounds = item->rcItem;

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

    bool isSelected = (item->itemState & ODS_SELECTED) != 0;
    bool isBeingDragged = (state && state->isDraggingIcon && static_cast<unsigned int>(state->draggedIconId) == item->CtlID);

    // Check if selection rectangle overlaps this icon
    bool isInSelectionRect = false;
    if (state && state->isDragging) {
        RECT selectRect;
        selectRect.left   = state->dragStart.x < state->dragEnd.x ? state->dragStart.x : state->dragEnd.x;
        selectRect.top    = state->dragStart.y < state->dragEnd.y ? state->dragStart.y : state->dragEnd.y;
        selectRect.right  = state->dragStart.x < state->dragEnd.x ? state->dragEnd.x   : state->dragStart.x;
        selectRect.bottom = state->dragStart.y < state->dragEnd.y ? state->dragEnd.y   : state->dragStart.y;

        POINT btnOrigin = {0, 0};
        ClientToScreen(item->hwndItem, &btnOrigin);
        ScreenToClient(GetParent(item->hwndItem), &btnOrigin);
        RECT btnRect = {btnOrigin.x, btnOrigin.y,
                        btnOrigin.x + (bounds.right - bounds.left),
                        btnOrigin.y + (bounds.bottom - bounds.top)};

        RECT dummy;
        isInSelectionRect = (IntersectRect(&dummy, &selectRect, &btnRect) != 0);
    }

    HFONT fontToUse = (state && state->labelFont) ? state->labelFont : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, fontToUse));

    int imgSize = 96;
    if (state) {
        auto layoutInfo = getIconLayoutInfo(state->iconSize);
        imgSize = layoutInfo.imageSize;
    }
    float imgW = static_cast<float>(imgSize);
    float imgH = imgW * 92.0f / 96.0f;

    if (item->CtlID == IdIconMyComputer) {
        if (myPortIconImg) {
            const int centerX = (bounds.left + bounds.right) / 2;
            const int top = bounds.top + 8;
            Gdiplus::Graphics graphics(dc);
            graphics.DrawImage(myPortIconImg, static_cast<Gdiplus::REAL>(centerX - imgW / 2.0f), static_cast<Gdiplus::REAL>(top), imgW, imgH);
        } else {
            drawPlaceholderTile(dc, bounds, PortBlue);
            drawSimpleDesktopGlyph(dc, bounds, IdIconMyComputer);
        }
        drawCenteredLabel(dc, bounds, "My Port");
    } else if (item->CtlID == IdIconFoxTerminal) {
        drawPlaceholderTile(dc, bounds, PortGreen);
        drawSimpleDesktopGlyph(dc, bounds, IdIconFoxTerminal);
        drawCenteredLabel(dc, bounds, "Terminal");
    } else if (item->CtlID == IdIconTrashBin) {
        Gdiplus::Image* imgToDraw = nullptr;
        if (state) {
            bool ok = false;
            std::string contents = state->kernel.getSandbox().listTrash(ok);
            bool isEmpty = (contents.find("empty") != std::string::npos || contents.find("Empty") != std::string::npos);
            imgToDraw = isEmpty ? trashEmptyIconImg : trashFullIconImg;
        } else {
            imgToDraw = trashEmptyIconImg;
        }

        if (imgToDraw) {
            const int centerX = (bounds.left + bounds.right) / 2;
            const int top = bounds.top + 8;
            Gdiplus::Graphics graphics(dc);
            graphics.DrawImage(imgToDraw, static_cast<Gdiplus::REAL>(centerX - imgW / 2.0f), static_cast<Gdiplus::REAL>(top), imgW, imgH);
        } else {
            drawPlaceholderTile(dc, bounds, PortRed);
            drawSimpleDesktopGlyph(dc, bounds, IdIconTrashBin);
        }
        drawCenteredLabel(dc, bounds, "Trash Bin");
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
            // Draw a beautiful premium yellow folder icon matching the sidebar folder shape
            drawPlaceholderTile(dc, bounds, RGB(255, 193, 7));
            const int centerX = (bounds.left + bounds.right) / 2;
            const int top = bounds.top + 28;
            HPEN pen = CreatePen(PS_SOLID, 3, RGB(17, 24, 38));
            HBRUSH brush = CreateSolidBrush(RGB(255, 213, 79));
            SelectObject(dc, pen);
            SelectObject(dc, brush);
            POINT pts[6] = {
                {centerX - 24, top + 6},
                {centerX - 24, top + 34},
                {centerX + 24, top + 34},
                {centerX + 24, top + 12},
                {centerX + 6,  top + 12},
                {centerX,      top + 6}
            };
            Polygon(dc, pts, 6);
            DeleteObject(pen);
            DeleteObject(brush);
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
        drawCenteredLabel(dc, bounds, displayName.c_str());
    }

    // Render selection overlay ON TOP of the icon image and label
    if (isSelected || isBeingDragged || isInSelectionRect) {
        Gdiplus::Graphics graphics(dc);
        Gdiplus::GraphicsPath path;
        int r = 16;
        int x = bounds.left + 2;
        int y = bounds.top + 2;
        int w = (bounds.right - bounds.left) - 4;
        int h = (bounds.bottom - bounds.top) - 4;
        path.AddArc(x, y, r, r, 180, 90);
        path.AddArc(x + w - r, y, r, r, 270, 90);
        path.AddArc(x + w - r, y + h - r, r, r, 0, 90);
        path.AddArc(x, y + h - r, r, r, 90, 90);
        path.CloseFigure();

        // 25% transparent black overlay
        Gdiplus::SolidBrush brush(Gdiplus::Color(64, 0, 0, 0));
        graphics.FillPath(&brush, &path);

        // Thin semi-transparent selection border
        Gdiplus::Pen pen(Gdiplus::Color(128, 164, 181, 241), 1.0f);
        graphics.DrawPath(&pen, &path);
    }

    SelectObject(dc, oldFont);
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

void drawDesktopBackground(HDC dc, const RECT& rect)
{
    if (cachedBackground) {
        HDC memDC = CreateCompatibleDC(dc);
        HGDIOBJ oldBitmap = SelectObject(memDC, cachedBackground);
        BitBlt(dc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBitmap);
        DeleteDC(memDC);
    } else {
        HBRUSH bg = CreateSolidBrush(DesktopBackground);
        FillRect(dc, &rect, bg);
        DeleteObject(bg);
    }
}

void drawTaskbar(HWND window, HDC dc, DesktopState* state)
{
    RECT rect;
    GetClientRect(window, &rect);

    const int taskbarHeight = 76;
    RECT taskbar = {rect.left, rect.bottom - taskbarHeight, rect.right, rect.bottom};
    HBRUSH taskbarBrush = CreateSolidBrush(TaskbarBackground);
    FillRect(dc, &taskbar, taskbarBrush);
    DeleteObject(taskbarBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, TaskbarBorder);
    SelectObject(dc, borderPen);
    MoveToEx(dc, taskbar.left, taskbar.top, nullptr);
    LineTo(dc, taskbar.right, taskbar.top);
    DeleteObject(borderPen);

    // Ensure all text on the taskbar is drawn transparently (fixes white background boxes)
    SetBkMode(dc, TRANSPARENT);

    const int inputLeft = 40;
    const int inputRightPadding = 260;
    RECT askBox = {inputLeft, taskbar.top + 18, rect.right - inputRightPadding, taskbar.top + 56};
    fillRoundRect(dc, askBox, 36, RGB(22, 28, 45), RGB(45, 55, 85));

    // Draw the '+' icon on the left inside the pill
    int pillCenterY = (askBox.top + askBox.bottom) / 2;
    int plusX = askBox.left + 24;
    HPEN plusPen = CreatePen(PS_SOLID, 2, RGB(164, 181, 241));
    SelectObject(dc, plusPen);
    MoveToEx(dc, plusX - 6, pillCenterY, nullptr);
    LineTo(dc, plusX + 6, pillCenterY);
    MoveToEx(dc, plusX, pillCenterY - 6, nullptr);
    LineTo(dc, plusX, pillCenterY + 6);
    DeleteObject(plusPen);

    // Draw "Super-Nova 1.0" dropdown on the right side of the pill
    RECT engineRect = {askBox.right - 180, askBox.top, askBox.right - 45, askBox.bottom};
    SetTextColor(dc, RGB(164, 181, 241));
    if (state && state->askFont) {
        SelectObject(dc, state->askFont);
    }
    DrawTextA(dc, "Super-Nova 1.0  v", -1, &engineRect, DT_VCENTER | DT_RIGHT | DT_SINGLELINE);

    // Draw microphone icon on the far right
    int micX = askBox.right - 26;
    HBRUSH micBrush = CreateSolidBrush(RGB(164, 181, 241));
    HPEN micPen = CreatePen(PS_SOLID, 1, RGB(164, 181, 241));
    SelectObject(dc, micBrush);
    SelectObject(dc, micPen);
    RECT micCap = {micX - 3, pillCenterY - 6, micX + 3, pillCenterY + 1};
    RoundRect(dc, micCap.left, micCap.top, micCap.right, micCap.bottom, 4, 4);
    MoveToEx(dc, micX, pillCenterY + 1, nullptr);
    LineTo(dc, micX, pillCenterY + 6);
    MoveToEx(dc, micX - 4, pillCenterY + 6, nullptr);
    LineTo(dc, micX + 4, pillCenterY + 6);
    DeleteObject(micBrush);
    DeleteObject(micPen);

    SYSTEMTIME localTime;
    GetLocalTime(&localTime);
    char timeText[32];
    char dateText[32];
    wsprintfA(timeText, "%02d:%02d", localTime.wHour, localTime.wMinute);
    wsprintfA(dateText, "%02d.%02d.%04d", localTime.wDay, localTime.wMonth, localTime.wYear);

    RECT clockRect = {rect.right - 210, taskbar.top + 12, rect.right - 36, taskbar.bottom - 12};
    SetTextColor(dc, PortText);
    DrawTextA(dc, timeText, -1, &clockRect, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    DrawTextA(dc, dateText, -1, &clockRect, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<DesktopState*>(GetWindowLongPtrA(window, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* createdState = new DesktopState();
        createdState->myPortWindow = nullptr;
        createdState->trashWindow = nullptr;
        createdState->aiStatusLabel = nullptr;
        createdState->iconSize = IconSizeOption::Medium;
        createdState->isDragging = false;
        createdState->dragStart = {0, 0};
        createdState->dragEnd = {0, 0};
        createdState->isDraggingIcon = false;
        createdState->draggedIconId = 0;
        createdState->iconDragOffset = {0, 0};

        // Initialize default icon positions
        createdState->myPortPos = {28, 34};
        createdState->trashPos = {28, 188};

        SetWindowLongPtrA(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createdState));

        createDesktopButton(window, IdIconMyComputer, "My Port", 28, 34);
        createDesktopButton(window, IdIconTrashBin, "Trash\r\nBin", 28, 188);

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
        SendMessageW(createdState->input, 0x1501, FALSE, reinterpret_cast<LPARAM>(L"Ask Port..."));

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
        RECT rect;
        GetClientRect(window, &rect);

        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;

        // Create double-buffer memory DC and bitmap
        HDC memDC = CreateCompatibleDC(dc);
        HBITMAP memBitmap = CreateCompatibleBitmap(dc, width, height);
        HGDIOBJ oldBitmap = SelectObject(memDC, memBitmap);

        // 1. Draw the background onto memDC
        drawDesktopBackground(memDC, rect);

        // 2. Draw the selection rectangle onto memDC if dragging
        if (state && state->isDragging) {
            Gdiplus::Graphics graphics(memDC);
            int x = state->dragStart.x < state->dragEnd.x ? state->dragStart.x : state->dragEnd.x;
            int y = state->dragStart.y < state->dragEnd.y ? state->dragStart.y : state->dragEnd.y;
            int widthSel = state->dragStart.x < state->dragEnd.x ? state->dragEnd.x - state->dragStart.x : state->dragStart.x - state->dragEnd.x;
            int heightSel = state->dragStart.y < state->dragEnd.y ? state->dragEnd.y - state->dragStart.y : state->dragStart.y - state->dragEnd.y;

            if (widthSel > 0 && heightSel > 0) {
                // Semi-transparent silver/grey fill
                Gdiplus::SolidBrush brush(Gdiplus::Color(40, 180, 180, 180));
                graphics.FillRectangle(&brush, x, y, widthSel, heightSel);

                // Semi-transparent silver/grey border
                Gdiplus::Pen pen(Gdiplus::Color(150, 180, 180, 180), 1.0f);
                graphics.DrawRectangle(&pen, x, y, widthSel, heightSel);
            }
        }

        // 3. Draw the taskbar onto memDC
        drawTaskbar(window, memDC, state);

        // 4. BitBlt the fully composed frame to the screen
        BitBlt(dc, rect.left, rect.top, width, height, memDC, 0, 0, SRCCOPY);

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

            // Do not start drag-select if click is in the taskbar area
            RECT clientRect;
            GetClientRect(window, &clientRect);
            if (pt.y >= clientRect.bottom - 76) {
                return 0;
            }

            // Do not start drag-select if a child window (icon button) is under the cursor
            POINT screenPt = pt;
            ClientToScreen(window, &screenPt);
            HWND hitWindow = WindowFromPoint(screenPt);
            if (hitWindow != window) {
                return 0;
            }

            SetFocus(window); // Clear focus from icons when clicking background
            state->isDragging = true;
            state->dragStart.x = pt.x;
            state->dragStart.y = pt.y;
            state->dragEnd = state->dragStart;
            SetCapture(window);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (state && state->isDragging) {
            RECT clientRect;
            GetClientRect(window, &clientRect);
            int maxY = clientRect.bottom - 76;

            int newX = static_cast<short>(LOWORD(lParam));
            int newY = static_cast<short>(HIWORD(lParam));
            if (newY > maxY) newY = maxY;

            state->dragEnd.x = newX;
            state->dragEnd.y = newY;
            
            RECT rect;
            GetClientRect(window, &rect);
            rect.bottom -= 76; // Exclude the taskbar area (76 pixels)
            InvalidateRect(window, &rect, FALSE);

            // Invalidate icon buttons so they repaint with updated icon highlight state without flickering
            HWND computerIcon = GetDlgItem(window, IdIconMyComputer);
            HWND trashIcon = GetDlgItem(window, IdIconTrashBin);
            InvalidateRect(computerIcon, nullptr, FALSE);
            InvalidateRect(trashIcon, nullptr, FALSE);

            // Invalidate all dynamic icons as well
            if (state) {
                for (const auto& di : state->dynamicIcons) {
                    if (di.hwnd && IsWindow(di.hwnd)) {
                        InvalidateRect(di.hwnd, nullptr, FALSE);
                    }
                }
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (state && state->isDragging) {
            state->isDragging = false;
            ReleaseCapture();
            RECT rect;
            GetClientRect(window, &rect);
            rect.bottom -= 76; // Exclude the taskbar area
            InvalidateRect(window, &rect, TRUE);
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
            rect.bottom -= 76; // Exclude taskbar
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
            SetTextColor(hdcEdit, RGB(220, 225, 238)); // PortText
            SetBkColor(hdcEdit, RGB(22, 28, 45)); // Dark blue background of the AskBox
            static HBRUSH hBrush = CreateSolidBrush(RGB(22, 28, 45));
            return reinterpret_cast<INT_PTR>(hBrush);
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

                // Breadcrumb label
                CreateWindowExA(0, "STATIC", "sandbox/",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    4, 6, w - 8, 20, mw,
                    reinterpret_cast<HMENU>(6001),
                    GetModuleHandleA(nullptr), nullptr);

                // Owner-draw listbox for icon file view
                CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", nullptr,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                    0, 30, w, h - 66, mw,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdMyPortList)),
                    GetModuleHandleA(nullptr), nullptr);

                CreateWindowExA(0, "BUTTON", "Refresh",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    w - 110, h - 32, 100, 26, mw,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdMyPortRefresh)),
                    GetModuleHandleA(nullptr), nullptr);

                auto* folderState = new FolderWindowState{ &state->kernel, "" };
                SetWindowLongPtrA(mw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(folderState));
                HWND lb = GetDlgItem(mw, IdMyPortList);
                refreshMyPortList(lb, &state->kernel.getSandbox(), "");
            }
            return 0;
        }
        case IdIconTrashBin: {
            if (state->trashWindow && IsWindow(state->trashWindow)) {
                SetForegroundWindow(state->trashWindow);
                return 0;
            }
            HWND tw = CreateWindowExA(
                0, "PortTrashWindow", "Trash Bin - Deleted Files",
                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                220, 170, 520, 400,
                window, nullptr, GetModuleHandleA(nullptr), nullptr
            );
            state->trashWindow = tw;
            if (tw) {
                HWND lb = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", nullptr,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                    10, 10, 480, 300, tw,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdTrashList)),
                    GetModuleHandleA(nullptr), nullptr);
                CreateWindowExA(0, "BUTTON", "Empty Trash",
                    WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                    10, 322, 120, 30, tw,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdTrashEmpty)),
                    GetModuleHandleA(nullptr), nullptr);
                SetWindowLongPtrA(tw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state->kernel));
                refreshTrashList(lb, &state->kernel.getSandbox());
            }
            return 0;
        }
        case 40001: { // Small Icons
            state->iconSize = IconSizeOption::Small;
            snapIconsToGrid(state);
            layoutDesktop(window, state);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }
        case 40002: { // Medium Icons
            state->iconSize = IconSizeOption::Medium;
            snapIconsToGrid(state);
            layoutDesktop(window, state);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }
        case 40003: { // Large Icons
            state->iconSize = IconSizeOption::Large;
            snapIconsToGrid(state);
            layoutDesktop(window, state);
            InvalidateRect(window, nullptr, TRUE);
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
            state->kernel.getSandbox().makeDir(path);
            
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
            refreshDesktopDynamicIcons(window, state);
            if (state->myPortWindow && IsWindow(state->myPortWindow)) {
                SendMessageA(state->myPortWindow, WM_COMMAND, IdMyPortRefresh, 0);
            }
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }
        case 40006: { // Settings
            MessageBoxA(window, "Port OS Settings\n\nVersion: 1.0\nCodename: Super-Nova\nEngine: AI Kernel Active", "Settings", MB_OK | MB_ICONINFORMATION);
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
                                    CreateWindowExA(0, "STATIC", ("sandbox/" + path).c_str(),
                                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        4, 6, w - 8, 20, mw,
                                        reinterpret_cast<HMENU>(6001),
                                        GetModuleHandleA(nullptr), nullptr);
                                    CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", nullptr,
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                        0, 30, w, h - 66, mw,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdMyPortList)),
                                        GetModuleHandleA(nullptr), nullptr);
                                    CreateWindowExA(0, "BUTTON", "Refresh",
                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        w - 110, h - 32, 100, 26, mw,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdMyPortRefresh)),
                                        GetModuleHandleA(nullptr), nullptr);

                                    auto* folderState = new FolderWindowState{ &state->kernel, path };
                                    SetWindowLongPtrA(mw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(folderState));
                                    HWND lb = GetDlgItem(mw, IdMyPortList);
                                    refreshMyPortList(lb, &state->kernel.getSandbox(), path);
                                }
                            } else {
                                bool ok = false;
                                std::string path = "Desktop/" + di.name;
                                std::string content = state->kernel.getSandbox().read(path, ok);
                                if (ok) {
                                    MessageBoxA(window, content.c_str(), di.name.c_str(), MB_OK | MB_ICONINFORMATION);
                                }
                            }
                        } else if (notification == 1) { // Delete (Move to Trash)
                            std::string path = "Desktop/" + di.name;
                            state->kernel.getSandbox().moveToTrash(path);
                            refreshDesktopDynamicIcons(window, state);
                            // Refresh trash icon live
                            HWND trashBtn = GetDlgItem(window, IdIconTrashBin);
                            if (trashBtn) InvalidateRect(trashBtn, nullptr, TRUE);
                        } else if (notification == 2) { // Properties
                            std::string path = "Desktop/" + di.name;
                            std::string msg = "Name: " + di.name + "\nType: " + 
                                              (di.isDir ? "Folder" : "File") + 
                                              "\nVirtual Location: sandbox/" + path;
                            MessageBoxA(window, msg.c_str(), "Properties", MB_OK | MB_ICONINFORMATION);
                        }
                        break;
                    }
                }
            }
            return 0;
        }
        }
    case WM_DRAWITEM:
        drawDesktopIcon(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_DESTROY:
        if (state) {
            if (state->askFont) DeleteObject(state->askFont);
            if (state->uiFont) DeleteObject(state->uiFont);
            if (state->labelFont) DeleteObject(state->labelFont);
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
    RECT r = dis->rcItem;

    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF bg = selected ? RGB(76, 118, 238) : RGB(22, 28, 48);
    COLORREF fg = selected ? RGB(255, 255, 255) : RGB(220, 225, 238);

    HBRUSH bgBrush = CreateSolidBrush(bg);
    FillRect(dc, &r, bgBrush);
    DeleteObject(bgBrush);

    // Draw icon glyph (folder = yellow, file = white)
    int iconX = r.left + 8;
    int iconY = r.top + (r.bottom - r.top - 20) / 2;
    COLORREF iconColor = isDir ? RGB(255, 200, 60) : RGB(164, 181, 241);
    HPEN iconPen = CreatePen(PS_SOLID, 1, iconColor);
    HBRUSH iconBrush = CreateSolidBrush(iconColor);
    SelectObject(dc, iconPen);
    SelectObject(dc, iconBrush);
    if (isDir) {
        // Folder shape: tab on top-left
        POINT pts[6] = {
            {iconX,      iconY + 5},
            {iconX,      iconY + 18},
            {iconX + 22, iconY + 18},
            {iconX + 22, iconY + 7},
            {iconX + 12, iconY + 7},
            {iconX + 10, iconY + 5}
        };
        Polygon(dc, pts, 6);
    } else {
        // File shape: rectangle with folded corner
        RECT fileRect = {iconX + 2, iconY + 1, iconX + 20, iconY + 19};
        HBRUSH fileBrush = CreateSolidBrush(RGB(220, 225, 238));
        FillRect(dc, &fileRect, fileBrush);
        DeleteObject(fileBrush);
        // Fold corner
        HPEN darkPen = CreatePen(PS_SOLID, 1, RGB(100, 120, 160));
        SelectObject(dc, darkPen);
        MoveToEx(dc, iconX + 14, iconY + 1, nullptr);
        LineTo(dc, iconX + 20, iconY + 7);
        MoveToEx(dc, iconX + 14, iconY + 1, nullptr);
        LineTo(dc, iconX + 14, iconY + 7);
        LineTo(dc, iconX + 20, iconY + 7);
        DeleteObject(darkPen);
    }
    DeleteObject(iconPen);
    DeleteObject(iconBrush);

    // Draw item text
    char buf[MAX_PATH] = {};
    SendMessageA(dis->hwndItem, LB_GETTEXT, dis->itemID, reinterpret_cast<LPARAM>(buf));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    RECT textRect = {r.left + 36, r.top, r.right, r.bottom};
    DrawTextA(dc, buf, -1, &textRect, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// Show a simple input dialog and return the entered string (empty = cancelled)
std::string showInputDialog(HWND parent, const char* title, const char* prompt, const char* defaultVal)
{
    // We'll use a simple MessageBox + clipboard trick approach... but actually
    // let's use DialogBoxParam with a mini dialog built inline
    struct DlgData {
        const char* prompt;
        const char* defaultVal;
        std::string result;
        bool ok;
    };
    DlgData data{ prompt, defaultVal, "", false };

    // Build a dialog template in memory
    struct alignas(WORD) DlgTemplate {
        DLGTEMPLATE tmpl;
        WORD menu, windowClass, title[32];
        WORD pointSize;
        wchar_t fontName[16];
    };

    // Use simple approach: CreateWindowEx modal dialog with edit + OK/Cancel
    // We implement this using a standard dialog via the Windows API
    // For cross-platform builds, we keep it simple: use a single MessageBoxA
    // for rename with GetWindowText (native input dialog is complex without
    // a dialog resource). Instead, use a simple edit-window approach.

    // Actually implement a proper modal window
    struct ModalCtx {
        const char* prompt;
        const char* defaultVal;
        std::string* result;
        bool* ok;
    };
    std::string result;
    bool okClicked = false;
    ModalCtx ctx{ prompt, defaultVal, &result, &okClicked };

    // Use a popup window as input dialog
    static ModalCtx* gCtx = nullptr;
    gCtx = &ctx;

    struct Dlg {
        static LRESULT CALLBACK proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
            switch (msg) {
            case WM_CREATE: {
                ModalCtx* c = reinterpret_cast<ModalCtx*>(reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);
                SetWindowLongPtrA(hw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(c));
                CreateWindowExA(0, "STATIC", c->prompt,
                    WS_CHILD | WS_VISIBLE, 12, 12, 260, 20, hw, nullptr, GetModuleHandleA(nullptr), nullptr);
                HWND edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", c->defaultVal,
                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 12, 38, 260, 24, hw,
                    reinterpret_cast<HMENU>(100), GetModuleHandleA(nullptr), nullptr);
                SendMessageA(edit, EM_SETSEL, 0, -1);
                SetFocus(edit);
                CreateWindowExA(0, "BUTTON", "OK",
                    WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 100, 74, 80, 26, hw,
                    reinterpret_cast<HMENU>(IDOK), GetModuleHandleA(nullptr), nullptr);
                CreateWindowExA(0, "BUTTON", "Cancel",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 192, 74, 80, 26, hw,
                    reinterpret_cast<HMENU>(IDCANCEL), GetModuleHandleA(nullptr), nullptr);
                return 0;
            }
            case WM_COMMAND: {
                ModalCtx* c = reinterpret_cast<ModalCtx*>(GetWindowLongPtrA(hw, GWLP_USERDATA));
                if (LOWORD(wp) == IDOK) {
                    HWND edit = GetDlgItem(hw, 100);
                    int len = GetWindowTextLengthA(edit);
                    if (len > 0) {
                        c->result->resize(static_cast<std::size_t>(len));
                        GetWindowTextA(edit, &(*c->result)[0], len + 1);
                    }
                    *c->ok = true;
                    DestroyWindow(hw);
                } else if (LOWORD(wp) == IDCANCEL) {
                    DestroyWindow(hw);
                }
                return 0;
            }
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) { DestroyWindow(hw); }
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcA(hw, msg, wp, lp);
        }
    };

    WNDCLASSA dlgClass = {};
    dlgClass.lpfnWndProc = Dlg::proc;
    dlgClass.hInstance = GetModuleHandleA(nullptr);
    dlgClass.lpszClassName = "PortInputDialog";
    dlgClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    dlgClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&dlgClass);

    HWND dlgWnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        "PortInputDialog", title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        0, 0, 296, 140,
        parent, nullptr, GetModuleHandleA(nullptr), &ctx
    );

    // Center over parent
    if (dlgWnd && parent) {
        RECT pr, dr;
        GetWindowRect(parent, &pr);
        GetWindowRect(dlgWnd, &dr);
        int x = pr.left + (pr.right - pr.left - (dr.right - dr.left)) / 2;
        int y = pr.top  + (pr.bottom - pr.top - (dr.bottom - dr.top)) / 2;
        SetWindowPos(dlgWnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    ShowWindow(dlgWnd, SW_SHOW);
    UpdateWindow(dlgWnd);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    UnregisterClassA("PortInputDialog", GetModuleHandleA(nullptr));
    return okClicked ? result : "";
}

LRESULT CALLBACK folderWindowProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* folderState = reinterpret_cast<FolderWindowState*>(GetWindowLongPtrA(hw, GWLP_USERDATA));

    switch (msg) {
    case WM_SIZE: {
        HWND lb = GetDlgItem(hw, IdMyPortList);
        HWND btn = GetDlgItem(hw, IdMyPortRefresh);
        RECT r;
        GetClientRect(hw, &r);
        int w = r.right - r.left;
        int h = r.bottom - r.top;
        if (lb)  MoveWindow(lb, 0, 30, w, h - 66, TRUE);
        if (btn) MoveWindow(btn, w - 110, h - 32, 100, 26, TRUE);
        // breadcrumb label
        HWND crumb = GetDlgItem(hw, 6001);
        if (crumb) MoveWindow(crumb, 4, 6, w - 8, 20, TRUE);
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
                    // Update breadcrumb
                    HWND crumb = GetDlgItem(hw, 6001);
                    if (crumb) {
                        std::string crumbText = "sandbox/" + folderState->currentDir;
                        SetWindowTextA(crumb, crumbText.c_str());
                    }
                }
            } else if (!selected.empty() && selected.back() == '/') {
                selected.pop_back();
                if (folderState) {
                    folderState->currentDir = folderState->currentDir.empty() ? selected : folderState->currentDir + "/" + selected;
                    refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
                    HWND crumb = GetDlgItem(hw, 6001);
                    if (crumb) {
                        std::string crumbText = "sandbox/" + folderState->currentDir;
                        SetWindowTextA(crumb, crumbText.c_str());
                    }
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
                folderState->kernel->getSandbox().write(path, "");
                HWND lb = GetDlgItem(hw, IdMyPortList);
                refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
            }
        } else if (controlId == IdFolderCtxNewDir) {
            if (!folderState) break;
            std::string name = showInputDialog(hw, "New Folder", "Folder name:", "New Folder");
            if (!name.empty()) {
                std::string path = folderState->currentDir.empty() ? name : folderState->currentDir + "/" + name;
                folderState->kernel->getSandbox().makeDir(path);
                HWND lb = GetDlgItem(hw, IdMyPortList);
                refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
            }
        } else if (controlId == IdFolderCtxRename) {
            if (!folderState) break;
            HWND lb = GetDlgItem(hw, IdMyPortList);
            int idx = SendMessageA(lb, LB_GETCURSEL, 0, 0);
            if (idx == LB_ERR) break;
            char buf[MAX_PATH];
            SendMessageA(lb, LB_GETTEXT, idx, reinterpret_cast<LPARAM>(buf));
            std::string oldName(buf);
            if (oldName == "..") break;
            if (!oldName.empty() && oldName.back() == '/') oldName.pop_back();
            std::string newName = showInputDialog(hw, "Rename", "New name:", oldName.c_str());
            if (!newName.empty() && newName != oldName) {
                std::string oldPath = folderState->currentDir.empty() ? oldName : folderState->currentDir + "/" + oldName;
                std::string newPath = folderState->currentDir.empty() ? newName : folderState->currentDir + "/" + newName;
                folderState->kernel->getSandbox().renameEntry(oldPath, newPath);
                refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
            }
        } else if (controlId == IdFolderCtxDelete) {
            if (!folderState) break;
            HWND lb = GetDlgItem(hw, IdMyPortList);
            int idx = SendMessageA(lb, LB_GETCURSEL, 0, 0);
            if (idx == LB_ERR) break;
            char buf[MAX_PATH];
            SendMessageA(lb, LB_GETTEXT, idx, reinterpret_cast<LPARAM>(buf));
            std::string name(buf);
            if (name == "..") break;
            if (!name.empty() && name.back() == '/') name.pop_back();
            std::string path = folderState->currentDir.empty() ? name : folderState->currentDir + "/" + name;
            std::string confirmMsg = "Move '" + name + "' to Trash?";
            if (MessageBoxA(hw, confirmMsg.c_str(), "Delete", MB_YESNO | MB_ICONWARNING) == IDYES) {
                folderState->kernel->getSandbox().moveToTrash(path);
                refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
                
                // Refresh desktop trash icon live
                HWND desktopWnd = GetParent(hw);
                HWND trashBtn = GetDlgItem(desktopWnd, IdIconTrashBin);
                if (trashBtn) InvalidateRect(trashBtn, nullptr, TRUE);
                
                // Refresh dynamic desktop icons and trash window
                DesktopState* desktopState = reinterpret_cast<DesktopState*>(GetWindowLongPtrA(desktopWnd, GWLP_USERDATA));
                if (desktopState) {
                    refreshDesktopDynamicIcons(desktopWnd, desktopState);
                    if (desktopState->trashWindow && IsWindow(desktopState->trashWindow)) {
                        HWND trashLb = GetDlgItem(desktopState->trashWindow, IdTrashList);
                        if (trashLb) refreshTrashList(trashLb, &folderState->kernel->getSandbox());
                    }
                }
            }
        } else if (controlId == IdFolderCtxRefresh) {
            if (folderState) {
                HWND lb = GetDlgItem(hw, IdMyPortList);
                refreshMyPortList(lb, &folderState->kernel->getSandbox(), folderState->currentDir);
            }
        }
        break;
    }
    case WM_CONTEXTMENU: {
        // Right-click in the folder window
        if (!folderState) break;
        POINT pt;
        GetCursorPos(&pt);
        HMENU hMenu = CreatePopupMenu();
        AppendMenuA(hMenu, MF_STRING, IdFolderCtxNewFile,  "New File");
        AppendMenuA(hMenu, MF_STRING, IdFolderCtxNewDir,   "New Folder");
        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        HWND lb = GetDlgItem(hw, IdMyPortList);
        int idx = SendMessageA(lb, LB_GETCURSEL, 0, 0);
        if (idx != LB_ERR) {
            AppendMenuA(hMenu, MF_STRING, IdFolderCtxRename, "Rename");
            AppendMenuA(hMenu, MF_STRING, IdFolderCtxDelete, "Move to Trash");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuA(hMenu, MF_STRING, IdFolderCtxRefresh, "Refresh");
        TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hw, nullptr);
        DestroyMenu(hMenu);
        break;
    }
    case WM_MEASUREITEM: {
        // Set item height for owner-draw listbox
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (mis->CtlID == static_cast<UINT>(IdMyPortList)) {
            mis->itemHeight = 32;
        }
        return TRUE;
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
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
        static HBRUSH lbBrush = CreateSolidBrush(RGB(22, 28, 48));
        return reinterpret_cast<INT_PTR>(lbBrush);
    }
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT r;
        GetClientRect(hw, &r);
        HBRUSH bg = CreateSolidBrush(RGB(15, 20, 35));
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        return 1;
    }
    case WM_DESTROY:
        if (folderState) {
            delete folderState;
            SetWindowLongPtrA(hw, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

LRESULT CALLBACK trashWindowProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND && LOWORD(wp) == IdTrashEmpty) {
        fox::AIKernel* kernel = reinterpret_cast<fox::AIKernel*>(GetWindowLongPtrA(hw, GWLP_USERDATA));
        if (kernel) {
            kernel->getSandbox().emptyTrash();
            HWND lb = GetDlgItem(hw, IdTrashList);
            refreshTrashList(lb, &kernel->getSandbox());
        }
    }
    if (msg == WM_DESTROY) SetWindowLongPtrA(hw, GWLP_USERDATA, 0);
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
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    desktopWallpaper = Gdiplus::Image::FromFile(L"Port Logo 2.png");
    if (desktopWallpaper && desktopWallpaper->GetLastStatus() != Gdiplus::Ok) {
        delete desktopWallpaper;
        desktopWallpaper = Gdiplus::Image::FromFile(L"../Port Logo 2.png");
        if (desktopWallpaper && desktopWallpaper->GetLastStatus() != Gdiplus::Ok) {
            delete desktopWallpaper;
            desktopWallpaper = nullptr;
        }
    }

    myPortIconImg = Gdiplus::Image::FromFile(L"myport-icon.png");
    if (myPortIconImg && myPortIconImg->GetLastStatus() != Gdiplus::Ok) {
        delete myPortIconImg;
        myPortIconImg = Gdiplus::Image::FromFile(L"../myport-icon.png");
        if (myPortIconImg && myPortIconImg->GetLastStatus() != Gdiplus::Ok) {
            delete myPortIconImg;
            myPortIconImg = nullptr;
        }
    }

    trashEmptyIconImg = Gdiplus::Image::FromFile(L"bin-empty.png");
    if (trashEmptyIconImg && trashEmptyIconImg->GetLastStatus() != Gdiplus::Ok) {
        delete trashEmptyIconImg;
        trashEmptyIconImg = Gdiplus::Image::FromFile(L"../bin-empty.png");
        if (trashEmptyIconImg && trashEmptyIconImg->GetLastStatus() != Gdiplus::Ok) {
            delete trashEmptyIconImg;
            trashEmptyIconImg = nullptr;
        }
    }

    trashFullIconImg = Gdiplus::Image::FromFile(L"bin-full.png");
    if (trashFullIconImg && trashFullIconImg->GetLastStatus() != Gdiplus::Ok) {
        delete trashFullIconImg;
        trashFullIconImg = Gdiplus::Image::FromFile(L"../bin-full.png");
        if (trashFullIconImg && trashFullIconImg->GetLastStatus() != Gdiplus::Ok) {
            delete trashFullIconImg;
            trashFullIconImg = nullptr;
        }
    }

    Gdiplus::Image* appIconImg = Gdiplus::Image::FromFile(L"icon.png");
    if (appIconImg && appIconImg->GetLastStatus() != Gdiplus::Ok) {
        delete appIconImg;
        appIconImg = Gdiplus::Image::FromFile(L"../icon.png");
        if (appIconImg && appIconImg->GetLastStatus() != Gdiplus::Ok) {
            delete appIconImg;
            appIconImg = nullptr;
        }
    }

    Gdiplus::Image* iconSource = appIconImg ? appIconImg : desktopWallpaper;

    HICON hAppIcon = nullptr;
    HICON hAppIconSm = nullptr;
    if (iconSource) {
        hAppIcon = createScaledIcon(iconSource, 48);
        hAppIconSm = createScaledIcon(iconSource, 16);
    }

    if (appIconImg) {
        delete appIconImg;
    }

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
    folderClass.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassA(&folderClass);

    WNDCLASSA trashClass = {};
    trashClass.lpfnWndProc = trashWindowProc;
    trashClass.hInstance = instance;
    trashClass.lpszClassName = "PortTrashWindow";
    trashClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    trashClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassA(&trashClass);

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
    if (desktopWallpaper) {
        delete desktopWallpaper;
        desktopWallpaper = nullptr;
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
#include "fox/ai_kernel.hpp"
#include "fox/command_router.hpp"

int main()
{
    std::cout << "==================================================\n";
    std::cout << "  Port OS Virtual environment (Linux App CLI)\n";
    std::cout << "==================================================\n";
    std::cout << "  Folders ready: System, Apps, Users, Documents\n";
    std::cout << "  Ask Port is active.\n\n";

    fox::AIKernel kernel;
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

        fox::CommandRouter router;
        auto command = router.parse(prompt);
        auto response = kernel.handleCommand(command);

        if (!response.message.empty()) {
            if (response.message.rfind("CONFIRM_REQUIRED: ", 0) == 0) {
                std::string filename = response.message.substr(18);
                std::cout << "Are you sure you want to delete '" << filename << "'? (yes/no): ";
                std::string confirm;
                if (std::getline(std::cin, confirm) && (confirm == "yes" || confirm == "y")) {
                    fox::Command confirmCommand = command;
                    confirmCommand.arg2 = "yes";
                    auto confirmResponse = kernel.handleCommand(confirmCommand);
                    std::cout << confirmResponse.message << "\n";
                } else {
                    std::cout << "Deletion cancelled.\n";
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
