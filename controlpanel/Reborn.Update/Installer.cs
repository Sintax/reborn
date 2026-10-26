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
    /// <summary>Exists only while an update or roll back is part way: {from, to, when}.</summary>
    public const string InProgressFile = "in-progress.json";
    public const string DeployedFile = "reborn-deployed.txt";
    /// <summary>In each saved copy: the exact reborn-deployed.txt text it was saved from (empty = there was none).</summary>
    public const string BackupMarkerFile = ".reborn-backup.txt";
    /// <summary>In each saved copy: the files the update away from that version added, one per line.</summary>
    public const string AddedListFile = ".reborn-added.txt";
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
    string InProgressPath => Path.Combine(VersionsDir, InProgressFile);

    /// <summary>Installs <paramref name="stagedDir"/> (the update zip's Win64 folder) as <paramref name="version"/>.
    /// Order: back up what will be replaced, mark the update as in progress, stop the servers, copy in,
    /// record the version, start matchmaking, wait up to 180 s for a game, clear the mark.</summary>
    public InstallResult Install(string stagedDir, string version)
    {
        version = (version ?? "").Trim();
        if (Versions.Parse(version) == 0)
            return Finish(version, false, $"\"{version}\" isn't a release number like 1.0.3. Nothing was changed.");
        if (Unfinished() is string from)
            return Finish(version, false, UnfinishedMessage(from));
        if (string.IsNullOrWhiteSpace(stagedDir) || !Directory.Exists(stagedDir))
            return Finish(version, false, "The downloaded update folder is missing. Nothing was changed.");
        return Apply(new Job(stagedDir, version, version + "\n", new List<string>(), Recovery: false,
            $"The update to {version}", $"Updated to {version}."));
    }

    /// <summary>Puts back a version kept in RebornVersions, with the same steps and safety as Install. Files
    /// that later versions added are removed, so the result matches that version. After an update that broke
    /// off, only a roll back to the version it started from is allowed; that one clears the in-progress mark.</summary>
    public InstallResult RollBack(string toVersion)
    {
        string name = (toVersion ?? "").Trim();
        string? from = Unfinished();
        if (from is not null && !CanRecover())
            return Finish(name, false, StuckMessage(InProgressFullPath));
        bool recovery = from is not null && string.Equals(name, from, StringComparison.OrdinalIgnoreCase);
        if (from is not null && !recovery)
            return Finish(name, false, UnfinishedMessage(from));

        string source = Path.Combine(VersionsDir, name);
        if (!IsVersionFolderName(name) || !File.Exists(Path.Combine(source, BackupMarkerFile)))
            return Finish(name, false, $"There is no saved copy of {name} to go back to. Nothing was changed.");
        if (!recovery && string.Equals(name, Installed(), StringComparison.OrdinalIgnoreCase))
            return Finish(name, false, $"{name} is already installed. Nothing was changed.");

        string recorded;
        List<string> deletes;
        try
        {
            recorded = File.ReadAllText(Path.Combine(source, BackupMarkerFile));
            deletes = AddedSince(name);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            var run = new Run();
            run.Fail(null, e);
            return Finish(name, false, $"The saved copy of {name} couldn't be read. Nothing was changed.", run);
        }
        return Apply(new Job(source, name, recorded.Trim().Length == 0 ? null : recorded, deletes, recovery,
            $"Going back to {name}", $"Went back to {name}."));
    }

    /// <summary>Keeps the newest <paramref name="keep"/> saved versions and deletes the rest. The installed
    /// version's own copy and the copy an unfinished update started from are never counted or deleted.</summary>
    public void Prune(int keep = 3)
    {
        string? pending = Unfinished();
        foreach (string name in Kept().Where(n => !string.Equals(n, pending, StringComparison.OrdinalIgnoreCase)).Skip(Math.Max(0, keep)))
            TryDeleteDirectory(Path.Combine(VersionsDir, name));
        if (Directory.Exists(VersionsDir))
            foreach (string dir in Directory.GetDirectories(VersionsDir, SavingPrefix + "*"))
                TryDeleteDirectory(dir);
    }

    /// <summary>The saved versions that RollBack can put back, newest first, not counting the installed one.
    /// The most recently saved copy leads, so the version that was running just before is never the one
    /// pruned (even after a roll back to an older build). Copies saved at the same moment go by build number.</summary>
    public IReadOnlyList<string> Kept()
    {
        string installed = Installed();
        return AllBackups().Where(n => !string.Equals(n, installed, StringComparison.OrdinalIgnoreCase)).ToList();
    }

    sealed record Job(string SourceDir, string Name, string? Recorded, List<string> Deletes, bool Recovery,
        string What, string Success);

    // Failed paths and the first error, for last-update.json "details".
    sealed class Run
    {
        readonly List<string> _paths = new();
        string? _firstError;

        public void Fail(string? path, Exception e)
        {
            if (path is not null && !_paths.Contains(path, StringComparer.OrdinalIgnoreCase)) _paths.Add(path);
            _firstError ??= e.Message;
        }

        public string Details =>
            string.Join(" ", new[]
            {
                _paths.Count > 0 ? $"Failed: {string.Join(", ", _paths)}." : null,
                _firstError is not null ? $"First error: {_firstError}" : null,
            }.Where(s => s is not null));
    }

    InstallResult Apply(Job job)
    {
        var run = new Run();
        List<string> files;
        try { files = SourceFiles(job.SourceDir); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            run.Fail(null, e);
            return Finish(job.Name, false, $"{job.What} couldn't start: its files couldn't be read. Nothing was changed.", run);
        }
        if (files.Count == 0)
            return Finish(job.Name, false, $"{job.What} couldn't start: there are no files in it. Nothing was changed.");

        var copied = new HashSet<string>(files, StringComparer.OrdinalIgnoreCase);
        List<string> deletes = job.Deletes.Where(d => !copied.Contains(d)).ToList();
        List<string> affected = files.Concat(deletes).ToList();
        string? deployedBefore = ReadDeployed();
        string current = FolderNameFor(deployedBefore);

        // 1. Keep a copy of everything that is about to change, then mark the update as in progress.
        //    Nothing is stopped yet. (A recovery roll back skips the copy: the live files are a broken mix.)
        string? backupDir = null;
        try
        {
            if (!job.Recovery)
            {
                backupDir = Backup(affected, files, current, deployedBefore);
                WriteInProgress(current, job.Name);
            }
        }
        catch (Exception e)
        {
            run.Fail(null, e);
            return Finish(job.Name, false,
                $"{job.What} couldn't start: a copy of the current version couldn't be saved first. " +
                "Check the server has free disk space. Nothing was changed.", run);
        }

        var createdDirs = new List<string>();
        bool started = false;
        string reason = "the servers couldn't be stopped";
        string? path = null;
        // How many of `files` were copied in, and the one being copied when something failed (it may be
        // half-written). Only these, plus the files a roll back removes, are put back after a failure:
        // a file the update never reached (one held open by another program, say) is left alone.
        int copiedCount = 0;
        string? copying = null;
        try
        {
            // 2. Stop everything so no file is in use.
            servers.StopAll();

            // 3. Remove files later versions added (roll back only), then copy the files in.
            foreach (string rel in deletes)
            {
                (path, reason) = (rel, $"{rel} couldn't be removed");
                string target = Path.Combine(win64, rel);
                if (File.Exists(target)) File.Delete(target);
            }
            foreach (string rel in files)
            {
                (path, reason) = (rel, $"{rel} couldn't be copied into place");
                string target = Path.Combine(win64, rel);
                copying = rel;
                CreateDirectories(Path.GetDirectoryName(target)!, createdDirs);
                File.Copy(Path.Combine(job.SourceDir, rel), target, overwrite: true);
                copying = null;
                copiedCount++;
            }

            // 4. Record which version is installed.
            (path, reason) = (DeployedFile, "the new version number couldn't be saved");
            WriteDeployed(job.Recorded);

            // 5. Start matchmaking, 6. and wait for a game server to start and check in.
            (path, reason) = (null, "matchmaking didn't start");
            started = true;
            if (servers.StartMatchmaking())
            {
                reason = "no game server started within 3 minutes";
                if (servers.WaitForGame(HealthTimeout))
                {
                    bool cleared = ClearInProgress(run);
                    try { Prune(); } catch (Exception) { /* an old copy left behind is harmless */ }
                    if (!cleared)
                        return Finish(job.Name, false,
                            $"{job.Success} A game is up and running, but the update's in-progress mark couldn't be removed, " +
                            $"so the panel thinks the update didn't finish. Delete {InProgressFullPath} by hand.", run);
                    return Finish(job.Name, true, $"{job.Success} A game is up and running.", run);
                }
            }
        }
        catch (Exception e)
        {
            run.Fail(path, e);   // `reason` already names the step that failed
        }

        List<string> touched = deletes.Concat(files.Take(copiedCount)).ToList();
        if (copying is not null) touched.Add(copying);
        return Restore(job, reason, current, backupDir, touched, createdDirs, deployedBefore, started, run);
    }

    InstallResult Restore(Job job, string reason, string current, string? backupDir, List<string> touched,
        List<string> createdDirs, string? deployedBefore, bool started, Run run)
    {
        bool restored = backupDir is not null;
        if (started)
        {
            try { servers.StopAll(); } catch (Exception e) { run.Fail(null, e); restored = false; }
        }
        if (backupDir is not null)
        {
            foreach (string rel in touched)
            {
                try
                {
                    string saved = Path.Combine(backupDir, rel);
                    string live = Path.Combine(win64, rel);
                    if (File.Exists(saved))
                    {
                        if (SameFile(saved, live)) continue;   // never changed (or already back): nothing to do
                        Directory.CreateDirectory(Path.GetDirectoryName(live)!);
                        File.Copy(saved, live, overwrite: true);
                    }
                    else if (File.Exists(live))
                    {
                        File.Delete(live);   // the version being put back didn't have this file
                    }
                }
                catch (Exception e) { run.Fail(rel, e); restored = false; }
            }
            try { WriteDeployed(deployedBefore); }
            catch (Exception e) { run.Fail(DeployedFile, e); restored = false; }
        }
        for (int i = createdDirs.Count - 1; i >= 0; i--)
        {
            try
            {
                if (Directory.Exists(createdDirs[i]) && !Directory.EnumerateFileSystemEntries(createdDirs[i]).Any())
                    Directory.Delete(createdDirs[i]);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        bool markStuck = restored && !ClearInProgress(run);

        bool running;
        try { running = servers.StartMatchmaking(); }
        catch (Exception e) { run.Fail(null, e); running = false; }

        string old = current == Unknown ? "the previous version" : current;
        var message = new StringBuilder($"{job.What} didn't work: {reason}. ");
        if (job.Recovery)
            message.Append($"Try going back to {job.Name} again. ")
                   .Append(running ? "Matchmaking is running." : "Matchmaking isn't running.");
        else if (restored)
            message.Append(running
                ? $"{Capital(old)} was put back and matchmaking is running again."
                : $"{Capital(old)} was put back, but matchmaking didn't start again. Try starting it from the panel.");
        else
            message.Append($"Putting {old} back didn't fully work, so the server needs a look. ")
                   .Append(running ? "Matchmaking is running." : "Matchmaking isn't running.");
        if (markStuck)
            message.Append($" The update's in-progress mark couldn't be removed: delete {InProgressFullPath} by hand.");
        return Finish(job.Name, false, message.ToString(), run);
    }

    // True when both files exist and hold the same bytes.
    static bool SameFile(string a, string b)
    {
        try
        {
            var fa = new FileInfo(a);
            var fb = new FileInfo(b);
            if (!fa.Exists || !fb.Exists || fa.Length != fb.Length) return false;
            using FileStream sa = new(a, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using FileStream sb = new(b, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            byte[] ba = new byte[81920], bb = new byte[81920];
            while (true)
            {
                int na = sa.ReadAtLeast(ba, ba.Length, throwOnEndOfStream: false);
                int nb = sb.ReadAtLeast(bb, bb.Length, throwOnEndOfStream: false);
                if (na != nb || !ba.AsSpan(0, na).SequenceEqual(bb.AsSpan(0, nb))) return false;
                if (na == 0) return true;
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }

    // Saves the files about to change into RebornVersions\<current>\, plus the exact deployed text and the
    // list of files the update will add. Built in a side folder first, so a failure leaves any older copy alone.
    // A release's files never change, so an existing copy of a release is the starting point: only live files
    // it lacks are added (live files may be a broken mix after an update that broke off).
    string Backup(List<string> affected, List<string> files, string current, string? deployedBefore)
    {
        Directory.CreateDirectory(VersionsDir);
        string final = Path.Combine(VersionsDir, current);
        string temp = Path.Combine(VersionsDir, SavingPrefix + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(temp);
            var earlierAdded = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            if (Versions.Parse(current) > 0 && Directory.Exists(final))
            {
                CopyTree(final, temp);
                earlierAdded.UnionWith(ReadAddedList(final));
            }

            var added = new HashSet<string>(earlierAdded, StringComparer.OrdinalIgnoreCase);
            foreach (string rel in files)
            {
                string live = Path.Combine(win64, rel);
                if (!File.Exists(live) && !Directory.Exists(live)) added.Add(rel);
            }
            foreach (string rel in affected)
            {
                string live = Path.Combine(win64, rel);
                string copy = Path.Combine(temp, rel);
                if (!File.Exists(live) || File.Exists(copy) || earlierAdded.Contains(rel)) continue;
                Directory.CreateDirectory(Path.GetDirectoryName(copy)!);
                File.Copy(live, copy);
            }
            File.WriteAllLines(Path.Combine(temp, AddedListFile), added.Order(StringComparer.OrdinalIgnoreCase));
            File.WriteAllText(Path.Combine(temp, BackupMarkerFile), deployedBefore ?? "");

            if (Directory.Exists(final)) Directory.Delete(final, recursive: true);
            Directory.Move(temp, final);
            return final;
        }
        catch
        {
            TryDeleteDirectory(temp);
            throw;
        }
    }

    // Files added since `name` was saved: its own list plus the lists of every copy saved after it.
    List<string> AddedSince(string name)
    {
        var result = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (string backup in AllBackups())
        {
            result.UnionWith(ReadAddedList(Path.Combine(VersionsDir, backup)));
            if (string.Equals(backup, name, StringComparison.OrdinalIgnoreCase)) break;
        }
        return result.Order(StringComparer.OrdinalIgnoreCase).ToList();
    }

    // Every saved copy, most recently saved first.
    List<string> AllBackups()
    {
        if (!Directory.Exists(VersionsDir)) return new List<string>();
        return Directory.GetDirectories(VersionsDir)
            .Select(d => Path.GetFileName(d))
            .Where(n => IsVersionFolderName(n) && File.Exists(Path.Combine(VersionsDir, n, BackupMarkerFile)))
            .OrderByDescending(n => File.GetLastWriteTimeUtc(Path.Combine(VersionsDir, n, BackupMarkerFile)))
            .ThenByDescending(n => Versions.Parse(n))
            .ThenBy(n => n, StringComparer.Ordinal)
            .ToList();
    }

    static IEnumerable<string> ReadAddedList(string backupDir)
    {
        string list = Path.Combine(backupDir, AddedListFile);
        if (!File.Exists(list)) return Array.Empty<string>();
        return File.ReadAllLines(list)
            .Select(l => l.Trim().Replace('/', '\\'))
            .Where(IsUpdatablePath)
            .ToList();
    }

    // Every file under dir as a Win64-relative path, minus the owner's files, the backups folder and
    // the saved copy's own bookkeeping files.
    static List<string> SourceFiles(string dir) =>
        Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories)
            .Select(f => Path.GetRelativePath(dir, f).Replace('/', '\\'))
            .Where(IsUpdatablePath)
            .OrderBy(rel => rel, StringComparer.OrdinalIgnoreCase)
            .ToList();

    static bool IsUpdatablePath(string rel) =>
        rel.Length > 0
        && !Path.IsPathRooted(rel)
        && rel.IndexOf(':') < 0
        && !rel.Split('\\').Any(part => part is "" or "." or "..")
        && !ProtectedFiles.Contains(rel)
        && !rel.Equals(BackupMarkerFile, StringComparison.OrdinalIgnoreCase)
        && !rel.Equals(AddedListFile, StringComparison.OrdinalIgnoreCase)
        && !rel.StartsWith(VersionsFolder + "\\", StringComparison.OrdinalIgnoreCase);

    // Creates dir and records each folder that didn't exist yet (outermost first).
    void CreateDirectories(string dir, List<string> created)
    {
        var missing = new Stack<string>();
        string root = Path.GetFullPath(win64).TrimEnd('\\');
        for (string? d = Path.GetFullPath(dir); d is not null && d.Length > root.Length && !Directory.Exists(d); d = Path.GetDirectoryName(d))
            missing.Push(d);
        Directory.CreateDirectory(dir);
        created.AddRange(missing);
    }

    static void CopyTree(string from, string to)
    {
        foreach (string file in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
        {
            string target = Path.Combine(to, Path.GetRelativePath(from, file));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(file, target);
        }
    }

    string? ReadDeployed()
    {
        try
        {
            string path = Path.Combine(win64, DeployedFile);
            return File.Exists(path) ? File.ReadAllText(path) : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    // Writes reborn-deployed.txt exactly; null means there was no such file.
    void WriteDeployed(string? text)
    {
        string path = Path.Combine(win64, DeployedFile);
        if (text is null) File.Delete(path);
        else File.WriteAllText(path, text);
    }

    string Installed() => FolderNameFor(ReadDeployed());

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

    // The version an update that broke off started from, or null when nothing is part way.
    // A mark that can't be read counts as "unknown".
    string? Unfinished() => File.Exists(InProgressPath) ? ReadMark() ?? Unknown : null;

    // The mark's "from", or null when the mark can't be read or names something that isn't a version folder.
    string? ReadMark()
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(InProgressPath));
            if (doc.RootElement.ValueKind == JsonValueKind.Object
                && doc.RootElement.TryGetProperty("from", out JsonElement from)
                && from.ValueKind == JsonValueKind.String
                && IsVersionFolderName(from.GetString()))
                return from.GetString();
        }
        catch (Exception e) when (e is JsonException or IOException or UnauthorizedAccessException) { }
        return null;
    }

    /// <summary>True when an update broke off and the roll back that puts it right can run: its mark can be
    /// read and the saved copy it started from is still there.</summary>
    public bool CanRecover() =>
        File.Exists(InProgressPath) && ReadMark() is string from && HasSavedCopy(from);

    bool HasSavedCopy(string name) =>
        IsVersionFolderName(name) && File.Exists(Path.Combine(VersionsDir, name, BackupMarkerFile));

    /// <summary>What to say when an update broke off and the roll back that would put it right can't run.</summary>
    public static string StuckMessage(string inProgressPath) =>
        $"An earlier update did not finish and its saved copy is gone. Delete {inProgressPath} and check the server by hand.";

    string UnfinishedMessage(string from) =>
        CanRecover() ? $"A previous update didn't finish. Roll back to {from} first." : StuckMessage(InProgressFullPath);

    string InProgressFullPath => Path.GetFullPath(InProgressPath);

    void WriteInProgress(string from, string to) =>
        File.WriteAllText(InProgressPath, JsonSerializer.Serialize(new { from, to, when = now() }));

    // False when the mark is still there afterwards.
    bool ClearInProgress(Run run)
    {
        try
        {
            if (File.Exists(InProgressPath)) File.Delete(InProgressPath);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            run.Fail(InProgressFile, e);
            return false;
        }
    }

    static string Capital(string s) => s.Length == 0 ? s : char.ToUpperInvariant(s[0]) + s[1..];

    InstallResult Finish(string version, bool ok, string message, Run? run = null)
    {
        try
        {
            Directory.CreateDirectory(VersionsDir);
            string json = JsonSerializer.Serialize(new { version, ok, message, details = run?.Details ?? "", when = now() },
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
