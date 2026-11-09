$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Load([string]$relative) {
    Get-Content -LiteralPath (Join-Path $root $relative) -Encoding UTF8 -Raw
}
function Require([bool]$condition, [string]$description) {
    if (-not $condition) { throw "PinTray review failed: $description" }
    Write-Host "OK: $description"
}
$cpp = Load 'src/main.cpp'
$i18n = Load 'src/i18n.h'
$rc = Load 'src/PinTray.rc'
$manifest = Load 'src/app.manifest'
$installer = Load 'packaging/PinTray.iss'
$versions = Load 'VERSIONING_RULES.md'
$notes = Load '.github/RELEASE_NOTES.md'
$workflow = Load '.github/workflows/build.yml'

$m = [regex]::Match($rc, 'VALUE "ProductVersion", "(\d+)\.(\d+)\.(\d+)\\0"')
Require $m.Success 'native executable has a three-part version'
$v = $m.Groups[1].Value + '.' + $m.Groups[2].Value + '.' + $m.Groups[3].Value
Require ($rc.Contains("FILEVERSION $($m.Groups[1].Value),$($m.Groups[2].Value),$($m.Groups[3].Value),0")) 'file version is synchronized'
Require ($manifest.Contains('version="' + $v + '.0"')) 'application manifest version is synchronized'
Require ($installer.Contains('AppVersion "' + $v + '"')) 'installer version is synchronized'
Require ($versions.Contains('Current development version:') -and $versions.Contains($v)) 'version rules record is synchronized'
Require ($notes.Contains('PinTray ' + $v)) 'release notes are synchronized'
Require ($v.StartsWith('0.')) 'major version stays 0 before Store approval'

Require ($workflow.Contains('/utf-8')) 'C++ is compiled in UTF-8 for all menu languages'
Require ($cpp.Contains('GetUserPreferredUILanguages') -or $i18n.Contains('GetUserPreferredUILanguages')) 'Windows display language is detected'
Require ($i18n.Contains('return english;')) 'unknown display languages use English fallback'
Require ($i18n.Contains('zh-tw') -and $i18n.Contains('zh-hk')) 'Traditional Chinese variants are handled'
Require ($cpp.Contains('GetNativeBadgeRect') -and $cpp.Contains('kBadgeMove')) 'caption pins move without full window enumerations'
Require ($cpp.Contains('EVENT_OBJECT_DESTROY') -and $cpp.Contains('RefreshBadges')) 'closed windows are cleaned up'
Require ($cpp.Contains('SetWindowPos(hwnd, destination')) 'native topmost toggle remains in place'
Require ($cpp.Contains('RegisterHotKey(')) 'global keyboard toggle remains in place'
Require (-not $cpp.Contains('WH_MOUSE_LL')) 'obsolete click-anywhere mode is removed'
Require (-not $cpp.Contains('GetWindowTextW(')) 'window-title text is not allocated during toggles'
Require ($cpp.Contains('DeleteObject(') -and $cpp.Contains('UnhookWinEvent(') -and $cpp.Contains('DestroyWindow(')) 'GDI, event hooks and overlays have cleanup paths'
Require ($cpp.Contains('WS_EX_NOACTIVATE')) 'pin overlay does not steal application focus'
Require ($cpp.Contains('TrackPopupMenuEx(')) 'tray context menu uses native Windows focus and dismissal'
Require (-not $cpp.Contains('PopupWindowProc')) 'obsolete custom popup window has been removed'
Write-Host "PinTray static regression checks passed ($v)."
