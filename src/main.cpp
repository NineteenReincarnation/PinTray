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
#include <cwchar>
#include <windowsx.h>
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
constexpr UINT kPopupCheckFocus = WM_APP + 2;
constexpr UINT kBadgeSync = WM_APP + 3;
constexpr UINT kTrayId = 1;
constexpr UINT kHotkeyMenuBase = 2000;
constexpr UINT kMenuAutostart = 3000;
constexpr UINT kMenuExit = 3003;
constexpr UINT kHotkeyIdA = 50;
constexpr UINT kHotkeyIdB = 51;

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

// The popup is allocated only while visible. Regular tray operation needs no UI framework.
enum class MenuPage { Main, Shortcuts };
enum MenuGlyph { GlyphNone, GlyphKeyboard, GlyphStartup, GlyphExit, GlyphBack };
constexpr UINT kMenuShortcuts = 3005;
constexpr UINT kMenuBack = 3006;

struct PopupRow {
    std::wstring label;
    UINT action = 0;
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
    int width = 0, height = 0, hover = -1;
    bool dark = true;
    bool hasBeenActivated = false;
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
                bool checked = false, bool next = false, bool disabled = false) {
    p.rows.push_back({name, command, glyph, checked, next, disabled, false});
}
void MenuSeparator(PopupState& p) {
    p.rows.push_back({L"", 0, GlyphNone, false, false, true, true});
}

void BuildPopupRows(PopupState& p) {
    p.rows.clear();
    p.hover = -1;
    if (p.page == MenuPage::Main) {
        AddMenuRow(p, g_text.shortcuts, kMenuShortcuts, GlyphKeyboard, false, true);
        MenuSeparator(p);
        AddMenuRow(p, g_text.startup, kMenuAutostart, GlyphStartup, g_autostart, false, g_packaged);
        MenuSeparator(p);
        AddMenuRow(p, g_text.quit, kMenuExit, GlyphExit);
    } else {
        AddMenuRow(p, g_text.shortcuts, kMenuBack, GlyphBack);
        for (size_t i = 0; i < std::size(kHotkeys); ++i) {
            AddMenuRow(p, kHotkeys[i].label, kHotkeyMenuBase + static_cast<UINT>(i),
                       GlyphKeyboard, g_hotkeyChoice == static_cast<int>(i));
        }
    }
}

void LayoutPopup(PopupState& p) {
    p.visible.clear();
    p.width = Scale(p, 260);
    // Measure localized text once when opening/changing the menu. The normal
    // idle process does not keep a graphics context or continually measure text.
    HDC dc = GetDC(nullptr);
    if (dc) {
        HFONT old = p.font ? static_cast<HFONT>(SelectObject(dc, p.font)) : nullptr;
        for (const PopupRow& row : p.rows) {
            if (row.separator) continue;
            SIZE measured{};
            if (GetTextExtentPoint32W(dc, row.label.c_str(),
                                      static_cast<int>(row.label.size()), &measured))
                p.width = std::max(p.width, static_cast<int>(measured.cx) + Scale(p, 112));
        }
        if (old) SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }
    p.width = std::min(p.width, Scale(p, 470));
    const int padding = Scale(p, 7);
    int y = padding;
    for (size_t i = 0; i < p.rows.size(); ++i) {
        const int height = Scale(p, p.rows[i].separator ? 9 : 43);
        p.visible.push_back({i, {padding, y, p.width - padding, y + height}});
        y += height;
    }
    p.height = y + padding;
}

