# PinTray

**PinTray** is a small Windows 11 utility that keeps selected windows always on top. It lives in the notification area, stays out of your way, and uses the native Windows topmost API.

> **Development preview: 0.1.1** · Not yet submitted to Microsoft Store.

## Features

- Always-on-top toggle for ordinary desktop windows.
- Left- or right-click the tray icon for a list of open windows; checked entries are topmost.
- Global shortcut: **Ctrl + Alt + P** by default. Customizable in Settings.
- Starts with Windows by default (per-user, unpackaged installation).
- Minimal settings window using the Windows 11 Fluent WPF theme, following light/dark mode.
- Single-instance operation. No continuous window polling.
- Exiting PinTray removes only the topmost flags applied by this instance; it does not reset unrelated apps' flags.

## 中文说明

PinTray 是一款轻量级的 Windows 11 窗口置顶工具。启动后常驻系统托盘，不显示主窗口。

- 左键或右键单击托盘图标，选择窗口即可切换置顶状态。
- 默认快捷键：**Ctrl + Alt + P**；可在设置中修改。它与 PowerToys 常用的 Win + Ctrl + T 避免了默认冲突。
- 默认注册当前用户开机启动；设置中可关闭。
- 退出时只取消 PinTray 本次运行所设置的置顶状态。
- 目前是 **0.1.1 开发预览版**，尚未符合微软商店正式发布标准。

## Requirements

Windows 11 x64. For development, use the .NET 10 SDK and Visual Studio with .NET desktop development support. The GitHub self-contained build does not require a separately installed .NET Desktop Runtime.

## Build

```powershell
dotnet build src/PinTray/PinTray.csproj -c Release -p:Platform=x64
dotnet publish src/PinTray/PinTray.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o artifacts/publish
```

Open `artifacts/publish/PinTray.exe` to run the portable build. Moving that executable after enabling startup requires launching it from its new location so its startup path can be refreshed.

On Windows, the GitHub Actions workflow also prepares a per-user Inno Setup installer; use that installer for normal distribution.

## Planned distribution

| Distribution | Status |
| --- | --- |
| GitHub independent build / installer | Initial implementation |
| Microsoft Store MSIX | Planned; **not yet implemented or certified** |

MSIX packaged startup is intentionally **not** implemented in this preview; packaged processes must use an appropriate StartupTask approach rather than the unpackaged HKCU Run registration. Packaging, icons, accessibility, localization and installer validation are required before Store submission.

## Versioning

See [VERSIONING_RULES.md](VERSIONING_RULES.md). The project uses `X.Y.Z`, **not SemVer**. `X` remains `0` throughout pre-release development and becomes `1` only after the repository owner explicitly confirms Microsoft Store readiness.

## License

Licensed under [Apache License 2.0](LICENSE).
