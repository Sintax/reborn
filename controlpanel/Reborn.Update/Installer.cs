using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Reborn.Update;

/// <summary>What happened, in words the owner reads in the panel.</summary>
public sealed record InstallResult(bool Ok, string Message);

/// <summary>Swaps a server update into the game's Win64 folder on the VM, keeping a copy of what it replaced.
/// Any failure once the servers are stopped puts the previous version back and starts matchmaking again.
/// Every call records its outcome in RebornVersions\last-update.json.</summary>
public sealed partial class Installer(string win64, IServerControl servers, Func<DateTime> now)
{
    /// <summary>How long a new version gets to list a game before it counts as broken.</summary>
    public static readonly TimeSpan HealthTimeout = TimeSpan.FromSeconds(180);
    public const string VersionsFolder = "RebornVersions";
    public const string LastUpdateFile = "last-update.json";
    public const string DeployedFile = "reborn-deployed.txt";
    public const string VersionFile = "reborn-version.txt";
    const string StagingFolder = "staging";
    const string SavingPrefix = ".saving-";
    const string Unknown = "unknown";

    /// <summary>The owner's own files (relative to Win64). An update never writes, backs up or restores them.</summary>
    public static readonly IReadOnlySet<string> ProtectedFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
    {
        @"matchmaking\appsettings.json",
        @"matchmaking\admin-token.txt",
        "reborn-users.json",
        "reborn-users-seen.json",
        "reborn-zips.json",
    };

    string VersionsDir => Path.Combine(win64, VersionsFolder);

    /// <summary>Installs <paramref name="stagedDir"/> (the update zip's Win64 folder) as <paramref name="version"/>.
    /// Order: back up what will be replaced, stop the servers, copy in, record the version,
    /// start matchmaking, wait up to 180 s for a game.</summary>
    public InstallResult Install(string stagedDir, string version)
    {
        version = (version ?? "").Trim();
        if (Versions.Parse(version) == 0)
            return Finish(version, false, $"\"{version}\" isn't a release number like 1.0.3. Nothing was changed.");
        if (string.IsNullOrWhiteSpace(stagedDir) || !Directory.Exists(stagedDir))
            return Finish(version, false, "The downloaded update folder is missing. Nothing was changed.");
        return Apply(stagedDir, version, version, $"The update to {version}", $"Updated to {version}.");
    }

