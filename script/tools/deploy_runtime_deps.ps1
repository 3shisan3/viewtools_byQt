<#
.SYNOPSIS
    Complete the runtime dependencies of the deploy directory.
.DESCRIPTION
    windeployqt and the FFmpeg deploy step only copy first level dependencies,
    so indirectly referenced libraries (libzstd, libshaderc_shared, ...) are
    often missing and the program dies with 0xC0000135.
    This script walks the import table of every exe/dll inside the deploy
    directory and copies whatever is missing but available in the toolchain
    bin directory (e.g. D:/Dev/msys64/ucrt64/bin).
.PARAMETER BinDir
    Deploy directory (bin)
.PARAMETER ToolchainBin
    Toolchain bin directory, e.g. D:/Dev/msys64/ucrt64/bin
#>

param(
    [Parameter(Mandatory = $true)][string]$BinDir,
    [Parameter(Mandatory = $true)][string]$ToolchainBin
)

$ErrorActionPreference = "Stop"

$objdump = Join-Path $ToolchainBin "objdump.exe"
if (-not (Test-Path $objdump)) {
    $objdump = (Get-Command objdump -ErrorAction SilentlyContinue).Source
}
if (-not $objdump) {
    Write-Host "[deps] objdump not found, skip" -ForegroundColor Yellow
    exit 0
}

$sysRoot = $env:SystemRoot
if (-not $sysRoot) { $sysRoot = "C:\Windows" }
$sysDir = Join-Path $sysRoot "System32"

# Dependencies provided by the OS, never copied
function Test-SystemDll {
    param([string]$name)
    if ($name -match '^(api-ms-|ext-ms-)') { return $true }
    if (Test-Path (Join-Path $sysDir $name)) { return $true }
    return $false
}

$pending = New-Object System.Collections.Queue
foreach ($f in (Get-ChildItem -Path $BinDir -Include *.exe, *.dll -Recurse -File)) {
    $pending.Enqueue($f.FullName)
}

$seen = @{}
$copied = @()
$unknown = @{}

while ($pending.Count -gt 0) {
    $file = $pending.Dequeue()
    $key = [System.IO.Path]::GetFileName($file).ToLower()
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true

    $deps = & $objdump -p $file 2>$null |
            Select-String "DLL Name" |
            ForEach-Object { ($_ -split ":\s*")[1].Trim() }

    foreach ($dep in $deps) {
        $lk = $dep.ToLower()
        if ($seen.ContainsKey($lk)) { continue }
        if (Test-Path (Join-Path $BinDir $dep)) { continue }
        if (Test-SystemDll $dep) { continue }

        $src = Join-Path $ToolchainBin $dep
        if (Test-Path $src) {
            Copy-Item -Path $src -Destination $BinDir -Force
            $copied += $dep
            $seen[$lk] = $true
            $pending.Enqueue((Join-Path $BinDir $dep))
        } else {
            $unknown[$dep] = $true
        }
    }
}

if ($copied.Count -gt 0) {
    Write-Host "[deps] copied: $($copied -join ', ')" -ForegroundColor Green
} else {
    Write-Host "[deps] nothing to copy" -ForegroundColor Gray
}
if ($unknown.Count -gt 0) {
    Write-Host "[deps] not found in toolchain: $($unknown.Keys -join ', ')" -ForegroundColor Yellow
}
exit 0
