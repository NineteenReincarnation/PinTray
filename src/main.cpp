#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <appmodel.h>
#include <string>
#include <vector>
#include <algorithm>
#include <iterator>
#include <utility>
#include <cwchar>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {
constexpr wchar_t kClassName[] = L"NineteenReincarnation.PinTray";
constexpr wchar_t kSettingsPath[] = L"Software\\NineteenReincarnation\\PinTray";
constexpr wchar_t kRunPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kSelectDone = WM_APP + 2;
constexpr UINT kSelectCancel = WM_APP + 3;
constexpr UINT kTrayId = 1;
constexpr UINT kWindowMenuBase = 1000;
constexpr UINT kHotkeyMenuBase = 2000;
constexpr UINT kMenuAutostart = 3000;
constexpr UINT kMenuClear = 3001;
constexpr UINT kMenuSelect = 3002;
constexpr UINT kMenuExit = 3003;
constexpr UINT kHotkeyIdA = 50;
constexpr UINT kHotkeyIdB = 51;
constexpr size_t kMaxMenuWindows = 22;

struct WindowEntry { HWND hwnd; DWORD pid; std::wstring title; bool topmost; };
struct PinState { HWND hwnd; DWORD pid; };
struct HotkeyChoice { UINT mods; UINT key; const wchar_t* label; };
constexpr HotkeyChoice kHotkeys[] = {
    { MOD_CONTROL | MOD_ALT, 'P', L"Ctrl + Alt + P" },
    { MOD_CONTROL | MOD_ALT, 'T', L"Ctrl + Alt + T" },
    { MOD_CONTROL | MOD_SHIFT, VK_SPACE, L"Ctrl + Shift + Space" },
    { MOD_CONTROL | MOD_WIN, 'T', L"Win + Ctrl + T" }
};

HINSTANCE g_instance = nullptr;
HWND g_hwnd = nullptr;
HICON g_icon = nullptr;
HANDLE g_mutex = nullptr;
HHOOK g_mouseHook = nullptr;
HHOOK g_keyHook = nullptr;
UINT g_taskbarCreated = 0;
UINT g_hotkeyId = 0;
int g_hotkeyChoice = 0;
bool g_autostart = true;
bool g_selecting = false;
bool g_selectionPending = false;
bool g_packaged = false;
std::vector<PinState> g_ownedPins;

bool IsPackaged() {
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
}

DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsPath, name, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS) return fallback;
    return value;
}

void WriteSetting(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
}

std::wstring ExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    while (path.size() < 32768) {
        DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (len == 0) return {};
        if (len < path.size() - 1) { path.resize(len); return path; }
        path.resize(path.size() * 2);
    }
    return {};
}

bool SyncStartup(bool enabled) {
    if (g_packaged) return false; // MSIX will have its own startup adapter.
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    LONG status;
    if (enabled) {
        const auto path = ExecutablePath();
        if (path.empty()) { RegCloseKey(key); return false; }
        const std::wstring command = L"\"" + path + L"\" --background";
        status = RegSetValueExW(key, L"PinTray", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(command.c_str()),
                   static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, L"PinTray");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool DescribeWindow(HWND hwnd, WindowEntry* out = nullptr) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || hwnd == g_hwnd) return false;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((style & WS_EX_TOOLWINDOW) && !(style & WS_EX_APPWINDOW)) return false;
    if (GetWindow(hwnd, GW_OWNER) && !(style & WS_EX_APPWINDOW)) return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) return false;
    if (out) {
        std::wstring title(static_cast<size_t>(length) + 1, L'\0');
        int read = GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()));
        if (read <= 0) return false;
        title.resize(static_cast<size_t>(read));
        *out = {hwnd, pid, std::move(title), (style & WS_EX_TOPMOST) != 0};
    }
    return true;
}