    /// <summary>Puts back a version kept in RebornVersions, with the same steps and safety as Install.</summary>
    public InstallResult RollBack(string toVersion)
    {
        string name = (toVersion ?? "").Trim();
        string source = Path.Combine(VersionsDir, name);
        if (!IsVersionFolderName(name) || !File.Exists(Path.Combine(source, VersionFile)))
            return Finish(name, false, $"There is no saved copy of {name} to go back to. Nothing was changed.");
        if (string.Equals(name, FolderNameFor(ReadDeployed()), StringComparison.OrdinalIgnoreCase))
            return Finish(name, false, $"{name} is already installed. Nothing was changed.");

        string recorded = name;
        try { recorded = File.ReadAllText(Path.Combine(source, VersionFile)).Trim() is { Length: > 0 } t ? t : name; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return Apply(source, name, recorded, $"Going back to {name}", $"Went back to {name}.");
    }

    /// <summary>Keeps the newest <paramref name="keep"/> saved versions and deletes the rest.</summary>
    public void Prune(int keep = 3)
    {
        foreach (string name in Kept().Skip(Math.Max(0, keep)))
            TryDeleteDirectory(Path.Combine(VersionsDir, name));
        if (Directory.Exists(VersionsDir))
            foreach (string dir in Directory.GetDirectories(VersionsDir, SavingPrefix + "*"))
                TryDeleteDirectory(dir);
    }

    /// <summary>The saved versions that RollBack can put back, newest first: the most recently saved copy
    /// leads, so the version that was running just before is never the one pruned (even after a roll back
    /// to an older build). Copies saved at the same moment go by build number.</summary>
    public IReadOnlyList<string> Kept()
    {
        if (!Directory.Exists(VersionsDir)) return Array.Empty<string>();
        return Directory.GetDirectories(VersionsDir)
            .Select(d => Path.GetFileName(d))
            .Where(n => IsVersionFolderName(n) && File.Exists(Path.Combine(VersionsDir, n, VersionFile)))
            .OrderByDescending(n => File.GetLastWriteTimeUtc(Path.Combine(VersionsDir, n, VersionFile)))
            .ThenByDescending(n => Versions.Parse(n))
            .ThenBy(n => n, StringComparer.Ordinal)
            .ToList();
    }

    // The shared steps. `name` labels the backup and messages; `recorded` goes into reborn-deployed.txt.
    InstallResult Apply(string sourceDir, string name, string recorded, string what, string success)
    {
        List<string> files;
        try { files = SourceFiles(sourceDir); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return Finish(name, false, $"{what} couldn't start: its files couldn't be read. Nothing was changed.");
        }
        if (files.Count == 0)
            return Finish(name, false, $"{what} couldn't start: there are no files in it. Nothing was changed.");

        string? deployedBefore = ReadDeployed();
        string current = FolderNameFor(deployedBefore);

        // 1. Keep a copy of everything that is about to be replaced. Nothing is stopped yet.
        string backupDir;
        List<string> backedUp;
        try { (backupDir, backedUp) = Backup(files, current, deployedBefore?.Trim() ?? Unknown); }
        catch (Exception)
        {
            return Finish(name, false,
                $"{what} couldn't start: a copy of the current version ({current}) couldn't be saved first. " +
                "Check the server has free disk space. Nothing was changed.");
        }

        var created = new List<string>();
        bool started = false;
        string reason = "the servers couldn't be stopped";
        try
        {
            // 2. Stop everything so no file is in use.
            servers.StopAll();

            // 3. Copy the new files in, never the owner's own files.
            foreach (string rel in files)
            {
                reason = $"{rel} couldn't be copied into place";
                string target = Path.Combine(win64, rel);
                if (!File.Exists(target) && !Directory.Exists(target)) created.Add(rel);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                File.Copy(Path.Combine(sourceDir, rel), target, overwrite: true);
            }

            // 4. Record which version is installed.
            reason = "the new version number couldn't be saved";
            File.WriteAllText(Path.Combine(win64, DeployedFile), recorded + "\n");

            // 5. Start matchmaking, 6. and wait for it to list a game.
            reason = "matchmaking didn't start";
            started = true;
            if (servers.StartMatchmaking())
            {
                reason = "no game showed up within 3 minutes";
                if (servers.WaitForGame(HealthTimeout))
                {
                    try { Prune(); } catch (Exception) { /* an old copy left behind is harmless */ }
                    return Finish(name, true, $"{success} A game is up and running.");
                }
            }
        }
        catch (Exception)
        {
            // `reason` already names the step that failed.
        }

        return Restore(name, what, reason, current, backupDir, backedUp, created, deployedBefore, started);
    }

    InstallResult Restore(string name, string what, string reason, string current, string backupDir,
        List<string> backedUp, List<string> created, string? deployedBefore, bool started)
    {
        bool restored = true;
        if (started)
        {
            try { servers.StopAll(); } catch (Exception) { restored = false; }
        }
        foreach (string rel in created)
        {
            try
            {
                string path = Path.Combine(win64, rel);
                if (File.Exists(path)) File.Delete(path);
            }
            catch (Exception) { restored = false; }
        }
        foreach (string rel in backedUp)
        {
            try { File.Copy(Path.Combine(backupDir, rel), Path.Combine(win64, rel), overwrite: true); }
            catch (Exception) { restored = false; }
        }
        try
        {
            string deployed = Path.Combine(win64, DeployedFile);
            if (deployedBefore is null) File.Delete(deployed);
            else File.WriteAllText(deployed, deployedBefore);
        }
        catch (Exception) { restored = false; }

        bool running;
        try { running = servers.StartMatchmaking(); } catch (Exception) { running = false; }

        var message = new StringBuilder($"{what} didn't work: {reason}. ");
        if (restored)
            message.Append(running
                ? $"{current} was put back and matchmaking is running again."
                : $"{current} was put back, but matchmaking didn't start again. Try starting it from the panel.");
        else
            message.Append($"Putting {current} back didn't fully work, so the server needs a look. ")
                   .Append(running ? "Matchmaking is running." : "Matchmaking isn't running.");
        return Finish(name, false, message.ToString());
    }

    // Saves the files the update will replace into RebornVersions\<current>\, plus reborn-version.txt.
    // Built in a side folder first, so a failure leaves any older copy of that version alone.
    (string dir, List<string> saved) Backup(List<string> files, string current, string versionText)
    {
        Directory.CreateDirectory(VersionsDir);
        string temp = Path.Combine(VersionsDir, SavingPrefix + Guid.NewGuid().ToString("N"));
        try
        {
            var saved = new List<string>();
            foreach (string rel in files)
            {
                string source = Path.Combine(win64, rel);
                if (!File.Exists(source)) continue;
                string copy = Path.Combine(temp, rel);
                Directory.CreateDirectory(Path.GetDirectoryName(copy)!);
                File.Copy(source, copy);
                saved.Add(rel);
            }
            Directory.CreateDirectory(temp);
            File.WriteAllText(Path.Combine(temp, VersionFile), versionText + "\n");

            string final = Path.Combine(VersionsDir, current);
            if (Directory.Exists(final)) Directory.Delete(final, recursive: true);
            Directory.Move(temp, final);
            return (final, saved);
        }
        catch
        {
            TryDeleteDirectory(temp);
            throw;
        }
    }

    // Every file under dir as a Win64-relative path, minus the owner's files and the backups folder.
    static List<string> SourceFiles(string dir) =>
        Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories)
            .Select(f => Path.GetRelativePath(dir, f).Replace('/', '\\'))
            .Where(rel => !ProtectedFiles.Contains(rel)
                && !rel.StartsWith(VersionsFolder + "\\", StringComparison.OrdinalIgnoreCase))
            .OrderBy(rel => rel, StringComparer.OrdinalIgnoreCase)
            .ToList();

