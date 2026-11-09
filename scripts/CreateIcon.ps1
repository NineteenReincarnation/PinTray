param(
    [string]$InputPng = "assets\PinTray.png",
    [string]$OutputIco = "assets\PinTray.ico"
)
$ErrorActionPreference = "Stop"
$png = [System.IO.File]::ReadAllBytes($InputPng)
# Windows Vista and newer support PNG-compressed icon frames.
$stream = [System.IO.File]::Create($OutputIco)
try {
    $writer = [System.IO.BinaryWriter]::new($stream)
    $writer.Write([uint16]0)       # Reserved
    $writer.Write([uint16]1)       # Icon
    $writer.Write([uint16]1)       # Frame count
    $writer.Write([byte]0)         # Width: 256
    $writer.Write([byte]0)         # Height: 256
    $writer.Write([byte]0)         # Colors
    $writer.Write([byte]0)         # Reserved
    $writer.Write([uint16]1)       # Planes
    $writer.Write([uint16]32)      # Bit depth
    $writer.Write([uint32]$png.Length)
    $writer.Write([uint32]22)      # Image offset
    $writer.Write($png)
    $writer.Flush()
} finally {
    $writer.Dispose()
}