bool TogglePin(HWND hwnd) {
    WindowEntry entry{};
    if (!DescribeWindow(hwnd, &entry)) return false;
    const HWND destination = entry.topmost ? HWND_NOTOPMOST : HWND_TOPMOST;
    if (!SetWindowPos(hwnd, destination, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) return false;
    if (entry.topmost) std::erase_if(g_ownedPins, [hwnd](const PinState& p) { return p.hwnd == hwnd; });
    else g_ownedPins.push_back({hwnd, entry.pid});
    return true;
}

void ClearOwnedPins() {
    for (const auto& p : g_ownedPins) {
        DWORD pid = 0;
        if (IsWindow(p.hwnd) && GetWindowThreadProcessId(p.hwnd, &pid) && pid == p.pid &&
            (GetWindowLongPtrW(p.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            SetWindowPos(p.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }
    std::vector<PinState>().swap(g_ownedPins);
}

void RefreshTooltip() {
    std::erase_if(g_ownedPins, [](const PinState& p) {
        DWORD pid = 0;
        return !IsWindow(p.hwnd) || !GetWindowThreadProcessId(p.hwnd, &pid) || pid != p.pid ||
               !(GetWindowLongPtrW(p.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST);
    });
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data); data.hWnd = g_hwnd; data.uID = kTrayId;
    data.uFlags = NIF_TIP;
    swprintf_s(data.szTip, L"PinTray (%zu pinned)", g_ownedPins.size());
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

bool AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data); data.hWnd = g_hwnd; data.uID = kTrayId;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    data.hIcon = g_icon; data.uCallbackMessage = kTrayMessage;
    wcscpy_s(data.szTip, L"PinTray");
    if (!Shell_NotifyIconW(NIM_ADD, &data)) return false;
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    RefreshTooltip();
    return true;
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data); data.hWnd = g_hwnd; data.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

bool ChangeHotkey(int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kHotkeys))) return false;
    if (g_hotkeyId && g_hotkeyChoice == index) return true;
    const UINT newId = g_hotkeyId == kHotkeyIdA ? kHotkeyIdB : kHotkeyIdA;
    if (!RegisterHotKey(g_hwnd, static_cast<int>(newId),
                        kHotkeys[index].mods | MOD_NOREPEAT, kHotkeys[index].key)) return false;
    if (g_hotkeyId) UnregisterHotKey(g_hwnd, static_cast<int>(g_hotkeyId));
    g_hotkeyId = newId;
    g_hotkeyChoice = index;
    WriteSetting(L"Hotkey", static_cast<DWORD>(index));
    return true;
}

std::vector<WindowEntry> GetWindows() {
    std::vector<WindowEntry> windows;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto* results = reinterpret_cast<std::vector<WindowEntry>*>(param);
        WindowEntry entry{};
        if (DescribeWindow(hwnd, &entry)) results->push_back(std::move(entry));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    std::stable_sort(windows.begin(), windows.end(), [](const auto& a, const auto& b) {
        return a.topmost && !b.topmost;
    });
    return windows;
}

std::wstring MenuTitle(std::wstring title) {
    if (title.size() > 45) title = title.substr(0, 44) + L"…";
    size_t offset = 0;
    while ((offset = title.find(L'&', offset)) != std::wstring::npos) {
        title.insert(offset, 1, L'&'); offset += 2;
    }
    return title;
}

void StopSelecting() {
    g_selecting = false;
    g_selectionPending = false;
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    if (g_keyHook) { UnhookWindowsHookEx(g_keyHook); g_keyHook = nullptr; }
    RefreshTooltip();
}

LRESULT CALLBACK MouseSelectProc(int code, WPARAM message, LPARAM lParam) {
    if (code == HC_ACTION && g_selecting && !g_selectionPending && message == WM_LBUTTONDOWN) {
        const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        HWND target = GetAncestor(WindowFromPoint(event->pt), GA_ROOT);
        g_selectionPending = PostMessageW(g_hwnd, kSelectDone, reinterpret_cast<WPARAM>(target), 0) != FALSE;
    }
    return CallNextHookEx(nullptr, code, message, lParam);
}

LRESULT CALLBACK KeyboardSelectProc(int code, WPARAM message, LPARAM lParam) {
    if (code == HC_ACTION && g_selecting && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)) {
        const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (event->vkCode == VK_ESCAPE) {
            if (!g_selectionPending)
                g_selectionPending = PostMessageW(g_hwnd, kSelectCancel, 0, 0) != FALSE;
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, message, lParam);
}

void StartSelecting() {
    if (g_selecting) { StopSelecting(); return; }
    g_selecting = true;
    g_selectionPending = false;
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseSelectProc, g_instance, 0);
    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardSelectProc, g_instance, 0);
    if (!g_mouseHook || !g_keyHook) {
        StopSelecting();
        MessageBoxW(nullptr, L"无法进入窗口选择模式。", L"PinTray", MB_OK | MB_ICONWARNING);
    }
}