void PlacePopup(PopupState& p) {
    HMONITOR monitor = MonitorFromPoint(p.anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const RECT work = info.rcWork;
    const int offset = Scale(p, 10);
    const int left = static_cast<int>(work.left);
    const int top = static_cast<int>(work.top);
    const int right = static_cast<int>(work.right);
    const int bottom = static_cast<int>(work.bottom);
    int x = std::clamp(static_cast<int>(p.anchor.x) - Scale(p, 48), left,
                       std::max(left, right - p.width));
    int y = (p.anchor.y + p.height + offset <= bottom)
        ? p.anchor.y + offset : p.anchor.y - p.height - offset;
    y = std::clamp(y, top, std::max(top, bottom - p.height));
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

HFONT CreateSystemMenuFont(UINT dpi) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        LOGFONTW font = metrics.lfMenuFont;
        font.lfHeight = MulDiv(font.lfHeight, static_cast<int>(dpi), 96);
        return CreateFontIndirectW(&font);
    }
    return CreateFontW(-MulDiv(15, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
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
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
                  (g_rtlLanguage ? DT_RTLREADING | DT_RIGHT : 0));
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
    BuildPopupRows(p);
    LayoutPopup(p);
    PlacePopup(p);
    InvalidateRect(p.hwnd, nullptr, FALSE);
}

void RunPopupAction(PopupState& p, int visibleIndex) {
    if (visibleIndex < 0 || visibleIndex >= static_cast<int>(p.visible.size())) return;
    const PopupRow row = p.rows[p.visible[visibleIndex].index];
    if (row.separator || row.disabled) return;
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
    DestroyWindow(p.hwnd);
    if (row.action == kMenuExit) DestroyWindow(g_hwnd);
    else if (row.action >= kHotkeyMenuBase && row.action < kHotkeyMenuBase + std::size(kHotkeys)) {
        if (!ChangeHotkey(static_cast<int>(row.action - kHotkeyMenuBase)))
            MessageBoxW(nullptr, g_text.occupied, L"PinTray",
                        MB_OK | MB_ICONINFORMATION);
    }
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
        if (LOWORD(wParam) != WA_INACTIVE) {
            state->hasBeenActivated = true;
        } else if (state->hasBeenActivated) {
            // The notification area can briefly change activation on right-click.
            // Check focus asynchronously rather than destroying the popup in WM_ACTIVATE.
            PostMessageW(hwnd, kPopupCheckFocus, 0, 0);
        }
        return 0;
    case kPopupCheckFocus:
        if (GetForegroundWindow() != hwnd) DestroyWindow(hwnd);
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
        state->font = CreateSystemMenuFont(state->dpi);
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
        // Never toggle the existing popup for a duplicate tray notification.
        if (IsWindow(g_popup->hwnd)) SetForegroundWindow(g_popup->hwnd);
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
    state->font = CreateSystemMenuFont(state->dpi);
    const int dark = state->dark ? 1 : 0;
    const int round = 2;          // DWMWCP_ROUND
    const int transient = 3;      // DWMSBT_TRANSIENTWINDOW (Windows 11)
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 33, &round, sizeof(round));
    DwmSetWindowAttribute(hwnd, 38, &transient, sizeof(transient));
    BuildPopupRows(*state);
    LayoutPopup(*state);
    PlacePopup(*state); // SetWindowPos already shows it without activation.
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
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
};
std::vector<BadgeEntry> g_badges;
HWINEVENTHOOK g_badgeHooks[4]{};
bool g_badgeRefreshQueued = false;
constexpr wchar_t kBadgeClass[] = L"PinTray.WindowBadge";
constexpr COLORREF kBadgeTransparency = RGB(255, 0, 255);

void RequestBadgeRefresh() {
    if (!g_hwnd || g_badgeRefreshQueued) return;
    g_badgeRefreshQueued = PostMessageW(g_hwnd, kBadgeSync, 0, 0) != FALSE;
}

bool IsBadgeEligible(HWND target, DWORD* processId = nullptr, RECT* bounds = nullptr) {
    if (!DescribeWindow(target) || IsIconic(target)) return false;
    const LONG_PTR style = GetWindowLongPtrW(target, GWL_STYLE);
    if (!(style & WS_CAPTION)) return false;
    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS,
                                    &rect, sizeof(rect))) && !GetWindowRect(target, &rect))
        return false;
    if (rect.right - rect.left < 220 || rect.bottom - rect.top < 70) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    if (processId) *processId = pid;
    if (bounds) *bounds = rect;
    return true;
}

bool IsTrackedPin(HWND target, DWORD pid) {
    return std::any_of(g_ownedPins.begin(), g_ownedPins.end(),
        [target, pid](const PinState& p) { return p.hwnd == target && p.pid == pid; });
}

// No permanent tile: only the native-size pin glyph is visible in the caption.
// A subtle hover background is drawn on demand, just like other caption buttons.
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

    HWND target = reinterpret_cast<HWND>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    const bool pinned = IsWindow(target) && (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST);
    const bool dark = UseDarkMenu();
    auto it = std::find_if(g_badges.begin(), g_badges.end(),
        [hwnd](const BadgeEntry& p) { return p.badge == hwnd; });
    const bool hovering = it != g_badges.end() && it->hover;
    if (hovering) {
        // Match the understated Win11 caption hover treatment. Never draw
        // the old square outline/background when the mouse is not over it.
        HBRUSH highlight = CreateSolidBrush(dark ? RGB(69, 69, 69) : RGB(226, 226, 226));
        FillRect(dc, &rect, highlight);
        DeleteObject(highlight);
    }

    const COLORREF ink = pinned ? RGB(23, 164, 229)
        : dark ? RGB(236, 236, 236) : RGB(35, 35, 35);
    const int cx = w / 2;
    const int cy = h / 2;
    const int size = std::max(10, std::min(w, h) * 18 / 32);
    const int unit = std::max(1, size / 14);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, size / 9), ink);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    auto line = [&](int x1, int y1, int x2, int y2) {
        MoveToEx(dc, cx + x1 * unit, cy + y1 * unit, nullptr);
        LineTo(dc, cx + x2 * unit, cy + y2 * unit);
    };
    line(-5, -6, 5, -6);
    line(-3, -6, -3, -1);
    line(3, -6, 3, -1);
    line(-6, -1, 6, -1);
    line(0, -1, 0, 7);
    if (pinned) line(-2, 6, 2, 6);
    SelectObject(dc, oldPen);
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

