# Battleborn Reborn installer for a friend's PC. Started by Install.bat; needs only Windows PowerShell.
#
# 1. Finds the Battleborn game folder (Steam's library list, or asks).
# 2. Tells Windows Defender to leave that folder alone (and this folder, so a re-extract survives).
# 3. Installs Microsoft's Visual C++ 2012 runtime (x64) if it is missing (Battleborn needs MSVCP110.dll).
# 4. Copies the mod, the Steam loader and the player's settings into the game folder.
# 5. Puts a "Battleborn Reborn" shortcut on the desktop.
#
# Test switches (no admin needed): -GameDir <Win64 folder> -SkipDefender -SkipRuntime -NoShortcut -NoPause
param(
    [string]$GameDir = "",
    [switch]$SkipDefender,
    [switch]$SkipRuntime,
    [switch]$NoShortcut,
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Files = Join-Path $Here "files"
$RuntimeUrl = "https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x64.exe"
$LoaderFiles = @("steamclient_loader_x64.exe", "steamclient.dll", "steamclient64.dll")

function Say($text) { Write-Host $text }
function Step($text) { Write-Host ""; Write-Host "== $text" -ForegroundColor Cyan }
function Finish($code) {
    if (-not $NoPause) { Write-Host ""; Read-Host "Press Enter to close" | Out-Null }
    exit $code
}
function Fail($text) { Write-Host ""; Write-Host "PROBLEM: $text" -ForegroundColor Red; Finish 1 }

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Defender and the runtime need administrator rights: ask Windows for them (the usual Yes/No prompt).
if (-not (Test-Admin) -and -not ($SkipDefender -and $SkipRuntime)) {
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$($MyInvocation.MyCommand.Path)`"")
    if ($GameDir) { $argList += @("-GameDir", "`"$GameDir`"") }
    if ($SkipDefender) { $argList += "-SkipDefender" }
    if ($SkipRuntime) { $argList += "-SkipRuntime" }
    if ($NoShortcut) { $argList += "-NoShortcut" }
    try { Start-Process powershell.exe -Verb RunAs -ArgumentList $argList | Out-Null }
    catch { Say "Setup needs administrator rights to change Windows Defender and install the runtime. Run Install.bat again and click Yes."; Finish 1 }
    exit 0
}

Write-Host "Battleborn Reborn setup" -ForegroundColor Green
if (-not (Test-Path (Join-Path $Files "reborn.dll"))) {
    Fail "The 'files' folder is missing next to Install.bat. Extract the whole zip first (right-click it, Extract All), then run Install.bat from the extracted folder."
}

# --- 1. Find the game folder ------------------------------------------------------------------
Step "Finding Battleborn"
function Find-Win64 {
    # The Steam loader rewrites Steam's registry location to its own folder (the game's Win64) while
    # it runs, so after the first game start the registry may point there: also try Steam's default
    # folders, and accept a registry path that is itself the game folder.
    $libs = New-Object System.Collections.Generic.List[string]
    foreach ($d in @("${env:ProgramFiles(x86)}\Steam", "$env:ProgramFiles\Steam")) { if ($d) { $libs.Add($d) } }
    foreach ($key in @("HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam", "HKLM:\SOFTWARE\Valve\Steam")) {
        foreach ($name in @("SteamPath", "InstallPath")) {
            try { $p = (Get-ItemProperty -Path $key -Name $name -ErrorAction Stop).$name; if ($p) { $libs.Add(($p -replace "/", "\")) } } catch {}
        }
    }
    foreach ($steam in @($libs)) {
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs.Add(($m.Groups[1].Value -replace "\\\\", "\")) }
        }
    }
    foreach ($lib in ($libs | Select-Object -Unique)) {
        if (Test-Path (Join-Path $lib "Battleborn.exe")) { return $lib }
        $w = Join-Path $lib "steamapps\common\Battleborn\Binaries\Win64"
        if (Test-Path (Join-Path $w "Battleborn.exe")) { return $w }
    }
    return $null
}

if ($GameDir) { $Win64 = $GameDir } else { $Win64 = Find-Win64 }
if (-not $Win64) {
    Say "Could not find Battleborn in your Steam libraries. Please pick the folder that contains Battleborn.exe"
    Say "(Steam > Battleborn > Manage > Browse local files > Binaries > Win64)."
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "Pick Battleborn's Binaries\Win64 folder (it contains Battleborn.exe)"
    if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $Win64 = $dlg.SelectedPath }
}
if (-not $Win64 -or -not (Test-Path (Join-Path $Win64 "Battleborn.exe"))) {
    Fail "Battleborn.exe was not found. Install Battleborn through Steam first, then run Install.bat again."
}
Say "Game folder: $Win64"

# --- 2. Windows Defender ----------------------------------------------------------------------
Step "Windows Defender"
if ($SkipDefender) { Say "Skipped (-SkipDefender)." }
else {
    try {
        Add-MpPreference -ExclusionPath $Win64
        Add-MpPreference -ExclusionPath $Here
        Say "Defender will no longer scan the game folder or this setup folder."
    } catch {
        Say "Could not add the exclusion automatically ($($_.Exception.Message))."
        Say "If another antivirus is installed, add the game folder to its exceptions by hand (see the README)."
    }
}
$missing = @($LoaderFiles | Where-Object { -not (Test-Path (Join-Path $Files $_)) })
if ($missing.Count -gt 0) {
    Fail ("Windows Defender removed " + ($missing -join ", ") + " before setup could protect it. " +
          "This setup folder is now excluded: delete it, extract the zip again into the same place, and run Install.bat again.")
}

# --- 3. Visual C++ 2012 runtime ---------------------------------------------------------------
Step "Microsoft Visual C++ 2012 runtime (64-bit)"
$haveRuntime = Test-Path (Join-Path $env:WINDIR "System32\msvcp110.dll")
if ($SkipRuntime) { Say "Skipped (-SkipRuntime)." }
elseif ($haveRuntime) { Say "Already installed." }
else {
    $exe = Join-Path $env:TEMP "vcredist2012_x64.exe"
    Say "Downloading from Microsoft..."
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $RuntimeUrl -OutFile $exe -UseBasicParsing
    } catch { Fail "Could not download the runtime ($($_.Exception.Message)). Download it yourself from $RuntimeUrl, run it, then run Install.bat again." }
    $sig = Get-AuthenticodeSignature $exe
    if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch "Microsoft") {
        Remove-Item $exe -Force -ErrorAction SilentlyContinue
        Fail "The downloaded runtime is not signed by Microsoft, so it was not run. Download it yourself from $RuntimeUrl."
    }
    Say "Installing..."
    $p = Start-Process -FilePath $exe -ArgumentList "/install", "/quiet", "/norestart" -Wait -PassThru
    Remove-Item $exe -Force -ErrorAction SilentlyContinue
    # 0 = done, 3010 = done (restart later), 1638 = a newer copy is already installed
    if (@(0, 3010, 1638) -notcontains $p.ExitCode) { Fail "The runtime installer stopped with code $($p.ExitCode). Run $RuntimeUrl by hand." }
    Say "Installed."
}

# --- 4. Copy the files ------------------------------------------------------------------------
Step "Copying the mod into the game folder"
$backup = Join-Path $Win64 ("rb_backups\before-setup-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
$count = 0
foreach ($src in Get-ChildItem -Path $Files -Recurse -File) {
    $rel = $src.FullName.Substring($Files.Length + 1)
    $dst = Join-Path $Win64 $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    if (Test-Path $dst) {
        $b = Join-Path $backup $rel
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $b) | Out-Null
        Copy-Item -LiteralPath $dst -Destination $b -Force
    }
    Copy-Item -LiteralPath $src.FullName -Destination $dst -Force
    Unblock-File -LiteralPath $dst -ErrorAction SilentlyContinue   # no "downloaded from the internet" warning
    $count++
}
Say "$count files copied."
if (Test-Path $backup) { Say "Files that were replaced are saved in $backup" }

# --- 5. Desktop shortcut ----------------------------------------------------------------------
if (-not $NoShortcut) {
    Step "Desktop shortcut"
    $lnk = Join-Path ([Environment]::GetFolderPath("Desktop")) "Battleborn Reborn.lnk"
    $shell = New-Object -ComObject WScript.Shell
    $s = $shell.CreateShortcut($lnk)
    $s.TargetPath = Join-Path $Win64 "steamclient_loader_x64.exe"
    $s.WorkingDirectory = $Win64
    $s.IconLocation = (Join-Path $Win64 "Battleborn.exe") + ",0"
    $s.Save()
    Say "Created 'Battleborn Reborn' on your desktop."
}

Write-Host ""
Write-Host "All done. Start the game with the 'Battleborn Reborn' shortcut (or steamclient_loader_x64.exe in the game folder)." -ForegroundColor Green
Finish 0
