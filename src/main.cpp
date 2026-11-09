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
#include <windowsx.h>

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
int g_hotkeyChoice = -1;
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

// The popup is allocated only while visible. Regular tray operation needs no UI framework.
enum class MenuPage { Main, Windows, Shortcuts };
enum MenuGlyph { GlyphNone, GlyphPin, GlyphWindow, GlyphKeyboard, GlyphStartup, GlyphClear, GlyphExit, GlyphBack };
constexpr UINT kMenuWindows = 3004;
constexpr UINT kMenuShortcuts = 3005;
constexpr UINT kMenuBack = 3006;
constexpr int kMaxVisibleWindows = 9;

struct PopupRow {
    std::wstring label;
    UINT action = 0;
    HWND target = nullptr;
    MenuGlyph glyph = GlyphNone;
    bool checked = false;
    bool next = false;
    bool disabled = false;
    bool separator = false;
};
struct DisplayRow { size_t index; RECT bounds; };
struct PopupState {
    HWND hwnd = nullptr;
    MenuPage page = MenuPage::Main;
    POINT anchor{};
    UINT dpi = 96;
    int width = 0, height = 0, hover = -1, scroll = 0;
    bool dark = true;
    HFONT font = nullptr;
    std::vector<PopupRow> rows;
    std::vector<DisplayRow> visible;
};
PopupState* g_popup = nullptr;

int Scale(const PopupState& p, int value) {
    return MulDiv(value, static_cast<int>(p.dpi), 96);
}

bool UseDarkMenu() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return true;
    return value == 0;
}

void AddMenuRow(PopupState& p, const wchar_t* name, UINT command, MenuGlyph glyph,
                bool checked = false, bool next = false, bool disabled = false, HWND target = nullptr) {
    p.rows.push_back({name, command, target, glyph, checked, next, disabled, false});
}
void MenuSeparator(PopupState& p) {
    p.rows.push_back({L"", 0, nullptr, GlyphNone, false, false, true, true});
}

void BuildPopupRows(PopupState& p) {
    p.rows.clear();
    p.hover = -1;
    if (p.page == MenuPage::Main) {
        AddMenuRow(p, L"选择窗口置顶", kMenuSelect, GlyphPin);
        AddMenuRow(p, L"窗口列表", kMenuWindows, GlyphWindow, false, true);
        AddMenuRow(p, L"快捷键", kMenuShortcuts, GlyphKeyboard, false, true);
        MenuSeparator(p);
        AddMenuRow(p, L"开机自启", kMenuAutostart, GlyphStartup, g_autostart, false, g_packaged);
        if (!g_ownedPins.empty())
            AddMenuRow(p, L"取消全部置顶", kMenuClear, GlyphClear);
        MenuSeparator(p);
        AddMenuRow(p, L"退出", kMenuExit, GlyphExit);
    } else if (p.page == MenuPage::Windows) {
        AddMenuRow(p, L"窗口列表", kMenuBack, GlyphBack);
        const auto windows = GetWindows();
        if (windows.empty()) AddMenuRow(p, L"没有可操作的窗口", 0, GlyphNone, false, false, true);
        for (const auto& window : windows) {
            p.rows.push_back({MenuTitle(window.title), 0, window.hwnd, GlyphWindow,
                              window.topmost, false, false, false});
        }
    } else {
        AddMenuRow(p, L"快捷键", kMenuBack, GlyphBack);
        for (size_t i = 0; i < std::size(kHotkeys); ++i) {
            AddMenuRow(p, kHotkeys[i].label, kHotkeyMenuBase + static_cast<UINT>(i),
                       GlyphKeyboard, g_hotkeyChoice == static_cast<int>(i));
        }
    }
}

void LayoutPopup(PopupState& p) {
    p.visible.clear();
    p.width = Scale(p, 304);
    const int padding = Scale(p, 7);
    const int rowHeight = Scale(p, 43);
    const int separatorHeight = Scale(p, 9);
    int y = padding;
    auto append = [&](size_t index) {
        int height = p.rows[index].separator ? separatorHeight : rowHeight;
        p.visible.push_back({index, {padding, y, p.width - padding, y + height}});
        y += height;
    };
    if (p.page == MenuPage::Windows) {
        append(0);
        const int count = static_cast<int>(p.rows.size()) - 1;
        p.scroll = std::clamp(p.scroll, 0, std::max(0, count - kMaxVisibleWindows));
        const int last = std::min(count, p.scroll + kMaxVisibleWindows);
        for (int i = p.scroll; i < last; ++i) append(static_cast<size_t>(i + 1));
    } else {
        p.scroll = 0;
        for (size_t i = 0; i < p.rows.size(); ++i) append(i);
    }
    p.height = y + padding;
}

