# Retire le plugin du dossier de plugins d'OBS.
$ErrorActionPreference = 'Stop'

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
	throw "OBS est ouvert : fermez-le avant de désinstaller."
}
Remove-Item -Recurse -Force 'C:\ProgramData\obs-studio\plugins\obs-nsfw-guard'
Write-Host "Plugin désinstallé."