// Compute coordinates from Windows' actual native caption-button region.
// DWM exposes this in window-relative coordinates, which remain usable with
// different DPI, window borders and Win11 Explorer tabbed title bars.
bool GetNativeBadgeRect(HWND target, RECT& placement) {
    RECT wr{};
    if (!GetWindowRect(target, &wr)) return false;
    const UINT dpi = std::max(96u, GetDpiForWindow(target));
    const int width = wr.right - wr.left;
    const int height = wr.bottom - wr.top;
    const int buttonWidth = std::max(30, GetSystemMetricsForDpi(SM_CXSIZE, dpi));
    const int defaultHeight = std::max(28, GetSystemMetricsForDpi(SM_CYSIZE, dpi));
    const int spacing = std::max(1, MulDiv(2, dpi, 96));
    int x = 0, y = 0, buttonHeight = defaultHeight;
    RECT buttons{};
    const bool hasNativeBounds =
        SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_CAPTION_BUTTON_BOUNDS,
                                        &buttons, sizeof(buttons))) &&
        buttons.right > buttons.left && buttons.bottom > buttons.top &&
        buttons.left >= 0 && buttons.top >= 0 &&
        buttons.right <= width && buttons.bottom <= height / 2;

    if (hasNativeBounds) {
        buttonHeight = std::clamp(static_cast<int>(buttons.bottom - buttons.top),
                                  MulDiv(26, dpi, 96), MulDiv(54, dpi, 96));
        const bool buttonsOnLeft = buttons.left < width / 2;
        x = wr.left + (buttonsOnLeft ? buttons.right + spacing
                                    : buttons.left - buttonWidth - spacing);
        y = wr.top + buttons.top +
            (static_cast<int>(buttons.bottom - buttons.top) - buttonHeight) / 2;
    } else {
        // Some custom-title-bar applications do not report DWM caption bounds.
        // Keep to the immediate left of the three standard system buttons.
        x = wr.right - 4 * buttonWidth - spacing;
        y = wr.top + MulDiv(IsZoomed(target) ? 0 : 4, dpi, 96);
    }
    // Never cover the system buttons, and avoid nonsensical placement on
    // narrow/custom windows.
    if (x < wr.left + MulDiv(120, dpi, 96) ||
        x + buttonWidth > wr.right - buttonWidth ||
        y < wr.top || y + buttonHeight > wr.bottom) return false;
    placement = {x, y, x + buttonWidth, y + buttonHeight};
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

    // A persistent caption button on EVERY eligible top-level window.
    // Enumeration is event-driven; there is no timer or constant polling.
    struct Target { HWND hwnd; DWORD pid; RECT placement; };
    std::vector<Target> targets;
    targets.reserve(24);
    EnumWindows([](HWND target, LPARAM context) -> BOOL {
        auto& targets = *reinterpret_cast<std::vector<Target>*>(context);
        DWORD pid = 0;
        RECT placement{};
        if (targets.size() < 128 && IsBadgeEligible(target, &pid) &&
            GetNativeBadgeRect(target, placement)) {
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
            g_badges.push_back({target, pid, badge, pinned, dark, false});
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

void CALLBACK OnBadgeWindowEvent(HWINEVENTHOOK, DWORD event, HWND target,
                                  LONG object, LONG child, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND) {
        RequestBadgeRefresh();
        return;
    }
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !target) return;
    // A newly shown/hidden root window can change the set of visible badges.
    if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE ||
        event == EVENT_SYSTEM_MINIMIZESTART || event == EVENT_SYSTEM_MINIMIZEEND) {
        if (target == GetAncestor(target, GA_ROOT)) RequestBadgeRefresh();
        return;
    }
    // Movement events can be frequent: only react to windows we actually track.
    if (event == EVENT_OBJECT_LOCATIONCHANGE &&
        std::any_of(g_badges.begin(), g_badges.end(),
                    [target](const BadgeEntry& b) { return b.target == target; })) {
        RequestBadgeRefresh();
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
        if (g_popup) {
            g_popup->dark = UseDarkMenu();
            BuildPopupRows(*g_popup);
            LayoutPopup(*g_popup);
            PlacePopup(*g_popup);
            InvalidateRect(g_popup->hwnd, nullptr, FALSE);
        }
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
    case WM_DESTROY:
        if (g_popup) DestroyWindow(g_popup->hwnd);
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