    string? ReadDeployed()
    {
        try
        {
            string path = Path.Combine(win64, DeployedFile);
            return File.Exists(path) ? File.ReadAllText(path) : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    // reborn-deployed.txt as a folder name: "1.0.4" stays, "2026-10-22 bab2c4e" becomes "2026-10-22-bab2c4e",
    // nothing at all becomes "unknown".
    static string FolderNameFor(string? deployed)
    {
        string name = UnsafeChars().Replace((deployed ?? "").Trim(), "-").Trim('-', '.');
        return IsVersionFolderName(name) ? name : Unknown;
    }

    static bool IsVersionFolderName(string? name) =>
        !string.IsNullOrEmpty(name)
        && name.Length <= 80
        && SafeName().IsMatch(name)
        && !string.Equals(name, StagingFolder, StringComparison.OrdinalIgnoreCase);

    [GeneratedRegex(@"^[A-Za-z0-9][A-Za-z0-9._-]*\z")]
    private static partial Regex SafeName();

    [GeneratedRegex(@"[^A-Za-z0-9._-]+")]
    private static partial Regex UnsafeChars();

    InstallResult Finish(string version, bool ok, string message)
    {
        try
        {
            Directory.CreateDirectory(VersionsDir);
            string json = JsonSerializer.Serialize(new { version, ok, message, when = now() },
                new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(Path.Combine(VersionsDir, LastUpdateFile), json);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { /* the result still goes back to the caller */ }
        return new InstallResult(ok, message);
    }

    static void TryDeleteDirectory(string dir)
    {
        try { if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
}
