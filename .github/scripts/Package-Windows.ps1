[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',

    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',

    # Also build the .exe installer (requires Inno Setup 6)
    [switch] $Installer
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Find-InnoSetup {
    $Command = Get-Command iscc.exe -ErrorAction SilentlyContinue
    if ( $Command ) { return $Command.Source }
    foreach ( $Root in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, "$env:LOCALAPPDATA\Programs") ) {
        if ( -not $Root ) { continue }
        $Candidate = Join-Path $Root 'Inno Setup 6\ISCC.exe'
        if ( Test-Path $Candidate ) { return $Candidate }
    }
    return $null
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."

    $UtilityFunctions = Get-ChildItem -Path "$ScriptHome/utils.pwsh/*.ps1" -Recurse
    foreach ($Utility in $UtilityFunctions) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path (Join-Path $ProjectRoot 'buildspec.json') -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version
    if ( -not $ProductName -or -not $ProductVersion ) {
        throw "buildspec.json must contain 'name' and 'version'."
    }

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"
    $ReleaseDir = Join-Path $ProjectRoot 'release'

    # OBS 33+ layout produced by `cmake --install`:
    #   release/<Config>/<plugin>/<plugin>.dll
    #   release/<Config>/<plugin>/data/...
    $PluginDir = Join-Path $ReleaseDir "${Configuration}/${ProductName}"
    if ( -not (Test-Path (Join-Path $PluginDir "${ProductName}.dll")) ) {
        throw "Plugin not found at ${PluginDir}. Run Build-Windows.ps1 first."
    }
    if ( Test-Path (Join-Path $PluginDir 'bin') ) {
        throw "Legacy 'bin/64bit' layout found in ${PluginDir}; OBS 33+ expects ${ProductName}.dll at the plugin folder root."
    }

    Remove-Item -ErrorAction SilentlyContinue -Path "${ReleaseDir}/${ProductName}-*-windows-*.zip", "${ReleaseDir}/${ProductName}-*-windows-*.exe"

    Log-Group "Archiving ${ProductName} (ZIP)..."
    $Staging = Join-Path $ReleaseDir "zip-staging"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $Staging
    New-Item -ItemType Directory -Force -Path $Staging | Out-Null
    Copy-Item -Recurse -Path $PluginDir -Destination $Staging
    @"
$($BuildSpec.displayName) ${ProductVersion} for OBS Studio 33+ (Windows x64)

Install
  1. Close OBS Studio.
  2. Copy the '${ProductName}' folder into:
       C:\ProgramData\obs-studio\plugins\
     so that this file exists:
       C:\ProgramData\obs-studio\plugins\${ProductName}\${ProductName}.dll
  3. Start OBS Studio and open Tools > Audio Stems Recorder...

Upgrading from 1.x: delete the old folder
  C:\ProgramData\obs-studio\plugins\${ProductName}\bin
and, if present,
  C:\Program Files\obs-studio\obs-plugins\64bit\${ProductName}.dll
(or simply use the .exe installer, which does this for you).

Portable OBS: copy the folder into <portable OBS>\plugins\ instead.
"@ | Set-Content -Encoding utf8 -Path (Join-Path $Staging 'INSTALL.txt')

    $CompressArgs = @{
        Path             = (Get-ChildItem -Path $Staging)
        CompressionLevel = 'Optimal'
        DestinationPath  = "${ReleaseDir}/${OutputName}.zip"
        Verbose          = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Remove-Item -Recurse -Force $Staging
    Log-Group

    if ( $Installer ) {
        Log-Group "Building ${ProductName} installer..."
        $Iscc = Find-InnoSetup
        if ( -not $Iscc ) {
            throw 'Inno Setup 6 (ISCC.exe) was not found; install it or omit -Installer.'
        }
        $IssArgs = @(
            "/DProductName=${ProductName}",
            "/DDisplayName=$($BuildSpec.displayName)",
            "/DProductVersion=${ProductVersion}",
            "/DAuthor=$($BuildSpec.author)",
            "/DWebsite=$($BuildSpec.website)",
            "/DSourceDir=$((Resolve-Path $PluginDir).Path)",
            "/DOutputDir=$((Resolve-Path $ReleaseDir).Path)",
            "/DOutputName=${OutputName}-installer",
            '/Qp',
            (Join-Path $ScriptHome 'windows/installer.iss')
        )
        Invoke-External $Iscc @IssArgs
        Log-Group
    }

    Write-Host "Packaging complete:"
    Get-ChildItem -Path "${ReleaseDir}/${OutputName}*" | ForEach-Object { Write-Host "  - $($_.FullName)" }
}

Package
