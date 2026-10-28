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
; Options (task names for /TASKS= or /MERGETASKS=):
;   firewall    let players through Windows Firewall (the two rules OpenFirewall.bat makes)   ticked
;   vcredist    install the Visual C++ 2012 runtime                     ticked only when it is missing
;   autostart   start matchmaking when the installing user signs in (a scheduled task)        ticked
;   shortcut    "Reborn Control Panel" on the desktop                                          ticked
;   trim\<id>   free-space parts (only parts that passed the regression games)               unticked
; GitHub access (a wizard page): a server without a signed-in gh can keep a read-only token for the private
;   builds repo, so the Control Panel can fetch updates. The page is skipped when `gh auth status` works. The
;   token is handed to ControlPanel.exe --save-github-token through a temp file, run as the user who started
;   Setup, and the file is deleted afterwards. The token is never logged or put on a command line.
; A step that fails does not stop Setup: the last page (and the log) says what to run by hand.
; The uninstaller removes the firewall rules and the scheduled task only if Setup made them.
;
; Test switches (inert unless given; they never change the PC):
;   /DRYRUN               firewall, runtime and scheduled-task commands are written to the log
;                         instead of run, and no desktop shortcut is made (the uninstaller too)
;   /FAILSTEP=firewall|vcredist|autostart   that step fails without running anything
;   /FAILSTEP=crash-<step>  that step (firewall, vcredist, autostart, shortcut, token, trim) stops with an
;                         error before running anything; the later steps must still run
;   /RUNTIMECHECK=<file>  decide "runtime missing" by this file instead of System32\msvcp110.dll
;   /FIREWALLSHOW=<file>  netsh's answer about the current game-server rule, from a file (none: no rule)
;   /GHTOKENFILE=<file>   the GitHub token comes from this file instead of the page (which is then skipped);
;                         a silent install without it asks for none. /FAILSTEP=token makes the hand-over fail.
; The game servers' ports belong to the Control Panel after the first install: Setup keeps the
; rule's ports and matchmaking's saved GamePorts, and opens 7777-7779 only when it has neither.

#ifndef Generated
  #error Build this with python -m tools.installer --version 1.0.N (it passes /DGenerated=...)
#endif
#include Generated
; Generated defines: AppVersion, StageDir, PartCount, PartsAllPassed (1 when the parts offered were
; also tested all removed together), and per part n (from 1): PartId<n>, PartLabel<n>, PartGlobs<n>
; (patterns joined with |), PartBytes<n>, PartSize<n> (e.g. "3.2 GB"); the same values as arrays
; from 0 (PartIds, PartLabels, PartGlobsA, PartSizes) when PartCount > 0.

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
; Every run starts from the recommended choices; a free-space part is never ticked by itself.
UsePreviousTasks=no
; The shortcut goes on the desktop of the owner who runs Setup (as tools/vm.py makes it).
UsedUserAreasWarning=no

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
; The Visual C++ 2012 runtime the game needs (the vcredist option runs it; it stays to run by hand).
Source: "{#StageDir}\Redist\vcredist_x64.exe"; DestDir: "{app}\Binaries\Win64\Redist"; Flags: ignoreversion

[Tasks]
Name: firewall; Description: "Let players through Windows Firewall (UDP 7777-7779 and TCP 5000)"
Name: vcredist; Description: "Install the Visual C++ 2012 runtime the game needs"
Name: autostart; Description: "Start matchmaking when I sign in to Windows"
Name: shortcut; Description: "Put ""Reborn Control Panel"" on the desktop"
#if PartCount > 0
Name: trim; Description: "Free up space: remove game files the server does not need (tick the parts below)"; GroupDescription: "Free up space (optional):"; Flags: unchecked
  #define I
  #sub EmitPartTask
Name: "trim\{#PartIds[I]}"; Description: "{#PartLabels[I]} (about {#PartSizes[I]})"; Flags: unchecked dontinheritcheck
  #endsub
  #for {I = 0; I < PartCount; I++} EmitPartTask
#endif

[Icons]
Name: "{userdesktop}\Reborn Control Panel"; Filename: "{app}\Binaries\Win64\ControlPanel\ControlPanel.exe"; WorkingDir: "{app}\Binaries\Win64\ControlPanel"; Comment: "Battleborn Reborn Control Panel"; Tasks: shortcut; Check: MakeShortcut

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
  { What Setup made that the uninstaller must remove (one name per line), next to the uninstaller. }
  MadeFile = 'RebornSetup\reborn-setup-made.txt';
  MsgFailFirewall = 'The firewall rules could not be added. Run OpenFirewall.bat in Binaries\Win64 once.';
  MsgFailRuntime = 'The Visual C++ 2012 runtime did not install. Run Binaries\Win64\Redist\vcredist_x64.exe.';
  MsgFailAutostart = 'Matchmaking will not start by itself. Start it with StartMatchmaking.bat.';
  MsgFailToken = 'The GitHub token was not saved. Run Setup again to add it.';
  MsgFailShortcut = 'The desktop shortcut was not made. Start the panel from Binaries\Win64\ControlPanel\ControlPanel.exe.';
  MsgFailParts = 'Removing the free-space parts stopped part-way; some of their files may still be there.';
  MsgTokenTitle = 'GitHub access';
  MsgTokenDescription = 'Reborn updates come from a private page on GitHub.';
  MsgTokenLabel = 'Read-only token for Sintax/reborn-builds (you can leave this empty and add it later)';
  TokenTempName = 'reborn-github-token.tmp';
  MsgFinishedProblems = 'Reborn is installed, but some steps did not work:';
  MsgPartsWarning = 'Removed files only come back by copying the game again (in Steam: Verify integrity of game files).';
  MsgPartsOneAtATime = 'Tested one at a time.';
  RuleServer = 'Battleborn Reborn server';
  RuleMatchmaking = 'Battleborn Reborn matchmaking';
  TaskName = 'Battleborn Reborn matchmaking';
  RuntimeDll = 'msvcp110.dll';
  AttrReparsePoint = $400;

