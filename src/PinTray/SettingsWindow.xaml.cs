using System.Windows;
using System.Windows.Controls;
using PinTray.Services;

namespace PinTray;

public partial class SettingsWindow : Window
{
    private readonly AppSettings _settings;
    private readonly Func<bool, bool> _setStartup;
    private readonly Func<string, bool> _setHotkey;
    private bool _initializing = true;

    public SettingsWindow(AppSettings settings, Func<bool, bool> setStartup,
        Func<string, bool> setHotkey)
    {
        _settings = settings;
        _setStartup = setStartup;
        _setHotkey = setHotkey;

        InitializeComponent();

        StartupCheckBox.IsChecked = _settings.StartWithWindows;
        if (StartupRegistration.IsPackaged)
        {
            StartupCheckBox.IsEnabled = false;
            StartupCheckBox.ToolTip = "商店打包版本的启动任务将在正式发布前接入。";
        }

        foreach (ComboBoxItem option in HotkeyComboBox.Items)
        {
            if (string.Equals(option.Content?.ToString(), _settings.Hotkey,
                    StringComparison.Ordinal))
            {
                HotkeyComboBox.SelectedItem = option;
                break;
            }
        }

        Version version = typeof(App).Assembly.GetName().Version ?? new Version(0, 0, 0);
        VersionText.Text = $"开发版本 {version.Major}.{version.Minor}.{version.Build}";
        StatusText.Text = "左键或右键点击托盘图标可打开窗口列表。";
        _initializing = false;
    }

    private void OnStartupChanged(object sender, RoutedEventArgs e)
    {
        if (_initializing)
            return;

        bool enabled = StartupCheckBox.IsChecked == true;
        if (_setStartup(enabled))
        {
            StatusText.Text = enabled ? "开机启动已启用。" : "开机启动已关闭。";
            return;
        }

        _initializing = true;
        StartupCheckBox.IsChecked = _settings.StartWithWindows;
        _initializing = false;
        StatusText.Text = "无法修改开机启动设置，请检查当前用户的注册表权限。";
    }

    private void OnHotkeyChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_initializing || HotkeyComboBox.SelectedItem is not ComboBoxItem selected)
            return;

        string candidate = selected.Content?.ToString() ?? "";
        if (_setHotkey(candidate))
        {
            StatusText.Text = $"快捷键已设置为 {candidate}。";
            return;
        }

        _initializing = true;
        foreach (ComboBoxItem option in HotkeyComboBox.Items)
        {
            if (string.Equals(option.Content?.ToString(), _settings.Hotkey,
                    StringComparison.Ordinal))
            {
                HotkeyComboBox.SelectedItem = option;
                break;
            }
        }
        _initializing = false;
        StatusText.Text = "快捷键不可用，可能已被其他应用占用。";
    }

    private void OnCloseClicked(object sender, RoutedEventArgs e) => Close();
}
