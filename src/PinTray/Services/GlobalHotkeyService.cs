using System.Windows.Interop;
using PinTray.Native;

namespace PinTray.Services;

/// <summary>
/// Registers one global keyboard shortcut against an invisible Win32 message sink.
/// Does not inject keystrokes and does not install a keyboard hook.
/// </summary>
public sealed class GlobalHotkeyService : IDisposable
{
    private readonly HwndSource _source;
    private readonly Action _onPressed;
    private int _activeId;
    private int _nextId = 1;

    public string? ActiveHotkey { get; private set; }

    public static readonly string[] Choices =
    [
        "Ctrl+Alt+P",
        "Ctrl+Alt+T",
        "Ctrl+Shift+Space",
        "Win+Ctrl+T"
    ];

    public GlobalHotkeyService(Action onPressed)
    {
        _onPressed = onPressed;
        var parameters = new HwndSourceParameters("PinTray.KeyboardShortcut")
        {
            Width = 0,
            Height = 0,
            WindowStyle = 0
        };
        _source = new HwndSource(parameters);
        _source.AddHook(WndProc);
    }

    public bool TryRegister(string choice)
    {
        if (string.Equals(ActiveHotkey, choice, StringComparison.Ordinal))
            return true;

        if (!TryDecode(choice, out uint modifiers, out uint key))
            return false;

        int newId = _nextId++;
        if (!WindowNative.RegisterHotKey(_source.Handle, newId,
                modifiers | WindowNative.ModNoRepeat, key))
            return false;

        if (_activeId != 0)
            WindowNative.UnregisterHotKey(_source.Handle, _activeId);

        _activeId = newId;
        ActiveHotkey = choice;
        return true;
    }

    private static bool TryDecode(string choice, out uint modifiers, out uint key)
    {
        (modifiers, key) = choice switch
        {
            "Ctrl+Alt+P" => (WindowNative.ModControl | WindowNative.ModAlt, 0x50),
            "Ctrl+Alt+T" => (WindowNative.ModControl | WindowNative.ModAlt, 0x54),
            "Ctrl+Shift+Space" => (WindowNative.ModControl | WindowNative.ModShift, 0x20),
            "Win+Ctrl+T" => (WindowNative.ModWin | WindowNative.ModControl, 0x54),
            _ => (0u, 0u)
        };

        return key != 0;
    }

    private nint WndProc(nint hwnd, int message, nint wParam, nint lParam, ref bool handled)
    {
        if (message == WindowNative.WmHotkey && wParam.ToInt64() == _activeId)
        {
            _onPressed();
            handled = true;
        }

        return nint.Zero;
    }

    public void Dispose()
    {
        if (_activeId != 0)
            WindowNative.UnregisterHotKey(_source.Handle, _activeId);

        _source.RemoveHook(WndProc);
        _source.Dispose();
    }
}
