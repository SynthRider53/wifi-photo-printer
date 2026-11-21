<#
.SYNOPSIS
    Build (and optionally flash) the photo printer firmware, ESP-IDF v5.5.5.

.EXAMPLE
    .\tools\build.ps1
    .\tools\build.ps1 -Flash -Port COM14
#>
param(
    [string]$Port = '',
    [switch]$Flash
)

$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot
$IdfRoot = 'C:\Espressif'
$IdfPath = Join-Path $IdfRoot 'frameworks\esp-idf-v5.5.5'

Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$env:IDF_TOOLS_PATH = $IdfRoot
$python = Get-ChildItem (Join-Path $IdfRoot 'tools\idf-python') -Directory | Select-Object -First 1
if ($python) { $env:PATH = "$($python.FullName);$env:PATH" }
. (Join-Path $IdfPath 'export.ps1') | Out-Null

Push-Location $Root
try {
    idf.py build
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
    if ($Flash) {
        if ($Port) { idf.py -p $Port flash } else { idf.py flash }
        if ($LASTEXITCODE -ne 0) { throw "flash failed ($LASTEXITCODE)" }
    }
}
finally {
    Pop-Location
}