type
  TPidList = array of Integer;
  TPortList = array of Integer;

var
  GameDirCandidates: TArrayOfString;
  OlderAllowedFor: String;          { the folder the owner said yes to the older version for }
  UninstallPartsRemoved: Boolean;
  Problems: String;                 { what failed, one line each, for the last page }
  RestartWanted: Boolean;
  UninstallMade: String;            { the made-file's lines, read before anything is removed }
  RuntimeDefaultDone: Boolean;
  TokenPage: TInputQueryWizardPage;
  GhState: Integer;                 { 0 not asked yet, 1 gh is signed in, 2 it is not }

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

{ ---------------------------------------------------------------- options: commands }

function HasSwitchPrefix(const Prefix: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(Copy(ParamStr(I), 1, Length(Prefix)), Prefix) = 0 then
    begin
      Result := True;
      Exit;
    end;
end;

{ /DRYRUN: a test switch. The commands that change Windows are logged, never run. }
function DryRun: Boolean;
begin
  Result := HasSwitch('/DRYRUN');
end;

function MakeShortcut: Boolean;
begin
  Result := not DryRun;
  if not Result then
    Log('Dry run (/DRYRUN): no desktop shortcut made');
end;

{ /FAILSTEP=<step>: a test switch. That step fails without running anything. }
function FailStepIs(const Step: String): Boolean;
begin
  Result := CompareText(ExpandConstant('{param:FAILSTEP|}'), Step) = 0;
end;

function ForcedToFail(const Step: String): Boolean;
begin
  Result := FailStepIs(Step);
  if Result then
    Log('Step ' + Step + ' forced to fail (/FAILSTEP); nothing was run');
end;

{ /FAILSTEP=crash-<step>: a test switch. That step stops with an error (as a failing Windows call
  would) before it runs anything. }
procedure CrashIfForced(const Step: String);
begin
  if FailStepIs('crash-' + Step) then
    RaiseException('step ' + Step + ' forced to stop with an error (/FAILSTEP)');
end;

{ Runs Exe with Params and waits; False when it could not start. With Dry, it is only logged. }
function RunCommand(const Exe, Params: String; const Dry: Boolean; var Code: Integer): Boolean;
begin
  if Dry then
  begin
    Log('Dry run, not running: ' + ExtractFileName(Exe) + ' ' + Params);
    Code := 0;
    Result := True;
    Exit;
  end;
  Log('Running: ' + ExtractFileName(Exe) + ' ' + Params);
  Result := Exec(Exe, Params, '', SW_HIDE, ewWaitUntilTerminated, Code);
  if Result then
    Log(Format('Exit code %d', [Code]))
  else
    Log('Could not start it: ' + SysErrorMessage(Code));
end;

function NetshExe: String;
begin
  Result := ExpandConstant('{sys}\netsh.exe');
end;

function SchtasksExe: String;
begin
  Result := ExpandConstant('{sys}\schtasks.exe');
end;

procedure AddProblem(const Msg: String);
begin
  Log('Problem: ' + Msg);
  Problems := Problems + Msg + #13#10;
end;

procedure DeleteFirewallRules(const Dry: Boolean);
var
  Code: Integer;
begin
  RunCommand(NetshExe, 'advfirewall firewall delete rule name="' + RuleServer + '"', Dry, Code);
  RunCommand(NetshExe, 'advfirewall firewall delete rule name="' + RuleMatchmaking + '"', Dry, Code);
end;

procedure DeleteSignInTask(const Dry: Boolean);
var
  Code: Integer;
begin
  RunCommand(SchtasksExe, '/Delete /TN "' + TaskName + '" /F', Dry, Code);
end;

{ ---- the game servers' ports. After the first install the Control Panel owns them: it saves the
  list in matchmaking\appsettings.json ("GamePorts") and opens those ports (plus any still in use)
  in the "Battleborn Reborn server" rule. Setup never closes a port: it keeps the rule's ports and
  the saved ones. Only with neither does it use the default 7777-7779. }

procedure AddPort(var Ports: TPortList; const P: Integer);
var
  I, J, N: Integer;
begin
  if (P < 1) or (P > 65535) then
    Exit;
  N := GetArrayLength(Ports);
  I := 0;
  while (I < N) and (Ports[I] < P) do
    I := I + 1;
  if (I < N) and (Ports[I] = P) then
    Exit;
  SetArrayLength(Ports, N + 1);
  for J := N downto I + 1 do
    Ports[J] := Ports[J - 1];
  Ports[I] := P;
end;

{ Adds a port list like "7777-7779,7781" (netsh's and OpenFirewall.bat's form). A part that is not
  a port or a short range (such as "Any") is kept as written, in Raw. }
procedure AddPortText(var Ports: TPortList; var Raw: String; Text: String);
var
  Item: String;
  P, D, A, B, K: Integer;
begin
  Text := Text + ',';
  while Text <> '' do
  begin
    P := Pos(',', Text);
    Item := Trim(Copy(Text, 1, P - 1));
    Text := Copy(Text, P + 1, Length(Text));
    if Item = '' then
      Continue;
    D := Pos('-', Item);
    A := -1;
    B := -1;
    if D > 0 then
    begin
      A := StrToIntDef(Trim(Copy(Item, 1, D - 1)), -1);
      B := StrToIntDef(Trim(Copy(Item, D + 1, Length(Item))), -1);
    end
    else
    begin
      A := StrToIntDef(Item, -1);
      B := A;
    end;
    if (A >= 1) and (B >= A) and (B <= 65535) and (B - A <= 1000) then
    begin
      for K := A to B do
        AddPort(Ports, K);
    end
    else if Pos(',' + Lowercase(Item) + ',', ',' + Lowercase(Raw) + ',') = 0 then
    begin
      if Raw <> '' then
        Raw := Raw + ',';
      Raw := Raw + Item;
    end;
  end;
end;

{ The "GamePorts" list in matchmaking\appsettings.json, the panel's saved ports. Nothing is added
  when the file or the list cannot be read. }
procedure AddSavedPorts(const Win64: String; var Ports: TPortList);
var
  S: AnsiString;
  T, Inner, Raw: String;
  P, Q: Integer;
begin
  if not LoadStringFromFile(Win64 + '\matchmaking\appsettings.json', S) then
    Exit;
  T := String(S);
  P := Pos('"gameports"', Lowercase(T));
  if P = 0 then
    Exit;
  T := Copy(T, P + 11, Length(T));
  P := Pos('[', T);
  Q := Pos(']', T);
  if (P = 0) or (Q < P) or (Trim(Copy(T, 1, P - 1)) <> ':') then
    Exit;
  Inner := Copy(T, P + 1, Q - P - 1);
  Raw := '';
  AddPortText(Ports, Raw, Inner);
  if Raw <> '' then
    Log('Some of the saved GamePorts are not ports: ' + Raw);
  Log('The Control Panel''s saved game ports are read from matchmaking\appsettings.json');
end;

{ The current "Battleborn Reborn server" rule: is there one, and its ports (Readable: False when it
  is there but its ports could not be read). Only reads; /FIREWALLSHOW=<file> (a test switch) gives
  netsh's answer from a file instead (no file: no rule). }
procedure ReadServerRule(var Found, Readable: Boolean; var Ports: TPortList; var Raw: String);
var
  Lines: TArrayOfString;
  Output: TExecOutput;
  Fake, Key: String;
  I, P, Code: Integer;
begin
  Found := False;
  Readable := False;
  Fake := ExpandConstant('{param:FIREWALLSHOW|}');
  if Fake <> '' then
  begin
    Log('Test switch /FIREWALLSHOW: the current rule is read from ' + Fake);
    Found := LoadStringsFromFile(Fake, Lines);
  end
  else
  begin
    try
      Found := ExecAndCaptureOutput(NetshExe, 'advfirewall firewall show rule name="' + RuleServer + '" verbose', '',
        SW_SHOWNORMAL, ewWaitUntilTerminated, Code, Output) and (Code = 0);
      Lines := Output.StdOut;
    except
      Log('Could not read the current firewall rule: ' + GetExceptionMessage);
      Found := False;
    end;
  end;
  if not Found then
    Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    P := Pos(':', Lines[I]);
    if P = 0 then
      Continue;
    Key := Trim(Copy(Lines[I], 1, P - 1));
    if CompareText(Key, 'LocalPort') = 0 then
    begin
      Readable := True;
      AddPortText(Ports, Raw, Copy(Lines[I], P + 1, Length(Lines[I])));
    end;
  end;
end;

function AllDigits(const S: String): Boolean;
var
  I: Integer;
begin
  Result := (S <> '') and (Length(S) <= 5);
  for I := 1 to Length(S) do
    if (S[I] < '0') or (S[I] > '9') then
      Result := False;
end;

{ True when every entry of Raw (as AddPortText keeps it) is a plain port or range netsh takes back,
  such as a wide range "7777-9999". False for words like "Any" or "RPC": netsh would refuse them. }
function RawPortsWellFormed(const Raw: String): Boolean;
var
  Text, Item: String;
  P, D, A, B: Integer;
begin
  Result := True;
  Text := Raw + ',';
  while Text <> '' do
  begin
    P := Pos(',', Text);
    Item := Trim(Copy(Text, 1, P - 1));
    Text := Copy(Text, P + 1, Length(Text));
    if Item = '' then
      Continue;
    D := Pos('-', Item);
    if D > 0 then
    begin
      if not (AllDigits(Trim(Copy(Item, 1, D - 1))) and AllDigits(Trim(Copy(Item, D + 1, Length(Item))))) then
      begin
        Result := False;
        Exit;
      end;
      A := StrToIntDef(Trim(Copy(Item, 1, D - 1)), -1);
      B := StrToIntDef(Trim(Copy(Item, D + 1, Length(Item))), -1);
    end
    else
    begin
      if not AllDigits(Item) then
      begin
        Result := False;
        Exit;
      end;
      A := StrToIntDef(Item, -1);
      B := A;
    end;
    if not ((A >= 1) and (B >= A) and (B <= 65535)) then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function PortListText(const Ports: TPortList; const Raw: String): String;
var
  I: Integer;
begin
  if (Raw = '') and ((GetArrayLength(Ports) = 0) or ((GetArrayLength(Ports) = 3) and (Ports[0] = 7777) and
     (Ports[1] = 7778) and (Ports[2] = 7779))) then
  begin
    Result := '7777-7779';
    Exit;
  end;
  Result := '';
  for I := 0 to GetArrayLength(Ports) - 1 do
  begin
    if Result <> '' then
      Result := Result + ',';
    Result := Result + IntToStr(Ports[I]);
  end;
  if Raw <> '' then
  begin
    if Result <> '' then
      Result := Result + ',';
    Result := Result + Raw;
  end;
end;

{ The two rules OpenFirewall.bat makes, with the same names and settings (the panel edits the first
  by name). Old ones go first so a moved game folder gets rules for its new place. The game
  servers' rule keeps every port it had (see above); one whose ports cannot be read stays as it is. }
function AddFirewallRules(const Win64: String): Boolean;
var
  Code: Integer;
  Found, Readable: Boolean;
  Ports: TPortList;
  Raw, List: String;
begin
  Result := False;
  if ForcedToFail('firewall') then
    Exit;
  CrashIfForced('firewall');
  Raw := '';
  SetArrayLength(Ports, 0);
  ReadServerRule(Found, Readable, Ports, Raw);
  if Found and not Readable then
  begin
    Log('The game servers'' firewall rule is there but its ports could not be read; it is kept as it is');
    Result := True;
  end
  else if Found and not RawPortsWellFormed(Raw) then
  begin
    { netsh would refuse these ports when the rule is added again, after the old one was deleted:
      the server would be left with no rule at all. }
    Log('The game servers'' firewall rule has ports Setup cannot write back (' + Raw + '); it is kept as it is');
    Result := True;
  end
  else
  begin
    AddSavedPorts(Win64, Ports);
    List := PortListText(Ports, Raw);
    if Found then
      Log('The game servers'' firewall rule is there; it keeps its ports and the saved ones: ' + List)
    else
      Log('No game servers'' firewall rule yet; it opens UDP ' + List);
    RunCommand(NetshExe, 'advfirewall firewall delete rule name="' + RuleServer + '"', DryRun, Code);
    Result := RunCommand(NetshExe, 'advfirewall firewall add rule name="' + RuleServer + '" dir=in action=allow profile=any program="' +
      Win64 + '\Serverborn.exe" protocol=UDP localport=' + List + ' edge=no', DryRun, Code) and (Code = 0);
  end;
  RunCommand(NetshExe, 'advfirewall firewall delete rule name="' + RuleMatchmaking + '"', DryRun, Code);
  Result := RunCommand(NetshExe, 'advfirewall firewall add rule name="' + RuleMatchmaking + '" dir=in action=allow profile=any program="' +
    Win64 + '\matchmaking\gamecontroller.exe" protocol=TCP localport=5000 edge=no', DryRun, Code) and (Code = 0) and Result;
end;

{ The file whose absence means "the runtime is missing": msvcp110.dll in the 64-bit System32
  (Setup runs in 64-bit mode, so the sys constant is that folder). /RUNTIMECHECK=<file> replaces it in tests. }
function RuntimeCheckFile: String;
begin
  Result := ExpandConstant('{param:RUNTIMECHECK|}');
  if Result = '' then
    Result := ExpandConstant('{sys}\' + RuntimeDll);
end;

function RuntimeMissing: Boolean;
begin
  Result := not FileExists(RuntimeCheckFile);
end;

function InstallRuntime(const Win64: String): Boolean;
var
  Code: Integer;
begin
  Result := False;
  if ForcedToFail('vcredist') then
    Exit;
  CrashIfForced('vcredist');
  if not RunCommand(Win64 + '\Redist\vcredist_x64.exe', '/install /quiet /norestart', DryRun, Code) then
    Exit;
  { 1638: a newer one is there already; 3010: it works after a restart. }
  Result := (Code = 0) or (Code = 1638) or (Code = 3010);
  if Code = 3010 then
    RestartWanted := True;
end;

function InstallingUser: String;
var
  Domain: String;
begin
  Domain := GetEnv('USERDOMAIN');
  if Domain = '' then
    Domain := GetComputerNameString;
  Result := Domain + '\' + GetUserNameString;
end;

{ Starts StartMatchmaking.bat whenever the user who ran Setup signs in, with full rights. }
function AddSignInTask(const Win64: String): Boolean;
var
  Code: Integer;
begin
  Result := False;
  if ForcedToFail('autostart') then
    Exit;
  CrashIfForced('autostart');
  Result := RunCommand(SchtasksExe, '/Create /TN "' + TaskName + '" /TR "\"' + Win64 + '\StartMatchmaking.bat\"" /SC ONLOGON /RU "' +
    InstallingUser + '" /IT /RL HIGHEST /F', DryRun, Code) and (Code = 0);
end;

{ Notes that Setup made Name, so the uninstaller removes it. A dry run notes "<name> (dry run)":
  then the uninstaller only logs the removal, even without /DRYRUN. }
procedure NoteMade(const Win64, Name: String);
var
  Lines: TArrayOfString;
  Entry: String;
  I: Integer;
begin
  Entry := Name;
  if DryRun then
    Entry := Name + ' (dry run)';
  if LoadStringsFromFile(Win64 + '\' + MadeFile, Lines) then
    for I := 0 to GetArrayLength(Lines) - 1 do
      if CompareText(Trim(Lines[I]), Entry) = 0 then
        Exit;
  if not SaveStringToFile(Win64 + '\' + MadeFile, Entry + #13#10, True) then
    Log('Could not write ' + MadeFile);
end;

function MadeHas(const Entry: String): Boolean;
begin
  Result := Pos(#10 + Lowercase(Entry) + #13#10, #10 + Lowercase(UninstallMade)) > 0;
end;

{ ---------------------------------------------------------------- options: free-space parts }

function PartCountValue: Integer;
begin
  Result := {#PartCount};
end;

function PartsTestedTogether: Boolean;
begin
  Result := {#PartsAllPassed} <> 0;
end;

{ The part ids and their patterns (one '|'-joined string per part), in the parts list's order. }
procedure GetParts(var Ids, Globs: TArrayOfString);
begin
  SetArrayLength(Ids, PartCountValue);
  SetArrayLength(Globs, PartCountValue);
#if PartCount > 0
  #sub EmitPartCode
  Ids[{#I}] := '{#PartIds[I]}';
  Globs[{#I}] := '{#PartGlobsA[I]}';
  #endsub
  #for {I = 0; I < PartCount; I++} EmitPartCode
#endif
end;

{ Does Path ('/'-separated, lower case) match glob G (lower case) from positions PI and SI? The
  same rules as tools/trim.py: '**/' is any number of folders (none too), '**' anything, '*' and
  '?' stay inside one name. }
function GlobFrom(const G, S: String; PI, SI: Integer): Boolean;
var
  K: Integer;
begin
  if PI > Length(G) then
  begin
    Result := SI > Length(S);
    Exit;
  end;
  Result := False;
  if (Copy(G, PI, 3) = '**/') then
  begin
    if GlobFrom(G, S, PI + 3, SI) then
    begin
      Result := True;
      Exit;
    end;
    for K := SI to Length(S) do
      if (S[K] = '/') and GlobFrom(G, S, PI + 3, K + 1) then
      begin
        Result := True;
        Exit;
      end;
  end
  else if Copy(G, PI, 2) = '**' then
  begin
    for K := SI to Length(S) + 1 do
      if GlobFrom(G, S, PI + 2, K) then
      begin
        Result := True;
        Exit;
      end;
  end
  else if G[PI] = '*' then
  begin
    K := SI;
    while True do
    begin
      if GlobFrom(G, S, PI + 1, K) then
      begin
        Result := True;
        Exit;
      end;
      if (K > Length(S)) or (S[K] = '/') then
        Exit;
      K := K + 1;
    end;
  end
  else if G[PI] = '?' then
    Result := (SI <= Length(S)) and (S[SI] <> '/') and GlobFrom(G, S, PI + 1, SI + 1)
  else
    Result := (SI <= Length(S)) and (S[SI] = G[PI]) and GlobFrom(G, S, PI + 1, SI + 1);
end;

function CleanGlob(G: String): String;
begin
  StringChangeEx(G, '\', '/', True);
  while (G <> '') and (G[1] = '/') do
    G := Copy(G, 2, Length(G));
  Result := Lowercase(G);
end;

{ True when Rel matches one of the '|'-joined patterns. }
function MatchesAny(const Rel, Joined: String): Boolean;
var
  Rest, G: String;
  P: Integer;
begin
  Result := False;
  Rest := Joined;
  while Rest <> '' do
  begin
    P := Pos('|', Rest);
    if P = 0 then
    begin
      G := Rest;
      Rest := '';
    end
    else
    begin
      G := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, Length(Rest));
    end;
    if (G <> '') and GlobFrom(CleanGlob(G), Lowercase(Rel), 1, 1) then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

{ Reborn's own folders and files and the owner's (Rel is relative to the game folder, '/'-separated):
  a free-space part never removes these, whatever its patterns say. }
function IsRebornOwn(const Rel: String; const IsDir: Boolean): Boolean;
var
  L, Name: String;
begin
  L := Lowercase(Rel);
  Result := False;
  if Copy(L, 1, 15) <> 'binaries/win64/' then
    Exit;
  Name := Copy(L, 16, Length(L));
  if IsDir then
  begin
    Result := (Name = 'matchmaking') or (Name = 'controlpanel') or (Name = 'redist') or (Name = 'rebornversions') or
      (Name = 'rebornsetup') or (Name = 'steam_settings') or (Name = 'rb_ids') or (Name = 'rb_backups');
    Exit;
  end;
  if Pos('/', Name) > 0 then
    Exit;   { a file in another folder of Win64: only the parts' patterns decide }
  Result := (Copy(Name, 1, 6) = 'reborn') or (Copy(Name, 1, 11) = 'steamclient') or (Name = 'battleborn.exe') or
    (Name = 'serverborn.exe') or (Name = 'dxgi.dll') or (Name = 'coldclientloader.ini') or
    (Name = 'startmatchmaking.bat') or (Name = 'stopmatchmaking.bat') or (Name = 'openfirewall.bat') or
    (Name = 'readme-server.txt');
end;

{ Deletes the files under Root that match Joined. Linked folders are not followed; Reborn's own
  folders and files are skipped. }
procedure RemoveMatching(const Root, Rel, Joined: String; var Count: Integer);
var
  F: TFindRec;
  Names: TArrayOfString;
  IsDir: TArrayOfBoolean;
  I, N: Integer;
  Sub, Dir: String;
begin
  N := 0;
  Dir := Root;
  if Rel <> '' then
  begin
    Dir := Root + '\' + Rel;
    StringChangeEx(Dir, '/', '\', True);
  end;
  if FindFirst(Dir + '\*', F) then
  try
    repeat
      if (F.Name <> '.') and (F.Name <> '..') and ((F.Attributes and AttrReparsePoint) = 0) then
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
  begin
    if Rel = '' then
      Sub := Names[I]
    else
      Sub := Rel + '/' + Names[I];
    if IsRebornOwn(Sub, IsDir[I]) then
      Continue;
    if IsDir[I] then
      RemoveMatching(Root, Sub, Joined, Count)
    else if MatchesAny(Sub, Joined) then
    begin
      if DeleteFile(Dir + '\' + Names[I]) then
        Count := Count + 1
      else
        Log('Could not remove ' + Sub);
    end;
  end;
end;

procedure NoteRemovedPart(const Win64, Id: String);
var
  Lines: TArrayOfString;
  I: Integer;
begin
  if LoadStringsFromFile(Win64 + '\' + RemovedPartsFile, Lines) then
    for I := 0 to GetArrayLength(Lines) - 1 do
      if CompareText(Trim(Lines[I]), Id) = 0 then
        Exit;
  if not SaveStringToFile(Win64 + '\' + RemovedPartsFile, Id + #13#10, True) then
    Log('Could not write ' + RemovedPartsFile);
end;

{ True when the command line names this exact item in its /TASKS= or /MERGETASKS= list. }
function ListNames(const Param, Item: String): Boolean;
var
  I: Integer;
  Prefix, Value: String;
begin
  { The last /<Param>= counts, as it does for Setup itself. }
  Prefix := '/' + Param + '=';
  Value := '';
  for I := 1 to ParamCount do
    if CompareText(Copy(ParamStr(I), 1, Length(Prefix)), Prefix) = 0 then
      Value := Copy(ParamStr(I), Length(Prefix) + 1, Length(ParamStr(I)));
  Result := Pos(',' + Lowercase(Item) + ',', ',' + Lowercase(Value) + ',') > 0;
end;

{ Is free-space part Id chosen? From the command line only when /TASKS= or /MERGETASKS= names
  trim\<id> itself: "trim" alone (or "*trim") never removes a part. }
function PartChosen(const Id: String): Boolean;
begin
  Result := WizardIsTaskSelected('trim\' + Id);
  if Result and (HasSwitchPrefix('/TASKS=') or HasSwitchPrefix('/MERGETASKS=')) then
    Result := ListNames('TASKS', 'trim\' + Id) or ListNames('MERGETASKS', 'trim\' + Id);
  if WizardIsTaskSelected('trim\' + Id) and not Result then
    Log('Free-space part ' + Id + ' is ticked only through "trim"; it is not removed (name trim\' + Id + ' to remove it)');
end;

{ The ticked free-space parts: their files go from the game folder, their ids into the record. }
procedure RemoveChosenParts(const GameDir, Win64: String);
var
  Ids, Globs: TArrayOfString;
  Picked: TArrayOfBoolean;
  I, Count, Chosen: Integer;
begin
  CrashIfForced('trim');
  GetParts(Ids, Globs);
  SetArrayLength(Picked, GetArrayLength(Ids));
  Chosen := 0;
  for I := 0 to GetArrayLength(Ids) - 1 do
  begin
    Picked[I] := PartChosen(Ids[I]);
    if Picked[I] then
      Chosen := Chosen + 1;
  end;
  Log(Format('Removing game files to save space: %d of %d parts chosen', [Chosen, GetArrayLength(Ids)]));
  for I := 0 to GetArrayLength(Ids) - 1 do
    if Picked[I] then
    begin
      Count := 0;
      RemoveMatching(RemoveBackslashUnlessRoot(GameDir), '', Globs[I], Count);
      Log(Format('Free-space part %s: removed %d files', [Ids[I], Count]));
      NoteRemovedPart(Win64, Ids[I]);
    end;
end;

{ ---------------------------------------------------------------- GitHub access }

{ Whether gh is signed in for the user who started Setup (asked once). Only reads. }
function GhSignedIn: Boolean;
var
  Code: Integer;
begin
  if GhState = 0 then
  begin
    GhState := 2;
    if ExecAsOriginalUser(ExpandConstant('{cmd}'), '/C gh auth status >nul 2>&1', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0) then
      GhState := 1;
    if GhState = 1 then
      Log('gh is signed in: the GitHub access page is skipped')
    else
      Log('gh is not signed in (or not installed): the GitHub access page is shown');
  end;
  Result := GhState = 1;
end;

{ The token to keep: from /GHTOKENFILE= (tests), else what was typed on the page. Never logged. }
function TokenText: String;
var
  Raw: AnsiString;
  FromFile: String;
begin
  Result := '';
  FromFile := ExpandConstant('{param:GHTOKENFILE|}');
  if FromFile <> '' then
  begin
    if LoadStringFromFile(FromFile, Raw) then
      Result := Trim(String(Raw));
    Exit;
  end;
  if (TokenPage <> nil) and not WizardSilent then
    Result := Trim(TokenPage.Values[0]);
end;

{ Hands the token to the Control Panel through a temp file (never a command line or the log), run as
  the user who started Setup so it is kept for that user. The temp file is deleted whatever happens. }
procedure SaveGitHubToken(const Win64: String);
var
  Token, Tmp, Panel: String;
  Code: Integer;
  Ok: Boolean;
begin
  Token := TokenText;
  if Token = '' then
  begin
    Log('GitHub access: no token given, nothing saved');
    Exit;
  end;
  Tmp := ExpandConstant('{tmp}\' + TokenTempName);
  Panel := Win64 + '\ControlPanel\ControlPanel.exe';
  Ok := False;
  try
    CrashIfForced('token');
    if ForcedToFail('token') then
      Ok := False
    else if not SaveStringToFile(Tmp, Token, False) then
      Log('GitHub access: could not write the hand-over file')
    else if DryRun then
    begin
      Log('Dry run (/DRYRUN), not running: ControlPanel.exe --save-github-token <hand-over file>');
      Ok := True;
    end
    else
    begin
      Log('Running as the user who started Setup: ControlPanel.exe --save-github-token <hand-over file>');
      if ExecAsOriginalUser(Panel, '--save-github-token "' + Tmp + '"', ExtractFilePath(Panel), SW_HIDE, ewWaitUntilTerminated, Code) then
      begin
        Log(Format('Exit code %d', [Code]));
        Ok := Code = 0;
      end
      else
        Log('Could not start it: ' + SysErrorMessage(Code));
    end;
  finally
    DeleteFile(Tmp);
    if FileExists(Tmp) then
      Log('GitHub access: the hand-over file could not be deleted')
    else
      Log('GitHub access: the hand-over file is gone');
    Token := '';
  end;
  if not Ok then
    AddProblem(MsgFailToken);
end;

<event('ShouldSkipPage')>
function SkipTokenPage(PageID: Integer): Boolean;
begin
  Result := (TokenPage <> nil) and (PageID = TokenPage.ID) and
            (HasSwitchPrefix('/GHTOKENFILE=') or WizardSilent or GhSignedIn);
end;

{ ---------------------------------------------------------------- options: the wizard }

{ The runtime is ticked only when it is missing, unless the command line chose for it. Done once,
  when the options list exists (it is filled when its page comes up, silent installs too). }
procedure ApplyRuntimeDefault();
begin
  if RuntimeDefaultDone then
    Exit;
  RuntimeDefaultDone := True;
  if RuntimeMissing then
    Log('The Visual C++ 2012 runtime is missing (' + RuntimeCheckFile + ' is not there): vcredist starts ticked')
  else
    Log('The Visual C++ 2012 runtime is there (' + RuntimeCheckFile + '): vcredist starts unticked');
  if HasSwitchPrefix('/TASKS=') or
     (HasSwitchPrefix('/MERGETASKS=') and (Pos('vcredist', Lowercase(ExpandConstant('{param:MERGETASKS|}'))) > 0)) then
    Exit;
  if RuntimeMissing then
    WizardSelectTasks('vcredist')
  else
    WizardSelectTasks('!vcredist');
end;

<event('CurPageChanged')>
procedure RuntimeDefaultOnPage(CurPageID: Integer);
begin
  if CurPageID = wpSelectTasks then
    ApplyRuntimeDefault();
end;

<event('NextButtonClick')>
function RuntimeDefaultOnNext(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectTasks then
    ApplyRuntimeDefault();
end;

procedure InitializeWizard();
var
  Note: TNewStaticText;
  Text: String;
  Space: Integer;
begin
  TokenPage := CreateInputQueryPage(wpSelectTasks, MsgTokenTitle, MsgTokenDescription, MsgTokenLabel);
  TokenPage.Add('Token:', True);
  if PartCountValue > 0 then
  begin
    Text := MsgPartsWarning;
    if not PartsTestedTogether then
      Text := Text + ' ' + MsgPartsOneAtATime;
    Note := TNewStaticText.Create(WizardForm);
    Note.Parent := WizardForm.TasksList.Parent;
    Note.AutoSize := False;
    Note.WordWrap := True;
    Note.Left := WizardForm.TasksList.Left;
    Note.Width := WizardForm.TasksList.Width;
    Note.Height := ScaleY(44);
    Space := Note.Height + ScaleY(8);
    WizardForm.TasksList.Height := WizardForm.TasksList.Height - Space;
    Note.Top := WizardForm.TasksList.Top + WizardForm.TasksList.Height + ScaleY(8);
    Note.Anchors := [akLeft, akRight, akBottom];
    Note.Caption := Text;
  end;
end;

<event('CurPageChanged')>
procedure ShowProblems(CurPageID: Integer);
var
  OldHeight, Grown: Integer;
begin
  { Added under Setup's own text, so "restart to finish" (when the runtime needs it) stays first.
    Setup sized the label for its own text and put the restart choice right under it: the label
    grows to fit the new lines, and whatever sits under it moves down by as much. }
  if (CurPageID = wpFinished) and (Problems <> '') then
  begin
    OldHeight := WizardForm.FinishedLabel.Height;
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 + MsgFinishedProblems + #13#10 + Problems;
    WizardForm.AdjustLabelHeight(WizardForm.FinishedLabel);
    Grown := WizardForm.FinishedLabel.Height - OldHeight;
    if Grown > 0 then
    begin
      WizardForm.YesRadio.Top := WizardForm.YesRadio.Top + Grown;
      WizardForm.NoRadio.Top := WizardForm.NoRadio.Top + Grown;
      WizardForm.RunList.Top := WizardForm.RunList.Top + Grown;
    end;
    Log(Format('Finished page: the problem list was added; the text grew by %d pixels', [Grown]));
  end;
end;

function NeedRestart(): Boolean;
begin
  Result := RestartWanted;
end;

{ The desktop shortcut is made by Setup itself (the Icons section, before the options run); this
  only checks that it is there. }
function ShortcutThere: Boolean;
begin
  CrashIfForced('shortcut');
  Result := True;
  if DryRun then
  begin
    Log('Dry run (/DRYRUN): the desktop shortcut is not checked');
    Exit;
  end;
  Result := FileExists(ExpandConstant('{userdesktop}\Reborn Control Panel.lnk'));
  if not Result then
    Log('The desktop shortcut is not there: ' + ExpandConstant('{userdesktop}\Reborn Control Panel.lnk'));
end;

{ A step that stopped with an error: noted for the last page with Windows' reason. }
procedure AddCrashProblem(const Msg: String);
begin
  AddProblem(Msg + ' (' + GetExceptionMessage + ')');
end;

{ After the files: the chosen options. A failed one is noted for the last page; Setup goes on.
  Each step runs on its own: one that stops with an error never keeps the later ones from running. }
procedure RunOptions(const GameDir, Win64: String);
begin
  Log('Chosen options: ' + WizardSelectedTasks(False));
  if DryRun then
    Log('Dry run (/DRYRUN): firewall, runtime and scheduled-task commands are only logged');
  if WizardIsTaskSelected('firewall') then
    try
      if not FailStepIs('firewall') then
        NoteMade(Win64, 'firewall');
      if not AddFirewallRules(Win64) then
        AddProblem(MsgFailFirewall);
    except
      AddCrashProblem(MsgFailFirewall);
    end;
  if WizardIsTaskSelected('vcredist') then
    try
      if not InstallRuntime(Win64) then
        AddProblem(MsgFailRuntime);
    except
      AddCrashProblem(MsgFailRuntime);
    end;
  if WizardIsTaskSelected('autostart') then
    try
      if not FailStepIs('autostart') then
        NoteMade(Win64, 'autostart');
      if not AddSignInTask(Win64) then
        AddProblem(MsgFailAutostart);
    except
      AddCrashProblem(MsgFailAutostart);
    end;
  if WizardIsTaskSelected('shortcut') then
    try
      if not ShortcutThere then
        AddProblem(MsgFailShortcut);
    except
      AddCrashProblem(MsgFailShortcut);
    end;
  try
    SaveGitHubToken(Win64);
  except
    AddCrashProblem(MsgFailToken);
  end;
  try
    RemoveChosenParts(GameDir, Win64);
  except
    AddCrashProblem(MsgFailParts);
  end;
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
    RunOptions(ExpandConstant('{app}'), Win64);
  end;
end;

{ ---------------------------------------------------------------- Uninstall }

function InitializeUninstall(): Boolean;
var
  S: AnsiString;
begin
  Result := True;
  if PanelOpen(Win64Of(ExpandConstant('{app}'))) then
  begin
    SuppressibleMsgBox(MsgUninstallPanelOpen, mbError, MB_OK, IDOK);
    Result := False;
    Exit;
  end;
  UninstallMade := '';
  if LoadStringFromFile(Win64Of(ExpandConstant('{app}')) + '\' + MadeFile, S) then
    UninstallMade := String(S);
  StringChangeEx(UninstallMade, #13#10, #10, True);
  StringChangeEx(UninstallMade, #10, #13#10, True);
  if (UninstallMade <> '') and (Copy(UninstallMade, Length(UninstallMade) - 1, 2) <> #13#10) then
    UninstallMade := UninstallMade + #13#10;
end;

{ Removes the firewall rules and the sign-in task, but only the ones Setup made. One a test made
  with /DRYRUN is only logged, so a test never removes the owner's real rules. }
procedure RemoveMadeThings();
var
  Dry: Boolean;
begin
  if MadeHas('firewall') or MadeHas('firewall (dry run)') then
  begin
    Dry := DryRun or not MadeHas('firewall');
    DeleteFirewallRules(Dry);
  end
  else
    Log('Setup made no firewall rules; leaving the firewall alone');
  if MadeHas('autostart') or MadeHas('autostart (dry run)') then
  begin
    Dry := DryRun or not MadeHas('autostart');
    DeleteSignInTask(Dry);
  end
  else
    Log('Setup made no sign-in task; leaving the scheduled tasks alone');
  DeleteFile(Win64Of(ExpandConstant('{app}')) + '\' + MadeFile);
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
    RemoveMadeThings();
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
    { Matchmaking's per-server copies of the Steam loader (one folder per game server it ran). }
    DelTree(Win64 + '\rb_ids', True, True, True);
    DeleteFile(Win64 + '\Serverborn.exe');
    DeleteFile(Win64 + '\' + DeployedFile);
  end
  else if (CurUninstallStep = usDone) and UninstallPartsRemoved then
    SuppressibleMsgBox(MsgPartsGone, mbInformation, MB_OK, IDOK);
end;
