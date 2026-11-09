using System.Threading;
using System.Windows;
using PinTray.Services;

namespace PinTray;

public partial class App : System.Windows.Application
{
    private Mutex? _singleInstance;
    private TrayController? _tray;
    internal AppSettings Settings { get; private set; } = new();

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        _singleInstance = new Mutex(true, @"Local\NineteenReincarnation.PinTray", out bool isFirstInstance);
        if (!isFirstInstance)
        {
            Shutdown();
            return;
        }

        Settings = SettingsStore.Load();
        try
        {
            StartupRegistration.Synchronize(Settings.StartWithWindows);
            _tray = new TrayController(this);
            _tray.Start();
        }
        catch (Exception ex)
        {
            System.Windows.MessageBox.Show($"PinTray could not start:\n{ex.Message}", "PinTray",
                System.Windows.MessageBoxButton.OK, System.Windows.MessageBoxImage.Error);
            Shutdown(1);
        }
    }

    internal void SaveSettings() => SettingsStore.Save(Settings);

    protected override void OnExit(ExitEventArgs e)
    {
        _tray?.Dispose();
        if (_singleInstance != null)
        {
            try { _singleInstance.ReleaseMutex(); }
            catch (ApplicationException) { /* A secondary instance never owned the mutex. */ }
            _singleInstance.Dispose();
        }

        base.OnExit(e);
    }
}
