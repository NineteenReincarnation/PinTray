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
#include <cstdint>
#include "i18n.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
constexpr wchar_t kClassName[] = L"NineteenReincarnation.PinTray";
constexpr wchar_t kSettingsPath[] = L"Software\\NineteenReincarnation\\PinTray";
constexpr wchar_t kRunPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kBadgeSync = WM_APP + 3;
constexpr UINT kBadgeMove = WM_APP + 4;
constexpr UINT kTrayId = 1;
constexpr UINT kHotkeyMenuBase = 2000;
constexpr UINT kMenuAutostart = 3000;
constexpr UINT kMenuExit = 3003;
constexpr UINT kHotkeyIdA = 50;
constexpr UINT kHotkeyIdB = 51;

struct WindowEntry { HWND hwnd; DWORD pid; bool topmost; };
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
UINT g_taskbarCreated = 0;
UINT g_hotkeyId = 0;
int g_hotkeyChoice = -1;
bool g_autostart = true;
bool g_packaged = false;
std::vector<PinState> g_ownedPins;
PinTrayLocale::Text g_text = PinTrayLocale::ForLanguage(L"en");
bool g_rtlLanguage = false;
bool IsRightToLeftLanguage() {
    const std::wstring name = PinTrayLocale::PrimaryLanguage();
    return name.starts_with(L"ar") || name.starts_with(L"he") ||
           name.starts_with(L"fa") || name.starts_with(L"ur") ||
           name.starts_with(L"ps");
}

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
    if (out) *out = {hwnd, pid, (style & WS_EX_TOPMOST) != 0};
    return true;
}

bool TogglePin(HWND hwnd) {
    WindowEntry entry{};
    if (!DescribeWindow(hwnd, &entry)) return false;
    const HWND destination = entry.topmost ? HWND_NOTOPMOST : HWND_TOPMOST;
    if (!SetWindowPos(hwnd, destination, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) return false;
    // Drop a stale record before tracking a newly pinned HWND. Windows can
    // reuse a handle after its previous window has been destroyed.
    std::erase_if(g_ownedPins, [hwnd](const PinState& p) { return p.hwnd == hwnd; });
    if (!entry.topmost) g_ownedPins.push_back({hwnd, entry.pid});
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

// Let Windows manage menu activation, outside-click dismissal, and keyboard focus.
// Do not retain a second custom popup window after the notification area closes.
bool UseDarkMenu() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return true;
    return value == 0;
}

void ShowMenu() {
    // TrackPopupMenuEx runs its own modal loop. This guard prevents re-entry.
    static bool showing = false;
    static DWORD lastShownAt = 0;
    const DWORD now = GetTickCount();
    if (showing || (lastShownAt != 0 && now - lastShownAt < 200)) return;
    showing = true;
    lastShownAt = now;

    HMENU menu = CreatePopupMenu();
    HMENU shortcuts = CreatePopupMenu();
    if (!menu || !shortcuts) {
        if (menu) DestroyMenu(menu);
        if (shortcuts) DestroyMenu(shortcuts);
        showing = false;
        return;
    }

    for (size_t i = 0; i < std::size(kHotkeys); ++i) {
        AppendMenuW(shortcuts, MF_STRING | (g_hotkeyChoice == static_cast<int>(i) ? MF_CHECKED : 0),
                    kHotkeyMenuBase + static_cast<UINT>(i), kHotkeys[i].label);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(shortcuts), g_text.shortcuts);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_autostart ? MF_CHECKED : 0) |
                (g_packaged ? MF_GRAYED : 0), kMenuAutostart, g_text.startup);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, g_text.quit);

    POINT point{};
    GetCursorPos(&point);
    MONITORINFO info{sizeof(info)};
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &info);
    const bool rightSide = point.x > (info.rcWork.left + info.rcWork.right) / 2;
    const bool bottomSide = point.y > (info.rcWork.top + info.rcWork.bottom) / 2;

    UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY |
                 (rightSide ? TPM_RIGHTALIGN : TPM_LEFTALIGN) |
                 (bottomSide ? TPM_BOTTOMALIGN : TPM_TOPALIGN);
    if (g_rtlLanguage) flags |= TPM_LAYOUTRTL;

    // Required for reliably dismissing notification-area menus when the user
    // clicks elsewhere; the subsequent WM_NULL prevents rapid reopen/close.
    SetForegroundWindow(g_hwnd);
    const UINT command = TrackPopupMenuEx(menu, flags, point.x, point.y, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu); // Also destroys its attached shortcuts submenu.
    showing = false;

    if (command == kMenuExit) {
        DestroyWindow(g_hwnd);
    } else if (command == kMenuAutostart && !g_packaged) {
        if (SyncStartup(!g_autostart)) {
            g_autostart = !g_autostart;
            WriteSetting(L"AutoStart", g_autostart ? 1 : 0);
        }
    } else if (command >= kHotkeyMenuBase &&
               command < kHotkeyMenuBase + static_cast<UINT>(std::size(kHotkeys))) {
        if (!ChangeHotkey(static_cast<int>(command - kHotkeyMenuBase)))
            MessageBoxW(nullptr, g_text.occupied, L"PinTray", MB_OK | MB_ICONINFORMATION);
    } else if (command == 0) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = g_hwnd;
        data.uID = kTrayId;
        Shell_NotifyIconW(NIM_SETFOCUS, &data);
    }
}

