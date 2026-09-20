; Installeur Inno Setup du plugin OBS "NSFW Guard".
; Compilation : scripts\installer.ps1 (après scripts\build.ps1 et scripts\package.ps1).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

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

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
french.ObsRunning=OBS Studio est ouvert. Fermez-le, puis cliquez sur « Précédent » et de nouveau sur « Suivant ».
english.ObsRunning=OBS Studio is running. Close it, then click Back and Next again.
french.ObsNotFound=OBS Studio ne semble pas installé dans son dossier habituel. Le plugin sera installé quand même dans %1.%n%nContinuer ?
english.ObsNotFound=OBS Studio does not seem to be installed in its usual folder. The plugin will be installed anyway in %1.%n%nContinue?
french.UninstallObsRunning=OBS Studio est ouvert. Fermez-le avant de désinstaller NSFW Guard.
english.UninstallObsRunning=OBS Studio is running. Close it before uninstalling NSFW Guard.
french.Done=Relancez OBS, puis ajoutez le filtre « NSFW Guard » à une source (clic droit > Filtres > +).
english.Done=Restart OBS, then add the "NSFW Guard" filter to a source (right-click > Filters > +).

[Files]
Source: "..\stage\obs-nsfw-guard\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Code]
function IsObsRunning(): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'),
    '/C tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe" >nul',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not FileExists(ExpandConstant('{commonpf64}\obs-studio\bin\64bit\obs64.exe')) then
    Result := SuppressibleMsgBox(
      FmtMessage(CustomMessage('ObsNotFound'), [ExpandConstant('{commonappdata}\obs-studio\plugins')]),
      mbConfirmation, MB_YESNO, IDYES) = IDYES;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if IsObsRunning() then
    Result := CustomMessage('ObsRunning');
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