void PlacePopup(PopupState& p) {
    HMONITOR monitor = MonitorFromPoint(p.anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const RECT work = info.rcWork;
    const int offset = Scale(p, 10);
    int x = std::clamp(p.anchor.x - Scale(p, 48), work.left,
                       std::max(work.left, work.right - p.width));
    int y = (p.anchor.y + p.height + offset <= work.bottom)
        ? p.anchor.y + offset : p.anchor.y - p.height - offset;
    y = std::clamp(y, work.top, std::max(work.top, work.bottom - p.height));
    SetWindowPos(p.hwnd, HWND_TOPMOST, x, y, p.width, p.height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void DrawMenuGlyph(HDC dc, int kind, int x, int y, const PopupState& p, COLORREF color) {
    const int unit = std::max(1, Scale(p, 1));
    HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(p, 2)), color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    auto line = [&](int ax, int ay, int bx, int by) {
        MoveToEx(dc, x + ax * unit, y + ay * unit, nullptr);
        LineTo(dc, x + bx * unit, y + by * unit);
    };
    switch (kind) {
    case GlyphPin:
        line(-6, -7, 6, -7);
        line(-4, -5, 4, -5);
        line(-3, -5, -2, 2);
        line(3, -5, 2, 2);
        line(-5, 2, 5, 2);
        line(0, 2, 0, 10);
        break;
    case GlyphWindow:
        RoundRect(dc, x - 9 * unit, y - 8 * unit, x + 9 * unit, y + 8 * unit,
                  4 * unit, 4 * unit);
        line(-8, -3, 8, -3);
        break;
    case GlyphKeyboard:
        RoundRect(dc, x - 10 * unit, y - 7 * unit, x + 10 * unit, y + 7 * unit,
                  4 * unit, 4 * unit);
        for (int i = -5; i <= 5; i += 5) line(i, -2, i + 1, -2);
        line(-5, 3, 5, 3);
        break;
    case GlyphStartup:
        Ellipse(dc, x - 8 * unit, y - 8 * unit, x + 8 * unit, y + 8 * unit);
        line(0, -4, 0, 4);
        line(0, 4, 4, 1);
        break;
    case GlyphClear:
        RoundRect(dc, x - 9 * unit, y - 5 * unit, x + 4 * unit, y + 7 * unit,
                  3 * unit, 3 * unit);
        RoundRect(dc, x - 3 * unit, y - 9 * unit, x + 10 * unit, y + 3 * unit,
                  3 * unit, 3 * unit);
        break;
    case GlyphExit:
        Arc(dc, x - 9 * unit, y - 9 * unit, x + 9 * unit, y + 9 * unit,
            x + 6 * unit, y - 7 * unit, x - 6 * unit, y - 7 * unit);
        line(0, -10, 0, 1);
        break;
    case GlyphBack:
        line(4, -7, -3, 0);
        line(-3, 0, 4, 7);
        break;
    default:
        break;
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void PaintPopup(PopupState& p) {
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(p.hwnd, &ps);
    if (!target) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, p.width, p.height);
    if (!dc || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        EndPaint(p.hwnd, &ps);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    const COLORREF bg = p.dark ? RGB(39, 39, 39) : RGB(248, 248, 248);
    const COLORREF hoverBg = p.dark ? RGB(64, 64, 64) : RGB(231, 231, 231);
    const COLORREF fore = p.dark ? RGB(244, 244, 244) : RGB(35, 35, 35);
    const COLORREF muted = p.dark ? RGB(153, 153, 153) : RGB(112, 112, 112);
    const COLORREF rule = p.dark ? RGB(77, 77, 77) : RGB(212, 212, 212);
    RECT canvas{0, 0, p.width, p.height};
    HBRUSH background = CreateSolidBrush(bg);
    FillRect(dc, &canvas, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, p.font);
    const int textOffset = Scale(p, 62);
    for (size_t i = 0; i < p.visible.size(); ++i) {
        const auto& viewRow = p.visible[i];
        const PopupRow& row = p.rows[viewRow.index];
        const RECT bounds = viewRow.bounds;
        if (row.separator) {
            HPEN pen = CreatePen(PS_SOLID, 1, rule);
            HGDIOBJ old = SelectObject(dc, pen);
            const int center = (bounds.top + bounds.bottom) / 2;
            MoveToEx(dc, Scale(p, 10), center, nullptr);
            LineTo(dc, p.width - Scale(p, 10), center);
            SelectObject(dc, old);
            DeleteObject(pen);
            continue;
        }
        if (static_cast<int>(i) == p.hover && !row.disabled) {
            HBRUSH hover = CreateSolidBrush(hoverBg);
            HGDIOBJ old = SelectObject(dc, hover);
            HPEN stroke = CreatePen(PS_SOLID, 1, p.dark ? RGB(116, 116, 116) : RGB(192, 192, 192));
            HGDIOBJ previous = SelectObject(dc, stroke);
            RoundRect(dc, bounds.left + Scale(p, 1), bounds.top + Scale(p, 2),
                      bounds.right - Scale(p, 1), bounds.bottom - Scale(p, 2),
                      Scale(p, 8), Scale(p, 8));
            SelectObject(dc, previous);
            SelectObject(dc, old);
            DeleteObject(stroke);
            DeleteObject(hover);
        }
        const COLORREF color = row.disabled ? muted : fore;
        DrawMenuGlyph(dc, row.glyph, Scale(p, 29), (bounds.top + bounds.bottom) / 2, p, color);
        RECT textRect{bounds.left + textOffset - Scale(p, 7), bounds.top,
                      bounds.right - Scale(p, 38), bounds.bottom};
        SetTextColor(dc, color);
        DrawTextW(dc, row.label.c_str(), -1, &textRect,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (row.next || row.checked) {
            HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(p, 1)), color);
            HGDIOBJ old = SelectObject(dc, pen);
            const int cx = p.width - Scale(p, 29);
            const int cy = (bounds.top + bounds.bottom) / 2;
            if (row.next) {
                MoveToEx(dc, cx - Scale(p, 3), cy - Scale(p, 5), nullptr);
                LineTo(dc, cx + Scale(p, 2), cy);
                LineTo(dc, cx - Scale(p, 3), cy + Scale(p, 5));
            } else {
                MoveToEx(dc, cx - Scale(p, 6), cy, nullptr);
                LineTo(dc, cx - Scale(p, 2), cy + Scale(p, 4));
                LineTo(dc, cx + Scale(p, 6), cy - Scale(p, 5));
            }
            SelectObject(dc, old);
            DeleteObject(pen);
        }
    }
    SelectObject(dc, oldFont);
    BitBlt(target, 0, 0, p.width, p.height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(p.hwnd, &ps);
}

int PopupHitTest(const PopupState& p, POINT point) {
    for (size_t i = 0; i < p.visible.size(); ++i) {
        const auto& row = p.visible[i];
        if (PtInRect(&row.bounds, point) && !p.rows[row.index].separator &&
            !p.rows[row.index].disabled) return static_cast<int>(i);
    }
    return -1;
}

void SwitchPopupPage(PopupState& p, MenuPage page) {
    p.page = page;
    p.scroll = 0;
    BuildPopupRows(p);
    LayoutPopup(p);
    PlacePopup(p);
    InvalidateRect(p.hwnd, nullptr, FALSE);
}

void RunPopupAction(PopupState& p, int visibleIndex) {
    if (visibleIndex < 0 || visibleIndex >= static_cast<int>(p.visible.size())) return;
    const PopupRow row = p.rows[p.visible[visibleIndex].index];
    if (row.separator || row.disabled) return;
    if (row.action == kMenuWindows) { SwitchPopupPage(p, MenuPage::Windows); return; }
    if (row.action == kMenuShortcuts) { SwitchPopupPage(p, MenuPage::Shortcuts); return; }
    if (row.action == kMenuBack) { SwitchPopupPage(p, MenuPage::Main); return; }
    if (row.action == kMenuAutostart) {
        if (SyncStartup(!g_autostart)) {
            g_autostart = !g_autostart;
            WriteSetting(L"AutoStart", g_autostart ? 1 : 0);
            BuildPopupRows(p);
            LayoutPopup(p);
            InvalidateRect(p.hwnd, nullptr, FALSE);
        }
        return;
    }
    const HWND menuWindow = p.hwnd;
    DestroyWindow(menuWindow);
    if (row.action == kMenuSelect) StartSelecting();
    else if (row.action == kMenuExit) DestroyWindow(g_hwnd);
    else if (row.action == kMenuClear) { ClearOwnedPins(); RefreshTooltip(); }
    else if (row.action >= kHotkeyMenuBase && row.action < kHotkeyMenuBase + std::size(kHotkeys)) {
        if (!ChangeHotkey(static_cast<int>(row.action - kHotkeyMenuBase)))
            MessageBoxW(nullptr, L"这个快捷键已被占用，请选择其他组合。", L"PinTray",
                        MB_OK | MB_ICONINFORMATION);
    } else if (row.target && TogglePin(row.target)) RefreshTooltip();
}

LRESULT CALLBACK PopupWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        PopupState* state = reinterpret_cast<PopupState*>(create->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    auto* state = reinterpret_cast<PopupState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!state) return DefWindowProcW(hwnd, message, wParam, lParam);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintPopup(*state); return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) DestroyWindow(hwnd);
        return 0;
    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int next = PopupHitTest(*state, point);
        if (next != state->hover) { state->hover = next; InvalidateRect(hwnd, nullptr, FALSE); }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        state->hover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RunPopupAction(*state, PopupHitTest(*state, point));
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (state->page == MenuPage::Windows && state->rows.size() > kMaxVisibleWindows + 1) {
            state->scroll = std::max(0, state->scroll - (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 2 : -2));
            LayoutPopup(*state);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == VK_LEFT) {
            if (state->page == MenuPage::Main) DestroyWindow(hwnd);
            else SwitchPopupPage(*state, MenuPage::Main);
            return 0;
        }
        if (wParam == VK_DOWN || wParam == VK_UP) {
            if (state->visible.empty()) return 0;
            int next = state->hover;
            int delta = wParam == VK_DOWN ? 1 : -1;
            for (size_t i = 0; i < state->visible.size(); ++i) {
                next = (next + delta + static_cast<int>(state->visible.size())) %
                       static_cast<int>(state->visible.size());
                const auto& row = state->rows[state->visible[next].index];
                if (!row.separator && !row.disabled) {
                    state->hover = next;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
            }
            return 0;
        }
        if (wParam == VK_RETURN || wParam == VK_SPACE || wParam == VK_RIGHT) {
            if (state->hover >= 0) RunPopupAction(*state, state->hover);
            return 0;
        }
        break;
    case WM_DPICHANGED:
        state->dpi = HIWORD(wParam);
        if (state->font) DeleteObject(state->font);
        state->font = CreateFontW(-Scale(*state, 15), 0, 0, 0, FW_NORMAL,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable");
        LayoutPopup(*state);
        PlacePopup(*state);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_NCDESTROY:
        if (state->font) DeleteObject(state->font);
        if (g_popup == state) g_popup = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete state;
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ShowMenu() {
    if (g_popup) {
        DestroyWindow(g_popup->hwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.hInstance = g_instance;
        klass.lpfnWndProc = PopupWindowProc;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.lpszClassName = L"PinTray.FluentPopup";
        klass.style = CS_DROPSHADOW;
        if (!RegisterClassExW(&klass)) return;
        registered = true;
    }
    RefreshTooltip();
    auto* state = new PopupState();
    state->dark = UseDarkMenu();
    GetCursorPos(&state->anchor);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                               L"PinTray.FluentPopup", L"", WS_POPUP,
                               state->anchor.x, state->anchor.y, 100, 100,
                               g_hwnd, nullptr, g_instance, state);
    if (!hwnd) { delete state; return; }
    g_popup = state;
    state->dpi = GetDpiForWindow(hwnd);
    state->font = CreateFontW(-Scale(*state, 15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                              L"Segoe UI Variable");
    const int dark = state->dark ? 1 : 0;
    const int round = 2;          // DWMWCP_ROUND
    const int transient = 3;      // DWMSBT_TRANSIENTWINDOW (Windows 11)
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 33, &round, sizeof(round));
    DwmSetWindowAttribute(hwnd, 38, &transient, sizeof(transient));
    BuildPopupRows(*state);
    LayoutPopup(*state);
    PlacePopup(*state);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
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
        if (g_popup) DestroyWindow(g_popup->hwnd);
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
