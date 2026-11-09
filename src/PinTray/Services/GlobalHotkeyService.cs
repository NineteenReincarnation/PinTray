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
        switch (choice)
        {
            case "Ctrl+Alt+P":
                modifiers = WindowNative.ModControl | WindowNative.ModAlt;
                key = 0x50;
                return true;
            case "Ctrl+Alt+T":
                modifiers = WindowNative.ModControl | WindowNative.ModAlt;
                key = 0x54;
                return true;
            case "Ctrl+Shift+Space":
                modifiers = WindowNative.ModControl | WindowNative.ModShift;
                key = 0x20;
                return true;
            case "Win+Ctrl+T":
                modifiers = WindowNative.ModWin | WindowNative.ModControl;
                key = 0x54;
                return true;
            default:
                modifiers = 0;
                key = 0;
                return false;
        }
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
