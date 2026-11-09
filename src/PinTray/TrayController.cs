using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;

using System.Windows.Threading;
using Forms = System.Windows.Forms;
using PinTray.Native;
using PinTray.Services;

namespace PinTray;

/// <summary>
/// The entire normal user experience lives in the Windows notification area.
/// No main window is created at startup.
/// </summary>
public sealed class TrayController : IDisposable
{
    private readonly App _app;
    private readonly WindowPinService _pins = new();
    private ForegroundTracker? _foreground;
    private GlobalHotkeyService? _hotkeys;
    private Forms.NotifyIcon? _tray;
    private System.Windows.Controls.ContextMenu? _menu;
    private SettingsWindow? _settingsWindow;
    private bool _disposed;

    public TrayController(App app) => _app = app;

    public void Start()
    {
        _foreground = new ForegroundTracker(_pins);
        _hotkeys = new GlobalHotkeyService(ToggleForegroundWindow);
        _hotkeys.TryRegister(_app.Settings.Hotkey);

        _tray = new Forms.NotifyIcon
        {
            Icon = CreatePinIcon(),
            Text = "PinTray",
            Visible = true
        };

        _tray.MouseUp += OnTrayMouseUp;
        UpdateTooltip();
    }

    private void OnTrayMouseUp(object? sender, Forms.MouseEventArgs e)
    {
        if (e.Button == Forms.MouseButtons.Left || e.Button == Forms.MouseButtons.Right)
            OpenMenu();
    }

    private void OpenMenu()
    {
        if (_tray is null)
            return;

        if (_menu != null)
            _menu.IsOpen = false;

        var menu = new System.Windows.Controls.ContextMenu
        {
            Placement = PlacementMode.MousePoint,
            FontFamily = new System.Windows.Media.FontFamily("Segoe UI Variable"),
            MinWidth = 225
        };

        nint recent = _foreground?.LastWindow ?? nint.Zero;
        if (_pins.TryDescribe(recent, out WindowEntry? foreground))
        {
            bool wasPinned = foreground!.IsTopmost;
            AddAction(menu, wasPinned ? "取消当前窗口置顶" : "置顶当前窗口",
                () => ToggleWindow(recent));
        }

        var windows = _pins.Enumerate()
            .OrderByDescending(x => x.IsTopmost)
            .Take(12)
            .ToList();

        menu.Items.Add(new Separator());
        var heading = new System.Windows.Controls.MenuItem { Header = "窗口列表", IsEnabled = false };
        menu.Items.Add(heading);

        if (windows.Count == 0)
        {
            menu.Items.Add(new System.Windows.Controls.MenuItem { Header = "没有可操作的窗口", IsEnabled = false });
        }
        else
        {
            foreach (WindowEntry window in windows)
            {
                nint handle = window.Handle;
                var item = new System.Windows.Controls.MenuItem
                {
                    Header = Shorten(window.Title, 38),
                    ToolTip = window.Title,
                    IsCheckable = true,
                    IsChecked = window.IsTopmost
                };
                item.Click += (_, _) => ToggleWindow(handle);
                menu.Items.Add(item);
            }
        }

        menu.Items.Add(new Separator());
        var clearPins = new System.Windows.Controls.MenuItem
        {
            Header = "取消全部由 PinTray 设置的置顶",
            IsEnabled = _pins.OwnedPinCount > 0
        };
        clearPins.Click += (_, _) =>
        {
            _pins.UnpinAllOwned();
            UpdateTooltip();
        };
        menu.Items.Add(clearPins);

        AddAction(menu, "设置", OpenSettings);
        AddAction(menu, "退出 PinTray", () => _app.Shutdown());

        _menu = menu;
        // WPF handles mouse placement, DPI and screen-edge flipping for this popup.
        menu.IsOpen = true;
    }

    private static void AddAction(System.Windows.Controls.ContextMenu menu, string label, Action action)
    {
        var item = new System.Windows.Controls.MenuItem { Header = label };
        item.Click += (_, _) => action();
        menu.Items.Add(item);
    }

    private static string Shorten(string title, int maxLength) =>
        title.Length <= maxLength ? title : title[..(maxLength - 1)] + "…";

    private void ToggleForegroundWindow()
    {
        // A global hotkey normally leaves its target window in the foreground.
        nint current = WindowNative.GetForegroundWindow();
        if (!_pins.TryDescribe(current, out _))
            current = _foreground?.LastWindow ?? nint.Zero;

        ToggleWindow(current);
    }

    private void ToggleWindow(nint handle)
    {
        _pins.Toggle(handle);
        UpdateTooltip();
    }

    private void UpdateTooltip()
    {
        if (_tray is not null)
        {
            int count = _pins.OwnedPinCount;
            _tray.Text = count == 0 ? "PinTray" : $"PinTray - {count} pinned";
        }
    }

    private void OpenSettings()
    {
        if (_settingsWindow is { IsVisible: true })
        {
            _settingsWindow.Activate();
            return;
        }

        _settingsWindow = new SettingsWindow(_app.Settings,
            SetAutostart, SetHotkey);
        _settingsWindow.Closed += (_, _) => _settingsWindow = null;
        _settingsWindow.Show();
        _settingsWindow.Activate();
    }

    private bool SetAutostart(bool enabled)
    {
        try
        {
            StartupRegistration.Synchronize(enabled);
            _app.Settings.StartWithWindows = enabled;
            _app.SaveSettings();
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    private bool SetHotkey(string choice)
    {
        if (_hotkeys is null || !_hotkeys.TryRegister(choice))
            return false;

        _app.Settings.Hotkey = choice;
        try
        {
            _app.SaveSettings();
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    private static Icon CreatePinIcon()
    {
        using var bitmap = new Bitmap(32, 32);
        using (Graphics graphics = Graphics.FromImage(bitmap))
        {
            graphics.Clear(Color.Transparent);
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            using var darkPen = new Pen(Color.FromArgb(20, 38, 58), 2.3f);
            using var faceBrush = new SolidBrush(Color.FromArgb(96, 165, 250));
            using var brightPen = new Pen(Color.FromArgb(222, 235, 255), 3.0f);
            graphics.TranslateTransform(16, 16);
            graphics.RotateTransform(45);
            graphics.FillRectangle(faceBrush, -7, -11, 14, 14);
            graphics.DrawLine(darkPen, -7, -5, 7, -5);
            graphics.DrawLine(brightPen, 0, 3, 0, 14);
            graphics.DrawEllipse(darkPen, -2, -12, 4, 4);
        }

        nint handle = bitmap.GetHicon();
        try
        {
            using var icon = Icon.FromHandle(handle);
            return (Icon)icon.Clone();
        }
        finally
        {
            WindowNative.DestroyIcon(handle);
        }
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;
        if (_menu is not null)
            _menu.IsOpen = false;

        if (_tray is not null)
        {
            _tray.MouseUp -= OnTrayMouseUp;
            _tray.Visible = false;
            _tray.Icon?.Dispose();
            _tray.Dispose();
        }

        _hotkeys?.Dispose();
        _foreground?.Dispose();
        _pins.UnpinAllOwned();
        _settingsWindow?.Close();
    }
}
