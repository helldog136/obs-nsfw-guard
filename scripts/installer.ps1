# Crée dist/obs-nsfw-guard-<version>-windows-x64-setup.exe avec Inno Setup.
# À lancer après scripts\build.ps1 et scripts\package.ps1 (package.ps1 ajoute les licences à stage/).
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(obs-nsfw-guard VERSION ([\d.]+)').Matches[0].Groups[1].Value

$iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
	$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
		Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $iscc) { throw "Inno Setup 6 introuvable (https://jrsoftware.org/isinfo.php)" }

if (-not (Test-Path (Join-Path $root 'stage\obs-nsfw-guard\LICENSE'))) { throw "lancez d'abord scripts\build.ps1 puis scripts\package.ps1" }

& $iscc "/DAppVersion=$version" (Join-Path $root 'installer\nsfw-guard.iss')
if ($LASTEXITCODE -ne 0) { throw "compilation de l'installeur échouée" }
Write-Host "Installeur : $(Join-Path $root "dist\obs-nsfw-guard-$version-windows-x64-setup.exe")"
