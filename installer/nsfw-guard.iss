; Installeur Inno Setup du plugin OBS "NSFW Guard".
; Compilation : scripts\installer.ps1 (après scripts\build.ps1 et scripts\package.ps1).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

; Runtime Visual C++ (x64) embarqué : téléchargé par scripts\installer.ps1. Il n'est installé
; que si celui de la machine est absent ou plus ancien (le plugin et ONNX Runtime en dépendent).
#define VCRedist "..\redist\vc_redist.x64.exe"
#if !FileExists(AddBackslash(SourcePath) + VCRedist)
  #error "redist\vc_redist.x64.exe manquant : lancez scripts\installer.ps1"
#endif
#define VCMajor 0
#define VCMinor 0
#define VCRev 0
#define VCBuild 0
#expr GetVersionComponents(AddBackslash(SourcePath) + VCRedist, VCMajor, VCMinor, VCRev, VCBuild)

[Setup]
AppId={{6B0F3B1A-7C52-4E0B-9A0D-2E3F5A9C41D7}
AppName=NSFW Guard (plugin OBS)
AppVersion={#AppVersion}
AppVerName=NSFW Guard {#AppVersion}
AppPublisher=Helldog136
AppPublisherURL=https://github.com/helldog136/obs-nsfw-guard
AppSupportURL=https://github.com/helldog136/obs-nsfw-guard/issues
AppUpdatesURL=https://github.com/helldog136/obs-nsfw-guard/releases
VersionInfoVersion={#AppVersion}

; OBS 32 lit les plugins utilisateur dans %ProgramData%\obs-studio\plugins.
DefaultDirName={commonappdata}\obs-studio\plugins\obs-nsfw-guard
DisableDirPage=yes
DisableProgramGroupPage=yes
DefaultGroupName=NSFW Guard
UsePreviousAppDir=no

; Administrateur par défaut ; /CURRENTUSER en ligne de commande pour les tests.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=obs-nsfw-guard-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=NSFW Guard (plugin OBS)
UninstallDisplayIcon={uninstallexe}
; Mise à jour : une nouvelle version s'installe par-dessus l'ancienne (même AppId) ; le
; désinstalleur reste unique et à jour.

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
french.UpdateFrom=Mise à jour de NSFW Guard : version %1 → %2
english.UpdateFrom=NSFW Guard update: version %1 → %2
french.Reinstall=Réinstallation de NSFW Guard %1
english.Reinstall=Reinstalling NSFW Guard %1
french.Downgrade=Une version plus récente de NSFW Guard (%1) est déjà installée. Installer quand même la version %2, plus ancienne ?
english.Downgrade=A newer version of NSFW Guard (%1) is already installed. Install the older version %2 anyway?
french.WaitingObs=Attente de la fermeture d'OBS Studio...
english.WaitingObs=Waiting for OBS Studio to close...
french.UninstallLink=Désinstaller NSFW Guard
english.UninstallLink=Uninstall NSFW Guard
french.ReleasesLink=NSFW Guard - versions et mises à jour
english.ReleasesLink=NSFW Guard - releases and updates
french.DeleteUserData=Supprimer aussi vos données NSFW Guard (modèles NudeNet téléchargés, préférences de mise à jour) ?%n%n%1%n%nLes réglages des filtres, enregistrés dans vos scènes OBS, ne sont pas touchés.
english.DeleteUserData=Also delete your NSFW Guard data (downloaded NudeNet models, update preferences)?%n%n%1%n%nFilter settings, saved in your OBS scenes, are not affected.
french.VCRedistInstall=Installation du runtime Visual C++ (Microsoft)...
english.VCRedistInstall=Installing the Visual C++ runtime (Microsoft)...
french.ObsTooOld=Votre version d'OBS Studio (%1) est trop ancienne : NSFW Guard demande OBS 30 ou plus récent (testé avec OBS 32). Mettez OBS à jour.%n%nInstaller quand même ?
english.ObsTooOld=Your OBS Studio version (%1) is too old: NSFW Guard requires OBS 30 or newer (tested with OBS 32). Please update OBS.%n%nInstall anyway?
french.ObsRunning=OBS Studio est ouvert. Fermez-le, puis cliquez sur « Précédent » et de nouveau sur « Suivant ».
english.ObsRunning=OBS Studio is running. Close it, then click Back and Next again.
french.ObsNotFound=OBS Studio n'a pas été trouvé (ni installation standard ni Steam). Le plugin sera installé quand même dans %1, où toute installation d'OBS (non portable) le trouvera.%n%nAttention : une version portable d'OBS ne lit pas ce dossier.%n%nContinuer ?
english.ObsNotFound=OBS Studio was not found (neither a standard nor a Steam installation). The plugin will be installed anyway in %1, where any (non-portable) OBS installation will find it.%n%nNote: a portable OBS does not read this folder.%n%nContinue?
french.UninstallObsRunning=OBS Studio est ouvert. Fermez-le avant de désinstaller NSFW Guard.
english.UninstallObsRunning=OBS Studio is running. Close it before uninstalling NSFW Guard.
french.Done=Relancez OBS, puis ajoutez le filtre « NSFW Guard » à une source (clic droit > Filtres > +).
english.Done=Restart OBS, then add the "NSFW Guard" filter to a source (right-click > Filters > +).

[Files]
Source: "..\stage\obs-nsfw-guard\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#VCRedist}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: VCRedistNeeded

[InstallDelete]
; Mise à jour : on retire les fichiers de l'ancienne version avant de copier les nouveaux, pour
; ne laisser aucun fichier obsolète (les modèles et préférences de l'utilisateur sont ailleurs).
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"

[Icons]
Name: "{group}\{cm:UninstallLink}"; Filename: "{uninstallexe}"

[INI]
Filename: "{group}\{cm:ReleasesLink}.url"; Section: "InternetShortcut"; Key: "URL"; \
  String: "https://github.com/helldog136/obs-nsfw-guard/releases/latest"

[UninstallDelete]
Type: files; Name: "{group}\{cm:ReleasesLink}.url"
Type: dirifempty; Name: "{group}"
Type: dirifempty; Name: "{app}"

[Run]
; Codes de retour ignorés : 1638 (version plus récente déjà présente) et 3010 (redémarrage
; conseillé) ne doivent pas faire échouer l'installation du plugin.
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; \
  StatusMsg: "{cm:VCRedistInstall}"; Check: VCRedistNeeded; Flags: waituntilterminated

[Code]
// Version du runtime Visual C++ x64 installé (registre), 0 s'il est absent.
function InstalledVCVersion(): Int64;
var
  Key: String;
  Installed, Major, Minor, Bld: Cardinal;
begin
  Result := 0;
  Key := 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  if not RegQueryDWordValue(HKLM64, Key, 'Installed', Installed) then
    if not RegQueryDWordValue(HKLM32, Key, 'Installed', Installed) then
      exit;
  if Installed <> 1 then
    exit;
  if not (RegQueryDWordValue(HKLM64, Key, 'Major', Major) and RegQueryDWordValue(HKLM64, Key, 'Minor', Minor) and
          RegQueryDWordValue(HKLM64, Key, 'Bld', Bld)) then
    if not (RegQueryDWordValue(HKLM32, Key, 'Major', Major) and RegQueryDWordValue(HKLM32, Key, 'Minor', Minor) and
            RegQueryDWordValue(HKLM32, Key, 'Bld', Bld)) then
      exit;
  Result := (Int64(Major) * 1000 + Minor) * 1000000 + Bld;
end;

function VCRedistNeeded(): Boolean;
begin
  Result := InstalledVCVersion() < (Int64({#VCMajor}) * 1000 + {#VCMinor}) * 1000000 + {#VCRev};
end;

// Dossier d'installation d'OBS (installeur officiel, puis Steam), vide s'il est introuvable.
function ObsDir(): String;
var
  Steam: String;
begin
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Result) and FileExists(Result + '\bin\64bit\obs64.exe') then
    exit;
  if RegQueryStringValue(HKLM32, 'SOFTWARE\OBS Studio', '', Result) and FileExists(Result + '\bin\64bit\obs64.exe') then
    exit;
  Result := ExpandConstant('{commonpf64}\obs-studio');
  if FileExists(Result + '\bin\64bit\obs64.exe') then
    exit;
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', Steam) then
  begin
    StringChangeEx(Steam, '/', '\', True);
    Result := Steam + '\steamapps\common\OBS Studio';
    if FileExists(Result + '\bin\64bit\obs64.exe') then
      exit;
  end;
  Result := '';
end;

// Version déjà installée (clé de désinstallation écrite par Inno Setup), vide sinon.
function InstalledVersion(): String;
var
  Key: String;
begin
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6B0F3B1A-7C52-4E0B-9A0D-2E3F5A9C41D7}_is1';
  if not RegQueryStringValue(HKLM64, Key, 'DisplayVersion', Result) then
    if not RegQueryStringValue(HKLM32, Key, 'DisplayVersion', Result) then
      if not RegQueryStringValue(HKCU, Key, 'DisplayVersion', Result) then
        Result := '';
end;

// Compare deux versions "a.b.c" : -1, 0 ou 1.
function CompareVersions(A, B: String): Integer;
var
  I, PA, PB, NA, NB: Integer;
begin
  Result := 0;
  for I := 1 to 3 do
  begin
    PA := Pos('.', A);
    PB := Pos('.', B);
    if PA = 0 then begin NA := StrToIntDef(A, 0); A := ''; end
    else begin NA := StrToIntDef(Copy(A, 1, PA - 1), 0); Delete(A, 1, PA); end;
    if PB = 0 then begin NB := StrToIntDef(B, 0); B := ''; end
    else begin NB := StrToIntDef(Copy(B, 1, PB - 1), 0); Delete(B, 1, PB); end;
    if NA < NB then begin Result := -1; exit; end;
    if NA > NB then begin Result := 1; exit; end;
  end;
end;

// /WAITOBS : lancé par le plugin à la fermeture d'OBS ; on attend qu'OBS ait fini de se fermer.
function WaitObsRequested(): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/WAITOBS') = 0 then
      Result := True;
end;

function IsObsRunning(): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'),
    '/C tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe" >nul',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function InitializeSetup(): Boolean;
var
  Dir, Version, Installed: String;
  Major: Integer;
begin
  Result := True;
  Installed := InstalledVersion();
  if Installed <> '' then
  begin
    Log('Version installée : ' + Installed);
    if CompareVersions(Installed, '{#AppVersion}') > 0 then
      if SuppressibleMsgBox(FmtMessage(CustomMessage('Downgrade'), [Installed, '{#AppVersion}']),
          mbConfirmation, MB_YESNO, IDNO) <> IDYES then
      begin
        Result := False;
        exit;
      end;
  end;
  Dir := ObsDir();
  if Dir = '' then
  begin
    Result := SuppressibleMsgBox(
      FmtMessage(CustomMessage('ObsNotFound'), [ExpandConstant('{commonappdata}\obs-studio\plugins')]),
      mbConfirmation, MB_YESNO, IDYES) = IDYES;
    exit;
  end;
  Log('OBS Studio : ' + Dir);
  if GetVersionNumbersString(Dir + '\bin\64bit\obs64.exe', Version) then
  begin
    Log('Version d''OBS : ' + Version);
    Major := StrToIntDef(Copy(Version, 1, Pos('.', Version) - 1), 0);
    if (Major > 0) and (Major < 30) then
      Result := SuppressibleMsgBox(FmtMessage(CustomMessage('ObsTooOld'), [Version]),
        mbConfirmation, MB_YESNO, IDNO) = IDYES;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  I: Integer;
begin
  Result := '';
  if WaitObsRequested() then
  begin
    Log(CustomMessage('WaitingObs'));
    for I := 1 to 120 do
    begin
      if not IsObsRunning() then
        break;
      Sleep(1000);
    end;
  end;
  if IsObsRunning() then
    Result := CustomMessage('ObsRunning');
end;

// Page « Prêt à installer » : indique s'il s'agit d'une mise à jour.
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Installed: String;
begin
  Installed := InstalledVersion();
  if Installed = '' then
    Result := ''
  else if CompareVersions(Installed, '{#AppVersion}') = 0 then
    Result := FmtMessage(CustomMessage('Reinstall'), [Installed]) + NewLine + NewLine
  else
    Result := FmtMessage(CustomMessage('UpdateFrom'), [Installed, '{#AppVersion}']) + NewLine + NewLine;
  Result := Result + MemoDirInfo;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  if IsObsRunning() then
  begin
    MsgBox(CustomMessage('UninstallObsRunning'), mbError, MB_OK);
    Result := False;
  end;
end;

// Désinstallation : les installeurs de mise à jour téléchargés sont toujours supprimés ; le
// reste des données de l'utilisateur (modèles NudeNet...) seulement s'il le demande.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep <> usPostUninstall then
    exit;
  DataDir := ExpandConstant('{userappdata}\obs-studio\plugin_config\obs-nsfw-guard');
  DelTree(DataDir + '\update', True, True, True);
  if DirExists(DataDir) then
    if SuppressibleMsgBox(FmtMessage(CustomMessage('DeleteUserData'), [DataDir]),
        mbConfirmation, MB_YESNO, IDNO) = IDYES then
      DelTree(DataDir, True, True, True);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and not WizardSilent() then
    Log(CustomMessage('Done'));
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption :=
      WizardForm.FinishedLabel.Caption + #13#10#13#10 + CustomMessage('Done');
end;
