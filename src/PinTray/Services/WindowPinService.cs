using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using PinTray.Native;

namespace PinTray.Services;

public sealed record WindowEntry(nint Handle, string Title, bool IsTopmost);

/// <summary>
/// Enumerates genuine desktop windows and uses the OS topmost flag as source of truth.
/// Only windows pinned by this process are reverted when PinTray exits.
/// </summary>
public sealed class WindowPinService
{
    private readonly HashSet<nint> _ownedPins = [];
    private static readonly uint OwnProcessId = (uint)Environment.ProcessId;

    public IReadOnlyList<WindowEntry> Enumerate()
    {
        var windows = new List<WindowEntry>();
        WindowNative.EnumWindows((hwnd, _) =>
        {
            if (TryDescribe(hwnd, out WindowEntry? entry))
                windows.Add(entry!);
            return true;
        }, nint.Zero);

        return windows;
    }

    public bool TryDescribe(nint hwnd, out WindowEntry? entry)
    {
        entry = null;
        if (hwnd == nint.Zero || !WindowNative.IsWindow(hwnd) || !WindowNative.IsWindowVisible(hwnd))
            return false;

        WindowNative.GetWindowThreadProcessId(hwnd, out uint processId);
        if (processId == OwnProcessId || WindowNative.GetWindow(hwnd, WindowNative.GwOwner) != nint.Zero)
            return false;

        long style = WindowNative.GetWindowLongPtr(hwnd, WindowNative.GwlExStyle).ToInt64();
        if ((style & WindowNative.WsExToolWindow) != 0)
            return false;

        if (WindowNative.DwmGetWindowAttribute(hwnd, WindowNative.DwmwaCloaked,
                out int cloaked, sizeof(int)) == 0 && cloaked != 0)
            return false;

        int length = WindowNative.GetWindowTextLength(hwnd);
        if (length <= 0)
            return false;

        var title = new StringBuilder(length + 1);
        if (WindowNative.GetWindowText(hwnd, title, title.Capacity) <= 0)
            return false;

        entry = new WindowEntry(hwnd, title.ToString(),
            (style & WindowNative.WsExTopmost) != 0);
        return true;
    }

    public bool Toggle(nint hwnd)
    {
        if (!TryDescribe(hwnd, out WindowEntry? window))
            return false;

        bool currentlyTopmost = window!.IsTopmost;
        nint destination = currentlyTopmost ? WindowNative.HwndNotTopmost : WindowNative.HwndTopmost;
        bool succeeded = WindowNative.SetWindowPos(hwnd, destination, 0, 0, 0, 0,
            WindowNative.SwpNoMove | WindowNative.SwpNoSize | WindowNative.SwpNoActivate);

        if (succeeded)
        {
            if (currentlyTopmost)
                _ownedPins.Remove(hwnd);
            else
                _ownedPins.Add(hwnd);
        }

        return succeeded;
    }

    public int OwnedPinCount
    {
        get
        {
            _ownedPins.RemoveWhere(hwnd => !TryDescribe(hwnd, out WindowEntry? window) || !window!.IsTopmost);
            return _ownedPins.Count;
        }
    }

    public void UnpinAllOwned()
    {
        foreach (nint hwnd in _ownedPins)
        {
            if (WindowNative.IsWindow(hwnd))
                WindowNative.SetWindowPos(hwnd, WindowNative.HwndNotTopmost, 0, 0, 0, 0,
                    WindowNative.SwpNoMove | WindowNative.SwpNoSize | WindowNative.SwpNoActivate);
        }

        _ownedPins.Clear();
    }
}
