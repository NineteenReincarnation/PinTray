using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace PinTray.Services;

/// <summary>
/// GitHub/unpackaged startup registration. MSIX requires a separate StartupTask adapter
/// before Store release; never write the virtualized Run key from a packaged process.
/// </summary>
public static class StartupRegistration
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "PinTray";
    private const int ErrorInsufficientBuffer = 122;

    public static bool IsPackaged
    {
        get
        {
            int length = 0;
            return GetCurrentPackageFullName(ref length, null) == ErrorInsufficientBuffer;
        }
    }

    public static void Synchronize(bool enabled)
    {
        if (IsPackaged)
            return;

        using RegistryKey? key = Registry.CurrentUser.OpenSubKey(RunKey, writable: true);
        if (key is null)
            throw new InvalidOperationException("Cannot open the current user's Windows startup registry key.");

        string? existing = key.GetValue(ValueName) as string;
        if (!enabled)
        {
            if (existing != null)
                key.DeleteValue(ValueName, throwOnMissingValue: false);
            return;
        }

        string executable = Environment.ProcessPath
            ?? throw new InvalidOperationException("Executable location is unavailable.");
        string command = $"\"{executable}\" --background";

        // Do not continuously rewrite an unchanged startup entry.
        if (!string.Equals(existing, command, StringComparison.Ordinal))
            key.SetValue(ValueName, command, RegistryValueKind.String);
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int GetCurrentPackageFullName(ref int packageFullNameLength,
        StringBuilder? packageFullName);
}
