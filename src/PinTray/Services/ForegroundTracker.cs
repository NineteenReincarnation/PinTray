using PinTray.Native;

namespace PinTray.Services;

/// <summary>
/// Keeps the last ordinary user window available when the tray receives focus.
/// This is an event-driven hook, not a continuous polling loop.
/// </summary>
public sealed class ForegroundTracker : IDisposable
{
    private readonly WindowPinService _windows;
    private readonly WindowNative.WinEventProc _callback;
    private nint _hook;

    public nint LastWindow { get; private set; }

    public ForegroundTracker(WindowPinService windows)
    {
        _windows = windows;
        _callback = OnForegroundChanged;

        nint current = WindowNative.GetForegroundWindow();
        if (_windows.TryDescribe(current, out _))
            LastWindow = current;

        _hook = WindowNative.SetWinEventHook(WindowNative.EventSystemForeground,
            WindowNative.EventSystemForeground, nint.Zero, _callback, 0, 0,
            WindowNative.WineventOutOfContext | WindowNative.WineventSkipOwnProcess);
    }

    private void OnForegroundChanged(nint hook, uint eventType, nint hwnd,
        int objectId, int childId, uint eventThread, uint eventTime)
    {
        if (objectId == 0 && childId == 0 && _windows.TryDescribe(hwnd, out _))
            LastWindow = hwnd;
    }

    public void Dispose()
    {
        if (_hook != nint.Zero)
        {
            WindowNative.UnhookWinEvent(_hook);
            _hook = nint.Zero;
        }
    }
}
