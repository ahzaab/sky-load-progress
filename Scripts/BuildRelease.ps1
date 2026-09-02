[CmdletBinding()]
param(
    [string]$Version,
    [string]$CMakeExe,
    [string]$VsDevCmd,
    [string]$NinjaExe,
    [string]$OutputDirectory,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# Use the CMake project version by default so the archive name and DLL metadata cannot drift apart.
if (-not $Version)
{
    $cmakeText = Get-Content -LiteralPath (Join-Path $repositoryRoot 'CMakeLists.txt') -Raw
    $match = [regex]::Match($cmakeText, '(?ms)project\s*\(\s*SkyrimLoadProgress\s+VERSION\s+(\d+\.\d+\.\d+(?:\.\d+)?)')
    if (-not $match.Success)
    {
        throw 'Could not read the project version from CMakeLists.txt.'
    }

    $Version = $match.Groups[1].Value
}

if ($Version -notmatch '^\d+\.\d+\.\d+(?:\.\d+)?$')
{
    throw "Invalid three- or four-part version: $Version"
}

if (-not $OutputDirectory)
{
    $OutputDirectory = Join-Path $repositoryRoot "release\$Version"
}

if (-not $SkipBuild)
{
    $buildArguments = @{ Configuration = 'Release' }
    if ($CMakeExe) { $buildArguments.CMakeExe = $CMakeExe }
    if ($VsDevCmd) { $buildArguments.VsDevCmd = $VsDevCmd }
    if ($NinjaExe) { $buildArguments.NinjaExe = $NinjaExe }

    & (Join-Path $repositoryRoot 'build.ps1') @buildArguments
    if ($LASTEXITCODE -ne 0)
    {
        throw "Release build failed with exit code $LASTEXITCODE."
    }
}

$pluginDll = Join-Path $repositoryRoot 'build\release-msvc\SkyrimLoadProgress.dll'
$pluginPdb = Join-Path $repositoryRoot 'build\release-msvc\SkyrimLoadProgress.pdb'
$sourceToml = Join-Path $repositoryRoot 'dist\SKSE\Plugins\SkyrimLoadProgress.toml'
$sourceInterface = Join-Path $repositoryRoot 'dist\Interface'
$requiredFiles = @(
    $pluginDll,
    $pluginPdb,
    $sourceToml,
    (Join-Path $sourceInterface 'SkyrimLoadProgress\LoadingProgressMeter.swf'),
    (Join-Path $sourceInterface 'Exported\SkyrimLoadProgress\LoadingProgressMeter.swf')
)

foreach ($requiredFile in $requiredFiles)
{
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf))
    {
        throw "Required release input is missing: $requiredFile"
    }
}

$versionParts = @($Version.Split('.') | ForEach-Object { [int]$_ })
while ($versionParts.Count -lt 4)
{
    $versionParts += 0
}
$normalizedVersion = $versionParts -join '.'

$dllVersion = (Get-Item -LiteralPath $pluginDll).VersionInfo.FileVersion
if ($dllVersion -and $dllVersion -ne $Version -and $dllVersion -ne $normalizedVersion)
{
    throw "Release DLL version is $dllVersion, but package version is $Version."
}

# CommonLib embeds the authoritative SKSE plugin version in generated source. This project does not
# currently add a Windows VERSIONINFO resource, so validate the generated source and header as well.
$expectedPluginVersion = 'REL::Version{ ' + ($versionParts -join ', ') + ' }'
$generatedPluginSource = Join-Path $repositoryRoot 'build\release-msvc\__SkyrimLoadProgressPlugin.cpp'
$generatedVersionHeader = Join-Path $repositoryRoot 'build\release-msvc\include\Version.h'
foreach ($generatedFile in @($generatedPluginSource, $generatedVersionHeader))
{
    if (-not (Test-Path -LiteralPath $generatedFile -PathType Leaf))
    {
        throw "Generated version input is missing: $generatedFile"
    }
}

if (-not (Get-Content -LiteralPath $generatedPluginSource -Raw).Contains($expectedPluginVersion))
{
    throw "Generated SKSE plugin metadata does not contain version $Version."
}
if (-not (Get-Content -LiteralPath $generatedVersionHeader -Raw).Contains('"' + $Version + '"'))
{
    throw "Generated version header does not contain version $Version."
}

$tempDirectory = Join-Path ([IO.Path]::GetTempPath()) ("skyrim-load-progress-release-" + [guid]::NewGuid().ToString('N'))
$packageDataDirectory = Join-Path $tempDirectory 'Data'
$packagePluginDirectory = Join-Path $packageDataDirectory 'SKSE\Plugins'
New-Item -ItemType Directory -Path $packagePluginDirectory -Force | Out-Null

try
{
    Copy-Item -LiteralPath $pluginDll -Destination (Join-Path $packagePluginDirectory 'SkyrimLoadProgress.dll') -Force
    Copy-Item -LiteralPath $pluginPdb -Destination (Join-Path $packagePluginDirectory 'SkyrimLoadProgress.pdb') -Force
    Copy-Item -LiteralPath $sourceToml -Destination (Join-Path $packagePluginDirectory 'SkyrimLoadProgress.toml') -Force
    Copy-Item -LiteralPath $sourceInterface -Destination $packageDataDirectory -Recurse -Force

    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $archiveName = 'SkyrimLoadProgress-' + $Version.Replace('.', '_') + '.zip'
    $releaseArchive = Join-Path $OutputDirectory $archiveName

    # Replace only this exact generated archive; the version directory may contain user notes.
    if (Test-Path -LiteralPath $releaseArchive -PathType Leaf)
    {
        Remove-Item -LiteralPath $releaseArchive -Force
    }

    Compress-Archive -LiteralPath $packageDataDirectory -DestinationPath $releaseArchive -CompressionLevel Optimal
    if (-not (Test-Path -LiteralPath $releaseArchive -PathType Leaf))
    {
        throw "Compress-Archive did not create $releaseArchive."
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($releaseArchive)
    try
    {
        $entries = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
        $unexpectedTopLevelEntries = @($entries | Where-Object {
            $_ -ne 'Data/' -and -not $_.StartsWith('Data/')
        })
        if ($unexpectedTopLevelEntries.Count -gt 0)
        {
            throw "Release archive contains entries outside the top-level Data directory: $($unexpectedTopLevelEntries -join ', ')"
        }

        $requiredEntries = @(
            'Data/SKSE/Plugins/SkyrimLoadProgress.dll',
            'Data/SKSE/Plugins/SkyrimLoadProgress.pdb',
            'Data/SKSE/Plugins/SkyrimLoadProgress.toml',
            'Data/Interface/SkyrimLoadProgress/LoadingProgressMeter.swf',
            'Data/Interface/Exported/SkyrimLoadProgress/LoadingProgressMeter.swf'
        )

        foreach ($requiredEntry in $requiredEntries)
        {
            if ($requiredEntry -notin $entries)
            {
                throw "Release archive is missing required entry: $requiredEntry"
            }
        }
    }
    finally
    {
        $zip.Dispose()
    }

    $archiveItem = Get-Item -LiteralPath $releaseArchive
    $archiveHash = Get-FileHash -LiteralPath $releaseArchive -Algorithm SHA256
    [pscustomobject]@{
        Version = $Version
        Path = $archiveItem.FullName
        Size = $archiveItem.Length
        SHA256 = $archiveHash.Hash
    }
}
finally
{
    # Always remove the unique staging tree, including after build-input or ZIP validation failures.
    if (Test-Path -LiteralPath $tempDirectory)
    {
        Remove-Item -LiteralPath $tempDirectory -Recurse -Force
    }
}