// Lightweight, non-activating pin buttons owned by their target windows.
// An owned popup stays above its window without floating above unrelated windows.
struct BadgeEntry {
    HWND target = nullptr;
    DWORD pid = 0;
    HWND badge = nullptr;
    bool pinned = false;
    bool dark = true;
    bool hover = false;
    bool moveQueued = false;
};
std::vector<BadgeEntry> g_badges;
HWINEVENTHOOK g_badgeHooks[6]{};
bool g_badgeRefreshQueued = false;
constexpr wchar_t kBadgeClass[] = L"PinTray.WindowBadge";
constexpr COLORREF kBadgeTransparency = RGB(255, 0, 255);

void RequestBadgeRefresh() {
    if (!g_hwnd || g_badgeRefreshQueued) return;
    g_badgeRefreshQueued = PostMessageW(g_hwnd, kBadgeSync, 0, 0) != FALSE;
}

bool IsBadgeEligible(HWND target, DWORD* processId = nullptr) {
    if (!DescribeWindow(target) || IsIconic(target)) return false;
    const LONG_PTR style = GetWindowLongPtrW(target, GWL_STYLE);
    // The edge tab does not depend on system caption buttons. Allow custom
    // title bars, but skip child windows and immersive borderless fullscreen.
    if (style & WS_CHILD) return false;
    RECT rect{};
    if (!GetWindowRect(target, &rect)) return false;
    if (rect.right - rect.left < 165 || rect.bottom - rect.top < 105) return false;
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &monitor) &&
        !(style & WS_CAPTION) &&
        rect.left <= monitor.rcMonitor.left + 2 &&
        rect.top <= monitor.rcMonitor.top + 2 &&
        rect.right >= monitor.rcMonitor.right - 2 &&
        rect.bottom >= monitor.rcMonitor.bottom - 2)
        return false;
    if (processId) GetWindowThreadProcessId(target, processId);
    return true;
}

