# Récupère les dépendances de compilation (deps/) et le modèle (models/).
# À lancer une fois avant la compilation. Idempotent : ne retélécharge pas ce qui existe.
#
# Prérequis : git, Visual Studio (ou Build Tools) avec la charge de travail C++,
# et OBS Studio installé (on en tire obs.lib depuis obs.dll).
param(
	[string]$ObsInstall = "C:\Program Files\obs-studio"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$deps = Join-Path $root 'deps'
$models = Join-Path $root 'models'

# Versions épinglées
$ObsVersion = '32.2.2'
$OrtVersion = '1.24.4'  # Microsoft.ML.OnnxRuntime.DirectML
$ModelRepo = 'taufiqdp/mobilenetv4_conv_small.e2400_r224_in1k_nsfw_classifier'

New-Item -ItemType Directory -Force $deps, $models | Out-Null

function Get-NuGetPackage($id, $version, $dest) {
	if (Test-Path $dest) { return }
	$zip = Join-Path $deps "$id.zip"
	$url = "https://api.nuget.org/v3-flatcontainer/$($id.ToLower())/$version/$($id.ToLower()).$version.nupkg"
	Write-Host "Téléchargement de $id $version"
	Invoke-WebRequest $url -OutFile $zip
	Expand-Archive $zip -DestinationPath $dest -Force
	Remove-Item $zip
}

# 1) En-têtes de libobs
$obsSrc = Join-Path $deps 'obs-src'
if (-not (Test-Path (Join-Path $obsSrc 'libobs\obs.h'))) {
	Write-Host "Clonage des en-têtes OBS $ObsVersion"
	git clone --depth 1 --branch $ObsVersion https://github.com/obsproject/obs-studio.git $obsSrc
	if ($LASTEXITCODE -ne 0) { throw "git clone a échoué" }
}

# 2) obs.lib, générée depuis l'obs.dll installé
$obsLib = Join-Path $deps 'obs.lib'
if (-not (Test-Path $obsLib)) {
	$obsDll = Join-Path $ObsInstall 'bin\64bit\obs.dll'
	if (-not (Test-Path $obsDll)) { throw "obs.dll introuvable : $obsDll (utilisez -ObsInstall)" }

	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
	if (-not $vs) { throw "Visual Studio avec les outils C++ est introuvable" }
	$msvcBin = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') | Sort-Object Name | Select-Object -Last 1
	$bin = Join-Path $msvcBin.FullName 'bin\Hostx64\x64'

	Write-Host "Génération de obs.lib depuis $obsDll"
	$exports = & (Join-Path $bin 'dumpbin.exe') /exports $obsDll
	$names = $exports | Where-Object { $_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)' } | ForEach-Object { $Matches[1] }
	if ($names.Count -lt 100) { throw "export de obs.dll inattendu ($($names.Count) symboles)" }
	$def = Join-Path $deps 'obs.def'
	("LIBRARY obs`r`nEXPORTS`r`n" + ($names -join "`r`n")) | Set-Content $def -Encoding ascii
	& (Join-Path $bin 'lib.exe') /nologo /def:$def /machine:x64 /out:$obsLib | Out-Null
}

# 3) ONNX Runtime avec le fournisseur DirectML (DirectML.dll vient de Windows)
Get-NuGetPackage 'Microsoft.ML.OnnxRuntime.DirectML' $OrtVersion (Join-Path $deps 'ort')

# 4) Modèle NSFW (MobileNetV4 small, licence Apache-2.0)
foreach ($f in 'mobilenetv4_conv_small.e2400_r224_in1k_nsfw_classifier.onnx', 'config.json') {
	$out = Join-Path $models $f
	if (-not (Test-Path $out)) {
		Write-Host "Téléchargement du modèle : $f"
		Invoke-WebRequest "https://huggingface.co/$ModelRepo/resolve/main/$f" -OutFile $out
	}
}

Write-Host "Dépendances prêtes."