void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    HMENU windowsMenu = CreatePopupMenu();
    HMENU shortcutsMenu = CreatePopupMenu();
    if (!menu || !windowsMenu || !shortcutsMenu) {
        if (menu) DestroyMenu(menu);
        if (windowsMenu) DestroyMenu(windowsMenu);
        if (shortcutsMenu) DestroyMenu(shortcutsMenu);
        return;
    }
    AppendMenuW(menu, MF_STRING, kMenuSelect, L"选择窗口置顶\t单击托盘图标");
    const auto windows = GetWindows();
    std::vector<HWND> menuTargets;
    menuTargets.reserve(std::min(windows.size(), kMaxMenuWindows));
    for (size_t i = 0; i < windows.size() && i < kMaxMenuWindows; ++i) {
        const auto& w = windows[i];
        menuTargets.push_back(w.hwnd);
        AppendMenuW(windowsMenu, MF_STRING | (w.topmost ? MF_CHECKED : 0),
                    kWindowMenuBase + static_cast<UINT>(i), MenuTitle(w.title).c_str());
    }
    if (menuTargets.empty()) AppendMenuW(windowsMenu, MF_STRING | MF_GRAYED, 0, L"没有可用窗口");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(windowsMenu), L"窗口列表");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_ownedPins.empty() ? MF_GRAYED : 0),
                kMenuClear, L"取消 PinTray 设置的全部置顶");
    AppendMenuW(menu, MF_STRING | (g_autostart ? MF_CHECKED : 0) |
                (g_packaged ? MF_GRAYED : 0), kMenuAutostart, L"开机自动启动");
    for (size_t i = 0; i < std::size(kHotkeys); ++i) {
        AppendMenuW(shortcutsMenu, MF_STRING | (static_cast<int>(i) == g_hotkeyChoice ? MF_CHECKED : 0),
                    kHotkeyMenuBase + static_cast<UINT>(i), kHotkeys[i].label);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(shortcutsMenu), L"快捷键");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出 PinTray");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_hwnd);
    const UINT choice = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                         cursor.x, cursor.y, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu); // Destroys child submenus as well.
    if (choice >= kWindowMenuBase && choice < kWindowMenuBase + menuTargets.size()) {
        TogglePin(menuTargets[choice - kWindowMenuBase]);
        RefreshTooltip();
    } else if (choice >= kHotkeyMenuBase && choice < kHotkeyMenuBase + std::size(kHotkeys)) {
        if (!ChangeHotkey(static_cast<int>(choice - kHotkeyMenuBase)))
            MessageBoxW(nullptr, L"该快捷键已被其他程序占用。", L"PinTray", MB_OK | MB_ICONINFORMATION);
    } else if (choice == kMenuClear) {
        ClearOwnedPins(); RefreshTooltip();
    } else if (choice == kMenuAutostart) {
        if (SyncStartup(!g_autostart)) {
            g_autostart = !g_autostart;
            WriteSetting(L"AutoStart", g_autostart ? 1 : 0);
        }
    } else if (choice == kMenuSelect) {
        StartSelecting();
    } else if (choice == kMenuExit) {
        DestroyWindow(g_hwnd);
    }

}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_taskbarCreated && msg != 0) { AddTrayIcon(); return 0; }
    switch (msg) {
    case kTrayMessage: {
        const UINT action = LOWORD(lParam);
        if (action == WM_CONTEXTMENU || action == WM_RBUTTONUP) ShowMenu();
        else if (action == NIN_SELECT || action == NIN_KEYSELECT || action == WM_LBUTTONUP) StartSelecting();
        return 0;
    }
    case WM_HOTKEY:
        if (static_cast<UINT>(wParam) == g_hotkeyId) {
            if (TogglePin(GetForegroundWindow())) RefreshTooltip();
        }
        return 0;
    case kSelectDone:
        StopSelecting();
        if (TogglePin(reinterpret_cast<HWND>(wParam))) RefreshTooltip();
        return 0;
    case kSelectCancel:
        StopSelecting();
        return 0;
    case WM_DESTROY:
        StopSelecting();
        ClearOwnedPins();
        RemoveTrayIcon();
        if (g_hotkeyId) UnregisterHotKey(hwnd, static_cast<int>(g_hotkeyId));
        if (g_icon) { DestroyIcon(g_icon); g_icon = nullptr; }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\NineteenReincarnation.PinTray.Singleton");
    if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_mutex) CloseHandle(g_mutex);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_packaged = IsPackaged();
    g_autostart = ReadSetting(L"AutoStart", 1) != 0;
    if (!g_packaged) SyncStartup(g_autostart);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls); cls.hInstance = instance;
    cls.lpfnWndProc = WindowProc; cls.lpszClassName = kClassName;
    if (!RegisterClassExW(&cls)) { CloseHandle(g_mutex); return 1; }
    g_hwnd = CreateWindowExW(0, kClassName, L"PinTray", WS_OVERLAPPED,
                             0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) { CloseHandle(g_mutex); return 1; }
    g_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!g_icon) g_icon = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    int chosen = static_cast<int>(ReadSetting(L"Hotkey", 0));
    if (!ChangeHotkey(chosen) && !ChangeHotkey(0)) {
        // The tray remains usable even when all shortcuts are taken.
    }
    if (!AddTrayIcon()) { DestroyWindow(g_hwnd); CloseHandle(g_mutex); return 1; }
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ReleaseMutex(g_mutex);
    CloseHandle(g_mutex);
    return static_cast<int>(msg.wParam);
}
