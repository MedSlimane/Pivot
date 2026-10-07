#Requires -Version 5.1
<#
  Build Pivot for Windows (native Win32, zero dependencies).
    .\build.ps1              build Pivot.exe with auto-detected compiler
    .\build.ps1 -Install     build + install to %LOCALAPPDATA%\Pivot + Run-at-login key
    .\build.ps1 -Uninstall   remove installed copy + Run-at-login key
  Compilers tried in order: g++ on PATH, WinGet WinLibs MinGW, MSVC cl.
#>
param([switch]$Install, [switch]$Uninstall)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $MyInvocation.MyCommand.Path -Parent)

$AppDir = Join-Path $env:LOCALAPPDATA 'Pivot'
$AppExe = Join-Path $AppDir 'Pivot.exe'

function Find-Gpp {
  $c = Get-Command 'g++.exe' -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  $wl = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*" -Directory -ErrorAction SilentlyContinue |
    Select-Object -First 1
  if ($wl) {
    $g = Join-Path $wl.FullName 'mingw64\bin\g++.exe'
    if (Test-Path -LiteralPath $g) { return $g }
  }
  foreach ($p in @('C:\mingw64\bin\g++.exe', 'C:\msys64\mingw64\bin\g++.exe')) {
    if (Test-Path -LiteralPath $p) { return $p }
  }
  return $null
}

if ($Uninstall) {
  Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'Pivot' -ErrorAction SilentlyContinue
  if (Test-Path -LiteralPath $AppExe) { Remove-Item -LiteralPath $AppExe -Force }
  Write-Output 'Uninstalled Pivot (exe + login item removed).'
  return
}

$gpp = Find-Gpp
if ($gpp) {
  Write-Output "Compiler: $gpp"
  & $gpp -O2 -std=c++17 -DUNICODE -D_UNICODE -mwindows -municode -static src\main.cc -o Pivot.exe -lshell32
  if ($LASTEXITCODE -ne 0) { throw "g++ failed with exit code $LASTEXITCODE" }
} else {
  $cl = Get-Command 'cl.exe' -ErrorAction SilentlyContinue
  if (-not $cl) { throw 'No compiler found. Install MinGW: winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT' }
  Write-Output "Compiler: $($cl.Source)"
  & $cl /O2 /std:c++17 /DUNICODE /D_UNICODE src\main.cc /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib /OUT:Pivot.exe
  if ($LASTEXITCODE -ne 0) { throw "cl failed with exit code $LASTEXITCODE" }
  Remove-Item -LiteralPath 'main.obj' -ErrorAction SilentlyContinue
}

$size = (Get-Item -LiteralPath 'Pivot.exe').Length
Write-Output ("Built: Pivot.exe ({0:N1} KB)" -f ($size / 1KB))

if ($Install) {
  New-Item -ItemType Directory -Path $AppDir -Force | Out-Null
  Copy-Item -LiteralPath 'Pivot.exe' -Destination $AppExe -Force
  New-Item -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Force | Out-Null
  Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'Pivot' -Value "`"$AppExe`""
  Write-Output "Installed: $AppExe (runs at login)"
}
