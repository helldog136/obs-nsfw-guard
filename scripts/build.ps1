# Compile le plugin avec le Visual Studio le plus récent installé.
param(
	[string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$version = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
if (-not $vs) { throw "Visual Studio avec les outils C++ est introuvable" }

$major = [int]($version.Split('.')[0])
$generator = switch ($major) {
	18 { 'Visual Studio 18 2026' }
	17 { 'Visual Studio 17 2022' }
	default { throw "Version de Visual Studio non prise en charge : $version" }
}

# CMake fourni avec Visual Studio, à défaut celui du PATH (>= 3.28 requis).
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = 'cmake' }

Write-Host "Générateur : $generator"
& $cmake -S $root -B (Join-Path $root 'build') -G $generator -A x64
if ($LASTEXITCODE -ne 0) { throw "configuration CMake échouée" }
& $cmake --build (Join-Path $root 'build') --config $Config
if ($LASTEXITCODE -ne 0) { throw "compilation échouée" }

Write-Host "Plugin prêt : $(Join-Path $root 'stage\obs-nsfw-guard')"
