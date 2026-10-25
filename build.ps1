# Builds bin\ReunionDLSS.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# MSVC is required: ReShade is built with it, and MinGW returns API structs differently.
# Usage: .\build.ps1 [-Install] [-GameBin <folder of Iris-Win64-Shipping.exe>]
param(
    [switch]$Install,
    [string]$GameBin = "E:\SteamLibrary\steamapps\common\LifeisStrangeReunion\Iris\Binaries\Win64"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = "Iris-Win64-Shipping.exe"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars64.bat runs vswhere by name
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$addon = "$root\bin\ReunionDLSS.addon64"

# ReShade add-on headers in third_party\reshade match ReShade 6.8.0.
$cmd = @(
    "call `"$vcvars`" >nul",
    "fxc /nologo /T cs_5_1 /E main /O3 /Vn kMotionCs /Fh `"$obj\motion_cs.h`" `"$root\src\motion.hlsl`" >nul",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$obj`" /I `"$root\third_party\imgui`" /I`"$root\third_party\reshade\include`" /I `"$root\third_party\dlss\include`" /I `"$root\third_party\streamline\include`" " +
     "/Fo`"$obj\\`" `"$root\src\main.cpp`" `"$root\third_party\reshade\utils\descriptor_tracking.cpp`" `"$root\src\upscaler.cpp`" `"$root\src\dlss.cpp`" `"$root\src\framegen.cpp`" user32.lib advapi32.lib ole32.lib d3d12.lib dxgi.lib `"$root\third_party\dlss\lib\nvsdk_ngx_s.lib`" " +
     "/link /DLL /OUT:`"$addon`" /PDB:`"$obj\ReunionDLSS.pdb`" /DEBUG")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "Built $addon"

if ($Install) {
    if (-not (Test-Path "$GameBin\$exe")) { throw "$exe not found in $GameBin (pass -GameBin)" }
    if (Get-Process ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Item $addon $GameBin -Force
    # Streamline 2.14.16 (NVIDIA-signed) for frame generation: Win64\streamline\
    $sl = "$root\third_party\streamline\bin\x64"
    New-Item -ItemType Directory -Force "$GameBin\streamline" | Out-Null
    foreach ($f in "sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll", "nvngx_dlssg.dll") {
        Copy-Item "$sl\$f" "$GameBin\streamline" -Force
    }
    Copy-Item "$root\third_party\streamline\license.txt" "$GameBin\streamline\streamline.license.txt" -Force
    Copy-Item "$sl\reflex.license.txt" "$GameBin\streamline" -Force
    Write-Host "Installed to $GameBin"
}