// Tiny edge tab with a circular surface so it stays legible over wallpaper.
// Gray/neutral when unpinned; blue when pinned. Only mouse hover changes its fill.
void DrawBadge(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    if (!dc) return;
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const int w = rect.right - rect.left;
    const int h = rect.bottom - rect.top;
    HBRUSH transparent = CreateSolidBrush(kBadgeTransparency);
    FillRect(dc, &rect, transparent);
    DeleteObject(transparent);

    auto it = std::find_if(g_badges.begin(), g_badges.end(),
        [hwnd](const BadgeEntry& badge) { return badge.badge == hwnd; });
    const bool pinned = it != g_badges.end() && it->pinned;
    const bool dark = it != g_badges.end() ? it->dark : UseDarkMenu();
    const bool hover = it != g_badges.end() && it->hover;

    const COLORREF face = pinned
        ? (hover ? RGB(0, 139, 224) : RGB(0, 116, 192))
        : dark ? (hover ? RGB(85, 85, 85) : RGB(55, 55, 55))
               : (hover ? RGB(225, 225, 225) : RGB(246, 246, 246));
    const COLORREF outline = pinned ? RGB(41, 164, 236)
        : dark ? RGB(123, 123, 123) : RGB(160, 160, 160);
    const COLORREF ink = pinned || dark ? RGB(255, 255, 255) : RGB(50, 50, 50);

    const int margin = std::max(1, std::min(w, h) / 14);
    HBRUSH brush = CreateSolidBrush(face);
    HPEN rim = CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ originalBrush = SelectObject(dc, brush);
    HGDIOBJ originalPen = SelectObject(dc, rim);
    Ellipse(dc, margin, margin, w - margin, h - margin);
    SelectObject(dc, originalBrush);
    SelectObject(dc, originalPen);
    DeleteObject(brush);
    DeleteObject(rim);

    const int scale = std::max(1, std::min(w, h) / 18);
    const int centerX = w / 2;
    const int centerY = h / 2;
    HPEN pen = CreatePen(PS_SOLID, std::max(2, std::min(w, h) / 12), ink);
    originalPen = SelectObject(dc, pen);
    auto line = [&](int x1, int y1, int x2, int y2) {
        MoveToEx(dc, centerX + x1 * scale, centerY + y1 * scale, nullptr);
        LineTo(dc, centerX + x2 * scale, centerY + y2 * scale);
    };
    line(-5, -5, 5, -5);
    line(-3, -5, -3, -1);
    line(3, -5, 3, -1);
    line(-6, -1, 6, -1);
    line(0, -1, 0, 6);
    SelectObject(dc, originalPen);
    DeleteObject(pen);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK BadgeWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
        DrawBadge(hwnd);
        return 0;
    case WM_MOUSEMOVE: {
        auto badge = std::find_if(g_badges.begin(), g_badges.end(),
            [hwnd](const BadgeEntry& p) { return p.badge == hwnd; });
        if (badge != g_badges.end() && !badge->hover) {
            badge->hover = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT event{sizeof(event), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&event);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        auto badge = std::find_if(g_badges.begin(), g_badges.end(),
            [hwnd](const BadgeEntry& p) { return p.badge == hwnd; });
        if (badge != g_badges.end() && badge->hover) {
            badge->hover = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        HWND target = reinterpret_cast<HWND>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (TogglePin(target)) RefreshTooltip();
        RequestBadgeRefresh();
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

// Anchor to the visible right edge, slightly below the title bar.
// Let a small portion protrude when space permits. Near the screen edge,
// pull the chip inside so it never disappears outside the monitor.
bool GetEdgeBadgeRect(HWND target, RECT& placement) {
    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS,
                                    &frame, sizeof(frame))) &&
        !GetWindowRect(target, &frame))
        return false;

    const UINT dpi = std::max(96u, GetDpiForWindow(target));
    const int side = std::max(24, MulDiv(28, dpi, 96));
    const int margin = std::max(2, MulDiv(5, dpi, 96));
    const int upperOffset = MulDiv(58, dpi, 96);
    if (frame.right - frame.left < side * 4 ||
        frame.bottom - frame.top < upperOffset + side + margin)
        return false;

    const POINT edge{frame.right - 1, frame.top + upperOffset + side / 2};
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromPoint(edge, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;

    RECT visible{};
    if (!IntersectRect(&visible, &frame, &monitor.rcWork))
        return false;
    if (visible.right - visible.left < side || visible.bottom - visible.top < side)
        return false;

    int x = frame.right - side / 2; // half outside the right window edge
    if (x + side > monitor.rcWork.right - margin)
        x = std::min(frame.right - side - margin,
                     static_cast<int>(monitor.rcWork.right) - side - margin);
    int y = std::max(frame.top + upperOffset,
                     static_cast<int>(monitor.rcWork.top) + margin);
    if (x < visible.left || y + side > visible.bottom - margin)
        return false;
    placement = {x, y, x + side, y + side};
    return true;
}

void RefreshBadges() {
    g_badgeRefreshQueued = false;
    const size_t initialPinCount = g_ownedPins.size();
    std::erase_if(g_ownedPins, [](const PinState& p) {
        DWORD pid = 0;
        return !IsWindow(p.hwnd) || !GetWindowThreadProcessId(p.hwnd, &pid) ||
               pid != p.pid || !(GetWindowLongPtrW(p.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST);
    });
    if (initialPinCount != g_ownedPins.size()) RefreshTooltip();

    // Persistent edge tabs on eligible top-level windows (never in caption buttons).
    // Enumeration is event-driven; there is no timer or constant polling.
    struct Target { HWND hwnd; DWORD pid; RECT placement; };
    std::vector<Target> targets;
    targets.reserve(24);
    EnumWindows([](HWND target, LPARAM context) -> BOOL {
        auto& targets = *reinterpret_cast<std::vector<Target>*>(context);
        DWORD pid = 0;
        RECT placement{};
        if (targets.size() < 128 && IsBadgeEligible(target, &pid) &&
            GetEdgeBadgeRect(target, placement)) {
            targets.push_back({target, pid, placement});
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&targets));

    std::erase_if(g_badges, [&](const BadgeEntry& badge) {
        const bool keep = IsWindow(badge.badge) &&
            std::any_of(targets.begin(), targets.end(), [&](const Target& t) {
                return t.hwnd == badge.target && t.pid == badge.pid;
            });
        if (!keep && IsWindow(badge.badge)) DestroyWindow(badge.badge);
        return !keep;
    });

    const bool dark = UseDarkMenu();
    for (const Target& entry : targets) {
        const HWND target = entry.hwnd;
        const DWORD pid = entry.pid;
        const RECT place = entry.placement;
        const int w = place.right - place.left;
        const int h = place.bottom - place.top;
        const bool pinned = (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        auto it = std::find_if(g_badges.begin(), g_badges.end(),
            [target, pid](const BadgeEntry& b) { return b.target == target && b.pid == pid; });
        if (it == g_badges.end()) {
            HWND badge = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                kBadgeClass, pinned ? g_text.unpin : g_text.pin, WS_POPUP,
                place.left, place.top, w, h, target, nullptr, g_instance, nullptr);
            if (!badge) continue;
            SetWindowLongPtrW(badge, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(target));
            SetLayeredWindowAttributes(badge, kBadgeTransparency, 0, LWA_COLORKEY);
            g_badges.push_back({target, pid, badge, pinned, dark, false, false});
            SetWindowPos(badge, nullptr, place.left, place.top, w, h,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        } else {
            RECT current{};
            const bool moved = !GetWindowRect(it->badge, &current) ||
                current.left != place.left || current.top != place.top ||
                current.right - current.left != w || current.bottom - current.top != h;
            if (moved) {
                SetWindowPos(it->badge, nullptr, place.left, place.top, w, h,
                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
            }
            if (moved || it->pinned != pinned || it->dark != dark)
                InvalidateRect(it->badge, nullptr, FALSE);
            if (it->pinned != pinned)
                SetWindowTextW(it->badge, pinned ? g_text.unpin : g_text.pin);
            it->pinned = pinned;
            it->dark = dark;
        }
    }
}

// During window drags, avoid a full EnumWindows + badge rebuild for
// every location-change event; just move the existing non-activating button.
void MoveBadgeWithWindow(HWND target) {
    auto badge = std::find_if(g_badges.begin(), g_badges.end(),
        [target](const BadgeEntry& b) { return b.target == target; });
    if (badge == g_badges.end()) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    RECT place{};
    if (pid != badge->pid || !IsBadgeEligible(target) ||
        !GetEdgeBadgeRect(target, place)) {
        RequestBadgeRefresh();
        return;
    }
    RECT current{};
    if (!GetWindowRect(badge->badge, &current) ||
        current.left != place.left || current.top != place.top ||
        current.right - current.left != place.right - place.left ||
        current.bottom - current.top != place.bottom - place.top) {
        SetWindowPos(badge->badge, nullptr, place.left, place.top,
                     place.right - place.left, place.bottom - place.top,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

void CALLBACK OnBadgeWindowEvent(HWINEVENTHOOK, DWORD event, HWND target,
                                  LONG object, LONG child, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND) {
        RequestBadgeRefresh();
        return;
    }
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !target) return;
    if (event == EVENT_OBJECT_DESTROY) {
        // Prune before Win32 can recycle the HWND for another window.
        const auto removed = std::erase_if(g_ownedPins,
            [target](const PinState& p) { return p.hwnd == target; });
        const bool hadBadge = std::any_of(g_badges.begin(), g_badges.end(),
            [target](const BadgeEntry& b) { return b.target == target; });
        if (removed || hadBadge) RequestBadgeRefresh();
        if (removed) RefreshTooltip();
        return;
    }
    // A newly shown/hidden root window can change the set of visible badges.
    if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE ||
        event == EVENT_OBJECT_NAMECHANGE ||
        event == EVENT_SYSTEM_MINIMIZESTART || event == EVENT_SYSTEM_MINIMIZEEND) {
        if (target == GetAncestor(target, GA_ROOT)) RequestBadgeRefresh();
        return;
    }
    // Coalesce drag events for each badge to avoid flooding the message queue.
    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        auto it = std::find_if(g_badges.begin(), g_badges.end(),
            [target](const BadgeEntry& badge) { return badge.target == target; });
        if (it != g_badges.end() && !it->moveQueued) {
            it->moveQueued = PostMessageW(g_hwnd, kBadgeMove,
                                         reinterpret_cast<WPARAM>(target), 0) != FALSE;
        }
    }
}

void InitializeBadges() {
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.hInstance = g_instance;
    cls.lpfnWndProc = BadgeWindowProc;
    cls.hCursor = LoadCursorW(nullptr, IDC_HAND);
    cls.lpszClassName = kBadgeClass;
    if (!RegisterClassExW(&cls)) return;
    constexpr UINT flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    g_badgeHooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                      nullptr, OnBadgeWindowEvent, 0, 0, flags);
    g_badgeHooks[1] = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
                                      nullptr, OnBadgeWindowEvent, 0, 0, flags);
    g_badgeHooks[2] = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE,
                                      nullptr, OnBadgeWindowEvent, 0, 0, flags);
    g_badgeHooks[3] = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                      nullptr, OnBadgeWindowEvent, 0, 0, flags);
    g_badgeHooks[4] = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY,
                                      nullptr, OnBadgeWindowEvent, 0, 0, flags);
    g_badgeHooks[5] = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
                                      nullptr, OnBadgeWindowEvent, 0, 0, flags);
    RequestBadgeRefresh();
}

