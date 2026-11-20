[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',

    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( -not [System.Environment]::Is64BitOperatingSystem ) {
    throw "A 64-bit system is required to build the project."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The build script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

# OBS Studio 33 is built with Visual Studio 2026; Visual Studio 2022 produces
# ABI-compatible binaries and is used as a fallback (e.g. older CI images).
function Get-VisualStudioGenerator {
    $VsWhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    if ( -not (Test-Path $VsWhere) ) { return $null }

    $Versions = & $VsWhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
    $Majors = $Versions | ForEach-Object { [int]($_ -split '\.')[0] } | Sort-Object -Descending -Unique

    foreach ( $Major in $Majors ) {
        switch ( $Major ) {
            18 { return 'Visual Studio 18 2026' }
            17 { return 'Visual Studio 17 2022' }
        }
    }
    return $null
}

function Build {
    trap {
        Pop-Location -Stack BuildTemp -ErrorAction 'SilentlyContinue'
        Write-Error $_
        Log-Group
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

    Push-Location -Stack BuildTemp
    Ensure-Location $ProjectRoot

    # CI builds treat warnings as errors; local builds use the regular preset.
    $ConfigurePreset = if ( $env:CI ) { "windows-ci-${Target}" } else { "windows-${Target}" }
    $BuildPreset = "windows-${Target}"

    $CmakeArgs = @('--preset', $ConfigurePreset)
    $Generator = Get-VisualStudioGenerator
    if ( $Generator ) {
        Write-Host "Using generator: ${Generator}"
        $CmakeArgs += @('-G', $Generator)
    }

    $CmakeBuildArgs = @('--build')
    $CmakeInstallArgs = @()

    if ( $DebugPreference -eq 'Continue' ) {
        $CmakeArgs += '--debug-output'
        $CmakeBuildArgs += '--verbose'
        $CmakeInstallArgs += '--verbose'
    }

    $CmakeBuildArgs += @(
        '--preset', $BuildPreset,
        '--config', $Configuration,
        '--parallel',
        '--', '/consoleLoggerParameters:Summary', '/noLogo'
    )

    $InstallPrefix = "${ProjectRoot}/release/${Configuration}"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $InstallPrefix

    $CmakeInstallArgs += @(
        '--install', "build_${Target}",
        '--prefix', $InstallPrefix,
        '--config', $Configuration
    )

    Log-Group "Configuring ${ProductName}..."
    Invoke-External cmake @CmakeArgs

    Log-Group "Building ${ProductName}..."
    Invoke-External cmake @CmakeBuildArgs

    Log-Group "Installing ${ProductName}..."
    Invoke-External cmake @CmakeInstallArgs

    Log-Group

    Pop-Location -Stack BuildTemp

    Write-Host "Build complete:"
    Write-Host "  - Installed to: $InstallPrefix/${ProductName}"
}

Build
