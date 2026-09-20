# Installe le plugin compilé (stage/) dans le dossier de plugins d'OBS.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
	throw "OBS est ouvert : fermez-le avant d'installer (les DLL sont verrouillées)."
}

$src = Join-Path $root 'stage\obs-nsfw-guard'
$dst = 'C:\ProgramData\obs-studio\plugins\obs-nsfw-guard'
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item "$src\*" $dst -Recurse -Force
Write-Host "Installé dans $dst"
