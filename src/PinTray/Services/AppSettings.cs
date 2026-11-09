namespace PinTray.Services;

public sealed class AppSettings
{
    public bool StartWithWindows { get; set; } = true;

    // An alternative to PowerToys' default Win+Ctrl+T, avoiding an immediate conflict.
    public string Hotkey { get; set; } = "Ctrl+Alt+P";
}
