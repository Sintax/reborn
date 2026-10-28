; Battleborn Reborn server installer. Turns a Windows machine with a Battleborn folder into a Reborn
; server: the mod, the Steam loader, the server scripts, matchmaking and the Control Panel go into the
; game's Binaries\Win64 folder.
;
; Build it with:  python -m tools.installer --version 1.0.N
; That tool unpacks the release into a staging folder and writes the generated include (Generated)
; this script needs. This file is public: no server address, key, token or loader bytes in here.
; The setup exe it makes holds the Steam loader: it stays private (dist/private, private builds repo).
;
; Silent use:  Reborn-Server-Setup-1.0.N.exe /VERYSILENT /SUPPRESSMSGBOXES [/DIR=<Battleborn folder>]
;   exit code 5   the Control Panel is open (close it first)
;   exit code 6   this server already has a newer Reborn; add /ALLOWOLDER to install this one anyway
;   exit code 7   the folder has no Binaries\Win64\Battleborn.exe, or a server would not stop
;   /STEAMPATH=<folder>  tests only: look for the game in this Steam folder only (no registry, no D:\)
; Silent uninstall:  unins000.exe /VERYSILENT /SUPPRESSMSGBOXES  keeps users and keys;
;   add /DELETEUSERS to delete them too (the same as answering Yes).
; Only matchmaking and game servers inside the chosen folder are stopped; same-named programs
; elsewhere, or ones whose folder Windows will not tell, are left alone.
;
; Later parts add to this file (keep these places):
;   [Tasks]/[Run] and the finished page: firewall rules, VC++ runtime (Redist\vcredist_x64.exe),
;   matchmaking at sign-in, the desktop shortcut, the free-space parts (PartCount ... from Generated).
;   Extra event handlers: use event attributes, e.g. <event('CurStepChanged')>, so the ones here stay.

#ifndef Generated
  #error Build this with python -m tools.installer --version 1.0.N (it passes /DGenerated=...)
#endif
#include Generated
; Generated defines: AppVersion, StageDir, PartCount, and per part n (from 1): PartId<n>,
; PartLabel<n>, PartGlobs<n> (patterns joined with |), PartBytes<n>, PartSize<n> (e.g. "3.2 GB").

#ifndef Compression
  #define Compression "lzma2"
#endif

