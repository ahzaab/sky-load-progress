[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$CMakeExe,
    [string]$VsDevCmd,
    [string]$NinjaExe
)

$ErrorActionPreference = 'Stop'
$configurePreset = if ($Configuration -eq 'Debug') { 'build-debug-msvc' } else { 'build-release-msvc' }
$buildPreset = if ($Configuration -eq 'Debug') { 'debug-msvc' } else { 'release-msvc' }

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or -not $env:INCLUDE) {
    if (-not $VsDevCmd) { $VsDevCmd = $env:VCVARS64 }
    if (-not (Test-Path -LiteralPath $VsDevCmd -PathType Leaf)) {
        throw 'Supply -VsDevCmd or set VCVARS64 to a Visual Studio x64 environment script.'
    }
    $taskVcpkgRoot = $env:VCPKG_ROOT
    $developerEnvironment = & $env:ComSpec /d /s /c "`"$VsDevCmd`" >nul && set"
    foreach ($line in $developerEnvironment) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
        }
    }
    if ($taskVcpkgRoot) { $env:VCPKG_ROOT = $taskVcpkgRoot }
}

if (-not $CMakeExe) { $CMakeExe = (Get-Command cmake.exe -ErrorAction Stop).Source }
if (-not $NinjaExe) { $NinjaExe = (Get-Command ninja.exe -ErrorAction Stop).Source }

Push-Location $PSScriptRoot
try {
    & $CMakeExe --preset $configurePreset -S $PSScriptRoot "-DCMAKE_MAKE_PROGRAM=$NinjaExe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $CMakeExe --build --preset $buildPreset --verbose
    exit $LASTEXITCODE
} finally {
    Pop-Location
}