void ShutdownBadges() {
    for (auto& hook : g_badgeHooks) {
        if (hook) { UnhookWinEvent(hook); hook = nullptr; }
    }
    for (const auto& badge : g_badges) {
        if (IsWindow(badge.badge)) DestroyWindow(badge.badge);
    }
    std::vector<BadgeEntry>().swap(g_badges);
    g_badgeRefreshQueued = false;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_taskbarCreated && msg != 0) { AddTrayIcon(); return 0; }
    switch (msg) {
    case kTrayMessage: {
        const UINT action = LOWORD(lParam);
        // Pin toggles are available only from the window badge or global hotkey.
        if (action == WM_CONTEXTMENU) ShowMenu();
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        g_text = PinTrayLocale::Detect();
        g_rtlLanguage = IsRightToLeftLanguage();
        RequestBadgeRefresh();
        return 0;
    case WM_HOTKEY:
        if (static_cast<UINT>(wParam) == g_hotkeyId) {
            if (TogglePin(GetForegroundWindow())) RefreshTooltip();
            RequestBadgeRefresh();
        }
        return 0;
    case kBadgeSync:
        RefreshBadges();
        return 0;
    case kBadgeMove: {
        HWND target = reinterpret_cast<HWND>(wParam);
        auto it = std::find_if(g_badges.begin(), g_badges.end(),
            [target](const BadgeEntry& badge) { return badge.target == target; });
        if (it != g_badges.end()) {
            it->moveQueued = false;
            MoveBadgeWithWindow(target);
        }
        return 0;
    }
    case WM_DESTROY:
        ShutdownBadges();
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
    g_text = PinTrayLocale::Detect();
    g_rtlLanguage = IsRightToLeftLanguage();
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
    InitializeBadges();
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ReleaseMutex(g_mutex);
    CloseHandle(g_mutex);
    return static_cast<int>(msg.wParam);
}
