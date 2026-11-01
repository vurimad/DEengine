# Loads the Visual Studio x64 developer environment into the current PowerShell session,
# so cmake/ninja/cl/clang-cl work from a plain terminal.
#
# Usage (dot-source it):   . tools/dev_env.ps1
#
# The VS developer shell overwrites VCPKG_ROOT with Visual Studio's bundled vcpkg;
# this script keeps the standalone vcpkg that VCPKG_ROOT pointed to before.

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'vswhere.exe not found: install Visual Studio 2022 or newer with the C++ workload.'
}

$vsPath = & $vswhere -latest -prerelease -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw 'No Visual Studio installation with the MSVC x64 tools was found.'
}

$keepVcpkgRoot = $env:VCPKG_ROOT

# VsDevCmd calls vswhere by name; put it on PATH so it doesn't print "not recognized".
$env:Path = (Split-Path $vswhere) + ';' + $env:Path

Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll') -ErrorAction Stop
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' `
    -ErrorAction Stop | Out-Null

if ($keepVcpkgRoot) {
    $env:VCPKG_ROOT = $keepVcpkgRoot
} else {
    Write-Warning 'VCPKG_ROOT was not set before; using the Visual Studio bundled vcpkg.'
}

Write-Host "VS: $vsPath"
Write-Host "VCPKG_ROOT: $env:VCPKG_ROOT"
