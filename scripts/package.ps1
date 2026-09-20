# Crée dist/obs-nsfw-guard-<version>-windows-x64.zip à partir de stage/.
# Le zip s'extrait dans C:\ProgramData\obs-studio\plugins.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(obs-nsfw-guard VERSION ([\d.]+)').Matches[0].Groups[1].Value
$stage = Join-Path $root 'stage\obs-nsfw-guard'
if (-not (Test-Path (Join-Path $stage 'bin\64bit\obs-nsfw-guard.dll'))) { throw "lancez d'abord scripts\build.ps1" }

# Licences à distribuer avec les binaires
Copy-Item (Join-Path $root 'LICENSE') $stage -Force
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') $stage -Force

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$zip = Join-Path $dist "obs-nsfw-guard-$version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "Archive : $zip"