[Setup]
AppId={{C59BF78F-F3F1-4811-A2EF-28B325192A20}
AppName=Battleborn Reborn Server
AppVersion={#AppVersion}
AppVerName=Battleborn Reborn Server {#AppVersion}
AppPublisher=Battleborn Reborn
VersionInfoVersion={#AppVersion}
; {app} is the Battleborn folder itself; every file goes into {app}\Binaries\Win64.
DefaultDirName={code:DefaultGameDir}
UsePreviousAppDir=yes
DisableDirPage=no
AppendDefaultDirName=no
DirExistsWarning=no
DisableProgramGroupPage=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallFilesDir={app}\Binaries\Win64\RebornSetup
UninstallDisplayName=Battleborn Reborn Server
; The script stops matchmaking and the game servers itself, and asks for the panel to be closed.
CloseApplications=no
RestartApplications=no
OutputBaseFilename=Reborn-Server-Setup-{#AppVersion}
Compression={#Compression}
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
ShowLanguageDialog=no

[Messages]
WizardSelectDir=Pick the Battleborn folder
SelectDirDesc=Reborn goes into your Battleborn game folder.
SelectDirLabel3=Reborn's files go into the Binaries\Win64 folder of the Battleborn folder below.
SelectDirBrowseLabel=If this is not your Battleborn folder, click Browse and pick the one that has Binaries\Win64\Battleborn.exe.

[Files]
; The release's server zip (Win64\...) plus the Steam loader. The owner's files are never in it.
Source: "{#StageDir}\Win64\*"; DestDir: "{app}\Binaries\Win64"; Flags: ignoreversion recursesubdirs createallsubdirs
; Lets the server reach players on the internet; a server that has its own keeps it.
Source: "{#StageDir}\loader\steam_settings\configs.main.ini"; DestDir: "{app}\Binaries\Win64\steam_settings"; Flags: onlyifdoesntexist
; Matchmaking's settings (which games run) belong to the owner: only a first install gets the default.
Source: "{#StageDir}\defaults\appsettings.json"; DestDir: "{app}\Binaries\Win64\matchmaking"; Flags: onlyifdoesntexist uninsneveruninstall
; The Visual C++ 2012 runtime the game needs. Running it is a later part; it is here to run by hand.
Source: "{#StageDir}\Redist\vcredist_x64.exe"; DestDir: "{app}\Binaries\Win64\Redist"; Flags: ignoreversion

[Code]
const
  MsgWrongFolder = 'This folder doesn''t have Battleborn in it. Pick the folder that has Binaries\Win64\Battleborn.exe.';
  MsgPanelOpen = 'Close the Reborn Control Panel, then press Next.';
  MsgPanelOpenLate = 'Close the Reborn Control Panel, then run Setup again. Nothing was changed.';
  MsgOlder = 'This server has Reborn %1, newer than this installer (%2). Install the older version anyway?';
  MsgNothingChanged = 'Nothing was changed.';
  MsgNotStopped = 'The matchmaking service or a game server did not stop. Restart the PC, then run Setup again. Nothing was changed.';
  MsgUninstallPanelOpen = 'Close the Reborn Control Panel, then run the uninstaller again.';
  MsgDeleteUsers = 'Also delete users and keys?';
  MsgPartsGone = 'Files removed to save space were not put back. In Steam use Verify integrity of game files to get them back.';
  ExitPanelOpen = 5;
  ExitOlder = 6;
  StopWaitMs = 20000;
  ProcessQueryLimited = $1000;   { PROCESS_QUERY_LIMITED_INFORMATION }
  AttrDirectory = $10;
  DeployedFile = 'reborn-deployed.txt';
  RemovedPartsFile = 'reborn-removed-parts.txt';

type
  TPidList = array of Integer;

var
  GameDirCandidates: TArrayOfString;
  OlderAllowedFor: String;          { the folder the owner said yes to the older version for }
  UninstallPartsRemoved: Boolean;

procedure ExitProcess(ExitCode: Cardinal);
  external 'ExitProcess@kernel32.dll stdcall';
function OpenProcess(DesiredAccess: Cardinal; InheritHandle: Boolean; ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function QueryFullProcessImageName(Process: THandle; Flags: Cardinal; ExeName: String; var Size: Cardinal): Boolean;
  external 'QueryFullProcessImageNameW@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

{ ---------------------------------------------------------------- small helpers }

function HasSwitch(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then
    begin
      Result := True;
      Exit;
    end;
end;

function Win64Of(const Dir: String): String;
begin
  Result := AddBackslash(Dir) + 'Binaries\Win64';
end;

function IsGameFolder(const Dir: String): Boolean;
begin
  Result := (Trim(Dir) <> '') and FileExists(Win64Of(Dir) + '\Battleborn.exe');
end;

{ The number before the first dot of S, and S without it; -1 when it is not a number. }
function TakeVersionPart(var S: String): Integer;
var
  P: Integer;
begin
  P := Pos('.', S);
  if P = 0 then
  begin
    Result := StrToIntDef(S, -1);
    S := '';
  end
  else
  begin
    Result := StrToIntDef(Copy(S, 1, P - 1), -1);
    S := Copy(S, P + 1, Length(S));
  end;
end;

{ Above 0 when release A (like 1.0.12) is newer than B; 0 when equal or either is unreadable. }
function CompareVersions(A, B: String): Integer;
var
  I, X, Y: Integer;
begin
  Result := 0;
  for I := 1 to 3 do
  begin
    X := TakeVersionPart(A);
    Y := TakeVersionPart(B);
    if (X < 0) or (Y < 0) then
    begin
      Result := 0;
      Exit;
    end;
    if X <> Y then
    begin
      Result := X - Y;
      Exit;
    end;
  end;
end;

function DeployedVersion(const Win64: String): String;
var
  S: AnsiString;
begin
  Result := '';
  if LoadStringFromFile(Win64 + '\' + DeployedFile, S) then
    Result := Trim(String(S));
end;

function OlderAllowed(const Dir: String): Boolean;
begin
  Result := HasSwitch('/ALLOWOLDER') or ((OlderAllowedFor <> '') and (CompareText(OlderAllowedFor, Dir) = 0));
end;

function NewerAlreadyThere(const Dir: String; var Old: String): Boolean;
begin
  Old := DeployedVersion(Win64Of(Dir));
  Result := CompareVersions(Old, '{#AppVersion}') > 0;
end;

{ Ends Setup at once with a code of its own (Inno Setup's own codes cannot say why). Only used
  before anything was written. }
procedure QuitSilently(const Code: Integer; const Why: String);
begin
  Log(Why);
  Log(Format('Stopping with exit code %d. Nothing was changed.', [Code]));
  DelTree(ExpandConstant('{tmp}'), True, True, True);
  ExitProcess(Code);
end;

{ ---------------------------------------------------------------- finding the game }

function CleanPath(P: String): String;
begin
  P := Trim(P);
  StringChangeEx(P, '/', '\', True);
  Result := RemoveBackslashUnlessRoot(P);
end;

procedure AddCandidate(const Dir: String);
var
  I, N: Integer;
  D: String;
begin
  D := CleanPath(Dir);
  if D = '' then
    Exit;
  N := GetArrayLength(GameDirCandidates);
  for I := 0 to N - 1 do
    if CompareText(GameDirCandidates[I], D) = 0 then
      Exit;
  SetArrayLength(GameDirCandidates, N + 1);
  GameDirCandidates[N] := D;
end;

{ Every "path" in Steam's libraryfolders.vdf (backslashes written as \\). }
procedure AddLibraries(const Vdf: String);
var
  Lines: TArrayOfString;
  I, J: Integer;
  S, P: String;
begin
  if not LoadStringsFromFile(Vdf, Lines) then
    Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    S := Trim(Lines[I]);
    if CompareText(Copy(S, 1, 6), '"path"') <> 0 then
      Continue;
    S := Trim(Copy(S, 7, Length(S)));
    if (Length(S) < 2) or (S[1] <> '"') then
      Continue;
    P := '';
    J := 2;
    while (J <= Length(S)) and (S[J] <> '"') do
    begin
      if (S[J] = '\') and (J < Length(S)) then
        J := J + 1;
      P := P + S[J];
      J := J + 1;
    end;
    AddCandidate(P + '\steamapps\common\Battleborn');
  end;
end;

procedure AddSteam(const Root: String);
var
  R: String;
begin
  R := CleanPath(Root);
  if R = '' then
    Exit;
  { While the Steam loader runs, Steam's registry value points at the game's own Win64 folder. }
  if (CompareText(ExtractFileName(R), 'Win64') = 0) and FileExists(R + '\Battleborn.exe') then
    AddCandidate(ExtractFileDir(ExtractFileDir(R)));
  AddCandidate(R + '\steamapps\common\Battleborn');
  AddLibraries(R + '\steamapps\libraryfolders.vdf');
end;

procedure AddSteamFromRegistry(const RootKey: Integer; const SubKey: String);
var
  S: String;
begin
  if RegQueryStringValue(RootKey, SubKey, 'SteamPath', S) then
    AddSteam(S);
  if RegQueryStringValue(RootKey, SubKey, 'InstallPath', S) then
    AddSteam(S);
end;

{ The folder the wizard suggests (a previous install's folder comes first: UsePreviousAppDir). }
function DefaultGameDir(Param: String): String;
var
  I: Integer;
  TestSteam: String;
begin
  SetArrayLength(GameDirCandidates, 0);
  TestSteam := ExpandConstant('{param:STEAMPATH|}');
  if TestSteam <> '' then
    AddSteam(TestSteam)
  else
  begin
    AddSteamFromRegistry(HKCU, 'Software\Valve\Steam');
    AddSteamFromRegistry(HKLM, 'SOFTWARE\WOW6432Node\Valve\Steam');
    AddSteamFromRegistry(HKLM, 'SOFTWARE\Valve\Steam');
    AddSteam(ExpandConstant('{commonpf32}\Steam'));
    AddSteam(ExpandConstant('{commonpf64}\Steam'));
    AddCandidate('D:\Battleborn');
  end;
  for I := 0 to GetArrayLength(GameDirCandidates) - 1 do
    if IsGameFolder(GameDirCandidates[I]) then
    begin
      Result := GameDirCandidates[I];
      Log('Found Battleborn in ' + Result);
      Exit;
    end;
  Log('Battleborn was not found in Steam''s folders; the owner picks it');
  if GetArrayLength(GameDirCandidates) > 0 then
    Result := GameDirCandidates[0]
  else
    Result := 'D:\Battleborn';
end;

{ ---------------------------------------------------------------- running programs }

function InFolder(const Path, Folder: String): Boolean;
begin
  Result := Pos(Lowercase(AddBackslash(Folder)), Lowercase(Path)) = 1;
end;

{ Where process Pid's program file is, or '' when Windows will not say. Asks only for the
  limited query right, which works for the same user's programs whether or not they run as admin. }
function ProcessPath(const Pid: Cardinal): String;
var
  H: THandle;
  Buf: String;
  Size: Cardinal;
begin
  Result := '';
  H := OpenProcess(ProcessQueryLimited, False, Pid);
  if H = 0 then
    Exit;
  try
    Size := 1024;
    Buf := StringOfChar(#0, Size);
    if QueryFullProcessImageName(H, 0, Buf, Size) then
      Result := Copy(Buf, 1, Size);
  finally
    CloseHandle(H);
  end;
end;

{ Running copies of ExeName inside Folder. A copy elsewhere, or one whose place Windows will
  not tell, is not ours and is left alone. False if Windows could not list the programs. }
function FindOurs(const ExeName, Folder: String; var Pids: TPidList): Boolean;
var
  Locator, Service, Procs, Proc: Variant;
  I, N, Pid: Integer;
  Path: String;
begin
  SetArrayLength(Pids, 0);
  Result := True;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Procs := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''' + ExeName + '''');
    for I := 0 to Procs.Count - 1 do
    begin
      Proc := Procs.ItemIndex(I);
      Pid := Proc.ProcessId;
      Path := ProcessPath(Pid);
      if Path = '' then
        Log(Format('%s (process %d): its folder cannot be read; leaving it alone', [ExeName, Pid]))
      else if InFolder(Path, Folder) then
      begin
        N := GetArrayLength(Pids);
        SetArrayLength(Pids, N + 1);
        Pids[N] := Pid;
      end;
    end;
  except
    Log('Could not list the running programs: ' + GetExceptionMessage);
    Result := False;
  end;
end;

{ 1 when a copy of ExeName runs inside Folder, 0 when none does, -1 when Windows could not list them. }
function RunningState(const ExeName, Folder: String): Integer;
var
  Pids: TPidList;
begin
  if not FindOurs(ExeName, Folder, Pids) then
    Result := -1
  else if GetArrayLength(Pids) > 0 then
    Result := 1
  else
    Result := 0;
end;

{ False when Windows could not list the programs (then nothing is stopped). }
function StopProgram(const ExeName, Folder: String): Boolean;
var
  Pids: TPidList;
  I, Code: Integer;
begin
  Result := FindOurs(ExeName, Folder, Pids);
  if not Result then
    Exit;
  for I := 0 to GetArrayLength(Pids) - 1 do
  begin
    Log(Format('Stopping %s (process %d)', [ExeName, Pids[I]]));
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /pid ' + IntToStr(Pids[I]), '', SW_HIDE, ewWaitUntilTerminated, Code);
  end;
end;

{ Stops matchmaking and the game servers of this folder (never copies elsewhere); waits up to
  20 seconds for them to go. False when one still runs or the programs could not be listed. }
function StopServers(const Win64: String): Boolean;
var
  Waited, A, B: Integer;
begin
  Result := False;
  if not (StopProgram('gamecontroller.exe', Win64) and StopProgram('Serverborn.exe', Win64)) then
    Exit;
  Waited := 0;
  repeat
    A := RunningState('gamecontroller.exe', Win64);
    B := RunningState('Serverborn.exe', Win64);
    if (A < 0) or (B < 0) then
      Exit;
    if (A = 0) and (B = 0) then
    begin
      Result := True;
      Exit;
    end;
    Sleep(500);
    Waited := Waited + 500;
  until Waited > StopWaitMs;
end;

function PanelOpen(const Win64: String): Boolean;
begin
  Result := RunningState('ControlPanel.exe', Win64) > 0;
end;

{ ---------------------------------------------------------------- files }

function IsOwnersMatchmakingFile(const Name: String): Boolean;
begin
  Result := (CompareText(Name, 'appsettings.json') = 0) or (CompareText(Name, 'admin-token.txt') = 0);
end;

function IsLogFile(const Name: String): Boolean;
begin
  Result := CompareText(ExtractFileExt(Name), '.log') = 0;
end;

{ Empties the matchmaking folder; with KeepOwners its appsettings.json and admin-token.txt stay,
  with KeepLogs its *.log files (the owner's history) stay. }
procedure ClearMatchmaking(const Dir: String; const KeepOwners, KeepLogs: Boolean);
var
  F: TFindRec;
  Names: TArrayOfString;
  IsDir: TArrayOfBoolean;
  I, N: Integer;
begin
  N := 0;
  if FindFirst(AddBackslash(Dir) + '*', F) then
  try
    repeat
      if (F.Name <> '.') and (F.Name <> '..') then
      begin
        SetArrayLength(Names, N + 1);
        SetArrayLength(IsDir, N + 1);
        Names[N] := F.Name;
        IsDir[N] := (F.Attributes and AttrDirectory) <> 0;
        N := N + 1;
      end;
    until not FindNext(F);
  finally
    FindClose(F);
  end;
  for I := 0 to N - 1 do
    if IsDir[I] then
      DelTree(AddBackslash(Dir) + Names[I], True, True, True)
    else if not ((KeepOwners and IsOwnersMatchmakingFile(Names[I])) or (KeepLogs and IsLogFile(Names[I]))) then
      DeleteFile(AddBackslash(Dir) + Names[I]);
end;

{ ---------------------------------------------------------------- Setup }

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir, Old: String;
begin
  Result := True;
  if (CurPageID <> wpSelectDir) or WizardSilent then
    Exit;   { a silent install is checked in PrepareToInstall }
  Dir := WizardDirValue;
  if not IsGameFolder(Dir) then
  begin
    MsgBox(MsgWrongFolder, mbError, MB_OK);
    Result := False;
    Exit;
  end;
  if NewerAlreadyThere(Dir, Old) and not OlderAllowed(Dir) then
  begin
    if MsgBox(FmtMessage(MsgOlder, [Old, '{#AppVersion}']), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) <> IDYES then
    begin
      Result := False;
      Exit;
    end;
    OlderAllowedFor := Dir;
  end;
  if PanelOpen(Win64Of(Dir)) then
  begin
    MsgBox(MsgPanelOpen, mbInformation, MB_OK);
    Result := False;
  end;
end;

{ Last checks before anything is written, then matchmaking and the game servers are stopped. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Dir, Old: String;
begin
  Result := '';
  Dir := WizardDirValue;
  if not IsGameFolder(Dir) then
  begin
    Result := MsgWrongFolder;
    Exit;
  end;
  if NewerAlreadyThere(Dir, Old) and not OlderAllowed(Dir) then
  begin
    if WizardSilent then
      QuitSilently(ExitOlder, FmtMessage(MsgOlder, [Old, '{#AppVersion}']) + ' Silent install: no (add /ALLOWOLDER for yes).');
    Result := MsgNothingChanged;
    Exit;
  end;
  if PanelOpen(Win64Of(Dir)) then
  begin
    if WizardSilent then
      QuitSilently(ExitPanelOpen, MsgPanelOpen);
    Result := MsgPanelOpenLate;
    Exit;
  end;
  if not StopServers(Win64Of(Dir)) then
    Result := MsgNotStopped;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Win64: String;
begin
  Win64 := Win64Of(ExpandConstant('{app}'));
  if CurStep = ssInstall then
  begin
    { Matchmaking is replaced as a whole (no files of an older release linger); the owner's stay. }
    if DirExists(Win64 + '\matchmaking') then
      ClearMatchmaking(Win64 + '\matchmaking', True, True);
  end
  else if CurStep = ssPostInstall then
  begin
    { The server is a renamed copy of the game: the mod acts as the server when it runs as Serverborn.exe. }
    if not FileExists(Win64 + '\Serverborn.exe') then
      if not CopyFile(Win64 + '\Battleborn.exe', Win64 + '\Serverborn.exe', True) then
        Log('Could not make Serverborn.exe; StartMatchmaking.bat makes it on first start');
    { The same record the panel and its updater keep: the version, then a line break. }
    if not SaveStringToFile(Win64 + '\' + DeployedFile, '{#AppVersion}' + #10, False) then
      Log('Could not write ' + DeployedFile);
  end;
end;

{ ---------------------------------------------------------------- Uninstall }

function InitializeUninstall(): Boolean;
begin
  Result := True;
  if PanelOpen(Win64Of(ExpandConstant('{app}'))) then
  begin
    SuppressibleMsgBox(MsgUninstallPanelOpen, mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Win64: String;
  DeleteUsers: Boolean;
begin
  Win64 := Win64Of(ExpandConstant('{app}'));
  if CurUninstallStep = usUninstall then
  begin
    if not StopServers(Win64) then
      Log('Matchmaking or a game server of this folder did not stop (or the running programs could not be listed); files they hold stay');
    { /DELETEUSERS answers yes without asking (scripted uninstalls); otherwise the owner is asked, No by default. }
    if HasSwitch('/DELETEUSERS') then
      DeleteUsers := True
    else
      DeleteUsers := SuppressibleMsgBox(MsgDeleteUsers, mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES;
    UninstallPartsRemoved := FileExists(Win64 + '\' + RemovedPartsFile);
    DeleteFile(Win64 + '\' + RemovedPartsFile);
    ClearMatchmaking(Win64 + '\matchmaking', not DeleteUsers, False);
    RemoveDir(Win64 + '\matchmaking');   { only goes when empty }
    if DeleteUsers then
    begin
      DeleteFile(Win64 + '\reborn-users.json');
      DeleteFile(Win64 + '\reborn-users-seen.json');
      DeleteFile(Win64 + '\reborn-zips.json');
    end;
    DelTree(Win64 + '\ControlPanel', True, True, True);
    DelTree(Win64 + '\RebornVersions', True, True, True);
    DeleteFile(Win64 + '\Serverborn.exe');
    DeleteFile(Win64 + '\' + DeployedFile);
  end
  else if (CurUninstallStep = usDone) and UninstallPartsRemoved then
    SuppressibleMsgBox(MsgPartsGone, mbInformation, MB_OK, IDOK);
end;
